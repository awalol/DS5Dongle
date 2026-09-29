#ifndef DS5_BATTERY_OUTPUT_POLICY_H
#define DS5_BATTERY_OUTPUT_POLICY_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "battery_feedback.h"

namespace ds5_battery::wire {

enum class FrameKind : uint8_t {
    Other,
    HostState31,
    InternalState32,
    SyntheticState32,
};

// Bytes within SetStateData, shared by USB 0x02, BT 0x31, and state-type BT 0x32.
constexpr size_t kStateBytes = 63;
constexpr size_t kStandardStateBytes = 47;
constexpr size_t kFlags1 = 1;
constexpr size_t kPlayer = 43;
constexpr size_t kRed = 44;
constexpr size_t kGreen = 45;
constexpr size_t kBlue = 46;
constexpr uint8_t kAllowRgb = 0x04;
constexpr uint8_t kResetLights = 0x08;
constexpr uint8_t kAllowPlayer = 0x10;

struct Applied {
    bool player = false;
    bool rgb = false;
    bool reset = false;
};

enum class DispatchChoice : uint8_t { None, Queued, Synthetic };

class CanSendRequest {
public:
    template <typename RequestFn>
    uint8_t issue(RequestFn request) {
        pending_ = true;
        const uint8_t status = request();
        if (status != 0) pending_ = false;
        return status;
    }
    void delivered() { pending_ = false; }
    void clear() { pending_ = false; }
    bool pending() const { return pending_; }

private:
    bool pending_ = false;
};

// One scalar per connection. An observed host light command advances the
// handback epoch even if its subsequent FIFO enqueue is refused.
class LightEpoch {
public:
    void start_handoff() {
        ++current_;
        handoff_ = true;
    }
    void note_light_intent() {
        if (handoff_) ++current_;
    }
    uint32_t tag() const { return current_; }
    bool stale(uint32_t tag) const { return handoff_ && tag != current_; }
    bool active() const { return handoff_; }
    void clear() { handoff_ = false; }

private:
    uint32_t current_ = 0;
    bool handoff_ = false;
};

inline bool request_needed(bool link_open, bool already_requested, uint64_t now_us,
                           uint64_t retry_after_us, bool fifo_pending, bool synthetic_due) {
    return link_open && !already_requested && now_us >= retry_after_us &&
           (fifo_pending || synthetic_due);
}

inline DispatchChoice choose_next(bool link_open, bool has_front, bool synthetic_due,
                                  bool synthetic_attempted) {
    if (!link_open) return DispatchChoice::None;
    if (synthetic_due && (!has_front || !synthetic_attempted)) {
        return DispatchChoice::Synthetic;
    }
    return has_front ? DispatchChoice::Queued : DispatchChoice::None;
}

inline bool copy_host_state(const uint8_t *report, size_t length,
                            std::array<uint8_t, kStateBytes> &state) {
    if (report == nullptr || length < 1 + kStandardStateBytes ||
        length > 1 + kStateBytes || report[0] != 0x02) {
        return false;
    }
    state.fill(0);
    const size_t copied = length - 1 < state.size() ? length - 1 : state.size();
    std::memcpy(state.data(), report + 1, copied);
    return true;
}

inline LightCommand capture_lights(const uint8_t *state, size_t length) {
    LightCommand command{};
    if (state == nullptr || length < kStandardStateBytes) return command;
    const uint8_t flags = state[kFlags1];
    command.allow_player = (flags & kAllowPlayer) != 0;
    if (command.allow_player) command.player_byte = state[kPlayer];
    command.allow_rgb = (flags & kAllowRgb) != 0;
    if (command.allow_rgb) {
        command.red = state[kRed];
        command.green = state[kGreen];
        command.blue = state[kBlue];
    }
    command.reset_lights = (flags & kResetLights) != 0;
    return command;
}

// A queued state may arrive after a newer restore, including when the latest
// host light command was not enqueued. Normalize only its qualified lights.
inline bool scrub_stale_lights(uint8_t *packet, size_t length, FrameKind kind,
                               bool stale, const Output &latest) {
    if (!stale || packet == nullptr || length == 0 || packet[0] != 0xa2) return false;

    size_t state_offset = 0;
    if (kind == FrameKind::HostState31 && length > 1 && packet[1] == 0x31) {
        state_offset = 4;
    } else if (kind == FrameKind::InternalState32 && length > 1 && packet[1] == 0x32) {
        state_offset = 5;
    } else {
        return false;
    }
    if (length < state_offset + kStandardStateBytes + 4) return false;
    uint8_t *state = packet + state_offset;
    bool changed = false;
    if (state[kFlags1] & kAllowPlayer) {
        if (latest.base_player_known) {
            changed |= state[kPlayer] != latest.base_player_byte;
            state[kPlayer] = latest.base_player_byte;
        } else {
            state[kFlags1] &= static_cast<uint8_t>(~kAllowPlayer);
            changed = true;
        }
    }
    if (state[kFlags1] & kAllowRgb) {
        if (latest.base_rgb_known) {
            changed |= state[kRed] != latest.base_red ||
                       state[kGreen] != latest.base_green ||
                       state[kBlue] != latest.base_blue;
            state[kRed] = latest.base_red;
            state[kGreen] = latest.base_green;
            state[kBlue] = latest.base_blue;
        } else {
            state[kFlags1] &= static_cast<uint8_t>(~kAllowRgb);
            changed = true;
        }
    }
    return changed;
}

inline Applied apply_output(uint8_t *packet, size_t length, FrameKind kind,
                             const Output &output) {
    Applied applied{};
    if (packet == nullptr || length == 0 || packet[0] != 0xa2) return applied;

    size_t state_offset = 0;
    if (kind == FrameKind::HostState31 && length > 1 && packet[1] == 0x31) {
        state_offset = 4;
    } else if ((kind == FrameKind::InternalState32 || kind == FrameKind::SyntheticState32) &&
               length > 1 && packet[1] == 0x32) {
        state_offset = 5;
    } else {
        return applied;
    }
    if (length < state_offset + kStandardStateBytes + 4) return applied;
    uint8_t *state = packet + state_offset;

    // Only the standalone synthetic state can complete handback. Queued host
    // states keep their own Allow and ResetLights commands in FIFO order.
    if (kind == FrameKind::SyntheticState32 && output.reset_restore) {
        state[kFlags1] = static_cast<uint8_t>(
            (state[kFlags1] & ~(kAllowPlayer | kAllowRgb)) | kResetLights);
        applied.reset = true;
        return applied;
    }

    // A host ResetLights remains its own ordered command. A later state send
    // may reassert the active overlay or the latest restore target.
    if ((state[kFlags1] & kResetLights) != 0) {
        applied.reset = true;
        return applied;
    }

    if (output.player_override) {
        state[kFlags1] |= kAllowPlayer;
        state[kPlayer] = static_cast<uint8_t>((state[kPlayer] & 0xc0) |
                                               (output.player_byte & 0x3f));
        applied.player = true;
    } else if (kind == FrameKind::SyntheticState32 && output.player_restore &&
               output.base_player_known) {
        state[kFlags1] |= kAllowPlayer;
        state[kPlayer] = output.base_player_byte;
        applied.player = true;
    }

    if (output.rgb_override) {
        state[kFlags1] |= kAllowRgb;
        state[kRed] = output.red;
        state[kGreen] = output.green;
        state[kBlue] = output.blue;
        applied.rgb = true;
    } else if (kind == FrameKind::SyntheticState32 && output.rgb_restore &&
               output.base_rgb_known) {
        state[kFlags1] |= kAllowRgb;
        state[kRed] = output.base_red;
        state[kGreen] = output.base_green;
        state[kBlue] = output.base_blue;
        applied.rgb = true;
    }
    return applied;
}

} // namespace ds5_battery::wire

#endif
