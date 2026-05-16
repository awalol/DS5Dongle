#include "oled.h"
#include "oled_font.h"
#include "bt.h"
#include "audio.h"

#include <cstdio>
#include <cstring>
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "pico/time.h"

extern uint8_t interrupt_in_data[63]; // defined in main.cpp

namespace {

constexpr uint kPinDC = 8;
constexpr uint kPinCS = 9;
constexpr uint kPinCLK = 10;
constexpr uint kPinMOSI = 11;
constexpr uint kPinRST = 12;
constexpr uint kPinKey0 = 15;
constexpr uint kPinKey1 = 17;

constexpr int kW = 128;
constexpr int kH = 64;
constexpr int kRowBytes = kW / 8;
constexpr int kFbBytes = kRowBytes * kH;

uint8_t fb[kFbBytes];

uint32_t last_render_us = 0;
constexpr uint32_t kFrameUs = 100000;
bool key0_prev = true;
bool key1_prev = true;
uint32_t key0_t_us = 0;
uint32_t key1_t_us = 0;
constexpr uint32_t kDebounceUs = 20000;

constexpr int kNumScreens = 6;
int current_screen = 0;

uint8_t lb_r = 0, lb_g = 0, lb_b = 0;

uint32_t rumble_off_at_us = 0;
bool rumble_active = false;
constexpr uint32_t kRumbleBurstUs = 250000;

int trigger_preset = 0;
const char* const kTrigPresetNames[] = {"Off", "Feedback", "Weapon", "Vibration", "Bow", "Gallop", "Machine"};
constexpr int kNumTrigPresets = 7;

void cmd(uint8_t c) {
    gpio_put(kPinDC, 0);
    gpio_put(kPinCS, 0);
    spi_write_blocking(spi1, &c, 1);
    gpio_put(kPinCS, 1);
}

void data_byte(uint8_t d) {
    gpio_put(kPinDC, 1);
    gpio_put(kPinCS, 0);
    spi_write_blocking(spi1, &d, 1);
    gpio_put(kPinCS, 1);
}

uint8_t reverse_byte(uint8_t b) {
    b = ((b & 0x55) << 1) | ((b & 0xAA) >> 1);
    b = ((b & 0x33) << 2) | ((b & 0xCC) >> 2);
    b = ((b & 0x0F) << 4) | ((b & 0xF0) >> 4);
    return b;
}

void hw_reset() {
    gpio_put(kPinRST, 1); sleep_ms(100);
    gpio_put(kPinRST, 0); sleep_ms(100);
    gpio_put(kPinRST, 1); sleep_ms(100);
}

void sh1107_init() {
    cmd(0xAE);
    cmd(0x00); cmd(0x10);
    cmd(0xB0);
    cmd(0xDC); cmd(0x00);
    cmd(0x81); cmd(0x6F);
    cmd(0x21);
    cmd(0xA0);
    cmd(0xC0);
    cmd(0xA4);
    cmd(0xA6);
    cmd(0xA8); cmd(0x3F);
    cmd(0xD3); cmd(0x60);
    cmd(0xD5); cmd(0x41);
    cmd(0xD9); cmd(0x22);
    cmd(0xDB); cmd(0x35);
    cmd(0xAD); cmd(0x8A);
    sleep_ms(50);
    cmd(0xAF);
}

void flush_fb() {
    cmd(0xB0);
    for (int j = 0; j < kH; j++) {
        const uint8_t col = kH - 1 - j;
        cmd(0x00 + (col & 0x0F));
        cmd(0x10 + (col >> 4));
        for (int i = 0; i < kRowBytes; i++) {
            data_byte(reverse_byte(fb[j * kRowBytes + i]));
        }
    }
}

void fb_clear() { memset(fb, 0, sizeof(fb)); }

void px(int x, int y, bool on) {
    if (x < 0 || x >= kW || y < 0 || y >= kH) return;
    uint8_t *p = &fb[y * kRowBytes + (x / 8)];
    uint8_t m = 1 << (7 - (x % 8));
    if (on) *p |= m; else *p &= ~m;
}

void rect_outline(int x, int y, int w, int h) {
    for (int i = 0; i < w; i++) { px(x + i, y, true); px(x + i, y + h - 1, true); }
    for (int i = 0; i < h; i++) { px(x, y + i, true); px(x + w - 1, y + i, true); }
}

void rect_filled(int x, int y, int w, int h) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            px(x + i, y + j, true);
}

