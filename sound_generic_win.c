// Windows port of sound_generic.c -- plain mono capture/playback thread
// pair for the generic-rig backend, targeting the classic Windows
// Multimedia API (winmm's waveIn/waveOut), not WASAPI. waveIn/waveOut was
// chosen deliberately over WASAPI or a third-party wrapper (PortAudio
// etc.): it's been stable and available unchanged since Windows 95, needs
// no extra runtime dependency, and -- same reasoning as sound_generic.c
// calling ALSA directly rather than through a portability shim -- matches
// this codebase's existing style of talking to the platform's own audio
// API directly. Confirmed to build identically for 32-bit (i686-w64-
// mingw32) and 64-bit (x86_64-w64-mingw32) targets; this file was written
// for a 32-bit target specifically (see the user's own ask), but nothing
// in it is bitness-specific -- int32_t is a fixed-width type regardless
// of build target, and winmm's API surface itself doesn't change with
// pointer width.
//
// UNBUILT, UNTESTED as of this writing -- no Windows toolchain was
// available in the session that wrote this (a Linux/ARM dev box). Written
// by close, careful mirroring of sound_generic.c's own structure and
// behavior (verified line-by-line against it), not by guessing, but it
// still needs a real MinGW build + real hardware test before trusting it
// the way sound_generic.c is trusted. See the porting sketch discussion
// this came out of for the wider picture (Makefile target, STATEDIR/
// SHAREDIR path handling, etc. -- all still open, not part of this file).
//
// Threading: pthread_create()/pthread_join() are used completely
// unchanged from sound_generic.c -- MinGW-w64's winpthreads library
// (linked automatically when building with -pthread under MinGW)
// implements real pthreads on top of native Win32 threads, so the
// capture/playback thread *structure* below needed no porting at all,
// only the ALSA calls inside each thread function.
//
// Buffering model difference from ALSA: snd_pcm_readi()/writei() are
// simple blocking calls -- read/write one buffer, block until it's
// ready. winmm has no equivalent single-call blocking API; its model is
// a small ring of WAVEHDR buffers submitted to the driver ahead of time
// (waveInAddBuffer()/waveOutWrite()), with completion signaled via an
// Event object (CALLBACK_EVENT) that the thread waits on. wavehdr_ring_t
// below wraps that ring so the actual capture/playback loops read almost
// the same as sound_generic.c's -- "wait, take the next completed
// buffer, do the same processing, requeue it" -- rather than restructuring
// the whole file around winmm's own queueing idiom.
//
// Deliberately duplicates autogain_update()/the spectrum FFT block
// (gen_spectrum_init/update) from sound_generic.c rather than sharing
// them via a new common file -- both are pure, platform-independent C
// (no ALSA calls in either), so duplicating them here is a real,
// acknowledged maintenance cost (two copies to keep in sync), chosen
// deliberately to avoid touching/risking the live, working Linux build
// while this port doesn't even have a compiler to verify it yet. Worth
// factoring both into a shared sound_generic_common.c once this file has
// actually been built and tested for real -- flagged here so it isn't
// forgotten, not because the duplication is a good long-term shape.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include <windows.h>
#include <mmsystem.h>
#include "sdr.h"
#include "sdr_ui.h"
#include "sound_generic.h"
#include "rig_generic.h"

// See sound_generic.c's own comments for the reasoning behind every one
// of these -- unchanged here, since none of it is ALSA-specific.
#define GENERIC_SAMPLE_RATE 96000
#define GENERIC_BUF_FRAMES  1024
#define GENERIC_PLAYBACK_RATE 48000
#define GENERIC_TX_SCALE_AT_MAX_DRIVE 14000000000.0f

static pthread_t capture_thread, playback_thread;
static volatile int running = 0;
static volatile int capture_open = 0;
static volatile int playback_open = 0;

static volatile float generic_rx_gain = 1.0f;

void sound_generic_set_rx_gain(float gain)
{
	generic_rx_gain = gain;
}

