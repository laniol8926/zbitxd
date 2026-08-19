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
#include <time.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include <alsa/asoundlib.h>
#include "sdr.h"
#include "sdr_ui.h"
#include "sound_generic.h"
#include "rig_generic.h"

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

// See sound_generic.h -- software RX gain, since most rigs on this
// backend have no CAT-controllable gain at all. A plain float write/read
// from a single capture thread and the main thread's field-change
// handler is safe without a lock here (same reasoning as rx_list->mode
// itself, read every capture loop iteration with no synchronization
// elsewhere in this file); a torn read at worst applies briefly-stale
// gain, never garbage.
static volatile float generic_rx_gain = 1.0f;

void sound_generic_set_rx_gain(float gain)
{
	generic_rx_gain = gain;
}

// Auto RX gain -- replaces the old manual RF slider entirely (user's own
// call: a strong nearby station overdriving its own amplifier and
// splattering across the band is a generic problem, not something the
// operator should have to notice and react to by hand every time). Runs
// entirely inside capture_thread_fn() below, evaluated on the *raw*
// samples straight off snd_pcm_readi() -- before generic_rx_gain is
// applied -- so it's judging the real front-end level, not its own past
// adjustments.
//
// Ceiling is unity (1.0): software gain above that doesn't add real
// information, just rescales already-quantized samples, so 1.0 already
// is "maximum sensitivity, nothing held back". Floor is a sanity clamp,
// not expected to be hit in normal operation.
//
// Stepped down fast on sustained overload (a couple of seconds, not one
// isolated burst -- a single loud transmission shouldn't yank gain
// around) and back up slowly once the band's clean again (tens of
// seconds per step), so it can't oscillate and doesn't snap straight
// back to full gain the instant an overloading station's transmission
// ends. Same gain value also drives rig_generic_set_rf_gain() (real
// hardware gain on a QMX, silent no-op on everything else that has no
// CAT path to one) -- see the "r1:gain" handler in sbitx.c, which no
// longer takes manual input for generic_rig_mode now that this owns it.
#define AUTOGAIN_CLIP_THRESHOLD ((float)INT32_MAX * 0.9f)
#define AUTOGAIN_CLIP_FRACTION 0.01f  // >1% of a read's samples near full-scale counts that read as overloaded
#define AUTOGAIN_STEPDOWN_SECONDS 1.5f
#define AUTOGAIN_STEPUP_SECONDS 15.0f
#define AUTOGAIN_STEPDOWN_RATIO 0.708f  // -3dB
#define AUTOGAIN_STEPUP_RATIO 1.122f    // +1dB
#define AUTOGAIN_FLOOR 0.05f            // -26dB, a sanity clamp against runaway, not a normal operating point
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
// A real FT8/FT4 signal is only ~50Hz wide. The original MAX_BINS(2048)-
// point FFT at 96ksps gives 46.875 Hz/bin -- barely one bin per signal even
// before considering that the underlying analysis WINDOW is only
// 2048/96000 = ~21ms long, which is fundamentally too short to resolve
// anything anywhere near 50Hz-narrow in the first place (frequency
// resolution is bounded by window length, not just bin count/spacing).
// Confirmed live: user reported "lucky to see 3 or 4" distinct signal
// traces where ~50-60 should fit across the 3kHz passband.
//
// Deliberately a SEPARATE FFT size from MAX_BINS (2048, still used by
// sbitx.c's now-mostly-vestigial old-SDR FFT machinery, and by the
// fft_bins[]/spectrum_plot[] OUTPUT arrays this writes into) rather than
// just bumping MAX_BINS itself -- that would also 8x every one of those
// unrelated legacy buffers/FFTW_MEASURE plans for no benefit. Only the
// INPUT analysis needs to get bigger; the OUTPUT still only ever needs to
// hold as many bins as the locked 3kHz span actually displays (see
// GENERIC_SPEC_N_BINS below), which comfortably fits within the
// existing, unchanged output array size.
//
// 96000/16384 = 5.859375 Hz/bin (~8.5 bins per 50Hz signal -- plenty of
// separation), and a ~170ms analysis window, a natural fit given FT8's
// own ~160ms symbol length.
#define GENERIC_SPEC_FFT_SIZE 16384

