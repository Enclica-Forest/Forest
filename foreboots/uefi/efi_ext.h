/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/efi_ext.h - EXTENSION protocol/service/GUID declarations
 * =============================================================================
 * This header is a strict, additive companion to efi.h. It declares ONLY the
 * UEFI protocols, boot-service call signatures and GUIDs that efi.h does not
 * yet expose but the new ForeB upgrade needs:
 *
 *   - EFI_SIMPLE_POINTER_PROTOCOL   (mouse: relative movement + buttons)
 *   - EFI_ABSOLUTE_POINTER_PROTOCOL (mouse/touch/tablet: absolute coords)
 *   - Callable signatures for BootServices->{LoadImage, StartImage, Exit,
 *     UnloadImage, LocateDevicePath, InstallMultipleProtocolInterfaces,
 *     UninstallMultipleProtocolInterfaces, ConnectController}. In efi.h these
 *     members are VOID* placeholders sitting at their firmware-defined offsets;
 *     here we give the real prototypes plus tiny cast-and-call inline wrappers
 *     so callers never write the cast by hand. We do NOT redefine the service
 *     table -- offsets are untouched.
 *   - EFI_DEVICE_PATH_PROTOCOL real node body + node type/subtype constants +
 *     ready-made node structs (file-path, vendor, end) and small builders.
 *   - EFI_LOAD_FILE2_PROTOCOL + LINUX_EFI_INITRD_MEDIA_GUID (Linux EFI-stub
 *     initrd delivery for `type=linux` entries).
 *   - EFI_DISK_IO_PROTOCOL (byte-granular reads for ext2/3/4 + recovery tools).
 *   - Convenience GUIDs.
 *
 * DESIGN RULE: nothing here shifts an offset in an efi.h service table. Every
 * BootServices helper casts the existing VOID* member to a proper function
 * pointer type and calls it -- the member already lives at the correct slot.
 *
 * Include order: include "efi.h" first (this header pulls it in defensively).
 * All firmware entry points carry EFIAPI, exactly as in efi.h. For AArch64 the
 * ms_abi convention still applies; for RISC-V see arch.h (the standard C ABI is
 * used and EFIAPI must be empty).
 * =============================================================================
 */

#ifndef FOREB_EFI_EXT_H
#define FOREB_EFI_EXT_H

#include "efi.h"

/* -----------------------------------------------------------------------------
 * OsIndications runtime-variable bits (UEFI spec 8.5.4).
 * The OsIndications / OsIndicationsSupported UINT64 global variables live under
 * EFI_GLOBAL_VARIABLE (efi.h) with the variable attributes EFI_VARIABLE_* (also
 * efi.h). Setting BOOT_TO_FW_UI in OsIndications then ResetSystem() asks the
 * firmware to enter its setup UI on the next boot (see uefi/fwsetup.c). Only the
 * bit ForeB uses is declared here; guarded so tools.h/fwsetup.h may repeat it.
 * --------------------------------------------------------------------------- */
#ifndef EFI_OS_INDICATIONS_BOOT_TO_FW_UI
#define EFI_OS_INDICATIONS_BOOT_TO_FW_UI  0x0000000000000001ULL
#endif

/* =============================================================================
 * 1. EFI_SIMPLE_POINTER_PROTOCOL  (UEFI spec 12.5)
 * -----------------------------------------------------------------------------
 * Relative-motion pointing device (PS/2-style mouse). RelativeMovement* are in
 * device units; scale by Mode->Resolution* (counts per mm) or just accumulate.
 * ========================================================================== */
struct _EFI_SIMPLE_POINTER_PROTOCOL;
typedef struct _EFI_SIMPLE_POINTER_PROTOCOL EFI_SIMPLE_POINTER_PROTOCOL;

typedef struct {
    INT32   RelativeMovementX;   /* signed motion since last GetState */
    INT32   RelativeMovementY;
    INT32   RelativeMovementZ;   /* wheel */
    BOOLEAN LeftButton;
    BOOLEAN RightButton;
} EFI_SIMPLE_POINTER_STATE;

