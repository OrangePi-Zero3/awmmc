/*++

Module Name:

    smhc.c

Abstract:

    sdport miniport entry points for the Allwinner SMHC (ACPI\AWMC0001).

    One-to-one with the callback table of Microsoft's SDHC miniport sample
    (sd/miniport/sdhc/sdhc.c); the register-level work lives in smhc_hw.c.

--*/

#include "smhc.h"

#ifdef ALLOC_PRAGMA
    #pragma alloc_text(INIT, DriverEntry)
#endif

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    SDPORT_INITIALIZATION_DATA InitializationData;

    RtlZeroMemory(&InitializationData, sizeof(InitializationData));
    InitializationData.StructureSize = sizeof(InitializationData);

    InitializationData.GetSlotCount = SmhcGetSlotCount;
    InitializationData.GetSlotCapabilities = SmhcGetSlotCapabilities;
    InitializationData.Initialize = SmhcSlotInitialize;
    InitializationData.IssueBusOperation = SmhcSlotIssueBusOperation;
    InitializationData.GetCardDetectState = SmhcSlotGetCardDetectState;
    InitializationData.GetWriteProtectState = SmhcSlotGetWriteProtectState;
    InitializationData.Interrupt = SmhcSlotInterrupt;
    InitializationData.IssueRequest = SmhcSlotIssueRequest;
    InitializationData.GetResponse = SmhcSlotGetResponse;
    InitializationData.ToggleEvents = SmhcSlotToggleEvents;
    InitializationData.ClearEvents = SmhcSlotClearEvents;
    InitializationData.RequestDpc = SmhcRequestDpc;
    InitializationData.SaveContext = SmhcSaveContext;
    InitializationData.RestoreContext = SmhcRestoreContext;
    InitializationData.PowerControlCallback = SmhcPoFxPowerControlCallback;
    InitializationData.Cleanup = SmhcCleanup;

    InitializationData.PrivateExtensionSize = sizeof(SMHC_EXTENSION);

    //
    // Crash-dump / hibernate support needs a polled path with no allocations and
    // no work items; not implemented yet.
    //
    InitializationData.CrashdumpSupported = FALSE;

    SMHC_LOG_INFO("DriverEntry\n");
    return SdPortInitialize(DriverObject, RegistryPath, &InitializationData);
}

NTSTATUS
SmhcGetSlotCount(
    _In_ PSD_MINIPORT Miniport,
    _Out_ PUCHAR SlotCount
    )
{
    //
    // sdhc.c SdhcGetSlotCount: "We don't currently have a mechanism to query the
    // slot count for ACPI enumerated host controllers. Default to one slot."
    //
    switch (Miniport->ConfigurationInfo.BusType) {
    case SdBusTypeAcpi:
        *SlotCount = 1;
        return STATUS_SUCCESS;

    default:
        *SlotCount = 0;
        return STATUS_INVALID_PARAMETER;
    }
}

VOID
SmhcGetSlotCapabilities(
    _In_ PVOID PrivateExtension,
    _Out_ PSDPORT_CAPABILITIES Capabilities
    )
{
    PSMHC_EXTENSION Ext;

    Ext = (PSMHC_EXTENSION)PrivateExtension;
    RtlCopyMemory(Capabilities, &Ext->Capabilities, sizeof(Ext->Capabilities));
}

