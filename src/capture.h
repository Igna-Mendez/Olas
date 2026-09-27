/*
 * capture — miniaudio WASAPI loopback capture for whisperpipe.
 *
 * Opens the *render* endpoint of the selected output device in WASAPI
 * loopback mode (ma_device_type_loopback) and delivers 16 kHz mono s16
 * frames (miniaudio resamples and downmixes) into a chunk-sized ring.
 *
 * The UI thread (see win32_ui.c) calls capture_read_chunk() to block until
 * a full chunk is available — same pump design as the Linux port's
 * pa_simple_read loop.
 */
#ifndef CAPTURE_H
#define CAPTURE_H

#include <stdint.h>

/*
 * Initialize miniaudio and enumerate playback devices into g_devices.
 * Returns 1 on success (g_n_devices >= 0).
 */
int  capture_init(void);

/* Set the chunk size (s16 mono frames) before the first capture_start. */
void capture_set_chunk_samples(int n);

/* Start loopback capture on playback device index (-1 = default device).
 * Returns 1 on success. */
int  capture_start(int device_index);

/* Stop capture and release the device. */
void capture_stop(void);

/* Release everything (context, device, ring). */
void capture_uninit(void);

/* Number of enumerated playback devices. */
int  capture_device_count(void);

/* Name of device index ("" if out of range). */
const char *capture_device_name(int index);

/* Is device index the system default? */
int  capture_device_is_default(int index);

/*
 * Block until g_chunk_samples s16 mono frames are available, then copy them
 * into out and return 1. Returns 0 if capture was stopped while waiting.
 */
int  capture_read_chunk(int16_t *out);

/* 1 if capture is currently running (between capture_start and capture_stop). */
int  capture_is_started(void);

#endif
