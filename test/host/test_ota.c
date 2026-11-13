// Host tests for network updates (clu_ota.c, included: auth, base64 and the POST handler are
// static). The HTTP server and the OTA API are fakes: a request is an Authorization header plus a
// body delivered in scripted chunks / timeouts; flash writes are collected in a buffer.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../components/clu_ota/clu_ota.c"

static int failures;

static void check(const char *name, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", name);
    failures += !cond;
}

// ---------------------------------------------------------------- fake HTTP server
static httpd_config_t s_hc;
static esp_err_t s_start_rc = ESP_OK;
static httpd_uri_t s_uris[4], s_uri; // all registered; s_uri = POST /ota
static int s_nuris;
esp_err_t httpd_start(httpd_handle_t *h, const httpd_config_t *c)
{
    s_hc = *c;
    *h = (httpd_handle_t)1;
    return s_start_rc;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *u)
{
    s_uris[s_nuris++] = *u;
    if (u->method == HTTP_POST) s_uri = *u;
    return ESP_OK;
}

static const char *s_auth;      // Authorization header, NULL = absent
static const char *s_body;      // request body
static size_t s_body_pos;
static int s_script[16], s_nscript; // per recv call: >0 = max bytes, HTTPD_SOCK_ERR_* = error
static int s_recv_calls;
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t len)
{
    if (!s_auth || strcmp(field, "Authorization")) {
        return ESP_FAIL;
    }
    if (strlen(s_auth) >= len) {
        return ESP_ERR_INVALID_ARG; // ESP_ERR_HTTPD_RESULT_TRUNC
    }
    strcpy(val, s_auth);
    return ESP_OK;
}
int httpd_req_recv(httpd_req_t *r, char *buf, size_t len)
{
    int step = s_recv_calls < s_nscript ? s_script[s_recv_calls] : (int)len;
    s_recv_calls++;
    if (step <= 0) {
        return step;
    }
    size_t avail = strlen(s_body) - s_body_pos;
    size_t n = (size_t)step < len ? (size_t)step : len;
    n = n < avail ? n : avail;
    memcpy(buf, s_body + s_body_pos, n);
    s_body_pos += n;
    return (int)n;
}
static char s_status[64], s_resp[256], s_www[64], s_acao[128], s_hdrs[512], s_type[64];
static char s_page[8192];
static int s_chunks_end;
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *st)
{
    strcpy(s_status, st);
    return ESP_OK;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *f, const char *v)
{
    if (!strcmp(f, "WWW-Authenticate")) strcpy(s_www, v);
    if (!strcmp(f, "Access-Control-Allow-Origin")) strcpy(s_acao, v);
    strcat(s_hdrs, f);
    strcat(s_hdrs, ";");
    return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *r, const char *b, ssize_t n)
{
    if (b) strncat(s_resp, b, (size_t)n);
    return ESP_OK;
}
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *b, ssize_t n)
{
    if (!b) {
        s_chunks_end++;
        return ESP_OK;
    }
    strncat(s_page, b, n == HTTPD_RESP_USE_STRLEN ? strlen(b) : (size_t)n);
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *t)
{
    strcpy(s_type, t);
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s)
{
    strcat(s_resp, s);
    return ESP_OK;
}

