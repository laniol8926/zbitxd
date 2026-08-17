TARGET ?= zbitxd
OWNER ?= $(TARGET)
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
STATEDIR ?= /var/lib/$(OWNER)
SHAREDIR ?= $(PREFIX)/share/$(OWNER)
SOURCES = $(wildcard *.c)
OBJECTS = $(SOURCES:.c=.o)
FFTOBJ = ft8_lib/.build/fft/kiss_fft.o ft8_lib/.build/fft/kiss_fftr.o
HEADERS = $(wildcard *.h)
CFLAGS = -I.
LIBS = -lasound -lm -lfftw3 -lfftw3f -pthread -lsqlite3 -lsystemd ft8_lib/libft8.a
ifdef SBITX_UNUSED
## remove and print unused code
CFLAGS += -ffunction-sections -fdata-sections
LIBS += -Wl,--gc-sections,--print-gc-sections
endif
ifdef SBITX_DEBUG
CFLAGS += -ggdb3 -fsanitize=address
LIBS += -fsanitize=address
endif
CC = gcc
LINK = gcc
STRIP = strip

$(TARGET): create_configure.h $(OBJECTS) ft8_lib/libft8.a
	$(LINK) $(LFLAGS) -o $(TARGET) $(OBJECTS) $(FFTOBJ) $(LIBPATH) $(LIBS)

.c.o: $(HEADERS)
	$(CC) -c $(CFLAGS) $(DEBUGFLAGS) $(INCPATH) -o $@ $<

create_configure.h:
	$(shell echo "#define STATEDIR \"$(STATEDIR)\"" > configure.h) 
	$(shell echo "#define SHAREDIR \"$(SHAREDIR)\"" >> configure.h) 

ft8_lib/libft8.a:
ifdef SBITX_DEBUG
	$(MAKE) FT8_DEBUG=1 -C ft8_lib
else
	$(MAKE) -C ft8_lib
endif

clean:
	-rm -f configure.h
	-rm -f $(OBJECTS)
	-rm -f *~ core *.core
	-rm -f $(TARGET)
	$(MAKE) -C ft8_lib clean

adduser:
	-adduser --system --group --home $(DESTDIR)/$(STATEDIR) --disabled-password $(OWNER)
	-adduser $(OWNER) audio
	# needed for CAT over a USB-serial rig (e.g. /dev/ttyACM0, owned
	# root:dialout) in the generic-rig backend -- without this rigctld
	# hangs with no CAT response and no error at all under systemd
	-adduser $(OWNER) dialout
	# only -h (shutdown) was ever whitelisted here, so the web UI's
	# Reboot button's "sudo /sbin/shutdown -r now" silently failed under
	# systemd (no TTY for the password sudo then demands) -- confirmed
	# via "sudo -u zbitxd sudo -n /sbin/shutdown -r now" -> "a password
	# is required". Both commands the daemon actually runs need listing.
	-printf '%s\n' "$(OWNER) ALL=NOPASSWD: /sbin/shutdown -h now" "$(OWNER) ALL=NOPASSWD: /sbin/shutdown -r now" >/etc/sudoers.d/999_zbitxd
	-chmod 440 /etc/sudoers.d/999_zbitxd

install: adduser
	install -D --mode=755 $(TARGET) $(DESTDIR)/$(BINDIR)/$(TARGET)
	install -d --owner=$(OWNER) --group=$(OWNER) $(DESTDIR)/$(SHAREDIR)/web
	install -m 644 --owner=$(OWNER) --group=$(OWNER) web/* $(DESTDIR)/$(SHAREDIR)/web
	install -d --owner=$(OWNER) --group=$(OWNER) $(DESTDIR)/$(STATEDIR)
	install -m 644 --owner=$(OWNER) --group=$(OWNER) data/default_hw_settings.ini $(DESTDIR)/$(STATEDIR)
	install -m 644 --owner=$(OWNER) --group=$(OWNER) data/default_settings.ini $(DESTDIR)/$(STATEDIR)
	# STATEDIR can already exist with files owned by whoever ran the
	# daemon manually before it ever ran as its own systemd user (e.g.
	# left over from testing as a login user) -- the daemon then
	# segfaults on startup with no useful error. Force ownership of
	# everything already in there, not just the two files just installed.
	chown -R $(OWNER):$(OWNER) $(DESTDIR)/$(STATEDIR)
	install -d $(DESTDIR)/$(PREFIX)/lib/systemd/system/
	install -m 644 systemd/zbitxd.service $(DESTDIR)/$(PREFIX)/lib/systemd/system
	ln -sf /var/lib/zbitxd/grids.txt /usr/local/share/zbitxd/web/grids.txt
	# Same pattern as grids.txt above -- export_adif() writes here
	# (STATEDIR, daemon-writable), symlinked into the served web root so
	# the client can just download it directly, no dedicated HTTP route
	# needed (see the "any other URI serves static files" comment in
	# webserver.c).
	ln -sf /var/lib/zbitxd/logbook_export.adi /usr/local/share/zbitxd/web/logbook_export.adi
ifeq ("$(wildcard $(DESTDIR)/$(STATEDIR)/sbitx.db)","")
	$(shell sqlite3 $(DESTDIR)/$(STATEDIR)/sbitx.db < data/create_db.sql)
endif

uninstall:
	rm -f $(DESTDIR)/$(BINDIR)/$(TARGET)
	rm -rf $(DESTDIR)/$(SHAREDIR)
	rm -f $(DESTDIR)/$(STATEDIR)/default_hw_settings.ini
	rm -f $(DESTDIR)/$(STATEDIR)/default_settings.ini
	rm -f $(DESTDIR)/$(PREFIX)/lib/systemd/system/zbitxd.service

.PHONY: adduser create_configure.h clean install uninstall
