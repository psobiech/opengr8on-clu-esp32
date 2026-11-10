// Host stub: the esp-mqtt client API used by clu_mqtt.c, implemented by the test.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct esp_mqtt_client *esp_mqtt_client_handle_t;
#include "esp_event.h"

typedef enum {
    MQTT_EVENT_ANY = -1, MQTT_EVENT_ERROR = 0, MQTT_EVENT_CONNECTED, MQTT_EVENT_DISCONNECTED,
    MQTT_EVENT_SUBSCRIBED, MQTT_EVENT_UNSUBSCRIBED, MQTT_EVENT_PUBLISHED, MQTT_EVENT_DATA,
} esp_mqtt_event_id_t;
typedef enum { MQTT_ERROR_TYPE_NONE, MQTT_ERROR_TYPE_TCP_TRANSPORT, MQTT_ERROR_TYPE_CONNECTION_REFUSED } esp_mqtt_error_type_t;

typedef struct {
    esp_err_t esp_tls_last_esp_err;
    int esp_tls_stack_err;
    esp_mqtt_error_type_t error_type;
    int connect_return_code;
} esp_mqtt_error_codes_t;

typedef struct {
    esp_mqtt_event_id_t event_id;
    esp_mqtt_client_handle_t client;
    char *data;
    int data_len;
    int total_data_len;
    int current_data_offset;
    char *topic;
    int topic_len;
    esp_mqtt_error_codes_t *error_handle;
} esp_mqtt_event_t;
typedef esp_mqtt_event_t *esp_mqtt_event_handle_t;

typedef struct {
    struct {
        struct { const char *uri; } address;
        struct {
            const char *certificate;
            esp_err_t (*crt_bundle_attach)(void *conf);
        } verification;
    } broker;
    struct {
        const char *username;
        const char *client_id;
        struct { const char *password; } authentication;
    } credentials;
    struct {
        struct { const char *topic, *msg; int qos, retain; } last_will;
        int keepalive;
    } session;
    struct { int reconnect_timeout_ms; } network;
    struct { int size; } buffer;
} esp_mqtt_client_config_t;

esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *cfg);
esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t c, int32_t id, esp_event_handler_t fn, void *arg);
esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t c);
esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t c);
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t c, const char *topic, int qos);
int esp_mqtt_client_publish(esp_mqtt_client_handle_t c, const char *topic, const char *data, int len, int qos, int retain);
int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t c, const char *topic, const char *data, int len, int qos,
                            int retain, bool store);
int esp_mqtt_client_get_outbox_size(esp_mqtt_client_handle_t c);
