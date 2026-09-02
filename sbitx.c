#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include <unistd.h>
// Windows port sketch: linux/types.h is unused here (no __u8/__u16/
// __u32/__u64/__le*/__be* anywhere in this file, confirmed via grep) --
// same dead-include pattern found and guarded in sbitx_daemon.c.
#ifndef _WIN32
#include <linux/types.h>
#endif
// Windows port sketch: PATH_MAX (the only thing linux/limits.h would
// provide) is unused here (confirmed via grep) -- same dead-include
// pattern found and guarded elsewhere tonight.
#ifndef _WIN32
#include <linux/limits.h>
#endif
#include <stdint.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
// Windows port sketch: the old zBitx SDR's own ALSA I/Q capture/
// playback thread is long gone from this codebase (see sound.h's own
// comment) -- sound_mixer()/sound_volume() below are the only two
// functions in this file that still touch ALSA directly at all, both
// pure hardware-mixer-control calls, nothing to do with the actual
// audio *path* (that's sound_generic.c/sound_generic_win.c). Guarded at
// their own definitions, not here -- see their own comments.
#ifndef _WIN32
#include <alsa/asoundlib.h>
#endif
#include "sdr.h"
#include "sdr_ui.h"
#include "sound.h"
#include "ini.h"
#include "configure.h"
#include "rig_generic.h"
#include "sound_generic.h"

#define DEBUG 0

// Standalone replacements for wiringPi's millis()/delay() -- see sdr.h
// for why these are still needed despite removing wiringPi itself.
// millis() matches wiringPi's own semantics: milliseconds since the
// first call (not since epoch), wrapping per unsigned int overflow.
unsigned int millis(void)
{
	static struct timespec start;
	static int have_start = 0;
	struct timespec now;

	if (!have_start) {
		clock_gettime(CLOCK_MONOTONIC, &start);
		have_start = 1;
	}
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned int)((now.tv_sec - start.tv_sec) * 1000
		+ (now.tv_nsec - start.tv_nsec) / 1000000);
}

void delay(unsigned int ms)
{
	usleep(ms * 1000);
}

char audio_card[32];
static int tx_shift = 512;

FILE* pf_debug = NULL;

// this is for processing FT8 decodes
// unsigned int	wallclock = 0;

#define TX_LINE 4
#define RX_LINE 16
#define BAND_SELECT 5
#define LPF_A 5
#define LPF_B 6
#define LPF_C 10
#define LPF_D 11
#define LPF_E 26

#define SBITX_DE (0)
#define SBITX_V2 (1)
#define SBITX_V4 (4)

int sbitx_version = -1;
int fwdpower, vswr;

// generic hamlib-rig backend selection (see sdr.h for the field descriptions)
int generic_rig_mode = 0;
char generic_rigctld_host[64] = "127.0.0.1";
// 4532 is hamlib.c's own hardcoded port (zbitxd emulating a controllable
// rig for *other* hamlib software) -- deliberately different here so our
// outbound rigctld client doesn't collide with it
int generic_rigctld_port = 4533;
char generic_capture_device[64] = "default";
char generic_playback_device[64] = "default";
float fft_bins[MAX_BINS]; // spectrum ampltiudes
int spectrum_plot[MAX_BINS];
fftw_complex* fft_spectrum;
fftw_plan plan_spectrum;
float spectrum_window[MAX_BINS];
void set_rx1(int frequency);
void tr_switch(int tx_on);

// Wisdom Defines for the FFTW and FFTWF libraries
// Options for WISDOM_MODE from least to most rigorous are FFTW_ESTIMATE, FFTW_MEASURE, FFTW_PATIENT, and FFTW_EXHAUSTIVE
// The FFTW_ESTIMATE mode seems to make completely incorrect Wisdom plan choices sometimes, and is not recommended.
// Wisdom plans found in an existing Wisdom file will negate the need for time consuming Wisdom plan calculations
// if the Wisdom plans in the file were generated at the same or more rigorous level.
#define WISDOM_MODE FFTW_MEASURE
#define PLANTIME -1                                // spend no more than plantime seconds finding the best FFT algorithm. -1 turns the platime cap off.
char wisdom_file[] = STATEDIR "/sbitx_wisdom.wis"; // Moved to default data directory - N3SB

fftw_complex* fft_out; // holds the incoming samples in freq domain (for rx as well as tx)
fftw_complex* fft_in;  // holds the incoming samples in time domain (for rx as well as tx)
fftw_complex* fft_m;   // holds previous samples for overlap and discard convolution
fftw_plan plan_fwd, plan_tx;
int bfo_freq = 40035000;
int freq_hdr = -1;
int si570_xtal = 0;

static double volume = 100.0;
static int tx_drive = 40;
static int rx_gain = 100;
static int rx_vol = 100;
static int tx_gain = 100;
static int tx_compress = 0;
static double spectrum_speed = 0.3;
static int in_tx = 0;
static int rx_tx_ramp = 0;
static int sidetone = 100;
struct vfo tone_a, tone_b, am_carrier; // these are audio tone generators
static int tx_use_line = 0;
struct rx* rx_list = NULL;

