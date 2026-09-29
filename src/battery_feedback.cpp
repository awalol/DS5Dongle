#include "battery_feedback.h"

namespace ds5_battery {
namespace {
constexpr uint64_t kConfirmUs = 3'000'000;
constexpr uint64_t kBlinkUs = 500'000;
constexpr uint64_t kPulseHalfUs = 800'000;
constexpr uint64_t kRgbStepUs = 50'000;
constexpr uint8_t kGaugeMasks[] = {0, 0x01, 0x03, 0x0b, 0x1b};

constexpr uint8_t tier_for_level(uint8_t level) {
    return level >= 9 ? 4 : level >= 6 ? 3 : level >= 3 ? 2 : 1;
}

uint64_t elapsed(uint64_t now_us, uint64_t since_us) {
    return now_us >= since_us ? now_us - since_us : 0;
}
} // namespace

void BatteryFeedback::on_link_open(uint32_t generation) {
    const bool enabled = enabled_;
    *this = BatteryFeedback{};
    enabled_ = enabled;
    generation_ = generation;
    link_open_ = true;
}

void BatteryFeedback::on_link_closed(uint32_t generation) {
    if (!link_open_ || generation != generation_) return;
    const bool enabled = enabled_;
    *this = BatteryFeedback{};
    enabled_ = enabled;
    generation_ = generation;
}

void BatteryFeedback::cancel_overlays() {
    const bool changed = player_owned_ || rgb_owned_ || confirmed_valid_ ||
                         candidate_valid_ || pulse_phase_ != PulsePhase::None ||
                         reassert_player_ || reassert_rgb_;
    if (changed) {
        restore_reset_sent_ = false;
    }
    player_restore_pending_ |= player_owned_;
    rgb_restore_pending_ |= rgb_owned_;
    player_owned_ = false;
    rgb_owned_ = false;
    confirmed_valid_ = false;
    candidate_valid_ = false;
    pulse_phase_ = PulsePhase::None;
    notification_tier_ = 0;
    reassert_player_ = false;
    reassert_rgb_ = false;
}

void BatteryFeedback::set_enabled(bool enabled, uint64_t now_us) {
    (void)now_us;
    if (enabled == enabled_) return;
    enabled_ = enabled;
    if (!enabled) {
        cancel_overlays();
    } else {
        // The next genuine discharge report establishes a new baseline.
        confirmed_valid_ = false;
        candidate_valid_ = false;
    }
}

void BatteryFeedback::start_pulses(uint8_t tier) {
    pulse_phase_ = PulsePhase::AwaitStart;
    pulse_index_ = 0;
    notification_tier_ = tier;
    reassert_rgb_ = true;
}

void BatteryFeedback::on_real_battery_report(uint32_t generation, uint8_t raw,
                                              uint64_t now_us) {
    if (!link_open_ || generation != generation_) return;
    const uint8_t power = raw >> 4;
    const uint8_t level = raw & 0x0f;
    if (power == 1 || power == 2) {
        cancel_overlays(); // Charging/Complete cancel in this report, with no debounce.
        return;
    }
    if (power != 0 || level > 10) {
        candidate_valid_ = false;
        return;
    }
    if (!enabled_) return;
    if (!confirmed_valid_) {
        confirmed_valid_ = true;
        confirmed_level_ = level;
        candidate_valid_ = false;
        low_phase_start_us_ = now_us;
        reassert_player_ = true;
        return; // Initial confirmation is gauge only, even at raw level one.
    }
    if (level == confirmed_level_) {
        candidate_valid_ = false;
        return;
    }
    if (!candidate_valid_ || candidate_level_ != level) {
        candidate_valid_ = true;
        candidate_level_ = level;
        candidate_since_us_ = now_us;
        return;
    }
    if (elapsed(now_us, candidate_since_us_) < kConfirmUs) return;

    const uint8_t previous = confirmed_level_;
    confirmed_level_ = level;
    candidate_valid_ = false;
    if (level <= 2) low_phase_start_us_ = now_us;
    reassert_player_ = true;
    const uint8_t tier = tier_for_level(level);
    if (level != 0 && tier < tier_for_level(previous)) start_pulses(tier);
}

bool BatteryFeedback::observe_base_lights(const LightCommand &command,
                                          bool rearm_during_handoff) {
    if (!link_open_ || (!command.allow_player && !command.allow_rgb && !command.reset_lights))
        return false;
    const bool player_changed = command.allow_player &&
        (!base_player_known_ || base_player_byte_ != command.player_byte ||
         (base_player_same_reset_ && !command.reset_lights));
    const bool rgb_changed = command.allow_rgb &&
        (!base_rgb_known_ || base_red_ != command.red ||
         base_green_ != command.green || base_blue_ != command.blue ||
         (base_rgb_same_reset_ && !command.reset_lights));
    const bool changed = command.reset_lights || player_changed || rgb_changed;
    // A new RGB-only or Player-only target does not undo an already accepted
    // ResetLights release for a different, still-unknown channel.
    if (command.reset_lights) restore_reset_sent_ = false;
    if (command.reset_lights) {
        base_player_known_ = command.allow_player;
        base_rgb_known_ = command.allow_rgb;
        base_player_same_reset_ = command.allow_player;
        base_rgb_same_reset_ = command.allow_rgb;
        reassert_player_ |= confirmed_valid_ && enabled_;
        reassert_rgb_ |= pulse_phase_ != PulsePhase::None;
    }
    if (command.allow_player) {
        base_player_known_ = true;
        base_player_byte_ = command.player_byte;
        if (!command.reset_lights) base_player_same_reset_ = false;
        reassert_player_ |= player_changed && confirmed_valid_ && enabled_;
    }
    if (command.allow_rgb) {
        base_rgb_known_ = true;
        base_red_ = command.red;
        base_green_ = command.green;
        base_blue_ = command.blue;
        if (!command.reset_lights) base_rgb_same_reset_ = false;
        reassert_rgb_ |= rgb_changed && pulse_phase_ != PulsePhase::None;
    }
    if (rearm_during_handoff) {
        if (command.reset_lights) {
            player_restore_pending_ = true;
            rgb_restore_pending_ = true;
        } else {
            player_restore_pending_ |= player_changed;
            rgb_restore_pending_ |= rgb_changed;
        }
    }
    return changed;
}

uint8_t BatteryFeedback::pulse_intensity(uint64_t now_us) const {
    switch (pulse_phase_) {
    case PulsePhase::AwaitStart:
        return 0;
    case PulsePhase::Rising: {
        const uint64_t age = elapsed(now_us, pulse_phase_since_us_);
        return age >= kPulseHalfUs ? 255 : static_cast<uint8_t>(age * 255 / kPulseHalfUs);
    }
    case PulsePhase::Falling: {
        const uint64_t age = elapsed(now_us, pulse_phase_since_us_);
        return age >= kPulseHalfUs ? 0 : static_cast<uint8_t>((kPulseHalfUs - age) * 255 / kPulseHalfUs);
    }
    case PulsePhase::None:
        return 0;
    }
    return 0;
}

Output BatteryFeedback::output_at(uint64_t now_us) const {
    Output output{};
    output.generation = generation_;
    output.link_open = link_open_;
    if (!link_open_) return output;

    output.base_player_known = base_player_known_;
    output.base_player_byte = base_player_byte_;
    output.base_rgb_known = base_rgb_known_;
    output.base_red = base_red_;
    output.base_green = base_green_;
    output.base_blue = base_blue_;

    output.player_override = enabled_ && confirmed_valid_;
    if (output.player_override) {
        const uint8_t tier = tier_for_level(confirmed_level_);
        const uint8_t mask = tier == 1
            ? (elapsed(now_us, low_phase_start_us_) % kBlinkUs < kBlinkUs / 2 ? 1 : 0)
            : kGaugeMasks[tier];
        output.player_byte = static_cast<uint8_t>(0x20 | mask);
    }

    output.rgb_override = enabled_ && pulse_phase_ != PulsePhase::None;
    if (output.rgb_override) {
        const uint8_t intensity = pulse_intensity(now_us);
        if (notification_tier_ == 3) {
            output.blue = intensity;
        } else if (notification_tier_ == 2) {
            output.red = intensity;
            output.green = intensity;
        } else {
            output.red = intensity;
        }
        output.pulse_index = pulse_index_;
        output.pulse_phase = static_cast<uint8_t>(pulse_phase_);
    }

    output.player_restore = player_restore_pending_ && !output.player_override;
    output.rgb_restore = rgb_restore_pending_ && !output.rgb_override;
    // An unknown base needs ResetLights as its release fallback. If the latest
    // base event combined ResetLights with explicit fields, preserve both bits;
    // their on-device precedence remains a hardware qualification question.
    output.reset_restore = !restore_reset_sent_ &&
        ((output.player_restore && !output.base_player_known) ||
         (output.rgb_restore && !output.base_rgb_known) ||
         (output.player_restore && base_player_same_reset_) ||
         (output.rgb_restore && base_rgb_same_reset_));

    const bool player_due = output.player_override &&
        (reassert_player_ || !last_player_sent_ || output.player_byte != last_player_byte_);
    const bool pulse_boundary = pulse_phase_ == PulsePhase::AwaitStart ||
        (pulse_phase_ == PulsePhase::Rising &&
         elapsed(now_us, pulse_phase_since_us_) >= kPulseHalfUs) ||
        (pulse_phase_ == PulsePhase::Falling &&
         elapsed(now_us, pulse_phase_since_us_) >= kPulseHalfUs);
    const bool rgb_changed = !last_rgb_sent_ || output.red != last_red_ ||
        output.green != last_green_ || output.blue != last_blue_;
    const bool rgb_due = output.rgb_override &&
        (reassert_rgb_ || pulse_boundary ||
         (rgb_changed && (!last_rgb_sent_ || elapsed(now_us, last_rgb_send_us_) >= kRgbStepUs)));
    output.synthetic_due = output.reset_restore ||
        (output.player_restore && output.base_player_known) ||
        (output.rgb_restore && output.base_rgb_known) || player_due || rgb_due;
    return output;
}

void BatteryFeedback::on_send_result(const Output &sent, const SendResult &result,
                                     uint64_t now_us) {
    if (!result.accepted || !link_open_ || sent.generation != generation_) return;

    if (sent.player_override && result.player_applied) {
        player_owned_ = true;
        last_player_sent_ = true;
        last_player_byte_ = sent.player_byte;
        reassert_player_ = false;
        player_restore_pending_ = false; // New takeover supersedes an older restore.
    }
    if (sent.rgb_override && result.rgb_applied) {
        rgb_owned_ = true;
        last_rgb_sent_ = true;
        last_red_ = sent.red;
        last_green_ = sent.green;
        last_blue_ = sent.blue;
        last_rgb_send_us_ = now_us;
        reassert_rgb_ = false;
        rgb_restore_pending_ = false;
    }
    // An accepted host ResetLights can clear an active overlay after a prior
    // synthetic frame; the next state send must reapply it.
    if (result.reset_applied) {
        reassert_player_ |= sent.player_override;
        reassert_rgb_ |= sent.rgb_override;
    }
    // A queued packet never certifies handback, even if its light bytes happen
    // to match the latest base intent. Its ResetLights may instead undo a
    // previously accepted synthetic restore.
    if (result.late_handoff_reset && result.reset_applied) {
        player_restore_pending_ |= base_player_known_;
        rgb_restore_pending_ |= base_rgb_known_;
        restore_reset_sent_ = false;
    }
    if (result.synthetic_restore) {
        if (sent.reset_restore && result.reset_applied) {
            restore_reset_sent_ = true;
            // ResetLights clears both channels, including one we did not own.
            if (sent.base_player_known) player_restore_pending_ = true;
            else player_restore_pending_ = false;
            if (sent.base_rgb_known) rgb_restore_pending_ = true;
            else rgb_restore_pending_ = false;
        } else {
            if (sent.player_restore && result.player_applied) player_restore_pending_ = false;
            if (sent.rgb_restore && result.rgb_applied) rgb_restore_pending_ = false;
        }
        if (!player_restore_pending_ && !rgb_restore_pending_) restore_reset_sent_ = false;
    }

    if (!sent.rgb_override || !result.rgb_applied ||
        sent.pulse_index != pulse_index_ ||
        sent.pulse_phase != static_cast<uint8_t>(pulse_phase_)) return;
    if (pulse_phase_ == PulsePhase::AwaitStart) {
        pulse_phase_ = PulsePhase::Rising;
        pulse_phase_since_us_ = now_us;
    } else if (pulse_phase_ == PulsePhase::Rising &&
               (sent.red | sent.green | sent.blue) == 255) {
        pulse_phase_ = PulsePhase::Falling;
        pulse_phase_since_us_ = now_us;
    } else if (pulse_phase_ == PulsePhase::Falling &&
               (sent.red | sent.green | sent.blue) == 0) {
        if (pulse_index_ < 4) {
            ++pulse_index_;
            pulse_phase_ = PulsePhase::Rising;
            pulse_phase_since_us_ = now_us;
        } else {
            pulse_phase_ = PulsePhase::None;
            notification_tier_ = 0;
            rgb_owned_ = false;
            rgb_restore_pending_ = true;
        }
    }
}

bool BatteryFeedback::restore_pending() const {
    return link_open_ && (player_restore_pending_ || rgb_restore_pending_);
}

} // namespace ds5_battery
