#pragma once
// Browser emulator: the radar's map cache partition, in memory (web/emu/emu_partition.c)
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef enum { ESP_PARTITION_TYPE_APP = 0, ESP_PARTITION_TYPE_DATA = 1 } esp_partition_type_t;
typedef struct { esp_partition_type_t type; int subtype; uint32_t address, size; char label[17]; } esp_partition_t;
const esp_partition_t *esp_partition_find_first(esp_partition_type_t type, int subtype, const char *label);
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t n);
esp_err_t esp_partition_write(const esp_partition_t *p, size_t off, const void *src, size_t n);
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t n);
