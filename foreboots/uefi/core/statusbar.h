#ifndef FOREB_STATUSBAR_H
#define FOREB_STATUSBAR_H
#include "../efi.h"

void statusbar_init(EFI_BOOT_SERVICES *bs, EFI_SIMPLE_TEXT_INPUT_PROTOCOL *conin);
void statusbar_draw(void);
int  statusbar_height(void);
void statusbar_set_mouse(int present);

#endif
