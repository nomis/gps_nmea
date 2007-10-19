#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#include "gps_nmea.h"

#define xerror(msg) do { perror(msg); exit(2); } while(0)
#define cerror(msg, expr) do { if (expr) xerror(msg); } while(0)

int main(int argc, char *argv[]) {
	int s, ifidx, one = 1;
	FILE *fd;
	struct sockaddr_in6 src;
	struct sockaddr_in6 dst;
	struct termios ios;
	char buf[1024];

	if (argc != 3) {
		printf("Usage: %s <device> <interface>\n", argv[0]);
		return 1;
	}

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

	fd = fopen(argv[1], "r");
	cerror(argv[1], !fd);

	cerror(argv[1], ioctl(fileno(fd), TCGETA, &ios));
	ios.c_cflag &= ~CRTSCTS;
	ios.c_lflag |= CLOCAL;
	ios.c_lflag |= ICANON;
	cerror(argv[1], ioctl(fileno(fd), TCSETA, &ios));

	cerror("Failed to set SO_REUSEADDR", setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)));
	cerror("Failed to bind source port", bind(s, (struct sockaddr*)&src, sizeof(src)));
	cerror("Failed to set multicast interface", setsockopt(s, SOL_IPV6, IPV6_MULTICAST_IF, &ifidx, sizeof(ifidx)));

	while (fgets(buf, 1024, fd) != NULL) {
		char checksum;
		int i, len;

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
		cerror(argv[2], sendto(s, buf, len, MSG_DONTWAIT|MSG_NOSIGNAL, (struct sockaddr*)&dst, sizeof(dst)) != len);
	}
	xerror(argv[1]);
}
