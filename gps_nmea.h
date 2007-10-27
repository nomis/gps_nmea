#define GROUP "FF02:0:0:0:0:0:0:114"
#define SPORT 87
#define DPORT 1986

#ifdef GPS_NTP_C
# define xerror(msg) \
	do { \
		if (msg != NULL) \
			syslog(LOG_ERR, "%s: %m", msg); \
		else \
			syslog(LOG_ERR, "%m"); \
		exit(2); \
	} while(0)
#else
# define xerror(msg) do { perror(msg); exit(2); } while(0)
#endif
#define cerror(msg, expr) do { if (expr) xerror(msg); } while(0)

#ifdef GPS_NTP_C
void ntp_init(void);
void ntp_pps(int fd, const struct sched_param *schedp;);
void ntp_nmea(const struct timeval tv, const char *buf);
#endif
