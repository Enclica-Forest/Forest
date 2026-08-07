# Audio Subsystem

Forest OS ships with a fully featured audio subsystem that supports everything from the humble PC speaker all the way up to modern HD Audio codecs. Whether you're booting on bare metal with a Sound Blaster 16 tucked in a ISA slot, running QEMU with an AC97 codec, or just listening to the TTY bell on a sound-card-less machine, the kernel has you covered.

---

## Architecture at a Glance

The audio stack is built in layers:

```
 Userspace apps
       |
 /dev/snd  (PCM character device, major 116)
       |
 Software Mixer  (sound_mixer.c / sound_pcm_device.c)
       |
 Sound Driver Interface  (SoundDriver vtable)
       |
 +------+------+------+------+------+------+------+------+
 | PC   | SB16 | SBPro| AC97 | HDA  | Ensoniq| OPL3 | USB  |
 | Speaker      |      |      |      | AudioPCI      |      |
 +------+------+------+------+------+------+------+------+
```

At the top, userspace talks to `/dev/snd` via standard `open()`/`write()`/`ioctl()` calls. The PCM character device feeds data into a kernel software mixer, which mixes multiple streams, resamples as needed, and hands the result to whichever hardware driver was detected at boot. The whole thing is gated by the `ENABLE_AUDIO` build flag -- when set to `no`, all sound sources are excluded from the build and the kernel links cleanly without any audio code.

Every hardware driver implements the same `SoundDriver` vtable (`sound.h:77-92`), which provides a uniform interface:

```c
typedef struct SoundDriver {
    const char* name;
    SoundDeviceType type;
    bool (*detect)(struct SoundDriver* driver);
    bool (*init)(struct SoundDriver* driver);
    bool (*play_pcm)(struct SoundDriver* driver, const uint8* data,
                     uint32 length, const SoundFormat* format);
    bool (*get_capabilities)(struct SoundDriver* driver, DeviceCapabilities* caps);
    void (*set_volume)(struct SoundDriver* driver, uint8 volume);
    void (*beep)(struct SoundDriver* driver, uint32 frequency_hz, uint32 duration_ms);
    void (*shutdown)(struct SoundDriver* driver);
    void* state;
    uint8 volume;
} SoundDriver;
```

This vtable approach means the mixer doesn't need to know which specific card is plugged in. It just calls `play_pcm()` and lets the driver handle the hardware-specific details. Nice and clean.

### Driver Probing Order

When `sound_system_init()` is called, the kernel tries drivers in a specific order (`sound.c:720-826`):

1. **Architecture-specific driver** -- On non-x86 platforms (ARM32, AArch64, RISC-V), this is the primary path. On x86 it returns NULL and falls through.
2. **HDA** (Intel HD Audio) -- Tried first on x86 since it's the most common modern standard.
3. **AC97** -- The classic Intel ICH codec family.
4. **Ensoniq AudioPCI** -- The ES1371, a beloved retro card.
5. **Sound Blaster 16** -- The legendary Creative card.
6. **Sound Blaster Pro** -- Its older sibling.
7. **OPL3** -- Yamaha FM synthesis chip.
8. **USB Audio** -- For USB sound devices.
9. **PC Speaker** -- The universal fallback. Always detects successfully.
10. **Universal Sound Driver** -- Last resort; falls back to PC speaker.

The first driver that detects and initializes successfully becomes the active driver. If nothing is found, the system still works -- beeps just go through the PC speaker, and `/dev/snd` returns `-ENOSYS` for audio operations.

---

## PC Speaker Support

**File:** `src/sound_pc_speaker.c` (216 lines)

Every PC has one, and Forest OS knows how to use it. The PC speaker driver talks to the PIT (Programmable Interval Timer) channel 2 and the speaker gate on port `0x61` to produce tones.

### How It Works

The driver programs PIT channel 2 in mode 3 (square wave generator) and toggles the speaker gate to produce sound at a requested frequency. The `pcspk_tone()` function calculates the PIT divisor as `1193180 / frequency_hz` and loads it into the timer. Toggling bits 0-1 of port `0x61` turns the speaker on and off.

