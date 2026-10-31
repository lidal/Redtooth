#include "settings.h"

#include "nvs.h"
#include "nvs_flash.h"

static nvs_handle_t s_nvs;

void settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nvs_open("vbctrl", NVS_READWRITE, &s_nvs));
}

uint8_t settings_get(const char *key, uint8_t fallback)
{
    uint8_t value;
    return nvs_get_u8(s_nvs, key, &value) == ESP_OK ? value : fallback;
}

void settings_set(const char *key, uint8_t value)
{
    nvs_set_u8(s_nvs, key, value);
    nvs_commit(s_nvs);
}
