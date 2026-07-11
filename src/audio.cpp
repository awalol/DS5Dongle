//
// Created by awalol on 2026/3/5.
//

/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "audio.h"
#include "bt.h"
#if ENABLE_DEBUG
#include "debug.h"
#endif
#include "resample.h"
#include "tusb.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "opus.h"
#include "utils.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "pico/util/queue.h"
#include "config.h"

#define INPUT_CHANNELS    4
#define OUTPUT_CHANNELS   2
#define SAMPLE_SIZE       64
#define SPEAKER_OPUS_SIZE 200
#define REPORT_SIZE       547
#define REPORT_ID         0x39
// #define VOLUME_GAIN       2
// #define BUFFER_LENGTH     48
#define MIC_CHANNELS      1
#define MIC_FRAMES        480
#define MIC_OPUS_SIZE     71   // bytes per opus-encoded mic frame from the DualSense

using std::clamp;
using std::max;

static WDL_Resampler resampler;
extern uint8_t reportSeqCounter;
extern uint8_t packetCounter;
static bool plug_headset = false;
static bool mic_active = false; // host has opened the mic IN interface (alt != 0)
alignas(8) static uint32_t audio_core1_stack[7000];
queue_t audio_fifo; // raw pcm data
queue_t mic_fifo;
queue_t mic_decode_fifo;
queue_t audio_spk_fifo; // opus data
queue_t haptics_fifo;

struct audio_raw_element {
    float data[512 * 2];
};

struct mic_element {
    uint8_t data[MIC_OPUS_SIZE];
};

struct mic_decode_element {
    int16_t data[MIC_FRAMES * MIC_CHANNELS];
    uint16_t len;
};

struct audio_spk_element {
    uint8_t data[SPEAKER_OPUS_SIZE];
};

struct haptics_element {
    uint8_t data[64];
};

void set_headset(bool state) {
    plug_headset = state;
}

// Called from tud_audio_set_itf_cb when the host opens/closes the mic IN
// interface. Gates controller-mic streaming so it only runs while recording.
void set_mic_active(bool active) {
    mic_active = active;
    update_mic_status();
}

bool audio_mic_active() {
    return mic_active;
}

void update_mic_status() {
    uint8_t pkt[142]{};
    pkt[0] = 0x32;
    pkt[1] = reportSeqCounter << 4;
    reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
    pkt[2] = 0x11 | 0 << 6 | 1 << 7;
    pkt[3] = 1;
    pkt[4] = (mic_active && get_config().mic_select != 3) ? 0b00000011 : 0b00000010;
    bt_write(pkt, sizeof(pkt));
}

void __not_in_flash_func(audio_bt_task)() {
    const Config_body &cfg = get_config();
    const bool mic_enabled = mic_active && cfg.mic_select != 3;
#if !DISABLE_SPEAKER_PROC
    const bool speaker_enabled = cfg.speaker_select != 3;
#endif

    if (queue_get_level(&haptics_fifo) < 2) {
        return;
    }
#if !DISABLE_SPEAKER_PROC
    if (speaker_enabled && queue_get_level(&audio_spk_fifo) < 2) {
        return;
    }
#endif

    uint8_t pkt[REPORT_SIZE]{};
    pkt[0] = REPORT_ID;
    pkt[1] = reportSeqCounter << 4;
    reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
    pkt[2] = 0x11 | 0 << 6 | 1 << 7;
    pkt[3] = 6;
    pkt[4] = mic_enabled ? 0b01111111 : 0b01111110;
    // byte 4 研究
    // bit 6 是必须的
    // 其余 bit 每多设置一个为0，就需要将pkt[3] - 1，然后将下面这些缩短一个字节的数据。
    // 最终实测，可以只保留一个 buf_len + packetCounter
    // pkt 5-7 的注释是根据 Nielk1 采样到的数据进行猜测。但是实际上修改还是发现有任何效果
    const auto buf_len = cfg.audio_buffer_length;
    pkt[5] = buf_len; // VolumeHeadphones - guess but no work
    pkt[6] = buf_len; // VolumeMic - guess but no work
    pkt[7] = buf_len; // VolumeSpeaker - guess but no work
    pkt[8] = buf_len; // AudioBufferLength
    pkt[9] = packetCounter += 2;
    pkt[10] = 0x12 | 1 << 6 | 1 << 7;
    pkt[11] = SAMPLE_SIZE;
    static haptics_element haptics_pb{};
    if (queue_get_level(&haptics_fifo) >= 2) {
        if (queue_try_remove(&haptics_fifo, &haptics_pb)) {
            memcpy(pkt + 12, haptics_pb.data,SAMPLE_SIZE);
        } else {
            printf("[Audio] Warning: Haptics queue remove failed\n");
        }
        if (queue_try_remove(&haptics_fifo, &haptics_pb)) {
            memcpy(pkt + 12 + SAMPLE_SIZE, haptics_pb.data,SAMPLE_SIZE);
        } else {
            printf("[Audio] Warning: Haptics queue remove failed\n");
        }
    }
#if !DISABLE_SPEAKER_PROC
    if (speaker_enabled) {
        pkt[140] = ((
            cfg.speaker_select == 2 || // lock headphone
            (cfg.speaker_select == 0 && plug_headset) // auto
            ) ? 0x16 : 0x13) | 1 << 6 | 1 << 7;
        pkt[141] = SPEAKER_OPUS_SIZE;
        static audio_spk_element spk_pb{};
        if (queue_get_level(&audio_spk_fifo) >= 2) {
            if (queue_try_remove(&audio_spk_fifo, &spk_pb)) {
                memcpy(pkt + 142, spk_pb.data,SPEAKER_OPUS_SIZE);
            } else {
                printf("[Audio] Warning: Speaker queue remove failed\n");
            }
            if (queue_try_remove(&audio_spk_fifo, &spk_pb)) {
                memcpy(pkt + 142 + SPEAKER_OPUS_SIZE, spk_pb.data,SPEAKER_OPUS_SIZE);
            } else {
                printf("[Audio] Warning: Speaker queue remove failed\n");
            }
        }
    }
#endif
    bt_write(pkt, sizeof(pkt));
}