void draw_char(int x, int y, char c) {
    if (c < 0x20 || c > 0x7E) return;
    const uint8_t *g = kFont5x7[c - 0x20];
    for (int col = 0; col < kFontW; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < kFontH; row++) {
            if (bits & (1 << row)) px(x + col, y + row, true);
        }
    }
}

void draw_text(int x, int y, const char *s) {
    while (*s) {
        draw_char(x, y, *s++);
        x += 6;
    }
}

void send_rumble(uint8_t amplitude) {
    uint8_t pkt[78] = {};
    pkt[0] = 0x31;
    pkt[1] = 0x00;
    pkt[2] = 0x10;
    pkt[3] = 0x03;
    pkt[5] = amplitude;
    pkt[6] = amplitude;
    bt_write(pkt, sizeof(pkt));
}

void rumble_burst_tick(uint32_t now) {
    if (rumble_active && (int32_t)(now - rumble_off_at_us) >= 0) {
        send_rumble(0);
        rumble_active = false;
    }
}

// Trigger effect param format follows dualsensectl's reverse-engineering.
// Modes 0x21/0x25/0x26 use bitpacked 10-zone arrays, not raw position bytes.
void send_trigger_effect(int preset) {
    uint8_t pkt[78] = {};
    pkt[0] = 0x31;
    pkt[2] = 0x10;
    pkt[3] = 0x0C; // valid_flag0: RIGHT_TRIGGER_MOTOR_ENABLE | LEFT_TRIGGER_MOTOR_ENABLE

    uint8_t mode = 0x05; // OFF
    uint8_t p[9] = {0};

    switch (preset) {
        case 0: // Off
            mode = 0x05;
            break;
        case 1: { // Feedback — all 10 zones at max strength 8
            mode = 0x21;
            const uint16_t active = 0x03FF;
            uint32_t strength = 0;
            for (int i = 0; i < 10; i++) strength |= (uint32_t)(7u << (3 * i));
            p[0] = active & 0xFF;
            p[1] = (active >> 8) & 0xFF;
            p[2] = strength & 0xFF;
            p[3] = (strength >> 8) & 0xFF;
            p[4] = (strength >> 16) & 0xFF;
            p[5] = (strength >> 24) & 0xFF;
            break;
        }
        case 2: { // Weapon — snap between positions 3 and 5, force 8
            mode = 0x25;
            const uint16_t start_stop = (1u << 3) | (1u << 5);
            p[0] = start_stop & 0xFF;
            p[1] = (start_stop >> 8) & 0xFF;
            p[2] = 7; // force = strength - 1
            break;
        }
        case 3: { // Vibration — all 10 zones at amplitude 8, frequency 30 Hz
            mode = 0x26;
            const uint16_t active = 0x03FF;
            uint32_t strength = 0;
            for (int i = 0; i < 10; i++) strength |= (uint32_t)(7u << (3 * i));
            p[0] = active & 0xFF;
            p[1] = (active >> 8) & 0xFF;
            p[2] = strength & 0xFF;
            p[3] = (strength >> 8) & 0xFF;
            p[4] = (strength >> 16) & 0xFF;
            p[5] = (strength >> 24) & 0xFF;
            p[8] = 30;
            break;
        }
        case 4: { // Bow — drawing resistance + snap at position 6
            mode = 0x22;
            const uint16_t start_stop = (1u << 2) | (1u << 6);
            const uint8_t force_pair = 7u | (7u << 3); // strength=8, snap=8
            p[0] = start_stop & 0xFF;
            p[1] = (start_stop >> 8) & 0xFF;
            p[2] = force_pair;
            break;
        }
        case 5: { // Galloping
            mode = 0x23;
            const uint16_t start_stop = (1u << 0) | (1u << 9);
            const uint8_t ratio = (5u & 0x07) | ((1u & 0x07) << 3);
            p[0] = start_stop & 0xFF;
            p[1] = (start_stop >> 8) & 0xFF;
            p[2] = ratio;
            p[3] = 5; // frequency
            break;
        }
        case 6: { // Machine gun
            mode = 0x27;
            const uint16_t start_stop = (1u << 1) | (1u << 8);
            const uint8_t force_pair = 7u | (7u << 3);
            p[0] = start_stop & 0xFF;
            p[1] = (start_stop >> 8) & 0xFF;
            p[2] = force_pair;
            p[3] = 20; // frequency
            p[4] = 0;  // period
            break;
        }
    }

    pkt[13] = mode;
    for (int i = 0; i < 9; i++) pkt[14 + i] = p[i];
    pkt[24] = mode;
    for (int i = 0; i < 9; i++) pkt[25 + i] = p[i];

    bt_write(pkt, sizeof(pkt));
}

