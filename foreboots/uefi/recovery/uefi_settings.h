#ifndef FOREB_UEFI_SETTINGS_H
#define FOREB_UEFI_SETTINGS_H
#include "../efi.h"

// Initialize the UEFI settings module with the system table
void uefi_settings_init(EFI_SYSTEM_TABLE *st);

// Open the UEFI settings window (wm_open with draw/event callbacks)
void uefi_settings_open(void);

#endif
