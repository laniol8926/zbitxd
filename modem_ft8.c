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
ftx_message_t ftx_tx_msg;
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
// Real gap, live (2026-08-24), user's own question surfaced it: the FT8
// exchange state (m1/m2/m3/m4 via ft8_message_tokenize()'s strtok() --
// itself non-reentrant -- call, the CALL field, ft8_repeat, etc.) is
// shared, file-static mutable state, read and written from *two*
// threads with nothing synchronizing them: the dedicated FT8 decode
// thread (sbitx_ft8_decode() -> ft8_process(), for a message it
// auto-detects addressed to us -- e.g. a station's reply landing right
// as we're giving up on them) and the main thread (ft8_poll(), every
// ~100ms per modems.c, *and* pre_ft8_check()/cmd_exec() -> ft8_process(),
// reached via ui_tick() draining queued client commands -- e.g. Auto
// Answer's own "FT8_check" for a freshly-picked target). ft8_tx_state_
// mutex above only ever covered TX audio buffer state, nothing about
// the exchange itself. Two of these landing at the same real moment
// could genuinely interleave and corrupt each other's state -- not a
// clean "one wins," but potential garbage for both. Both ft8_process()
// (thin wrapper around the real ft8_process_impl(), see its own
// comment) and ft8_poll() take this for their entire body so the two
// threads can never run either one concurrently.
static pthread_mutex_t ft8_process_mutex = PTHREAD_MUTEX_INITIALIZER;
static int ft8_do_decode = 0;
// Real report, live (2026-08-23), user's own screenshots (Band
// Activity/CQ Panel showing e.g. "WV2B K4MFC EM73" and "CQ KD4KKF
// EM63" each appearing twice, same embedded slot timestamp, same SNR
// -- a genuine duplicate, not two different real transmissions):
// ft8_rx()'s decode trigger below re-checks every ~500ms for the
// entire 13-15s tail of a slot, and a real decode pass can legitimately
// take 1.6-3s (SIC/AP passes) -- longer than that 500ms gate. A second
// ft8_rx() call landing before the first decode (and its natural
// buffer reset at the *next* slot's slot_time<500) finishes re-armed
// ft8_do_decode again, so ft8_thread_function() ran a second decode
// pass over the *same* audio a couple seconds later.
// decoded_hashtable in sbitx_ft8_decode() is local to each call (reset
// fresh every time), so it gives zero protection across two separate
// calls like this -- only within one. Tracks which slot (by its own
// ft8_rx_buff_start_ms, which only changes on a genuine new slot) has
// already been triggered, so a slot can only ever queue one decode.
// Callsign->grid directory hand-off (see this block's use in
// sbitx_ft8_decode(), and callsign_grid_ensure_table()'s own comment,
// logbook.c) -- same mutex-protected-queue pattern as ft8_process_mutex
// above, for the same reason: logbook.c's db handle has no existing
// precedent for cross-thread access, and this codebase already hit a
// real cross-thread race in this exact subsystem once before. Pushed
// from the FT8 decode thread, drained (and only there does the actual
// sqlite write happen, via callsign_grid_set()) from the main thread's
// own ui_tick(). Capacity comfortably above kMax_decoded_messages (50)
// -- a full decode pass can't produce more CQ-with-grid candidates than
// that in one slot.
#define FT8_GRID_QUEUE_CAP 64
struct ft8_grid_queue_entry { char callsign[16]; char grid[8]; };
static struct ft8_grid_queue_entry ft8_grid_queue[FT8_GRID_QUEUE_CAP];
static int ft8_grid_queue_head = 0, ft8_grid_queue_count = 0;
static pthread_mutex_t ft8_grid_queue_mutex = PTHREAD_MUTEX_INITIALIZER;

// FT8 decode thread only. Never blocks waiting on the main thread --
// silently drops on overflow rather than stalling decoding; a dropped
// grid just gets re-queued the next time that station calls CQ.
static void ft8_grid_queue_push(const char *callsign, int callsign_len, const char *grid){
	pthread_mutex_lock(&ft8_grid_queue_mutex);
	if (ft8_grid_queue_count < FT8_GRID_QUEUE_CAP){
		int idx = (ft8_grid_queue_head + ft8_grid_queue_count) % FT8_GRID_QUEUE_CAP;
		int n = callsign_len < (int)sizeof(ft8_grid_queue[idx].callsign) - 1 ?
			callsign_len : (int)sizeof(ft8_grid_queue[idx].callsign) - 1;
		memcpy(ft8_grid_queue[idx].callsign, callsign, n);
		ft8_grid_queue[idx].callsign[n] = 0;
		strncpy(ft8_grid_queue[idx].grid, grid, sizeof(ft8_grid_queue[idx].grid) - 1);
		ft8_grid_queue[idx].grid[sizeof(ft8_grid_queue[idx].grid) - 1] = 0;
		ft8_grid_queue_count++;
	}
	pthread_mutex_unlock(&ft8_grid_queue_mutex);
}

// Main thread only (ui_tick(), sbitx_daemon.c) -- the only thread ever
// allowed to touch logbook.c's db handle.
void ft8_grid_queue_drain(void){
	while (1){
		struct ft8_grid_queue_entry e;
		pthread_mutex_lock(&ft8_grid_queue_mutex);
		if (ft8_grid_queue_count == 0){
			pthread_mutex_unlock(&ft8_grid_queue_mutex);
			break;
		}
		e = ft8_grid_queue[ft8_grid_queue_head];
		ft8_grid_queue_head = (ft8_grid_queue_head + 1) % FT8_GRID_QUEUE_CAP;
		ft8_grid_queue_count--;
		pthread_mutex_unlock(&ft8_grid_queue_mutex);
		callsign_grid_set(e.callsign, e.grid);
	}
}

static int ft8_decode_triggered_for_ms = -1;
static int ft8_do_tx = 0;
static int ft8_pitch = 0;
// number of repetitions left for the current message, counting down from the user setting
static int ft8_repeat = 5;
// see ft8_suspend()/ft8_resume() further down
static int ft8_tx_suspended = 0;
static pthread_t ft8_thread;
static bool is_cq = false; // is ft8_tx_text a CQ?
static bool ft8_tx1st = true;
// Auto CQ mode (FT8_AUTO=="AUTOCQ"): ft8_autocq_running marks the whole
// "keep calling CQ until answered or aborted, and keep going after each
// QSO" session, independent of ft8_repeat's own per-message countdown
// (which ft8_poll() refuses to let expire while a CQ call is under way
// in this mode -- see there). ft8_autocq_resume_pending bridges the gap
// between a completed QSO (ft8_process()'s "73"/"RR73" branches) and
// the next ft8_poll() cycle actually re-queuing CQ -- can't requeue
// immediately from ft8_process() itself when a courtesy "73" still
// needs to go out first, so it's deferred to the next idle poll instead.
static bool ft8_autocq_running = false;
static bool ft8_autocq_resume_pending = false;
// Real report, live (2026-08-22): the RX Frequency panel (client) switched
// to the CQ panel the instant the operator's closing "73" started sending,
// not once it actually finished -- because enter_qso()/call_wipe() used to
// run synchronously right where the closing "73" gets queued (ft8_tx_3f()
// only enqueues it; the real over-the-air transmission is the *next* TX
// slot), logging/clearing the exchange before any RF for it had even gone
// out. Also silently broke the "other station didn't get my 73, re-sends
// RR73, I should be able to see that exchange again" case: call_wipe()
// already having run meant CALL was empty by the time a resend's RR73
// arrived, so the client's own #CALL-gated re-display of RX Frequency
// never got the state it needed to recognize the resend as the same
// exchange. Same bridge-the-gap pattern as ft8_autocq_resume_pending right
// above: set here, consumed by ft8_poll() once ft8_repeat naturally
// reaches 0 (the courtesy "73" -- always a single shot, see its own
// comment -- has actually finished transmitting), not the instant it's
// merely queued.
static bool ft8_qso_log_pending = false;

// Real report, live (2026-08-24), same bridge-the-gap pattern as
// ft8_qso_log_pending right above -- and the exact same class of bug
// that flag was invented to fix, just in a different spot: ft8_poll()'s
// "an in-QSO reply ran out of retries" give-up branch used to call
// ft8_finalize_pending_qso()/call_wipe() *immediately*, in the same
// instant it queues the last (already-exhausted) retry's own
// transmission -- not once that transmission has actually gone out.
// CALL going empty now client-side clears RX Frequency (see the 'CALL'
// case in web/index.html) -- so this early call_wipe() cleared the
// panel *before* the final retry had even transmitted. User's own
// report: "the clear of the rx frequency panel happened when the
// counter got to zero but before the actual transmission occurred."
// Set here instead, consumed by ft8_poll() once ft8_repeat naturally
// reaches 0 on a *later* poll (i.e. once tx_is_on's own early return
// confirms we're not still mid-transmission).
static bool ft8_give_up_pending = false;

