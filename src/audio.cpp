//
// Created by awalol on 2026/3/5.
//

/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <atomic>
#include "port/port.h"
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
#include "config.h"
#include "elastic.h"

#define INPUT_CHANNELS    4
#define OUTPUT_CHANNELS   2
#define SAMPLE_SIZE       64
#define SPEAKER_OPUS_SIZE 200
#define REPORT_ID_SINGLE  0x36
#define REPORT_SIZE_SINGLE 398
#define REPORT_ID_DOUBLE  0x39
#define REPORT_SIZE_DOUBLE 547
#define REPORT_ID_HAPTICS_ONLY 0x32
#define REPORT_SIZE_HAPTICS_ONLY 142
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
// Written on the main task (set_mic_active), read on the core-1 audio worker.
// Genuinely concurrent on both targets -- two real cores -- so not a plain bool.
static std::atomic<bool> mic_active{false}; // host has opened the mic IN interface (alt != 0)
alignas(8) static uint32_t audio_core1_stack[7000];
port::Queue audio_fifo; // raw pcm data
port::Queue mic_fifo;
port::Queue mic_decode_fifo;
port::Queue audio_spk_fifo; // opus data
// Incremented on the audio worker, drained by the main task for reporting.
static volatile uint32_t audio_spk_overrun_count = 0;
port::Queue haptics_fifo;

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

#if defined(DS5_AUDIO_STATS)
// Per-stage counters for the speaker path, so a silent speaker can be pinned to
// a stage instead of guessed at. The chain is:
//   USB iso OUT -> audio_fifo -> speaker_proc (core 1, resample+opus)
//                             -> audio_spk_fifo -> audio_bt_task -> BT
// Printed once a second, and only while something is moving, so an idle console
// stays readable.
static uint32_t stat_usb_bytes = 0;    // bytes taken off the USB OUT endpoint
static uint32_t stat_fifo_drop = 0;    // dropped because audio_fifo was full
static uint32_t stat_opus_frames = 0;  // opus frames produced on core 1
static uint32_t stat_bt_pkts = 0;      // audio reports handed to bt_write()
static uint32_t stat_bt_with_audio = 0;// ...of those, ones carrying speaker data

static void audio_stats_tick() {
    static uint32_t last_ms = 0;
    static uint32_t last_total = 0;
    const uint32_t now = port::now_ms();
    if (now - last_ms < 1000) return;
    last_ms = now;
    const uint32_t total = stat_usb_bytes + stat_bt_pkts;
    if (total == last_total) return;
    last_total = total;
    printf("[AUDIO] usb=%luB drop=%lu opus=%lu bt=%lu (with audio %lu)\n",
           (unsigned long) stat_usb_bytes, (unsigned long) stat_fifo_drop,
           (unsigned long) stat_opus_frames, (unsigned long) stat_bt_pkts,
           (unsigned long) stat_bt_with_audio);
}
#endif

// Single-packet audio report (the v0.7.2 layout without its SetStateData
// block): one haptics frame followed by one Opus speaker frame.
// `haptics` holds SAMPLE_SIZE bytes, `speaker` SPEAKER_OPUS_SIZE bytes.
static void PORT_FAST_FUNC(audio_send_single)(const uint8_t *haptics, const uint8_t *speaker) {
    const Config_body &cfg = get_config();
    const bool mic_enabled = mic_active && cfg.mic_select != 3;

    uint8_t pkt[REPORT_SIZE_SINGLE]{};
    pkt[0] = REPORT_ID_SINGLE;
    pkt[1] = reportSeqCounter << 4;
    reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
    pkt[2] = 0x11 | 0 << 6 | 1 << 7;
    // pkt[3] 是 pkt[4] 中置 1 的位数。bit 6 是必须的且不占用后面的字节，
    // 其余每多置 1 位，就在下面多放一个字节。最终实测可以只保留一个
    // buf_len + packetCounter，这里沿用 v0.7.2 的 5 个 buf_len。
    pkt[3] = 7;
    pkt[4] = mic_enabled ? 0b11111111 : 0b11111110; // bit0: 手柄麦克风
    const auto buf_len = cfg.audio_buffer_length;
    pkt[5] = buf_len; // VolumeHeadphones - guess but no work
    pkt[6] = buf_len; // VolumeMic - guess but no work
    pkt[7] = buf_len; // VolumeSpeaker - guess but no work
    pkt[8] = buf_len; // AudioBufferLength
    pkt[9] = buf_len; // AudioBufferLength
    pkt[10] = packetCounter++;

    // Haptics Audio Data（单帧）：2 字节块头 + SAMPLE_SIZE 字节数据。
    // v0.7.2 里它前面还有一个 0x10 SetStateData 块（2+63 字节），现在 state
    // 走独立的 0x31 报文，所以整包前移到 pkt[11]（若以后要重新内嵌 state，
    // 需要在这里插回 0x90 块，并把后面的偏移整体 +65）。
    pkt[11] = 0x12 | 0 << 6 | 1 << 7;
    pkt[12] = SAMPLE_SIZE;
    memcpy(pkt + 13, haptics, SAMPLE_SIZE);

#if !DISABLE_SPEAKER_PROC
    if (cfg.speaker_select != 3) {
        pkt[77] = ((
            cfg.speaker_select == 2 || // lock headphone
            (cfg.speaker_select == 0 && plug_headset) // auto
            ) ? 0x16 : 0x13) | 0 << 6 | 1 << 7;
        pkt[78] = SPEAKER_OPUS_SIZE;
        memcpy(pkt + 79, speaker, SPEAKER_OPUS_SIZE);
    }
#endif
    bt_write(pkt, sizeof(pkt));
}

