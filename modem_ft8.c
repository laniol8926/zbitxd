#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdbool.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <time.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include <pthread.h>
#include <unistd.h>
#include "sdr.h"
#include "sdr_ui.h"
#include "modem_ft8.h"
#include "logbook.h"

// override ft8_lib's log level by defining this before the includes
#define LOG_LEVEL LOG_INFO

#include "ft8_lib/common/common.h"
#include "ft8_lib/common/wave.h"
#include "ft8_lib/ft8/debug.h"
#include "ft8_lib/ft8/decode.h"
#include "ft8_lib/ft8/encode.h"
#include "ft8_lib/ft8/constants.h"
#include "ft8_lib/ft8/text.h"
#include "ft8_lib/fft/kiss_fftr.h"

static float ft8_rx_buffer[FT8_MAX_BUFF];
static float ft8_tx_buffer[FT8_MAX_BUFF];
// Scratch copy of the RX buffer used only by sbitx_ft8_decode()'s
// successive-interference-cancellation passes -- see its own comment.
// Static (not stack) to match ft8_rx_buffer/ft8_tx_buffer's own sizing
// convention and avoid a ~844KB stack allocation every decode cycle.
static float ft8_sic_residual[FT8_MAX_BUFF];
static char ft8_tx_text[128];
static char ft8_xota_text[14];
ftx_message_t ftx_tx_msg;
ftx_message_t ftx_xota_msg;
static int ft8_rx_buff_index = 0;
// real wallclock_day_ms at the moment ft8_rx_buff_index was actually
// reset to 0 for the CURRENT slot (set in ft8_rx()'s slot_time<500
// branch) -- see sbitx_ft8_decode()'s own comment on why DT needs this
static int ft8_rx_buff_start_ms = -1;
static int ft8_tx_buff_index = 0;
static int ft8_tx_nsamples = 0;
// ft8_tx_buffer/ft8_tx_nsamples/ft8_tx_buff_index are written by
// ftx_start_tx() (main thread, via ft8_poll()) and read by
// ft8_next_sample() -- with the generic-rig backend, that read now
// happens from a separate playback thread (sound_generic.c), which
// wasn't a concern when everything ran on one thread. Without this,
// ft8_next_sample() can observe a torn/stale mix of old and new buffer
// contents mid-transmission (confirmed on real hardware: tone samples
// interleaved with stretches of near-zero mid-message).
static pthread_mutex_t ft8_tx_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static int ft8_do_decode = 0;
static int ft8_do_tx = 0;
static int ft8_pitch = 0;
// number of repetitions left for the current message, counting down from the user setting
static int ft8_repeat = 5;
static pthread_t ft8_thread;
static bool is_cq = false; // is ft8_tx_text a CQ?
static bool ft8_tx1st = true;
static bool ft8_cq_alt = false;
static bool ft8_xota = false;

static const int kMin_score = 10; // Minimum sync score threshold for candidates
// Matched to ft8_lib's own reference demo tool (ft8_lib/demo/decode_ft8.c
// uses 140/25) -- this app had been running slightly below that. Part of
// closing the sensitivity gap vs jt9/WSJT-X on identical audio (task #25,
// n=288 jt9 decodes vs n=158 here); real headroom for this exists since
// current decode time (793-891ms) is well under the ~2s/15s-cycle budget.
static const int kMax_candidates = 140;
static const int kLDPC_iterations = 25;

static const int kMax_decoded_messages = 50;

static const int kFreq_osr = 2; // Frequency oversampling rate (bin subdivision)
static const int kTime_osr = 2; // Time oversampling rate (symbol subdivision)

// styles to use for each enum value in ftx_field_t
static const int kFieldType_style_map[] = {
	STYLE_LOG,		// FTX_FIELD_UNKNOWN
	STYLE_LOG,		// FTX_FIELD_NONE
	STYLE_FT8_RX,	// FTX_FIELD_TOKEN
	STYLE_FT8_RX,	// FTX_FIELD_TOKEN_WITH_ARG
	STYLE_CALLER,	// FTX_FIELD_CALL
	STYLE_GRID,		// FTX_FIELD_GRID
	STYLE_LOG		// FTX_FIELD_RST
};

#define SECS_IN_DAY (24 * 60 * 60)
static int wallclock_day_ms = 0; // starts from 0 each day

static void ftx_update_clock()
{
    struct timespec  ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
       perror("clock_gettime");
       exit(EXIT_FAILURE);
    }

    time_t ms = ts.tv_nsec / 1000000;
    wallclock_day_ms = (ts.tv_sec % SECS_IN_DAY) * 1000 + ms;
    //~ printf("time %lld.%lld: %d min ms %d\n", ts.tv_sec, ms, wallclock_day_ms, wallclock_day_ms % 60000);
}

/*!
    Format the day-time-in-millseconds \a day_ms
    (or wallclock time instead if \a day_ms is -1)
    as HHMMSS to the given buffer, and return
    a pointer to the character after what has been printed
    (e.g. to continue printing something else with sprintf).
    Always prints exactly 8 characters and returns buf + 8.
*/
// TODO move this and ftx_update_clock to sbitx.h or so: use a global clock
static char *hmst_time_sprint(char *buf, int day_ms)
{
    const int dms = day_ms >= 0 ? day_ms : wallclock_day_ms;
    int wallclock_day_s = dms / 1000;
    // simple arithmetic instead of using modulus (%): assuming the latter is much more expensive
    // it could be that this is less efficient though; it could be checked with callgrind or so
    int h = wallclock_day_s / (3600);
    int m = (wallclock_day_s - (h * 3600)) / 60;
    int s = wallclock_day_s - (h * 3600 + m * 60);
    // used to conditionally append ".<tenths>" when nonzero -- but the
    // RX decode path (raw_ms, rounded to the slot boundary) never has a
    // nonzero tenths digit, while the TX/queued path (live
    // wallclock_day_ms, not rounded) almost always does, so operators'
    // own CQ/TX lines showed a stray ".7"/".1"/etc that incoming decode
    // lines never did -- confirmed as unwanted, not a needed feature
    // (user: "there is a dot and a number appended to the time on my
    // cq's... it shouldn't be there"). Always the plain whole-second
    // form now, both paths consistent. Width must stay exactly 8 chars
    // either way -- other code relies on that (semantic-span column
    // math, prefix_len calculations).
    const int len = sprintf(buf, "%02d%02d%02d  ", h, m, s);
    return buf + len;
}

static char *hmst_wallclock_time_sprint(char *buf)
{
    return hmst_time_sprint(buf, -1);
}

#define FT8_SYMBOL_BT 2.0f ///< symbol smoothing filter bandwidth factor (BT)
#define FT4_SYMBOL_BT 1.0f ///< symbol smoothing filter bandwidth factor (BT)

#define GFSK_CONST_K 5.336446f ///< == pi * sqrt(2 / log(2))

#define CALLSIGN_HASHTABLE_SIZE 256

static struct
{
    char callsign[12]; ///> Up to 11 symbols of callsign + trailing zeros (always filled)
    uint32_t hash;     ///> 8 MSBs contain the age of callsign; 22 LSBs contain hash value
} callsign_hashtable[CALLSIGN_HASHTABLE_SIZE];

static int callsign_hashtable_size;

void hashtable_init(void)
{
    callsign_hashtable_size = 0;
    memset(callsign_hashtable, 0, sizeof(callsign_hashtable));
}

void hashtable_cleanup(uint8_t max_age)
{
    for (int idx_hash = 0; idx_hash < CALLSIGN_HASHTABLE_SIZE; ++idx_hash)
    {
        if (callsign_hashtable[idx_hash].callsign[0] != '\0')
        {
            uint8_t age = (uint8_t)(callsign_hashtable[idx_hash].hash >> 24);
            if (age > max_age)
            {
                LOG(LOG_INFO, "Removing [%s] from hash table, age = %d\n", callsign_hashtable[idx_hash].callsign, age);
                // free the hash entry
                callsign_hashtable[idx_hash].callsign[0] = '\0';
                callsign_hashtable[idx_hash].hash = 0;
                callsign_hashtable_size--;
            }
            else
            {
                // increase callsign age
                callsign_hashtable[idx_hash].hash = (((uint32_t)age + 1u) << 24) | (callsign_hashtable[idx_hash].hash & 0x3FFFFFu);
            }
        }
    }
}

void hashtable_add(const char* callsign, uint32_t hash)
{
    uint16_t hash10 = (hash >> 12) & 0x3FFu;
    int idx_hash = (hash10 * 23) % CALLSIGN_HASHTABLE_SIZE;
    while (callsign_hashtable[idx_hash].callsign[0] != '\0')
    {
        if (((callsign_hashtable[idx_hash].hash & 0x3FFFFFu) == hash) && (0 == strcmp(callsign_hashtable[idx_hash].callsign, callsign)))
        {
            // reset age
            callsign_hashtable[idx_hash].hash &= 0x3FFFFFu;
            LOG(LOG_DEBUG, "Found a duplicate [%s]\n", callsign);
            return;
        }
        else
        {
            LOG(LOG_DEBUG, "Hash table clash!\n");
            // Move on to check the next entry in hash table
            idx_hash = (idx_hash + 1) % CALLSIGN_HASHTABLE_SIZE;
        }
    }
    callsign_hashtable_size++;
    strncpy(callsign_hashtable[idx_hash].callsign, callsign, 11);
    callsign_hashtable[idx_hash].callsign[11] = '\0';
    callsign_hashtable[idx_hash].hash = hash;
}

