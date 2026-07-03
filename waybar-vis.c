#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include "fft.h"

#define N_SAMPLES 512
#define N_BINS    256
#define RING_SIZE 8192
#define RING_MASK (RING_SIZE - 1)

#define MIN_DBFS -60.0f
#define SMOOTH_ATTACK 0.0f
#define SMOOTH_DECAY  0.82f

static struct pw_main_loop *main_loop = NULL;
static struct pw_core *core = NULL;
static struct pw_stream *stream = NULL;
static struct spa_source *timer_src = NULL;
static struct spa_hook stream_listener;

static float ring_buf[RING_SIZE];
static volatile int write_pos = 0;
static int read_pos = 0;

static float *smoothed = NULL;
static int n_bands = 16;
static int refresh_rate = 30;

static int running = 1;

static void sighandler(int sig)
{
    (void)sig;
    running = 0;
    if (main_loop)
        pw_main_loop_quit(main_loop);
}

static void compute_bands(const float *freq, float *bands, int n)
{
    float xscale[n + 1];
    for (int i = 0; i <= n; i++)
        xscale[i] = powf(256.0f, (float)i / n) - 0.5f;

    for (int b = 0; b < n; b++)
    {
        int a = (int)ceilf(xscale[b]);
        int b2 = (int)floorf(xscale[b + 1]);
        float val = 0.0f;

        if (b2 < a)
        {
            int idx = b2;
            if (idx >= 0 && idx < N_BINS)
                val = freq[idx] * (xscale[b + 1] - xscale[b]);
        }
        else
        {
            if (a > 0 && a - 1 < N_BINS)
                val += freq[a - 1] * (a - xscale[b]);
            for (int k = a; k < b2 && k < N_BINS; k++)
                val += freq[k];
            if (b2 < N_BINS)
                val += freq[b2] * (xscale[b + 1] - b2);
        }

        val *= (float)n / 12.0f;
        float db = 20.0f * log10f(val + 1e-10f);
        float norm = (db - MIN_DBFS) / (-MIN_DBFS);
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;
        bands[b] = norm;
    }
}

static void on_process(void *userdata)
{
    (void)userdata;
    struct pw_buffer *b = pw_stream_dequeue_buffer(stream);
    if (!b) return;

    struct spa_buffer *spa_buf = b->buffer;
    struct spa_data *d = &spa_buf->datas[0];

    if (d->data)
    {
        float *samples = (float *)d->data;
        int n = d->chunk->size / sizeof(float);
        int wp = write_pos;
        for (int i = 0; i < n && (wp - read_pos) < RING_SIZE - 1; i++)
        {
            ring_buf[wp & RING_MASK] = samples[i];
            wp++;
        }
        write_pos = wp;
    }

    pw_stream_queue_buffer(stream, b);
}

static void on_timer(void *userdata, uint64_t expirations)
{
    (void)userdata;
    (void)expirations;
    int wp = write_pos;
    int avail = wp - read_pos;

    if (avail < N_SAMPLES) return;

    float pcm[N_SAMPLES];
    for (int i = 0; i < N_SAMPLES; i++)
        pcm[i] = ring_buf[(read_pos + i) & RING_MASK];
    read_pos += N_SAMPLES;

    float freq[N_BINS];
    calculate_spectrum(pcm, N_SAMPLES, freq, 44100);

    float raw[n_bands];
    compute_bands(freq, raw, n_bands);

    static const char *blocks[] = {
        "\xe2\x96\x81", "\xe2\x96\x82", "\xe2\x96\x83", "\xe2\x96\x84",
        "\xe2\x96\x85", "\xe2\x96\x86", "\xe2\x96\x87", "\xe2\x96\x88"
    };

    char text[256];
    int pos = 0;
    for (int i = 0; i < n_bands; i++)
    {
        if (raw[i] > smoothed[i])
            smoothed[i] = raw[i];
        else
            smoothed[i] = smoothed[i] * SMOOTH_DECAY +
                          raw[i] * (1.0f - SMOOTH_DECAY);

        int level = (int)(smoothed[i] * 8.0f);
        if (level < 0) level = 0;
        if (level > 7) level = 7;
        pos += snprintf(text + pos, sizeof(text) - pos, "%s", blocks[level]);
    }

    printf("{\"text\":\"%s\",\"class\":\"vis\",\"tooltip\":\"Spectrum %d bands\"}\n",
           text, n_bands);
    fflush(stdout);
}

static void on_state_changed(void *userdata, enum pw_stream_state old,
                             enum pw_stream_state state, const char *error)
{
    (void)userdata;
    (void)old;
    if (state == PW_STREAM_STATE_ERROR)
    {
        fprintf(stderr, "stream error: %s\n", error ? error : "unknown");
        pw_main_loop_quit(main_loop);
    }
}

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc)
            n_bands = atoi(argv[++i]);
        else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc)
            refresh_rate = atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0)
        {
            printf("usage: waybar-vis [-b bands] [-r rate]\n");
            return 0;
        }
    }

    if (n_bands < 4) n_bands = 4;
    if (n_bands > 64) n_bands = 64;
    if (refresh_rate < 10) refresh_rate = 10;
    if (refresh_rate > 60) refresh_rate = 60;

    smoothed = calloc(n_bands, sizeof(float));
    if (!smoothed) return 1;

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    signal(SIGPIPE, SIG_IGN);

    pw_init(&argc, &argv);

    main_loop = pw_main_loop_new(NULL);
    if (!main_loop) { fprintf(stderr, "failed to create main loop\n"); return 1; }

    struct pw_loop *loop = pw_main_loop_get_loop(main_loop);
    struct pw_context *context = pw_context_new(loop, NULL, 0);
    if (!context) { fprintf(stderr, "failed to create context\n"); return 1; }

    struct pw_properties *props = pw_properties_new(
        PW_KEY_NODE_NAME, "waybar-vis",
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_STREAM_CAPTURE_SINK, "true",
        NULL
    );

    core = pw_context_connect(context, NULL, 0);
    if (!core) { fprintf(stderr, "failed to connect core\n"); return 1; }

    stream = pw_stream_new(core, "waybar-vis", props);
    if (!stream) { fprintf(stderr, "failed to create stream\n"); return 1; }

    struct pw_stream_events events = {
        .version = PW_VERSION_STREAM_EVENTS,
        .process = on_process,
        .state_changed = on_state_changed,
    };
    pw_stream_add_listener(stream, &stream_listener, &events, NULL);

    uint8_t params_buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
        &SPA_AUDIO_INFO_RAW_INIT(
            .format = SPA_AUDIO_FORMAT_F32,
            .channels = 1,
            .rate = 44100
        ));

    if (pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
            params, 1) < 0)
    {
        fprintf(stderr, "failed to connect stream\n");
        return 1;
    }

    timer_src = pw_loop_add_timer(loop, on_timer, NULL);
    if (!timer_src) { fprintf(stderr, "failed to create timer\n"); return 1; }

    struct timespec ts = {0, 1000000000 / refresh_rate};
    pw_loop_update_timer(loop, timer_src, &ts, &ts, false);

    pw_main_loop_run(main_loop);

    pw_loop_destroy_source(loop, timer_src);
    spa_hook_remove(&stream_listener);
    pw_stream_destroy(stream);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(main_loop);
    pw_deinit();
    free(smoothed);

    return 0;
}
