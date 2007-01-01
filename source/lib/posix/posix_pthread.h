#if OS_WIN
# include "lib/sysdep/win/wposix/wpthread.h"
#else
# include <pthread.h>
# include <semaphore.h>
#endif

#include "posix_errno.h"	// for user convenience
