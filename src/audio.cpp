//
// Created by awalol on 2026/3/5.
//

#include "audio.h"
#include "bt.h"
#include "resample.h"
#include "tusb.h"
#include "usb.h"
#include <algorithm>
#include <cstdio>

#include "opus.h"
#include "utils.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"

#define INPUT_CHANNELS    4
#define OUTPUT_CHANNELS   2
#define SAMPLE_SIZE       64
#define REPORT_SIZE       398
#define REPORT_ID         0x36
// #define VOLUME_GAIN       2
#define BUFFER_LENGTH     48

using std::clamp;
using std::max;

static WDL_Resampler resampler;
static uint8_t reportSeqCounter = 0;
static uint8_t packetCounter = 0;
static bool plug_headset = false;
alignas(8) static uint32_t audio_core1_stack[8192];
queue_t audio_fifo;
queue_t opus_fifo;
struct audio_raw_element {
    float data[512 * 2];
};
struct opus_element {
    uint8_t data[200];
};

void set_headset(bool state) {
    plug_headset = state;
}

// 有一个重构的想法，就是也把haptics也放进队列里面，使用定时器来发送数据
// 定时器伪代码:
// static uint8_t haptics_buf[64];
// static uint8_t speaker_buf[200];
// static auto last = time_us32();
// auto now = time_us32();
// if(now - last < 10666) return;
// func: send haptics and speaker;
// try_queue_remove - haptics and speaker
// 缺点是可能会导致haptics有延迟，不够实时？
void audio_loop() {
    // 1. 读取 USB 音频数据
    if (!tud_audio_available()) return;

    int16_t raw[192];
    uint32_t bytes_read = tud_audio_read(raw, sizeof(raw)); // 每次读入 384 bytes
    int frames = bytes_read / (INPUT_CHANNELS * sizeof(int16_t));
    if (frames == 0) {
        return;
    }

    static float audio_buf[512 * 2];
    static uint audio_buf_pos = 0;
    // 2. 从4ch中提取ch3/ch4，转换为float输入重采样器
    WDL_ResampleSample *in_buf;
    int nframes = resampler.ResamplePrepare(frames, OUTPUT_CHANNELS, &in_buf);

    for (int i = 0; i < nframes; i++) {
        audio_buf[audio_buf_pos++] = raw[i * INPUT_CHANNELS] / 32768.0f * (volume[0] - 1.0f);
        audio_buf[audio_buf_pos++] = raw[i * INPUT_CHANNELS + 1] / 32768.0f * (volume[0] - 1.0f);
        if (audio_buf_pos == 512 * 2) {
            static audio_raw_element element{};
            memcpy(element.data,audio_buf,512 * 2 * 4);
            if (queue_is_full(&audio_fifo)){
                queue_try_remove(&audio_fifo,NULL);
            }
            if (!queue_try_add(&audio_fifo,&element)) {
                printf("[Audio] Warning: audio_fifo add failed\n");
            }
            audio_buf_pos = 0;
        }

        in_buf[i * 2] = (WDL_ResampleSample) raw[i * INPUT_CHANNELS + 2] / 32768.0f;
        in_buf[i * 2 + 1] = (WDL_ResampleSample) raw[i * INPUT_CHANNELS + 3] / 32768.0f;
    }

    // 3. 48kHz -> 3kHz 重采样
    static WDL_ResampleSample out_buf[SAMPLE_SIZE]; // 64 floats = 32帧 × 2ch
    int out_frames = resampler.ResampleOut(out_buf, nframes, SAMPLE_SIZE / OUTPUT_CHANNELS, OUTPUT_CHANNELS);

    static int8_t haptic_buf[SAMPLE_SIZE];
    static int haptic_buf_pos = 0;

    // 4. 转换为int8并缓冲，满64字节即组包发送
    for (int i = 0; i < out_frames; i++) {
        int val_l = (int) (out_buf[i * 2] * 127.0f * max(volume[1],1.0f));
        int val_r = (int) (out_buf[i * 2 + 1] * 127.0f * max(volume[1],1.0f));
        haptic_buf[haptic_buf_pos++] = (int8_t) clamp(val_l, -128, 127); // 似乎clamp有点多余？还是以防万一吧
        haptic_buf[haptic_buf_pos++] = (int8_t) clamp(val_r, -128, 127);

        if (haptic_buf_pos != SAMPLE_SIZE) {
            continue;
        }
        uint8_t pkt[REPORT_SIZE]{};
        pkt[0] = REPORT_ID;
        pkt[1] = reportSeqCounter << 4;
        reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
        pkt[2] = 0x11 | (1 << 7);
        pkt[3] = 7;
        pkt[4] = 0b11111110;
        pkt[5] = BUFFER_LENGTH;
        pkt[6] = BUFFER_LENGTH;
        pkt[7] = BUFFER_LENGTH;
        pkt[8] = BUFFER_LENGTH;
        pkt[9] = BUFFER_LENGTH; // buffer length
        pkt[10] = packetCounter++;
        pkt[11] = 0x12 | (1 << 7);
        pkt[12] = SAMPLE_SIZE;
        memcpy(pkt + 13, haptic_buf, SAMPLE_SIZE);
        static opus_element opus_element{};
        if (queue_try_remove(&opus_fifo,&opus_element)) {
            pkt[77] = (plug_headset ? 0x16 : 0x13) | 0 << 6 | 1 << 7; // Speaker: 0x13
            // L Headset Mono: 0x14
            // L Headset R Speaker: 0x15
            // Headset: 0x16
            pkt[78] = 200;
            memcpy(pkt + 79,opus_element.data,200);
        }else {
            printf("[Audio] Warning: opus_fifo try remove failed\n");
        }

        bt_write(pkt, sizeof(pkt));
        haptic_buf_pos = 0;
    }
}

