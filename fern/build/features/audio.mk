# build/features/audio.mk
#
# Audio subsystem gating. ENABLE_AUDIO is the parent. When =no, all sound
# sources are excluded. Per-driver/per-format toggles gate individual files.
# All paths use $(wildcard ...) so missing files never break the build.

ifeq ($(ENABLE_AUDIO),no)
EXCLUDED_CSOURCES += $(wildcard \
    $(SRCDIR)/sound.c \
    $(SRCDIR)/sound_mixer.c \
    $(SRCDIR)/sound_pcm_device.c \
    $(SRCDIR)/sound_universal.c \
    $(SRCDIR)/sound_usb.c \
    $(SRCDIR)/audio.c \
    $(SRCDIR)/audio_wav.c \
    $(SRCDIR)/sound_ac97.c \
    $(SRCDIR)/sound_ac97_driver.c \
    $(SRCDIR)/sound_sb16.c \
    $(SRCDIR)/sound_sb16_test.c \
    $(SRCDIR)/sound_sbpro.c \
    $(SRCDIR)/sound_hda.c \
    $(SRCDIR)/sound_hda_driver.c \
    $(SRCDIR)/sound_ensoniq.c \
    $(SRCDIR)/sound_opl3.c \
    $(SRCDIR)/sound_pc_speaker.c \
    $(SRCDIR)/sound_vu_meter.c \
    $(SRCDIR)/stb_vorbis.c \
    $(SRCDIR)/stb_vorbis_port.c)
endif

ifeq ($(ENABLE_SOUND_SB16),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/sound_sb16.c $(SRCDIR)/sound_sb16_test.c)
endif

ifeq ($(ENABLE_SOUND_SBPRO),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/sound_sbpro.c)
endif

ifeq ($(ENABLE_SOUND_AC97),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/sound_ac97.c $(SRCDIR)/sound_ac97_driver.c)
endif

ifeq ($(ENABLE_SOUND_HDA),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/sound_hda.c $(SRCDIR)/sound_hda_driver.c)
endif

ifeq ($(ENABLE_SOUND_ENSONIQ),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/sound_ensoniq.c)
endif

ifeq ($(ENABLE_SOUND_OPL3),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/sound_opl3.c)
endif

ifeq ($(ENABLE_SOUND_PC_SPEAKER),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/sound_pc_speaker.c)
endif

ifeq ($(ENABLE_SOUND_USB),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/sound_usb.c)
endif

ifeq ($(ENABLE_AUDIO_WAV),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/audio_wav.c)
endif

ifeq ($(ENABLE_AUDIO_VORBIS),no)
EXCLUDED_CSOURCES += $(wildcard $(SRCDIR)/stb_vorbis.c $(SRCDIR)/stb_vorbis_port.c)
endif