void send_lightbar_color(uint8_t r, uint8_t g, uint8_t b);

void handle_buttons() {
    const uint32_t now = time_us_32();
    const bool k0 = gpio_get(kPinKey0);
    const bool k1 = gpio_get(kPinKey1);
    if (!k0 && key0_prev && (now - key0_t_us) > kDebounceUs) {
        key0_t_us = now;
        current_screen = (current_screen + 1) % kNumScreens;
        last_render_us = 0;
    }
    if (!k1 && key1_prev && (now - key1_t_us) > kDebounceUs) {
        key1_t_us = now;
        if (current_screen == 2) {
            trigger_preset = (trigger_preset + 1) % kNumTrigPresets;
            send_trigger_effect(trigger_preset);
        } else if (current_screen == 5) {
            send_lightbar_color(lb_r, lb_g, lb_b);
        } else {
            send_rumble(0xC0);
            rumble_active = true;
            rumble_off_at_us = now + kRumbleBurstUs;
        }
    }
    key0_prev = k0;
    key1_prev = k1;
}

void render_screen() {
    fb_clear();

    const bool connected = bt_is_connected();

    draw_text(0, 0, "DS5 Bridge v0.5.4");
    draw_text(104, 0, connected ? "[ON]" : "[--]");

    if (connected) {
        uint8_t a[6];
        bt_get_addr(a);
        char buf[24];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                 a[0], a[1], a[2], a[3], a[4], a[5]);
        draw_text(0, 9, buf);

        const uint8_t pwr = interrupt_in_data[52];
        int pct = (pwr & 0x0F) * 10;
        if (pct > 100) pct = 100;
        const uint8_t pstate = pwr >> 4;
        char marker = ' ';
        if (pstate == 1) marker = '+';      // Charging
        else if (pstate == 2) marker = '*'; // Complete
        else if (pstate >= 0xA) marker = '!'; // Error
        char bbuf[16];
        snprintf(bbuf, sizeof(bbuf), "Batt:%3d%%%c", pct, marker);
        draw_text(0, 18, bbuf);
        rect_outline(64, 18, 64, 7);
        int fill = (pct * 60) / 100;
        if (fill > 0) rect_filled(66, 20, fill, 3);

        rect_outline(0, 30, 32, 32);
        int lx = 2 + (interrupt_in_data[0] * 27) / 255;
        int ly = 32 + (interrupt_in_data[1] * 27) / 255;
        rect_filled(lx - 1, ly - 1, 3, 3);

        rect_outline(96, 30, 32, 32);
        int rx = 98 + (interrupt_in_data[2] * 27) / 255;
        int ry = 32 + (interrupt_in_data[3] * 27) / 255;
        rect_filled(rx - 1, ry - 1, 3, 3);

        // L2/R2 analog trigger bars (vertical, fill from bottom)
        rect_outline(32, 33, 4, 29);
        const int l2_fill = (interrupt_in_data[4] * 27) / 255;
        if (l2_fill > 0) rect_filled(33, 61 - l2_fill, 2, l2_fill);
        rect_outline(92, 33, 4, 29);
        const int r2_fill = (interrupt_in_data[5] * 27) / 255;
        if (r2_fill > 0) rect_filled(93, 61 - r2_fill, 2, r2_fill);

        const uint8_t b7 = interrupt_in_data[7];
        const uint8_t b8 = interrupt_in_data[8];

        // D-pad indicator (4 directions; lit for primary + diagonals)
        const int dp = b7 & 0x0F;
        const bool dp_n = (dp == 7 || dp == 0 || dp == 1);
        const bool dp_e = (dp == 1 || dp == 2 || dp == 3);
        const bool dp_s = (dp == 3 || dp == 4 || dp == 5);
        const bool dp_w = (dp == 5 || dp == 6 || dp == 7);
        const int dcx = 46, dcy = 46;
        auto dot = [&](int dx, int dy, bool on) {
            if (on) rect_filled(dcx + dx - 1, dcy + dy - 1, 3, 3);
            else    rect_outline(dcx + dx - 1, dcy + dy - 1, 3, 3);
        };
        dot(0,  -5, dp_n);
        dot(5,   0, dp_e);
        dot(0,   5, dp_s);
        dot(-5,  0, dp_w);

        const int fcx = 64, fcy = 46;
        auto sq = [&](int dx, int dy, bool on) {
            if (on) rect_filled(fcx + dx - 2, fcy + dy - 2, 5, 5);
            else    rect_outline(fcx + dx - 2, fcy + dy - 2, 5, 5);
        };
        // shift face buttons right so they don't collide with d-pad
        const int fcx_off = 18;
        sq(fcx_off + 0,  -8, b7 & 0x80); // Triangle
        sq(fcx_off + 8,   0, b7 & 0x40); // Circle
        sq(fcx_off + 0,   8, b7 & 0x20); // Cross
        sq(fcx_off - 8,   0, b7 & 0x10); // Square

        if (b8 & 0x01) rect_filled(36, 30, 12, 3); else rect_outline(36, 30, 12, 3); // L1
        if (b8 & 0x02) rect_filled(80, 30, 12, 3); else rect_outline(80, 30, 12, 3); // R1
    } else {
        draw_text(0, 9, "(waiting for DS5)");
    }

    flush_fb();
}