// Double-packet audio report: two haptics frames and two Opus speaker frames in
// one 0x39 report. `haptics` holds 2 * SAMPLE_SIZE bytes, `speaker`
// 2 * SPEAKER_OPUS_SIZE bytes (the two frames are contiguous).
static void PORT_FAST_FUNC(audio_send_double)(const uint8_t *haptics, const uint8_t *speaker) {
    const Config_body &cfg = get_config();
    const bool mic_enabled = mic_active && cfg.mic_select != 3;

    uint8_t pkt[REPORT_SIZE_DOUBLE]{};
    pkt[0] = REPORT_ID_DOUBLE;
    pkt[1] = reportSeqCounter << 4;
    reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
    pkt[2] = 0x11 | 0 << 6 | 1 << 7;
    // 见 audio_send_single 里对 pkt[3]/pkt[4] 的说明。双包模式只保留
    // 4 个 buf_len，因此 bit7 置 0、pkt[3] 减 1。
    pkt[3] = 6;
    pkt[4] = mic_enabled ? 0b01111111 : 0b01111110; // bit0: 手柄麦克风
    const auto buf_len = cfg.audio_buffer_length;
    pkt[5] = buf_len; // VolumeHeadphones - guess but no work
    pkt[6] = buf_len; // VolumeMic - guess but no work
    pkt[7] = buf_len; // VolumeSpeaker - guess but no work
    pkt[8] = buf_len; // AudioBufferLength
    pkt[9] = packetCounter += 2;

    // Haptics Audio Data（双帧）：块头 bit6 置 1 表示这一块带两帧数据，
    // 长度字节仍然是一帧的大小。
    pkt[10] = 0x12 | 1 << 6 | 1 << 7;
    pkt[11] = SAMPLE_SIZE;
    memcpy(pkt + 12, haptics, 2 * SAMPLE_SIZE);

#if !DISABLE_SPEAKER_PROC
    if (cfg.speaker_select != 3) {
        pkt[140] = ((
            cfg.speaker_select == 2 || // lock headphone
            (cfg.speaker_select == 0 && plug_headset) // auto
            ) ? 0x16 : 0x13) | 1 << 6 | 1 << 7;
        pkt[141] = SPEAKER_OPUS_SIZE;
        memcpy(pkt + 142, speaker, 2 * SPEAKER_OPUS_SIZE);
    }
#endif
    bt_write(pkt, sizeof(pkt));
}

static void PORT_FAST_FUNC(audio_send_haptics_only)(const uint8_t *haptics, const bool double_frame) {
    const Config_body &cfg = get_config();
    const bool mic_enabled = mic_active && cfg.mic_select != 3;
    const auto buf_len = cfg.audio_buffer_length;

    uint8_t pkt[REPORT_SIZE_HAPTICS_ONLY]{};
    pkt[0] = REPORT_ID_HAPTICS_ONLY;
    pkt[1] = reportSeqCounter << 4;
    reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
    pkt[2] = 0x11 | 0 << 6 | 1 << 7;
    pkt[3] = 4;
    // bit 5 - buf_len adjust
    // guess: bit 3 - mic vol?
    pkt[4] = mic_enabled ? 0b01100111 : 0b01100110; // bit0: 手柄麦克风
    pkt[5] = buf_len; // AudioBufferLength
    pkt[6] = buf_len; // AudioBufferLength
    pkt[7] = packetCounter++;
    if (double_frame) {
        packetCounter++;
    }

    pkt[8] = 0x12 | (double_frame ? 1 : 0) << 6 | 1 << 7;
    pkt[9] = SAMPLE_SIZE;
    memcpy(pkt + 10, haptics, SAMPLE_SIZE * (double_frame ? 2 : 1));
    bt_write(pkt, sizeof(pkt));
}

