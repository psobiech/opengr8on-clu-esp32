/**
 * @file clu_ota.c
 * @brief Password-protected firmware update over HTTP (POST /ota) and rollback confirmation.
 */
#include "clu_ota.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_page.h"

static const char *TAG = "clu_ota";

#define OTA_PASS_MIN 8
#define OTA_PASS_MAX 64
#define OTA_BUF 4096
#define OTA_FAIL_DELAY_MS 2000   // per wrong password: one request at a time -> ~0.5 guesses / s
#define OTA_REBOOT_DELAY_MS 500  // lets the response leave before the restart
#define OTA_RECV_RETRIES 3       // consecutive receive timeouts tolerated
#define OTA_LOG_STEP (256 * 1024)

static char s_pass[OTA_PASS_MAX + 1];
static char s_origin[128]; // CORS: the one origin allowed to call POST /ota from a browser, "" = none

/** @brief Value of one base64 character, -1 when not in the alphabet. */
static int b64_val(char character)
{
    if (character >= 'A' && character <= 'Z') return character - 'A';
    if (character >= 'a' && character <= 'z') return character - 'a' + 26;
    if (character >= '0' && character <= '9') return character - '0' + 52;
    if (character == '+') return 62;
    if (character == '/') return 63;
    return -1;
}

/**
 * @brief Decode base64 (padding optional, nothing after it).
 * @return Decoded length, -1 when malformed or longer than @p cap.
 */
static int b64_decode(const char *in, char *out, size_t cap)
{
    size_t decoded_length = 0;
    uint32_t acc = 0;
    int bits = 0, pad = 0;
    for (; *in; in++) {
        if (*in == '=') {
            pad++;
            continue;
        }
        int digit_value = b64_val(*in);
        if (digit_value < 0 || pad) {
            return -1;
        }
        acc = acc << 6 | (uint32_t)digit_value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (decoded_length >= cap) {
                return -1;
            }
            out[decoded_length++] = (char)(acc >> bits);
        }
    }
    return pad > 2 ? -1 : (int)decoded_length;
}

/** @brief Compare with the configured password; time depends only on its length. */
static bool pass_ok(const char *got, size_t len)
{
    size_t want = strlen(s_pass);
    unsigned diff = len != want;
    for (size_t i = 0; i < want; i++) {
        diff |= (uint8_t)s_pass[i] ^ (uint8_t)(i < len ? got[i] : 0);
    }
    return diff == 0;
}

/** @brief "Authorization: Basic base64(user:password)" with the right password (user ignored). */
static bool authorized(httpd_req_t *req)
{
    char hdr[192];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof hdr) != ESP_OK ||
        strncasecmp(hdr, "Basic ", 6) != 0) {
        return false;
    }
    char cred[136];
    int credential_length = b64_decode(hdr + 6, cred, sizeof cred);
    const char *colon = credential_length > 0 ? memchr(cred, ':', (size_t)credential_length) : NULL;
    if (!colon) {
        return false;
    }
    colon++;
    return pass_ok(colon, (size_t)(cred + credential_length - colon));
}

/** @brief CORS: let the configured origin read the response. */
static void cors(httpd_req_t *req)
{
    if (s_origin[0]) {
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", s_origin);
        httpd_resp_set_hdr(req, "Vary", "Origin");
    }
}

/** @brief Send a short plain-text response with the given status line. */
static esp_err_t reply(httpd_req_t *req, const char *status, const char *msg)
{
    cors(req);
    httpd_resp_set_status(req, status);
    httpd_resp_sendstr(req, msg);
    return ESP_FAIL; // errors close the connection (any unread body is dropped with it)
}

/** @brief Receive the body into the OTA handle. */
static esp_err_t receive(httpd_req_t *req, esp_ota_handle_t ota_handle)
{
    char *buf = malloc(OTA_BUF);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    size_t left = req->content_len, next_log = OTA_LOG_STEP;
    int timeouts = 0;
    while (left > 0 && err == ESP_OK) {
        int received_count = httpd_req_recv(req, buf, left < OTA_BUF ? left : OTA_BUF);
        if (received_count == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < OTA_RECV_RETRIES) {
            continue;
        }
        if (received_count <= 0) {
            ESP_LOGE(TAG, "upload interrupted, %u bytes missing", (unsigned)left);
            err = ESP_ERR_TIMEOUT;
            break;
        }
        timeouts = 0;
        err = esp_ota_write(ota_handle, buf, (size_t)received_count);
        left -= (size_t)received_count;
        if (req->content_len - left >= next_log) {
            ESP_LOGI(TAG, "%u / %u bytes", (unsigned)(req->content_len - left), (unsigned)req->content_len);
            next_log += OTA_LOG_STEP;
        }
    }
    free(buf);
    return err;
}