void render_screen_diag() {
    fb_clear();

    draw_text(0, 0, "Diagnostics");

    const uint32_t uptime_s = time_us_32() / 1000000u;
    const uint32_t h = uptime_s / 3600u;
    const uint32_t m = (uptime_s / 60u) % 60u;
    const uint32_t s = uptime_s % 60u;
    char buf[24];
    snprintf(buf, sizeof(buf), "Up:%luh %02lum %02lus", (unsigned long)h, (unsigned long)m, (unsigned long)s);
    draw_text(0, 9, buf);

    snprintf(buf, sizeof(buf), "HCI errs:    %lu", (unsigned long)bt_hci_err_count());
    draw_text(0, 18, buf);
    snprintf(buf, sizeof(buf), "Aud drops:   %lu", (unsigned long)audio_fifo_drops());
    draw_text(0, 27, buf);
    snprintf(buf, sizeof(buf), "Opus drops:  %lu", (unsigned long)opus_fifo_drops());
    draw_text(0, 36, buf);

    snprintf(buf, sizeof(buf), "BT: %s", bt_is_connected() ? "connected" : "waiting");
    draw_text(0, 45, buf);

    draw_text(0, 56, "K0=next K1=rumble");

    flush_fb();
}

void render_screen_triggers() {
    fb_clear();
    draw_text(0, 0, "Trigger Test");

    char buf[24];
    snprintf(buf, sizeof(buf), "Mode: %s", kTrigPresetNames[trigger_preset]);
    draw_text(0, 12, buf);

    if (bt_is_connected()) {
        const uint8_t l2 = interrupt_in_data[4];
        const uint8_t r2 = interrupt_in_data[5];
        snprintf(buf, sizeof(buf), "L2:%3d  R2:%3d", l2, r2);
        draw_text(0, 24, buf);

        rect_outline(0, 35, 60, 9);
        int lfill = (l2 * 56) / 255;
        if (lfill > 0) rect_filled(2, 37, lfill, 5);
        rect_outline(68, 35, 60, 9);
        int rfill = (r2 * 56) / 255;
        if (rfill > 0) rect_filled(70, 37, rfill, 5);
    } else {
        draw_text(0, 24, "(no controller)");
    }

    draw_text(0, 56, "K0=next K1=cycle");
    flush_fb();
}