void __not_in_flash_func(audio_loop)() {
    const Config_body &cfg = get_config();
    const bool mic_enabled = mic_active && cfg.mic_select != 3;
    const bool speaker_enabled = cfg.speaker_select != 3;

    // Mic playback: drain decoded mic PCM into the USB IN endpoint
    static mic_decode_element mic_pb{};
    if (queue_try_remove(&mic_decode_fifo, &mic_pb)) {
        if (mic_enabled) {
            // The controller mic is mono, but the USB descriptor presents a 2-channel
            // mic (matching the real DS5) so Windows doesn't conflict with its cached
            // DS5 audio format. Pack each mono int16 sample as L/R in one 32-bit word.
            static uint32_t mic_stereo[MIC_FRAMES];
            const int mono_samples = mic_pb.len / 2;
            for (int i = 0; i < mono_samples; i++) {
                const uint16_t sample = static_cast<uint16_t>(mic_pb.data[i]);
                mic_stereo[i] = static_cast<uint32_t>(sample) | (static_cast<uint32_t>(sample) << 16);
            }
            const uint16_t stereo_len = (uint16_t) (mono_samples * sizeof(uint32_t));
            uint16_t written = tud_audio_write(mic_stereo, stereo_len);
            if (written != stereo_len) {
                // Gated behind ENABLE_VERBOSE: when the host has not opened the mic
                // interface (the common case -- most games never do) tud_audio_write
                // short-writes every frame, so an unconditional log would flood
                // core0's hot path with the newlib formatting chain.
#if ENABLE_VERBOSE
                printf("[Audio] Warning: USB mic FIFO wrote %u/%u bytes\n", written, stereo_len);
#endif
            }
        }
    }

    audio_bt_task();

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
    WDL_ResampleSample *in_buf;
    int nframes = resampler.ResamplePrepare(frames, OUTPUT_CHANNELS, &in_buf);

    // const float audio_gain = mute[0] ? 0.0f : powf(10.0f, get_config().speaker_volume / 20.0f);
    const float haptics_gain = cfg.haptics_gain;
#if !DISABLE_SPEAKER_PROC
    if (!speaker_enabled) {
        audio_buf_pos = 0;
        while (queue_try_remove(&audio_fifo, NULL)) {
        }
    }
#endif
    for (int i = 0; i < nframes; i++) {
#if !DISABLE_SPEAKER_PROC
        if (speaker_enabled) {
            audio_buf[audio_buf_pos++] = raw[i * INPUT_CHANNELS] / 32768.0f;
            audio_buf[audio_buf_pos++] = raw[i * INPUT_CHANNELS + 1] / 32768.0f;
            if (audio_buf_pos == 512 * 2) {
                static audio_raw_element element{};
                memcpy(element.data, audio_buf, 512 * 2 * 4);
                if (queue_is_full(&audio_fifo)) {
                    queue_try_remove(&audio_fifo, NULL);
                }
                if (!queue_try_add(&audio_fifo, &element)) {
                    printf("[Audio] Warning: audio_fifo add failed\n");
                }
                audio_buf_pos = 0;
            }
        }
#endif

        in_buf[i * 2] = static_cast<WDL_ResampleSample>(clamp(raw[i * INPUT_CHANNELS + 2] / 32768.0f * haptics_gain,
                                                              -1.0f, 1.0f));
        in_buf[i * 2 + 1] = static_cast<WDL_ResampleSample>(clamp(raw[i * INPUT_CHANNELS + 3] / 32768.0f * haptics_gain,
                                                                  -1.0f, 1.0f));
    }

    // 3. 48kHz -> 3kHz 重采样
    static WDL_ResampleSample out_buf[SAMPLE_SIZE]; // 64 floats = 32帧 × 2ch
    const int out_frames = resampler.ResampleOut(out_buf, nframes, nframes / 4, OUTPUT_CHANNELS);

    static int8_t haptic_buf[SAMPLE_SIZE];
    static int haptic_buf_pos = 0;

    // 4. 转换为int8并缓冲，满64字节即组包发送
    for (int i = 0; i < out_frames; i++) {
        int val_l = static_cast<int>(out_buf[i * 2] * 127.0f);
        int val_r = static_cast<int>(out_buf[i * 2 + 1] * 127.0f);
        haptic_buf[haptic_buf_pos++] = (int8_t) clamp(val_l, -128, 127); // 似乎clamp有点多余？还是以防万一吧
        haptic_buf[haptic_buf_pos++] = (int8_t) clamp(val_r, -128, 127);

        if (haptic_buf_pos != SAMPLE_SIZE) {
            continue;
        }
        static haptics_element element{};
        memcpy(element.data, haptic_buf,SAMPLE_SIZE);
        if (queue_is_full(&haptics_fifo)) {
            queue_try_remove(&haptics_fifo, NULL);
        }
        if (!queue_try_add(&haptics_fifo, &element)) {
            printf("[Audio] Warning: haptics_fifo add failed\n");
        }
        haptic_buf_pos = 0;
    }
}