// ---------------------------------------------------------------- fake OTA / system
static esp_partition_t s_part = {.label = "ota_1", .size = 64};
static const esp_partition_t s_running = {.label = "ota_0", .size = 64};
static bool s_have_part = true;
static esp_err_t s_begin_rc, s_write_rc, s_end_rc, s_desc_rc, s_boot_rc, s_state_rc, s_valid_rc;
static char s_flash[256];
static size_t s_flash_len;
static int s_aborts, s_restarts, s_valid_calls;
static uint32_t s_delayed;
static esp_ota_img_states_t s_state = ESP_OTA_IMG_VALID;
static esp_app_desc_t s_new_desc = {.version = "2.0.0", .project_name = "opengr8on_clu_esp32"};
static esp_app_desc_t s_run_desc = {.version = "1.0.0", .project_name = "opengr8on_clu_esp32"};
static const esp_partition_t *s_boot;

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *s) { return s_have_part ? &s_part : NULL; }
const esp_partition_t *esp_ota_get_running_partition(void) { return &s_running; }
esp_err_t esp_ota_begin(const esp_partition_t *p, size_t size, esp_ota_handle_t *h)
{
    s_flash_len = 0;
    *h = 7;
    return s_begin_rc;
}
esp_err_t esp_ota_write(esp_ota_handle_t h, const void *d, size_t n)
{
    if (s_write_rc == ESP_OK) {
        if (s_flash_len + n <= sizeof s_flash) memcpy(s_flash + s_flash_len, d, n); // big images: counted only
        s_flash_len += n;
    }
    return s_write_rc;
}
esp_err_t esp_ota_end(esp_ota_handle_t h) { return s_end_rc; }
esp_err_t esp_ota_abort(esp_ota_handle_t h) { s_aborts++; return ESP_OK; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p)
{
    if (s_boot_rc == ESP_OK) s_boot = p;
    return s_boot_rc;
}
esp_err_t esp_ota_get_partition_description(const esp_partition_t *p, esp_app_desc_t *d)
{
    if (s_desc_rc == ESP_OK) *d = s_new_desc;
    return s_desc_rc;
}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *st)
{
    *st = s_state;
    return p == &s_running ? s_state_rc : ESP_FAIL;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) { s_valid_calls++; return s_valid_rc; }
const esp_app_desc_t *esp_app_get_description(void) { return &s_run_desc; }
void vTaskDelay(TickType_t t) { s_delayed += t; }
void esp_restart(void) { s_restarts++; }

// ---------------------------------------------------------------- helpers
// "Basic " + base64("ota:secret123")
#define GOOD "Basic b3RhOnNlY3JldDEyMw=="

static esp_err_t post(const char *auth, const char *body, size_t content_len)
{
    s_auth = auth;
    s_body = body;
    s_body_pos = 0;
    s_recv_calls = 0;
    s_status[0] = s_resp[0] = s_www[0] = s_acao[0] = s_hdrs[0] = 0;
    s_delayed = 0;
    s_restarts = s_aborts = 0;
    s_boot = NULL;
    httpd_req_t r = {.content_len = content_len};
    esp_err_t rc = s_uri.handler(&r);
    s_nscript = 0;
    return rc;
}

static bool auth_with(const char *hdr) { return post(hdr, "x", 1) == ESP_OK; }

