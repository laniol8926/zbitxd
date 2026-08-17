#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h> 
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include <unistd.h>
#include <linux/types.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>
#include <sys/types.h>
#include <stdint.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <ctype.h>
#include <arpa/inet.h>
#include "sdr.h"
#include "sdr_ui.h"
#include "logbook.h"
#include "configure.h"

#include <sqlite3.h>

static int rc;
static sqlite3 *db=NULL;

void logbook_open();
int logbook_fill(int from_id, int count, char *query);

/* writes the output to /tmp/zbitx_result_rows.txt
	if the from_id is negative, it returns the later 50 records (higher id)
	if the from_id is positive, it returns the prior 50 records (lower id) */

int logbook_query(char *query, int from_id, char *result_file){
	sqlite3_stmt *stmt;
	char statement[200], param[2000];

	if (db == NULL)
		logbook_open();

	//add to the bottom of the logbook
	if (from_id > 0){
		if (query)
			sprintf(statement, "select * from logbook "
				"where (callsign_recv LIKE '%s%%' AND id < %d) ",
				query, from_id);
		else
			sprintf(statement, "select * from logbook where id < %d ", from_id);
	}
	//last 50 QSOs
	else if (from_id == 0){
		if (query)
			sprintf(statement, "select * from logbook "
				"where callsign_recv LIKE '%s%%' ", query);
		else
			strcpy(statement, "select * from logbook ");
	}
	//latest QSOs after from_id (top of the log)
	else {
		if (query)
			sprintf(statement, "select * from logbook "
				"where (callsign_recv LIKE '%s%%' AND id > %d) ",
				query, -from_id);
		else 
			sprintf(statement, "select * from logbook where id > %d ", -from_id); 
	}
	strcat(statement, "ORDER BY id DESC LIMIT 50;");

	//printf("[%s]\n", statement);
	sqlite3_prepare_v2(db, statement, -1, &stmt, NULL);

	const char *output_path = "/tmp/zbitx_result_rows.txt";
	strcpy(result_file, output_path);
	
	FILE *pf = fopen(output_path, "w");
	if (!pf)
		return -1;

	int rec = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int i;
		int num_cols = sqlite3_column_count(stmt);
		for (i = 0; i < num_cols; i++){
			switch (sqlite3_column_type(stmt, i))
			{
			case (SQLITE3_TEXT):
				strcpy(param, sqlite3_column_text(stmt, i));
				break;
			case (SQLITE_INTEGER):
				sprintf(param, "%d", sqlite3_column_int(stmt, i));
				break;
			case (SQLITE_FLOAT):
				sprintf(param, "%g", sqlite3_column_double(stmt, i));
				break;
			case (SQLITE_NULL):
				break;
			default:
				sprintf(param, "%d", sqlite3_column_type(stmt, i));
				break;
			}
			//printf("%s|", param);
			fprintf(pf, "%s|", param);
		}
		//printf("\n");
		fprintf(pf, "\n");
	}
	sqlite3_finalize(stmt);
	fclose(pf);
	return rec;
}

int logbook_count_dup(const char *callsign, int last_seconds){
	char date_str[100], time_str[100], statement[1000];
	sqlite3_stmt *stmt;

	time_t log_time = time(NULL) - last_seconds;
	struct tm *tmp = gmtime(&log_time);
	sprintf(date_str, "%04d-%02d-%02d", tmp->tm_year + 1900, tmp->tm_mon + 1, tmp->tm_mday);
	sprintf(time_str, "%02d%02d", tmp->tm_hour, tmp->tm_min);
	
	sprintf(statement, "select * from logbook where "
		"callsign_recv=\"%s\" AND qso_date >= \"%s\" AND qso_time >= \"%s\"",
		callsign, date_str, time_str);

	sqlite3_prepare_v2(db, statement, -1, &stmt, NULL);
	int rec = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		rec++;
	}
	sqlite3_finalize(stmt);
	return rec;
}

int logbook_get_grids(void (*f)(char *,int)) {
	sqlite3_stmt *stmt;

	char *statement = "SELECT exch_recv, COUNT(*) AS n FROM logbook "
		"GROUP BY exch_recv order by exch_recv";

	int res = sqlite3_prepare_v2(db, statement, -1, &stmt, NULL);
	//printf("%s : %d\n", statement, res);
	int cnt = 0;
	char grid[10];
	int n = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int num_cols = sqlite3_column_count(stmt);
		for (int i = 0; i < num_cols; i++){
			char const *col_name = sqlite3_column_name(stmt, i);
			if (!strcmp(col_name, "exch_recv")) { 
				strcpy(grid, sqlite3_column_text(stmt, i));
			} else
			if (!strcmp(col_name, "n")) { 
				n = sqlite3_column_int(stmt, i);
			}
		}
		f(grid,n);
		cnt++;
	}
	sqlite3_finalize(stmt);
	return cnt;
}

