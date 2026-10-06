#pragma once
// Browser emulator: the firmware's version (routes.c's /api/config) is the emulator's build (EMU_VERSION, web/emu/emu_web.c)
typedef struct { char version[32]; char project_name[32]; } esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
