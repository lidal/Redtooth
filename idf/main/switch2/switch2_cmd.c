// Answers to the commands a Switch 2 sends a Pro Controller 2 while connecting
// and pairing. Ported from ESP32-BLE5-NSController-Emulator (MIT, see LICENSE);
// command reference: https://github.com/ndeadly/switch2_controller_research
// (commands.md). Byte values marked as unknown are copied from captures of a
// real controller.

#include <string.h>

#include "aes/esp_aes.h"
#include "esp_log.h"
#include "switch2_priv.h"

static const char *TAG = "switch2";

// ---- Simulated controller flash (command 0x02 reads it; unwritten bytes are 0xff) ----

// Device info
static const uint8_t flash_013000[] = {
    0x01, 0x00,
    // serial number "HEJ71001123456"
    0x48, 0x45, 0x4A, 0x37, 0x31, 0x30, 0x30, 0x31, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x00, 0x00,
    0x7E, 0x05,        // vendor ID: Nintendo
    0x69, 0x20,        // product ID: Pro Controller 2
    0x01, 0x06, 0x01,  // unknown
    0x23, 0x23, 0x23,  // body colour
    0xC8, 0x10, 0x2E,  // button colour: Virtual Boy red
    0xE6, 0xE6, 0xE6,  // highlight colour
    0x32, 0x32, 0x32,  // grip colour
};

// Unknown
static const uint8_t flash_013040[] = {
    0x3B, 0xE0, 0xD3, 0x41, 0xC6, 0x60, 0x6A, 0xBC, 0x4D, 0xD7, 0xA2, 0xBB, 0x71, 0x1E, 0xDD, 0x37,
};

// Unknown, needed for pairing; 0x0130C0 is the same
static const uint8_t flash_013080[] = {
    0x01, 0xAD, 0xD9, 0x9A, 0x55, 0x56, 0x65, 0xA0, 0x00, 0x0A, 0xA0, 0x00, 0x0A, 0xE2,
    0x20, 0x0E, 0xE2, 0x20, 0x0E, 0x9A, 0xAD, 0xD9, 0x9A, 0xAD, 0xD9, 0x0A, 0xA5, 0x50,
    0x0A, 0xA5, 0x50, 0x2F, 0xF6, 0x62, 0x2F, 0xF6, 0x62, 0x0A, 0xFF, 0xFF,
};

// Left stick factory calibration: centre (0x7B3, 0x836), then the ranges either side
static const uint8_t flash_0130A8[] = { 0xB3, 0x67, 0x83, 0x2E, 0x66, 0x5E, 0x3A, 0x06, 0x5F };

// Right stick factory calibration: centre (0x82C, 0x840), then the ranges
static const uint8_t flash_0130E8[] = { 0x2C, 0x08, 0x84, 0xD1, 0x65, 0x63, 0x2A, 0x26, 0x62 };

// Unknown
static const uint8_t flash_013100[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xA6, 0xF2, 0x62, 0xBD, 0xA8, 0x00, 0x08, 0x3D, 0x2F, 0xED, 0x20, 0x41,
};

typedef struct {
    uint32_t addr;
    size_t len;
    const uint8_t *data;
} flash_block_t;

// Blocks with no data are known but blank (0x1FC040: user stick calibration)
static const flash_block_t flash_blocks[] = {
    { 0x013000, sizeof(flash_013000), flash_013000 },
    { 0x013040, sizeof(flash_013040), flash_013040 },
    { 0x013060, 0, NULL },
    { 0x013080, sizeof(flash_013080), flash_013080 },
    { 0x0130A8, sizeof(flash_0130A8), flash_0130A8 },
    { 0x0130C0, sizeof(flash_013080), flash_013080 },
    { 0x0130E8, sizeof(flash_0130E8), flash_0130E8 },
    { 0x013100, sizeof(flash_013100), flash_013100 },
    { 0x1FC040, 0, NULL },
};

static int read_flash(uint32_t addr, size_t len, uint8_t *out)
{
    memset(out, 0xFF, len);
    uint32_t end = addr + len;
    int found = 0;
    for (size_t i = 0; i < sizeof(flash_blocks) / sizeof(flash_blocks[0]); i++) {
        const flash_block_t *block = &flash_blocks[i];
        uint32_t block_end = block->addr + block->len;
        if (block_end < addr || block->addr > end) continue;
        found = 1;
        if (!block->len) continue;
        uint32_t from = addr > block->addr ? addr : block->addr;
        uint32_t to = end < block_end ? end : block_end;
        if (to > from) memcpy(out + (from - addr), block->data + (from - block->addr), to - from);
    }
    return found ? 0 : -1;
}

// ---- Pairing ----

