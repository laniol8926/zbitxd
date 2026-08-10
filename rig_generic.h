// rigctld client used by the generic-rig backend (radio=generic in
// hw_settings.ini) in place of the zBitx SDR's si5351/i2c tuning path.
// See sdr.h for generic_rigctld_host/generic_rigctld_port.

#include <stddef.h>

void rig_generic_init(void);
void rig_generic_set_freq(long freq_hz);
void rig_generic_set_ptt(int on);

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