typedef struct {
    UINT64  ResolutionX;         /* counts per mm on X */
    UINT64  ResolutionY;
    UINT64  ResolutionZ;
    BOOLEAN LeftButton;          /* device HAS a left button  */
    BOOLEAN RightButton;         /* device HAS a right button */
} EFI_SIMPLE_POINTER_MODE;

typedef EFI_STATUS (EFIAPI *EFI_SIMPLE_POINTER_RESET)(
    IN EFI_SIMPLE_POINTER_PROTOCOL *This,
    IN BOOLEAN ExtendedVerification);

typedef EFI_STATUS (EFIAPI *EFI_SIMPLE_POINTER_GET_STATE)(
    IN EFI_SIMPLE_POINTER_PROTOCOL *This,
    OUT EFI_SIMPLE_POINTER_STATE *State);   /* EFI_NOT_READY if no new data */

struct _EFI_SIMPLE_POINTER_PROTOCOL {
    EFI_SIMPLE_POINTER_RESET      Reset;
    EFI_SIMPLE_POINTER_GET_STATE  GetState;
    EFI_EVENT                     WaitForInput;
    EFI_SIMPLE_POINTER_MODE      *Mode;
};

#define EFI_SIMPLE_POINTER_PROTOCOL_GUID \
    { 0x31878c87, 0x0b75, 0x11d5, { 0x9a, 0x4f, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

/* =============================================================================
 * 2. EFI_ABSOLUTE_POINTER_PROTOCOL  (UEFI spec 12.7)
 * -----------------------------------------------------------------------------
 * Absolute-coordinate pointer (touchscreen / QEMU `-device usb-tablet`). This
 * is the PREFERRED cursor source under QEMU: CurrentX/Y are already in the
 * protocol's own coordinate space [AbsoluteMin, AbsoluteMax]; map linearly onto
 * the GOP resolution to place the cursor sprite.
 * ========================================================================== */
struct _EFI_ABSOLUTE_POINTER_PROTOCOL;
typedef struct _EFI_ABSOLUTE_POINTER_PROTOCOL EFI_ABSOLUTE_POINTER_PROTOCOL;

typedef struct {
    UINT64 AbsoluteMinX;
    UINT64 AbsoluteMinY;
    UINT64 AbsoluteMinZ;
    UINT64 AbsoluteMaxX;
    UINT64 AbsoluteMaxY;
    UINT64 AbsoluteMaxZ;
    UINT32 Attributes;
} EFI_ABSOLUTE_POINTER_MODE;

/* EFI_ABSOLUTE_POINTER_MODE.Attributes bits */
#define EFI_ABSP_SupportsAltActive    0x00000001u
#define EFI_ABSP_SupportsPressureAsZ  0x00000002u

typedef struct {
    UINT64 CurrentX;
    UINT64 CurrentY;
    UINT64 CurrentZ;             /* pressure, if SupportsPressureAsZ */
    UINT32 ActiveButtons;        /* bit0 = touch/left active */
} EFI_ABSOLUTE_POINTER_STATE;

/* EFI_ABSOLUTE_POINTER_STATE.ActiveButtons bits */
#define EFI_ABSP_TouchActive          0x00000001u
#define EFI_ABS_POINTER_AltActive     0x00000002u

typedef EFI_STATUS (EFIAPI *EFI_ABSOLUTE_POINTER_RESET)(
    IN EFI_ABSOLUTE_POINTER_PROTOCOL *This,
    IN BOOLEAN ExtendedVerification);

typedef EFI_STATUS (EFIAPI *EFI_ABSOLUTE_POINTER_GET_STATE)(
    IN EFI_ABSOLUTE_POINTER_PROTOCOL *This,
    OUT EFI_ABSOLUTE_POINTER_STATE *State);   /* EFI_NOT_READY if no new data */

struct _EFI_ABSOLUTE_POINTER_PROTOCOL {
    EFI_ABSOLUTE_POINTER_RESET      Reset;
    EFI_ABSOLUTE_POINTER_GET_STATE  GetState;
    EFI_EVENT                       WaitForInput;
    EFI_ABSOLUTE_POINTER_MODE      *Mode;
};

#define EFI_ABSOLUTE_POINTER_PROTOCOL_GUID \
    { 0x8d59d32b, 0xc655, 0x4ae9, { 0x9b, 0x15, 0xf2, 0x59, 0x04, 0x99, 0x2a, 0x43 } }

/* =============================================================================
 * 3. EFI_DEVICE_PATH_PROTOCOL  (real node body + constants + builders)
 * -----------------------------------------------------------------------------
 * efi.h forward-declares `struct _EFI_DEVICE_PATH_PROTOCOL` as opaque; we give
 * it its true single-node header here. A device path is a packed sequence of
 * these nodes terminated by an END node. Length is a 2-byte LITTLE-ENDIAN field
 * stored as a byte pair to avoid alignment assumptions.
 * ========================================================================== */
struct _EFI_DEVICE_PATH_PROTOCOL {
    UINT8 Type;
    UINT8 SubType;
    UINT8 Length[2];   /* total node length incl. this header, little-endian */
};

/* Node Type values */
#define HARDWARE_DEVICE_PATH   0x01
#define ACPI_DEVICE_PATH       0x02
#define MESSAGING_DEVICE_PATH  0x03
#define MEDIA_DEVICE_PATH      0x04
#define BBS_DEVICE_PATH        0x05
#define END_DEVICE_PATH_TYPE   0x7f

/* MEDIA_DEVICE_PATH SubTypes */
#define MEDIA_HARDDRIVE_DP     0x01
#define MEDIA_CDROM_DP         0x02
#define MEDIA_VENDOR_DP        0x03
#define MEDIA_FILEPATH_DP      0x04
#define MEDIA_PROTOCOL_DP      0x05
#define MEDIA_PIWG_FW_FILE_DP  0x06
#define MEDIA_PIWG_FW_VOL_DP   0x07
#define MEDIA_RAM_DISK_DP      0x09

/* END_DEVICE_PATH_TYPE SubTypes */
#define END_INSTANCE_DP_SUBTYPE  0x01
#define END_ENTIRE_DP_SUBTYPE    0xff

/* Little-endian Length helpers (nodes are not 2-byte aligned in general). */
#define EFI_DP_NODE_LEN(node)  ((UINT16)((node)->Length[0] | ((UINT16)(node)->Length[1] << 8)))
#define EFI_DP_SET_LEN(node, len) do { \
        (node)->Length[0] = (UINT8)((len) & 0xff); \
        (node)->Length[1] = (UINT8)(((len) >> 8) & 0xff); \
    } while (0)

/* Advance to the next node in a packed device path. */
#define EFI_DP_NEXT(node) \
    ((EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)(node) + EFI_DP_NODE_LEN(node)))