// Our half of the key exchange (command 0x15/0x04): a published fixed value
static const uint8_t ltk_key_b1[S2_LTK_LEN] = {
    0x5C, 0xF6, 0xEE, 0x79, 0x2C, 0xDF, 0x05, 0xE1, 0xBA, 0x2B, 0x63, 0x25, 0xC4, 0x1A, 0x5F, 0x10,
};

void s2_reverse(const uint8_t *in, uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i++) out[i] = in[len - 1 - i];
}

static int aes128_ecb(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    esp_aes_context ctx;
    esp_aes_init(&ctx);
    int rc = esp_aes_setkey(&ctx, key, 128);
    if (rc == 0) rc = esp_aes_crypt_ecb(&ctx, ESP_AES_ENCRYPT, in, out);
    esp_aes_free(&ctx);
    return rc;
}

// ---- Command handlers: return the answer's data length ----

// NFC
static size_t cmd_01(uint8_t sub, const uint8_t *in, uint8_t *out)
{
    if (sub != 0x0c) return 0;
    static const uint8_t answer[] = { 0x61, 0x12, 0x50, 0x10 };
    memcpy(out, answer, sizeof(answer));
    return sizeof(answer);
}

// Flash memory. Read: 02 xx xx 04 | 8 bytes, length at [8], address at [12..14]
static size_t cmd_02(uint8_t sub, const uint8_t *in, uint8_t *out)
{
    if (sub != 0x04) return 0;
    uint32_t addr = (in[14] << 16) | (in[13] << 8) | in[12];
    size_t len = in[8];
    if (len > S2_RSP_DATA_MAX - 8) {
        ESP_LOGE(TAG, "Flash read too long: 0x%02x bytes", (unsigned)len);
        return 0;
    }
    memcpy(out, in + 8, 8);
    out[1] = 0x00;
    if (read_flash(addr, len, out + 8) != 0) {
        ESP_LOGW(TAG, "Unknown flash address 0x%06lx, length 0x%02x", (unsigned long)addr, (unsigned)len);
        return 0;
    }
    return len + 8;
}

// Initialisation; 0x07 carries the console's LTK, no answer data
static size_t cmd_03(uint8_t sub, const uint8_t *in, uint8_t *out)
{
    return 0;
}

// Unknown, during initialisation
static size_t cmd_07(uint8_t sub, const uint8_t *in, uint8_t *out)
{
    if (sub != 0x01) return 0;
    out[0] = 0x00;
    return 1;
}

// Feature select: which parts the input reports carry
static size_t cmd_0c(uint8_t sub, const uint8_t *in, uint8_t *out)
{
    memset(out, 0x00, 12);
    switch (sub) {
    case 0x01:
        // Supported: buttons (0x07), sticks (0x07), IMU (0x01 on a Pro Controller)
        out[4] = 0x07;
        out[5] = 0x07;
        out[6] = 0x01;
        return 12;
    case 0x04:  // enable features: the console is ready for input
        s2_on_features_enabled();
        return 4;
    case 0x02:  // set feature mask
    case 0x03:  // clear feature mask
    case 0x05:  // disable features
        return 4;
    }
    return 0;
}

// Firmware info: version 2.1.4, type Pro Controller 2, Bluetooth patch 12.0.0, DSP 2.3.0
static size_t cmd_10(uint8_t sub, const uint8_t *in, uint8_t *out)
{
    if (sub != 0x01) return 0;
    static const uint8_t info[] = { 0x02, 0x01, 0x04, 0x02, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0x00 };
    memcpy(out, info, sizeof(info));
    return sizeof(info);
}

// Unknown
static size_t cmd_11(uint8_t sub, const uint8_t *in, uint8_t *out)
{
    static const uint8_t answer_01[] = { 0x01, 0x00, 0x00, 0x00 };
    static const uint8_t answer_03[] = {
        0x01, 0x20, 0x03, 0x00, 0x00, 0x0a, 0xe8, 0x1c, 0x3b, 0x79, 0x7d, 0x8b, 0x3a, 0x0a, 0xe8,
        0x9c, 0x42, 0x58, 0xa0, 0x0b, 0x42, 0x0a, 0xe8, 0x9c, 0x41, 0x58, 0xa0, 0x0b, 0x41,
    };
    if (sub == 0x01) {
        memcpy(out, answer_01, sizeof(answer_01));
        return sizeof(answer_01);
    }
    if (sub == 0x03) {
        memcpy(out, answer_03, sizeof(answer_03));
        return sizeof(answer_03);
    }
    return 0;
}

