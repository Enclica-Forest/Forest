/* =============================================================================
 * ForeB - Forest Bootloader
 * uefi/audio.c - PC speaker (8254 PIT channel 2) beeper + UI event tones.
 * =============================================================================
 * The PC speaker is the one sound source present before the OS on essentially
 * every x86 machine. PIT channel 2 generates a square wave whose frequency is
 * 1193182 / divisor; port 0x61 bits 0-1 gate that wave into the speaker.
 *
 * "Volume" cannot be set on a bare beeper (the wave is fixed amplitude), so we
 * approximate it by PWM-gating the whole tone on/off at a supra-audio rate with
 * a duty cycle == volume% - a lower duty is perceived as quieter. Sample (WAV)
 * playback is likewise approximated by mapping the file's loudness envelope onto
 * a short beep sequence; a real HD-Audio backend would replace audio_play_wav()
 * without changing its callers. See audio.h.
 * ========================================================================== */
#include "audio.h"

#define PIT_HZ        1193182u
#define PIT_CH2_DATA  0x42
#define PIT_CMD       0x43
#define SPK_PORT      0x61
#define SPK_GATE_BITS 0x03      /* bit0 = timer2 gate, bit1 = speaker data on  */

static EFI_BOOT_SERVICES *a_bs = 0;

static struct audio_cfg a_cfg;   /* active config (see audio_default below)     */

/* Timer event that gates a UI blip *off* asynchronously: audio_event() opens
 * the speaker and arms this one-shot; the firmware signals it after the tone's
 * duration and its notify (tone_off_notify) drops the gate. This keeps nav /
 * select / back tones off the event loop so the cursor + input never stall
 * during a beep (the old path Stall()ed for the whole tone). Created in
 * audio_init(); 0 (or non-x86) transparently falls back to no async tone.  */
static EFI_EVENT a_tone_ev = 0;

