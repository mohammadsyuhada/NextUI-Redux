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

TARGET = mediaplayer
# Paths adjusted for location in workspace/all/mediaplayer/
INCDIR = -I. -I../common/ -I../../$(PLATFORM)/platform/ -I../minarch/libretro-common/include
INCDIR += -I../include -I../include/mbedtls_lib

# mbedTLS source files (for HTTPS support)
MBEDTLS_SRC = $(wildcard ../include/mbedtls_lib/*.c)

SOURCE = $(TARGET).c ffplay_engine.c video_browser.c settings.c wifi.c keyboard.c \
         wget_fetch.c \
         youtube.c subscriptions.c iptv.c iptv_curated.c \
         module_common.c module_menu.c module_player.c module_youtube.c module_subscriptions.c module_iptv.c module_settings.c \
         ui_fonts.c ui_icons.c ui_utils.c ui_main.c ui_player.c ui_youtube.c ui_subscriptions.c ui_iptv.c ui_settings.c \
         ../include/parson/parson.c \
         ../include/mbedtls_entropy_alt.c \
         $(MBEDTLS_SRC) \
         ../common/utils.c ../common/api.c ../common/config.c ../common/scaler.c ../common/ui_components.c ../common/ui_toast.c ../common/ui_list.c ../common/ui_keyboard.c ../common/ytdlp_updater.c \
         ../../$(PLATFORM)/platform/platform.c

CC = $(CROSS_COMPILE)gcc

# Reset CFLAGS to avoid issues with platform makefile.env
MY_CFLAGS = -O2 -fomit-frame-pointer
# mbedTLS config
MY_CFLAGS += -DMBEDTLS_CONFIG_FILE='<mbedtls_config.h>'
MY_CFLAGS += $(INCDIR) -DPLATFORM=\"$(PLATFORM)\" -std=gnu99
MY_CFLAGS += -DUSE_SDL2 -DUSE_GLES -DGL_GLEXT_PROTOTYPES
MY_CFLAGS += -I$(PREFIX)/include -I$(PREFIX_LOCAL)/include
MY_CFLAGS += -I../../$(PLATFORM)/libmsettings

MY_LDFLAGS = -L$(PREFIX)/lib -L$(PREFIX_LOCAL)/lib
MY_LDFLAGS += -L$(PREFIX)/lib/$(CROSS_TRIPLE)
MY_LDFLAGS += -L../../$(PLATFORM)/libmsettings
MY_LDFLAGS += -L../minarch/build/$(PLATFORM)
MY_LDFLAGS += $(shell pkg-config --libs sdl2 glesv2 2>/dev/null || echo "-lSDL2 -lGLESv2")
MY_LDFLAGS += -lSDL2_image -lSDL2_ttf
MY_LDFLAGS += -lmsettings -lsamplerate -lzip -lm -lpthread -ldl -lz

# Platform-specific dependencies
ifeq ($(PLATFORM), tg5050)
MY_LDFLAGS += -ltinyalsa
endif

PRODUCT = build/$(PLATFORM)/$(TARGET).elf

all:
	@mkdir -p build/$(PLATFORM)
	$(CC) $(SOURCE) -o $(PRODUCT) $(MY_CFLAGS) $(MY_LDFLAGS)

clean:
	rm -f build/tg5040/$(TARGET).elf build/tg5050/$(TARGET).elf