// Relocated from sbitx_sound.c (now removed along with the rest of the
// zBitx SDR's own ALSA I/Q capture/playback thread -- sound_generic.c
// is the real audio path for this backend). Only these four still had
// live callers outside that dead thread.

#ifdef _WIN32
// Windows always runs in generic_rig_mode (no zBitx hardware is
// possible there at all) -- the exact same condition the Linux version
// of this function already early-returns on before touching ALSA (see
// its own comment, kept below), so this is a behaviorally-identical
// no-op, not a change: a Windows build would have taken that early
// return every single time regardless.
void sound_mixer(char* card_name, char* element, int make_on)
{
	(void)card_name; (void)element; (void)make_on;
}
#else
void sound_mixer(char* card_name, char* element, int make_on)
{
	long min, max;
	snd_mixer_t* handle;
	snd_mixer_selem_id_t* sid;
	char* card = card_name;

	// the zBitx's own mixer control names ("Input Mux", "Mic Boost", etc.)
	// and card index don't exist on a generic rig's plain USB audio
	// device -- ALSA aborts the whole process on a null element rather
	// than failing gracefully, so skip this entirely in generic mode
	if (generic_rig_mode)
		return;

	snd_mixer_open(&handle, 0);
	snd_mixer_attach(handle, card);
	snd_mixer_selem_register(handle, NULL, NULL);
	snd_mixer_load(handle);

	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, element);
	snd_mixer_elem_t* elem = snd_mixer_find_selem(handle, sid);

	// find out if the his element is capture side or plaback
	if (snd_mixer_selem_has_capture_switch(elem)) {
		snd_mixer_selem_set_capture_switch_all(elem, make_on);
	} else if (snd_mixer_selem_has_playback_switch(elem)) {
		snd_mixer_selem_set_playback_switch_all(elem, make_on);
	} else if (snd_mixer_selem_has_playback_volume(elem)) {
		long volume = make_on;
		snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
		snd_mixer_selem_set_playback_volume_all(elem, volume * max / 100);
	} else if (snd_mixer_selem_has_capture_volume(elem)) {
		long volume = make_on;
		snd_mixer_selem_get_capture_volume_range(elem, &min, &max);
		snd_mixer_selem_set_capture_volume_all(elem, volume * max / 100);
	} else if (snd_mixer_selem_is_enumerated(elem)) {
		snd_mixer_selem_set_enum_item(elem, 0, make_on);
	}
	snd_mixer_close(handle);
}
#endif

#ifdef _WIN32
// Not called from anywhere in the codebase at all (confirmed via grep)
// -- kept as a real no-op rather than deleted, same "guard, don't
// delete" approach used elsewhere tonight, in case that changes.
void sound_volume(char* card_name, char* element, int volume)
{
	(void)card_name; (void)element; (void)volume;
}
#else
void sound_volume(char* card_name, char* element, int volume)
{
	long min, max;
	snd_mixer_t* handle;
	snd_mixer_selem_id_t* sid;
	char* card;

	card = card_name;
	snd_mixer_open(&handle, 0);
	snd_mixer_attach(handle, card);
	snd_mixer_selem_register(handle, NULL, NULL);
	snd_mixer_load(handle);

	snd_mixer_selem_id_alloca(&sid);
	snd_mixer_selem_id_set_index(sid, 0);
	snd_mixer_selem_id_set_name(sid, element);
	snd_mixer_elem_t* elem = snd_mixer_find_selem(handle, sid);

	snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
	snd_mixer_selem_set_playback_volume_all(elem, volume * max / 100);

	snd_mixer_close(handle);
}
#endif

static int use_virtual_cable = 0;

void sound_input(int loop)
{
	if (loop)
		use_virtual_cable = 1;
	else
		use_virtual_cable = 0;
}

// Was updated every capture block by the SDR's own ALSA thread; that
// thread is gone, so this is now frozen at whatever it last was
// (0, since it was never running for this backend to begin with).
// Only remaining caller is modem_cw.c's cw_tx_get_sample() -- pre-existing,
// harmless here since CW TX was never wired up for the generic-rig
// backend (RX/decode only, see the original project scope notes).
static unsigned long sound_millis = 0;

unsigned long sbitx_millis()
{
	return sound_millis;
}
struct rx* tx_list = NULL;
struct filter* tx_filter; // convolution filter
static double tx_amp = 0.0;
static double alc_level = 1.0;
static int tr_relay = 0;
static int rx_pitch = 700; // used only to offset the lo for CW,CWR
static int bridge_compensation = 100;
static double voice_clip_level = 0.1;
static int in_calibration = 1; // this turns off alc, clipping et al
static int mode_in_tune = MODE_USB;
static int in_tune_tx = 0;

#define MUTE_MAX 6
static int mute_count = 50;

FILE* pf_record;
int16_t record_buffer[1024];
int32_t modulation_buff[MAX_BINS];

/* the power gain of the tx varies widely from
band to band. these data structures help in flattening
the gain */

struct power_settings {
	int f_start;
	int f_stop;
	int max_watts;
	double scale;
};