bool logbook_caller_exists(char * id) {
	sqlite3_stmt *stmt;
	char * statement = "SELECT EXISTS(SELECT 1 FROM logbook WHERE callsign_recv=?)";
	int res = sqlite3_prepare_v2(db, statement, -1, &stmt, NULL);
	if (res != SQLITE_OK) return false;
	bool exists = false;
	res = sqlite3_bind_text(stmt, 1, id, strlen(id), SQLITE_STATIC);
	if (res == SQLITE_OK) {
		res = sqlite3_step(stmt);
		int i = sqlite3_column_int(stmt, 0);
		exists = ( res == SQLITE_ROW && i != 0);
	}
	sqlite3_finalize(stmt);
	return exists;
}

bool logbook_grid_exists(char *id) {
	sqlite3_stmt *stmt;
	char * statement = "SELECT EXISTS(SELECT 1 FROM logbook WHERE exch_recv=?)";
	int res = sqlite3_prepare_v2(db, statement, -1, &stmt, NULL);
	if (res != SQLITE_OK) return false;
	bool exists = false;
	res = sqlite3_bind_text(stmt, 1, id, strlen(id), SQLITE_STATIC);
	if (res == SQLITE_OK) {
		res = sqlite3_step(stmt);
		int i = sqlite3_column_int(stmt, 0);
		exists = ( res == SQLITE_ROW && i != 0);
	}
	sqlite3_finalize(stmt);
	return exists;
}

int logbook_prev_log(const char *callsign, char *result){
	char statement[1000], param[2000];
	sqlite3_stmt *stmt;

	sprintf(statement, "select * from logbook where "
		"callsign_recv=\"%s\" ORDER BY id DESC",
		callsign);
	strcpy(result, callsign);
	strcat(result, ": ");
	int res = sqlite3_prepare_v2(db, statement, -1, &stmt, NULL);
	//printf("%s : %d\n", statement, res);
	int rec = 0;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int i;
		int num_cols = sqlite3_column_count(stmt);
		if (rec == 0) {

			for (i = 0; i < num_cols; i++){
				char const *col_name = sqlite3_column_name(stmt, i);
			    if (!strcmp(col_name, "id")) { continue; }
				if (!strcmp(col_name, "callsign_recv")) { continue; }
				switch (sqlite3_column_type(stmt, i))
				{
				case (SQLITE3_TEXT):
					strcpy(param, sqlite3_column_text(stmt, i));
					break;
				case (SQLITE_INTEGER):
					sprintf(param, "%d", sqlite3_column_int(stmt, i));
					break;
				case (SQLITE_FLOAT):
					sprintf(param, "%g", sqlite3_column_double(stmt, i));
					break;
				case (SQLITE_NULL):
					break;
				default:
					sprintf(param, "%d", sqlite3_column_type(stmt, i));
					break;
				}
				//printf("%s : %s\n", col_name, param);
				strcat(result, param);
				if (!strcmp(col_name, "qso_date")) strcat(result, "_");
				else strcat(result, " ");
			}
		}
		rec++;
	}
	sqlite3_finalize(stmt);
	sprintf(param, ": %d", rec);
	strcat(result, param);
	/*if (rec > 1) {
		sprintf(param, "\nand %d more.", rec-1);
		strcat(result, param);
	} else
	if (rec == 0) {
		sprintf(result, "%s not logged.", callsign);
	}*/
	return rec;
}

void logbook_open(){
	const char *db_path = STATEDIR "/sbitx.db";

	rc = sqlite3_open(db_path, &db);
}

