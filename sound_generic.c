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
#define GENERIC_BUF_FRAMES  960 // 10ms per read/write

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
	snd_pcm_t *pcm = open_pcm(generic_capture_device, SND_PCM_STREAM_CAPTURE, GENERIC_SAMPLE_RATE);
	int32_t buf[GENERIC_BUF_FRAMES];

	(void)arg;
	if (!pcm)
		return NULL;

	while (running) {
		snd_pcm_sframes_t n = snd_pcm_readi(pcm, buf, GENERIC_BUF_FRAMES);
		if (n == -EPIPE) {
			snd_pcm_prepare(pcm);
			continue;
		}
		if (n < 0) {
			fprintf(stderr, "sound_generic: capture error: %s\n", snd_strerror((int)n));
			continue;
		}
		modem_rx(rx_list->mode, buf, (int)n);
	}

	snd_pcm_close(pcm);
	return NULL;
}

// AI5II temporary diagnostic: dump the first ~25s of actual generated
// playback audio (post-scaling, exactly what reaches the QMX) to a WAV
// file for objective analysis, instead of continued guess-and-check.
static void ai5ii_write_wav_header(FILE *f, int sample_rate, int channels, int bits_per_sample, uint32_t data_bytes)
{
	uint32_t riff_size = 36 + data_bytes;
	uint16_t block_align = channels * bits_per_sample / 8;
	uint32_t byte_rate = sample_rate * block_align;
	uint32_t fmt_size = 16;
	uint16_t audio_format = 1;
	uint16_t ch = channels, bps = bits_per_sample;
	uint32_t sr = sample_rate;

	fseek(f, 0, SEEK_SET);
	fwrite("RIFF", 1, 4, f);
	fwrite(&riff_size, 4, 1, f);
	fwrite("WAVE", 1, 4, f);
	fwrite("fmt ", 1, 4, f);
	fwrite(&fmt_size, 4, 1, f);
	fwrite(&audio_format, 2, 1, f);
	fwrite(&ch, 2, 1, f);
	fwrite(&sr, 4, 1, f);
	fwrite(&byte_rate, 4, 1, f);
	fwrite(&block_align, 2, 1, f);
	fwrite(&bps, 2, 1, f);
	fwrite("data", 1, 4, f);
	fwrite(&data_bytes, 4, 1, f);
}

static void *playback_thread_fn(void *arg)
{
	snd_pcm_t *pcm = open_pcm(generic_playback_device, SND_PCM_STREAM_PLAYBACK, GENERIC_PLAYBACK_RATE);
	int32_t buf[GENERIC_BUF_FRAMES];
	FILE *ai5ii_dump = fopen("/tmp/tx_dump.wav", "wb");
	uint32_t ai5ii_dump_bytes = 0;
	const uint32_t ai5ii_dump_max_bytes = GENERIC_PLAYBACK_RATE * sizeof(int32_t) * 25;
	if (ai5ii_dump)
		fseek(ai5ii_dump, 44, SEEK_SET);

	(void)arg;
	if (!pcm)
		return NULL;

	static double tune_phase = 0;

	while (running) {
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
			n = snd_pcm_recover(pcm, (int)n, 0);
			if (n >= 0)
				// don't just drop this chunk of TX audio -- a string of
				// dropped chunks mid-transmission is exactly what would
				// produce "brief burst of audio, then nothing"
				snd_pcm_writei(pcm, buf, GENERIC_BUF_FRAMES);
		}

		if (ai5ii_dump && ai5ii_dump_bytes < ai5ii_dump_max_bytes) {
			size_t to_write = sizeof(buf);
			if (ai5ii_dump_bytes + to_write > ai5ii_dump_max_bytes)
				to_write = ai5ii_dump_max_bytes - ai5ii_dump_bytes;
			fwrite(buf, 1, to_write, ai5ii_dump);
			ai5ii_dump_bytes += to_write;
			if (ai5ii_dump_bytes >= ai5ii_dump_max_bytes) {
				ai5ii_write_wav_header(ai5ii_dump, GENERIC_PLAYBACK_RATE, 1, 32, ai5ii_dump_bytes);
				fclose(ai5ii_dump);
				ai5ii_dump = NULL;
				fprintf(stderr, "sound_generic: TX audio dump complete (/tmp/tx_dump.wav)\n");
			}
		}
	}

	snd_pcm_close(pcm);
	return NULL;
}

void sound_generic_start(void)
{
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

void sound_generic_restart(void)
{
	sound_generic_stop();
	sound_generic_start();
}
