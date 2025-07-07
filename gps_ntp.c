#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>
#include <systemd/sd-daemon.h>
#include <syslog.h>
#include <time.h>
#include <pthread.h>
#include <stdlib.h>
#include <termios.h>

#define GPS_NTP_C
#include "gps_nmea.h"
#include "gps_send.c"
#define UNIT 0
#define QUIET

#define REFID_GPS "GPS"
#define REFID_PPS "PPS"

#define tv_to_ull(x) (unsigned long long)((unsigned long long)(x).tv_sec*1000000 + (unsigned long long)(x).tv_usec)

/* Some code copied from gpsd (http://gpsd.berlios.be/). */

#define DD(s)   ((int)((s)[0]-'0')*10+(int)((s)[1]-'0'))
#define DDD(s)   ((int)((s)[0]-'0')*100+(int)((s)[1]-'0')*10+(int)((s)[2]-'0'))
#define CENTURY_BASE 2000

/* Some code copied from radioclkd (Jonathan A. Buzzard <jonathan@buzzard.org.uk>). */

#define SHMKEY 0x4e545030
struct shmTime {
	int mode;
	int count;
	time_t clockTimeStampSec;
	int clockTimeStampUSec;
	time_t receiveTimeStampSec;
	int receiveTimeStampUSec;
	int leap;
	int precision;
	int nsamples;
	int valid;
	uint32_t refid;
	char dummy[sizeof(int) * 10 - sizeof(uint32_t)];
};

enum { LEAP_NOWARNING=0x00, LEAP_NOTINSYNC=0x03};

#ifndef SIMPLE
/* Accuracy is assumed to be 2^PRECISION seconds -20 is approximately 954ns */
# define PRECISION (-20)
#else
/* Accuracy is assumed to be 2^PRECISION seconds -10 is approximately 976ms */
# define PRECISION (-10)
#endif

void PutTimeStamp(const struct timeval *local, const struct timeval *nmea, struct shmTime *shm, int leap, const char *refid) {
	shm->mode = 1;
	shm->valid = 0;

	__asm__ __volatile__ ("":::"memory");

	shm->leap = leap;
	shm->precision = PRECISION;
	shm->clockTimeStampSec = (time_t)nmea->tv_sec;
	shm->clockTimeStampUSec = (int)nmea->tv_usec;
	shm->receiveTimeStampSec = (time_t)local->tv_sec;
	shm->receiveTimeStampUSec = (int)local->tv_usec;
	shm->refid = *(uint32_t*)refid;

	__asm__ __volatile__ ("":::"memory");

	shm->count++;
	shm->valid = 1;

	return;
}

/*
 * Attach the shared memory segment for the reference clock driver
 */
struct shmTime *AttachSharedMemory(int unit, int *shmid) {
	struct shmTime *shm;

	*shmid = shmget(SHMKEY+unit, sizeof(struct shmTime), IPC_CREAT | 0700);
	if (*shmid==-1)
		return NULL;

	shm = (struct shmTime *) shmat(*shmid, 0, 0);
	if ((shm==(void *) -1) || (shm==0))
		return NULL;

	return shm;
}

/* ---- */

struct shmTime *gps;
#ifndef SIMPLE
volatile struct timeval lastpps;
#endif

void ntp_init(void) {
	int shmid;

	openlog("gps_ntp", LOG_PID | LOG_CONS | LOG_PERROR, LOG_DAEMON);
	cerror("Failed to set time zone", putenv("TZ=UTC"));
	cerror("Failed to lock memory pages", mlockall(MCL_CURRENT | MCL_FUTURE));
	cerror("Failed to attach shared memory for refclock",
		(gps = AttachSharedMemory(UNIT, &shmid)) == NULL || shmid == -1);
	sd_notifyf(0, "READY=1\nSTATUS=Init\n");
}

#ifndef SIMPLE
static void *ntp_ppsmon(void *data) {
	int fd = (int)(size_t)data;
	int state = 0, last = 0;

	while (ioctl(fd, TIOCMIWAIT, TIOCM_CD) == 0) {
		struct timeval tv;
		gettimeofday(&tv, NULL);

		if (ioctl(fd, TIOCMGET, &state) != 0) {
			xerror("Failed to get serial IO status");
			return NULL;
		}

		state = (int)((state & TIOCM_CD) != 0);

		if (last != state && state == 1) {
			lastpps.tv_usec = tv.tv_usec;
			lastpps.tv_sec = tv.tv_sec;
#ifndef QUIET
			printf("PPS at %lu.%06lu\n", tv.tv_sec, tv.tv_usec);
#endif
		}

		last = state;
	}
	xerror("Failed to wait for DCD");
	return NULL;
}

