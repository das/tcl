/*
 * tclWinPort.h --
 *
 *	This header file handles porting issues that occur because of
 *	differences between Windows and Unix. It should be the only
 *	file that contains #ifdefs to handle different flavors of OS.
 *
 * Copyright (c) 1994-1996 Sun Microsystems, Inc.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * SCCS: @(#) tclWinPort.h 1.53 97/07/30 14:12:17
 */

#ifndef _TCLWINPORT
#define _TCLWINPORT

#include <malloc.h>
#include <stdio.h>

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <process.h>
#include <signal.h>
#include <winsock.h>
#include <sys/stat.h>
#include <sys/timeb.h>
#include <time.h>
#include <io.h>
#include <fcntl.h>
#include <float.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN

/*
 * Define EINPROGRESS in terms of WSAEINPROGRESS.
 */

#ifndef	EINPROGRESS
#define EINPROGRESS WSAEINPROGRESS
#endif

/*
 * If ENOTSUP is not defined, define it to a value that will never occur.
 */

#ifndef ENOTSUP
#define	ENOTSUP		-1030507
#endif

/*
 * The following defines wrap the system memory allocation routines for
 * use by tclAlloc.c.
 */

#define TclpSysAlloc(size, isBin)	((void*)GlobalAlloc(GMEM_FIXED, \
					    (DWORD)size))
#define TclpSysFree(ptr)		(GlobalFree((HGLOBAL)ptr))
#define TclpSysRealloc(ptr, size)	((void*)GlobalReAlloc((HGLOBAL)ptr, \
					    (DWORD)size, 0))

/*
 * The default platform eol translation on Windows is TCL_TRANSLATE_CRLF:
 */

#define	TCL_PLATFORM_TRANSLATION	TCL_TRANSLATE_CRLF

/*
 * Declare dynamic loading extension macro.
 */

#define TCL_SHLIB_EXT ".dll"

/*
 * Supply definitions for macros to query wait status, if not already
 * defined in header files above.
 */

#if TCL_UNION_WAIT
#   define WAIT_STATUS_TYPE union wait
#else
#   define WAIT_STATUS_TYPE int
#endif

#ifndef WIFEXITED
#   define WIFEXITED(stat)  (((*((int *) &(stat))) & 0xff) == 0)
#endif

#ifndef WEXITSTATUS
#   define WEXITSTATUS(stat) (((*((int *) &(stat))) >> 8) & 0xff)
#endif

#ifndef WIFSIGNALED
#   define WIFSIGNALED(stat) (((*((int *) &(stat)))) && ((*((int *) &(stat))) == ((*((int *) &(stat))) & 0x00ff)))
#endif

#ifndef WTERMSIG
#   define WTERMSIG(stat)    ((*((int *) &(stat))) & 0x7f)
#endif

#ifndef WIFSTOPPED
#   define WIFSTOPPED(stat)  (((*((int *) &(stat))) & 0xff) == 0177)
#endif

#ifndef WSTOPSIG
#   define WSTOPSIG(stat)    (((*((int *) &(stat))) >> 8) & 0xff)
#endif

/*
 * Define constants for waitpid() system call if they aren't defined
 * by a system header file.
 */

#ifndef WNOHANG
#   define WNOHANG 1
#endif
#ifndef WUNTRACED
#   define WUNTRACED 2
#endif

/*
 * Define MAXPATHLEN in terms of MAXPATH if available
 */

#ifndef MAXPATH
#define MAXPATH MAX_PATH
#endif /* MAXPATH */

#ifndef MAXPATHLEN
#define MAXPATHLEN MAXPATH
#endif /* MAXPATHLEN */

#ifndef F_OK
#    define F_OK 00
#endif
#ifndef X_OK
#    define X_OK 01
#endif
#ifndef W_OK
#    define W_OK 02
#endif
#ifndef R_OK
#    define R_OK 04
#endif

/*
 * Define macros to query file type bits, if they're not already
 * defined.
 */

