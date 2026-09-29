#ifndef DS5_BATTERY_FEEDBACK_H
#define DS5_BATTERY_FEEDBACK_H

#include <cstdint>

namespace ds5_battery {

// The transport supplies only genuine, complete Bluetooth input observations.
// This module owns no Pico, BTstack, USB, or queue objects.
struct LightCommand {
    bool allow_player = false;
    uint8_t player_byte = 0;
    bool allow_rgb = false;
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    bool reset_lights = false;
};

struct Output {
    uint32_t generation = 0;
    bool link_open = false;

    bool player_override = false;
    uint8_t player_byte = 0; // Low five bits: LEDs; bit 5: instantaneous change.
    bool rgb_override = false;
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;

    bool player_restore = false;
    bool rgb_restore = false;
    bool reset_restore = false;
    bool base_player_known = false;
    uint8_t base_player_byte = 0;
    bool base_rgb_known = false;
    uint8_t base_red = 0;
    uint8_t base_green = 0;
    uint8_t base_blue = 0;

    bool synthetic_due = false; // Ask for a coalescible state send if no normal state can carry this.
    uint8_t pulse_index = 0;    // Opaque delivery token: pass Output back to on_send_result.
    uint8_t pulse_phase = 0;
};

struct SendResult {
    bool accepted = false; // True only after BTstack accepts the state packet.
    bool player_applied = false;
    bool rgb_applied = false;
    bool reset_applied = false;
    bool synthetic_restore = false; // Queued host state never completes a handback.
    bool late_handoff_reset = false; // Accepted queued ResetLights after a handback send.
};

class BatteryFeedback {
public:
    void on_link_open(uint32_t generation);
    void on_link_closed(uint32_t generation);
    void set_enabled(bool enabled, uint64_t now_us);
    void on_real_battery_report(uint32_t generation, uint8_t raw, uint64_t now_us);
    // Returns true for a new qualified light intent. During a handoff guard,
    // rearm only the channels changed by this command (both for ResetLights).
    bool observe_base_lights(const LightCommand &command,
                             bool rearm_during_handoff = false);
    Output output_at(uint64_t now_us) const;
    void on_send_result(const Output &sent, const SendResult &result, uint64_t now_us);
    bool restore_pending() const;

private:
    enum class PulsePhase : uint8_t { None, AwaitStart, Rising, Falling };

    void cancel_overlays();
    void start_pulses(uint8_t tier);
    uint8_t pulse_intensity(uint64_t now_us) const;

    // Group timestamps to avoid padding between the always-resident fields.
    uint64_t candidate_since_us_ = 0;
    uint64_t low_phase_start_us_ = 0;
    uint64_t pulse_phase_since_us_ = 0;
    uint64_t last_rgb_send_us_ = 0;
    uint32_t generation_ = 0;

    bool enabled_ = false;
    bool link_open_ = false;
    bool confirmed_valid_ = false;
    uint8_t confirmed_level_ = 0;
    bool candidate_valid_ = false;
    uint8_t candidate_level_ = 0;

    bool player_owned_ = false;
    bool rgb_owned_ = false;
    bool player_restore_pending_ = false;
    bool rgb_restore_pending_ = false;
    bool restore_reset_sent_ = false;

    bool base_player_known_ = false;
    uint8_t base_player_byte_ = 0;
    bool base_rgb_known_ = false;
    uint8_t base_red_ = 0;
    uint8_t base_green_ = 0;
    uint8_t base_blue_ = 0;
    // A ResetLights command invalidates each earlier channel independently.
    // Track when the latest value shared a packet with that ResetLights.
    bool base_player_same_reset_ = false;
    bool base_rgb_same_reset_ = false;
    bool reassert_player_ = false;
    bool reassert_rgb_ = false;

    PulsePhase pulse_phase_ = PulsePhase::None;
    uint8_t pulse_index_ = 0;
    uint8_t notification_tier_ = 0;

    bool last_player_sent_ = false;
    uint8_t last_player_byte_ = 0;
    bool last_rgb_sent_ = false;
    uint8_t last_red_ = 0;
    uint8_t last_green_ = 0;
    uint8_t last_blue_ = 0;
};

} // namespace ds5_battery

#endif