void ntp_pps(int fd, const struct sched_param *schedp) {
	pthread_t pt;

	cerror("Failed to start PPS thread", pthread_create(&pt, NULL, ntp_ppsmon, (void*)(size_t)fd));
	cerror("Failed to set scheduler policy for PPS thread", pthread_setschedparam(pt, SCHED_FIFO, schedp));
}

struct timeval ntp_getpps() {
	return lastpps;
}

void ntp_invalidate() {
	lastpps.tv_sec = 0;
}
#else
void ntp_invalidate() {}
#endif

void ntp_nmea(const struct timeval tv, const char *buf) {
	/* $GPRMC,191809,V,0000.0000,N,00000.0000,W,,,191007,004.8,W,N*01 */
	/*              1 2         3 4          5 678      9             */
	/* $GNRMC,122632.000,A,0000.0000,N,00000.0000,W,0.11,83.54,070725,,,A*56 */
	/*                  1 2         3 4          5 6    7     8      9       */
	if (!strncmp(buf, "$GPRMC,", 7)) {
		const char *nmea_time, *nmea_skip, *nmea_date;
		struct tm nmea_tm = { 0 };
		time_t nmea_t;
		struct timeval nmea_tv;
#ifndef SIMPLE
		struct timeval pps;
#endif
		char sync;

		nmea_time = &buf[7];
		if (*nmea_time == '\0') { ntp_invalidate(); sd_notifyf(0, "STATUS=Invalid: %s\n", buf); return; }

		nmea_skip = strstr(nmea_time, ","); // 1
		if (nmea_skip == NULL) { ntp_invalidate(); sd_notifyf(0, "STATUS=Invalid: %s\n", buf); return; }

		nmea_skip = strstr(&nmea_skip[1], ","); // 2
		if (nmea_skip == NULL) { ntp_invalidate(); sd_notifyf(0, "STATUS=Invalid: %s\n", buf); return; }

		nmea_skip = strstr(&nmea_skip[1], ","); // 3
		if (nmea_skip == NULL) { ntp_invalidate(); sd_notifyf(0, "STATUS=Invalid: %s\n", buf); return; }

		nmea_skip = strstr(&nmea_skip[1], ","); // 4
		if (nmea_skip == NULL) { ntp_invalidate(); sd_notifyf(0, "STATUS=Invalid: %s\n", buf); return; }

		nmea_skip = strstr(&nmea_skip[1], ","); // 5
		if (nmea_skip == NULL) { ntp_invalidate(); sd_notifyf(0, "STATUS=Invalid: %s\n", buf); return; }

		nmea_skip = strstr(&nmea_skip[1], ","); // 6
		if (nmea_skip == NULL) { ntp_invalidate(); sd_notifyf(0, "STATUS=Invalid: %s\n", buf); return; }

		nmea_skip = strstr(&nmea_skip[1], ","); // 7
		if (nmea_skip == NULL) { ntp_invalidate(); sd_notifyf(0, "STATUS=Invalid: %s\n", buf); return; }

		nmea_skip = strstr(&nmea_skip[1], ","); // 8
		if (nmea_skip == NULL) { ntp_invalidate(); sd_notifyf(0, "STATUS=Invalid: %s\n", buf); return; }

		nmea_date = &nmea_skip[1];
		if (*nmea_date == '\0') { ntp_invalidate(); sd_notifyf(0, "STATUS=Invalid: %s\n", buf); return; }

		nmea_skip = strstr(&nmea_skip[1], ","); // 9
		if (nmea_skip == NULL) { ntp_invalidate(); sd_notifyf(0, "STATUS=Invalid: %s\n", buf); return; }

		nmea_tm.tm_hour = DD(nmea_time);
		nmea_tm.tm_min = DD(nmea_time+2);
		nmea_tm.tm_sec = DD(nmea_time+4);

		nmea_tm.tm_year = (CENTURY_BASE + DD(nmea_date+4)) - 1900;
		nmea_tm.tm_mon = DD(nmea_date+2)-1;
		nmea_tm.tm_mday = DD(nmea_date);

		sync = nmea_time[7];

		nmea_t = mktime(&nmea_tm);
		if (nmea_t == -1) {
			ntp_invalidate();
			sd_notifyf(0, "STATUS=Invalid time: %s (year=%d, mon=%d, mday=%d, hour=%d, min=%d, sec=%d, isdst=%d)\n",
				buf, nmea_tm.tm_year, nmea_tm.tm_mon, nmea_tm.tm_mday,
				nmea_tm.tm_hour, nmea_tm.tm_min, nmea_tm.tm_sec, nmea_tm.tm_isdst);
			return;
		}
		nmea_tv.tv_sec = nmea_t;
		nmea_tv.tv_usec = 0;

		if (nmea_time[6] == '.') {
			nmea_tv.tv_usec = DD(nmea_time+7) * 1000;
			if (nmea_tv.tv_usec != 0) {
				ntp_invalidate();
				sd_notifyf(0, "STATUS=Invalid time: %s (year=%d, mon=%d, mday=%d, hour=%d, min=%d, sec=%d, msec=%ld, isdst=%d)\n",
					buf, nmea_tm.tm_year, nmea_tm.tm_mon, nmea_tm.tm_mday,
					nmea_tm.tm_hour, nmea_tm.tm_min, nmea_tm.tm_sec, nmea_tv.tv_usec / 1000, nmea_tm.tm_isdst);
				return;
			}
		}

#ifndef SIMPLE
		pps = ntp_getpps();
		if (pps.tv_sec == 0) {
#ifndef QUIET
			printf("got time %lu, but no pps\n", nmea_t);
#endif
			sd_notifyf(0, "STATUS=Time %lu, no pps (%c)\n", nmea_t, sync);
			return;
		}

		if (tv_to_ull(pps) > tv_to_ull(tv)) {
			ntp_invalidate();
			sd_notifyf(0, "STATUS=Time %lu, pps late %lu.%06lu > %lu.%06lu (%c)\n",
				nmea_t, pps.tv_sec, pps.tv_usec, tv.tv_sec, tv.tv_usec, sync);
			return;
		}

		if (tv_to_ull(tv) - tv_to_ull(pps) > 500000) {
			ntp_invalidate();
			sd_notifyf(0, "STATUS=Time %lu, nmea late %lu.%06lu - %lu.%06lu = %llu (%c)\n",
				nmea_t, tv.tv_sec, tv.tv_usec, pps.tv_sec, pps.tv_usec, tv_to_ull(tv) - tv_to_ull(pps), sync);
			return;
		}
#else
		nmea_tv.tv_sec++;
		nmea_tv.tv_usec = 250000;
#endif

		if (sync != 'A') {
#ifndef SIMPLE
			if (pps.tv_usec <= 25000) {
				nmea_tv.tv_sec = pps.tv_sec;
			} else if (pps.tv_usec >= 975000) {
				nmea_tv.tv_sec = pps.tv_sec+1;
			} else {
				ntp_invalidate();
				sd_notifyf(0, "STATUS=Time %lu, at %lu.%06lu pps %lu.%06lu out of range (%c)\n",
					nmea_t, tv.tv_sec, tv.tv_usec, pps.tv_sec, pps.tv_usec, sync);
				return;
			}
#else
			return;
#endif
		}

#ifndef SIMPLE
		PutTimeStamp(&pps, &nmea_tv, gps, LEAP_NOWARNING, sync == 'A' ? REFID_GPS : REFID_PPS);
#else
		if (tv.tv_sec < 365*86400) {
			settimeofday(&nmea_tv, NULL);
		} else {
			PutTimeStamp(&tv, &nmea_tv, gps, LEAP_NOWARNING, REFID_GPS);
		}
#endif
#ifndef QUIET
#ifndef SIMPLE
		printf("put time %lu, pps %lu.%06lu\n", nmea_t, pps.tv_sec, pps.tv_usec);
#else
		printf("put time %lu.%06lu, nmea %lu.%06lu\n", tv.tv_sec, tv.tv_usec, nmea_tv.tv_sec, nmea_tv.tv_usec);
#endif
		fflush(NULL);
#endif

#ifndef SIMPLE
		sd_notifyf(0, "STATUS=Put (%u) time %lu, at %lu.%06lu pps %lu.%06lu (%c)\n",
			gps->count, nmea_t, tv.tv_sec, tv.tv_usec, pps.tv_sec, pps.tv_usec, sync);
#endif
	}
}
