#include "battery_feedback.h"
#include "battery_output_policy.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

using ds5_battery::BatteryFeedback;
using ds5_battery::Output;
using ds5_battery::wire::DispatchChoice;
using ds5_battery::wire::FrameKind;

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

// The hardware CRC helper is Pico-specific. This deterministic stand-in checks
// that a changed packet gets a new checksum without claiming CRC equivalence.
void checksum(std::vector<uint8_t> &bytes) {
    uint32_t value = 0x31415926;
    for (size_t i = 0; i + 4 < bytes.size(); ++i) value = value * 33 + bytes[i];
    for (unsigned i = 0; i != 4; ++i) bytes[bytes.size() - 4 + i] = value >> (8 * i);
}

struct Frame {
    std::vector<uint8_t> bytes;
    FrameKind kind = FrameKind::Other;
    uint32_t epoch = 0;
};

struct Attempt {
    DispatchChoice choice = DispatchChoice::None;
    Frame frame;
    std::vector<uint8_t> before_rewrite;
    bool accepted = false;
    bool checksum_changed = false;
};

// This is a small transport driver, not an alternative light policy. Its event
// order follows bt_write -> queue_try_add and CAN_SEND_NOW -> one FIFO pop or
// one synthetic send in bt.cpp. There is no arbitrary FIFO reordering.
class Transport {
public:
    void open(uint32_t generation) {
        require(!link_open_, "open requires a closed link");
        generation_ = generation;
        link_open_ = true;
        feedback_.on_link_open(generation);
    }

    void close() {
        if (link_open_) feedback_.on_link_closed(generation_);
        link_open_ = false;
        fifo_.clear();
        epoch_ = ds5_battery::wire::LightEpoch{};
        last_restore_mask_ = 0;
        synthetic_attempted_last_ = false;
    }

    void enable(bool value, uint64_t now) {
        feedback_.set_enabled(value, now);
        sync_handoff(now);
    }

    void battery(uint32_t generation, uint8_t raw, uint64_t now) {
        feedback_.on_real_battery_report(generation, raw, now);
        sync_handoff(now);
    }

    bool write_host(uint8_t flags, uint8_t player, uint8_t red, uint8_t game,
                    uint64_t now) {
        Frame frame;
        frame.bytes.resize(79);
        frame.bytes[0] = 0xa2;
        frame.bytes[1] = 0x31;
        frame.bytes[4 + ds5_battery::wire::kFlags1] = flags;
        frame.bytes[4 + ds5_battery::wire::kPlayer] = player;
        frame.bytes[4 + ds5_battery::wire::kRed] = red;
        frame.bytes[4 + 8] = game;
        frame.kind = FrameKind::HostState31;
        return write(std::move(frame), now);
    }

    bool write_audio(uint8_t marker, uint64_t now) {
        Frame frame;
        frame.bytes.resize(143);
        frame.bytes[0] = 0xa2;
        frame.bytes[1] = 0x32;
        // Deliberately resembles a state packet. Origin, not report ID or size,
        // decides whether the battery feature may inspect it.
        frame.bytes[5 + ds5_battery::wire::kFlags1] = 0x1c;
        frame.bytes[5 + ds5_battery::wire::kPlayer] = marker;
        frame.bytes[5 + ds5_battery::wire::kRed] = marker;
        frame.bytes[5 + 8] = marker;
        frame.kind = FrameKind::Other;
        return write(std::move(frame), now);
    }

