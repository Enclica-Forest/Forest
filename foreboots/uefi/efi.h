/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/efi.h - Self-contained UEFI type & protocol definitions for x86_64
 * =============================================================================
 * This header is a MINIMAL, dependency-free replacement for gnu-efi. It defines
 * only the UEFI types, calling convention, tables, protocols and GUIDs that the
 * native ForeB UEFI loader (uefi/bootx64.c) actually uses. It links against NO
 * libc and NO gnu-efi.
 *
 * Correctness note: the layouts of EFI_SYSTEM_TABLE, EFI_BOOT_SERVICES,
 * EFI_GRAPHICS_OUTPUT_PROTOCOL, EFI_FILE_PROTOCOL, etc. are a hard ABI contract
 * with real firmware. Function-pointer members whose signatures the loader does
 * not call are declared as plain `void *` PLACEHOLDERS so that every used member
 * still lands at its correct, firmware-defined offset. Do NOT reorder or remove
 * any member of the service tables.
 *
 * Calling convention: UEFI on x86_64 uses the Microsoft x64 ABI. Every firmware
 * entry point therefore carries EFIAPI == __attribute__((ms_abi)). Build the
 * loader with clang -target x86_64-unknown-windows so the C code that CALLS
 * these pointers also emits ms_abi call sites.
 * =============================================================================
 */

#ifndef FOREB_EFI_H
#define FOREB_EFI_H

#include <stdint.h>
#include <stddef.h>

/* -----------------------------------------------------------------------------
 * Calling convention
 * -------------------------------------------------------------------------- */
#if defined(__GNUC__) || defined(__clang__)
#define EFIAPI __attribute__((ms_abi))
#else
#define EFIAPI
#endif

#ifndef IN
#define IN
#endif
#ifndef OUT
#define OUT
#endif
#ifndef OPTIONAL
#define OPTIONAL
#endif
#ifndef CONST
#define CONST const
#endif

/* -----------------------------------------------------------------------------
 * Base scalar types (UEFI spec, Appendix "Data Types")
 * -------------------------------------------------------------------------- */
typedef uint8_t   UINT8;
typedef int8_t    INT8;
typedef uint16_t  UINT16;
typedef int16_t   INT16;
typedef uint32_t  UINT32;
typedef int32_t   INT32;
typedef uint64_t  UINT64;
typedef int64_t   INT64;

/* Natural-width integers: 64-bit on x86_64 UEFI. */
typedef uint64_t  UINTN;
typedef int64_t   INTN;

typedef uint8_t   BOOLEAN;
typedef uint8_t   CHAR8;
typedef uint16_t  CHAR16;   /* UCS-2, use with -fshort-wchar and L"..." */
typedef void      VOID;

#ifndef TRUE
#define TRUE  ((BOOLEAN)1)
#endif
#ifndef FALSE
#define FALSE ((BOOLEAN)0)
#endif
#ifndef NULL
#define NULL ((void *)0)
#endif

/* EFI_STATUS is a UINTN whose top bit signals an error on the native width. */
typedef UINTN EFI_STATUS;

typedef VOID *EFI_HANDLE;
typedef VOID *EFI_EVENT;

typedef UINT64 EFI_PHYSICAL_ADDRESS;
typedef UINT64 EFI_VIRTUAL_ADDRESS;
typedef UINT64 EFI_LBA;
typedef UINTN  EFI_TPL;

/* -----------------------------------------------------------------------------
 * EFI_STATUS values and helpers
 * -------------------------------------------------------------------------- */
#define EFI_ERROR_BIT       ((UINTN)1 << (sizeof(UINTN) * 8 - 1)) /* 0x8000...0 */
#define EFIERR(a)           (EFI_ERROR_BIT | (UINTN)(a))
#define EFI_ERROR(status)   (((INTN)(EFI_STATUS)(status)) < 0)