bool hashtable_lookup(ftx_callsign_hash_type_t hash_type, uint32_t hash, char* callsign)
{
    uint8_t hash_shift = (hash_type == FTX_CALLSIGN_HASH_10_BITS) ? 12 : (hash_type == FTX_CALLSIGN_HASH_12_BITS ? 10 : 0);
    uint16_t hash10 = (hash >> (12 - hash_shift)) & 0x3FFu;
    int idx_hash = (hash10 * 23) % CALLSIGN_HASHTABLE_SIZE;
    while (callsign_hashtable[idx_hash].callsign[0] != '\0')
    {
        if (((callsign_hashtable[idx_hash].hash & 0x3FFFFFu) >> hash_shift) == hash)
        {
            strcpy(callsign, callsign_hashtable[idx_hash].callsign);
            return true;
        }
        // Move on to check the next entry in hash table
        idx_hash = (idx_hash + 1) % CALLSIGN_HASHTABLE_SIZE;
    }
    callsign[0] = '\0';
    return false;
}

ftx_callsign_hash_interface_t hash_if = {
    .lookup_hash = hashtable_lookup,
    .save_hash = hashtable_add
};

/// Computes a GFSK smoothing pulse.
/// The pulse is theoretically infinitely long, however, here it's truncated at 3 times the symbol length.
/// This means the pulse array has to have space for 3*n_spsym elements.
/// @param[in] n_spsym Number of samples per symbol
/// @param[in] b Shape parameter (values defined for FT8/FT4)
/// @param[out] pulse Output array of pulse samples
///
static void gfsk_pulse(int n_spsym, float symbol_bt, float* pulse)
{
    for (int i = 0; i < 3 * n_spsym; ++i)
    {
        float t = i / (float)n_spsym - 1.5f;
        float arg1 = GFSK_CONST_K * symbol_bt * (t + 0.5f);
        float arg2 = GFSK_CONST_K * symbol_bt * (t - 0.5f);
        pulse[i] = (erff(arg1) - erff(arg2)) / 2;
    }
}

/// Synthesize waveform data using GFSK phase shaping.
/// The output waveform will contain n_sym symbols.
/// @param[in] symbols Array of symbols (tones) (0-7 for FT8)
/// @param[in] n_sym Number of symbols in the symbol array
/// @param[in] f0 Audio frequency in Hertz for the symbol 0 (base frequency)
/// @param[in] symbol_bt Symbol smoothing filter bandwidth (2 for FT8, 1 for FT4)
/// @param[in] symbol_period Symbol period (duration), seconds
/// @param[in] signal_rate Sample rate of synthesized signal, Hertz
/// @param[out] signal Output array of signal waveform samples (should have space for n_sym*n_spsym samples)
///
static void synth_gfsk(const uint8_t* symbols, int n_sym, float f0, float symbol_bt, float symbol_period, int signal_rate, float* signal)
{
    int n_spsym = (int)(0.5f + signal_rate * symbol_period); // Samples per symbol
    int n_wave = n_sym * n_spsym;                            // Number of output samples
    float hmod = 1.0f;

    LOG(LOG_DEBUG, "n_spsym = %d\n", n_spsym);
    // Compute the smoothed frequency waveform.
    // Length = (nsym+2)*n_spsym samples, first and last symbols extended
    float dphi_peak = 2 * M_PI * hmod / n_spsym;
    float dphi[n_wave + 2 * n_spsym];

    // Shift frequency up by f0
    for (int i = 0; i < n_wave + 2 * n_spsym; ++i)
    {
        dphi[i] = 2 * M_PI * f0 / signal_rate;
    }

    float pulse[3 * n_spsym];
    gfsk_pulse(n_spsym, symbol_bt, pulse);

    for (int i = 0; i < n_sym; ++i)
    {
        int ib = i * n_spsym;
        for (int j = 0; j < 3 * n_spsym; ++j)
        {
            dphi[j + ib] += dphi_peak * symbols[i] * pulse[j];
        }
    }

    // Add dummy symbols at beginning and end with tone values equal to 1st and last symbol, respectively
    for (int j = 0; j < 2 * n_spsym; ++j)
    {
        dphi[j] += dphi_peak * pulse[j + n_spsym] * symbols[0];
        dphi[j + n_sym * n_spsym] += dphi_peak * pulse[j] * symbols[n_sym - 1];
    }

    // Calculate and insert the audio waveform
    float phi = 0;
    for (int k = 0; k < n_wave; ++k)
    { // Don't include dummy symbols
        signal[k] = sinf(phi);
        phi = fmodf(phi + dphi[k + n_spsym], 2 * M_PI);
    }

    // Apply envelope shaping to the first and last symbols
    int n_ramp = n_spsym / 8;
    for (int i = 0; i < n_ramp; ++i)
    {
        float env = (1 - cosf(2 * M_PI * i / (2 * n_ramp))) / 2;
        signal[i] *= env;
        signal[n_wave - 1 - i] *= env;
    }
}

/*!
	Encode ftx_tx_msg or ftx_xota_msg payload onto audio carrier \a freq and output to \a signal.
	@return the number of audio samples
*/
int sbitx_ftx_msg_audio(int32_t freq, float *signal)
{
	if (!freq)
		freq = field_int("TX_PITCH");
    float frequency = 1.0 * freq;

	bool is_ft4 = !strcmp(field_str("MODE"), "FT4");

	int num_tones = is_ft4 ? FT4_NN : FT8_NN;
	float symbol_period = is_ft4 ? FT4_SYMBOL_PERIOD : FT8_SYMBOL_PERIOD;
	float symbol_bt = is_ft4 ? FT4_SYMBOL_BT : FT8_SYMBOL_BT;
	float slot_time = is_ft4 ? FT4_SLOT_TIME : FT8_SLOT_TIME;

    // Second, encode the binary message as a sequence of FSK tones
    uint8_t tones[num_tones]; // Array of 79 tones (symbols)
    if (is_ft4)
        ft4_encode(ft8_xota ? ftx_xota_msg.payload : ftx_tx_msg.payload, tones);
    else
        ft8_encode(ft8_xota ? ftx_xota_msg.payload : ftx_tx_msg.payload, tones);

    // Third, convert the FSK tones into an audio signal
    int sample_rate = 12000;
    int num_samples = (int)(0.5f + num_tones * symbol_period * sample_rate); // samples in the data signal
    int num_silence = (slot_time * sample_rate - num_samples) / 2;           // Silence  to make 15 seconds
    int num_total_samples = num_silence + num_samples + num_silence;         // total Number samples

    for (int i = 0; i < num_silence; i++) {
        signal[i] = 0;
        signal[i + num_samples + num_silence] = 0;
    }

    // Synthesize waveform data (signal) and save it as WAV file
    LOG(LOG_DEBUG, "%05d %s '%s' synth_gfsk %d %f %f %f %d samples %d silence %d\n",
        wallclock_day_ms % 60000, (is_ft4 ? "FT4" : "FT8"), ft8_xota ? ft8_xota_text : ft8_tx_text, num_tones,
        frequency, symbol_bt, symbol_period, sample_rate, num_samples, num_silence);
    synth_gfsk(tones, num_tones, frequency, symbol_bt, symbol_period, sample_rate, signal + num_silence);
    return num_total_samples;
}

/*!
	Encode \a message into ftx_tx_msg.
	This should only be used when the user has typed \a message;
	for programmatic cases, prefer sbitx_ft8_encode_3f'()
	@return the return value from ftx_message_encode (see enum ftx_message_rc_t in ft8_lib/message.h)
*/
int sbitx_ft8_encode(char *message)
{
    ftx_message_rc_t rc = ftx_message_encode(&ftx_tx_msg, &hash_if, message);
    if (rc != FTX_MESSAGE_RC_OK)
    {
        printf("Cannot encode FTx message! RC = %d\n", (int)rc);
        return -1;
    }

	return rc;
}

/*!
	Compose a message from the 3 fields \a call_to, \a call_de and \a extra into ftx_tx_msg.
	@return the return value from ftx_message_encode_std/nonstd/free
	(see enum ftx_message_rc_t in ft8_lib/message.h)
*/
int sbitx_ft8_encode_3f(const char* call_to, const char* call_de, const char* extra)
{
	ftx_message_rc_t rc = ftx_message_encode_std(&ftx_tx_msg, &hash_if, call_to, call_de, extra);
	if (rc != FTX_MESSAGE_RC_OK) {
		LOG(LOG_DEBUG, "   ftx_message_encode_std failed: %d\n", rc);
		rc = ftx_message_encode_nonstd(&ftx_tx_msg, &hash_if, call_to, call_de, extra);
		if (rc != FTX_MESSAGE_RC_OK) {
			LOG(LOG_DEBUG, "   ftx_message_encode_nonstd failed: %d\n", rc);
			rc = ftx_message_encode_free(&ftx_tx_msg, ft8_tx_text);
		}
	}

    if (rc != FTX_MESSAGE_RC_OK)
        printf("Cannot encode FTx 3-field message! RC = %d\n", (int)rc);

	return rc;
}

