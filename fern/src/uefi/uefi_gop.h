#ifndef UEFI_GOP_H
#define UEFI_GOP_H

#include "uefi_boot_services.h"

/* Graphics Output Protocol GUID */
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x72, 0xde, 0x31, 0xc6, 0xc2, 0xb0}}

/* Pixel Formats */
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor = 0,
    PixelBlueGreenRedReserved8BitPerColor = 1,
    PixelBitMask = 2,
    PixelBltOnly = 3,
    PixelFormatMax = 4
} EFI_GRAPHICS_PIXEL_FORMAT;

/* Pixel Bitmask */
typedef struct {
    uint32_t RedMask;
    uint32_t GreenMask;
    uint32_t BlueMask;
    uint32_t ReservedMask;
} EFI_PIXEL_BITMASK;

/* Graphics Output Mode Information */
typedef struct {
    uint32_t Version;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    EFI_PIXEL_BITMASK PixelInformation;
    uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

/* Graphics Output Protocol Mode */
typedef struct {
    uint32_t MaxMode;
    uint32_t Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* Info;
    uint64_t SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    uint64_t FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

/* Graphics Output Protocol Blt Pixel */
typedef struct {
    uint8_t Blue;
    uint8_t Green;
    uint8_t Red;
    uint8_t Reserved;
} EFI_GRAPHICS_OUTPUT_BLT_PIXEL;

/* Blt Operations */
typedef enum {
    EfiBltVideoFill,
    EfiBltVideoToBltBuffer,
    EfiBltBufferToVideo,
    EfiBltVideoToVideo,
    EfiBltOperationMax
} EFI_GRAPHICS_OUTPUT_BLT_OPERATION;

/* Graphics Output Protocol */
typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE)(
    EFI_GRAPHICS_OUTPUT_PROTOCOL* This,
    uint32_t ModeNumber,
    uint64_t* SizeOfInfo,
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION** Info
);

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE)(
    EFI_GRAPHICS_OUTPUT_PROTOCOL* This,
    uint32_t ModeNumber
);

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT)(
    EFI_GRAPHICS_OUTPUT_PROTOCOL* This,
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL* BltBuffer,
    EFI_GRAPHICS_OUTPUT_BLT_OPERATION BltOperation,
    uint64_t SourceX,
    uint64_t SourceY,
    uint64_t DestinationX,
    uint64_t DestinationY,
    uint64_t Width,
    uint64_t Height,
    uint64_t Delta
);

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE QueryMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE SetMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE* Mode;
};

/* Kernel-friendly framebuffer info structure */
typedef struct {
    void* BaseAddress;
    uint64_t BufferSize;
    uint32_t Width;
    uint32_t Height;
    uint32_t PixelsPerScanLine;
    uint32_t BitsPerPixel;
    uint32_t PixelFormat;
    uint32_t RedMask;
    uint32_t GreenMask;
    uint32_t BlueMask;
    uint32_t ReservedMask;
} uefi_gop_info_t;

#endif /* UEFI_GOP_H */
