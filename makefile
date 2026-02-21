# NextUI

# NOTE: this runs on the host system (eg. macOS) not in a docker image
# it has to, otherwise we'd be running a docker in a docker and oof

# prevent accidentally triggering a full build with invalid calls
ifneq (,$(PLATFORM))
ifeq (,$(MAKECMDGOALS))
$(error found PLATFORM arg but no target, did you mean "make PLATFORM=$(PLATFORM) shell"?)
endif
endif

ifeq (,$(PLATFORMS))
#PLATFORMS = miyoomini trimuismart rg35xx rg35xxplus my355 tg5040 zero28 rgb30 m17 gkdpixel my282 magicmini
PLATFORMS = tg5050 tg5040
endif

###########################################################

BUILD_HASH:=$(shell git rev-parse --short HEAD)
BUILD_TAG:=$(shell git describe --tags --abbrev=0 2>/dev/null || echo "untagged")
BUILD_BRANCH:=$(shell (git symbolic-ref --short HEAD 2>/dev/null || git rev-parse --short HEAD) | sed 's/\//-/g')
RELEASE_TIME:=$(shell TZ=GMT date +%Y%m%d)
ifeq ($(BUILD_BRANCH),main)
  RELEASE_BETA :=
else
  RELEASE_BETA := -$(BUILD_BRANCH)
endif
ifeq ($(PLATFORM), desktop)
	TOOLCHAIN_FILE := makefile.native
else
	TOOLCHAIN_FILE := makefile.toolchain
endif
RELEASE_NAME ?= NextUI-$(RELEASE_TIME)$(RELEASE_BETA)

# Extra paks to ship
VENDOR_DEST := ./build/VENDOR/Tools
PACKAGE_URL_MAPPINGS := \
	"https://github.com/UncleJunVIP/nextui-pak-store/releases/latest/download/Pak.Store.pakz nextui.pak_store.pakz"
	# add more URLs as needed

###########################################################

.PHONY: build

export MAKEFLAGS=--no-print-directory

ifndef PLATFORM
deploy:
	$(error PLATFORM is required for deploy (e.g. make deploy PLATFORM=tg5040))
else
deploy: setup $(PLATFORMS) special package
	adb push ./build/BASE/MinUI-$(PLATFORM).zip /mnt/SDCARD/MinUI.zip && adb shell reboot
endif

all: setup $(PLATFORMS) special package done
	
shell:
	make -f $(TOOLCHAIN_FILE) PLATFORM=$(PLATFORM)

name: 
	@echo $(RELEASE_NAME)

build:
	# ----------------------------------------------------
	make build -f $(TOOLCHAIN_FILE) PLATFORM=$(PLATFORM) COMPILE_CORES=$(COMPILE_CORES)
	# ----------------------------------------------------

build-cores:
	make build-cores -f $(TOOLCHAIN_FILE) PLATFORM=$(PLATFORM) COMPILE_CORES=true
	# ----------------------------------------------------

cores-json:
	@cat workspace/$(PLATFORM)/cores/makefile | grep ^CORES | cut -d' ' -f2 | jq  --raw-input .  | jq --slurp -cM .

build-core:
ifndef CORE
	$(error CORE is not set)
endif
	make build-core -f $(TOOLCHAIN_FILE) PLATFORM=$(PLATFORM) COMPILE_CORES=true CORE=$(CORE)

system:
	make -f ./workspace/$(PLATFORM)/platform/makefile.copy PLATFORM=$(PLATFORM)
	
	# populate system