### PCM Playback

Yes, the PC speaker can actually play PCM audio! The driver implements a PWM (Pulse Width Modulation) mode (`pcspk_pwm_output()`) where it rapidly toggles the speaker on and off at a fixed rate to approximate audio samples. It's not going to win any audio quality awards -- it's mono, 8-bit, and limited to about 22050 Hz sample rate -- but it's a clever trick that lets you hear WAV files on a machine with literally nothing but the motherboard speaker.

### The Bell

The `snd_pcspeaker_beep()` convenience function plays the classic 800 Hz, 120 ms terminal bell tone. This is also used as a fallback when no real sound card is available, so the TTY can still ring even on a headless server.

---

## Sound Blaster 16 / Pro Drivers

**Files:** `src/sound_sb16.c` (779 lines), `src/sound_sbpro.c` (156 lines)

The Sound Blaster 16 is one of the most iconic PC audio cards, and Forest OS has a proper driver for it. The SB16 supports both 8-bit and 16-bit DMA playback, with stereo support in 16-bit mode.

### DSP Reset and Detection

The driver starts by resetting the DSP (Digital Signal Processor) via the standard reset sequence: write `1` to the reset port (`0x226`), wait, write `0`, then poll the read status port (`0x22E`) until you get back `0xAA`. It then reads the DSP version with command `0xE1` and verifies it's version 4.x or newer -- that's what distinguishes a real SB16 from an older Sound Blaster.

### DMA Programming

The SB16 uses ISA DMA, which requires some care:

- **8-bit DMA** uses channels 0-3 with 8-bit address registers. The driver programs the mask, mode, base address, count, and page registers via the standard DMA controller ports.
- **16-bit DMA** uses channels 5-7 with word-aligned addresses. The physical address is right-shifted by 1 when programming the address registers, and counts are in words, not bytes.

The driver allocates a 64KB bounce buffer from low physical memory using `bitmap_pmm_alloc_contiguous_pages()` with 64KB alignment. This is required because ISA DMA can only access the first 16MB of physical memory. The buffer is mapped into kernel virtual space with `mm_map_physical_page()`.

### Interrupt Handling

The driver registers an IRQ handler (default IRQ 5) that fires when DMA completes. It checks the SB16's mixer register `0x82` for interrupt status bits -- bit 0 for 8-bit DMA, bit 1 for 16-bit DMA. In single-shot mode, the interrupt marks playback as complete. In auto-init mode (streaming), playback continues through the circular buffer.

### Sample Rate and Format

- **8-bit mode:** Uses the time constant register (command `0x40`) with the formula `256 - (1000000 / sample_rate)`. Limited to mono, ~22050 Hz.
- **16-bit mode:** Uses the direct sample rate register (command `0x41`) for rates from 4000 to 44100 Hz. Supports stereo with signed 16-bit samples.

### Beep

The SB16 driver can generate beeps by synthesizing a square wave in the DMA buffer and playing it back through the DSP. It programs a sample rate at twice the target frequency, fills the buffer with alternating high/low samples, and uses 16-bit DMA channel 5.

### Sound Blaster Pro

The SBPro driver (`sound_sbpro.c`) is a simpler version. It probes common port addresses (`0x220`, `0x240`, `0x260`, `0x280`) by resetting the DSP and checking for the `0xAA` response. PCM playback is stubbed out (TODO), but the infrastructure is there. Beeps fall back to the PC speaker.

---

## AC97 Codec Support

**Files:** `src/sound_ac97.c` (832 lines), `src/sound_ac97_driver.c` (272 lines)

