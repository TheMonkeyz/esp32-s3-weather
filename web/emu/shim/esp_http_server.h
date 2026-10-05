#pragma once
// Browser emulator: only the types forge_net's web.h names (ui.c includes it for web_key); no server here
#include "esp_err.h"
typedef struct httpd_req httpd_req_t;
typedef enum { HTTP_GET = 1, HTTP_POST = 3 } httpd_method_t;
