/** @file
  Common definitions for the Khadas VIM3 ACPI tables.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef VIM3_ACPI_TABLES_H_
#define VIM3_ACPI_TABLES_H_

#include <IndustryStandard/Acpi.h>

#define VIM3_ACPI_OEM_ID               { 'K', 'H', 'A', 'D', 'A', 'S' }
#define VIM3_ACPI_OEM_TABLE_ID         SIGNATURE_64 ('V', 'I', 'M', '3', 'A', 'C', 'P', 'I')
#define VIM3_ACPI_OEM_REVISION         0x00000001
#define VIM3_ACPI_CREATOR_ID           SIGNATURE_32 ('E', 'D', 'K', '2')
#define VIM3_ACPI_CREATOR_REVISION     0x00000001

#define VIM3_ACPI_HEADER(Signature, Type, Revision) { \
  Signature,                                          \
  sizeof (Type),                                      \
  Revision,                                           \
  0,                                                  \
  VIM3_ACPI_OEM_ID,                                   \
  VIM3_ACPI_OEM_TABLE_ID,                             \
  VIM3_ACPI_OEM_REVISION,                             \
  VIM3_ACPI_CREATOR_ID,                               \
  VIM3_ACPI_CREATOR_REVISION                          \
}

#define VIM3_GIC_DISTRIBUTOR_BASE    0xFFC01000ULL
#define VIM3_GIC_CPU_INTERFACE_BASE  0xFFC02000ULL
//
// GIC-400 virtualization interface.  The firmware hands the OS off at EL2
// (Linux reports "All CPU(s) started at EL2" and brings up KVM), so an EL2
// OS that runs a hypervisor - Windows ARM64 launches Hyper-V/VBS at EL2 by
// default - needs the GICv2 virtual CPU interface (GICV) and hypervisor
// control (GICH) described, plus the VGIC maintenance interrupt.  A Linux
// *host* runs without these (it only needs them to launch guests), which is
// why Linux boots on this ACPI but Windows stalls before its kernel.
// Addresses and GSIV are the board's ground truth (DT reg + KVM "vgic
// interrupt IRQ9" == PPI 9 == GSIV 25).
//
#define VIM3_GIC_HYPERVISOR_BASE     0xFFC04000ULL
#define VIM3_GIC_VIRTUAL_BASE        0xFFC06000ULL
#define VIM3_GIC_MAINTENANCE_GSIV    25U

#define VIM3_TIMER_SECURE_GSIV       29U
#define VIM3_TIMER_NON_SECURE_GSIV   30U
#define VIM3_TIMER_VIRTUAL_GSIV      27U
#define VIM3_TIMER_HYPERVISOR_GSIV   26U

#define VIM3_XHCI_BASE               0xFF500000U
#define VIM3_XHCI_SIZE               0x00100000U
#define VIM3_XHCI_GSIV               62U

#endif
