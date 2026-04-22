//
// Created by awalol on 2026/3/5.
//

#include "audio.h"
#include "bt.h"
#include "resample.h"
#include "tusb.h"
#include "usb.h"
#include <algorithm>
#include <chrono>
#include <iostream>

#include "class/vendor/vendor_device.h"

#define INPUT_CHANNELS    4
#define OUTPUT_CHANNELS   2
#define SAMPLE_SIZE       64
#define REPORT_SIZE       142
#define REPORT_ID         0x32
// #define VOLUME_GAIN       2
#define BUFFER_LENGTH     48

static WDL_Resampler resampler;
static uint8_t reportSeqCounter = 0;
static uint8_t packetCounter = 0;

void audio_loop() {
    // 1. 读取 USB 音频数据
    if (!tud_audio_available()) return;

    static int16_t raw[192];
    uint32_t bytes_read = tud_audio_read(raw, sizeof(raw));
    int frames = bytes_read / (INPUT_CHANNELS * sizeof(int16_t));
    if (frames == 0){
        return;
    }

    // 2. 从4ch中提取ch3/ch4，转换为float输入重采样器
    static WDL_ResampleSample *in_buf;
    int nframes = resampler.ResamplePrepare(frames, OUTPUT_CHANNELS, &in_buf);

    for (int i = 0; i < nframes; i++) {
        in_buf[i * 2] = (WDL_ResampleSample) raw[i * INPUT_CHANNELS + 2] / 32768.0f;
        in_buf[i * 2 + 1] = (WDL_ResampleSample) raw[i * INPUT_CHANNELS + 3] / 32768.0f;
    }

    // 3. 48kHz -> 3kHz 重采样
    static WDL_ResampleSample out_buf[SAMPLE_SIZE];  // 64 floats = 32帧 × 2ch
    int out_frames = resampler.ResampleOut(out_buf, nframes, SAMPLE_SIZE / OUTPUT_CHANNELS, OUTPUT_CHANNELS);

    static int8_t haptic_buf[SAMPLE_SIZE];
    static int haptic_buf_pos = 0;

    // 4. 转换为int8并缓冲，满64字节即组包发送
    for (int i = 0; i < out_frames; i++) {
        int val_l = (int) (out_buf[i * 2] * 127.0f * (volume[0] ?: 1));
        int val_r = (int) (out_buf[i * 2 + 1] * 127.0f * (volume[0] ?: 1));
        haptic_buf[haptic_buf_pos++] = (int8_t) std::clamp(val_l, -128, 127);
        haptic_buf[haptic_buf_pos++] = (int8_t) std::clamp(val_r, -128, 127);

        if (haptic_buf_pos != SAMPLE_SIZE) {
            continue;
        }
        send_haptics(haptic_buf);
        haptic_buf_pos = 0;
    }
}

void send_haptics(const int8_t* data) {
    static uint8_t pkt[REPORT_SIZE] = {};
    pkt[0] = REPORT_ID;
    pkt[1] = reportSeqCounter << 4;
    reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
    pkt[2] = 0x11 | 0 << 6 | 1 << 7;
    pkt[3] = 7;
    pkt[4] = 0b11111110;
    pkt[5] = BUFFER_LENGTH;
    pkt[6] = BUFFER_LENGTH;
    pkt[7] = BUFFER_LENGTH;
    pkt[8] = BUFFER_LENGTH;
    pkt[9] = BUFFER_LENGTH; // buffer length
    pkt[10] = packetCounter++;

    pkt[11] = 0x12 | 0 << 6 | 1 << 7;
    pkt[12] = SAMPLE_SIZE;
    memcpy(pkt + 13, data, SAMPLE_SIZE);

    bt_write(pkt, sizeof(pkt));
}

void send_speaker(const uint8_t* data) {
    static uint8_t pkt[270] = {};
    pkt[0] = 0x34;
    pkt[1] = reportSeqCounter << 4;
    reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
    pkt[2] = 0x11 | 0 << 6 | 1 << 7;
    pkt[3] = 7;
    pkt[4] = 0b11111110;
    pkt[5] = BUFFER_LENGTH;
    pkt[6] = BUFFER_LENGTH;
    pkt[7] = BUFFER_LENGTH;
    pkt[8] = BUFFER_LENGTH;
    pkt[9] = BUFFER_LENGTH; // buffer length
    pkt[10] = packetCounter++;
    pkt[11] = 0x16 | 0 << 6 | 1 << 7; // Speaker: 0x13 Headset: 0x16
    pkt[12] = 200;
    memcpy(pkt + 13, data, 200);

    bt_write(pkt, sizeof(pkt));
}

// 当前扬声器的逻辑是：扬声器要480frames，震动是512frames (32 * 16)，所以扬声器会先完成编码。
// 将编码完成的数据进行存储，在震动的时候一起发送出去。
// 正好也在之前HeadsetPlayMusic的Demo里面，10.666ms的周期发送才播放正常，而这个值也是震动的发送周期时间，具体计算请看SAXense
void send_combine(const uint8_t* speaker,const uint8_t* haptics) {
    static uint8_t pkt[334] = {};
    pkt[0] = 0x35;
    pkt[1] = reportSeqCounter << 4;
    reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
    pkt[2] = 0x11 | 0 << 6 | 1 << 7;
    pkt[3] = 7;
    pkt[4] = 0b11111110;
    pkt[5] = BUFFER_LENGTH;
    pkt[6] = BUFFER_LENGTH;
    pkt[7] = BUFFER_LENGTH;
    pkt[8] = BUFFER_LENGTH;
    pkt[9] = BUFFER_LENGTH; // buffer length
    pkt[10] = packetCounter++;
    pkt[11] = 0x13 | 0 << 6 | 1 << 7; // Speaker: 0x13 Headset: 0x16
    pkt[12] = 200;
    memcpy(pkt + 13, speaker, 200);
    pkt[213] = 0x12 | 0 << 6 | 1 << 7;
    pkt[214] = 64;
    memcpy(pkt + 215,haptics,64);
    bt_write(pkt, sizeof(pkt));
}

void audio_init() {
    resampler.SetMode(true, 0, false);
    resampler.SetRates(48000, 3000);
    resampler.SetFeedMode(true);
    resampler.Prealloc(2, 480, 32);
}

void tud_vendor_rx_cb(uint8_t idx, const uint8_t *buffer, uint32_t bufsize) {
    (void)idx;
    (void)buffer;
    (void)bufsize;
}

// 未启用，备用
void vendor_loop() {
    while (tud_vendor_available()) {
        static int speaker_buf_pos = 0;
        static uint8_t buf[64];
        static uint8_t save[264];
        static bool wait = false;

        const uint32_t count = tud_vendor_read(buf,sizeof(buf));
        // 防溢出
        if (wait) {
            if (count == 8) {
                wait = false;
                speaker_buf_pos = 0;
            }
            return;
        }
        if (speaker_buf_pos + count > 264) {
            wait = true;
            return;
        }
        memcpy(save + speaker_buf_pos,buf,count);
        speaker_buf_pos += count;
        if (count == 8) {
            static auto last = time_us_32();
            auto now = time_us_32();
            // send_combine(save,save + 200);
            std::cout << (now - last) << std::endl;
            last = now;
        }
    }
}