struct power_settings band_power[] = {
	{ 3500000, 4000000, 37, 0.002 },
	{ 5200000, 5800009, 40, 0.0015 },
	{ 7000000, 7300009, 40, 0.0015 },
	{ 10000000, 10200000, 35, 0.0019 },
	{ 14000000, 14300000, 35, 0.0025 },
	{ 18000000, 18200000, 20, 0.0023 },
	{ 21000000, 21450000, 20, 0.003 },
	{ 24800000, 25000000, 20, 0.0034 },
	{ 28000000, 29700000, 20, 0.0037 }
};

#define CMD_TX (2)
#define CMD_RX (3)
#define TUNING_SHIFT (0)
#define MDS_LEVEL (-135)

struct Queue qremote;

void fft_init()
{
	// int mem_needed;

	// printf("initializing the fft\n");
	fflush(stdout);

	// mem_needed = sizeof(fftw_complex) * MAX_BINS;

	fft_m = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * MAX_BINS / 2);
	fft_in = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * MAX_BINS);
	fft_out = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * MAX_BINS);
	fft_spectrum = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * MAX_BINS);

	memset(fft_spectrum, 0, sizeof(fftw_complex) * MAX_BINS);
	memset(fft_in, 0, sizeof(fftw_complex) * MAX_BINS);
	memset(fft_out, 0, sizeof(fftw_complex) * MAX_BINS);
	memset(fft_m, 0, sizeof(fftw_complex) * MAX_BINS / 2);

	fftw_set_timelimit(PLANTIME);
	fftwf_set_timelimit(PLANTIME);
	int e = fftw_import_wisdom_from_filename(wisdom_file);
	if (e == 0) {
		printf("Generating Wisdom File...\n");
	}
	plan_fwd = fftw_plan_dft_1d(MAX_BINS, fft_in, fft_out, FFTW_FORWARD, WISDOM_MODE);           // Was FFTW_ESTIMATE N3SB
	plan_spectrum = fftw_plan_dft_1d(MAX_BINS, fft_in, fft_spectrum, FFTW_FORWARD, WISDOM_MODE); // Was FFTW_ESTIMATE N3SB
	fftw_export_wisdom_to_filename(wisdom_file);

	// zero up the previous 'M' bins
	for (int i = 0; i < MAX_BINS / 2; i++) {
		__real__ fft_m[i] = 0.0;
		__imag__ fft_m[i] = 0.0;
	}

	make_hann_window(spectrum_window, MAX_BINS);
}

void fft_reset_m_bins()
{
	// zero up the previous 'M' bins
	memset(fft_in, 0, sizeof(fftw_complex) * MAX_BINS);
	memset(fft_out, 0, sizeof(fftw_complex) * MAX_BINS);
	memset(fft_m, 0, sizeof(fftw_complex) * MAX_BINS / 2);
	memset(fft_spectrum, 0, sizeof(fftw_complex) * MAX_BINS);
	memset(tx_list->fft_time, 0, sizeof(fftw_complex) * MAX_BINS);
	memset(tx_list->fft_freq, 0, sizeof(fftw_complex) * MAX_BINS);
	/*	for (int i= 0; i < MAX_BINS/2; i++){
	        __real__ fft_m[i]  = 0.0;
	        __imag__ fft_m[i]  = 0.0;
	    }
	*/
}

int mag2db(double mag)
{
	int m = abs(mag) * 10000000;

	int c = 31;
	int p = 0x80000000;
	while (c > 0) {
		if (p & m)
			break;
		c--;
		p = p >> 1;
	}
	return c;
}

void set_spectrum_speed(int speed)
{
	spectrum_speed = speed;
	for (int i = 0; i < MAX_BINS; i++)
		fft_bins[i] = 0;
}

void spectrum_reset()
{
	for (int i = 0; i < MAX_BINS; i++)
		fft_bins[i] = 0;
}

void spectrum_update()
{
	// we are only using the lower half of the bins,
	// so this copies twice as many bins,
	// it can be optimized. leaving it here just in case
	// someone wants to try I Q channels
	// in hardware

	// this has been hand optimized to lower
	// the inordinate cpu usage
	for (int i = 1269; i < 1803; i++) {

		fft_bins[i] = ((1.0 - spectrum_speed) * fft_bins[i]) + (spectrum_speed * cabs(fft_spectrum[i]));

		int y = power2dB(cnrmf(fft_bins[i]));
		spectrum_plot[i] = y;
	}
}

