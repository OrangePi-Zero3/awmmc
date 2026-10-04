/*
 * Minimal host-side stand-in for <ntddk.h>, ONLY to compile-check and unit-test
 * the SMHC driver sources on a Linux host.  It encodes this project's
 * assumptions about the kernel API; it is NOT a substitute for a WDK build.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

typedef unsigned char       UCHAR, *PUCHAR, BOOLEAN, *PBOOLEAN;
typedef unsigned short      USHORT, *PUSHORT;
typedef unsigned int        ULONG, *PULONG;
typedef int                 LONG, *PLONG;
typedef uint64_t            ULONGLONG, *PULONGLONG;
typedef int64_t             LONGLONG;
typedef void                VOID, *PVOID;
typedef size_t              SIZE_T, *PSIZE_T;
typedef uintptr_t           ULONG_PTR;
typedef int32_t             NTSTATUS;
typedef void               *HANDLE;
typedef const char         *PCSTR;
typedef const wchar_t      *PCWSTR;
typedef const void         *LPCGUID;
typedef wchar_t             WCHAR;

#define TRUE  1
#define FALSE 0

#define _In_
#define _Out_
#define _Inout_
#define _In_opt_
#define _Out_opt_
#define _In_reads_(x)
#define _In_reads_bytes_opt_(x)
#define _Out_writes_bytes_opt_(x)
#define _Out_writes_all_(x)
#define _IRQL_requires_max_(x)

#define __forceinline inline __attribute__((always_inline))

#define NT_SUCCESS(s)           ((s) >= 0)
#define STATUS_SUCCESS                  ((NTSTATUS)0x00000000)
#define STATUS_PENDING                  ((NTSTATUS)0x00000103)
#define STATUS_MORE_PROCESSING_REQUIRED ((NTSTATUS)0xC0000016)
#define STATUS_INVALID_PARAMETER        ((NTSTATUS)0xC000000D)
#define STATUS_NOT_SUPPORTED            ((NTSTATUS)0xC00000BB)
#define STATUS_NOT_IMPLEMENTED          ((NTSTATUS)0xC0000002)
#define STATUS_IO_TIMEOUT               ((NTSTATUS)0xC00000B5)
#define STATUS_CRC_ERROR                ((NTSTATUS)0xC000003F)
#define STATUS_DEVICE_DATA_ERROR        ((NTSTATUS)0xC000009C)
#define STATUS_DEVICE_PROTOCOL_ERROR    ((NTSTATUS)0xC0000186)
#define STATUS_DEVICE_POWER_FAILURE     ((NTSTATUS)0xC000009E)
#define STATUS_IO_DEVICE_ERROR          ((NTSTATUS)0xC0000185)
#define STATUS_BUFFER_TOO_SMALL         ((NTSTATUS)0xC0000023)
#define STATUS_OBJECT_TYPE_MISMATCH     ((NTSTATUS)0xC0000024)
#define STATUS_DEVICE_CONFIGURATION_ERROR ((NTSTATUS)0xC0000182)

#define NULL_PTR NULL
#define UNREFERENCED_PARAMETER(p) ((void)(p))
#define NT_ASSERTMSG(m, c) ((void)0)
#define NT_ASSERT(c) ((void)0)
#define CONTAINING_RECORD(address, type, field) \
    ((type *)((char *)(address) - offsetof(type, field)))

typedef union _LARGE_INTEGER {
    struct { ULONG LowPart; LONG HighPart; };
    LONGLONG QuadPart;
} LARGE_INTEGER, PHYSICAL_ADDRESS;

typedef struct _UNICODE_STRING { USHORT Length, MaximumLength; PCWSTR Buffer; } UNICODE_STRING, *PUNICODE_STRING;

struct _DEVICE_OBJECT;
typedef struct _DEVOBJ_EXTENSION { struct _DEVICE_OBJECT *AttachedTo; } DEVOBJ_EXTENSION;
typedef struct _DEVICE_OBJECT {
    DEVOBJ_EXTENSION *DeviceObjectExtension;
} DEVICE_OBJECT, *PDEVICE_OBJECT;
typedef struct _DRIVER_OBJECT { int dummy; } DRIVER_OBJECT, *PDRIVER_OBJECT;
typedef NTSTATUS DRIVER_INITIALIZE(PDRIVER_OBJECT, PUNICODE_STRING);

/* debug print */
#define DPFLTR_IHVDRIVER_ID 77
#define DPFLTR_ERROR_LEVEL   0
#define DPFLTR_WARNING_LEVEL 1
#define DPFLTR_TRACE_LEVEL   2
#define DPFLTR_INFO_LEVEL    3
extern int g_mock_log_level;
static inline int DbgPrintEx(ULONG comp, ULONG level, PCSTR fmt, ...)
{
    va_list ap; (void)comp;
    if ((int)level > g_mock_log_level) return 0;
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    return 0;
}

