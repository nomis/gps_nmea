CFLAGS=-Wall -Wextra -O2
.PHONY: all clean
all: gps_send gps_recv gps_ntp
clean:
	rm -f gps_send gps_recv gps_ntp
gps_send: gps_send.c gps_nmea.h
	$(CC) $(CFLAGS) -o $@ $<
gps_recv: gps_recv.c gps_nmea.h
	$(CC) $(CFLAGS) -o $@ $<
gps_ntp: gps_ntp.c gps_send.c gps_nmea.h
	$(CC) $(CFLAGS) -o $@ $< -lpthread
