//
// Created by awalol on 2026/5/4.
//

#include "config.h"

#include <cmath>
#include <cstring>

#include "state_mgr.h"
#include "utils.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/cyw43_arch.h"

constexpr uint32_t CONFIG_MAGIC = 0x66ccff00;
constexpr uint16_t CONFIG_VERSION = 2;
constexpr uint32_t CONFIG_FLASH_OFFSET = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE;
static Config config{};
bool is_dse = false;

// 编译期保护
// 判断Config结构体是否能放进flash 256bytes
static_assert(sizeof(Config) <= FLASH_PAGE_SIZE);
// 配置区起始地址必须按 flash sector 对齐。
static_assert(CONFIG_FLASH_OFFSET % FLASH_SECTOR_SIZE == 0);

uint32_t calc_config_crc(const Config &con) {
    return crc32(reinterpret_cast<const uint8_t *>(&con.body), sizeof(Config_body));
}

const Config *flash_config() {
    return reinterpret_cast<const Config *>(XIP_BASE + CONFIG_FLASH_OFFSET);
}

void config_valid() {
    // valid config and set default value
    if (config.magic != CONFIG_MAGIC) {
        config.magic = CONFIG_MAGIC;
        printf("[Config] Config Magic Header is invalid\n");
    }
    if (config.size != sizeof(Config_body)) {
        config.size = sizeof(Config_body);
        printf("[Config] Config Body size is invalid\n");
    }
    auto body = &config.body;
    if (std::isnan(body->haptics_gain) || body->haptics_gain < 1.0f || body->haptics_gain > 2.0f) {
        body->haptics_gain = 1.0f;
        printf("[Config] Haptics Gain value is invalid\n");
    }
    if (body->speaker_volume < 0 || body->speaker_volume > 127) {
        body->speaker_volume = 100;
        printf("[Config] Speaker Volume is invalid\n");
    }
    if (body->headset_volume < 0 || body->headset_volume > 127) {
        body->headset_volume = 100;
        printf("[Config] Headset Volume is invalid\n");
    }
    if (body->sync_spk_headset_volume > 1) {
        body->sync_spk_headset_volume = 0;
        printf("[Config] sync_spk_headset_volume is invalid\n");
    }
    if (body->speaker_gain < 0 || body->speaker_gain > 7) {
        body->speaker_gain = 2;
        printf("[Config] speaker_gain is invalid\n");
    }
    if (body->inactive_time < 5 || body->inactive_time > 60) {
        body->inactive_time = 30;
        printf("[Config] Inactive time is invalid\n");
    }
    if (body->disable_inactive_disconnect > 1) {
        body->disable_inactive_disconnect = 0;
        printf("[Config] disable_auto_disconnect is invalid\n");
    }
    if (body->disable_pico_led > 1) {
        body->disable_pico_led = 0;
        printf("[Config] disable_pico_led is invalid\n");
    }
    if (body->polling_rate_mode > 2) {
        body->polling_rate_mode = 0;
        printf("[Config] polling_rate_mode is invalid\n");
    }
    if (body->audio_buffer_length < 16 || body->audio_buffer_length > 128) {
        body->audio_buffer_length = 64;
        printf("[Config] haptics_buffer_length is invalid\n");
    }
    if (body->controller_mode > 2) {
        body->controller_mode = 2;
        printf("[Config] controller_mode is invalid\n");
    }
    if (body->config_version != CONFIG_VERSION) {
        body->config_version = CONFIG_VERSION;
        printf("[Config] Warning: Config may breaking change\n");
    }
    if (body->lock_volume > 1) {
        body->lock_volume = 0;
        printf("[Config] lock_volume is invalid\n");
    }
    if (body->disable_usb_sn > 1) {
        body->disable_usb_sn = 0;
        printf("[Config] Warning: disable_usb_sn is invalid\n");
    }
    if (body->ps_shortcut_enabled > 1) {
        body->ps_shortcut_enabled = 0;
        printf("[Config] ps_shortcut_enabled is invalid\n");
    }
}

void config_load() {
    memcpy(&config, flash_config(), sizeof(Config));

    config_valid();
}

bool config_save() {
    config.crc32 = calc_config_crc(config);
    alignas(4) uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xff, sizeof(page));
    memcpy(page, &config, sizeof(Config));

    const uint32_t interrupts = save_and_disable_interrupts();
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CONFIG_FLASH_OFFSET, page, sizeof(page));
    restore_interrupts(interrupts);

    Config verify{};
    memcpy(&verify, flash_config(), sizeof(verify));
    const auto verify_crc32 = calc_config_crc(verify);
    if (verify_crc32 == config.crc32) {
        printf("[Config] Config write flash verify success\n");
        return true;
    }
    printf("[Config] Config write flash verify failed\n");
    return false;
}

Config_body& get_config() {
    return config.body;
}

void set_config(const uint8_t *new_config, const uint16_t len) {
    const auto copy_len = len < sizeof(Config_body) ? len : sizeof(Config_body);
    memcpy(&config.body, new_config, copy_len);
    config_valid();
    if (config.body.disable_pico_led) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
    }else {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
    }
    set_volume(config.body.speaker_volume,config.body.headset_volume);
    set_gain(config.body.speaker_gain);
}

void set_config(const Config_body &new_config) {
    config.body = new_config;
    config_valid();
}
