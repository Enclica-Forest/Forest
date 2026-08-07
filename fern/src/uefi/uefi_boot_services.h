#ifndef UEFI_BOOT_SERVICES_H
#define UEFI_BOOT_SERVICES_H

#include <stdint.h>

/* UEFI Basic Types */
typedef uint64_t UINTN;
typedef uint64_t EFI_STATUS;
typedef void* EFI_HANDLE;
typedef uint64_t EFI_PHYSICAL_ADDRESS;
typedef uint64_t EFI_VIRTUAL_ADDRESS;

/* EFI Status Codes */
#define EFI_SUCCESS             0
#define EFI_LOAD_ERROR          1
#define EFI_INVALID_PARAMETER   2
#define EFI_UNSUPPORTED         3
#define EFI_BAD_BUFFER_SIZE     4
#define EFI_BUFFER_TOO_SMALL    5
#define EFI_NOT_READY           6
#define EFI_DEVICE_ERROR        7
#define EFI_WRITE_PROTECTED     8
#define EFI_OUT_OF_RESOURCES    9
#define EFI_NOT_FOUND           14
#define EFIERR(x) (1UL | (x << 16))
#define EFI_ERROR(Status) (((int64_t)(Status)) < 0)

/* TPL (Task Priority Level) */
typedef UINTN EFI_TPL;
#define TPL_APPLICATION     4
#define TPL_CALLBACK        8
#define TPL_NOTIFY          16
#define TPL_HIGH_LEVEL      31

/* Memory Types */
typedef enum {
    EfiReservedMemoryType = 0,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiUnacceptedMemoryType,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

/* Memory Descriptor */
typedef struct {
    uint32_t Type;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* Memory Attribute Masks */
#define EFI_MEMORY_UC          0x0000000000000001
#define EFI_MEMORY_WC          0x0000000000000002
#define EFI_MEMORY_WT          0x0000000000000004
#define EFI_MEMORY_WB          0x0000000000000008
#define EFI_MEMORY_UCE         0x0000000000000010
#define EFI_MEMORY_WP          0x0000000000001000
#define EFI_MEMORY_RP          0x0000000000002000
#define EFI_MEMORY_XP          0x0000000000004000
#define EFI_MEMORY_RO          0x0000000000010000
#define EFI_MEMORY_RUNTIME     0x8000000000000000

/* Event Types */
#define EVT_TIMER              0x80000000
#define EVT_RUNTIME            0x40000000
#define EVT_NOTIFY_WAIT        0x00000100
#define EVT_NOTIFY_SIGNAL      0x00000200
#define EVT_SIGNAL_EXIT_BOOT_SERVICES  0x00000201
#define EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE  0x00000600

/* Timer Delays */
typedef enum {
    TimerCancel,
    TimerPeriodic,
    TimerRelative
} EFI_TIMER_DELAY;

/* Timer Period (100ns units) */
#define EFI_TIMER_PERIOD_SECONDS(n) ((n) * 10000000ULL)

/* GUID Structure */
typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} EFI_GUID;

/* EFI Time */
typedef struct {
    uint16_t Year;
    uint8_t  Month;
    uint8_t  Day;
    uint8_t  Hour;
    uint8_t  Minute;
    uint8_t  Second;
    uint8_t  Pad1;
    uint32_t Nanosecond;
    int16_t  TimeZone;
    uint8_t  Daylight;
    uint8_t  Pad2;
} EFI_TIME;

/* Forward Declarations */
typedef struct _EFI_BOOT_SERVICES EFI_BOOT_SERVICES;
typedef struct _EFI_RUNTIME_SERVICES EFI_RUNTIME_SERVICES;
typedef struct _EFI_SYSTEM_TABLE EFI_SYSTEM_TABLE;
typedef struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* Event Type */
typedef void* EFI_EVENT;

/* Input Key */
typedef struct {
    uint16_t ScanCode;
    uint16_t UnicodeChar;
} EFI_INPUT_KEY;

/* Simple Text Output Mode */
typedef struct {
    int32_t MaxMode;
    int32_t Mode;
    int32_t Attribute;
    int32_t CursorColumn;
    int32_t CursorRow;
    int8_t CursorVisible;
} SIMPLE_TEXT_OUTPUT_MODE;

/* ========================================================================
 * EFI_SIMPLE_TEXT_INPUT_PROTOCOL
 * ======================================================================== */
struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL* This, int ExtendedVerification);
    EFI_STATUS (EFIAPI *ReadKeyStroke)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL* This, EFI_INPUT_KEY* Key);
    EFI_EVENT WaitForKey;
};

