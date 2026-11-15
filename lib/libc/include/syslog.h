/* Inside libc: the shared formatting behind syslog(3) and syslog_r(3). */
#ifndef _LIBC_SYSLOG_H_
#define _LIBC_SYSLOG_H_

#include_next <syslog.h>

void	__vsyslog_r(int, struct syslog_data *, int, const char *, __va_list);

#endif /* _LIBC_SYSLOG_H_ */