// Bluetooth pairing
static size_t cmd_15(uint8_t sub, const uint8_t *in, uint8_t *out)
{
    switch (sub) {
    case 0x01:  // exchange addresses: the console sends its own at [10..15]
        if (memcmp(in + 10, s2.console_addr, S2_ADDR_LEN) != 0) {
            ESP_LOGE(TAG, "Pairing: console address doesn't match the connection");
            return 0;
        }
        out[0] = 0x01;
        out[1] = 0x04;
        out[2] = 0x01;
        memcpy(out + 3, s2.ctrl_addr_le, S2_ADDR_LEN);
        return 3 + S2_ADDR_LEN;

    case 0x04:  // exchange key halves: LTK = A1 xor B1 (byte order reversed)
        for (int i = 0; i < S2_LTK_LEN; i++) {
            s2.ltk[i] = in[9 + S2_LTK_LEN - 1 - i] ^ ltk_key_b1[S2_LTK_LEN - 1 - i];
        }
        out[0] = 0x01;
        memcpy(out + 1, ltk_key_b1, S2_LTK_LEN);
        return 1 + S2_LTK_LEN;

    case 0x02: {  // confirm the key: answer with AES-128(LTK, challenge)
        uint8_t challenge[S2_LTK_LEN];
        s2_reverse(in + 9, challenge, S2_LTK_LEN);
        if (aes128_ecb(s2.ltk, challenge, out + 1) != 0) {
            ESP_LOGE(TAG, "Pairing: key confirmation failed");
            return 0;
        }
        s2_reverse(s2.ltk, s2.ltk_le, S2_LTK_LEN);
        out[0] = 0x01;
        return 1 + S2_LTK_LEN;
    }

    case 0x03:  // pairing finished
        if ((in[8] == 0x00 || in[9] == 0x00) && s2_on_pairing_done() == 0) {
            out[0] = 0x01;
            return 1;
        }
        return 0;
    }
    return 0;
}

// Unknown
static size_t cmd_16(uint8_t sub, const uint8_t *in, uint8_t *out)
{
    if (sub != 0x01) return 0;
    memset(out, 0x00, 0x18);
    return 0x18;
}

// Unknown
static size_t cmd_18(uint8_t sub, const uint8_t *in, uint8_t *out)
{
    if (sub != 0x01) return 0;
    static const uint8_t answer[] = { 0x00, 0x00, 0x40, 0xf0, 0x00, 0x00, 0x60, 0x00 };
    memcpy(out, answer, sizeof(answer));
    return sizeof(answer);
}

int s2_cmd_process(const uint8_t *cmd, size_t cmd_len, uint8_t rsp[S2_RSP_MAX], size_t *rsp_len)
{
    if (cmd_len < 8 + S2_LTK_LEN + 1) {
        // Long enough for every field the handlers read
        static uint8_t padded[8 + S2_LTK_LEN + 1];
        memset(padded, 0, sizeof(padded));
        memcpy(padded, cmd, cmd_len);
        cmd = padded;
    }
    uint8_t command = cmd[0];
    uint8_t sub = cmd[3];
    uint8_t *data = rsp + S2_RSP_PREFIX_LEN + 8;
    size_t len = 0;

    ESP_LOGD(TAG, "Command 0x%02x/0x%02x", command, sub);
    switch (command) {
    case 0x01: len = cmd_01(sub, cmd, data); break;
    case 0x02: len = cmd_02(sub, cmd, data); break;
    case 0x03: len = cmd_03(sub, cmd, data); break;
    case 0x07: len = cmd_07(sub, cmd, data); break;
    case 0x0c: len = cmd_0c(sub, cmd, data); break;
    case 0x10: len = cmd_10(sub, cmd, data); break;
    case 0x11: len = cmd_11(sub, cmd, data); break;
    case 0x15: len = cmd_15(sub, cmd, data); break;
    case 0x16: len = cmd_16(sub, cmd, data); break;
    case 0x18: len = cmd_18(sub, cmd, data); break;
    case 0x09:
    case 0x0a:
        break;  // acknowledged without data
    default:
        ESP_LOGW(TAG, "Unknown command 0x%02x/0x%02x", command, sub);
        break;
    }

    // Zero prefix | the command's first 4 bytes | 10 78 00 00 | data
    static const uint8_t magic[] = { 0x10, 0x78, 0x00, 0x00 };
    memset(rsp, 0, S2_RSP_PREFIX_LEN);
    memcpy(rsp + S2_RSP_PREFIX_LEN, cmd, 4);
    memcpy(rsp + S2_RSP_PREFIX_LEN + 4, magic, sizeof(magic));
    if (rsp[S2_RSP_PREFIX_LEN + 1] == 0x91) rsp[S2_RSP_PREFIX_LEN + 1] = 0x01;
    *rsp_len = S2_RSP_PREFIX_LEN + 8 + len;
    return 0;
}
