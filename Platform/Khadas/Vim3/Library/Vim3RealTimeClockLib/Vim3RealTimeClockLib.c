/** @file
  EFI RealTimeClock services backed by the VIM3's HYM8563.

  ARCHITECTURE - boot-time anchored, deliberately.

  The obvious implementation talks to the RTC on every GetTime call.  That
  would mean touching AO I2C MMIO after ExitBootServices, which this
  platform learned the hard way not to do (see the phase-1 runtime-MMIO
  finding), and it would put firmware on the same I2C bus that Linux's
  rtc-hym8563 driver owns once it boots - two masters, no arbitration.

  So the chip is read exactly once, at initialization.  That reading anchors
  an epoch, and GetTime serves time from the architected counter's advance
  since the anchor.  The counter is a system register, so GetTime is safe at
  runtime and needs no virtual-address fixups.  The cost is that firmware
  will not observe an RTC changed behind its back mid-boot, which nothing
  does.

  SetTime writes the chip, but only before ExitBootServices; afterwards the
  OS owns the device (Linux binds it as a normal rtc class device) and
  firmware returns EFI_UNSUPPORTED rather than fighting for the bus.

  TIME ZONES.  The chip stores wall-clock fields with no zone information.
  This library stores exactly what SetTime is given and reports
  EFI_UNSPECIFIED_TIMEZONE, which is what the OS convention on this board
  expects (Debian keeps the RTC in UTC).  No sign conversion is applied in
  either direction, so there is no convention to get backwards.

  Register definitions are from the public HYM8563 datasheet.

  DEBUGGING.  Vim3.dsc binds DebugLib to BaseDebugLibNull for every runtime
  driver, so none of the DEBUG output below reaches the serial console in a
  normal build.  It is kept for whoever temporarily swaps that binding.  The
  observable oracles are the shell's `date`/`time`, and, once Linux is up,
  its rtc-efi device (this library) next to its rtc-hym8563 device (the same
  chip, decoded independently).

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/RealTimeClockLib.h>
#include <Library/TimeBaseLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiRuntimeLib.h>
#include <Library/Vim3AoI2cLib.h>

#define HYM8563_I2C_ADDRESS  0x51

#define HYM8563_REG_CTL1  0x00
#define HYM8563_REG_CTL2  0x01
#define HYM8563_REG_TIME  0x02

///
/// CTL1 bit 5 halts the counter chain so a multi-byte time write cannot be
/// torn by a carry landing between two register writes.
///
#define HYM8563_CTL1_STOP  BIT5

///
/// Seconds bit 7 is the voltage-low flag.  The chip sets it whenever its
/// supply has dropped far enough to make the count untrustworthy, and it
/// stays set until the seconds register is written.  A board with no RTC
/// battery fitted comes up with this set on every cold boot.
///
#define HYM8563_SEC_VL  BIT7

///
/// Month bit 7 is the century flag: clear means 20xx, set means 21xx.
///
#define HYM8563_MON_CENTURY  BIT7

#define HYM8563_SEC_MASK      0x7F
#define HYM8563_MIN_MASK      0x7F
#define HYM8563_HOUR_MASK     0x3F
#define HYM8563_DAY_MASK      0x3F
#define HYM8563_WEEKDAY_MASK  0x07
#define HYM8563_MONTH_MASK    0x1F

///
/// Index into the seven-byte block starting at HYM8563_REG_TIME.
///
#define TIME_SEC      0
#define TIME_MIN      1
#define TIME_HOUR     2
#define TIME_DAY      3
#define TIME_WEEKDAY  4
#define TIME_MONTH    5
#define TIME_YEAR     6
#define TIME_BYTES    7

STATIC BOOLEAN  mRtcPresent;
STATIC UINT64   mEpochAnchor;
STATIC UINT64   mCounterAnchor;
STATIC UINT64   mCounterFrequency;

/**
  Convert the chip's seven time registers into an EFI_TIME.

  @param[in]  Registers  Seven bytes read from HYM8563_REG_TIME.
  @param[out] Time       Receives the decoded wall clock.
**/
STATIC
VOID
RegistersToTime (
  IN  CONST UINT8  *Registers,
  OUT EFI_TIME     *Time
  )
{
  ZeroMem (Time, sizeof (*Time));

  Time->Second = BcdToDecimal8 (Registers[TIME_SEC] & HYM8563_SEC_MASK);
  Time->Minute = BcdToDecimal8 (Registers[TIME_MIN] & HYM8563_MIN_MASK);
  Time->Hour   = BcdToDecimal8 (Registers[TIME_HOUR] & HYM8563_HOUR_MASK);
  Time->Day    = BcdToDecimal8 (Registers[TIME_DAY] & HYM8563_DAY_MASK);
  Time->Month  = BcdToDecimal8 (Registers[TIME_MONTH] & HYM8563_MONTH_MASK);
  Time->Year   = (UINT16)(2000 + BcdToDecimal8 (Registers[TIME_YEAR]));

  if ((Registers[TIME_MONTH] & HYM8563_MON_CENTURY) != 0) {
    Time->Year += 100;
  }

  Time->TimeZone = EFI_UNSPECIFIED_TIMEZONE;
  Time->Daylight = 0;
}

