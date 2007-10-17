CFLAGS=-Wall -O2
.PHONY: all clean
all: gps_send gps_recv
clean:
	rm -f gps_send gps_recv
gps_send: gps_send.c gps_nmea.h
gps_recv: gps_recv.c gps_nmea.h
