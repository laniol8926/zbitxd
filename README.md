# zbitxd (generic-rig-backend fork)

This is a fork of [dg0jde/zbitxd](https://github.com/dg0jde/zbitxd), a headless daemon for controlling the zBitx transceiver — same idea (browser/touchscreen control, no GTK desktop needed, systemd-managed), but extended to also drive **any rig supported by [Hamlib](https://hamlib.github.io/)** through `rigctld`, not just zBitx hardware. Development happens on the `generic-rig-backend` branch.

If you just want the original zBitx-hardware-only daemon, use the upstream repo instead. This fork is for running zbitxd's web UI, FT8 engine, and logging against a separate rig (tested against a QRP Labs QMX and an M0NKA mcHF) over CAT + a USB audio interface.


## What this fork adds over upstream

- **Generic rig backend** (`radio=generic`): CAT control and audio I/O through `rigctld`/ALSA instead of the zBitx's own hardware, with a searchable rig-model picker, serial/audio device pickers, and a dedicated **Connect** panel for wiring it all up after login.
- **Successive interference cancellation (SIC) and AP decoding** added to the FT8/FT4 decode pipeline.
- **Auto CQ / Auto Answer**, with a proper on-the-fly repeat-counter reset and safety guards against changing frequency mid-transmit.
- **QSO logging extras**: TX Power, Antenna, and Comments fields folded into every logged QSO; correctly-formatted **ADIF export** (button in the FT8 panel); **live QSO broadcast to a UDP-listening logger** (e.g. CQRLog) using WSJT-X's own UDP protocol, so QSOs appear in your regular logger the moment they're logged — no manual export/import step.
- Extensive mobile/responsive UI cleanup (draggable/resizable panels, band-activity auto-hide, waterfall/CQ-panel fixes, RX Frequency panel fixes), a WSJT-X-style per-band/per-mode frequency table, and a long tail of decoder/keying/timing correctness fixes.

See `git log generic-rig-backend` for the full history.


## Quick install

On a fresh Raspberry Pi OS Lite (64-bit) install, with the SD card already prepared and booted (see **Preparing the SD card** below if you haven't done that part yet):

```
curl -fsSL https://raw.githubusercontent.com/laniol8926/zbitxd/generic-rig-backend/install.sh | sh
```

or, if you'd rather review it first (recommended):

```
git clone --recurse-submodules -b generic-rig-backend https://github.com/laniol8926/zbitxd.git ~/zbitxd
less ~/zbitxd/install.sh
~/zbitxd/install.sh
```

`install.sh` is idempotent — re-run it any time to pick up dependency or code updates. It installs base packages, the ALSA aloop virtual soundcards, the zBitx boot-config overlay, a from-source Hamlib build (needed for `rigctld` — see below), then builds and installs zbitxd itself and starts the service.

The rest of this document covers what the script does in more detail, plus everything specific to this fork (generic rig setup, new Settings fields, logging).


## Preparing the SD card

- Install Raspberry Pi Imager on your PC.
- Insert the new SD card into the connected card reader.
- Start Raspberry Pi Imager and select:
  - Operating System: Raspberry Pi OS (other) > Raspberry Pi OS Lite (64-bit)
  - Storage: the new SD card
  - Advanced Options (gear icon):
    - Set hostname (e.g. `zbitx`)
    - Enable SSH, with password or public-key auth
    - Set username and password
    - Configure wireless LAN
    - Set locale/timezone/keyboard layout
    - Save

### WiFi problems

Some WiFi networks fail to connect due to a firmware incompatibility on the Raspberry Pi Zero 2 W's WiFi chip ([raspberrypi/bookworm-feedback#279](https://github.com/raspberrypi/bookworm-feedback/issues/279)). Fix: create `/etc/modprobe.d/brcmfmac.conf` containing:
```
options brcmfmac feature_disable=0x2000
```
If you have no working WiFi yet to SSH in and do this, either mount the SD card's ext4 partition on a Linux PC and create the file there under `<mountpoint>/etc/modprobe.d/`, or connect a monitor/keyboard directly to the zBitx, log in at the console, and run:
```
sudo mkdir /etc/modprobe.d
echo "options brcmfmac feature_disable=0x2000" | sudo tee /etc/modprobe.d/brcmfmac.conf
sudo reboot
```


## Manual install

If you'd rather not run `install.sh`, or want to understand each step, here's what it does:

```
sudo apt update && sudo apt upgrade && sudo reboot
```

```
sudo apt install git libasound2-dev libfftw3-dev libsqlite3-dev libsystemd-dev sqlite3 \
    build-essential autoconf automake libtool libusb-1.0-0-dev libltdl-dev
```

ALSA aloop, used to pass audio between zbitxd and other programs:
```
echo "snd-aloop" | sudo tee -a /etc/modules
echo "options snd-aloop enable=1,1,1 index=1,2,3" | sudo tee /etc/modprobe.d/snd-aloop.conf
```

zBitx boot config:
```
echo "# zBitx related options" | sudo tee -a /boot/firmware/config.txt
echo "gpio=4,5,9,10,11,17,22,27=ip,pu" | sudo tee -a /boot/firmware/config.txt
echo "gpio=24,23=op,pu" | sudo tee -a /boot/firmware/config.txt
echo "avoid_warnings=1" | sudo tee -a /boot/firmware/config.txt
echo "dtoverlay=audioinjector-wm8731-audio" | sudo tee -a /boot/firmware/config.txt
echo "dtoverlay=i2c-rtc-gpio,ds1307,bus=2,i2c_gpio_sda=13,i2c_gpio_scl=6" | sudo tee -a /boot/firmware/config.txt
sudo sed -i "s/dtparam=audio=on/##dtparam=audio=on/" /boot/firmware/config.txt
sudo sed -i "s/dtoverlay=vc4-kms-v3d/dtoverlay=vc4-kms-v3d,noaudio/" /boot/firmware/config.txt
```
(The GPIO lines only matter if you're running actual zBitx hardware — harmless to leave in for the generic-rig backend too.)

zbitxd doesn't use its own time-sync routines; it relies on the Pi's system clock, which needs to be accurate for FT8. Disable the fake hardware clock so the real RTC (if fitted) or NTP take over:
```
sudo systemctl disable fake-hwclock
```

**Hamlib, from source** — this fork's generic-rig backend needs this even if your distro already has `libhamlib` packaged. A system-packaged `libhamlib.so.4` and a from-source build can register under the identical SONAME; the dynamic linker silently picks whichever it indexed first, which can leave `rigctld` reporting "Unknown rig num" for a rig your distro's older Hamlib doesn't know about yet. `rig_generic.c` looks for a build at `/usr/local/bin/rigctld` specifically and forces its own lib dir, sidestepping this:
```
cd
git clone https://github.com/Hamlib/Hamlib.git
cd Hamlib
./bootstrap
./configure
make
sudo make install
sudo ldconfig
```

Finally, zbitxd itself:
```
cd
git clone --recurse-submodules -b generic-rig-backend https://github.com/laniol8926/zbitxd.git
cd zbitxd
make
sudo make install
```

If you have config/log data from a previous install to carry over (`hw_settings.ini`, `sbitx.db`, `user_settings.ini`), copy them into `/var/lib/zbitxd` now and fix ownership:
```
sudo chown zbitxd:zbitxd /var/lib/zbitxd/*
```

Start it:
```
sudo systemctl daemon-reload
sudo systemctl start zbitxd
sudo systemctl enable zbitxd   # start automatically on boot
```


## Updating

```
cd ~/zbitxd
git pull
git submodule update --init --recursive
make
sudo make install
sudo systemctl restart zbitxd
```
or just re-run `install.sh`.


## Using the generic rig backend

1. Open the web UI (`http://<pi-address>:8080`) and log in.
2. The **Connect** panel opens automatically after every login (so a renumbered serial port or changed audio device is always easy to fix, not buried in a menu). Pick your rig from the searchable model list, the serial device it's on, and its baud rate; pick the audio capture/playback devices for its USB audio interface.
3. Click **Connect to Rig** (starts `rigctld`, from the `/usr/local/bin` build above, against your rig) and **Connect Audio** (wires up the audio path). The panel shows live connection status for each.
4. Once connected, the panel closes and the normal web UI (waterfall, FT8 panels, logbook) behaves the same as on real zBitx hardware.

Settings relevant to this: `RIGMODEL` / `RIGDEVICE` / `RIGBAUD` (under the hood; normally set through the Connect panel's pickers rather than typed by hand), `CAPTUREDEV` / `PLAYBACKDEV`.


## Settings this fork adds

In the Settings panel, alongside the existing My Call/My Grid fields:

| Field | Purpose |
|---|---|
| TX Power | Logged with every QSO (own `power` column) |
| Antenna | Folded into the logged Comments, alongside rig info |
| Comments | Free-text, folded into the logged Comments |
| UDP Log Host | Live-broadcast target (see below). Blank = disabled. |
| UDP Log Port | Live-broadcast port. Defaults to `2237` (WSJT-X/CQRLog's own default). |


## Logging

Every logged QSO records TX Power and a Comments field (rig description if running the generic backend, plus your own Antenna/Comments text) alongside the usual call/grid/report/frequency/mode.

**ADIF export**: the "Export ADIF" button in the FT8 panel's toolbar exports the whole logbook to a correctly-formatted ADIF 3.1.4 file and downloads it — useful for importing into QRZ Logbook or any logger that only takes file import.

**Live logging to CQRLog (or any WSJT-X-UDP-compatible logger)**: set **UDP Log Host** to the IP of the machine your logger runs on (this is normally a *different* machine than the Pi, so unlike a same-machine WSJT-X setup this can't default to `127.0.0.1`) and **UDP Log Port** to match your logger's listener (CQRLog's default is `2237`). Every QSO is then broadcast live over WSJT-X's own UDP "QSO Logged" protocol the moment it's logged — no export/import needed for that logger.

If using CQRLog specifically: make sure **Preferences → WSJT-X → Mode from** is set to **wsjtx** (the default on a fresh install). If it's set to "CQRLOG" or "default" instead, CQRLog's own parser skips reading the Mode field off the wire entirely, which misaligns every field after it (including both date/time fields) and can throw a spurious "date error" popup — a CQRLog-side setting, not something zbitxd can work around from the sending end.


## Additional extensions

### Automated WiFi Access Point

From [raspberryconnect.com](https://www.raspberryconnect.com/projects/65-raspberrypi-hotspot-accesspoints/203-automated-switching-accesspoint-wifi-network):
```
cd /tmp
curl "https://www.raspberryconnect.com/images/scripts/AccessPopup.tar.gz" -o AccessPopup.tar.gz
tar -xvf ./AccessPopup.tar.gz
cd AccessPopup
sudo ./installconfig.sh
```
- Installation: press 1
- Configuration SSID and pre-shared key: press 2 (e.g. `zBitxAP`/`1234567890`)
- End: enter 10