AC97 (Audio Codec '97) is the standard that Intel pushed in the late 1990s, and it's the codec you'll find in most older PCs and QEMU by default. Forest OS has two AC97 implementations -- a full-featured one and a simpler one. The full driver in `sound_ac97.c` is the one that gets used.

### PCI Detection

The driver scans for PCI devices by class code (`0x04/0x01` for multimedia audio) or by known vendor/device IDs. It knows about Intel ICH through ICH6 (`0x2415`, `0x2425`, `0x24D5`, `0x24DD`, `0x266E`) and the Ensoniq ES1371 (`0x1274:0x1371`). BAR0 gives the NAM (Native Audio Mixer) base, and BAR1 gives the NABM (Native Audio Bus Mastering) base.

### Codec Initialization

The init sequence is carefully ordered:

1. Enable PCI bus mastering and I/O space decode.
2. Power up all codec components via the power management register (`0x26`).
3. Reset the NAM and perform a cold reset on the NABM global control register.
4. Verify the codec is alive by reading the vendor ID registers (`0x7C`, `0x7E`).
5. Detect extended capabilities -- importantly, Variable Rate Audio (VRA) support, which allows sample rates other than 48kHz.

### DMA and Buffer Descriptors

AC97 uses a Buffer Descriptor List (BDL) -- an array of entries in physical memory, each pointing to a DMA buffer segment. The driver allocates:

- A DMA buffer (8KB total, split into 2 half-buffers for double-buffering)
- A BDL page (16 bytes per entry, 2 entries)

Each BDL entry contains a physical address, length, and flags. The IOC (Interrupt on Completion) flag on the last entry triggers an IRQ when that segment finishes playing.

### Sample Rate Conversion

AC97 natively plays at 48kHz. If the source audio is at a different rate and VRA isn't available, the driver performs resampling. It uses linear interpolation with 16.16 fixed-point math to convert between rates, handling both mono and stereo S16 PCM. When VRA is available, the driver programs the front DAC rate register directly and the codec handles the conversion in hardware.

### Volume Control

The driver manages three volume registers:
- **Master volume** (`0x02`) -- controls overall output level
- **Headphone volume** (`0x04`) -- for the headphone jack
- **PCM volume** (`0x18`) -- controls the PCM DAC level

Each register uses 5-bit attenuation values for left and right channels, with a mute bit. The driver converts Forest OS's 0-255 volume range to AC97's 0-31 attenuation range.

---

## HDA (High Definition Audio) Support

**Files:** `src/sound_hda.c` (1468 lines), `src/sound_hda_driver.c` (227 lines)

HDA is the modern successor to AC97, and it's what you'll find in any PC made in the last 15+ years. It's also the most complex driver in the audio subsystem -- HDA is a much richer specification than AC97.

### MMIO and Controller Setup

Unlike AC97's I/O port-based register access, HDA uses memory-mapped I/O (MMIO). The driver maps BAR0 into kernel virtual space and accesses registers through pointer dereferencing. The first thing it does is reset the controller by toggling the `CRST` bit in the Global Control register (`0x08`), then waits for the controller to acknowledge.

### CORB/RIRB Communication

HDA communicates with codecs through two DMA rings:

- **CORB** (Command Output Ring Buffer) -- The kernel writes verb commands here, and the controller sends them to the appropriate codec.
- **RIRB** (Response Input Ring Buffer) -- Codecs write responses here, and the kernel reads them.

Each ring is a page of physically contiguous memory (256 entries). The driver sets up DMA for both, enables interrupts on the RIRB, and uses a write pointer / read pointer protocol to send verbs and receive responses.

### Codec Discovery and Widget Parsing

After setting up CORB/RIRB, the driver probes all 4 possible codec addresses by checking the `STATESTS` register or sending GET_PARAMETER verbs. For each detected codec, it walks the audio function group nodes and discovers:

- **Output converter widgets** -- These are the DACs that produce audio output.
- **Pin widgets** -- These represent physical jacks and speaker outputs.
- **Mixer and selector widgets** -- For routing audio between converters and pins.

Each widget has capability bits that describe what it can do (output capable, headphone capable, EAPD support, etc.).

### Output Path Configuration

To play audio, the driver needs to find a complete output path: a converter widget connected to a pin widget. It iterates through all codecs, finds pins with output capability, and pairs them with available converters. The pin's connection select is set to route audio from the converter, and the pin widget control is set to enable output (and headphone if supported).

### Stream Descriptors and DMA

HDA uses stream descriptors for DMA. Each descriptor is a 32-byte register block starting at offset `0x80`. The driver programs:

- **Buffer Descriptor List (BDL)** -- An array of up to 256 entries, each with a physical address, length, and IOC flag.
- **Cyclic Buffer Length (CBL)** -- Total bytes to play.
- **Last Valid Index (LVI)** -- Index of the last valid BDL entry.
- **Format** -- Sample rate, bit depth, and channel count encoded in a 16-bit format register.

The driver allocates a 64KB DMA buffer split into 4 periods, allowing the hardware to play one period while the software prepares the next.

### Format Negotiation

HDA supports a wide range of formats (8/16/20/24/32-bit, up to 8 channels, rates from 32kHz to 192kHz depending on the codec). The driver queries each converter's format capabilities and picks the best combination. It defaults to 16-bit stereo at 48kHz, which is universally supported.

### Beep Generation

The HDA beep works by generating a square wave in the DMA buffer and playing it through the first output stream. It configures the converter for 16-bit stereo, programs the BDL, and waits for the interrupt. The square wave amplitude is set to 8000 (out of 32767) for a comfortable volume.

---

## OPL3 FM Synthesis

**File:** `src/sound_opl3.c` (142 lines)

The Yamaha OPL3 is the FM synthesis chip that powered PC gaming audio in the early 1990s. Forest OS detects it at the standard port `0x388` (or `0x398`) by writing `0x00` and `0xFF` to the register port and checking that the read-back values differ.

Currently the OPL3 driver is more of a placeholder -- PCM playback is stubbed out (TODO), and beeps fall back to the PC speaker. The infrastructure is in place for someone to implement proper FM synthesis voice programming in the future. The detection and initialization are solid, and the driver correctly reports its capabilities (S16/U8, stereo, 22050/11025 Hz).

---

## Ensoniq AudioPCI Support

**File:** `src/sound_ensoniq.c` (328 lines)

The Ensoniq AudioPCI (ES1371) is a beloved retro sound card known for its excellent sound quality. Forest OS has a working driver for it.

### Hardware Interface

The ES1371 uses I/O ports via BAR0. Key registers include:

- **Control register** (`0x00`) -- Starts/stops playback, controls DAC enable.
- **Status register** (`0x04`) -- Interrupt status.
- **Sample Rate Converter** (`0x10`) -- Programs the sample rate via codec registers `0x75` and `0x77`.
- **Codec R/W** (`0x14`) -- Direct access to the AC97 codec connected to the ES1371.
- **Playback2 address/length/frames** (`0x38`, `0x3C`, `0x28`) -- DMA buffer physical address and size.

### Playback

The driver allocates a 64KB DMA buffer from low physical memory, copies PCM data into it, programs the serial interface register for the correct bit depth and channel configuration, sets the playback address and frame count, and enables the DAC2 (Playback 2) channel.

### Sample Rate Programming

The ES1371 has a built-in sample rate converter. The driver programs it by writing to codec registers `0x75` and `0x77` through the sample rate converter register (`0x10`). The frequency value is calculated as `(rate << 16) / 3000`.

### Format Conversion

The driver includes `convert_to_ensoniq_pcm()`, which converts any source format (S16, F32) to the Ensoniq's preferred format (S16 stereo at 44100 or 48000 Hz). It uses linear interpolation for rate conversion and handles mono-to-stereo upmixing.

---

## PCM Device Layer

**File:** `src/sound_pcm_device.c` (510 lines)

The PCM character device (`/dev/snd`, major 116) is the primary way userspace applications interact with the audio system. It provides a clean, POSIX-like interface: `open()`, `write()`, `ioctl()`, `close()`.

### How It Works

When an application opens `/dev/snd`, the device driver resolves the active sound hardware and creates a software mixer stream. The default format is S16LE stereo at 44100 Hz, but applications can change this via ioctls.

Write calls push PCM bytes into the mixer stream's ring buffer. A dedicated drain thread (`snd_pcm_drain_thread`) continuously pulls mixed frames from the mixer and feeds them to the hardware driver via `play_pcm()`. This double-buffering approach gives smooth, back-pressured playback without blocking the application on every DMA completion.

### Ioctl Interface

The device supports a rich set of ioctls:

| Ioctl | Description |
|---|---|
| `SND_IOCTL_SET_FORMAT` | Set input PCM format (rate, channels, format) |
| `SND_IOCTL_GET_FORMAT` | Query current input format |
| `SND_IOCTL_SET_RATE` | Set sample rate (8000-192000 Hz) |
| `SND_IOCTL_SET_CHANNELS` | Set channel count (1 or 2) |
| `SND_IOCTL_SET_VOLUME` | Set master and PCM volumes (0-255) |
| `SND_IOCTL_GET_VOLUME` | Query current volume |
| `SND_IOCTL_START` | Begin playback |
| `SND_IOCTL_STOP` | Pause playback |
| `SND_IOCTL_DRAIN` | Wait for all queued data to finish playing |
| `SND_IOCTL_WRITE_PCM` | Write PCM data via ioctl (alternative to write()) |
| `SND_IOCTL_GET_INFO` | Get device capabilities and name |
| `SND_IOCTL_GET_CAPS` | Get supported formats, rates, and feature flags |
| `SND_IOCTL_GET_POSITION` | Get playback position in frames |
| `SND_IOCTL_GET_LEVEL` | Get VU meter levels (RMS + peak per channel) |

### VU Meter

The `snd_vu_meter_compute()` function (`sound_vu_meter.c`) calculates per-channel RMS and peak levels from any interleaved PCM buffer. It supports S8, U8, S16LE, S16BE, S24LE, S32LE, and F32LE formats. The results are scaled to 0-32767 so applications can render VU bars directly.

### Graceful Degradation

Even when no sound hardware is available (audio disabled at build time or no driver detected), `/dev/snd` still exists. Ioctls return `-ENOSYS` and writes return `-ENODEV`, giving callers a clean failure path instead of a missing device node error.

---

## WAV File Playback

**Files:** `src/audio_wav.c` (852 lines), `src/include/audio_wav.h` (105 lines)

Forest OS has a comprehensive WAV file parser and decoder that handles streaming playback directly from disk without loading the entire file into memory.

### Header Parsing

The `wav_parse_header()` function scans through RIFF chunks to find the `fmt ` and `data` chunks. It handles:

- Standard PCM format (`0x0001`)
- IEEE float (`0x0003`)
- A-law (`0x0006`) and mu-law (`0x0007`)
- Extensible format (`0xFFFE`) -- extracts the real format from the sub-format field
- Word-aligned chunk padding

### Format Conversion

The decoder converts everything to 16-bit signed PCM as an intermediate format. Supported conversions include:

- **8-bit unsigned** to S16 (center at 128, scale by 256)
- **16-bit signed** -- pass through
- **24-bit signed** to S16 (right shift by 8)
- **32-bit signed** to S16 (right shift by 16)
- **32-bit float** to S16 (IEEE 754 decode, scale by 32767)
- **64-bit double** to S16 (IEEE 754 decode)
- **A-law** and **mu-law** via 256-entry lookup tables

### Streaming Playback

The `sound_play_wav()` function queues a WAV file for playback. It parses the header, creates a mixer stream, and sets up the stream to read from the VFS file. The mixer thread then reads chunks from disk, decodes them through `wav_decode_to_canonical()`, converts to the output format, and feeds them to the hardware driver. This means WAV files play without loading the entire file into memory.

---

## Mixer Functionality

**Files:** `src/sound_mixer.c` (344 lines), `src/include/sound_mixer.h` (67 lines)

The kernel software mixer is the heart of the audio system. It mixes multiple input streams -- each with its own format, sample rate, channel count, and volume -- into a single output stream at the hardware's native format.

### Ring Buffers

Each mixer stream has a 16KB ring buffer (`SND_MIXER_RING_SIZE = 16384`). Data is written to the ring via `snd_mixer_stream_write()` and consumed by `snd_mixer_process()`. The ring uses head/tail pointers with wraparound masking, so no copies are needed for the common case.

### Resampling

When input and output rates differ, the mixer uses 16.16 fixed-point linear interpolation. The step size is `(input_rate << 16) / output_rate`. For each output frame, it finds the corresponding input frame index and fractional offset, then interpolates between the two nearest samples. This avoids floating-point math entirely.

### Mixing

The mixer processes one output frame at a time. For each frame, it sums the contributions from all active streams, applying per-stream volume (0-255) and master volume. The result is clamped to the 16-bit signed range to prevent overflow distortion. If the output is mono, left and right channels are averaged.

### Format Support

The mixer can accept input in any of these formats:
- S8, U8 (1 byte/sample)
- S16LE, S16BE (2 bytes/sample)
- S24LE (3 bytes/sample)
- S32LE, F32LE (4 bytes/sample)

The output is always 16-bit signed PCM at the hardware's native rate and channel count.

---

## Userspace Interaction

Userspace applications interact with the audio system through two paths:

### 1. The `/dev/snd` Device

This is the primary path. A typical playback sequence looks like:

```c
int fd = open("/dev/snd", O_WRONLY);

// Set format
snd_pcm_format_t fmt = { .format = PCM_S16LE, .channels = 2, .rate = 44100 };
ioctl(fd, SND_IOCTL_SET_FORMAT, &fmt);

// Start playback
ioctl(fd, SND_IOCTL_START, 0);

// Write PCM data
write(fd, pcm_data, pcm_size);

// Wait for completion
ioctl(fd, SND_IOCTL_DRAIN, 0);

// Get volume levels
snd_level_t level;
ioctl(fd, SND_IOCTL_GET_LEVEL, &level);

close(fd);
```

### 2. The Forestcore Audio API

The `libs/forestcore/src/audio.c` file provides a higher-level C library API for applications that don't want to deal with ioctls directly:

```c
#include <audio.h>

// Register a driver (called by kernel components)
audio_driver_hooks_t hooks = { .output = my_output_fn, .beep = my_beep_fn };
audio_register_driver(&hooks);

// Play audio
audio_play_pcm(data, length, &format);

// Generate a beep
audio_beep(800, 120);

// Volume control
audio_set_master_volume(75);
```

This API is also available in userspace via the forestcore library, and it provides a simple hook-based system where kernel components can register their own audio handlers.

---

## Audio Buffer Management and DMA

The audio subsystem uses several layers of buffering to ensure smooth playback:

### Ring Buffers

Each mixer stream has a 16KB ring buffer for incoming PCM data. The ring is circular with power-of-2 size for efficient masking. Head and tail pointers advance independently, and the ring handles wraparound transparently. When the ring is full, `snd_mixer_stream_write()` returns a partial write count, and the drain thread backs off with a short sleep.

### DMA Buffers

Hardware drivers allocate DMA buffers from physically contiguous low memory (below 16MB for ISA DMA, below 4GB for PCI DMA). The allocation uses `bitmap_pmm_alloc_contiguous_pages()` with alignment constraints:

| Driver | Buffer Size | Alignment | Notes |
|---|---|---|---|
| PC Speaker | 4KB | None | Simple sample buffer |
| SB16 | 64KB | 64KB | ISA DMA requirement |
| AC97 | 8KB (2x4KB) | Page | Double-buffered via BDL |
| HDA | 64KB | Page | 4 periods via BDL |
| Ensoniq | 64KB | Page | Single buffer |

### Buffer Descriptor Lists (BDL)

AC97 and HDA use BDLs -- arrays of descriptors in physical memory that tell the DMA engine where to find audio data. Each entry contains a physical address, length, and flags (including IOC for interrupt-on-completion). The BDL itself is allocated from a physically contiguous page.

### The Mixer's Role

The mixer thread runs continuously when audio is active. On each iteration:

1. **Process work queue** -- Handle deferred operations (play WAV, stop stream, set volume, destroy stream).
2. **Produce data** -- For streams with file sources, read chunks from VFS, decode WAV to canonical format, convert to output format, and push into the ring buffer.
3. **Check completion** -- If a stream's source is EOF and its ring is empty, mark it done and fire its callback.
4. **Mix** -- Pull data from all active streams, mix with volume scaling, and produce output frames.
5. **Output** -- Hand the mixed buffer to the hardware driver's `play_pcm()` function.
6. **Sleep** -- Either for the calculated frame duration (when active) or for a short timeout (when idle).

The sleep calculation is clever: it sleeps for `(frames_mixed * 1000) / sample_rate` milliseconds, which keeps the mixer thread's wakeup rate matched to the audio output rate. This avoids both busy-waiting and excessive latency.

---

## Build Configuration

The audio subsystem is gated by feature flags in `build/features/audio.mk`. Each driver and format can be independently disabled:

| Flag | Default | Controls |
|---|---|---|
| `ENABLE_AUDIO` | yes | Entire audio subsystem |
| `ENABLE_SOUND_SB16` | yes | Sound Blaster 16 driver |
| `ENABLE_SOUND_SBPRO` | yes | Sound Blaster Pro driver |
| `ENABLE_SOUND_AC97` | yes | AC97 codec driver |
| `ENABLE_SOUND_HDA` | yes | Intel HD Audio driver |
| `ENABLE_SOUND_ENSONIQ` | yes | Ensoniq AudioPCI driver |
| `ENABLE_SOUND_OPL3` | yes | Yamaha OPL3 FM synthesis |
| `ENABLE_SOUND_PC_SPEAKER` | yes | PC speaker driver |
| `ENABLE_SOUND_USB` | yes | USB audio driver |
| `ENABLE_SOUND_VIRTIO` | yes | VirtIO sound driver |
| `ENABLE_AUDIO_WAV` | yes | WAV file parser/decoder |
| `ENABLE_AUDIO_VORBIS` | yes | Vorbis audio decoder |

Setting any of these to `no` excludes the corresponding source files from the build. Setting `ENABLE_AUDIO=no` excludes everything at once.

---

## Quick Reference

### Key Source Files

| File | Lines | Purpose |
|---|---|---|
| `src/sound.c` | 1024 | Core mixer, stream management, WAV playback |
| `src/include/sound.h` | 216 | SoundDriver vtable, PCM formats, ioctl definitions |
| `src/sound_pcm_device.c` | 510 | `/dev/snd` character device |
| `src/sound_mixer.c` | 344 | Software mixer with resampling |
| `src/audio_wav.c` | 852 | WAV parser and decoder |
| `src/sound_sb16.c` | 779 | Sound Blaster 16 driver |
| `src/sound_ac97.c` | 832 | AC97 codec driver |
| `src/sound_hda.c` | 1468 | Intel HD Audio driver |
| `src/sound_ensoniq.c` | 328 | Ensoniq AudioPCI driver |
| `src/sound_pc_speaker.c` | 216 | PC speaker driver |
| `src/sound_opl3.c` | 142 | OPL3 FM synthesis (placeholder) |
| `src/sound_sbpro.c` | 156 | Sound Blaster Pro driver |
| `src/sound_vu_meter.c` | 111 | VU meter level computation |

### Key Data Types

- `SoundDriver` -- Hardware driver vtable
- `SoundFormat` -- PCM format descriptor (rate, channels, bits)
- `DeviceCapabilities` -- What a driver supports
- `PcmDesc` -- Compact PCM format (format, channels, rate)
- `wav_info_t` -- Parsed WAV file metadata
- `snd_pcm_format_t` -- Userspace PCM format (for ioctls)
- `snd_volume_t` -- Volume levels (master, pcm, left, right, muted)
- `snd_level_t` -- VU meter levels (RMS + peak per channel)