#ifndef S_ISREG
#   ifdef S_IFREG
#       define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#   else
#       define S_ISREG(m) 0
#   endif
# endif
#ifndef S_ISDIR
#   ifdef S_IFDIR
#       define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#   else
#       define S_ISDIR(m) 0
#   endif
# endif
#ifndef S_ISCHR
#   ifdef S_IFCHR
#       define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#   else
#       define S_ISCHR(m) 0
#   endif
# endif
#ifndef S_ISBLK
#   ifdef S_IFBLK
#       define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#   else
#       define S_ISBLK(m) 0
#   endif
# endif
#ifndef S_ISFIFO
#   ifdef S_IFIFO
#       define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#   else
#       define S_ISFIFO(m) 0
#   endif
# endif

/*
 * Define pid_t and uid_t if they're not already defined.
 */

#if ! TCL_PID_T
#   define pid_t int
#endif
#if ! TCL_UID_T
#   define uid_t int
#endif

/*
 * Provide a stub definition for TclGetUserHome().
 */

#define TclGetUserHome(name,bufferPtr) (NULL)

/*
 * Visual C++ has some odd names for common functions, so we need to
 * define a few macros to handle them.  Also, it defines EDEADLOCK and
 * EDEADLK as the same value, which confuses Tcl_ErrnoId().
 */

#ifdef _MSC_VER
#    define environ _environ
#    define hypot _hypot
#    define exception _exception
#    undef EDEADLOCK
#endif /* _MSC_VER */

/*
 * The following defines redefine the Windows Socket errors as
 * BSD errors so Tcl_PosixError can do the right thing.
 */

#ifndef EWOULDBLOCK
#define EWOULDBLOCK             EAGAIN
#endif
#ifndef EALREADY
#define EALREADY	149	/* operation already in progress */
#endif
#ifndef ENOTSOCK
#define ENOTSOCK	95	/* Socket operation on non-socket */
#endif
#ifndef EDESTADDRREQ
#define EDESTADDRREQ	96	/* Destination address required */
#endif
#ifndef EMSGSIZE
#define EMSGSIZE	97	/* Message too long */
#endif
#ifndef EPROTOTYPE
#define EPROTOTYPE	98	/* Protocol wrong type for socket */
#endif
#ifndef ENOPROTOOPT
#define ENOPROTOOPT	99	/* Protocol not available */
#endif
#ifndef EPROTONOSUPPORT
#define EPROTONOSUPPORT	120	/* Protocol not supported */
#endif
#ifndef ESOCKTNOSUPPORT
#define ESOCKTNOSUPPORT	121	/* Socket type not supported */
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP	122	/* Operation not supported on socket */
#endif
#ifndef EPFNOSUPPORT
#define EPFNOSUPPORT	123	/* Protocol family not supported */
#endif
#ifndef EAFNOSUPPORT
#define EAFNOSUPPORT	124	/* Address family not supported */
#endif
#ifndef EADDRINUSE
#define EADDRINUSE	125	/* Address already in use */
#endif
#ifndef EADDRNOTAVAIL
#define EADDRNOTAVAIL	126	/* Can't assign requested address */
#endif
#ifndef ENETDOWN
#define ENETDOWN	127	/* Network is down */
#endif
#ifndef ENETUNREACH
#define ENETUNREACH	128	/* Network is unreachable */
#endif
#ifndef ENETRESET
#define ENETRESET	129	/* Network dropped connection on reset */
#endif
#ifndef ECONNABORTED
#define ECONNABORTED	130	/* Software caused connection abort */
#endif
#ifndef ECONNRESET
#define ECONNRESET	131	/* Connection reset by peer */
#endif
#ifndef ENOBUFS
#define ENOBUFS		132	/* No buffer space available */
#endif
#ifndef EISCONN
#define EISCONN		133	/* Socket is already connected */
#endif
#ifndef ENOTCONN
#define ENOTCONN	134	/* Socket is not connected */
#endif
#ifndef ESHUTDOWN
#define ESHUTDOWN	143	/* Can't send after socket shutdown */
#endif
#ifndef ETOOMANYREFS
#define ETOOMANYREFS	144	/* Too many references: can't splice */
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT	145	/* Connection timed out */
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED	146	/* Connection refused */
#endif
#ifndef ELOOP
#define ELOOP		90	/* Symbolic link loop */
#endif
#ifndef EHOSTDOWN
#define EHOSTDOWN	147	/* Host is down */
#endif
#ifndef EHOSTUNREACH
#define EHOSTUNREACH	148	/* No route to host */
#endif
#ifndef ENOTEMPTY
#define ENOTEMPTY 	93	/* directory not empty */
#endif
#ifndef EUSERS
#define EUSERS		94	/* Too many users (for UFS) */
#endif
#ifndef EDQUOT
#define EDQUOT		49	/* Disc quota exceeded */
#endif
#ifndef ESTALE
#define ESTALE		151	/* Stale NFS file handle */
#endif
#ifndef EREMOTE
#define EREMOTE		66	/* The object is remote */
#endif

