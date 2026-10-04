// esp_http_client for the browser: a request is a fetch(), awaited through ASYNCIFY, so the firmware's own forecast,
// air-quality and alerts code (weather.c, alerts.c) runs unchanged. The services all allow cross-origin requests.
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>
#include "esp_http_client.h"

struct esp_http_client {
    char *url;
    http_event_handle_cb handler;
    void *user;
    int status;
};

// The body in a malloc'ed buffer (NULL: no answer); *status = HTTP status (-1: no answer), *len = its length
EM_ASYNC_JS(uint8_t *, js_fetch, (const char *url, int *status, int *len), {
    try {
        const r = await fetch(UTF8ToString(url));
        const b = new Uint8Array(await r.arrayBuffer());
        const p = _malloc(b.length + 1);
        HEAPU8.set(b, p);
        HEAPU8[p + b.length] = 0;
        setValue(status, r.status, 'i32');
        setValue(len, b.length, 'i32');
        return p;
    } catch (e) {
        console.warn('fetch failed', UTF8ToString(url), e);
        setValue(status, -1, 'i32');
        setValue(len, 0, 'i32');
        return 0;
    }
});

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg)
{
    struct esp_http_client *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->url = strdup(cfg->url);
    c->handler = cfg->event_handler;
    c->user = cfg->user_data;
    return c;
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t c)
{
    int status = -1, len = 0;
    uint8_t *body = js_fetch(c->url, &status, &len);
    c->status = status;
    if (!body) return ESP_ERR_HTTP_CONNECT;
    esp_http_client_event_t e = { .client = c, .user_data = c->user };
    if (c->handler) {
        e.event_id = HTTP_EVENT_ON_CONNECTED;
        c->handler(&e);
        for (int o = 0; o < len; o += 4096) {             // in pieces, as the firmware receives them
            e.event_id = HTTP_EVENT_ON_DATA;
            e.data = body + o;
            e.data_len = len - o < 4096 ? len - o : 4096;
            c->handler(&e);
        }
        e.event_id = HTTP_EVENT_ON_FINISH;
        e.data = NULL;
        e.data_len = 0;
        c->handler(&e);
    }
    free(body);
    return ESP_OK;
}

int esp_http_client_get_status_code(esp_http_client_handle_t c) { return c->status; }

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c)
{
    if (!c) return ESP_OK;
    free(c->url);
    free(c);
    return ESP_OK;
}

esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char *url)
{
    free(c->url);
    c->url = strdup(url);
    return ESP_OK;
}

esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t c, void *data) { c->user = data; return ESP_OK; }

esp_err_t esp_crt_bundle_attach(void *conf) { (void)conf; return ESP_OK; }
