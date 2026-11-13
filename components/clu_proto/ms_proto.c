/**
 * @file ms_proto.c
 * @brief MySensors serial protocol: parse / format one line.
 */
#include "ms_proto.h"

#include <stdio.h>
#include <string.h>

/** @brief Parse a decimal 0..255 field ending at ';'; advances *p past the ';'. */
static bool field(const char **p, uint8_t *out)
{
    const char *s = *p;
    unsigned v = 0;
    int n = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (unsigned)(*s++ - '0');
        if (++n > 3 || v > 255) {
            return false;
        }
    }
    if (!n || *s != ';') {
        return false;
    }
    *out = (uint8_t)v;
    *p = s + 1;
    return true;
}

bool ms_parse(const char *line, ms_msg_t *out)
{
    const char *p = line;
    if (!field(&p, &out->node) || !field(&p, &out->child) || !field(&p, &out->cmd) ||
        !field(&p, &out->ack) || !field(&p, &out->type)) {
        return false;
    }
    if (out->cmd > MS_C_STREAM || out->ack > 1) {
        return false;
    }
    size_t n = strcspn(p, "\r\n");
    if (n >= sizeof out->payload) {
        n = sizeof out->payload - 1;
    }
    memcpy(out->payload, p, n);
    out->payload[n] = 0;
    return true;
}

int ms_format(const ms_msg_t *m, char *out, size_t cap)
{
    if (strpbrk(m->payload, "\r\n")) {
        return -1;
    }
    int n = snprintf(out, cap, "%u;%u;%u;%u;%u;%s\n", m->node, m->child, m->cmd, m->ack, m->type, m->payload);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}
