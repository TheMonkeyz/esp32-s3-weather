// The radar's map cache ("mapcache", 4 MB of flash on the display), in memory: maps are kept for the visit
#include <stdlib.h>
#include <string.h>
#include "esp_partition.h"

static esp_partition_t part = { ESP_PARTITION_TYPE_DATA, 0x40, 0x620000, 4 << 20, "mapcache" };
static uint8_t *mem;

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type, int subtype, const char *label)
{
    if (type != ESP_PARTITION_TYPE_DATA || subtype != 0x40 || (label && strcmp(label, "mapcache"))) return NULL;
    if (!mem && (mem = malloc(part.size))) memset(mem, 0xFF, part.size);
    return mem ? &part : NULL;
}
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t n)
{
    if (off + n > p->size) return ESP_FAIL;
    memcpy(dst, mem + off, n);
    return ESP_OK;
}
esp_err_t esp_partition_write(const esp_partition_t *p, size_t off, const void *src, size_t n)
{
    if (off + n > p->size) return ESP_FAIL;
    memcpy(mem + off, src, n);
    return ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t n)
{
    if (off + n > p->size) return ESP_FAIL;
    memset(mem + off, 0xFF, n);
    return ESP_OK;
}