/**
  Encode an EFI_TIME into the chip's seven time registers.
**/
STATIC
VOID
TimeToRegisters (
  IN  CONST EFI_TIME  *Time,
  OUT UINT8           *Registers
  )
{
  UINT16  Year;

  Year = Time->Year;

  Registers[TIME_SEC]  = DecimalToBcd8 (Time->Second);
  Registers[TIME_MIN]  = DecimalToBcd8 (Time->Minute);
  Registers[TIME_HOUR] = DecimalToBcd8 (Time->Hour);
  Registers[TIME_DAY]  = DecimalToBcd8 (Time->Day);
  //
  // The chip numbers weekdays 0-6 from Sunday, which is what EfiTimeToWday
  // returns.  Nothing in EFI_TIME carries a weekday, so it is derived.
  //
  Registers[TIME_WEEKDAY] = (UINT8)EfiTimeToWday ((EFI_TIME *)Time);
  Registers[TIME_MONTH]   = DecimalToBcd8 (Time->Month);

  if (Year >= 2100) {
    Registers[TIME_MONTH] |= HYM8563_MON_CENTURY;
    Year                  -= 100;
  }

  Registers[TIME_YEAR] = DecimalToBcd8 ((UINT8)(Year - 2000));
}

/**
  Re-anchor the software clock on an epoch value.
**/
STATIC
VOID
AnchorEpoch (
  IN UINT64  EpochSeconds
  )
{
  mEpochAnchor   = EpochSeconds;
  mCounterAnchor = GetPerformanceCounter ();
}