// Give-up grace period: real report, live (2026-08-27), KE8ESJ -- our
// own reply's repeat budget (FT8_REPEAT) hitting 0 immediately called
// call_wipe() (via ft8_give_up_pending above), but the other station's
// reply was already in flight and decoded only ~15s later -- well
// within FT8's own normal cadence, not a slow/late response. CALL was
// already wiped by then, so their perfectly on-time reply got
// misclassified by ft8_process_impl()'s auto-respond gate as a *fresh*
// cold call instead of a continuation, restarting the exchange instead
// of just continuing it. One more full receive window before actually
// giving up (finalizing/wiping) gives a normal-speed reply a real
// chance to be recognized for what it is. Only applies to the
// is_cq==false give-up path (our own reply exhausting) -- the CQ's own
// give-up (is_cq==true, ft8_autocq_stop() below) never wipes CALL in
// the first place (nobody had answered yet), so there's no wipe race
// to guard against there.
// Known limitation, same as the rest of this file's wallclock_day_ms-
// based timing (it resets to 0 at local midnight, nothing here accounts
// for that): a deadline computed within the last 16s of the day wraps
// via the modulo below to a small value already less than the current
// wallclock_day_ms, firing the grace check immediately instead of after
// a real 16s wait -- degrades to this file's original immediate-give-up
// behavior for that one ~16s window per day, not stuck pending forever
// (the alternative of not wrapping at all).
static bool ft8_give_up_grace_pending = false;
static int ft8_give_up_grace_deadline_ms = 0;
#define FT8_GIVE_UP_GRACE_MS 16000 // one FT8 slot (15s) + margin

// Real regression, caught live (2026-08-23) right after first adding
// the RRR/RR73 dedup fix below: reusing RECV to detect a repeated
// RRR/RR73 clobbered it with the literal string "RRR"/"RR73" -- RECV
// is also the *real* received-signal-report field logbook_add() writes
// out, so the logged QSO's RST ended up as "RRR" instead of the actual
// numeric report from earlier in the exchange. Tracks the callsign
// we've already sent a courtesy "73" to instead, entirely separate
// from RECV. Reset in set_call_field() (called at the start of every
// genuinely new exchange, cold-call or otherwise) so a later, real
// exchange with the same station isn't incorrectly blocked.
static char ft8_courtesy_73_sent_to[16] = "";

static const int kMin_score = 10; // Minimum sync score threshold for candidates
// Matched to ft8_lib's own reference demo tool (ft8_lib/demo/decode_ft8.c
// uses 140/25) -- this app had been running slightly below that. Part of
// closing the sensitivity gap vs jt9/WSJT-X on identical audio (task #25,
// n=288 jt9 decodes vs n=158 here); real headroom for this exists since
// current decode time (793-891ms) is well under the ~2s/15s-cycle budget.
static const int kMax_candidates = 140;
static const int kLDPC_iterations = 25;

// Real task #25 ask, from a real QSO-reliability concern: with all
// other FT8 panels hidden during a QSO (user's own established
// preference), the only decode that actually matters right now is the
// other station's own reply -- yet ftx_find_candidates() still spends
// its fixed kMax_candidates heap budget across the whole band by
// default, letting unrelated traffic elsewhere in the passband crowd
// out a weak-but-real reply (a real, measured failure mode this same
// session -- see project_ft8_ldpc_sensitivity memory). qso_lock_freq_hz
// adds a small *supplementary* search around a known frequency once one
// is set, run alongside (never instead of) the normal full-band search
// -- see its use at the ftx_find_candidates() call site for why it
// can't just restrict the main search instead (breaks SIC). < 0 means
// unlocked (no supplementary search, unchanged normal behavior).
static float qso_lock_freq_hz = -1.0f;

// Search margin either side of qso_lock_freq_hz, in Hz. FT8's own
// signal footprint is 8 tones * 6.25Hz = 50Hz, but that's already
// handled by ftx_find_candidates()'s own inner num_tones bounds check
// on *each* candidate base frequency it tries -- this margin only
// needs to cover how far the other station's own base tone frequency
// could plausibly have drifted since we last decoded them, not the
// signal's own width on top of that. User's own real-world judgment,
// after an initial +/-25Hz proposal: "drift is real but [not] that
// much. then somebody's got a problem" -- kept tight and easy to
// widen later if real QSO testing shows genuine replies being missed.
static const float kQSO_lock_margin_hz = 10.0f;

// Small dedicated heap size for the supplementary lock-window search --
// this window is narrow enough (a handful of freq_offset bins) that it
// will never have anywhere near kMax_candidates worth of real distinct
// signals in it; kept small on purpose so a locked search can't itself
// start crowding out real candidates found by the main search once
// merged together.
static const int kQSO_lock_extra_candidates = 8;