#define EFI_SUCCESS               ((EFI_STATUS)0)
#define EFI_LOAD_ERROR            EFIERR(1)
#define EFI_INVALID_PARAMETER     EFIERR(2)
#define EFI_UNSUPPORTED           EFIERR(3)
#define EFI_BAD_BUFFER_SIZE       EFIERR(4)
#define EFI_BUFFER_TOO_SMALL      EFIERR(5)
#define EFI_NOT_READY             EFIERR(6)
#define EFI_DEVICE_ERROR          EFIERR(7)
#define EFI_WRITE_PROTECTED       EFIERR(8)
#define EFI_OUT_OF_RESOURCES      EFIERR(9)
#define EFI_VOLUME_CORRUPTED      EFIERR(10)
#define EFI_VOLUME_FULL           EFIERR(11)
#define EFI_NO_MEDIA              EFIERR(12)
#define EFI_MEDIA_CHANGED         EFIERR(13)
#define EFI_NOT_FOUND             EFIERR(14)
#define EFI_ACCESS_DENIED         EFIERR(15)
#define EFI_NO_RESPONSE           EFIERR(16)
#define EFI_NO_MAPPING            EFIERR(17)
#define EFI_TIMEOUT               EFIERR(18)
#define EFI_NOT_STARTED           EFIERR(19)
#define EFI_ALREADY_STARTED       EFIERR(20)
#define EFI_ABORTED               EFIERR(21)
#define EFI_SECURITY_VIOLATION    EFIERR(26)

/* -----------------------------------------------------------------------------
 * EFI_GUID
 * -------------------------------------------------------------------------- */
typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

/* -----------------------------------------------------------------------------
 * EFI_TABLE_HEADER (common to System/Boot/Runtime tables)
 * -------------------------------------------------------------------------- */
typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

/* -----------------------------------------------------------------------------
 * Memory services types
 * -------------------------------------------------------------------------- */
typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

typedef enum {
    EfiReservedMemoryType,
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

/* Memory attribute bits (subset). */
#define EFI_MEMORY_UC             0x0000000000000001ULL
#define EFI_MEMORY_WC             0x0000000000000002ULL
#define EFI_MEMORY_WT             0x0000000000000004ULL
#define EFI_MEMORY_WB             0x0000000000000008ULL
#define EFI_MEMORY_RUNTIME        0x8000000000000000ULL

/* One entry in the map returned by GetMemoryMap. NOTE: firmware reports the
 * real stride via DescriptorSize; never index this array with sizeof(). */
typedef struct {
    UINT32               Type;           /* an EFI_MEMORY_TYPE value */
    UINT32               Pad;            /* natural alignment padding */
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS  VirtualStart;
    UINT64               NumberOfPages;  /* 4 KiB pages */
    UINT64               Attribute;
} EFI_MEMORY_DESCRIPTOR;

#define EFI_PAGE_SIZE   4096U
#define EFI_PAGE_SHIFT  12U

/* -----------------------------------------------------------------------------
 * EFI_TIME (used by EFI_FILE_INFO)
 * -------------------------------------------------------------------------- */
typedef struct {
    UINT16 Year;        /* 1900 - 9999 */
    UINT8  Month;       /* 1 - 12 */
    UINT8  Day;         /* 1 - 31 */
    UINT8  Hour;        /* 0 - 23 */
    UINT8  Minute;      /* 0 - 59 */
    UINT8  Second;      /* 0 - 59 */
    UINT8  Pad1;
    UINT32 Nanosecond;  /* 0 - 999,999,999 */
    INT16  TimeZone;    /* -1440 to 1440 or 2047 */
    UINT8  Daylight;
    UINT8  Pad2;
} EFI_TIME;

/* =============================================================================
 * Simple Text Output Protocol
 * ========================================================================== */
struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef struct {
    INT32   MaxMode;
    INT32   Mode;
    INT32   Attribute;
    INT32   CursorColumn;
    INT32   CursorRow;
    BOOLEAN CursorVisible;
} SIMPLE_TEXT_OUTPUT_MODE;

typedef EFI_STATUS (EFIAPI *EFI_TEXT_RESET)(
    IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    IN BOOLEAN ExtendedVerification);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
    IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    IN CHAR16 *String);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_TEST_STRING)(
    IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    IN CHAR16 *String);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_QUERY_MODE)(
    IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    IN UINTN ModeNumber,
    OUT UINTN *Columns,
    OUT UINTN *Rows);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_MODE)(
    IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    IN UINTN ModeNumber);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_ATTRIBUTE)(
    IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    IN UINTN Attribute);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN)(
    IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_CURSOR_POSITION)(
    IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    IN UINTN Column,
    IN UINTN Row);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_ENABLE_CURSOR)(
    IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    IN BOOLEAN Visible);

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_TEXT_RESET                Reset;
    EFI_TEXT_STRING               OutputString;
    EFI_TEXT_TEST_STRING          TestString;
    EFI_TEXT_QUERY_MODE           QueryMode;
    EFI_TEXT_SET_MODE             SetMode;
    EFI_TEXT_SET_ATTRIBUTE        SetAttribute;
    EFI_TEXT_CLEAR_SCREEN         ClearScreen;
    EFI_TEXT_SET_CURSOR_POSITION  SetCursorPosition;
    EFI_TEXT_ENABLE_CURSOR        EnableCursor;
    SIMPLE_TEXT_OUTPUT_MODE      *Mode;
};