void message_add(char *mode, unsigned int frequency, int outgoing, char *message){
	char date_str[10], time_str[10], freq_str[12], statement[1000], *err_msg;
	static int err_output = 1;

	/* get the frequency */
	get_field_value("r1:freq", freq_str);
	frequency = frequency + atoi(freq_str);

	/* get the time */
	time_t log_time = time(NULL);
	struct tm *tmp = gmtime(&log_time);

	int date_utc = ((tmp->tm_year + 1900)*10000) 
		+ ((tmp->tm_mon+1) * 100) + (tmp->tm_mday);
	int time_utc = (tmp->tm_hour * 10000) + (tmp->tm_min * 100) + tmp->tm_sec;

	sprintf(statement,
		"INSERT INTO messages (mode, freq, qso_date, qso_time, is_outgoing, data)"
		" VALUES('%s', '%d', '%d', '%d',  '%d','%s');",
			mode, frequency, date_utc, time_utc, outgoing, message);

	if (db == NULL)
		logbook_open();

	int res = sqlite3_exec(db, statement, 0,0, &err_msg);
	if (res != 0 && err_output) {
		printf("message_add: db err %d %s\n", res, err_msg);
		if (err_msg) sqlite3_free(err_msg);
		// only complain once, if the error is "no such table"
		// (it's quite alright to delete this table to avoid constant writing to the SSD)
		if (res == 1)
			err_output = 0;
	}
}

void logbook_add(char *contact_callsign, char *rst_sent, char *exchange_sent,
	char *rst_recv, char *exchange_recv){
	char statement[1000], *err_msg, date_str[11], time_str[5];
	char freq[12], log_freq[12], mode[10], mycallsign[12], comments[200];
	char txpower[16], antenna[40], opcomments[80];

	time_t log_time = time(NULL);
	struct tm *tmp = gmtime(&log_time);
	get_field_value("r1:freq", freq);
	get_field_value("r1:mode", mode);
	get_field_value("#mycallsign", mycallsign);
	get_field_value("#txpower", txpower);
	get_field_value("#antenna", antenna);
	get_field_value("#opcomments", opcomments);

	// r1:freq alone is just the dial/LO frequency -- audio (FT8/FT4
	// tone, CW sidetone) is generated at TX_PITCH above it and the
	// actual on-air QSO frequency is dial + audio offset (USB
	// convention, always upper sideband regardless of band in this
	// app), not the bare dial reading. Integer /1000 division used to
	// floor away the sub-kHz remainder entirely (e.g. a 1866 Hz offset
	// just vanished instead of showing as .866) -- keep it as the
	// actual fractional kHz instead, same units export_adif() already
	// expects (its own /1000.0 to MHz still works unchanged).
	sprintf(log_freq, "%.3f", (atoi(freq) + field_int("TX_PITCH")) / 1000.0);

	sprintf(date_str, "%04d-%02d-%02d", tmp->tm_year + 1900, tmp->tm_mon + 1, tmp->tm_mday);
	sprintf(time_str, "%02d%02d", tmp->tm_hour, tmp->tm_min);

	// generic-rig backend: record which rig actually made this QSO.
	// RIGMODEL holds "<hamlib model id> <description>" (e.g. "1045
	// M0NKA mcHF QRP") once picked from the rig catalog -- the id is
	// the same number passed as rigctld's own "-m" argument in
	// rig_generic_connect(), but the id alone means nothing to a human
	// reading the logbook later, so store the description past the
	// first space instead. Falls back to the bare field (whatever it
	// is) if no description is present. zBitx-hardware mode has no
	// rigctld/-m at all, so leave blank.
	comments[0] = 0;
	if (generic_rig_mode) {
		const char *rigmodel = field_str("RIGMODEL");
		const char *desc = strchr(rigmodel, ' ');
		snprintf(comments, sizeof(comments), "rig %s", desc ? desc + 1 : rigmodel);
	}
	// Antenna and Comments settings fold into this same column alongside
	// the rig info above, rather than getting their own columns --
	// user's own call. TX Power gets its own real column instead (see
	// the INSERT below).
	if (antenna[0]){
		if (comments[0]) strncat(comments, "; ", sizeof(comments) - strlen(comments) - 1);
		strncat(comments, antenna, sizeof(comments) - strlen(comments) - 1);
	}
	if (opcomments[0]){
		if (comments[0]) strncat(comments, "; ", sizeof(comments) - strlen(comments) - 1);
		strncat(comments, opcomments, sizeof(comments) - strlen(comments) - 1);
	}

	sprintf(statement,
		"INSERT INTO logbook (freq, mode, qso_date, qso_time, callsign_sent,"
		"rst_sent, exch_sent, callsign_recv, rst_recv, exch_recv, comments, power) "
		"VALUES('%s', '%s', '%s', '%s',  '%s','%s','%s',  '%s','%s','%s', '%s', '%s');",
			log_freq, mode, date_str, time_str, mycallsign,
			 rst_sent, exchange_sent, contact_callsign, rst_recv, exchange_recv, comments, txpower);

	if (db == NULL)
		logbook_open();

	int res = sqlite3_exec(db, statement, 0,0, &err_msg);
	if (res != 0) {
		printf("logbook_add db: %d err=%s", res, err_msg);
		if (err_msg) sqlite3_free(err_msg);
	}
}

