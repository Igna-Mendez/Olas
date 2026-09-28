/*
 * capture.c — miniaudio WASAPI loopback capture (fixed).
 *
 * Key fixes vs. previous version:
 *   - low-latency profile instead of conservative.
 *   - Explicit diagnostic dump of the ACTUAL native format + first-callback
 *     frame counts, so we can prove whether miniaudio is resampling or not.
 *   - If miniaudio hands us non-16kHz/non-mono/non-s16 data (because the
 *     loopback resampler did not engage), we now resample and convert it
 *     ourselves with ma_resampler + ma_channel_converter, then push 16 kHz
 *     mono s16 into the ring.
 *   - Error paths from ma_device_init now report the actual result code.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "miniaudio.h"
#include "capture.h"

#define SAMPLE_RATE   16000
#define MAX_CHUNK_SEC 10
#define MAX_DEVICES 64

#define RING_FRAMES ((ma_uint64)1u << 19)
#define RING_MASK   (RING_FRAMES - 1u)

/* ---- 64-bit atomics ---- */
static ma_uint64 atomic_load_u64(volatile ma_uint64 *p) {
#ifdef _MSC_VER
    return (ma_uint64)InterlockedCompareExchange64((volatile LONG64 *)p, 0, 0);
#else
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
#endif
}
static void atomic_store_u64(volatile ma_uint64 *p, ma_uint64 v) {
#ifdef _MSC_VER
    InterlockedExchange64((volatile LONG64 *)p, (LONG64)v);
#else
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
#endif
}

/* ---- module state ---- */
static ma_context  g_ctx;
static ma_device   g_dev;
static int         g_chunk_samples = 0;

/* Conversion state — created in capture_start() if the native format
 * delivered by miniaudio is not already 16 kHz mono s16. */
static ma_resampler         g_resampler;
static ma_channel_converter g_chconv;
static int                  g_have_resampler = 0;
static int                  g_have_chconv    = 0;

static ma_device_id  g_devices[MAX_DEVICES];
static char          g_dev_names[MAX_DEVICES][MA_MAX_DEVICE_NAME_LENGTH + 1];
static ma_bool32     g_dev_is_default[MAX_DEVICES];
static int           g_n_devices = 0;

static int16_t             g_ring[RING_FRAMES];
static volatile ma_uint64  g_ring_read    = 0;
static volatile ma_uint64  g_ring_written = 0;

static HANDLE g_have_chunk = NULL;
static int    g_ctx_ok = 0;
static int    g_cs_ok = 0;
static int    g_started = 0;
static CRITICAL_SECTION g_cs;

/* ---- device enumeration ---- */
struct enum_ctx { int index; };

static ma_bool32 ma_enum_devices(ma_context *pContext, ma_device_type deviceType,
                                 const ma_device_info *pDevice, void *pUserData) {
    (void)pContext;
    if (deviceType != ma_device_type_playback) return MA_TRUE;
    struct enum_ctx *e = (struct enum_ctx *)pUserData;
    if (e->index >= MAX_DEVICES) return MA_FALSE;
    memcpy(&g_devices[e->index], &pDevice->id, sizeof(ma_device_id));
    strncpy(g_dev_names[e->index], pDevice->name, sizeof(g_dev_names[0]) - 1);
    g_dev_names[e->index][sizeof(g_dev_names[0]) - 1] = '\0';
    g_dev_is_default[e->index] = pDevice->isDefault;
    e->index++;
    return MA_TRUE;
}

void capture_set_chunk_samples(int n) { g_chunk_samples = n; }

int capture_init(void) {
    ma_context_config cc = ma_context_config_init();
    if (ma_context_init(NULL, 0, &cc, &g_ctx) != MA_SUCCESS) {
        fprintf(stderr, "capture: ma_context_init failed\n");
        return 0;
    }

    struct enum_ctx e = { 0 };
    ma_context_enumerate_devices(&g_ctx, ma_enum_devices, &e);
    g_n_devices = e.index;

    if (g_n_devices == 0)
        fprintf(stderr, "capture: no playback devices found\n");

    g_cs_ok = 1;
    InitializeCriticalSection(&g_cs);

    g_have_chunk = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_have_chunk) {
        fprintf(stderr, "capture: CreateEventW failed\n");
        return 0;
    }
    g_ctx_ok = 1;
    return g_n_devices > 0;
}

