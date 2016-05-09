.PHONY: all clean install

CFLAGS=-Wall -Wextra -Wshadow -ggdb -O2 -DSET_UID=1 -DSET_GID=1 -DNOFORK
INSTALL=install

prefix=/usr
exec_prefix=$(prefix)
bindir=$(exec_prefix)/bin
sbindir=$(exec_prefix)/sbin

all: gps_send gps_recv gps_ntp gps_simple_ntp
clean:
	rm -f gps_send gps_recv gps_ntp gps_simple_ntp
install: all
	$(INSTALL) -m 755 -D gps_recv $(DESTDIR)$(bindir)/gps_recv
	$(INSTALL) -m 755 -D gps_send $(DESTDIR)$(sbindir)/gps_send
	$(INSTALL) -m 755 -D gps_ntp $(DESTDIR)$(sbindir)/gps_ntp
	$(INSTALL) -m 755 -D gps_simple_ntp $(DESTDIR)$(sbindir)/gps_simple_ntp
	$(INSTALL) -m 644 -D gps-nmea-ntp@.service $(DESTDIR)/lib/systemd/system/gps-nmea-ntp@.service
gps_send: gps_send.c gps_nmea.h
	$(CC) $(CFLAGS) -o $@ $<
gps_recv: gps_recv.c gps_nmea.h
	$(CC) $(CFLAGS) -o $@ $<
gps_ntp: gps_ntp.c gps_send.c gps_nmea.h
	$(CC) $(CFLAGS) -o $@ $< -lpthread
gps_simple_ntp: gps_ntp.c gps_send.c gps_nmea.h
	$(CC) $(CFLAGS) -DSIMPLE -o $@ $< -lpthread
