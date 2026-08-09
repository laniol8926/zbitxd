// Plain mono ALSA capture/playback for the generic-rig backend (see
// rig_generic.h for the CAT half). Bypasses the SDR's I/Q pipeline
// (sbitx_sound.c / sound_process()) entirely -- a real transceiver already
// does its own downconversion/filtering, so this only needs to move audio
// samples in and out at generic_capture_device/generic_playback_device.

void sound_generic_start(void);
void sound_generic_stop(void);
