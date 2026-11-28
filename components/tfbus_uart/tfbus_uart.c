/**
 * @file tfbus_uart.c
 * @brief RS485 half-duplex UART transport for TFBUS (ESP-IDF UART driver in RS485 mode).
 */
#include "tfbus_uart.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "tfbus_frame.h"

// Options from menuconfig (main/Kconfig.clu); the host build uses the Kconfig defaults.
#ifdef CONFIG_CLU_TFBUS_MASTER
#define LATE_REPLY_GUARD_MS CONFIG_CLU_TFBUS_LATE_REPLY_GUARD_MS
#ifdef CONFIG_CLU_TFBUS_UART_ERROR_STATS
#define UART_ERROR_STATS 1
#else
#define UART_ERROR_STATS 0
#endif
#else
#define LATE_REPLY_GUARD_MS 2
#define UART_ERROR_STATS    1
#endif
#define UART_EVENT_QUEUE_LENGTH 16 // drained before every frame sent: a few RX events per transfer

static const char *TAG = "tfbus_uart";
static int s_uart = -1;
static bool s_nowire; // initialised without a UART: only local endpoints answer
static bool s_auto_direction; // no DE pin: plain UART mode, our own frame may come back as echo
static bool s_echo_reported;  // whether the transceiver echoes was logged (once per boot)
static tfbus_uart_local_fn_t s_local[TFBUS_UART_MAX_LOCAL];
static int s_nlocal;
static bool s_last_local; // the last tfbus_uart_transact() was owned by a local endpoint

// Bus utilization: summed by the bus task over LOAD_WINDOW_US, then latched for tfbus_uart_load().
#define LOAD_WINDOW_US (10 * 1000 * 1000)
#define BITS_PER_BYTE  10 // 8N1: start + 8 data + stop
static int s_baud;
static bool s_window_open;
static int64_t s_window_start_us, s_window_busy_us;
static uint32_t s_window_wire_bytes;
static volatile uint16_t s_wire_permille, s_busy_permille;
static volatile bool s_load_valid;
static volatile uint32_t s_load_latched_ms; // esp_timer ms (wraps), single word: read from any task

// Receive problems (tfbus_uart_stats()): written by the bus task only, single words read from any task.
static QueueHandle_t s_uart_events; // UART driver events (UART_ERROR_STATS), NULL if not installed
static volatile uint32_t s_late_replies, s_rx_overflows, s_frame_errors, s_breaks;
static bool s_late_reply_reported; // the first late reply is logged (once per boot)

static void account_bus_use(int64_t started_us, size_t wire_bytes);
static uint16_t permille(int64_t part_us, int64_t whole_us);
static void count_uart_errors(void);

void tfbus_uart_set_local(tfbus_uart_local_fn_t local_endpoint)
{
    s_nlocal = 0;
    tfbus_uart_add_local(local_endpoint);
}

bool tfbus_uart_add_local(tfbus_uart_local_fn_t local_endpoint)
{
    if (!local_endpoint || s_nlocal == TFBUS_UART_MAX_LOCAL) {
        return false;
    }
    s_local[s_nlocal++] = local_endpoint;
    return true;
}

/** @brief Offer a frame to the local endpoints in turn: the first that owns it answers. */
static int local(const uint8_t *request_frame, size_t request_length, uint8_t *reply_buffer, size_t reply_capacity)
{
    for (int i = 0; i < s_nlocal; i++) {
        int reply_length = s_local[i](request_frame, request_length, reply_buffer, reply_capacity);
        if (reply_length != 0) {
            return reply_length;
        }
    }
    return 0;
}

