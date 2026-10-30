/* glass-wav.c: 3.4-kernel-native ALSA PCM WAV player & recorder for Google Glass
 * (TWL6040 / OMAP4 ABE, card 0 "Notle").
 *
 * Why this exists:
 *   Modern 32-bit ARM userlands (Alpine 3.20+, Debian Trixie+) build alsa-lib
 *   with 64-bit time_t, which issues SNDRV_PCM_IOCTL_SYNC_PTR (0xc0884123).
 *   Google's stock XE24 3.4.94 kernel only implements the 32-bit time_t
 *   SNDRV_PCM_IOCTL_SYNC_PTR, causing aplay/arecord to fail or stutter.
 *   Additionally, `musegadget voice-call` requires exact PCM16LE mono 24000 Hz
 *   WAV files (and does not resample), whereas the OMAP4 ABE / TWL6040 hardware
 *   runs at 48000 Hz.
 *
 * Commands:
 *   glass-wav play <in.wav> [-d /dev/snd/pcmC0D0p]
 *   glass-wav record <out.wav> [seconds] [-d /dev/snd/pcmC0D0c]
 *   glass-wav synth <out.wav> [freq_hz] [seconds]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"

static volatile sig_atomic_t stop_flag = 0;
static void on_sig(int sig) {
    (void)sig;
    stop_flag = 1;
}

/* Linux 3.4 32-bit ARM ALSA PCM ioctl structures (protocol 2.0.10) */
typedef unsigned long k_uframes;
typedef long k_sframes;
struct k_mask { uint32_t bits[8]; };
struct k_interval {
    unsigned min, max;
    unsigned openmin:1, openmax:1, integer:1, empty:1;
};
struct k_hw_params {
    unsigned flags;
    struct k_mask masks[3];
    struct k_mask mres[5];
    struct k_interval intervals[12];
    struct k_interval ires[9];
    unsigned rmask, cmask, info, msbits, rate_num, rate_den;
    k_uframes fifo_size;
    unsigned char reserved[64];
};
struct k_sw_params {
    int tstamp_mode;
    unsigned period_step, sleep_min;
    k_uframes avail_min, xfer_align, start_threshold, stop_threshold;
    k_uframes silence_threshold, silence_size, boundary;
    unsigned char reserved[64];
};
struct k_xferi { k_sframes result; void *buf; k_uframes frames; };

#define K_HW_REFINE _IOWR('A', 0x10, struct k_hw_params)
#define K_HW_PARAMS _IOWR('A', 0x11, struct k_hw_params)
#define K_SW_PARAMS _IOWR('A', 0x13, struct k_sw_params)
#define K_PREPARE   _IO('A', 0x40)
#define K_START     _IO('A', 0x42)
#define K_DROP      _IO('A', 0x43)
#define K_DRAIN     _IO('A', 0x44)
#define K_WRITEI    _IOW('A', 0x50, struct k_xferi)
#define K_READI     _IOR('A', 0x51, struct k_xferi)

enum {
    P_ACCESS = 0, P_FORMAT = 1, P_SUBFORMAT = 2,
    P_FIRST_INTERVAL = 8,
    P_SAMPLE_BITS = 8, P_FRAME_BITS = 9,
    P_CHANNELS = 10, P_RATE = 11,
    P_PERIOD_SIZE = 13, P_PERIODS = 15, P_BUFFER_SIZE = 17
};
#define ACCESS_RW_INTERLEAVED 3
#define FORMAT_S16_LE 2
#define SUBFORMAT_STD 0

static void hw_any(struct k_hw_params *p) {
    memset(p, 0, sizeof *p);
    for (int i = 0; i < 3; i++) memset(p->masks[i].bits, 0xff, sizeof p->masks[i].bits);
    for (int i = 0; i < 12; i++) { p->intervals[i].min = 0; p->intervals[i].max = UINT_MAX; }
    p->rmask = ~0u;
    p->info = ~0u;
}
static void hw_mask(struct k_hw_params *p, int param, unsigned v) {
    memset(p->masks[param].bits, 0, sizeof p->masks[param].bits);
    p->masks[param].bits[v / 32] = 1u << (v % 32);
}
static void hw_int(struct k_hw_params *p, int param, unsigned val) {
    struct k_interval *i = &p->intervals[param - P_FIRST_INTERVAL];
    i->min = val; i->max = val; i->openmin = i->openmax = 0; i->integer = 1; i->empty = 0;
}
static void hw_min(struct k_hw_params *p, int param, unsigned min) {
    struct k_interval *i = &p->intervals[param - P_FIRST_INTERVAL];
    i->min = min;
}
static unsigned hw_val(const struct k_hw_params *p, int param) {
    return p->intervals[param - P_FIRST_INTERVAL].min;
}