/*
 * The following define ensures that we use the native putenv
 * implementation to modify the environment array.  This keeps
 * the C level environment in synch with the system level environment.
 */

#define USE_PUTENV	1
    
/*
 * The following defines map from standard socket names to our internal
 * wrappers that redirect through the winSock function table (see the
 * file tclWinSock.c).
 */

#define getservbyname	TclWinGetServByName
#define getsockopt	TclWinGetSockOpt
#define ntohs		TclWinNToHS
#define setsockopt	TclWinSetSockOpt

/*
 * The following implements the Windows method for exiting the process.
 */
#define TclPlatformExit(status) exit(status)


/*
 * The following declarations belong in tclInt.h, but depend on platform
 * specific types (e.g. struct tm).
 */

EXTERN struct tm *	TclpGetDate _ANSI_ARGS_((const time_t *tp,
			    int useGMT));
EXTERN unsigned long	TclpGetPid _ANSI_ARGS_((Tcl_Pid pid));
EXTERN size_t		TclStrftime _ANSI_ARGS_((char *s, size_t maxsize,
			    const char *format, const struct tm *t));

/*
 * The following prototypes and defines replace the Windows versions
 * of POSIX function that various compilier vendors didn't implement 
 * well or consistantly.
 */

#define stat(path, buf)		TclWinStat(path, buf)
#define lstat			stat
#define access(path, mode)	TclWinAccess(path, mode)

EXTERN int		TclWinStat _ANSI_ARGS_((CONST char *path, 
			    struct stat *buf));
EXTERN int		TclWinAccess _ANSI_ARGS_((CONST char *path, 
			    int mode));

#define TclpReleaseFile(file)	ckfree((char *) file)

/*
 * Declarations for Windows specific functions.
 */

EXTERN void		TclWinConvertError _ANSI_ARGS_((DWORD errCode));
EXTERN void		TclWinConvertWSAError _ANSI_ARGS_((DWORD errCode));
EXTERN struct servent * PASCAL FAR
			TclWinGetServByName _ANSI_ARGS_((const char FAR *nm,
		            const char FAR *proto));
EXTERN int PASCAL FAR	TclWinGetSockOpt _ANSI_ARGS_((SOCKET s, int level,
		            int optname, char FAR * optval, int FAR *optlen));
EXTERN HINSTANCE	TclWinGetTclInstance _ANSI_ARGS_((void));
EXTERN HINSTANCE	TclWinLoadLibrary _ANSI_ARGS_((char *name));
EXTERN u_short PASCAL FAR
			TclWinNToHS _ANSI_ARGS_((u_short ns));
EXTERN int PASCAL FAR	TclWinSetSockOpt _ANSI_ARGS_((SOCKET s, int level,
		            int optname, const char FAR * optval, int optlen));
#endif /* _TCLWINPORT */