ifneq ($(PLATFORM), desktop)
	cp ./workspace/$(PLATFORM)/keymon/keymon.elf ./build/SYSTEM/$(PLATFORM)/bin/
	cp ./workspace/all/syncsettings/build/$(PLATFORM)/syncsettings.elf ./build/SYSTEM/$(PLATFORM)/bin/
	cp ./workspace/all/audiomon/build/$(PLATFORM)/audiomon.elf ./build/SYSTEM/$(PLATFORM)/bin/
	cp ./workspace/all/show2/build/$(PLATFORM)/show2.elf ./build/SYSTEM/$(PLATFORM)/bin/

	# battery tracking
	cp ./workspace/all/libbatmondb/build/$(PLATFORM)/libbatmondb.so ./build/SYSTEM/$(PLATFORM)/lib
	cp ./workspace/all/batmon/build/$(PLATFORM)/batmon.elf ./build/SYSTEM/$(PLATFORM)/bin/
	cp ./workspace/all/battery/build/$(PLATFORM)/battery.elf ./build/EXTRAS/Tools/$(PLATFORM)/Battery.pak/

	# game time tracking
	cp ./workspace/all/libgametimedb/build/$(PLATFORM)/libgametimedb.so ./build/SYSTEM/$(PLATFORM)/lib
	cp ./workspace/all/gametimectl/build/$(PLATFORM)/gametimectl.elf ./build/SYSTEM/$(PLATFORM)/bin/
	cp ./workspace/all/gametime/build/$(PLATFORM)/gametime.elf ./build/EXTRAS/Tools/$(PLATFORM)/Game\ Tracker.pak/
endif
	cp ./workspace/$(PLATFORM)/libmsettings/libmsettings.so ./build/SYSTEM/$(PLATFORM)/lib
	cp ./workspace/all/nextui/build/$(PLATFORM)/nextui.elf ./build/SYSTEM/$(PLATFORM)/bin/
	cp ./workspace/all/minarch/build/$(PLATFORM)/minarch.elf ./build/SYSTEM/$(PLATFORM)/bin/
	cp ./workspace/all/nextval/build/$(PLATFORM)/nextval.elf ./build/SYSTEM/$(PLATFORM)/bin/
	cp ./workspace/all/clock/build/$(PLATFORM)/clock.elf ./build/EXTRAS/Tools/$(PLATFORM)/Clock.pak/
	cp ./workspace/all/input/build/$(PLATFORM)/input.elf ./build/EXTRAS/Tools/$(PLATFORM)/Input.pak/
	cp ./workspace/all/settings/build/$(PLATFORM)/settings.elf ./build/EXTRAS/Tools/$(PLATFORM)/Settings.pak/
	cp ./workspace/all/updater/build/$(PLATFORM)/updater.elf ./build/EXTRAS/Tools/$(PLATFORM)/Updater.pak/
ifneq (,$(filter $(PLATFORM),tg5040 tg5050))
	cp ./workspace/all/bootlogo/build/$(PLATFORM)/bootlogo.elf ./build/EXTRAS/Tools/$(PLATFORM)/Bootlogo.pak/
ifeq ($(PLATFORM), tg5040)
	# Limbo fix
	cp ./workspace/$(PLATFORM)/poweroff_next/build/$(PLATFORM)/poweroff_next.elf ./build/SYSTEM/$(PLATFORM)/bin/poweroff_next
endif
	# Audio resampling
	cp ./workspace/all/minarch/build/$(PLATFORM)/libsamplerate.* ./build/SYSTEM/$(PLATFORM)/lib/

	# ROM decompression and SRM support
	cp ./workspace/all/minarch/build/$(PLATFORM)/libzip.* ./build/SYSTEM/$(PLATFORM)/lib/
	cp ./workspace/all/minarch/build/$(PLATFORM)/libbz2.* ./build/SYSTEM/$(PLATFORM)/lib/
	cp ./workspace/all/minarch/build/$(PLATFORM)/liblzma.* ./build/SYSTEM/$(PLATFORM)/lib/
	cp ./workspace/all/minarch/build/$(PLATFORM)/libzstd.* ./build/SYSTEM/$(PLATFORM)/lib/

	# libchdr for RetroAchievements CHD hashing
	cp ./workspace/all/minarch/build/$(PLATFORM)/libchdr.so.* ./build/SYSTEM/$(PLATFORM)/lib/

ifeq ($(PLATFORM), tg5040)
	# liblz4 for Rewind support
	cp -L ./workspace/all/minarch/build/$(PLATFORM)/liblz4.so.1 ./build/SYSTEM/$(PLATFORM)/lib/
endif
endif