static float hann_i(int i, int N)
{
    float x = sinf((float)M_PI * i / N);
    return x * x;
}

static float hamming_i(int i, int N)
{
    const float a0 = (float)25 / 46;
    const float a1 = 1 - a0;

    float x1 = cosf(2 * (float)M_PI * i / N);
    return a0 - a1 * x1;
}

static float blackman_i(int i, int N)
{
    const float alpha = 0.16f; // or 2860/18608
    const float a0 = (1 - alpha) / 2;
    const float a1 = 1.0f / 2;
    const float a2 = alpha / 2;

    float x1 = cosf(2 * (float)M_PI * i / N);
    float x2 = 2 * x1 * x1 - 1; // Use double angle formula

    return a0 - a1 * x1 + a2 * x2;
}

void waterfall_init(ftx_waterfall_t* me, int max_blocks, int num_bins, int time_osr, int freq_osr)
{
    size_t mag_size = max_blocks * time_osr * freq_osr * num_bins * sizeof(me->mag[0]);
    me->max_blocks = max_blocks;
    me->num_blocks = 0;
    me->num_bins = num_bins;
    me->time_osr = time_osr;
    me->freq_osr = freq_osr;
    me->block_stride = (time_osr * freq_osr * num_bins);
    me->mag = (uint8_t  *)malloc(mag_size);
    LOG(LOG_DEBUG, "Waterfall size = %zu\n", mag_size);
}

void waterfall_free(ftx_waterfall_t* me)
{
    free(me->mag);
}

/// Configuration options for FT4/FT8 monitor
typedef struct
{
    float f_min;             ///< Lower frequency bound for analysis
    float f_max;             ///< Upper frequency bound for analysis
    int sample_rate;         ///< Sample rate in Hertz
    int time_osr;            ///< Number of time subdivisions
    int freq_osr;            ///< Number of frequency subdivisions
    ftx_protocol_t protocol; ///< Protocol: FT4 or FT8
} monitor_config_t;

/// FT4/FT8 monitor object that manages DSP processing of incoming audio data
/// and prepares a waterfall object
typedef struct
{
    float symbol_period; ///< FT4/FT8 symbol period in seconds
    int block_size;      ///< Number of samples per symbol (block)
    int subblock_size;   ///< Analysis shift size (number of samples)
    int nfft;            ///< FFT size
    float fft_norm;      ///< FFT normalization factor
    float* window;       ///< Window function for STFT analysis (nfft samples)
    float* last_frame;   ///< Current STFT analysis frame (nfft samples)
    ftx_waterfall_t wf;      ///< Waterfall object
    float max_mag;       ///< Maximum detected magnitude (debug stats)

    // KISS FFT housekeeping variables
    void* fft_work;        ///< Work area required by Kiss FFT
    kiss_fftr_cfg fft_cfg; ///< Kiss FFT housekeeping object
} monitor_t;

static void monitor_init(monitor_t* me, const monitor_config_t* cfg)
{
    float slot_time = (cfg->protocol == FTX_PROTOCOL_FT4) ? FT4_SLOT_TIME : FT8_SLOT_TIME;
    float symbol_period = (cfg->protocol == FTX_PROTOCOL_FT4) ? FT4_SYMBOL_PERIOD : FT8_SYMBOL_PERIOD;
    // Compute DSP parameters that depend on the sample rate
    me->block_size = (int)(cfg->sample_rate * symbol_period); // samples corresponding to one FSK symbol
    me->subblock_size = me->block_size / cfg->time_osr;
    me->nfft = me->block_size * cfg->freq_osr;
    me->fft_norm = 2.0f / me->nfft;
    // const int len_window = 1.8f * me->block_size; // hand-picked and optimized

    me->window = (float *)malloc(me->nfft * sizeof(me->window[0]));
    for (int i = 0; i < me->nfft; ++i)
    {
        // window[i] = 1;
        me->window[i] = hann_i(i, me->nfft);
        // me->window[i] = blackman_i(i, me->nfft);
        // me->window[i] = hamming_i(i, me->nfft);
        // me->window[i] = (i < len_window) ? hann_i(i, len_window) : 0;
    }
    me->last_frame = (float *)malloc(me->nfft * sizeof(me->last_frame[0]));

    size_t fft_work_size;
    kiss_fftr_alloc(me->nfft, 0, 0, &fft_work_size);

    //LOG(LOG_INFO, "Block size = %d\n", me->block_size);
    //LOG(LOG_INFO, "Subblock size = %d\n", me->subblock_size);
    //LOG(LOG_INFO, "N_FFT = %d\n", me->nfft);
    LOG(LOG_DEBUG, "FFT work area = %zu\n", fft_work_size);

    me->fft_work = malloc(fft_work_size);
    me->fft_cfg = kiss_fftr_alloc(me->nfft, 0, me->fft_work, &fft_work_size);

    const int max_blocks = (int)(slot_time / symbol_period);
    const int num_bins = (int)(cfg->sample_rate * symbol_period / 2);
    waterfall_init(&me->wf, max_blocks, num_bins, cfg->time_osr, cfg->freq_osr);
    me->wf.protocol = cfg->protocol;
    me->symbol_period = symbol_period;

    me->max_mag = -120.0f;
}

static void monitor_free(monitor_t* me)
{
    waterfall_free(&me->wf);
    free(me->fft_work);
    free(me->last_frame);
    free(me->window);
}

// Compute FFT magnitudes (log wf) for a frame in the signal and update waterfall data
static void monitor_process(monitor_t* me, const float* frame)
{
    // Check if we can still store more waterfall data
    if (me->wf.num_blocks >= me->wf.max_blocks)
        return;

    int offset = me->wf.num_blocks * me->wf.block_stride;
    int frame_pos = 0;

    // Loop over block subdivisions
    for (int time_sub = 0; time_sub < me->wf.time_osr; ++time_sub)
    {
        kiss_fft_scalar timedata[me->nfft];
        kiss_fft_cpx freqdata[me->nfft / 2 + 1];

        // Shift the new data into analysis frame
        for (int pos = 0; pos < me->nfft - me->subblock_size; ++pos)
        {
            me->last_frame[pos] = me->last_frame[pos + me->subblock_size];
        }
        for (int pos = me->nfft - me->subblock_size; pos < me->nfft; ++pos)
        {
            me->last_frame[pos] = frame[frame_pos];
            ++frame_pos;
        }

        // Compute windowed analysis frame
        for (int pos = 0; pos < me->nfft; ++pos)
        {
            timedata[pos] = me->fft_norm * me->window[pos] * me->last_frame[pos];
        }

        kiss_fftr(me->fft_cfg, timedata, freqdata);

        // Loop over two possible frequency bin offsets (for averaging)
        for (int freq_sub = 0; freq_sub < me->wf.freq_osr; ++freq_sub)
        {
            for (int bin = 0; bin < me->wf.num_bins; ++bin)
            {
                int src_bin = (bin * me->wf.freq_osr) + freq_sub;
                float mag2 = (freqdata[src_bin].i * freqdata[src_bin].i) + (freqdata[src_bin].r * freqdata[src_bin].r);
                float db = 10.0f * log10f(1E-12f + mag2);
                // Scale decibels to unsigned 8-bit range and clamp the value
                // Range 0-240 covers -120..0 dB in 0.5 dB steps
                int scaled = (int)(2 * db + 240);

                me->wf.mag[offset] = (scaled < 0) ? 0 : ((scaled > 255) ? 255 : scaled);
                ++offset;

                if (db > me->max_mag)
                    me->max_mag = db;
            }
        }
    }

    ++me->wf.num_blocks;
}

static void monitor_reset(monitor_t* me)
{
    me->wf.num_blocks = 0;
    me->max_mag = 0;
}

static int message_callsign_count(const ftx_message_offsets_t *spans)
{
	int ret = 0;
	for (int i = 0; i < FTX_MAX_MESSAGE_FIELDS; ++i)
		if (spans->types[i] == FTX_FIELD_CALL)
			++ret;
	return ret;
}

