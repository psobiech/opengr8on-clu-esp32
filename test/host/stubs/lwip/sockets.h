// Host stub: BSD sockets for udp_server.c. The socket calls are routed to host_* fakes that the
// test defines (captured sends, scripted receives); the types and inet_* helpers are the host's.
#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static inline char *inet_ntoa_r(struct in_addr a, char *buf, int len)
{
    return (char *)inet_ntop(AF_INET, &a, buf, (socklen_t)len);
}

int host_socket(int domain, int type, int proto);
int host_bind(int s, const struct sockaddr *a, socklen_t len);
int host_setsockopt(int s, int level, int opt, const void *v, socklen_t len);
ssize_t host_sendto(int s, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tl);
ssize_t host_recvfrom(int s, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fl);
int host_close(int s);
int host_listen(int s, int backlog);
int host_accept(int s, struct sockaddr *a, socklen_t *al);
ssize_t host_send(int s, const void *buf, size_t len, int flags);
ssize_t host_recv(int s, void *buf, size_t len, int flags);
int host_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv);
#define socket host_socket
#define bind host_bind
#define setsockopt host_setsockopt
#define sendto host_sendto
#define recvfrom host_recvfrom
#define close host_close
#define listen host_listen
#define accept host_accept
#define send host_send
#define recv host_recv
#define select host_select
