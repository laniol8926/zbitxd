// Plain mono ALSA capture/playback thread pair for the generic-rig backend.
// RX side feeds straight into modem_rx() -- the same mode-agnostic dispatcher
// (modems.c) that the SDR's own I/Q-demodulated audio already feeds -- so
// ft8_rx()/cw_rx() need no changes at all. TX side is the mirror image via
// modem_next_sample(), which normally gets pulled into the SDR's upconversion
// loop (sbitx.c); here it goes straight to the playback device instead.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include <alsa/asoundlib.h>
#include "sdr.h"
#include "sound_generic.h"

// modem_next_sample()/ft8_next_sample() are tuned for 96ksps -- see the
// comment on modem_next_sample() in modems.c ("the ft8 samples are
// generated at 12ksps, we need to feed the sdr with 96 ksps"). Match that
// rate here instead of resampling.
#define GENERIC_SAMPLE_RATE 96000
#define GENERIC_BUF_FRAMES  960 // 10ms per read/write

static pthread_t capture_thread, playback_thread;
static volatile int running = 0;

static snd_pcm_t *open_pcm(const char *device, snd_pcm_stream_t stream)
{
	snd_pcm_t *handle;
	snd_pcm_hw_params_t *hw;
	unsigned int rate = GENERIC_SAMPLE_RATE;
	snd_pcm_uframes_t period_size = GENERIC_BUF_FRAMES;
	int err;

	err = snd_pcm_open(&handle, device, stream, 0);
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

	err = snd_pcm_hw_params(handle, hw);
	if (err < 0) {
		fprintf(stderr, "sound_generic: hw_params failed on %s: %s\n",
			device, snd_strerror(err));
		snd_pcm_close(handle);
		return NULL;
	}

	snd_pcm_prepare(handle);
	return handle;
}

static void *capture_thread_fn(void *arg)
{
	snd_pcm_t *pcm = open_pcm(generic_capture_device, SND_PCM_STREAM_CAPTURE);
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

static void *playback_thread_fn(void *arg)
{
	snd_pcm_t *pcm = open_pcm(generic_playback_device, SND_PCM_STREAM_PLAYBACK);
	int32_t buf[GENERIC_BUF_FRAMES];

	(void)arg;
	if (!pcm)
		return NULL;

	while (running) {
		for (int i = 0; i < GENERIC_BUF_FRAMES; i++) {
			// Runs continuously; modem_next_sample() returns 0
			// (silence) on its own whenever nothing is queued to
			// transmit, so this doesn't need to gate on in_tx.
			//
			// XXX: scaling factor is a first guess, roughly matching
			// the magnitude the existing SDR TX path applies before
			// its own upconversion (sbitx.c: i_sample = ... / 3) --
			// needs calibrating against the real rig's mic/line
			// input level once this runs on actual hardware.
			float sample = modem_next_sample(tx_list->mode);
			buf[i] = (int32_t)(sample * 200000000.0f);
		}
		snd_pcm_sframes_t n = snd_pcm_writei(pcm, buf, GENERIC_BUF_FRAMES);
		if (n == -EPIPE)
			snd_pcm_prepare(pcm);
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