/* Text foreground/background attribute helpers. */
#define EFI_BLACK        0x00
#define EFI_BLUE         0x01
#define EFI_GREEN        0x02
#define EFI_CYAN         0x03
#define EFI_RED          0x04
#define EFI_MAGENTA      0x05
#define EFI_BROWN        0x06
#define EFI_LIGHTGRAY    0x07
#define EFI_DARKGRAY     0x08
#define EFI_LIGHTBLUE    0x09
#define EFI_LIGHTGREEN   0x0A
#define EFI_LIGHTCYAN    0x0B
#define EFI_LIGHTRED     0x0C
#define EFI_LIGHTMAGENTA 0x0D
#define EFI_YELLOW       0x0E
#define EFI_WHITE        0x0F
#define EFI_TEXT_ATTR(fg, bg)  ((fg) | ((bg) << 4))

/* =============================================================================
 * Simple Text Input Protocol (ConIn) - used for the graphical boot menu.
 * ========================================================================== */
struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
typedef struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef struct {
    UINT16 ScanCode;
    CHAR16 UnicodeChar;
} EFI_INPUT_KEY;

typedef EFI_STATUS (EFIAPI *EFI_INPUT_RESET)(
    IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
    IN BOOLEAN ExtendedVerification);

typedef EFI_STATUS (EFIAPI *EFI_INPUT_READ_KEY)(
    IN EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
    OUT EFI_INPUT_KEY *Key);

struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_INPUT_RESET     Reset;
    EFI_INPUT_READ_KEY  ReadKeyStroke;
    EFI_EVENT           WaitForKey;
};

/* Scan codes reported in EFI_INPUT_KEY.ScanCode (UEFI spec, EFI Scan Codes). */
#define SCAN_NULL       0x0000
#define SCAN_UP         0x0001
#define SCAN_DOWN       0x0002
#define SCAN_RIGHT      0x0003
#define SCAN_LEFT       0x0004
#define SCAN_HOME       0x0005
#define SCAN_END        0x0006
#define SCAN_INSERT     0x0007
#define SCAN_DELETE     0x0008
#define SCAN_PAGE_UP    0x0009
#define SCAN_PAGE_DOWN  0x000A
#define SCAN_F1         0x000B
#define SCAN_F2         0x000C
#define SCAN_F3         0x000D
#define SCAN_ESC        0x0017

/* Control characters delivered in EFI_INPUT_KEY.UnicodeChar (ScanCode == 0). */
#define CHAR_NULL       0x0000
#define CHAR_BACKSPACE  0x0008
#define CHAR_TAB        0x0009
#define CHAR_LINEFEED   0x000A
/* Enter arrives as UnicodeChar == carriage return, ScanCode == 0. */
#define CHAR_CR         0x000D

/* GUID that identifies the Simple Text Input protocol on a handle. */
#define EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID \
    { 0x387477c1, 0x69c7, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

/* =============================================================================
 * Boot Services
 * ========================================================================== */
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
    IN EFI_ALLOCATE_TYPE Type,
    IN EFI_MEMORY_TYPE MemoryType,
    IN UINTN Pages,
    IN OUT EFI_PHYSICAL_ADDRESS *Memory);

typedef EFI_STATUS (EFIAPI *EFI_FREE_PAGES)(
    IN EFI_PHYSICAL_ADDRESS Memory,
    IN UINTN Pages);

typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    IN OUT UINTN *MemoryMapSize,
    IN OUT EFI_MEMORY_DESCRIPTOR *MemoryMap,
    OUT UINTN *MapKey,
    OUT UINTN *DescriptorSize,
    OUT UINT32 *DescriptorVersion);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(
    IN EFI_MEMORY_TYPE PoolType,
    IN UINTN Size,
    OUT VOID **Buffer);

typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(
    IN VOID *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(
    IN EFI_HANDLE Handle,
    IN EFI_GUID *Protocol,
    OUT VOID **Interface);

typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(
    IN EFI_HANDLE ImageHandle,
    IN UINTN MapKey);

typedef EFI_STATUS (EFIAPI *EFI_STALL)(
    IN UINTN Microseconds);

typedef EFI_STATUS (EFIAPI *EFI_OPEN_PROTOCOL)(
    IN EFI_HANDLE Handle,
    IN EFI_GUID *Protocol,
    OUT VOID **Interface OPTIONAL,
    IN EFI_HANDLE AgentHandle,
    IN EFI_HANDLE ControllerHandle,
    IN UINT32 Attributes);

/* OpenProtocol attribute bits. */
#define EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL   0x00000001
#define EFI_OPEN_PROTOCOL_GET_PROTOCOL         0x00000002
#define EFI_OPEN_PROTOCOL_TEST_PROTOCOL        0x00000004
#define EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER  0x00000008
#define EFI_OPEN_PROTOCOL_BY_DRIVER            0x00000010
#define EFI_OPEN_PROTOCOL_EXCLUSIVE            0x00000020

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    IN EFI_GUID *Protocol,
    IN VOID *Registration OPTIONAL,
    OUT VOID **Interface);

typedef VOID (EFIAPI *EFI_COPY_MEM)(
    IN VOID *Destination,
    IN VOID *Source,
    IN UINTN Length);

typedef VOID (EFIAPI *EFI_SET_MEM)(
    IN VOID *Buffer,
    IN UINTN Size,
    IN UINT8 Value);

/* -----------------------------------------------------------------------------
 * Event & Timer services (used for timer-driven menu animation + WaitForEvent)
 * -------------------------------------------------------------------------- */
/* EFI_EVENT Type bits (CreateEvent). */
#define EVT_TIMER                          0x80000000u
#define EVT_RUNTIME                        0x40000000u
#define EVT_NOTIFY_WAIT                    0x00000100u
#define EVT_NOTIFY_SIGNAL                  0x00000200u
#define EVT_SIGNAL_EXIT_BOOT_SERVICES      0x00000201u
#define EVT_SIGNAL_VIRTUAL_ADDRESS_CHANGE  0x60000202u

/* Task Priority Levels (NotifyTpl). */
#define TPL_APPLICATION  4
#define TPL_CALLBACK     8
#define TPL_NOTIFY       16
#define TPL_HIGH_LEVEL   31

typedef VOID (EFIAPI *EFI_EVENT_NOTIFY)(
    IN EFI_EVENT Event,
    IN VOID *Context);

typedef EFI_STATUS (EFIAPI *EFI_CREATE_EVENT)(
    IN UINT32 Type,
    IN EFI_TPL NotifyTpl,
    IN EFI_EVENT_NOTIFY NotifyFunction OPTIONAL,
    IN VOID *NotifyContext OPTIONAL,
    OUT EFI_EVENT *Event);

typedef enum {
    TimerCancel,
    TimerPeriodic,
    TimerRelative
} EFI_TIMER_DELAY;

typedef EFI_STATUS (EFIAPI *EFI_SET_TIMER)(
    IN EFI_EVENT Event,
    IN EFI_TIMER_DELAY Type,
    IN UINT64 TriggerTime);   /* units of 100 ns */

typedef EFI_STATUS (EFIAPI *EFI_WAIT_FOR_EVENT)(
    IN UINTN NumberOfEvents,
    IN EFI_EVENT *Event,
    OUT UINTN *Index);

typedef EFI_STATUS (EFIAPI *EFI_CLOSE_EVENT)(
    IN EFI_EVENT Event);

/* -----------------------------------------------------------------------------
 * Handle enumeration (lsblk / drives shell commands)
 * -------------------------------------------------------------------------- */
typedef enum {
    AllHandles,
    ByRegisterNotify,
    ByProtocol
} EFI_LOCATE_SEARCH_TYPE;

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_HANDLE_BUFFER)(
    IN EFI_LOCATE_SEARCH_TYPE SearchType,
    IN EFI_GUID *Protocol OPTIONAL,
    IN VOID *SearchKey OPTIONAL,
    OUT UINTN *NoHandles,
    OUT EFI_HANDLE **Buffer);

/*
 * EFI_BOOT_SERVICES: full member list preserved so every used entry point sits
 * at its firmware-defined offset. Unused entries are `void *` placeholders --
 * do NOT reorder, insert, or delete members.
 */