    Attempt can_send(uint64_t now, bool l2cap_accept) {
        sync_handoff(now);
        const bool has_front = !fifo_.empty();
        const Output output = feedback_.output_at(now);
        Attempt attempt;
        attempt.choice = ds5_battery::wire::choose_next(
            link_open_, has_front, output.synthetic_due, synthetic_attempted_last_);
        if (attempt.choice == DispatchChoice::None) return attempt;

        if (attempt.choice == DispatchChoice::Synthetic) {
            attempt.frame.bytes.resize(143);
            attempt.frame.bytes[0] = 0xa2;
            attempt.frame.bytes[1] = 0x32;
            attempt.frame.bytes[2] = 0x10;
            attempt.frame.bytes[3] = 0x90;
            attempt.frame.bytes[4] = 0x3f;
            attempt.frame.kind = FrameKind::SyntheticState32;
            const auto applied = ds5_battery::wire::apply_output(
                attempt.frame.bytes.data(), attempt.frame.bytes.size(),
                attempt.frame.kind, output);
            require(applied.player || applied.rgb || applied.reset,
                    "synthetic due has at least one light command");
            checksum(attempt.frame.bytes);
            attempt.checksum_changed = true;
            synthetic_attempted_last_ = true;
            attempt.accepted = l2cap_accept;
            const bool restoring = output.player_restore || output.rgb_restore ||
                                   output.reset_restore;
            feedback_.on_send_result(output,
                {l2cap_accept, applied.player, applied.rgb, applied.reset,
                 restoring, false}, now);
            sync_handoff(now);
            finish_handoff();
            return attempt;
        }

        require(!fifo_.empty(), "queued selection has a FIFO head");
        attempt.frame = std::move(fifo_.front());
        fifo_.pop_front(); // bt.cpp pops before l2cap_send, including rejection.
        attempt.before_rewrite = attempt.frame.bytes;
        const bool state = attempt.frame.kind == FrameKind::HostState31 ||
                           attempt.frame.kind == FrameKind::InternalState32;
        ds5_battery::wire::Applied applied{};
        const bool stale = epoch_.stale(attempt.frame.epoch);
        if (state) {
            const bool scrubbed = ds5_battery::wire::scrub_stale_lights(
                attempt.frame.bytes.data(), attempt.frame.bytes.size(),
                attempt.frame.kind, stale, output);
            applied = ds5_battery::wire::apply_output(
                attempt.frame.bytes.data(), attempt.frame.bytes.size(),
                attempt.frame.kind, output);
            if (scrubbed || applied.player || applied.rgb) {
                checksum(attempt.frame.bytes);
                attempt.checksum_changed = true;
            }
        }
        attempt.accepted = l2cap_accept;
        synthetic_attempted_last_ = false;
        if (state) {
            feedback_.on_send_result(output,
                {l2cap_accept, applied.player, applied.rgb, applied.reset,
                 false, stale && applied.reset}, now);
            sync_handoff(now);
        }
        if (epoch_.active() && !feedback_.restore_pending() &&
            (fifo_.empty() || attempt.frame.epoch == epoch_.tag())) {
            epoch_.clear();
        }
        return attempt;
    }

    size_t queued() const { return fifo_.size(); }
    bool restore_pending() const { return feedback_.restore_pending(); }
    Output output(uint64_t now) const { return feedback_.output_at(now); }

private:
    bool write(Frame frame, uint64_t now) {
        if (!link_open_) return false;
        if (frame.kind == FrameKind::HostState31 ||
            frame.kind == FrameKind::InternalState32) {
            const size_t offset = frame.kind == FrameKind::HostState31 ? 4 : 5;
            const auto command = ds5_battery::wire::capture_lights(
                frame.bytes.data() + offset, frame.bytes.size() - offset - 4);
            if (feedback_.observe_base_lights(command, epoch_.active()) &&
                epoch_.active()) {
                epoch_.note_light_intent();
                synthetic_attempted_last_ = false;
                sync_handoff(now);
            }
        }
        frame.epoch = epoch_.tag();
        checksum(frame.bytes);
        if (fifo_.size() == 10) return false;
        fifo_.push_back(std::move(frame));
        return true;
    }

    void sync_handoff(uint64_t now) {
        const auto state = feedback_.output_at(now);
        const uint8_t mask = static_cast<uint8_t>(
            (state.player_restore ? 1 : 0) | (state.rgb_restore ? 2 : 0));
        if ((mask & ~last_restore_mask_) != 0) {
            epoch_.start_handoff();
            synthetic_attempted_last_ = false;
        }
        last_restore_mask_ = mask;
    }

    void finish_handoff() {
        if (epoch_.active() && !feedback_.restore_pending() && fifo_.empty())
            epoch_.clear();
    }

