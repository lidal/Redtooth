#pragma once

// Shared between the Switch 2 GATT/pairing code and its command handlers.
// Ported from ESP32-BLE5-NSController-Emulator (MIT, see LICENSE).

#include <stddef.h>
#include <stdint.h>

#define S2_ADDR_LEN 6
#define S2_LTK_LEN  16

// The console writes commands to 0x0016 behind 0x21 zero bytes; the answers
// go out on 0x001e behind 0x0e zero bytes.
#define S2_CMD_PREFIX_LEN 0x21
#define S2_RSP_PREFIX_LEN 0x0e
#define S2_RSP_DATA_MAX   0x80
#define S2_RSP_MAX        (S2_RSP_PREFIX_LEN + 8 + S2_RSP_DATA_MAX)

typedef struct {
    uint8_t ctrl_addr[S2_ADDR_LEN];     // our address, most significant byte first (78:81:8c:...)
    uint8_t ctrl_addr_le[S2_ADDR_LEN];  // the same, least significant byte first
    uint8_t console_addr[S2_ADDR_LEN];  // least significant byte first, as NimBLE has it
    uint8_t ltk[S2_LTK_LEN];            // most significant byte first
    uint8_t ltk_le[S2_LTK_LEN];
} s2_pairing_t;

extern s2_pairing_t s2;

// Builds the answer to one command (cmd points behind the zero prefix).
// Returns 0 and the answer's length, or -1.
int s2_cmd_process(const uint8_t *cmd, size_t cmd_len, uint8_t rsp[S2_RSP_MAX], size_t *rsp_len);

// Called by the command handlers
void s2_on_features_enabled(void);  // the console is ready for input reports
int s2_on_pairing_done(void);       // the LTK is agreed: store it

void s2_reverse(const uint8_t *in, uint8_t *out, size_t len);