#define SCALING_TRIM 200.0 // Use this to tune your meter response 2.7 worked at 51% and my inverted L
// S-Meter test W2JON
int calculate_s_meter()
{
	double signal_strength = 0.0;

	// Summing up the magnitudes of the FFT output bins
	for (int i = 0; i < MAX_BINS / 2; i++) {
		double magnitude = cabs(rx_list->fft_time[i]); // Magnitude of complex FFT output in time domain
		signal_strength += magnitude;
	}

	// Now average out the "signal strength"
	signal_strength /= (MAX_BINS / 2);

	// Logarithmic scaling based on rx_gain setting in percentage [0-100]
	double gain_scaling_factor = log10((rx_gain * 1.0) / 100.0 + 1.0);

	// Convert to pseudo dB
	double reference_power = 1e-4; // 0.1 mW
	double signal_power = signal_strength * signal_strength * reference_power;
	double s_meter_db = 10 * log10(signal_power / reference_power); // pseudo dB

	s_meter_db += gain_scaling_factor * SCALING_TRIM; // Adjust calcs dynamically based on rx_gain * SCALING_TRIM

	// Calculate S-units and additional dB
	int s_units = (int)(s_meter_db / 6.0);                 // Each S-unit corresponds to 6 dB
	int additional_db = (int)(s_meter_db - (s_units * 6)); // Remaining 'dB' above S9

	// Ensure non-negative values
	if (s_units < 0)
		s_units = 0;
	if (additional_db < 0)
		additional_db = 0;

	// Cap additional S-units at 20+ for simplicity
	if (s_units >= 9) {
		if (additional_db > 20)
			additional_db = 20;
	}

	// Return the value formatted as "S-unit * 100 + additional dB"
	return (s_units * 100) + additional_db;
}

int remote_audio_output(int16_t* samples)
{
	int length = q_length(&qremote);
	for (int i = 0; i < length; i++) {
		samples[i] = q_read(&qremote) / 32786;
	}
	return length;
}

void set_rx1(int frequency)
{
	static int last_frequency; // Holds the last frequency set - used by the Auto IF Gain algorithm

	last_frequency = frequency;
	if (frequency == freq_hdr)
		return;

	rig_generic_set_freq(frequency);
	freq_hdr = frequency;
}

void set_volume(double v)
{
	volume = v;
}

FILE* wav_start_writing(const char* path)
{
	char subChunk1ID[4] = { 'f', 'm', 't', ' ' };
	uint32_t subChunk1Size = 16; // 16 for PCM
	uint16_t audioFormat = 1;    // PCM = 1
	uint16_t numChannels = 1;
	uint16_t bitsPerSample = 16;
	uint32_t sampleRate = 12000;
	uint16_t blockAlign = numChannels * bitsPerSample / 8;
	uint32_t byteRate = sampleRate * blockAlign;

	char subChunk2ID[4] = { 'd', 'a', 't', 'a' };
	uint32_t subChunk2Size = 0Xffffffff; // num_samples * blockAlign;

	char chunkID[4] = { 'R', 'I', 'F', 'F' };
	uint32_t chunkSize = 4 + (8 + subChunk1Size) + (8 + subChunk2Size);
	char format[4] = { 'W', 'A', 'V', 'E' };

	FILE* f = fopen(path, "w");
	// ZBITXD LOCAL CHANGE (2026-08-26): real crash, live -- fopen()
	// failing here (confirmed cause: zbitxd runs as its own dedicated
	// system user, HOME=/var/lib/zbitxd, whose sbitx/audio/ subdirectory
	// was never created by anything) fell straight through into the
	// fwrite() calls below with f == NULL, segfaulting the entire
	// daemon -- mid-QSO, on a live band -- over what should have just
	// been "recording couldn't start." Never skip a null check just
	// because the failure "shouldn't" happen.
	if (!f) {
		fprintf(stderr, "wav_start_writing: fopen(%s) failed: %s\n", path, strerror(errno));
		return NULL;
	}

	// NOTE: works only on little-endian architecture
	fwrite(chunkID, sizeof(chunkID), 1, f);
	fwrite(&chunkSize, sizeof(chunkSize), 1, f);
	fwrite(format, sizeof(format), 1, f);

	fwrite(subChunk1ID, sizeof(subChunk1ID), 1, f);
	fwrite(&subChunk1Size, sizeof(subChunk1Size), 1, f);
	fwrite(&audioFormat, sizeof(audioFormat), 1, f);
	fwrite(&numChannels, sizeof(numChannels), 1, f);
	fwrite(&sampleRate, sizeof(sampleRate), 1, f);
	fwrite(&byteRate, sizeof(byteRate), 1, f);
	fwrite(&blockAlign, sizeof(blockAlign), 1, f);
	fwrite(&bitsPerSample, sizeof(bitsPerSample), 1, f);

	fwrite(subChunk2ID, sizeof(subChunk2ID), 1, f);
	fwrite(&subChunk2Size, sizeof(subChunk2Size), 1, f);

	return f;
}

// ZBITXD LOCAL CHANGE (2026-08-26): real, long-standing bug -- user's
// own report, from actually trying this a couple years ago against
// jt9/ft8modem and never getting a WAV file either would read.
// wav_start_writing() above writes the "data" subchunk size as a
// 0xffffffff placeholder (a real convention for a still-open/streaming
// WAV, but not one strict readers like jt9's own WAV loader accept),
// meant to be patched once the real byte count is known -- but "REC
// OFF" (sbitx.c's own record command handler) just called fclose()
// directly, never patching it. The file's header ends up claiming a
// ~4GB data chunk regardless of how much was actually recorded, which
// a strict WAV parser rejects outright even though the file itself is
// otherwise perfectly valid mono 16-bit 12kHz PCM (already the exact
// sample rate WSJT-X/jt9 itself expects for FT8, so no resampling
// needed). Seeks back and writes the real sizes before closing --
// standard canonical-WAV byte offsets, RIFF size at 4, data size at
// 40 (the 44-byte header written just above).
void wav_finish_writing(FILE *f)
{
	if (!f)
		return;
	long file_size = ftell(f);
	uint32_t data_size = file_size - 44;
	uint32_t riff_size = file_size - 8;
	fseek(f, 4, SEEK_SET);
	fwrite(&riff_size, sizeof(riff_size), 1, f);
	fseek(f, 40, SEEK_SET);
	fwrite(&data_size, sizeof(data_size), 1, f);
	fclose(f);
}

