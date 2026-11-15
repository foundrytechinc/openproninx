/* Inside libcrypt: what the password routines keep to themselves. */
#ifndef _LIBCRYPT_PWD_H_
#define _LIBCRYPT_PWD_H_

#include_next <pwd.h>

int	_bcrypt_autorounds(void);

#endif /* _LIBCRYPT_PWD_H_ */