static int pcm_open_34(const char *dev_path, int is_capture,
                       unsigned *out_rate, unsigned *out_ch) {
    int fd = open(dev_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "glass-wav: open %s: %s\n", dev_path, strerror(errno));
        return -1;
    }
    static const unsigned rates[] = {48000, 24000, 44100, 16000};
    static const unsigned chans[] = {2, 1};
    struct k_hw_params p;
    int ok = 0;
    for (unsigned ri = 0; ri < 4 && !ok; ri++) {
        for (unsigned ci = 0; ci < 2 && !ok; ci++) {
            unsigned r = rates[ri], c = chans[ci];
            hw_any(&p);
            hw_mask(&p, P_ACCESS, ACCESS_RW_INTERLEAVED);
            hw_mask(&p, P_FORMAT, FORMAT_S16_LE);
            hw_mask(&p, P_SUBFORMAT, SUBFORMAT_STD);
            hw_min(&p, P_PERIOD_SIZE, 1056);
            hw_int(&p, P_SAMPLE_BITS, 16);
            hw_int(&p, P_FRAME_BITS, c * 16);
            hw_int(&p, P_CHANNELS, c);
            hw_int(&p, P_PERIODS, 4);
            hw_int(&p, P_RATE, r);
            ioctl(fd, K_HW_REFINE, &p);
            if (ioctl(fd, K_HW_PARAMS, &p) == 0)
                ok = 1;
        }
    }
    if (!ok) {
        fprintf(stderr, "glass-wav: K_HW_PARAMS failed on %s: %s\n", dev_path, strerror(errno));
        close(fd);
        return -1;
    }
    *out_rate = hw_val(&p, P_RATE);
    *out_ch = hw_val(&p, P_CHANNELS);
    k_uframes period = hw_val(&p, P_PERIOD_SIZE);
    k_uframes buffer = hw_val(&p, P_BUFFER_SIZE);

    struct k_sw_params s;
    memset(&s, 0, sizeof s);
    s.period_step = 1;
    s.avail_min = period;
    s.xfer_align = 1;
    s.start_threshold = is_capture ? 1 : period;
    s.stop_threshold = buffer * 2;
    s.boundary = buffer * 256;
    ioctl(fd, K_SW_PARAMS, &s);
    if (ioctl(fd, K_PREPARE) < 0) {
        fprintf(stderr, "glass-wav: K_PREPARE failed on %s: %s\n", dev_path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int read_volume_pct(void) {
    static const char *const paths[] = {
        "/run/glass/volume",
        "/var/lib/musegadget/volume",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        FILE *f = fopen(paths[i], "r");
        if (f) {
            char b[16] = "";
            if (fgets(b, sizeof b, f)) {
                fclose(f);
                int v = atoi(b);
                if (v >= 5 && v <= 150) return v;
            } else {
                fclose(f);
            }
        }
    }
    return 100;
}

static inline int16_t scale_vol(int32_t s, int vol_pct) {
    int32_t v = (s * vol_pct) / 100;
    if (v > 32767) v = 32767;
    else if (v < -32768) v = -32768;
    return (int16_t)v;
}

static void write_rms_file(int rms_val) {
    char buf[24];
    int n = snprintf(buf, sizeof buf, "%d\n", rms_val);
    int fd = open("/run/glass/nova-rms", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        ssize_t r = write(fd, buf, (size_t)n);
        (void)r;
        close(fd);
    }
}

static int compute_pcm16_rms(const int16_t *samples, size_t count, unsigned stride) {
    if (count == 0) return 0;
    uint64_t sum_sq = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t v = samples[i * stride];
        sum_sq += (uint64_t)(v * v);
    }
    return (int)sqrt((double)sum_sq / (double)count);
}

static int cmd_chime(const char *kind, const char *dev_path) {
    unsigned hw_rate = 48000, hw_ch = 2;
    int fd = pcm_open_34(dev_path, 0, &hw_rate, &hw_ch);
    if (fd < 0) return 1;

    double f1 = 660.0, f2 = 990.0, dur = 0.14;
    if (strcmp(kind, "think") == 0) {
        f1 = 880.0; f2 = 587.33; dur = 0.12;
    } else if (strcmp(kind, "done") == 0) {
        f1 = 523.25; f2 = 783.99; dur = 0.14;
    }
    unsigned total = (unsigned)(hw_rate * dur);
    int16_t buf[1024 * 2];
    unsigned pos = 0;
    int vol_pct = read_volume_pct();
    while (pos < total && !stop_flag) {
        unsigned chunk = total - pos;
        if (chunk > 1024) chunk = 1024;
        for (unsigned i = 0; i < chunk; i++) {
            unsigned idx = pos + i;
            double freq = (idx < total / 2) ? f1 : f2;
            double env = 1.0;
            unsigned half = total / 2;
            unsigned local = idx % half;
            if (local < 240) env = (double)local / 240.0;
            else if (half - local < 480) env = (double)(half - local) / 480.0;
            int16_t s = scale_vol((int32_t)(sin(2.0 * 3.14159265358979323846 * freq * idx / hw_rate) * 6500.0 * env), vol_pct);
            buf[2 * i] = s;
            buf[2 * i + 1] = s;
        }
        int16_t *p = buf;
        k_uframes rem = chunk;
        while (rem > 0 && !stop_flag) {
            struct k_xferi x = {0, p, rem};
            if (ioctl(fd, K_WRITEI, &x) < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                if (errno == EPIPE) { ioctl(fd, K_PREPARE); continue; }
                rem = 0;
                break;
            }
            rem -= (k_uframes)x.result;
            p += (size_t)x.result * hw_ch;
        }
        pos += chunk;
    }
    ioctl(fd, K_DRAIN);
    close(fd);
    return 0;
}

static void write_wav_header(FILE *f, uint32_t sample_rate, uint16_t channels, uint32_t num_samples) {
    uint32_t data_bytes = num_samples * channels * 2;
    uint32_t riff_size = 36 + data_bytes;
    uint32_t byte_rate = sample_rate * channels * 2;
    uint16_t block_align = channels * 2;
    uint16_t bits_per_sample = 16;
    uint16_t audio_fmt = 1; /* PCM */
    uint32_t fmt_size = 16;

    fwrite("RIFF", 1, 4, f);
    fwrite(&riff_size, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_size, 4, 1, f);
    fwrite(&audio_fmt, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&sample_rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bits_per_sample, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);
}

static int cmd_synth(const char *out_path, double freq_hz, double duration_s) {
    uint32_t rate = 24000;
    uint32_t total = (uint32_t)(rate * duration_s);
    FILE *f = fopen(out_path, "wb");
    if (!f) { perror(out_path); return 1; }
    write_wav_header(f, rate, 1, total);
    for (uint32_t i = 0; i < total; i++) {
        double env = 1.0;
        if (i < 480) env = (double)i / 480.0;
        else if (total - i < 480) env = (double)(total - i) / 480.0;
        int16_t s = (int16_t)(sin(2.0 * 3.14159265358979323846 * freq_hz * i / rate) * 12000.0 * env);
        fwrite(&s, sizeof s, 1, f);
    }
    fclose(f);
    printf("glass-wav: synthesized %u samples (%.2fs @ %u Hz mono PCM16LE) -> %s\n",
           total, duration_s, rate, out_path);
    return 0;
}

static int cmd_play_mp3(const char *in_path, const char *dev_path) {
    FILE *f = fopen(in_path, "rb");
    if (!f) { perror(in_path); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 16 * 1024 * 1024) {
        fclose(f);
        return 1;
    }
    uint8_t *mp3 = (uint8_t *)malloc((size_t)sz);
    if (!mp3) { fclose(f); return 1; }
    if (fread(mp3, 1, (size_t)sz, f) != (size_t)sz) {
        free(mp3);
        fclose(f);
        return 1;
    }
    fclose(f);

    unsigned hw_rate = 48000, hw_ch = 2;
    int fd = pcm_open_34(dev_path, 0, &hw_rate, &hw_ch);
    if (fd < 0) { free(mp3); return 1; }

    mp3dec_t dec;
    mp3dec_init(&dec);
    mp3dec_frame_info_t info;
    int16_t pcm_frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int16_t out_buf[8192 * 2];
    size_t off = 0;
    uint32_t frac = 0;
    int32_t prev_s = 0;

    printf("glass-wav: playing MP3 %s (%ld bytes) -> %s (%u Hz %u ch)\n",
           in_path, sz, dev_path, hw_rate, hw_ch);

    while (off < (size_t)sz && !stop_flag) {
        int samples = mp3dec_decode_frame(&dec, mp3 + off, (int)((size_t)sz - off), pcm_frame, &info);
        if (info.frame_bytes <= 0) break;
        off += (size_t)info.frame_bytes;
        if (samples <= 0 || info.hz <= 0 || info.channels <= 0) continue;

        int vol_pct = read_volume_pct();
        uint32_t step = (uint32_t)(((uint64_t)info.hz << 16) / hw_rate);
        unsigned out_frames = 0;
        for (int i = 0; i < samples; i++) {
            int32_t cur_s = (info.channels == 2)
                ? ((int32_t)pcm_frame[2 * i] + (int32_t)pcm_frame[2 * i + 1]) / 2
                : pcm_frame[i];
            cur_s = scale_vol(cur_s, vol_pct);
            while (frac < 65536 && out_frames < 8192) {
                int16_t v = (int16_t)(prev_s + (((cur_s - prev_s) * (int32_t)frac) >> 16));
                if (hw_ch == 2) {
                    out_buf[2 * out_frames] = v;
                    out_buf[2 * out_frames + 1] = v;
                } else {
                    out_buf[out_frames] = v;
                }
                out_frames++;
                frac += step;
            }
            frac -= 65536;
            prev_s = cur_s;
        }

        int16_t *p = out_buf;
        k_uframes rem = out_frames;
        int rms = compute_pcm16_rms(out_buf, out_frames, hw_ch);
        write_rms_file(rms);
        while (rem > 0 && !stop_flag) {
            struct k_xferi x = {0, p, rem};
            if (ioctl(fd, K_WRITEI, &x) < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                if (errno == EPIPE) { ioctl(fd, K_PREPARE); continue; }
                rem = 0;
                break;
            }
            rem -= (k_uframes)x.result;
            p += (size_t)x.result * hw_ch;
        }
    }

    write_rms_file(0);
    ioctl(fd, K_DRAIN);
    close(fd);
    free(mp3);
    return 0;
}

static int cmd_nv12_conv(const char *in_nv12, int w, int h, const char *out_bgra, const char *out_scanlines) {
    size_t frame_sz = (size_t)w * (size_t)h * 3 / 2;
    FILE *f = fopen(in_nv12, "rb");
    if (!f) { perror(in_nv12); return 1; }
    fseek(f, 0, SEEK_END);
    long total_sz = ftell(f);
    if (total_sz < (long)frame_sz) {
        fprintf(stderr, "glass-wav nv12-conv: file too small (%ld < %zu)\n", total_sz, frame_sz);
        fclose(f);
        return 1;
    }
    /* Seek to the last complete NV12 frame so camera AE/AWB has settled */
    long nframes = total_sz / (long)frame_sz;
    fseek(f, (nframes - 1) * (long)frame_sz, SEEK_SET);
    uint8_t *nv12 = (uint8_t *)malloc(frame_sz);
    if (!nv12 || fread(nv12, 1, frame_sz, f) != frame_sz) {
        free(nv12);
        fclose(f);
        return 1;
    }
    fclose(f);

    uint8_t *bgra = (uint8_t *)malloc((size_t)w * (size_t)h * 4);
    size_t scan_sz = (size_t)h * (1 + (size_t)w * 3);
    uint8_t *scan = (uint8_t *)malloc(scan_sz);
    if (!bgra || !scan) {
        free(nv12); free(bgra); free(scan);
        return 1;
    }

    const uint8_t *y_plane = nv12;
    const uint8_t *uv_plane = nv12 + (size_t)w * (size_t)h;
    for (int y = 0; y < h; y++) {
        uint8_t *srow = scan + (size_t)y * (1 + (size_t)w * 3);
        srow[0] = 0; /* PNG filter byte 0 (None) */
        uint8_t *brow = bgra + (size_t)y * (size_t)w * 4;
        const uint8_t *yrow = y_plane + (size_t)y * (size_t)w;
        const uint8_t *uvrow = uv_plane + (size_t)(y >> 1) * (size_t)w;
        for (int x = 0; x < w; x++) {
            int Y = (int)yrow[x];
            int U = (int)uvrow[x & ~1] - 128;
            int V = (int)uvrow[(x & ~1) + 1] - 128;
            int C = Y - 16;
            if (C < 0) C = 0;
            int R = (298 * C + 409 * V + 128) >> 8;
            int G = (298 * C - 100 * U - 208 * V + 128) >> 8;
            int B = (298 * C + 516 * U + 128) >> 8;
            if (R < 0) R = 0; else if (R > 255) R = 255;
            if (G < 0) G = 0; else if (G > 255) G = 255;
            if (B < 0) B = 0; else if (B > 255) B = 255;
            brow[4 * x + 0] = (uint8_t)B;
            brow[4 * x + 1] = (uint8_t)G;
            brow[4 * x + 2] = (uint8_t)R;
            brow[4 * x + 3] = 255;
            srow[1 + 3 * x + 0] = (uint8_t)R;
            srow[1 + 3 * x + 1] = (uint8_t)G;
            srow[1 + 3 * x + 2] = (uint8_t)B;
        }
    }

    FILE *fb = fopen(out_bgra, "wb");
    if (fb) { fwrite(bgra, 1, (size_t)w * (size_t)h * 4, fb); fclose(fb); }
    FILE *fs = fopen(out_scanlines, "wb");
    if (fs) { fwrite(scan, 1, scan_sz, fs); fclose(fs); }
    free(nv12); free(bgra); free(scan);
    return 0;
}

static int cmd_play(const char *in_path, const char *dev_path) {
    FILE *f = fopen(in_path, "rb");
    if (!f) { perror(in_path); return 1; }
    uint8_t hdr[44];
    if (fread(hdr, 1, 44, f) != 44 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(f);
        return cmd_play_mp3(in_path, dev_path);
    }
    uint16_t in_ch = (uint16_t)(hdr[22] | (hdr[23] << 8));
    uint32_t in_rate = (uint32_t)(hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24));
    if (in_ch < 1 || in_ch > 2 || in_rate < 8000 || in_rate > 96000) {
        fprintf(stderr, "glass-wav: unsupported WAV format (%u Hz, %u ch)\n", in_rate, in_ch);
        fclose(f);
        return 1;
    }

    /* Scan forward to 'data' chunk if needed */
    if (memcmp(hdr + 36, "data", 4) != 0) {
        fseek(f, 12, SEEK_SET);
        while (!feof(f)) {
            uint8_t chdr[8];
            if (fread(chdr, 1, 8, f) != 8) break;
            uint32_t csz = (uint32_t)(chdr[4] | (chdr[5] << 8) | (chdr[6] << 16) | (chdr[7] << 24));
            if (memcmp(chdr, "data", 4) == 0) break;
            fseek(f, csz, SEEK_CUR);
        }
    }

    unsigned hw_rate = 48000, hw_ch = 2;
    int fd = pcm_open_34(dev_path, 0, &hw_rate, &hw_ch);
    if (fd < 0) { fclose(f); return 1; }

    printf("glass-wav: playing %s (%u Hz %u ch) -> %s (%u Hz %u ch)\n",
           in_path, in_rate, in_ch, dev_path, hw_rate, hw_ch);

    int16_t in_buf[1024 * 2];
    int16_t out_buf[4096 * 2];
    uint32_t step = (uint32_t)(((uint64_t)in_rate << 16) / hw_rate);
    uint32_t frac = 0;
    int32_t prev_s = 0;

    while (!stop_flag) {
        size_t frames_read = fread(in_buf, in_ch * sizeof(int16_t), 1024, f);
        if (frames_read == 0) break;

        int vol_pct = read_volume_pct();
        unsigned out_frames = 0;
        for (size_t i = 0; i < frames_read; i++) {
            int32_t cur_s = (in_ch == 2)
                ? ((int32_t)in_buf[2 * i] + (int32_t)in_buf[2 * i + 1]) / 2
                : in_buf[i];
            cur_s = scale_vol(cur_s, vol_pct);
            while (frac < 65536) {
                int16_t v = (int16_t)(prev_s + (((cur_s - prev_s) * (int32_t)frac) >> 16));
                if (hw_ch == 2) {
                    out_buf[2 * out_frames] = v;
                    out_buf[2 * out_frames + 1] = v;
                } else {
                    out_buf[out_frames] = v;
                }
                out_frames++;
                frac += step;
            }
            frac -= 65536;
            prev_s = cur_s;
        }

        int16_t *p = out_buf;
        k_uframes rem = out_frames;
        int rms = compute_pcm16_rms(out_buf, out_frames, hw_ch);
        write_rms_file(rms);
        while (rem > 0 && !stop_flag) {
            struct k_xferi x = {0, p, rem};
            if (ioctl(fd, K_WRITEI, &x) < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                if (errno == EPIPE) { ioctl(fd, K_PREPARE); continue; }
                perror("glass-wav: K_WRITEI");
                rem = 0;
                break;
            }
            rem -= (k_uframes)x.result;
            p += (size_t)x.result * hw_ch;
        }
    }

    write_rms_file(0);
    ioctl(fd, K_DRAIN);
    close(fd);
    fclose(f);
    return 0;
}