void PORT_FAST_FUNC(audio_bt_task)() {
    const Config_body &cfg = get_config();
    const bool speaker_enabled = cfg.speaker_select != 3;
    // packet_mode: 0 = 双包（0x39，一包两帧），1 = 单包（0x36，一包一帧）。
    // 扬声器禁用时同样遵守单双包模式，只是报文换成 0x32 纯 haptics 报文。
    const uint8_t frames = cfg.packet_mode == 1 ? 1 : 2;

    // 等够一整包的数据再取，避免把一帧拆到两个报文里
    if (haptics_fifo.level() < frames) {
        return;
    }
#if !DISABLE_SPEAKER_PROC
    if (speaker_enabled && audio_spk_fifo.level() < frames) {
        return;
    }
#endif

    // 一次取出整包的数据，帧之间连续存放，发送函数直接按指针读取
    static uint8_t haptics_pb[2 * SAMPLE_SIZE];
    for (uint8_t i = 0; i < frames; i++) {
        if (!haptics_fifo.try_remove(haptics_pb + i * SAMPLE_SIZE)) {
            printf("[Audio] Warning: Haptics queue remove failed\n");
            return;
        }
    }
#if !DISABLE_SPEAKER_PROC
    static uint8_t speaker_pb[2 * SPEAKER_OPUS_SIZE];
    if (speaker_enabled) {
        for (uint8_t i = 0; i < frames; i++) {
            if (!audio_spk_fifo.try_remove(speaker_pb + i * SPEAKER_OPUS_SIZE)) {
                printf("[Audio] Warning: Speaker queue remove failed\n");
                return;
            }
        }
    }
    const uint8_t *spk = speaker_pb;
#else
    const uint8_t *spk = nullptr;
#endif

    if (!speaker_enabled) {
        audio_send_haptics_only(haptics_pb, frames == 2);
    } else if (frames == 1) {
        audio_send_single(haptics_pb, spk);
    } else {
        audio_send_double(haptics_pb, spk);
    }
#if defined(DS5_AUDIO_STATS)
    ++stat_bt_pkts;
    if (speaker_enabled) ++stat_bt_with_audio;
#endif
}

