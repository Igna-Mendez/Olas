/*
 * capture.c — miniaudio WASAPI loopback capture.
 *
 * Loopback in miniaudio 0.11: ma_device_type_loopback opens the *render*
 * endpoint of a playback device with AUDCLNT_STREAMFLAGS_LOOPBACK and
 * delivers its rendered audio. We set config.sampleRate to 16000 and
 * capture.channels to 1 so miniaudio's built-in resampler (native rate ->
 * 16 kHz, linear) and channel mixer (stereo -> mono) do the work in the
 * backend thread; the data callback then only copies s16 frames into the
 * ring. No hand-rolled resampler, no PortAudio.
 */

#define WIN32_LEAN_AND_MEAN
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

/* ring sized for 2 * max chunk: the writer can be one full chunk ahead of
 * the reader without overwriting unread frames */
#define RING_FRAMES (SAMPLE_RATE * MAX_CHUNK_SEC * 2)

static ma_context  g_ctx;
static ma_device   g_dev;
static int         g_chunk_samples = 0;   /* set by the app before capture_start */

static ma_device_id  g_devices[MAX_DEVICES];
static char          g_dev_names[MAX_DEVICES][MA_MAX_DEVICE_NAME_LENGTH + 1];
static ma_bool32     g_dev_is_default[MAX_DEVICES];
static int           g_n_devices = 0;

static int16_t   g_ring[RING_FRAMES];
static volatile ma_uint64 g_ring_read  = 0;  /* advanced by capture_read_chunk (UI thread) */
static ma_uint64 g_ring_written = 0;         /* advanced by the miniaudio callback   */

static HANDLE g_have_chunk = NULL;   /* auto-reset event: set when a chunk is ready */
static int    g_ctx_ok = 0;
static int    g_cs_ok = 0;
static int    g_started = 0;
static CRITICAL_SECTION g_cs;        /* guards g_started */

/* ---------- device enumeration ---------- */

struct enum_ctx { int index; };

/*
 * WASAPI enumerates playback AND capture endpoints; loopback opens the
 * *render* endpoint, so we only want playback devices.
 */
static ma_bool32 ma_enum_devices(ma_context *pContext, ma_device_type deviceType,
                                 const ma_device_info *pDevice, void *pUserData) {
    (void)pContext;
    if (deviceType != ma_device_type_playback) return MA_TRUE;
    struct enum_ctx *e = (struct enum_ctx *)pUserData;
    if (e->index >= MAX_DEVICES) return MA_FALSE;
    memcpy(&g_devices[e->index], &pDevice->id, sizeof(ma_device_id));
    strncpy(g_dev_names[e->index], pDevice->name,
            sizeof(g_dev_names[0]) - 1);
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

    g_have_chunk = CreateEventW(NULL, FALSE, FALSE, NULL);  /* auto-reset */
    if (!g_have_chunk) {
        fprintf(stderr, "capture: CreateEventW failed\n");
        return 0;
    }

    g_ctx_ok = 1;
    return g_n_devices > 0;
}

/* ---------- capture device ---------- */

/* miniaudio audio callback: copy s16 mono frames into the ring.
 * Skips writing while g_started == 0 (device stopped or not yet started),
 * so a still-callbacking previous device can't write into a ring that
 * capture_start is about to reset. */
static void ma_data_callback(ma_device *pDevice, void *pOut,
                                        const void *pIn, ma_uint32 frameCount) {
    (void)pDevice; (void)pOut;
    if (!g_started) return;
    const int16_t *in = (const int16_t *)pIn;
    for (ma_uint32 i = 0; i < frameCount; i++) {
        ma_uint64 pos = (g_ring_written + i) % RING_FRAMES;
        g_ring[pos] = in[i];
    }
    g_ring_written += frameCount;

    if (g_ring_written - g_ring_read >= (ma_uint64)g_chunk_samples)
        SetEvent(g_have_chunk);
}

int capture_start(int device_index) {
    if (g_chunk_samples <= 0 || g_chunk_samples > RING_FRAMES) {
        fprintf(stderr, "capture: chunk_samples out of range (%d)\n", g_chunk_samples);
        return 0;
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_loopback);
    cfg.sampleRate = SAMPLE_RATE;
    cfg.capture.channels = 1;
    cfg.capture.format   = ma_format_s16;
    if (device_index >= 0 && device_index < g_n_devices)
        cfg.capture.pDeviceID = &g_devices[device_index];
    cfg.dataCallback = ma_data_callback;

    if (ma_device_init(&g_ctx, &cfg, &g_dev) != MA_SUCCESS) {
        fprintf(stderr, "capture: ma_device_init (loopback) failed\n");
        return 0;
    }
    if (ma_device_start(&g_dev) != MA_SUCCESS) {
        fprintf(stderr, "capture: ma_device_start failed\n");
        ma_device_uninit(&g_dev);
        return 0;
    }

    /* drop whatever was in the ring before start; both pointers stay in sync.
     * Safe without a device uninit here: capture_start is only called after
     * a previous capture_stop fully joined the old callback thread. */
    ResetEvent(g_have_chunk);
    EnterCriticalSection(&g_cs);
    g_ring_read = g_ring_written = 0;
    g_started = 1;
    LeaveCriticalSection(&g_cs);
    /* note: the pump thread polls capture_is_started() on its ~200 ms
     * timeout, so the newest chunk is picked up within that window after a
     * start; audio keeps accumulating in the ring meanwhile. */
    return 1;
}

void capture_stop(void) {
    int was_started;
    EnterCriticalSection(&g_cs);
    was_started = g_started;
    if (was_started) g_started = 0;
    LeaveCriticalSection(&g_cs);
    if (was_started) {
        /* uninit (which joins the miniaudio backend/callback thread) OUTSIDE
         * g_cs: a reader in capture_read_chunk() may hold/need g_cs, and the
         * callback may run one last frame between device_stop and uninit */
        ma_device_stop(&g_dev);
        ma_device_uninit(&g_dev);
        /* wake any reader blocked in capture_read_chunk() */
        SetEvent(g_have_chunk);
    }
}

/* ---------- public accessors ---------- */

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
    EnterCriticalSection(&g_cs);
    started = g_started;
    LeaveCriticalSection(&g_cs);
    return started;
}

int  capture_read_chunk(int16_t *out) {
    /* block until a full chunk has been written, or capture stops */
    for (;;) {
        if (WaitForSingleObject(g_have_chunk, 50) != WAIT_OBJECT_0) {
            EnterCriticalSection(&g_cs);
            int started = g_started;
            LeaveCriticalSection(&g_cs);
            if (!started) return 0;
            continue;
        }
        ma_uint64 avail = g_ring_written - g_ring_read;
        if (avail >= (ma_uint64)g_chunk_samples)
            break;
        EnterCriticalSection(&g_cs);
        int started = g_started;
        LeaveCriticalSection(&g_cs);
        if (!started) return 0;
    }

    for (int i = 0; i < g_chunk_samples; i++) {
        ma_uint64 pos = (g_ring_read + i) % RING_FRAMES;
        out[i] = g_ring[pos];
    }
    g_ring_read += (ma_uint64)g_chunk_samples;
    return 1;
}

void capture_uninit(void) {
    capture_stop();
    if (g_have_chunk) {
        CloseHandle(g_have_chunk);
        g_have_chunk = NULL;
    }
    if (g_ctx_ok) {
        ma_context_uninit(&g_ctx);
        g_ctx_ok = 0;
    }
    if (g_cs_ok) {
        DeleteCriticalSection(&g_cs);
        g_cs_ok = 0;
    }
}
