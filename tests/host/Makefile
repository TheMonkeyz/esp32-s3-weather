# Host unit tests: firmware C files built with gcc against small shims of the ESP-IDF headers (shim/) and a scripted
# HTTP client (fake.c), with AddressSanitizer. Linux or WSL:   make -C tests/host
# IDF_PATH: an ESP-IDF checkout (for cJSON); on the Windows PC through WSL it is /mnt/c/Espressif/esp-idf. On macOS
# ESP-IDF's export.sh sets it (docs/MACOS.md); Apple's clang builds these as gcc does.
IDF_PATH ?= /mnt/c/Espressif/esp-idf
CJSON ?= $(IDF_PATH)/components/json/cJSON
ifeq ($(wildcard $(CJSON)/cJSON.c),)
$(error no cJSON at $(CJSON): set IDF_PATH (source ESP-IDF's export.sh) or CJSON=<folder with cJSON.c>)
endif
MAIN := ../../main
# espforge's components (version, png_rows, textfit, the i18n core; svc.h, http_once.h, utf8.h): the firmware's copy
# at the tag in main/idf_component.yml (managed_components after an idf.py build), else tools/fetch_forge.py's
FORGE ?= $(if $(wildcard ../../managed_components/forge_core/CMakeLists.txt),../../managed_components,../../.espforge/components)
ifeq ($(wildcard $(FORGE)/forge_core/i18n.c),)
$(error no espforge components at $(FORGE): build the firmware once, or python tools/fetch_forge.py, or FORGE=<espforge>/components)
endif
CORE := $(FORGE)/forge_core
OUT := build
CFLAGS := -std=gnu11 -D_DEFAULT_SOURCE -g -O1 -Wall -Wno-unused-function -fsanitize=address,undefined \
          -fno-sanitize-recover=undefined -Ishim -I$(MAIN) -I$(CORE)/include -I$(FORGE)/forge_net/include \
          -I$(CJSON) -include shim/host.h
COMMON := fake.c $(CJSON)/cJSON.c
TESTS := weather alerts utf8 i18n version png textfit presence_blob

all: $(TESTS:%=$(OUT)/test_%)
	@set -e; for t in $(TESTS); do ./$(OUT)/test_$$t; done

$(OUT)/test_weather: test_weather.c $(MAIN)/weather.c $(COMMON) | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_weather.c $(MAIN)/weather.c $(COMMON) -lm
$(OUT)/test_alerts: test_alerts.c $(MAIN)/alerts.c $(COMMON) | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_alerts.c $(COMMON) -lm

# miniz 3.0.2 (MIT) for test_png: its inflate is the ROM's tinfl, its deflate builds the test images. Downloaded once.
MINIZ := $(OUT)/miniz
$(MINIZ)/miniz.c: | $(OUT)
	mkdir -p $(MINIZ)
	curl -fsSL -o $(OUT)/miniz.zip https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
	python3 -c "import zipfile; zipfile.ZipFile('$(OUT)/miniz.zip').extractall('$(MINIZ)', ['miniz.c', 'miniz.h'])"
$(OUT)/test_png: test_png.c $(CORE)/png_rows.c $(MINIZ)/miniz.c | $(OUT)
	$(CC) $(CFLAGS) -I$(MINIZ) -o $@ test_png.c $(CORE)/png_rows.c $(MINIZ)/miniz.c

$(OUT):
	mkdir -p $(OUT)
clean:
	rm -rf $(OUT)
.PHONY: all clean
$(OUT)/test_utf8: test_utf8.c $(CORE)/include/utf8.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_utf8.c fake.c $(CJSON)/cJSON.c -lm
$(OUT)/test_i18n: test_i18n.c $(MAIN)/i18n.c $(MAIN)/i18n_strings.h $(CORE)/i18n.c | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_i18n.c $(CORE)/i18n.c fake.c $(CJSON)/cJSON.c -lm
$(OUT)/test_version: test_version.c $(CORE)/version.c $(MAIN)/sound.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_version.c $(CORE)/version.c
# forge_presence's import of the old presence.c settings blob (its presence_sm.c is pure C)
$(OUT)/test_presence_blob: test_presence_blob.c $(FORGE)/forge_presence/presence_sm.c | $(OUT)
	$(CC) $(CFLAGS) -I$(FORGE)/forge_presence/include -o $@ test_presence_blob.c $(FORGE)/forge_presence/presence_sm.c -lm
$(OUT)/test_textfit: test_textfit.c $(CORE)/textfit.c $(MAIN)/montserrat.ttf $(MAIN)/syllabics.ttf | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_textfit.c $(CORE)/textfit.c