#define EFI_DP_IS_END(node) \
    ((node)->Type == END_DEVICE_PATH_TYPE && (node)->SubType == END_ENTIRE_DP_SUBTYPE)

/* MEDIA_FILEPATH_DP node: a CHAR16 path relative to the volume root, e.g.
 * L"\\forebo\\vmlinuz" or L"\\EFI\\BOOT\\BOOTX64.EFI". Declared with a 1-elem
 * tail; over-allocate the real path length + terminator. */
typedef struct {
    EFI_DEVICE_PATH_PROTOCOL Header;   /* Type=MEDIA, SubType=MEDIA_FILEPATH_DP */
    CHAR16                   PathName[1];
} FILEPATH_DEVICE_PATH;

/* MEDIA_VENDOR_DP node: a GUID that identifies a vendor-specific media source.
 * Used to publish the Linux initrd handle (Guid = LINUX_EFI_INITRD_MEDIA). */
typedef struct {
    EFI_DEVICE_PATH_PROTOCOL Header;   /* Type=MEDIA, SubType=MEDIA_VENDOR_DP */
    EFI_GUID                 Guid;
} VENDOR_DEVICE_PATH;

#define EFI_DEVICE_PATH_PROTOCOL_GUID \
    { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

/* Optional firmware helpers to convert text<->device path. Prefer building
 * paths by hand (portable across ARM/RISC-V firmware); these are only used when
 * present. */
#define EFI_DEVICE_PATH_FROM_TEXT_PROTOCOL_GUID \
    { 0x05c99a21, 0xc70f, 0x4ad2, { 0x8a, 0x5f, 0x35, 0xdf, 0x33, 0x43, 0xf5, 0x1e } }
#define EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID \
    { 0x8b843e20, 0x8132, 0x4852, { 0x90, 0xcc, 0x55, 0x1a, 0x4e, 0x4a, 0x7f, 0x1c } }

/* =============================================================================
 * 4. Image + protocol-install BootServices call signatures
 * -----------------------------------------------------------------------------
 * These match the VOID* members already present in EFI_BOOT_SERVICES (efi.h).
 * Use the inline wrappers below so call sites stay readable and the cast lives
 * in exactly one place.
 * ========================================================================== */
typedef EFI_STATUS (EFIAPI *EFI_IMAGE_LOAD)(
    IN BOOLEAN BootPolicy,
    IN EFI_HANDLE ParentImageHandle,
    IN EFI_DEVICE_PATH_PROTOCOL *DevicePath OPTIONAL,
    IN VOID *SourceBuffer OPTIONAL,
    IN UINTN SourceSize,
    OUT EFI_HANDLE *ImageHandle);

typedef EFI_STATUS (EFIAPI *EFI_IMAGE_START)(
    IN EFI_HANDLE ImageHandle,
    OUT UINTN *ExitDataSize,
    OUT CHAR16 **ExitData OPTIONAL);

typedef EFI_STATUS (EFIAPI *EFI_EXIT)(
    IN EFI_HANDLE ImageHandle,
    IN EFI_STATUS ExitStatus,
    IN UINTN ExitDataSize,
    IN CHAR16 *ExitData OPTIONAL);

/* NOTE: efi.h already declares EFI_IMAGE_UNLOAD (used by LoadedImage.Unload);
 * BootServices->UnloadImage has the identical signature, so we reuse it. */

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_DEVICE_PATH)(
    IN EFI_GUID *Protocol,
    IN OUT EFI_DEVICE_PATH_PROTOCOL **DevicePath,
    OUT EFI_HANDLE *Device);

