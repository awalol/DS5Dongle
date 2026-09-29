#include "battery_feedback.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using ds5_battery::BatteryFeedback;
using ds5_battery::LightCommand;
using ds5_battery::Output;
using ds5_battery::SendResult;

static void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

static void accept(BatteryFeedback &feedback, const Output &output, uint64_t now_us) {
    SendResult result{};
    result.accepted = true;
    result.player_applied = output.player_override || output.player_restore;
    result.rgb_applied = output.rgb_override || output.rgb_restore;
    result.reset_applied = output.reset_restore;
    result.synthetic_restore = output.player_restore || output.rgb_restore ||
                               output.reset_restore;
    feedback.on_send_result(output, result, now_us);
}

static void confirmed_transition(BatteryFeedback &feedback, uint32_t generation,
                                 uint8_t level, uint64_t first_us) {
    feedback.on_real_battery_report(generation, level, first_us);
    feedback.on_real_battery_report(generation, level, first_us + 3'000'000);
}

static void initial_baseline_and_all_gauge_levels() {
    for (uint8_t level = 0; level <= 10; ++level) {
        BatteryFeedback feedback;
        feedback.on_link_open(1);
        feedback.on_real_battery_report(1, level, 100);
        require(!feedback.output_at(100).player_override, "default off never takes Player LEDs");
        feedback.set_enabled(true, 200);
        require(!feedback.output_at(200).player_override, "enable does not use an old cached report");
        feedback.on_real_battery_report(1, level, 300);
        const Output output = feedback.output_at(300);
        const uint8_t expected[] = {1, 1, 1, 3, 3, 3, 11, 11, 11, 27, 27};
        require(output.player_override, "first valid discharge report starts the gauge");
        require(output.player_byte == static_cast<uint8_t>(0x20 | expected[level]),
                "gauge maps all raw levels to four outer Player patterns");
        require((output.player_byte & 0x04) == 0, "battery gauge never lights the center LED");
        require(!output.rgb_override, "first baseline never starts a notification");
    }
}

