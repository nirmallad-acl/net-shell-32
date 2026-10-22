/**
 * @file cli_line_asm.c
 * @brief PURE: byte stream to command-line state machine, validation and parse
 *
 * @author Nirmal Lad <nirmal.lad@acldigital.com>
 * @date 2026-10-19
 *
 * @copyright Copyright (c) 2026 ACL Digital Pvt Ltd. All rights reserved.
 *
 * CONFIDENTIALITY NOTICE:
 * This software and documentation are the confidential and proprietary
 * information of ACL Digital Pvt Ltd. Unauthorized copying, distribution,
 * modification, or reverse engineering of this file, via any medium,
 * is strictly prohibited.
 *
 * REQ-FUNC-05..08, 13, 16, REQ-SEC-02.
 */
#include "cli_line_asm.h"

#include <string.h>

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static bool is_sep(char c)
{
    return c == ' ' || c == '\t';
}

void cli_line_asm_reset(cli_line_asm_t *a, uint32_t gen)
{
    if (a == NULL) {
        return;
    }
    memset(a->buf, 0, sizeof(a->buf));
    a->len = 0;
    a->overflow = false;
    a->bad_char = false;
    a->gen = gen;
}

bool cli_line_asm_sync(cli_line_asm_t *a, uint32_t gen)
{
    if (a == NULL || a->gen == gen) {
        return false;
    }
    cli_line_asm_reset(a, gen);
    return true;
}

/* Evaluates a completed line (state is reset by the caller afterwards). */
static bool evaluate(cli_line_asm_t *a, cli_lookup_fn lookup, cli_result_t *out)
{
    out->cmd_id = -1;
    if (a->overflow || a->len > CLI_LINE_MAX_BYTES) {
        out->kind = CLI_KIND_ERR_OVERSIZED;
        return true;
    }
    if (a->bad_char) {
        out->kind = CLI_KIND_ERR_INVALID_CHAR;
        return true;
    }
    size_t b = 0;
    size_t e = a->len;
    while (b < e && is_ws(a->buf[b])) {
        b++;
    }
    while (e > b && is_ws(a->buf[e - 1u])) {
        e--;
    }
    if (b == e) {
        return false; /* empty after trimming: no output */
    }
    size_t t = b;
    while (t < e && !is_sep(a->buf[t])) {
        t++;
    }
    int id = (lookup != NULL) ? lookup(&a->buf[b], t - b) : -1;
    if (id < 0) {
        out->kind = CLI_KIND_ERR_UNKNOWN;
        return true;
    }
    while (t < e && is_sep(a->buf[t])) {
        t++;
    }
    out->cmd_id = id;
    out->kind = (t < e) ? CLI_KIND_ERR_ARGS : CLI_KIND_CMD;
    return true;
}

size_t cli_line_asm_feed(cli_line_asm_t *a, const uint8_t *data, size_t n, cli_lookup_fn lookup,
                         cli_result_cb cb, void *user)
{
    if (a == NULL || (data == NULL && n > 0) || cb == NULL) {
        return 0;
    }
    size_t delivered = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t b = data[i];
        if (b == (uint8_t)'\n') {
            if (!a->overflow && a->len > 0 && a->buf[a->len - 1u] == '\r') {
                a->len--; /* CRLF: drop the CR */
            }
            cli_result_t r;
            if (evaluate(a, lookup, &r)) {
                cb(&r, user);
                delivered++;
            }
            const uint32_t g = a->gen;
            cli_line_asm_reset(a, g);
            continue;
        }
        if (a->len >= CLI_LINE_BUF_BYTES) {
            a->overflow = true; /* discard up to and including the next LF */
            continue;
        }
        a->buf[a->len++] = (char)b;
        const bool printable = (b >= 0x20u && b <= 0x7Eu);
        if (!printable && b != (uint8_t)'\t' && b != (uint8_t)'\r') {
            a->bad_char = true;
        }
    }
    return delivered;
}