typedef EFI_STATUS (EFIAPI *EFI_INSTALL_MULTIPLE_PROTOCOL_INTERFACES)(
    IN OUT EFI_HANDLE *Handle, ...);   /* NULL-terminated (GUID*, iface) pairs */

typedef EFI_STATUS (EFIAPI *EFI_UNINSTALL_MULTIPLE_PROTOCOL_INTERFACES)(
    IN EFI_HANDLE Handle, ...);

typedef EFI_STATUS (EFIAPI *EFI_CONNECT_CONTROLLER)(
    IN EFI_HANDLE ControllerHandle,
    IN EFI_HANDLE *DriverImageHandle OPTIONAL,
    IN EFI_DEVICE_PATH_PROTOCOL *RemainingDevicePath OPTIONAL,
    IN BOOLEAN Recursive);

/* --- cast-and-call wrappers (member already at correct offset) ------------- */
static inline EFI_STATUS foreb_LoadImage(
        EFI_BOOT_SERVICES *bs, BOOLEAN bootPolicy, EFI_HANDLE parent,
        EFI_DEVICE_PATH_PROTOCOL *dp, VOID *src, UINTN srcSize,
        EFI_HANDLE *outImage) {
    return ((EFI_IMAGE_LOAD)bs->LoadImage)(bootPolicy, parent, dp, src, srcSize, outImage);
}
static inline EFI_STATUS foreb_StartImage(
        EFI_BOOT_SERVICES *bs, EFI_HANDLE image,
        UINTN *exitDataSize, CHAR16 **exitData) {
    return ((EFI_IMAGE_START)bs->StartImage)(image, exitDataSize, exitData);
}
static inline EFI_STATUS foreb_Exit(
        EFI_BOOT_SERVICES *bs, EFI_HANDLE image, EFI_STATUS st,
        UINTN exitDataSize, CHAR16 *exitData) {
    return ((EFI_EXIT)bs->Exit)(image, st, exitDataSize, exitData);
}
static inline EFI_STATUS foreb_UnloadImage(EFI_BOOT_SERVICES *bs, EFI_HANDLE image) {
    return ((EFI_IMAGE_UNLOAD)bs->UnloadImage)(image);
}
static inline EFI_STATUS foreb_LocateDevicePath(
        EFI_BOOT_SERVICES *bs, EFI_GUID *proto,
        EFI_DEVICE_PATH_PROTOCOL **dp, EFI_HANDLE *dev) {
    return ((EFI_LOCATE_DEVICE_PATH)bs->LocateDevicePath)(proto, dp, dev);
}
static inline EFI_STATUS foreb_ConnectController(
        EFI_BOOT_SERVICES *bs, EFI_HANDLE ctrl, EFI_HANDLE *drivers,
        EFI_DEVICE_PATH_PROTOCOL *remaining, BOOLEAN recursive) {
    return ((EFI_CONNECT_CONTROLLER)bs->ConnectController)(ctrl, drivers, remaining, recursive);
}
/* InstallMultipleProtocolInterfaces is variadic: cast the member and call it
 * directly at the site so the compiler sees the varargs (an inline wrapper
 * cannot forward C varargs). Example:
 *   ((EFI_INSTALL_MULTIPLE_PROTOCOL_INTERFACES)bs->InstallMultipleProtocolInterfaces)(
 *       &handle, &gEfiDevicePathProtocolGuid, dp,
 *               &gEfiLoadFile2ProtocolGuid, &initrdLoadFile2, NULL); */