static fftw_complex *gen_fft_in = NULL, *gen_fft_out = NULL;
static fftw_plan gen_fft_plan;
static int32_t gen_spec_buf[GENERIC_SPEC_FFT_SIZE];
static float gen_spec_window[GENERIC_SPEC_FFT_SIZE];
static int gen_spectrum_ready = 0;

// This backend only ever handles USB-based digital modes (FT8/FT4) on a
// rig that's already demodulated the RF down to plain baseband audio
// before it ever reaches here. USB only contains audio content for RF
// *above* the dial frequency (0-3000Hz audio = dial to dial+3000Hz RF)
// -- there is no real "negative audio frequency" signal to show, so the
// dial frequency belongs at the LEFT edge of the display, not centered
// with content mirrored on both sides.
//
// The previous centered/mirrored layout (write the same magnitude to
// both CENTER_BIN+k and CENTER_BIN-k) was inherited from the original
// zBitx SDR hardware's own spectrum convention without being reconsidered
// for whether it's still correct here -- that hardware's own comment
// ("the center frequency is at the center of the LOWER SIDEBAND") makes
// sense for its own single-conversion IF/I-Q scheme, which genuinely
// carries information on both sides of a reference point. A generic
// rig's plain mono, already-USB-demodulated audio doesn't have that;
// mirroring it doesn't recover any real second sideband, it just
// duplicates the one real (0-3000Hz) passband onto both halves of the
// display, which is a real FFT's own true (but not meaningful here)
// conjugate symmetry, not additional information. Caught live by direct
// user correction: "14.074 is not the center frequency... we're in USB
// mode it is the left edge of the waterfall."
//
// GENERIC_SPEC_N_BINS is the full 0-3000Hz span at this resolution
// (3000 Hz / 5.859375 Hz-per-bin = 512 exactly) written sequentially
// starting at GENERIC_SPEC_START_BIN, in increasing-frequency order --
// no mirroring, no center point. Assumes spectrum_span stays locked at
// 3000 (see the SPAN removal/lock elsewhere in this project) -- if that
// lock is ever removed, this needs to become a runtime calculation
// against the real spectrum_span instead of a compile-time constant.
// GENERIC_SPEC_START_BIN keeps the same offset the old centered layout
// used (bin 1536 of the MAX_BINS=2048 output arrays) purely because it's
// already a known-safe, already-verified-in-bounds choice -- 1536 to
// 1536+512-1=2047 fits the array exactly, with zero remaining meaning
// behind "1536" itself now that there's no more center concept.
#define GENERIC_SPEC_N_BINS 512
#define GENERIC_SPEC_START_BIN ((3 * MAX_BINS) / 4)
#define GENERIC_SPEC_SMOOTHING 0.3f

// gen_spectrum_update() is called once per capture read (GENERIC_BUF_FRAMES
// = 1024 samples, ~10.67ms at 96kHz) to keep the sliding window current,
// but actually re-running the 16384-point FFT that often is unnecessary
// and expensive -- found live, not guessed: one capture thread pegged at
// ~87% CPU sustained (confirmed via `sudo gdb -p <tid> -batch -ex bt`,
// which landed inside libfftw3), driving the whole Pi's load average
// over 9 on a quad-core board and causing real, reported waterfall
// stalls. GENERIC_SPEC_REFRESH_SAMPLES throttles the actual FFT+bin-write
// work to roughly this many new samples between runs (100ms's worth here)
// -- comfortably faster than anything the display could show a
// difference for (even the fastest current waterfall scroll rate adds a
// new row much slower than that), while cutting FFT calls/sec by roughly
// the same ~9.4x this window is longer than one capture read.
#define GENERIC_SPEC_REFRESH_SAMPLES (GENERIC_SAMPLE_RATE / 10)

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
	// FFTW_ESTIMATE, not the wisdom-measured plan sbitx.c's own FFT uses --
	// this only needs to be fast to create (built fresh on every daemon
	// start), not the fastest possible transform.
	gen_fft_plan = fftw_plan_dft_1d(GENERIC_SPEC_FFT_SIZE, gen_fft_in, gen_fft_out, FFTW_FORWARD, FFTW_ESTIMATE);
	gen_spectrum_ready = 1;
}