typedef struct {
    EFI_TABLE_HEADER        Hdr;

    /* Task Priority Services */
    VOID                   *RaiseTPL;                        /* EFI_RAISE_TPL   */
    VOID                   *RestoreTPL;                      /* EFI_RESTORE_TPL */

    /* Memory Services */
    EFI_ALLOCATE_PAGES      AllocatePages;
    EFI_FREE_PAGES          FreePages;
    EFI_GET_MEMORY_MAP      GetMemoryMap;
    EFI_ALLOCATE_POOL       AllocatePool;
    EFI_FREE_POOL           FreePool;

    /* Event & Timer Services */
    EFI_CREATE_EVENT        CreateEvent;
    EFI_SET_TIMER           SetTimer;
    EFI_WAIT_FOR_EVENT      WaitForEvent;
    VOID                   *SignalEvent;
    EFI_CLOSE_EVENT         CloseEvent;
    VOID                   *CheckEvent;

    /* Protocol Handler Services */
    VOID                   *InstallProtocolInterface;
    VOID                   *ReinstallProtocolInterface;
    VOID                   *UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL     HandleProtocol;
    VOID                   *Reserved;
    VOID                   *RegisterProtocolNotify;
    VOID                   *LocateHandle;
    VOID                   *LocateDevicePath;
    VOID                   *InstallConfigurationTable;

    /* Image Services */
    VOID                   *LoadImage;
    VOID                   *StartImage;
    VOID                   *Exit;
    VOID                   *UnloadImage;
    EFI_EXIT_BOOT_SERVICES  ExitBootServices;

    /* Miscellaneous Services */
    VOID                   *GetNextMonotonicCount;
    EFI_STALL               Stall;
    VOID                   *SetWatchdogTimer;

    /* DriverSupport Services */
    VOID                   *ConnectController;
    VOID                   *DisconnectController;

    /* Open and Close Protocol Services */
    EFI_OPEN_PROTOCOL       OpenProtocol;
    VOID                   *CloseProtocol;
    VOID                   *OpenProtocolInformation;

    /* Library Services */
    VOID                     *ProtocolsPerHandle;
    EFI_LOCATE_HANDLE_BUFFER  LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL       LocateProtocol;
    VOID                   *InstallMultipleProtocolInterfaces;
    VOID                   *UninstallMultipleProtocolInterfaces;

    /* 32-bit CRC Services */
    VOID                   *CalculateCrc32;

    /* Miscellaneous Services (continued) */
    EFI_COPY_MEM            CopyMem;
    EFI_SET_MEM             SetMem;
    VOID                   *CreateEventEx;
} EFI_BOOT_SERVICES;

/* =============================================================================
 * Runtime Services (header-only; loader does not call any member before exit).
 * The full member list is not required because the System Table references it
 * through a pointer, so its internal layout does not shift other offsets.
 * ========================================================================== */
typedef enum {
    EfiResetCold,
    EfiResetWarm,
    EfiResetShutdown,
    EfiResetPlatformSpecific
} EFI_RESET_TYPE;

/* ResetSystem() lives at its correct offset in EFI_RUNTIME_SERVICES below as a
 * VOID* placeholder; cast it to this signature to invoke a machine reset (valid
 * both before and after ExitBootServices). */
typedef VOID (EFIAPI *EFI_RESET_SYSTEM)(
    IN EFI_RESET_TYPE ResetType,
    IN EFI_STATUS ResetStatus,
    IN UINTN DataSize,
    IN VOID *ResetData OPTIONAL);

/* UEFI variable attribute bits (GetVariable Attributes out / SetVariable in). */
#define EFI_VARIABLE_NON_VOLATILE                          0x00000001u
#define EFI_VARIABLE_BOOTSERVICE_ACCESS                    0x00000002u
#define EFI_VARIABLE_RUNTIME_ACCESS                        0x00000004u
#define EFI_VARIABLE_HARDWARE_ERROR_RECORD                 0x00000008u
#define EFI_VARIABLE_AUTHENTICATED_WRITE_ACCESS            0x00000010u
#define EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS 0x00000020u
#define EFI_VARIABLE_APPEND_WRITE                          0x00000040u

typedef EFI_STATUS (EFIAPI *EFI_GET_VARIABLE)(
    IN CHAR16 *VariableName,
    IN EFI_GUID *VendorGuid,
    OUT UINT32 *Attributes OPTIONAL,
    IN OUT UINTN *DataSize,
    OUT VOID *Data OPTIONAL);

typedef EFI_STATUS (EFIAPI *EFI_GET_NEXT_VARIABLE_NAME)(
    IN OUT UINTN *VariableNameSize,
    IN OUT CHAR16 *VariableName,
    IN OUT EFI_GUID *VendorGuid);

typedef EFI_STATUS (EFIAPI *EFI_SET_VARIABLE)(
    IN CHAR16 *VariableName,
    IN EFI_GUID *VendorGuid,
    IN UINT32 Attributes,
    IN UINTN DataSize,
    IN VOID *Data);