    BatteryFeedback feedback_;
    std::deque<Frame> fifo_;
    ds5_battery::wire::LightEpoch epoch_;
    uint32_t generation_ = 0;
    uint8_t last_restore_mask_ = 0;
    bool link_open_ = false;
    bool synthetic_attempted_last_ = false;
};

uint8_t player(const Attempt &attempt) {
    const size_t offset = attempt.frame.kind == FrameKind::HostState31 ? 4 : 5;
    return attempt.frame.bytes[offset + ds5_battery::wire::kPlayer];
}

uint8_t red(const Attempt &attempt) {
    const size_t offset = attempt.frame.kind == FrameKind::HostState31 ? 4 : 5;
    return attempt.frame.bytes[offset + ds5_battery::wire::kRed];
}

uint8_t flags(const Attempt &attempt) {
    const size_t offset = attempt.frame.kind == FrameKind::HostState31 ? 4 : 5;
    return attempt.frame.bytes[offset + ds5_battery::wire::kFlags1];
}

void default_off_preserves_host_fifo_and_never_sends_synthetic_lights() {
    Transport transport;
    transport.open(100);
    transport.battery(100, 5, 0);
    require(!transport.output(0).synthetic_due &&
            !transport.output(0).player_override,
            "default-off real battery report does not take lights");
    require(transport.write_host(0x1c, 0x15, 0x70, 0x91, 1),
            "default-off host light and ResetLights command enters FIFO");
    const auto host = transport.can_send(2, true);
    require(host.choice == DispatchChoice::Queued && host.accepted &&
            host.frame.bytes == host.before_rewrite &&
            (flags(host) & 0x1c) == 0x1c && player(host) == 0x15 &&
            red(host) == 0x70 && host.frame.bytes[4 + 8] == 0x91,
            "default-off host output keeps its light command and game bytes");
    require(transport.can_send(3, true).choice == DispatchChoice::None,
            "default-off transport has no synthetic light slot");
}

void full_fifo_dropped_latest_intent_cannot_be_undone_by_late_older_packets() {
    Transport transport;
    transport.open(1);
    transport.enable(true, 0);
    transport.battery(1, 3, 0);
    require(transport.can_send(0, true).choice == DispatchChoice::Synthetic,
            "first real report establishes an accepted gauge");
    transport.battery(1, 1, 1'000'000);
    transport.battery(1, 1, 4'000'000);
    const auto pulse = transport.can_send(4'000'000, true);
    require(pulse.choice == DispatchChoice::Synthetic &&
            (flags(pulse) & ds5_battery::wire::kAllowRgb),
            "confirmed 3-to-1 owns RGB before cancellation");

    require(transport.write_host(0x14, 0x01, 0x11, 0xa1, 4'000'001),
            "A enters FIFO while the overlay is active");
    transport.enable(false, 4'000'002);
    require(transport.write_host(0x14, 0x02, 0x22, 0xb2, 4'000'003),
            "B enters FIFO after cancellation");
    for (unsigned i = 0; i != 8; ++i)
        require(transport.write_audio(static_cast<uint8_t>(i), 4'000'004 + i),
                "audio fills remaining FIFO slots");
    require(transport.queued() == 10, "real BT FIFO depth is ten");
    require(!transport.write_host(0x14, 0x03, 0x33, 0xc3, 4'000'012),
            "C is observed before the full FIFO refuses its packet");
    const auto target = transport.output(4'000'013);
    require(target.base_player_byte == 0x03 && target.base_red == 0x33 &&
            target.player_restore && target.rgb_restore,
            "refused C remains the latest qualified handback intent");

    const auto restored = transport.can_send(4'000'014, true);
    require(restored.choice == DispatchChoice::Synthetic && restored.accepted &&
            player(restored) == 0x03 && red(restored) == 0x33 &&
            !transport.restore_pending(),
            "coalesced synthetic slot sends C despite the full FIFO");
    const auto a = transport.can_send(4'000'015, true);
    const auto b = transport.can_send(4'000'016, true);
    require(a.choice == DispatchChoice::Queued && b.choice == DispatchChoice::Queued &&
            a.frame.bytes[4 + 8] == 0xa1 && b.frame.bytes[4 + 8] == 0xb2,
            "A then B retain FIFO game output order");
    require(player(a) == 0x03 && red(a) == 0x33 &&
            player(b) == 0x03 && red(b) == 0x33 &&
            a.checksum_changed && b.checksum_changed &&
            a.frame.bytes != a.before_rewrite &&
            b.frame.bytes != b.before_rewrite,
            "A and B cannot relatch stale lights after accepted C");
    for (unsigned i = 0; i != 8; ++i) {
        const auto audio = transport.can_send(4'000'017 + i, true);
        require(audio.choice == DispatchChoice::Queued &&
                audio.frame.kind == FrameKind::Other &&
                audio.frame.bytes[5 + 8] == i && !audio.checksum_changed &&
                audio.frame.bytes == audio.before_rewrite,
                "audio 0x32 drains in FIFO order without light rewriting");
    }
    for (unsigned i = 0; i != 3; ++i)
        require(transport.write_host(0, 0xff, 0xff,
                                     static_cast<uint8_t>(0xd0 + i), 4'000'100 + i),
                "continuous nonlight game commands can enqueue");
    for (unsigned i = 0; i != 3; ++i) {
        const auto game = transport.can_send(4'000'200 + i, true);
        require(game.choice == DispatchChoice::Queued &&
                game.frame.bytes[4 + 8] == 0xd0 + i &&
                (flags(game) & 0x1c) == 0,
                "continuous game output preserves state without reasserting lights");
    }
}

