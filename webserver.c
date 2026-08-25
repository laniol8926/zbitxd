// Based on https://mongoose.ws/tutorials/websocket-server/

#include "mongoose.h"
#include "webserver.h"
#include <pthread.h>
#include <math.h>
#include <complex.h>
#include <fftw3.h>
#include "sdr.h"
#include "sdr_ui.h"
#include "logbook.h"
#include "hist_disp.h"
#include "configure.h"
#include "rig_generic.h"

static const char *s_listen_on = "ws://0.0.0.0:8080";
static const char *s_web_root = SHAREDIR "/web";
static char session_cookie[100];
static struct mg_mgr mgr;  // Event manager

// Real report, user's own framing: this app is single-control-operator
// only (one session_cookie, period -- see web_despatcher()'s own check),
// but nothing at all ran when that one session's connection dropped --
// a network failure, a locked phone, a closed tab, all left Auto CQ (and
// any other in-flight/queued transmission) running completely unattended
// server-side, indefinitely, with no operator present to stop it. That's
// not a UI inconvenience, it's a real regulatory problem (unattended
// automatic transmission without a control operator present) -- "some
// hams would love that... set repeats to 9999, go to bed, wake up to 900
// QSOs logged" is exactly the failure mode this exists to prevent, not
// enable. Tracks which single connection is the currently-authenticated
// one (set in do_login(), matching the single-session design already in
// place) so fn()'s MG_EV_CLOSE/MG_EV_ERROR handler can tell whether the
// connection that just dropped was genuinely the operator's, not some
// unrelated/failed connection attempt that never even logged in.
static struct mg_connection *authenticated_conn = NULL;

static void web_respond(struct mg_connection *c, char *message){
	mg_ws_send(c, message, strlen(message), WEBSOCKET_OP_TEXT);
}

static void get_console(struct mg_connection *c){
	char buff[2100];
	int n = web_get_console(buff, 2000);
	if (!n)
		return;
	//char first20 [21]; strncpy(first20, buff, 20); first20[20] = 0;  tlog("get_console", first20, n);
	mg_ws_send(c, buff, strlen(buff), WEBSOCKET_OP_TEXT);
}

static void get_updates(struct mg_connection *c, int all){
	//send the settings of all the fields to the client
	char buff[2000];
	int i = 0;

	get_console(c);

	while(1){
		int update = remote_update_field(i, buff);
		// return of -1 indicates the eof fields
		if (update == -1)
			return;
	//send the status anyway
		if (all || update )
			mg_ws_send(c, buff, strlen(buff), WEBSOCKET_OP_TEXT); 
		i++;
	}
}

static void do_login(struct mg_connection *c, char *key){

	char passkey[20];
	get_field_value("#passkey", passkey);

	//look for key only on non-local ip addresses
	if ((!key || strcmp(passkey, key)) && (c->rem.ip != 16777343)){
		web_respond(c, "login error");
		c->is_draining = 1;
		printf("passkey didn't match. Closing socket\n");
		return;
	}

	hd_createGridList(); // llh: make the list up to date at the beginning of a session

	// Same "always reset to manual" default main() forces at daemon
	// startup (see its own comment) -- but that's a one-shot, so a
	// client reload/reconnect hours into an already-running daemon
	// never saw it, and could show Auto CQ/A. Ans still checked from
	// whatever was last selected. User: "the default on reload is that
	// both the auto cq and the auto answer check boxes should be
	// unchecked" -- every login gets the same fresh-start default, not
	// just the very first one after boot.
	set_field("#ft8_auto", "OFF");

	sprintf(session_cookie, "%x", rand());
	// See authenticated_conn's own comment -- this is what lets a
	// disconnect of *this* connection specifically (not some unrelated
	// one) trigger the safety stop in fn()'s MG_EV_CLOSE/MG_EV_ERROR
	// handler. A fresh login from a new device is a deliberate handoff to
	// a new, present operator, not the unattended case this guards
	// against -- deliberately not stopping Auto CQ here, just retargeting
	// which connection is being watched.
	authenticated_conn = c;
	char response[100];
	sprintf(response, "login %s", session_cookie);
	web_respond(c, response);
	get_updates(c, 1);
}

