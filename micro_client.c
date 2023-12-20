#undef NDEBUG
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
//#include <arpa/inet.h>
#include <netinet/in.h>
//#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "util.h"

struct conn {
  int sock;
  ssize_t (*read)(struct conn  *c, void *buf, size_t count);
  ssize_t (*write)(struct conn *c, const void *buf, size_t count);
};

static ssize_t tcp_read(struct conn *c, void *buf, size_t count) {
  if (c == NULL) {
    fprintf(stderr, "Invalid connection provided for tcp read.\n");
    abort();
  }
  return read(c->sock, buf, count);
}

static ssize_t tcp_write(struct conn *c, const void *buf, size_t count) {
  if (c == NULL) {
    fprintf(stderr, "Invalid connection provided for tcp write.\n");
    abort();
  }
  return write(c->sock, buf, count);
}

static void close_conn(struct conn *con) {
  if (!con) {
    return;
  }
  if (con->sock > 0) {
    close(con->sock);
  }
  free(con);
}

static struct addrinfo *lookuphost(const char *hostname, in_port_t port) {
  struct addrinfo *ai = 0;
  struct addrinfo hints = { .ai_family = AF_UNSPEC,
                            .ai_socktype = SOCK_STREAM,
                            .ai_protocol = IPPROTO_TCP };
  char service[NI_MAXSERV];
  int error;

  (void)snprintf(service, NI_MAXSERV, "%d", port);
  if ((error = getaddrinfo(hostname, service, &hints, &ai)) != 0) {
    if (error != EAI_SYSTEM) {
      fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(error));
    } else {
      perror("getaddrinfo()");
    }
  }
  return ai;
}

static struct conn *connect_server(const char *hostname, in_port_t port, bool nonblock) {
  struct conn *c;
  if (!(c = (struct conn *)calloc(1, sizeof(struct conn)))) {
    fprintf(stderr, "Failed to allocate the client connection: %s\n", strerror(errno));
    return NULL;
  }

  struct addrinfo *ai = lookuphost(hostname, port);
  int sock = -1;
  if (ai != NULL) {
    if ((sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) != -1) {
      if (connect(sock, ai->ai_addr, ai->ai_addrlen) == -1) {
        fprintf(stderr, "Failed to connect socket: %s\n", strerror(errno));
        close(sock);
        sock = -1;
      } else if (nonblock) {
        int flags = fcntl(sock, F_GETFL, 0);
        if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
          fprintf(stderr, "Failed to enable nonblocking mode: %s\n", strerror(errno));
          close(sock);
          sock = -1;
        }
      }
    } else {
      fprintf(stderr, "Failed to create socket: %s\n", strerror(errno));
    }
    freeaddrinfo(ai);
  }
  c->sock = sock;
  c->read = tcp_read;
  c->write = tcp_write;
  return c;
}

static void send_ascii_command(struct conn *con, const char *buf) {
  off_t offset = 0;
  const char* ptr = buf;
  size_t len = strlen(buf);

  do {
    ssize_t nw = con->write((void*)con, ptr + offset, len - offset);
    if (nw == -1) {
      if (errno != EINTR) {
        fprintf(stderr, "Failed to write: %s\n", strerror(errno));
        abort();
      }
    } else {
      offset += nw;
    }
  } while (offset < len);
}

static void read_ascii_response(struct conn *con, char *buffer, size_t size) {
  off_t offset = 0;
  bool need_more = true;
  do {
    ssize_t nr = con->read(con, buffer + offset, 1);
    if (nr == -1) {
      if (errno != EINTR) {
        fprintf(stderr, "Failed to read: %s\n", strerror(errno));
        abort();
      }
    } else if (nr == 1) {
      if (buffer[offset] == '\n') {
        need_more = false;
        buffer[offset + 1] = '\0';
      }
      offset += nr;
      if (offset + 1 >= size) {
        fprintf(stderr, "Invalid: Buffer size exceeds set limit.\n");
        abort();
      }
    } else { /* nr == 0 */
      fprintf(stderr, "Invalid number of bytes read.\n");
      abort();
    }
  } while (need_more);
}

static int exit_loop = 0;
static void set_loop_exit(int signum) {
  exit_loop = 1;
}

int main(int argc, char **argv) {

  if (argc != 5) {
    fprintf(stderr, "Micro Client args: host-name, port, runtime(s), inter-request-delay(us)\n");
    abort();
  }
  char *hostname = argv[1];
  in_port_t port = atoi(argv[2]);
  int nsecs      = atoi(argv[3]);
  int delay      = atoi(argv[4]);

  signal(SIGALRM, set_loop_exit);
  alarm(nsecs);

  struct conn *con = connect_server(hostname, port, false);
  if (!con || !con->sock) {
    fprintf(stderr, "Unable to connect to %s on port %d.\n", hostname, port);
    abort();
  }

  char buffer[1024];
  int count = 0;
  while (exit_loop == 0) {
    send_ascii_command(con, "stats cachedump 1 0 0\r\n");
    read_ascii_response(con, buffer, sizeof(buffer));
    if (strncmp(buffer, "END", strlen("END"))) {
      fprintf(stderr, "Received invalid stats from cachedump :\n%s\n", buffer);
      abort();
    }
    count++;
    printf("Dummy stats cachedump requests processed = %d\r", count);
    usleep(delay);
  }
  printf("\n");

  send_ascii_command(con, "shutdown\r\n");
  /* verify that the server closed the connection */
  if (con->read(con, buffer, sizeof(buffer))) {
    fprintf(stderr, "Unable to shutdown the server.\n");
    abort();
  }
  close_conn(con);

  return 0;
}
