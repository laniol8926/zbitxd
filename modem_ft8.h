#define FT8_MAX_BUFF (12000 * 18)

typedef enum {
    FTX_START_QSO,
    FTX_CONTINUE_QSO
}  ftx_operation;

// functions for FT8 and FT4
void ft8_rx(int32_t *samples, int count);
void ft8_init();
void ft8_abort();
void ft8_tx(char *message, int freq);
void ft8_tx_3f(const char* call_to, const char* call_de, const char* extra);
void ft8_poll(int tx_is_on);
float ft8_next_sample();
void ft8_process(char *message, ftx_operation operation);
int ft8_is_repeating();
// On-demand mid-cycle reset of the countdown, see its own comment.
void ft8_repeat_reset();
// Arms Auto CQ mode (queues the first CQ call and marks it to keep
// repeating indefinitely, resuming after each QSO, until aborted) --
// see the ft8_autocq_running comment in modem_ft8.c for the full design.
void ft8_autocq_start();
// Fully terminates Auto CQ mode -- called from abort_tx() only, not
// ft8_abort() (see ft8_autocq_stop()'s own comment for why).
void ft8_autocq_stop();
// Pause/resume transmission without touching QSO state -- user's own
// distinction: the existing "abort" (TX Enabled click, sbitx_daemon.c's
// abort_tx()) is for abandoning an attempt entirely and deliberately
// wipes the logger's CALL/exchange fields (see its own comment). This
// is for briefly suspending TX mid-QSO (e.g. to check what's actually
// sitting on your own TX frequency, invisible while transmitting) and
// resuming the *same* exchange after -- CALL/exchange fields are left
// alone. ft8_suspend() stops transmission immediately (tx_off()) and
// blocks ft8_poll() from starting anything new; ft8_resume() lets it
// resume normally on its own next opportunity.
void ft8_suspend();
void ft8_resume();
// Logs a QSO whose closing "73" was still repeating (ft8_qso_log_pending,
// modem_ft8.c) if one is pending, using whatever CALL/exchange fields are
// still valid right now -- a no-op otherwise. Must be called before any
// call_wipe() that could otherwise silently discard it; see abort_tx()
// (sbitx_daemon.c) for the real case this exists for.
void ft8_finalize_pending_qso();
// TEMPORARY, task #25 SIC validation only -- remove once done.
int ft8_decode_file(const char *path);

// Real task #25 ask: while continuing a QSO, restrict candidate search
// to a narrow window around the other station's already-known
// frequency instead of the whole band, so it can't lose a heap slot to
// unrelated traffic elsewhere in the passband. freq_hz < 0 means
// unlocked (search the whole band, the default/normal behavior). See
// its own definition in modem_ft8.c for the margin used.
void ft8_set_qso_lock(float freq_hz);

// Drains the FT8-decode-thread -> main-thread callsign->grid queue,
// writing each entry to the persistent directory (logbook.c). Main
// thread only -- see the queue's own comment in modem_ft8.c.
void ft8_grid_queue_drain(void);

// Gives a jt9-only catch (decoder-merge task, fed in via the
// FT8CONTINUE remote command) its own Band Activity/CQ Panel row,
// deduped against our own decoder's own catches -- see this function's
// own comment in modem_ft8.c for why this was needed at all (neither
// FT8CONTINUE nor ft8_process() ever displayed anything on their own).
// Call before ft8_process(message, FTX_CONTINUE_QSO), with the same
// untouched raw message text -- this tokenizes its own private copy.
void jt9_display_decode(const char *message);