esp_err_t tfbus_uart_init(const tfbus_uart_cfg_t *cfg)
{
    s_nowire = cfg->uart_num < 0;
    s_window_open = s_load_valid = false; // a new bus: utilization from scratch
    s_window_busy_us = 0;
    s_window_wire_bytes = 0;
    s_late_replies = s_rx_overflows = s_frame_errors = s_breaks = 0;
    s_uart_events = NULL;
    if (s_nowire) {
        ESP_LOGI(TAG, "no RS485 wire: emulated modules only");
        return ESP_OK;
    }
    const uart_config_t uart_config = {
        .baud_rate = cfg->baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(cfg->uart_num, 512, 0, UART_ERROR_STATS ? UART_EVENT_QUEUE_LENGTH : 0,
                                        UART_ERROR_STATS ? &s_uart_events : NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(cfg->uart_num, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(cfg->uart_num, cfg->tx_pin, cfg->rx_pin, cfg->de_pin, UART_PIN_NO_CHANGE));
    // RS485 half-duplex mode resets the RX FIFO in the TX_DONE interrupt (to drop the echo) before
    // releasing RTS. With DE on RTS the transceiver cannot receive until then, so nothing is lost.
    // An auto-direction transceiver receives at once: a fast module's first reply byte can reach
    // the FIFO before that interrupt runs and be wiped. Without DE: plain UART, echo skipped in receive().
    s_auto_direction = cfg->de_pin < 0;
    ESP_ERROR_CHECK(uart_set_mode(cfg->uart_num, s_auto_direction ? UART_MODE_UART : UART_MODE_RS485_HALF_DUPLEX));
    // ~3 byte times at 115200 -> frame boundary detection
    ESP_ERROR_CHECK(uart_set_rx_timeout(cfg->uart_num, 3));
    s_uart = cfg->uart_num;
    s_baud = cfg->baud;
    ESP_LOGI(TAG, "RS485 on UART%d tx=%d rx=%d de=%d @%d", cfg->uart_num, cfg->tx_pin,
             cfg->rx_pin, cfg->de_pin, cfg->baud);
    return ESP_OK;
}

/** @brief Put a frame on the wire. */
static esp_err_t wire_send(const uint8_t *request_frame, size_t request_length)
{
    if (s_nowire) {
        return ESP_OK; // nobody out there
    }
    if (s_uart < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    count_uart_errors();
    uart_flush_input(s_uart);
    if (uart_write_bytes(s_uart, request_frame, request_length) != (int)request_length) {
        return ESP_FAIL;
    }
    return uart_wait_tx_done(s_uart, pdMS_TO_TICKS(20));
}

esp_err_t tfbus_uart_send(const uint8_t *request_frame, size_t request_length)
{
    for (int i = 0; i < s_nlocal; i++) {
        uint8_t reply_buffer[TFBUS_MAX_FRAME];
        s_local[i](request_frame, request_length, reply_buffer, sizeof reply_buffer); // e.g. RESET broadcast: local modules hear it too
    }
    int64_t started_us = esp_timer_get_time();
    esp_err_t send_result = wire_send(request_frame, request_length);
    if (send_result == ESP_OK && !s_nowire) {
        account_bus_use(started_us, request_length);
    }
    return send_result;
}

/** @brief Reply collector state for tfbus_uart_transact() / tfbus_uart_collect(). */
typedef struct {
    const uint8_t *request_frame;
    size_t request_length;
    tfbus_uart_reply_t *replies;
    int max_replies;
    size_t reply_capacity; /**< Largest reply kept. */
    bool listen_full_window; /**< Keep listening after the first reply (collect). */
    int got;               /**< Replies kept. */
    size_t framed_bytes;   /**< Received bytes that formed frames (any frame, kept or not). */
    size_t echo_matched;   /**< Bytes of our own request seen back so far (auto-direction echo). */
    bool echo_done;        /**< Echo over (complete, mismatched or not expected). */
    bool late;             /**< Request timed out: frames still arriving are late replies, dropped. */
    uint8_t raw[24];       /**< First bytes received (broadcast diagnostics). */
    size_t raw_length;
} collect_t;

static int receive(collect_t *collector, int timeout_ms);
static bool read_chunk(collect_t *collector, tfbus_rx_t *parser, int64_t deadline_us, size_t *received_total);
static size_t drop_late_reply(collect_t *collector, tfbus_rx_t *parser);
static size_t skip_echo(collect_t *collector, tfbus_rx_t *parser, const uint8_t *chunk, size_t chunk_length);
static void on_frame(const uint8_t *frame, size_t len, void *ctx);

int tfbus_uart_transact(const uint8_t *request_frame, size_t request_length, uint8_t *reply_buffer, size_t reply_capacity, int timeout_ms)
{
    int local_reply_length = local(request_frame, request_length, reply_buffer, reply_capacity);
    s_last_local = local_reply_length != 0;
    if (s_last_local) {
        return local_reply_length > 0 ? local_reply_length : 0;
    }
    if (s_nowire) {
        return 0; // no module on a wire answers
    }
    int64_t started_us = esp_timer_get_time();
    if (wire_send(request_frame, request_length) != ESP_OK) {
        return -1;
    }
    tfbus_uart_reply_t reply;
    collect_t collector = {.request_frame = request_frame, .request_length = request_length, .replies = &reply,
                           .max_replies = 1,
                           .reply_capacity = reply_capacity < sizeof reply.frame ? reply_capacity : sizeof reply.frame};
    int received = receive(&collector, timeout_ms);
    account_bus_use(started_us, request_length + (size_t)received);
    if (collector.got == 0) {
        return 0;
    }
    memcpy(reply_buffer, reply.frame, reply.length);
    return (int)reply.length;
}

int tfbus_uart_collect(const uint8_t *request_frame, size_t request_length, tfbus_uart_reply_t *replies, int max_replies, int timeout_ms, bool *garbled)
{
    *garbled = false;
    int reply_count = 0;
    for (int i = 0; i < s_nlocal && reply_count < max_replies; i++) {
        int reply_length = s_local[i](request_frame, request_length, replies[reply_count].frame, sizeof replies[reply_count].frame);
        if (reply_length > 0) {
            replies[reply_count++].length = (size_t)reply_length;
        }
    }
    if (reply_count > 0 || s_nowire) {
        return reply_count; // local modules first (no 1 s wire window each), the wire after them
    }
    int64_t started_us = esp_timer_get_time();
    if (wire_send(request_frame, request_length) != ESP_OK) {
        return -1;
    }
    collect_t collector = {.request_frame = request_frame, .request_length = request_length,
                           .replies = replies, .max_replies = max_replies,
                           .reply_capacity = sizeof replies->frame, .listen_full_window = true};
    size_t received = (size_t)receive(&collector, timeout_ms);
    account_bus_use(started_us, request_length + received);
    *garbled = received > collector.framed_bytes;
    return collector.got;
}

bool tfbus_uart_last_local(void)
{
    return s_last_local;
}

bool tfbus_uart_load(tfbus_uart_load_t *load)
{
    if (s_nowire || s_uart < 0 || !s_load_valid) {
        return false;
    }
    uint32_t latched_age_ms = (uint32_t)(esp_timer_get_time() / 1000) - s_load_latched_ms;
    bool stale = latched_age_ms > 2 * (LOAD_WINDOW_US / 1000); // bus parked or idle: no window closes
    load->wire_permille = stale ? 0 : s_wire_permille;
    load->busy_permille = stale ? 0 : s_busy_permille;
    return true;
}

bool tfbus_uart_stats(tfbus_uart_stats_t *stats)
{
    if (s_nowire || s_uart < 0) {
        return false;
    }
    *stats = (tfbus_uart_stats_t){.late_replies = s_late_replies, .rx_overflows = s_rx_overflows,
                                  .frame_errors = s_frame_errors, .breaks = s_breaks};
    return true;
}

/**
 * @brief Count the receive errors the UART driver reported since the last frame (bus task, before
 * sending; the flush right after drops what an overflow left behind). Other events are discarded.
 */
static void count_uart_errors(void)
{
    uart_event_t event;
    while (s_uart_events && xQueueReceive(s_uart_events, &event, 0) == pdTRUE) {
        switch (event.type) {
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL: s_rx_overflows++; break;
        case UART_FRAME_ERR: s_frame_errors++; break;
        case UART_BREAK: s_breaks++; break;
        default: break;
        }
    }
}

/**
 * @brief Add one wire transfer (bus task only) to the load window; latch the window once it spans
 * LOAD_WINDOW_US.
 * @param started_us Time the transfer began (before sending).
 * @param wire_bytes Bytes it put on and took off the wire (echo of our own frame excluded).
 */
static void account_bus_use(int64_t started_us, size_t wire_bytes)
{
    int64_t now_us = esp_timer_get_time();
    if (!s_window_open) {
        s_window_open = true;
        s_window_start_us = started_us;
    }
    s_window_busy_us += now_us - started_us;
    s_window_wire_bytes += (uint32_t)wire_bytes;
    int64_t window_us = now_us - s_window_start_us;
    if (window_us < LOAD_WINDOW_US) {
        return;
    }
    int64_t wire_us = (int64_t)s_window_wire_bytes * BITS_PER_BYTE * 1000000 / s_baud;
    s_wire_permille = permille(wire_us, window_us);
    s_busy_permille = permille(s_window_busy_us, window_us);
    s_load_latched_ms = (uint32_t)(now_us / 1000);
    s_load_valid = true;
    s_window_start_us = now_us;
    s_window_busy_us = 0;
    s_window_wire_bytes = 0;
}

/** @brief @p part_us / @p whole_us in permille, at most 1000. */
static uint16_t permille(int64_t part_us, int64_t whole_us)
{
    int64_t ratio = part_us * 1000 / whole_us;
    return (uint16_t)(ratio > 1000 ? 1000 : ratio);
}

/**
 * @brief Read the wire until the first kept reply (or, with listen_full_window, until the timeout);
 * after a timed-out tfbus_uart_transact(), drop_late_reply().
 * @return Bytes received, without the echo of our own request.
 */
static int receive(collect_t *collector, int timeout_ms)
{
    tfbus_rx_t parser;
    tfbus_rx_init(&parser);
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    size_t total = 0;
    collector->echo_done = !s_auto_direction;
    while ((collector->listen_full_window || collector->got == 0) && esp_timer_get_time() < deadline) {
        read_chunk(collector, &parser, deadline, &total);
    }
    // Discovery diagnostics: a broadcast got bytes back that do not parse (collision / turnaround).
    const uint8_t *request_frame = collector->request_frame;
    if (collector->got == 0 && total > 0 && collector->request_length >= 3 && request_frame[1] == TFBUS_BROADCAST) {
        char hex[3 * sizeof collector->raw + 1] = "";
        for (size_t i = 0; i < collector->raw_length; i++) {
            snprintf(hex + 3 * i, 4, "%02x ", collector->raw[i]);
        }
        ESP_LOGW(TAG, "bcast type 0x%02x: no reply parsed, %u byte(s) received: %s", request_frame[2],
                 (unsigned)total, hex);
    }
    if (LATE_REPLY_GUARD_MS > 0 && collector->got == 0 && !collector->listen_full_window) {
        total += drop_late_reply(collector, &parser);
    }
    return (int)total;
}

/**
 * @brief Wait until @p deadline_us for received bytes and feed them, echo skipped, to @p parser.
 *
 * uart_read_bytes() returns only when `length` bytes arrived or the timeout expired: wait for one
 * byte, then take whatever else is already buffered, so a reply is parsed as soon as it is
 * complete instead of after the full timeout.
 * @param received_total Increased by the bytes received (echo excluded).
 * @return true if anything was received (echo included).
 */
static bool read_chunk(collect_t *collector, tfbus_rx_t *parser, int64_t deadline_us, size_t *received_total)
{
    uint8_t chunk[64];
    int64_t left_us = deadline_us - esp_timer_get_time();
    if (left_us <= 0) {
        return false;
    }
    TickType_t ticks = pdMS_TO_TICKS(left_us / 1000);
    int bytes_read = uart_read_bytes(s_uart, chunk, 1, ticks ? ticks : 1);
    if (bytes_read <= 0) {
        return false;
    }
    size_t more = 0;
    if (uart_get_buffered_data_len(s_uart, &more) == ESP_OK && more > 0) {
        if (more > sizeof chunk - 1) {
            more = sizeof chunk - 1;
        }
        int extra_bytes_read = uart_read_bytes(s_uart, chunk + 1, more, 0);
        if (extra_bytes_read > 0) {
            bytes_read += extra_bytes_read;
        }
    }
    size_t echo_length = skip_echo(collector, parser, chunk, (size_t)bytes_read);
    const uint8_t *received = chunk + echo_length;
    size_t received_length = (size_t)bytes_read - echo_length;
    for (size_t i = 0; i < received_length && collector->raw_length < sizeof collector->raw; i++) {
        collector->raw[collector->raw_length++] = received[i];
    }
    *received_total += received_length;
    tfbus_rx_push(parser, received, received_length, on_frame, collector);
    return true;
}

/**
 * @brief Late reply guard: the request timed out; read on until the line has been quiet for
 * LATE_REPLY_GUARD_MS and drop what arrives.
 *
 * Replies carry no sender address: a reply (or the rest of one) coming in just after the timeout
 * would otherwise be taken as the reply to the next frame (another module's status, a command's
 * ACK). Bounded by one guard window plus the time of a full frame, so a jammed line cannot hold
 * the bus task.
 * @return Bytes received meanwhile (echo excluded).
 */
static size_t drop_late_reply(collect_t *collector, tfbus_rx_t *parser)
{
    int64_t max_frame_us = (int64_t)TFBUS_MAX_FRAME * BITS_PER_BYTE * 1000000 / (s_baud ? s_baud : 115200);
    int64_t hard_stop_us = esp_timer_get_time() + LATE_REPLY_GUARD_MS * 1000 + max_frame_us;
    size_t drained = 0;
    collector->late = true;
    for (;;) {
        int64_t quiet_until_us = esp_timer_get_time() + LATE_REPLY_GUARD_MS * 1000;
        if (!read_chunk(collector, parser, quiet_until_us < hard_stop_us ? quiet_until_us : hard_stop_us, &drained) ||
            esp_timer_get_time() >= hard_stop_us) {
            return drained;
        }
    }
}

/**
 * @brief Length of the echo of our own request at the start of @p chunk.
 *
 * Plain UART mode (no DE pin) keeps receiving while we transmit, so an auto-direction transceiver
 * may hand our frame back ahead of the reply. Bytes equal to the request, in order, are echo. On
 * the first byte that differs the echo ends: the bytes held back so far go to @p parser after all
 * (a reply also starts with TFBUS_START).
 */
static size_t skip_echo(collect_t *collector, tfbus_rx_t *parser, const uint8_t *chunk, size_t chunk_length)
{
    size_t echo_length = 0;
    while (!collector->echo_done && echo_length < chunk_length) {
        if (chunk[echo_length] != collector->request_frame[collector->echo_matched]) {
            tfbus_rx_push(parser, collector->request_frame, collector->echo_matched, on_frame, collector);
            collector->echo_done = true;
            if (!s_echo_reported && collector->echo_matched <= 1) { // a reply right away: no echo
                s_echo_reported = true;
                ESP_LOGI(TAG, "auto-direction transceiver: no echo of our own frames");
            }
            break;
        }
        echo_length++;
        collector->echo_matched++;
        collector->echo_done = collector->echo_matched == collector->request_length;
    }
    if (!s_echo_reported && collector->echo_done && collector->echo_matched == collector->request_length) {
        s_echo_reported = true;
        ESP_LOGI(TAG, "auto-direction transceiver: echoes our own frames (skipped)");
    }
    return echo_length;
}

/**
 * @brief RX parser callback: keep module -> CLU frames.
 *
 * The UART drops our own TX in RS485 half-duplex mode (ESP-IDF clears rs485tx_rx_en); in plain
 * UART mode (no DE pin) skip_echo() removes it, so no echo reaches the parser.
 * @param frame Complete frame.
 * @param len   Frame length.
 * @param ctx   ::collect_t.
 */
static void on_frame(const uint8_t *frame, size_t len, void *ctx)
{
    collect_t *collector = ctx;
    collector->framed_bytes += len;
    if (collector->late) {
        if (frame[1] == TFBUS_CLU_ADDR) {
            s_late_replies++;
            if (!s_late_reply_reported) {
                s_late_reply_reported = true;
                ESP_LOGW(TAG, "late reply to a frame for addr %u dropped (more: sys_info tfbus_late_replies; raise "
                              "CLU_TFBUS_POLL_TIMEOUT_MS if they keep coming)", collector->request_frame[1]);
            }
        }
        return;
    }
    if (collector->got >= collector->max_replies) {
        return;
    }
    if (frame[1] != TFBUS_CLU_ADDR || len > collector->reply_capacity) {
        return;
    }
    tfbus_uart_reply_t *reply = &collector->replies[collector->got++];
    memcpy(reply->frame, frame, len);
    reply->length = len;
}