void audio_init() {
    resampler.SetMode(true, 0, false);
    // 在有线连接的 DS5，其内部的 hd 震动也是工作在 3000Hz 的音频
    // 怎么发现的呢？打开频率发生器，发现在 0-3000 以及 3000-6000
    // 以及往后相同范围的区域，声音都是先升后降
    // 因此推断，DS5内部也是 3000Hz 的工作频率，并且没有低通滤波
    // 但是SetStateData有一个开关？还没进行测试
    // ------
    // resampler.SetMode(true, 2, false);
    // resampler.SetFilterParms(0.85f, 0.707f);
    // ------
    resampler.SetRates(48000, 3000);
    resampler.SetFeedMode(true);
    resampler.Prealloc(2, 48, 4);
    queue_init(&haptics_fifo, sizeof(haptics_element), 2);
    // Mic queues are read from audio_loop on core0 every iteration, so they
    // must exist regardless of the speaker-proc build flag.
    queue_init(&mic_fifo, sizeof(mic_element), 2);
    queue_init(&mic_decode_fifo, sizeof(mic_decode_element), 2);
#if !DISABLE_SPEAKER_PROC
    queue_init(&audio_fifo, sizeof(audio_raw_element), 2);
    queue_init(&audio_spk_fifo, sizeof(audio_spk_element), 2);
#if ENABLE_DEBUG
    // 通常 stack 最大使用 25836 bytes 即 stack[6459]
    debug_fill_core1_stack_watermark(audio_core1_stack,
                                     sizeof(audio_core1_stack) / sizeof(audio_core1_stack[0]));
#endif
    multicore_launch_core1_with_stack(core1_entry, audio_core1_stack, sizeof(audio_core1_stack));
#endif
}

static OpusEncoder *encoder;
static OpusDecoder *decoder; // mic decoder
static WDL_Resampler resampler_audio;