// called once per capture read (n <= GENERIC_SPEC_FFT_SIZE,
// GENERIC_BUF_FRAMES=1024 well under that) -- slides the new real samples
// into a GENERIC_SPEC_FFT_SIZE-long window (takes 16 calls, ~170ms, to
// fully cycle -- a deliberately long analysis window, see
// GENERIC_SPEC_FFT_SIZE's comment) on every call, but only actually
// re-runs the FFT/writes spectrum_plot[] once GENERIC_SPEC_REFRESH_SAMPLES
// new samples have accumulated (see its own comment for why) -- the
// window itself still slides on every call so the next refresh always
// analyzes the most current audio, only the expensive transform is
// throttled.
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

	// Stall heartbeat -- real report: waterfall showed a band of static,
	// unchanging vertical streaking (not real signal texture -- no
	// row-to-row wobble at all) during the single busiest SIC/AP decode
	// slot of a whole test session (12 decodes vs. the usual 4-8),
	// coinciding with heavy CPU load, with no TX active (ruled out via
	// journalctl -- not the known TX-freeze behavior). Working theory,
	// not yet confirmed: the capture thread (this function) got starved
	// of CPU long enough that spectrum_plot[] stopped being refreshed
	// for a stretch, so the client's independent scroll timer just kept
	// redrawing the same stale last_wf_row -- frozen data looks exactly
	// like straight, unchanging streaks once painted repeatedly. This
	// refresh is throttled to land roughly every GENERIC_SPEC_REFRESH_
	// SAMPLES (~100ms) apart; logging only the rare cases that land far
	// outside that (not every refresh, which would flood the log at
	// ~10/sec) turns a future recurrence into a direct, checkable
	// journalctl timestamp gap instead of another screenshot to guess
	// from.
	static struct timespec last_refresh_ts = {0, 0};
	struct timespec now_ts;
	clock_gettime(CLOCK_MONOTONIC, &now_ts);
	if (last_refresh_ts.tv_sec != 0) {
		long gap_ms = (now_ts.tv_sec - last_refresh_ts.tv_sec) * 1000
			+ (now_ts.tv_nsec - last_refresh_ts.tv_nsec) / 1000000;
		if (gap_ms > 300)
			fprintf(stderr, "sound_generic: spectrum refresh stalled, gap %ld ms (expected ~100ms)\n", gap_ms);
	}
	last_refresh_ts = now_ts;

	// same raw-sample-to-float divisor sbitx.c's own rx_linear() uses
	// (input_rx[j] / 20000000.0) for its FFT input, kept here for a
	// consistent dB scale/threshold with the SDR's own display
	for (int i = 0; i < GENERIC_SPEC_FFT_SIZE; i++) {
		double v = gen_spec_window[i] * (gen_spec_buf[i] / 20000000.0);
		__real__ gen_fft_in[i] = v;
		__imag__ gen_fft_in[i] = 0;
	}

	fftw_execute(gen_fft_plan);

	// k=0 (0Hz/dial frequency) writes to GENERIC_SPEC_START_BIN; k
	// increasing writes to increasing audio frequency/increasing bin
	// index -- no mirroring, matches the left-edge-anchored layout
	// documented above.
	for (int k = 0; k < GENERIC_SPEC_N_BINS; k++) {
		float mag = cabs(gen_fft_out[k]);
		float smoothed = ((1.0f - GENERIC_SPEC_SMOOTHING) * fft_bins[GENERIC_SPEC_START_BIN + k])
			+ (GENERIC_SPEC_SMOOTHING * mag);
		fft_bins[GENERIC_SPEC_START_BIN + k] = smoothed;
		int y = power2dB(cnrmf(smoothed));
		spectrum_plot[GENERIC_SPEC_START_BIN + k] = y;
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
			// Judge the real front-end level on the raw samples, before
			// our own gain (if any) is applied below.
			autogain_update(buf, (int)n, (float)n / GENERIC_SAMPLE_RATE);
			if (generic_rx_gain != 1.0f) {
				float gain = generic_rx_gain;
				for (int i = 0; i < (int)n; i++) {
					float scaled = buf[i] * gain;
					if (scaled > (float)INT32_MAX)
						scaled = (float)INT32_MAX;
					else if (scaled < (float)INT32_MIN)
						scaled = (float)INT32_MIN;
					buf[i] = (int32_t)scaled;
				}
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