void render_screen_gyro() {
    fb_clear();
    draw_text(0, 0, "Gyro Tilt");
    if (bt_is_connected()) {
        int16_t ax, ay, az;
        memcpy(&ax, &interrupt_in_data[21], 2);
        memcpy(&ay, &interrupt_in_data[23], 2);
        memcpy(&az, &interrupt_in_data[25], 2);
        char buf[16];
        snprintf(buf, sizeof(buf), "X%+5d", ax); draw_text(0,  10, buf);
        snprintf(buf, sizeof(buf), "Y%+5d", ay); draw_text(44, 10, buf);
        snprintf(buf, sizeof(buf), "Z%+5d", az); draw_text(88, 10, buf);

        const int bx = 44, by = 22, bw = 40, bh = 40;
        rect_outline(bx, by, bw, bh);
        for (int x = bx + 1; x < bx + bw - 1; x++) px(x, by + bh / 2, true);
        for (int y = by + 1; y < by + bh - 1; y++) px(bx + bw / 2, y, true);
        int dx = ((int)ax * (bw / 2 - 3)) / 8192;
        int dy = ((int)ay * (bh / 2 - 3)) / 8192;
        int cx = bx + bw / 2 + dx;
        int cy = by + bh / 2 + dy;
        if (cx < bx + 2) cx = bx + 2;
        if (cx > bx + bw - 3) cx = bx + bw - 3;
        if (cy < by + 2) cy = by + 2;
        if (cy > by + bh - 3) cy = by + bh - 3;
        rect_filled(cx - 1, cy - 1, 3, 3);
    } else {
        draw_text(0, 30, "(no controller)");
    }
    flush_fb();
}

void render_screen_touchpad() {
    fb_clear();
    draw_text(0, 0, "Touchpad");
    if (bt_is_connected()) {
        rect_outline(4, 12, 120, 30);
        int active = 0;
        for (int finger = 0; finger < 2; finger++) {
            const int off = 32 + finger * 4;
            const uint32_t f = (uint32_t)interrupt_in_data[off] |
                               ((uint32_t)interrupt_in_data[off + 1] << 8) |
                               ((uint32_t)interrupt_in_data[off + 2] << 16) |
                               ((uint32_t)interrupt_in_data[off + 3] << 24);
            const bool not_touching = (f >> 7) & 1u;
            if (not_touching) continue;
            const uint16_t fx = (f >> 8) & 0xFFFu;
            const uint16_t fy = (f >> 20) & 0xFFFu;
            int sx = 5 + ((int)fx * 114) / 1919;
            int sy = 13 + ((int)fy * 26) / 1079;
            if (sx < 5)   sx = 5;
            if (sx > 122) sx = 122;
            if (sy < 13)  sy = 13;
            if (sy > 40)  sy = 40;
            rect_filled(sx - 1, sy - 1, 3, 3);
            active++;
        }
        char buf[20];
        snprintf(buf, sizeof(buf), "Fingers: %d", active);
        draw_text(0, 46, buf);
    } else {
        draw_text(0, 30, "(no controller)");
    }
    draw_text(0, 56, "K0=next");
    flush_fb();
}

void send_lightbar_color(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t pkt[78] = {};
    pkt[0] = 0x31;
    pkt[2] = 0x10;
    pkt[4] = 0x04; // valid_flag1: LIGHTBAR_CONTROL_ENABLE (bit 2)
    pkt[47] = r;   // lightbar_red
    pkt[48] = g;   // lightbar_green
    pkt[49] = b;   // lightbar_blue
    bt_write(pkt, sizeof(pkt));
}