// --- autogain -- verbatim copy of sound_generic.c's own, see its
// comments there for the full reasoning. Only the definition site
// differs (this file vs. the ALSA one).
#define AUTOGAIN_CLIP_THRESHOLD ((float)INT32_MAX * 0.9f)
#define AUTOGAIN_CLIP_FRACTION 0.01f
#define AUTOGAIN_STEPDOWN_SECONDS 1.5f
#define AUTOGAIN_STEPUP_SECONDS 15.0f
#define AUTOGAIN_STEPDOWN_RATIO 0.708f
#define AUTOGAIN_STEPUP_RATIO 1.122f
#define AUTOGAIN_FLOOR 0.05f
#define AUTOGAIN_CEILING 1.0f

static float autogain_overload_seconds = 0.0f;
static float autogain_clean_seconds = 0.0f;

static void autogain_update(const int32_t *buf, int n_samples, float seconds_this_read)
{
	int clipped = 0;
	for (int i = 0; i < n_samples; i++) {
		int32_t s = buf[i];
		if (s > AUTOGAIN_CLIP_THRESHOLD || s < -AUTOGAIN_CLIP_THRESHOLD)
			clipped++;
	}
	int overloaded = clipped > (int)(n_samples * AUTOGAIN_CLIP_FRACTION);

	if (overloaded) {
		autogain_clean_seconds = 0.0f;
		autogain_overload_seconds += seconds_this_read;
		if (autogain_overload_seconds >= AUTOGAIN_STEPDOWN_SECONDS) {
			autogain_overload_seconds = 0.0f;
			float g = generic_rx_gain * AUTOGAIN_STEPDOWN_RATIO;
			if (g < AUTOGAIN_FLOOR)
				g = AUTOGAIN_FLOOR;
			generic_rx_gain = g;
			rig_generic_set_rf_gain((int)(g * 100.0f));
		}
	} else {
		autogain_overload_seconds = 0.0f;
		autogain_clean_seconds += seconds_this_read;
		if (autogain_clean_seconds >= AUTOGAIN_STEPUP_SECONDS) {
			autogain_clean_seconds = 0.0f;
			float g = generic_rx_gain * AUTOGAIN_STEPUP_RATIO;
			if (g > AUTOGAIN_CEILING)
				g = AUTOGAIN_CEILING;
			generic_rx_gain = g;
			rig_generic_set_rf_gain((int)(g * 100.0f));
		}
	}
}

// --- spectrum -- verbatim copy of sound_generic.c's own gen_spectrum_*,
// see its comments there (GENERIC_SPEC_FFT_SIZE's sizing rationale,
// GENERIC_SPEC_REFRESH_SAMPLES's CPU-throttling rationale, the left-
// edge-anchored/non-mirrored layout, the stall-heartbeat). cnrmf()/
// power2dB()/make_hann_window() are shared, platform-independent helpers
// declared in sdr.h -- not duplicated, called directly same as there.
#define GENERIC_SPEC_FFT_SIZE 16384
#define GENERIC_SPEC_N_BINS 512
#define GENERIC_SPEC_START_BIN ((3 * MAX_BINS) / 4)
#define GENERIC_SPEC_SMOOTHING 0.3f
#define GENERIC_SPEC_REFRESH_SAMPLES (GENERIC_SAMPLE_RATE / 10)

static fftw_complex *gen_fft_in = NULL, *gen_fft_out = NULL;
static fftw_plan gen_fft_plan;
static int32_t gen_spec_buf[GENERIC_SPEC_FFT_SIZE];
static float gen_spec_window[GENERIC_SPEC_FFT_SIZE];
static int gen_spectrum_ready = 0;