// ADIF field headers, see note above
const static char *adif_names[]={"ID","MODE","FREQ","QSO_DATE","TIME_ON","OPERATOR","RST_SENT","STX_String","CALL","RST_RCVD","SRX_String","STX","COMMENTS"};

struct band_name {
	char *name;
	int from, to;
} bands[] = {
	{"160M", 1800, 2000},
	{"80M", 3500, 4000},
	{"60M", 5000, 5500},
	{"40M", 7000, 7300},
	{"30M", 10000, 10150},
	{"20M", 14000, 14350},
	{"17M", 18000, 18200},
	{"15M", 21000, 21450},
	{"12M", 24800, 25000},
	{"10M", 28000, 29700},
};

static void strip_chr(char *str, const char to_remove){
    int i, j, len;

    len = strlen(str);
    for(i=0; i<len; i++) {
        if(str[i] == to_remove) {
            for(j=i; j<len; j++)
                str[j] = str[j+1];
            len--;
            i--;
        }
    }
}

int export_adif(char *path, char *start_date, char *end_date){
	sqlite3_stmt *stmt;
	char statement[200], param[2000], qso_band[20];
	

	//add to the bottom of the logbook
	sprintf(statement, "select * from logbook where (qso_date >= '%s' AND  qso_date <= '%s')  ORDER BY id DESC;",
		start_date, end_date);

	FILE *pf = fopen(path, "w");
	sqlite3_prepare_v2(db, statement, -1, &stmt, NULL);
	fprintf(pf, "/ADIF file\n");
	fprintf(pf, "generated from sBITX log db by Log2ADIF program\n");	
	fprintf(pf, "<adif version:5>3.1.4\n");	
	fprintf(pf, "<EOH>\n");	

	int rec = 0;

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int i;
		int num_cols = sqlite3_column_count(stmt);
		for (i = 0; i < num_cols; i++){
			switch (sqlite3_column_type(stmt, i))
			{
			case (SQLITE3_TEXT):
				strcpy(param, sqlite3_column_text(stmt, i));
				break;
			case (SQLITE_INTEGER):
				sprintf(param, "%d", sqlite3_column_int(stmt, i));
				break;
			case (SQLITE_FLOAT):
				sprintf(param, "%g", sqlite3_column_double(stmt, i));
				break;
			case (SQLITE_NULL):
				break;
			default:
				sprintf(param, "%d", sqlite3_column_type(stmt, i));
				break;
			}
			if (i == 2){
				long f = atoi(param);
				float ffreq=atof(param)/1000.0;  // convert kHz to MHz
				sprintf(param, "%.3f",ffreq); // write out with 3 decimal digits
				for (int j = 0 ; j < sizeof(bands)/sizeof(struct band_name); j++)
					if (bands[j].from <= f && f <= bands[j].to){
						fprintf(pf, "<BAND:%d>%s", strlen(bands[j].name), bands[j].name); 
					}
			}
			else if (i == 3) //it is the date
				strip_chr(param, '-');
	   	fprintf(pf, "<%s:%d>%s", adif_names[i], strlen(param), param);
		}
		fprintf(pf, "<EOR>\n");
		//printf("\n");
	}
	sqlite3_finalize(stmt);
	fclose(pf);
}

