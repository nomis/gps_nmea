#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "gps_nmea.h"

int main(int argc, char *argv[]) {
	int s, len, ifidx, one = 1;
	struct sockaddr_in6 src;
	struct sockaddr_in6 dst;
	struct sockaddr_in6 rcv;
	socklen_t rcvlen = sizeof(rcv);
	struct group_source_req gsreq;
	char buf[1024];

	if (argc != 3) {
		printf("Usage: %s <source interface> <source ip>\n", argv[0]);
		return 1;
	}

	s = socket(PF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	cerror("Socket error", !s);

	ifidx = if_nametoindex(argv[1]);
	cerror("Interface not found", !ifidx);

	src.sin6_family = AF_INET6;
	cerror("Invalid source", !inet_pton(AF_INET6, argv[2], &src.sin6_addr));
	src.sin6_port = htons(SPORT);
	src.sin6_scope_id = ifidx;

	dst.sin6_family = AF_INET6;
	cerror("Invalid group", !inet_pton(AF_INET6, GROUP, &dst.sin6_addr));
	dst.sin6_port = htons(DPORT);
	dst.sin6_scope_id = ifidx;

	cerror("Failed to set SO_REUSEADDR", setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)));
	cerror("Failed to bind destination port", bind(s, (struct sockaddr*)&dst, sizeof(dst)));
	gsreq.gsr_interface = ifidx;
	memcpy(&gsreq.gsr_source, &src, sizeof(src));
	memcpy(&gsreq.gsr_group, &dst, sizeof(dst));
	cerror("Failed to join multicast group", setsockopt(s, IPPROTO_IPV6, MCAST_JOIN_SOURCE_GROUP, &gsreq, sizeof(gsreq)));

	while ((len = recvfrom(s, buf, 1023, MSG_NOSIGNAL, (struct sockaddr*)&rcv, &rcvlen)) >= 0) {
		if (rcvlen != sizeof(src)
				|| rcv.sin6_family != src.sin6_family
				|| rcv.sin6_port != src.sin6_port
				|| rcv.sin6_scope_id != src.sin6_scope_id)
			continue;
		buf[len] = '\0';
		cerror(NULL, puts(buf) <= 0);
		cerror(NULL, fflush(stdout) != 0);
	}
	xerror("Error receiving data");
}
