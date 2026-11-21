#include "platform.h"
#include <cassert>

static USBGetStateData st;

static void finger(int i, bool down, int x = 0, int y = 0, int index = 1) {
    st.Touch.Finger[i].NotTouching = down ? 0 : 1;
    st.Touch.Finger[i].Index = index;
    st.Touch.Finger[i].FingerX = x;
    st.Touch.Finger[i].FingerY = y;
}
static void tick(uint32_t ms = 4) { now_us += ms * 1000ull; touchpad_mouse_tick(st); }
static void lift() { finger(0, false); finger(1, false); }
static void reset_out() { out = {}; nreports = 0; }

int main() {
    cfg.touchpad_mouse = TouchpadMouseShared;
    cfg.touchpad_mouse_speed = 10;
    touchpad_mouse_enumerated = true;
    lift(); tick();

    // One finger: relative motion, no motion on the first contact.
    reset_out();
    finger(0, true, 100, 100, 1); tick(); assert(nreports == 0);
    finger(0, true, 104, 103, 1); tick(); assert(total_x > 0 && total_y > 0);
    long px = total_x;
    finger(0, true, 90, 103, 1); tick(); assert(total_x < px);
    lift(); tick(200); // long touch: no tap
    assert(out.buttons == 0);

    // A new finger index never jumps from the old coordinates.
    total_x = total_y = 0;
    finger(0, true, 1800, 900, 2); tick(); assert(total_x == 0 && total_y == 0);
    lift(); tick(300);
    touchpad_mouse_task();

    // Quick still tap -> left click press, then release after the hold time.
    finger(0, true, 500, 500, 3); tick(); finger(0, true, 502, 500, 3); tick();
    lift(); tick(); assert(out.buttons == 0x01);
    now_us += 25000; touchpad_mouse_task(); assert(out.buttons == 0);

    // Two-finger tap -> right click.
    finger(0, true, 500, 500, 4); finger(1, true, 800, 500, 5); tick();
    lift(); tick(); assert(out.buttons == 0x02);
    now_us += 25000; touchpad_mouse_task(); assert(out.buttons == 0);
    tick(300);

    // Physical click: left with one finger, right with two; no tap afterwards.
    finger(0, true, 500, 500, 6); st.ButtonPad = 1; tick(); assert(out.buttons == 0x01);
    st.ButtonPad = 0; tick(); assert(out.buttons == 0);
    lift(); tick(); assert(out.buttons == 0);
    finger(0, true, 500, 500, 7); finger(1, true, 900, 500, 8); st.ButtonPad = 1; tick();
    assert(out.buttons == 0x02);
    st.ButtonPad = 0; lift(); tick(300); assert(out.buttons == 0);

    // Two fingers moving -> scroll, never cursor motion.
    total_x = total_y = total_wheel = 0;
    finger(0, true, 500, 500, 9); finger(1, true, 900, 500, 10); tick(300);
    for (int i = 1; i <= 10; ++i) {
        finger(0, true, 500, 500 - 10 * i, 9); finger(1, true, 900, 500 - 10 * i, 10); tick();
    }
    assert(total_wheel < 0 && total_x == 0 && total_y == 0);
    const long slow_wheel = total_wheel;
    // Lifting one finger after a scroll does not move the cursor.
    finger(1, false); finger(0, true, 600, 300, 9); tick(); assert(total_x == 0);
    lift(); tick(300);

    // Scroll speed doubles the notches; invert flips the direction.
    auto scroll_up = [](int idx) {
        total_wheel = 0;
        finger(0, true, 500, 500, idx); finger(1, true, 900, 500, idx + 1); tick(300);
        for (int i = 1; i <= 10; ++i) {
            finger(0, true, 500, 500 - 10 * i, idx); finger(1, true, 900, 500 - 10 * i, idx + 1); tick();
        }
        lift(); tick(300);
        return total_wheel;
    };
    cfg.touchpad_mouse_scroll_speed = 20;
    assert(scroll_up(20) < slow_wheel);
    cfg.touchpad_mouse_scroll_invert = 1;
    assert(scroll_up(22) > 0);
    cfg.touchpad_mouse_scroll_speed = 10; cfg.touchpad_mouse_scroll_invert = 0;

    // Acceleration: a fast swipe moves further per unit with more acceleration,
    // and exactly 1:1 (speed 10) with it off.
    auto swipe = [](int idx) {
        total_x = 0;
        finger(0, true, 100, 500, idx); tick();
        for (int i = 1; i <= 10; ++i) { finger(0, true, 100 + 60 * i, 500, idx); tick(); }
        lift(); tick(300);
        return total_x;
    };
    cfg.touchpad_mouse_accel = 0;
    assert(swipe(30) == 600);
    cfg.touchpad_mouse_accel = 3;
    const long mild = swipe(31);
    cfg.touchpad_mouse_accel = 10;
    const long strong = swipe(32);
    assert(600 < mild && mild < strong);
    cfg.touchpad_mouse_accel = 3;

    // Tap to click can be turned off.
    cfg.touchpad_mouse_tap_disable = 1; reset_out();
    finger(0, true, 500, 500, 24); tick(); lift(); tick();
    now_us += 25000; touchpad_mouse_task(); assert(nreports == 0);
    cfg.touchpad_mouse_tap_disable = 0;

    // Exclusive mode hides the touchpad from the gamepad report while active.
    cfg.touchpad_mouse = TouchpadMouseExclusive;
    USBGetStateData r{};
    r.Touch.Finger[0].NotTouching = 0; r.ButtonPad = 1;
    touchpad_mouse_filter_report(r);
    assert(r.Touch.Finger[0].NotTouching == 1 && r.ButtonPad == 0);

    // PS + click toggles off: no motion, and the gamepad sees the touchpad again.
    st.ButtonHome = 1; st.ButtonPad = 1; tick(300);
    USBGetStateData r2{}; r2.ButtonPad = 1; touchpad_mouse_filter_report(r2);
    assert(r2.ButtonPad == 0); // The chord itself stays hidden.
    assert(out.buttons == 0);
    st.ButtonHome = 0; st.ButtonPad = 0; tick();
    total_x = 0;
    finger(0, true, 100, 100, 11); tick(); finger(0, true, 200, 100, 11); tick();
    assert(total_x == 0);
    USBGetStateData r3{}; r3.Touch.Finger[0].NotTouching = 0; r3.ButtonPad = 1;
    touchpad_mouse_filter_report(r3);
    assert(r3.Touch.Finger[0].NotTouching == 0 && r3.ButtonPad == 1);
    lift(); tick(300);
    // ...and back on.
    st.ButtonHome = 1; st.ButtonPad = 1; tick(); st.ButtonHome = 0; st.ButtonPad = 0; tick();
    finger(0, true, 100, 100, 12); tick(); finger(0, true, 200, 100, 12); tick();
    assert(total_x > 0);
    lift(); tick(300);

    // Backed-up endpoint: motion accumulates and is delivered on retry.
    total_x = 0; ready = false;
    finger(0, true, 100, 100, 13); tick(); finger(0, true, 140, 100, 13); tick();
    assert(total_x == 0);
    ready = true; touchpad_mouse_task(); assert(total_x > 0);
    lift(); tick(300);

    // Off, or not enumerated: nothing is sent and the report is untouched.
    cfg.touchpad_mouse = TouchpadMouseOff; reset_out();
    finger(0, true, 100, 100, 14); tick(); finger(0, true, 300, 100, 14); tick();
    assert(nreports == 0);
    USBGetStateData r4{}; r4.ButtonPad = 1; touchpad_mouse_filter_report(r4); assert(r4.ButtonPad == 1);
    return 0;
}