static int cmd_stream_play(const char *dev_path) {
    unsigned hw_rate = 48000, hw_ch = 2;
    int fd = pcm_open_34(dev_path, 0, &hw_rate, &hw_ch);
    if (fd < 0 && strcmp(dev_path, "/dev/snd/pcmC0D0p") != 0)
        fd = pcm_open_34("/dev/snd/pcmC0D0p", 0, &hw_rate, &hw_ch);
    if (fd < 0) return 1;

    uint32_t in_rate = 24000;
    uint32_t step = (uint32_t)(((uint64_t)in_rate << 16) / hw_rate);
    uint32_t frac = 0;
    int32_t prev_s = 0;
    int16_t in_buf[1024];
    int16_t out_buf[4096 * 2];
    uint8_t *raw_in = (uint8_t *)in_buf;
    size_t carry = 0;

    while (!stop_flag) {
        if (access("/run/glass/stop-call", F_OK) == 0) {
            ioctl(fd, K_DROP);
            break;
        }
        ssize_t n = read(STDIN_FILENO, raw_in + carry, sizeof(in_buf) - carry);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            break;
        }
        size_t total_b = carry + (size_t)n;
        size_t samples = total_b / 2;
        carry = total_b & 1;

        int vol_pct = read_volume_pct();
        unsigned out_frames = 0;
        for (size_t i = 0; i < samples; i++) {
            int32_t cur_s = scale_vol(in_buf[i], vol_pct);
            while (frac < 65536 && out_frames < 4096) {
                int16_t v = (int16_t)(prev_s + (((cur_s - prev_s) * (int32_t)frac) >> 16));
                if (hw_ch == 2) {
                    out_buf[2 * out_frames] = v;
                    out_buf[2 * out_frames + 1] = v;
                } else {
                    out_buf[out_frames] = v;
                }
                out_frames++;
                frac += step;
            }
            frac -= 65536;
            prev_s = cur_s;
        }
        if (carry)
            raw_in[0] = raw_in[samples * 2];

        int16_t *p = out_buf;
        k_uframes rem = out_frames;
        int rms = compute_pcm16_rms(out_buf, out_frames, hw_ch);
        write_rms_file(rms);
        while (rem > 0 && !stop_flag) {
            struct k_xferi x = {0, p, rem};
            if (ioctl(fd, K_WRITEI, &x) < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                if (errno == EPIPE) { ioctl(fd, K_PREPARE); continue; }
                rem = 0;
                break;
            }
            rem -= (k_uframes)x.result;
            p += (size_t)x.result * hw_ch;
        }
    }

    write_rms_file(0);
    ioctl(fd, K_DRAIN);
    close(fd);
    return 0;
}

