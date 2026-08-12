// Plain mono ALSA capture/playback thread pair for the generic-rig backend.
// RX side feeds straight into modem_rx() -- the same mode-agnostic dispatcher
// (modems.c) that the SDR's own I/Q-demodulated audio already feeds -- so
// ft8_rx()/cw_rx() need no changes at all. TX side is the mirror image via
// modem_next_sample(), which normally gets pulled into the SDR's upconversion
// loop (sbitx.c); here it goes straight to the playback device instead.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include <alsa/asoundlib.h>
#include "sdr.h"
#include "sdr_ui.h"
#include "sound_generic.h"

// modem_next_sample()/ft8_next_sample() are tuned for 96ksps -- see the
// comment on modem_next_sample() in modems.c ("the ft8 samples are
// generated at 12ksps, we need to feed the sdr with 96 ksps"). Used for
// capture, which is confirmed working well via real decode on the air.
#define GENERIC_SAMPLE_RATE 96000
// Must be a multiple of 1024 (decimation_factor(8) * n_bins(128)) --
// modem_cw.c's cw_rx() hard-asserts if the sample count it's called with
// isn't. The SDR's own native capture path happens to call modem_rx() with
// exactly MAX_BINS/2 (1024) samples, satisfying this by construction; this
// backend has no such built-in alignment, and used to crash the whole
// daemon (assert(0), taking down every connected client) the moment a
// user switched to a band whose remembered mode was CW/CWR -- confirmed by
// reproducing it directly. 1024 (~10.67ms at 96ksps) is the smallest valid
// choice.
#define GENERIC_BUF_FRAMES  1024

// The QMX's USB audio hardware only natively supports 48000 Hz (confirmed
// via `aplay -D hw:qmxaudio,0 --dump-hw-params`), not 96000. Requesting
// 96000 on the playback side let ALSA's plughw layer silently resample
// underneath, which can shift ft8_next_sample()'s synthesized tone
// frequencies (calibrated for exactly 96ksps) enough to land outside the
// radio's audio passband -- a plausible cause of "keys up but weak"
// independent of digital sample level. Playback opens at the true native
// rate instead, and pulls two modem_next_sample() calls per output frame,
// keeping one (straightforward 2:1 decimation) -- this halves the sample
// count while preserving correct real-time pitch, rather than leaving an
// unverified automatic resample in the loop.
#define GENERIC_PLAYBACK_RATE 48000

// ceiling for the existing DRIVE control (0-100, field label "DRIVE",
// cmd "tx_power") to scale against, at DRIVE=100. ft8_next_sample()'s
// peak output is ~0.143 (a unit sinf() tone / 7 -- see synth_gfsk() and
// ft8_next_sample() in modem_ft8.c). Per the user (confirmed via prior
// WSJT-X use of the same radio): unlike a typical SSB rig, the QMX's
// modulation scheme specifically wants close to 100% digital audio
// level, both OS mixer and application level -- moderate headroom
// (previously tried at 25%, then 75%, both still reported "low") isn't
// the right target here. Pushed to ~93% of S32 full scale at DRIVE=100
// (14000000000 * 0.143 =~ 2.00e9, vs INT32_MAX ~2.147e9 -- small margin
// kept to avoid actual overflow/wraparound, which would be far worse
// than merely quiet). Tune DRIVE from the web UI to taste; no rebuild
// needed for that.
#define GENERIC_TX_SCALE_AT_MAX_DRIVE 14000000000.0f

static pthread_t capture_thread, playback_thread;
static volatile int running = 0;
static volatile int capture_open = 0;
static volatile int playback_open = 0;