void PORT_FAST_FUNC(audio_loop)() {
#if defined(DS5_AUDIO_STATS)
    audio_stats_tick();
#endif
    // Surface the worker's overrun counter from here, where printf is allowed.
    static uint32_t reported_spk_overruns = 0;
    const uint32_t spk_overruns = audio_spk_overrun_count;
    if (spk_overruns != reported_spk_overruns) {
        printf("[Audio] audio_spk_fifo overruns: %lu\n",
               (unsigned long) (spk_overruns - reported_spk_overruns));
        reported_spk_overruns = spk_overruns;
    }

    const Config_body &cfg = get_config();
    const bool mic_enabled = mic_active && cfg.mic_select != 3;
    const bool speaker_enabled = cfg.speaker_select != 3;

    // Mic: one 1 ms slice per millisecond of the dongle clock, which the
    // controller's mic follows. Refilling the IN FIFO whenever it has room
    // instead makes TinyUSB's flow control read the full FIFO as a fast device
    // clock and send 49 frames per USB frame, 2% more than the mic delivers,
    // so about 20 ms of silence would go in every second. A slice is still
    // sent while the elastic buffer refills (silence then), which keeps the
    // stream alive for hosts such as macOS CoreAudio.
    static bool mic_clock_running = false;
    static uint64_t mic_t0_us = 0;
    static uint64_t mic_slices = 0;
    if (mic_enabled) {
        static mic_decode_element decoded{};
        while (mic_decode_fifo.try_remove(&decoded)) {
            mic_elastic_write(decoded.data, decoded.len / sizeof(int16_t));
        }
        tu_fifo_t* tx_fifo = tud_audio_get_ep_in_ff();
        const uint64_t now = port::now_us();
        if (!mic_clock_running && tx_fifo) {
            // Start at TinyUSB's flow-control threshold so it sends nominal packets.
            int16_t silence[96] = {0};
            while (tu_fifo_count(tx_fifo) + 192 <= tud_audio_get_ep_in_fifo_threshold()) {
                tud_audio_write(silence, sizeof(silence));
            }
            mic_clock_running = true;
            mic_t0_us = now;
            mic_slices = 0;
        }
        while (tx_fifo && mic_slices < (now - mic_t0_us) / 1000) {
            if (tu_fifo_remaining(tx_fifo) < 192) {
                // The host stopped reading: start over once it reads again.
                mic_clock_running = false;
                break;
            }
            mic_slices++;
            int16_t mono[48];
            int16_t usb_tx_buf[96]; // 48 stereo frames (192 bytes)
            if (mic_elastic_read(mono, 48)) {
                for (int i = 0; i < 48; i++) {
                    usb_tx_buf[i * 2] = mono[i];     // Duplicate mono to Left
                    usb_tx_buf[i * 2 + 1] = mono[i]; // Duplicate mono to Right
                }
            } else {
                memset(usb_tx_buf, 0, sizeof(usb_tx_buf));
            }
            tud_audio_write(usb_tx_buf, sizeof(usb_tx_buf));
        }
    } else {
        mic_clock_running = false;
    }

    audio_bt_task();

    // 1. 读取 USB 音频数据
    if (!tud_audio_available()) return;

    int16_t raw[192];
    uint32_t bytes_read = tud_audio_read(raw, sizeof(raw)); // 每次读入 384 bytes
#if defined(DS5_AUDIO_STATS)
    stat_usb_bytes += bytes_read;
#endif
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
        while (audio_fifo.try_remove(NULL)) {
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
                if (audio_fifo.is_full()) {
                    audio_fifo.try_remove(NULL);
                }
                if (!audio_fifo.try_add(&element)) {
#if defined(DS5_AUDIO_STATS)
                    ++stat_fifo_drop;
#endif
                    printf("[Audio] Warning: audio_fifo add failed\n");
                }
                audio_buf_pos = 0;
            }
        }
