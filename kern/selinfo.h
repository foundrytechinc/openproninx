#ifndef PRONINX_SELINFO_H
#define PRONINX_SELINFO_H

// who waits in select for an object: one thread, or a collision
struct proc;
struct selinfo {
  struct proc *si_proc;
  int si_coll;
};

void selrecord(struct selinfo *sip);
void selwakeup(struct selinfo *sip);

#endif
