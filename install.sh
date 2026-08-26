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

# The `-e` on the shebang above only takes effect when this file is
# executed directly (./install.sh) -- README's own documented one-liner
# (`curl ... | sh`) and a plain `sh install.sh` both invoke `sh` on the
# script's *content*, which never looks at the shebang line at all, so
# -e was silently not in effect for the exact flow most people actually
# use. Confirmed live: a fresh box with no GitHub SSH key hit a real
# submodule-clone failure, the build failed after it, `make install`
# failed after that, and this script still ran to the end and printed
# the "installed and started" banner below -- every failure silently
# swallowed. set -e here makes it explicit regardless of how sh got
# invoked.
set -e

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
# libncurses-dev: sbitx_daemon.c #includes <ncurses.h> (a vestigial header-
# only dependency, confirmed live -- no ncurses.h symbols are actually
# called and the Makefile never links -lncurses, so this is purely a
# compile-time need) -- missing on a genuinely fresh box, confirmed live
# via a real "ncurses.h: No such file or directory" build failure.
sudo apt install -y \
	git libasound2-dev libfftw3-dev libsqlite3-dev libsystemd-dev sqlite3 \
	build-essential autoconf automake libtool libusb-1.0-0-dev libltdl-dev \
	libncurses-dev python3 curl

echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
echo "*** system time (FT8 needs it accurate)"
echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
# fake-hwclock only exists on systems with no battery-backed RTC (common
# on SBCs, not exclusive to any one board) -- harmless no-op elsewhere.
if systemctl list-unit-files fake-hwclock.service >/dev/null 2>&1; then
	sudo systemctl disable fake-hwclock
fi

echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
echo "*** swap (the from-source Hamlib build below is memory-heavy --"
echo "*** make -j\$(nproc) further down means that many parallel GCC"
echo "*** jobs at once -- a fresh low-RAM board's small default swap"
echo "*** (confirmed live: a Pi Zero 2 W, 512MB total) can OOM or thrash"
echo "*** badly here. Learned the hard way -- user's own words.)"
echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
MIN_SWAP_MB=1024
current_swap_kb=$(awk '/SwapTotal/{print $2}' /proc/meminfo)
current_swap_mb=$((current_swap_kb / 1024))
if [ "$current_swap_mb" -ge "$MIN_SWAP_MB" ]; then
	echo "Current swap: ${current_swap_mb}MB, already at/above the ${MIN_SWAP_MB}MB minimum."
elif [ -f /etc/dphys-swapfile ]; then
	echo "Current swap: ${current_swap_mb}MB, below the ${MIN_SWAP_MB}MB minimum -- resizing via dphys-swapfile."
	sudo dphys-swapfile swapoff
	# Handles CONF_SWAPSIZE already present (commented or not) or missing
	# entirely -- sed only touches an existing uncommented line; the grep
	# fallback covers every other case by just appending an active one
	# (a later CONF_SWAPSIZE= line takes effect over an earlier commented
	# one regardless).
	sudo sed -i "s/^CONF_SWAPSIZE=.*/CONF_SWAPSIZE=${MIN_SWAP_MB}/" /etc/dphys-swapfile
	if ! grep -q "^CONF_SWAPSIZE=${MIN_SWAP_MB}$" /etc/dphys-swapfile; then
		echo "CONF_SWAPSIZE=${MIN_SWAP_MB}" | sudo tee -a /etc/dphys-swapfile >/dev/null
	fi
	sudo dphys-swapfile setup
	sudo dphys-swapfile swapon
else
	echo "Current swap: ${current_swap_mb}MB, below the ${MIN_SWAP_MB}MB minimum, and"
	echo "dphys-swapfile isn't present on this system (not a Raspberry Pi OS"
	echo "image, or swap is managed some other way) -- skipping the automatic"
	echo "resize. The Hamlib build below may be memory-tight on a low-RAM"
	echo "board without it; increase swap manually first if you hit OOM."
fi

echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
echo "*** pkg-config (Hamlib's own configure.ac requires pkg.m4 >= 0.29.2"
echo "*** via PKG_PREREQ -- an old distro's packaged pkg-config, e.g."
echo "*** Debian/Raspbian Buster's 0.29, fails this outright and the"
echo "*** Hamlib build below never even gets to ./configure)"
echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
pkgconfig_version="$(pkg-config --version 2>/dev/null || echo 0)"
if dpkg --compare-versions "$pkgconfig_version" lt 0.29.2; then
	echo "pkg-config ${pkgconfig_version} is too old (Hamlib needs >= 0.29.2) -- switching to pkgconf."
	# pkgconf implements the pkg-config CLI/API and Debian's package
	# diverts /usr/bin/pkg-config, /usr/share/aclocal/pkg.m4, etc. to
	# point at it automatically (and pulls pkg-config itself out via a
	# package conflict) -- confirmed live, no manual symlinking needed.
	sudo apt install -y pkgconf
	# But even pkgconf's own pkg.m4 self-declares PKG_MACROS_VERSION
	# 0.29.1 -- one patch version short of what Hamlib's PKG_PREREQ
	# actually checks for -- confirmed live against pkgconf's current
	# upstream master, not just a stale Buster package. pkgconf's real
	# capability is a strict superset of pkg-config 0.29.2's, so
	# bumping this one version string it self-reports is safe.
	if [ -f /usr/share/aclocal/pkg.m4 ]; then
		sudo sed -i "s/\[PKG_MACROS_VERSION\], \[0.29.1\]/[PKG_MACROS_VERSION], [0.29.2]/" /usr/share/aclocal/pkg.m4
	fi
