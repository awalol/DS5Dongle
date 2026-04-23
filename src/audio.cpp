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

#include "CircularBuffer.h"
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
static CircularBuffer audioBuffer(200 * 10);
static CircularBuffer hapticsBuffer(64 * 10);
static bool audioCache = true;
static bool hapticsCache = true;

void haptics_proc() {

    if (!tud_audio_available()) return;

    // 每次读入384bytes
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

    for (int i = 0; i < out_frames; i++) {
        int val_l = (int) (out_buf[i * 2] * 127.0f * volume[0]);
        int val_r = (int) (out_buf[i * 2 + 1] * 127.0f * volume[0]);
        uint8_t c_l = (uint8_t) std::clamp(val_l, -128, 127);
        uint8_t c_r = (uint8_t) std::clamp(val_r, -128, 127);
        hapticsBuffer.Write(&c_l,0,1);
        hapticsBuffer.Write(&c_r,0,1);
    }

    return;
}

void send_haptics(const uint8_t* data) {
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
    pkt[11] = 0x13 | 0 << 6 | 1 << 7; // Speaker: 0x13 Headset: 0x16
    pkt[12] = 200;
    memcpy(pkt + 13, data, 200);

    bt_write(pkt, sizeof(pkt));
}

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

void audio_loop() {
    static auto lastTime = time_us_32();
    auto now = time_us_32();
    if (now - lastTime < 10000) {
        return;
    }
    lastTime = now;

    static uint8_t k = 0;
    if (++k == 16) {
        k = 0;
        return;
    }

    // 记录是否有震动和音频的数据
    bool haptics = false;
    bool audio = false;
    uint8_t hapticsData[64] = {};
    if (hapticsCache) {
        if (hapticsBuffer.Count() >= 64 * 5) {
            hapticsCache = false;
        }
    }else {
        if (hapticsBuffer.Count() >= 64) {
            hapticsCache = false;
            hapticsBuffer.Read(hapticsData,0,64);
            haptics = true;
        }else {
            printf("enter haptics cache state\n");
            hapticsCache = true;
        }
    }
    uint8_t audioData[200] = {};
    if (audioCache) {
        if (audioBuffer.Count() >= 200 * 5) {
            audioCache = false;
        }
    }else {
        if (audioBuffer.Count() >= 200) {
            audioCache = false;
            audioBuffer.Read(audioData,0,200);
            audio = true;
        }else {
            printf("enter audio cache state\n");
            audioCache = true;
        }
    }
    if (haptics && audio) {
        send_combine(audioData,hapticsData);
    }else if (haptics) {
        send_haptics(hapticsData);
    }else if (audio) {
        send_speaker(audioData);
    }
}

// 当前扬声器的逻辑是：扬声器要480frames，震动是512frames (32 * 16)，所以扬声器会先完成编码。
// 将编码完成的数据进行存储，在震动的时候一起发送出去。
// 正好也在之前HeadsetPlayMusic的Demo里面，10.666ms的周期发送才播放正常，而这个值也是震动的发送周期时间，具体计算请看SAXense
// 神奇参数: 16 * 10 / 15 = 10.666
// ++k % 16 == 0 jump

void audio_init() {
    resampler.SetMode(true, 0, false);
    // resampler.SetMode(true,1,false); // filtercnt 最大为1, 设置为2会卡顿
    // resampler.SetFilterParms();
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
        static uint8_t save[256];
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
        if (speaker_buf_pos + count > 200) {
            wait = true;
            return;
        }
        memcpy(save + speaker_buf_pos,buf,count);
        speaker_buf_pos += count;
        if (count == 8) {
            audioBuffer.Write(save,0,200);
            speaker_buf_pos = 0;
        }
    }
}
