#pragma once

#include <stdint.h>

// Small settings kept across power-offs, in the same NVS namespace and keys
// the Arduino sketch used ("vbctrl": mode, lstick, swapab, addrgen).
void settings_init(void);
uint8_t settings_get(const char *key, uint8_t fallback);
void settings_set(const char *key, uint8_t value);
