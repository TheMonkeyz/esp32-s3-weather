#pragma once
// Browser emulator: one "partition", for the status page
#include "esp_err.h"
typedef struct { char label[17]; } esp_partition_t;
static inline const esp_partition_t *esp_ota_get_running_partition(void)
{
    static const esp_partition_t p = { "browser" };
    return &p;
}