// Successive interference cancellation: once a candidate has actually
// been decoded (LDPC already recovered its exact 77-bit payload, error
// corrected), regenerate its clean waveform and subtract it from the
// residual signal before the next decode pass -- lets weaker signals
// that were masked/overlapping the strong one get found on a later
// pass. Reconstructs from the decoded payload itself (via the existing
// ft8_encode()/synth_gfsk() TX-side machinery) rather than resynthesizing
// from captured phase data (ft8_lib's own monitor_resynth(), disabled in
// this fork behind WATERFALL_USE_PHASE -- doubling the waterfall's
// memory for phase storage on a Pi Zero 2 W) -- more accurate anyway,
// since LDPC already gives the ideal noise-free bit sequence.
//
// Amplitude is estimated by least-squares matched-filter scaling (the
// standard technique: the scale that minimizes residual energy over the
// overlap is sum(signal*synth)/sum(synth*synth)) rather than guessed
// from the candidate's score/SNR, which aren't in the same units as raw
// sample amplitude.
static void sic_subtract_candidate(const monitor_t *mon, const ftx_candidate_t *cand,
	const ftx_message_t *message, bool is_ft4, float *residual, int num_samples)
{
	int num_tones = is_ft4 ? FT4_NN : FT8_NN;
	float symbol_period = is_ft4 ? FT4_SYMBOL_PERIOD : FT8_SYMBOL_PERIOD;
	float symbol_bt = is_ft4 ? FT4_SYMBOL_BT : FT8_SYMBOL_BT;
	int sample_rate = 12000;

	uint8_t tones[FT4_NN > FT8_NN ? FT4_NN : FT8_NN];
	if (is_ft4)
		ft4_encode(message->payload, tones);
	else
		ft8_encode(message->payload, tones);

	int freq_hz = lroundf((cand->freq_offset + (float)cand->freq_sub / mon->wf.freq_osr) / mon->symbol_period);
	// Raw ft8_lib-internal alignment -- deliberately NOT including
	// dt_algorithm_calibration_sec, which is purely a *display*
	// calibration against jt9's own DT convention, not a real offset
	// into this buffer.
	float raw_time_sec = (cand->time_offset + (float)cand->time_sub / mon->wf.time_osr) * mon->symbol_period;
	int sample_offset = lroundf(raw_time_sec * sample_rate);

	int n_spsym = (int)(0.5f + sample_rate * symbol_period);
	int n_wave = num_tones * n_spsym;
	float synth[n_wave];
	synth_gfsk(tones, num_tones, (float)freq_hz, symbol_bt, symbol_period, sample_rate, synth);

	// Overlap region between the synthesized waveform and the actual
	// buffer -- candidates near either edge of the capture window can
	// run off either end.
	int start = sample_offset < 0 ? 0 : sample_offset;
	int end = sample_offset + n_wave > num_samples ? num_samples : sample_offset + n_wave;
	if (start >= end)
		return;

	double dot_sr = 0, dot_ss = 0;
	for (int i = start; i < end; ++i){
		float s = synth[i - sample_offset];
		dot_sr += (double)residual[i] * s;
		dot_ss += (double)s * s;
	}
	if (dot_ss <= 0)
		return;
	float scale = (float)(dot_sr / dot_ss);

	for (int i = start; i < end; ++i)
		residual[i] -= scale * synth[i - sample_offset];
}

