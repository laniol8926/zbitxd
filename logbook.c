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
#include <netdb.h>
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

// Moved up from further down in this file (originally right before
// export_adif(), its other user) -- logbook_prev_log() below now needs
// it too, and C needs it declared before first use.
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

// Real report, live (2026-08-25): "i called KA1MXL again... no
// transmit" -- worked before, but never on the band actually in use
// (confirmed: "not on 30M" via the logbook). pre_ft8_check()'s dupe-
// confirmation gate (a deliberate safety step: a first click on an
// already-worked station doesn't transmit, a second confirming click
// does) used to call this with just the callsign, matching *any* prior
// QSO on *any* band/mode -- inconsistent with FT8_already_worked()
// (web/index.html), which already deliberately scopes "worked before"
// to the current band+mode ("working the same station again on a
// different band or mode is a legitimate new contact"). Scoped the
// same way here now: cur_freq_khz is looked up against bands[] (same
// table/logic export_adif() already uses) to bound the query to the
// current band; band-less callers (or a frequency outside every known
// band) fall back to the old unscoped match rather than silently
// hiding a real prior contact.
int logbook_prev_log(const char *callsign, char *result, long cur_freq_khz, const char *mode){
	char statement[1000], param[2000];
	sqlite3_stmt *stmt;
	long band_from = -1, band_to = -1;

	for (int j = 0; j < sizeof(bands)/sizeof(struct band_name); j++)
		if (bands[j].from <= cur_freq_khz && cur_freq_khz <= bands[j].to){
			band_from = bands[j].from;
			band_to = bands[j].to;
			break;
		}

	if (band_from >= 0 && mode && mode[0])
		sprintf(statement, "select * from logbook where "
			"callsign_recv=\"%s\" AND freq BETWEEN %ld AND %ld AND mode=\"%s\" ORDER BY id DESC",
			callsign, band_from, band_to, mode);
	else
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

// Live QSO broadcast to a logger (cqrlog etc) using WSJT-X's UDP
// NetworkMessage protocol, type 5 ("QSO Logged") -- a de-facto standard
// cqrlog/JTDX/GridTracker/N1MM all speak. Purely additive/best-effort:
// never touches whether the real SQLite insert in logbook_add() below
// succeeds, and silently does nothing at all if #udp_log_host isn't
// configured.
//
// Two things tried and ruled out first, both confirmed by reading
// cqrlog's own source (ok2cqr/cqrlog, src/fNewQSO.pas):
// - type 12 ("Logged ADIF", a self-contained ADIF-text record --
//   avoids binary date parsing entirely) isn't implemented by cqrlog
//   at all -- confirmed no match for it anywhere in their MsgType
//   dispatch, and confirmed live (packet verified byte-correct on
//   arrival, cqrlog did nothing with it).
// - type 5 byte-verified correct against WSJT-X's own spec, but a
//   real cqrlog import threw the *exact* text of a known cqrlog quirk
//   ("'' is not valid date error", their own comment: "usually at
//   first logged qso") -- turned out to be a real field-alignment bug
//   in *their* parser, not ours: cqrlog's Preferences has a "Mode
//   from" radio group (CQRLOG/wsjtx/default) that gates whether the
//   Mode string field even gets read off the wire at all; unless it's
//   set to "wsjtx" (cqrini value 1, the fresh-install default),
//   cqrlog silently skips over our Mode field's bytes, which
//   permanently misaligns every field after it -- including the
//   *second* QDateTime block -- for the rest of the message. Nothing
//   we send can route around a receiver-side setting; this needs
//   "Mode from" = wsjtx on the cqrlog side.
//
// All multi-byte integers are big-endian (Qt QDataStream convention).
// utf8 strings are a big-endian quint32 byte length followed by the raw
// bytes (no null terminator) -- write_utf8() below.

static void udp_write_u32(unsigned char *buf, size_t *pos, uint32_t v){
	buf[(*pos)++] = (v >> 24) & 0xff;
	buf[(*pos)++] = (v >> 16) & 0xff;
	buf[(*pos)++] = (v >> 8) & 0xff;
	buf[(*pos)++] = v & 0xff;
}

static void udp_write_u64(unsigned char *buf, size_t *pos, uint64_t v){
	for (int shift = 56; shift >= 0; shift -= 8)
		buf[(*pos)++] = (v >> shift) & 0xff;
}

static void udp_write_utf8(unsigned char *buf, size_t *pos, const char *s){
	uint32_t len = s ? strlen(s) : 0;
	udp_write_u32(buf, pos, len);
	memcpy(buf + *pos, s, len);
	*pos += len;
}

// QDateTime (schema 2): qint64 Julian day, quint32 ms-since-midnight,
// quint8 timespec. timespec=1 (UTC) needs no further bytes -- the
// simplest correct choice, and matches how this app already logs
// (gmtime()), avoiding the offset/timezone-name fields entirely.
// Byte-verified against cqrlog's own int64Buf/ui32Buf/ui8Buf field
// order for this exact block (fNewQSO.pas) -- structurally correct.
static void udp_write_qdatetime_utc(unsigned char *buf, size_t *pos, struct tm *tmp){
	// Standard Julian day number algorithm (proleptic Gregorian).
	int y = tmp->tm_year + 1900, m = tmp->tm_mon + 1, d = tmp->tm_mday;
	int a = (14 - m) / 12;
	int yy = y + 4800 - a;
	int mm = m + 12 * a - 3;
	int64_t jdn = d + (153 * mm + 2) / 5 + 365LL * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
	udp_write_u64(buf, pos, (uint64_t)jdn); // qint64, but never negative here
	uint32_t ms = (uint32_t)((tmp->tm_hour * 3600 + tmp->tm_min * 60 + tmp->tm_sec) * 1000);
	udp_write_u32(buf, pos, ms);
	buf[(*pos)++] = 1; // timespec = UTC
}

// Best-effort: any failure (bad host, socket error, cqrlog not
// running) is silently ignored -- this must never be allowed to affect
// the real logbook write in logbook_add().
static void udp_broadcast_qso_logged(const char *dx_call, const char *dx_grid,
	uint64_t freq_hz, const char *mode, const char *rst_sent, const char *rst_recv,
	const char *tx_power, const char *comments, struct tm *tmp,
	const char *mycall, const char *my_grid_sent,
	const char *exch_sent, const char *exch_recv){

	char host[64], port_s[8];
	get_field_value("#udp_log_host", host);
	if (!host[0])
		return; // disabled -- no host configured
	get_field_value("#udp_log_port", port_s);
	if (!port_s[0])
		strcpy(port_s, "2237");

	struct addrinfo hints, *res;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(host, port_s, &hints, &res) != 0)
		return;

	int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (s < 0){
		freeaddrinfo(res);
		return;
	}

	unsigned char buf[512];
	size_t pos = 0;
	// Must contain "WSJT" (case-sensitive) -- cqrlog's own parser only
	// reads the Operator call/My call/My grid/Exchange sent/Exchange
	// received tail of this message `if Pos('WSJT',RemoteName)>0`
	// (fNewQSO.pas, their comment: "no contest in JTDX"), gating on the
	// sender Id string itself.
	static const char *id = "zbitxd (WSJT-X protocol)";

	udp_write_u32(buf, &pos, 0xadbccbda); // magic
	udp_write_u32(buf, &pos, 2);          // schema 2 (avoids schema 3's extra tz fields)
	udp_write_u32(buf, &pos, 5);          // type 5 = QSO Logged
	udp_write_utf8(buf, &pos, id);        // sender Id -- the *only* Id field on the wire.
	// WSJT-X's own doc text lists a type-5-specific "Id (unique key)"
	// field right after this, which reads as a second wire field but
	// isn't one -- it's the same header Id referenced a second time in
	// their prose, with type-5-specific semantics attached, not an
	// actual extra string on the wire. Confirmed against cqrlog's own
	// parser (fNewQSO.pas): its type-5 handler does exactly one StrBuf()
	// read before the Date&Time Off QDateTime. Sending a real second
	// copy here (as an earlier version of this code did) shifts every
	// field after it by 10 bytes -- including both QDateTime blocks --
	// producing a garbage Julian day and the exact cqrlog "date error"
	// seen on a real import.
	udp_write_qdatetime_utc(buf, &pos, tmp); // Date & Time Off
	udp_write_utf8(buf, &pos, dx_call);
	udp_write_utf8(buf, &pos, dx_grid);
	udp_write_u64(buf, &pos, freq_hz);
	udp_write_utf8(buf, &pos, mode);
	udp_write_utf8(buf, &pos, rst_sent);
	udp_write_utf8(buf, &pos, rst_recv);
	udp_write_utf8(buf, &pos, tx_power);
	udp_write_utf8(buf, &pos, comments);
	udp_write_utf8(buf, &pos, "");        // Name -- not tracked
	udp_write_qdatetime_utc(buf, &pos, tmp); // Date & Time On -- same value, see comment above logbook_add()
	udp_write_utf8(buf, &pos, mycall);    // Operator call
	udp_write_utf8(buf, &pos, mycall);    // My call
	udp_write_utf8(buf, &pos, my_grid_sent);
	udp_write_utf8(buf, &pos, exch_sent);
	udp_write_utf8(buf, &pos, exch_recv);

	sendto(s, buf, pos, 0, res->ai_addr, res->ai_addrlen);
	close(s);
	freeaddrinfo(res);
}