void wav_record(int32_t* samples, int count)
{
	int16_t* w;
	int32_t* s;
	int i = 0, j = 0;
	int decimation_factor = 96000 / 12000;

	if (!pf_record)
		return;

	w = record_buffer;
	while (i < count) {
		*w++ = *samples / 32786;
		samples += decimation_factor;
		i += decimation_factor;
		j++;
	}
	fwrite(record_buffer, j, sizeof(int16_t), pf_record);
}

/*
The sound process is called by the duplex sound system for each block of samples
In this demo, we read and equivalent block from the file instead of processing from
the input I and Q signals.
*/

int32_t in_i[MAX_BINS];
int32_t in_q[MAX_BINS];
int32_t out_i[MAX_BINS];
int32_t out_q[MAX_BINS];
short is_ready = 0;

void tx_init(int frequency, short mode, int bpf_low, int bpf_high)
{

	// we assume that there are 96000 samples / sec, giving us a 48khz slice
	// the tuning can go up and down only by 22 KHz from the center_freq

	tx_filter = filter_new(1024, 1025);
	//	filter_tune(tx_filter, (1.0 * bpf_low)/96000.0, (1.0 * bpf_high)/96000.0 , 5);
}

struct rx* add_tx(int frequency, short mode, int bpf_low, int bpf_high)
{

	// we assume that there are 96000 samples / sec, giving us a 48khz slice
	// the tuning can go up and down only by 22 KHz from the center_freq

	struct rx* r = malloc(sizeof(struct rx));
	r->low_hz = bpf_low;
	r->high_hz = bpf_high;
	r->tuned_bin = 512;

	// create fft complex arrays to convert the frequency back to time
	r->fft_time = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * MAX_BINS);
	r->fft_freq = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * MAX_BINS);

	int e = fftw_import_wisdom_from_filename(wisdom_file);
	if (e == 0) {
		printf("Generating Wisdom File...\n");
	}
	r->plan_rev = fftw_plan_dft_1d(MAX_BINS, r->fft_freq, r->fft_time, FFTW_BACKWARD, WISDOM_MODE); // Was FFTW_ESTIMATE N3SB
	fftw_export_wisdom_to_filename(wisdom_file);

	r->output = 0;
	r->next = NULL;
	r->mode = mode;

	r->filter = filter_new(1024, 1025);
	filter_tune(r->filter, (1.0 * bpf_low) / 96000.0, (1.0 * bpf_high) / 96000.0, 5);

	if (abs(bpf_high - bpf_low) < 1000) {
		r->agc_speed = 10;
		r->agc_threshold = -60;
		r->agc_loop = 0;
	} else {
		r->agc_speed = 10;
		r->agc_threshold = -60;
		r->agc_loop = 0;
	}

	// the modems drive the tx at 12000 Hz, this has to be upconverted
	// to the radio's sampling rate

	r->next = tx_list;
	tx_list = r;
}

struct rx* add_rx(int frequency, short mode, int bpf_low, int bpf_high)
{

	// we assume that there are 96000 samples / sec, giving us a 48khz slice
	// the tuning can go up and down only by 22 KHz from the center_freq

	struct rx* r = malloc(sizeof(struct rx));
	r->low_hz = bpf_low;
	r->high_hz = bpf_high;
	r->tuned_bin = 512;
	r->agc_gain = 0.0;

	// create fft complex arrays to convert the frequency back to time
	r->fft_time = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * MAX_BINS);
	r->fft_freq = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * MAX_BINS);

	int e = fftw_import_wisdom_from_filename(wisdom_file);
	if (e == 0) {
		printf("Generating Wisdom File...\n");
	}
	r->plan_rev = fftw_plan_dft_1d(MAX_BINS, r->fft_freq, r->fft_time, FFTW_BACKWARD, WISDOM_MODE); // Was FFTW_ESTIMATE N3SB
	fftw_export_wisdom_to_filename(wisdom_file);

	r->output = 0;
	r->next = NULL;
	r->mode = mode;

	r->filter = filter_new(1024, 1025);
	filter_tune(r->filter, (1.0 * bpf_low) / 96000.0, (1.0 * bpf_high) / 96000.0, 5);

	if (abs(bpf_high - bpf_low) < 1000) {
		r->agc_speed = 300;
		r->agc_threshold = -60;
		r->agc_loop = 0;
		r->signal_avg = 0;
	} else {
		r->agc_speed = 300;
		r->agc_threshold = -60;
		r->agc_loop = 0;
		r->signal_avg = 0;
	}

	// the modems are driven by 12000 samples/sec
	// the queue is for 20 seconds, 5 more than 15 sec needed for the FT8

	r->next = rx_list;
	rx_list = r;
}

int count = 0;

