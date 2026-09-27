/*
 * capture.c — miniaudio WASAPI loopback capture.
 *
 * Loopback in miniaudio 0.11: ma_device_type_loopback opens the *render*
 * endpoint of a playback device with AUDCLNT_STREAMFLAGS_LOOPBACK and
 * delivers its rendered audio.  We set config.sampleRate to 16000 and
 * capture.channels to 1 so miniaudio's built-in resampler + channel mixer
 * do the work in the backend thread; the data callback only copies s16
 * frames into the ring.
 *
 * Ring indices are _Atomic with acquire/release on the write side so the
 * data writes performed by the WASAPI callback are guaranteed visible to
 * the consumer before it observes the advanced write pointer.  The ring
 * length is a power of two so both sides can mask instead of taking a
 * per-sample integer modulo on the realtime thread.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* MSVC's C11 atomics require /experimental:c11atomics and aren't on by
 * default. Wrap the two 64-bit atomic ops we need behind tiny macros so this
 * file compiles cleanly on MSVC and clang/gcc without any build-flag
 * changes. On MSVC the Interlocked* intrinsics give a full barrier on x64;
 * on everything else we use C11 stdatomic. */
#ifdef _MSC_VER
#  include <intrin.h>
typedef volatile LONG64      atomic_u64;
#  define atomic_u64_load(p)     ((ma_uint64)_InterlockedCompareExchange64((volatile LONG64 *)(p), 0, 0))
#  define atomic_u64_store(p, v) ((void)_InterlockedExchange64((volatile LONG64 *)(p), (LONG64)(v)))
#else
#  include <stdatomic.h>
typedef _Atomic ma_uint64    atomic_u64;
#  define atomic_u64_load(p)     atomic_load_explicit((p), memory_order_acquire)
#  define atomic_u64_store(p, v) atomic_store_explicit((p), (v), memory_order_release)
#endif

#include "miniaudio.h"
#include "capture.h"

#define SAMPLE_RATE   16000
#define MAX_CHUNK_SEC 10

#define MAX_DEVICES 64

/* Ring: enough headroom for one full chunk of writer lead.  Power of two so
 * the hot paths can use a mask.  1<<19 = 524288 frames = 32.768 s @ 16 kHz. */
#define RING_FRAMES ((ma_uint64)1u << 19)
#define RING_MASK   (RING_FRAMES - 1u)

static ma_context  g_ctx;
static ma_device   g_dev;
static int         g_chunk_samples = 0;

static ma_device_id  g_devices[MAX_DEVICES];
static char          g_dev_names[MAX_DEVICES][MA_MAX_DEVICE_NAME_LENGTH + 1];
static ma_bool32     g_dev_is_default[MAX_DEVICES];
static int           g_n_devices = 0;

static int16_t             g_ring[RING_FRAMES];
static atomic_u64          g_ring_read    = 0;
static atomic_u64          g_ring_written = 0;

static HANDLE g_have_chunk = NULL;
static int    g_ctx_ok = 0;
static int    g_cs_ok = 0;
static int    g_started = 0;
static CRITICAL_SECTION g_cs;

/* ---------- device enumeration ---------- */

struct enum_ctx { int index; };

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

    g_have_chunk = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_have_chunk) {
        fprintf(stderr, "capture: CreateEventW failed\n");
        return 0;
    }

    g_ctx_ok = 1;
    return g_n_devices > 0;
}

/* ---------- capture device ---------- */

static void ma_data_callback(ma_device *pDevice, void *pOut,
                             const void *pIn, ma_uint32 frameCount) {
    (void)pDevice; (void)pOut;
    if (!g_started) return;

    const int16_t *in = (const int16_t *)pIn;
    ma_uint64 w = atomic_u64_load(&g_ring_written);

    for (ma_uint32 i = 0; i < frameCount; i++)
        g_ring[(w + i) & RING_MASK] = in[i];

    w += frameCount;
    /* Release: sample writes must be visible before the new write pointer. */
    atomic_u64_store(&g_ring_written, w);

    const ma_uint64 r = atomic_u64_load(&g_ring_read);
    if (w - r >= (ma_uint64)g_chunk_samples)
        SetEvent(g_have_chunk);

int capture_start(int device_index) {
    if (g_chunk_samples <= 0 || (ma_uint64)g_chunk_samples > RING_FRAMES) {
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

    ResetEvent(g_have_chunk);
    EnterCriticalSection(&g_cs);
    atomic_u64_store(&g_ring_read,    0);
    atomic_u64_store(&g_ring_written, 0);
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

int capture_read_chunk(int16_t *out) {
    for (;;) {
        if (WaitForSingleObject(g_have_chunk, 50) != WAIT_OBJECT_0) {
            EnterCriticalSection(&g_cs);
            int started = g_started;
            LeaveCriticalSection(&g_cs);
            if (!started) return 0;
            continue;
        }

        /* Acquire: pairs with the producer's release store above. */
        const ma_uint64 w = atomic_u64_load(&g_ring_written);
        const ma_uint64 r = atomic_u64_load(&g_ring_read);
        if (w - r >= (ma_uint64)g_chunk_samples)
            break;
        EnterCriticalSection(&g_cs);
        int started = g_started;
        LeaveCriticalSection(&g_cs);
        if (!started) return 0;
    }

    const ma_uint64 r = atomic_u64_load(&g_ring_read);
    for (int i = 0; i < g_chunk_samples; i++)
        out[i] = g_ring[(r + i) & RING_MASK];
    atomic_u64_store(&g_ring_read, r + (ma_uint64)g_chunk_samples);
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
