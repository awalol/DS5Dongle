//
// Created by awalol on 2026/5/15.
//

#include "state_mgr.h"

#include <cstddef>
#include <cstring>

#include "config.h"
#include "utils.h"
#include "bt.h"

extern int reportSeqCounter;

namespace {
    constexpr size_t kAudioControlOffset = offsetof(SetStateData, MuteLightMode) - sizeof(uint8_t);
    constexpr size_t kMuteControlOffset = offsetof(SetStateData, RightTriggerFFB) - sizeof(uint8_t);
    constexpr size_t kMotorPowerLevelOffset = offsetof(SetStateData, HostTimestamp) + sizeof(uint32_t);
    constexpr size_t kAudioControl2Offset = kMotorPowerLevelOffset + sizeof(uint8_t);
    constexpr size_t kHapticLowPassFilterOffset = offsetof(SetStateData, LightFadeAnimation) - 2 * sizeof(uint8_t);
    constexpr size_t kPlayerIndicatorsOffset = offsetof(SetStateData, LedRed) - sizeof(uint8_t);
}

static constexpr uint8_t state_init_data[63] = {
    0xfd, 0xf7, 0x0, 0x0,
    0x0, 0x0, // Headphones, Speaker
    0xff, 0x9, 0x0, 0x0F, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0xa,
    0x7, 0x0, 0x0, 0x2, 0x1,
    0x00,
    0xff, 0xd7, 0x00 // RGB LED: R, G, B (Nijika Color!)✨
};

SetStateData state{};

void state_init() {
    memcpy(&state, state_init_data, sizeof(state));
    state.VolumeSpeaker = get_config().speaker_volume;
    state.VolumeHeadphones = get_config().headset_volume;
    set_volume(get_config().speaker_volume,get_config().headset_volume);
    set_gain(get_config().speaker_gain);
}

void state_set(uint8_t *data, const uint8_t size) {
    if (size > 63) {
        printf("[StateMgr] Warning: State Set over 63 bytes\n");
    }
    memcpy(data, &state, size);
}