static int16_t remote_samples[10000]; //the max samples are set by the queue lenght in modems.c

static void get_spectrum(struct mg_connection *c){
	char buff[3000];
	web_get_spectrum(buff);
	mg_ws_send(c, buff, strlen(buff), WEBSOCKET_OP_TEXT);
	get_updates(c, 0);
}

static void get_audio(struct mg_connection *c){
	char buff[3000];
	web_get_spectrum(buff);
	mg_ws_send(c, buff, strlen(buff), WEBSOCKET_OP_TEXT);
	get_updates(c, 0);

	int count = remote_audio_output(remote_samples);		
	if (count > 0)
		mg_ws_send(c, remote_samples, count * sizeof(int16_t), WEBSOCKET_OP_BINARY);
}

static void get_logs(struct mg_connection *c, char *args){
	char logbook_path[PATH_MAX];
	char row_response[1000], row[1000];
	char query[100];
	int	row_id;

	query[0] = 0;
	row_id = atoi(strtok(args, " "));
	logbook_query(strtok(NULL, " \t\n"), row_id, logbook_path);
	FILE *pf = fopen(logbook_path, "r");
	if (!pf)
		return;
	while(fgets(row, sizeof(row), pf)){
		sprintf(row_response, "QSO %s", row);
		web_respond(c, row_response); 
	}
	fclose(pf);
}

void get_macros_list(struct mg_connection *c){
	char macros_list[2000], out[3000];
	macro_list(macros_list);
	sprintf(out, "macros_list %s", macros_list);
	web_respond(c, out);
}

void get_macro_labels(struct mg_connection *c){
	char key_list[2000], out[3000];
	macro_get_keys(key_list);
	sprintf(out, "macro_labels %s", key_list);
	web_respond(c, out);
}

// hamlib's full rig catalog for the generic-rig backend's rig-picker
// (web UI datalist) -- see rig_generic_list()
void get_riglist(struct mg_connection *c){
	static char riglist[16384];
	char out[16400];
	rig_generic_list(riglist, sizeof(riglist));
	snprintf(out, sizeof(out), "RIGLIST %s", riglist);
	mg_ws_send(c, out, strlen(out), WEBSOCKET_OP_TEXT);
}

// currently-present serial devices, for the Rig Device picker
void get_seriallist(struct mg_connection *c){
	static char list[4096];
	char out[4200];
	rig_generic_list_serial_devices(list, sizeof(list));
	snprintf(out, sizeof(out), "SERIALLIST %s", list);
	mg_ws_send(c, out, strlen(out), WEBSOCKET_OP_TEXT);
}

// currently-present ALSA cards, for the Capture/Playback Device pickers
void get_audiolist(struct mg_connection *c){
	static char list[4096];
	char out[4200];
	rig_generic_list_audio_devices(list, sizeof(list));
	snprintf(out, sizeof(out), "AUDIOLIST %s", list);
	mg_ws_send(c, out, strlen(out), WEBSOCKET_OP_TEXT);
}

// band/mode dial frequency table, for the Settings > Frequencies editable
// table -- see band_freq_list() in logbook.c
void get_bandfreqlist(struct mg_connection *c){
	static char list[2048];
	char out[2100];
	band_freq_list(list, sizeof(list));
	snprintf(out, sizeof(out), "BANDFREQLIST %s", list);
	mg_ws_send(c, out, strlen(out), WEBSOCKET_OP_TEXT);
}

// On-demand callsign->grid lookup against the persistent directory
// (logbook.c) -- the client only ever asks about a specific callsign
// it's already seeing decoded and can't resolve itself (see
// gridmap_maybe_request_grid(), web/index.html), never a bulk sync --
// the directory can hold hundreds of thousands of ULS-seeded rows, far
// too much to push wholesale. Always responds, even on a miss (no
// trailing grid token) -- that's the client's signal the server has
// authoritatively answered "unknown," so it can stop asking rather than
// silently getting nothing back and re-requesting forever.
static void get_grid(struct mg_connection *c, char *args){
	char grid[8], out[64];
	char *callsign = strtok(args, " \t\n");
	if (!callsign)
		return;
	if (callsign_grid_get(callsign, grid, sizeof(grid)))
		snprintf(out, sizeof(out), "GRID %s %s", callsign, grid);
	else
		snprintf(out, sizeof(out), "GRID %s", callsign);
	web_respond(c, out);
}