static int sbitx_ft8_decode(float *signal, int num_samples)
{
    int sample_rate = 12000;
	bool is_ft8 = !strcmp(field_str("MODE"), "FT8");

    LOG(LOG_DEBUG, "sbitx_ftx_decode: %s sample rate %d Hz, %d samples, %.3f seconds\n",
		(is_ft8 ? "FT8" : "FT4"), sample_rate, num_samples, (double)num_samples / sample_rate);

    // Compute FFT over the whole signal and store it
    monitor_t mon;
    monitor_config_t mon_cfg = {
        .f_min = 100,
        .f_max = 3000,
        .sample_rate = sample_rate,
        .time_osr = kTime_osr,
        .freq_osr = kFreq_osr,
        .protocol = is_ft8 ? FTX_PROTOCOL_FT8 : FTX_PROTOCOL_FT4
    };

	// timestamp the packets
	// the time is shifted back by the time it took to capture these samples
	const int packet_time_ms = is_ft8 ? 15000 : 7500;
	const int raw_ms = (wallclock_day_ms / packet_time_ms) * packet_time_ms;

	// DT needs to be measured from the TRUE slot boundary (raw_ms), not
	// from wherever ft8_rx_buff_index actually got reset to 0 for this
	// slot -- that reset (ft8_rx()'s slot_time<500 branch) only runs on
	// its own ~500ms-granularity check plus real audio-callback timing,
	// so buffer index 0 can land up to ~500ms+ *after* the true boundary.
	// Measured live: this part is actually tiny in practice (~5-7ms), so
	// it's kept for correctness but isn't the real story below.
	const float dt_buffer_start_correction_sec = (ft8_rx_buff_start_ms >= 0) ?
		(ft8_rx_buff_start_ms - raw_ms) / 1000.0f : 0.0f;

	// The dominant DT error turned out to be a fixed offset inherent to
	// ft8_lib's own candidate-finder, not a timing bug in this app.
	// Proven by dumping the EXACT signal[]/num_samples buffer this
	// function is about to decode to a WAV file and running real jt9
	// (WSJT-X's own decoder) on the IDENTICAL samples -- same audio, same
	// moment, zero environmental confound. Across 10 paired captures
	// (n=288 jt9 decodes vs n=158 decodes here), jt9's DT clustered
	// zero-centered (mean 0.18s, median 0.1s, matching real WSJT-X/
	// WSJT-Z), while ft8_lib's own time_offset on the exact same audio
	// clustered ~0.65-0.7s higher (mean 0.83s, median 0.8s) and was
	// NEVER negative -- the formula itself already matches ft8_lib's own
	// reference demo tool exactly, so this is a real, consistent
	// difference in the two libraries' own window-alignment conventions,
	// not a bug to hunt further inside this app's own capture pipeline.
	// User: "the numbers are way off compared to what i see running
	// wsjt-x or wsjt-z... .7 .8 1.5 etc is not correct compared to wsjt
	// showing 0.1, 0.2, or 0.0." Calibrated empirically rather than
	// derived analytically from ftx_find_candidates()'s own internals
	// (a nontrivial correlation search) -- if a future ft8_lib update
	// changes its own windowing, this constant would need re-measuring
	// the same way (dump a buffer, compare against real jt9 on it).
	const float dt_algorithm_calibration_sec = -0.7f;

	const float dt_correction_sec = dt_buffer_start_correction_sec + dt_algorithm_calibration_sec;

	int i;
	char mycallsign_upper[20];
	char mycallsign[20];
	get_field_value("#mycallsign", mycallsign);
	for (i = 0; i < strlen(mycallsign); i++)
		mycallsign_upper[i] = toupper(mycallsign[i]);
	mycallsign_upper[i] = 0;

    // Scratch copy for successive interference cancellation -- signal
    // (ft8_rx_buffer) itself isn't touched, each pass works against the
    // residual with every candidate actually decoded so far subtracted
    // out (see sic_subtract_candidate()).
    if (num_samples > FT8_MAX_BUFF)
        num_samples = FT8_MAX_BUFF;
    memcpy(ft8_sic_residual, signal, num_samples * sizeof(float));

    // TEMPORARY, task #25 SIC validation only -- remove once done.
    // Captures the first 12 real decode cycles' raw audio to disk
    // (same technique as the earlier DT-calibration jt9 comparison),
    // so kMaxPasses=1 vs 3 can be re-run against IDENTICAL real audio
    // via the DECODEFILE test command below, rather than comparing
    // across two different moments of real traffic.
    {
        static int sic_dump_count = 0;
        if (sic_dump_count < 12) {
            char path[64];
            snprintf(path, sizeof(path), "/tmp/ft8_sic_%d_%d.wav", wallclock_day_ms, sic_dump_count);
            save_wav(signal, num_samples, sample_rate, path);
            ++sic_dump_count;
        }
    }

    // Successive interference cancellation: each additional pass rebuilds
    // the waterfall from the residual and searches it again, catching
    // weaker signals that were masked by a stronger overlapping one on
    // the first pass -- the real technique behind most of jt9/WSJT-X's
    // sensitivity advantage over a single-pass decoder (n=288 jt9
    // decodes vs n=158 here on identical audio, task #25); ft8_lib
    // itself has no SIC of its own. Stops early once a pass adds
    // nothing new, since further passes on an unchanged residual just
    // burn CPU for no gain -- current single-pass decode time
    // (793-891ms) leaves real headroom for a few extra passes within
    // the ~2s/15s-cycle real-time budget, but that budget still needs
    // confirming on real hardware once this is deployed.
    const int kMaxPasses = 3;

    // Hash table for decoded messages (to check for duplicates), shared
    // across all passes so a signal that survives incomplete
    // cancellation and reappears in a later pass's candidate list isn't
    // double-counted or subtracted twice.
    int num_decoded = 0;
    ftx_message_t decoded[kMax_decoded_messages];
    ftx_message_t* decoded_hashtable[kMax_decoded_messages];
    for (int i = 0; i < kMax_decoded_messages; ++i)
        decoded_hashtable[i] = NULL;

    int n_decodes = 0;
    int crc_mismatches = 0;

    for (int pass = 0; pass < kMaxPasses; ++pass){
    monitor_init(&mon, &mon_cfg);

    // Process the waveform data frame by frame - you could have a live loop here with data from an audio device
    for (int frame_pos = 0; frame_pos + mon.block_size <= num_samples; frame_pos += mon.block_size)
        monitor_process(&mon, ft8_sic_residual + frame_pos);

//    LOG(LOG_DEBUG, "Waterfall accumulated %d symbols\n", mon.wf.num_blocks);
//    LOG(LOG_INFO, "Max magnitude: %.1f dB\n", mon.max_mag);

    // Find top candidates by Costas sync score and localize them in time and frequency
    ftx_candidate_t candidate_list[kMax_candidates];
    int num_candidates = ftx_find_candidates(&mon.wf, kMax_candidates, candidate_list, kMin_score);

    int new_this_pass = 0;
    // Go over candidates and attempt to decode messages
    for (int idx = 0; idx < num_candidates; ++idx)
    {
        const ftx_candidate_t* cand = &candidate_list[idx];
        if (cand->score < kMin_score)
            continue;

        int freq_hz = lroundf((cand->freq_offset + (float)cand->freq_sub / mon.wf.freq_osr) / mon.symbol_period);
		//~ printf("freq_hz: (%d + %d / %d) / %f = %d\n", cand->freq_offset, cand->freq_sub, mon.wf.freq_osr, mon.symbol_period, freq_hz);
        float time_sec = (cand->time_offset + (float)cand->time_sub / mon.wf.time_osr) * mon.symbol_period
			+ dt_correction_sec;

        ftx_message_t message;
        ftx_decode_status_t status;
        if (!ftx_decode_candidate(&mon.wf, cand, kLDPC_iterations, &message, &status)){
            // printf("000000 %3d %+4.2f %4.0f ~  ---\n", cand->score, time_sec, freq_hz);
	    if (status.crc_calculated != status.crc_extracted)
		++crc_mismatches;
            //~ else if (status.ldpc_errors > 0)
                //~ LOG(LOG_DEBUG, "LDPC decode: %d errors\n", status.ldpc_errors);
            continue;
        }

        LOG(LOG_DEBUG, "Checking hash table for %4.1fs / %dHz [%d]...\n", time_sec, freq_hz, cand->score);
        int idx_hash = message.hash % kMax_decoded_messages;
        bool found_empty_slot = false;
        bool found_duplicate = false;
        do {
            if (decoded_hashtable[idx_hash] == NULL) {
                LOG(LOG_DEBUG, "Found an empty slot\n");
                found_empty_slot = true;
            }
            else if ((decoded_hashtable[idx_hash]->hash == message.hash) &&
			         (0 == memcmp(decoded_hashtable[idx_hash]->payload, message.payload, FTX_PAYLOAD_LENGTH_BYTES))) {
				//~ ftx_message_print(&message);
                LOG(LOG_DEBUG, "Found a duplicate\n");
                found_duplicate = true;
            }
            else {
                LOG(LOG_DEBUG, "Hash table clash!\n");
                // Move on to check the next entry in hash table
                idx_hash = (idx_hash + 1) % kMax_decoded_messages;
            }
        } while (!found_empty_slot && !found_duplicate);

        if (found_empty_slot) {
           // Fill the empty hashtable slot
           memcpy(&decoded[idx_hash], &message, sizeof(message));
           decoded_hashtable[idx_hash] = &decoded[idx_hash];
           ++num_decoded;
           ++new_this_pass;
           sic_subtract_candidate(&mon, cand, &message, !is_ft8, ft8_sic_residual, num_samples);

            char text[FTX_MAX_MESSAGE_LENGTH];
			ftx_message_offsets_t spans;
            ftx_message_rc_t unpack_status = ftx_message_decode(&message, &hash_if, text, &spans);
            if (unpack_status != FTX_MESSAGE_RC_OK)
                LOG(LOG_DEBUG, "Error [%d] while unpacking!", (int)unpack_status);

			//message_add(char *mode, unsigned int frequency, int outgoing, char *message);
			// TODO if allowed by settings:
			message_add("FT8", freq_hz, 0, text);

			char buf[64];
			const int message_type = ftx_message_get_i3(&message);
			//~ char type_utf8[4] = {0xE2, message_type ? 0x91 : 0x93, message_type ? 0xA0 + message_type - 1 : 0xAA, 0 };
			// DT (time delta from the ideal slot boundary, in seconds --
			// same field WSJT-X/other variants show) replaces cand->score
			// (an internal sync-confidence number, not a timing value --
			// not useful to an operator and not shown by any other WSJT
			// variant). time_sec was already computed above for exactly
			// this purpose but previously only reached a commented-out
			// troubleshooting printf (n1qm's note, now superseded).
			int prefix_len = 8 + snprintf(hmst_time_sprint(buf, raw_ms), sizeof(buf) - 8, " %+4.1f %+03d %4d ~ ", time_sec, cand->snr, freq_hz);
			int line_len = prefix_len + snprintf(buf + prefix_len, sizeof(buf) - prefix_len, "%s\n", text);
			if (message_type) // not type 0
				LOG(LOG_INFO, ">> %d %s\n", message_type, buf);
			else // type 0 : we care about the subtype (n3)
				LOG(LOG_INFO, ">> %d.%d %s\n", message_type, ftx_message_get_n3(&message), buf);
			text_span_semantic sem[FTX_MAX_MESSAGE_FIELDS + 4];
			memset(sem, 0, sizeof(sem));
			bool my_call_found = false;
			int calls_found = 0;
			int total_calls = message_callsign_count(&spans);
			int span_i = 0;
			int sem_i = 0;
			int col = 0;
			sem[sem_i].length = line_len;
			sem[sem_i++].semantic = STYLE_FT8_RX;
			sem[sem_i].length = 8;
			sem[sem_i++].semantic = STYLE_TIME;
			col = 8 + 6; // skip " DT " (1 space + 4-char DT + 1 space)
			sem[sem_i].start_column = col;
			sem[sem_i].length = 3;
			sem[sem_i++].semantic = STYLE_SNR;
			col += 4;
			sem[sem_i].start_column = col;
			sem[sem_i].length = 4;
			sem[sem_i++].semantic = STYLE_FREQ;

			for (; span_i < FTX_MAX_MESSAGE_FIELDS && sem_i < MAX_CONSOLE_LINE_STYLES &&
					spans.offsets[span_i] >= 0; ++span_i, ++sem_i) {
				sem[sem_i].start_column = prefix_len + spans.offsets[span_i];
				// each span ends where the next starts (ftx_message_offsets_t does not have lengths, so far)
				if (sem_i > 4) {
					sem[sem_i - 1].length = sem[sem_i].start_column - sem[sem_i - 1].start_column;
					//~ printf("length of span %d: %d - %d = %d\n", sem_i - 1, sem[sem_i].start_column, sem[sem_i - 1].start_column, sem[sem_i - 1].length);
				}
				if (spans.types[span_i] == FTX_FIELD_CALL) {
					// detect whether it's my callsign or the caller's
					char *call = text + spans.offsets[span_i];
					char *call_end = strchr(call, ' ');
					if (!call_end)
						call_end = call + strlen(call);
					assert(call_end);
					if (*call == '<')
						++call;
					if (*(call_end - 1) == '>')
						--call_end;
					//~ printf("considering call %d of %d: first %d chars of %s\n", calls_found, total_calls, call_end - call, call);
					if (!strncmp(call, mycallsign_upper, call_end - call)) {
						sem[sem_i].semantic = STYLE_MYCALL;
						my_call_found = true;
					} else if (!calls_found && total_calls > 1) {
						// the first callsign is the callee, unless it's a single-call message (such as CQ):
						// less interesting then, unless it's my call
						sem[sem_i].semantic = STYLE_CALLEE;
					} else {
						// otherwise the callsign is presumably the caller
						// (since we don't support multi-part messages yet)
						sem[sem_i].semantic = STYLE_CALLER;
					}
					++calls_found;
					continue; // with the for loop, so as to skip the next line below
				}
				sem[sem_i].semantic = kFieldType_style_map[spans.types[span_i]];
			}
			// set length of the last span (no next span, but null terminator in text)
			if (span_i > 0)
				sem[sem_i - 1].length = strlen(text + spans.offsets[span_i - 1]);
			write_console_semantic(buf, sem, sem_i);

			if (my_call_found)
				ft8_process(buf, FTX_CONTINUE_QSO);
			n_decodes++;
        }
    }
    monitor_free(&mon);
    LOG(LOG_DEBUG, "SIC pass %d: %d new decodes (total %d)\n", pass, new_this_pass, n_decodes);
    if (new_this_pass == 0)
        break;
    }
    //LOG(LOG_INFO, "Decoded %d messages\n", num_decoded);
    if (crc_mismatches)
        LOG(LOG_DEBUG, "%d CRC mismatches\n", crc_mismatches);

    hashtable_cleanup(10);

    return n_decodes;
}

// TEMPORARY, task #25 SIC validation only -- remove once done, along
// with the dump in sbitx_ft8_decode() above and the DECODEFILE command
// in cmd_exec(). Re-runs the real decoder (whatever kMaxPasses is
// currently compiled in) against a previously captured WAV file, so the
// same real audio can be decoded again under a different build without
// needing fresh live traffic.
int ft8_decode_file(const char *path){
	static float file_signal[FT8_MAX_BUFF];
	int num_samples = FT8_MAX_BUFF;
	int file_sample_rate = 0;
	if (load_wav(file_signal, &num_samples, &file_sample_rate, path) != 0){
		fprintf(stderr, "ft8_decode_file: failed to load %s\n", path);
		return -1;
	}
	return sbitx_ft8_decode(file_signal, num_samples);
}

