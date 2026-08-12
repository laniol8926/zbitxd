// rigctld client used by the generic-rig backend (radio=generic in
// hw_settings.ini) in place of the zBitx SDR's si5351/i2c tuning path.
// See sdr.h for generic_rigctld_host/generic_rigctld_port.

#include <stddef.h>

void rig_generic_init(void);
void rig_generic_set_freq(long freq_hz);

// Reads the rig's actual current VFO frequency via CAT. Returns -1 on
// failure (not connected, communication error, unparseable reply).
long rig_generic_get_freq(void);

void rig_generic_set_ptt(int on);

// hamlib rigctl "L" (set_level). value is normalized 0.0-1.0 per
// hamlib's convention for float-typed levels like "RF"/"AF"/"RFPOWER"
// -- callers scale their own UI range down before calling this.
void rig_generic_set_level(const char *level_name, float value);

// Sends a CAT mode change for the app's own mode name ("USB", "FT8", or
// "FT4" -- see sbitx.c's sdr_request() "r1:mode" case, the only caller).
// Not a single fixed string: which rigctld mode string actually means
// "digital" depends on which specific rig is connected (tracked from the
// model id passed to rig_generic_connect()) -- e.g. a QMX has its own
// dedicated PKTUSB digital mode, distinct from voice USB, while other
// rigs (RS-978, QDX) have no such distinction and just use USB for both.
// See project notes for the full per-rig reasoning.
void rig_generic_set_mode(const char *app_mode);

// Spawns (or respawns, if already running) rigctld for the given hamlib
// model number, serial device, and optional baud rate (pass "" or NULL
// to let rigctld pick its own default for that model), e.g.
// rig_generic_connect("2057", "/dev/ttyUSB0", "9600") for a QMX, then
// (re)connects the client. Triggered from the web UI's settings panel
// "Connect to Rig" button (RIGCONNECT field).
void rig_generic_connect(const char *model, const char *device, const char *baud);

// Fetches hamlib's full rig catalog (via `rigctl --list`) formatted as
// one "<id> <Manufacturer> <Model>" entry per line, for the web UI's
// rig-picker datalist. Truncates to fit out_size; always NUL-terminated.
void rig_generic_list(char *out, size_t out_size);

// Lists currently-present serial devices for the Rig Device picker --
// prefers /dev/serial/by-id/* (stable across reboots/replugging),
// falling back to raw /dev/ttyACM*/ttyUSB* if none exist.
void rig_generic_list_serial_devices(char *out, size_t out_size);

// Lists currently-present ALSA cards, formatted as ready-to-use
// "plughw:<card>,0" strings, for the Capture/Playback Device pickers.
void rig_generic_list_audio_devices(char *out, size_t out_size);

// True once rigctld has accepted a connection and no send/recv has failed
// since -- used by the web UI's connect_panel to show "already connected"
// instead of leaving the user to guess whether they need to click Connect.
int rig_generic_is_connected(void);
