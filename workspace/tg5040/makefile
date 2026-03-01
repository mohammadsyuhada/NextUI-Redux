###########################################################

ifeq (,$(PLATFORM))
PLATFORM=$(UNION_PLATFORM)
endif

ifeq (,$(PLATFORM))
$(error please specify PLATFORM, eg. PLATFORM=rgb30 make)
endif

###########################################################

REQUIRES_EVTEST=other/evtest
REQUIRES_JSTEST=other/jstest
REQUIRES_UNZIP60=other/unzip60
REQUIRES_FILEMGR=other/NextCommander
REQUIRES_MUPEN64PLUS=other/mupen64plus

all: readmes

early: $(REQUIRES_EVTEST) $(REQUIRES_JSTEST) $(REQUIRES_UNZIP60) $(REQUIRES_FILEMGR)
	mkdir -p other
	cd $(REQUIRES_FILEMGR) && make -j
	cd $(REQUIRES_EVTEST) && $(CROSS_COMPILE)gcc -o evtest evtest.c
	cd $(REQUIRES_JSTEST) && $(CROSS_COMPILE)gcc -o jstest jstest.c
	cd $(REQUIRES_UNZIP60) && make -f unix/Makefile.trimuismart unzip

clean:
	cd $(REQUIRES_FILEMGR) && make clean
	cd $(REQUIRES_UNZIP60) && make -f unix/Makefile.trimuismart clean

###########################################################

$(REQUIRES_FILEMGR):
	git clone --depth 1 https://github.com/LoveRetro/NextCommander.git $(REQUIRES_FILEMGR)

$(REQUIRES_EVTEST):
	git clone --depth 1 https://github.com/freedesktop-unofficial-mirror/evtest.git $(REQUIRES_EVTEST)

$(REQUIRES_JSTEST):
	git clone --depth 1 https://github.com/datrh/joyutils.git $(REQUIRES_JSTEST)

$(REQUIRES_UNZIP60):
	git clone --depth 1 https://github.com/shauninman/unzip60.git $(REQUIRES_UNZIP60)

$(REQUIRES_MUPEN64PLUS):
	mkdir -p $(REQUIRES_MUPEN64PLUS)
	git clone --depth 1 https://github.com/mupen64plus/mupen64plus-core.git $(REQUIRES_MUPEN64PLUS)/mupen64plus-core
	git clone --depth 1 https://github.com/mupen64plus/mupen64plus-ui-console.git $(REQUIRES_MUPEN64PLUS)/mupen64plus-ui-console
	git clone --depth 1 --recurse-submodules https://github.com/gonetz/GLideN64.git $(REQUIRES_MUPEN64PLUS)/GLideN64-standalone

include ../all/readmes/makefile