void audio_init() {
    resampler.SetMode(true, 0, false);
    resampler.SetRates(48000, 3000);
    resampler.SetFeedMode(true);
    // resampler.Prealloc(2, 480, 32);
    queue_init(&audio_fifo,sizeof(audio_raw_element),2);
    queue_init(&opus_fifo,sizeof(opus_element),2);
    multicore_launch_core1_with_stack(core1_entry,audio_core1_stack,sizeof(audio_core1_stack));
}

static OpusEncoder *encoder;
static WDL_Resampler resampler_audio;

void core1_entry() {
    int error = 0;
    encoder = opus_encoder_create(48000,2,OPUS_APPLICATION_AUDIO,&error);
    if (error != 0) {
        printf("[Audio] OpusEncoder create failed\n");
        return;
    }
    opus_encoder_ctl(encoder,OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
    opus_encoder_ctl(encoder,OPUS_SET_BITRATE(200 * 8 * 100));
    opus_encoder_ctl(encoder,OPUS_SET_VBR(false));
    opus_encoder_ctl(encoder,OPUS_SET_COMPLEXITY(0)); // max 4
    resampler_audio.SetMode(true,0,false);
    resampler_audio.SetRates(51200,48000);
    resampler_audio.SetFeedMode(true);

    while (true) {
        static audio_raw_element audio_element{};
        queue_remove_blocking(&audio_fifo,&audio_element);
        // 将 512 frames 重采样成 480 frames 以解决噪音问题。感谢 @Junhoo
        WDL_ResampleSample *in_buf;
        int nframes = resampler_audio.ResamplePrepare(512, 2, &in_buf);
        for (int i = 0; i < nframes * 2;i++) {
            in_buf[i] = audio_element.data[i];
        }
        static WDL_ResampleSample out_buf[480 * 2];
        resampler_audio.ResampleOut(out_buf,nframes,480,2);
        static opus_element opus_element{};
        (void)opus_encode_float(encoder,out_buf,480,opus_element.data,200);
        if (queue_is_full(&opus_fifo)) {
            queue_try_remove(&opus_fifo,NULL);
        }
        if (!queue_try_add(&opus_fifo,&opus_element)) {
            printf("[Audio] Warning: opus_fifo add failed\n");
        }
    }
}