static bool encode_xota() {
    const char *xota = field_str("xOTA");
    const char *xota_loc = field_str("LOCATION");
    if (!xota[0] || !xota_loc[0] || !strcmp(xota, "NONE"))
	return false;
    else {
	snprintf(ft8_xota_text, sizeof(ft8_xota_text), "%c%c %s", xota[0], xota[1], xota_loc);
	LOG(LOG_DEBUG, "%05d encode_xota %s '%s'\n", wallclock_day_ms % 60000, xota, ft8_xota_text);
	ftx_message_rc_t rc = ftx_message_encode_free(&ftx_xota_msg, ft8_xota_text);
	if (rc != FTX_MESSAGE_RC_OK)
	    LOG(LOG_INFO, "failed to encode xOTA message '%s': %d\n", ft8_xota_text, rc);
    }
    return true;
}

/*!
    Returns \c true if we have anything to send at this moment (is it the right time to start?)
    and updates ft8_pitch and is_cq.
    If ft8_tx_text is a CQ, then it also updates ft8_tx1st, ft8_cq_alt, ft8_xota and ftx_xota_msg
    according to current settings.
*/
static bool ftx_would_send() {
    ftx_update_clock();
    bool start = false;
    is_cq = !strncmp(ft8_tx_text, "CQ ", 3);
    bool is_ft4 = !strcmp(field_str("MODE"), "FT4");
    int slot_time = 0;

    ft8_pitch = field_int("TX_PITCH");

    // the FT8_TX1ST setting applies only to initiating a CQ call;
    // otherwise, leave ft8_tx1st as set earlier, e.g. in ft8_process()
    if (is_cq) {
	ft8_tx1st = !strcmp(field_str("FT8_TX1ST"), "ON");
	ft8_cq_alt = !strcmp(field_str("FT8_AUTO"), "CQ_ALT");
	ft8_xota = !strcasecmp(field_str("FT8_AUTO"), "xOTA");
    } else {
	ft8_xota = false;
    }

    if (is_ft4) {
	int two_slot_clock = wallclock_day_ms % 15000;
	int four_slot_clock = wallclock_day_ms % 30000;
	if (two_slot_clock < 7500) {
	    if (ft8_tx1st)
		start = true;
	} else {
	    if (!ft8_tx1st)
		start = true;
	}
	// if we have a timeslot based on even/odd setting, and we would otherwise send CQ,
	// then decide what to send, or to skip it, in case of xOTA or CQ_alt settings respectively
	if (start) {
	    if (is_cq && four_slot_clock > 10000) {
		// wait until next minute for CQ; either send ft8_xota_text instead, or stay silent
		if (ft8_xota) { // set from the setting, above
		    start = encode_xota(); // generate ftx_xota_msg
		} else if (ft8_cq_alt) {
		    start = false;
		}
	    } else {
		ft8_xota = false; // regular message this time
	    }
	}
    } else {
	int two_slot_clock = wallclock_day_ms % 30000;
	int four_slot_clock = wallclock_day_ms % 60000;
	if (two_slot_clock < 15000) {
	    if (ft8_tx1st)
		start = true;
	} else {
	    if (!ft8_tx1st)
		start = true;
	}
	// if we have a timeslot based on even/odd setting, and we would otherwise send CQ,
	// then decide what to send, or to skip it, in case of xOTA or CQ_alt settings respectively
	if (start) {
	    if (is_cq && four_slot_clock > 20000) {
		// wait until next minute for CQ; either send ft8_xota_text instead, or stay silent
		if (ft8_xota) { // set from the setting, above
		    start = encode_xota(); // generate ftx_xota_msg
		} else if (ft8_cq_alt) {
		    start = false;
		}
	    } else {
		ft8_xota = false; // regular message this time
	    }
	}
    }
    if (!start)
	ft8_xota = false;
    return start;
}

static void ftx_start_tx(int offset_ms){
	char buf[100];
	int freq = field_int("TX_PITCH");
	if (freq != ft8_pitch)
		ft8_pitch = freq;
	pthread_mutex_lock(&ft8_tx_state_mutex);
	ft8_tx_nsamples = sbitx_ftx_msg_audio(freq,  ft8_tx_buffer);

	snprintf(hmst_wallclock_time_sprint(buf), sizeof(buf) - 8, "  TX     %4d ~ %s\n",
		ft8_pitch, ft8_xota ? ft8_xota_text : ft8_tx_text);
	write_console(STYLE_FT8_TX, buf);
	message_add("FT8", ft8_pitch, 1, ft8_xota ? ft8_xota_text : ft8_tx_text);

	const int message_type = ftx_message_get_i3(&ftx_tx_msg);
	if (message_type) // not type 0
		LOG(LOG_INFO, "<< %d %s", message_type, buf);
	else // type 0 : we care about the subtype (n3)
		LOG(LOG_INFO, "<< %d.%d %s", message_type, ftx_message_get_n3(&ftx_tx_msg), buf);

	// start at the beginning if at all reasonable
	if (offset_ms < 1000)
		offset_ms = 0;
	ft8_tx_buff_index = offset_ms * 96;
	pthread_mutex_unlock(&ft8_tx_state_mutex);
	LOG(LOG_DEBUG, "%05d ftx_start_tx: starting @index %d based on offset_ms %d '%s'\n",
		wallclock_day_ms % 60000, ft8_tx_buff_index, offset_ms, ft8_xota ? ft8_xota_text : ft8_tx_text);
}

/*!
	Encode and schedule \a message for transmission, modulated on \a freq.
	It is picked up by ft8_poll to do the actual transmission.
	\a message may be anything: ft8_lib has to parse it and guess the message type to use.
	So it's better to call ft8_tx_3f(to, de, extra) in all programmatic cases,
	and use this function only when the user is doing the typing.
*/
void ft8_tx(char *message, int freq){
	char buf[64];

	for (int i = 0; i < strlen(message); i++)
		message[i] = toupper(message[i]);
	if (sbitx_ft8_encode(message) != FTX_MESSAGE_RC_OK) {
		LOG(LOG_INFO, "failed to encode: nothing to transmit\n");
		return;
	}

	strncpy(ft8_tx_text, message, sizeof(ft8_tx_text));
	const int message_type = ftx_message_get_i3(&ftx_tx_msg);
	ftx_would_send(); // update wallclock_day_ms, ft8_pitch, ft8_tx1st, ft8_cq_alt, ft8_xota, ft8_xota_text
	if (!freq)
		freq = ft8_pitch;
	snprintf(hmst_wallclock_time_sprint(buf), sizeof(buf) - 8, "  TX     %4d ~ %s\n", freq, ft8_xota ? ft8_xota_text : ft8_tx_text);
	write_console(STYLE_FT8_QUEUED, buf);
	if (message_type) // not type 0
		LOG(LOG_INFO, "<- %d %s", message_type, buf);
	else // type 0 : we care about the subtype (n3)
		LOG(LOG_INFO, "<- %d.%d %s", message_type, ftx_message_get_n3(&ftx_tx_msg), buf);

	//also set the times of transmission
	char str_tx1st[10], str_repeat[10];
	get_field_value_by_label("FT8_TX1ST", str_tx1st);
	get_field_value_by_label("FT8_REPEAT", str_repeat);
	int slot_second = time(NULL) % 15;

	//no repeat for '73'
	int msg_length = strlen(message);
	if (msg_length > 3 && !strcmp(message + msg_length - 3, " 73"))
		ft8_repeat = 1;
	else
		ft8_repeat = field_int("FT8_REPEAT");
	update_tx_active_field();

	LOG(LOG_DEBUG, "%05d ft8_tx '%s' even? %d ft8_cq_alt %d ft8_xota %d '%s'\n",
		wallclock_day_ms % 60000, ft8_tx_text, ft8_tx1st, ft8_cq_alt, ft8_xota, ft8_xota_text);
}

/*!
	Encode and schedule a message for transmission, composed from the 3 fields
	\a call_to (which may alternatively be things like "CQ", "CQ SOTA", ...),
	\a call_de, and \a extra (which is for grid, RST, RRnn, RRR, 73).
	It is picked up by ft8_poll to do the actual transmission.
	The encoding will be std if possible, falling back to nonstd otherwise,
	and then falling back to free text if all else fails.
*/
void ft8_tx_3f(const char* call_to, const char* call_de, const char* extra) {
	char buf[64];

	snprintf(ft8_tx_text, sizeof(ft8_tx_text), "%s %s %s", call_to, call_de, extra);
	if (sbitx_ft8_encode_3f(call_to, call_de, extra) != FTX_MESSAGE_RC_OK) {
		LOG(LOG_INFO, "failed to encode: nothing to transmit\n");
		return;
	}
	const int message_type = ftx_message_get_i3(&ftx_tx_msg);
	// nice idea to let the user edit the outgoing message right away... but we don't necessarily want to log it
	// field_set("TEXT", ft8_tx_text);
	ftx_would_send(); // update ft8_pitch, is_cq, ft8_tx1st, ft8_cq_alt, ft8_xota, ft8_xota_text
	snprintf(hmst_wallclock_time_sprint(buf), sizeof(buf) - 8, "  TX     %4d ~ %s\n", ft8_pitch, ft8_xota ? ft8_xota_text : ft8_tx_text);
	write_console(STYLE_FT8_QUEUED, buf);
	LOG(LOG_INFO, "<- %d.%c '%s' '%s' '%s'",
		message_type, message_type ? ' ' : '0' + ftx_message_get_n3(&ftx_tx_msg), call_to, call_de, extra);

	// no repeat for '73'
	if (!strcmp(extra, " 73"))
		ft8_repeat = 1;
	else
		ft8_repeat = field_int("FT8_REPEAT");
	update_tx_active_field();
}