static void gen_spectrum_init(void)
{
	if (gen_spectrum_ready)
		return;
	gen_fft_in = (fftw_complex *)fftw_malloc(sizeof(fftw_complex) * GENERIC_SPEC_FFT_SIZE);
	gen_fft_out = (fftw_complex *)fftw_malloc(sizeof(fftw_complex) * GENERIC_SPEC_FFT_SIZE);
	memset(gen_fft_in, 0, sizeof(fftw_complex) * GENERIC_SPEC_FFT_SIZE);
	memset(gen_fft_out, 0, sizeof(fftw_complex) * GENERIC_SPEC_FFT_SIZE);
	memset(gen_spec_buf, 0, sizeof(gen_spec_buf));
	make_hann_window(gen_spec_window, GENERIC_SPEC_FFT_SIZE);
	gen_fft_plan = fftw_plan_dft_1d(GENERIC_SPEC_FFT_SIZE, gen_fft_in, gen_fft_out, FFTW_FORWARD, FFTW_ESTIMATE);
	gen_spectrum_ready = 1;
}

static void gen_spectrum_update(int32_t *samples, int n)
{
	static int samples_since_refresh = 0;

	if (n <= 0)
		return;
	if (n > GENERIC_SPEC_FFT_SIZE)
		n = GENERIC_SPEC_FFT_SIZE;

	memmove(gen_spec_buf, gen_spec_buf + n, (GENERIC_SPEC_FFT_SIZE - n) * sizeof(int32_t));
	memcpy(gen_spec_buf + (GENERIC_SPEC_FFT_SIZE - n), samples, n * sizeof(int32_t));

	samples_since_refresh += n;
	if (samples_since_refresh < GENERIC_SPEC_REFRESH_SAMPLES)
		return;
	samples_since_refresh = 0;

	static struct timespec last_refresh_ts = {0, 0};
	struct timespec now_ts;
	clock_gettime(CLOCK_MONOTONIC, &now_ts);
	if (last_refresh_ts.tv_sec != 0) {
		long gap_ms = (long)((now_ts.tv_sec - last_refresh_ts.tv_sec) * 1000
			+ (now_ts.tv_nsec - last_refresh_ts.tv_nsec) / 1000000);
		if (gap_ms > 300)
			fprintf(stderr, "sound_generic_win: spectrum refresh stalled, gap %ld ms (expected ~100ms)\n", gap_ms);
	}
	last_refresh_ts = now_ts;

	for (int i = 0; i < GENERIC_SPEC_FFT_SIZE; i++) {
		double v = gen_spec_window[i] * (gen_spec_buf[i] / 20000000.0);
		__real__ gen_fft_in[i] = v;
		__imag__ gen_fft_in[i] = 0;
	}

	fftw_execute(gen_fft_plan);

	for (int k = 0; k < GENERIC_SPEC_N_BINS; k++) {
		float mag = cabs(gen_fft_out[k]);
		float smoothed = ((1.0f - GENERIC_SPEC_SMOOTHING) * fft_bins[GENERIC_SPEC_START_BIN + k])
			+ (GENERIC_SPEC_SMOOTHING * mag);
		fft_bins[GENERIC_SPEC_START_BIN + k] = smoothed;
		int y = power2dB(cnrmf(smoothed));
		spectrum_plot[GENERIC_SPEC_START_BIN + k] = y;
	}
}

// --- winmm plumbing -----------------------------------------------

// Small ring of prepared WAVEHDR buffers submitted ahead of time, with
// completion signaled through a shared Event (CALLBACK_EVENT) -- see
// this file's own top comment for why this exists instead of a direct
// ALSA-style blocking read/write. N_RING_BUFFERS=4 mirrors the ALSA
// side's own "several periods of headroom against scheduling jitter"
// choice (buffer_size = period_size * 6 there) -- enough in-flight
// buffers that one being briefly slow to service doesn't starve the
// driver, without so many that latency grows needlessly.
#define N_RING_BUFFERS 4
#define RING_BUF_BYTES (GENERIC_BUF_FRAMES * sizeof(int32_t)) // mono, 32-bit samples

typedef struct {
	WAVEHDR hdr[N_RING_BUFFERS];
	int32_t data[N_RING_BUFFERS][GENERIC_BUF_FRAMES];
	HANDLE event;
} wavehdr_ring_t;