int logbook_fill(int from_id, int count, char *query){
	sqlite3_stmt *stmt;
	char statement[200], param[2000];

	if (db == NULL)
		logbook_open();

	//add to the bottom of the logbook
	if (from_id > 0){
		if (query)
			sprintf(statement, "select * from logbook "
				"where (callsign_recv LIKE '%s%%' AND id < %d) ",
				query, from_id);
		else
			sprintf(statement, "select * from logbook where id < %d ", from_id);
	}
	//last 200 QSOs
	else if (from_id == 0){
		if (query)
			sprintf(statement, "select * from logbook "
				"where callsign_recv LIKE '%s%%' ", query);
		else
			strcpy(statement, "select * from logbook ");
	}
	//latest QSOs after from_id (top of the log)
	else {
		if (query)
			sprintf(statement, "select * from logbook "
				"where (callsign_recv LIKE '%s%%' AND id > %d) ",
				query, -from_id);
		else 
			sprintf(statement, "select * from logbook where id > %d ", -from_id); 
	}
	char stmt_count[100];
	sprintf(stmt_count, "ORDER BY id DESC LIMIT %d;", count);
	strcat(statement, stmt_count);
	//printf("[%s]\n", statement);
	sqlite3_prepare_v2(db, statement, -1, &stmt, NULL);

	int rec = 0;

	char id[10], qso_time[20], qso_date[20], freq[20], mode[20], callsign[20],
	rst_recv[20], exchange_recv[20], rst_sent[20], exchange_sent[20], comments[1000];

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int i;
		int num_cols = sqlite3_column_count(stmt);
		for (i = 0; i < num_cols; i++){

			char const *col_name = sqlite3_column_name(stmt, i);
			if (!strcmp(col_name, "id"))
				strcpy(id, sqlite3_column_text(stmt, i));
			else if (!strcmp(col_name, "qso_date"))
				strcpy(qso_date, sqlite3_column_text(stmt, i));
			else if (!strcmp(col_name, "qso_time"))
				strcpy(qso_time, sqlite3_column_text(stmt, i));
			else if (!strcmp(col_name, "qso_time"))
				strcpy(qso_time, sqlite3_column_text(stmt, i));
			else if (!strcmp(col_name, "freq"))
				strcpy(freq, sqlite3_column_text(stmt, i));
			else if (!strcmp(col_name, "mode"))
				strcpy(mode, sqlite3_column_text(stmt, i));
			else if (!strcmp(col_name, "callsign_recv"))
				strcpy(callsign, sqlite3_column_text(stmt, i));
			else if (!strcmp(col_name, "rst_sent"))
				strcpy(rst_sent, sqlite3_column_text(stmt, i));
			else if (!strcmp(col_name, "rst_recv"))
				strcpy(rst_recv, sqlite3_column_text(stmt, i));
			else if (!strcmp(col_name, "exch_sent"))
				strcpy(exchange_sent, sqlite3_column_text(stmt, i));
			else if (!strcmp(col_name, "exch_recv"))
				strcpy(exchange_recv, sqlite3_column_text(stmt, i));
			else if (!strcmp(col_name, "comments"))
				strcpy(comments, sqlite3_column_text(stmt, i));
		}
	}
	sqlite3_finalize(stmt);
}

void logbook_delete(int id){
	char statement[100], *err_msg;
	sprintf(statement, "DELETE FROM logbook WHERE id='%d';", id);
	sqlite3_exec(db, statement, 0,0, &err_msg);
}

// WSJT-X-style per-band/per-mode dial frequency table (Settings >
// Frequencies there). Standard, widely-published FT8/FT4 calling
// frequencies, used only to seed the table on first run -- INSERT OR
// IGNORE means a user's own edit (band_freq_set()) always wins once a
// row exists.
struct band_freq_default {
	char *band;
	char *mode;
	long freq;
};

static struct band_freq_default band_freq_defaults[] = {
	{"80M", "FT8", 3573000}, {"80M", "FT4", 3575000},
	{"40M", "FT8", 7074000}, {"40M", "FT4", 7047500},
	{"30M", "FT8", 10136000}, {"30M", "FT4", 10140000},
	{"20M", "FT8", 14074000}, {"20M", "FT4", 14080000},
	{"17M", "FT8", 18100000}, {"17M", "FT4", 18104000},
	{"15M", "FT8", 21074000}, {"15M", "FT4", 21140000},
	{"12M", "FT8", 24915000}, {"12M", "FT4", 24919000},
	{"10M", "FT8", 28074000}, {"10M", "FT4", 28180000},
};

