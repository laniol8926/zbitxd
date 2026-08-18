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
// TEMPORARY, task #25 SIC validation only -- remove once done.
int ft8_decode_file(const char *path);
