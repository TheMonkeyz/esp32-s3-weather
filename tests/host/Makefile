# Host unit tests: firmware C files built with gcc against small shims of the ESP-IDF headers (shim/) and a scripted
# HTTP client (fake.c), with AddressSanitizer. Linux or WSL:   make -C tests/host
# IDF_PATH: an ESP-IDF checkout (for cJSON); on the Windows PC through WSL it is /mnt/c/Espressif/esp-idf.
IDF_PATH ?= /mnt/c/Espressif/esp-idf
CJSON := $(IDF_PATH)/components/json/cJSON
MAIN := ../../main
OUT := build
CFLAGS := -std=gnu11 -D_DEFAULT_SOURCE -g -O1 -Wall -Wno-unused-function -fsanitize=address,undefined \
          -fno-sanitize-recover=undefined -Ishim -I$(MAIN) -I$(CJSON) -include shim/host.h
COMMON := fake.c $(CJSON)/cJSON.c
TESTS := weather alerts utf8 i18n version

all: $(TESTS:%=$(OUT)/test_%)
	@set -e; for t in $(TESTS); do ./$(OUT)/test_$$t; done

$(OUT)/test_weather: test_weather.c $(MAIN)/weather.c $(COMMON) | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_weather.c $(MAIN)/weather.c $(COMMON) -lm
$(OUT)/test_alerts: test_alerts.c $(MAIN)/alerts.c $(COMMON) | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_alerts.c $(COMMON) -lm

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
