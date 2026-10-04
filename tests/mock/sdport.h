/*
 * Host-side stand-in for <sdport.h>.  Reconstructed from the identifiers the
 * Microsoft sample (sdhc.c/sdhc.h) and dwcmshc.cpp use; field order, sizes and
 * the numeric values of enums/event bits are NOT those of the real header.
 */

#pragma once

#include "ntddk.h"

/* events / errors (bit values are arbitrary here) */
#define SDPORT_EVENT_CARD_RESPONSE      0x0001
#define SDPORT_EVENT_CARD_RW_END        0x0002
#define SDPORT_EVENT_DMA_COMPLETE       0x0008
#define SDPORT_EVENT_BUFFER_EMPTY       0x0010
#define SDPORT_EVENT_BUFFER_FULL        0x0020
#define SDPORT_EVENT_CARD_CHANGE        0x00C0
#define SDPORT_EVENT_ERROR              0x8000

#define SDPORT_ERROR_CMD_TIMEOUT        0x0001
#define SDPORT_ERROR_CMD_CRC_ERROR      0x0002
#define SDPORT_ERROR_CMD_END_BIT_ERROR  0x0004
#define SDPORT_ERROR_CMD_INDEX_ERROR    0x0008
#define SDPORT_ERROR_DATA_TIMEOUT       0x0010
#define SDPORT_ERROR_DATA_CRC_ERROR     0x0020
#define SDPORT_ERROR_DATA_END_BIT_ERROR 0x0040
#define SDPORT_ERROR_BUS_POWER_ERROR    0x0080
#define SDPORT_ERROR_ADMA_ERROR         0x0200
#define SDPORT_GENERIC_IO_ERROR         0x4000

typedef enum { SdBusTypePci, SdBusTypeAcpi } SDPORT_BUS_TYPE;
typedef enum { SdResetTypeAll, SdResetTypeCmd, SdResetTypeDat } SDPORT_RESET_TYPE;
typedef enum { SdBusWidth1Bit = 1, SdBusWidth4Bit = 4, SdBusWidth8Bit = 8 } SDPORT_BUS_WIDTH;
typedef enum { SdBusVoltageOff, SdBusVoltage33, SdBusVoltage30, SdBusVoltage18 } SDPORT_BUS_VOLTAGE;
typedef enum { SdSignalingVoltage33, SdSignalingVoltage18 } SDPORT_SIGNALING_VOLTAGE;
typedef enum { SdBusSpeedNormal, SdBusSpeedHigh, SdBusSpeedSDR12, SdBusSpeedSDR25, SdBusSpeedSDR50,
               SdBusSpeedDDR50, SdBusSpeedSDR104, SdBusSpeedHS200, SdBusSpeedHS400 } SDPORT_BUS_SPEED;
typedef enum { SdResetHw, SdResetHost, SdSetClock, SdSetVoltage, SdSetPower, SdSetBusWidth, SdSetBusSpeed,
               SdSetSignalingVoltage, SdSetDriveStrength, SdSetDriverType, SdSetPresetValue,
               SdSetBlockGapInterrupt, SdExecuteTuning } SDPORT_BUS_OPERATION_TYPE;
typedef enum { SdRequestTypeCommandNoTransfer, SdRequestTypeCommandWithTransfer, SdRequestTypeStartTransfer } SDPORT_REQUEST_TYPE;
typedef enum { SdTransferTypeUndefined, SdTransferTypeNone, SdTransferTypeSingleBlock, SdTransferTypeMultiBlock,
               SdTransferTypeMultiBlockNoStop } SDPORT_TRANSFER_TYPE;
typedef enum { SdTransferDirectionUndefined, SdTransferDirectionRead, SdTransferDirectionWrite } SDPORT_TRANSFER_DIRECTION;
typedef enum { SdTransferMethodUndefined, SdTransferMethodPio, SdTransferMethodSgDma } SDPORT_TRANSFER_METHOD;
typedef enum { SdResponseTypeUndefined, SdResponseTypeNone, SdResponseTypeR1, SdResponseTypeR1B, SdResponseTypeR2,
               SdResponseTypeR3, SdResponseTypeR4, SdResponseTypeR5, SdResponseTypeR5B, SdResponseTypeR6 } SDPORT_RESPONSE_TYPE;
typedef enum { SdCommandClassStandard, SdCommandClassApp } SDPORT_COMMAND_CLASS;

typedef struct _SDPORT_CAPABILITIES {
    UCHAR SpecVersion;
    UCHAR MaximumOutstandingRequests;
    USHORT MaximumBlockSize;
    USHORT MaximumBlockCount;
    ULONG BaseClockFrequencyKhz;
    ULONG DmaDescriptorSize;
    ULONG AlignmentRequirement;
    ULONG PioTransferMaxThreshold;
    ULONG TuningTimerCountInSeconds;
    struct {
        ULONG ScatterGatherDma:1, Address64Bit:1, BusWidth8Bit:1, HighSpeed:1, SDR50:1, DDR50:1, SDR104:1,
              HS200:1, HS400:1, SignalingVoltage18V:1, TuningForSDR50:1, SoftwareTuning:1, Voltage18V:1,
              Voltage30V:1, Voltage33V:1, DriverTypeA:1, DriverTypeB:1, DriverTypeC:1, DriverTypeD:1,
              Limit200mA:1, Limit400mA:1, Limit600mA:1, Limit800mA:1, AutoCmd12:1, AutoCmd23:1;
    } Supported;
    struct { ULONG UsePioForRead:1, UsePioForWrite:1; } Flags;
} SDPORT_CAPABILITIES, *PSDPORT_CAPABILITIES;