/*
 * Variable + ResetSystem members carry real callable typedefs (used by the
 * shell: efivars/bootvars list vars, reboot resets). All other members remain
 * VOID* placeholders so every callable entry keeps its firmware-defined offset.
 */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    VOID *GetTime;
    VOID *SetTime;
    VOID *GetWakeupTime;
    VOID *SetWakeupTime;
    VOID *SetVirtualAddressMap;
    VOID *ConvertPointer;
    EFI_GET_VARIABLE            GetVariable;
    EFI_GET_NEXT_VARIABLE_NAME  GetNextVariableName;
    EFI_SET_VARIABLE            SetVariable;
    VOID *GetNextHighMonotonicCount;
    EFI_RESET_SYSTEM            ResetSystem;
    VOID *UpdateCapsule;
    VOID *QueryCapsuleCapabilities;
    VOID *QueryVariableInfo;
} EFI_RUNTIME_SERVICES;

/* =============================================================================
 * EFI Configuration Table
 * ========================================================================== */
typedef struct {
    EFI_GUID  VendorGuid;
    VOID     *VendorTable;
} EFI_CONFIGURATION_TABLE;

/* =============================================================================
 * EFI System Table
 * ========================================================================== */
typedef struct {
    EFI_TABLE_HEADER                  Hdr;
    CHAR16                           *FirmwareVendor;
    UINT32                            FirmwareRevision;
    EFI_HANDLE                        ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL   *ConIn;
    EFI_HANDLE                        ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *ConOut;
    EFI_HANDLE                        StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *StdErr;
    EFI_RUNTIME_SERVICES             *RuntimeServices;
    EFI_BOOT_SERVICES                *BootServices;
    UINTN                             NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE          *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* =============================================================================
 * Graphics Output Protocol (GOP)
 * ========================================================================== */
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,  /* RGBA (byte order R,G,B,X)      */
    PixelBlueGreenRedReserved8BitPerColor,  /* BGRA (byte order B,G,R,X) <- x86*/
    PixelBitMask,                           /* use PixelInformation bitmasks   */
    PixelBltOnly,                           /* no direct framebuffer access    */
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32                    Version;
    UINT32                    HorizontalResolution;
    UINT32                    VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK         PixelInformation;
    UINT32                    PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    UINT32                                MaxMode;
    UINT32                                Mode;            /* current mode number*/
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN                                 SizeOfInfo;
    EFI_PHYSICAL_ADDRESS                  FrameBufferBase;
    UINTN                                 FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL;
typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* Blt pixel + operation are declared for completeness (Blt() is a placeholder). */
typedef struct {
    UINT8 Blue;
    UINT8 Green;
    UINT8 Red;
    UINT8 Reserved;
} EFI_GRAPHICS_OUTPUT_BLT_PIXEL;

typedef enum {
    EfiBltVideoFill,
    EfiBltVideoToBltBuffer,
    EfiBltBufferToVideo,
    EfiBltVideoToVideo,
    EfiGraphicsOutputBltOperationMax
} EFI_GRAPHICS_OUTPUT_BLT_OPERATION;

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE)(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    IN UINT32 ModeNumber,
    OUT UINTN *SizeOfInfo,
    OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE)(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    IN UINT32 ModeNumber);

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT)(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    IN OUT EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer OPTIONAL,
    IN EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
    IN UINTN SourceX,
    IN UINTN SourceY,
    IN UINTN DestinationX,
    IN UINTN DestinationY,
    IN UINTN Width,
    IN UINTN Height,
    IN UINTN Delta OPTIONAL);

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE  QueryMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE    SetMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT         Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE       *Mode;
};

/* =============================================================================
 * Simple File System + File Protocol
 * ========================================================================== */
struct _EFI_FILE_PROTOCOL;
typedef struct _EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;

struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME)(
    IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
    OUT EFI_FILE_PROTOCOL **Root);

struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64                                       Revision;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME  OpenVolume;
};

/* File open modes (Open() OpenMode parameter). */
#define EFI_FILE_MODE_READ    0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE   0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE  0x8000000000000000ULL

/* File attributes. */
#define EFI_FILE_READ_ONLY    0x0000000000000001ULL
#define EFI_FILE_HIDDEN       0x0000000000000002ULL
#define EFI_FILE_SYSTEM       0x0000000000000004ULL
#define EFI_FILE_RESERVED     0x0000000000000008ULL
#define EFI_FILE_DIRECTORY    0x0000000000000010ULL
#define EFI_FILE_ARCHIVE      0x0000000000000020ULL
#define EFI_FILE_VALID_ATTR   0x0000000000000037ULL

