/* pitg.c — Pi Timecode Generator
 *
 * LTC SMPTE timecode generator for Raspberry Pi
 * Outputs via 3.5mm audio jack using libltc + ALSA
 *
 * Usage: pitg [-r fps] [-d device] [-s HH:MM:SS:FF]
 *
 * Frame rates: 24 | 25 | 29.97 | 29.97df | 30  (default: 25)
 *
 * Dependencies: libltc-dev libasound2-dev
 *   sudo apt-get install -y libltc-dev libasound2-dev
 *
 * ALSA device on Pi 1:
 *   Use default "default" (PipeWire/PulseAudio passthrough), or
 *   use "plughw:Headphones,0" for direct ALSA (lower jitter).
 *   List available devices: aplay -l
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <alsa/asoundlib.h>
#include <ltc.h>

#define SAMPLE_RATE    48000
#define BUFFER_FRAMES  4096   /* ~85ms of audio headroom */
#define PERIOD_FRAMES  1024   /* ~21ms per period */

/* Max PCM samples in one LTC frame (at 24fps: 48000/24 = 2000) */
#define MAX_FRAME_SAMPLES  2100

/* ── Signal handling ─────────────────────────────────────────────── */

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ── Frame rate table ────────────────────────────────────────────── */

typedef struct {
    const char      *name;
    double           fps;
    int              drop;    /* 1 = drop-frame */
    int              ifps;    /* integer fps (frame count ceiling) */
    enum LTC_TV_STANDARD standard;
} FPSOption;

static const FPSOption fps_opts[] = {
    { "24",      24.0,          0, 24, LTC_TV_FILM_24 },
    { "25",      25.0,          0, 25, LTC_TV_625_50  },
    { "29.97",   30000.0/1001,  0, 30, LTC_TV_525_60  },
    { "29.97df", 30000.0/1001,  1, 30, LTC_TV_525_60  },
    { "30",      30.0,          0, 30, LTC_TV_525_60  },
};

#define N_FPS_OPTS ((int)(sizeof(fps_opts) / sizeof(fps_opts[0])))

static const FPSOption *find_fps(const char *s)
{
    for (int i = 0; i < N_FPS_OPTS; i++)
        if (strcmp(s, fps_opts[i].name) == 0)
            return &fps_opts[i];
    return NULL;
}

/* ── Timecode counter ────────────────────────────────────────────── */

typedef struct { int h, m, s, f; } TC;

/*
 * Advance one frame. Handles drop-frame for 29.97df:
 * Skip frames 00 and 01 at the start of each minute,
 * except at multiples of 10 minutes.
 */
static void tc_advance(TC *tc, int ifps, int drop)
{
    tc->f++;
    if (tc->f < ifps)
        return;

    tc->f = 0;
    tc->s++;
    if (tc->s < 60)
        return;

    tc->s = 0;
    tc->m++;

    if (drop && (tc->m % 10) != 0)
        tc->f = 2; /* skip frames 00 and 01 */

    if (tc->m < 60)
        return;

    tc->m = 0;
    tc->h = (tc->h + 1) % 24;
}

/* Initialise TC from system wall clock. */
static void tc_from_clock(TC *tc, double fps)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm *t = localtime(&ts.tv_sec);

    tc->h = t->tm_hour;
    tc->m = t->tm_min;
    tc->s = t->tm_sec;
    tc->f = (int)((ts.tv_nsec / 1.0e9) * fps);

    /* Clamp to valid range */
    int maxf = (int)fps;
    if (tc->f >= maxf) tc->f = maxf - 1;
    if (tc->f < 0)     tc->f = 0;
}

/* Parse HH:MM:SS:FF — returns 1 on success, 0 on error. */
static int tc_parse(TC *tc, const char *s)
{
    if (sscanf(s, "%d:%d:%d:%d", &tc->h, &tc->m, &tc->s, &tc->f) != 4)
        return 0;
    return (tc->h >= 0 && tc->h < 24 &&
            tc->m >= 0 && tc->m < 60 &&
            tc->s >= 0 && tc->s < 60 &&
            tc->f >= 0);
}

/* ── ALSA setup ──────────────────────────────────────────────────── */

static snd_pcm_t *alsa_open(const char *device, unsigned int *rate_out)
{
    snd_pcm_t *pcm;
    int err;

    if ((err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        fprintf(stderr, "pitg: cannot open '%s': %s\n",
                device, snd_strerror(err));
        return NULL;
    }

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);

    if ((err = snd_pcm_hw_params_set_access(
                pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (err = snd_pcm_hw_params_set_format(
                pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
        (err = snd_pcm_hw_params_set_channels(pcm, hw, 1)) < 0) {
        fprintf(stderr, "pitg: cannot set hw params: %s\n",
                snd_strerror(err));
        snd_pcm_close(pcm);
        return NULL;
    }

    unsigned int rate = SAMPLE_RATE;
    snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0);
    *rate_out = rate;

    snd_pcm_uframes_t buf = BUFFER_FRAMES;
    snd_pcm_uframes_t per = PERIOD_FRAMES;
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buf);
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &per, 0);

    if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
        fprintf(stderr, "pitg: snd_pcm_hw_params: %s\n", snd_strerror(err));
        snd_pcm_close(pcm);
        return NULL;
    }

    return pcm;
}