void render_screen_lightbar() {
    fb_clear();
    draw_text(0, 0, "Lightbar");
    if (bt_is_connected()) {
        int16_t ax, ay, az;
        memcpy(&ax, &interrupt_in_data[21], 2);
        memcpy(&ay, &interrupt_in_data[23], 2);
        memcpy(&az, &interrupt_in_data[25], 2);
        const int rr = ((int)ax + 8192) * 255 / 16384;
        const int gg = ((int)ay + 8192) * 255 / 16384;
        const int bb = ((int)az + 8192) * 255 / 16384;
        lb_r = (uint8_t)(rr < 0 ? 0 : rr > 255 ? 255 : rr);
        lb_g = (uint8_t)(gg < 0 ? 0 : gg > 255 ? 255 : gg);
        lb_b = (uint8_t)(bb < 0 ? 0 : bb > 255 ? 255 : bb);

        char buf[16];
        snprintf(buf, sizeof(buf), "R:%3u", lb_r); draw_text(0,  12, buf);
        snprintf(buf, sizeof(buf), "G:%3u", lb_g); draw_text(44, 12, buf);
        snprintf(buf, sizeof(buf), "B:%3u", lb_b); draw_text(88, 12, buf);

        // Three intensity bars below each label
        const int by = 22, bh = 8;
        rect_outline(0,  by, 40, bh); int rf = (lb_r * 36) / 255; if (rf > 0) rect_filled(2,  by + 2, rf, bh - 4);
        rect_outline(44, by, 40, bh); int gf = (lb_g * 36) / 255; if (gf > 0) rect_filled(46, by + 2, gf, bh - 4);
        rect_outline(88, by, 40, bh); int bf = (lb_b * 36) / 255; if (bf > 0) rect_filled(90, by + 2, bf, bh - 4);

        draw_text(0, 38, "Tilt X/Y/Z = R/G/B");
        draw_text(0, 48, "LIVE preview!");

        send_lightbar_color(lb_r, lb_g, lb_b);
    } else {
        draw_text(0, 30, "(no controller)");
    }
    draw_text(0, 56, "K0=next K1=lock");
    flush_fb();
}

void boot_splash() {
    fb_clear();
    auto cx_for = [](const char* s) {
        int n = 0; while (s[n]) n++;
        return (128 - (n * 6 - 1)) / 2;
    };
    const char* l1 = "DS5 Bridge";
    const char* l2 = "v0.5.4";
    const char* l3 = "Pico2W + OLED";
    draw_text(cx_for(l1), 16, l1);
    draw_text(cx_for(l2), 30, l2);
    draw_text(cx_for(l3), 44, l3);
    flush_fb();
    sleep_ms(1500);
}

} // namespace

void oled_init() {
    spi_init(spi1, 10 * 1000 * 1000);
    gpio_set_function(kPinCLK, GPIO_FUNC_SPI);
    gpio_set_function(kPinMOSI, GPIO_FUNC_SPI);

    gpio_init(kPinCS);   gpio_set_dir(kPinCS, GPIO_OUT);  gpio_put(kPinCS, 1);
    gpio_init(kPinDC);   gpio_set_dir(kPinDC, GPIO_OUT);  gpio_put(kPinDC, 0);
    gpio_init(kPinRST);  gpio_set_dir(kPinRST, GPIO_OUT); gpio_put(kPinRST, 1);

    gpio_init(kPinKey0); gpio_set_dir(kPinKey0, GPIO_IN); gpio_pull_up(kPinKey0);
    gpio_init(kPinKey1); gpio_set_dir(kPinKey1, GPIO_IN); gpio_pull_up(kPinKey1);

    hw_reset();
    sh1107_init();
    fb_clear();
    boot_splash();
}

void oled_loop() {
    handle_buttons();
    const uint32_t now = time_us_32();
    rumble_burst_tick(now);
    if ((now - last_render_us) < kFrameUs) return;
    last_render_us = now;
    switch (current_screen) {
        case 0: render_screen();           break;
        case 1: render_screen_diag();      break;
        case 2: render_screen_triggers();  break;
        case 3: render_screen_gyro();      break;
        case 4: render_screen_touchpad();  break;
        case 5: render_screen_lightbar();  break;
    }
}