typedef EFI_STATUS (EFIAPI *EFI_FILE_OPEN)(
    IN EFI_FILE_PROTOCOL *This,
    OUT EFI_FILE_PROTOCOL **NewHandle,
    IN CHAR16 *FileName,
    IN UINT64 OpenMode,
    IN UINT64 Attributes);

typedef EFI_STATUS (EFIAPI *EFI_FILE_CLOSE)(
    IN EFI_FILE_PROTOCOL *This);

typedef EFI_STATUS (EFIAPI *EFI_FILE_DELETE)(
    IN EFI_FILE_PROTOCOL *This);

typedef EFI_STATUS (EFIAPI *EFI_FILE_READ)(
    IN EFI_FILE_PROTOCOL *This,
    IN OUT UINTN *BufferSize,
    OUT VOID *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_FILE_WRITE)(
    IN EFI_FILE_PROTOCOL *This,
    IN OUT UINTN *BufferSize,
    IN VOID *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_FILE_GET_POSITION)(
    IN EFI_FILE_PROTOCOL *This,
    OUT UINT64 *Position);

typedef EFI_STATUS (EFIAPI *EFI_FILE_SET_POSITION)(
    IN EFI_FILE_PROTOCOL *This,
    IN UINT64 Position);

typedef EFI_STATUS (EFIAPI *EFI_FILE_GET_INFO)(
    IN EFI_FILE_PROTOCOL *This,
    IN EFI_GUID *InformationType,
    IN OUT UINTN *BufferSize,
    OUT VOID *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_FILE_SET_INFO)(
    IN EFI_FILE_PROTOCOL *This,
    IN EFI_GUID *InformationType,
    IN UINTN BufferSize,
    IN VOID *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_FILE_FLUSH)(
    IN EFI_FILE_PROTOCOL *This);

/* EFI_FILE_PROTOCOL revision 1 layout, extended with the rev-2 async members as
 * placeholders so the struct size/layout matches firmware. */
struct _EFI_FILE_PROTOCOL {
    UINT64                 Revision;
    EFI_FILE_OPEN          Open;
    EFI_FILE_CLOSE         Close;
    EFI_FILE_DELETE        Delete;
    EFI_FILE_READ          Read;
    EFI_FILE_WRITE         Write;
    EFI_FILE_GET_POSITION  GetPosition;
    EFI_FILE_SET_POSITION  SetPosition;
    EFI_FILE_GET_INFO      GetInfo;
    EFI_FILE_SET_INFO      SetInfo;
    EFI_FILE_FLUSH         Flush;
    /* Revision 2 (EFI_FILE_PROTOCOL_REVISION2) async I/O -- unused placeholders */
    VOID                  *OpenEx;
    VOID                  *ReadEx;
    VOID                  *WriteEx;
    VOID                  *FlushEx;
};

#define EFI_FILE_PROTOCOL_REVISION   0x00010000
#define EFI_FILE_PROTOCOL_REVISION2  0x00020000

/* EFI_FILE_INFO: variable-length; FileName is a NUL-terminated CHAR16 tail. */
typedef struct {
    UINT64   Size;          /* total size of this record incl. FileName */
    UINT64   FileSize;      /* bytes of file contents */
    UINT64   PhysicalSize;
    EFI_TIME CreateTime;
    EFI_TIME LastAccessTime;
    EFI_TIME ModificationTime;
    UINT64   Attribute;
    CHAR16   FileName[1];   /* actually FileName[]; [1] keeps ANSI C happy */
} EFI_FILE_INFO;

/* =============================================================================
 * Loaded Image Protocol
 * ========================================================================== */
struct _EFI_DEVICE_PATH_PROTOCOL;
typedef struct _EFI_DEVICE_PATH_PROTOCOL EFI_DEVICE_PATH_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_IMAGE_UNLOAD)(
    IN EFI_HANDLE ImageHandle);

