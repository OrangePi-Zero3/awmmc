/*++

Module Name:

    smhc.h

Abstract:

    Allwinner SMHC (H616/H618) SD/MMC miniport for sdport.sys.

    Structure follows Microsoft's SDHC miniport sample
    (Windows-driver-samples/sd/miniport/sdhc/sdhc.c); the hardware layer is
    replaced by the Allwinner SMHC register set (smhc_regs.h).  dwcmshc.cpp
    (a DesignWare-MMC miniport for sdport) was used as the structural reference
    for non-SDHCI controllers.

Environment:

    Kernel mode only.

--*/

#pragma once

#include <ntddk.h>
#include <sdport.h>
#if defined(_ARM64_) || defined(_M_ARM64)
#include <intrin.h>
#endif

#include "smhc_core.h"

//
// ---------------------------------------------------------------------------
// Tunables / defaults.
// ---------------------------------------------------------------------------
//

//
// Module clock the firmware leaves programmed for SMHC0 (mu-silicium: 24 MHz,
// OSC24M).  The driver does not own the CCU in this version.  Overridable with
// the ModuleClockHz registry value, see smhc.inx.
//
#define SMHC_DEFAULT_MODULE_CLOCK_HZ    24000000UL

//
// Highest card clock the driver will ever request, kHz.  The effective maximum is
// also bounded by the module clock (no CCU access => no faster clock than the
// firmware provides).
//
#define SMHC_DEFAULT_MAX_FREQUENCY_KHZ  50000UL

//
// Transfer mode, registry value TransferMode:
//   0 = PIO (CPU <-> FIFO) only        -- bring-up default, no cache dependence
//   1 = IDMAC scatter/gather DMA
//
#define SMHC_TRANSFER_MODE_PIO          0UL
#define SMHC_TRANSFER_MODE_DMA          1UL
#define SMHC_DEFAULT_TRANSFER_MODE      SMHC_TRANSFER_MODE_PIO

#define SMHC_MAX_BLOCK_SIZE             512
#define SMHC_MAX_BLOCK_COUNT            8192        // [LX] mmc->max_blk_count

//
// Timeouts (microseconds).
//
#define SMHC_RESET_TIMEOUT_US           250000      // [LX] sunxi_mmc_reset_host: 250 ms
#define SMHC_CLOCK_UPDATE_TIMEOUT_US    750000      // [LX] sunxi_mmc_oclk_onoff: 750 ms
#define SMHC_STOP_TIMEOUT_US            1000000     // [LX] sunxi_mmc_send_manual_stop: 1 s
#define SMHC_BUSY_TIMEOUT_US            2000000     // [UB-C] R1b busy: 2 s
#define SMHC_REMOVABLE_SETTLE_US        10000       // sdhc.c SdhcSetClock: 10 ms for removable

//
// SD command indices used locally (sdport.h exposes SDCMD_* in some WDK
// versions; defining our own keeps the build independent of that).
//
#define SMHC_SDCMD_GO_IDLE_STATE        0
#define SMHC_SDCMD_STOP_TRANSMISSION    12

//
// ---------------------------------------------------------------------------
// Logging.  DbgPrintEx under DPFLTR_IHVDRIVER_ID; enable in WinDbg with
//     ed nt!Kd_IHVDRIVER_Mask 0xF
// Every line starts with "SMHC:".  Levels are DbgPrintEx bit positions, so the
// mask selects them:  0x1 errors, 0x2 +warnings, 0x4 +info, 0x8 +trace (ISR,
// register-level); use 0xF for everything, 0x3 for quiet operation.
// ---------------------------------------------------------------------------
//

#define SMHC_LOG_ERROR_LEVEL            DPFLTR_ERROR_LEVEL
#define SMHC_LOG_WARN_LEVEL             DPFLTR_WARNING_LEVEL
#define SMHC_LOG_INFO_LEVEL             DPFLTR_TRACE_LEVEL      // bit 2
#define SMHC_LOG_TRACE_LEVEL            DPFLTR_INFO_LEVEL       // bit 3

#define SMHC_LOG(_Level, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, (_Level), "SMHC: " __VA_ARGS__)

#define SMHC_LOG_ERROR(...)             SMHC_LOG(SMHC_LOG_ERROR_LEVEL, __VA_ARGS__)
#define SMHC_LOG_WARN(...)              SMHC_LOG(SMHC_LOG_WARN_LEVEL, __VA_ARGS__)
#define SMHC_LOG_INFO(...)              SMHC_LOG(SMHC_LOG_INFO_LEVEL, __VA_ARGS__)
#define SMHC_LOG_TRACE(...)             SMHC_LOG(SMHC_LOG_TRACE_LEVEL, __VA_ARGS__)