/**
  Read the chip and anchor from it.

  @retval EFI_SUCCESS       The chip answered, whether or not the value it
                            held was valid.
  @retval other             The chip did not answer.
**/
STATIC
EFI_STATUS
AnchorFromRtc (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT8       Registers[TIME_BYTES];
  EFI_TIME    Time;

  Status = Vim3AoI2cRead (
             HYM8563_I2C_ADDRESS,
             HYM8563_REG_TIME,
             Registers,
             sizeof (Registers)
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mRtcPresent = TRUE;

  if ((Registers[TIME_SEC] & HYM8563_SEC_VL) != 0) {
    //
    // The chip is telling us its own count is meaningless.  Keep the build
    // epoch as the anchor so time still advances monotonically; a later
    // SetTime clears the flag in the chip and makes it trustworthy again.
    //
    DEBUG ((
      DEBUG_WARN,
      "Vim3Rtc: HYM8563 reports voltage-low; its time is not trustworthy "
      "(is an RTC battery fitted?)\n"
      ));
    return EFI_SUCCESS;
  }

  RegistersToTime (Registers, &Time);

  if (!IsTimeValid (&Time)) {
    DEBUG ((
      DEBUG_WARN,
      "Vim3Rtc: HYM8563 holds an invalid date %04u-%02u-%02u %02u:%02u:%02u\n",
      Time.Year,
      Time.Month,
      Time.Day,
      Time.Hour,
      Time.Minute,
      Time.Second
      ));
    return EFI_SUCCESS;
  }

  AnchorEpoch (EfiTimeToEpoch (&Time));

  DEBUG ((
    DEBUG_INFO,
    "Vim3Rtc: anchored on HYM8563 %04u-%02u-%02u %02u:%02u:%02u UTC\n",
    Time.Year,
    Time.Month,
    Time.Day,
    Time.Hour,
    Time.Minute,
    Time.Second
    ));

  return EFI_SUCCESS;
}

/**
  Returns the current time and date, and the time-keeping capabilities.

  @param[out] Time          Receives the current time.
  @param[out] Capabilities  Optionally receives the clock's capabilities.

  @retval EFI_SUCCESS            The time was returned.
  @retval EFI_INVALID_PARAMETER  Time is NULL.
  @retval EFI_DEVICE_ERROR       The counter frequency is unknown.
**/
EFI_STATUS
EFIAPI
LibGetTime (
  OUT EFI_TIME               *Time,
  OUT EFI_TIME_CAPABILITIES  *Capabilities
  )
{
  UINT64  Elapsed;

  if (Time == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (mCounterFrequency == 0) {
    return EFI_DEVICE_ERROR;
  }

  //
  // No DEBUG or MMIO on this path: it is reachable after ExitBootServices,
  // where the only safe sources are this module's data and the architected
  // counter.
  //
  Elapsed = DivU64x64Remainder (
              GetPerformanceCounter () - mCounterAnchor,
              mCounterFrequency,
              NULL
              );

  EpochToEfiTime ((UINTN)(mEpochAnchor + Elapsed), Time);

  Time->TimeZone = EFI_UNSPECIFIED_TIMEZONE;
  Time->Daylight = 0;

  if (Capabilities != NULL) {
    //
    // The chip ticks at 1 Hz and this library resolves to whole seconds.
    // SetsToZero is FALSE: SetTime writes the seconds field it is given.
    //
    Capabilities->Resolution = 1;
    Capabilities->Accuracy   = 0;
    Capabilities->SetsToZero = FALSE;
  }

  return EFI_SUCCESS;
}

/**
  Sets the current local time and date.

  @param[in] Time  The time to set.

  @retval EFI_SUCCESS            The time was set.
  @retval EFI_INVALID_PARAMETER  Time is NULL or describes an invalid date.
  @retval EFI_UNSUPPORTED        Called after ExitBootServices, or no chip.
  @retval EFI_DEVICE_ERROR       The chip did not accept the transfer.
**/
EFI_STATUS
EFIAPI
LibSetTime (
  IN EFI_TIME  *Time
  )
{
  EFI_STATUS  Status;
  UINT8       Registers[TIME_BYTES];
  UINT8       Control;

  if (Time == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (!IsTimeValid (Time)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // The OS owns this I2C bus once it has taken over.  Refuse rather than
  // contend with it - and refuse before touching any MMIO, which is the
  // rule that matters most here.
  //
  if (EfiAtRuntime () || !mRtcPresent) {
    return EFI_UNSUPPORTED;
  }

  TimeToRegisters (Time, Registers);

  //
  // Halt the counter across the write so a carry cannot land between the
  // seconds and minutes registers, then release it.
  //
  Status = Vim3AoI2cRead (
             HYM8563_I2C_ADDRESS,
             HYM8563_REG_CTL1,
             &Control,
             sizeof (Control)
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Control |= HYM8563_CTL1_STOP;
  Status   = Vim3AoI2cWrite (
               HYM8563_I2C_ADDRESS,
               HYM8563_REG_CTL1,
               &Control,
               sizeof (Control)
               );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Writing the seconds register with bit 7 clear is also what clears the
  // voltage-low flag, so a successful SetTime makes the chip trustworthy.
  //
  Status = Vim3AoI2cWrite (
             HYM8563_I2C_ADDRESS,
             HYM8563_REG_TIME,
             Registers,
             sizeof (Registers)
             );

  Control &= ~HYM8563_CTL1_STOP;
  Vim3AoI2cWrite (
    HYM8563_I2C_ADDRESS,
    HYM8563_REG_CTL1,
    &Control,
    sizeof (Control)
    );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Vim3Rtc: HYM8563 time write failed: %r\n", Status));
    return Status;
  }

  AnchorEpoch (EfiTimeToEpoch (Time));

  return EFI_SUCCESS;
}

/**
  Returns the current wakeup alarm clock setting.

  The HYM8563 has an alarm, but nothing on this platform wires its interrupt
  to a wake source, so reporting one would be a lie.

  @retval EFI_UNSUPPORTED  Always.
**/
EFI_STATUS
EFIAPI
LibGetWakeupTime (
  OUT BOOLEAN   *Enabled,
  OUT BOOLEAN   *Pending,
  OUT EFI_TIME  *Time
  )
{
  return EFI_UNSUPPORTED;
}

/**
  Sets the system wakeup alarm clock time.

  @retval EFI_UNSUPPORTED  Always; see LibGetWakeupTime.
**/
EFI_STATUS
EFIAPI
LibSetWakeupTime (
  IN BOOLEAN    Enabled,
  OUT EFI_TIME  *Time
  )
{
  return EFI_UNSUPPORTED;
}

/**
  Initialize the RTC.

  @param[in] ImageHandle  The image handle, unused.
  @param[in] SystemTable  The system table, unused.

  @retval EFI_SUCCESS  Time services are usable.  This never fails: a board
                       whose RTC does not answer still gets a monotonic
                       clock anchored on the build epoch, which is strictly
                       better than refusing to install the arch protocol.
**/
EFI_STATUS
EFIAPI
LibRtcInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  mCounterFrequency = GetPerformanceCounterProperties (NULL, NULL);
  ASSERT (mCounterFrequency != 0);

  AnchorEpoch (BUILD_EPOCH);

  Vim3AoI2cInitialize ();

  Status = AnchorFromRtc ();
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "Vim3Rtc: no answer from HYM8563 at 0x%02x (%r); running on the build "
      "epoch\n",
      HYM8563_I2C_ADDRESS,
      Status
      ));
  }

  return EFI_SUCCESS;
}
