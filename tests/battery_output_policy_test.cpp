#include "battery_output_policy.h"
#include "bt.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>

using ds5_battery::Output;
using ds5_battery::wire::FrameKind;

static void require(bool condition, const char *name) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", name);
        std::exit(1);
    }
}

int main() {
    require(bt_output_payload_fits(547),
            "largest audio payload plus A2 prefix fits the fixed send packet");
    require(!bt_output_payload_fits(548),
            "a payload one byte above the audio maximum is rejected");
    require(bt_output_payload_fits(142) && bt_output_payload_fits(78),
            "internal state and host state remain within send capacity");

    std::array<uint8_t, 63> state{};
    std::array<uint8_t, 64> host{};
    host[0] = 0x02;
    host[1] = 0x5a;
    host[47] = 0x7b;
    host[63] = 0x9c;

    require(!ds5_battery::wire::copy_host_state(host.data(), 47, state),
            "short DS5 payload rejected");
    require(state[0] == 0, "rejected payload leaves state alone");
    require(ds5_battery::wire::copy_host_state(host.data(), 48, state),
            "47-byte DS5 state accepted");
    require(state[0] == 0x5a && state[46] == 0x7b && state[62] == 0,
            "DS5 state copied to exact bound and missing Edge tail zeroed");
    require(ds5_battery::wire::copy_host_state(host.data(), 64, state),
            "63-byte Edge state accepted");
    require(state[62] == 0x9c, "Edge tail included");
    std::array<uint8_t, 65> oversized_host{};
    oversized_host[0] = 0x02;
    require(!ds5_battery::wire::copy_host_state(oversized_host.data(), oversized_host.size(), state),
            "oversized host output rejected instead of silently truncating a malformed report");
    host[0] = 0x03;
    require(!ds5_battery::wire::copy_host_state(host.data(), 64, state),
            "wrong report ID rejected");
    host[0] = 0x02;

    state.fill(0);
    state[43] = 0x35;
    state[44] = 0xe1;
    state[45] = 0x42;
    state[46] = 0x17;
    auto command = ds5_battery::wire::capture_lights(state.data(), state.size());
    require(!command.allow_player && !command.allow_rgb && !command.reset_lights,
            "unqualified light bytes are not commands");
    state[1] = 0x18;
    command = ds5_battery::wire::capture_lights(state.data(), state.size());
    require(command.allow_player && command.player_byte == 0x35 &&
            !command.allow_rgb && command.reset_lights,
            "Player allow and ResetLights are independent");
    state[1] = 0x04;
    command = ds5_battery::wire::capture_lights(state.data(), state.size());
    require(!command.allow_player && command.allow_rgb &&
            command.red == 0xe1 && command.green == 0x42 && command.blue == 0x17 &&
            !command.reset_lights, "RGB allow captures only qualified color");

    std::array<uint8_t, 79> host_packet{};
    host_packet[0] = 0xa2;
    host_packet[1] = 0x31;
    host_packet[4 + 8] = 0xa7; // Rumble/other state must survive final overlay.
    host_packet[4 + 43] = 0x80;
    Output active{};
    active.player_override = true;
    active.player_byte = 0x23;
    active.rgb_override = true;
    active.red = 0xff;
    active.green = 0;
    active.blue = 0;
    auto applied = ds5_battery::wire::apply_output(host_packet.data(), host_packet.size(),
                                                    FrameKind::HostState31, active);
    require(applied.player && applied.rgb && !applied.reset,
            "host state receives both active overrides");
    require(host_packet[4 + 1] == 0x14 && host_packet[4 + 43] == 0xa3 &&
            host_packet[4 + 44] == 0xff && host_packet[4 + 45] == 0 &&
            host_packet[4 + 46] == 0 && host_packet[4 + 8] == 0xa7,
            "overlay sets only qualified light fields and preserves other state");

    std::array<uint8_t, 143> internal{};
    internal[0] = 0xa2;
    internal[1] = 0x32;
    internal[5 + 8] = 0x6a;
    auto untouched = ds5_battery::wire::apply_output(internal.data(), internal.size(),
                                                      FrameKind::Other, active);
    require(!untouched.player && !untouched.rgb && internal[5 + 1] == 0,
            "audio 0x32 never treated as state by length or report ID");
    applied = ds5_battery::wire::apply_output(internal.data(), internal.size(),
                                               FrameKind::InternalState32, active);
    require(applied.player && applied.rgb && internal[5 + 43] == 0x23 &&
            internal[5 + 8] == 0x6a, "explicit internal-state 0x32 is overlaid");

    host_packet.fill(0);
    host_packet[0] = 0xa2;
    host_packet[1] = 0x31;
    host_packet[4 + 1] = 0x14;
    host_packet[4 + 43] = 0x2f;
    host_packet[4 + 44] = 0xff;
    host_packet[4 + 45] = 0;
    host_packet[4 + 46] = 0;
    Output restore{};
    restore.player_restore = true;
    restore.base_player_known = true;
    restore.base_player_byte = 0x16;
    restore.rgb_restore = true;
    restore.base_rgb_known = true;
    restore.base_red = 0x01;
    restore.base_green = 0x02;
    restore.base_blue = 0x03;
    applied = ds5_battery::wire::apply_output(host_packet.data(), host_packet.size(),
                                               FrameKind::HostState31, restore);
    require(!applied.player && !applied.rgb && !applied.reset &&
            host_packet[4 + 43] == 0x2f && host_packet[4 + 44] == 0xff &&
            host_packet[4 + 45] == 0 && host_packet[4 + 46] == 0,
            "ordinary queued state never claims synthetic-only handback");
    std::array<uint8_t, 143> restore_packet{};
    restore_packet[0] = 0xa2;
    restore_packet[1] = 0x32;
    applied = ds5_battery::wire::apply_output(restore_packet.data(), restore_packet.size(),
                                               FrameKind::SyntheticState32, restore);
    require(applied.player && applied.rgb && !applied.reset &&
            restore_packet[5 + 1] == 0x14 && restore_packet[5 + 43] == 0x16 &&
            restore_packet[5 + 44] == 0x01 && restore_packet[5 + 45] == 0x02 &&
            restore_packet[5 + 46] == 0x03,
            "standalone synthetic state restores latest known Player and RGB");

    host_packet[4 + 1] = 0;
    host_packet[4 + 43] = 0x1f;
    Output unknown_player{};
    unknown_player.player_restore = true;
    unknown_player.reset_restore = true;
    std::array<uint8_t, 143> reset_packet{};
    reset_packet[0] = 0xa2;
    reset_packet[1] = 0x32;
    applied = ds5_battery::wire::apply_output(reset_packet.data(), reset_packet.size(),
                                               FrameKind::SyntheticState32, unknown_player);
    require(!applied.player && applied.reset && reset_packet[5 + 1] == 0x08,
            "unknown Player intent attempts ResetLights in a separate synthetic state");

    std::array<uint8_t, 143> two_step{};
    two_step[0] = 0xa2;
    two_step[1] = 0x32;
    Output known_after_reset = restore;
    known_after_reset.reset_restore = true;
    applied = ds5_battery::wire::apply_output(two_step.data(), two_step.size(),
                                               FrameKind::SyntheticState32, known_after_reset);
    require(applied.reset && !applied.player && !applied.rgb && two_step[5 + 1] == 0x08,
            "synthetic reset does not mix latest known lights into its first packet");
    two_step[5 + 1] = 0;
    known_after_reset.reset_restore = false;
    applied = ds5_battery::wire::apply_output(two_step.data(), two_step.size(),
                                               FrameKind::SyntheticState32, known_after_reset);
    require(!applied.reset && applied.player && applied.rgb &&
            two_step[5 + 1] == 0x14 && two_step[5 + 43] == 0x16 &&
            two_step[5 + 44] == 0x01 && two_step[5 + 45] == 0x02 &&
            two_step[5 + 46] == 0x03,
            "accepted reset can be followed by a separate known-light restore");

    reset_packet[5 + 1] = 0;
    Output pending_reset{};
    pending_reset.player_restore = true;
    pending_reset.reset_restore = true;
    pending_reset.synthetic_due = true;
    host_packet[4 + 1] = 0x04;
    host_packet[4 + 44] = 0x12;
    host_packet[4 + 45] = 0x34;
    host_packet[4 + 46] = 0x56;
    require(ds5_battery::wire::choose_next(true, true, true, false) ==
            ds5_battery::wire::DispatchChoice::Synthetic,
            "standalone reset can precede queued host RGB without losing FIFO item");
    require(ds5_battery::wire::choose_next(true, true, true, true) ==
            ds5_battery::wire::DispatchChoice::Queued,
            "newer host command gets a turn even if synthetic ResetLights was rejected");
    applied = ds5_battery::wire::apply_output(reset_packet.data(), reset_packet.size(),
                                               FrameKind::SyntheticState32, pending_reset);
    require(applied.reset && !applied.player && !applied.rgb &&
            reset_packet[5 + 1] == 0x08,
            "separate synthetic reset carries no host light fields");
    applied = ds5_battery::wire::apply_output(host_packet.data(), host_packet.size(),
                                               FrameKind::HostState31, pending_reset);
    require(!applied.reset && !applied.player && !applied.rgb &&
            host_packet[4 + 1] == 0x04 && host_packet[4 + 44] == 0x12 &&
            host_packet[4 + 45] == 0x34 && host_packet[4 + 46] == 0x56,
            "queued host RGB passes unchanged while synthetic reset is pending");
    Output after_accepted_reset = pending_reset;
    after_accepted_reset.reset_restore = false;
    applied = ds5_battery::wire::apply_output(host_packet.data(), host_packet.size(),
                                               FrameKind::HostState31, after_accepted_reset);
    require(!applied.reset && !applied.player && !applied.rgb &&
            host_packet[4 + 1] == 0x04 && host_packet[4 + 44] == 0x12,
            "unknown Player wait after accepted reset does not reset queued RGB again");
    host_packet[4 + 1] = 0x0c;
    applied = ds5_battery::wire::apply_output(host_packet.data(), host_packet.size(),
                                               FrameKind::HostState31, pending_reset);
    require(applied.reset && host_packet[4 + 1] == 0x0c &&
            host_packet[4 + 44] == 0x12 && host_packet[4 + 45] == 0x34 &&
            host_packet[4 + 46] == 0x56,
            "queued host ResetLights plus RGB remains intact after synthetic reset");

    host_packet[4 + 1] = 0;
    restore.reset_restore = true;
    applied = ds5_battery::wire::apply_output(host_packet.data(), host_packet.size(),
                                               FrameKind::HostState31, restore);
    require(!applied.reset && !applied.player && !applied.rgb &&
            (host_packet[4 + 1] & 0x1c) == 0,
            "queued host state waits unchanged while synthetic reset is pending");

    host_packet[4 + 1] = 0x14; // Older queued host Player and RGB allows.
    host_packet[4 + 8] = 0x79; // Nonlight game state must still travel.
    applied = ds5_battery::wire::apply_output(host_packet.data(), host_packet.size(),
                                               FrameKind::HostState31, restore);
    require(!applied.reset && !applied.player && !applied.rgb &&
            (host_packet[4 + 1] & 0x1c) == 0x14 && host_packet[4 + 8] == 0x79,
            "reset handback leaves queued host light allows and game state intact");

    host_packet[4 + 1] = 0x08;
    active.player_override = true;
    applied = ds5_battery::wire::apply_output(host_packet.data(), host_packet.size(),
                                               FrameKind::HostState31, active);
    require(applied.reset && !applied.player && !applied.rgb &&
            host_packet[4 + 1] == 0x08,
            "host ResetLights passes through before a later overlay opportunity");

    require(ds5_battery::wire::request_needed(true, false, 100'000, 0, false, true),
            "synthetic slot requests CAN_SEND_NOW with empty FIFO");
    require(!ds5_battery::wire::request_needed(true, true, 100'000, 0, false, true),
            "already requested CAN_SEND_NOW is not duplicated");
    require(!ds5_battery::wire::request_needed(true, false, 105'000, 110'000, false, true),
            "failed synthetic send waits for bounded retry deadline");
    require(ds5_battery::wire::request_needed(true, false, 110'000, 110'000, false, true),
            "failed synthetic send remains pending at retry deadline");

    ds5_battery::wire::CanSendRequest request;
    const uint8_t request_status = request.issue([&request]() -> uint8_t {
        require(request.pending(), "CAN_SEND request marked pending before BTstack call");
        request.delivered(); // BTstack is permitted to emit synchronously.
        return 0;
    });
    require(request_status == 0 && !request.pending(),
            "synchronous CAN_SEND callback does not leave a stale pending flag");
    require(request.issue([]() -> uint8_t { return 1; }) == 1 &&
            !request.pending(), "rejected CAN_SEND request stays retryable");
    require(request.issue([]() -> uint8_t { return 0; }) == 0 && request.pending(),
            "accepted CAN_SEND request stays pending until its event arrives");
    request.delivered();
    require(!request.pending(), "CAN_SEND event clears pending request");
    require(request.issue([]() -> uint8_t { return 0; }) == 0 && request.pending(),
            "later CAN_SEND request can be issued after delivery");
    request.clear();
    require(!request.pending(), "link close clears outstanding CAN_SEND request");

    require(ds5_battery::wire::choose_next(true, false, true, true) ==
            ds5_battery::wire::DispatchChoice::Synthetic,
            "empty FIFO retries the one synthetic slot after rejection");
    require(ds5_battery::wire::choose_next(false, true, true, false) ==
            ds5_battery::wire::DispatchChoice::None,
            "closed link never dispatches queued controller output");
    require(ds5_battery::wire::choose_next(true, true, true, false) ==
            ds5_battery::wire::DispatchChoice::Synthetic,
            "synthetic animation may pass an audio FIFO head once");
    require(ds5_battery::wire::choose_next(true, true, true, true) ==
            ds5_battery::wire::DispatchChoice::Queued,
            "permanently failing synthetic slot yields to queued audio after each attempt");
    require(ds5_battery::wire::choose_next(true, true, false, false) ==
            ds5_battery::wire::DispatchChoice::Queued,
            "default-off host output keeps ordinary FIFO behavior");

    ds5_battery::wire::LightEpoch epoch;
    require(!epoch.stale(epoch.tag()), "default-off host output has no stale light epoch");
    struct QueuedLight {
        std::array<uint8_t, 79> packet{};
        uint32_t tag = 0;
    };
    std::deque<QueuedLight> fifo;
    auto make_host_light = [](uint8_t player, uint8_t red, uint8_t game) {
        QueuedLight item{};
        item.packet[0] = 0xa2;
        item.packet[1] = 0x31;
        item.packet[4 + 1] = 0x14;
        item.packet[4 + 43] = player;
        item.packet[4 + 44] = red;
        item.packet[4 + 8] = game;
        return item;
    };
    auto a = make_host_light(0x01, 0x11, 0xa1);
    a.tag = epoch.tag();
    fifo.push_back(a); // A was enqueued before cancellation.
    epoch.start_handoff();
    epoch.note_light_intent();
    auto b = make_host_light(0x02, 0x22, 0xb2);
    b.tag = epoch.tag();
    fifo.push_back(b); // FIFO capacity two is now full.
    epoch.note_light_intent(); // C changes latest host intent, then queue_try_add fails.
    require(fifo.size() == 2 && epoch.stale(a.tag) && epoch.stale(b.tag),
            "failed C enqueue still makes older A and B lights stale");

    Output c{};
    c.base_player_known = true;
    c.base_player_byte = 0x03;
    c.base_rgb_known = true;
    c.base_red = 0x33;
    c.base_green = 0x44;
    c.base_blue = 0x55;
    c.player_restore = true;
    c.rgb_restore = true;
    c.synthetic_due = true;
    std::array<uint8_t, 143> c_restore{};
    c_restore[0] = 0xa2;
    c_restore[1] = 0x32;
    require(ds5_battery::wire::choose_next(true, !fifo.empty(), true, false) ==
                ds5_battery::wire::DispatchChoice::Synthetic,
            "latest C restore sends despite full ordinary FIFO");
    applied = ds5_battery::wire::apply_output(c_restore.data(), c_restore.size(),
                                               FrameKind::SyntheticState32, c);
    require(applied.player && applied.rgb && c_restore[5 + 43] == 0x03 &&
            c_restore[5 + 44] == 0x33,
            "synthetic restore carries dropped C's latest Player and RGB intent");

    for (uint8_t game : {uint8_t{0xa1}, uint8_t{0xb2}}) {
        require(ds5_battery::wire::choose_next(true, !fifo.empty(), false, true) ==
                    ds5_battery::wire::DispatchChoice::Queued,
                "accepted C restoration leaves FIFO game traffic in order");
        auto front = fifo.front();
        fifo.pop_front();
        require(ds5_battery::wire::scrub_stale_lights(front.packet.data(),
                    front.packet.size(), FrameKind::HostState31, epoch.stale(front.tag), c) &&
                front.packet[4 + 43] == 0x03 && front.packet[4 + 44] == 0x33 &&
                front.packet[4 + 45] == 0x44 && front.packet[4 + 46] == 0x55 &&
                front.packet[4 + 8] == game,
                "queued A or B cannot relatch stale lights after synthetic C");
        if (game == 0xa1) {
            auto ordinary_game = make_host_light(0, 0, 0xd4);
            ordinary_game.packet[4 + 1] = 0; // No Allow bits: no light intent.
            ordinary_game.tag = epoch.tag();
            fifo.push_back(ordinary_game); // Continuous game output continues.
        }
    }
    require(!fifo.empty() && fifo.front().packet[4 + 8] == 0xd4,
            "new game output remains queued while stale light packets drain");

    auto reset = make_host_light(0x09, 0x66, 0xe5);
    reset.packet[4 + 1] = 0x1c;
    reset.tag = b.tag;
    require(ds5_battery::wire::scrub_stale_lights(reset.packet.data(), reset.packet.size(),
                FrameKind::HostState31, epoch.stale(reset.tag), c) &&
            (reset.packet[4 + 1] & 0x1c) == 0x1c && reset.packet[4 + 8] == 0xe5,
            "late ResetLights bit and nonlight state survive stale-light scrub");
    applied = ds5_battery::wire::apply_output(reset.packet.data(), reset.packet.size(),
                                               FrameKind::HostState31, c);
    require(applied.reset && !applied.player && !applied.rgb,
            "late ResetLights remains visible so feedback can rearm C restore");

    std::array<uint8_t, 143> audio_32{};
    audio_32[0] = 0xa2;
    audio_32[1] = 0x32;
    audio_32[5 + 1] = 0x14;
    audio_32[5 + 43] = 0x09;
    require(!ds5_battery::wire::scrub_stale_lights(audio_32.data(), audio_32.size(),
                FrameKind::Other, true, c) && audio_32[5 + 43] == 0x09,
            "audio 0x32 remains byte-for-byte untouched even with stale tag");
    require(ds5_battery::wire::scrub_stale_lights(audio_32.data(), audio_32.size(),
                FrameKind::InternalState32, true, c) && audio_32[5 + 43] == 0x03,
            "explicit state 0x32 is eligible for light scrub");

    auto unknown = make_host_light(0x07, 0x77, 0xf6);
    unknown.packet[4 + 1] |= 0x08;
    Output no_base{};
    require(ds5_battery::wire::scrub_stale_lights(unknown.packet.data(),
                unknown.packet.size(), FrameKind::HostState31, true, no_base) &&
            (unknown.packet[4 + 1] & 0x1c) == 0x08 && unknown.packet[4 + 8] == 0xf6,
            "unknown latest light intent clears stale Allow bits but keeps ResetLights");
    auto truncated = make_host_light(0x07, 0x77, 0xf8);
    require(!ds5_battery::wire::scrub_stale_lights(truncated.packet.data(),
                4 + 47 + 3, FrameKind::HostState31, true, c) &&
            truncated.packet[4 + 43] == 0x07,
            "incomplete queued state cannot be modified as a light report");
    auto current = make_host_light(0x03, 0x33, 0xf7);
    require(!ds5_battery::wire::scrub_stale_lights(current.packet.data(),
                current.packet.size(), FrameKind::HostState31, false, c) &&
            current.packet[4 + 43] == 0x03 && current.packet[4 + 8] == 0xf7,
            "current host lights and unrelated game bytes pass through");
    epoch.clear();
    require(!epoch.stale(a.tag), "link close or completed handback retires stale tags");

    std::puts("battery_output_policy_test: PASS");
    return 0;
}