void reset_lights_is_ordered_and_reasserted_when_it_lands_late() {
    Transport transport;
    transport.open(2);
    transport.enable(true, 0);
    transport.battery(2, 5, 0);
    require(transport.can_send(0, true).accepted,
            "gauge accepted before a queued ResetLights command");
    require(transport.write_host(0x08, 0, 0, 0x41, 1),
            "ResetLights is queued after accepted gauge");
    const auto reset = transport.can_send(2, true);
    require(reset.choice == DispatchChoice::Queued &&
            (flags(reset) & 0x08) != 0 && reset.frame.bytes[4 + 8] == 0x41,
            "queued ResetLights remains an ordered host command");
    const auto gauge = transport.can_send(3, true);
    require(gauge.choice == DispatchChoice::Synthetic &&
            (flags(gauge) & 0x10) && player(gauge) == 0x23,
            "accepted ResetLights is followed by renewed gauge output");

    require(transport.write_host(0x08, 0, 0, 0x42, 4),
            "second ResetLights queues before cancellation");
    require(transport.write_host(0x10, 0x16, 0, 0x43, 5),
            "newer Player command queues after ResetLights");
    transport.enable(false, 6);
    const auto first_restore = transport.can_send(7, true);
    require(first_restore.choice == DispatchChoice::Synthetic &&
            player(first_restore) == 0x16 && !transport.restore_pending(),
            "synthetic handback uses latest known Player intent");
    const auto late_reset = transport.can_send(8, true);
    require(late_reset.choice == DispatchChoice::Queued &&
            (flags(late_reset) & 0x08) && transport.restore_pending(),
            "late FIFO ResetLights reopens handback after it cleared Player");
    const auto second_restore = transport.can_send(9, true);
    require(second_restore.choice == DispatchChoice::Synthetic &&
            player(second_restore) == 0x16 && !transport.restore_pending(),
            "accepted second handback repairs late ResetLights");
    const auto late_player = transport.can_send(10, true);
    require(late_player.choice == DispatchChoice::Queued &&
            player(late_player) == 0x16 && late_player.frame.bytes[4 + 8] == 0x43,
            "remaining host Player packet keeps latest pattern and FIFO order");
}

