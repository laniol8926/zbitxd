// rigctld client used by the generic-rig backend (radio=generic in
// hw_settings.ini) in place of the zBitx SDR's si5351/i2c tuning path.
// See sdr.h for generic_rigctld_host/generic_rigctld_port.

void rig_generic_init(void);
void rig_generic_set_freq(long freq_hz);
void rig_generic_set_ptt(int on);

// Spawns (or respawns, if already running) rigctld for the given hamlib
// model number and serial device, e.g. rig_generic_connect("2057",
// "/dev/ttyUSB0") for a QMX, then (re)connects the client. Triggered from
// the web UI's settings panel "Connect to Rig" button (RIGCONNECT field).
void rig_generic_connect(const char *model, const char *device);
