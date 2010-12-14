/*
 * tclUnixSock.c --
 *
 *	This file contains Unix-specific socket related code.
 *
 * Copyright (c) 1995 Sun Microsystems, Inc.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id$
 */

#include "tclInt.h"

/*
 * Helper macros to make parts of this file clearer. The macros do exactly
 * what they say on the tin. :-) They also only ever refer to their arguments
 * once, and so can be used without regard to side effects.
 */

#define SET_BITS(var, bits)	((var) |= (bits))
#define CLEAR_BITS(var, bits)	((var) &= ~(bits))

/*
 * This is needed to comply with the strict aliasing rules of GCC, but it also
 * simplifies casting between the different sockaddr types.
 */

typedef union {
    struct sockaddr sa;
    struct sockaddr_in sa4;
    struct sockaddr_in6 sa6;
    struct sockaddr_storage sas;
} address;

/*
 * This structure describes per-instance state of a tcp based channel.
 */

typedef struct TcpState TcpState;

typedef struct TcpFdList {
    TcpState *statePtr;
    int fd;
    struct TcpFdList *next;
} TcpFdList;

struct TcpState {
    Tcl_Channel channel;	/* Channel associated with this file. */
    TcpFdList *fds;		/* The file descriptors of the sockets. */
    int flags;			/* ORed combination of the bitfields defined
				 * below. */
    Tcl_TcpAcceptProc *acceptProc;
				/* Proc to call on accept. */
    ClientData acceptProcData;	/* The data for the accept proc. */
};

/*
 * These bits may be ORed together into the "flags" field of a TcpState
 * structure.
 */

#define TCP_ASYNC_SOCKET	(1<<0)	/* Asynchronous socket. */
#define TCP_ASYNC_CONNECT	(1<<1)	/* Async connect in progress. */

/*
 * The following defines the maximum length of the listen queue. This is the
 * number of outstanding yet-to-be-serviced requests for a connection on a
 * server socket, more than this number of outstanding requests and the
 * connection request will fail.
 */

#ifndef SOMAXCONN
#   define SOMAXCONN	100
#endif /* SOMAXCONN */

#if (SOMAXCONN < 100)
#   undef  SOMAXCONN
#   define SOMAXCONN	100
#endif /* SOMAXCONN < 100 */

/*
 * The following defines how much buffer space the kernel should maintain for
 * a socket.
 */

#define SOCKET_BUFSIZE	4096

/*
 * Static routines for this file:
 */

static TcpState *	CreateClientSocket(Tcl_Interp *interp, int port,
			    const char *host, const char *myaddr,
			    int myport, int async);
static void		TcpAccept(ClientData data, int mask);
static int		TcpBlockModeProc(ClientData data, int mode);
static int		TcpCloseProc(ClientData instanceData,
			    Tcl_Interp *interp);
static int		TcpClose2Proc(ClientData instanceData,
			    Tcl_Interp *interp, int flags);
static int		TcpGetHandleProc(ClientData instanceData,
			    int direction, ClientData *handlePtr);
static int		TcpGetOptionProc(ClientData instanceData,
			    Tcl_Interp *interp, const char *optionName,
			    Tcl_DString *dsPtr);
static int		TcpInputProc(ClientData instanceData, char *buf,
			    int toRead, int *errorCode);
static int		TcpOutputProc(ClientData instanceData,
			    const char *buf, int toWrite, int *errorCode);
static void		TcpWatchProc(ClientData instanceData, int mask);
static int		WaitForConnect(TcpState *statePtr, int *errorCodePtr);

/*
 * This structure describes the channel type structure for TCP socket
 * based IO:
 */

static const Tcl_ChannelType tcpChannelType = {
    "tcp",			/* Type name. */
    TCL_CHANNEL_VERSION_5,	/* v5 channel */
    TcpCloseProc,		/* Close proc. */
    TcpInputProc,		/* Input proc. */
    TcpOutputProc,		/* Output proc. */
    NULL,			/* Seek proc. */
    NULL,			/* Set option proc. */
    TcpGetOptionProc,		/* Get option proc. */
    TcpWatchProc,		/* Initialize notifier. */
    TcpGetHandleProc,		/* Get OS handles out of channel. */
    TcpClose2Proc,		/* Close2 proc. */
    TcpBlockModeProc,		/* Set blocking or non-blocking mode.*/
    NULL,			/* flush proc. */
    NULL,			/* handler proc. */
    NULL,			/* wide seek proc. */
    NULL,			/* thread action proc. */
    NULL			/* truncate proc. */
};

/*
 * The following variable holds the network name of this host.
 */

static TclInitProcessGlobalValueProc InitializeHostName;
static ProcessGlobalValue hostName =
	{0, 0, NULL, NULL, InitializeHostName, NULL, NULL};