/* ========================================================================
 * EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL
 * ======================================================================== */
struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This, int ExtendedVerification);
    EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This, uint16_t* String);
    EFI_STATUS (EFIAPI *TestString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This, uint16_t* String);
    EFI_STATUS (EFIAPI *QueryMode)(
        EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This,
        UINTN ModeNumber,
        UINTN* Columns,
        UINTN* Rows
    );
    EFI_STATUS (EFIAPI *SetMode)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This, UINTN ModeNumber);
    EFI_STATUS (EFIAPI *SetAttribute)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This, UINTN Attribute);
    EFI_STATUS (EFIAPI *ClearScreen)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This);
    EFI_STATUS (EFIAPI *SetCursorPosition)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This, UINTN Column, UINTN Row);
    EFI_STATUS (EFIAPI *EnableCursor)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This, int Visible);
    SIMPLE_TEXT_OUTPUT_MODE* Mode;
};

/* ========================================================================
 * EFI_FILE_PROTOCOL
 * ======================================================================== */
typedef struct _EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;

struct _EFI_FILE_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL* This, EFI_FILE_PROTOCOL** NewHandle, uint16_t* FileName, uint64_t OpenMode, uint64_t Attributes);
    EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL* This);
    EFI_STATUS (EFIAPI *Delete)(EFI_FILE_PROTOCOL* This);
    EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL* This, UINTN* BufferSize, void* Buffer);
    EFI_STATUS (EFIAPI *Write)(EFI_FILE_PROTOCOL* This, UINTN* BufferSize, void* Buffer);
    EFI_STATUS (EFIAPI *GetInfo)(EFI_FILE_PROTOCOL* This, EFI_GUID* InformationType, UINTN* BufferSize, void* Buffer);
    EFI_STATUS (EFIAPI *SetInfo)(EFI_FILE_PROTOCOL* This, EFI_GUID* InformationType, UINTN BufferSize, void* Buffer);
    EFI_STATUS (EFIAPI *Flush)(EFI_FILE_PROTOCOL* This);
};

/* ========================================================================
 * EFI_SIMPLE_FILE_SYSTEM_PROTOCOL
 * ======================================================================== */
