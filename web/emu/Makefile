# The display in the browser (README.md): LVGL + the firmware's screens built to WebAssembly with Emscripten.
#   make            # in WSL, after `source ~/emsdk/emsdk_env.sh`: build/emu.js + build/emu.wasm, then open index.html
# LVGL: the firmware's own copy (ESP-IDF's component manager puts it in managed_components); cJSON: ESP-IDF's.
LVGL ?= ../../managed_components/lvgl__lvgl
# espforge's components (i18n core, png_rows, textfit; the headers of net, web, ota, svc): the firmware's copy at the tag
# in main/idf_component.yml (managed_components after an idf.py build), else tools/fetch_forge.py's .espforge
FORGE ?= $(if $(wildcard ../../managed_components/forge_core/CMakeLists.txt),../../managed_components,../../.espforge/components)
IDF_PATH ?= /mnt/c/Espressif/esp-idf
CJSON ?= $(IDF_PATH)/components/json/cJSON
ifeq ($(filter clean,$(MAKECMDGOALS)),)
ifeq ($(wildcard $(CJSON)/cJSON.c),)
$(error no cJSON at $(CJSON): set IDF_PATH (source ESP-IDF's export.sh) or CJSON=<folder with cJSON.c>)
endif
ifeq ($(wildcard $(FORGE)/forge_core/i18n.c),)
$(error no espforge components at $(FORGE): build the firmware once, or python tools/fetch_forge.py, or FORGE=<espforge>/components)
endif
ifeq ($(wildcard $(LVGL)/lvgl.h),)
$(error no LVGL at $(LVGL): build the firmware once (idf.py fetches it into managed_components) or LVGL=<lvgl checkout>)
endif
endif
MAIN := ../../main
OUT := build
VERSION ?= $(shell git describe --tags --always 2>/dev/null || echo dev)

FIRMWARE := ui.c slide.c pager.c config.c i18n.c weather.c alerts.c radar.c sound.c services.c audio.c routes.c
FORGE_SRC := i18n.c png_rows.c textfit.c
FORGE_OTA_SRC := ota_web.c
# Screen dimming: espforge's forge_presence as the firmware builds it (its task, NVS settings, console commands, web
# routes), with the board's hooks from emu_main.c (main/audio.c on emu_audio.c, emu_imu.c, emu_touch.c)
FORGE_PRESENCE_SRC := presence.c presence_sm.c presence_json.c presence_web.c
EMU := emu_main.c emu_display.c emu_touch.c emu_http.c emu_nvs.c emu_stubs.c emu_tasks.c emu_partition.c emu_audio.c \
       emu_web.c emu_imu.c
LV_SRC := $(shell find $(LVGL)/src -name '*.c')

MINIZ := $(OUT)/miniz
CFLAGS := -O2 -DEMU_BUILD '-DEMU_VERSION="$(VERSION)"' -I. -Ishim -I$(MAIN) -I$(FORGE)/forge_core/include \
          -I$(FORGE)/forge_net/include -I$(FORGE)/forge_ota/include -I$(FORGE)/forge_presence/include \
          -I$(LVGL) -I$(LVGL)/src -I$(CJSON) -I$(MINIZ) \
          -include string.h -include stdint.h '-DLV_CONF_KCONFIG_EXTERNAL_INCLUDE="lv_kconfig.h"' \
          -DLV_LVGL_H_INCLUDE_SIMPLE -Wno-unused-parameter
LDFLAGS := -sASYNCIFY -sASYNCIFY_STACK_SIZE=65536 -sALLOW_MEMORY_GROWTH -sSTACK_SIZE=262144 -sENVIRONMENT=web \
           -sEXPORTED_FUNCTIONS=_main,_emu_fb,_emu_fb_dirty,_emu_touch,_emu_brightness,_emu_mic,_malloc,_free \
           -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU16 -sMODULARIZE -sEXPORT_NAME=WeatherEmu

OBJ := $(patsubst %.c,$(OUT)/fw/%.o,$(FIRMWARE)) $(patsubst %.c,$(OUT)/forge/%.o,$(FORGE_SRC)) \
       $(patsubst %.c,$(OUT)/forge_ota/%.o,$(FORGE_OTA_SRC)) $(patsubst %.c,$(OUT)/forge_presence/%.o,$(FORGE_PRESENCE_SRC)) \
       $(patsubst %.c,$(OUT)/emu/%.o,$(EMU)) \
       $(patsubst $(LVGL)/%.c,$(OUT)/lvgl/%.o,$(LV_SRC)) $(OUT)/cjson/cJSON.o $(OUT)/emu/fonts.o $(OUT)/miniz/miniz.o

all: $(OUT)/emu.js $(OUT)/index.html $(OUT)/settings.html $(OUT)/emu-settings.js

$(OUT)/emu.js: $(OBJ)
	emcc $(CFLAGS) $(LDFLAGS) $(OBJ) -o $@
	@ls -l $(OUT)/emu.js $(OUT)/emu.wasm

$(OUT)/index.html: index.html
	cp $< $@

# The display's own settings page under the emulator: unchanged, with emu-settings.js first in its <head> (its requests
# to /api/ go to the emulator, emu_web.c)
$(OUT)/settings.html: $(MAIN)/web/index.html
	sed 's|<head>|<head><script src="emu-settings.js"></script>|' $< > $@   # (one <head>; macOS's sed has no 0,/re/)
	@grep -q 'emu-settings.js' $@ || (echo "settings.html: no <head> to put emu-settings.js in" && rm -f $@ && false)
$(OUT)/emu-settings.js: emu-settings.js
	cp $< $@

$(OUT)/fw/%.o: $(MAIN)/%.c lv_kconfig.h | $(MINIZ)/miniz.h
	@mkdir -p $(dir $@)
	emcc $(CFLAGS) -c $< -o $@
$(OUT)/forge/%.o: $(FORGE)/forge_core/%.c lv_kconfig.h | $(MINIZ)/miniz.h
	@mkdir -p $(dir $@)
	emcc $(CFLAGS) -c $< -o $@
$(OUT)/forge_ota/%.o: $(FORGE)/forge_ota/%.c lv_kconfig.h
	@mkdir -p $(dir $@)
	emcc $(CFLAGS) -c $< -o $@
$(OUT)/forge_presence/%.o: $(FORGE)/forge_presence/%.c
	@mkdir -p $(dir $@)
	emcc $(CFLAGS) -c $< -o $@
$(OUT)/emu/%.o: %.c lv_kconfig.h
	@mkdir -p $(dir $@)
	emcc $(CFLAGS) -c $< -o $@
$(OUT)/emu/fonts.o: $(OUT)/fonts.c
	emcc $(CFLAGS) -c $< -o $@
$(OUT)/lvgl/%.o: $(LVGL)/%.c lv_kconfig.h
	@mkdir -p $(dir $@)
	@emcc $(CFLAGS) -c $< -o $@
$(OUT)/cjson/cJSON.o: $(CJSON)/cJSON.c
	@mkdir -p $(dir $@)
	emcc $(CFLAGS) -c $< -o $@

# miniz 3.0.2 (MIT): its tinfl is what png_rows.c finds in the ESP32-S3's ROM (as tests/host). Downloaded once.
$(MINIZ)/miniz.c: $(MINIZ)/miniz.h
$(MINIZ)/miniz.h:
	@mkdir -p $(MINIZ)
	curl -fsSL -o $(OUT)/miniz.zip https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
	python3 -c "import zipfile; zipfile.ZipFile('$(OUT)/miniz.zip').extractall('$(MINIZ)', ['miniz.c', 'miniz.h'])"
$(OUT)/miniz/miniz.o: $(MINIZ)/miniz.c
	emcc $(CFLAGS) -c $< -o $@

# The two TTF fonts the firmware embeds, as arrays (ui.c, EMU_BUILD)
$(OUT)/fonts.c: $(MAIN)/montserrat.ttf $(MAIN)/syllabics.ttf
	@mkdir -p $(OUT)
	python3 -c "import sys; \
f = lambda n, p: 'const unsigned char %s_start[] = {%s};\nconst unsigned char *const %s_end = %s_start + %d;\n' % (n, ','.join(str(b) for b in open(p, 'rb').read()), n, n, len(open(p, 'rb').read())); \
open('$@', 'w').write(f('ttf', '$(MAIN)/montserrat.ttf') + f('syl', '$(MAIN)/syllabics.ttf'))"

clean:
	rm -rf $(OUT)
.PHONY: all clean