static void low_blink_and_silence_hold() {
    BatteryFeedback feedback;
    feedback.on_link_open(7);
    feedback.set_enabled(true, 0);
    feedback.on_real_battery_report(7, 1, 0);
    require(feedback.output_at(0).player_byte == 0x21, "low gauge starts lit");
    accept(feedback, feedback.output_at(0), 0);
    require(feedback.output_at(250'000).player_byte == 0x20, "low gauge dark at half cycle");
    require(feedback.output_at(250'000).synthetic_due, "low blink edge requests a send");
    require(feedback.output_at(500'000).player_byte == 0x21, "low gauge lit after full 500ms cycle");
    require(feedback.output_at(600'000'000).player_override, "silence holds confirmed low gauge indefinitely");
    require(feedback.output_at(600'250'000).player_byte == 0x20,
            "silence holds the low blink rather than expiring the gauge");
}

static void raw_level_debounce_requires_later_matching_real_report() {
    BatteryFeedback feedback;
    feedback.on_link_open(4);
    feedback.set_enabled(true, 0);
    feedback.on_real_battery_report(4, 3, 0);
    feedback.on_real_battery_report(4, 1, 10'000'000);
    require(feedback.output_at(13'000'000).player_byte == 0x23,
            "tick at three seconds cannot confirm without a real report");
    require(!feedback.output_at(13'000'000).rgb_override, "tick cannot start notification");
    feedback.on_real_battery_report(4, 1, 12'999'999);
    require(feedback.output_at(12'999'999).player_byte == 0x23,
            "same candidate before three seconds remains unconfirmed");
    feedback.on_real_battery_report(4, 1, 13'000'000);
    require(feedback.output_at(13'000'000).player_byte == 0x21,
            "3-to-1 confirms at the exact three-second boundary");
    require(feedback.output_at(13'000'000).rgb_override,
            "confirmed jump from above one to one starts notification");

    // Both invalid PowerState and out-of-range raw level break a candidate.
    const uint8_t invalid_codes[] = {0x31, 0x0b};
    for (uint8_t invalid_raw : invalid_codes) {
        BatteryFeedback invalid;
        invalid.on_link_open(5);
        invalid.set_enabled(true, 0);
        invalid.on_real_battery_report(5, 3, 0);
        invalid.on_real_battery_report(5, 1, 1'000'000);
        invalid.on_real_battery_report(5, invalid_raw, 4'000'000);
        invalid.on_real_battery_report(5, 1, 5'000'000);
        require(invalid.output_at(5'000'000).player_byte == 0x23,
                "invalid report cannot preserve candidate elapsed time");
        invalid.on_real_battery_report(5, 1, 8'000'000);
        require(invalid.output_at(8'000'000).rgb_override,
                "new candidate confirms only after another matching real report");
    }

    BatteryFeedback changed;
    changed.on_link_open(6);
    changed.set_enabled(true, 0);
    changed.on_real_battery_report(6, 3, 0);
    changed.on_real_battery_report(6, 1, 1'000'000);
    changed.on_real_battery_report(6, 2, 2'000'000);
    changed.on_real_battery_report(6, 1, 4'000'000);
    require(changed.output_at(4'000'000).player_byte == 0x23 &&
                !changed.output_at(4'000'000).rgb_override,
            "intervening raw level resets the earlier low candidate");
    changed.on_real_battery_report(6, 1, 7'000'000);
    require(changed.output_at(7'000'000).rgb_override,
            "candidate confirms three seconds after latest first observation");
}

static void raw_transitions_and_retrigger_rules() {
    BatteryFeedback feedback;
    feedback.on_link_open(9);
    feedback.set_enabled(true, 0);
    feedback.on_real_battery_report(9, 0, 0);
    confirmed_transition(feedback, 9, 1, 1'000'000);
    require(!feedback.output_at(4'000'000).rgb_override,
            "confirmed zero-to-one never starts notification");
    confirmed_transition(feedback, 9, 2, 5'000'000);
    require(feedback.output_at(8'000'000).player_byte == 0x21,
            "raw level two maps to same gauge bit as one");
    confirmed_transition(feedback, 9, 1, 9'000'000);
    require(!feedback.output_at(12'000'000).rgb_override,
            "raw two-to-one stays in the same white tier and does not notify");
    confirmed_transition(feedback, 9, 3, 13'000'000);
    require(!feedback.output_at(16'000'000).rgb_override,
            "rising from one to two white tiers does not notify");
    confirmed_transition(feedback, 9, 2, 17'000'000);
    require(feedback.output_at(20'000'000).rgb_override,
            "confirmed two-to-one white-tier drop starts red notification");

    BatteryFeedback zero;
    zero.on_link_open(10);
    zero.set_enabled(true, 0);
    zero.on_real_battery_report(10, 3, 0);
    confirmed_transition(zero, 10, 0, 1'000'000);
    require(!zero.output_at(4'000'000).rgb_override,
            "raw zero does not start a notification even when white tier falls");
}

static void tier_colors_and_skipped_tiers() {
    struct ColorCase {
        uint8_t from;
        uint8_t to;
        uint8_t red;
        uint8_t green;
        uint8_t blue;
    };
    const ColorCase color_cases[] = {
        {9, 8, 0, 0, 255},      // 4 -> 3: blue.
        {6, 5, 255, 255, 0},    // 3 -> 2: yellow.
        {3, 2, 255, 0, 0},      // 2 -> 1: red.
        {9, 5, 255, 255, 0},    // 4 -> 2: destination only.
        {9, 2, 255, 0, 0},      // 4 -> 1: destination only.
        {8, 1, 255, 0, 0},      // 3 -> 1: destination only.
    };
    for (const auto &test : color_cases) {
        BatteryFeedback feedback;
        feedback.on_link_open(1);
        feedback.set_enabled(true, 0);
        feedback.on_real_battery_report(1, test.from, 0);
        confirmed_transition(feedback, 1, test.to, 1'000'000);
        const Output first = feedback.output_at(4'000'000);
        require(first.rgb_override && first.red == 0 && first.green == 0 &&
                    first.blue == 0 && first.synthetic_due,
                "confirmed white-tier drop requests a black pulse boundary");
        accept(feedback, first, 4'000'000);
        const Output peak = feedback.output_at(4'800'000);
        require(peak.red == test.red && peak.green == test.green &&
                    peak.blue == test.blue,
                "pulse peak uses only the destination white tier's color");
    }

    const uint8_t same_tier[][2] = {{10, 9}, {8, 6}, {5, 3}, {2, 1}};
    for (const auto &levels : same_tier) {
        BatteryFeedback feedback;
        feedback.on_link_open(2);
        feedback.set_enabled(true, 0);
        feedback.on_real_battery_report(2, levels[0], 0);
        confirmed_transition(feedback, 2, levels[1], 1'000'000);
        require(!feedback.output_at(4'000'000).rgb_override,
                "raw level changes within one white tier do not notify");
    }
}

static void five_delivery_gated_blue_and_yellow_pulses() {
    struct ColorCase { uint8_t from, to, red, green, blue; };
    const ColorCase cases[] = {{9, 8, 0, 0, 255}, {6, 5, 255, 255, 0}};
    for (const auto &test : cases) {
        BatteryFeedback feedback;
        feedback.on_link_open(1);
        feedback.observe_base_lights(LightCommand{false, 0, true, 20, 30, 40, false});
        feedback.set_enabled(true, 0);
        feedback.on_real_battery_report(1, test.from, 0);
        confirmed_transition(feedback, 1, test.to, 1'000'000);
        uint64_t now = 4'000'000;
        accept(feedback, feedback.output_at(now), now);
        for (uint8_t pulse = 0; pulse < 5; ++pulse) {
            const Output peak = feedback.output_at(now + 800'000);
            require(peak.rgb_override && peak.pulse_index == pulse &&
                        peak.red == test.red && peak.green == test.green &&
                        peak.blue == test.blue,
                    "each colored pulse reaches its full target color");
            SendResult rejected{};
            feedback.on_send_result(peak, rejected, now + 800'000);
            require(feedback.output_at(now + 1'000'000).pulse_index == pulse,
                    "a rejected colored peak cannot advance the pulse");
            accept(feedback, peak, now + 800'000);
            const Output tail = feedback.output_at(now + 1'600'000);
            require(tail.rgb_override && tail.red == 0 && tail.green == 0 &&
                        tail.blue == 0,
                    "each colored pulse ends at black");
            accept(feedback, tail, now + 1'600'000);
            now += 1'600'000;
        }
        const Output restore = feedback.output_at(now);
        require(!restore.rgb_override && restore.rgb_restore &&
                    restore.base_red == 20 && restore.base_green == 30 &&
                    restore.base_blue == 40,
                "five colored pulses hand RGB back to the latest host intent");
    }
}

static void five_delivery_gated_pulses_and_natural_restore() {
    BatteryFeedback feedback;
    feedback.on_link_open(11);
    feedback.observe_base_lights(LightCommand{false, 0, true, 20, 30, 40, false});
    feedback.set_enabled(true, 0);
    feedback.on_real_battery_report(11, 3, 0);
    confirmed_transition(feedback, 11, 1, 1'000'000);
    uint64_t start = 4'000'000;
    Output first = feedback.output_at(start);
    require(first.rgb_override && first.red == 0 && first.synthetic_due,
            "notification requests first black boundary frame");
    SendResult rejected{};
    feedback.on_send_result(first, rejected, start);
    require(feedback.output_at(start + 5'000'000).red == 0,
            "rejected frame does not advance first pulse");
    start += 5'000'000;
    accept(feedback, feedback.output_at(start), start);
    require(feedback.output_at(start + 400'000).red >= 126 &&
                feedback.output_at(start + 400'000).red <= 128,
            "rising half of 1.6-second triangle reaches half brightness");

    for (uint8_t pulse = 0; pulse < 5; ++pulse) {
        const Output peak = feedback.output_at(start + 800'000);
        require(peak.rgb_override && peak.red == 255 && peak.pulse_index == pulse,
                "each pulse reaches full red peak");
        feedback.on_send_result(peak, rejected, start + 800'000);
        require(feedback.output_at(start + 1'800'000).red == 255,
                "unsent peak stalls pulse instead of consuming it");
        start += 1'800'000;
        accept(feedback, feedback.output_at(start), start);
        require(feedback.output_at(start + 400'000).red >= 126 &&
                    feedback.output_at(start + 400'000).red <= 128,
                "falling half reaches half brightness");
        const Output zero = feedback.output_at(start + 800'000);
        require(zero.rgb_override && zero.red == 0, "pulse zero boundary remains an override");
        start += 800'000;
        accept(feedback, zero, start);
    }
    const Output restore = feedback.output_at(start);
    require(!restore.rgb_override && restore.rgb_restore && restore.base_rgb_known &&
                restore.base_red == 20 && restore.base_green == 30 && restore.base_blue == 40,
            "five accepted pulse tails request the latest game RGB color");
    accept(feedback, restore, start + 1);
    require(!feedback.restore_pending(), "accepted RGB restoration finishes the sequence");
}

static void later_confirmed_drop_restarts_the_running_episode() {
    BatteryFeedback feedback;
    feedback.on_link_open(12);
    feedback.set_enabled(true, 0);
    feedback.on_real_battery_report(12, 9, 0);
    confirmed_transition(feedback, 12, 8, 1'000'000);
    accept(feedback, feedback.output_at(4'000'000), 4'000'000);
    const Output first_peak = feedback.output_at(4'800'000);
    require(first_peak.blue == 255,
            "first blue episode is rising before a later edge");
    accept(feedback, first_peak, 4'800'000);
    confirmed_transition(feedback, 12, 9, 5'000'000);
    confirmed_transition(feedback, 12, 8, 8'001'000);
    const Output restart = feedback.output_at(11'001'000);
    require(restart.rgb_override && restart.pulse_index == 0 &&
                restart.red == 0 && restart.green == 0 && restart.blue == 0 &&
                restart.synthetic_due,
            "a second confirmed 4-to-3 drop restarts blue immediately at black");
    accept(feedback, restart, 11'001'000);
    require(feedback.output_at(11'801'000).blue == 255,
            "the restarted episode reaches a fresh first blue peak");

    confirmed_transition(feedback, 12, 5, 12'000'000);
    const Output yellow = feedback.output_at(15'000'000);
    require(yellow.rgb_override && yellow.pulse_index == 0 &&
                yellow.red == 0 && yellow.green == 0 && yellow.blue == 0,
            "new lower destination preempts blue with a fresh yellow episode");
    accept(feedback, yellow, 15'000'000);
    const Output yellow_peak = feedback.output_at(15'800'000);
    require(yellow_peak.red == 255 && yellow_peak.green == 255 &&
                yellow_peak.blue == 0,
            "preempted episode uses the current destination color");
}

static void charging_disable_restore_and_generation() {
    BatteryFeedback feedback;
    feedback.on_link_open(21);
    feedback.observe_base_lights(LightCommand{true, 0x12, true, 12, 34, 56, false});
    feedback.set_enabled(true, 0);
    feedback.on_real_battery_report(21, 3, 0);
    accept(feedback, feedback.output_at(0), 0);
    confirmed_transition(feedback, 21, 1, 1'000'000);
    require(feedback.output_at(4'000'000).rgb_override, "notification active before charging");
    accept(feedback, feedback.output_at(4'000'000), 4'000'000);
    feedback.observe_base_lights(LightCommand{false, 0, true, 90, 80, 70, false});
    feedback.on_real_battery_report(21, 0x11, 4'000'001); // Charging, level one.
    const Output restore = feedback.output_at(4'000'001);
    require(!restore.player_override && !restore.rgb_override,
            "charging cancels both overlays in the same report");
    require(restore.player_restore && restore.rgb_restore && feedback.restore_pending(),
            "charging requests both taken-over channels restored");
    require(restore.base_player_known && restore.base_player_byte == 0x12 &&
                restore.base_rgb_known && restore.base_red == 90 && restore.base_green == 80 &&
                restore.base_blue == 70,
            "restore uses latest explicit base intent by channel");
    feedback.on_real_battery_report(21, 0x11, 4'000'002);
    require(feedback.output_at(4'000'002).player_restore &&
                feedback.output_at(4'000'002).rgb_restore,
            "repeated charging reports cannot discard a pending restore");
    SendResult rejected{};
    rejected.synthetic_restore = true;
    rejected.player_applied = true;
    rejected.rgb_applied = true;
    feedback.on_send_result(restore, rejected, 4'000'002);
    require(feedback.restore_pending(), "BTstack-rejected synthetic restore remains pending");
    accept(feedback, restore, 4'000'003);
    require(!feedback.restore_pending(), "accepted current restore clears pending state");
    feedback.on_real_battery_report(21, 0x02, 4'000'004); // Complete.
    feedback.on_real_battery_report(21, 1, 4'000'005);
    require(feedback.output_at(4'000'005).player_override &&
                !feedback.output_at(4'000'005).rgb_override,
            "discharge after charging starts a fresh baseline without notification");

    feedback.on_link_closed(21);
    require(!feedback.output_at(4'000'006).link_open && !feedback.restore_pending(),
            "close cancels output and pending restore");
    feedback.on_link_open(22);
    feedback.on_real_battery_report(21, 1, 4'000'007);
    require(!feedback.output_at(4'000'007).player_override,
            "old generation reports cannot seed a new connection");
    feedback.on_real_battery_report(22, 1, 4'000'008);
    require(feedback.output_at(4'000'008).player_override &&
                !feedback.output_at(4'000'008).rgb_override,
            "new generation begins with fresh gauge baseline");
    feedback.set_enabled(false, 4'000'009);
    require(!feedback.output_at(4'000'009).player_override && !feedback.restore_pending(),
            "disable before first accepted new-link gauge requires no restore");
}

static void cancel_before_first_accepted_output_does_not_disturb_lights() {
    BatteryFeedback feedback;
    feedback.on_link_open(25);
    feedback.observe_base_lights(LightCommand{true, 0x14, true, 1, 2, 3, false});
    feedback.set_enabled(true, 0);
    feedback.on_real_battery_report(25, 3, 0);
    feedback.set_enabled(false, 1);
    require(!feedback.restore_pending(),
            "unsubmitted gauge cannot claim and restore Player LEDs");
    feedback.set_enabled(true, 2);
    feedback.on_real_battery_report(25, 3, 3);
    confirmed_transition(feedback, 25, 1, 10);
    require(feedback.output_at(3'000'010).rgb_override,
            "unsent red episode was scheduled");
    feedback.on_real_battery_report(25, 0x11, 3'000'011);
    require(!feedback.restore_pending(),
            "charging before any accepted effect frame does not send a restore");
}

static void reset_lights_is_not_an_all_off_pattern() {
    BatteryFeedback feedback;
    feedback.on_link_open(30);
    feedback.set_enabled(true, 0);
    feedback.on_real_battery_report(30, 4, 0);
    accept(feedback, feedback.output_at(0), 0);
    feedback.observe_base_lights(LightCommand{false, 0, false, 0, 0, 0, true});
    feedback.set_enabled(false, 1);
    const Output restore = feedback.output_at(1);
    require(restore.reset_restore && !restore.base_player_known,
            "latest reset event is replayed rather than fabricated all-off Player bits");
    SendResult accepted{};
    accepted.accepted = true;
    accepted.reset_applied = true;
    accepted.synthetic_restore = true;
    feedback.on_send_result(restore, accepted, 2);
    require(!feedback.restore_pending(), "accepted reset clears unknown Player restore");
}

static void unknown_player_intent_requests_reset_fallback() {
    BatteryFeedback feedback;
    feedback.on_link_open(31);
    feedback.set_enabled(true, 0);
    feedback.on_real_battery_report(31, 5, 0);
    accept(feedback, feedback.output_at(0), 0);
    feedback.set_enabled(false, 1);
    const Output restore = feedback.output_at(1);
    require(restore.player_restore && !restore.base_player_known && restore.reset_restore,
            "unknown pre-gauge Player pattern requests ResetLights instead of all-off bytes");
    SendResult accepted{};
    accepted.accepted = true;
    accepted.reset_applied = true;
    accepted.synthetic_restore = true;
    feedback.on_send_result(restore, accepted, 2);
    require(!feedback.restore_pending(), "accepted reset fallback completes pending restore");
}

static void host_reset_requires_reapplying_active_overlay() {
    BatteryFeedback feedback;
    feedback.on_link_open(32);
    feedback.set_enabled(true, 0);
    feedback.on_real_battery_report(32, 5, 0);
    accept(feedback, feedback.output_at(0), 0);
    require(!feedback.output_at(1).synthetic_due, "unchanged accepted gauge needs no resend");
    feedback.observe_base_lights(LightCommand{false, 0, false, 0, 0, 0, true});
    const Output reassert = feedback.output_at(2);
    require(reassert.synthetic_due, "host ResetLights makes unchanged gauge due again");
    SendResult pass_through{};
    pass_through.accepted = true;
    pass_through.reset_applied = true;
    feedback.on_send_result(reassert, pass_through, 2);
    require(feedback.output_at(3).synthetic_due,
            "pass-through host ResetLights does not count as overlay delivery");
    accept(feedback, feedback.output_at(3), 3);
    require(!feedback.output_at(4).synthetic_due,
            "accepted reapplication satisfies the active overlay");
}

static void delayed_host_reset_reasserts_after_gauge_was_accepted() {
    BatteryFeedback feedback;
    feedback.on_link_open(35);
    feedback.set_enabled(true, 0);
    // This ResetLights may sit behind audio in the transport FIFO.
    feedback.observe_base_lights(LightCommand{false, 0, false, 0, 0, 0, true});
    feedback.on_real_battery_report(35, 5, 1);
    accept(feedback, feedback.output_at(1), 1);
    require(!feedback.output_at(2).synthetic_due,
            "gauge send initially covers the earlier ResetLights event");
    SendResult late_reset{};
    late_reset.accepted = true;
    late_reset.reset_applied = true;
    feedback.on_send_result(feedback.output_at(3), late_reset, 3);
    require(feedback.output_at(4).synthetic_due,
            "late accepted ResetLights requeues the still-active gauge");
    accept(feedback, feedback.output_at(4), 4);
    require(!feedback.output_at(5).synthetic_due,
            "accepted post-reset gauge clears reassert request");
}

static void latest_base_intent_is_used_for_synchronous_restore() {
    BatteryFeedback feedback;
    feedback.on_link_open(33);
    feedback.observe_base_lights(LightCommand{true, 0x11, false, 0, 0, 0, false});
    feedback.set_enabled(true, 0);
    feedback.on_real_battery_report(33, 5, 0);
    accept(feedback, feedback.output_at(0), 0);
    feedback.set_enabled(false, 1);
    // The latest base is captured before the next synchronous send attempt.
    feedback.observe_base_lights(LightCommand{true, 0x14, false, 0, 0, 0, false});
    const Output current = feedback.output_at(2);
    require(current.base_player_known && current.base_player_byte == 0x14,
            "latest Player pattern replaces an older queued target before send");
    accept(feedback, current, 3);
    require(!feedback.restore_pending(), "latest accepted Player restoration completes handback");
}

static void reset_and_explicit_lights_in_one_command_are_both_preserved() {
    BatteryFeedback feedback;
    feedback.on_link_open(34);
    feedback.observe_base_lights(LightCommand{true, 0x04, true, 9, 8, 7, true});
    feedback.set_enabled(true, 0);
    feedback.on_real_battery_report(34, 3, 0);
    accept(feedback, feedback.output_at(0), 0);
    confirmed_transition(feedback, 34, 1, 1'000'000);
    accept(feedback, feedback.output_at(4'000'000), 4'000'000);
    feedback.set_enabled(false, 4'000'001);
    const Output restore = feedback.output_at(4'000'001);
    require(restore.player_restore && restore.rgb_restore && restore.reset_restore &&
                restore.base_player_known && restore.base_rgb_known,
            "same-command ResetLights and explicit fields survive into restoration intent");
    SendResult reset_sent{};
    reset_sent.accepted = true;
    reset_sent.reset_applied = true;
    reset_sent.synthetic_restore = true;
    feedback.on_send_result(restore, reset_sent, 4'000'002);
    const Output explicit_restore = feedback.output_at(4'000'002);
    require(!explicit_restore.reset_restore && explicit_restore.player_restore &&
                explicit_restore.rgb_restore,
            "accepted ResetLights advances to an explicit Player/RGB restoration step");
    accept(feedback, explicit_restore, 4'000'003);
    require(!feedback.restore_pending(), "accepted explicit fields complete two-step restore");
}

static void synthetic_unknown_reset_replays_unowned_known_rgb() {
    BatteryFeedback feedback;
    feedback.on_link_open(36);
    feedback.observe_base_lights(LightCommand{false, 0, true, 9, 8, 7, false});
    feedback.set_enabled(true, 0);
    feedback.on_real_battery_report(36, 5, 0);
    accept(feedback, feedback.output_at(0), 0); // Only Player was taken over.
    feedback.set_enabled(false, 1);
    const Output reset = feedback.output_at(1);
    require(reset.player_restore && !reset.base_player_known && reset.reset_restore,
            "unknown Player handback uses ResetLights rather than fabricated off bytes");

    SendResult delivered{};
    delivered.accepted = true;
    delivered.reset_applied = true;
    delivered.synthetic_restore = true;
    feedback.on_send_result(reset, delivered, 2);
    const Output rgb = feedback.output_at(2);
    require(!rgb.player_restore && rgb.rgb_restore && rgb.base_rgb_known &&
                rgb.base_red == 9 && rgb.base_green == 8 && rgb.base_blue == 7 &&
                !rgb.reset_restore && rgb.synthetic_due,
            "synthetic Reset replays latest unowned RGB because it cleared the controller");
    SendResult queued{};
    queued.accepted = true;
    queued.rgb_applied = true;
    feedback.on_send_result(rgb, queued, 3);
    require(feedback.restore_pending(),
            "a matching queued RGB state cannot certify the separate synthetic handback");
    accept(feedback, feedback.output_at(4), 4);
    require(!feedback.restore_pending(), "accepted synthetic RGB completes cross-channel handback");
}
static void reset_then_player_only_keeps_rgb_unknown() {
    BatteryFeedback feedback;
    feedback.on_link_open(43);
    feedback.observe_base_lights(LightCommand{true, 0x04, true, 10, 20, 30, false});
    feedback.set_enabled(true, 0);
    feedback.on_real_battery_report(43, 3, 0);
    accept(feedback, feedback.output_at(0), 0);
    confirmed_transition(feedback, 43, 1, 1'000'000);
    accept(feedback, feedback.output_at(4'000'000), 4'000'000);

    feedback.observe_base_lights(LightCommand{false, 0, false, 0, 0, 0, true});
    feedback.observe_base_lights(LightCommand{true, 0x14, false, 0, 0, 0, false});
    feedback.set_enabled(false, 4'000'001);
    const Output restore = feedback.output_at(4'000'001);
    require(restore.base_player_known && restore.base_player_byte == 0x14 &&
                !restore.base_rgb_known && restore.reset_restore,
            "Reset followed by Player-only intent keeps RGB unknown");
    SendResult reset{};
    reset.accepted = true;
    reset.reset_applied = true;
    reset.synthetic_restore = true;
    feedback.on_send_result(restore, reset, 4'000'002);
    const Output player = feedback.output_at(4'000'002);
    require(player.player_restore && !player.reset_restore && player.base_player_known &&
                player.base_player_byte == 0x14 && !player.rgb_restore,
            "Reset release is followed by the latest separate Player intent");
    accept(feedback, player, 4'000'003);
    require(!feedback.restore_pending(), "Player reapply completes mixed known/unknown handback");
}

static void reset_then_rgb_only_keeps_player_unknown() {
    BatteryFeedback feedback;
    feedback.on_link_open(44);
    feedback.observe_base_lights(LightCommand{true, 0x04, true, 10, 20, 30, false});
    feedback.set_enabled(true, 0);
    feedback.on_real_battery_report(44, 5, 0);
    accept(feedback, feedback.output_at(0), 0);

    feedback.observe_base_lights(LightCommand{false, 0, false, 0, 0, 0, true});
    feedback.observe_base_lights(LightCommand{false, 0, true, 40, 50, 60, false});
    feedback.set_enabled(false, 1);
    const Output restore = feedback.output_at(1);
    require(!restore.base_player_known && restore.base_rgb_known &&
                restore.base_red == 40 && restore.base_green == 50 &&
                restore.base_blue == 60 && restore.reset_restore,
            "Reset followed by RGB-only intent keeps Player unknown");
    SendResult reset{};
    reset.accepted = true;
    reset.reset_applied = true;
    reset.synthetic_restore = true;
    feedback.on_send_result(restore, reset, 2);
    const Output rgb = feedback.output_at(2);
    require(!rgb.player_restore && rgb.rgb_restore && rgb.base_rgb_known &&
                !rgb.reset_restore,
            "Reset release is followed by latest separate RGB intent");
    accept(feedback, rgb, 3);
    require(!feedback.restore_pending(), "RGB reapply completes mixed known/unknown handback");
}

static void same_packet_reset_marker_is_per_channel() {
    BatteryFeedback feedback;
    feedback.on_link_open(45);
    feedback.observe_base_lights(LightCommand{true, 0x04, true, 10, 20, 30, true});
    feedback.set_enabled(true, 0);
    feedback.on_real_battery_report(45, 3, 0);
    accept(feedback, feedback.output_at(0), 0);
    confirmed_transition(feedback, 45, 1, 1'000'000);
    accept(feedback, feedback.output_at(4'000'000), 4'000'000);

    feedback.observe_base_lights(LightCommand{true, 0x14, false, 0, 0, 0, false});
    feedback.set_enabled(false, 4'000'001);
    const Output rgb_still_same_packet = feedback.output_at(4'000'001);
    require(rgb_still_same_packet.base_player_known &&
                rgb_still_same_packet.base_player_byte == 0x14 &&
                rgb_still_same_packet.base_rgb_known &&
                rgb_still_same_packet.reset_restore,
            "new Player command does not erase RGB's ResetLights pairing");

    feedback.observe_base_lights(LightCommand{false, 0, true, 40, 50, 60, false});
    const Output both_separate = feedback.output_at(4'000'002);
    require(both_separate.base_player_known && both_separate.base_rgb_known &&
                both_separate.base_red == 40 && !both_separate.reset_restore,
            "later separate writes to both channels retire same-packet Reset marker");
}

static void identical_host_intent_does_not_rearm_finished_handoff() {
    BatteryFeedback feedback;
    feedback.on_link_open(46);
    feedback.observe_base_lights(LightCommand{true, 0x04, false, 0, 0, 0, false});
    feedback.set_enabled(true, 0);
    feedback.on_real_battery_report(46, 5, 0);
    accept(feedback, feedback.output_at(0), 0);
    feedback.set_enabled(false, 1);
    accept(feedback, feedback.output_at(1), 2);
    require(!feedback.restore_pending(), "first accepted synthetic Player handback finishes");

    const bool changed = feedback.observe_base_lights(
        LightCommand{true, 0x04, false, 0, 0, 0, false}, true);
    require(!changed && !feedback.restore_pending() && !feedback.output_at(3).synthetic_due,
            "identical qualified host light command causes no redundant handback");
}

static void dropped_new_player_intent_rearms_only_player_handoff() {
    BatteryFeedback feedback;
    feedback.on_link_open(47);
    feedback.observe_base_lights(LightCommand{true, 0x04, true, 90, 80, 70, false});
    feedback.set_enabled(true, 0);
    feedback.on_real_battery_report(47, 5, 0);
    accept(feedback, feedback.output_at(0), 0);
    feedback.set_enabled(false, 1);
    accept(feedback, feedback.output_at(1), 2);
    require(!feedback.restore_pending(), "first synthetic Player handback finishes");

    // The transport can reject the new host packet while retaining its latest
    // qualified intent. The older queued packet may have no Player Allow bit.
    const bool changed = feedback.observe_base_lights(
        LightCommand{true, 0x14, false, 0, 0, 0, false}, true);
    const Output latest = feedback.output_at(3);
    require(changed && latest.player_restore && latest.base_player_byte == 0x14 &&
                !latest.rgb_restore && !latest.reset_restore && latest.synthetic_due,
            "dropped latest Player intent rearms only Player without resetting RGB");

    SendResult rejected{};
    rejected.player_applied = true;
    rejected.synthetic_restore = true;
    feedback.on_send_result(latest, rejected, 4);
    require(feedback.restore_pending(), "rejected synthetic handback retains latest Player intent");
    accept(feedback, feedback.output_at(5), 5);
    require(!feedback.restore_pending(), "accepted retry finishes latest Player handback");
}

static void reset_and_rgb_intent_rearms_two_step_handoff() {
    BatteryFeedback feedback;
    feedback.on_link_open(48);
    feedback.observe_base_lights(LightCommand{true, 0x04, false, 0, 0, 0, false});
    feedback.set_enabled(true, 0);
    feedback.on_real_battery_report(48, 5, 0);
    accept(feedback, feedback.output_at(0), 0);
    feedback.set_enabled(false, 1);
    accept(feedback, feedback.output_at(1), 2);

    const bool changed = feedback.observe_base_lights(
        LightCommand{false, 0, true, 10, 20, 30, true}, true);
    const Output reset = feedback.output_at(3);
    require(changed && reset.reset_restore && reset.player_restore &&
                reset.rgb_restore && !reset.base_player_known && reset.base_rgb_known,
            "new Reset+RGB reopens both channels of handoff");
    SendResult delivered{};
    delivered.accepted = true;
    delivered.reset_applied = true;
    delivered.synthetic_restore = true;
    feedback.on_send_result(reset, delivered, 4);
    const Output rgb = feedback.output_at(4);
    require(!rgb.reset_restore && !rgb.player_restore && rgb.rgb_restore &&
                rgb.base_red == 10 && rgb.base_green == 20 && rgb.base_blue == 30,
            "accepted Reset releases unknown Player, then requests explicit RGB");
    accept(feedback, rgb, 5);
    require(!feedback.restore_pending(), "explicit RGB completes Reset+RGB handback");
}

static void queued_state_cannot_complete_synthetic_handoff() {
    BatteryFeedback feedback;
    feedback.on_link_open(49);
    feedback.observe_base_lights(LightCommand{true, 0x04, false, 0, 0, 0, false});
    feedback.set_enabled(true, 0);
    feedback.on_real_battery_report(49, 5, 0);
    accept(feedback, feedback.output_at(0), 0);
    feedback.set_enabled(false, 1);
    const Output restore = feedback.output_at(1);
    SendResult queued{};
    queued.accepted = true;
    queued.player_applied = true;
    feedback.on_send_result(restore, queued, 2);
    require(feedback.restore_pending() && feedback.output_at(2).synthetic_due,
            "queued state never certifies the separate synthetic handback");
    accept(feedback, feedback.output_at(3), 3);
    require(!feedback.restore_pending(), "synthetic frame certifies Player handback");
}

static void late_queued_reset_rearms_known_handoff() {
    BatteryFeedback feedback;
    feedback.on_link_open(50);
    feedback.observe_base_lights(LightCommand{true, 0x04, true, 20, 30, 40, false});
    feedback.set_enabled(true, 0);
    feedback.on_real_battery_report(50, 5, 0);
    accept(feedback, feedback.output_at(0), 0);
    feedback.set_enabled(false, 1);
    accept(feedback, feedback.output_at(1), 2);
    require(!feedback.restore_pending(), "known Player handback first finishes");

    const Output before_late_reset = feedback.output_at(3);
    SendResult late{};
    late.accepted = true;
    late.reset_applied = true;
    late.late_handoff_reset = true;
    feedback.on_send_result(before_late_reset, late, 3);
    const Output reapply = feedback.output_at(4);
    require(reapply.player_restore && reapply.rgb_restore &&
                reapply.base_player_byte == 0x04 && reapply.base_red == 20 &&
                reapply.synthetic_due,
            "late queued Reset re-arms both known host light channels");
    accept(feedback, reapply, 5);
    require(!feedback.restore_pending(), "accepted synthetic reapply finishes after late Reset");
}

int main() {
    require(sizeof(BatteryFeedback) <= 72,
            "battery feedback keeps the always-resident Core 0 state within 72 bytes");
    initial_baseline_and_all_gauge_levels();
    low_blink_and_silence_hold();
    raw_level_debounce_requires_later_matching_real_report();
    raw_transitions_and_retrigger_rules();
    tier_colors_and_skipped_tiers();
    five_delivery_gated_pulses_and_natural_restore();
    five_delivery_gated_blue_and_yellow_pulses();
    later_confirmed_drop_restarts_the_running_episode();
    charging_disable_restore_and_generation();
    cancel_before_first_accepted_output_does_not_disturb_lights();
    reset_lights_is_not_an_all_off_pattern();
    unknown_player_intent_requests_reset_fallback();
    host_reset_requires_reapplying_active_overlay();
    delayed_host_reset_reasserts_after_gauge_was_accepted();
    latest_base_intent_is_used_for_synchronous_restore();
    reset_and_explicit_lights_in_one_command_are_both_preserved();
    synthetic_unknown_reset_replays_unowned_known_rgb();
    reset_then_player_only_keeps_rgb_unknown();
    reset_then_rgb_only_keeps_player_unknown();
    same_packet_reset_marker_is_per_channel();
    identical_host_intent_does_not_rearm_finished_handoff();
    dropped_new_player_intent_rearms_only_player_handoff();
    reset_and_rgb_intent_rearms_two_step_handoff();
    queued_state_cannot_complete_synthetic_handoff();
    late_queued_reset_rearms_known_handoff();
    std::puts("PASS battery_feedback: baseline, raw debounce, gauge, pulse delivery, cancel, restore, generation");
}