/* ── Usage ───────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-r fps] [-d device] [-s HH:MM:SS:FF]\n\n"
        "  -r  Frame rate: 24 | 25 | 29.97 | 29.97df | 30  (default: 25)\n"
        "  -d  ALSA device  (default: default)\n"
        "  -s  Start timecode, e.g. 01:00:00:00  (default: system clock)\n",
        prog);
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    const FPSOption *fps = &fps_opts[1]; /* default: 25fps */
    const char *device = "default";
    TC tc = {0};
    int use_clock = 1;
    int opt;

    while ((opt = getopt(argc, argv, "r:d:s:h")) != -1) {
        switch (opt) {
        case 'r':
            fps = find_fps(optarg);
            if (!fps) {
                fprintf(stderr, "pitg: unknown frame rate '%s'\n", optarg);
                usage(argv[0]);
                return 1;
            }
            break;
        case 'd':
            device = optarg;
            break;
        case 's':
            if (!tc_parse(&tc, optarg)) {
                fprintf(stderr,
                        "pitg: bad timecode '%s' (use HH:MM:SS:FF)\n",
                        optarg);
                return 1;
            }
            use_clock = 0;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (use_clock)
        tc_from_clock(&tc, fps->fps);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    /* Open ALSA */
    unsigned int actual_rate = 0;
    snd_pcm_t *pcm = alsa_open(device, &actual_rate);
    if (!pcm)
        return 1;

    if (actual_rate != SAMPLE_RATE)
        fprintf(stderr,
                "pitg: warning: sample rate negotiated to %u Hz "
                "(wanted %d)\n", actual_rate, SAMPLE_RATE);

    /* Create LTC encoder */
    LTCEncoder *enc = ltc_encoder_create(
            actual_rate, fps->fps, fps->standard, LTC_USE_DATE);
    if (!enc) {
        fprintf(stderr, "pitg: failed to create LTC encoder\n");
        snd_pcm_close(pcm);
        return 1;
    }

    fprintf(stderr,
            "pitg: %s fps  |  device=%s  |  start %02d:%02d:%02d:%02d%s\n",
            fps->name, device,
            tc.h, tc.m, tc.s, tc.f,
            fps->drop ? " DF" : "");
    fprintf(stderr, "pitg: Ctrl-C to stop\n");

    int16_t conv[MAX_FRAME_SAMPLES];

    while (g_running) {
        /* Load current timecode into encoder */
        SMPTETimecode st = {
            .hours = (unsigned char)tc.h,
            .mins  = (unsigned char)tc.m,
            .secs  = (unsigned char)tc.s,
            .frame = (unsigned char)tc.f,
        };
        ltc_encoder_set_timecode(enc, &st);

        /* Set drop-frame bit in the raw LTC frame */
        LTCFrame lf;
        ltc_encoder_get_frame(enc, &lf);
        lf.dfbit = fps->drop ? 1 : 0;
        ltc_encoder_set_frame(enc, &lf);

        /* Encode one LTC frame into the internal PCM buffer */
        ltc_encoder_encode_frame(enc);

        /* Retrieve pointer to encoded samples */
        ltcsnd_sample_t *ltcbuf = NULL;
        int n = ltc_encoder_get_bufferptr(enc, &ltcbuf, 1);

        if (n <= 0 || n > MAX_FRAME_SAMPLES) {
            fprintf(stderr, "pitg: unexpected sample count %d\n", n);
            break;
        }

        /*
         * Convert ltcsnd_sample_t → int16_t for ALSA S16_LE output.
         * libltc samples are unsigned char (0–255), centred at 128.
         * Scale to full int16 range for maximum signal level.
         * Adjust output level with: amixer sset PCM <percent>%
         */
#ifdef LTC_FLOAT_SAMPLES
        for (int i = 0; i < n; i++)
            conv[i] = (int16_t)(ltcbuf[i] * 32767.0f);
#else
        for (int i = 0; i < n; i++)
            conv[i] = (int16_t)(((int)ltcbuf[i] - 128) * 256);
#endif

        /* Write to ALSA — blocking write paces the generator */
        const int16_t *ptr = conv;
        snd_pcm_sframes_t remain = (snd_pcm_sframes_t)n;

        while (remain > 0 && g_running) {
            snd_pcm_sframes_t r = snd_pcm_writei(pcm, ptr, (snd_pcm_uframes_t)remain);
            if (r == -EPIPE) {
                /* Buffer underrun — recover and retry */
                snd_pcm_prepare(pcm);
            } else if (r == -EINTR) {
                continue;
            } else if (r < 0) {
                fprintf(stderr, "pitg: ALSA write error: %s\n",
                        snd_strerror((int)r));
                g_running = 0;
                break;
            } else {
                ptr    += r;
                remain -= r;
            }
        }

        /* Advance timecode for next frame */
        tc_advance(&tc, fps->ifps, fps->drop);
    }

    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    ltc_encoder_free(enc);

    fprintf(stderr, "\npitg: stopped at %02d:%02d:%02d:%02d\n",
            tc.h, tc.m, tc.s, tc.f);
    return 0;
}