ifeq ($(PLATFORM), desktop)
cores:
	# stock cores
	#cp ./workspace/$(PLATFORM)/cores/output/gambatte_libretro.so ./build/SYSTEM/$(PLATFORM)/cores
	#cp ./workspace/$(PLATFORM)/cores/output/gpsp_libretro.so ./build/SYSTEM/$(PLATFORM)/cores
	#cp ./workspace/$(PLATFORM)/cores/output/mgba_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/MGBA.pak
else
cores: # TODO: can't assume every platform will have the same stock cores (platform should be responsible for copy too)
	# stock cores
	cp ./workspace/$(PLATFORM)/cores/output/fceumm_libretro.so ./build/SYSTEM/$(PLATFORM)/cores
	cp ./workspace/$(PLATFORM)/cores/output/gambatte_libretro.so ./build/SYSTEM/$(PLATFORM)/cores
	cp ./workspace/$(PLATFORM)/cores/output/gpsp_libretro.so ./build/SYSTEM/$(PLATFORM)/cores
	cp ./workspace/$(PLATFORM)/cores/output/picodrive_libretro.so ./build/SYSTEM/$(PLATFORM)/cores
	cp ./workspace/$(PLATFORM)/cores/output/snes9x_libretro.so ./build/SYSTEM/$(PLATFORM)/cores
	cp ./workspace/$(PLATFORM)/cores/output/pcsx_rearmed_libretro.so ./build/SYSTEM/$(PLATFORM)/cores
	
	# extras
	cp ./workspace/$(PLATFORM)/cores/output/a5200_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/A5200.pak
	cp ./workspace/$(PLATFORM)/cores/output/prosystem_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/A7800.pak
	cp ./workspace/$(PLATFORM)/cores/output/stella2014_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/A2600.pak
	cp ./workspace/$(PLATFORM)/cores/output/handy_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/LYNX.pak
	cp ./workspace/$(PLATFORM)/cores/output/mgba_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/MGBA.pak
	cp ./workspace/$(PLATFORM)/cores/output/mgba_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/SGB.pak
	cp ./workspace/$(PLATFORM)/cores/output/mednafen_pce_fast_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/PCE.pak
	cp ./workspace/$(PLATFORM)/cores/output/pokemini_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/PKM.pak
	cp ./workspace/$(PLATFORM)/cores/output/race_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/NGP.pak
	cp ./workspace/$(PLATFORM)/cores/output/race_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/NGPC.pak
	cp ./workspace/$(PLATFORM)/cores/output/fbneo_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/FBN.pak
	cp ./workspace/$(PLATFORM)/cores/output/mednafen_supafaust_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/SUPA.pak
	cp ./workspace/$(PLATFORM)/cores/output/mednafen_vb_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/VB.pak
	cp ./workspace/$(PLATFORM)/cores/output/cap32_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/CPC.pak
	cp ./workspace/$(PLATFORM)/cores/output/puae2021_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/PUAE.pak
	cp ./workspace/$(PLATFORM)/cores/output/prboom_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/PRBOOM.pak
	cp ./workspace/$(PLATFORM)/cores/output/vice_x64_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/C64.pak
	cp ./workspace/$(PLATFORM)/cores/output/vice_x128_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/C128.pak
	cp ./workspace/$(PLATFORM)/cores/output/vice_xplus4_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/PLUS4.pak
	cp ./workspace/$(PLATFORM)/cores/output/vice_xpet_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/PET.pak
	cp ./workspace/$(PLATFORM)/cores/output/vice_xvic_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/VIC.pak
	cp ./workspace/$(PLATFORM)/cores/output/bluemsx_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/MSX.pak
	cp ./workspace/$(PLATFORM)/cores/output/gearcoleco_libretro.so ./build/EXTRAS/Emus/$(PLATFORM)/COLECO.pak
endif

common: build system cores
	
format:
	git ls-files '*.c' '*.h' | xargs clang-format -i

