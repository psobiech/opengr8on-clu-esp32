// In-memory NVS for host tests (include once per test binary). Entries are keyed by
// namespace + key; nvs_fail_open makes nvs_open() fail.
#pragma once
#include <string.h>
#include "nvs.h"

typedef struct {
    char ns[16], key[16];
    char type; // 'u' u32, 'i' i32, 's' str, 'b' blob
    uint8_t val[512];
    size_t len;
} nvs_entry_t;
static nvs_entry_t nvs_db[64];
static int nvs_n, nvs_commits, nvs_fail_open;
static char nvs_open_ns[8][16]; // namespace per handle (1-based)
static int nvs_nopen;

static void nvs_reset(void)
{
    nvs_n = nvs_commits = nvs_fail_open = nvs_nopen = 0;
}

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h)
{
    (void)mode;
    if (nvs_fail_open) {
        return ESP_FAIL;
    }
    int i = nvs_nopen++ % 8;
    strncpy(nvs_open_ns[i], ns, sizeof nvs_open_ns[i] - 1);
    *h = (nvs_handle_t)(i + 1);
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; nvs_commits++; return ESP_OK; }

static nvs_entry_t *nvs_find(const char *ns, const char *k, bool create)
{
    for (int i = 0; i < nvs_n; i++) {
        if (!strcmp(nvs_db[i].ns, ns) && !strcmp(nvs_db[i].key, k)) {
            return &nvs_db[i];
        }
    }
    if (!create || nvs_n == 64) {
        return NULL;
    }
    nvs_entry_t *e = &nvs_db[nvs_n++];
    memset(e, 0, sizeof *e);
    strncpy(e->ns, ns, sizeof e->ns - 1);
    strncpy(e->key, k, sizeof e->key - 1);
    return e;
}
static esp_err_t nvs_put_raw(nvs_handle_t h, const char *k, char type, const void *v, size_t len)
{
    nvs_entry_t *e = nvs_find(nvs_open_ns[h - 1], k, true);
    e->type = type;
    e->len = len;
    memcpy(e->val, v, len);
    return ESP_OK;
}
static esp_err_t nvs_get_raw(nvs_handle_t h, const char *k, char type, void *v, size_t *len)
{
    nvs_entry_t *e = nvs_find(nvs_open_ns[h - 1], k, false);
    if (!e || e->type != type) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (!v && len) { // length query, as in ESP-IDF
        *len = e->len;
        return ESP_OK;
    }
    if (len) {
        if (*len < e->len) {
            return ESP_FAIL;
        }
        *len = e->len;
    }
    memcpy(v, e->val, e->len);
    return ESP_OK;
}
esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t v) { return nvs_put_raw(h, k, 'u', &v, 4); }
esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *v) { return nvs_get_raw(h, k, 'u', v, NULL); }
esp_err_t nvs_set_i32(nvs_handle_t h, const char *k, int32_t v) { return nvs_put_raw(h, k, 'i', &v, 4); }
esp_err_t nvs_get_i32(nvs_handle_t h, const char *k, int32_t *v) { return nvs_get_raw(h, k, 'i', v, NULL); }
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v) { return nvs_put_raw(h, k, 's', v, strlen(v) + 1); }
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *v, size_t *len) { return nvs_get_raw(h, k, 's', v, len); }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len) { return nvs_put_raw(h, k, 'b', v, len); }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *len) { return nvs_get_raw(h, k, 'b', v, len); }

// Read back for checks; NULL if absent.
static const nvs_entry_t *nvs_peek(const char *ns, const char *k) { return nvs_find(ns, k, false); }
