//
// Created by awalol on 2026/3/5.
//

#include "audio.h"
#include "bt.h"
#include "resample.h"
#include "tusb.h"
#include "pico/time.h"
#include <algorithm>

#define INPUT_CHANNELS    4
#define OUTPUT_CHANNELS   2
#define SAMPLE_SIZE   64
#define REPORT_SIZE   142
#define REPORT_ID     0x32

static WDL_Resampler resampler;
static uint8_t reportSeqCounter = 0;
static uint8_t packetCounter = 0;

void process_audio() {
    // 1. 读取 USB 音频数据
    if (!tud_audio_available()){
        return;
    }

    int16_t raw[1024];
    uint32_t bytes_read = tud_audio_read(raw, sizeof(raw));
    int frames = bytes_read / (INPUT_CHANNELS * sizeof(int16_t));
    if (frames == 0){
        return;
    }

    // 2. 从4ch中提取ch3/ch4，转换为float输入重采样器
    WDL_ResampleSample *in_buf;
    int nsamples = resampler.ResamplePrepare(frames, OUTPUT_CHANNELS, &in_buf);

    for (int i = 0; i < nsamples; i++) {
        in_buf[i * 2] = (WDL_ResampleSample) raw[i * INPUT_CHANNELS + 2] / 32768.0f;
        in_buf[i * 2 + 1] = (WDL_ResampleSample) raw[i * INPUT_CHANNELS + 3] / 32768.0f;
    }

    // 3. 48kHz -> 3kHz 重采样
    WDL_ResampleSample out_buf[SAMPLE_SIZE];  // 64 floats = 32帧 × 2ch
    int out_frames = resampler.ResampleOut(out_buf, nsamples, SAMPLE_SIZE / OUTPUT_CHANNELS, OUTPUT_CHANNELS);

    static int8_t haptic_buf[SAMPLE_SIZE];
    static int haptic_buf_pos = 0;

    // 4. 转换为int8并缓冲，满64字节即组包发送
    for (int i = 0; i < out_frames; i++) {
        int val_l = (int) (out_buf[i * 2] * 254.0f);
        int val_r = (int) (out_buf[i * 2 + 1] * 254.0f);
        haptic_buf[haptic_buf_pos++] = (int8_t) std::clamp(val_l, -128, 127);
        haptic_buf[haptic_buf_pos++] = (int8_t) std::clamp(val_r, -128, 127);

        if (haptic_buf_pos != SAMPLE_SIZE) {
            continue;
        }
        uint8_t pkt[REPORT_SIZE] = {};
        pkt[0] = REPORT_ID;
        pkt[1] = reportSeqCounter << 4;
        reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
        pkt[2] = 0x11 | (1 << 7);
        pkt[3] = 7;
        pkt[4] = 0b11111110;
        pkt[5] = 0;
        pkt[6] = 0;
        pkt[7] = 0;
        pkt[8] = 0;
        pkt[9] = 0xFF;
        pkt[10] = packetCounter++;
        pkt[11] = 0x12 | (1 << 7);
        pkt[12] = SAMPLE_SIZE;
        memcpy(pkt + 13, haptic_buf, SAMPLE_SIZE);

        bt_write(pkt, sizeof(pkt));
        haptic_buf_pos = 0;
    }
}

void core1_entry() {
    resampler.SetMode(true, 0, false);
    resampler.SetRates(48000, 3000);
    resampler.SetFeedMode(true);
    resampler.Prealloc(2, 480, 32);

    while (1) {
        if (!tud_ready()) {
            sleep_ms(10);
            continue;
        }
        process_audio();
    }
}