compile-commands:
	@echo "Generating compile_commands.json..."
	@echo '[' > compile_commands.json
	@first=1; \
	git ls-files '*.c' | \
	grep -v 'cores/src' | \
		grep -v 'libretro-common' | \
		grep -v 'libchdr' | \
		grep -v 'rcheevos/src' | \
		grep -v 'nextui-music-player' | \
		grep -v 'nextui-video-player' | \
		grep -v 'nextui-netplay' | \
	while read -r file; do \
		if [ "$$first" = "1" ]; then first=0; else echo ',' >> compile_commands.json; fi; \
		printf '  {"directory": "%s", "file": "%s/%s", "arguments": ["clang", "-std=gnu99", "-DUSE_SDL2", "-DUSE_GLES", "-DGL_GLEXT_PROTOTYPES", "-DPLATFORM=\\"tg5040\\"", "-I%s/workspace/all/common", "-I%s/workspace/all/nextui", "-I%s/workspace/all/minarch", "-I%s/workspace/all/minarch/libretro-common/include", "-I%s/workspace/tg5040/platform", "-I%s/workspace/tg5050/platform", "-I%s/workspace/desktop/platform", "-I%s/workspace/tg5040/libmsettings", "-I%s/workspace/tg5050/libmsettings", "-I%s/workspace/desktop/libmsettings", "-I/opt/homebrew/include", "-c", "%s/%s"]}' \
			"$(CURDIR)" "$(CURDIR)" "$$file" \
			"$(CURDIR)" "$(CURDIR)" "$(CURDIR)" "$(CURDIR)" "$(CURDIR)" "$(CURDIR)" "$(CURDIR)" "$(CURDIR)" "$(CURDIR)" "$(CURDIR)" \
			"$(CURDIR)" "$$file" >> compile_commands.json; \
	done
	@echo '' >> compile_commands.json
	@echo ']' >> compile_commands.json
	@echo "Done. Generated compile_commands.json"

clean:
	rm -rf ./build
	make clean -f $(TOOLCHAIN_FILE) PLATFORM=$(PLATFORM) COMPILE_CORES=$(COMPILE_CORES)

setup: name
	# ----------------------------------------------------
	# make sure we're running in an input device
	tty -s || echo "No tty detected"
	
	# ready fresh build
	rm -rf ./build
	mkdir -p ./releases
	cp -R ./skeleton ./build
	
	# remove authoring detritus
	cd ./build && find . -type f -name '.keep' -delete
	cd ./build && find . -type f -name '*.meta' -delete
	echo $(BUILD_HASH) > ./workspace/hash.txt
	
	# copy readmes to workspace so we can use Linux fmt instead of host's
	mkdir -p ./workspace/readmes
	cp ./skeleton/BASE/README.txt ./workspace/readmes/BASE-in.txt
	cp ./skeleton/EXTRAS/README.txt ./workspace/readmes/EXTRAS-in.txt
	
done:
	# say "done" 2>/dev/null || true

special:
	# setup miyoomini/trimui/magicx family .tmp_update in BOOT
	mv ./build/BOOT/common ./build/BOOT/.tmp_update
#	mv ./build/BOOT/miyoo ./build/BASE/
	mv ./build/BOOT/trimui ./build/BASE/
#	mv ./build/BOOT/magicx ./build/BASE/
#	cp -R ./build/BOOT/.tmp_update ./build/BASE/miyoo/app/
	cp -R ./build/BOOT/.tmp_update ./build/BASE/trimui/app/
#	cp -R ./build/BOOT/.tmp_update ./build/BASE/magicx/
#	cp -R ./build/BASE/miyoo ./build/BASE/miyoo354
#	cp -R ./build/BASE/miyoo ./build/BASE/miyoo355
#ifneq (,$(findstring my355, $(PLATFORMS)))
#	cp -R ./workspace/my355/init ./build/BASE/miyoo355/app/my355
#	cp -r ./workspace/my355/other/squashfs/output/* ./build/BASE/miyoo355/app/my355/payload/
#endif

tidy:
	@for plat in $(PLATFORMS); do \
		rm -f releases/$(RELEASE_NAME)-$$plat.zip; \
	done
	# ----------------------------------------------------
	# copy update from merged platform to old pre-merge platform bin so old cards update properly

