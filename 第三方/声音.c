/* 声音.c —— 音频胶水层（dr_wav + dr_mp3）
 *
 * 跟 运行库.c 一样，这层不认识简言的任何结构体：只收裸指针，
 * 返回 malloc 出来的缓冲区，由调用方决定什么时候 free。
 *
 * 单位说明：
 *   帧   一帧 = 所有声道在同一时刻的采样，立体声一帧两个 float
 *   样本 样本是 float，名义范围 [-1, 1]
 *
 * 不做的事：
 *   · 不钳位。合成时几路叠加超过 1 是常事，削不削波由使用者决定，
 *     这里悄悄钳一下反而会让人以为音量已经调好了。
 *   · 不重采样。读进来什么采样率就是什么采样率，要变就自己插值。
 *   · 不做实时播放。沙箱里没有声卡，能验证的只有"读得对、写得对、
 *     数值算得对"，那就只做这些。
 */
#define DR_WAV_IMPLEMENTATION
#include "dr/dr_wav.h"

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_ONLY_MP3
#include "dr/dr_mp3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- 探测 -- */

/* 按文件头判断，不看扩展名：把 .mp3 改名成 .wav 也能读对。 */
static int 看着像_wav(const unsigned char *头)
{
    return 头[0] == 'R' && 头[1] == 'I' && 头[2] == 'F' && 头[3] == 'F';
}

static int 看着像_mp3(const unsigned char *头)
{
    /* ID3v2 标签，或直接是帧同步字 0xFFEx / 0xFFFx */
    if (头[0] == 'I' && 头[1] == 'D' && 头[2] == '3') return 1;
    if (头[0] == 0xFF && (头[1] & 0xE0) == 0xE0) return 1;
    return 0;
}

static float *读_wav(const char *路径, long long *帧数, long long *声道,
                     long long *采样率, char *错)
{
    drwav w;
    float *样本;
    drwav_uint64 总帧;
    drwav_uint64 读到;

    if (!drwav_init_file(&w, 路径, NULL)) {
        snprintf(错, 256, "WAV 打不开或格式不支持");
        return NULL;
    }
    总帧 = w.totalPCMFrameCount;
    样本 = (float *)malloc((size_t)(总帧 ? 总帧 : 1) * w.channels * sizeof(float));
    if (!样本) {
        drwav_uninit(&w);
        snprintf(错, 256, "内存不够：%llu 帧", (unsigned long long)总帧);
        return NULL;
    }
    读到 = drwav_read_pcm_frames_f32(&w, 总帧, 样本);
    *帧数 = (long long)读到;
    *声道 = (long long)w.channels;
    *采样率 = (long long)w.sampleRate;
    drwav_uninit(&w);
    return 样本;
}

static float *读_mp3(const char *路径, long long *帧数, long long *声道,
                     long long *采样率, char *错)
{
    drmp3_config 配;
    drmp3_uint64 总帧 = 0;
    float *样本;

    memset(&配, 0, sizeof 配);
    /* 一次读进内存：整首歌也就几十 MB，而且调用方随后要随机取样，
     * 流式读反而要自己管缓冲区。 */
    样本 = drmp3_open_file_and_read_pcm_frames_f32(路径, &配, &总帧, NULL);
    if (!样本) {
        snprintf(错, 256, "MP3 解不开（文件损坏或不是 MP3）");
        return NULL;
    }
    *帧数 = (long long)总帧;
    *声道 = (long long)配.channels;
    *采样率 = (long long)配.sampleRate;
    return 样本;
}

/* ---------------------------------------------------------------- 读 -- */

float *简_声_载(const char *路径, long long *帧数, long long *声道,
                long long *采样率, char *错)
{
    unsigned char 头[4];
    FILE *f;
    int n;

    错[0] = 0;
    *帧数 = 0; *声道 = 0; *采样率 = 0;

    if (!路径 || !*路径) { snprintf(错, 256, "路径是空的"); return NULL; }

    f = fopen(路径, "rb");
    if (!f) { snprintf(错, 256, "打不开：%s", 路径); return NULL; }
    n = (int)fread(头, 1, 4, f);
    fclose(f);
    if (n < 4) { snprintf(错, 256, "文件不足 4 字节"); return NULL; }

    if (看着像_wav(头)) return 读_wav(路径, 帧数, 声道, 采样率, 错);
    if (看着像_mp3(头)) return 读_mp3(路径, 帧数, 声道, 采样率, 错);

    /* 头不认识：可能是 WAV 的变体，试着当 wav 开一次，错误信息更具体 */
    return 读_wav(路径, 帧数, 声道, 采样率, 错);
}

/* ---------------------------------------------------------------- 写 -- */

int 简_声_存(const char *路径, const float *样本, long long 帧数,
             long long 声道, long long 采样率)
{
    drwav_data_format 式;
    drwav w;
    short 块[2048 * 2];
    long long 余 = 帧数;
    long long 处 = 0;

    if (!路径 || !样本 || 帧数 <= 0) return 0;
    if (声道 <= 0 || 声道 > 2) return 0;        /* 16 位交错写，先用 1~2 声道 */
    if (采样率 <= 0 || 采样率 > 384000) return 0;

    式.container = drwav_container_riff;
    式.format = DR_WAVE_FORMAT_PCM;
    式.channels = (drwav_uint32)声道;
    式.sampleRate = (drwav_uint32)采样率;
    式.bitsPerSample = 16;

    if (!drwav_init_file_write(&w, 路径, &式, NULL)) return 0;

    while (余 > 0) {
        long long 这次 = 余 < 2048 ? 余 : 2048;
        long long 个 = 这次 * 声道;
        for (long long i = 0; i < 个; i++) {
            double v = (double)样本[处 + i] * 32767.0;
            /* 这里必须钳：16 位整数的取值范围是死的，不钳就是溢出回绕，
             * 听感上是刺耳的爆音，比削波难听得多。 */
            if (v > 32767.0) v = 32767.0;
            if (v < -32768.0) v = -32768.0;
            块[i] = (short)(v < 0 ? v - 0.5 : v + 0.5);
        }
        if (drwav_write_pcm_frames(&w, (drwav_uint64)这次, 块) != (drwav_uint64)这次) {
            drwav_uninit(&w);
            return 0;
        }
        处 += 个;
        余 -= 这次;
    }
    drwav_uninit(&w);
    return 1;
}

/* -------------------------------------------------------------- 新建 -- */

float *简_声_新(long long 帧数, long long 声道)
{
    size_t 个;
    if (帧数 <= 0 || 声道 <= 0) return NULL;
    个 = (size_t)帧数 * (size_t)声道;
    /* calloc 而不是 malloc：新建出来的是静音，不是随机噪声 */
    return (float *)calloc(个, sizeof(float));
}

void 简_声_放(float *样本) { free(样本); }

/* -------------------------------------------------------------- 取设 -- */

float 简_声_取(const float *样本, long long 帧数, long long 声道数,
               long long 帧, long long 声道)
{
    if (!样本 || 帧 < 0 || 帧 >= 帧数 || 声道 < 0 || 声道 >= 声道数) return 0.0f;
    return 样本[帧 * 声道数 + 声道];
}

void 简_声_设(float *样本, long long 帧数, long long 声道数,
              long long 帧, long long 声道, double 值)
{
    if (!样本 || 帧 < 0 || 帧 >= 帧数 || 声道 < 0 || 声道 >= 声道数) return;
    样本[帧 * 声道数 + 声道] = (float)值;
}
