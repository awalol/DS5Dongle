//
// Created by awalol on 2026/5/4.
//

#include "config.h"

#include <cmath>
#include <cstring>

#include "bt.h"
#include "status_gpio.h"
#include "utils.h"
#include "port/port.h"

constexpr uint32_t CONFIG_MAGIC = 0x66ccff00;
constexpr uint16_t CONFIG_VERSION = 5; // 如果想要强制重置配置，再更新 CONFIG_VERSION。
static Config config{};
bool is_dse = false;

// 编译期保护
// 判断Config结构体是否能放进flash 256bytes
static_assert(sizeof(Config) <= port::kConfigStoragePageSize);

static uint32_t calc_config_crc(const Config &con) {
    return crc32(reinterpret_cast<const uint8_t *>(&con.body), sizeof(Config_body));
}

const Config *flash_config() {
    return static_cast<const Config *>(port::config_storage_read());
}

// Migration for upstream #244, which moved the config off the last flash sector
// because it collides with BTstack's link-key bank. Reads whatever the port
// reports as the legacy location; targets with no such location return nullptr
// and this is a no-op.
static bool load_old_config() {
    const void *old_addr = port::config_storage_read_legacy();
    if (old_addr == nullptr) return false;
    uint32_t magic_header;
    memcpy(&magic_header, old_addr, sizeof(uint32_t));
    if (magic_header != CONFIG_MAGIC) return false;
    printf("[Config] Trying load old sector config\n");
    memset(&config, 0xFF, sizeof(Config)); // 先进行 0xFF 填充，确保后续缺项能够正常初始化
    memcpy(&config, old_addr, sizeof(Config));
    printf("[Config] Old Config loaded\n");
    return true;
}

void config_valid() {
    // valid config and set default value
    if (config.magic != CONFIG_MAGIC) {
        if (!load_old_config()) {
            config.magic = CONFIG_MAGIC;
            printf("[Config] Config Magic Header is invalid\n");
        }
    }
    if (config.size != sizeof(Config_body)) {
        config.size = sizeof(Config_body);
        printf("[Config] Config Body size is invalid\n");
    }
    auto body = &config.body;
    if (body->config_version != CONFIG_VERSION) {
        memset(body, 0xFF, sizeof(Config_body));
        body->config_version = CONFIG_VERSION;
        printf("[Config] Warning: Config may breaking change. Reset to default\n");
    }
    if (std::isnan(body->haptics_gain) || body->haptics_gain < 1.0f || body->haptics_gain > 2.0f) {
        body->haptics_gain = 1.0f;
        printf("[Config] Haptics Gain value is invalid\n");
    }
    if (body->speaker_volume > 127) {
        body->speaker_volume = 100;
        printf("[Config] Speaker Volume is invalid\n");
    }
    if (body->headset_volume > 127) {
        body->headset_volume = 100;
        printf("[Config] Headset Volume is invalid\n");
    }
    if (body->speaker_gain > 7) {
        body->speaker_gain = 2;
        printf("[Config] speaker_gain is invalid\n");
    }
    if (body->inactive_time > 60) {
        body->inactive_time = 30;
        printf("[Config] Inactive time is invalid\n");
    }
    if (body->disable_pico_led > 1) {
        body->disable_pico_led = 0;
        printf("[Config] disable_pico_led is invalid\n");
    }
    if (body->polling_rate_mode > 2) {
        body->polling_rate_mode = 1;
        printf("[Config] polling_rate_mode is invalid\n");
    }
    if (body->audio_buffer_length < 16 || body->audio_buffer_length > 128) {
        body->audio_buffer_length = 48;
        printf("[Config] haptics_buffer_length is invalid\n");
    }
    if (body->controller_mode > 2) {
        body->controller_mode = 2;
        printf("[Config] controller_mode is invalid\n");
    }
    if (body->enable_usb_sn > 1) {
        body->enable_usb_sn = 0;
        printf("[Config] Warning: enable_usb_sn is invalid\n");
    }
    if (body->ps_shortcut_enabled > 1) {
        body->ps_shortcut_enabled = 0;
        printf("[Config] ps_shortcut_enabled is invalid\n");
    }
    if (body->mic_select > 3) {
        body->mic_select = 0;
        printf("[Config] mic_select is invalid\n");
    }
    if (body->speaker_select > 3) {
        body->speaker_select = 0;
        printf("[Config] speaker_select is invalid\n");
    }
    if (body->enable_wake > 1) {
        body->enable_wake = 0;
        printf("[Config] enable_wake is invalid\n");
    }
    if (body->trigger_reduce > 10) {
        body->trigger_reduce = 0;
        printf("[Config] trigger_reduce is invalid\n");
    }
    if (body->lock_volume > 1) {
        body->lock_volume = 0;
        printf("[Config] lock_volume is invalid\n");
    }
    if (!status_gpio_pin_valid(body->status_gpio_pin)) {
        body->status_gpio_pin = STATUS_GPIO_DISABLED;
        printf("[Config] status_gpio_pin is invalid\n");
    }
    if (body->status_gpio_mode > STATUS_GPIO_MODE_BUTTON) {
        body->status_gpio_mode = 0;
        printf("[Config] status_gpio_mode is invalid\n");
    }
}

void config_load() {
    memcpy(&config, flash_config(), sizeof(Config));

    config_valid();
}

bool config_save() {
    config.crc32 = calc_config_crc(config);

    if (!port::config_storage_write(&config, sizeof(Config))) {
        printf("[Config] config_save storage write failed\n");
        return false;
    }

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
    const bool controller_connected = bt_is_connected();
    gpio_on_disconnect();

    const auto copy_len = len < sizeof(Config_body) ? len : sizeof(Config_body);
    memcpy(&config.body, new_config, copy_len);
    config_valid();

    if (controller_connected && config.body.status_gpio_mode != STATUS_GPIO_MODE_BUTTON) {
        gpio_on_connect();
    } else {
        gpio_on_disconnect();
    }

    port::led_set(!config.body.disable_pico_led);
    SetStateData state{};
    if (config.body.trigger_reduce > 0) {
        state.AllowMotorPowerLevel = 1;
        state.TriggerMotorPowerReduction = config.body.trigger_reduce;
    }
    if (config.body.speaker_gain > 0) {
        state.AllowAudioControl2 = 1;
        state.SpeakerCompPreGain = config.body.speaker_gain;
    }
    state.AllowAudioControl = 1;
    state.MicSelect = config.body.mic_select;
    state.NoiseCancelEnable = 1;
    update_state(state);
}

void set_config(const Config_body &new_config) {
    const bool controller_connected = bt_is_connected();
    gpio_on_disconnect();

    config.body = new_config;
    config_valid();

    if (controller_connected && config.body.status_gpio_mode != STATUS_GPIO_MODE_BUTTON) {
        gpio_on_connect();
    } else {
        gpio_on_disconnect();
    }
}