char request[200];
int request_index = 0;

static void web_despatcher(struct mg_connection *c, struct mg_ws_message *wm){
	// every message is "<cookie>\n<field> <value>" -- a 40-char cookie
	// plus a field like RIGDEVICE (max 100 chars, to fit a
	// /dev/serial/by-id/... path) already exceeds the old 99-byte cap
	// here, so long device paths were silently dropped with no
	// response and no log line at all. Tie the check to the actual
	// buffer size instead of a stale magic number.
	if (wm->data.len > sizeof(request) - 1)
		return;

	strncpy(request, wm->data.ptr, wm->data.len);	
	request[wm->data.len] = 0;
	//handle the 'no-cookie' situation
	char *cookie = NULL;
	char *field = NULL;
	char *value = NULL;

	cookie = strtok(request, "\n");
	field = strtok(NULL, "=");
	value = strtok(NULL, "\n");

	if (field == NULL || cookie == NULL){
		printf("Invalid request on websocket\n");
		web_respond(c, "quit Invalid request on websocket");
		c->is_draining = 1;
	}
	// space-separated commands (the common case -- no '=' in the
	// message) have no delimiter for strtok(NULL, "=") to stop at, so
	// `field` ends up holding "fieldname value" together, not just the
	// name -- e.g. RIGDEVICE's own value alone is allowed up to 100
	// chars (to fit a /dev/serial/by-id/... path), which combined with
	// the field name can never fit a 100-char cap. Bounded by the
	// request buffer itself (with room for the cookie+newline already
	// consumed ahead of it), not a separate arbitrary number.
	else if (strlen(field) > sizeof(request) - 20 || strlen(field) <  2 || strlen(cookie) > 40 || strlen(cookie) < 4){
		printf("Ill formed request on websocket\n");
		web_respond(c, "quit Illformed request");
		c->is_draining = 1;
	}
	else if (!strcmp(field, "login")){
		printf("trying login with passkey : [%s]\n", value);
		do_login(c, value);
	}
	else if (cookie == NULL || strcmp(cookie, session_cookie)){
		web_respond(c, "quit expired");
		printf("Cookie not found, closing socket %s vs %s\n", cookie, session_cookie);
		c->is_draining = 1;
	}
	else if (!strcmp(field, "spectrum"))
		get_spectrum(c);
	else if (!strcmp(field, "audio"))
		get_audio(c);
	else if (!strcmp(field, "logbook"))
		get_logs(c, value);
	else if (!strcmp(field, "macros_list"))
		get_macros_list(c);
	else if (!strcmp(field, "riglist"))
		get_riglist(c);
	else if (!strcmp(field, "seriallist"))
		get_seriallist(c);
	else if (!strcmp(field, "audiolist"))
		get_audiolist(c);
	else if (!strcmp(field, "bandfreqlist"))
		get_bandfreqlist(c);
	else if (!strcmp(field, "gridlookup"))
		get_grid(c, value);
	else if (!strcmp(field, "refresh"))
		get_updates(c, 1);
	else{
		char buff[1200];
		if (value)
			sprintf(buff, "%s %s", field, value);
		else
			strcpy(buff, field);
		printf("remote[%s]\n", buff); 
		remote_execute(buff);
		get_updates(c, 0);
	}
}