/* ---- port I/O (x86 only) ------------------------------------------------- */
#if defined(__x86_64__) || defined(__i386__)
static inline void au_outb(UINT16 p, UINT8 v)
{ __asm__ __volatile__("outb %0,%1" : : "a"(v), "Nd"(p)); }
static inline UINT8 au_inb(UINT16 p)
{ UINT8 v; __asm__ __volatile__("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
#define AUDIO_X86 1
#else
static inline void au_outb(UINT16 p, UINT8 v) { (void)p; (void)v; }
static inline UINT8 au_inb(UINT16 p) { (void)p; return 0; }
#define AUDIO_X86 0
#endif

static void au_stall(UINT32 us) { if (a_bs) a_bs->Stall((UINTN)us); }

/* Program PIT channel 2's divisor for freq_hz (does NOT touch the speaker
 * gate). Split out of spk_on() so a PWM loop that gates the same frequency
 * on/off repeatedly can program the divisor once and just toggle the gate. */
static void spk_set_freq(UINT32 freq_hz)
{
#if AUDIO_X86
    if (freq_hz < 20) freq_hz = 20;
    UINT32 div = PIT_HZ / freq_hz;
    if (div < 1) div = 1;
    if (div > 0xFFFF) div = 0xFFFF;
    au_outb(PIT_CMD, 0xB6);                       /* ch2, lo/hi, mode3, binary */
    au_outb(PIT_CH2_DATA, (UINT8)(div & 0xFF));
    au_outb(PIT_CH2_DATA, (UINT8)((div >> 8) & 0xFF));
#else
    (void)freq_hz;
#endif
}

/* Open the speaker gate (PIT divisor must already be programmed). */
static void spk_gate_on(void)
{
#if AUDIO_X86
    UINT8 s = au_inb(SPK_PORT);
    if ((s & SPK_GATE_BITS) != SPK_GATE_BITS)
        au_outb(SPK_PORT, (UINT8)(s | SPK_GATE_BITS));
#endif
}

static void spk_off(void)
{
#if AUDIO_X86
    UINT8 s = au_inb(SPK_PORT);
    au_outb(SPK_PORT, (UINT8)(s & ~SPK_GATE_BITS));
#endif
}

/* Program the divisor and open the gate (used for full-volume/steady tones). */
static void spk_on(UINT32 freq_hz)
{
    spk_set_freq(freq_hz);
    spk_gate_on();
}

/* One-shot timer notify: the tone's duration elapsed, so close the gate. Runs
 * at TPL_NOTIFY out of the firmware timer tick, so it does only port I/O. */
static void EFIAPI tone_off_notify(EFI_EVENT ev, VOID *ctx)
{
    (void)ev; (void)ctx;
    spk_off();
}

/* Disarm a pending async tone (if any). Called before a blocking beep so a
 * late timer signal can't cut a blocking tone short. */
static void tone_cancel(void)
{
    if (a_bs && a_tone_ev) a_bs->SetTimer(a_tone_ev, TimerCancel, 0);
}

/* Non-blocking UI blip: open the speaker at fixed amplitude and arm the
 * off-timer for `ms`, then return immediately. No PWM (volume shaping) here -
 * the whole point is to never Stall the event loop; blocking/volume-shaped
 * tones stay on the beep_vol() path used by audio_beep()/WAV playback. */
static void beep_async(UINT32 freq_hz, UINT32 ms)
{
    if (!AUDIO_X86 || !a_bs || !a_tone_ev || freq_hz == 0 || ms == 0) return;
    spk_on(freq_hz);
    /* TriggerTime is in 100 ns units: ms milliseconds == ms * 10000. */
    a_bs->SetTimer(a_tone_ev, TimerRelative, (UINT64)ms * 10000u);
}

void audio_silence(void) { tone_cancel(); spk_off(); }

/* Emit freq for `ms`, using volume as a PWM gate duty so lower volume is
 * quieter. A ~1 kHz gating chop keeps the clicks above/near the audible edge
 * while still lowering perceived loudness. */
static void beep_vol(UINT32 freq_hz, UINT32 ms, int vol)
{
    if (!AUDIO_X86 || !a_bs || freq_hz == 0 || ms == 0) return;
    tone_cancel();   /* a stray async-off must not chop this blocking tone */
    if (vol <= 0)   { spk_off(); au_stall(ms * 1000u); return; }
    if (vol >= 100) { spk_on(freq_hz); au_stall(ms * 1000u); spk_off(); return; }

    /* PWM gate: 1 ms slices, `vol`% of each slice on. freq_hz is constant for
     * the whole tone, so program the PIT divisor once and just toggle the
     * speaker gate (port 0x61) each slice instead of reprogramming channel 2
     * every millisecond. */
    spk_set_freq(freq_hz);
    UINT32 on = (1000u * (UINT32)vol) / 100u;       /* microseconds on */
    for (UINT32 t = 0; t < ms; t++) {
        spk_gate_on();
        au_stall(on);
        spk_off();
        au_stall(1000u - on);
    }
}

void audio_beep(UINT32 freq_hz, UINT32 ms)
{
    beep_vol(freq_hz, ms, a_cfg.enabled ? a_cfg.volume : 0);
}

/* ---- defaults + config --------------------------------------------------- */
static void audio_default(struct audio_cfg *c)
{
    c->enabled = 0;                 /* silent unless forebo.cfg opts in        */
    c->volume  = 80;
    /* Pleasant short UI blips (freq Hz, ms). */
    c->tone[AUDIO_EV_NAV]    = (struct audio_tone){ 880, 18 };
    c->tone[AUDIO_EV_SELECT] = (struct audio_tone){ 1320, 40 };
    c->tone[AUDIO_EV_OPEN]   = (struct audio_tone){ 660, 30 };
    c->tone[AUDIO_EV_ERROR]  = (struct audio_tone){ 220, 90 };
    c->tone[AUDIO_EV_BACK]   = (struct audio_tone){ 494, 22 };
}

void audio_init(EFI_BOOT_SERVICES *bs)
{
    a_bs = bs;
    audio_default(&a_cfg);
    /* One-shot timer that gates UI blips off without blocking the loop. If the
     * firmware refuses (or non-x86), a_tone_ev stays 0 and beep_async() no-ops,
     * so tones simply don't sound - never a hang. */
    a_tone_ev = 0;
    if (bs)
        bs->CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_NOTIFY,
                        tone_off_notify, 0, &a_tone_ev);
    spk_off();
}

void audio_configure(const struct audio_cfg *cfg)
{
    if (!cfg) return;
    a_cfg.enabled = cfg->enabled;
    if (cfg->volume >= 0 && cfg->volume <= 100) a_cfg.volume = cfg->volume;
    for (int i = 0; i < AUDIO_EV__COUNT; i++) {
        /* freq 0 keeps the default; a real 0 means "silent event" -> use 1 to
         * express that in config, which rounds to inaudible. */
        if (cfg->tone[i].freq) a_cfg.tone[i].freq = cfg->tone[i].freq;
        if (cfg->tone[i].ms)   a_cfg.tone[i].ms   = cfg->tone[i].ms;
    }
}

int audio_enabled(void) { return (AUDIO_X86 && a_bs && a_cfg.enabled) ? 1 : 0; }

void audio_event(int ev)
{
    if (!audio_enabled() || ev < 0 || ev >= AUDIO_EV__COUNT) return;
    struct audio_tone t = a_cfg.tone[ev];
    /* UI blips take the non-blocking timer path (fixed amplitude) so nav /
     * select / open / back never freeze the cursor or input. volume<=0 is
     * treated as muted; the blocking beep_vol() PWM path is reserved for
     * audio_beep()/WAV callers that explicitly want a shaped, blocking tone. */
    if (t.freq && a_cfg.volume > 0) beep_async(t.freq, t.ms ? t.ms : 20);
}

/* ---- WAV envelope approximation ----------------------------------------- *
 * There is no portable pre-OS PCM path, so we read the WAV header, then map the
 * sample stream's coarse amplitude envelope onto a short beeper sequence: the
 * file is split into a fixed number of windows, each window's peak amplitude
 * sets that blip's duty (loudness) at a pitch derived from the window index,
 * producing a recognizable chirp of the sample's rhythm. Bounded reads only. */

/* Minimal little-endian readers. */
static UINT32 rd_le32(const UINT8 *p) { return p[0] | (p[1]<<8) | (p[2]<<16) | ((UINT32)p[3]<<24); }
static UINT16 rd_le16(const UINT8 *p) { return (UINT16)(p[0] | (p[1]<<8)); }

#define WAV_WINDOWS  24     /* cap total blips so long files stay short        */
#define WAV_BLIP_MS  22     /* per-window blip duration                        */

/* Incremental WAV "playback" state. The envelope->beeper chirp used to run to
 * completion inside audio_play_wav() (Stall-blocking the whole file); now that
 * function just opens + validates the file and seeds this state, and the UI
 * loop drives it via audio_poll(), which emits at most one blip per tick and
 * returns - so a long sample never freezes the cursor/input. buf/pos/got hold
 * the current read block across ticks so no samples are dropped at a boundary. */
static struct {
    EFI_FILE_PROTOCOL *f;              /* open handle, 0 => idle               */
    int    active;
    UINT16 bits;                       /* 8 or 16 (sample stride)              */
    UINT32 samples_per_win;            /* ~1/6 s of samples                    */
    UINT32 seen;                       /* samples accumulated in current window*/
    int    win, peak;                  /* window index + running peak amplitude*/
    UINT8  buf[1024];                  /* current file read block              */
    UINTN  got, pos;                   /* valid bytes + consume cursor in buf  */
} a_wav;

/* Stop playback: silence, close the handle, mark idle. Safe to call twice. */
static void wav_stop(void)
{
    if (a_wav.f) { spk_off(); a_wav.f->Close(a_wav.f); }
    a_wav.f = 0;
    a_wav.active = 0;
}

int audio_play_wav(void *root_v, const CHAR16 *path)
{
    if (!AUDIO_X86 || !a_bs || !audio_enabled() || !root_v || !path) return 0;

    /* Only one WAV plays at a time; a new request supersedes any in progress. */
    if (a_wav.active) wav_stop();

    EFI_FILE_PROTOCOL *root = (EFI_FILE_PROTOCOL *)root_v;
    EFI_FILE_PROTOCOL *f = 0;
    if (EFI_ERROR(root->Open(root, &f, (CHAR16 *)path, 0x1 /*READ*/, 0)) || !f)
        return 0;

    /* Read the 44-byte canonical header (fmt + data chunk start). */
    UINT8 hdr[64];
    UINTN n = sizeof(hdr);
    if (EFI_ERROR(f->Read(f, &n, hdr)) || n < 44) { f->Close(f); return 0; }
    if (hdr[0]!='R'||hdr[1]!='I'||hdr[2]!='F'||hdr[3]!='F'||
        hdr[8]!='W'||hdr[9]!='A'||hdr[10]!='V'||hdr[11]!='E') { f->Close(f); return 0; }

    UINT16 fmt   = rd_le16(hdr + 20);   /* 1 = PCM                             */
    UINT16 chans = rd_le16(hdr + 22);
    UINT32 rate  = rd_le32(hdr + 24);
    UINT16 bits  = rd_le16(hdr + 34);
    if (fmt != 1 || (bits != 8 && bits != 16) || chans < 1 || rate < 4000) {
        f->Close(f); return 0;
    }

    /* Seed incremental state; audio_poll() emits the chirp one blip per tick. */
    UINT32 spw = (rate * chans) / 6;    /* ~1/6 s of samples per window        */
    if (spw < 256) spw = 256;
    a_wav.f = f;
    a_wav.bits = bits;
    a_wav.samples_per_win = spw;
    a_wav.seen = 0;
    a_wav.win = 0;
    a_wav.peak = 0;
    a_wav.got = 0;
    a_wav.pos = 0;
    a_wav.active = 1;
    return 1;                           /* playback started (parsed OK)        */
}

/* Advance WAV playback by at most one window/blip. Call once per UI-loop tick.
 * No-op when nothing is playing, so it is cheap on the common (silent) frame. */
void audio_poll(void)
{
    if (!a_wav.active || !a_bs) return;

    /* Refill the block buffer when drained; a short/zero read means EOF. */
    if (a_wav.pos >= a_wav.got) {
        UINTN got = sizeof(a_wav.buf);
        if (EFI_ERROR(a_wav.f->Read(a_wav.f, &got, a_wav.buf)) || got == 0) {
            /* Flush a final partial-window blip, then stop. */
            if (a_wav.peak && a_wav.win < WAV_WINDOWS)
                beep_vol((UINT32)(440 + a_wav.win * 60), WAV_BLIP_MS,
                         (a_wav.peak * a_cfg.volume) / 128);
            wav_stop();
            return;
        }
        a_wav.got = got;
        a_wav.pos = 0;
    }

    /* Consume samples until one window completes (emit exactly one blip and
     * return) or the buffer drains (return; next tick refills). */
    UINTN step = (a_wav.bits == 16) ? 2u : 1u;
    while (a_wav.pos + step <= a_wav.got) {
        int amp;
        if (a_wav.bits == 16) {
            INT16 s = (INT16)rd_le16(a_wav.buf + a_wav.pos);
            amp = s < 0 ? -s : s; amp >>= 8;
        } else {
            amp = (int)a_wav.buf[a_wav.pos] - 128; if (amp < 0) amp = -amp;
        }
        a_wav.pos += step;
        if (amp > a_wav.peak) a_wav.peak = amp;
        if (++a_wav.seen >= a_wav.samples_per_win) {
            int duty = (a_wav.peak * a_cfg.volume) / 128;   /* 0..~volume      */
            if (duty > a_cfg.volume) duty = a_cfg.volume;
            int freq = 440 + a_wav.win * 60;                 /* rising chirp    */
            beep_vol((UINT32)freq, WAV_BLIP_MS, duty);
            a_wav.seen = 0; a_wav.peak = 0;
            if (++a_wav.win >= WAV_WINDOWS) wav_stop();
            return;                      /* exactly one blip per tick           */
        }
    }
    /* Buffer drained mid-window: fall through, refill on the next tick. */
}
