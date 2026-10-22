/**
 * @file cli_line_asm.h
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
 */

/*
 * tech_spec.md section 2.5.3. The 64-byte line limit is a wire contract, not a
 * Kconfig value. The command-name lookup is injected so this module needs no
 * other component (the BLE client passes diag_cmd_lookup_ble()).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLI_LINE_MAX_BYTES 64u                   /* excluding the terminator */
#define CLI_LINE_BUF_BYTES (CLI_LINE_MAX_BYTES + 1u) /* the extra slot holds a CR before the LF */

typedef enum {
    CLI_KIND_CMD = 0,          /* a valid command with no arguments */
    CLI_KIND_ERR_OVERSIZED,    /* ERR oversized_input */
    CLI_KIND_ERR_INVALID_CHAR, /* ERR invalid_argument */
    CLI_KIND_ERR_UNKNOWN,      /* ERR unknown_command */
    CLI_KIND_ERR_ARGS,         /* ERR invalid_argument cmd=<name> */
} cli_kind_t;

typedef struct {
    cli_kind_t kind;
    int        cmd_id; /* valid for CLI_KIND_CMD and CLI_KIND_ERR_ARGS, otherwise -1 */
} cli_result_t;

/* Returns the command id (>= 0) for an exact, case-sensitive name of `len`
 * characters, or a negative value if it is not one of the known commands. */
typedef int (*cli_lookup_fn)(const char *name, size_t len);
typedef void (*cli_result_cb)(const cli_result_t *r, void *user);

typedef struct {
    char     buf[CLI_LINE_BUF_BYTES];
    uint8_t  len;
    bool     overflow;
    bool     bad_char;
    uint32_t gen;
} cli_line_asm_t;

/* Discards any partial line and binds the assembler to a connection generation. */
void cli_line_asm_reset(cli_line_asm_t *a, uint32_t gen);

/* Generation filter on the input side (REQ-FUNC-62): if the assembler is bound to another
 * generation, discard the partial line and rebind. Returns true if it was reset. */
bool cli_line_asm_sync(cli_line_asm_t *a, uint32_t gen);

/* Feeds n bytes; `cb` is called once per completed non-empty line, in order.
 * A partial line stays buffered with no output. LF (with or without a preceding
 * CR) terminates a line. Precedence at the line end: oversized, invalid
 * character, unknown command, extra arguments. No prefix of an oversized line is
 * ever parsed. Returns the number of results delivered. */
size_t cli_line_asm_feed(cli_line_asm_t *a, const uint8_t *data, size_t n, cli_lookup_fn lookup,
                         cli_result_cb cb, void *user);

#ifdef __cplusplus
}
#endif