typedef struct {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(void* This, EFI_FILE_PROTOCOL** Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    {0x0964e5b2, 0x6409, 0x47af, {0xb1, 0x4c, 0xf0, 0x34, 0xb7, 0x4a, 0x87, 0xf8}}

/* ========================================================================
 * EFI_BOOT_SERVICES - Core UEFI boot services
 * ======================================================================== */
struct _EFI_BOOT_SERVICES {
    /* Task Priority Services */
    EFI_TPL (EFIAPI *RaiseTPL)(EFI_TPL NewPriority);
    void (EFIAPI *RestoreTPL)(EFI_TPL OldPriority);

    /* Memory Services */
    EFI_STATUS (EFIAPI *AllocatePages)(
        int AllocateType,
        EFI_MEMORY_TYPE MemoryType,
        UINTN NumPages,
        EFI_PHYSICAL_ADDRESS* Memory
    );

    EFI_STATUS (EFIAPI *FreePages)(
        EFI_PHYSICAL_ADDRESS Memory,
        UINTN NumPages
    );

    EFI_STATUS (EFIAPI *GetMemoryMap)(
        UINTN* MemoryMapSize,
        EFI_MEMORY_DESCRIPTOR* MemoryMap,
        UINTN* MapKey,
        UINTN* DescriptorSize,
        uint32_t* DescriptorVersion
    );

    EFI_STATUS (EFIAPI *AllocatePool)(
        EFI_MEMORY_TYPE PoolType,
        UINTN Size,
        void** Buffer
    );

    EFI_STATUS (EFIAPI *FreePool)(void* Buffer);

    /* Event & Timer Services */
    EFI_STATUS (EFIAPI *CreateEvent)(
        uint32_t Type,
        EFI_TPL NotifyTpl,
        EFIAPI void (*NotifyFunction)(EFI_EVENT, void*),
        void* NotifyContext,
        EFI_EVENT* Event
    );

    EFI_STATUS (EFIAPI *SetTimer)(
        EFI_EVENT Event,
        EFI_TIMER_DELAY Type,
        uint64_t TriggerTime
    );

    EFI_STATUS (EFIAPI *WaitForEvent)(
        UINTN NumberOfEvents,
        EFI_EVENT* Event,
        UINTN* Index
    );

    EFI_STATUS (EFIAPI *SignalEvent)(EFI_EVENT Event);
    EFI_STATUS (EFIAPI *CloseEvent)(EFI_EVENT Event);
    EFI_STATUS (EFIAPI *CheckEvent)(EFI_EVENT Event);

    /* Protocol Handler Services */
    EFI_STATUS (EFIAPI *InstallProtocolInterface)(
        EFI_HANDLE* Handle,
        EFI_GUID* Protocol,
        void* InterfaceType,
        void* Interface
    );

    EFI_STATUS (EFIAPI *ReinstallProtocolInterface)(
        EFI_HANDLE Handle,
        EFI_GUID* Protocol,
        void* OldInterface,
        void* NewInterface
    );

    EFI_STATUS (EFIAPI *UninstallProtocolInterface)(
        EFI_HANDLE Handle,
        EFI_GUID* Protocol,
        void* Interface
    );

    EFI_STATUS (EFIAPI *HandleProtocol)(
        EFI_HANDLE Handle,
        EFI_GUID* Protocol,
        void** Interface
    );

    void* Reserved;

    EFI_STATUS (EFIAPI *LocateHandleBuffer)(
        EFI_GUID* Protocol,
        void* SearchKey,
        UINTN* NoHandles,
        EFI_HANDLE** Buffer
    );

    EFI_STATUS (EFIAPI *LocateHandle)(
        EFI_GUID* Protocol,
        void* SearchKey,
        UINTN* BufferSize,
        EFI_HANDLE* Buffer
    );

    EFI_STATUS (EFIAPI *LocateProtocol)(
        EFI_GUID* Protocol,
        void* Registration,
        void** Interface
    );

    EFI_STATUS (EFIAPI *InstallMultipleProtocolInterfaces)(
        EFI_HANDLE* Handle,
        ...
    );

    EFI_STATUS (EFIAPI *UninstallMultipleProtocolInterfaces)(
        EFI_HANDLE Handle,
        ...
    );

    /* Library Services */
    EFI_STATUS (EFIAPI *OpenProtocolInformation)(
        EFI_HANDLE Handle,
        EFI_GUID* Protocol,
        void** OpenProtocolInformation,
        UINTN* Access
    );

    /* Image Services */
    EFI_STATUS (EFIAPI *LoadImage)(
        int BootPolicy,
        EFI_HANDLE ParentImageHandle,
        void* FilePath,
        UINTN FilePathSize,
        EFI_HANDLE* ImageHandle
    );

    EFI_STATUS (EFIAPI *StartImage)(
        EFI_HANDLE ImageHandle,
        UINTN* ExitDataSize,
        uint16_t** ExitData
    );

    EFI_STATUS (EFIAPI *Exit)(
        EFI_HANDLE ImageHandle,
        EFI_STATUS ExitStatus,
        UINTN ExitDataSize,
        uint16_t* ExitData
    );

    EFI_STATUS (EFIAPI *UnloadImage)(EFI_HANDLE ImageHandle);
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);

    /* Misc Services */
    EFI_STATUS (EFIAPI *GetNextMonotonicCount)(uint64_t* Count);
    EFI_STATUS (EFIAPI *Stall)(UINTN Microseconds);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(
        UINTN Timeout,
        uint64_t WatchdogCode,
        UINTN DataSize,
        uint16_t* WatchdogData
    );

    /* DriverSupport Services */
    EFI_STATUS (EFIAPI *ConnectController)(
        EFI_HANDLE ControllerHandle,
        EFI_HANDLE* DriverImageHandle,
        void* RemainingDevicePath,
        int Recursive
    );

    EFI_STATUS (EFIAPI *DisconnectController)(
        EFI_HANDLE ControllerHandle,
        EFI_HANDLE* DriverImageHandle,
        EFI_HANDLE* ChildHandle
    );

    /* Open and Close Protocol Services */
    EFI_STATUS (EFIAPI *OpenProtocol)(
        EFI_HANDLE Handle,
        EFI_GUID* Protocol,
        void** Interface,
        EFI_HANDLE AgentHandle,
        EFI_HANDLE ControllerHandle,
        uint32_t Attributes
    );

    EFI_STATUS (EFIAPI *CloseProtocol)(
        EFI_HANDLE Handle,
        EFI_GUID* Protocol,
        EFI_HANDLE AgentHandle,
        EFI_HANDLE ControllerHandle
    );

    /* Variable Services */
    EFI_STATUS (EFIAPI *GetVariable)(
        uint16_t* VariableName,
        EFI_GUID* VendorGuid,
        uint32_t* Attributes,
        UINTN* DataSize,
        void* Data
    );

    EFI_STATUS (EFIAPI *GetNextVariableName)(
        UINTN* VariableNameSize,
        uint16_t* VariableName,
        EFI_GUID* VendorGuid
    );

    EFI_STATUS (EFIAPI *SetVariable)(
        uint16_t* VariableName,
        EFI_GUID* VendorGuid,
        uint32_t Attributes,
        UINTN DataSize,
        void* Data
    );
};

/* ========================================================================
 * EFI_RUNTIME_SERVICES
 * ======================================================================== */
struct _EFI_RUNTIME_SERVICES {
    EFI_STATUS (EFIAPI *GetTime)(EFI_TIME* Time, void* Capabilities);
    EFI_STATUS (EFIAPI *SetTime)(EFI_TIME* Time);
    EFI_STATUS (EFIAPI *GetWakeupTime)(int* Enabled, int* Pending, EFI_TIME* Time);
    EFI_STATUS (EFIAPI *SetWakeupTime)(int Enable, EFI_TIME* Time);
    EFI_STATUS (EFIAPI *SetVirtualAddressMap)(
        UINTN MemoryMapSize,
        UINTN DescriptorSize,
        uint32_t DescriptorVersion,
        EFI_MEMORY_DESCRIPTOR* VirtualMap
    );
    EFI_STATUS (EFIAPI *ConvertPointer)(UINTN DebugDisposition, void** Address);
    EFI_STATUS (EFIAPI *GetVariable)(
        uint16_t* VariableName,
        EFI_GUID* VendorGuid,
        uint32_t* Attributes,
        UINTN* DataSize,
        void* Data
    );
    EFI_STATUS (EFIAPI *GetNextVariableName)(
        UINTN* VariableNameSize,
        uint16_t* VariableName,
        EFI_GUID* VendorGuid
    );
    EFI_STATUS (EFIAPI *SetVariable)(
        uint16_t* VariableName,
        EFI_GUID* VendorGuid,
        uint32_t Attributes,
        UINTN DataSize,
        void* Data
    );
    void* Reserved;
};

/* ========================================================================
 * EFI_TABLE_HEADER
 * ======================================================================== */
typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

/* ========================================================================
 * EFI_SYSTEM_TABLE
 * ======================================================================== */
struct _EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER Hdr;
    uint16_t* FirmwareVendor;
    uint32_t FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL* ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* ConOut;
    EFI_HANDLE ConsoleErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* StdErr;
    EFI_RUNTIME_SERVICES* RuntimeServices;
    EFI_BOOT_SERVICES* BootServices;
    UINTN NumberOfTableEntries;
    void** ConfigurationTable;
};

#endif /* UEFI_BOOT_SERVICES_H */