typedef struct _SDPORT_COMMAND {
    ULONG Index;
    SDPORT_COMMAND_CLASS Class;
    ULONG Argument;
    SDPORT_RESPONSE_TYPE ResponseType;
    SDPORT_TRANSFER_TYPE TransferType;
    SDPORT_TRANSFER_DIRECTION TransferDirection;
    SDPORT_TRANSFER_METHOD TransferMethod;
    ULONG BlockSize;
    ULONG BlockCount;
    ULONG Length;
    PUCHAR DataBuffer;
    BOOLEAN UseAutoCmd12;
    PSCATTER_GATHER_LIST ScatterGatherList;
    PVOID DmaVirtualAddress;
    PHYSICAL_ADDRESS DmaPhysicalAddress;
} SDPORT_COMMAND, *PSDPORT_COMMAND;

typedef struct _SDPORT_REQUEST {
    SDPORT_REQUEST_TYPE Type;
    SDPORT_COMMAND Command;
    ULONG RequiredEvents;
    NTSTATUS Status;
} SDPORT_REQUEST, *PSDPORT_REQUEST;

typedef struct _SDPORT_BUS_OPERATION {
    SDPORT_BUS_OPERATION_TYPE Type;
    union {
        SDPORT_RESET_TYPE ResetType;
        ULONG FrequencyKhz;
        SDPORT_BUS_VOLTAGE Voltage;
        SDPORT_BUS_WIDTH BusWidth;
        SDPORT_BUS_SPEED BusSpeed;
        SDPORT_SIGNALING_VOLTAGE SignalingVoltage;
    } Parameters;
} SDPORT_BUS_OPERATION, *PSDPORT_BUS_OPERATION;

struct _SD_MINIPORT;
typedef struct _SDPORT_SLOT_EXTENSION {
    struct _SD_MINIPORT *Miniport;
    UCHAR PrivateExtension[1];
} SDPORT_SLOT_EXTENSION, *PSDPORT_SLOT_EXTENSION;

typedef struct _SD_MINIPORT {
    struct { SDPORT_BUS_TYPE BusType; PDEVICE_OBJECT DeviceObject; } ConfigurationInfo;
    ULONG SlotCount;
    PSDPORT_SLOT_EXTENSION SlotExtensionList[1];
} SD_MINIPORT, *PSD_MINIPORT;

typedef struct _SDPORT_INITIALIZATION_DATA {
    ULONG StructureSize;
    NTSTATUS (*GetSlotCount)(PSD_MINIPORT, PUCHAR);
    VOID (*GetSlotCapabilities)(PVOID, PSDPORT_CAPABILITIES);
    NTSTATUS (*Initialize)(PVOID, PHYSICAL_ADDRESS, PVOID, ULONG, BOOLEAN);
    NTSTATUS (*IssueBusOperation)(PVOID, PSDPORT_BUS_OPERATION);
    BOOLEAN (*GetCardDetectState)(PVOID);
    BOOLEAN (*GetWriteProtectState)(PVOID);
    BOOLEAN (*Interrupt)(PVOID, PULONG, PULONG, PBOOLEAN, PBOOLEAN, PBOOLEAN);
    NTSTATUS (*IssueRequest)(PVOID, PSDPORT_REQUEST);
    VOID (*GetResponse)(PVOID, PSDPORT_COMMAND, PVOID);
    VOID (*ToggleEvents)(PVOID, ULONG, BOOLEAN);
    VOID (*ClearEvents)(PVOID, ULONG);
    VOID (*RequestDpc)(PVOID, PSDPORT_REQUEST, ULONG, ULONG);
    VOID (*SaveContext)(PVOID);
    VOID (*RestoreContext)(PVOID);
    NTSTATUS (*PowerControlCallback)(PSD_MINIPORT, LPCGUID, PVOID, SIZE_T, PVOID, SIZE_T, PSIZE_T);
    VOID (*Cleanup)(PSD_MINIPORT);
    ULONG PrivateExtensionSize;
    BOOLEAN CrashdumpSupported;
} SDPORT_INITIALIZATION_DATA, *PSDPORT_INITIALIZATION_DATA;

NTSTATUS SdPortInitialize(PVOID driver, PVOID regpath, PSDPORT_INITIALIZATION_DATA init);
VOID SdPortCompleteRequest(PSDPORT_REQUEST request, NTSTATUS status);
VOID SdPortWait(ULONG microseconds);
ULONG SdPortReadRegisterUlong(PVOID base, ULONG reg);
VOID SdPortWriteRegisterUlong(PVOID base, ULONG reg, ULONG value);