void *ft8_thread_function(void *ptr){
	//wake up every 100 msec to see if there is anything to decode
	while(1){
		usleep(1000);

		if (!ft8_do_decode)
			continue;

		ft8_do_decode = 0;
		sbitx_ft8_decode(ft8_rx_buffer, ft8_rx_buff_index);
		//let the next batch begin
		ft8_rx_buff_index = 0;
	}
}

// the ft8 sampling is at 12000, the incoming samples are at
// 96000 samples/sec
void ft8_rx(int32_t *samples, int count) {

	bool is_ft4 = !strcmp(field_str("MODE"), "FT4");
	int decimation_ratio = 96000/12000;

	//if there is an overflow, then reset to the begining
	if (ft8_rx_buff_index + (count/decimation_ratio) >= FT8_MAX_BUFF){
		ft8_rx_buff_index = 0;
		printf("Buffer Overflow\n");
	}

	//down convert to 12000 Hz sampling rate
	for (int i = 0; i < count; i += decimation_ratio)
		ft8_rx_buffer[ft8_rx_buff_index++] = samples[i] / 200000000.0f;

	int time_was = wallclock_day_ms;
	ftx_update_clock();
	// we only need to check every half-second
	if (time_was / 500 == wallclock_day_ms / 500)
		return;

	int slot_time = wallclock_day_ms % 15000;
	int min_secs = 12000;
	int slot_time_decode = 13000;
	if (is_ft4) {
		slot_time = wallclock_day_ms % 7500;
		min_secs = 6000;
		slot_time_decode = 13000 / 2;
	}
//~ printf("time %d -> %d; slot %d; ft8_rx_buff_index %d\n", time_was % 60000, wallclock_day_ms % 60000, slot_time, ft8_rx_buff_index);

	if (slot_time < 500){
		ft8_rx_buff_index = 0;
		ft8_rx_buff_start_ms = wallclock_day_ms;
	}

	//we should have at least 6 or 12 seconds of samples to decode
	if (ft8_rx_buff_index >= 13 * min_secs && slot_time > slot_time_decode) {
		ft8_do_decode = 1;
//~ printf("ft8_rx decoding trigger index %d, clock %d, slot_time %d\n", ft8_rx_buff_index, wallclock_day_ms % 60000, slot_time);
	}
}

void ft8_poll(int tx_is_on){
	//if we are already transmitting, we continue
	//until we run out of ft8 sampels
	if (tx_is_on){
		//tx_off should not abort repeats from modem_poll, when called from here
		int ft8_repeat_save = ft8_repeat;
		if (ft8_tx_nsamples == 0){
			tx_off();
			ft8_repeat = ft8_repeat_save;
			// tx_off() -> modem_abort() -> ft8_abort() just zeroed
			// #tx_active along with ft8_repeat -- now that ft8_repeat
			// is restored, re-sync the field so it doesn't stay wrong
			// until the next real mutation
			update_tx_active_field();
		}
		return;
	}

	if (!ft8_repeat)
		return;

	//we poll for this only once every half-second
	//we are here only if we are rx-ing and we have a pending transmission

	if (ftx_would_send()) {
		const bool is_ft4 = !strcmp(field_str("MODE"), "FT4");

		// FT4: two transmissions take 15 secs; are we interested in the first slot or the second?
		// FT8: two transmissions take 30 secs; are we interested in the first slot or the second?
		const int slot_time = is_ft4 ? wallclock_day_ms % 7500 : wallclock_day_ms % 15000;

		// ftx_would_send() is true for this whole half-window, not just
		// its first instant -- ft8_poll() runs ~10x/sec (see modems.c),
		// so a message queued (e.g. F1 clicked) partway into an
		// already-open window used to fire right here, seeding
		// ftx_start_tx()'s buffer index from slot_time (seconds already
		// elapsed), keying up mid-burst instead of at the real slot
		// boundary. User: "the radio started to transmit as soon as i
		// clicked... it should have waited until the beginning of the
		// next 15 second [slot]." Gate on slot_time so we only ever key
		// up right at the true start of a window -- anything later just
		// defers to the next poll, which keeps returning true for the
		// rest of this window and then goes false until the next one
		// opens, arriving here again with a small slot_time right at
		// that real boundary. 200ms (2x the ~100ms poll interval, as
		// jitter margin) rather than the 1000ms ftx_start_tx() itself
		// uses to zero out a small offset -- other stations' decoders
		// are synced to the true slot start, so this needs to stay
		// tight, not just "close enough to not be considered a
		// mid-burst start." Comfortably under that 1000ms threshold
		// means a pass here always zeroes the buffer index too, i.e.
		// starts from sample 0, never partway into the waveform.
		if (slot_time < 200) {
			LOG(LOG_DEBUG, "%05d ft8_poll: tx_is_on %d ft8_tx_nsamples %d start '%s'\n",
				wallclock_day_ms % 60000, tx_is_on, ft8_tx_nsamples, ft8_xota ? ft8_xota_text : ft8_tx_text);
			ftx_start_tx(slot_time); // modulate audio at current frequency setting
			if (ft8_tx_nsamples)
				tx_on(TX_SOFT);
			ft8_repeat--;
			update_tx_active_field();
		}
	}
}

float ft8_next_sample(){
		float sample = 0;
		pthread_mutex_lock(&ft8_tx_state_mutex);
		if (ft8_tx_buff_index/8 < ft8_tx_nsamples){
			sample = ft8_tx_buffer[ft8_tx_buff_index/8]/7;
			ft8_tx_buff_index++;
		}
		else //stop transmitting ft8
			ft8_tx_nsamples = 0;
		pthread_mutex_unlock(&ft8_tx_state_mutex);
		return sample;
}

bool is_token_char(char ch) {
	switch(ch) {
		case 0: // quick check for terminator: faster than isalnum(), perhaps
			return false;
		case '+':
		case '-':
		case '/':
			return true;
		default:
			return isalnum(ch);
	}
}

// like strncpy, but skips <> brackets (as found in hashed callsigns),
// stops at the end of alphanumeric characters plus -+/, and returns count copied
int tokncpy(char *dst, const char *src, size_t dsize){
	if (*src == '<')
		++src;
	int c = 0;
	for (; c < dsize && is_token_char(*src); ++c)
		*dst++ = *src++;
	*dst = 0;
	return c;
}

/* these are used to process the current message */
static char m1[32], m2[32], m3[32], m4[32], signal_strength[10], mygrid[10],
	reply_message[100];
static int rx_pitch, confidence_score, msg_time;
static const char *call = NULL, *exchange = NULL,
	*report_send = NULL, *report_received = NULL, *mycall = NULL;

int ft8_message_tokenize(char *message){
	char *p;

	//tokenize the message
	p = strtok(message, " \r\n");
	if (!p) return -1;
	msg_time = atoi(p);

	p = strtok(NULL, " \r\n");
	if (!p) return -1;
	confidence_score = atoi(p);

	p = strtok(NULL, " \r\n");
	if (!p) return -1;
	strcpy(signal_strength, p);

	p = strtok(NULL, " \r\n");
	if (!p) return -1;
	rx_pitch = atoi(p);

	// we should get a tilde '~' now, but not if it comes from the zbitx front panel
	p = strtok(NULL, " \r\n");
	if (!p)
		return -1;
	if (!strcmp(p, "~"))
		p = strtok(NULL, " \r\n");

	if (!p) return -1;
	tokncpy(m1, p, sizeof(m1));

	p = strtok(NULL, " \r\n");
	if (!p) return -1;
	tokncpy(m2, p, sizeof(m2));

	p = strtok(NULL, " \r\n");
	if (p){
		tokncpy(m3, p, sizeof(m3));

		p = strtok(NULL, " \r\n");
		if (p)
			tokncpy(m4, p, sizeof(m4));
		else
			m4[0] = 0;
	}
	else
		m3[0] = 0;

	return 0;
}

void set_call_field(const char *s) {
	if (strcmp(s, "<...>") == 0)
		return;
	char call[16];
	strncpy(call, s, sizeof(call));
	field_set("CALL", trim_brackets(call));
}