static int cmd_stream_mic(double duration_s, const char *dev_path) {
    unsigned hw_rate = 48000, hw_ch = 2;
    int fd = pcm_open_34(dev_path, 1, &hw_rate, &hw_ch);
    if (fd < 0 && strcmp(dev_path, "/dev/snd/pcmC0D0c") != 0)
        fd = pcm_open_34("/dev/snd/pcmC0D0c", 1, &hw_rate, &hw_ch);
    if (fd < 0) return 1;

    uint32_t out_rate = 24000;
    uint32_t lead_silence = 8400;   /* 0.35s leading silence so server_vad arms immediately */
    uint32_t trail_silence = 15600; /* 0.65s trailing silence so server_vad fires speech_stopped */
    uint32_t max_speech_samples = (uint32_t)(out_rate * duration_s);

    int16_t zero_buf[960];
    memset(zero_buf, 0, sizeof zero_buf);
    uint32_t rem_lead = lead_silence;
    while (rem_lead > 0) {
        uint32_t n = rem_lead > 960 ? 960 : rem_lead;
        if (fwrite(zero_buf, sizeof(int16_t), n, stdout) != n) break;
        rem_lead -= n;
    }
    fflush(stdout);

    unlink("/run/glass/stop-record");
    ioctl(fd, K_START);

    int16_t in_buf[1024 * 2];
    int16_t out_buf[1024];
    uint32_t written = 0;
    uint32_t step = (uint32_t)(((uint64_t)hw_rate << 16) / out_rate);
    uint32_t acc_pos = 0;
    int speech_chunks = 0;
    int quiet_after_speech_chunks = 0;

    while (written < max_speech_samples && !stop_flag) {
        if (access("/run/glass/stop-record", F_OK) == 0 ||
            access("/run/glass/stop-call", F_OK) == 0) {
            unlink("/run/glass/stop-record");
            break;
        }
        struct k_xferi x = {0, in_buf, 1024};
        if (ioctl(fd, K_READI, &x) < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (errno == EPIPE) { ioctl(fd, K_PREPARE); ioctl(fd, K_START); continue; }
            break;
        }
        k_uframes got = (k_uframes)x.result;
        unsigned out_n = 0;
        while ((acc_pos >> 16) < got && written + out_n < max_speech_samples) {
            unsigned idx = acc_pos >> 16;
            int16_t s = (hw_ch == 2)
                ? (int16_t)(((int32_t)in_buf[2 * idx] + (int32_t)in_buf[2 * idx + 1]) / 2)
                : in_buf[idx];
            int32_t boosted = ((int32_t)s * 7) / 2; /* 3.5x digital mic boost */
            if (boosted > 32000) boosted = 32000;
            if (boosted < -32000) boosted = -32000;
            out_buf[out_n++] = (int16_t)boosted;
            acc_pos += step;
        }
        if ((acc_pos >> 16) >= got)
            acc_pos -= (got << 16);
        if (out_n > 0) {
            int rms = compute_pcm16_rms(out_buf, out_n, 1);
            write_rms_file(rms * 2);
            if (fwrite(out_buf, sizeof(int16_t), out_n, stdout) != out_n)
                break;
            fflush(stdout);
            written += out_n;
            if (rms > 320) {
                speech_chunks++;
                quiet_after_speech_chunks = 0;
            } else if (speech_chunks >= 16) {
                quiet_after_speech_chunks++;
                if (quiet_after_speech_chunks >= 58) { /* ~1.25s silence after speech -> auto-commit */
                    break;
                }
            }
        }
    }

    uint32_t rem_trail = trail_silence;
    while (rem_trail > 0) {
        uint32_t n = rem_trail > 960 ? 960 : rem_trail;
        if (fwrite(zero_buf, sizeof(int16_t), n, stdout) != n) break;
        rem_trail -= n;
    }
    fflush(stdout);
    write_rms_file(0);
    ioctl(fd, K_DROP);
    close(fd);
    return 0;
}

