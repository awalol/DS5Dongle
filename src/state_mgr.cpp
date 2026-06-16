//
// Created by awalol on 2026/5/15.
//

#include "state_mgr.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "utils.h"
#include "bt.h"
#include "pico/time.h"

extern int reportSeqCounter;
extern uint8_t interrupt_in_data[63];
extern bool spk_active;

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

namespace {
    constexpr size_t kSetStateSize = 63;
    constexpr uint32_t kMinSensorTimestamp = 10200000u;
    constexpr uint32_t kPostGameLightDelayMs = 2000;

    constexpr size_t kAudioControlOffset = offsetof(SetStateData, MuteLightMode) - sizeof(uint8_t);
    constexpr size_t kMuteControlOffset = offsetof(SetStateData, RightTriggerFFB) - sizeof(uint8_t);
    constexpr size_t kMotorPowerLevelOffset = offsetof(SetStateData, HostTimestamp) + sizeof(uint32_t);
    constexpr size_t kAudioControl2Offset = kMotorPowerLevelOffset + sizeof(uint8_t);
    constexpr size_t kHapticLowPassFilterOffset = offsetof(SetStateData, LightFadeAnimation) - 2 * sizeof(uint8_t);
    constexpr size_t kPlayerIndicatorsOffset = offsetof(SetStateData, LedRed) - sizeof(uint8_t);

    absolute_time_t post_game_lights_at = nil_time;

    void clear_light_allows(SetStateData *payload)
    {
        payload->AllowLedColor = 0;
        payload->AllowColorLightFadeAnimation = 0;
        payload->AllowLightBrightnessChange = 0;
        payload->AllowPlayerIndicators = 0;
        payload->AllowMuteLight = 0;
        payload->ResetLights = 0;
    }

    uint32_t controller_sensor_timestamp(void)
    {
        uint32_t ts = 0;
        memcpy(&ts, interrupt_in_data + 27, sizeof(ts));
        if (ts < kMinSensorTimestamp) {
            ts = kMinSensorTimestamp;
        }
        return ts;
    }

    void push_setstate_report(const SetStateData *payload, const bool pulse_reset_lights)
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

    void apply_idle_light_profile(SetStateData *payload)
    {
        // Gold bar bytes live at indices 52-54 in state_init_data, not at LedRed
        // (offset 44). Assign explicitly — same as the pre-refactor restore path.
        payload->LedRed = 0xff;
        payload->LedGreen = 0xd7;
        payload->LedBlue = 0x00;
        payload->LightFadeAnimation = LightFadeAnimation::Nothing;
        payload->LightBrightness = LightBrightness::Mid;
        payload->PlayerLight1 = 0;
        payload->PlayerLight2 = 0;
        payload->PlayerLight3 = 0;
        payload->PlayerLight4 = 0;
        payload->PlayerLight5 = 0;
        payload->PlayerLightFade = 0;
    }

    void apply_post_game_lights(void)
    {
        if (!bt_is_connected()) {
            return;
        }

        apply_idle_light_profile(&state);

        SetStateData out{};
        memcpy(&out, &state, sizeof(SetStateData));
        out.AllowLedColor = 1;
        out.AllowColorLightFadeAnimation = 1;
        out.AllowLightBrightnessChange = 1;
        out.AllowMuteLight = 0;
        out.ResetLights = 0;
        out.HostTimestamp = controller_sensor_timestamp();

        push_setstate_report(&out, true);
        out.ResetLights = 0;
        out.HostTimestamp = controller_sensor_timestamp();
        push_setstate_report(&out, false);
    }
}

void state_init() {
    memcpy(&state, state_init_data, sizeof(state));
    state.VolumeSpeaker = get_config().speaker_volume;
    state.VolumeHeadphones = get_config().headset_volume;
    set_volume(get_config().speaker_volume, get_config().headset_volume);
    set_gain(get_config().speaker_gain);
}

void state_set(uint8_t *data, const uint8_t size) {
    if (size > kSetStateSize) {
        printf("[StateMgr] Warning: State Set over %u bytes\n", kSetStateSize);
    }
    memcpy(data, &state, size);
}

void state_embed_for_audio(uint8_t *data, const uint8_t size)
{
    if (size > kSetStateSize) {
        printf("[StateMgr] Warning: State embed over %u bytes\n", kSetStateSize);
    }
    memcpy(data, &state, size);
    if (!spk_active) {
        clear_light_allows(reinterpret_cast<SetStateData *>(data));
    }
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
        if (!allowed || end < offset || end > sizeof(state) || end > size) {
            return;
        }

        memcpy(state_bytes + offset, data + offset, length);
    };

    state.EnableRumbleEmulation = update.EnableRumbleEmulation;
    state.UseRumbleNotHaptics = update.UseRumbleNotHaptics;
    state.EnableImprovedRumbleEmulation = update.EnableImprovedRumbleEmulation;
    if (update.RumbleEmulationLeft > 0 || update.RumbleEmulationRight > 0) {
        state.UseRumbleNotHaptics = true;
    }
    if (state.EnableRumbleEmulation ||
        state.UseRumbleNotHaptics ||
        state.EnableImprovedRumbleEmulation) {
        state.RumbleEmulationLeft = update.RumbleEmulationLeft;
        state.RumbleEmulationRight = update.RumbleEmulationRight;
    }

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
    if (get_config().sync_spk_headset_volume) {
        state.VolumeSpeaker = value;
        get_config().speaker_volume = value;
    }
    state.VolumeHeadphones = value;
    get_config().headset_volume = value;
}

void set_volume(const uint8_t speaker, const uint8_t headset) {
    state.VolumeSpeaker = speaker;
    state.VolumeHeadphones = headset;
}

void set_gain(const uint8_t value) {
    state.SpeakerCompPreGain = value;
    state.BeamformingEnable = true;
}

void state_note_speaker_alt(const bool was_active, const bool now_active)
{
    if (now_active) {
        post_game_lights_at = nil_time;
        return;
    }
    if (was_active) {
        post_game_lights_at = make_timeout_time_ms(kPostGameLightDelayMs);
    }
}

void state_post_game_task(void)
{
    if (spk_active || is_nil_time(post_game_lights_at) || !time_reached(post_game_lights_at)) {
        return;
    }
    post_game_lights_at = nil_time;
    apply_post_game_lights();
}