void logbook_add(char *contact_callsign, char *rst_sent, char *exchange_sent,
	char *rst_recv, char *exchange_recv){
	char statement[1000], *err_msg, date_str[11], time_str[5];
	char freq[12], log_freq[12], mode[10], mycallsign[12], comments[200];
	char txpower[16], antenna[40], opcomments[80], mygrid[12];

	time_t log_time = time(NULL);
	struct tm *tmp = gmtime(&log_time);
	get_field_value("r1:freq", freq);
	get_field_value("r1:mode", mode);
	get_field_value("#mycallsign", mycallsign);
	get_field_value("#txpower", txpower);
	get_field_value("#antenna", antenna);
	get_field_value("#opcomments", opcomments);
	get_field_value("#mygrid", mygrid);

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

	// Live broadcast to a logger (cqrlog etc), after the real insert
	// above -- best-effort, no-op if #udp_log_host isn't configured.
	// log_freq is a kHz decimal string with 3 fractional digits (Hz
	// precision, see the comment above); converting through an
	// intermediate rounded-to-integer-kHz value first would throw that
	// precision away (e.g. "10137.640" kHz -> round to 10138 kHz ->
	// *1000 = 10,138,000 Hz, 360 Hz off from the real 10,137,640 Hz) --
	// go straight to Hz instead.
	udp_broadcast_qso_logged(contact_callsign, exchange_recv,
		(uint64_t)(atof(log_freq) * 1000.0 + 0.5), mode, rst_sent, rst_recv,
		txpower, comments, tmp, mycallsign, mygrid,
		exchange_sent, exchange_recv);
}