void ft8_set_qso_lock(float freq_hz){
	qso_lock_freq_hz = freq_hz;
}

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
	Encode ftx_tx_msg payload onto audio carrier \a freq and output to \a signal.
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
        ft4_encode(ftx_tx_msg.payload, tones);
    else
        ft8_encode(ftx_tx_msg.payload, tones);

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
        wallclock_day_ms % 60000, (is_ft4 ? "FT4" : "FT8"), ft8_tx_text, num_tones,
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
	// TEMPORARY: diagnostic for why SIC found zero new decodes on real
	// audio -- restore to LOG_DEBUG (or drop) after.
	LOG(LOG_INFO, "SIC subtract: freq %dHz offset %d samples [%d,%d) scale %f\n",
		freq_hz, sample_offset, start, end, scale);

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

	// AP (a-priori) decoding: WSJT-X/jt9's own single-pass decoder gets
	// most of its real sensitivity advantage this way, not from
	// successive interference cancellation alone (confirmed by reading
	// its actual source, lib/ft8/ft8b.f90/ft8_decode.f90, in
	// wsjt-z-2.0.18 -- task #25). When a candidate fails a normal blind
	// LDPC decode, retried with the CALL_TO field's bits forced to
	// "addressed to my own callsign" -- the single highest-value
	// hypothesis, since those are the messages we most need to hear
	// (replies to our own CQ, or continuing an existing QSO). A wrong
	// guess is still caught by the normal CRC check afterward, same as
	// any other decode attempt, so this can't produce false decodes,
	// only recover real ones a blind decode missed.
	//
	// Built once per decode cycle (not per candidate/pass) since
	// mycallsign doesn't change mid-cycle -- ftx_message_encode_std()
	// packs "CALL_TO CALL_DE EXTRA" into 29+29+16+3 bits (see
	// message.c's own comment); only the first 29 bits (CALL_TO) are
	// real here, the placeholder DE callsign/grid are discarded.
	uint8_t ap_known_bits[29];
	bool ap_available = false;
	if (mycallsign_upper[0]) {
		ftx_message_t ap_msg;
		if (ftx_message_encode_std(&ap_msg, &hash_if, mycallsign_upper, "K1ABC", "FN00") == FTX_MESSAGE_RC_OK) {
			for (int b = 0; b < 29; ++b)
				ap_known_bits[b] = (ap_msg.payload[b / 8] >> (7 - (b % 8))) & 1;
			ap_available = true;
		}
	}

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

    // Find top candidates by Costas sync score and localize them in time and frequency.
    //
    // Real task #25 finding, caught live during testing: this MUST stay
    // a full-band search, not restricted to qso_lock_freq_hz's window --
    // successive interference cancellation (below) only works because
    // this search finds and later subtracts *every* real signal in the
    // capture, including ones nowhere near the locked frequency. A
    // stronger nearby signal masking the locked one can only get out of
    // the way once it's itself been found and cancelled; narrowing this
    // search to just the lock window means that masking signal is never
    // found at all, and the very signal the lock exists to protect stays
    // masked forever -- confirmed live: a real signal at a known,
    // exactly-correct locked frequency decoded 0 times with the naive
    // restrict-everything version of this, on a file that decoded it
    // fine unlocked.
    ftx_candidate_t candidate_list[kMax_candidates + kQSO_lock_extra_candidates];
    int num_candidates = ftx_find_candidates(&mon.wf, kMax_candidates, candidate_list, kMin_score, 0, mon.wf.num_bins);

    // qso_lock_freq_hz's own comment above explains why this exists.
    // Run as a small, *additional* narrow-window search on top of the
    // full one above (never instead of it, see that comment) -- this is
    // what actually protects the locked frequency from losing its
    // fixed-size heap slot to unrelated candidates elsewhere in the
    // band, without breaking SIC's need to see the whole capture.
    if (qso_lock_freq_hz >= 0.0f){
        int center_bin = lroundf(qso_lock_freq_hz * mon.symbol_period);
        int margin_bins = (int)ceilf(kQSO_lock_margin_hz * mon.symbol_period);
        int lock_lo = center_bin - margin_bins;
        int lock_hi = center_bin + margin_bins;
        ftx_candidate_t lock_candidates[kQSO_lock_extra_candidates];
        int n_lock = ftx_find_candidates(&mon.wf, kQSO_lock_extra_candidates, lock_candidates, kMin_score, lock_lo, lock_hi);
        int n_merged = 0;
        for (int li = 0; li < n_lock; ++li){
            bool already_present = false;
            for (int mi = 0; mi < num_candidates; ++mi){
                if (candidate_list[mi].time_offset == lock_candidates[li].time_offset &&
                    candidate_list[mi].freq_offset == lock_candidates[li].freq_offset &&
                    candidate_list[mi].time_sub == lock_candidates[li].time_sub &&
                    candidate_list[mi].freq_sub == lock_candidates[li].freq_sub){
                    already_present = true;
                    break;
                }
            }
            if (!already_present){
                candidate_list[num_candidates++] = lock_candidates[li];
                ++n_merged;
            }
        }
        // Only logged when the supplementary search actually rescues a
        // candidate the main search's own heap would otherwise have
        // dropped -- real signal that the lock mechanism did something,
        // not routine per-cycle noise (should be rare on a normally-
        // loaded band, more common on a busy one -- see this feature's
        // own real-world verification note in project_ft8_ldpc_sensitivity
        // memory).
        if (n_merged > 0)
            LOG(LOG_INFO, "qso_lock: rescued %d candidate(s) near %.1fHz that the main search's heap would have dropped\n",
                n_merged, qso_lock_freq_hz);
    }

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
            // Blind decode failed -- retry once assuming this candidate
            // is addressed to us (CALL_TO = mycallsign) before giving up
            // on it entirely. See ap_known_bits' own comment above.
            //
            // Real task #25 finding (2026-08-20): a second AP hypothesis
            // (CQ-pattern, jt9's iaptype=1) was tried here and reverted --
            // 550 real attempts across 3 real captures, 0 recoveries.
            // Combined with this original mycall-only hint *also* having
            // shown zero measured effect in the earlier session, two
            // independently-implemented AP hypotheses both landing on
            // zero is a real signal: a candidate that survives neither
            // blind decode nor a correct, well-targeted hint is more
            // likely a genuinely bad *candidate* (poor time/frequency
            // localization from ftx_find_candidates() itself) than a
            // good one just missing the right guess. Investigating
            // ftx_find_candidates() directly is the follow-up, not more
            // AP hypothesis variety -- see [[project_ft8_ldpc_sensitivity]].
            if (!ap_available || !ftx_decode_candidate_ap(&mon.wf, cand, kLDPC_iterations,
                    ap_known_bits, 29, &message, &status)){
#ifdef FTX_OSD_FALLBACK
                // OSD fallback: task #25's highest-measured remaining lever
                // (+36% recovery on real candidates that survived neither
                // blind nor AP decode -- see project_ft8_ldpc_sensitivity
                // memory). Needs no a-priori guess, unlike AP, so it's tried
                // unconditionally here rather than gated by ap_available.
                if (!ftx_decode_candidate_osd(&mon.wf, cand, &message, &status)){
#endif
                // printf("000000 %3d %+4.2f %4.0f ~  ---\n", cand->score, time_sec, freq_hz);
	        if (status.crc_calculated != status.crc_extracted)
		    ++crc_mismatches;
                //~ else if (status.ldpc_errors > 0)
                    //~ LOG(LOG_DEBUG, "LDPC decode: %d errors\n", status.ldpc_errors);
                continue;
#ifdef FTX_OSD_FALLBACK
                }
                LOG(LOG_INFO, "osd_decode: recovered %4.1fs / %dHz [%d] that blind+AP decode missed\n",
                    time_sec, freq_hz, cand->score);
#endif
            }
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
            if (unpack_status != FTX_MESSAGE_RC_OK){
                // Real report, live (2026-08-22): Band Activity showed a row
                // with a real time/DT/SNR/frequency label and nothing after
                // it -- CRC passed (this candidate already made it into
                // found_empty_slot), but ftx_message_decode() itself failed
                // to render *this* payload to text (same family of failure
                // already guarded against inside ftx_decode_candidate_osd()
                // for i3=6/7/3, see decode.c's own comment -- but that guard
                // only covers OSD's *own* internal unpack call; this is a
                // second, separate unpack of the same message.payload done
                // unconditionally for every candidate regardless of which
                // decode path found it, and had no equivalent check). This
                // used to just log and fall through anyway with `text` left
                // however ftx_message_decode() happened to leave it (empty,
                // in practice) -- displaying, logging, and QSO-processing a
                // message with no actual content. Skipping display entirely
                // now; the SIC subtraction and hashtable/dedup bookkeeping
                // above already ran and are left as-is (still correct
                // regardless of whether this candidate's text can be shown).
                LOG(LOG_DEBUG, "Error [%d] while unpacking!", (int)unpack_status);
                continue;
            }
            // Real report, live (2026-08-22), same incident as the guard
            // just above -- that one wasn't the whole story. Confirmed by
            // reading ftx_message_decode() itself: FTX_MESSAGE_TYPE_FREE_TEXT
            // and FTX_MESSAGE_TYPE_TELEMETRY *unconditionally* return
            // FTX_MESSAGE_RC_OK regardless of what they actually produced --
            // the unpack_status check above can never catch a bad decode of
            // either type. ftx_message_decode_free() in particular ends with
            // strcpy(text, trim(c14)) -- a garbage/CRC-aliased payload of
            // this type can legitimately trim down to nothing (e.g. an
            // all-blank c14), leaving text empty with rc still OK. Confirmed
            // this was still reaching the client after the first guard went
            // out: the label showed real time/DT/SNR/frequency, still
            // nothing after it.
            if (text[0] == 0)
                continue;

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

			// Callsign->grid directory feed (persistent, see
			// callsign_grid_ensure_table()'s own comment, logbook.c) --
			// protocol-level, not a rendered-text heuristic: unpackgrid()
			// (ft8_lib/ft8/message.c) only ever sets types[2]==FTX_FIELD_GRID
			// for a genuine decoded 4-char grid, never for RR73/RRR/73 (those
			// get FTX_FIELD_TOKEN) or a signal report (FTX_FIELD_RST) -- can't
			// alias the way a string-shape check (e.g. "RR73" incidentally
			// matching a grid's own A-R/A-R/0-9/0-9 pattern) can. Deliberately
			// scoped to CQ only (types[0] a token, not a real callsign -- a
			// reply-to-CQ's types[0] is FTX_FIELD_CALL, the callee): the CQ is
			// the one unambiguous case of a station broadcasting its own grid
			// to nobody in particular. FT8 decoding runs on its own thread
			// (ft8_thread_function) -- logbook.c's db handle is only ever
			// touched from the main thread (no existing precedent for
			// cross-thread sqlite access in this codebase, and this project
			// already hit a real cross-thread race in this exact subsystem
			// once before, see ft8_process_mutex's own comment above) -- so
			// this only ever queues the candidate; ft8_grid_queue_drain()
			// (main thread, ui_tick()) is what actually writes it.
			if (span_i >= 3 && spans.offsets[2] >= 0 && spans.types[2] == FTX_FIELD_GRID &&
					spans.offsets[1] >= 0 && spans.types[1] == FTX_FIELD_CALL &&
					spans.offsets[0] >= 0 && spans.types[0] != FTX_FIELD_CALL &&
					!strncmp(text + spans.offsets[0], "CQ", 2)) {
				char *sender = text + spans.offsets[1];
				char *sender_end = strchr(sender, ' ');
				if (!sender_end)
					sender_end = sender + strlen(sender);
				if (*sender == '<')
					++sender;
				if (sender_end > sender && *(sender_end - 1) == '>')
					--sender_end;
				char *grid_txt = text + spans.offsets[2];
				if (grid_txt[0] == 'R' && grid_txt[1] == ' ') // ir>0 case, unpackgrid()
					grid_txt += 2;
				int sender_len = (int)(sender_end - sender);
				// "..." is lookup_callsign()'s literal unresolved-hash text
				// (ft8_lib/ft8/message.c) -- not a real identifier, never
				// write it to the directory.
				bool sender_is_hash_miss = (sender_len == 3 && !strncmp(sender, "...", 3));
				if (sender_len > 0 && sender_len < 16 && !sender_is_hash_miss)
					ft8_grid_queue_push(sender, sender_len, grid_txt);
			}
			// Real report, live (2026-08-23): during Auto CQ, RX Frequency
			// stayed empty for the message that actually triggered the
			// auto-answer -- only showed content once the *other*
			// station's next message arrived. Root cause: ft8_process()
			// (which, for Auto CQ's own "someone answered my CQ" branch,
			// sets CALL server-side for the very first time this
			// exchange) used to run *after* write_console_semantic()
			// pushed this same decoded text to the client. The client's
			// own RX-Frequency gate (FT8_new_message(), web/index.html)
			// requires #CALL to already match before appending anything
			// -- but #CALL's field update and this console text arrive
			// as two independent websocket messages, and the client
			// processes them in the order they were sent. Console text
			// first meant the client checked #CALL before the server had
			// even decided to set it, missing the one message that
			// should have opened the panel. A manual click or Auto
			// Answer's own FT8_check path never hit this (see
			// FT8_new_message()'s own comment: both already append
			// directly, sidestepping the race entirely) -- only Auto
			// CQ's server-only auto-response path relies purely on this
			// ordering. Swapped so CALL is set before the client ever
			// sees the text that depends on it.
			//
			// Real report, live (2026-08-23): once CALL-set-before-push was
			// fixed, a *different* real bug surfaced -- every incoming
			// message actually directed at us (signal reports, RR73) went
			// missing from RX Frequency, while unrelated band traffic and
			// our own TX echoes displayed fine. Root cause: ft8_process()
			// calls ft8_message_tokenize(), which uses strtok(message, " \r\n")
			// -- a destructive, in-place tokenizer that punches a NUL byte
			// into the buffer at every delimiter it crosses. buf here is
			// the *same* buffer write_console_semantic() uses right below,
			// so by the time it runs, buf (read from its start) had already
			// been shredded down to just its first token (the timestamp) --
			// a ~7-char stub, comfortably under the client's own >30-char
			// filter (update_data(), index.html), so it silently vanished
			// there. Confirmed live: KP2B/KE2ALP/K0TT/K0JV all logged
			// correctly (the QSO state machine reads the separately-
			// tokenized m1/m2/m3 globals, never this buf), yet none of
			// their reports ever reached RX Frequency. Only messages
			// directed at us hit this at all (my_call_found gates the
			// ft8_process() call), which is exactly why unrelated traffic
			// was unaffected. Fix: hand ft8_process() a private copy so its
			// internal strtok() can't touch the buffer write_console_
			// semantic() still needs.
			if (my_call_found) {
				char buf_copy[sizeof(buf)];
				strncpy(buf_copy, buf, sizeof(buf_copy));
				buf_copy[sizeof(buf_copy) - 1] = 0;
				ft8_process(buf_copy, FTX_CONTINUE_QSO);
			}
			write_console_semantic(buf, sem, sem_i);
			n_decodes++;
        }
    }
    monitor_free(&mon);
    // TEMPORARY: bumped to LOG_INFO (from LOG_DEBUG, compiled out at
    // this file's LOG_LEVEL) while debugging why SIC found zero new
    // decodes on real captured audio -- restore to LOG_DEBUG after.
    LOG(LOG_INFO, "SIC pass %d: %d new decodes (total %d)\n", pass, new_this_pass, n_decodes);
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

