#include "user.h"
#include "inc/product.h"

#define SERVICE_COMMAND_PATH "/fnusvc.command"
#define SERVICE_STATUS_PATH "/fnusvc.status"
#define MAX_RESTARTS 3

enum service_state { SERVICE_STOPPED, SERVICE_STARTING, SERVICE_RUNNING, SERVICE_FAILED };

struct service {
  char *name;
  char *path;
  char *argv[2];
  pid_t pid;
  int state;
  int restarts;
  int restart_pending;
};

static struct service services[] = {
    {"fnu-health", "/bin/fnu-health", {"/bin/fnu-health", 0}, -1,
     SERVICE_STOPPED, 0, 0},
    {"sh", "/bin/sh", {"/bin/sh", 0}, -1, SERVICE_STOPPED, 0, 0},
};

static int service_count = sizeof(services) / sizeof(services[0]);

static char *state_name(int state) {
  if (state == SERVICE_RUNNING)
    return "running";
  if (state == SERVICE_FAILED)
    return "failed";
  if (state == SERVICE_STARTING)
    return "starting";
  return "stopped";
}

// Best-effort local snapshot. It is neither an audit log nor an atomic IPC
// protocol; consumers must tolerate a missing or partial file.
static void publish_status(void) {
  int fd, i;

  fd = open(SERVICE_STATUS_PATH, O_WRONLY);
  if (fd < 0) {
    printf("fnusvc: cannot publish service status\n");
    return;
  }
  dprintf(fd, "name state pid restarts\n");
  for (i = 0; i < service_count; i++)
    dprintf(fd, "%s %s %d %d\n", services[i].name,
            state_name(services[i].state), services[i].pid,
            services[i].restarts);
  close(fd);
}

static int service_index(char *name) {
  int i;
  for (i = 0; i < service_count; i++)
    if (strncmp(name, services[i].name, 32) == 0)
      return i;
  return -1;
}

static int start_service(struct service *service) {
  pid_t pid;

  if (service->pid > 0 || service->state == SERVICE_RUNNING)
    return 0;
  service->state = SERVICE_STARTING;
  pid = fork();
  if (pid < 0) {
    service->pid = -1;
    service->state = SERVICE_FAILED;
    printf("fnusvc: cannot fork %s\n", service->name);
    return -1;
  }
  if (pid == 0) {
    exec(service->path, service->argv);
    // Keep legacy development images bootable while preferring the UFS2
    // system layout used by release images.
    service->argv[0] = service->name;
    exec(service->name, service->argv);
    printf("fnusvc: exec %s failed\n", service->name);
    exit();
  }
  service->pid = pid;
  service->state = SERVICE_RUNNING;
  printf("fnusvc: started %s (pid %d)\n", service->name, pid);
  return 0;
}

static void stop_service(struct service *service, int restart) {
  if (service->pid <= 0) {
    if (restart) {
      service->restarts = 0;
      start_service(service);
    } else {
      service->state = SERVICE_STOPPED;
    }
    return;
  }
  service->restart_pending = restart;
  service->state = restart ? SERVICE_STARTING : SERVICE_STOPPED;
  if (kill(service->pid) < 0)
    printf("fnusvc: cannot stop %s\n", service->name);
}

static void reap_service(pid_t pid) {
  int i;
  for (i = 0; i < service_count; i++) {
    struct service *service = &services[i];
    if (service->pid != pid)
      continue;

    service->pid = -1;
    if (service->state == SERVICE_STOPPED) {
      printf("fnusvc: %s stopped\n", service->name);
    } else if (service->restart_pending) {
      service->restart_pending = 0;
      service->restarts = 0;
      start_service(service);
    } else if (++service->restarts > MAX_RESTARTS) {
      service->state = SERVICE_FAILED;
      printf("fnusvc: %s exceeded restart limit\n", service->name);
    } else {
      printf("fnusvc: %s exited; restarting (%d/%d)\n", service->name,
             service->restarts, MAX_RESTARTS);
      sleep(1);
      start_service(service);
    }
    publish_status();
    return;
  }
  printf("fnusvc: reaped unmanaged child %d\n", pid);
}

static int read_command(char *command, int size) {
  int fd, count;
  fd = open(SERVICE_COMMAND_PATH, O_RDONLY);
  if (fd < 0)
    return 0;
  count = read(fd, command, size - 1);
  close(fd);
  if (count <= 0)
    return 0;
  command[count] = 0;
  return 1;
}

static void process_command(char *command) {
  char *name;
  int i, length;

  length = strlen(command);
  while (length > 0 && (command[length - 1] == '\n' || command[length - 1] == '\r'))
    command[--length] = 0;

  if (strncmp(command, "start ", 6) == 0)
    name = command + 6;
  else if (strncmp(command, "stop ", 5) == 0)
    name = command + 5;
  else if (strncmp(command, "restart ", 8) == 0)
    name = command + 8;
  else {
    printf("fnusvc: rejected command\n");
    return;
  }

  i = service_index(name);
  if (i < 0) {
    printf("fnusvc: unknown service %s\n", name);
    return;
  }
  if (strncmp(command, "start ", 6) == 0) {
    services[i].restarts = 0;
    start_service(&services[i]);
  } else if (strncmp(command, "stop ", 5) == 0) {
    stop_service(&services[i], 0);
  } else {
    stop_service(&services[i], 1);
  }
  publish_status();
}

int main(void) {
  char command[64];
  int i;
  pid_t pid;

  printf("%s service supervisor\n", FNU_PRODUCT_NAME);
  for (i = 0; i < service_count; i++)
    start_service(&services[i]);
  publish_status();

  for (;;) {
    while ((pid = waitpid(-1, WNOHANG)) > 0)
      reap_service(pid);
    if (read_command(command, sizeof(command)))
      process_command(command);
    sleep(1);
  }
}
