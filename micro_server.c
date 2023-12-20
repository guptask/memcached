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
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "util.h"

static pid_t start_server(in_port_t *port_out, bool daemon, int timeout) {
  char environment[80];
  snprintf(environment, sizeof(environment),
             "MEMCACHED_PORT_FILENAME=/tmp/ports.%lu", (long)getpid());
  char *filename= environment + strlen("MEMCACHED_PORT_FILENAME=");
  char pid_file[80];
  snprintf(pid_file, sizeof(pid_file), "/tmp/pid.%lu", (long)getpid());

  remove(filename);
  remove(pid_file);

  pid_t pid = fork();
  assert(pid != -1);

  if (pid == 0) {
    /* Child */
    char *argv[24];
    int arg = 0;
    char tmo[24];
    snprintf(tmo, sizeof(tmo), "%u", timeout);

    putenv(environment);

    if (!daemon) {
      argv[arg++] = (char*)"./timedrun";
      argv[arg++] = tmo;
    }
    argv[arg++] = (char*)"./memcached";
    argv[arg++] = (char*)"-A";
    argv[arg++] = (char*)"-p";
    argv[arg++] = (char*)"-1";
    argv[arg++] = (char*)"-U";
    argv[arg++] = (char*)"0";

    /* Handle rpmbuild and the like doing this as root */
    if (getuid() == 0) {
      argv[arg++] = (char*)"-u";
      argv[arg++] = (char*)"root";
    }
    if (daemon) {
      argv[arg++] = (char*)"-d";
      argv[arg++] = (char*)"-P";
      argv[arg++] = pid_file;
    }
    argv[arg++] = NULL;
    assert(execv(argv[0], argv) != -1);
  }

  /* Yeah just let us "busy-wait" for the file to be created ;-) */
  while (access(filename, F_OK) == -1) {
    usleep(10);
  }

  FILE *fp = fopen(filename, "r");
  if (fp == NULL) {
    fprintf(stderr, "Failed to open the file containing port numbers: %s\n", strerror(errno));
    assert(false);
  }

  *port_out = (in_port_t)-1;
  char buffer[80];
  while ((fgets(buffer, sizeof(buffer), fp)) != NULL) {
    if (strncmp(buffer, "TCP INET: ", 10) == 0) {
      int32_t val;
      assert(safe_strtol(buffer + 10, &val));
      *port_out = (in_port_t)val;
    }
  }
  fclose(fp);
  assert(remove(filename) == 0);

  if (daemon) {
    /* loop and wait for the pid file.. There is a potential race
     * condition that the server just created the file but isn't
     * finished writing the content, so we loop a few times
     * reading as well
     */
    while (access(pid_file, F_OK) == -1) {
      usleep(10);
    }

    fp = fopen(pid_file, "r");
    if (fp == NULL) {
      fprintf(stderr, "Failed to open pid file: %s\n", strerror(errno));
      assert(false);
    }

    /* Avoid race by retrying 20 times */
    for (int x = 0; x < 20 && fgets(buffer, sizeof(buffer), fp) == NULL; x++) {
      usleep(10);
    }
    fclose(fp);

    int32_t val;
    assert(safe_strtol(buffer, &val));
    pid = (pid_t)val;
  }
  return pid;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "Micro server requires the following args - time_to_live(secs)\n");
    abort();
  }
  int ttl = atoi(argv[1]);
  in_port_t port;
  pid_t server_pid = start_server(&port, false, ttl);
  assert(kill(server_pid, 0) == 0);
  printf("Server started at port %d.\n", port);
  return 0;
}