//
// ---------------------------------------------------------------------------
// Slot extension.
// ---------------------------------------------------------------------------
//

#define MINIPORT_FROM_SLOTEXT(_SlotExtension) \
    (CONTAINING_RECORD(_SlotExtension, SDPORT_SLOT_EXTENSION, PrivateExtension)->Miniport)

typedef struct _SMHC_EXTENSION {

    //
    // Register space.
    //
    PHYSICAL_ADDRESS PhysicalBaseAddress;
    PVOID BaseAddress;
    ULONG BaseAddressSpaceSize;

    PDEVICE_OBJECT Fdo;
    BOOLEAN CrashdumpMode;
    BOOLEAN Initialized;                    // SlotInitialize cannot fail; see dwcmshc.cpp

    //
    // Configuration (defaults overridden from the device registry key).
    //
    ULONG ModuleClockHz;
    ULONG MaxFrequencyKhz;
    ULONG TransferMode;

    SDPORT_CAPABILITIES Capabilities;

    //
    // Cached bus state, re-applied after every controller reset.
    //
    ULONG Clkcr;                            // CLKCR without the clock-on bits
    BOOLEAN ClockEnabled;
    ULONG BusFrequencyKhz;
    ULONG BusWidthReg;                      // SMHC_WIDTH_*

    //
    // Shadow of IMASK.  IMASK is modified from the ISR (data-request masking),
    // from sdport's ToggleEvents and from request issue; all changes go through
    // SmhcImaskUpdate so a stale read-modify-write cannot drop a bit.
    //
    volatile LONG ImaskShadow;

    //
    // Request tracking.  One request at a time (MaximumOutstandingRequests = 1).
    //
    PSDPORT_REQUEST volatile OutstandingRequest;
    volatile LONG CurrentEvents;
    volatile LONG CurrentErrors;
    volatile BOOLEAN AutoStopActive;        // current data command uses AUTO_STOP (ISR reads this)
    volatile LONG BusyPending;              // completion deferred to the busy work item
    volatile LONG NeedStop;                 // a data request failed: card still in the data state
    volatile LONG PhaseClaim;               // 1 = the current request phase is being completed (DPC vs StartTransfer)

    //
    // Data phase.
    //
    BOOLEAN DmaActive;
    ULONG PioWordsRemaining;
    ULONG PioThreshold;

    PIO_WORKITEM BusyWorkItem;

} SMHC_EXTENSION, *PSMHC_EXTENSION;

//
// ---------------------------------------------------------------------------
// Register access.  Always 32-bit (the SMHC register file is word-accessed in
// every reference).
// ---------------------------------------------------------------------------
//

static __forceinline ULONG
SmhcRead(
    _In_ PSMHC_EXTENSION Ext,
    _In_ ULONG Register
    )
{
    return SdPortReadRegisterUlong(Ext->BaseAddress, Register);
}

static __forceinline VOID
SmhcWrite(
    _In_ PSMHC_EXTENSION Ext,
    _In_ ULONG Register,
    _In_ ULONG Value
    )
{
    SdPortWriteRegisterUlong(Ext->BaseAddress, Register, Value);
}

//
// Make descriptor / buffer writes (normal memory) visible before the doorbell
// write (device memory).  [LX] sunxi_mmc_init_idma_des ends with wmb() for the
// same reason.
//
static __forceinline VOID
SmhcDmaBarrier(
    VOID
    )
{
#if defined(_ARM64_) || defined(_M_ARM64)
    __dsb(_ARM64_BARRIER_SY);
#else
    KeMemoryBarrier();
#endif
}

//
// ---------------------------------------------------------------------------
// sdport callbacks (smhc.c).
// ---------------------------------------------------------------------------
//

DRIVER_INITIALIZE DriverEntry;

NTSTATUS
SmhcGetSlotCount(
    _In_ PSD_MINIPORT Miniport,
    _Out_ PUCHAR SlotCount
    );

VOID
SmhcGetSlotCapabilities(
    _In_ PVOID PrivateExtension,
    _Out_ PSDPORT_CAPABILITIES Capabilities
    );

NTSTATUS
SmhcSlotInitialize(
    _In_ PVOID PrivateExtension,
    _In_ PHYSICAL_ADDRESS PhysicalBase,
    _In_ PVOID VirtualBase,
    _In_ ULONG Length,
    _In_ BOOLEAN CrashdumpMode
    );