/* ---- push converted 16 kHz mono s16 frames into the ring ---- */
static void ring_push(const int16_t *frames, ma_uint32 count) {
    if (count == 0) return;
    ma_uint64 w = atomic_load_u64(&g_ring_written);
    for (ma_uint32 i = 0; i < count; ++i)
        g_ring[(w + i) & RING_MASK] = frames[i];
    w += count;
    atomic_store_u64(&g_ring_written, w);

    const ma_uint64 r = atomic_load_u64(&g_ring_read);
    if (w - r >= (ma_uint64)g_chunk_samples)
        SetEvent(g_have_chunk);
}

/* ---- data callback ---- */
static void ma_data_callback(ma_device *pDevice, void *pOut,
                             const void *pIn, ma_uint32 frameCount) {
    (void)pDevice; (void)pOut;
    if (!g_started || !pIn || frameCount == 0) return;

    /* One-shot diagnostic to prove what miniaudio is actually feeding us. */
    static int logged = 0;
    if (!logged) {
        logged = 1;
        fprintf(stderr,
            "capture: first callback frameCount=%u native=%u Hz ch=%u fmt=%d "
            "(converter %s)\n",
            frameCount, g_dev.sampleRate, g_dev.capture.channels,
            (int)g_dev.capture.format,
            g_have_resampler ? "engaged" : "passthrough");
        fflush(stderr);
    }

    /* Fast path: miniaudio already delivered 16 kHz mono s16. */
    if (!g_have_resampler && !g_have_chconv) {
        ring_push((const int16_t *)pIn, frameCount);
        return;
    }

    /* Slow path: convert native → f32, channel-convert → mono,
     * resample → 16 kHz, quantize → s16, push. */
    static float  fbuf_in[16384];
    static float  fbuf_mono[16384];
    static int16_t outbuf[16384];

    ma_uint32 n = frameCount;
    if (n > 16384) n = 16384;   /* clamp; should never happen */

    if (g_dev.capture.format == ma_format_f32) {
        memcpy(fbuf_in, pIn, n * sizeof(float));
    } else {
        ma_convert_pcm_frames_format(fbuf_in, ma_format_f32,
                                     pIn, g_dev.capture.format,
                                     n, g_dev.capture.channels,
                                     ma_dither_mode_none);
    }

    ma_uint32 mono_frames = n;
    const float *mono_ptr = fbuf_in;
    if (g_have_chconv) {
        ma_channel_converter_process_pcm_frames(&g_chconv,
                                                fbuf_mono, fbuf_in, n);
        mono_ptr = fbuf_mono;
    }

    ma_uint64 out_frames = 0;
    if (g_have_resampler) {
        ma_uint64 in_frames_64 = mono_frames;
        ma_resampler_process_pcm_frames(&g_resampler,
                                        mono_ptr, &in_frames_64,
                                        fbuf_mono, &out_frames);
        for (ma_uint64 i = 0; i < out_frames; ++i) {
            float s = fbuf_mono[i];
            if (s >  1.0f) s =  1.0f;
            if (s < -1.0f) s = -1.0f;
            outbuf[i] = (int16_t)(s * 32767.0f);
        }
        ring_push(outbuf, (ma_uint32)out_frames);
    } else {
        for (ma_uint32 i = 0; i < mono_frames; ++i) {
            float s = mono_ptr[i];
            if (s >  1.0f) s =  1.0f;
            if (s < -1.0f) s = -1.0f;
            outbuf[i] = (int16_t)(s * 32767.0f);
        }
        ring_push(outbuf, mono_frames);
    }
}