/* =============================================================================
 * 5. EFI_LOAD_FILE2_PROTOCOL + LINUX_EFI_INITRD_MEDIA  (initrd for Linux stub)
 * -----------------------------------------------------------------------------
 * The modern Linux EFI stub fetches its initrd by locating a handle that (a)
 * has a MEDIA_VENDOR device path whose GUID == LINUX_EFI_INITRD_MEDIA_GUID and
 * (b) exposes EFI_LOAD_FILE2_PROTOCOL. It calls LoadFile with BootPolicy=FALSE:
 * first with Buffer==NULL to size, then again to fill. Publish both interfaces
 * on a throw-away handle via InstallMultipleProtocolInterfaces before
 * StartImage(vmlinuz), and uninstall afterward.
 * ========================================================================== */
struct _EFI_LOAD_FILE2_PROTOCOL;
typedef struct _EFI_LOAD_FILE2_PROTOCOL EFI_LOAD_FILE2_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_LOAD_FILE2)(
    IN EFI_LOAD_FILE2_PROTOCOL *This,
    IN EFI_DEVICE_PATH_PROTOCOL *FilePath,
    IN BOOLEAN BootPolicy,          /* MUST be FALSE for LoadFile2 */
    IN OUT UINTN *BufferSize,
    OUT VOID *Buffer OPTIONAL);

struct _EFI_LOAD_FILE2_PROTOCOL {
    EFI_LOAD_FILE2 LoadFile;
};

#define EFI_LOAD_FILE2_PROTOCOL_GUID \
    { 0x4006c0c1, 0xfcb3, 0x403e, { 0x99, 0x6d, 0x4a, 0x6c, 0x87, 0x24, 0xe0, 0x6d } }

/* The vendor GUID the Linux stub searches for on the initrd media handle. */
#define LINUX_EFI_INITRD_MEDIA_GUID \
    { 0x5568e427, 0x68fc, 0x4f3d, { 0xac, 0x74, 0xca, 0x55, 0x52, 0x31, 0xcc, 0x68 } }

/* Ready-to-copy device path exposing the initrd: a MEDIA_VENDOR node with the
 * initrd GUID, followed by an END node. Install this (as EFI_DEVICE_PATH) plus
 * the LoadFile2 interface on one fresh handle. */
typedef struct {
    VENDOR_DEVICE_PATH       Vendor;
    EFI_DEVICE_PATH_PROTOCOL End;
} FOREB_INITRD_DEVICE_PATH;