// Speaker path: USB OUT PCM (core0 audio_fifo) -> resample -> opus encode ->
// opus_buf for the haptics/speaker BT report. Non-blocking so core1 can also
// service the mic path. Kept in RAM to remove XIP miss latency from the loop.
static void __not_in_flash_func(speaker_proc)() {
    static audio_raw_element audio_element{};
    if (!queue_try_remove(&audio_fifo, &audio_element)) {
        return;
    }
    if (get_config().speaker_select == 3) {
        return;
    }
    // 将 512 frames 重采样成 480 frames 以解决噪音问题。感谢 @Junhoo
    WDL_ResampleSample *in_buf;
    int nframes = resampler_audio.ResamplePrepare(512, 2, &in_buf);
    for (int i = 0; i < nframes * 2; i++) {
        in_buf[i] = audio_element.data[i];
    }
    static WDL_ResampleSample out_buf[480 * 2];
    resampler_audio.ResampleOut(out_buf, nframes, 480, 2);

    static uint8_t out[SPEAKER_OPUS_SIZE];
    const int encoded_len = opus_encode_float(encoder, out_buf, 480, out, sizeof(out));
    if (encoded_len <= 0) {
#if ENABLE_VERBOSE
        printf("[Audio] OpusEncoder encode failed: %d\n", encoded_len);
#endif
        return;
    }

    static audio_spk_element spk_ele{};
    memcpy(spk_ele.data, out, encoded_len);
    if (encoded_len < (int) sizeof(spk_ele.data)) {
        memset(spk_ele.data + encoded_len, 0, sizeof(spk_ele.data) - encoded_len);
    }
    if (queue_is_full(&audio_spk_fifo)) {
        queue_try_remove(&audio_spk_fifo, NULL);
    }
    if (!queue_try_add(&audio_spk_fifo, &spk_ele)) {
        printf("[Audio] Warning: audio_spk_fifo add failed\n");
    }
}

// Mic path: opus packets from the controller (core0 mic_fifo) -> opus decode ->
// PCM into mic_decode_fifo for audio_loop to push to the USB IN endpoint.
static void __not_in_flash_func(mic_proc)() {
    static mic_element mic_packet{};
    if (!queue_try_remove(&mic_fifo, &mic_packet)) {
        return;
    }
    if (!mic_active || get_config().mic_select == 3) {
        return;
    }
    static mic_decode_element decode_element{};
    auto decoded_samples = opus_decode(decoder, mic_packet.data, MIC_OPUS_SIZE, decode_element.data, MIC_FRAMES, false);
    if (decoded_samples <= 0) {
        // Gated behind ENABLE_VERBOSE: printf pulls the newlib formatting chain
        // (flash) onto core1's path. Release builds compile it out so core1's
        // audio loop stays fully RAM-resident (no XIP fetches on this core).
#if ENABLE_VERBOSE
        printf("[Audio] OpusDecoder decode failed: %d\n", decoded_samples);
#endif
        return;
    }
    decode_element.len = decoded_samples * MIC_CHANNELS * sizeof(int16_t);
    if (queue_is_full(&mic_decode_fifo)) {
        queue_try_remove(&mic_decode_fifo, NULL);
    }
    queue_try_add(&mic_decode_fifo, &decode_element);
}

void __not_in_flash_func(core1_entry)() {
    // Register core1 as a flash-safe victim so core0's flash_safe_execute() really
    // parks this core while flash is accessed, instead of letting it fault on XIP.
    // Used by config_save() (flash erase/program) and the BOOTSEL poll (which briefly
    // floats QSPI CSn) - the latter makes polling BOOTSEL safe while audio streams on
    // core1. Requires PICO_FLASH_ASSUME_CORE1_SAFE=0.
    flash_safe_execute_core_init();
    int error = 0;
    encoder = opus_encoder_create(48000, 2,OPUS_APPLICATION_AUDIO, &error);
    if (error != 0) {
        printf("[Audio] OpusEncoder create failed\n");
        return;
    }
    opus_encoder_ctl(encoder,OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
    opus_encoder_ctl(encoder,OPUS_SET_BITRATE(200 * 8 * 100));
    opus_encoder_ctl(encoder,OPUS_SET_VBR(false));
    opus_encoder_ctl(encoder,OPUS_SET_COMPLEXITY(0)); // max 4
    resampler_audio.SetMode(true, 0, false);
    resampler_audio.SetRates(51200, 48000);
    resampler_audio.SetFeedMode(true);
    resampler_audio.Prealloc(2, 512, 480);
    decoder = opus_decoder_create(48000, MIC_CHANNELS, &error);
    if (error != 0) {
        printf("[Audio] OpusDecoder create failed\n");
    }

    while (true) {
        speaker_proc();
        mic_proc();
    }
}

// data points at the opus mic payload, len is the bytes available there.
// In RAM (consistent with the BT-receive path) and validates len so a short
// or malformed report can't over-read past the packet buffer.
void __not_in_flash_func(mic_add_queue)(uint8_t *data, uint16_t len) {
    if (!mic_active || get_config().mic_select == 3) return;
    if (len < MIC_OPUS_SIZE) return;
    static mic_element mic_packet{};
    memcpy(mic_packet.data, data, MIC_OPUS_SIZE);
    if (queue_is_full(&mic_fifo)) {
        queue_try_remove(&mic_fifo, NULL);
    }
    queue_try_add(&mic_fifo, &mic_packet);
}