// Same idea as band_freq_ensure_table() below, but for adding a column
// to an already-existing table rather than the whole table -- an
// already-deployed sbitx.db (real logged QSOs, never recreated
// wholesale) needs this too, not just data/create_db.sql's fresh-install
// schema. Checks PRAGMA table_info() first rather than just running
// "ALTER TABLE ... ADD COLUMN" unconditionally: this deployment's
// sqlite3 (3.27, Debian-vintage) predates "ADD COLUMN IF NOT EXISTS"
// (added in 3.35), and ALTER TABLE ADD COLUMN on a column that already
// exists is a hard error, unlike CREATE TABLE IF NOT EXISTS.
void logbook_ensure_columns(void){
	sqlite3_stmt *stmt;
	int has_power = 0;
	char *err_msg;

	if (db == NULL)
		logbook_open();

	sqlite3_prepare_v2(db, "PRAGMA table_info(logbook);", -1, &stmt, NULL);
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *col_name = (const char *)sqlite3_column_text(stmt, 1);
		if (col_name && !strcmp(col_name, "power"))
			has_power = 1;
	}
	sqlite3_finalize(stmt);

	if (!has_power)
		sqlite3_exec(db, "ALTER TABLE logbook ADD COLUMN power TEXT DEFAULT '';", 0, 0, &err_msg);
}

// CREATE TABLE IF NOT EXISTS here (not just in data/create_db.sql) so an
// already-deployed sbitx.db -- which already has real logged QSOs in it,
// not something to ever recreate wholesale -- picks up this table too.
// data/create_db.sql only ever runs against a brand new database (see
// the Makefile's install target).
void band_freq_ensure_table(void){
	char *err_msg;

	if (db == NULL)
		logbook_open();

	sqlite3_exec(db,
		"CREATE TABLE IF NOT EXISTS band_frequencies ("
		"id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,"
		"band TEXT NOT NULL, mode TEXT NOT NULL, freq INTEGER NOT NULL,"
		"UNIQUE(band, mode));", 0, 0, &err_msg);

	int n = sizeof(band_freq_defaults) / sizeof(struct band_freq_default);
	for (int i = 0; i < n; i++){
		char statement[200];
		snprintf(statement, sizeof(statement),
			"INSERT OR IGNORE INTO band_frequencies (band, mode, freq)"
			" VALUES('%s','%s','%ld');",
			band_freq_defaults[i].band, band_freq_defaults[i].mode,
			band_freq_defaults[i].freq);
		sqlite3_exec(db, statement, 0, 0, &err_msg);
	}
}

// Returns -1 if this band/mode has no row yet (shouldn't normally happen
// once band_freq_ensure_table() has seeded the defaults, but callers
// should still treat <=0 as "no known frequency" rather than assuming).
long band_freq_get(const char *band, const char *mode){
	sqlite3_stmt *stmt;
	long freq = -1;

	if (db == NULL)
		logbook_open();

	if (sqlite3_prepare_v2(db,
			"SELECT freq FROM band_frequencies WHERE band=? AND mode=?;",
			-1, &stmt, NULL) != SQLITE_OK)
		return -1;
	sqlite3_bind_text(stmt, 1, band, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, mode, -1, SQLITE_STATIC);
	if (sqlite3_step(stmt) == SQLITE_ROW)
		freq = sqlite3_column_int64(stmt, 0);
	sqlite3_finalize(stmt);
	return freq;
}

void band_freq_set(const char *band, const char *mode, long freq){
	sqlite3_stmt *stmt;

	if (db == NULL)
		logbook_open();

	if (sqlite3_prepare_v2(db,
			"INSERT INTO band_frequencies (band, mode, freq) VALUES(?,?,?)"
			" ON CONFLICT(band, mode) DO UPDATE SET freq=excluded.freq;",
			-1, &stmt, NULL) != SQLITE_OK)
		return;
	sqlite3_bind_text(stmt, 1, band, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, mode, -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt, 3, freq);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

// Plain-text dump for the client's Frequencies settings table -- one
// "band mode freq" line per row, same style as
// rig_generic_list_serial_devices()/rig_generic_list_audio_devices().
void band_freq_list(char *out, size_t out_size){
	sqlite3_stmt *stmt;
	size_t used = 0;

	out[0] = 0;
	if (db == NULL)
		logbook_open();

	if (sqlite3_prepare_v2(db,
			"SELECT band, mode, freq FROM band_frequencies ORDER BY id;",
			-1, &stmt, NULL) != SQLITE_OK)
		return;

	while (sqlite3_step(stmt) == SQLITE_ROW){
		char line[64];
		int n = snprintf(line, sizeof(line), "%s %s %d\n",
			sqlite3_column_text(stmt, 0), sqlite3_column_text(stmt, 1),
			sqlite3_column_int(stmt, 2));
		if (n > 0 && used + (size_t)n < out_size){
			memcpy(out + used, line, (size_t)n);
			used += (size_t)n;
		}
	}
	sqlite3_finalize(stmt);
	out[used] = 0;
}
