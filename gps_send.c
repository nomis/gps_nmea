#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#ifdef GPS_NTP_C
# include <sys/time.h>
#endif
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#ifdef GPS_NTP_C
# include <sched.h>
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef GPS_NTP_C
# include <syslog.h>
#endif
#include <termios.h>
#include <unistd.h>

#include "gps_nmea.h"

/* Some code copied from taylor-uucp. */

#define ICLEAR_IFLAG (BRKINT | ICRNL | IGNBRK | IGNCR | IGNPAR \
	| INLCR | INPCK | ISTRIP | IXOFF | IXON \
	| PARMRK | IMAXBEL)
#define ICLEAR_OFLAG (OPOST)
#define ICLEAR_CFLAG (CSIZE | PARENB | PARODD | HUPCL)
#define ISET_CFLAG (CS8 | CREAD | CLOCAL)
#define ICLEAR_LFLAG (ECHO | ECHOE | ECHOK | ECHONL | ICANON | IEXTEN \
	| ISIG | NOFLSH | TOSTOP | PENDIN | CRTSCTS)

/* ---- */

int main(int argc, char *argv[]) {
	int s, ifidx, one = 1, fd, iflags;
	FILE *dev;
	struct sockaddr_in6 src;
	struct sockaddr_in6 dst;
	struct termios ios;
	char buf[1024];
#ifdef GPS_NTP_C
	struct sched_param schedp;
	pid_t pid;
#endif

	if (argc != 3) {
		printf("Usage: %s <device> <interface>\n", argv[0]);
		return 1;
	}

#ifdef GPS_NTP_C
	ntp_init();
#endif

	s = socket(PF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	cerror("Socket error", !s);

	ifidx = if_nametoindex(argv[2]);
	cerror("Interface not found", !ifidx);

	src.sin6_family = AF_INET6;
	src.sin6_addr = in6addr_any;
	src.sin6_port = htons(SPORT);
	src.sin6_scope_id = 0;

	dst.sin6_family = AF_INET6;
	if (!inet_pton(AF_INET6, GROUP, &dst.sin6_addr))
		return 3;
	dst.sin6_port = htons(DPORT);
	dst.sin6_scope_id = ifidx;

	cerror("Failed to set SO_REUSEADDR", setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)));
	cerror("Failed to bind source port", bind(s, (struct sockaddr*)&src, sizeof(src)));
	cerror("Failed to set multicast interface", setsockopt(s, SOL_IPV6, IPV6_MULTICAST_IF, &ifidx, sizeof(ifidx)));

	fd = open(argv[1], O_RDONLY|O_NONBLOCK);
	cerror(argv[1], fd < 0);

	iflags = fcntl(fd, F_GETFL, 0);
	cerror("Failed to get file descriptor flags for opened serial port", iflags < 0);
	iflags &= ~(O_NONBLOCK|O_NDELAY);
	iflags = fcntl(fd, F_SETFL, iflags);
	cerror("Failed to set file descriptor flags", (iflags & O_NONBLOCK) != 0);
	cerror("Failed to get terminal attributes", tcgetattr(fd, &ios));
	
	ios.c_iflag &=~ ICLEAR_IFLAG;
	ios.c_oflag &=~ ICLEAR_OFLAG;
	ios.c_cflag &=~ ICLEAR_CFLAG;
	ios.c_cflag |= ISET_CFLAG;
	ios.c_lflag &=~ ICLEAR_LFLAG;
	ios.c_cc[VMIN] = 1;
	ios.c_cc[VTIME] = 0;
	cfsetispeed(&ios, B38400);
	cfsetospeed(&ios, B38400);

	cerror("Failed to flush terminal input", ioctl(fd, TCFLSH, 0) < 0);
	cerror("Failed to set terminal attributes", tcsetattr(fd, TCSANOW, &ios));

	dev = fdopen(fd, "r");
	cerror(argv[1], !dev);

#ifdef GPS_NTP_C
	pid = getpid();
	cerror("Failed to get max scheduler priority",
		(schedp.sched_priority = sched_get_priority_max(SCHED_FIFO)) < 0);
	if (schedp.sched_priority > 10) {
		schedp.sched_priority -= 10;
		cerror("Failed to set scheduler policy", sched_setscheduler(pid, SCHED_FIFO, &schedp));
		schedp.sched_priority += 5;
	} else {
		cerror("Failed to set scheduler policy", sched_setscheduler(pid, SCHED_FIFO, &schedp));
	}
#endif

#ifdef GPS_NTP_C
	pid = fork();
	cerror("Failed to become a daemon", pid < 0);
	if (pid)
		exit(0);
	close(0);
	close(1);
	close(2);

	ntp_pps(fd, &schedp);
#endif

	if (geteuid() == 0) {
		cerror("Failed to drop SGID permissions", setregid(getgid(), getgid()));
		cerror("Failed to drop SUID permissions", setreuid(getuid(), getuid()));
	}

	while (!(errno = 0) && fgets(buf, 1024, dev) != NULL) {
		char checksum;
		int i, len;
#ifdef GPS_NTP_C
		struct timeval tv;

		cerror("Failed to get current time", gettimeofday(&tv, NULL));
#endif

		len = strlen(buf);
		if (buf[0] != '$' || len < 2)
			continue;

		checksum = buf[1];
		for (i = 2; ; i++)
			if (buf[i] == '*' || buf[i] == '\0')
				break;
			else
				checksum ^= buf[i];

		if (buf[i] != '*' || len - i < 2)
			continue;

		if (!(buf[i+1] >= '0' && buf[i+1] <= '9')
				&& !(buf[i+1] >= 'A' && buf[i+1] <= 'F')
				&& !(buf[i+2] >= '0' && buf[i+2] <= '9')
				&& !(buf[i+2] >= 'A' && buf[i+2] <= 'F'))
			continue;

		if (buf[i+1] >= '0' && buf[i+1] <= '9'
				&& (buf[i+1] - '0') != (checksum & 0xF0) >> 4)
			continue;

		if (buf[i+1] >= 'A' && buf[i+1] <= 'F'
				&& (buf[i+1] - 'A' + 10) != (checksum & 0xF0) >> 4)
			continue;

		if (buf[i+2] >= '0' && buf[i+2] <= '9'
				&& (buf[i+2] - '0') != (checksum & 0x0F))
			continue;

		if (buf[i+2] >= 'A' && buf[i+2] <= 'F'
				&& (buf[i+2] - 'A' + 10) != (checksum & 0x0F))
			continue;
		
		len = i+3;
		buf[len+1] = '\0';
#ifdef GPS_NTP_C
		ntp_nmea(tv, buf);
#endif
		cerror(argv[2], sendto(s, buf, len, MSG_DONTWAIT|MSG_NOSIGNAL, (struct sockaddr*)&dst, sizeof(dst)) != len);
	}
	cerror("Failed to close serial device", close(fd));
	exit(0);
}