void state_update(const uint8_t *data, const uint8_t size) {
    if (size > sizeof(SetStateData)) {
        printf(
            "[StateMgr] Error: SetStateData max %u bytes, request %u\n",
            static_cast<unsigned>(sizeof(SetStateData)),
            size
        );
        return;
    }

    SetStateData update{};
    memcpy(&update, data, size);

    auto *state_bytes = reinterpret_cast<uint8_t *>(&state);
    const auto copy_if_allowed = [&](const bool allowed, const size_t offset, const size_t length) {
        const size_t end = offset + length;
        // offset/length are byte ranges in SetStateData. Skip the copy if the
        // sender did not allow this field, or if a short report does not contain it.
        if (!allowed || end < offset || end > sizeof(state) || end > size) {
            return;
        }

        memcpy(state_bytes + offset, data + offset, length);
    };
    /*auto set_bit = [](uint8_t &byte, const int bit, const bool value) {
        byte = (byte & ~(1 << bit)) | (value << bit);
    };*/

    state.EnableRumbleEmulation = update.EnableRumbleEmulation;
    state.UseRumbleNotHaptics = update.UseRumbleNotHaptics;
    state.EnableImprovedRumbleEmulation = update.EnableImprovedRumbleEmulation;
    copy_if_allowed(
        update.UseRumbleNotHaptics || update.EnableRumbleEmulation,
        offsetof(SetStateData, RumbleEmulationRight),
        2
    );

    if (!get_config().lock_volume && update.AllowHeadphoneVolume) {
        get_config().headset_volume = update.VolumeHeadphones;
        state.VolumeHeadphones = update.VolumeHeadphones;
    }
    if (!get_config().lock_volume && update.AllowSpeakerVolume) {
        get_config().speaker_volume = update.VolumeSpeaker;
        state.VolumeSpeaker = update.VolumeSpeaker;
    }
    copy_if_allowed(
        update.AllowMicVolume,
        offsetof(SetStateData, VolumeMic),
        sizeof(update.VolumeMic)
    );
    copy_if_allowed(
        update.AllowAudioControl,
        kAudioControlOffset,
        sizeof(uint8_t)
    );

    copy_if_allowed(
        update.AllowMuteLight,
        offsetof(SetStateData, MuteLightMode),
        sizeof(update.MuteLightMode)
    );

    copy_if_allowed(
        update.AllowAudioMute,
        kMuteControlOffset,
        sizeof(uint8_t)
    );

    copy_if_allowed(
        update.AllowRightTriggerFFB,
        offsetof(SetStateData, RightTriggerFFB),
        sizeof(update.RightTriggerFFB)
    );
    copy_if_allowed(
        update.AllowLeftTriggerFFB,
        offsetof(SetStateData, LeftTriggerFFB),
        sizeof(update.LeftTriggerFFB)
    );

    copy_if_allowed(
        update.AllowMotorPowerLevel,
        kMotorPowerLevelOffset,
        sizeof(uint8_t)
    );
    copy_if_allowed(
        !get_config().lock_volume && update.AllowAudioControl2,
        kAudioControl2Offset,
        sizeof(uint8_t)
    );
    if (!get_config().lock_volume && update.AllowAudioControl2) {
        get_config().speaker_gain = update.SpeakerCompPreGain;
    }
    copy_if_allowed(
        update.AllowHapticLowPassFilter,
        kHapticLowPassFilterOffset,
        sizeof(uint8_t)
    );

    copy_if_allowed(
        update.AllowColorLightFadeAnimation,
        offsetof(SetStateData, LightFadeAnimation),
        sizeof(update.LightFadeAnimation)
    );
    copy_if_allowed(
        update.AllowLightBrightnessChange,
        offsetof(SetStateData, LightBrightness),
        sizeof(update.LightBrightness)
    );
    copy_if_allowed(
        update.AllowPlayerIndicators,
        kPlayerIndicatorsOffset,
        sizeof(uint8_t)
    );
    copy_if_allowed(
        update.AllowLedColor,
        offsetof(SetStateData, LedRed),
        sizeof(update.LedRed) * 3
    );
}

void set_volume(const uint8_t value) {
    // printf("[StateMgr] SetVolume: %u\n",value);
    if (get_config().sync_spk_headset_volume) {
        state.VolumeSpeaker = value;
        get_config().speaker_volume = value;
    }
    state.VolumeHeadphones = value;
    get_config().headset_volume = value;
}

void set_volume(const uint8_t speaker,const uint8_t headset) {
    state.VolumeSpeaker = speaker;
    state.VolumeHeadphones = headset;
}

void set_gain(const uint8_t value) {
    state.SpeakerCompPreGain = value;
    state.BeamformingEnable = true;
}

static void push_setstate_report(const SetStateData *payload, const bool pulse_reset_lights)
{
    if (!bt_is_connected()) {
        return;
    }
    uint8_t outputData[78]{};
    outputData[0] = 0x31;
    outputData[1] = static_cast<uint8_t>(reportSeqCounter << 4);
    if (++reportSeqCounter == 256) {
        reportSeqCounter = 0;
    }
    outputData[2] = 0x10;
    memcpy(outputData + 3, payload, sizeof(SetStateData));
    if (pulse_reset_lights) {
        outputData[4] |= static_cast<uint8_t>(1u << 3);
    }
    bt_write(outputData, sizeof(outputData));
}

void state_restore_idle_lights(void)
{
    if (!bt_is_connected()) {
        return;
    }
    state_init();
    SetStateData idle{};
    memcpy(&idle, &state, sizeof(SetStateData));
    idle.AllowLedColor = 1;
    idle.AllowColorLightFadeAnimation = 1;
    idle.AllowLightBrightnessChange = 1;

    push_setstate_report(&idle, true);
    idle.ResetLights = 0;
    push_setstate_report(&idle, false);
    memcpy(&state, &idle, sizeof(SetStateData));
}