/* interlocked */
#define InterlockedOr(p, v)     __sync_fetch_and_or((volatile LONG *)(p), (LONG)(v))
#define InterlockedAnd(p, v)    __sync_fetch_and_and((volatile LONG *)(p), (LONG)(v))
static inline LONG InterlockedExchange(volatile LONG *p, LONG v) { return __sync_lock_test_and_set(p, v); }
static inline LONG InterlockedCompareExchange(volatile LONG *p, LONG x, LONG c) { return __sync_val_compare_and_swap(p, c, x); }
static inline PVOID InterlockedExchangePointer(PVOID volatile *p, PVOID v) { return __sync_lock_test_and_set(p, v); }
static inline void KeMemoryBarrier(void) { __sync_synchronize(); }

/* irql / work items */
#define DISPATCH_LEVEL 2
#define PASSIVE_LEVEL  0
extern int g_mock_irql;
static inline int KeGetCurrentIrql(void) { return g_mock_irql; }
typedef struct _IO_WORKITEM { int queued; } IO_WORKITEM, *PIO_WORKITEM;
typedef void (*PIO_WORKITEM_ROUTINE)(PDEVICE_OBJECT, PVOID);
#define CriticalWorkQueue 0
PIO_WORKITEM IoAllocateWorkItem(PDEVICE_OBJECT dev);
void IoFreeWorkItem(PIO_WORKITEM w);
void IoQueueWorkItem(PIO_WORKITEM w, PIO_WORKITEM_ROUTINE r, int type, PVOID ctx);

/* registry */
#define KEY_READ 1
#define PLUGPLAY_REGKEY_DEVICE 1
#define REG_DWORD 4
typedef enum { KeyValuePartialInformation } KEY_VALUE_INFORMATION_CLASS;
typedef struct _KEY_VALUE_PARTIAL_INFORMATION {
    ULONG TitleIndex; ULONG Type; ULONG DataLength; UCHAR Data[1];
} KEY_VALUE_PARTIAL_INFORMATION, *PKEY_VALUE_PARTIAL_INFORMATION;
NTSTATUS IoOpenDeviceRegistryKey(PDEVICE_OBJECT pdo, ULONG type, ULONG access, HANDLE *key);
NTSTATUS ZwQueryValueKey(HANDLE key, PUNICODE_STRING name, KEY_VALUE_INFORMATION_CLASS c,
                         PVOID buf, ULONG len, PULONG result);
static inline NTSTATUS ZwClose(HANDLE h) { (void)h; return STATUS_SUCCESS; }
static inline void RtlInitUnicodeString(PUNICODE_STRING s, PCWSTR w) { s->Buffer = w; s->Length = 0; s->MaximumLength = 0; }
#define RtlZeroMemory(d, l) memset((d), 0, (l))
#define RtlCopyMemory(d, s, l) memcpy((d), (s), (l))

typedef struct _SCATTER_GATHER_ELEMENT { PHYSICAL_ADDRESS Address; ULONG Length; ULONG_PTR Reserved; } SCATTER_GATHER_ELEMENT;
typedef struct _SCATTER_GATHER_LIST {
    ULONG NumberOfElements; ULONG_PTR Reserved; SCATTER_GATHER_ELEMENT Elements[1];
} SCATTER_GATHER_LIST, *PSCATTER_GATHER_LIST;