void set_rx_filter()
{
	// on AM filter at the IF level, instead of the baseband
	if (rx_list->mode == MODE_AM) {
		filter_tune(rx_list->filter,
		    (1.0 * (24000 - rx_list->high_hz)) / 96000.0,
		    (1.0 * (24000 + rx_list->high_hz)) / 96000.0,
		    5);
	} else if (rx_list->mode == MODE_LSB || rx_list->mode == MODE_CWR)
		filter_tune(rx_list->filter,
		    (1.0 * -rx_list->high_hz) / 96000.0,
		    (1.0 * -rx_list->low_hz) / 96000.0,
		    5);
	else
		filter_tune(rx_list->filter,
		    (1.0 * rx_list->low_hz) / 96000.0,
		    (1.0 * rx_list->high_hz) / 96000.0,
		    5);
}


static int hw_init_index = 0;
static int hw_settings_handler(void* user, const char* section,
    const char* name, const char* value)
{
	char cmd[1000];
	char new_value[200];

	// Real bug, found live (2026-09-02) porting to Windows: this is the
	// first time default_hw_settings.ini's own fallback path (not just
	// a real hw_settings.ini already sitting in STATEDIR, which is what
	// every existing Pi install actually has) got genuinely exercised.
	// default_hw_settings.ini has 10 [tx_band] sections; band_power[]
	// (its own compile-time initializer list, above) only has 9 --
	// hw_init_index reached 9 on the 10th section's "scale" key,
	// writing one struct past the end of the array. Undefined behavior
	// from there is exactly as unpredictable as it sounds: silently
	// corrupted whatever static/global data happened to sit right after
	// band_power[] in memory on this specific build/platform, with no
	// crash Windows would even log -- the process just stopped. Same
	// class of bug regardless of platform; simply never triggered
	// before because every real Pi install already has its own
	// hw_settings.ini and never falls back to this file at all.
	// Bounds-checked here rather than just fixing the current 10-vs-9
	// mismatch, so a future edit to either side of this pairing can't
	// silently reintroduce the exact same corruption.
	const int band_power_max = sizeof(band_power) / sizeof(band_power[0]);
	if (hw_init_index < band_power_max) {
		if (!strcmp(name, "f_start"))
			band_power[hw_init_index].f_start = atoi(value);
		if (!strcmp(name, "f_stop"))
			band_power[hw_init_index].f_stop = atoi(value);
		if (!strcmp(name, "scale"))
			band_power[hw_init_index++].scale = atof(value);
	}

	if (!strcmp(name, "bfo_freq"))
		bfo_freq = atoi(value);
	if (!strcmp(name, "si570_xtal"))
		si570_xtal = atoi(value);
	if (!strcmp(name, "hw"))
		sbitx_version = atoi(value);

	if (!strcmp(name, "radio"))
		generic_rig_mode = !strcmp(value, "generic");
	if (!strcmp(name, "rigctld_host"))
		strncpy(generic_rigctld_host, value, sizeof(generic_rigctld_host) - 1);
	if (!strcmp(name, "rigctld_port"))
		generic_rigctld_port = atoi(value);
	if (!strcmp(name, "capture_device"))
		strncpy(generic_capture_device, value, sizeof(generic_capture_device) - 1);
	if (!strcmp(name, "playback_device"))
		strncpy(generic_playback_device, value, sizeof(generic_playback_device) - 1);
}

static void read_hw_ini()
{
	hw_init_index = 0;
	if (ini_parse(STATEDIR "/hw_settings.ini", hw_settings_handler, NULL) < 0) {
		printf("Unable to load hw_settings.ini\nLoading default_hw_settings.ini instead\n");
		ini_parse(STATEDIR "/default_hw_settings.ini", hw_settings_handler, NULL);
	}
}

/*
     the PA gain varies across the band from 3.5 MHz to 30 MHz
    here we adjust the drive levels to keep it up, almost level
*/
void set_tx_power_levels()
{
	// int tx_power_gain = 0;

	// search for power in the approved bands
	for (int i = 0; i < sizeof(band_power) / sizeof(struct power_settings); i++) {
		if (band_power[i].f_start <= freq_hdr && freq_hdr <= band_power[i].f_stop) {

			// next we do a decimal coversion of the power reduction needed
			tx_amp = (1.0 * tx_drive * band_power[i].scale);
		}
	}
	printf("tx_amp is set to %g for %d drive\n", tx_amp, tx_drive);
	// we keep the audio card output 'volume' constant'
	// Set a level for transmitting - right channel
	sound_mixer(audio_card, "Master", 95);
	sound_mixer(audio_card, "Capture", tx_gain);
	alc_level = 1.0;
}


void tr_switch(int tx_on)
{
	rig_generic_set_ptt(tx_on);
}