package: tidy
	# ----------------------------------------------------
	# zip up build

	# move formatted readmes from workspace to build
	cp ./workspace/readmes/BASE-out.txt ./build/BASE/README.txt
	cp ./workspace/readmes/EXTRAS-out.txt ./build/EXTRAS/README.txt
	rm -rf ./workspace/readmes

	cd ./build/SYSTEM && printf "%s\n%s\n%s\n" "$(RELEASE_NAME)" "$(BUILD_HASH)" "$(BUILD_TAG)" > version.txt
	./commits.sh > ./build/SYSTEM/commits.txt
	cd ./build && find . -type f -name '.DS_Store' -delete

	# Fetch, rename, and stage vendored packages
	mkdir -p $(VENDOR_DEST)
	@for entry in $(PACKAGE_URL_MAPPINGS); do \
		url=$$(echo $$entry | awk '{print $$1}'); \
		target=$$(echo $$entry | awk '{print $$2}'); \
		echo "Downloading $$url → $(VENDOR_DEST)/$$target"; \
		curl -Ls -o "$(VENDOR_DEST)/$$target" "$$url"; \
	done

	# Move renamed .pakz files into base folder
	mkdir -p ./build/BASE
	mv $(VENDOR_DEST)/* ./build/BASE/

	# --- Per-platform packaging ---
	@for plat in $(PLATFORMS); do \
		echo "# ===== Packaging $$plat ====="; \
		rm -rf ./build/PAYLOAD-$$plat; \
		mkdir -p ./build/PAYLOAD-$$plat/.system; \
		\
		echo "  assembling .system/$$plat"; \
		cp -R ./build/SYSTEM/$$plat   ./build/PAYLOAD-$$plat/.system/$$plat; \
		cp -R ./build/SYSTEM/res      ./build/PAYLOAD-$$plat/.system/res; \
		cp -R ./build/SYSTEM/shared   ./build/PAYLOAD-$$plat/.system/shared; \
		cp ./build/SYSTEM/version.txt ./build/PAYLOAD-$$plat/.system/version.txt; \
		cp ./build/SYSTEM/commits.txt ./build/PAYLOAD-$$plat/.system/commits.txt; \
		\
		echo "  assembling .tmp_update"; \
		cp -R ./build/BOOT/.tmp_update ./build/PAYLOAD-$$plat/.tmp_update; \
		\
		echo "  assembling Tools/$$plat"; \
		mkdir -p ./build/PAYLOAD-$$plat/Tools; \
		cp -R ./build/EXTRAS/Tools/$$plat ./build/PAYLOAD-$$plat/Tools/$$plat; \
		\
		echo "  creating MinUI.zip"; \
		cd ./build/PAYLOAD-$$plat && zip -r MinUI.zip .system .tmp_update Tools && cd ../..; \
		cp ./build/PAYLOAD-$$plat/MinUI.zip ./build/BASE/MinUI-$$plat.zip; \
		\
		echo "  creating release zip"; \
		cd ./build/BASE && zip -r ../../releases/$(RELEASE_NAME)-$$plat.zip \
			Bios Cheats Collections Favorites Music Overlays \
			"Recently Played" Roms Saves Shaders trimui *.pakz README.txt \
			&& cd ../..; \
		cd ./build/PAYLOAD-$$plat && zip -r ../../releases/$(RELEASE_NAME)-$$plat.zip MinUI.zip && cd ../..; \
		cd ./build/EXTRAS && zip -r ../../releases/$(RELEASE_NAME)-$$plat.zip \
			Bios Cheats Roms Saves Overlays README.txt \
			&& cd ../..; \
		cd ./build/EXTRAS && zip -r ../../releases/$(RELEASE_NAME)-$$plat.zip \
			Emus/$$plat Tools/$$plat \
			&& cd ../..; \
		if [ -d ./build/PAKZ/$$plat ]; then \
			cd ./build/PAKZ/$$plat && zip -r ../../../releases/$(RELEASE_NAME)-$$plat.zip *.pakz && cd ../../..; \
		fi; \
		\
		rm -rf ./build/PAYLOAD-$$plat; \
		echo "# ===== Done: $$plat ====="; \
	done
	
###########################################################

.DEFAULT:
	# ----------------------------------------------------
	# $@
	@echo "$(PLATFORMS)" | grep -q "\b$@\b" && (make common PLATFORM=$@) || (exit 1)
	
