/**
 * @file ms_proto.c
 * @brief MySensors serial protocol: parse / format one line.
 */
#include "ms_proto.h"

#include <stdio.h>
#include <string.h>

/** @brief Parse a decimal 0..255 field ending at ';'; advances *p past the ';'. */
static bool field(const char **cursor, uint8_t *out)
{
    const char *field_text = *cursor;
    unsigned field_value = 0;
    int digit_count = 0;
    while (*field_text >= '0' && *field_text <= '9') {
        field_value = field_value * 10 + (unsigned)(*field_text++ - '0');
        if (++digit_count > 3 || field_value > 255) {
            return false;
        }
    }
    if (!digit_count || *field_text != ';') {
        return false;
    }
    *out = (uint8_t)field_value;
    *cursor = field_text + 1;
    return true;
}

bool ms_parse(const char *line, ms_msg_t *out)
{
    const char *payload_start = line;
    if (!field(&payload_start, &out->node) || !field(&payload_start, &out->child) || !field(&payload_start, &out->cmd) ||
        !field(&payload_start, &out->ack) || !field(&payload_start, &out->type)) {
        return false;
    }
    if (out->cmd > MS_C_STREAM || out->ack > 1) {
        return false;
    }
    size_t payload_length = strcspn(payload_start, "\r\n");
    if (payload_length >= sizeof out->payload) {
        payload_length = sizeof out->payload - 1;
    }
    memcpy(out->payload, payload_start, payload_length);
    out->payload[payload_length] = 0;
    return true;
}

int ms_format(const ms_msg_t *message, char *out, size_t cap)
{
    if (strpbrk(message->payload, "\r\n")) {
        return -1;
    }
    int formatted_length = snprintf(out, cap, "%u;%u;%u;%u;%u;%s\n", message->node, message->child, message->cmd, message->ack, message->type, message->payload);
    return formatted_length < 0 || (size_t)formatted_length >= cap ? -1 : formatted_length;
}