else
	echo "pkg-config ${pkgconfig_version}, already at/above the 0.29.2 minimum."
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
	# --branch here matters, not just cosmetic: without it this clones
	# the repo's *default* branch (main) first and resolves submodules
	# against *that* commit, before the checkout below ever runs -- and
	# main can lag generic-rig-backend (confirmed live: main was still
	# on an older commit with a since-fixed SSH submodule URL, so a
	# fresh clone failed here even after that fix had already landed on
	# generic-rig-backend).
	git clone --recurse-submodules --branch "$BRANCH" "$REPO_URL" "$ZBITXD_DIR"
fi
(
	cd "$ZBITXD_DIR"
	git checkout "$BRANCH"
	git pull
	git submodule update --init --recursive
	make
	sudo make install
)

echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
echo "*** Callsign->grid directory (one-time FCC ULS seed)"
echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
# sbitx.db is guaranteed to exist by now (created by the Makefile's own
# install target, data/create_db.sql, as part of the "sudo make install"
# just above) but zbitxd itself hasn't been (re)started yet -- so this
# writes with zero contention and doesn't need to worry about the
# main-thread-only db-access rule that applies once the daemon is live
# (see ft8_grid_queue's own comment, modem_ft8.c). Owned by the zbitxd
# system user once the daemon's Makefile install step chown -R's
# STATEDIR, same as every other file there -- but this script itself
# refuses to run as root (see the check above), so the actual sqlite
# write below needs sudo, same as systemctl further down.
SBITX_DB="/var/lib/zbitxd/sbitx.db"
sudo sqlite3 "$SBITX_DB" "CREATE TABLE IF NOT EXISTS callsign_grid (callsign TEXT PRIMARY KEY NOT NULL, grid TEXT NOT NULL, source TEXT NOT NULL DEFAULT 'decode', updated_at INTEGER NOT NULL DEFAULT 0);"
EXISTING_ULS_ROWS=$(sudo sqlite3 "$SBITX_DB" "SELECT COUNT(*) FROM callsign_grid WHERE source='uls';")
if [ "$EXISTING_ULS_ROWS" -gt 0 ]; then
	echo "callsign_grid already seeded ($EXISTING_ULS_ROWS rows), skipping."
else
	sudo python3 "$ZBITXD_DIR/scripts/seed_callsign_grid.py" "$SBITX_DB"
fi
sudo chown zbitxd:zbitxd "$SBITX_DB"

echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
echo "*** ttyd (web terminal for the CMD button)"
echo ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>"
# web/index.html's CMD button opens http://<host>:7681/ directly (ttyd
# running "login", so it authenticates against the box's own real
# accounts, not a separate credential) -- not packaged for Debian/
# Raspbian at all (confirmed live: empty apt-cache search on Raspbian
# Buster), so this was a real gap between what the UI already assumed
# existed and what install.sh actually set up. Pulls the matching
# static prebuilt binary from ttyd's own GitHub releases (no build
# toolchain needed -- these are statically linked) rather than building
# from source, since ttyd's own build deps (libwebsockets, json-c) not
# being in Buster's repos either would just move the same gap one step
# back. uname -m -> release asset name mapping covers every arch ttyd
# actually publishes; anything else just skips this step rather than
# guessing. -W: ttyd defaults to read-only (confirmed live -- the
# login prompt showed but rejected every keystroke, journalctl showing
# exactly why: "The --writable option is not set, will start in
# readonly mode") -- without it this is a terminal you can only look
# at, not use.
TTYD_ARCH=""
case "$(uname -m)" in
	x86_64) TTYD_ARCH="x86_64" ;;
	aarch64) TTYD_ARCH="aarch64" ;;
	armv6l|armv7l) TTYD_ARCH="armhf" ;;
	armv5*) TTYD_ARCH="arm" ;;
	i686) TTYD_ARCH="i686" ;;
esac
if [ -z "$TTYD_ARCH" ]; then
	echo "Unrecognized architecture $(uname -m) -- skipping ttyd, CMD button won't work."
elif [ -x /usr/local/bin/ttyd ]; then
	echo "ttyd already installed, skipping download."
else
	TTYD_VERSION="1.7.7"
	curl -sL -o /tmp/ttyd."$TTYD_ARCH" \
		"https://github.com/tsl0922/ttyd/releases/download/$TTYD_VERSION/ttyd.$TTYD_ARCH"
	sudo install -m 755 /tmp/ttyd."$TTYD_ARCH" /usr/local/bin/ttyd
	rm /tmp/ttyd."$TTYD_ARCH"
fi
if [ -x /usr/local/bin/ttyd ]; then
	sudo tee /etc/systemd/system/ttyd.service > /dev/null <<-EOF
	[Unit]
	Description=ttyd web terminal (used by the sBitx web UI's CMD button)
	After=network.target

	[Service]
	Type=simple
	ExecStart=/usr/local/bin/ttyd -W -p 7681 login
	Restart=on-failure
	RestartSec=2

	[Install]
	WantedBy=multi-user.target
	EOF
	sudo systemctl daemon-reload
	sudo systemctl enable --now ttyd
fi

sudo systemctl daemon-reload
sudo systemctl enable zbitxd
sudo systemctl restart zbitxd

echo "<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<"
echo "zbitxd ($BRANCH) has been installed and started."
echo "If this box has an existing hw_settings.ini/sbitx.db/user_settings.ini"
echo "to carry over, copy them into /var/lib/zbitxd now and run:"
echo "  sudo chown zbitxd:zbitxd /var/lib/zbitxd/*"
echo "  sudo systemctl restart zbitxd"