/*!
    Returns \c true if we have anything to send at this moment (is it the right time to start?)
    and updates ft8_pitch and is_cq.
    If ft8_tx_text is a CQ, then it also updates ft8_tx1st according to current settings.
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
    }

    if (is_ft4) {
	int two_slot_clock = wallclock_day_ms % 15000;
	if (two_slot_clock < 7500) {
	    if (ft8_tx1st)
		start = true;
	} else {
	    if (!ft8_tx1st)
		start = true;
	}
    } else {
	int two_slot_clock = wallclock_day_ms % 30000;
	if (two_slot_clock < 15000) {
	    if (ft8_tx1st)
		start = true;
	} else {
	    if (!ft8_tx1st)
		start = true;
	}
    }
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
		ft8_pitch, ft8_tx_text);
	write_console(STYLE_FT8_TX, buf);
	message_add("FT8", ft8_pitch, 1, ft8_tx_text);

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
		wallclock_day_ms % 60000, ft8_tx_buff_index, offset_ms, ft8_tx_text);
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
	// Live "what's queued to transmit" indicator -- see the identical
	// call/full reasoning in ft8_tx_3f() above.
	set_field("#ft8_tx_pending", ft8_tx_text);
	ftx_would_send(); // update wallclock_day_ms, ft8_pitch, ft8_tx1st
	if (!freq)
		freq = ft8_pitch;
	snprintf(hmst_wallclock_time_sprint(buf), sizeof(buf) - 8, "  TX     %4d ~ %s\n", freq, ft8_tx_text);
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

	// Real report: "73" used to be single-shot, no retry -- but the QSO
	// is already logged by the time it's sent (see the RR73/RRR branch
	// in ft8_process()), decoupled from how many times "73" itself
	// gets (re)transmitted, so there's no re-logging risk in letting it
	// use the same repeat count as everything else. If it never
	// reaches the other station, they're left waiting on a "73" that
	// never arrives -- exactly what retrying this avoids.
	ft8_repeat = field_int("FT8_REPEAT");
	update_tx_active_field();

	LOG(LOG_DEBUG, "%05d ft8_tx '%s' even? %d autocq %d\n",
		wallclock_day_ms % 60000, ft8_tx_text, ft8_tx1st, ft8_autocq_running);
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
	// Live "what's queued to transmit" indicator, user's own ask -- a
	// dedicated display-only field (#ft8_tx_pending), not the shared
	// keyboard/CW "TEXT" field the abandoned attempt above would have
	// reused (that field is real user-typed input -- see #text_in in
	// sbitx_daemon.c -- overwriting it here would have corrupted
	// keyboard/CW text entry, not shown a TX preview).
	set_field("#ft8_tx_pending", ft8_tx_text);
	ftx_would_send(); // update ft8_pitch, is_cq, ft8_tx1st
	snprintf(hmst_wallclock_time_sprint(buf), sizeof(buf) - 8, "  TX     %4d ~ %s\n", ft8_pitch, ft8_tx_text);
	write_console(STYLE_FT8_QUEUED, buf);
	LOG(LOG_INFO, "<- %d.%c '%s' '%s' '%s'",
		message_type, message_type ? ' ' : '0' + ftx_message_get_n3(&ftx_tx_msg), call_to, call_de, extra);

	// See the identical fix (and full reasoning) in ft8_tx() above --
	// "73" now gets the same repeat count as everything else, not a
	// hardcoded single shot.
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
		// Real "missing every other decode cycle" bug, root-caused live:
		// this used to also reset ft8_rx_buff_index = 0 here. ft8_rx()
		// (capture thread) already resets it correctly at the true start
		// of each new slot (slot_time < 500, wallclock-synced) -- real
		// audio for the *next* slot starts accumulating there right on
		// schedule. But decode itself now routinely takes 1.6-3s (SIC/AP
		// passes), so this decode-completion-triggered reset fired well
		// after that -- clobbering 1.6-3s of audio the next slot had
		// already legitimately started accumulating. Losing that much of
		// a 15s slot (only 2s of margin over the 13s needed to trigger)
		// reliably pushed the buffer's 13s threshold past the slot's own
		// window, so decode kept slipping to the *following* slot instead
		// -- a clean, deterministic "every 30s instead of 15s" pattern,
		// confirmed via real wall-clock log timestamps (decode finishing
		// fast, then a ~27s idle gap before the next one even started).
	}
}

// Task: decoder merge (2026-08-29) -- step 1 of running jt9 as a second,
// independent decoder alongside our own and merging (union of) both
// sets of decodes, per last night's design discussion and the offline
// prototype that validated it's worth doing (real captured 20M audio:
// jt9 86/18 CQs, our own decoder 69/15 CQs, merged union 102/21 CQs).
// This step only dumps a WAV file per completed slot -- nothing reads
// it yet, that's the next step (spawn jt9 against it, parse its stdout,
// feed the result into our own decode pipeline the same way a real
// decode from sbitx_ft8_decode() would be).
//
// buf/n_samples here is always exactly one FT8/FT4 slot's worth of
// already-decimated 12kHz mono float audio -- the *same* data
// sbitx_ft8_decode() itself decodes from (ft8_rx_buffer, passed in by
// ft8_rx() right where it would otherwise reset that buffer for the
// next slot), so this needs no decimation or resampling of its own,
// and is already exactly the format jt9 expects (see wave.c's own
// comment: 12kHz mono 16-bit PCM, no resampling needed).
//
// Ping-pongs between two fixed filenames rather than a growing set of
// timestamped ones -- nothing to clean up, and a reader (jt9, next
// step) always finds a stable, fully-written file from the *previous*
// slot while the current one is still accumulating into the other, so
// it can never observe a half-written file. /tmp matches this file's
// own already-established convention for short-lived diagnostic WAV
// dumps (see the SIC dump path elsewhere in this file) -- these are
// working files for the next decode cycle, not something meant to
// persist like an operator's own manual recording (REC ON/OFF, sbitx.c
// -- a completely separate file/feature, deliberately untouched by
// this one).
static int jt9_slot_wav_toggle = 0;
// start_ms: the just-completed slot's own real wall-clock start time
// (ft8_rx_buff_start_ms at the call site) -- jt9 has no real-time
// reference of its own from a bare WAV file (its own decode output
// always shows a placeholder "000000"), so this gets embedded directly
// in the filename for the consuming service (decoder-merge task) to
// recover and substitute in, the same way zbitxd's own decoder derives
// msg_time from when the slot it decoded actually started.
//
// is_ft4: real bug, caught before it ever shipped -- jt9_bridge.py
// originally always ran "jt9 -8" (FT8), regardless of which mode the
// slot was actually decoded in. FT4 audio fed through FT8's own symbol
// timing decodes nothing -- jt9 needs "-5"/"--ft4" for that instead
// ("-4" is the unrelated older JT4 mode, an easy trap). The WAV
// filename had no way to tell the two apart, so the consuming service
// had no way to pick the right flag -- embedded here for the same
// reason start_ms is.
static void jt9_dump_slot_wav(const float *buf, int n_samples, int start_ms, bool is_ft4){
	if (n_samples <= 0)
		return;
	if (n_samples > FT8_MAX_BUFF)
		n_samples = FT8_MAX_BUFF;

	int total_sec = start_ms / 1000;
	int hh = total_sec / 3600;
	int mm = (total_sec / 60) % 60;
	int ss = total_sec % 60;
	char path[72];
	snprintf(path, sizeof(path), "/tmp/zbitxd_jt9_slot_%d_%02d%02d%02d_%s.wav",
		jt9_slot_wav_toggle, hh, mm, ss, is_ft4 ? "FT4" : "FT8");
	jt9_slot_wav_toggle ^= 1;

	// Same float -> int16 conversion/clamp ft8_lib's own save_wav()
	// uses (wave.c) -- ft8_rx_buffer's values are already in that same
	// roughly -1..+1 range sbitx_ft8_decode() itself trusts, so this is
	// just the standard PCM16 scale-and-clamp, not a new calibration.
	static int16_t pcm[FT8_MAX_BUFF];
	for (int i = 0; i < n_samples; i++){
		float v = buf[i] * 32767.0f;
		if (v > 32767.0f)
			v = 32767.0f;
		else if (v < -32768.0f)
			v = -32768.0f;
		pcm[i] = (int16_t)v;
	}

	FILE *f = wav_start_writing(path);
	if (!f)
		return;
	fwrite(pcm, sizeof(int16_t), n_samples, f);
	wav_finish_writing(f);
}

// the ft8 sampling is at 12000, the incoming samples are at
// 96000 samples/sec
void ft8_rx(int32_t *samples, int count) {

	bool is_ft4 = !strcmp(field_str("MODE"), "FT4");
	int decimation_ratio = 96000/12000;

	//if there is an overflow, then reset to the begining
	if (ft8_rx_buff_index + (count/decimation_ratio) >= FT8_MAX_BUFF){
		// Real bug, root-caused live: this used to reset only
		// ft8_rx_buff_index, leaving ft8_rx_buff_start_ms stale at
		// whatever the last true slot_time<500 boundary was (see that
		// branch below). sbitx_ft8_decode()'s dt_buffer_start_correction_sec
		// is computed FROM ft8_rx_buff_start_ms -- so any decode that
		// completed against buffer contents starting here, before the
		// next real slot boundary refreshed it, got a dt off by however
		// stale start_ms was (observed live: a decode with dt off by
		// -14.7s in the same window this overflow fired). Buffer is
		// sized for 18s, slots are only 15s (7.5s FT4), so this should
		// only ever fire if the capture thread misses a slot_time<500
		// window entirely (e.g. a scheduling stall) -- rare, but when it
		// does, start_ms must be resynced here too, not just the index.
		ft8_rx_buff_index = 0;
		ft8_rx_buff_start_ms = wallclock_day_ms;
		LOG(LOG_INFO, "%05d ft8_rx: Buffer Overflow, resetting\n", wallclock_day_ms % 100000);
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
		// See jt9_dump_slot_wav()'s own comment -- must run before the
		// reset just below, while ft8_rx_buffer/ft8_rx_buff_index still
		// hold the just-completed slot's own data, not the next one's.
		jt9_dump_slot_wav(ft8_rx_buffer, ft8_rx_buff_index, ft8_rx_buff_start_ms, is_ft4);
		ft8_rx_buff_index = 0;
		ft8_rx_buff_start_ms = wallclock_day_ms;
	}

	//we should have at least 6 or 12 seconds of samples to decode
	// ft8_decode_triggered_for_ms guard: see its own comment (real
	// duplicate-decode bug, live). This whole condition stays true for
	// the entire 13-15s tail of a slot, and this function gets called
	// repeatedly (every ~500ms, the gate above) throughout that window
	// -- without this guard, a decode pass still running past a later
	// ~500ms re-check (routinely 1.6-3s, SIC/AP passes) got a second,
	// redundant ft8_do_decode = 1 before its own natural buffer reset
	// (slot_time<500, next slot) ever happened, running the same audio
	// through sbitx_ft8_decode() twice -- confirmed live: every decode
	// from that slot shown twice in Band Activity/CQ Panel, same
	// embedded timestamp, moments apart. Only trigger once per genuinely
	// new slot (identified by its own buffer-start timestamp).
	if (ft8_rx_buff_index >= 13 * min_secs && slot_time > slot_time_decode
			&& ft8_decode_triggered_for_ms != ft8_rx_buff_start_ms) {
		ft8_do_decode = 1;
		ft8_decode_triggered_for_ms = ft8_rx_buff_start_ms;
//~ printf("ft8_rx decoding trigger index %d, clock %d, slot_time %d\n", ft8_rx_buff_index, wallclock_day_ms % 60000, slot_time);
	}
}

static void ft8_poll_impl(int tx_is_on){
	// ft8_suspend() already forced tx_off() synchronously, so this is
	// really only guarding the "start something new" branch further
	// down -- but checked first regardless, unconditionally, so nothing
	// below it can ever run while suspended.
	if (ft8_tx_suspended)
		return;
	//if we are already transmitting, we continue
	//until we run out of ft8 sampels
	if (tx_is_on){
		//tx_off should not abort repeats from modem_poll, when called from here
		int ft8_repeat_save = ft8_repeat;
		if (ft8_tx_nsamples == 0){
			tx_off();
			ft8_repeat = ft8_repeat_save;
			// tx_off() -> modem_abort() -> ft8_abort() just zeroed
			// #tx_active and #ft8_repeat_count along with ft8_repeat --
			// now that ft8_repeat is restored, re-sync both fields so
			// they don't stay wrong (badge stuck at 0 mid-cycle) until
			// the next real mutation
			set_field_int("#ft8_repeat_count", ft8_repeat_save > 0 ? ft8_repeat_save : 0);
			update_tx_active_field();
		}
		return;
	}

	// ft8_qso_log_pending must be in this guard too, or a courtesy "73"
	// sent with Auto CQ *not* running (ft8_autocq_resume_pending always
	// false then) would return above before ever reaching the block below
	// that actually consumes it -- the deferred log/call-wipe would just
	// never happen. ft8_give_up_pending needs the same treatment -- see
	// its own comment. ft8_give_up_grace_pending too -- it needs to keep
	// getting polled (to notice its deadline passing) for as long as it
	// stays set, same reasoning.
	if (!ft8_repeat && !ft8_autocq_resume_pending && !ft8_qso_log_pending
			&& !ft8_give_up_pending && !ft8_give_up_grace_pending)
		return;

	// See ft8_qso_log_pending's own comment: the courtesy "73" queued in
	// ft8_process()'s RR73/RRR branch has now actually finished
	// transmitting (ft8_repeat naturally reached 0, and tx_is_on's own
	// early return above means we're not still mid-transmission) --
	// log/clear the exchange now, not when it was merely queued.
	if (!ft8_repeat && ft8_qso_log_pending){
		ft8_qso_log_pending = false;
		enter_qso();
		call_wipe();
	}

	// See ft8_give_up_pending's own comment: the exhausted reply queued
	// in the give-up branch below has now actually finished
	// transmitting -- clear the exchange now, not when it was merely
	// queued.
	if (!ft8_repeat && ft8_give_up_pending){
		ft8_give_up_pending = false;
		ft8_finalize_pending_qso();
		call_wipe();
		if (ft8_autocq_running)
			ft8_autocq_resume_pending = true;
	}

	// See ft8_give_up_grace_pending's own comment: only actually finalize
	// once the grace deadline has passed with nothing further heard from
	// this station -- ft8_process_impl() cancels this early the instant a
	// genuine continuation arrives, so reaching here at all means the
	// grace window really did run out.
	if (ft8_give_up_grace_pending){
		if (wallclock_day_ms >= ft8_give_up_grace_deadline_ms){
			ft8_give_up_grace_pending = false;
			ft8_finalize_pending_qso();
			call_wipe();
			if (ft8_autocq_running)
				ft8_autocq_resume_pending = true;
		}
		// Real bug, caught live (2026-08-28) via a real QSO with ER5DX,
		// STILL happening after the "still waiting" return below was
		// added: that fix only covered the *not yet expired* half of
		// this check -- the *just expired, finalizing right now* half
		// above had no return at all, so execution still fell straight
		// through into the normal ftx_would_send()/repeat-decrement/
		// actual-transmit block below, in this exact same tick.
		// call_wipe() clears CALL/SENT/RECV/EXCH/NR but deliberately
		// never touches ft8_tx_text (a separate buffer) -- so if a slot
		// boundary happened to land on this same tick (likely, since
		// the grace deadline was itself derived from real slot timing),
		// it retransmitted the same already-finished message again,
		// decremented ft8_repeat to -1, re-triggered this exact give-up
		// branch, and re-armed a *fresh* 16s grace period. Forever, in
		// a stable 16s-period loop -- confirmed live, retransmissions
		// exactly 16s (FT8_GIVE_UP_GRACE_MS) apart. CALL genuinely did
		// get wiped each cycle (why the CQ Panel correctly reappeared,
		// per the user's own report) even while this kept transmitting
		// underneath it the whole time. Unconditional return now,
		// whether grace just expired or is still pending -- either way
		// there is nothing left to legitimately transmit this tick; a
		// genuinely new exchange starting fresh always goes through
		// ft8_on_start_qso(), a completely different call path where
		// this flag is false.
		return;
	}

	// Auto CQ: the QSO that just finished (ft8_process()'s "73"/"RR73"
	// branches) scheduled this instead of re-queuing CQ directly there,
	// since a courtesy final "73" may still have needed to go out first
	// -- queue the next CQ call now that we're actually idle again, then
	// fall through to the normal slot-boundary logic below like any
	// fresh CQ call.
	if (!ft8_repeat && ft8_autocq_resume_pending){
		ft8_autocq_resume_pending = false;
		queue_cq_call();
	}

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
		// up right at (or acceptably close to) the true start of a
		// window -- anything later than the cutoff just defers to the
		// next poll, which keeps returning true for the rest of this
		// window and then goes false until the next one opens, arriving
		// here again with a small slot_time right at that real boundary.
		//
		// Cutoff widened twice now, both times from a real measured
		// miss: 200ms -> 2000ms after a genuine reply landed at
		// slot_time 1746ms; then 2000ms -> 4500ms after a second one
		// landed at 2160ms, missing that gate by just 160ms -- and
		// paying for it with a full extra 30s/one alternating-pair
		// cycle instead of transmitting a few seconds late. Two real
		// near-misses this close to whatever the gate happened to be
		// set to says these are a normal, recurring part of real
		// operation (a reply queued right as its window opens), not a
		// rare edge case -- and the cost of missing (30s) is severe
		// while the cost of allowing a later start is minor.
		//
		// ftx_start_tx(slot_time) already seeds a real, correctly-
		// synced offset into the waveform for any slot_time >= 1000ms
		// (not the sample-0 restart the original 200ms threshold was
		// guarding against), so a late-but-still-open window transmits
		// a truncated but still correctly-timed message instead of
		// being skipped outright. 4500ms is reasoned from FT8's actual
		// structure this time, not another round number close to the
		// latest miss: a message is 7-symbol Costas + 29-symbol data +
		// 7-symbol Costas + 29-symbol data + 7-symbol Costas (79
		// symbols, 0.16s/symbol), so the 2nd Costas sync block starts
		// at 5.76s -- 4500ms trims at most the 1st Costas block plus
		// some leading data, comfortably (1.26s of margin) before
		// touching the 2nd, still leaving 2 of 3 Costas blocks and the
		// majority of the payload intact. Past that point a skip to
		// the next window is judged better than transmitting too
		// little of the message to realistically decode.
		if (slot_time < 4500) {
			LOG(LOG_DEBUG, "%05d ft8_poll: tx_is_on %d ft8_tx_nsamples %d start '%s'\n",
				wallclock_day_ms % 60000, tx_is_on, ft8_tx_nsamples, ft8_tx_text);
			ftx_start_tx(slot_time); // modulate audio at current frequency setting
			if (ft8_tx_nsamples)
				tx_on(TX_SOFT);
			ft8_repeat--;
			// ft8_repeat hitting 0 is the real "give up on this, move
			// on" signal everywhere else in this file -- if Auto CQ is
			// running and that ever happens without something noticing,
			// the radio just sits idle forever with Auto CQ still
			// technically armed but nothing left scheduled to resume it.
			// Two different real "give up" moments reach this same
			// point, so both need to be caught right here, not just the
			// explicit QSO-completion branches in ft8_process():
			//  - is_cq: the CQ call's own repeat count hit 0 with nobody
			//    answering after FT8_REPEAT tries -- that's the give-up
			//    signal, so Auto CQ disarms itself entirely (user's own
			//    call: "auto cq stops calling cq when the counter gets
			//    to zero"), same as if TX Enabled had been clicked to
			//    abort.
			//  - !is_cq: an in-QSO reply (queued via ft8_tx_3f() -- a
			//    signal report, RR73, etc) ran out of retries with no
			//    response, e.g. the other station went silent mid-
			//    exchange without ever sending a final 73/RR73 -- that
			//    specific dead-end isn't covered by ft8_process()'s
			//    "73"/"RR73" completion branches at all, since those
			//    only fire when the *other* station actually replies.
			//    Schedule the same resume ft8_process() schedules on a
			//    genuine QSO completion, so this dead-end doesn't
			//    silently strand Auto CQ idle forever either.
			if (ft8_repeat <= 0){
				if (is_cq){
					// Give up -- nobody answered after FT8_REPEAT tries.
					// Deliberately leaves #ft8_auto/the checkbox alone
					// (user's own call): only the armed/running state and
					// TX Enabled drop, via the normal TXACTIVE update
					// below once this last burst finishes transmitting.
					// A bare click on TX Enabled re-arms and starts a
					// fresh round -- no need to touch the checkbox again.
					// Only Auto CQ itself has an armed/running state to
					// stop -- a manual single CQ call has nothing to do
					// here.
					if (ft8_autocq_running)
						ft8_autocq_stop();
				}
				else {
					// Real bug, confirmed live: this dead-end (an in-QSO
					// reply -- e.g. our own answer to someone else's CQ --
					// running out of retries with no response) scheduled
					// the Auto CQ resume but never cleared CALL, unlike
					// every other "this exchange is over" path (the "73"
					// and RR73/RRR branches in ft8_process(), abort_tx()).
					// Left stale, it silently blocked ft8_process()'s own
					// auto-respond gate ("!strlen(call)") from recognizing
					// the *next* genuine new caller answering a fresh CQ --
					// confirmed live: Auto CQ kept re-transmitting its own
					// CQ instead of replying to a real new answer, because
					// the auto-respond path never even ran. Same class of
					// stale-CALL problem already documented on the "73"
					// branch above, just a different path that hadn't been
					// covered yet.
					// Real report, live (2026-08-22), user's own protocol
					// point: the other station typically logs their side
					// and moves on the moment *they* send RR73 -- they
					// often never send a confirming "73" back at all, so
					// our own closing "73" repeats exhausting naturally
					// (ft8_repeat reaching 0 right here, with nothing ever
					// heard back) is the *normal*, expected end for it, not
					// a rare edge case. If that's what's happening when
					// this dead-end fires, ft8_repeat has just gone from 1
					// to 0 in this same pass, before ft8_poll()'s own
					// top-of-function check (which only sees the *previous*
					// poll's value) ever gets a chance to consume
					// ft8_qso_log_pending -- call_wipe() below would
					// otherwise silently discard a real, complete QSO the
					// same way abort_tx() used to. See
					// ft8_finalize_pending_qso()'s own comment.
					//
					// Real report, live (2026-08-24): this whole block used
					// to be gated on ft8_autocq_running, so Auto Answer
					// running *without* Auto CQ also armed never reached
					// call_wipe() here at all -- confirmed live: "when the
					// repeat counter got to zero i continued to call the
					// same station... another station selection did not
					// occur." CALL staying stale here is the same class of
					// bug already fixed above for Auto CQ's own version of
					// this dead-end, just needed regardless of which
					// automation (if any) is actually running -- a manual
					// answer that goes unanswered needs its CALL cleared
					// too, or the *next* genuine caller (auto-answered or
					// manually clicked) can't be recognized either.
					//
					// Real report, live (2026-08-24), same session: this used
					// to call ft8_finalize_pending_qso()/call_wipe() right
					// here -- but ft8_repeat has *just* been decremented to 0
					// for the retry this same pass is still queuing (see
					// ftx_start_tx() a few lines up); it hasn't actually
					// transmitted yet. CALL going empty now clears RX
					// Frequency client-side, so this cleared the panel before
					// the final retry had even gone out. Deferred instead
					// (see ft8_give_up_pending's own comment), same pattern
					// as ft8_qso_log_pending right above it.
					//
					// Real report, live (2026-08-27), KE8ESJ: deferred only
					// to "once this transmission finishes" wasn't enough --
					// a normal-speed reply routinely arrives *after* that
					// point too. Grace-period version instead (see
					// ft8_give_up_grace_pending's own comment) -- gives one
					// more full receive window before actually finalizing/
					// wiping, cancelled early if a genuine continuation
					// arrives first (ft8_process_impl()).
					ft8_give_up_grace_pending = true;
					ft8_give_up_grace_deadline_ms =
						(wallclock_day_ms + FT8_GIVE_UP_GRACE_MS) % (24 * 3600 * 1000);
				}
			}
			// Live countdown for the operator -- broadcast every time
			// ft8_repeat changes (decrement above, plus any refill just
			// above it), not just once.
			set_field_int("#ft8_repeat_count", ft8_repeat > 0 ? ft8_repeat : 0);
			update_tx_active_field();
		}
		// Still investigating whether a *genuine* miss (now past the
		// widened 2000ms gate above) ever recurs -- kept at LOG_INFO
		// (cheap, rate-limited to once per distinct missed window via
		// last_logged_window, not every ~100ms poll for as long as it
		// stays open). The original real occurrence that justified
		// widening the gate (click queued right at a slot boundary, TX
		// delayed a full 30s/one alternating-pair cycle) is expected to
		// no longer trigger this at all now, having landed at 1746ms --
		// comfortably under the new 2000ms cutoff.
		//
		// User's own real report (2026-08-27): a station answered a CQ
		// right after its 3rd/final repeat, and the reply back to them
		// missed a slot. is_cq/CALL added so a real recurrence of this
		// line unambiguously says which case it was -- our own CQ
		// missing a repeat (is_cq 1, CALL empty) vs. a reply to someone
		// missing its window (is_cq 0, CALL is who we were replying to)
		// -- instead of having to infer it from ft8_tx_text alone.
		else {
			static int last_logged_window = -1;
			int window_id = wallclock_day_ms / (is_ft4 ? 7500 : 15000);
			if (window_id != last_logged_window) {
				last_logged_window = window_id;
				LOG(LOG_INFO, "%05d ft8_poll: MISSED window (is_cq %d, CALL '%s'), slot_time %d ms (>= 4500ms gate), queued '%s' -- deferred to next matching slot\n",
					wallclock_day_ms % 60000, is_cq, field_str("CALL"), slot_time, ft8_tx_text);
			}
		}
	}
}

// See ft8_process_mutex's own comment -- ft8_poll() (main thread, every
// ~100ms) reads/writes the exact same shared exchange state ft8_process()
// does, from a different thread than the one ft8_process() already
// protects itself against. Thin wrapper, same pattern.
void ft8_poll(int tx_is_on){
	pthread_mutex_lock(&ft8_process_mutex);
	ft8_poll_impl(tx_is_on);
	pthread_mutex_unlock(&ft8_process_mutex);
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
	// New exchange starting (or being rebound) -- see
	// ft8_courtesy_73_sent_to's own comment: this must clear so a later,
	// genuinely new exchange with this same station isn't blocked.
	ft8_courtesy_73_sent_to[0] = 0;
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
	// Keep the EVEN/ODD box in sync -- this is the one place ft8_tx1st
	// changes automatically (every time a QSO actually starts, auto-
	// answer or a manual click alike) without going through set_field(),
	// so the display used to only ever reflect the last manual click,
	// never a real automatic slot switch.
	set_field("#ft8_tx1st", ft8_tx1st ? "ON" : "OFF");
	LOG(LOG_DEBUG, "msg_second %d slot_in_minute %d odd? %d reply tx1st? %d\n", msg_second, slot_in_minute, slot_in_minute % 2, ft8_tx1st);
}

// this kicks stars a new qso either as a CQ message or
// as a reply to someone's cq or as a 'break' with signal report to
// a concluding qso
void ft8_on_start_qso(char *message){
	modem_abort();
	tx_off();
	// Real report, live (2026-08-23): starting a brand-new exchange
	// (manual click or auto-answer alike -- see this function's own call
	// sites) while a *previous* exchange's closing "73" was still
	// repeating (ft8_qso_log_pending, full FT8_REPEAT count now applies
	// -- see that flag's own comment) wiped CALL/SENT/RECV here before
	// that pending log ever got a chance to fire, the same way abort_tx()
	// and the Auto CQ give-up path already needed fixing for. Confirmed
	// live: RR73 from one station, closing "73" queued and still
	// repeating, operator moves on to answer a different station calling
	// -- the first station's real, complete QSO was silently discarded.
	// modem_abort()/tx_off() above already stop the old repeat sequence
	// cleanly; this just needs to log it before reusing CALL for the new
	// one.
	ft8_finalize_pending_qso();
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
		// Real bug, root-caused live (2026-08-28) via two corrupted
		// historical logbook rows (2026-08-23/24: RECV holding the
		// literal text "RRR"/"RR73" instead of a numeric report, EXCH
		// left empty when it should have held a real grid). "RRR"/
		// "RR73"/"73" are never valid content for the *first* message
		// of a fresh exchange -- they only ever mean something as the
		// closing steps of one already under way. Reaching this branch
		// at all requires CALL to already be empty (this function's own
		// call_wipe() above, or the auto-respond gate's !strlen(call)
		// check) -- so a message shaped like this here is either a
		// stray decode with no real exchange to attach to (nothing
		// useful to do with it), or -- the actual root cause found live
		// -- a genuine, on-time continuation whose CALL got wiped out
		// from under it by some other give-up/completion race finishing
		// moments before this arrived (today's give-up grace period
		// closes one such race, but not every call_wipe() site has one).
		// Either way, blindly logging it into RECV via the "not a grid
		// shape, must be a report" fallback below is simply wrong.
		// Ignoring it here closes the actual corruption path regardless
		// of which race let CALL go empty.
		if (!strcmp(m3, "RRR") || !strcmp(m3, "RR73") || !strcmp(m3, "73"))
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
		// See the cold-call branch's own comment just above -- same
		// reasoning: "RRR"/"RR73"/"73" can never be valid content for
		// *starting* a fresh exchange, manual break-in or not.
		if (!strcmp(m3, "RRR") || !strcmp(m3, "RR73") || !strcmp(m3, "73"))
			return;
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
	// Replies transmit at our *own* frequency (ft8_pitch, from
	// TX_PITCH), not the other station's -- user's own confirmation:
	// "has to be at my frequency". Standard FT8 convention: the station
	// being answered retunes to reply back to us at our frequency for
	// the rest of the exchange, not the other way around. (A different
	// fix was tried here first, based on a wrong assumption that a real
	// CQ decoded at 1391 Hz getting a reply at TX_PITCH's 2046 Hz was a
	// bug -- it wasn't; reverted.)
	ft8_tx(reply_message, ft8_pitch);
	// ft8_tx() above already set ft8_repeat from FT8_REPEAT -- one
	// shared counter/give-up signal for CQ calls, answering a CQ, and
	// in-QSO replies alike (user's own call).
}

void ft8_on_signal_report(){
	set_call_field(m2);
	// Real bug, live (2026-08-23), confirmed via journal trace of a real
	// stuck exchange: this used to unconditionally send a fresh reply
	// every single time this function ran, with no check for whether the
	// incoming message was a genuine new report or just a duplicate
	// decode of one we'd already replied to (the other station repeating
	// because they haven't heard our own reply yet -- completely normal,
	// same reason our own replies here go through ft8_tx_3f()'s repeat
	// mechanism). Each duplicate re-triggered a *fresh* RR73/report send,
	// resetting our own repeat sequence before it could ever finish --
	// neither side's repeat ever won the race, so the exchange never
	// closed on its own. Confirmed live: DL5BWG and us traded R-22/RR73
	// back and forth for over a minute while a completely different
	// station (K5AHL) got silently ignored ("ignoring stale-exchange
	// message") the whole time, since CALL stayed locked to DL5BWG.
	// Skipping when RECV already matches what we're about to set it to
	// -- a real new report always changes RECV, so this only ever
	// catches genuine duplicates of an exchange step already handled.
	if (m3[0] == 'R'){
		//skip the 'R'
		if (!strcmp(field_str("RECV"), m3 + 1))
			return;
		field_set("RECV", m3+1);
		ft8_tx_3f(call, mycall, "RR73");
	}
	else{
		if (!strcmp(field_str("RECV"), m3))
			return;
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

static void ft8_process_impl(char *message, ftx_operation operation){
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
		// User's own real report (2026-08-27): a station answered a CQ
		// right after its 3rd/final repeat, and the reply back to them
		// missed its own slot -- suspected root cause is decode/dispatch
		// latency eating into the tight ~1-2s margin before the reply's
		// target window opens (see ft8_poll_impl()'s "MISSED window" log
		// and its own 4500ms-gate comment for the full reasoning). This
		// line records how far into the *current* slot we already are by
		// the time the reply actually gets queued -- cross-reference
		// against a "MISSED window" line moments later (same CALL) to
		// confirm whether a real miss traces back to this.
		//
		// Real bug caught live (2026-08-27), same investigation: this
		// branch also fires for a genuine *cold call* (nobody was
		// calling CQ at all), and -- after the give-up grace period just
		// below was added -- can no longer fire mid-QSO the way it once
		// could either. "auto-answering CQ" was flatly wrong for either
		// of those; can't tell from here whether *we* were CQing (that's
		// only decided inside ft8_on_start_qso(), from whether m1 there
		// is literally "CQ" -- not the case in this branch, since m1 is
		// already mycall by the condition above). Logged generically
		// instead: a fresh/idle exchange starting from '%s'.
		{
			const bool is_ft4_now = !strcmp(field_str("MODE"), "FT4");
			const int slot_time_now = is_ft4_now ? wallclock_day_ms % 7500 : wallclock_day_ms % 15000;
			LOG(LOG_INFO, "%05d ft8_process: new exchange (CALL was empty) starting with '%s', queuing reply at slot_time %d ms\n",
				wallclock_day_ms % 60000, m2, slot_time_now);
		}
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

	// Real bug, confirmed live via journalctl: none of the three
	// branches below (73/RR73/RRR/signal-report) verified that THIS
	// message's actual sender (m2) is the same station CALL is
	// currently tracking -- only that CALL was non-empty. In a real
	// pileup, answering one CQ, then abandoning it to answer another
	// before the first one resolves, is completely normal operation --
	// user's own framing: "this one is of my own making; but I can see
	// it happening in the real world." When a late decode from the
	// FIRST (already-abandoned) station arrived after CALL had moved on
	// to a second, unrelated one, it still got auto-processed as if it
	// were that second exchange's next step: enter_qso() would have
	// logged the wrong callsign (it logs field_str("CALL"), not m2),
	// and ft8_on_signal_report() auto-transmitted a real reply to a
	// station with zero operator action at all -- confirmed exactly:
	// W8FSM's -15 report auto-triggered a transmitted R-04 with no
	// preceding click. A manual click never hits this -- it always
	// goes through ft8_on_start_qso() above (operation ==
	// FTX_START_QSO), which deliberately rebinds CALL to whatever was
	// clicked -- so this only guards the auto-detect path.
	if (strlen(call) && strcmp(m2, call)){
		LOG(LOG_INFO, "ft8_process: ignoring stale-exchange message from '%s' (CALL is '%s')\n", m2, call);
		return;
	}

	// See ft8_give_up_grace_pending's own comment: reaching here with
	// CALL non-empty and m2==call (didn't return above) means this
	// message genuinely continues the exchange the grace period was
	// counting down for -- cancel it so the deferred finalize/wipe in
	// ft8_poll_impl() doesn't fire later and clobber a now-active
	// exchange out from under itself. Harmless no-op if it wasn't
	// pending.
	ft8_give_up_grace_pending = false;

	if (!strcmp(m3, "73")){
		ft8_abort();
		enter_qso(); // W9JES
		// enter_qso() deliberately skips call_wipe() for FT8/FT4 (it only
		// resets PITCH back to TX_PITCH there -- see its own comment).
		// The RR73/RRR-received branch below covers itself with its own
		// explicit call_wipe() right after enter_qso(); this is the other
		// half of the same exchange (the side that sent RR73 first and is
		// now receiving the closing "73"), which never gets one. Left
		// stale, CALL keeps holding the just-finished station's callsign,
		// which permanently blocks auto-answer's "!strlen(call)" gate the
		// next time someone answers a fresh CQ -- confirmed live: neither
		// of two real over-the-air replies auto-answered after this exact
		// gap, both needed a manual click.
		call_wipe();
		ft8_repeat = 0;
		// Real report, live (2026-08-22): if we were still in the middle
		// of repeating our own closing "73" (see ft8_qso_log_pending's own
		// comment -- restored to using the full FT8_REPEAT count, not
		// capped to one) when their "73" arrived here first, ft8_abort()
		// above already stopped the repeats and enter_qso()/call_wipe()
		// just ran directly, immediately, right above -- so there's
		// nothing left for ft8_poll() to do once ft8_repeat reaches 0 from
		// the abort. Left set, ft8_poll() would run enter_qso()/
		// call_wipe() a second, redundant time (harmless in practice,
		// enter_qso()'s own 5-minute dedup window catches it -- but still
		// a real gap worth closing directly).
		ft8_qso_log_pending = false;
		// Auto CQ: this QSO is done and we have nothing further to send
		// -- schedule ft8_poll() to re-queue CQ on its next idle cycle
		// (see ft8_autocq_resume_pending's own comment).
		if (ft8_autocq_running)
			ft8_autocq_resume_pending = true;
		update_tx_active_field();
		return;
	}

	//the other station has sent either an RRR or an RR73
	//this maybe arriving after we have cleared the log
	//we don't check it against any fields of the logger
	if (!strcmp(m3, "RR73") || !strcmp(m3, "RRR")){
		// Real report, live (2026-08-23), screenshot confirmed: the other
		// station repeating RR73/RRR because they hadn't yet heard our
		// first courtesy "73" re-triggered a *second* courtesy "73" --
		// this branch had no dedup at all, unlike ft8_on_signal_report()
		// just above (see its own comment, fixed in 127079d for the exact
		// same class of ping-pong at an earlier exchange stage). Same
		// idea, but tracked in ft8_courtesy_73_sent_to instead of RECV
		// this time -- see its own comment for why reusing RECV here (an
		// earlier version of this fix) was a real regression: it
		// clobbered the actual received-signal-report value that gets
		// logged, so the QSO's logged RST came out as the literal string
		// "RRR" instead of a real report.
		if (!strcmp(ft8_courtesy_73_sent_to, call))
			return;
		strncpy(ft8_courtesy_73_sent_to, call, sizeof(ft8_courtesy_73_sent_to) - 1);
		ft8_courtesy_73_sent_to[sizeof(ft8_courtesy_73_sent_to) - 1] = 0;
		ft8_tx_3f(m2, mycall, "73");
		// Real report, live (2026-08-22): enter_qso()/call_wipe() used to
		// run right here, synchronously with *queueing* the closing "73"
		// above -- not with it actually going out over the air (that's
		// the next TX slot). Logging/clearing the exchange this early
		// switched the client's display away from RX Frequency before the
		// operator had even started transmitting the courtesy "73", and
		// (since CALL was already wiped) left no way for a genuine resend
		// -- the other station repeating RR73/RRR because they missed our
		// first "73" -- to be recognized as the same exchange either.
		// Deferred instead (see ft8_qso_log_pending's own comment); picked
		// up by ft8_poll() once ft8_repeat naturally reaches 0, i.e. once
		// this single-shot "73" has actually finished transmitting.
		ft8_qso_log_pending = true;
		// Real report, live (2026-08-23), user's own protocol insight:
		// a real closing "73" was earlier restored to the full FT8_REPEAT
		// count (see this branch's own git history), reasoning that RX
		// Frequency should stay visible for as long as the closing "73"
		// might still be going back and forth. In practice this made
		// nearly *every* QSO tie up the UI for the full repeat window
		// (up to several minutes): WSJT-X and similar software commonly
		// treat sending RR73/RRR as "already logged, moving on" on the
		// *other* station's end -- confirmed live, repeatedly, stations
		// calling CQ again within seconds of sending us RR73 -- so our
		// own "stop early once their real 73 is heard" condition rarely
		// if ever fires, and the full repeat count runs to completion
		// every time. User's own call: send our courtesy "73" once, log,
		// and move on -- "after that it becomes the other station's
		// choice to log the QSO or not". Capped back to a single
		// transmission; ft8_qso_log_pending (see its own comment) still
		// defers the actual log until that one transmission genuinely
		// finishes, so this doesn't reintroduce the original "switches
		// before I've even started sending" bug -- it just shrinks the
		// window from several minutes back down to one ~15s slot.
		ft8_repeat = 1;
		// Auto CQ: our courtesy "73" above still needs to actually go
		// out first -- resume gets picked up once all of its repeats
		// finish and ft8_repeat naturally reaches 0 (see ft8_poll()).
		if (ft8_autocq_running)
			ft8_autocq_resume_pending = true;
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

void ft8_process(char *message, ftx_operation operation){
	pthread_mutex_lock(&ft8_process_mutex);
	ft8_process_impl(message, operation);
	pthread_mutex_unlock(&ft8_process_mutex);
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
}

void ft8_abort(){
	ft8_tx_nsamples = 0;
	ft8_repeat = 0;
	// Without this, the countdown badge sticks at whatever it last
	// showed (e.g. a round cancelled mid-cycle at "2") -- ft8_poll()'s
	// own decrement is the only other place this field gets touched,
	// and an abort never passes through there. Confirmed live: reload
	// kept showing a stale non-zero count from before the abort.
	set_field_int("#ft8_repeat_count", 0);
	update_tx_active_field();
}

// See the comment on these two in modem_ft8.h -- deliberately doesn't
// touch anything ft8_abort()/abort_tx() touch (CALL/exchange fields,
// Auto CQ armed state), only whether a transmission is allowed to
// happen at all right now. Declared with the other file-scope statics
// near the top (not here) since ft8_poll()'s own guard, earlier in the
// file, needs it in scope first.
void ft8_suspend(){
	// tx_off() -> modem_abort() -> ft8_abort() zeroes ft8_repeat and
	// #ft8_repeat_count -- real bug caught before it shipped: a
	// suspend/resume must not cost the in-progress repeat count, same
	// reasoning (and same save/restore pattern) as ft8_poll()'s own
	// tx_is_on branch a few dozen lines up, which hits this exact
	// problem for the same underlying reason.
	int ft8_repeat_save = ft8_repeat;
	tx_off();
	ft8_repeat = ft8_repeat_save;
	set_field_int("#ft8_repeat_count", ft8_repeat_save > 0 ? ft8_repeat_save : 0);
	update_tx_active_field();
	ft8_tx_suspended = 1;
}

void ft8_resume(){
	ft8_tx_suspended = 0;
}

// Real report, live (2026-08-22): manually clicking TX Enabled to abort
// while the closing "73" was still repeating (see ft8_qso_log_pending's
// own comment) silently discarded a genuinely-complete QSO -- abort_tx()
// (sbitx_daemon.c) calls call_wipe() directly, with no knowledge of this
// file's own static ft8_qso_log_pending, wiping CALL/SENT/RECV before
// ft8_poll() ever got a chance to consume the pending flag and log it.
// The exchange itself (their RR73 received, our "73" sent at least once)
// was real and complete by that point -- only the *extra* repeat
// attempts were what got cut short by the abort, which is exactly what
// an operator aborting mid-repeat actually means ("I'm confident they
// got it, stop resending"), not "throw away what already happened."
// abort_tx() calls this before its own call_wipe() so a pending log
// still goes through with the still-valid fields, instead of being
// silently lost to whichever runs first.
void ft8_finalize_pending_qso(){
	// A new exchange taking over (every real caller of this function)
	// is about to call_wipe() its own way regardless -- if a give-up
	// was still waiting on its own final retry to actually transmit
	// (see ft8_give_up_pending's own comment), that's now moot: just
	// drop it rather than leaving it to misfire later against whatever
	// exchange happens to be running the next time ft8_repeat hits 0.
	// Same reasoning for a still-pending give-up grace period (see its
	// own comment) -- a new exchange taking over means whatever it was
	// counting down for is moot too.
	ft8_give_up_pending = false;
	ft8_give_up_grace_pending = false;
	if (!ft8_qso_log_pending)
		return;
	ft8_qso_log_pending = false;
	enter_qso();
}

int ft8_is_repeating(){
	// || resume_pending keeps the TX Enabled indicator solid red through
	// the brief gap between a QSO completing and ft8_poll()'s next idle
	// cycle actually re-queuing CQ, instead of flickering grey for one
	// poll interval.
	return ft8_repeat > 0 || ft8_autocq_resume_pending;
}

// On-demand mid-cycle reset, clicking the countdown badge -- user's own
// ask: "gets down to one and i choose to keep going" (rather than
// letting it hit zero and give up on this CQ call/reply/Auto CQ round).
// Deliberately a no-op while idle (ft8_repeat already 0, nothing being
// repeated to extend) rather than resurrecting a finished round.
//
// Also called automatically from cmd_exec() (sbitx_daemon.c) any time
// FT8_REPEAT itself changes -- otherwise an in-flight countdown just
// kept ticking down toward its old total regardless of a live setting
// change, giving up (and Auto CQ disarming itself) exactly on schedule
// as if the change had never happened.
void ft8_repeat_reset(){
	if (ft8_repeat <= 0)
		return;
	ft8_repeat = field_int("FT8_REPEAT");
	set_field_int("#ft8_repeat_count", ft8_repeat);
}

// Fully terminates Auto CQ mode -- deliberately NOT part of ft8_abort()
// above, since that gets called directly (bypassing this) from two
// normal, non-terminating contexts: the tx_is_on save/restore dance in
// ft8_poll() (every burst-end, immediately restored) and a QSO
// completing in ft8_process() (where Auto CQ should keep going, not
// stop). Only abort_tx() in sbitx_daemon.c -- the real "operator/system
// wants everything pending cancelled" boundary (explicit abort click,
// mode/band/frequency change) -- calls this.
//
// Doesn't touch #ft8_auto itself (the checkbox selection) -- that used
// to be deliberate, on the reasoning that a band change shouldn't cost
// re-selecting Auto CQ/Auto Answer, only re-arming via TX Enabled.
// Superseded, user's own explicit reversal: change_band() now resets
// #ft8_auto to OFF itself, right after each of its own abort_tx()
// calls, same as a fresh login already does (webserver.c's
// do_login()). Left un-touched here since abort_tx() is also the
// explicit-abort-click path, which is a different situation and keeps
// its own original behavior (only the armed/running state drops,
// selection survives) -- ft8_poll()'s own is_cq give-up branch is
// still the one place *that* checkbox reset happens.
void ft8_autocq_stop(){
	ft8_autocq_running = false;
	ft8_autocq_resume_pending = false;
	// See #ft8_autocq_running's own comment in sbitx_daemon.c's field
	// table -- keeps a reconnecting/newly-logging-in client's checkbox
	// honest about whether this is genuinely still running server-side.
	set_field("#ft8_autocq_running", "OFF");
}

// Arms Auto CQ mode: clicking TX Enabled while idle with FT8_AUTO ==
// "AUTOCQ" (see cmd_exec()'s AUTOCQSTART command in sbitx_daemon.c).
// Queues the first CQ call via the same queue_cq_call() every other CQ
// call in this mode uses (F1's do_macro() branch, the post-QSO resume
// in ft8_poll()), so all three produce identical CQ text.
void ft8_autocq_start(){
	ft8_autocq_running = true;
	ft8_autocq_resume_pending = false;
	set_field("#ft8_autocq_running", "ON");
	queue_cq_call();
}
