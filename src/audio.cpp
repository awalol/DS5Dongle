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
#include "resample.h"
#include "tusb.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "opus.h"
#include "utils.h"
#include "pico/multicore.h"
#include "pico/platform.h"
#include "pico/flash.h"
#include "pico/util/queue.h"
#include "config.h"
#include "state_mgr.h"
#include "usb.h"

#define INPUT_CHANNELS    4
#define OUTPUT_CHANNELS   2
#define SAMPLE_SIZE       64
#define REPORT_SIZE       398
#define REPORT_ID         0x36
// #define VOLUME_GAIN       2
// #define BUFFER_LENGTH     48
#define MIC_CHANNELS      1
#define MIC_FRAMES        480
#define MIC_OPUS_SIZE     71   // bytes per opus-encoded mic frame from the DualSense

using std::clamp;
using std::max;

static WDL_Resampler resampler;
static uint8_t reportSeqCounter = 0;
static uint8_t packetCounter = 0;
static bool plug_headset = false;
static bool mic_active = false; // host has opened the mic IN interface (alt != 0)
alignas(8) static uint32_t audio_core1_stack[8192];
queue_t audio_fifo;
queue_t mic_fifo;
queue_t mic_decode_fifo;
static uint8_t opus_buf[200];
critical_section_t opus_cs;

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
    pkt[3] = 7;
    pkt[4] = (mic_active && !get_config().disable_mic) ? 0b11111111 : 0b11111110;
    const auto buf_len = get_config().audio_buffer_length;
    pkt[5] = buf_len;
    pkt[6] = buf_len;
    pkt[7] = buf_len;
    pkt[8] = buf_len;
    pkt[9] = buf_len;
    pkt[10] = packetCounter++;
    bt_write(pkt,sizeof(pkt));
}

