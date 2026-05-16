#include "oled.h"
#include "oled_font.h"
#include "bt.h"

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

void send_test_rumble() {
    uint8_t pkt[78] = {};
    pkt[0] = 0x31;
    pkt[1] = 0x00;
    pkt[2] = 0x10;
    pkt[3] = 0x03;
    pkt[5] = 0xC0;
    pkt[6] = 0xC0;
    bt_write(pkt, sizeof(pkt));
}

void handle_buttons() {
    const uint32_t now = time_us_32();
    const bool k0 = gpio_get(kPinKey0);
    const bool k1 = gpio_get(kPinKey1);
    if (!k0 && key0_prev && (now - key0_t_us) > kDebounceUs) {
        key0_t_us = now;
        // KEY0 placeholder: screen flash by inverting
        for (size_t i = 0; i < sizeof(fb); i++) fb[i] = ~fb[i];
        flush_fb();
    }
    if (!k1 && key1_prev && (now - key1_t_us) > kDebounceUs) {
        key1_t_us = now;
        send_test_rumble();
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
        char bbuf[16];
        snprintf(bbuf, sizeof(bbuf), "Batt:%3d%%", pct);
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

        const uint8_t b7 = interrupt_in_data[7];
        const uint8_t b8 = interrupt_in_data[8];
        const int cx = 64;
        const int cy = 46;
        auto sq = [&](int dx, int dy, bool on) {
            if (on) rect_filled(cx + dx - 2, cy + dy - 2, 5, 5);
            else    rect_outline(cx + dx - 2, cy + dy - 2, 5, 5);
        };
        sq(0,  -8, b7 & 0x80); // Triangle
        sq(8,   0, b7 & 0x40); // Circle
        sq(0,   8, b7 & 0x20); // Cross
        sq(-8,  0, b7 & 0x10); // Square

        if (b8 & 0x01) rect_filled(36, 30, 12, 3); else rect_outline(36, 30, 12, 3); // L1
        if (b8 & 0x02) rect_filled(80, 30, 12, 3); else rect_outline(80, 30, 12, 3); // R1
    } else {
        draw_text(0, 9, "(waiting for DS5)");
    }

    flush_fb();
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
    flush_fb();
}

void oled_loop() {
    handle_buttons();
    const uint32_t now = time_us_32();
    if ((now - last_render_us) < kFrameUs) return;
    last_render_us = now;
    render_screen();
}