typedef struct {
    UINT32                    Revision;
    EFI_HANDLE                ParentHandle;
    EFI_SYSTEM_TABLE         *SystemTable;
    /* Source location of the image */
    EFI_HANDLE                DeviceHandle;
    EFI_DEVICE_PATH_PROTOCOL *FilePath;
    VOID                     *Reserved;
    /* Image's load options */
    UINT32                    LoadOptionsSize;
    VOID                     *LoadOptions;
    /* Location where image was loaded */
    VOID                     *ImageBase;
    UINT64                    ImageSize;
    EFI_MEMORY_TYPE           ImageCodeType;
    EFI_MEMORY_TYPE           ImageDataType;
    EFI_IMAGE_UNLOAD          Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

#define EFI_LOADED_IMAGE_PROTOCOL_REVISION  0x1000

/* =============================================================================
 * Block I/O Protocol (raw sector access for lsblk / read / write shell cmds)
 * ========================================================================== */
struct _EFI_BLOCK_IO_PROTOCOL;
typedef struct _EFI_BLOCK_IO_PROTOCOL EFI_BLOCK_IO_PROTOCOL;

/* Media descriptor. Firmware-defined layout -- do NOT reorder. Revision-2/3
 * fields are appended; older media simply do not touch them. */
typedef struct {
    UINT32  MediaId;
    BOOLEAN RemovableMedia;
    BOOLEAN MediaPresent;
    BOOLEAN LogicalPartition;
    BOOLEAN ReadOnly;
    BOOLEAN WriteCaching;
    UINT32  BlockSize;
    UINT32  IoAlign;
    EFI_LBA LastBlock;
    /* Revision 2 (EFI_BLOCK_IO_PROTOCOL_REVISION2) */
    EFI_LBA LowestAlignedLba;
    UINT32  LogicalBlocksPerPhysicalBlock;
    /* Revision 3 (EFI_BLOCK_IO_PROTOCOL_REVISION3) */
    UINT32  OptimalTransferLengthGranularity;
} EFI_BLOCK_IO_MEDIA;

typedef EFI_STATUS (EFIAPI *EFI_BLOCK_RESET)(
    IN EFI_BLOCK_IO_PROTOCOL *This,
    IN BOOLEAN ExtendedVerification);

typedef EFI_STATUS (EFIAPI *EFI_BLOCK_READ)(
    IN EFI_BLOCK_IO_PROTOCOL *This,
    IN UINT32 MediaId,
    IN EFI_LBA Lba,
    IN UINTN BufferSize,
    OUT VOID *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_BLOCK_WRITE)(
    IN EFI_BLOCK_IO_PROTOCOL *This,
    IN UINT32 MediaId,
    IN EFI_LBA Lba,
    IN UINTN BufferSize,
    IN VOID *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_BLOCK_FLUSH)(
    IN EFI_BLOCK_IO_PROTOCOL *This);

struct _EFI_BLOCK_IO_PROTOCOL {
    UINT64               Revision;
    EFI_BLOCK_IO_MEDIA  *Media;
    EFI_BLOCK_RESET      Reset;
    EFI_BLOCK_READ       ReadBlocks;
    EFI_BLOCK_WRITE      WriteBlocks;
    EFI_BLOCK_FLUSH      FlushBlocks;
};

#define EFI_BLOCK_IO_PROTOCOL_REVISION   0x00010000
#define EFI_BLOCK_IO_PROTOCOL_REVISION2  0x00020001
#define EFI_BLOCK_IO_PROTOCOL_REVISION3  0x0002001F

/* =============================================================================
 * Protocol / info GUIDs
 * ========================================================================== */
#define EFI_BLOCK_IO_PROTOCOL_GUID \
    { 0x964e5b21, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

/* Vendor GUID for the standard UEFI global variables (Boot####, BootOrder,
 * Timeout, ...). Used by the shell's bootvars/efivars listing. */
#define EFI_GLOBAL_VARIABLE \
    { 0x8be4df61, 0x93ca, 0x11d2, { 0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c } }

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x0964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#define EFI_FILE_INFO_ID \
    { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

/* Optional: ready-to-use static instances. Guard with FOREB_EFI_DEFINE_GUIDS in
 * exactly one translation unit if you prefer named globals over inline inits. */
#ifdef FOREB_EFI_DEFINE_GUIDS
EFI_GUID gEfiGraphicsOutputProtocolGuid   = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
EFI_GUID gEfiSimpleFileSystemProtocolGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
EFI_GUID gEfiLoadedImageProtocolGuid      = EFI_LOADED_IMAGE_PROTOCOL_GUID;
EFI_GUID gEfiFileInfoGuid                 = EFI_FILE_INFO_ID;
EFI_GUID gEfiBlockIoProtocolGuid          = EFI_BLOCK_IO_PROTOCOL_GUID;
EFI_GUID gEfiSimpleTextInputProtocolGuid  = EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID;
EFI_GUID gEfiGlobalVariableGuid           = EFI_GLOBAL_VARIABLE;
#endif

#endif /* FOREB_EFI_H */