void __not_in_flash_func(audio_loop)() {
    // Mic playback: drain decoded mic PCM into the USB IN endpoint
    static mic_decode_element mic_pb{};
    if (queue_try_remove(&mic_decode_fifo, &mic_pb)) {
        // The controller mic is mono, but the USB descriptor presents a 2-channel
        // mic (matching the real DS5) so Windows doesn't conflict with its cached
        // DS5 audio format. Duplicate each mono sample into L and R.
        static int16_t mic_stereo[MIC_FRAMES * 2];
        const int mono_samples = mic_pb.len / 2;
        for (int i = 0; i < mono_samples; i++) {
            mic_stereo[2 * i] = mic_pb.data[i];
            mic_stereo[2 * i + 1] = mic_pb.data[i];
        }
        const uint16_t stereo_len = (uint16_t) (mono_samples * 2 * 2);
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

    // const float audio_gain = mute[0] ? 0.0f : powf(10.0f, get_config().speaker_volume / 20.0f);
    const float haptics_gain = get_config().haptics_gain;
    for (int i = 0; i < nframes; i++) {
 #if !DISABLE_SPEAKER_PROC       
        audio_buf[audio_buf_pos++] = raw[i * INPUT_CHANNELS] / 32768.0f;
        audio_buf[audio_buf_pos++] = raw[i * INPUT_CHANNELS + 1] / 32768.0f;
        if (audio_buf_pos == 512 * 2) {
            static audio_raw_element element{};
            memcpy(element.data, audio_buf, 512 * 2 * 4);
            if (queue_is_full(&audio_fifo)) {
                queue_try_remove(&audio_fifo,NULL);
            }
            if (!queue_try_add(&audio_fifo, &element)) {
                printf("[Audio] Warning: audio_fifo add failed\n");
            }
            audio_buf_pos = 0;
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
        uint8_t pkt[REPORT_SIZE]{};
        pkt[0] = REPORT_ID;
        pkt[1] = reportSeqCounter << 4;
        reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
        pkt[2] = 0x11 | 0 << 6 | 1 << 7;
        pkt[3] = 7;
        // bit0 enables controller mic streaming. Gate it on the host actually
        // opening the mic IN interface (set_mic_active from tud_audio_set_itf_cb)
        // AND on the user not disabling the mic (config.disable_mic), so the
        // DualSense only streams mic audio -- and core1 only decodes it -- while
        // an app is recording. Other bits (speaker/haptics) stay enabled.
        pkt[4] = (mic_active && !get_config().disable_mic) ? 0b11111111 : 0b11111110;
        const auto buf_len = get_config().audio_buffer_length;
        pkt[5] = buf_len;
        pkt[6] = buf_len;
        pkt[7] = buf_len;
        pkt[8] = buf_len; // 这 4 个字节的作用未知，调整没有效果
        pkt[9] = buf_len; // audio buffer length 只有调整这个字节生效。
        pkt[10] = packetCounter++;
        // SetStateData
        pkt[11] = 0x10 | 0 << 6 | 1 << 7;
        pkt[12] = 63;
        state_embed_for_audio(pkt + 13, 63);
        // Haptics Audio Data
        pkt[76] = 0x12 | 0 << 6 | 1 << 7;
        pkt[77] = SAMPLE_SIZE;
        memcpy(pkt + 78, haptic_buf, SAMPLE_SIZE);
#if !DISABLE_SPEAKER_PROC
        // Speaker Audio Data -- omitted entirely when the user disables the
        // speaker/headset (config.disable_speaker), so the controller's speaker
        // amp isn't driven (mirrors the Pico W no-speaker report).
        if (!get_config().disable_speaker) {
            pkt[142] = (plug_headset ? 0x16 : 0x13) | 0 << 6 | 1 << 7; // Speaker: 0x13
            // L Headset Mono: 0x14
            // L Headset R Speaker: 0x15
            // Headset: 0x16
            pkt[143] = 200;
            critical_section_enter_blocking(&opus_cs);
            memcpy(pkt + 144, opus_buf, 200);
            critical_section_exit(&opus_cs);
        }
#endif

        bt_write(pkt, sizeof(pkt));
        haptic_buf_pos = 0;
    }
}

void audio_init() {
    resampler.SetMode(true, 0, false);
    resampler.SetRates(48000, 3000);
    resampler.SetFeedMode(true);
    resampler.Prealloc(2, 24, 6);
    // Mic queues are read from audio_loop on core0 every iteration, so they
    // must exist regardless of the speaker-proc build flag.
    queue_init(&mic_fifo, sizeof(mic_element), 2);
    queue_init(&mic_decode_fifo, sizeof(mic_decode_element), 2);
#if !DISABLE_SPEAKER_PROC
    queue_init(&audio_fifo, sizeof(audio_raw_element), 2);
    critical_section_init(&opus_cs);
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
    // 将 512 frames 重采样成 480 frames 以解决噪音问题。感谢 @Junhoo
    WDL_ResampleSample *in_buf;
    int nframes = resampler_audio.ResamplePrepare(512, 2, &in_buf);
    for (int i = 0; i < nframes * 2; i++) {
        in_buf[i] = audio_element.data[i];
    }
    static WDL_ResampleSample out_buf[480 * 2];
    resampler_audio.ResampleOut(out_buf, nframes, 480, 2);

    static uint8_t out[200];
    (void) opus_encode_float(encoder, out_buf, 480, out, 200);
    critical_section_enter_blocking(&opus_cs);
    memcpy(opus_buf, out, 200);
    critical_section_exit(&opus_cs);
}

// Mic path: opus packets from the controller (core0 mic_fifo) -> opus decode ->
// PCM into mic_decode_fifo for audio_loop to push to the USB IN endpoint.
static void __not_in_flash_func(mic_proc)() {
    static mic_element mic_packet{};
    if (!queue_try_remove(&mic_fifo, &mic_packet)) {
        return;
    }
    static int16_t decoded_data[MIC_FRAMES * MIC_CHANNELS];
    auto decoded_samples = opus_decode(decoder, mic_packet.data, MIC_OPUS_SIZE, decoded_data, MIC_FRAMES, false);
    if (decoded_samples <= 0) {
        // Gated behind ENABLE_VERBOSE: printf pulls the newlib formatting chain
        // (flash) onto core1's path. Release builds compile it out so core1's
        // audio loop stays fully RAM-resident (no XIP fetches on this core).
#if ENABLE_VERBOSE
        printf("[Audio] OpusDecoder decode failed: %d\n", decoded_samples);
#endif
        return;
    }
    static mic_decode_element decode_element{};
    decode_element.len = decoded_samples * MIC_CHANNELS * sizeof(int16_t);
    memcpy(decode_element.data, decoded_data, decode_element.len);
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
    if (len < MIC_OPUS_SIZE) return;
    static mic_element mic_packet{};
    memcpy(mic_packet.data, data, MIC_OPUS_SIZE);
    if (queue_is_full(&mic_fifo)) {
        queue_try_remove(&mic_fifo, NULL);
    }
    queue_try_add(&mic_fifo, &mic_packet);
}