int main(void)
{
    // ---- start
    check("no password: OTA off, no server", !clu_ota_start(&(clu_ota_cfg_t){.port = 8080}) && !s_uri.handler);
    check("empty password: OTA off", !clu_ota_start(&(clu_ota_cfg_t){.port = 8080, .password = ""}));
    check("password shorter than 8: OTA off", !clu_ota_start(&(clu_ota_cfg_t){.port = 8080, .password = "short"}));
    char longp[66];
    memset(longp, 'p', 65);
    longp[65] = 0;
    check("password longer than 64: OTA off", !clu_ota_start(&(clu_ota_cfg_t){.port = 8080, .password = longp}));
    s_start_rc = ESP_FAIL;
    check("HTTP server failure: OTA off", !clu_ota_start(&(clu_ota_cfg_t){.port = 8080, .password = "secret123"}) &&
                                              !s_uri.handler);
    s_start_rc = ESP_OK;
    char longo[200];
    memset(longo, 'o', 199);
    longo[199] = 0;
    s_nuris = 0;
    check("valid password, CORS origin too long: server on, no CORS",
          clu_ota_start(&(clu_ota_cfg_t){.port = 8080, .password = "secret123", .cors_origin = longo}) &&
              !s_origin[0]);
    s_nuris = 0;
    check("valid password: server on the port, POST /ota + OPTIONS /ota + GET / registered",
          clu_ota_start(&(clu_ota_cfg_t){.port = 8080, .password = "secret123"}) && s_hc.server_port == 8080 &&
              s_hc.max_open_sockets == 2 && s_hc.max_uri_handlers == 3 && s_hc.lru_purge_enable &&
              s_uri.method == HTTP_POST && !strcmp(s_uri.uri, "/ota") && s_uri.handler == ota_post &&
              s_nuris == 3 && s_uris[1].method == HTTP_OPTIONS && s_uris[1].handler == ota_options &&
              s_uris[2].method == HTTP_GET && !strcmp(s_uris[2].uri, "/") && s_uris[2].handler == ota_page);

    // ---- page and preflight
    httpd_req_t gr = {0};
    strcpy(s_run_desc.version, "v1<2>x&y\"z");
    check("GET /: HTML page with the escaped running version, not cached",
          ota_page(&gr) == ESP_OK && !strcmp(s_type, "text/html; charset=utf-8") && strstr(s_hdrs, "Cache-Control;") &&
              strstr(s_page, "Running: <b>v1?2?x?y?z</b>") && strstr(s_page, "fetch") == NULL &&
              strstr(s_page, "'/ota'") && s_chunks_end == 1);
    strcpy(s_run_desc.version, "1.0.0");
    s_hdrs[0] = s_resp[0] = s_status[0] = 0;
    check("OPTIONS without CORS origin: 204, no CORS headers",
          ota_options(&gr) == ESP_OK && !strcmp(s_status, "204 No Content") && !s_hdrs[0]);
    s_nuris = 0;
    clu_ota_start(&(clu_ota_cfg_t){.port = 8080, .password = "secret123", .cors_origin = "https://example.github.io"});
    s_hdrs[0] = s_acao[0] = 0;
    check("OPTIONS with CORS origin: origin, POST, Authorization + Content-Type, private network allowed",
          ota_options(&gr) == ESP_OK && !strcmp(s_acao, "https://example.github.io") &&
              !strcmp(s_hdrs, "Access-Control-Allow-Origin;Vary;Access-Control-Allow-Methods;"
                              "Access-Control-Allow-Headers;Access-Control-Allow-Private-Network;"
                              "Access-Control-Max-Age;"));

    // ---- authentication
    check("no Authorization header: 401 readable cross-origin, no browser login dialog, 2 s penalty, nothing written",
          post(NULL, "abc", 3) == ESP_FAIL && !strcmp(s_status, "401 Unauthorized") && !s_www[0] &&
              !strcmp(s_acao, "https://example.github.io") && s_delayed == OTA_FAIL_DELAY_MS && s_recv_calls == 0);
    check("password check by the page (empty body, right password): 411, nothing written",
          post(GOOD, "", 0) == ESP_FAIL && !strncmp(s_status, "411", 3) && s_recv_calls == 0);
    check("wrong password (ota:secret124): 401", !auth_with("Basic b3RhOnNlY3JldDEyNA=="));
    check("password prefix (ota:secret12): 401", !auth_with("Basic b3RhOnNlY3JldDEy"));
    check("longer password (ota:secret1234): 401", !auth_with("Basic b3RhOnNlY3JldDEyMzQ="));
    check("not Basic (Bearer): 401", !auth_with("Bearer b3RhOnNlY3JldDEyMw=="));
    check("malformed base64: 401", !auth_with("Basic b3Rh*nNlY3JldDEyMw=="));
    check("data after padding: 401", !auth_with("Basic b3Rh=nNl"));
    check("too much padding: 401", !auth_with("Basic b3RhOg==="));
    check("no colon (base64 'secret123'): 401", !auth_with("Basic c2VjcmV0MTIz"));
    check("empty credentials: 401", !auth_with("Basic "));
    char huge[300] = "Basic ";
    memset(huge + 6, 'Q', 250);
    check("header too long for the buffer: 401", !auth_with(huge));
    char big[200] = "Basic ";
    memset(big + 6, 'Q', 184);
    check("credentials longer than the decode buffer: 401", !auth_with(big));
    check("user name ignored, lower-case scheme (basic :secret123): accepted",
          auth_with("basic OnNlY3JldDEyMw==") && s_restarts == 1);

    // ---- upload
    s_nscript = 3;
    s_script[0] = 5;
    s_script[1] = HTTPD_SOCK_ERR_TIMEOUT;
    s_script[2] = 100;
    check("good upload: chunks (timeout retried) written, boot partition set, 200, restart after the reply",
          post(GOOD, "0123456789abcdef", 16) == ESP_OK && s_flash_len == 16 && !memcmp(s_flash, "0123456789abcdef", 16) &&
              s_boot == &s_part && !s_status[0] && !strcmp(s_resp, "OK, restarting\n") && s_restarts == 1 &&
              !strcmp(s_acao, "https://example.github.io") &&
              s_delayed == OTA_REBOOT_DELAY_MS && !s_aborts);

    s_have_part = false;
    check("no OTA partition: 500", post(GOOD, "x", 1) == ESP_FAIL && !strncmp(s_status, "500", 3) && !s_restarts);
    s_have_part = true;
    check("no Content-Length / empty body: 411", post(GOOD, "", 0) == ESP_FAIL && !strncmp(s_status, "411", 3));
    check("image larger than the slot: 413, nothing written",
          post(GOOD, "x", 65) == ESP_FAIL && !strncmp(s_status, "413", 3) && s_recv_calls == 0);

    s_begin_rc = ESP_FAIL;
    check("esp_ota_begin failure: 500", post(GOOD, "x", 1) == ESP_FAIL && !strncmp(s_status, "500", 3) && !s_aborts);
    s_begin_rc = ESP_OK;

    s_nscript = 3;
    s_script[0] = s_script[1] = s_script[2] = HTTPD_SOCK_ERR_TIMEOUT;
    check("three receive timeouts in a row: aborted, 500",
          post(GOOD, "abcd", 4) == ESP_FAIL && s_aborts == 1 && !strncmp(s_status, "500", 3) && !s_boot);
    s_nscript = 2;
    s_script[0] = 2;
    s_script[1] = HTTPD_SOCK_ERR_FAIL;
    check("connection lost mid-upload: aborted", post(GOOD, "abcd", 4) == ESP_FAIL && s_aborts == 1 && !s_restarts);
    check("body shorter than Content-Length (peer closed): aborted",
          post(GOOD, "ab", 4) == ESP_FAIL && s_aborts == 1);

    s_write_rc = ESP_FAIL;
    check("flash write failure: aborted, 500", post(GOOD, "abcd", 4) == ESP_FAIL && s_aborts == 1 && !s_boot);
    s_write_rc = ESP_OK;

    s_end_rc = ESP_ERR_OTA_VALIDATE_FAILED;
    check("image fails verification: 400, not booted",
          post(GOOD, "abcd", 4) == ESP_FAIL && !strncmp(s_status, "400", 3) && strstr(s_resp, "-ota.bin") && !s_boot);
    s_end_rc = ESP_OK;

    strcpy(s_new_desc.project_name, "other_project");
    check("another project's firmware: 400, not booted",
          post(GOOD, "abcd", 4) == ESP_FAIL && !strcmp(s_resp, "not OpenGr8on CLU firmware\n") && !s_boot);
    strcpy(s_new_desc.project_name, "opengr8on_clu_esp32");
    s_desc_rc = ESP_FAIL;
    check("unreadable image description: 400", post(GOOD, "abcd", 4) == ESP_FAIL && !s_boot);
    s_desc_rc = ESP_OK;

    s_boot_rc = ESP_FAIL;
    check("boot partition not set: 500, no restart",
          post(GOOD, "abcd", 4) == ESP_FAIL && !strncmp(s_status, "500", 3) && !s_restarts);
    s_boot_rc = ESP_OK;

    // large upload: many 4 KB buffers, progress logged every 256 KB
    s_part.size = 1 << 20;
    size_t isz = 600 * 1024;
    char *img = malloc(isz + 1);
    memset(img, 'z', isz);
    img[isz] = 0;
    check("600 KB image: received in 4 KB reads, all written, restart",
          post(GOOD, img, isz) == ESP_OK && s_flash_len == isz && s_recv_calls == (int)(isz / OTA_BUF) &&
              s_restarts == 1);
    free(img);

    // ---- rollback confirmation
    s_state = ESP_OTA_IMG_PENDING_VERIFY;
    clu_ota_confirm();
    check("fresh OTA image pending verification: marked valid", s_valid_calls == 1);
    s_state = ESP_OTA_IMG_VALID;
    clu_ota_confirm();
    check("already valid: untouched", s_valid_calls == 1);
    s_state = ESP_OTA_IMG_PENDING_VERIFY;
    s_state_rc = ESP_FAIL;
    clu_ota_confirm();
    check("state unknown (no otadata): untouched", s_valid_calls == 1);
    s_state_rc = ESP_OK;
    s_valid_rc = ESP_FAIL;
    clu_ota_confirm();
    check("mark-valid failure tolerated", s_valid_calls == 2);

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
