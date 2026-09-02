// Volatile local IPC for the development-only supervisor. Keeping it in
// memory makes an immutable UFS2 system image usable without pretending its
// data volume is writable. This is deliberately not durable or authenticated.
#include "defs.h"
#include "file.h"
#include "spinlock.h"

#define FNU_STATE_COMMAND_SIZE 64
#define FNU_STATE_TEXT_SIZE 256

struct fnu_state {
  struct spinlock lock;
  char command[FNU_STATE_COMMAND_SIZE];
  int command_length;
  char services[FNU_STATE_TEXT_SIZE];
  int services_length;
  char health[FNU_STATE_TEXT_SIZE];
  int health_length;
};

static struct fnu_state state;

void fnustateinit(void) {
  initlock(&state.lock, "fnustate");
  devsw[FNU_STATE].read = fnustateread;
  devsw[FNU_STATE].write = fnustatewrite;
}

static int state_copy(char *source, int length, char *destination, int size) {
  int count = length < size ? length : size;
  memmove(destination, source, count);
  return count;
}

int fnustateread(struct inode *ip, char *destination, int size) {
  int count = 0;
  if (size < 0)
    return -1;
  acquire(&state.lock);
  if (ip->minor == FNU_STATE_COMMAND) {
    count = state_copy(state.command, state.command_length, destination, size);
    state.command_length = 0;
  } else if (ip->minor == FNU_STATE_SERVICES) {
    count = state_copy(state.services, state.services_length, destination, size);
  } else if (ip->minor == FNU_STATE_HEALTH) {
    count = state_copy(state.health, state.health_length, destination, size);
  }
  release(&state.lock);
  return count;
}

int fnustatewrite(struct inode *ip, char *source, int size) {
  char *destination;
  int *length;
  int capacity;
  if (size < 0)
    return -1;
  if (ip->minor == FNU_STATE_COMMAND) {
    destination = state.command;
    length = &state.command_length;
    capacity = FNU_STATE_COMMAND_SIZE;
  } else if (ip->minor == FNU_STATE_SERVICES) {
    destination = state.services;
    length = &state.services_length;
    capacity = FNU_STATE_TEXT_SIZE;
  } else if (ip->minor == FNU_STATE_HEALTH) {
    destination = state.health;
    length = &state.health_length;
    capacity = FNU_STATE_TEXT_SIZE;
  } else {
    return -1;
  }
  if (size > capacity)
    return -1;
  acquire(&state.lock);
  memmove(destination, source, size);
  *length = size;
  release(&state.lock);
  return size;
}
