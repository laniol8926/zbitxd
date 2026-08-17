#!/bin/sh -e
#
# Installer for the laniol8926/zbitxd fork (generic-rig-backend branch).
#
# Runs on any Debian/Ubuntu-based Linux system -- nothing here is tied
# to Raspberry Pi hardware. The generic-rig backend drives a
# real transceiver over CAT (USB serial) + its own USB audio interface,
# so there's no SBC-specific GPIO/boot-config setup needed at all --
# just standard USB devices Linux already handles natively.
#
# Covers everything in README.md's manual install section: base OS
# dependencies, disabling fake-hwclock (only if present -- matters on
# any headless box with no battery-backed RTC, not Pi-specific), a
# from-source Hamlib build (needed for rigctld to support rigs newer
# than whatever your distro's packaged libhamlib knows about -- see
# rig_generic.c's own comment on this), and finally zbitxd itself.
#
# Safe to re-run: every step here either checks before it acts, or is
# naturally idempotent (git pull, make, apt install of an
# already-installed package).
#
# Run as your normal user, NOT as root -- this calls sudo itself
# wherever root is actually needed.

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
echo "*** system time (FT8 needs it accurate)"
echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
# fake-hwclock only exists on systems with no battery-backed RTC (common
# on SBCs, not exclusive to any one board) -- harmless no-op elsewhere.
if systemctl list-unit-files fake-hwclock.service >/dev/null 2>&1; then
	sudo systemctl disable fake-hwclock
fi

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
