/*
The old zBitx SDR's own ALSA I/Q capture/playback thread (formerly
sbitx_sound.c) is gone -- sound_generic.c is the real audio path for
this backend now. These four survived because they still had live
callers outside that dead thread (see their definitions in sbitx.c
for detail); sound_mixer() specifically is a safe no-op in generic
mode since the zBitx's own hardcoded mixer control names don't exist
on a generic rig's plain USB audio device.
*/
void sound_volume(char *card_name, char *element, int volume);
void sound_mixer(char *card_name, char *element, int make_on);
void sound_input(int loop);
unsigned long sbitx_millis();
