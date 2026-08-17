#include <stddef.h>

void logbook_add(char *contact_callsign, char *rst_sent, char *exchange_sent,
	char *rst_recv, char *exchange_recv);
int logbook_query(char *query, int from_id, char *result_file);
int logbook_count_dup(const char *callsign, int last_seconds);
int logbook_prev_log(const char *callsign, char *result);
int logbook_get_grids(void (*f)(char *,int));
void logbook_open();
bool logbook_grid_exists(char *id);
bool logbook_caller_exists(char * id);
void logbook_delete(int id);
int export_adif(char *path, char *start_date, char *end_date);
void message_add(char *mode, unsigned int frequency, int outgoing, char *message);
void band_freq_ensure_table(void);
void logbook_ensure_columns(void);
long band_freq_get(const char *band, const char *mode);
void band_freq_set(const char *band, const char *mode, long freq);
void band_freq_list(char *out, size_t out_size);
