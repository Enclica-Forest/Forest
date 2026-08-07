/*
 * feature_stubs.c - Stub implementations for disabled features
 *
 * When ENABLE_NETWORKING, ENABLE_AUDIO, or ENABLE_USB are not defined,
 * these stubs satisfy linker references from kernel.c, syscall.c, etc.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef uint32_t uint32;
typedef int32_t  int32;
typedef uint8_t  uint8;
typedef uint16_t uint16;

/* ---- Networking stubs ---- */
#ifndef ENABLE_NETWORKING

#include "include/net.h"

bool net_init(void) { return false; }
bool net_is_fd(uint32 fd) { (void)fd; return false; }
int32 net_close(uint32 fd) { (void)fd; return -1; }
void net_close_all_for_task(uint32 pid) { (void)pid; }
int32 net_socket_create(uint32 domain, uint32 type, uint32 protocol) {
    (void)domain; (void)type; (void)protocol; return -1;
}
int32 net_bind(uint32 fd, uint16 port) { (void)fd; (void)port; return -1; }
int32 net_send_datagram(uint32 fd, const uint8* buffer, uint32 length,
                        uint32 dest_addr, uint16 dest_port) {
    (void)fd; (void)buffer; (void)length; (void)dest_addr; (void)dest_port; return -1;
}
int32 net_recv_datagram(uint32 fd, uint8* buffer, uint32 length,
                        uint32* src_addr, uint16* src_port) {
    (void)fd; (void)buffer; (void)length; (void)src_addr; (void)src_port; return -1;
}
uint32 net_snapshot(net_socket_info_t* out, uint32 max_entries) {
    (void)out; (void)max_entries; return 0;
}

#endif /* !ENABLE_NETWORKING */

/* ---- Audio stubs ---- */
#ifndef ENABLE_AUDIO

#include "include/sound.h"
#include "include/sound_pcspeaker.h"

bool sound_system_init(void) { return false; }
void sound_shutdown(void) {}
const SoundDriver* sound_active_driver(void) { return NULL; }
bool sound_play_wav(const char* path) { (void)path; return false; }
void sound_beep(uint32 frequency_hz, uint32 duration_ms) {
    (void)frequency_hz; (void)duration_ms;
}
void sound_set_volume(uint8 volume) { (void)volume; }

/* PC speaker fallback stubs (TTY bell resolves to a no-op when audio is
 * disabled at build time). */
void snd_pcspeaker_tone(uint32 freq, uint32 ms) { (void)freq; (void)ms; }
void snd_pcspeaker_beep(void) {}
bool snd_pcspeaker_available(void) { return false; }

#endif /* !ENABLE_AUDIO */

/* ---- USB stubs ---- */
#ifndef ENABLE_USB

bool usb_init(void) { return false; }
void usb_poll(void) {}

#endif /* !ENABLE_USB */