/*
This is the one-time initialization code
*/
void setup(char* audio_output_device)
{

	printf("Audio Output Device is: %s\n", audio_output_device);

	read_hw_ini();

	fft_init();
	vfo_init_phase_table();
	q_init(&qremote, 8000);

	modem_init();

	add_rx(7000000, MODE_LSB, -3000, -300);
	add_tx(7000000, MODE_LSB, -3000, -300);
	rx_list->tuned_bin = 512;
	tx_list->tuned_bin = 512;
	tx_init(7000000, MODE_LSB, -3000, -150);

	rig_generic_init();
	// hw_settings.ini's [generic_rig] capture_device/playback_device
	// (read above by read_hw_ini()) only ever set the C globals --
	// the #capture_device/#playback_device FIELDs the web UI actually
	// displays kept their compiled-in "default" placeholder regardless
	// of what device was really opened. That let AUDIO_CONNECT
	// silently resubmit the stale "default" text and tear down a
	// working device -- confirmed live (real device was
	// "plughw:mchf,0", UI still showed "default", AUDIO_CONNECT killed
	// both capture and playback with "cannot open default"). A
	// user_settings.ini entry saved later still wins, since that's
	// parsed after this.
	set_field("#capture_device", generic_capture_device);
	set_field("#playback_device", generic_playback_device);
	// SPAN control removed from the web UI (locked at 3kHz, see the
	// matching "SPAN " lock in do_control_action()) -- set the real
	// starting value directly too, since spectrum_span's own
	// compiled-in default (48000) would otherwise be whatever a
	// stale persisted SPAN selection last left it at until the next
	// SPAN request (which may never come now that there's no
	// dropdown to send one).
	spectrum_span = 3000;
	sound_generic_start();

	sleep(1); // why? to allow the aloop to initialize?

	vfo_start(&tone_a, 700, 0);
	vfo_start(&tone_b, 1900, 0);
	vfo_start(&am_carrier, 24000, 0);

	sleep(2);
	// pf_debug = fopen("tx.raw", "w");
}

