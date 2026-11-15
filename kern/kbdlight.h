#ifndef PRONINX_KBDLIGHT_H
#define PRONINX_KBDLIGHT_H

// a machine driver that lights the keys
struct kbdlight {
  const char *name;
  int max; // levels 0..max
  int (*get)(void *arg);
  void (*set)(void *arg, int level);
  void *arg;
};

void kbdlight_attach(const struct kbdlight *l);
void kbdlight_changed(int level);
void kbdlight_suspend(void);
void kbdlight_resume(void);

#endif