// Waterfall/spectrum for the generic-rig backend. The SDR's own spectrum
// pipeline (sbitx.c's rx_linear()/sound_process()) does FFT-based SSB
// extraction from a real IF-sampled ADC signal, with bin indexing
// calibrated to that specific IF scheme -- not applicable here, since a
// generic rig hands over audio that's already been demodulated to baseband
// by the radio itself. This is a separate, simpler real-audio spectrum:
// FFT the captured audio directly and write magnitude-in-dB into the same
// spectrum_plot[]/fft_bins[] globals web_get_spectrum() already reads, so
// no client-side (index.html) changes are needed.
//
// web_get_spectrum() reads spectrum_plot[] centered on bin (3*MAX_BINS)/4
// (=1536 at MAX_BINS=2048) as the 0 Hz/dial-frequency reference, with
// increasing bin index = increasing audio frequency. A real (not I/Q)
// audio signal's magnitude spectrum is inherently symmetric around 0 Hz --
// there's no way to recover which sideband a signal came from once it's
// already been demodulated to plain audio -- so both sides of bin 1536 are
// written with the same value; this only looks "mirrored" compared to the
// SDR's own asymmetric panorama because that's genuinely all the
// information a mono audio stream carries.
static fftw_complex *gen_fft_in = NULL, *gen_fft_out = NULL;
static fftw_plan gen_fft_plan;
static int32_t gen_spec_buf[MAX_BINS];
static int gen_spectrum_ready = 0;

// how far from center (bin 1536) to compute/write -- capped by having only
// MAX_BINS/4 (512) bins of headroom above 1536 before running off the end
// of spectrum_plot[]/fft_bins[] (both sized MAX_BINS=2048)
#define GENERIC_SPEC_HALF_BINS (MAX_BINS / 4 - 1)
#define GENERIC_SPEC_CENTER_BIN ((3 * MAX_BINS) / 4)
#define GENERIC_SPEC_SMOOTHING 0.3f

static void gen_spectrum_init(void)
{
	if (gen_spectrum_ready)
		return;
	gen_fft_in = (fftw_complex *)fftw_malloc(sizeof(fftw_complex) * MAX_BINS);
	gen_fft_out = (fftw_complex *)fftw_malloc(sizeof(fftw_complex) * MAX_BINS);
	memset(gen_fft_in, 0, sizeof(fftw_complex) * MAX_BINS);
	memset(gen_fft_out, 0, sizeof(fftw_complex) * MAX_BINS);
	memset(gen_spec_buf, 0, sizeof(gen_spec_buf));
	// FFTW_ESTIMATE, not the wisdom-measured plan sbitx.c's own FFT uses --
	// this only needs to be fast to create (built fresh on every daemon
	// start), not the fastest possible transform; a 2048-point FFT is
	// cheap enough either way on this hardware
	gen_fft_plan = fftw_plan_dft_1d(MAX_BINS, gen_fft_in, gen_fft_out, FFTW_FORWARD, FFTW_ESTIMATE);
	gen_spectrum_ready = 1;
}

// called once per capture read (n <= MAX_BINS, GENERIC_BUF_FRAMES=960 well
// under that) -- slides the new real samples into a MAX_BINS-long window,
// windows+transforms it, and writes the result into spectrum_plot[]
static void gen_spectrum_update(int32_t *samples, int n)
{
	if (n <= 0)
		return;
	if (n > MAX_BINS)
		n = MAX_BINS;

	memmove(gen_spec_buf, gen_spec_buf + n, (MAX_BINS - n) * sizeof(int32_t));
	memcpy(gen_spec_buf + (MAX_BINS - n), samples, n * sizeof(int32_t));

	// same raw-sample-to-float divisor sbitx.c's own rx_linear() uses
	// (input_rx[j] / 20000000.0) for its FFT input, kept here for a
	// consistent dB scale/threshold with the SDR's own display
	for (int i = 0; i < MAX_BINS; i++) {
		double v = spectrum_window[i] * (gen_spec_buf[i] / 20000000.0);
		__real__ gen_fft_in[i] = v;
		__imag__ gen_fft_in[i] = 0;
	}

	fftw_execute(gen_fft_plan);

	for (int k = 0; k <= GENERIC_SPEC_HALF_BINS; k++) {
		float mag = cabs(gen_fft_out[k]);
		float smoothed = ((1.0f - GENERIC_SPEC_SMOOTHING) * fft_bins[GENERIC_SPEC_CENTER_BIN + k])
			+ (GENERIC_SPEC_SMOOTHING * mag);
		fft_bins[GENERIC_SPEC_CENTER_BIN + k] = smoothed;
		fft_bins[GENERIC_SPEC_CENTER_BIN - k] = smoothed;
		int y = power2dB(cnrmf(smoothed));
		spectrum_plot[GENERIC_SPEC_CENTER_BIN + k] = y;
		spectrum_plot[GENERIC_SPEC_CENTER_BIN - k] = y;
	}
}

