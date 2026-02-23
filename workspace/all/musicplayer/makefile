###########################################################
# Supported platforms
SUPPORTED_PLATFORMS = tg5040 tg5050
DEFAULT_PLATFORM = tg5040

# Set platform from environment or default
ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
PLATFORM=$(DEFAULT_PLATFORM)
endif

# Validate platform
ifeq (,$(filter $(PLATFORM),$(SUPPORTED_PLATFORMS)))
$(error Invalid PLATFORM '$(PLATFORM)'. Supported: $(SUPPORTED_PLATFORMS))
endif

# Validate cross-compiler
ifeq (,$(CROSS_COMPILE))
$(error Missing CROSS_COMPILE for this toolchain)
endif

###########################################################

include ../../$(PLATFORM)/platform/makefile.env

TARGET = musicplayer
INCDIR = -I. -I./audio -I../common/ -I../../$(PLATFORM)/platform/ -I../minarch/libretro-common/include
INCDIR += -I../include -I../include/mbedtls_lib -I./include/yxml
INCDIR += -I./include/libogg -I./include/libopus/include -I./include/opusfile/include

# mbedTLS source files (for HTTPS support)
MBEDTLS_SRC = $(wildcard ../include/mbedtls_lib/*.c)

# Opus/OGG library sources (compiled separately to avoid header conflicts)
LIBOGG_SRC = include/libogg/src/bitwise.c include/libogg/src/framing.c
LIBOPUS_SRC = $(filter-out %/opus_custom_demo.c %/opus_demo.c %/opus_compare.c %/qext_compare.c %/repacketizer_demo.c %/mini_kfft.c, \
              $(wildcard include/libopus/src/*.c) \
              $(wildcard include/libopus/celt/*.c) \
              $(wildcard include/libopus/silk/*.c) \
              $(wildcard include/libopus/silk/float/*.c))
OPUSFILE_SRC = include/opusfile/src/info.c include/opusfile/src/internal.c \
               include/opusfile/src/opusfile.c include/opusfile/src/stream.c
OPUS_ALL_SRC = $(LIBOGG_SRC) $(LIBOPUS_SRC) $(OPUSFILE_SRC)

# Opus-specific include paths and flags (isolated to avoid API.h conflicts with project)
OPUS_INCDIR = -I./include/libogg -I./include/libopus/include -I./include/opusfile/include \
              -I./include/libopus -I./include/libopus/src -I./include/libopus/celt \
              -I./include/libopus/silk -I./include/libopus/silk/float
OPUS_CFLAGS = -O2 -fomit-frame-pointer -DOPUS_BUILD -DVAR_ARRAYS -DHAVE_LRINTF \
              -DOP_DISABLE_HTTP -DOP_DISABLE_FLOAT_API -std=gnu99

SOURCE = $(TARGET).c player.c playlist.c playlist_m3u.c radio.c radio_net.c album_art.c lyrics.c radio_hls.c radio_curated.c downloader.c \
         podcast.c podcast_rss.c podcast_search.c http_download.c wget_fetch.c wifi.c settings.c resume.c add_to_playlist.c background.c \
         module_common.c module_menu.c module_library.c module_player.c module_playlist.c module_radio.c module_podcast.c module_downloader.c module_settings.c \
         ui_fonts.c ui_icons.c ui_utils.c browser.c ui_album_art.c ui_main.c ui_music.c ui_radio.c ui_downloader.c ui_podcast.c ui_playlist.c ui_settings.c \
         spectrum.c audio/kiss_fft.c audio/kiss_fftr.c \
         include/yxml/yxml.c \
         ../include/parson/parson.c \
         ../include/mbedtls_entropy_alt.c \
         $(MBEDTLS_SRC) \
         ../common/utils.c ../common/api.c ../common/config.c ../common/scaler.c ../common/ui_components.c ../common/ui_toast.c ../common/ui_list.c ../common/ui_listdialog.c ../common/ui_keyboard.c ../common/ytdlp_updater.c \
         ../../$(PLATFORM)/platform/platform.c

CC = $(CROSS_COMPILE)gcc

# Reset CFLAGS to avoid issues with platform makefile.env
MY_CFLAGS = -O2 -fomit-frame-pointer
# mbedTLS config
MY_CFLAGS += -DMBEDTLS_CONFIG_FILE='<mbedtls_config.h>'
MY_CFLAGS += -DOPUS_BUILD -DHAVE_LRINTF -DOP_DISABLE_HTTP -DOP_DISABLE_FLOAT_API
MY_CFLAGS += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -std=gnu99
MY_CFLAGS += -DUSE_SDL2 -DUSE_GLES -DGL_GLEXT_PROTOTYPES
MY_CFLAGS += -I$(PREFIX)/include -I$(PREFIX_LOCAL)/include -I./include/fdk_aac
MY_CFLAGS += -I../../$(PLATFORM)/libmsettings

MY_LDFLAGS = -L$(PREFIX)/lib -L$(PREFIX_LOCAL)/lib -L./include/fdk_aac/lib
MY_LDFLAGS += -L$(PREFIX)/lib/$(CROSS_TRIPLE)
MY_LDFLAGS += -L../../$(PLATFORM)/libmsettings
MY_LDFLAGS += -L../minarch/build/$(PLATFORM)
MY_LDFLAGS += $(shell pkg-config --libs sdl2 glesv2 2>/dev/null || echo "-lSDL2 -lGLESv2")
MY_LDFLAGS += -lSDL2_image -lSDL2_ttf
MY_LDFLAGS += -lmsettings -lsamplerate -lzip -lm -lpthread -ldl -lz
MY_LDFLAGS += -lasound -lfdk-aac

# Platform-specific dependencies
ifeq ($(PLATFORM), tg5050)
MY_LDFLAGS += -ltinyalsa
endif

PRODUCT = build/$(PLATFORM)/$(TARGET).elf
OPUS_OBJ_DIR = opus_obj

all:
	@mkdir -p build/$(PLATFORM) $(OPUS_OBJ_DIR)
	$(CC) -c $(OPUS_ALL_SRC) $(OPUS_CFLAGS) $(OPUS_INCDIR)
	@mv *.o $(OPUS_OBJ_DIR)/
	$(CC) $(SOURCE) $(OPUS_OBJ_DIR)/*.o -o $(PRODUCT) $(MY_CFLAGS) $(MY_LDFLAGS)
	@rm -rf $(OPUS_OBJ_DIR)

clean:
	rm -f build/tg5040/$(TARGET).elf build/tg5050/$(TARGET).elf
	rm -rf $(OPUS_OBJ_DIR)
