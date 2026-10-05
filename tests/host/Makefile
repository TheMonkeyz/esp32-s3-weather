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
OUT := build
CFLAGS := -std=gnu11 -D_DEFAULT_SOURCE -g -O1 -Wall -Wno-unused-function -fsanitize=address,undefined \
          -fno-sanitize-recover=undefined -Ishim -I$(MAIN) -I$(CJSON) -include shim/host.h
COMMON := fake.c $(CJSON)/cJSON.c
TESTS := weather alerts utf8 i18n version png textfit

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
$(OUT)/test_png: test_png.c $(MAIN)/png_rows.c $(MINIZ)/miniz.c | $(OUT)
	$(CC) $(CFLAGS) -I$(MINIZ) -o $@ test_png.c $(MAIN)/png_rows.c $(MINIZ)/miniz.c

$(OUT):
	mkdir -p $(OUT)
clean:
	rm -rf $(OUT)
.PHONY: all clean
$(OUT)/test_utf8: test_utf8.c $(MAIN)/utf8.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_utf8.c fake.c $(CJSON)/cJSON.c -lm
$(OUT)/test_i18n: test_i18n.c $(MAIN)/i18n.c $(MAIN)/i18n_strings.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_i18n.c fake.c $(CJSON)/cJSON.c -lm
$(OUT)/test_version: test_version.c $(MAIN)/version.c $(MAIN)/sound.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_version.c $(MAIN)/version.c
$(OUT)/test_textfit: test_textfit.c $(MAIN)/textfit.c $(MAIN)/textfit.h $(MAIN)/montserrat.ttf $(MAIN)/syllabics.ttf | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_textfit.c $(MAIN)/textfit.c