void sdr_request(char* request, char* response)
{
	char cmd[100], value[1000];

	char* p = strchr(request, '=');
	int n = p - request;
	if (!p)
		return;
	strncpy(cmd, request, n);
	cmd[n] = 0;
	strcpy(value, request + n + 1);

	if (!strcmp(cmd, "stat:tx")) {
		if (in_tx)
			strcpy(response, "ok on");
		else
			strcpy(response, "ok off");
	} else if (!strcmp(cmd, "r1:freq")) {
		int d = atoi(value);
		set_rx1(d);
		// printf("Frequency set to %d\n", freq_hdr);
		strcpy(response, "ok");
	} else if (!strcmp(cmd, "smeter")) {
		sprintf(response, "%d", calculate_s_meter());
	} else if (!strcmp(cmd, "r1:mode")) {
		if (!strcmp(value, "LSB"))
			rx_list->mode = MODE_LSB;
		else if (!strcmp(value, "CW"))
			rx_list->mode = MODE_CW;
		else if (!strcmp(value, "CWR"))
			rx_list->mode = MODE_CWR;
		else if (!strcmp(value, "2TONE"))
			rx_list->mode = MODE_2TONE;
		else if (!strcmp(value, "TUNE"))
			rx_list->mode = MODE_TUNE;
		else if (!strcmp(value, "FT4"))
			rx_list->mode = MODE_FT4;
		else if (!strcmp(value, "FT8"))
			rx_list->mode = MODE_FT8;
		else if (!strcmp(value, "AM"))
			rx_list->mode = MODE_AM;
		else if (!strcmp(value, "DIGI"))
			rx_list->mode = MODE_DIGITAL;
		else
			rx_list->mode = MODE_USB;

		// set the tx mode to that of the rx1
		tx_list->mode = rx_list->mode;

		if (generic_rig_mode)
			// the rig does its own demod/filtering -- tell it the new
			// mode over CAT instead of any local filter/oscillator work
			rig_generic_set_mode(value);

		// An interesting but non-essential note:
		// the sidebands inverted twice, to come out correctly after all
		// conisder that the second oscillator is set to 27.025 MHz and
		// a 7 MHz signal is tuned in by a 34 Mhz oscillator.
		// The first IF will be 25 Mhz, converted to a second IF of 25 KHz
		// Now, imagine that the signal at 7 Mhz moves up by 1 Khz
		// the IF now is going to be 34 - 7.001 MHz = 26.999 MHz which
		// converts to a second IF of 26.999 - 27.025 = 26 KHz
		// Effectively, if a signal moves up, so does the second IF

		if (rx_list->mode == MODE_AM) {
			filter_tune(tx_list->filter,
			    (1.0 * 20000) / 96000.0,
			    (1.0 * 30000) / 96000.0,
			    5);
			filter_tune(tx_filter,
			    (1.0 * 20000) / 96000.0,
			    (1.0 * 30000) / 96000.0,
			    5);
		} else if (rx_list->mode == MODE_LSB || rx_list->mode == MODE_CWR) {
			filter_tune(tx_list->filter,
			    (1.0 * -3000) / 96000.0,
			    (1.0 * -300) / 96000.0,
			    5);
			filter_tune(tx_filter,
			    (1.0 * -3000) / 96000.0,
			    (1.0 * -300) / 96000.0,
			    5);
		} else {
			filter_tune(tx_list->filter,
			    (1.0 * 300) / 96000.0,
			    (1.0 * 3000) / 96000.0,
			    5);
			filter_tune(tx_filter,
			    (1.0 * 300) / 96000.0,
			    (1.0 * 3000) / 96000.0,
			    5);
		}

		// we need to nudge the oscillator to adjust
		// to cw offset. setting it to the already tuned freq
		// doesnt recalculte the offsets

		int f = freq_hdr;
		set_rx1(f - 10);
		set_rx1(f);

		// printf("mode set to %d\n", rx_list->mode);
		strcpy(response, "ok");
	} else if (!strcmp(cmd, "txmode")) {
		if (!strcmp(value, "LSB") || !strcmp(value, "CWR"))
			filter_tune(tx_filter, (1.0 * -3000) / 96000.0, (1.0 * -300) / 96000.0, 5);
		else
			filter_tune(tx_filter, (1.0 * 300) / 96000.0, (1.0 * 3000) / 96000.0, 5);
	} else if (!strcmp(cmd, "record")) {
		if (!strcmp(value, "off")) {
			wav_finish_writing(pf_record);
			pf_record = NULL;
		} else {
			// Real leak, confirmed live: a second "record=..." while
			// one was already open just overwrote pf_record, leaving
			// the first file's handle open forever with its header
			// never flushed past 0 bytes (same underlying "can't
			// selectively close a stdio FILE* except by closing it"
			// reasoning as everywhere else this file gets touched).
			if (pf_record)
				wav_finish_writing(pf_record);
			pf_record = wav_start_writing(value);
		}
	} else if (!strcmp(cmd, "tx")) {
		if (!strcmp(value, "on"))
			tr_switch(1);
		else
			tr_switch(0);
		strcpy(response, "ok");
	} else if (!strcmp(cmd, "rx_pitch")) {
		rx_pitch = atoi(value);
	} else if (!strcmp(cmd, "tx_gain")) {
		tx_gain = atoi(value);
		if (in_tx)
			set_tx_power_levels();
	} else if (!strcmp(cmd, "tx_power")) {
		tx_drive = atoi(value);
		// DRIVE already scales the digital TX audio level fed to the
		// rig's mic/audio input (sound_generic.c's GENERIC_TX_SCALE_AT_
		// MAX_DRIVE) -- confirmed working, kept unchanged. This is a
		// genuinely separate knob on a real rig: RFPOWER controls the
		// actual RF output level itself, not how "loud" the audio
		// driving it is. Sent unconditionally (not gated on in_tx, e.g.
		// how RX RF avoids CAT during TX) since it's just a rig-side
		// setting, not something that needs to land at a precise
		// instant -- takes effect on whatever transmission comes next.
		if (generic_rig_mode)
			rig_generic_set_level("RFPOWER", tx_drive / 100.0f);
		if (in_tx)
			set_tx_power_levels();
	} else if (!strcmp(cmd, "bridge")) {
		bridge_compensation = atoi(value);
	} else if (!strcmp(cmd, "r1:gain")) {
		rx_gain = atoi(value);
		// generic_rig_mode: RX gain is now fully closed-loop, managed by
		// autogain_update() in sound_generic.c's capture thread -- a
		// nearby station overdriving its own amplifier and splattering
		// across the band is a generic problem the operator shouldn't
		// have to notice and react to by hand (user's own call). No
		// longer takes manual input here; the UI's RF slider was removed
		// to match. Left as a plain no-op rather than deleting this
		// branch outright, so a stray "r1:gain"/"IF" command can't fight
		// the auto-gain loop by writing a stale value out from under it.
		if (!generic_rig_mode && !in_tx)
			sound_mixer(audio_card, "Capture", rx_gain);
	} else if (!strcmp(cmd, "r1:volume")) {
		rx_vol = atoi(value);
		if (!in_tx) {
			//			printf("Audio Card: %s\n", audio_card);		// N3SB Hack
			// Set a level for receiver volume - left channel
			sound_mixer(audio_card, "Master", rx_vol); // Need to change - N3SB
			                                           //			sound_mixer("hw:0", "Master", rx_vol);
		}
	} else if (!strcmp(cmd, "r1:high")) {
		rx_list->high_hz = atoi(value);
		set_rx_filter();
	} else if (!strcmp(cmd, "r1:low")) {
		rx_list->low_hz = atoi(value);
		set_rx_filter();
	} else if (!strcmp(cmd, "r1:agc")) {
		if (!strcmp(value, "OFF"))
			rx_list->agc_speed = -1;
		else if (!strcmp(value, "SLOW"))
			rx_list->agc_speed = 100;
		else if (!strcmp(value, "MED"))
			rx_list->agc_speed = 33;
		else if (!strcmp(value, "FAST"))
			rx_list->agc_speed = 10;
	} else if (!strcmp(cmd, "sidetone")) { // between 100 and 0
		float t_sidetone = atof(value);
		if (0 <= t_sidetone && t_sidetone <= 100)
			sidetone = t_sidetone;
	} else if (!strcmp(cmd, "mod")) {
		if (!strcmp(value, "MIC"))
			tx_use_line = 0;
		else if (!strcmp(value, "LINE"))
			tx_use_line = 1;
	} else if (!strcmp(cmd, "tx_compress"))
		tx_compress = atoi(value);
	/* else
	      printf("*Error request[%s] not accepted\n", request); */
}
