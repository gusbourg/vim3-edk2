/** @file
  Runtime FVB over the command-driven Meson G12B SPI flash controller.

  The controller has no CPU-addressable flash aperture.  PrePi reserves a
  runtime RAM shadow for the variable/FTW window; this driver loads it before
  publishing FVB and keeps it coherent after verified NOR operations.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Guid/EventGroup.h>
#include <Guid/SystemNvDataGuid.h>
#include <Guid/VariableFlashInfo.h>
#include <Guid/VariableFormat.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/HobLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeLib.h>
#include <Pi/PiFirmwareVolume.h>
#include <Protocol/FirmwareVolumeBlock.h>
#include <Protocol/MesonNorFlash.h>

#include <MesonG12B.h>

#define MESON_SPIFC_SIGNATURE  SIGNATURE_32 ('S', 'F', 'V', 'B')

#define SPIFC_REG_CMD          0x00
#define SPIFC_REG_CTRL         0x08
#define SPIFC_REG_CLOCK        0x18
#define SPIFC_REG_USER         0x1C
#define SPIFC_REG_USER1        0x20
#define SPIFC_REG_USER4        0x2C
#define SPIFC_REG_SLAVE        0x30
#define SPIFC_REG_BUFFER       0x40

#define SPIFC_CMD_USER         BIT18
#define SPIFC_CTRL_AHB_ENABLE  BIT17
#define SPIFC_USER_DIN_ENABLE  BIT0
#define SPIFC_USER_COMPATIBLE  BIT2
#define SPIFC_USER_CLOCK_NOT_INVERTED  BIT7
#define SPIFC_USER_STAGE_MASK  (0x1FU << 27)
#define SPIFC_USER_DOUT_STAGE  BIT27
#define SPIFC_USER4_CS_POLARITY_HIGH  BIT23
#define SPIFC_USER4_CS_ACTIVE  BIT30
#define SPIFC_SLAVE_DONE       BIT4
#define SPIFC_SLAVE_MODE       BIT30
#define SPIFC_SLAVE_RESET      BIT31

#define SPIFC_TRANSFER_MAX     64U
#define SPIFC_COMMAND_MAX_DATA (SPIFC_TRANSFER_MAX - 4U)
#define SPIFC_TRANSFER_TIMEOUT_US  5000U
#define NOR_PROGRAM_TIMEOUT_US     1000000U
#define NOR_ERASE_TIMEOUT_US       5000000U

#define NOR_CMD_READ           0x03
#define NOR_CMD_WRITE_ENABLE   0x06
#define NOR_CMD_READ_STATUS    0x05
#define NOR_CMD_PAGE_PROGRAM   0x02
#define NOR_CMD_SECTOR_ERASE   0x20
#define NOR_CMD_READ_ID        0x9F

#define NOR_STATUS_BUSY        BIT0
#define NOR_STATUS_WEL         BIT1
#define NOR_STATUS_PROTECTION  0xFCU

#define VIM3_EXPECTED_ID0      0xEF
#define VIM3_EXPECTED_ID1      0x60
#define VIM3_EXPECTED_ID2      0x18

#define FVB_BLOCK_COUNT        (VIM3_NOR_RUNTIME_SIZE / VIM3_NOR_ERASE_SIZE)
#define FVB_VARIABLE_LAST_LBA  ((VIM3_NOR_VARIABLE_SIZE / VIM3_NOR_ERASE_SIZE) - 1U)
#define FVB_WORKING_FIRST_LBA  \
  ((VIM3_NOR_FTW_WORKING_BASE - VIM3_NOR_RUNTIME_BASE) / VIM3_NOR_ERASE_SIZE)
#define FVB_WORKING_LAST_LBA   \
  (FVB_WORKING_FIRST_LBA + (VIM3_NOR_FTW_WORKING_SIZE / VIM3_NOR_ERASE_SIZE) - 1U)
#define FVB_SPARE_FIRST_LBA    \
  ((VIM3_NOR_FTW_SPARE_BASE - VIM3_NOR_RUNTIME_BASE) / VIM3_NOR_ERASE_SIZE)
#define FVB_SPARE_LAST_LBA     \
  (FVB_SPARE_FIRST_LBA + (VIM3_NOR_FTW_SPARE_SIZE / VIM3_NOR_ERASE_SIZE) - 1U)

#define PERIPHS_MUX_BOOT0_7   (MESON_G12B_PERIPHS_MUX_BASE + (0x0 * 4))
#define PERIPHS_MUX_BOOT8_15  (MESON_G12B_PERIPHS_MUX_BASE + (0x1 * 4))
#define PERIPHS_PULL_BOOT     (MESON_G12B_PERIPHS_PULL_BASE + (0 * 4))
#define PERIPHS_PULLEN_BOOT   (MESON_G12B_PERIPHS_PULLEN_BASE + (0 * 4))

typedef struct {
  EFI_FIRMWARE_VOLUME_HEADER  FvHeader;
  EFI_FV_BLOCK_MAP_ENTRY      EndBlockMap;
  VARIABLE_STORE_HEADER       VariableStore;
} VIM3_VARIABLE_HEADER;

STATIC_ASSERT (
  OFFSET_OF (VIM3_VARIABLE_HEADER, VariableStore) ==
  sizeof (EFI_FIRMWARE_VOLUME_HEADER) + sizeof (EFI_FV_BLOCK_MAP_ENTRY),
  "Unexpected variable-store header layout"
  );

typedef struct {
  UINT32                               Signature;
  EFI_HANDLE                           Handle;
  EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  Fvb;
  EFI_FVB_ATTRIBUTES_2                 Attributes;
  EFI_PHYSICAL_ADDRESS                 ShadowPhysical;
  UINT8                                *Shadow;
  UINT8                                *Registers;
  EFI_EVENT                            VirtualAddressEvent;
  BOOLEAN                              Writable;
} MESON_SPIFC_FVB;

#define INSTANCE_FROM_FVB(a) \
  CR (a, MESON_SPIFC_FVB, Fvb, MESON_SPIFC_SIGNATURE)

STATIC MESON_SPIFC_FVB            mSpifcFvb;
STATIC MESON_NOR_FLASH_PROTOCOL   mNorFlash;
STATIC BOOLEAN                    mRestoreStateLogged;
STATIC BOOLEAN                    mFtwReadLogged;

STATIC
VOID
LogFtwHeader (
  IN CONST CHAR8  *Source,
  IN CONST UINT8  *Header
  )
{
  DEBUG ((
    DEBUG_WARN,
    "MesonSPIFC: %a FTW header "
    "%08x %08x %08x %08x %08x %08x %08x %08x\n",
    Source,
    ReadUnaligned32 ((CONST UINT32 *)&Header[0x00]),
    ReadUnaligned32 ((CONST UINT32 *)&Header[0x04]),
    ReadUnaligned32 ((CONST UINT32 *)&Header[0x08]),
    ReadUnaligned32 ((CONST UINT32 *)&Header[0x0C]),
    ReadUnaligned32 ((CONST UINT32 *)&Header[0x10]),
    ReadUnaligned32 ((CONST UINT32 *)&Header[0x14]),
    ReadUnaligned32 ((CONST UINT32 *)&Header[0x18]),
    ReadUnaligned32 ((CONST UINT32 *)&Header[0x1C])
    ));
}

STATIC
BOOLEAN
BufferIsErased (
  IN CONST UINT8  *Buffer,
  IN UINTN        Length
  )
{
  UINTN  Index;

  for (Index = 0; Index < Length; Index++) {
    if (Buffer[Index] != 0xFF) {
      return FALSE;
    }
  }

  return TRUE;
}

STATIC
VOID
Rmw32 (
  IN UINTN   Address,
  IN UINT32  ClearMask,
  IN UINT32  SetMask
  )
{
  MmioWrite32 (Address, (MmioRead32 (Address) & ~ClearMask) | SetMask);
}

STATIC
VOID
ConfigureSpifcPins (
  VOID
  )
{
  //
  // G12B function 3 maps BOOT4/5/6/14 to NOR D/Q/C/CS.  Leave BOOT0..3,
  // BOOT8, BOOT10, and BOOT13 assigned to the four-bit eMMC.
  //
  Rmw32 (
    PERIPHS_MUX_BOOT0_7,
    (0xFFFU << 16),
    (0x333U << 16)
    );
  Rmw32 (
    PERIPHS_MUX_BOOT8_15,
    (0xFU << 24),
    (0x3U << 24)
    );
  Rmw32 (
    PERIPHS_PULLEN_BOOT,
    BIT4 | BIT5 | BIT6 | BIT14,
    0
    );
  Rmw32 (
    PERIPHS_PULL_BOOT,
    BIT4 | BIT5 | BIT6 | BIT14,
    0
  );
}

STATIC
VOID
ConfigureSpifcController (
  VOID
  )
{
  MmioWrite32 ((UINTN)mSpifcFvb.Registers + SPIFC_REG_CLOCK, 0x0000C14CU);
  MmioOr32 (
    (UINTN)mSpifcFvb.Registers + SPIFC_REG_SLAVE,
    SPIFC_SLAVE_RESET
    );
  MmioAnd32 (
    (UINTN)mSpifcFvb.Registers + SPIFC_REG_USER,
    ~(SPIFC_USER_COMPATIBLE | SPIFC_USER_CLOCK_NOT_INVERTED)
    );
  MmioAnd32 (
    (UINTN)mSpifcFvb.Registers + SPIFC_REG_USER4,
    ~SPIFC_USER4_CS_POLARITY_HIGH
    );
  MmioAnd32 (
    (UINTN)mSpifcFvb.Registers + SPIFC_REG_SLAVE,
    ~SPIFC_SLAVE_MODE
    );
  MemoryFence ();
}

STATIC
VOID
RestoreSpifcIo (
  VOID
  )
{
  //
  // The eMMC and SPIFC drivers intentionally split the shared BOOT bank,
  // but later board initialization may still disturb pinmux or controller
  // state.  Reassert the complete known-good SPIFC mode at every externally
  // visible FVB operation so variable writes remain reliable after DXE
  // dispatch.
  //
  // Linux supplies virtual mappings only for the runtime SPIFC window, not
  // the SoC pin controller.  Firmware owns these pins for the whole boot, so
  // they only need reassertion before ExitBootServices.
  //
  if (!EfiAtRuntime ()) {
    ConfigureSpifcPins ();
    if (!mRestoreStateLogged) {
      DEBUG ((
        DEBUG_INFO,
        "MesonSPIFC: pre-restore ctrl=%08x clock=%08x user=%08x "
        "user4=%08x slave=%08x\n",
        MmioRead32 ((UINTN)mSpifcFvb.Registers + SPIFC_REG_CTRL),
        MmioRead32 ((UINTN)mSpifcFvb.Registers + SPIFC_REG_CLOCK),
        MmioRead32 ((UINTN)mSpifcFvb.Registers + SPIFC_REG_USER),
        MmioRead32 ((UINTN)mSpifcFvb.Registers + SPIFC_REG_USER4),
        MmioRead32 ((UINTN)mSpifcFvb.Registers + SPIFC_REG_SLAVE)
        ));
      mRestoreStateLogged = TRUE;
    }
  }

  ConfigureSpifcController ();
}

/**
  One SPIFC bufferload, optionally leaving chip select asserted.

  KeepCs streams a transaction longer than the 64-byte buffer: every
  bufferload except the last runs with SPIFC_USER4_CS_ACTIVE set, so the
  flash sees ONE continuous command (reference: mainline
  spi-meson-spifc.c meson_spifc_txrx and its USER4_CS_ACT handling).

  This exists because multi-command partial-page programming is BANNED on
  this flash: issuing several separate page-program commands into one
  256-byte page deterministically under-programs certain data patterns
  (bits left at 1), while the identical bytes written as a single CS-held
  page program are clean - measured 0/20 vs 20/20 on the bench part.
**/
STATIC
EFI_STATUS
SpifcTransferEx (
  IN  CONST UINT8  *Transmit,
  OUT UINT8        *Receive OPTIONAL,
  IN  UINTN        Length,
  IN  BOOLEAN      KeepCs
  )
{
  UINTN   Index;
  UINTN   Timeout;
  UINT32  Value;

  if ((Transmit == NULL) || (Length == 0) || (Length > SPIFC_TRANSFER_MAX)) {
    return EFI_INVALID_PARAMETER;
  }

  MmioAnd32 ((UINTN)mSpifcFvb.Registers + SPIFC_REG_CTRL, ~SPIFC_CTRL_AHB_ENABLE);

  for (Index = 0; Index < Length; Index += sizeof (UINT32)) {
    Value = 0;
    CopyMem (
      &Value,
      Transmit + Index,
      MIN (sizeof (Value), Length - Index)
      );
    MmioWrite32 (
      (UINTN)mSpifcFvb.Registers + SPIFC_REG_BUFFER + Index,
      Value
      );
  }

  Rmw32 (
    (UINTN)mSpifcFvb.Registers + SPIFC_REG_USER,
    SPIFC_USER_STAGE_MASK,
    SPIFC_USER_DOUT_STAGE | SPIFC_USER_DIN_ENABLE
    );
  MmioWrite32 (
    (UINTN)mSpifcFvb.Registers + SPIFC_REG_USER1,
    (UINT32)((Length * 8U - 1U) << 17)
    );
  if (KeepCs) {
    MmioOr32 (
      (UINTN)mSpifcFvb.Registers + SPIFC_REG_USER4,
      SPIFC_USER4_CS_ACTIVE
      );
  } else {
    MmioAnd32 (
      (UINTN)mSpifcFvb.Registers + SPIFC_REG_USER4,
      ~SPIFC_USER4_CS_ACTIVE
      );
  }

  MmioAnd32 (
    (UINTN)mSpifcFvb.Registers + SPIFC_REG_SLAVE,
    ~SPIFC_SLAVE_DONE
    );
  MmioOr32 (
    (UINTN)mSpifcFvb.Registers + SPIFC_REG_CMD,
    SPIFC_CMD_USER
    );

  for (Timeout = 0; Timeout < SPIFC_TRANSFER_TIMEOUT_US; Timeout++) {
    if ((MmioRead32 (
           (UINTN)mSpifcFvb.Registers + SPIFC_REG_SLAVE
           ) & SPIFC_SLAVE_DONE) != 0)
    {
      break;
    }

    MicroSecondDelay (1);
  }

  if (Timeout == SPIFC_TRANSFER_TIMEOUT_US) {
    //
    // Never leave CS asserted behind an aborted CS-held sequence.
    //
    MmioAnd32 (
      (UINTN)mSpifcFvb.Registers + SPIFC_REG_USER4,
      ~SPIFC_USER4_CS_ACTIVE
      );
    MmioOr32 (
      (UINTN)mSpifcFvb.Registers + SPIFC_REG_CTRL,
      SPIFC_CTRL_AHB_ENABLE
      );
    return EFI_TIMEOUT;
  }

  if (Receive != NULL) {
    for (Index = 0; Index < Length; Index += sizeof (UINT32)) {
      Value = MmioRead32 (
                (UINTN)mSpifcFvb.Registers + SPIFC_REG_BUFFER + Index
                );
      CopyMem (
        Receive + Index,
        &Value,
        MIN (sizeof (Value), Length - Index)
        );
    }
  }

  MmioOr32 (
    (UINTN)mSpifcFvb.Registers + SPIFC_REG_CTRL,
    SPIFC_CTRL_AHB_ENABLE
    );
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
SpifcTransfer (
  IN  CONST UINT8  *Transmit,
  OUT UINT8        *Receive OPTIONAL,
  IN  UINTN        Length
  )
{
  return SpifcTransferEx (Transmit, Receive, Length, FALSE);
}

STATIC
EFI_STATUS
NorReadStatus (
  OUT UINT8  *FlashStatus
  )
{
  UINT8       Receive[2];
  CONST UINT8 Transmit[2] = { NOR_CMD_READ_STATUS, 0xFF };
  EFI_STATUS  Status;

  Status = SpifcTransfer (Transmit, Receive, sizeof (Transmit));
  if (!EFI_ERROR (Status)) {
    *FlashStatus = Receive[1];
  }

  return Status;
}

STATIC
EFI_STATUS
NorWaitReady (
  IN UINTN  TimeoutUs
  )
{
  EFI_STATUS  Status;
  UINT8       FlashStatus;
  UINTN       Timeout;

  for (Timeout = 0; Timeout < TimeoutUs; Timeout += 10) {
    Status = NorReadStatus (&FlashStatus);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((FlashStatus & NOR_STATUS_BUSY) == 0) {
      return EFI_SUCCESS;
    }

    MicroSecondDelay (10);
  }

  return EFI_TIMEOUT;
}

STATIC
EFI_STATUS
NorWriteEnable (
  VOID
  )
{
  CONST UINT8 Command = NOR_CMD_WRITE_ENABLE;
  EFI_STATUS  Status;
  UINT8       FlashStatus;

  Status = SpifcTransfer (&Command, NULL, sizeof (Command));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = NorReadStatus (&FlashStatus);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((FlashStatus & NOR_STATUS_WEL) == 0) {
    DEBUG ((
      DEBUG_ERROR,
      "MesonSPIFC: write enable did not latch (status 0x%02x)\n",
      FlashStatus
      ));
    return EFI_WRITE_PROTECTED;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
NorRead (
  IN  UINT32  Address,
  OUT UINT8   *Buffer,
  IN  UINTN   Length
  )
{
  UINT8       Receive[SPIFC_TRANSFER_MAX];
  UINT8       Transmit[SPIFC_TRANSFER_MAX];
  UINTN       Chunk;
  EFI_STATUS  Status;

  if ((Buffer == NULL) ||
      (Address >= VIM3_NOR_SIZE) ||
      (Length > VIM3_NOR_SIZE - Address))
  {
    return EFI_INVALID_PARAMETER;
  }

  while (Length > 0) {
    Chunk       = MIN (Length, SPIFC_COMMAND_MAX_DATA);
    Transmit[0] = NOR_CMD_READ;
    Transmit[1] = (UINT8)(Address >> 16);
    Transmit[2] = (UINT8)(Address >> 8);
    Transmit[3] = (UINT8)Address;
    SetMem (&Transmit[4], Chunk, 0xFF);

    Status = SpifcTransfer (Transmit, Receive, Chunk + 4);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    CopyMem (Buffer, &Receive[4], Chunk);
    Address += (UINT32)Chunk;
    Buffer  += Chunk;
    Length  -= Chunk;
  }

  return EFI_SUCCESS;
}

/**
  Program up to one page as a SINGLE flash command.

  Length must not cross a 256-byte page boundary.  The command+address+
  data stream is longer than the 64-byte SPIFC buffer, so it is issued as
  several bufferloads with chip select held between them - the flash sees
  one continuous page program.  Never split a page into multiple program
  COMMANDS: that under-programs certain data patterns on this flash.
**/
STATIC
EFI_STATUS
NorProgramPage (
  IN UINT32       Address,
  IN CONST UINT8  *Buffer,
  IN UINTN        Length
  )
{
  UINT8       Transmit[SPIFC_TRANSFER_MAX];
  UINTN       Sent;
  UINTN       Chunk;
  EFI_STATUS  Status;

  if ((Length == 0) ||
      (Length > VIM3_NOR_PAGE_SIZE - (Address % VIM3_NOR_PAGE_SIZE)))
  {
    return EFI_INVALID_PARAMETER;
  }

  Status = NorWriteEnable ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Transmit[0] = NOR_CMD_PAGE_PROGRAM;
  Transmit[1] = (UINT8)(Address >> 16);
  Transmit[2] = (UINT8)(Address >> 8);
  Transmit[3] = (UINT8)Address;
  Chunk       = MIN (Length, (UINTN)SPIFC_COMMAND_MAX_DATA);
  CopyMem (&Transmit[4], Buffer, Chunk);
  Status = SpifcTransferEx (Transmit, NULL, Chunk + 4, (BOOLEAN)(Chunk < Length));
  if (EFI_ERROR (Status)) {
    NorWaitReady (NOR_PROGRAM_TIMEOUT_US);
    return Status;
  }

  Sent = Chunk;
  while (Sent < Length) {
    Chunk  = MIN (Length - Sent, (UINTN)SPIFC_TRANSFER_MAX);
    Status = SpifcTransferEx (
               Buffer + Sent,
               NULL,
               Chunk,
               (BOOLEAN)(Sent + Chunk < Length)
               );
    if (EFI_ERROR (Status)) {
      //
      // CS has been released (SpifcTransferEx clears it on error), which
      // makes the flash execute a TRUNCATED program.  Wait it out so the
      // caller's next command is not issued into a busy part.
      //
      NorWaitReady (NOR_PROGRAM_TIMEOUT_US);
      return Status;
    }

    Sent += Chunk;
  }

  return NorWaitReady (NOR_PROGRAM_TIMEOUT_US);
}

STATIC
EFI_STATUS
NorProgram (
  IN UINT32       Address,
  IN CONST UINT8  *Buffer,
  IN UINTN        Length
  )
{
  UINTN       Chunk;
  EFI_STATUS  Status;

  if ((Buffer == NULL) ||
      (Address >= VIM3_NOR_SIZE) ||
      (Length > VIM3_NOR_SIZE - Address))
  {
    return EFI_INVALID_PARAMETER;
  }

  while (Length > 0) {
    Chunk  = MIN (Length, VIM3_NOR_PAGE_SIZE - (Address % VIM3_NOR_PAGE_SIZE));
    Status = NorProgramPage (Address, Buffer, Chunk);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Address += (UINT32)Chunk;
    Buffer  += Chunk;
    Length  -= Chunk;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
NorEraseSector (
  IN UINT32  Address
  )
{
  UINT8       Command[4];
  EFI_STATUS  Status;

  if ((Address >= VIM3_NOR_SIZE) ||
      ((Address % VIM3_NOR_ERASE_SIZE) != 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  Status = NorWriteEnable ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Command[0] = NOR_CMD_SECTOR_ERASE;
  Command[1] = (UINT8)(Address >> 16);
  Command[2] = (UINT8)(Address >> 8);
  Command[3] = (UINT8)Address;
  Status     = SpifcTransfer (Command, NULL, sizeof (Command));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return NorWaitReady (NOR_ERASE_TIMEOUT_US);
}

STATIC
BOOLEAN
LbaWritable (
  IN EFI_LBA  Lba
  )
{
  return (Lba <= FVB_VARIABLE_LAST_LBA) ||
         ((Lba >= FVB_WORKING_FIRST_LBA) && (Lba <= FVB_WORKING_LAST_LBA)) ||
         ((Lba >= FVB_SPARE_FIRST_LBA) && (Lba <= FVB_SPARE_LAST_LBA));
}

STATIC
EFI_STATUS
EFIAPI
FvbGetAttributes (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  OUT EFI_FVB_ATTRIBUTES_2                      *Attributes
  )
{
  MESON_SPIFC_FVB  *Instance;

  if (Attributes == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Instance  = INSTANCE_FROM_FVB (This);
  *Attributes = Instance->Attributes;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FvbSetAttributes (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  IN OUT EFI_FVB_ATTRIBUTES_2                   *Attributes
  )
{
  MESON_SPIFC_FVB  *Instance;

  if (Attributes == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Instance = INSTANCE_FROM_FVB (This);
  if (*Attributes != Instance->Attributes) {
    *Attributes = Instance->Attributes;
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FvbGetPhysicalAddress (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  OUT EFI_PHYSICAL_ADDRESS                      *Address
  )
{
  MESON_SPIFC_FVB  *Instance;

  if (Address == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Instance = INSTANCE_FROM_FVB (This);
  *Address = Instance->ShadowPhysical;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FvbGetBlockSize (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  IN EFI_LBA                                    Lba,
  OUT UINTN                                     *BlockSize,
  OUT UINTN                                     *NumberOfBlocks
  )
{
  if ((BlockSize == NULL) || (NumberOfBlocks == NULL) ||
      (Lba >= FVB_BLOCK_COUNT))
  {
    return EFI_INVALID_PARAMETER;
  }

  *BlockSize      = VIM3_NOR_ERASE_SIZE;
  *NumberOfBlocks = FVB_BLOCK_COUNT - (UINTN)Lba;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FvbRead (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  IN EFI_LBA                                    Lba,
  IN UINTN                                      Offset,
  IN OUT UINTN                                  *NumBytes,
  OUT UINT8                                     *Buffer
  )
{
  MESON_SPIFC_FVB  *Instance;
  EFI_STATUS       Status;

  if ((NumBytes == NULL) || (Buffer == NULL) ||
      (Lba >= FVB_BLOCK_COUNT) || (Offset > VIM3_NOR_ERASE_SIZE))
  {
    return EFI_INVALID_PARAMETER;
  }

  Status = EFI_SUCCESS;
  if (*NumBytes > VIM3_NOR_ERASE_SIZE - Offset) {
    *NumBytes = VIM3_NOR_ERASE_SIZE - Offset;
    Status    = EFI_BAD_BUFFER_SIZE;
  }

  Instance = INSTANCE_FROM_FVB (This);
  if (!mFtwReadLogged &&
      (Lba == FVB_WORKING_FIRST_LBA) &&
      (Offset == 0) &&
      (*NumBytes >= sizeof (EFI_FAULT_TOLERANT_WORKING_BLOCK_HEADER)))
  {
    LogFtwHeader (
      "FVB read",
      Instance->Shadow + ((UINTN)Lba * VIM3_NOR_ERASE_SIZE)
      );
    mFtwReadLogged = TRUE;
  }

  CopyMem (
    Buffer,
    Instance->Shadow + ((UINTN)Lba * VIM3_NOR_ERASE_SIZE) + Offset,
    *NumBytes
    );
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
FvbWrite (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  IN EFI_LBA                                    Lba,
  IN UINTN                                      Offset,
  IN OUT UINTN                                  *NumBytes,
  IN UINT8                                      *Buffer
  )
{
  MESON_SPIFC_FVB  *Instance;
  UINT8            Verify[SPIFC_COMMAND_MAX_DATA];
  UINT8            *Destination;
  UINTN            Index;
  UINTN            Chunk;
  UINTN            Written;
  UINT32           NorAddress;
  EFI_STATUS       Status;

  if ((NumBytes == NULL) || (Buffer == NULL) ||
      (Lba >= FVB_BLOCK_COUNT) || (Offset > VIM3_NOR_ERASE_SIZE))
  {
    return EFI_INVALID_PARAMETER;
  }

  Instance = INSTANCE_FROM_FVB (This);
  if (!Instance->Writable || !LbaWritable (Lba)) {
    return EFI_WRITE_PROTECTED;
  }

  RestoreSpifcIo ();

  if (*NumBytes > VIM3_NOR_ERASE_SIZE - Offset) {
    *NumBytes = VIM3_NOR_ERASE_SIZE - Offset;
    return EFI_BAD_BUFFER_SIZE;
  }

  Destination = Instance->Shadow +
                ((UINTN)Lba * VIM3_NOR_ERASE_SIZE) + Offset;
  for (Index = 0; Index < *NumBytes; Index++) {
    if ((Destination[Index] & Buffer[Index]) != Buffer[Index]) {
      return EFI_ACCESS_DENIED;
    }
  }

  NorAddress = VIM3_NOR_RUNTIME_BASE +
               (UINT32)Lba * VIM3_NOR_ERASE_SIZE + (UINT32)Offset;

  //
  // ONE NorProgram call for the whole span: NorProgram splits internally
  // at page boundaries only, one CS-held program command per page.
  // Chunking here (the pre-fix code did 60-byte pieces) would re-create
  // the banned multiple-program-commands-per-page sequence for the
  // VARIABLE STORE.
  //
  Status = NorProgram (NorAddress, Buffer, *NumBytes);
  if (EFI_ERROR (Status)) {
    *NumBytes = 0;
    return Status;
  }

  Written = 0;
  while (Written < *NumBytes) {
    Chunk  = MIN (*NumBytes - Written, sizeof (Verify));
    Status = NorRead (NorAddress + (UINT32)Written, Verify, Chunk);
    if (EFI_ERROR (Status) ||
        (CompareMem (Verify, Buffer + Written, Chunk) != 0))
    {
      *NumBytes = Written;
      return EFI_DEVICE_ERROR;
    }

    CopyMem (Destination + Written, Buffer + Written, Chunk);
    Written += Chunk;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
FvbEraseBlocks (
  IN CONST EFI_FIRMWARE_VOLUME_BLOCK2_PROTOCOL  *This,
  ...
  )
{
  MESON_SPIFC_FVB  *Instance;
  UINT8            Verify[SPIFC_COMMAND_MAX_DATA];
  VA_LIST          Arguments;
  EFI_LBA          StartLba;
  UINTN            BlockCount;
  UINTN            Index;
  UINTN            Offset;
  UINTN            Chunk;
  UINT32           Address;
  EFI_STATUS       Status;

  Instance = INSTANCE_FROM_FVB (This);
  if (!Instance->Writable) {
    return EFI_WRITE_PROTECTED;
  }

  RestoreSpifcIo ();

  VA_START (Arguments, This);
  for ( ; ;) {
    StartLba = VA_ARG (Arguments, EFI_LBA);
    if (StartLba == EFI_LBA_LIST_TERMINATOR) {
      break;
    }

    BlockCount = VA_ARG (Arguments, UINTN);
    if ((BlockCount == 0) ||
        (StartLba >= FVB_BLOCK_COUNT) ||
        (BlockCount > FVB_BLOCK_COUNT - (UINTN)StartLba))
    {
      VA_END (Arguments);
      return EFI_INVALID_PARAMETER;
    }

    for (Index = 0; Index < BlockCount; Index++) {
      if (!LbaWritable (StartLba + Index)) {
        VA_END (Arguments);
        return EFI_WRITE_PROTECTED;
      }
    }
  }

  VA_END (Arguments);
  VA_START (Arguments, This);
  for ( ; ;) {
    StartLba = VA_ARG (Arguments, EFI_LBA);
    if (StartLba == EFI_LBA_LIST_TERMINATOR) {
      break;
    }

    BlockCount = VA_ARG (Arguments, UINTN);
    for (Index = 0; Index < BlockCount; Index++) {
      Address = VIM3_NOR_RUNTIME_BASE +
                (UINT32)(StartLba + Index) * VIM3_NOR_ERASE_SIZE;
      Status = NorEraseSector (Address);
      if (EFI_ERROR (Status)) {
        VA_END (Arguments);
        return Status;
      }

      for (Offset = 0; Offset < VIM3_NOR_ERASE_SIZE; Offset += Chunk) {
        Chunk  = MIN (sizeof (Verify), VIM3_NOR_ERASE_SIZE - Offset);
        Status = NorRead (Address + (UINT32)Offset, Verify, Chunk);
        if (EFI_ERROR (Status) ||
            !BufferIsErased (Verify, Chunk))
        {
          DEBUG ((
            DEBUG_ERROR,
            "MesonSPIFC: erase verify failed at 0x%08x: %r\n",
            Address + (UINT32)Offset,
            Status
            ));
          VA_END (Arguments);
          return EFI_DEVICE_ERROR;
        }
      }

      SetMem (
        Instance->Shadow +
        ((UINTN)(StartLba + Index) * VIM3_NOR_ERASE_SIZE),
        VIM3_NOR_ERASE_SIZE,
        0xFF
        );
    }
  }

  VA_END (Arguments);
  return EFI_SUCCESS;
}

//
// MESON_NOR_FLASH_PROTOCOL - raw chip access for the platform FMP, below
// the FVB.  Boot-services only: none of these pointers are converted at
// SetVirtualAddressMap, so each entry refuses to run at runtime rather
// than jump through a stale mapping.  Program/Erase stop short of the
// firmware-owned runtime region, which keeps the RAM shadow coherent by
// construction - nothing writable through this protocol overlaps it.
//

STATIC
EFI_STATUS
EFIAPI
NorFlashRead (
  IN  MESON_NOR_FLASH_PROTOCOL  *This,
  IN  UINT32                    Address,
  OUT VOID                      *Buffer,
  IN  UINTN                     Length
  )
{
  if (EfiAtRuntime ()) {
    return EFI_UNSUPPORTED;
  }

  RestoreSpifcIo ();
  return NorRead (Address, Buffer, Length);
}

STATIC
EFI_STATUS
EFIAPI
NorFlashProgram (
  IN MESON_NOR_FLASH_PROTOCOL  *This,
  IN UINT32                    Address,
  IN CONST VOID                *Buffer,
  IN UINTN                     Length
  )
{
  if (EfiAtRuntime ()) {
    return EFI_UNSUPPORTED;
  }

  if (!mSpifcFvb.Writable) {
    return EFI_WRITE_PROTECTED;
  }

  if ((Address >= VIM3_NOR_RUNTIME_BASE) ||
      (Length > VIM3_NOR_RUNTIME_BASE - Address))
  {
    return EFI_INVALID_PARAMETER;
  }

  RestoreSpifcIo ();
  return NorProgram (Address, Buffer, Length);
}

STATIC
EFI_STATUS
EFIAPI
NorFlashErase (
  IN MESON_NOR_FLASH_PROTOCOL  *This,
  IN UINT32                    Address,
  IN UINTN                     Length
  )
{
  EFI_STATUS  Status;

  if (EfiAtRuntime ()) {
    return EFI_UNSUPPORTED;
  }

  if (!mSpifcFvb.Writable) {
    return EFI_WRITE_PROTECTED;
  }

  if (((Address % VIM3_NOR_ERASE_SIZE) != 0) ||
      ((Length % VIM3_NOR_ERASE_SIZE) != 0) ||
      (Address >= VIM3_NOR_RUNTIME_BASE) ||
      (Length > VIM3_NOR_RUNTIME_BASE - Address))
  {
    return EFI_INVALID_PARAMETER;
  }

  RestoreSpifcIo ();

  while (Length > 0) {
    Status = NorEraseSector (Address);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Address += VIM3_NOR_ERASE_SIZE;
    Length  -= VIM3_NOR_ERASE_SIZE;
  }

  return EFI_SUCCESS;
}

STATIC
BOOLEAN
VariableHeadersValid (
  IN CONST UINT8  *Shadow
  )
{
  CONST EFI_FIRMWARE_VOLUME_HEADER  *FvHeader;
  CONST VARIABLE_STORE_HEADER       *VariableStore;

  FvHeader = (CONST EFI_FIRMWARE_VOLUME_HEADER *)Shadow;
  if ((FvHeader->Revision != EFI_FVH_REVISION) ||
      (FvHeader->Signature != EFI_FVH_SIGNATURE) ||
      (FvHeader->FvLength != VIM3_NOR_RUNTIME_SIZE) ||
      (FvHeader->HeaderLength !=
       OFFSET_OF (VIM3_VARIABLE_HEADER, VariableStore)) ||
      !CompareGuid (&FvHeader->FileSystemGuid, &gEfiSystemNvDataFvGuid) ||
      (CalculateSum16 (
         (CONST UINT16 *)FvHeader,
         FvHeader->HeaderLength
         ) != 0))
  {
    return FALSE;
  }

  VariableStore = (CONST VARIABLE_STORE_HEADER *)(
                    Shadow + FvHeader->HeaderLength
                    );
  return CompareGuid (
           &VariableStore->Signature,
           &gEfiAuthenticatedVariableGuid
           ) &&
         (VariableStore->Size ==
          VIM3_NOR_VARIABLE_SIZE - FvHeader->HeaderLength) &&
         (VariableStore->Format == VARIABLE_STORE_FORMATTED) &&
         (VariableStore->State == VARIABLE_STORE_HEALTHY);
}

STATIC
VOID
BuildVariableHeaders (
  OUT VIM3_VARIABLE_HEADER  *Headers
  )
{
  SetMem (Headers, sizeof (*Headers), 0xFF);
  ZeroMem (&Headers->FvHeader.ZeroVector, sizeof (Headers->FvHeader.ZeroVector));
  CopyGuid (
    &Headers->FvHeader.FileSystemGuid,
    &gEfiSystemNvDataFvGuid
    );
  Headers->FvHeader.FvLength     = VIM3_NOR_RUNTIME_SIZE;
  Headers->FvHeader.Signature    = EFI_FVH_SIGNATURE;
  Headers->FvHeader.Attributes   =
    EFI_FVB2_READ_ENABLED_CAP |
    EFI_FVB2_READ_STATUS |
    EFI_FVB2_WRITE_ENABLED_CAP |
    EFI_FVB2_WRITE_STATUS |
    EFI_FVB2_STICKY_WRITE |
    EFI_FVB2_MEMORY_MAPPED |
    EFI_FVB2_ERASE_POLARITY;
  //
  // Use the field offset rather than sizeof (VIM3_VARIABLE_HEADER) minus
  // sizeof (VARIABLE_STORE_HEADER): the containing structure has tail
  // padding, which is not part of the on-flash firmware-volume header.
  //
  Headers->FvHeader.HeaderLength =
    OFFSET_OF (VIM3_VARIABLE_HEADER, VariableStore);
  Headers->FvHeader.ExtHeaderOffset       = 0;
  Headers->FvHeader.Reserved[0]           = 0;
  Headers->FvHeader.Revision              = EFI_FVH_REVISION;
  Headers->FvHeader.BlockMap[0].NumBlocks = FVB_BLOCK_COUNT;
  Headers->FvHeader.BlockMap[0].Length    = VIM3_NOR_ERASE_SIZE;
  Headers->EndBlockMap.NumBlocks          = 0;
  Headers->EndBlockMap.Length             = 0;
  Headers->FvHeader.Checksum = 0;
  Headers->FvHeader.Checksum = CalculateCheckSum16 (
                                 (CONST UINT16 *)&Headers->FvHeader,
                                 Headers->FvHeader.HeaderLength
                                 );

  CopyGuid (
    &Headers->VariableStore.Signature,
    &gEfiAuthenticatedVariableGuid
    );
  Headers->VariableStore.Size =
    VIM3_NOR_VARIABLE_SIZE - Headers->FvHeader.HeaderLength;
  Headers->VariableStore.Format    = VARIABLE_STORE_FORMATTED;
  Headers->VariableStore.State     = VARIABLE_STORE_HEALTHY;
  Headers->VariableStore.Reserved  = 0;
  Headers->VariableStore.Reserved1 = 0;
}

STATIC
EFI_STATUS
FormatVariableStore (
  IN OUT MESON_SPIFC_FVB  *Instance
  )
{
  VIM3_VARIABLE_HEADER  Headers;
  EFI_STATUS            Status;
  UINT32                Address;
  UINT8                 Verify[sizeof (Headers)];

  //
  // Validate the complete synthesized header before the first destructive
  // operation.  A formatter regression must never erase a recoverable store.
  //
  BuildVariableHeaders (&Headers);
  if (!VariableHeadersValid ((CONST UINT8 *)&Headers)) {
    return EFI_COMPROMISED_DATA;
  }

  for (Address = VIM3_NOR_VARIABLE_BASE;
       Address < VIM3_NOR_VARIABLE_BASE + VIM3_NOR_VARIABLE_SIZE;
       Address += VIM3_NOR_ERASE_SIZE)
  {
    Status = NorEraseSector (Address);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  for (Address = VIM3_NOR_FTW_WORKING_BASE;
       Address < VIM3_NOR_FTW_WORKING_BASE + VIM3_NOR_FTW_WORKING_SIZE;
       Address += VIM3_NOR_ERASE_SIZE)
  {
    Status = NorEraseSector (Address);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  for (Address = VIM3_NOR_FTW_SPARE_BASE;
       Address < VIM3_NOR_FTW_SPARE_BASE + VIM3_NOR_FTW_SPARE_SIZE;
       Address += VIM3_NOR_ERASE_SIZE)
  {
    Status = NorEraseSector (Address);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  SetMem (Instance->Shadow, VIM3_NOR_VARIABLE_SIZE, 0xFF);
  SetMem (
    Instance->Shadow +
    (VIM3_NOR_FTW_WORKING_BASE - VIM3_NOR_RUNTIME_BASE),
    VIM3_NOR_FTW_WORKING_SIZE,
    0xFF
    );
  SetMem (
    Instance->Shadow +
    (VIM3_NOR_FTW_SPARE_BASE - VIM3_NOR_RUNTIME_BASE),
    VIM3_NOR_FTW_SPARE_SIZE,
    0xFF
    );

  Status = NorProgram (
             VIM3_NOR_VARIABLE_BASE,
             (CONST UINT8 *)&Headers,
             sizeof (Headers)
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = NorRead (
             VIM3_NOR_VARIABLE_BASE,
             Verify,
             sizeof (Verify)
             );
  if (EFI_ERROR (Status) ||
      (CompareMem (Verify, &Headers, sizeof (Headers)) != 0))
  {
    return EFI_DEVICE_ERROR;
  }

  CopyMem (Instance->Shadow, &Headers, sizeof (Headers));
  return EFI_SUCCESS;
}

STATIC
VOID
EFIAPI
VirtualAddressChange (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EfiConvertPointer (0, (VOID **)&mSpifcFvb.Shadow);
  EfiConvertPointer (0, (VOID **)&mSpifcFvb.Registers);
}

STATIC
EFI_STATUS
PrepareRuntimeMmio (
  VOID
  )
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR  Descriptor;
  EFI_STATUS                       Status;

  Status = gDS->AddMemorySpace (
                  EfiGcdMemoryTypeMemoryMappedIo,
                  MESON_G12B_SPIFC_BASE,
                  MESON_G12B_SPIFC_SIZE,
                  EFI_MEMORY_UC | EFI_MEMORY_RUNTIME
                  );
  if ((Status != EFI_SUCCESS) && (Status != EFI_ACCESS_DENIED)) {
    return Status;
  }

  Status = gDS->GetMemorySpaceDescriptor (
                  MESON_G12B_SPIFC_BASE,
                  &Descriptor
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return gDS->SetMemorySpaceAttributes (
                MESON_G12B_SPIFC_BASE,
                MESON_G12B_SPIFC_SIZE,
                Descriptor.Attributes | EFI_MEMORY_UC | EFI_MEMORY_RUNTIME
                );
}

EFI_STATUS
EFIAPI
MesonSpifcFvbDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE    *GuidHob;
  VARIABLE_FLASH_INFO  *FlashInfo;
  UINT8                IdTransmit[4];
  UINT8                IdReceive[4];
  UINT8                SecondRead[sizeof (VIM3_VARIABLE_HEADER)];
  UINT8                FlashStatus;
  EFI_STATUS           Status;

  STATIC_ASSERT (
    (VIM3_NOR_RUNTIME_SIZE % VIM3_NOR_ERASE_SIZE) == 0,
    "runtime shadow must contain whole erase blocks"
    );
  STATIC_ASSERT (
    VIM3_NOR_FTW_SPARE_SIZE >= VIM3_NOR_VARIABLE_SIZE,
    "FTW spare must cover the variable store"
    );
  STATIC_ASSERT (
    VIM3_NOR_BOARD_DATA_BASE ==
    VIM3_NOR_RUNTIME_BASE + VIM3_NOR_RUNTIME_SIZE,
    "runtime FVB must stop before board data"
    );

  ZeroMem (&mSpifcFvb, sizeof (mSpifcFvb));
  mSpifcFvb.Signature = MESON_SPIFC_SIGNATURE;
  mSpifcFvb.Registers = (UINT8 *)(UINTN)MESON_G12B_SPIFC_BASE;

  GuidHob = GetFirstGuidHob (&gVariableFlashInfoHobGuid);
  if (GuidHob == NULL) {
    return EFI_NOT_FOUND;
  }

  FlashInfo = GET_GUID_HOB_DATA (GuidHob);
  if ((FlashInfo->Version != VARIABLE_FLASH_INFO_HOB_VERSION) ||
      (FlashInfo->NvVariableLength != VIM3_NOR_VARIABLE_SIZE) ||
      (FlashInfo->FtwWorkingLength != VIM3_NOR_FTW_WORKING_SIZE) ||
      (FlashInfo->FtwSpareLength != VIM3_NOR_FTW_SPARE_SIZE))
  {
    return EFI_COMPROMISED_DATA;
  }

  mSpifcFvb.ShadowPhysical = FlashInfo->NvVariableBaseAddress;
  mSpifcFvb.Shadow = (UINT8 *)(UINTN)mSpifcFvb.ShadowPhysical;

  Status = PrepareRuntimeMmio ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MesonSPIFC: runtime MMIO registration: %r\n", Status));
    return Status;
  }

  ConfigureSpifcPins ();
  //
  // CLK81 is the always-on parent described by the G12B DT.  Divider 12
  // produces a conservative ~12.8 MHz bus from the observed 166.6 MHz CLK81.
  //
  ConfigureSpifcController ();

  IdTransmit[0] = NOR_CMD_READ_ID;
  IdTransmit[1] = 0xFF;
  IdTransmit[2] = 0xFF;
  IdTransmit[3] = 0xFF;
  Status = SpifcTransfer (IdTransmit, IdReceive, sizeof (IdTransmit));
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "MesonSPIFC: JEDEC %02x %02x %02x\n",
    IdReceive[1],
    IdReceive[2],
    IdReceive[3]
    ));
  if ((IdReceive[1] != VIM3_EXPECTED_ID0) ||
      (IdReceive[2] != VIM3_EXPECTED_ID1) ||
      (IdReceive[3] != VIM3_EXPECTED_ID2))
  {
    DEBUG ((DEBUG_ERROR, "MesonSPIFC: unexpected flash; refusing FVB\n"));
    return EFI_UNSUPPORTED;
  }

  Status = NorReadStatus (&FlashStatus);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mSpifcFvb.Writable = (FlashStatus & NOR_STATUS_PROTECTION) == 0;
  Status = NorRead (
             VIM3_NOR_RUNTIME_BASE,
             mSpifcFvb.Shadow,
             VIM3_NOR_RUNTIME_SIZE
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  LogFtwHeader (
    "NOR load",
    mSpifcFvb.Shadow +
    (VIM3_NOR_FTW_WORKING_BASE - VIM3_NOR_RUNTIME_BASE)
    );

  Status = NorRead (
             VIM3_NOR_VARIABLE_BASE,
             SecondRead,
             sizeof (SecondRead)
             );
  if (EFI_ERROR (Status) ||
      (CompareMem (SecondRead, mSpifcFvb.Shadow, sizeof (SecondRead)) != 0))
  {
    return EFI_DEVICE_ERROR;
  }

  if (!VariableHeadersValid (mSpifcFvb.Shadow)) {
    if (!mSpifcFvb.Writable) {
      DEBUG ((DEBUG_ERROR, "MesonSPIFC: invalid read-only variable store\n"));
      return EFI_WRITE_PROTECTED;
    }

    DEBUG ((DEBUG_WARN, "MesonSPIFC: formatting invalid variable store\n"));
    Status = FormatVariableStore (&mSpifcFvb);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  mSpifcFvb.Attributes =
    EFI_FVB2_READ_ENABLED_CAP |
    EFI_FVB2_READ_STATUS |
    EFI_FVB2_STICKY_WRITE |
    EFI_FVB2_MEMORY_MAPPED |
    EFI_FVB2_ERASE_POLARITY;
  if (mSpifcFvb.Writable) {
    mSpifcFvb.Attributes |=
      EFI_FVB2_WRITE_ENABLED_CAP | EFI_FVB2_WRITE_STATUS;
  }

  mSpifcFvb.Fvb.GetAttributes      = FvbGetAttributes;
  mSpifcFvb.Fvb.SetAttributes      = FvbSetAttributes;
  mSpifcFvb.Fvb.GetPhysicalAddress = FvbGetPhysicalAddress;
  mSpifcFvb.Fvb.GetBlockSize       = FvbGetBlockSize;
  mSpifcFvb.Fvb.Read               = FvbRead;
  mSpifcFvb.Fvb.Write              = FvbWrite;
  mSpifcFvb.Fvb.EraseBlocks        = FvbEraseBlocks;
  mSpifcFvb.Fvb.ParentHandle       = NULL;

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_NOTIFY,
                  VirtualAddressChange,
                  NULL,
                  &gEfiEventVirtualAddressChangeGuid,
                  &mSpifcFvb.VirtualAddressEvent
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mNorFlash.Read    = NorFlashRead;
  mNorFlash.Program = NorFlashProgram;
  mNorFlash.Erase   = NorFlashErase;

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mSpifcFvb.Handle,
                  &gEfiFirmwareVolumeBlockProtocolGuid,
                  &mSpifcFvb.Fvb,
                  &gMesonNorFlashProtocolGuid,
                  &mNorFlash,
                  NULL
                  );
  DEBUG ((DEBUG_INFO, "MesonSPIFC: runtime FVB installed: %r\n", Status));
  return Status;
}