// This RESTful server implements the following endpoints:
//   /websocket - upgrade to Websocket, and implement websocket echo server
//   /rest - respond with JSON string {"result": 123}
//   any other URI serves static files from s_web_root
static void fn(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
  if (ev == MG_EV_OPEN) {
    // c->is_hexdumping = 1;
	} else if (ev == MG_EV_ERROR || ev == MG_EV_CLOSE){
		// Real safety fix -- see authenticated_conn's own comment. Only
		// the currently-authenticated connection dropping triggers this;
		// an unrelated/never-logged-in connection erroring out or closing
		// is routine (a stray HTTP request, a browser tab that never
		// bothered to log in) and must not disturb a real, separate
		// active session. No grace period, no "give it a second to
		// reconnect" -- the whole point is that automatic transmission
		// must not continue even briefly with no operator connected, so
		// this fires the instant the connection is gone, not after some
		// timeout.
		if (c == authenticated_conn){
			printf("Operator's connection closed/errored -- stopping any automatic transmission (Auto CQ, queued replies) for safety.\n");
			authenticated_conn = NULL;
			abort_tx();
		}
  } else if (ev == MG_EV_HTTP_MSG) {
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;
    if (mg_http_match_uri(hm, "/websocket")) {
      // Upgrade to websocket. From now on, a connection is a full-duplex
      // Websocket connection, which will receive MG_EV_WS_MSG events.
      mg_ws_upgrade(c, hm, NULL);
    } else if (mg_http_match_uri(hm, "/rest")) {
      // Serve REST response
      mg_http_reply(c, 200, "", "{\"result\": %d}\n", 123);
    } else {
      // Serve static files. No-cache headers: without these, browsers
      // (mobile ones especially) can hang onto an old index.html/
      // style.css across page reloads with no obvious way for the user
      // to tell they're not looking at what's actually deployed -- this
      // caused real, repeated confusion this session (fixes that were
      // genuinely live on the server looked like they weren't there).
      // A single-page field-radio-control app has no benefit from
      // browser caching that outweighs always serving what's actually
      // installed.
      //
      // Clear-Site-Data alongside Cache-Control, not instead of it --
      // user's own reasoning: different browsers/intermediate layers
      // handle caching differently, and Cache-Control alone hasn't
      // fully solved this in practice (still needed manual hard
      // refreshes more than once this session despite it already being
      // here). Clear-Site-Data is a real, purpose-built header that
      // actively instructs the browser to wipe cached data for this
      // origin, not just decline to reuse it going forward -- well
      // supported in Chrome/Edge/Samsung Internet (this project's real
      // targets), weaker support in Safari. Belt-and-suspenders with
      // the ?v= cache-busting on script/style URLs in index.html (a
      // third, independent mechanism that doesn't rely on the browser
      // honoring any caching directive at all).
      //
      // logbook_export.adi specifically: real report -- clicking Export
      // ADIF opened the file inline in the browser instead of
      // downloading it. .adi isn't a MIME type mongoose knows, so it
      // fell back to a generic text type, and every browser displays
      // plain text inline rather than downloading it absent an explicit
      // instruction otherwise. Content-Disposition: attachment forces a
      // real download regardless of what MIME type gets guessed.
      struct mg_http_serve_opts opts = {
          .root_dir = s_web_root,
          .extra_headers = mg_http_match_uri(hm, "/logbook_export.adi")
              ? "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                "Clear-Site-Data: \"cache\"\r\n"
                "Content-Disposition: attachment; filename=\"logbook_export.adi\"\r\n"
              : "Cache-Control: no-cache, no-store, must-revalidate\r\n"
                "Clear-Site-Data: \"cache\"\r\n"
      };
      mg_http_serve_dir(c, ev_data, &opts);
    }
  } else if (ev == MG_EV_WS_MSG) {
    // Got websocket frame. Received data is wm->data
    struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
    web_despatcher(c, wm);
  }
  (void) fn_data;
}

void *webserver_thread_function(void *server){
  mg_mgr_init(&mgr);  // Initialise event manager
  mg_http_listen(&mgr, s_listen_on, fn, NULL);  // Create HTTP listener
  for (;;) mg_mgr_poll(&mgr, 1000);             // Infinite event loop
	printf("exiting webserver thread\n");
}

void webserver_stop(){
  mg_mgr_free(&mgr);
}

static pthread_t webserver_thread;

void webserver_start(){
	//logbook_open();
 	pthread_create( &webserver_thread, NULL, webserver_thread_function, 
		(void*)NULL);
}