#endif

        in_buf[i * 2] = raw[i * INPUT_CHANNELS + 2]  / 32768.0f;
        in_buf[i * 2 + 1] = raw[i * INPUT_CHANNELS + 3]  / 32768.0f;
    }

    // 3. 48kHz -> 3kHz 重采样
    static WDL_ResampleSample out_buf[SAMPLE_SIZE]; // 64 floats = 32帧 × 2ch
    const int out_frames = resampler.ResampleOut(out_buf, nframes, nframes / 4, OUTPUT_CHANNELS);

    static int8_t haptic_buf[SAMPLE_SIZE];
    static int haptic_buf_pos = 0;

    // 4. 转换为int8并缓冲，满64字节即组包发送
    for (int i = 0; i < out_frames; i++) {
        int val_l = static_cast<int>(out_buf[i * 2] * 127.0f * haptics_gain);
        int val_r = static_cast<int>(out_buf[i * 2 + 1] * 127.0f * haptics_gain);
        haptic_buf[haptic_buf_pos++] = static_cast<int8_t>(clamp(val_l, -128, 127));
        haptic_buf[haptic_buf_pos++] = static_cast<int8_t>(clamp(val_r, -128, 127));

        if (haptic_buf_pos != SAMPLE_SIZE) {
            continue;
        }
        static haptics_element element{};
        memcpy(element.data, haptic_buf,SAMPLE_SIZE);
        if (haptics_fifo.is_full()) {
            haptics_fifo.try_remove(NULL);
        }
        if (!haptics_fifo.try_add(&element)) {
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
    haptics_fifo.init(sizeof(haptics_element), 2);
    // Mic queues are read from audio_loop on core0 every iteration, so they
    // must exist regardless of the speaker-proc build flag.
    //
    // BUGFIX: ELASTIC BUFFER EXTENSION:
    // Increased microphone queues to depth 8 (~80ms buffer size).
    // This absorbs initial Opus encoder/decoder warm-up delays and mitigates
    // startup crackling/stuttering under Core 1 task schedulers.
    // mic_fifo.init(sizeof(mic_element), 2);
    // mic_decode_fifo.init(sizeof(mic_decode_element), 2);
    mic_fifo.init(sizeof(mic_element), 8);
    mic_decode_fifo.init(sizeof(mic_decode_element), 8);
#if !DISABLE_SPEAKER_PROC
    audio_fifo.init(sizeof(audio_raw_element), 2);
    audio_spk_fifo.init(sizeof(audio_spk_element), 2);
#if ENABLE_DEBUG
    // 通常 stack 最大使用 25836 bytes 即 stack[6459]
    debug_fill_core1_stack_watermark(audio_core1_stack,
                                     sizeof(audio_core1_stack) / sizeof(audio_core1_stack[0]));
#endif
    port::launch_worker(core1_entry, audio_core1_stack, sizeof(audio_core1_stack));
#endif
}

static OpusEncoder *encoder;
static OpusDecoder *decoder; // mic decoder
static WDL_Resampler resampler_audio;

// Speaker path: USB OUT PCM (core0 audio_fifo) -> resample -> opus encode ->
// opus_buf for the haptics/speaker BT report. Non-blocking so core1 can also
// service the mic path. Kept in RAM to remove XIP miss latency from the loop.
static void PORT_FAST_FUNC(speaker_proc)() {
    static audio_raw_element audio_element{};
    if (!audio_fifo.try_remove(&audio_element)) {
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
    // Deliberately NOT dropping the oldest here. Doing so would make this a
    // second consumer of audio_spk_fifo, racing audio_bt_task()'s paired
    // removes on the other core: its level() check would pass, then this drop
    // would steal a frame, the second remove would fail and the whole report
    // would be skipped -- a click. Overflow now drops the newest frame instead,
    // which only happens when the pipeline is already behind.
#if defined(DS5_AUDIO_STATS)
    ++stat_opus_frames;
#endif
    if (!audio_spk_fifo.try_add(&spk_ele)) {
        // Counted, not printed. This runs on the audio worker, whose whole
        // contract is that it never blocks -- and the console is a blocking
        // UART behind a mutex the main task holds constantly, so a line here
        // costs milliseconds of a 10 ms frame budget at precisely the moment
        // the FIFO is already overflowing. Printing would deepen the overrun
        // it is reporting. audio_loop() reports the delta from the main task.
        ++audio_spk_overrun_count;
    }
}

// Mic path: opus packets from the controller (core0 mic_fifo) -> opus decode ->
// PCM into mic_decode_fifo for audio_loop to push to the USB IN endpoint.
static void PORT_FAST_FUNC(mic_proc)() {
    static mic_element mic_packet{};
    if (!mic_fifo.try_remove(&mic_packet)) {
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
    if (mic_decode_fifo.is_full()) {
        mic_decode_fifo.try_remove(NULL);
    }
    mic_decode_fifo.try_add(&decode_element);
}

void PORT_FAST_FUNC(core1_entry)() {
    // Register core1 as a flash-safe victim so core0's flash_safe_execute() really
    // parks this core while flash is accessed, instead of letting it fault on XIP.
    // Used by config_save() (flash erase/program) and the BOOTSEL poll (which briefly
    // floats QSPI CSn) - the latter makes polling BOOTSEL safe while audio streams on
    // core1. Requires PICO_FLASH_ASSUME_CORE1_SAFE=0.
    port::worker_flash_safe_init();
    
    // Allow Core 0 to fully initialize Bluetooth and USB stacks before Core 1 starts processing
    // otherwise the dongle could shut down at initialization
    // TODO: Search for initialization callbacks of core 0
    port::delay_ms(300);

    int error = 0;
    encoder = opus_encoder_create(48000, 2,OPUS_APPLICATION_RESTRICTED_CELT, &error);
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
        bool work_done = false;
        
        // Only enter processing if data is actually waiting.
        // This avoids constantly acquiring queue locks (spinlocks) when idle,
        // which would otherwise thrash the RP2350 system bus and starve Core 0.
        if (audio_fifo.level() > 0) {
            speaker_proc();
            work_done = true;
        }
        if (mic_fifo.level() > 0) {
            mic_proc();
            work_done = true;
        }
        
        // If both queues are empty, we can safely sleep.
        // This prevents 100% CPU usage while maintaining sub-millisecond response times.
        if (!work_done) {
            port::delay_us(10); 
        }
    }
}

// data points at the opus mic payload, len is the bytes available there.
// In RAM (consistent with the BT-receive path) and validates len so a short
// or malformed report can't over-read past the packet buffer.
void PORT_FAST_FUNC(mic_add_queue)(uint8_t *data, uint16_t len) {
    if (!mic_active || get_config().mic_select == 3) return;
    if (len < MIC_OPUS_SIZE) return;
    static mic_element mic_packet{};
    memcpy(mic_packet.data, data, MIC_OPUS_SIZE);
    if (mic_fifo.is_full()) {
        mic_fifo.try_remove(NULL);
    }
    mic_fifo.try_add(&mic_packet);
}
