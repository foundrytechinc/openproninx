#ifndef _SYS_REBOOT_H_
#define _SYS_REBOOT_H_

#define RB_AUTOBOOT 0
#define RB_HALT 0x0008
#define RB_POWEROFF 0x4000
#define RB_NOSYNC 0x0004

int reboot(int howto);

#endif