static void wfx_init(WAVEFORMATEX *wfx, unsigned int rate)
{
	memset(wfx, 0, sizeof(*wfx));
	wfx->wFormatTag = WAVE_FORMAT_PCM;
	wfx->nChannels = 1;
	wfx->nSamplesPerSec = rate;
	wfx->wBitsPerSample = 32;
	wfx->nBlockAlign = (wfx->nChannels * wfx->wBitsPerSample) / 8;
	wfx->nAvgBytesPerSec = wfx->nSamplesPerSec * wfx->nBlockAlign;
	wfx->cbSize = 0;
}

// Device selection: sound_generic's own generic_capture_device/
// generic_playback_device (sdr.h) are plain strings, same as ALSA's own
// "hw:1,0"-style device names -- but winmm identifies devices by integer
// ID (waveInGetNumDevs()/waveInGetDevCaps()), not by name. Matched here
// by substring against each device's own szPname (case-insensitive, a
// real device name like "USB Audio CODEC" rarely needs an exact match to
// be unambiguous) so the same free-text device field in the web UI keeps
// working unchanged rather than needing a Windows-specific numeric-index
// UI. Empty string, or no match found, falls back to WAVE_MAPPER --
// Windows' own "just pick the current default device" sentinel, the
// direct equivalent of ALSA's plain "default".
static UINT find_input_device(const char *name)
{
	if (!name || !name[0])
		return WAVE_MAPPER;
	UINT n = waveInGetNumDevs();
	for (UINT i = 0; i < n; i++) {
		WAVEINCAPS caps;
		if (waveInGetDevCaps(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
			continue;
		if (strstr(caps.szPname, name))
			return i;
	}
	return WAVE_MAPPER;
}

static UINT find_output_device(const char *name)
{
	if (!name || !name[0])
		return WAVE_MAPPER;
	UINT n = waveOutGetNumDevs();
	for (UINT i = 0; i < n; i++) {
		WAVEOUTCAPS caps;
		if (waveOutGetDevCaps(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
			continue;
		if (strstr(caps.szPname, name))
			return i;
	}
	return WAVE_MAPPER;
}

static void log_mmresult(const char *what, MMRESULT r)
{
	char msg[256];
	waveInGetErrorTextA(r, msg, sizeof(msg)); // same text table for waveIn/waveOut errors
	fprintf(stderr, "sound_generic_win: %s failed: %s (%u)\n", what, msg, (unsigned)r);
}

static void *capture_thread_fn(void *arg)
{
	(void)arg;

	while (running) {
		WAVEFORMATEX wfx;
		wfx_init(&wfx, GENERIC_SAMPLE_RATE);
		UINT dev = find_input_device(generic_capture_device);

		wavehdr_ring_t ring;
		memset(&ring, 0, sizeof(ring));
		ring.event = CreateEvent(NULL, FALSE, FALSE, NULL);

		HWAVEIN hwi;
		MMRESULT mr = waveInOpen(&hwi, dev, &wfx, (DWORD_PTR)ring.event, 0, CALLBACK_EVENT);
		if (mr != MMSYSERR_NOERROR) {
			log_mmresult("waveInOpen", mr);
			CloseHandle(ring.event);
			capture_open = 0;
			Sleep(2000);
			continue;
		}

		// Prepare and queue every ring buffer up front, same as ALSA's
		// own multi-period buffer_size giving headroom before the loop
		// even starts.
		for (int i = 0; i < N_RING_BUFFERS; i++) {
			ring.hdr[i].lpData = (LPSTR)ring.data[i];
			ring.hdr[i].dwBufferLength = RING_BUF_BYTES;
			waveInPrepareHeader(hwi, &ring.hdr[i], sizeof(WAVEHDR));
			waveInAddBuffer(hwi, &ring.hdr[i], sizeof(WAVEHDR));
		}
		waveInStart(hwi);
		capture_open = 1;

		int fatal = 0;
		while (running && !fatal) {
			WaitForSingleObject(ring.event, 500); // 500ms: just a periodic
				// wake to re-check `running` for a clean stop -- real
				// completions signal the event directly, this is not a
				// polling interval for normal operation.

			for (int i = 0; i < N_RING_BUFFERS; i++) {
				if (!(ring.hdr[i].dwFlags & WHDR_DONE))
					continue;

				int32_t *buf = (int32_t *)ring.hdr[i].lpData;
				int n = (int)(ring.hdr[i].dwBytesRecorded / sizeof(int32_t));

				// Same processing sequence as sound_generic.c's capture
				// loop, unchanged: judge real front-end level before our
				// own gain, apply gain, feed the modem, update the
				// spectrum, feed any active WAV recording.
				autogain_update(buf, n, (float)n / GENERIC_SAMPLE_RATE);
				if (generic_rx_gain != 1.0f) {
					float gain = generic_rx_gain;
					for (int j = 0; j < n; j++) {
						float scaled = buf[j] * gain;
						if (scaled > (float)INT32_MAX)
							scaled = (float)INT32_MAX;
						else if (scaled < (float)INT32_MIN)
							scaled = (float)INT32_MIN;
						buf[j] = (int32_t)scaled;
					}
				}
				modem_rx(rx_list->mode, buf, n);
				gen_spectrum_update(buf, n);
				wav_record(buf, n);

				// Requeue this buffer for the driver to fill again.
				waveInUnprepareHeader(hwi, &ring.hdr[i], sizeof(WAVEHDR));
				ring.hdr[i].dwFlags = 0;
				mr = waveInPrepareHeader(hwi, &ring.hdr[i], sizeof(WAVEHDR));
				if (mr == MMSYSERR_NOERROR)
					mr = waveInAddBuffer(hwi, &ring.hdr[i], sizeof(WAVEHDR));
				if (mr != MMSYSERR_NOERROR) {
					// Same "reopen from scratch rather than retry a dead
					// handle forever" reasoning as the ALSA recover()-
					// failed path.
					log_mmresult("waveInAddBuffer (requeue)", mr);
					fatal = 1;
					break;
				}
			}
		}

		capture_open = 0;
		waveInStop(hwi);
		waveInReset(hwi);
		for (int i = 0; i < N_RING_BUFFERS; i++)
			waveInUnprepareHeader(hwi, &ring.hdr[i], sizeof(WAVEHDR));
		waveInClose(hwi);
		CloseHandle(ring.event);
	}

	capture_open = 0;
	return NULL;
}

static void *playback_thread_fn(void *arg)
{
	(void)arg;

	static double tune_phase = 0;

	while (running) {
		WAVEFORMATEX wfx;
		wfx_init(&wfx, GENERIC_PLAYBACK_RATE);
		UINT dev = find_output_device(generic_playback_device);

		wavehdr_ring_t ring;
		memset(&ring, 0, sizeof(ring));
		ring.event = CreateEvent(NULL, FALSE, FALSE, NULL);

		HWAVEOUT hwo;
		MMRESULT mr = waveOutOpen(&hwo, dev, &wfx, (DWORD_PTR)ring.event, 0, CALLBACK_EVENT);
		if (mr != MMSYSERR_NOERROR) {
			log_mmresult("waveOutOpen", mr);
			CloseHandle(ring.event);
			playback_open = 0;
			Sleep(2000);
			continue;
		}
		playback_open = 1;

		// Prime every ring buffer with real audio before the first
		// Write() -- unlike capture, there's no "wait for the driver to
		// hand us a completed buffer" starting point, we have to feed it
		// first.
		int fatal = 0;
		for (int i = 0; i < N_RING_BUFFERS && !fatal; i++) {
			float drive_scale = (field_int("DRIVE") / 100.0f) * GENERIC_TX_SCALE_AT_MAX_DRIVE;
			int is_tune = (tx_list->mode == MODE_TUNE);
			double tune_freq = is_tune ? (double)field_int("TX_PITCH") : 0;

			for (int j = 0; j < GENERIC_BUF_FRAMES; j++) {
				float sample;
				if (is_tune) {
					sample = (float)(0.143 * sin(tune_phase));
					tune_phase += 2.0 * M_PI * tune_freq / GENERIC_PLAYBACK_RATE;
					if (tune_phase > 2.0 * M_PI)
						tune_phase -= 2.0 * M_PI;
				} else {
					sample = modem_next_sample(tx_list->mode);
					modem_next_sample(tx_list->mode); // discarded half of the pair, same 2:1 decimation as ALSA side
				}
				ring.data[i][j] = (int32_t)(sample * drive_scale);
			}

			ring.hdr[i].lpData = (LPSTR)ring.data[i];
			ring.hdr[i].dwBufferLength = RING_BUF_BYTES;
			waveOutPrepareHeader(hwo, &ring.hdr[i], sizeof(WAVEHDR));
			mr = waveOutWrite(hwo, &ring.hdr[i], sizeof(WAVEHDR));
			if (mr != MMSYSERR_NOERROR) {
				log_mmresult("waveOutWrite (priming)", mr);
				fatal = 1;
			}
		}

		while (running && !fatal) {
			WaitForSingleObject(ring.event, 500); // same periodic-wake reasoning as the capture side

			for (int i = 0; i < N_RING_BUFFERS; i++) {
				if (!(ring.hdr[i].dwFlags & WHDR_DONE))
					continue;

				float drive_scale = (field_int("DRIVE") / 100.0f) * GENERIC_TX_SCALE_AT_MAX_DRIVE;
				int is_tune = (tx_list->mode == MODE_TUNE);
				double tune_freq = is_tune ? (double)field_int("TX_PITCH") : 0;

				for (int j = 0; j < GENERIC_BUF_FRAMES; j++) {
					float sample;
					if (is_tune) {
						sample = (float)(0.143 * sin(tune_phase));
						tune_phase += 2.0 * M_PI * tune_freq / GENERIC_PLAYBACK_RATE;
						if (tune_phase > 2.0 * M_PI)
							tune_phase -= 2.0 * M_PI;
					} else {
						sample = modem_next_sample(tx_list->mode);
						modem_next_sample(tx_list->mode);
					}
					ring.data[i][j] = (int32_t)(sample * drive_scale);
				}

				waveOutUnprepareHeader(hwo, &ring.hdr[i], sizeof(WAVEHDR));
				ring.hdr[i].dwFlags = 0;
				mr = waveOutPrepareHeader(hwo, &ring.hdr[i], sizeof(WAVEHDR));
				if (mr == MMSYSERR_NOERROR)
					mr = waveOutWrite(hwo, &ring.hdr[i], sizeof(WAVEHDR));
				if (mr != MMSYSERR_NOERROR) {
					// Same "don't just drop this chunk of TX audio"
					// concern as the ALSA side's writei() retry -- but
					// unlike there, there's no second synchronous attempt
					// available in this buffer-queue model; reopening
					// (outer while(running) loop) is the equivalent
					// recovery path instead.
					log_mmresult("waveOutWrite (requeue)", mr);
					fatal = 1;
					break;
				}
			}
		}

		playback_open = 0;
		waveOutReset(hwo);
		for (int i = 0; i < N_RING_BUFFERS; i++)
			waveOutUnprepareHeader(hwo, &ring.hdr[i], sizeof(WAVEHDR));
		waveOutClose(hwo);
		CloseHandle(ring.event);
	}

	playback_open = 0;
	return NULL;
}

void sound_generic_start(void)
{
	gen_spectrum_init();
	running = 1;
	pthread_create(&capture_thread, NULL, capture_thread_fn, NULL);
	pthread_create(&playback_thread, NULL, playback_thread_fn, NULL);
}

void sound_generic_stop(void)
{
	running = 0;
	pthread_join(capture_thread, NULL);
	pthread_join(playback_thread, NULL);
}

int sound_generic_capture_connected(void)
{
	return capture_open;
}

int sound_generic_playback_connected(void)
{
	return playback_open;
}

void sound_generic_restart(void)
{
	sound_generic_stop();
	sound_generic_start();
}
