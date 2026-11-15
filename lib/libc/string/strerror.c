// strerror(3): one text per errno of sys/errno.h
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <limits.h>

char *sys_errlist[] = {
    "Undefined error: 0",
    "Operation not permitted",
    "No such file or directory",
    "No such process",
    "Interrupted system call",
    "Input/output error",
    "Device not configured",
    "Argument list too long",
    "Exec format error",
    "Bad file descriptor",
    "No child processes",
    "Resource deadlock avoided",
    "Cannot allocate memory",
    "Permission denied",
    "Bad address",
    "Block device required",
    "Device busy",
    "File exists",
    "Cross-device link",
    "Operation not supported by device",
    "Not a directory",
    "Is a directory",
    "Invalid argument",
    "Too many open files in system",
    "Too many open files",
    "Inappropriate ioctl for device",
    "Text file busy",
    "File too large",
    "No space left on device",
    "Illegal seek",
    "Read-only file system",
    "Too many links",
    "Broken pipe",
    "Numerical argument out of domain",
    "Result too large",
    "Resource temporarily unavailable",
    "Operation now in progress",
    "Operation already in progress",
    "Socket operation on non-socket",
    "Destination address required",
    "Message too long",
    "Protocol wrong type for socket",
    "Protocol not available",
    "Protocol not supported",
    "Socket type not supported",
    "Operation not supported",
    "Protocol family not supported",
    "Address family not supported by protocol family",
    "Address already in use",
    "Can't assign requested address",
    "Network is down",
    "Network is unreachable",
    "Network dropped connection on reset",
    "Software caused connection abort",
    "Connection reset by peer",
    "No buffer space available",
    "Socket is already connected",
    "Socket is not connected",
    "Can't send after socket shutdown",
    "Too many references: can't splice",
    "Operation timed out",
    "Connection refused",
    "Too many levels of symbolic links",
    "File name too long",
    "Host is down",
    "No route to host",
    "Directory not empty",
    "Too many processes",
    "Too many users",
    "Disk quota exceeded",
    "Stale NFS file handle",
    "Too many levels of remote in path",
    "RPC struct is bad",
    "RPC version wrong",
    "RPC program not available",
    "Program version wrong",
    "Bad procedure for program",
    "No locks available",
    "Function not implemented",
    "Inappropriate file type or format",
    "Authentication error",
    "Need authenticator",
    "IPsec processing failure",
    "Attribute not found",
    "Illegal byte sequence",
    "No medium found",
    "Wrong medium type",
    "Value too large to be stored in data type",
    "Operation canceled",
    "Identifier removed",
    "No message of desired type",
    "Not supported",
    "Bad message",
    "State not recoverable",
    "Previous owner died",
    "Protocol error",
};

int sys_nerr = (int)(sizeof(sys_errlist) / sizeof(sys_errlist[0]));

int strerror_r(int errnum, char *buf, size_t buflen) {
  if (errnum > 0 && errnum < (int)(sizeof(sys_errlist) / sizeof(sys_errlist[0]))) {
    if (strlcpy(buf, sys_errlist[errnum], buflen) >= buflen)
      return ERANGE;
    return 0;
  }
  if ((size_t)snprintf(buf, buflen, "Unknown error: %d", errnum) >= buflen)
    return ERANGE;
  return EINVAL;
}

char *strerror(int errnum) {
  static char buf[NL_TEXTMAX];

  strerror_r(errnum, buf, sizeof(buf));
  return buf;
}