// ADIF field headers, see note above -- must stay in the same order as
// the logbook table's own columns (SELECT * in export_adif() below), and
// grow whenever a column does (TX_PWR added alongside the "power" column,
// see logbook_ensure_columns()).
//
// exch_sent/exch_recv hold grid squares for a normal (non-contest) QSO
// -- FT8's whole exchange *is* the grid -- so these need the standard
// ADIF grid fields (MY_GRIDSQUARE/GRIDSQUARE), not the contest-exchange
// ones (STX_String/SRX_String) used here before. Confirmed live: a real
// cqrlog import came in with the contact's locator missing, because
// cqrlog (correctly) doesn't treat SRX_String as a grid square at all.
// COMMENT (singular) -- not the plain-English "COMMENTS" used here
// before, which isn't a real ADIF field name at all. Same class of bug
// as the grid squares above: confirmed live, comments also came in
// missing on a real cqrlog import.
const static char *adif_names[]={"ID","MODE","FREQ","QSO_DATE","TIME_ON","OPERATOR","RST_SENT","MY_GRIDSQUARE","CALL","RST_RCVD","GRIDSQUARE","STX","COMMENT","TX_PWR"};

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

// Exports the whole logbook (or a qso_date range) to a correctly
// formatted ADIF 3.1.4 file. Deliberately no "already exported" flag/
// bookkeeping -- always exports fresh from scratch every time, relying
// on the importing logbook's own duplicate-QSO rejection (every real
// logger does this) rather than tracking export state here. Returns
// the number of QSOs written, or -1 on failure (bad path, or the query
// itself failing to prepare).
int export_adif(char *path, char *start_date, char *end_date){
	sqlite3_stmt *stmt;
	char statement[200], param[2000];

	if (db == NULL)
		logbook_open();

	sprintf(statement, "select * from logbook where (qso_date >= '%s' AND  qso_date <= '%s')  ORDER BY id DESC;",
		start_date, end_date);

	FILE *pf = fopen(path, "w");
	if (!pf)
		return -1;
	if (sqlite3_prepare_v2(db, statement, -1, &stmt, NULL) != SQLITE_OK){
		fclose(pf);
		return -1;
	}
	fprintf(pf, "ADIF export from zbitxd\n");
	// ADIF_VER, not "adif version" -- ADIF tag names can't contain
	// spaces; a strict/correct parser would never have recognized the
	// old malformed tag here as the version header at all.
	fprintf(pf, "<ADIF_VER:5>3.1.4\n");
	fprintf(pf, "<PROGRAMID:6>zbitxd\n");
	fprintf(pf, "<EOH>\n");

	int rec = 0;

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int i;
		int num_cols = sqlite3_column_count(stmt);
		for (i = 0; i < num_cols; i++){
			param[0] = 0; // SQLITE_NULL (or an unhandled type) leaves this field blank, not stale data from the previous column
			switch (sqlite3_column_type(stmt, i))
			{
			case (SQLITE3_TEXT):
				strcpy(param, (const char *)sqlite3_column_text(stmt, i));
				break;
			case (SQLITE_INTEGER):
				sprintf(param, "%d", sqlite3_column_int(stmt, i));
				break;
			case (SQLITE_FLOAT):
				sprintf(param, "%g", sqlite3_column_double(stmt, i));
				break;
			default:
				break;
			}
			if (i == 2){
				// logbook.freq is stored in kHz as a decimal string
				// (e.g. "14074.000", see logbook_add()'s dial+TX_PITCH
				// sum /1000.0) -- confirmed against the real deployed
				// database, not assumed. bands[]'s own ranges are kHz
				// too, so that comparison needs the raw value; ADIF's
				// <FREQ> tag wants MHz, hence the /1000 here.
				long freq_khz = atol(param);
				float freq_mhz = atof(param) / 1000.0;
				sprintf(param, "%.6f", freq_mhz);
				for (int j = 0 ; j < sizeof(bands)/sizeof(struct band_name); j++)
					if (bands[j].from <= freq_khz && freq_khz <= bands[j].to){
						fprintf(pf, "<BAND:%d>%s ", (int)strlen(bands[j].name), bands[j].name);
						break;
					}
			}
			else if (i == 3) //it is the date
				strip_chr(param, '-');
			fprintf(pf, "<%s:%d>%s ", adif_names[i], (int)strlen(param), param);
		}
		fprintf(pf, "<EOR>\n");
		++rec;
	}
	sqlite3_finalize(stmt);
	fclose(pf);
	return rec;
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

