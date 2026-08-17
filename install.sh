#!/bin/sh -e
#
# Installer for the laniol8926/zbitxd fork (generic-rig-backend branch).
#
# Covers everything in README.md's manual install section: base OS
# dependencies, the ALSA aloop virtual soundcards, boot config for the
# zBitx hardware, disabling fake-hwclock, a from-source
# Hamlib build (needed for rigctld to support rigs newer than whatever
# your distro's packaged libhamlib knows about -- see rig_generic.c's
# own comment on this), and finally zbitxd itself.
#
# Safe to re-run: every step here either checks before it acts, or is
# naturally idempotent (git pull, make, apt install of an
# already-installed package).
#
# Run as the normal user (e.g. "pi"), NOT as root -- this calls sudo
# itself wherever root is actually needed.

REPO_URL="https://github.com/laniol8926/zbitxd.git"
BRANCH="generic-rig-backend"
ZBITXD_DIR="$HOME/zbitxd"
HAMLIB_DIR="$HOME/hamlib"

if [ "$(id -u)" = "0" ]; then
	echo "Run this as your normal user, not root (it calls sudo itself where needed)." >&2
	exit 1
fi

echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
echo "*** base packages"
echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
sudo apt update
sudo apt install -y \
	git libasound2-dev libfftw3-dev libsqlite3-dev libsystemd-dev sqlite3 \
	build-essential autoconf automake libtool libusb-1.0-0-dev libltdl-dev

echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
echo "*** ALSA aloop virtual soundcards"
echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
if ! grep -qxF "snd-aloop" /etc/modules 2>/dev/null; then
	echo "snd-aloop" | sudo tee -a /etc/modules
fi
if [ ! -f /etc/modprobe.d/snd-aloop.conf ]; then
	echo "options snd-aloop enable=1,1,1 index=1,2,3" | sudo tee /etc/modprobe.d/snd-aloop.conf
fi

echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
echo "*** boot config (zBitx GPIO/audio overlay)"
echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
BOOT_CFG=/boot/firmware/config.txt
add_boot_line() {
	grep -qxF "$1" "$BOOT_CFG" 2>/dev/null || echo "$1" | sudo tee -a "$BOOT_CFG" >/dev/null
}
add_boot_line "# zBitx related options"
add_boot_line "gpio=4,5,9,10,11,17,22,27=ip,pu"
add_boot_line "gpio=24,23=op,pu"
add_boot_line "avoid_warnings=1"
add_boot_line "dtoverlay=audioinjector-wm8731-audio"
add_boot_line "dtoverlay=i2c-rtc-gpio,ds1307,bus=2,i2c_gpio_sda=13,i2c_gpio_scl=6"
sudo sed -i "s/^dtparam=audio=on/##dtparam=audio=on/" "$BOOT_CFG"
sudo sed -i "s/^dtoverlay=vc4-kms-v3d$/dtoverlay=vc4-kms-v3d,noaudio/" "$BOOT_CFG"

echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
echo "*** system time (FT8 needs it accurate -- disable fake-hwclock)"
echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
sudo systemctl disable fake-hwclock || true

echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
echo "*** Hamlib (from source -- see rig_generic.c for why the packaged"
echo "*** version isn't enough for the generic-rig backend)"
echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
if [ -x /usr/local/bin/rigctld ]; then
	echo "/usr/local/bin/rigctld already present, skipping Hamlib build."
	echo "(delete it and re-run this script if you need a newer Hamlib.)"
else
	if [ ! -d "$HAMLIB_DIR" ]; then
		git clone https://github.com/Hamlib/Hamlib.git "$HAMLIB_DIR"
	fi
	(
		cd "$HAMLIB_DIR"
		git pull
		./bootstrap
		./configure
		make -j"$(nproc)"
		sudo make install
		sudo ldconfig
	)
fi

echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
echo "*** zbitxd itself ($BRANCH)"
echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
if [ ! -d "$ZBITXD_DIR" ]; then
	git clone --recurse-submodules "$REPO_URL" "$ZBITXD_DIR"
fi
(
	cd "$ZBITXD_DIR"
	git checkout "$BRANCH"
	git pull
	git submodule update --init --recursive
	make
	sudo make install
)

sudo systemctl daemon-reload
sudo systemctl enable zbitxd
sudo systemctl restart zbitxd

echo "<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<"
echo "zbitxd ($BRANCH) has been installed and started."
echo "If this box has an existing hw_settings.ini/sbitx.db/user_settings.ini"
echo "to carry over, copy them into /var/lib/zbitxd now and run:"
echo "  sudo chown zbitxd:zbitxd /var/lib/zbitxd/*"
echo "  sudo systemctl restart zbitxd"
echo ""
echo "A reboot is recommended for the boot config / WiFi driver changes"
echo "above to take effect, if this is a first-time install."