NTSTATUS
SmhcSlotIssueBusOperation(
    _In_ PVOID PrivateExtension,
    _In_ PSDPORT_BUS_OPERATION BusOperation
    );

BOOLEAN
SmhcSlotGetCardDetectState(
    _In_ PVOID PrivateExtension
    );

BOOLEAN
SmhcSlotGetWriteProtectState(
    _In_ PVOID PrivateExtension
    );

BOOLEAN
SmhcSlotInterrupt(
    _In_ PVOID PrivateExtension,
    _Out_ PULONG Events,
    _Out_ PULONG Errors,
    _Out_ PBOOLEAN CardChange,
    _Out_ PBOOLEAN SdioInterrupt,
    _Out_ PBOOLEAN Tuning
    );

NTSTATUS
SmhcSlotIssueRequest(
    _In_ PVOID PrivateExtension,
    _In_ PSDPORT_REQUEST Request
    );

VOID
SmhcSlotGetResponse(
    _In_ PVOID PrivateExtension,
    _In_ PSDPORT_COMMAND Command,
    _Out_ PVOID ResponseBuffer
    );

VOID
SmhcSlotToggleEvents(
    _In_ PVOID PrivateExtension,
    _In_ ULONG EventMask,
    _In_ BOOLEAN Enable
    );

VOID
SmhcSlotClearEvents(
    _In_ PVOID PrivateExtension,
    _In_ ULONG EventMask
    );

VOID
SmhcRequestDpc(
    _In_ PVOID PrivateExtension,
    _Inout_ PSDPORT_REQUEST Request,
    _In_ ULONG Events,
    _In_ ULONG Errors
    );

VOID
SmhcSaveContext(
    _In_ PVOID PrivateExtension
    );

VOID
SmhcRestoreContext(
    _In_ PVOID PrivateExtension
    );

NTSTATUS
SmhcPoFxPowerControlCallback(
    _In_ PSD_MINIPORT Miniport,
    _In_ LPCGUID PowerControlCode,
    _In_reads_bytes_opt_(InputBufferSize) PVOID InputBuffer,
    _In_ SIZE_T InputBufferSize,
    _Out_writes_bytes_opt_(OutputBufferSize) PVOID OutputBuffer,
    _In_ SIZE_T OutputBufferSize,
    _Out_opt_ PSIZE_T BytesReturned
    );

VOID
SmhcCleanup(
    _In_ PSD_MINIPORT Miniport
    );

//
// ---------------------------------------------------------------------------
// Hardware layer (smhc_hw.c).
// ---------------------------------------------------------------------------
//

VOID
SmhcLoadConfiguration(
    _Inout_ PSMHC_EXTENSION Ext
    );

NTSTATUS
SmhcResetHost(
    _In_ PSMHC_EXTENSION Ext,
    _In_ SDPORT_RESET_TYPE ResetType
    );

NTSTATUS
SmhcSetClock(
    _In_ PSMHC_EXTENSION Ext,
    _In_ ULONG FrequencyKhz
    );

NTSTATUS
SmhcSetBusWidth(
    _In_ PSMHC_EXTENSION Ext,
    _In_ SDPORT_BUS_WIDTH Width
    );

VOID
SmhcImaskUpdate(
    _In_ PSMHC_EXTENSION Ext,
    _In_ ULONG SetBits,
    _In_ ULONG ClearBits
    );

VOID
SmhcRestoreRegisters(
    _In_ PSMHC_EXTENSION Ext
    );

NTSTATUS
SmhcSendCommand(
    _In_ PSMHC_EXTENSION Ext,
    _In_ PSDPORT_REQUEST Request
    );

NTSTATUS
SmhcStartTransfer(
    _In_ PSMHC_EXTENSION Ext,
    _In_ PSDPORT_REQUEST Request
    );

VOID
SmhcCompleteRequest(
    _In_ PSMHC_EXTENSION Ext,
    _In_ PSDPORT_REQUEST Request,
    _In_ NTSTATUS Status
    );

VOID
SmhcGetResponse(
    _In_ PSMHC_EXTENSION Ext,
    _In_ PSDPORT_COMMAND Command,
    _Out_ PVOID ResponseBuffer
    );

NTSTATUS
SmhcStopDmaAndFifo(
    _In_ PSMHC_EXTENSION Ext
    );

NTSTATUS
SmhcSendStopCommandPolled(
    _In_ PSMHC_EXTENSION Ext
    );

NTSTATUS
SmhcConvertErrorToStatus(
    _In_ ULONG Errors
    );
