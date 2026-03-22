//
// Created by awalol on 2026/3/5.
//

#include "audio.h"
#include "bt.h"
#include "resample.h"
#include "tusb.h"
#include "usb.h"
#include <algorithm>
#include <vector>
#include <cstdio>

#define INPUT_CHANNELS    4
#define OUTPUT_CHANNELS   2

// 目前测试 最大128 最小 48， 这个值小的话就会多次调用 audio_loop，
// 增加 CPU 负担和 BT 传输次数，但过大可能导致单次处理时间过长。
// 可根据实际情况调整。最好是16的倍数以配合重采样率（48000/3000=16），避免出现过多小余量包。
#define READ_FRAMES      128

// SAMPLE_SIZE: 每个 BT 包携带的触觉音频字节数，应该是只能固定为64
#define SAMPLE_SIZE       64

// 真实发送的音频数据字节数，只要填充到这个数值就打包发送，剩余部分填0。可能能降低延迟，过小可能导致蓝牙发包过多
// 最好是 (READ_FRAMES/16) 的倍数以配合重采样率，避免出现过多小余量包。
#define REAL_SAMPLE_SIZE  40

#define REPORT_SIZE       142
#define REPORT_ID         0x32
// BUFFER_LENGTH: 告知 DS5 控制器的目标缓冲深度。
// 值越小控制器侧延迟越低，代价是在 BT 传输抖动时可能出现轻微断续。
// 原始值 55，已调低以减少控制器侧缓冲延迟。可在 5~20 之间调试。
#define BUFFER_LENGTH     10

static WDL_Resampler resampler;
static uint8_t reportSeqCounter = 0;
static uint8_t packetCounter = 0;

void audio_loop() {
    // 1. 读取 USB 音频数据
    if (!tud_audio_available()) return;

    static int16_t raw[READ_FRAMES * INPUT_CHANNELS];
    uint32_t bytes_read = tud_audio_read(raw, sizeof(raw));
    int frames = bytes_read / (INPUT_CHANNELS * sizeof(int16_t));
    if (frames == 0) {
        return;
    }

    // 2. 从4ch中提取ch3/ch4，转换为float输入重采样器
    WDL_ResampleSample *in_buf;
    int nframes = resampler.ResamplePrepare(frames, OUTPUT_CHANNELS, &in_buf);

    for (int i = 0; i < nframes; i++) {
        in_buf[i * 2] = (WDL_ResampleSample) raw[i * INPUT_CHANNELS + 2] / 32768.0f;
        in_buf[i * 2 + 1] = (WDL_ResampleSample) raw[i * INPUT_CHANNELS + 3] / 32768.0f;
    }

    // 3. 48kHz -> 3kHz 重采样
    // 按本次输入帧数估算输出上限（ceil(nframes * 3000 / 48000)），并留少量余量（+2）。
    // 使用静态 vector 复用容量，避免固定大栈数组和每次分配。
    static std::vector<WDL_ResampleSample> out_buf;
    int max_out_frames = (nframes + 15) / 16 + 2;
    max_out_frames = std::min(max_out_frames, READ_FRAMES); // 绝对上限为输入帧数（即不允许插值），以防异常情况导致过度分配
    out_buf.resize(max_out_frames * OUTPUT_CHANNELS);
    int out_frames = resampler.ResampleOut(out_buf.data(), nframes, max_out_frames, OUTPUT_CHANNELS);

    // 4. 直接构建含 BT_WRITE_PACKET_HEAD 前缀的完整 BT 包，通过 move 零拷贝入队，消除以下中间拷贝：
    //    ① haptic_buf 暂存  ② memcpy(pkt+13, haptic_buf)  ③ bt_write 内的 memcpy
    // 包布局（共 1+REPORT_SIZE=143 字节）：
    //   [0]=BT_WRITE_PACKET_HEAD  [1]=REPORT_ID  [2..13]=报文头  [14..13+SAMPLE_SIZE]=音频数据  其余=0
    static std::vector<uint8_t> pkt;
    static int audio_pos = 0; // 已写入音频区的字节数

    int i = 0;
    while (i < out_frames) {
        // 开始新包：分配缓冲并填写固定头部
        if (audio_pos == 0) {
            pkt.assign(1 + REPORT_SIZE, 0);
            pkt[0]  = BT_WRITE_PACKET_HEAD;
            pkt[1]  = REPORT_ID;
            pkt[2]  = reportSeqCounter << 4;
            reportSeqCounter = (reportSeqCounter + 1) & 0x0F;
            pkt[3]  = 0x11 | (1 << 7);
            pkt[4]  = 7;
            pkt[5]  = 0b11111110;
            pkt[6]  = BUFFER_LENGTH;
            pkt[7]  = BUFFER_LENGTH;
            pkt[8]  = BUFFER_LENGTH;
            pkt[9]  = BUFFER_LENGTH;
            pkt[10] = BUFFER_LENGTH;
            pkt[11] = packetCounter++;
            pkt[12] = 0x12 | (1 << 7);
            pkt[13] = SAMPLE_SIZE;
        }

        // 将重采样输出直接写入包的音频数据区（偏移14 = 0xA2(1) + 报文头(13)）
        while (i < out_frames && audio_pos < SAMPLE_SIZE) {
            int val_l = (int)(out_buf[i * 2]     * 127.0f * (volume[0] ?: 1));
            int val_r = (int)(out_buf[i * 2 + 1] * 127.0f * (volume[0] ?: 1));
            pkt[14 + audio_pos++] = (int8_t)std::clamp(val_l, -128, 127);
            pkt[14 + audio_pos++] = (int8_t)std::clamp(val_r, -128, 127);
            i++;
        }

        // 包满则通过 move 语义零拷贝送入 BT 发送队列
        if (audio_pos >= REAL_SAMPLE_SIZE) {
            bt_write(std::move(pkt));
            audio_pos = 0;
        }
    }
}

void audio_init() {
    resampler.SetMode(true, 0, false);
    resampler.SetRates(48000, 3000);
    resampler.SetFeedMode(true);
    resampler.Prealloc(2, 480, 32);
}