void rejection_charge_disable_and_disconnect_keep_truthful_state() {
    Transport transport;
    transport.open(3);
    transport.enable(true, 0);
    transport.battery(3, 5, 0);
    require(transport.can_send(0, true).accepted, "active gauge acquired Player");
    require(transport.write_host(0x10, 0x14, 0, 0x50, 1),
            "host Player intent queued before disable");
    transport.enable(false, 2);
    require(transport.write_audio(0x77, 3), "audio 0x32 queues during handback");
    const auto rejected = transport.can_send(4, false);
    require(rejected.choice == DispatchChoice::Synthetic && !rejected.accepted &&
            transport.restore_pending(),
            "BTstack rejection cannot claim Player handback");
    const auto host = transport.can_send(5, false);
    require(host.choice == DispatchChoice::Queued && !host.accepted &&
            host.frame.bytes[4 + 8] == 0x50 && transport.queued() == 1 &&
            transport.restore_pending(),
            "ordinary BTstack refusal drops one FIFO packet but keeps handback intent");
    const auto second_rejection = transport.can_send(6, false);
    require(second_rejection.choice == DispatchChoice::Synthetic &&
            !second_rejection.accepted && transport.restore_pending(),
            "permanent synthetic rejection stays pending after queued failure");
    const auto audio = transport.can_send(7, true);
    require(audio.choice == DispatchChoice::Queued &&
            audio.frame.kind == FrameKind::Other &&
            audio.frame.bytes[5 + 1] == 0x1c &&
            audio.frame.bytes[5 + 43] == 0x77 &&
            audio.frame.bytes[5 + 44] == 0x77 &&
            !audio.checksum_changed &&
            audio.frame.bytes == audio.before_rewrite &&
            transport.restore_pending(),
            "permanent synthetic rejection yields to unmodified audio 0x32");
    const auto recovered = transport.can_send(8, true);
    require(recovered.choice == DispatchChoice::Synthetic && recovered.accepted &&
            player(recovered) == 0x14 && !transport.restore_pending(),
            "later accepted synthetic restores the latest qualified Player intent");

    Transport charging;
    charging.open(4);
    charging.enable(true, 0);
    charging.battery(4, 3, 0);
    require(charging.can_send(0, true).accepted, "charging case begins with owned gauge");
    charging.battery(4, 1, 1'000'000);
    charging.battery(4, 1, 4'000'000);
    const auto pulse = charging.can_send(4'000'000, true);
    require(pulse.accepted && (flags(pulse) & ds5_battery::wire::kAllowRgb),
            "charging case starts a red notification before power transition");
    charging.battery(4, 0x15, 4'000'001);
    require(!charging.output(4'000'001).player_override &&
            !charging.output(4'000'001).rgb_override &&
            charging.restore_pending(),
            "charging report cancels gauge and running pulse immediately");
    const auto charge_restore = charging.can_send(4'000'002, true);
    require(charge_restore.accepted &&
            (flags(charge_restore) & ds5_battery::wire::kResetLights) &&
            !charging.restore_pending() &&
            !charging.output(4'000'002).rgb_override,
            "accepted ResetLights fallback completes charging cancellation");

    Transport disconnected;
    disconnected.open(5);
    disconnected.enable(true, 0);
    disconnected.battery(5, 5, 0);
    require(disconnected.can_send(0, true).accepted, "disconnect case owns Player");
    require(disconnected.write_audio(0x66, 1), "audio queued before link close");
    disconnected.enable(false, 2);
    disconnected.close();
    require(disconnected.queued() == 0 &&
            disconnected.can_send(3, true).choice == DispatchChoice::None &&
            !disconnected.write_host(0x10, 0x01, 0, 0, 4),
            "link close clears FIFO and forbids further sends");
    disconnected.open(6);
    disconnected.enable(true, 5);
    disconnected.battery(5, 1, 6); // Old physical link generation.
    require(!disconnected.output(6).player_override &&
            disconnected.can_send(6, true).choice == DispatchChoice::None,
            "late report from previous link cannot establish new gauge");
}

} // namespace

int main() {
    default_off_preserves_host_fifo_and_never_sends_synthetic_lights();
    full_fifo_dropped_latest_intent_cannot_be_undone_by_late_older_packets();
    reset_lights_is_ordered_and_reasserted_when_it_lands_late();
    rejection_charge_disable_and_disconnect_keep_truthful_state();
    std::puts("battery_transport_sequence_test: PASS");
}