static snd_pcm_t *open_pcm(const char *device, snd_pcm_stream_t stream, unsigned int rate)
{
	snd_pcm_t *handle;
	snd_pcm_hw_params_t *hw;
	snd_pcm_uframes_t period_size = GENERIC_BUF_FRAMES;
	int err;

	// on a stop/start restart (AUDIO_CONNECT switching devices live), the
	// just-closed handle's USB-audio-class device can briefly still show
	// as busy at the kernel level even after snd_pcm_close() returns --
	// confirmed by reproducing "Device or resource busy" immediately
	// after a restart, then succeeding on a bare retry a few seconds
	// later. A few short retries absorbs that race without making a
	// genuinely wrong/missing device silently hang.
	for (int attempt = 0; attempt < 10; attempt++) {
		err = snd_pcm_open(&handle, device, stream, 0);
		if (err != -EBUSY)
			break;
		usleep(200000);
	}
	if (err < 0) {
		fprintf(stderr, "sound_generic: cannot open %s (%s): %s\n",
			device, stream == SND_PCM_STREAM_CAPTURE ? "capture" : "playback",
			snd_strerror(err));
		return NULL;
	}

	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(handle, hw);
	snd_pcm_hw_params_set_access(handle, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(handle, hw, SND_PCM_FORMAT_S32_LE);
	snd_pcm_hw_params_set_channels(handle, hw, 1);
	snd_pcm_hw_params_set_rate_near(handle, hw, &rate, 0);
	snd_pcm_hw_params_set_period_size_near(handle, hw, &period_size, 0);
	// several periods of headroom against scheduling jitter (this thread
	// runs at normal pthread priority, not SCHED_FIFO) -- an underrun
	// mid-transmission would produce exactly the "brief burst, then
	// nothing" symptom reported on real hardware
	snd_pcm_uframes_t buffer_size = period_size * 6;
	snd_pcm_hw_params_set_buffer_size_near(handle, hw, &buffer_size);

	err = snd_pcm_hw_params(handle, hw);
	if (err < 0) {
		fprintf(stderr, "sound_generic: hw_params failed on %s: %s\n",
			device, snd_strerror(err));
		snd_pcm_close(handle);
		return NULL;
	}

	fprintf(stderr, "sound_generic: %s (%s) opened at %u Hz (requested %u)\n",
		device, stream == SND_PCM_STREAM_CAPTURE ? "capture" : "playback", rate,
		stream == SND_PCM_STREAM_CAPTURE ? GENERIC_SAMPLE_RATE : GENERIC_PLAYBACK_RATE);

	snd_pcm_prepare(handle);
	return handle;
}

static void *capture_thread_fn(void *arg)
{
	int32_t buf[GENERIC_BUF_FRAMES];

	(void)arg;

	// Used to give up and exit the whole thread the moment open_pcm()
	// failed once (e.g. a bad CAPTUREDEV string from AUDIO_CONNECT, or
	// the rig's USB audio dropping out) -- that permanently killed the
	// waterfall/decode with no recovery short of a service restart,
	// confirmed live (AUDIO_CONNECT sent "default", which doesn't
	// resolve on this Pi -- "cannot open default (capture)" -- and
	// decoding never came back). Loop and retry instead, since for
	// portable field use a cable can come loose and get reseated later.
	while (running) {
		snd_pcm_t *pcm = open_pcm(generic_capture_device, SND_PCM_STREAM_CAPTURE, GENERIC_SAMPLE_RATE);
		if (!pcm) {
			capture_open = 0;
			sleep(2);
			continue;
		}
		capture_open = 1;

		while (running) {
			snd_pcm_sframes_t n = snd_pcm_readi(pcm, buf, GENERIC_BUF_FRAMES);
			if (n < 0) {
				int err = snd_pcm_recover(pcm, (int)n, 0);
				if (err < 0) {
					// recover() itself failed -- e.g. the USB audio
					// interface dropped off entirely -- not just an
					// xrun snd_pcm_recover() can paper over. Reopen
					// from scratch rather than retrying reads on a
					// dead handle forever.
					fprintf(stderr, "sound_generic: capture error: %s (recover failed: %s), reopening\n",
						snd_strerror((int)n), snd_strerror(err));
					break;
				}
				continue;
			}
			modem_rx(rx_list->mode, buf, (int)n);
			gen_spectrum_update(buf, (int)n);
		}

		capture_open = 0;
		snd_pcm_close(pcm);
	}

	capture_open = 0;
	return NULL;
}

static void *playback_thread_fn(void *arg)
{
	int32_t buf[GENERIC_BUF_FRAMES];

	(void)arg;

	static double tune_phase = 0;

	// See capture_thread_fn()'s matching comment -- open_pcm() failing
	// once (bad PLAYBACKDEV, USB audio dropout) used to exit this thread
	// for good, silently taking TX audio out with it until a service
	// restart.
	while (running) {
		snd_pcm_t *pcm = open_pcm(generic_playback_device, SND_PCM_STREAM_PLAYBACK, GENERIC_PLAYBACK_RATE);
		if (!pcm) {
			playback_open = 0;
			sleep(2);
			continue;
		}
		playback_open = 1;

		int fatal = 0;
		while (running && !fatal) {
			// read once per buffer, not per sample -- DRIVE only needs to
			// track UI changes at human speed, not audio-sample speed
			float drive_scale = (field_int("DRIVE") / 100.0f) * GENERIC_TX_SCALE_AT_MAX_DRIVE;
			int is_tune = (tx_list->mode == MODE_TUNE);
			double tune_freq = is_tune ? (double)field_int("TX_PITCH") : 0;

			for (int i = 0; i < GENERIC_BUF_FRAMES; i++) {
				float sample;
				if (is_tune) {
					// modem_next_sample() only knows FT8/FT4/CW -- MODE_TUNE
					// falls through to silence there (that's what a real QMX
					// keying with "no audio" symptom traced back to). The
					// SDR's own TUNE tone lives entirely inside sbitx.c's
					// upconversion loop, which this backend bypasses, so
					// generate a plain steady tone directly here instead.
					sample = (float)(0.143 * sin(tune_phase));
					tune_phase += 2.0 * M_PI * tune_freq / GENERIC_PLAYBACK_RATE;
					if (tune_phase > 2.0 * M_PI)
						tune_phase -= 2.0 * M_PI;
				} else {
					// modem_next_sample() is calibrated for 96ksps; this
					// device only runs at 48ksps, so pull two logical
					// samples per real output frame and keep one -- 2:1
					// decimation, matching real elapsed time (and so tone
					// pitch) rather than an unverified automatic resample.
					// Runs continuously; modem_next_sample() returns 0
					// (silence) on its own whenever nothing is queued to
					// transmit, so this doesn't need to gate on in_tx.
					sample = modem_next_sample(tx_list->mode);
					modem_next_sample(tx_list->mode); // discarded half of the pair
				}
				buf[i] = (int32_t)(sample * drive_scale);
			}
			snd_pcm_sframes_t n = snd_pcm_writei(pcm, buf, GENERIC_BUF_FRAMES);
			if (n < 0) {
				fprintf(stderr, "sound_generic: playback xrun/error (%s), recovering\n", snd_strerror((int)n));
				int err = snd_pcm_recover(pcm, (int)n, 0);
				if (err < 0) {
					fprintf(stderr, "sound_generic: playback recover failed: %s, reopening\n",
						snd_strerror(err));
					fatal = 1;
					break;
				}
				// don't just drop this chunk of TX audio -- a string of
				// dropped chunks mid-transmission is exactly what would
				// produce "brief burst of audio, then nothing"
				snd_pcm_writei(pcm, buf, GENERIC_BUF_FRAMES);
			}
		}

		playback_open = 0;
		snd_pcm_close(pcm);
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