int capture_start(int device_index) {
    if (g_chunk_samples <= 0 || (ma_uint64)g_chunk_samples > RING_FRAMES) {
        fprintf(stderr, "capture: chunk_samples out of range (%d)\n", g_chunk_samples);
        return 0;
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_loopback);
    cfg.performanceProfile = ma_performance_profile_low_latency;
    cfg.wasapi.usage       = ma_wasapi_usage_pro_audio;

    /* Ask for what we want. If miniaudio honours it, we get 16 kHz mono s16
     * and skip all per-callback conversion. If it doesn't, the callback
     * still receives native-rate data and the converters below fix it. */
    cfg.sampleRate       = SAMPLE_RATE;
    cfg.capture.channels = 1;
    cfg.capture.format   = ma_format_s16;

    if (device_index >= 0 && device_index < g_n_devices)
        cfg.capture.pDeviceID = &g_devices[device_index];
    cfg.dataCallback = ma_data_callback;

    ma_result r = ma_device_init(&g_ctx, &cfg, &g_dev);
    if (r != MA_SUCCESS) {
        fprintf(stderr, "capture: ma_device_init failed (%d)\n", (int)r);
        return 0;
    }

    /* Detect whether miniaudio actually gave us 16 kHz mono s16. */
    g_have_resampler = 0;
    g_have_chconv    = 0;

    const ma_format  got_fmt  = g_dev.capture.format;
    const ma_uint32  got_ch   = g_dev.capture.channels;
    const ma_uint32  got_rate = g_dev.sampleRate;

    fprintf(stderr,
        "capture: negotiated %u Hz, %u ch, fmt=%d (requested 16000/1/s16)\n",
        got_rate, got_ch, (int)got_fmt);

    if (got_ch > 1) {
        ma_channel_converter_config cc = ma_channel_converter_config_init(
            got_fmt, got_ch, NULL, 1, NULL, ma_channel_mix_mode_default);
        if (ma_channel_converter_init(&cc, NULL, &g_chconv) == MA_SUCCESS)
            g_have_chconv = 1;
        else
            fprintf(stderr, "capture: channel converter init failed\n");
    }

    if (got_rate != SAMPLE_RATE) {
        ma_resampler_config rc = ma_resampler_config_init(
            ma_format_f32, 1, got_rate, SAMPLE_RATE,
            ma_resample_algorithm_linear);
        if (ma_resampler_init(&rc, NULL, &g_resampler) == MA_SUCCESS)
            g_have_resampler = 1;
        else
            fprintf(stderr, "capture: resampler init failed\n");
    }

    r = ma_device_start(&g_dev);
    if (r != MA_SUCCESS) {
        fprintf(stderr, "capture: ma_device_start failed (%d)\n", (int)r);
        ma_device_uninit(&g_dev);
        return 0;
    }

    ResetEvent(g_have_chunk);
    EnterCriticalSection(&g_cs);
    atomic_store_u64(&g_ring_read,    0);
    atomic_store_u64(&g_ring_written, 0);
    g_started = 1;
    LeaveCriticalSection(&g_cs);
    return 1;
}

void capture_stop(void) {
    int was_started;
    EnterCriticalSection(&g_cs);
    was_started = g_started;
    if (was_started) g_started = 0;
    LeaveCriticalSection(&g_cs);
    if (was_started) {
        ma_device_stop(&g_dev);
        ma_device_uninit(&g_dev);
        if (g_have_resampler) { ma_resampler_uninit(&g_resampler, NULL); g_have_resampler = 0; }
        if (g_have_chconv)    { ma_channel_converter_uninit(&g_chconv, NULL); g_have_chconv = 0; }
        SetEvent(g_have_chunk);
    }
}

int  capture_device_count(void) { return g_n_devices; }
const char *capture_device_name(int index) {
    if (index < 0 || index >= g_n_devices) return "";
    return g_dev_names[index];
}
int  capture_device_is_default(int index) {
    if (index < 0 || index >= g_n_devices) return 0;
    return g_dev_is_default[index];
}
int  capture_is_started(void) {
    int started;
    EnterCriticalSection(&g_cs); started = g_started; LeaveCriticalSection(&g_cs);
    return started;
}

int capture_read_chunk(int16_t *out) {
    for (;;) {
        if (WaitForSingleObject(g_have_chunk, 50) != WAIT_OBJECT_0) {
            EnterCriticalSection(&g_cs);
            int started = g_started;
            LeaveCriticalSection(&g_cs);
            if (!started) return 0;
            continue;
        }
        const ma_uint64 w = atomic_load_u64(&g_ring_written);
        const ma_uint64 r = atomic_load_u64(&g_ring_read);
        if (w - r >= (ma_uint64)g_chunk_samples) break;
        EnterCriticalSection(&g_cs);
        int started = g_started;
        LeaveCriticalSection(&g_cs);
        if (!started) return 0;
    }
    const ma_uint64 r = atomic_load_u64(&g_ring_read);
    for (int i = 0; i < g_chunk_samples; i++)
        out[i] = g_ring[(r + i) & RING_MASK];
    atomic_store_u64(&g_ring_read, r + (ma_uint64)g_chunk_samples);
    return 1;
}

void capture_uninit(void) {
    capture_stop();
    if (g_have_chunk) { CloseHandle(g_have_chunk); g_have_chunk = NULL; }
    if (g_ctx_ok)     { ma_context_uninit(&g_ctx); g_ctx_ok = 0; }
    if (g_cs_ok)      { DeleteCriticalSection(&g_cs); g_cs_ok = 0; }
}