/* =============================================================================
 * 6. EFI_DISK_IO_PROTOCOL  (byte-granular reads for ext2/3/4 + recovery)
 * -----------------------------------------------------------------------------
 * Optional but convenient: reads at arbitrary byte Offset without manual LBA
 * math over BlockIo. Present on the same handle as BlockIo when a DiskIo driver
 * is bound. If absent, fall back to BlockIo->ReadBlocks with sector rounding.
 * ========================================================================== */
struct _EFI_DISK_IO_PROTOCOL;
typedef struct _EFI_DISK_IO_PROTOCOL EFI_DISK_IO_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_DISK_READ)(
    IN EFI_DISK_IO_PROTOCOL *This,
    IN UINT32 MediaId,
    IN UINT64 Offset,
    IN UINTN BufferSize,
    OUT VOID *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_DISK_WRITE)(
    IN EFI_DISK_IO_PROTOCOL *This,
    IN UINT32 MediaId,
    IN UINT64 Offset,
    IN UINTN BufferSize,
    IN VOID *Buffer);

struct _EFI_DISK_IO_PROTOCOL {
    UINT64          Revision;
    EFI_DISK_READ   ReadDisk;
    EFI_DISK_WRITE  WriteDisk;
};

#define EFI_DISK_IO_PROTOCOL_REVISION  0x00010000
#define EFI_DISK_IO_PROTOCOL_GUID \
    { 0xce345171, 0xba0b, 0x11d2, { 0x8e, 0x4f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

/* =============================================================================
 * 7. EFI_CPU_ARCH_PROTOCOL  (PI spec 2.x, Vol 2 -- CPU Architectural Protocol)
 * -----------------------------------------------------------------------------
 * A DXE architectural protocol published by the firmware's CPU driver. ForeB
 * needs exactly ONE member: SetMemoryAttributes, to mark the GOP framebuffer
 * range EFI_MEMORY_WC (write-combining) in ui_init() so the per-row blits in
 * ui_present() burst to VRAM instead of trickling as uncached word stores. This
 * is the portable, in-spec route to WC while BootServices is still up -- no raw
 * MTRR/PAT MSR pokes, and it works the same on x86_64 and AArch64.
 *
 * Locate it by GUID with BootServices->LocateProtocol; it is NOT guaranteed to
 * exist on every firmware, so callers MUST treat absence (or a non-EFI_SUCCESS
 * SetMemoryAttributes) as non-fatal and simply fall back to the untuned path.
 *
 * The interrupt/timer members below are declared only to keep SetMemoryAttributes
 * at its correct firmware-defined offset; ForeB never calls them. Their handler
 * signature references the arch-specific EFI_SYSTEM_CONTEXT union, which we do
 * NOT reproduce here -- the slots are typed opaquely (all pointer-sized) so the
 * struct layout/ABI is exact without pulling that dependency in.
 * ========================================================================== */
struct _EFI_CPU_ARCH_PROTOCOL;
typedef struct _EFI_CPU_ARCH_PROTOCOL EFI_CPU_ARCH_PROTOCOL;

typedef enum {
    EfiCpuFlushTypeWriteBackInvalidate,
    EfiCpuFlushTypeWriteBack,
    EfiCpuFlushTypeInvalidate,
    EfiCpuMaxFlushType
} EFI_CPU_FLUSH_TYPE;

typedef enum {
    EfiCpuInit,
    EfiCpuMaxInitType
} EFI_CPU_INIT_TYPE;

typedef EFI_STATUS (EFIAPI *EFI_CPU_FLUSH_DATA_CACHE)(
    IN EFI_CPU_ARCH_PROTOCOL *This,
    IN EFI_PHYSICAL_ADDRESS   Start,
    IN UINT64                 Length,
    IN EFI_CPU_FLUSH_TYPE     FlushType);

typedef EFI_STATUS (EFIAPI *EFI_CPU_ENABLE_INTERRUPT)(
    IN EFI_CPU_ARCH_PROTOCOL *This);

typedef EFI_STATUS (EFIAPI *EFI_CPU_DISABLE_INTERRUPT)(
    IN EFI_CPU_ARCH_PROTOCOL *This);

typedef EFI_STATUS (EFIAPI *EFI_CPU_GET_INTERRUPT_STATE)(
    IN  EFI_CPU_ARCH_PROTOCOL *This,
    OUT BOOLEAN               *State);

typedef EFI_STATUS (EFIAPI *EFI_CPU_INIT)(
    IN EFI_CPU_ARCH_PROTOCOL *This,
    IN EFI_CPU_INIT_TYPE      InitType);

/* Real prototype uses (EFI_EXCEPTION_TYPE, EFI_CPU_INTERRUPT_HANDLER); the
 * latter references the arch-specific EFI_SYSTEM_CONTEXT union we intentionally
 * omit. Typed opaquely -- ForeB never registers a handler. */
typedef EFI_STATUS (EFIAPI *EFI_CPU_REGISTER_INTERRUPT_HANDLER)(
    IN EFI_CPU_ARCH_PROTOCOL *This,
    IN INTN                   InterruptType,
    IN VOID                  *InterruptHandler);

typedef EFI_STATUS (EFIAPI *EFI_CPU_GET_TIMER_VALUE)(
    IN  EFI_CPU_ARCH_PROTOCOL *This,
    IN  UINT32                 TimerIndex,
    OUT UINT64                *TimerValue,
    OUT UINT64                *TimerPeriod OPTIONAL);

/* The one member ForeB uses. Attributes is a mask of EFI_MEMORY_* (efi.h):
 * pass EFI_MEMORY_WC for the framebuffer range. */
typedef EFI_STATUS (EFIAPI *EFI_CPU_SET_MEMORY_ATTRIBUTES)(
    IN EFI_CPU_ARCH_PROTOCOL *This,
    IN EFI_PHYSICAL_ADDRESS   BaseAddress,
    IN UINT64                 Length,
    IN UINT64                 Attributes);

struct _EFI_CPU_ARCH_PROTOCOL {
    EFI_CPU_FLUSH_DATA_CACHE            FlushDataCache;
    EFI_CPU_ENABLE_INTERRUPT           EnableInterrupt;
    EFI_CPU_DISABLE_INTERRUPT          DisableInterrupt;
    EFI_CPU_GET_INTERRUPT_STATE        GetInterruptState;
    EFI_CPU_INIT                       Init;
    EFI_CPU_REGISTER_INTERRUPT_HANDLER RegisterInterruptHandler;
    EFI_CPU_GET_TIMER_VALUE            GetTimerValue;
    EFI_CPU_SET_MEMORY_ATTRIBUTES      SetMemoryAttributes;
    UINT32                             NumberOfTimers;
    UINT32                             DmaBufferAlignment;
};

#define EFI_CPU_ARCH_PROTOCOL_GUID \
    { 0x26baccb1, 0x6f42, 0x11d4, { 0x9a, 0x38, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

/* =============================================================================
 * 8. Optional named GUID globals (mirrors efi.h's FOREB_EFI_DEFINE_GUIDS).
 * Define FOREB_EFI_EXT_DEFINE_GUIDS in exactly ONE translation unit.
 * ========================================================================== */
#ifdef FOREB_EFI_EXT_DEFINE_GUIDS
EFI_GUID gEfiSimplePointerProtocolGuid   = EFI_SIMPLE_POINTER_PROTOCOL_GUID;
EFI_GUID gEfiAbsolutePointerProtocolGuid = EFI_ABSOLUTE_POINTER_PROTOCOL_GUID;
EFI_GUID gEfiDevicePathProtocolGuid      = EFI_DEVICE_PATH_PROTOCOL_GUID;
EFI_GUID gEfiLoadFile2ProtocolGuid       = EFI_LOAD_FILE2_PROTOCOL_GUID;
EFI_GUID gLinuxEfiInitrdMediaGuid        = LINUX_EFI_INITRD_MEDIA_GUID;
EFI_GUID gEfiDiskIoProtocolGuid          = EFI_DISK_IO_PROTOCOL_GUID;
EFI_GUID gEfiCpuArchProtocolGuid         = EFI_CPU_ARCH_PROTOCOL_GUID;
#endif

#endif /* FOREB_EFI_EXT_H */