static int cmd_record(const char *out_path, double duration_s, const char *dev_path) {
    unsigned hw_rate = 48000, hw_ch = 2;
    int fd = pcm_open_34(dev_path, 1, &hw_rate, &hw_ch);
    if (fd < 0) return 1;

    FILE *f = fopen(out_path, "wb");
    if (!f) { perror(out_path); close(fd); return 1; }

    uint32_t out_rate = 24000;
    uint32_t lead_silence = 20400; /* 0.85s leading silence so server-side VAD arms reliably */
    uint32_t trail_silence = 14400; /* 0.60s trailing silence so server-side VAD commits speech_stopped */
    uint32_t max_speech_samples = (uint32_t)(out_rate * duration_s);
    write_wav_header(f, out_rate, 1, lead_silence + max_speech_samples + trail_silence);

    /* Write 0.85s leading silence */
    int16_t zero_buf[1024];
    memset(zero_buf, 0, sizeof zero_buf);
    uint32_t rem_lead = lead_silence;
    while (rem_lead > 0) {
        uint32_t n = rem_lead > 1024 ? 1024 : rem_lead;
        fwrite(zero_buf, sizeof(int16_t), n, f);
        rem_lead -= n;
    }

    unlink("/run/glass/stop-record");
    printf("glass-wav: recording up to %.1fs from %s (%u Hz %u ch) -> %s (24000 Hz mono PCM16LE)\n",
           duration_s, dev_path, hw_rate, hw_ch, out_path);

    ioctl(fd, K_START);

    int16_t in_buf[1024 * 2];
    int16_t out_buf[1024];
    uint32_t written = 0;
    uint32_t step = (uint32_t)(((uint64_t)hw_rate << 16) / out_rate);
    uint32_t acc_pos = 0;
    int speech_chunks = 0;
    int quiet_after_speech_chunks = 0;

    while (written < max_speech_samples && !stop_flag) {
        if (access("/run/glass/stop-record", F_OK) == 0) {
            unlink("/run/glass/stop-record");
            break;
        }
        struct k_xferi x = {0, in_buf, 1024};
        if (ioctl(fd, K_READI, &x) < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            if (errno == EPIPE) { ioctl(fd, K_PREPARE); ioctl(fd, K_START); continue; }
            perror("glass-wav: K_READI");
            break;
        }
        k_uframes got = (k_uframes)x.result;
        unsigned out_n = 0;
        while ((acc_pos >> 16) < got && written + out_n < max_speech_samples) {
            unsigned idx = acc_pos >> 16;
            int16_t s = (hw_ch == 2)
                ? (int16_t)(((int32_t)in_buf[2 * idx] + (int32_t)in_buf[2 * idx + 1]) / 2)
                : in_buf[idx];
            /* Boost digital mic sample by 2.5x with soft clip so quiet speech triggers server VAD cleanly */
            int32_t boosted = ((int32_t)s * 5) / 2;
            if (boosted > 32000) boosted = 32000;
            if (boosted < -32000) boosted = -32000;
            out_buf[out_n++] = (int16_t)boosted;
            acc_pos += step;
        }
        if ((acc_pos >> 16) >= got)
            acc_pos -= (got << 16);
        if (out_n > 0) {
            int rms = compute_pcm16_rms(out_buf, out_n, 1);
            write_rms_file(rms * 2);
            if (rms > 320) {
                speech_chunks++;
                quiet_after_speech_chunks = 0;
            } else if (speech_chunks >= 18) {
                quiet_after_speech_chunks++;
                if (quiet_after_speech_chunks >= 60) { /* ~1.3s silence after speech -> auto-send! */
                    fwrite(out_buf, sizeof(int16_t), out_n, f);
                    written += out_n;
                    break;
                }
            }
            fwrite(out_buf, sizeof(int16_t), out_n, f);
            written += out_n;
        }
    }

    /* Append 0.60s trailing silence for immediate server-side VAD speech_stopped */
    uint32_t rem_trail = trail_silence;
    while (rem_trail > 0) {
        uint32_t n = rem_trail > 1024 ? 1024 : rem_trail;
        fwrite(zero_buf, sizeof(int16_t), n, f);
        rem_trail -= n;
    }

    write_rms_file(0);
    uint32_t total_written = lead_silence + written + trail_silence;
    fseek(f, 0, SEEK_SET);
    write_wav_header(f, out_rate, 1, total_written);
    fclose(f);
    ioctl(fd, K_DROP);
    close(fd);
    printf("glass-wav: recorded %u speech samples (%u total w/ VAD pad) -> %s\n",
           written, total_written, out_path);
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    if (argc >= 7 && strcmp(argv[1], "nv12-conv") == 0) {
        return cmd_nv12_conv(argv[2], atoi(argv[3]), atoi(argv[4]), argv[5], argv[6]);
    }
    if (argc >= 2 && strcmp(argv[1], "chime") == 0) {
        const char *kind = argc >= 3 ? argv[2] : "start";
        return cmd_chime(kind, "/dev/snd/pcmC0D0p");
    }
    if (argc >= 3 && strcmp(argv[1], "synth") == 0) {
        double freq = argc >= 4 ? atof(argv[3]) : 440.0;
        double dur = argc >= 5 ? atof(argv[4]) : 2.0;
        return cmd_synth(argv[2], freq, dur);
    }
    if (argc >= 2 && strcmp(argv[1], "stream-play") == 0) {
        const char *dev = "/dev/snd/pcmC0D0p";
        for (int i = 2; i + 1 < argc; i++) {
            if (strcmp(argv[i], "-d") == 0) dev = argv[++i];
        }
        return cmd_stream_play(dev);
    }
    if (argc >= 2 && strcmp(argv[1], "stream-mic") == 0) {
        double dur = (argc >= 3 && argv[2][0] != '-') ? atof(argv[2]) : 12.0;
        const char *dev = "/dev/snd/pcmC0D2c";
        for (int i = 2; i + 1 < argc; i++) {
            if (strcmp(argv[i], "-d") == 0) dev = argv[++i];
        }
        return cmd_stream_mic(dur, dev);
    }
    if (argc >= 3 && strcmp(argv[1], "play") == 0) {
        const char *dev = "/dev/snd/pcmC0D0p";
        for (int i = 3; i + 1 < argc; i++) {
            if (strcmp(argv[i], "-d") == 0) dev = argv[++i];
        }
        return cmd_play(argv[2], dev);
    }
    if (argc >= 3 && strcmp(argv[1], "record") == 0) {
        double dur = (argc >= 4 && argv[3][0] != '-') ? atof(argv[3]) : 5.0;
        const char *dev = "/dev/snd/pcmC0D2c";
        for (int i = 3; i + 1 < argc; i++) {
            if (strcmp(argv[i], "-d") == 0) dev = argv[++i];
        }
        return cmd_record(argv[2], dur, dev);
    }

    fprintf(stderr,
            "Usage:\n"
            "  glass-wav play <in.wav> [-d /dev/snd/pcmC0D0p]\n"
            "  glass-wav record <out.wav> [seconds] [-d /dev/snd/pcmC0D2c]\n"
            "  glass-wav synth <out.wav> [freq_hz] [seconds]\n");
    return 2;
}
