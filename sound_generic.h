// Plain mono ALSA capture/playback for the generic-rig backend (see
// rig_generic.h for the CAT half). Bypasses the SDR's I/Q pipeline
// (sbitx_sound.c / sound_process()) entirely -- a real transceiver already
// does its own downconversion/filtering, so this only needs to move audio
// samples in and out at generic_capture_device/generic_playback_device.

void sound_generic_start(void);
void sound_generic_stop(void);

// Stops and restarts the capture/playback threads, picking up whatever
// generic_capture_device/generic_playback_device currently are -- lets
// the web UI's Capture/Playback Device fields take effect live (CAT and
// audio are genuinely separate for some rigs, e.g. an FT-857D's external
// USB sound card vs. its own CAT adapter, so this is deliberately not
// tied to rig_generic_connect()).
void sound_generic_restart(void);

// True once the respective ALSA device has been opened successfully and
// hasn't since hit an unrecoverable error (see capture_thread_fn()/
// playback_thread_fn()'s retry loop in sound_generic.c) -- used by the web
// UI's connect_panel to show "already connected" instead of leaving the
// user to guess whether they need to click Connect.
int sound_generic_capture_connected(void);
int sound_generic_playback_connected(void);

// Software RX gain stage, applied to captured samples before they reach
// modem_rx()/the waterfall -- most rigs on this backend have no CAT path
// to a hardware RF/AF gain at all (confirmed for the RS-978/mcHF by
// reading UHSDR's actual firmware source: its FT-817 CAT emulation has
// no gain opcode and no EEPROM-mapped gain parameter, so gain there is
// physical-knob-only). This mirrors what WSJT-X itself actually does in
// this situation -- its own "Rx" slider is a software gain on the
// captured audio, not a CAT command to the radio. gain is a plain
// multiplier (1.0 = unchanged, 0.0 = muted); callers scale their own
// UI range down before calling this.
void sound_generic_set_rx_gain(float gain);