NTSTATUS
SmhcSlotInitialize(
    _In_ PVOID PrivateExtension,
    _In_ PHYSICAL_ADDRESS PhysicalBase,
    _In_ PVOID VirtualBase,
    _In_ ULONG Length,
    _In_ BOOLEAN CrashdumpMode
    )
{
    PSMHC_EXTENSION Ext;
    PSDPORT_CAPABILITIES Capabilities;
    PSD_MINIPORT Miniport;
    NTSTATUS Status;

    Ext = (PSMHC_EXTENSION)PrivateExtension;

    RtlZeroMemory(Ext, sizeof(*Ext));
    Ext->PhysicalBaseAddress = PhysicalBase;
    Ext->BaseAddress = VirtualBase;
    Ext->BaseAddressSpaceSize = Length;
    Ext->CrashdumpMode = CrashdumpMode;
    Ext->BusWidthReg = SMHC_WIDTH_1BIT;

    Miniport = MINIPORT_FROM_SLOTEXT(PrivateExtension);
    Ext->Fdo = (PDEVICE_OBJECT)Miniport->ConfigurationInfo.DeviceObject;

    SmhcLoadConfiguration(Ext);

    SMHC_LOG_INFO("slot init: phys 0x%llX len 0x%lX module clock %lu Hz max %lu kHz mode %s%s\n",
                  (ULONGLONG)PhysicalBase.QuadPart,
                  Length,
                  Ext->ModuleClockHz,
                  Ext->MaxFrequencyKhz,
                  (Ext->TransferMode == SMHC_TRANSFER_MODE_DMA) ? "DMA" : "PIO",
                  CrashdumpMode ? " (crashdump)" : "");

    //
    // ---- Capabilities.  Field usage follows sdhc.c SdhcSlotInitialize and
    // ---- dwcmshc.cpp MshcSlotInitialize.
    //
    Capabilities = &Ext->Capabilities;
    RtlZeroMemory(Capabilities, sizeof(*Capabilities));

    Capabilities->SpecVersion = 3;                          // as dwcmshc.cpp (non-SDHCI host)
    Capabilities->MaximumOutstandingRequests = 1;
    Capabilities->MaximumBlockSize = SMHC_MAX_BLOCK_SIZE;
    Capabilities->MaximumBlockCount = SMHC_MAX_BLOCK_COUNT;
    Capabilities->BaseClockFrequencyKhz = Ext->ModuleClockHz / 1000;

    if (Ext->TransferMode == SMHC_TRANSFER_MODE_DMA) {
        Capabilities->Supported.ScatterGatherDma = 1;
        Capabilities->DmaDescriptorSize = sizeof(SMHC_IDMAC_DESCRIPTOR);
        Capabilities->AlignmentRequirement = 3;             // 4-byte: IDMAC addresses are >> 2

    } else {
        //
        // PIO only.  No scatter/gather support => sdport uses PIO for every
        // transfer; the flags tell it PIO is intended.
        //
        Capabilities->Flags.UsePioForRead = TRUE;
        Capabilities->Flags.UsePioForWrite = TRUE;
        Capabilities->PioTransferMaxThreshold =
            SMHC_MAX_BLOCK_SIZE * SMHC_MAX_BLOCK_COUNT;     // UNVERIFIED: meaning in sdport (sample: 64)
    }

    //
    // 32-bit DMA addressing only (see smhc_core.h).
    //
    Capabilities->Supported.Address64Bit = 0;

    //
    // SMHC0 on the Orange Pi Zero 3 routes D0-D3 only (PF0-PF5 = CLK, CMD, D0-D3
    // per sun50i-h616.dtsi mmc0_pins).
    //
    Capabilities->Supported.BusWidth8Bit = 0;

    //
    // 3.3 V, normal speed only.  High-speed (50 MHz) needs a module clock above
    // the 24 MHz firmware leaves; UHS-I needs a switchable 1.8 V supply (AXP313A
    // DLDO1 / PMIC, not reachable from here).  Both are future work.
    //
    Capabilities->Supported.HighSpeed = 0;
    Capabilities->Supported.SDR50 = 0;
    Capabilities->Supported.DDR50 = 0;
    Capabilities->Supported.SDR104 = 0;
    Capabilities->Supported.HS200 = 0;
    Capabilities->Supported.HS400 = 0;
    Capabilities->Supported.SignalingVoltage18V = 0;
    Capabilities->Supported.TuningForSDR50 = 0;
    Capabilities->Supported.SoftwareTuning = 0;

    Capabilities->Supported.Voltage33V = 1;                 // UNVERIFIED how sdport uses these
    Capabilities->Supported.DriverTypeB = 1;
    Capabilities->Supported.Limit200mA = 1;
    Capabilities->Supported.Limit400mA = 1;
    Capabilities->Supported.Limit600mA = 1;
    Capabilities->Supported.Limit800mA = 1;

    Capabilities->Supported.AutoCmd12 = 1;
    Capabilities->Supported.AutoCmd23 = 0;

    //
    // SlotInitialize cannot return failure (sdport crashes; see dwcmshc.cpp), so
    // remember the outcome and refuse bus operations instead.
    //
    Status = STATUS_SUCCESS;
    if (Length < SMHC_MIN_REGISTER_SPACE) {
        SMHC_LOG_ERROR("register window 0x%lX too small (need 0x%X)\n",
                       Length,
                       SMHC_MIN_REGISTER_SPACE);

        Status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    //
    // A gated bus clock or a reset left asserted by firmware shows up as an
    // all-ones read (or, on ARM64, as an external abort that this cannot catch).
    //
    if (NT_SUCCESS(Status) && (SmhcRead(Ext, SMHC_REG_GCTRL) == 0xFFFFFFFFUL)) {
        SMHC_LOG_ERROR("SMHC registers read as all ones: bus clock gated / in reset?\n");
        Status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    if (NT_SUCCESS(Status) && !CrashdumpMode) {
        Ext->BusyWorkItem = IoAllocateWorkItem(Ext->Fdo);
    }

    if (NT_SUCCESS(Status)) {
        Status = SmhcResetHost(Ext, SdResetTypeAll);
        if (!NT_SUCCESS(Status)) {
            SMHC_LOG_ERROR("initial reset failed 0x%08X\n", Status);
        }
    }

    Ext->Initialized = NT_SUCCESS(Status);
    return STATUS_SUCCESS;
}

NTSTATUS
SmhcSlotIssueBusOperation(
    _In_ PVOID PrivateExtension,
    _In_ PSDPORT_BUS_OPERATION BusOperation
    )
{
    PSMHC_EXTENSION Ext;
    NTSTATUS Status;

    Ext = (PSMHC_EXTENSION)PrivateExtension;

    if (!Ext->Initialized) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    switch (BusOperation->Type) {
    case SdResetHw:
        //
        // HWRST (0x78) is the eMMC RST_n line; an SD slot has no such pin.
        //
        Status = STATUS_NOT_SUPPORTED;
        break;

    case SdResetHost:
        SMHC_LOG_INFO("bus op: reset %d\n", (int)BusOperation->Parameters.ResetType);
        Status = SmhcResetHost(Ext, BusOperation->Parameters.ResetType);
        break;

    case SdSetClock:
        SMHC_LOG_INFO("bus op: clock %lu kHz\n", BusOperation->Parameters.FrequencyKhz);
        Status = SmhcSetClock(Ext, BusOperation->Parameters.FrequencyKhz);
        break;

    case SdSetVoltage:
        //
        // The card supply (AXP313A DLDO1) is left on at 3.3 V by firmware and cannot
        // be controlled from here, so every request is accepted without effect.
        // Consequence: sdport cannot power-cycle the card; CMD0 resets its state.
        //
        SMHC_LOG_INFO("bus op: voltage %d (no-op)\n", (int)BusOperation->Parameters.Voltage);
        Status = STATUS_SUCCESS;
        break;

    case SdSetPower:
        Status = STATUS_SUCCESS;
        break;

    case SdSetBusWidth:
        SMHC_LOG_INFO("bus op: width %d\n", (int)BusOperation->Parameters.BusWidth);
        Status = SmhcSetBusWidth(Ext, BusOperation->Parameters.BusWidth);
        break;

    case SdSetBusSpeed:
        SMHC_LOG_INFO("bus op: speed %d\n", (int)BusOperation->Parameters.BusSpeed);
        switch (BusOperation->Parameters.BusSpeed) {
        case SdBusSpeedNormal:
        case SdBusSpeedHigh:
            Status = STATUS_SUCCESS;        // clock rate is set by SdSetClock
            break;

        default:
            Status = STATUS_NOT_SUPPORTED;
            break;
        }

        break;

    case SdSetSignalingVoltage:
        Status = (BusOperation->Parameters.SignalingVoltage == SdSignalingVoltage33) ?
                     STATUS_SUCCESS : STATUS_NOT_SUPPORTED;

        break;

    case SdSetDriveStrength:
    case SdSetDriverType:
    case SdSetPresetValue:
    case SdSetBlockGapInterrupt:
    case SdExecuteTuning:
        Status = STATUS_NOT_SUPPORTED;
        break;

    default:
        Status = STATUS_INVALID_PARAMETER;
        break;
    }

    return Status;
}

BOOLEAN
SmhcSlotGetCardDetectState(
    _In_ PVOID PrivateExtension
    )
{
    //
    // Card detect is not usable on this board: the schematic wires CD to PF6 via an
    // inverter "but it just doesn't work" (sun50i-h618-orangepi-zero3.dts, &mmc0
    // { broken-cd; }).  Report the card as present, like Linux's broken-cd, and let
    // sdport's enumeration fail cleanly if the slot is empty.  STATUS.CARD_PRESENT
    // (bit 8) is not used for the same reason.
    //
    UNREFERENCED_PARAMETER(PrivateExtension);
    return TRUE;
}

BOOLEAN
SmhcSlotGetWriteProtectState(
    _In_ PVOID PrivateExtension
    )
{
    //
    // microSD sockets have no write-protect switch.
    //
    UNREFERENCED_PARAMETER(PrivateExtension);
    return FALSE;
}

BOOLEAN
SmhcSlotInterrupt(
    _In_ PVOID PrivateExtension,
    _Out_ PULONG Events,
    _Out_ PULONG Errors,
    _Out_ PBOOLEAN CardChange,
    _Out_ PBOOLEAN SdioInterrupt,
    _Out_ PBOOLEAN Tuning
    )
{
    PSMHC_EXTENSION Ext;
    ULONG Mint;
    ULONG Idst;
    ULONG Idie;
    ULONG Ack;

    Ext = (PSMHC_EXTENSION)PrivateExtension;

    *Events = 0;
    *Errors = 0;
    *CardChange = FALSE;
    *SdioInterrupt = FALSE;
    *Tuning = FALSE;

    if (!Ext->Initialized) {
        return FALSE;
    }

    //
    // [LX] sunxi_mmc_irq: MISTA (masked) + IDST.  IDST is a raw status, so only the
    // bits enabled in IDIE count as ours.
    //
    Mint = SmhcRead(Ext, SMHC_REG_MINT);
    Idie = SmhcRead(Ext, SMHC_REG_IDIE);
    Idst = SmhcRead(Ext, SMHC_REG_IDST) & Idie;

    //
    // All ones: the controller is not accessible (clock gated / powered down).
    //
    if ((Mint == 0xFFFFFFFFUL) || ((Mint == 0) && (Idst == 0))) {
        return FALSE;
    }

    //
    // The data-request interrupts stay asserted until the FIFO has been serviced:
    // mask them now to avoid an interrupt storm and acknowledge them after the data
    // has been moved (SmhcStartPioTransfer).  Same approach as dwcmshc.cpp.
    //
    if (Mint & (SMHC_INT_RX_DATA_REQUEST | SMHC_INT_TX_DATA_REQUEST)) {
        SmhcImaskUpdate(Ext, 0, SMHC_INT_RX_DATA_REQUEST | SMHC_INT_TX_DATA_REQUEST);
    }

    Ack = Mint & ~(SMHC_INT_RX_DATA_REQUEST | SMHC_INT_TX_DATA_REQUEST);
    if (Ack != 0) {
        SmhcWrite(Ext, SMHC_REG_RINT, Ack);
    }

    if (Idst != 0) {
        SmhcWrite(Ext, SMHC_REG_IDST, Idst);
    }

    SmhcConvertInterrupts(Mint, Idst, Ext->AutoStopActive, Events, Errors);

    //
    // As sdhc.c / dwcmshc.cpp: report "handled" only if there is something for sdport
    // to act on.  Bits that translate to nothing (e.g. DATA_OVER of an auto-stop
    // transfer) have been acknowledged and have deasserted the level interrupt.
    //
    return (*Events != 0) || (*Errors != 0);
}

NTSTATUS
SmhcSlotIssueRequest(
    _In_ PVOID PrivateExtension,
    _In_ PSDPORT_REQUEST Request
    )
{
    PSMHC_EXTENSION Ext;
    NTSTATUS Status;

    Ext = (PSMHC_EXTENSION)PrivateExtension;

    if (!Ext->Initialized) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    //
    // One request at a time (MaximumOutstandingRequests = 1).
    //
    InterlockedExchange(&Ext->PhaseClaim, 0);

    if (InterlockedExchangePointer((PVOID volatile *)&Ext->OutstandingRequest, Request) != NULL) {
        SMHC_LOG_WARN("request issued while another was outstanding; previous one dropped\n");
    }

    switch (Request->Type) {
    case SdRequestTypeCommandNoTransfer:
    case SdRequestTypeCommandWithTransfer:
        Status = SmhcSendCommand(Ext, Request);
        break;

    case SdRequestTypeStartTransfer:
        Status = SmhcStartTransfer(Ext, Request);
        break;

    default:
        Status = STATUS_NOT_SUPPORTED;
        break;
    }

    if ((Status != STATUS_PENDING) && !NT_SUCCESS(Status)) {
        InterlockedExchangePointer((PVOID volatile *)&Ext->OutstandingRequest, NULL);
    }

    return Status;
}

VOID
SmhcSlotGetResponse(
    _In_ PVOID PrivateExtension,
    _In_ PSDPORT_COMMAND Command,
    _Out_ PVOID ResponseBuffer
    )
{
    SmhcGetResponse((PSMHC_EXTENSION)PrivateExtension, Command, ResponseBuffer);
}

VOID
SmhcSlotToggleEvents(
    _In_ PVOID PrivateExtension,
    _In_ ULONG EventMask,
    _In_ BOOLEAN Enable
    )
{
    PSMHC_EXTENSION Ext;
    ULONG Bits;

    Ext = (PSMHC_EXTENSION)PrivateExtension;
    if (!Ext->Initialized) {
        return;
    }

    Bits = SmhcEventsToIntMask(EventMask);

    //
    // CARD_CHANGE is never enabled (broken-cd).  DMA_COMPLETE is controlled through
    // IDIE by the transfer itself, not by sdport.
    //
    if (Enable) {
        SmhcImaskUpdate(Ext, Bits, 0);
    } else {
        SmhcImaskUpdate(Ext, 0, Bits);
    }
}

VOID
SmhcSlotClearEvents(
    _In_ PVOID PrivateExtension,
    _In_ ULONG EventMask
    )
{
    PSMHC_EXTENSION Ext;

    Ext = (PSMHC_EXTENSION)PrivateExtension;
    if (Ext->Initialized) {
        SmhcWrite(Ext, SMHC_REG_RINT, SmhcEventsToIntMask(EventMask));
    }
}

VOID
SmhcRequestDpc(
    _In_ PVOID PrivateExtension,
    _Inout_ PSDPORT_REQUEST Request,
    _In_ ULONG Events,
    _In_ ULONG Errors
    )
{
    PSMHC_EXTENSION Ext;
    BOOLEAN Waiting;
    ULONG AllErrors;
    ULONG AllEvents;
    NTSTATUS Status;

    Ext = (PSMHC_EXTENSION)PrivateExtension;

    if ((Events == 0) && (Errors == 0)) {
        return;
    }

    //
    // Remember everything seen for this command.  A phase may finish before the
    // next one starts waiting for it (short reads: DATA_OVER can arrive before the
    // FIFO has been drained), see dwcmshc.cpp MshcRequestDpc.
    //
    InterlockedOr(&Ext->CurrentEvents, (LONG)Events);
    InterlockedOr(&Ext->CurrentErrors, (LONG)Errors);

    //
    // sdport does not keep a request state, so a late or duplicated notification can
    // arrive for a request that is not (or no longer) on the bus.
    //
    if (Ext->OutstandingRequest != Request) {
        SMHC_LOG_TRACE("event for non-outstanding request, ignored (ev 0x%X err 0x%X)\n",
                       Events,
                       Errors);

        return;
    }

    //
    // Completion already deferred to the busy work item, or nothing left to wait for.
    //
    Waiting = (Request->RequiredEvents != 0);
    if (Ext->BusyPending || !Waiting) {
        return;
    }

    Request->RequiredEvents &= ~Events;

    AllErrors = (ULONG)InterlockedCompareExchange(&Ext->CurrentErrors, 0, 0);
    AllEvents = (ULONG)InterlockedCompareExchange(&Ext->CurrentEvents, 0, 0);

    if (AllErrors != 0) {
        //
        // [LX] sunxi_mmc_irq: "Wait for COMMAND_DONE on RESPONSE_TIMEOUT before
        // finalize" - the controller still finishes the command after a response
        // timeout; completing earlier lets the next command collide with it.
        //
        if ((AllErrors & SDPORT_ERROR_CMD_TIMEOUT) &&
            !(AllEvents & SDPORT_EVENT_CARD_RESPONSE)) {

            return;
        }

        if (InterlockedCompareExchange(&Ext->PhaseClaim, 1, 0) != 0) {
            return;                         // the phase is already being completed elsewhere
        }

        //
        // The card is left in its data state unless the command itself failed.
        //
        if ((Request->Command.TransferType != SdTransferTypeNone) &&
            (Request->Command.TransferType != SdTransferTypeUndefined) &&
            !(AllErrors & SDPORT_ERROR_CMD_TIMEOUT)) {

            InterlockedExchange(&Ext->NeedStop, 1);
        }

        Request->RequiredEvents = 0;
        Status = SmhcConvertErrorToStatus(AllErrors);
        SMHC_LOG_WARN("CMD%lu failed: errors 0x%X -> 0x%08X\n",
                      Request->Command.Index,
                      AllErrors,
                      Status);

        InterlockedExchange(&Ext->CurrentErrors, 0);
        SmhcCompleteRequest(Ext, Request, Status);

    } else if (Request->RequiredEvents == 0) {
        if (InterlockedCompareExchange(&Ext->PhaseClaim, 1, 0) != 0) {
            return;
        }

        if (Request->Status != STATUS_MORE_PROCESSING_REQUIRED) {
            Request->Status = STATUS_SUCCESS;
        }

        SmhcCompleteRequest(Ext, Request, Request->Status);
    }
}

VOID
SmhcSaveContext(
    _In_ PVOID PrivateExtension
    )
{
    //
    // All state is cached in the extension (Clkcr, BusWidthReg, ImaskShadow).
    //
    UNREFERENCED_PARAMETER(PrivateExtension);
}

VOID
SmhcRestoreContext(
    _In_ PVOID PrivateExtension
    )
{
    PSMHC_EXTENSION Ext;

    Ext = (PSMHC_EXTENSION)PrivateExtension;
    if (Ext->Initialized) {
        SmhcRestoreRegisters(Ext);

        //
        // CLKCR is restored without the clock-on bits; latch it and bring the card
        // clock back if it was running ([LX] runtime_resume: init_host, set_bus_width,
        // set_clk).
        //
        SmhcSetClock(Ext, Ext->BusFrequencyKhz);
    }
}

NTSTATUS
SmhcPoFxPowerControlCallback(
    _In_ PSD_MINIPORT Miniport,
    _In_ LPCGUID PowerControlCode,
    _In_reads_bytes_opt_(InputBufferSize) PVOID InputBuffer,
    _In_ SIZE_T InputBufferSize,
    _Out_writes_bytes_opt_(OutputBufferSize) PVOID OutputBuffer,
    _In_ SIZE_T OutputBufferSize,
    _Out_opt_ PSIZE_T BytesReturned
    )
{
    UNREFERENCED_PARAMETER(Miniport);
    UNREFERENCED_PARAMETER(PowerControlCode);
    UNREFERENCED_PARAMETER(InputBuffer);
    UNREFERENCED_PARAMETER(InputBufferSize);
    UNREFERENCED_PARAMETER(OutputBuffer);
    UNREFERENCED_PARAMETER(OutputBufferSize);
    UNREFERENCED_PARAMETER(BytesReturned);

    return STATUS_NOT_IMPLEMENTED;
}

VOID
SmhcCleanup(
    _In_ PSD_MINIPORT Miniport
    )
{
    ULONG Index;

    for (Index = 0; Index < Miniport->SlotCount; Index += 1) {
        PSMHC_EXTENSION Ext;

        Ext = (PSMHC_EXTENSION)Miniport->SlotExtensionList[Index]->PrivateExtension;
        if ((Ext != NULL) && (Ext->BusyWorkItem != NULL)) {
            IoFreeWorkItem(Ext->BusyWorkItem);
            Ext->BusyWorkItem = NULL;
        }
    }
}