/*!
	Decide whether to reply on the even or odd timeslot, based on
	the received second \a msg_second within the minute.
	(i.e. \a msg_second is from 0 to 59)
*/
static void set_reply_tx1st(int msg_second)
{
	// When replying to an FT8 message that started in the 0- or 30-second "even" timeslot,
	// send in the "odd" 15- or 45-second timeslot; and vice-versa.
	// FT4 is similar, except we have 8 slots per minute.
	const int slot_len = (!strcmp(field_str("MODE"), "FT4") ? 7500 : 15000);
	const int msg_ms = 1000 * msg_second;
	// integer division is truncation: round to the nearest timeslot instead
	const int slot_in_minute = (msg_ms + slot_len / 2) / slot_len;
	// time 0 is slot 0: that's even, set ft8_tx1st = 0 to reply in odd slot;
	// FT8 15 secs is slot 1: that's odd, set ft8_tx1st = 1 to reply in even slot;
	// FT8 22.5 secs is slot 3: that's odd, set ft8_tx1st = 1 to reply in even slot (e.g. 30 or 45 secs)
	ft8_tx1st = slot_in_minute % 2;
	LOG(LOG_DEBUG, "msg_second %d slot_in_minute %d odd? %d reply tx1st? %d\n", msg_second, slot_in_minute, slot_in_minute % 2, ft8_tx1st);
}

// this kicks stars a new qso either as a CQ message or
// as a reply to someone's cq or as a 'break' with signal report to
// a concluding qso
void ft8_on_start_qso(char *message){
	modem_abort();
	tx_off();
	call_wipe();
	set_reply_tx1st(msg_time % 100);
	set_field_int("rx_pitch", rx_pitch);

	if (!strcmp(m1, "CQ")){
		if (m4[0]){
			set_call_field(m3);
			field_set("EXCH", m4);
			field_set("SENT", signal_strength);
		}
		else {
			set_call_field(m2);
			field_set("EXCH", m3);
			field_set("SENT", signal_strength);
		}
		LOG(LOG_DEBUG, "ft8_on_start_qso CQ: rst s %s\n", signal_strength);
		sprintf(reply_message, "%s %s %s", call, mycall, mygrid);
	}
	//whoa, someone cold called us
	else if (!strcmp(m1, mycall)){
		if (!m2[0])
			return;
		set_call_field(m2);
		field_set("SENT", signal_strength);
		LOG(LOG_DEBUG, "ft8_on_start_qso cold call: rst s %s\n", signal_strength);
		//they might have directly sent us a signal report
		if (isalpha(m3[0]) && isalpha(m3[1]) && strncmp(m3,"RR",2)!=0){ // R- RR are not EXCH
			field_set("EXCH", m3);
			sprintf(reply_message, "%s %s %s", call, mycall, signal_strength);
		}
		else {
			field_set("RECV", m3);
			sprintf(reply_message, "%s %s R%s", call, mycall, signal_strength);
		}
	}
	else { //we are breaking into someone else's qso
		set_call_field(m2);
		if (isalpha(m3[0]) && isalpha(m3[1]) && strncmp(m3,"RR",2)!=0){ // R- RR are not EXCH
			field_set("EXCH", m3); // the gridId is valid - use it
		} else {
			field_set("EXCH", "");
		}
		field_set("SENT", signal_strength);
		LOG(LOG_DEBUG, "ft8_on_start_qso break-in: rst s %s\n", signal_strength);
		sprintf(reply_message, "%s %s %s", call, mycall, signal_strength);
	}
	field_set("NR", mygrid);
	ft8_tx(reply_message, ft8_pitch);
}

void ft8_on_signal_report(){
	set_call_field(m2);
	if (m3[0] == 'R'){
		//skip the 'R'
		field_set("RECV", m3+1);
		ft8_tx_3f(call, mycall, "RR73");
	}
	else{
		field_set("RECV", m3);
		// in case ft8_on_start_qso() was not called: ensure that we send some numeric signal report
		if (!field_str("SENT")[0]) {
			field_set("SENT", signal_strength);
			report_send = field_str("SENT");
		}
		char report[5];
		snprintf(report, sizeof(report), "R%s", report_send);
		ft8_tx_3f(call, mycall, report);
	}

	//Disabled this because of early logging - W9JES
	//enter_qso();
}

/*!
	start a QSO: call the callsign specified by the "CALL" field,
	based on a previous selected message that occurred at time \a sel_time.
	The "SENT" field may hold previously-observed RST,
	and "EXCH" may hold the recipient's grid.
*/
void ft8_call(int sel_time) {

	call = field_str("CALL");
	if (!call[0]) {
		printf("CALL field empty: nobody to call\n");
		return;
	}

	modem_abort();
	tx_off();

	exchange = field_str("EXCH");
	report_send = field_str("SENT");
	mycall = field_str("MYCALLSIGN");
	// initial pitch; but it can also be adjusted between timeslots
	// (audio is re-generated in ftx_start_tx())
	ft8_pitch = field_int("TX_PITCH");
	//use only the first 4 letters of the grid
	strcpy(mygrid, field_str("MYGRID"));
	mygrid[4] = 0;
	field_set("NR", mygrid);
	set_reply_tx1st(sel_time % 100);
	ft8_tx_3f(call, mycall, mygrid);
}

/*!
	Start or continue a QSO as appropriate for the \a message:
	\a operation may be FTX_START_QSO or FTX_CONTINUE_QSO
	This should mostly not be used, because it throws away information that we already have:
	if \a message came from ft8_lib, we also have spans to identify the fields;
	or if we want to start a QSO, call ft8_call() above (which depends
	on fields containing information that we already have).
	The remaining legitimate usecase is only when the user types the message in the "TEXT" field.
*/

// Pushes the RX audio-pitch field so the web UI's waterfall overlay
// moves its RX (cyan) line to match -- the client does this itself
// when the user clicks a decoded line directly (see FT8_message_
// chosen() in index.html), but there's no click at all when the
// auto-responder answers a call to us on its own, so that path needs
// the server to push it instead.
static void ft8_set_rx_pitch_field(int pitch){
	char pitch_str[8];
	snprintf(pitch_str, sizeof(pitch_str), "%d", pitch);
	field_set("PITCH", pitch_str);
}

void ft8_process(char *message, ftx_operation operation){
	char buff[100], reply_message[100], *p;
	int auto_respond = 0;

	printf("ft8_process:%d[%s]\n", operation, message);


	if (ft8_message_tokenize(message) == -1)
		return;

	call = field_str("CALL");
	exchange = field_str("EXCH");
	report_send = field_str("SENT");
	report_received = field_str("RECV");
	mycall = field_str("MYCALLSIGN");
	ft8_pitch = field_int("TX_PITCH");
	// if FT8_AUTO is not OFF, it's ON or one of the others: automation is expected
	if (strcmp(field_str("FT8_AUTO"), "OFF"))
		auto_respond = 1;

	//use only the first 4 letters of the grid
	strcpy(mygrid, field_str("MYGRID"));
	mygrid[4] = 0;

	//we can start call in reply to a cq, cq dx or anyone else ending the call
	if (operation == FTX_START_QSO){
		ft8_set_rx_pitch_field(rx_pitch);
		ft8_on_start_qso(message);
		return;
	}

	// see if you are on auto responder, the logger is empty and we are the called party
	if (auto_respond && !strlen(call) && !strcmp(m1, mycall)){
		// Someone answered our own CQ -- move the RX line (client's
		// waterfall overlay) to their frequency automatically, same as
		// clicking their decode line would, since no click ever happens
		// on this path (the auto-responder detected and is replying to
		// them without any user interaction at all).
		ft8_set_rx_pitch_field(rx_pitch);
		ft8_on_start_qso(message);
		return;
	}

	//by now, any message that comes to us should have our callsign as m1
	if (strcmp(m1, mycall)){
		printf("FT8: Not a message for %s\n", mycall);
		return;
	}

	if (!strcmp(m3, "73")){
		ft8_abort();
		enter_qso(); // W9JES
		ft8_repeat = 0;
		update_tx_active_field();
		return;
	}

	//the other station has sent either an RRR or an RR73
	//this maybe arriving after we have cleared the log
	//we don't check it against any fields of the logger
	if (!strcmp(m3, "RR73") || !strcmp(m3, "RRR")){
		ft8_tx_3f(m2, mycall, "73");
		enter_qso();
		call_wipe();
		ft8_repeat = 1;
		update_tx_active_field();
	}

	//beyond this point, we need to have a call filled up in the logger
	if (!strlen(call))
		return;

	//this is a signal report, at times, other call can just send their sig report
	if (m3[0] == '-' || (m3[0] == 'R' && m3[1] == '-') || m3[0] == '+' || (m3[0] == 'R' && m3[1] == '+')){
		ft8_on_signal_report();
		return;
	}
}

void ft8_init(){
	ft8_rx_buff_index = 0;
	ft8_tx_buff_index = 0;
	ft8_tx_nsamples = 0;
	hashtable_init();
	pthread_create( &ft8_thread, NULL, ft8_thread_function, (void*)NULL);
	memset(ft8_rx_buffer, 0, sizeof(ft8_rx_buffer));
	memset(ft8_tx_buffer, 0, sizeof(ft8_tx_buffer));
	memset(ft8_tx_text, 0, sizeof(ft8_tx_text));
	memset(ft8_xota_text, 0, sizeof(ft8_xota_text));
}

void ft8_abort(){
	ft8_tx_nsamples = 0;
	ft8_repeat = 0;
	update_tx_active_field();
}

int ft8_is_repeating(){
	return ft8_repeat > 0;
}