/** @brief POST /ota: authenticate, write the image to the next slot, verify, boot it. */
static esp_err_t ota_post(httpd_req_t *req)
{
    if (!authorized(req)) {
        ESP_LOGW(TAG, "update refused: wrong or missing password");
        vTaskDelay(pdMS_TO_TICKS(OTA_FAIL_DELAY_MS));
        // no WWW-Authenticate: browsers would pop up their own login dialog over the page's
        return reply(req, "401 Unauthorized", "wrong or missing password\n");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return reply(req, "500 Internal Server Error", "no OTA partition\n");
    }
    if (req->content_len == 0) {
        return reply(req, "411 Length Required", "send the image as the request body (Content-Length)\n");
    }
    if (req->content_len > part->size) {
        return reply(req, "413 Content Too Large", "image larger than the OTA partition\n");
    }
    ESP_LOGW(TAG, "update: %u bytes -> %s", (unsigned)req->content_len, part->label);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return reply(req, "500 Internal Server Error", "cannot start the update\n");
    }
    err = receive(req, ota_handle);
    if (err != ESP_OK) {
        esp_ota_abort(ota_handle);
        ESP_LOGE(TAG, "update failed: %s", esp_err_to_name(err));
        return reply(req, "500 Internal Server Error", "upload / flash write failed\n");
    }
    err = esp_ota_end(ota_handle); // verifies header, chip and SHA-256
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "image rejected: %s", esp_err_to_name(err));
        return reply(req, "400 Bad Request", "not a valid application image for this chip "
                                             "(use the -ota.bin image, not the full one)\n");
    }
    esp_app_desc_t desc = {0};
    const esp_app_desc_t *run = esp_app_get_description();
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK ||
        strncmp(desc.project_name, run->project_name, sizeof desc.project_name) != 0) {
        ESP_LOGE(TAG, "image rejected: project \"%.32s\", not \"%.32s\"", desc.project_name, run->project_name);
        return reply(req, "400 Bad Request", "not OpenGr8on CLU firmware\n");
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return reply(req, "500 Internal Server Error", "cannot select the new image\n");
    }
    ESP_LOGW(TAG, "firmware %.32s written to %s, restarting", desc.version, part->label);
    cors(req);
    httpd_resp_sendstr(req, "OK, restarting\n");
    vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
    esp_restart();
    return ESP_OK;
}

/**
 * @brief OPTIONS /ota: CORS preflight (Authorization and Content-Type are not "simple"). Also
 * answers Chrome's local / private network access check for pages served over HTTPS.
 */
static esp_err_t ota_options(httpd_req_t *req)
{
    cors(req);
    if (s_origin[0]) {
        httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization, Content-Type");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
        httpd_resp_set_hdr(req, "Access-Control-Max-Age", "600");
    }
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/** @brief GET /: the upload page (ota_page.h) with the running firmware version. */
static esp_err_t ota_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send_chunk(req, OTA_PAGE_HEAD, HTTPD_RESP_USE_STRLEN);
    const esp_app_desc_t *app_description = esp_app_get_description();
    char ver[sizeof app_description->version + 1];
    size_t version_length = 0;
    for (size_t i = 0; i < sizeof app_description->version && app_description->version[i]; i++) {
        char version_character = app_description->version[i];
        ver[version_length++] = version_character == '<' || version_character == '>' || version_character == '&' || version_character == '"' ? '?' : version_character; // git describe never has these
    }
    ver[version_length] = 0;
    httpd_resp_send_chunk(req, ver, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, OTA_PAGE_TAIL, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
}

bool clu_ota_start(const clu_ota_cfg_t *cfg)
{
    const char *ota_password = cfg->password ? cfg->password : "";
    size_t password_length = strlen(ota_password);
    if (!password_length) {
        ESP_LOGI(TAG, "no OTA password: network updates off");
        return false;
    }
    if (password_length < OTA_PASS_MIN || password_length > OTA_PASS_MAX) {
        ESP_LOGE(TAG, "OTA password must be %d..%d characters: network updates off", OTA_PASS_MIN, OTA_PASS_MAX);
        return false;
    }
    memcpy(s_pass, ota_password, password_length + 1);
    const char *configured_origin = cfg->cors_origin ? cfg->cors_origin : "";
    if (strlen(configured_origin) >= sizeof s_origin) {
        ESP_LOGE(TAG, "CORS origin too long, ignored");
        configured_origin = "";
    }
    strcpy(s_origin, configured_origin);

    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.server_port = cfg->port;
    server_config.max_open_sockets = 2;
    server_config.max_uri_handlers = 3;
    server_config.lru_purge_enable = true;
    server_config.recv_wait_timeout = 10;
    server_config.stack_size = 6144;
    httpd_handle_t srv;
    esp_err_t err = httpd_start(&srv, &server_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server on port %u: %s", (unsigned)cfg->port, esp_err_to_name(err));
        return false;
    }
    static const httpd_uri_t uris[] = {
        {.uri = "/ota", .method = HTTP_POST, .handler = ota_post},
        {.uri = "/ota", .method = HTTP_OPTIONS, .handler = ota_options},
        {.uri = "/", .method = HTTP_GET, .handler = ota_page},
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) {
        httpd_register_uri_handler(srv, &uris[i]);
    }
    ESP_LOGI(TAG, "network updates on: http://<CLU IP>:%u/ (page), POST /ota", (unsigned)cfg->port);
    return true;
}

void clu_ota_confirm(void)
{
    esp_ota_img_states_t image_state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &image_state) == ESP_OK &&
        image_state == ESP_OTA_IMG_PENDING_VERIFY && esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGW(TAG, "new firmware confirmed (network up): no rollback");
    }
}