// Persistent callsign->grid directory, backing the web UI's Grid Map.
// Same self-healing CREATE TABLE IF NOT EXISTS story as
// band_freq_ensure_table() just above -- an already-deployed sbitx.db
// picks this up on next restart, no fresh-install-only path needed.
// Bulk-seeded once from the FCC ULS database (scripts/seed_callsign_grid.py,
// writing directly via the sqlite3 CLI, source='uls') and kept fresh by
// every live FT8 CQ decode (modem_ft8.c, via callsign_grid_set() below,
// source='decode') -- see this feature's own design note for why CQ only:
// the CQ message is the one unambiguous case where a station is
// self-broadcasting its own grid to nobody in particular.
void callsign_grid_ensure_table(void){
	char *err_msg;

	if (db == NULL)
		logbook_open();

	sqlite3_exec(db,
		"CREATE TABLE IF NOT EXISTS callsign_grid ("
		"callsign TEXT PRIMARY KEY NOT NULL,"
		"grid TEXT NOT NULL,"
		"source TEXT NOT NULL DEFAULT 'decode',"
		"updated_at INTEGER NOT NULL DEFAULT 0);", 0, 0, &err_msg);
}

// Last-write-wins by design: a live decode overwriting a ULS-seeded
// mailing-address-derived grid with the station's own self-reported CQ
// grid is strictly more accurate, and a station that's moved or been
// reissued should update too. Always stamps source='decode' -- the
// seed script's own writes go directly via the sqlite3 CLI, never
// through this function, so there's no need for a source parameter.
void callsign_grid_set(const char *callsign, const char *grid){
	sqlite3_stmt *stmt;

	if (db == NULL)
		logbook_open();

	if (sqlite3_prepare_v2(db,
			"INSERT INTO callsign_grid (callsign, grid, source, updated_at) VALUES(?,?,'decode',?)"
			" ON CONFLICT(callsign) DO UPDATE SET grid=excluded.grid, source='decode', updated_at=excluded.updated_at;",
			-1, &stmt, NULL) != SQLITE_OK)
		return;
	sqlite3_bind_text(stmt, 1, callsign, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, grid, -1, SQLITE_STATIC);
	sqlite3_bind_int64(stmt, 3, (sqlite3_int64)time(NULL));
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

// Returns true and fills out (caller-owned, out_size >= 5) if this
// callsign's grid is already known -- either bulk-seeded or previously
// decoded. Used by webserver.c's on-demand gridlookup= handler.
bool callsign_grid_get(const char *callsign, char *out, size_t out_size){
	sqlite3_stmt *stmt;
	bool found = false;

	if (db == NULL)
		logbook_open();

	if (sqlite3_prepare_v2(db,
			"SELECT grid FROM callsign_grid WHERE callsign=?;",
			-1, &stmt, NULL) != SQLITE_OK)
		return false;
	sqlite3_bind_text(stmt, 1, callsign, -1, SQLITE_STATIC);
	if (sqlite3_step(stmt) == SQLITE_ROW){
		const unsigned char *g = sqlite3_column_text(stmt, 0);
		if (g){
			strncpy(out, (const char*)g, out_size - 1);
			out[out_size - 1] = 0;
			found = true;
		}
	}
	sqlite3_finalize(stmt);
	return found;
}
