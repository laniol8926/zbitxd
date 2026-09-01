void webserver_start();
void webserver_poll();
void webserver_stop();
void  web_update(char *message);
// Pings the (single) connected web client that a QSO was just logged --
// see this function's own comment in webserver.c for the real bug this
// fixes. No-op if nobody is currently connected/logged in.
void notify_qso_logged(void);