/*
 *----------------------------------------------------------------------
 *
 * InitializeHostName --
 *
 * 	This routine sets the process global value of the name of the local
 * 	host on which the process is running.
 *
 * Results:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
InitializeHostName(
    char **valuePtr,
    int *lengthPtr,
    Tcl_Encoding *encodingPtr)
{
    const char *native = NULL;

#ifndef NO_UNAME
    struct utsname u;
    struct hostent *hp;

    memset(&u, (int) 0, sizeof(struct utsname));
    if (uname(&u) > -1) {				/* INTL: Native. */
        hp = TclpGetHostByName(u.nodename);		/* INTL: Native. */
	if (hp == NULL) {
	    /*
	     * Sometimes the nodename is fully qualified, but gets truncated
	     * as it exceeds SYS_NMLN. See if we can just get the immediate
	     * nodename and get a proper answer that way.
	     */

	    char *dot = strchr(u.nodename, '.');

	    if (dot != NULL) {
		char *node = ckalloc((unsigned) (dot - u.nodename + 1));

		memcpy(node, u.nodename, (size_t) (dot - u.nodename));
		node[dot - u.nodename] = '\0';
		hp = TclpGetHostByName(node);
		ckfree(node);
	    }
	}
        if (hp != NULL) {
	    native = hp->h_name;
        } else {
	    native = u.nodename;
        }
    }
    if (native == NULL) {
	native = tclEmptyStringRep;
    }
#else
    /*
     * Uname doesn't exist; try gethostname instead.
     *
     * There is no portable macro for the maximum length of host names
     * returned by gethostbyname(). We should only trust SYS_NMLN if it is at
     * least 255 + 1 bytes to comply with DNS host name limits.
     *
     * Note: SYS_NMLN is a restriction on "uname" not on gethostbyname!
     *
     * For example HP-UX 10.20 has SYS_NMLN == 9, while gethostbyname() can
     * return a fully qualified name from DNS of up to 255 bytes.
     *
     * Fix suggested by Viktor Dukhovni (viktor@esm.com)
     */

#    if defined(SYS_NMLN) && (SYS_NMLEN >= 256)
    char buffer[SYS_NMLEN];
#    else
    char buffer[256];
#    endif

    if (gethostname(buffer, sizeof(buffer)) > -1) {	/* INTL: Native. */
	native = buffer;
    }
#endif

    *encodingPtr = Tcl_GetEncoding(NULL, NULL);
    *lengthPtr = strlen(native);
    *valuePtr = ckalloc((unsigned) (*lengthPtr) + 1);
    memcpy(*valuePtr, native, (size_t)(*lengthPtr)+1);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetHostName --
 *
 *	Returns the name of the local host.
 *
 * Results:
 *	A string containing the network name for this machine, or an empty
 *	string if we can't figure out the name. The caller must not modify or
 *	free this string.
 *
 * Side effects:
 *	Caches the name to return for future calls.
 *
 *----------------------------------------------------------------------
 */

const char *
Tcl_GetHostName(void)
{
    return Tcl_GetString(TclGetProcessGlobalValue(&hostName));
}

/*
 *----------------------------------------------------------------------
 *
 * TclpHasSockets --
 *
 *	Detect if sockets are available on this platform.
 *
 * Results:
 *	Returns TCL_OK.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
TclpHasSockets(
    Tcl_Interp *interp)		/* Not used. */
{
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclpFinalizeSockets --
 *
 *	Performs per-thread socket subsystem finalization.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
TclpFinalizeSockets(void)
{
    return;
}

/*
 *----------------------------------------------------------------------
 *
 * TcpBlockModeProc --
 *
 *	This function is invoked by the generic IO level to set blocking and
 *	nonblocking mode on a TCP socket based channel.
 *
 * Results:
 *	0 if successful, errno when failed.
 *
 * Side effects:
 *	Sets the device into blocking or nonblocking mode.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
static int
TcpBlockModeProc(
    ClientData instanceData,	/* Socket state. */
    int mode)			/* The mode to set. Can be one of
				 * TCL_MODE_BLOCKING or
				 * TCL_MODE_NONBLOCKING. */
{
    TcpState *statePtr = (TcpState *) instanceData;

    if (mode == TCL_MODE_BLOCKING) {
	CLEAR_BITS(statePtr->flags, TCP_ASYNC_SOCKET);
    } else {
	SET_BITS(statePtr->flags, TCP_ASYNC_SOCKET);
    }
    if (TclUnixSetBlockingMode(statePtr->fds->fd, mode) < 0) {
	return errno;
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * WaitForConnect --
 *
 *	Wait for a connection on an asynchronously opened socket to be
 *	completed.  In nonblocking mode, just test if the connection
 *	has completed without blocking.
 *
 * Results:
 * 	0 if the connection has completed, -1 if still in progress
 * 	or there is an error.
 *
 *----------------------------------------------------------------------
 */

static int
WaitForConnect(
    TcpState *statePtr,		/* State of the socket. */
    int *errorCodePtr)		/* Where to store errors? */
{
    int timeOut;		/* How long to wait. */
    int state;			/* Of calling TclWaitForFile. */

    /*
     * If an asynchronous connect is in progress, attempt to wait for it to
     * complete before reading.
     */

    if (statePtr->flags & TCP_ASYNC_CONNECT) {
	if (statePtr->flags & TCP_ASYNC_SOCKET) {
	    timeOut = 0;
	} else {
	    timeOut = -1;
	}
	errno = 0;
	state = TclUnixWaitForFile(statePtr->fds->fd,
		TCL_WRITABLE | TCL_EXCEPTION, timeOut);
	if (state & TCL_EXCEPTION) {
	    return -1;
	}
	if (state & TCL_WRITABLE) {
	    CLEAR_BITS(statePtr->flags, TCP_ASYNC_CONNECT);
	} else if (timeOut == 0) {
	    *errorCodePtr = errno = EWOULDBLOCK;
	    return -1;
	}
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * TcpInputProc --
 *
 *	This function is invoked by the generic IO level to read input from a
 *	TCP socket based channel.
 *
 *	NOTE: We cannot share code with FilePipeInputProc because here we must
 *	use recv to obtain the input from the channel, not read.
 *
 * Results:
 *	The number of bytes read is returned or -1 on error. An output
 *	argument contains the POSIX error code on error, or zero if no error
 *	occurred.
 *
 * Side effects:
 *	Reads input from the input device of the channel.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
static int
TcpInputProc(
    ClientData instanceData,	/* Socket state. */
    char *buf,			/* Where to store data read. */
    int bufSize,		/* How much space is available in the
				 * buffer? */
    int *errorCodePtr)		/* Where to store error code. */
{
    TcpState *statePtr = (TcpState *) instanceData;
    int bytesRead;

    *errorCodePtr = 0;
    if (WaitForConnect(statePtr, errorCodePtr) != 0) {
	return -1;
    }
    bytesRead = recv(statePtr->fds->fd, buf, (size_t) bufSize, 0);
    if (bytesRead > -1) {
	return bytesRead;
    }
    if (errno == ECONNRESET) {
	/*
	 * Turn ECONNRESET into a soft EOF condition.
	 */

	return 0;
    }
    *errorCodePtr = errno;
    return -1;
}

/*
 *----------------------------------------------------------------------
 *
 * TcpOutputProc --
 *
 *	This function is invoked by the generic IO level to write output to a
 *	TCP socket based channel.
 *
 *	NOTE: We cannot share code with FilePipeOutputProc because here we
 *	must use send, not write, to get reliable error reporting.
 *
 * Results:
 *	The number of bytes written is returned. An output argument is set to
 *	a POSIX error code if an error occurred, or zero.
 *
 * Side effects:
 *	Writes output on the output device of the channel.
 *
 *----------------------------------------------------------------------
 */

static int
TcpOutputProc(
    ClientData instanceData,	/* Socket state. */
    const char *buf,		/* The data buffer. */
    int toWrite,		/* How many bytes to write? */
    int *errorCodePtr)		/* Where to store error code. */
{
    TcpState *statePtr = (TcpState *) instanceData;
    int written;

    *errorCodePtr = 0;
    if (WaitForConnect(statePtr, errorCodePtr) != 0) {
	return -1;
    }
    written = send(statePtr->fds->fd, buf, (size_t) toWrite, 0);
    if (written > -1) {
	return written;
    }
    *errorCodePtr = errno;
    return -1;
}

/*
 *----------------------------------------------------------------------
 *
 * TcpCloseProc --
 *
 *	This function is invoked by the generic IO level to perform
 *	channel-type-specific cleanup when a TCP socket based channel is
 *	closed.
 *
 * Results:
 *	0 if successful, the value of errno if failed.
 *
 * Side effects:
 *	Closes the socket of the channel.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
static int
TcpCloseProc(
    ClientData instanceData,	/* The socket to close. */
    Tcl_Interp *interp)		/* For error reporting - unused. */
{
    TcpState *statePtr = (TcpState *) instanceData;
    int errorCode = 0;
    TcpFdList *fds;

    /*
     * Delete a file handler that may be active for this socket if this is a
     * server socket - the file handler was created automatically by Tcl as
     * part of the mechanism to accept new client connections. Channel
     * handlers are already deleted in the generic IO channel closing code
     * that called this function, so we do not have to delete them here.
     */
    
    for (fds = statePtr->fds; fds != NULL; fds = statePtr->fds) {
	statePtr->fds = fds->next;
	Tcl_DeleteFileHandler(fds->fd);
	if (close(fds->fd) < 0) {
	    errorCode = errno;
	}
	ckfree((char *) fds);
    }
    ckfree((char *) statePtr);
    return errorCode;
}

/*
 *----------------------------------------------------------------------
 *
 * TcpClose2Proc --
 *
 *	This function is called by the generic IO level to perform the channel
 *	type specific part of a half-close: namely, a shutdown() on a socket.
 *
 * Results:
 *	0 if successful, the value of errno if failed.
 *
 * Side effects:
 *	Shuts down one side of the socket.
 *
 *----------------------------------------------------------------------
 */

static int
TcpClose2Proc(
    ClientData instanceData,	/* The socket to close. */
    Tcl_Interp *interp,		/* For error reporting. */
    int flags)			/* Flags that indicate which side to close. */
{
    TcpState *statePtr = (TcpState *) instanceData;
    int errorCode = 0;
    int sd;

    /*
     * Shutdown the OS socket handle.
     */

    switch(flags) {
    case TCL_CLOSE_READ:
        sd = SHUT_RD;
        break;
    case TCL_CLOSE_WRITE:
        sd = SHUT_WR;
        break;
    default:
        if (interp) {
            Tcl_AppendResult(interp,
                    "Socket close2proc called bidirectionally", NULL);
        }
        return TCL_ERROR;
    }
    if (shutdown(statePtr->fds->fd,sd) < 0) {
	errorCode = errno;
    }

    return errorCode;
}

/*
 *----------------------------------------------------------------------
 *
 * TcpGetOptionProc --
 *
 *	Computes an option value for a TCP socket based channel, or a list of
 *	all options and their values.
 *
 *	Note: This code is based on code contributed by John Haxby.
 *
 * Results:
 *	A standard Tcl result. The value of the specified option or a list of
 *	all options and their values is returned in the supplied DString. Sets
 *	Error message if needed.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
TcpGetOptionProc(
    ClientData instanceData,	/* Socket state. */
    Tcl_Interp *interp,		/* For error reporting - can be NULL. */
    const char *optionName,	/* Name of the option to retrieve the value
				 * for, or NULL to get all options and their
				 * values. */
    Tcl_DString *dsPtr)		/* Where to store the computed value;
				 * initialized by caller. */
{
    TcpState *statePtr = (TcpState *) instanceData;
    char host[NI_MAXHOST], port[NI_MAXSERV];
    size_t len = 0;
    int reverseDNS = 0;
#define SUPPRESS_RDNS_VAR "::tcl::unsupported::noReverseDNS"

    if (optionName != NULL) {
	len = strlen(optionName);
    }

    if ((len > 1) && (optionName[1] == 'e') &&
	    (strncmp(optionName, "-error", len) == 0)) {
	socklen_t optlen = sizeof(int);
	int err, ret;

	ret = getsockopt(statePtr->fds->fd, SOL_SOCKET, SO_ERROR,
		(char *)&err, &optlen);
	if (ret < 0) {
	    err = errno;
	}
	if (err != 0) {
	    Tcl_DStringAppend(dsPtr, Tcl_ErrnoMsg(err), -1);
	}
	return TCL_OK;
    }

    if (interp != NULL && Tcl_GetVar(interp, SUPPRESS_RDNS_VAR, 0) != NULL) {
        reverseDNS = NI_NUMERICHOST;
    }
    
    if ((len == 0) ||
	    ((len > 1) && (optionName[1] == 'p') &&
		    (strncmp(optionName, "-peername", len) == 0))) {
        address peername;
        socklen_t size = sizeof(peername);

	if (getpeername(statePtr->fds->fd, &peername.sa, &size) >= 0) {
	    if (len == 0) {
		Tcl_DStringAppendElement(dsPtr, "-peername");
		Tcl_DStringStartSublist(dsPtr);
	    }
            
	    getnameinfo(&peername.sa, size, host, sizeof(host), NULL, 0,
                    NI_NUMERICHOST);
	    Tcl_DStringAppendElement(dsPtr, host);
	    getnameinfo(&peername.sa, size, host, sizeof(host), port,
                    sizeof(port), reverseDNS | NI_NUMERICSERV);
	    Tcl_DStringAppendElement(dsPtr, host);
	    Tcl_DStringAppendElement(dsPtr, port);
	    if (len) {
                return TCL_OK;
            }
            Tcl_DStringEndSublist(dsPtr);
	} else {
	    /*
	     * getpeername failed - but if we were asked for all the options
	     * (len==0), don't flag an error at that point because it could be
	     * an fconfigure request on a server socket (which have no peer).
	     * Same must be done on win&mac.
	     */

	    if (len) {
		if (interp) {
		    Tcl_AppendResult(interp, "can't get peername: ",
			    Tcl_PosixError(interp), NULL);
		}
		return TCL_ERROR;
	    }
	}
    }

    if ((len == 0) ||
	    ((len > 1) && (optionName[1] == 's') &&
	    (strncmp(optionName, "-sockname", len) == 0))) {
	TcpFdList *fds;
        address sockname;
        socklen_t size;
	int found = 0;

	if (len == 0) {
	    Tcl_DStringAppendElement(dsPtr, "-sockname");
	    Tcl_DStringStartSublist(dsPtr);
	}
	for (fds = statePtr->fds; fds != NULL; fds = fds->next) {
	    size = sizeof(sockname);
	    if (getsockname(fds->fd, &(sockname.sa), &size) >= 0) {
                int flags = reverseDNS;

		found = 1;
                getnameinfo(&sockname.sa, size, host, sizeof(host), NULL, 0,
                        NI_NUMERICHOST);
                Tcl_DStringAppendElement(dsPtr, host);

                /*
                 * We don't want to resolve INADDR_ANY and sin6addr_any; they
                 * can sometimes cause problems (and never have a name).
                 */

                flags |= NI_NUMERICSERV;
                if (sockname.sa.sa_family == AF_INET) {
                    if (sockname.sa4.sin_addr.s_addr == INADDR_ANY) {
                        flags |= NI_NUMERICHOST;
                    }
#ifndef NEED_FAKE_RFC2553
                } else if (sockname.sa.sa_family == AF_INET6) {
                    if ((IN6_ARE_ADDR_EQUAL(&sockname.sa6.sin6_addr,
                                &in6addr_any))
                        || (IN6_IS_ADDR_V4MAPPED(&sockname.sa6.sin6_addr) &&
                            sockname.sa6.sin6_addr.s6_addr[12] == 0 &&
                            sockname.sa6.sin6_addr.s6_addr[13] == 0 &&
                            sockname.sa6.sin6_addr.s6_addr[14] == 0 &&
                            sockname.sa6.sin6_addr.s6_addr[15] == 0)) {
                        flags |= NI_NUMERICHOST;
                    }
#endif
                }
		getnameinfo(&sockname.sa, size, host, sizeof(host), port,
                        sizeof(port), flags);
		Tcl_DStringAppendElement(dsPtr, host);
		Tcl_DStringAppendElement(dsPtr, port);
	    }
	}
        if (found) {
            if (len) {
                return TCL_OK;
            }
            Tcl_DStringEndSublist(dsPtr);
        } else {
            if (interp) {
                Tcl_AppendResult(interp, "can't get sockname: ",
                        Tcl_PosixError(interp), NULL);
            }
	    return TCL_ERROR;
	}
    }

    if (len > 0) {
	return Tcl_BadChannelOption(interp, optionName, "peername sockname");
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TcpWatchProc --
 *
 *	Initialize the notifier to watch the fd from this channel.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Sets up the notifier so that a future event on the channel will be
 *	seen by Tcl.
 *
 *----------------------------------------------------------------------
 */

static void
TcpWatchProc(
    ClientData instanceData,	/* The socket state. */
    int mask)			/* Events of interest; an OR-ed combination of
				 * TCL_READABLE, TCL_WRITABLE and
				 * TCL_EXCEPTION. */
{
    TcpState *statePtr = (TcpState *) instanceData;

    /*
     * Make sure we don't mess with server sockets since they will never be
     * readable or writable at the Tcl level. This keeps Tcl scripts from
     * interfering with the -accept behavior.
     */

    if (!statePtr->acceptProc) {
	TcpFdList *fds;

	for (fds = statePtr->fds; fds != NULL; fds = fds->next) {
	    if (mask) {
		Tcl_CreateFileHandler(fds->fd, mask,
			(Tcl_FileProc *) Tcl_NotifyChannel,
			(ClientData) statePtr->channel);
	    } else {
		Tcl_DeleteFileHandler(fds->fd);
	    }
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TcpGetHandleProc --
 *
 *	Called from Tcl_GetChannelHandle to retrieve OS handles from inside a
 *	TCP socket based channel.
 *
 * Results:
 *	Returns TCL_OK with the fd in handlePtr, or TCL_ERROR if there is no
 *	handle for the specified direction.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
static int
TcpGetHandleProc(
    ClientData instanceData,	/* The socket state. */
    int direction,		/* Not used. */
    ClientData *handlePtr)	/* Where to store the handle. */
{
    TcpState *statePtr = (TcpState *) instanceData;

    *handlePtr = INT2PTR(statePtr->fds->fd);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * CreateSocket --
 *
 *	This function opens a new socket in client or server mode and
 *	initializes the TcpState structure.
 *
 * Results:
 *	Returns a new TcpState, or NULL with an error in the interp's result,
 *	if interp is not NULL.
 *
 * Side effects:
 *	Opens a socket.
 *
 *----------------------------------------------------------------------
 */

static TcpState *
CreateClientSocket(
    Tcl_Interp *interp,		/* For error reporting; can be NULL. */
    int port,			/* Port number to open. */
    const char *host,		/* Name of host on which to open port. */
    const char *myaddr,		/* Optional client-side address.
                                 * NULL implies INADDR_ANY/in6addr_any */
    int myport,			/* Optional client-side port */
    int async)			/* If nonzero and creating a client socket,
				 * attempt to do an async connect. Otherwise
				 * do a synchronous connect or bind. */
{
    int status = -1, connected = 0, sock = -1;
    struct addrinfo *addrlist = NULL, *addrPtr;
                                /* Socket address */
    struct addrinfo *myaddrlist = NULL, *myaddrPtr;
                                /* Socket address for client */
    TcpState *statePtr;
    const char *errorMsg = NULL;

    if (!TclCreateSocketAddress(interp, &addrlist, host, port, 0, &errorMsg)) {
	goto error;
    }
    if (!TclCreateSocketAddress(interp, &myaddrlist, myaddr, myport, 1, &errorMsg)) {
	goto error;
    }

    for (addrPtr = addrlist; addrPtr != NULL;
         addrPtr = addrPtr->ai_next) {
        for (myaddrPtr = myaddrlist; myaddrPtr != NULL;
             myaddrPtr = myaddrPtr->ai_next) {
            int reuseaddr;
            
	    /*
	     * No need to try combinations of local and remote addresses of
	     * different families.
	     */

	    if (myaddrPtr->ai_family != addrPtr->ai_family) {
		continue;
	    }

	    sock = socket(addrPtr->ai_family, SOCK_STREAM, 0);
	    if (sock < 0) {
		continue;
	    }

	    /*
	     * Set the close-on-exec flag so that the socket will not get
	     * inherited by child processes.
	     */
	    
	    fcntl(sock, F_SETFD, FD_CLOEXEC);
	    
	    /*
	     * Set kernel space buffering
	     */
	    
	    TclSockMinimumBuffers(INT2PTR(sock), SOCKET_BUFSIZE);
    
	    if (async) {
		status = TclUnixSetBlockingMode(sock, TCL_MODE_NONBLOCKING);
		if (status < 0) {
		    goto looperror;
		}
	    }

            reuseaddr = 1;
            (void) setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                    (char *) &reuseaddr, sizeof(reuseaddr));
            status = bind(sock, myaddrPtr->ai_addr, myaddrPtr->ai_addrlen);
            if (status < 0) {
                goto looperror;
            }

	    /*
	     * Attempt to connect. The connect may fail at present with an
	     * EINPROGRESS but at a later time it will complete. The caller
	     * will set up a file handler on the socket if she is interested
	     * in being informed when the connect completes.
	     */
	    
	    status = connect(sock, addrPtr->ai_addr, addrPtr->ai_addrlen);
	    if (status < 0 && errno == EINPROGRESS) {
		status = 0;
	    }
	    if (status == 0) {
		connected = 1;
		break;
	    }
	looperror:
	    if (sock != -1) {
		close(sock);
		sock = -1;
	    }
	}
	if (connected) {
	    break;
	} 
	status = -1;
	if (sock >= 0) {
	    close(sock);
	    sock = -1;
	}
    }
    if (async) {
	/*
	 * Restore blocking mode.
	 */

	status = TclUnixSetBlockingMode(sock, TCL_MODE_BLOCKING);
    }

error:
    if (addrlist) {
	freeaddrinfo(addrlist);
    }
    if (myaddrlist) {
	freeaddrinfo(myaddrlist);
    }
    
    if (status < 0) {
	if (interp != NULL) {
	    Tcl_AppendResult(interp, "couldn't open socket: ",
		    Tcl_PosixError(interp), NULL);
	    if (errorMsg != NULL) {
		Tcl_AppendResult(interp, " (", errorMsg, ")", NULL);
	    }
	}
	if (sock != -1) {
	    close(sock);
	}
	return NULL;
    }

    /*
     * Allocate a new TcpState for this socket.
     */

    statePtr = (TcpState *) ckalloc((unsigned) sizeof(TcpState));
    statePtr->flags = async ? TCP_ASYNC_CONNECT : 0;
    statePtr->fds = (TcpFdList *) ckalloc((unsigned) sizeof(TcpFdList));
    memset(statePtr->fds, (int) 0, sizeof(TcpFdList));
    statePtr->fds->fd = sock;

    return statePtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_OpenTcpClient --
 *
 *	Opens a TCP client socket and creates a channel around it.
 *
 * Results:
 *	The channel or NULL if failed. An error message is returned in the
 *	interpreter on failure.
 *
 * Side effects:
 *	Opens a client socket and creates a new channel.
 *
 *----------------------------------------------------------------------
 */

Tcl_Channel
Tcl_OpenTcpClient(
    Tcl_Interp *interp,		/* For error reporting; can be NULL. */
    int port,			/* Port number to open. */
    const char *host,		/* Host on which to open port. */
    const char *myaddr,		/* Client-side address */
    int myport,			/* Client-side port */
    int async)			/* If nonzero, attempt to do an asynchronous
				 * connect. Otherwise we do a blocking
				 * connect. */
{
    TcpState *statePtr;
    char channelName[16 + TCL_INTEGER_SPACE];

    /*
     * Create a new client socket and wrap it in a channel.
     */

    statePtr = CreateClientSocket(interp, port, host, myaddr, myport, async);
    if (statePtr == NULL) {
	return NULL;
    }

    statePtr->acceptProc = NULL;
    statePtr->acceptProcData = NULL;

    sprintf(channelName, "sock%d", statePtr->fds->fd);

    statePtr->channel = Tcl_CreateChannel(&tcpChannelType, channelName,
	    statePtr, (TCL_READABLE | TCL_WRITABLE));
    if (Tcl_SetChannelOption(interp, statePtr->channel, "-translation",
	    "auto crlf") == TCL_ERROR) {
	Tcl_Close(NULL, statePtr->channel);
	return NULL;
    }
    return statePtr->channel;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_MakeTcpClientChannel --
 *
 *	Creates a Tcl_Channel from an existing client TCP socket.
 *
 * Results:
 *	The Tcl_Channel wrapped around the preexisting TCP socket.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_Channel
Tcl_MakeTcpClientChannel(
    ClientData sock)		/* The socket to wrap up into a channel. */
{
    return TclpMakeTcpClientChannelMode(sock, (TCL_READABLE | TCL_WRITABLE));
}

/*
 *----------------------------------------------------------------------
 *
 * TclpMakeTcpClientChannelMode --
 *
 *	Creates a Tcl_Channel from an existing client TCP socket
 *	with given mode.
 *
 * Results:
 *	The Tcl_Channel wrapped around the preexisting TCP socket.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_Channel
TclpMakeTcpClientChannelMode(
    ClientData sock,		/* The socket to wrap up into a channel. */
    int mode)			/* ORed combination of TCL_READABLE and
				 * TCL_WRITABLE to indicate file mode. */
{
    TcpState *statePtr;
    char channelName[16 + TCL_INTEGER_SPACE];

    statePtr = (TcpState *) ckalloc((unsigned) sizeof(TcpState));
    statePtr->fds = (TcpFdList *) ckalloc((unsigned) sizeof(TcpFdList));
    memset(statePtr->fds, (int) 0, sizeof(TcpFdList));
    statePtr->fds->fd = PTR2INT(sock);
    statePtr->flags = 0;
    statePtr->acceptProc = NULL;
    statePtr->acceptProcData = NULL;

    sprintf(channelName, "sock%d", statePtr->fds->fd);

    statePtr->channel = Tcl_CreateChannel(&tcpChannelType, channelName,
	    statePtr, mode);
    if (Tcl_SetChannelOption(NULL, statePtr->channel, "-translation",
	    "auto crlf") == TCL_ERROR) {
	Tcl_Close(NULL, statePtr->channel);
	return NULL;
    }
    return statePtr->channel;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_OpenTcpServer --
 *
 *	Opens a TCP server socket and creates a channel around it.
 *
 * Results:
 *	The channel or NULL if failed. If an error occurred, an error message
 *	is left in the interp's result if interp is not NULL.
 *
 * Side effects:
 *	Opens a server socket and creates a new channel.
 *
 *----------------------------------------------------------------------
 */

Tcl_Channel
Tcl_OpenTcpServer(
    Tcl_Interp *interp,		/* For error reporting - may be NULL. */
    int port,			/* Port number to open. */
    const char *myHost,		/* Name of local host. */
    Tcl_TcpAcceptProc *acceptProc,
				/* Callback for accepting connections from new
				 * clients. */
    ClientData acceptProcData)	/* Data for the callback. */
{
    int status = 0, sock = -1, reuseaddr = 1, chosenport = 0;
    struct addrinfo *addrlist = NULL, *addrPtr;	/* socket address */
    TcpState *statePtr = NULL;
    char channelName[16 + TCL_INTEGER_SPACE];
    const char *errorMsg = NULL;
    TcpFdList *fds = NULL, *newfds;

    if (!TclCreateSocketAddress(interp, &addrlist, myHost, port, 1, &errorMsg)) {
	goto error;
    }

    for (addrPtr = addrlist; addrPtr != NULL; addrPtr = addrPtr->ai_next) {
	sock = socket(addrPtr->ai_family, SOCK_STREAM, 0);
	if (sock == -1) {
	    continue;
	}
	
	/*
	 * Set the close-on-exec flag so that the socket will not get
	 * inherited by child processes.
	 */
	
	fcntl(sock, F_SETFD, FD_CLOEXEC);
	
	/*
	 * Set kernel space buffering
	 */
	
	TclSockMinimumBuffers(INT2PTR(sock), SOCKET_BUFSIZE);
	
	/*
	 * Set up to reuse server addresses automatically and bind to the
	 * specified port.
	 */
	
	(void) setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, 
		(char *) &reuseaddr, sizeof(reuseaddr));
	
        /*
         * Make sure we use the same port number when opening two server
         * sockets for IPv4 and IPv6 on a random port.
         *
         * As sockaddr_in6 uses the same offset and size for the port member
         * as sockaddr_in, we can handle both through the IPv4 API.
         */

	if (port == 0 && chosenport != 0) {
	    ((struct sockaddr_in *) addrPtr->ai_addr)->sin_port =
                    htons(chosenport);
	}

#ifdef IPV6_V6ONLY
	/* Missing on: Solaris 2.8 */
        if (addrPtr->ai_family == AF_INET6) {
            int v6only = 1;

            (void) setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY,
                    &v6only, sizeof(v6only));
        }
#endif

	status = bind(sock, addrPtr->ai_addr, addrPtr->ai_addrlen);
        if (status == -1) {
            close(sock);
            continue;
        }
        if (port == 0 && chosenport == 0) {
            address sockname;
            socklen_t namelen = sizeof(sockname);

            /*
             * Synchronize port numbers when binding to port 0 of multiple
             * addresses.
             */

            if (getsockname(sock, &sockname.sa, &namelen) >= 0) {
                chosenport = ntohs(sockname.sa4.sin_port);
            }
        }
        status = listen(sock, SOMAXCONN);
        if (status < 0) {
            close(sock);
            continue;
        }
        newfds = (TcpFdList *) ckalloc((unsigned) sizeof(TcpFdList));
        memset(newfds, (int) 0, sizeof(TcpFdList));
        if (statePtr == NULL) {
            /*
             * Allocate a new TcpState for this socket.
             */
            
            statePtr = (TcpState *) ckalloc((unsigned) sizeof(TcpState));
            statePtr->fds = newfds;
            statePtr->acceptProc = acceptProc;
            statePtr->acceptProcData = acceptProcData;
            sprintf(channelName, "sock%d", sock);
        } else {
            fds->next = newfds;
        }
        newfds->fd = sock;
        newfds->statePtr = statePtr;
        fds = newfds;
	
        /*
         * Set up the callback mechanism for accepting connections from new
         * clients.
         */
        
        Tcl_CreateFileHandler(sock, TCL_READABLE, TcpAccept, fds);
    }

  error:
    if (addrlist != NULL) {
	freeaddrinfo(addrlist);
    }
    if (statePtr != NULL) {
	statePtr->channel = Tcl_CreateChannel(&tcpChannelType, channelName,
		statePtr, 0);
	return statePtr->channel;
    }
    if (interp != NULL) {
	Tcl_AppendResult(interp, "couldn't open socket: ",
		Tcl_PosixError(interp), NULL);
	if (errorMsg != NULL) {
	    Tcl_AppendResult(interp, " (", errorMsg, ")", NULL);
	}
    }
    if (sock != -1) {
	close(sock);
    }
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * TcpAccept --
 *	Accept a TCP socket connection.	 This is called by the event loop.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Creates a new connection socket. Calls the registered callback for the
 *	connection acceptance mechanism.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
static void
TcpAccept(
    ClientData data,		/* Callback token. */
    int mask)			/* Not used. */
{
    TcpFdList *fds;		/* Client data of server socket. */
    int newsock;		/* The new client socket */
    TcpState *newSockState;	/* State for new socket. */
    address addr;		/* The remote address */
    socklen_t len;		/* For accept interface */
    char channelName[16 + TCL_INTEGER_SPACE];
    char host[NI_MAXHOST], port[NI_MAXSERV];
    
    fds = (TcpFdList *) data;

    len = sizeof(addr);
    newsock = accept(fds->fd, &(addr.sa), &len);
    if (newsock < 0) {
	return;
    }

    /*
     * Set close-on-exec flag to prevent the newly accepted socket from being
     * inherited by child processes.
     */

    (void) fcntl(newsock, F_SETFD, FD_CLOEXEC);

    newSockState = (TcpState *) ckalloc((unsigned) sizeof(TcpState));

    newSockState->flags = 0;
    newSockState->fds = (TcpFdList *) ckalloc(sizeof(TcpFdList));
    memset(newSockState->fds, (int) 0, sizeof(TcpFdList));
    newSockState->fds->fd = newsock;
    newSockState->acceptProc = NULL;
    newSockState->acceptProcData = NULL;

    sprintf(channelName, "sock%d", newsock);
    newSockState->channel = Tcl_CreateChannel(&tcpChannelType, channelName,
	    newSockState, (TCL_READABLE | TCL_WRITABLE));

    Tcl_SetChannelOption(NULL, newSockState->channel, "-translation",
	    "auto crlf");

    if (fds->statePtr->acceptProc != NULL) {
	getnameinfo(&(addr.sa), len, host, sizeof(host), port, sizeof(port),
                NI_NUMERICHOST|NI_NUMERICSERV);
	fds->statePtr->acceptProc(fds->statePtr->acceptProcData,
                newSockState->channel, host, atoi(port));
    }
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 */
