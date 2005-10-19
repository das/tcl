/*
 * tclIORChan.c --
 *
 *	This file contains the implementation of Tcl's generic channel
 *	reflection code, which allows the implementation of Tcl channels in
 *	Tcl code.
 *
 *	Parts of this file are based on code contributed by Jean-Claude
 *	Wippler.
 *
 *	See TIP #219 for the specification of this functionality.
 *
 * Copyright (c) 2004-2005 ActiveState, a divison of Sophos
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id$
 */

#include <tclInt.h>
#include <tclIO.h>
#include <assert.h>

#ifndef EINVAL
#define EINVAL	9
#endif
#ifndef EOK
#define EOK	0
#endif

/*
 * Signatures of all functions used in the C layer of the reflection.
 */

static int		ReflectClose(ClientData clientData,
			    Tcl_Interp *interp);
static int		ReflectInput(ClientData clientData, char *buf,
			    int toRead, int *errorCodePtr);
static int		ReflectOutput(ClientData clientData, CONST char *buf,
			    int toWrite, int *errorCodePtr);
static void		ReflectWatch(ClientData clientData, int mask);
static int		ReflectBlock(ClientData clientData, int mode);
static Tcl_WideInt	ReflectSeekWide(ClientData clientData,
			    Tcl_WideInt offset, int mode, int *errorCodePtr);
static int		ReflectSeek(ClientData clientData, long offset,
			    int mode, int *errorCodePtr);
static int		ReflectGetOption(ClientData clientData,
			    Tcl_Interp *interp, CONST char *optionName,
			    Tcl_DString *dsPtr);
static int		ReflectSetOption(ClientData clientData,
			    Tcl_Interp *interp, CONST char *optionName,
			    CONST char *newValue);

/*
 * The C layer channel type/driver definition used by the reflection. This is
 * a version 3 structure.
 */

static Tcl_ChannelType tclRChannelType = {
    "tclrchannel",	/* Type name.					*/
    TCL_CHANNEL_VERSION_3,
    ReflectClose,	/* Close channel, clean instance data		*/
    ReflectInput,	/* Handle read request				*/
    ReflectOutput,	/* Handle write request				*/
    ReflectSeek,	/* Move location of access point.    NULL'able	*/
    ReflectSetOption,	/* Set options.			     NULL'able	*/
    ReflectGetOption,	/* Get options.			     NULL'able	*/
    ReflectWatch,	/* Initialize notifier				*/
    NULL,		/* Get OS handle from the channel.   NULL'able	*/
    NULL,		/* No close2 support.		     NULL'able	*/
    ReflectBlock,	/* Set blocking/nonblocking.	     NULL'able	*/
    NULL,		/* Flush channel. Not used by core.  NULL'able	*/
    NULL,		/* Handle events.		     NULL'able	*/
    ReflectSeekWide	/* Move access point (64 bit).	     NULL'able	*/
};

/*
 * Instance data for a reflected channel. ===========================
 */

typedef struct {
    Tcl_Channel chan;		/* Back reference to generic channel
				 * structure. */
    Tcl_Interp *interp;		/* Reference to the interpreter containing the
				 * Tcl level part of the channel. */
#ifdef TCL_THREADS
    Tcl_ThreadId thread;	/* Thread the 'interp' belongs to. */
#endif

    /* See [==] as well.
     * Storage for the command prefix and the additional words required for
     * the invocation of methods in the command handler.
     *
     * argv [0] ... [.] | [argc-2] [argc-1] | [argc]  [argc+2]
     *      cmd ... pfx | method   chan     | detail1 detail2
     *      ~~~~ CT ~~~            ~~ CT ~~
     *
     * CT = Belongs to the 'Command handler Thread'.
     */

    int argc;			/* Number of preallocated words - 2 */
    Tcl_Obj **argv;		/* Preallocated array for calling the handler.
				 * args[0] is placeholder for cmd word.
				 * Followed by the arguments in the prefix,
				 * plus 4 placeholders for method, channel,
				 * and at most two varying (method specific)
				 * words. */
    int methods;		/* Bitmask of supported methods */

    /*
     * NOTE (9): Should we have predefined shared literals for the method
     * names?
     */

    int mode;			/* Mask of R/W mode */
    int interest;		/* Mask of events the channel is interested
				 * in. */

    /*
     * Note regarding the usage of timers.
     *
     * Most channel implementations need a timer in the C level to ensure that
     * data in buffers is flushed out through the generation of fake file
     * events.
     *
     * See 'rechan', 'memchan', etc.
     *
     * Here this is _not_ required. Interest in events is posted to the Tcl
     * level via 'watch'. And posting of events is possible from the Tcl level
     * as well, via 'chan postevent'. This means that the generation of all
     * events, fake or not, timer based or not, is completely in the hands of
     * the Tcl level. Therefore no timer here.
     */
} ReflectedChannel;

/*
 * Event literals. ==================================================
 */

static CONST char *eventOptions[] = {
    "read", "write", NULL
};
typedef enum {
    EVENT_READ, EVENT_WRITE
} EventOption;

/*
 * Method literals. ==================================================
 */

static CONST char *methodNames[] = {
    "blocking",		/* OPT */
    "cget",		/* OPT \/ Together or none */
    "cgetall",		/* OPT /\ of these two     */
    "configure",	/* OPT */
    "finalize",		/*     */
    "initialize",	/*     */
    "read",		/* OPT */
    "seek",		/* OPT */
    "watch",		/*     */
    "write",		/* OPT */
    NULL
};
typedef enum {
    METH_BLOCKING,
    METH_CGET,
    METH_CGETALL,
    METH_CONFIGURE,
    METH_FINAL,
    METH_INIT,
    METH_READ,
    METH_SEEK,
    METH_WATCH,
    METH_WRITE,
} MethodName;

#define FLAG(m) (1 << (m))
#define REQUIRED_METHODS \
	(FLAG(METH_INIT) | FLAG(METH_FINAL) | FLAG(METH_WATCH))
#define NULLABLE_METHODS \
	(FLAG(METH_BLOCKING) | FLAG(METH_SEEK) | \
	FLAG(METH_CONFIGURE) | FLAG(METH_CGET) | FLAG(METH_CGETALL))

#define RANDW \
	(TCL_READABLE | TCL_WRITABLE)

#define IMPLIES(a,b)	((!(a)) || (b))
#define NEGIMPL(a,b)
#define HAS(x,f)	(x & FLAG(f))

#ifdef TCL_THREADS
/*
 * Thread specific types and structures.
 *
 * We are here essentially creating a very specific implementation of 'thread
 * send'.
 */

/*
 * Enumeration of all operations which can be forwarded.
 */

typedef enum {
    ForwardedClose,
    ForwardedInput,
    ForwardedOutput,
    ForwardedSeek,
    ForwardedWatch,
    ForwardedBlock,
    ForwardedSetOpt,
    ForwardedGetOpt,
    ForwardedGetOptAll
} ForwardedOperation;

/*
 * Event used to forward driver invocations to the thread actually managing
 * the channel. We cannot construct the command to execute and forward
 * that. Because then it will contain a mixture of Tcl_Obj's belonging to both
 * the command handler thread (CT), and the thread managing the channel (MT),
 * executed in CT. Tcl_Obj's are not allowed to cross thread boundaries. So we
 * forward an operation code, the argument details, and reference to results.
 * The command is assembled in the CT and belongs fully to that thread. No
 * sharing problems.
 */

typedef struct ForwardParamBase {
    int code;			/* O: Ok/Fail of the cmd handler */
    char *msgStr;		/* O: Error message for handler failure */
    int mustFree;		/* O: True if msgStr is allocated, false if
				 * otherwise (static). */
} ForwardParamBase;

/*
 * Operation specific parameter/result structures. (These are "subtypes" of
 * ForwardParamBase. Where an operation does not need any special types, it
 * has no "subtype" and just uses ForwardParamBase, as listed above.)
 */

struct ForwardParamInput {
    ForwardParamBase base;	/* "Supertype". MUST COME FIRST. */
    char *buf;			/* O: Where to store the read bytes */
    int toRead;			/* I: #bytes to read,
				 * O: #bytes actually read */
};
struct ForwardParamOutput {
    ForwardParamBase base;	/* "Supertype". MUST COME FIRST. */
    CONST char *buf;		/* I: Where the bytes to write come from */
    int toWrite;		/* I: #bytes to write,
				 * O: #bytes actually written */
};
struct ForwardParamSeek {
    ForwardParamBase base;	/* "Supertype". MUST COME FIRST. */
    int seekMode;		/* I: How to seek */
    Tcl_WideInt offset;		/* I: Where to seek,
				 * O: New location */
};
struct ForwardParamWatch {
    ForwardParamBase base;	/* "Supertype". MUST COME FIRST. */
    int mask;			/* I: What events to watch for */
};
struct ForwardParamBlock {
    ForwardParamBase base;	/* "Supertype". MUST COME FIRST. */
    int nonblocking;		/* I: What mode to activate */
};
struct ForwardParamSetOpt {
    ForwardParamBase base;	/* "Supertype". MUST COME FIRST. */
    CONST char *name;		/* Name of option to set */
    CONST char *value;		/* Value to set */
};
struct ForwardParamGetOpt {
    ForwardParamBase base;	/* "Supertype". MUST COME FIRST. */
    CONST char *name;		/* Name of option to get, maybe NULL */
    Tcl_DString *value;		/* Result */
};

/*
 * Now join all these together in a single union for convenience.
 */

typedef union ForwardParam {
    ForwardParamBase base;
    struct ForwardParamInput input;
    struct ForwardParamOutput output;
    struct ForwardParamSeek seek;
    struct ForwardParamWatch watch;
    struct ForwardParamBlock block;
    struct ForwardParamSetOpt setOpt;
    struct ForwardParamGetOpt getOpt;
} ForwardParam;

/*
 * Forward declaration.
 */

typedef struct ForwardingResult ForwardingResult;

/*
 * General event structure, with reference to operation specific data.
 */

typedef struct ForwardingEvent {
    Tcl_Event event;		/* Basic event data, has to be first item */
    ForwardingResult *resultPtr;
    ForwardedOperation op;	/* Forwarded driver operation */
    ReflectedChannel *rcPtr;	/* Channel instance */
    ForwardParam *param;	/* Packaged arguments and return values, a
				 * ForwardParam pointer. */
} ForwardingEvent;

/*
 * Structure to manage the result of the forwarding. This is not the result of
 * the operation itself, but about the success of the forward event itself.
 * The event can be successful, even if the operation which was forwarded
 * failed. It is also there to manage the synchronization between the involved
 * threads.
 */

struct ForwardingResult {
    Tcl_ThreadId src;		/* Originating thread. */
    Tcl_ThreadId dst;		/* Thread the op was forwarded to. */
    Tcl_Condition done;		/* Condition variable the forwarder blocks
				 * on. */
    int result;			/* TCL_OK or TCL_ERROR */
    ForwardingEvent *evPtr;	/* Event the result belongs to. */
    ForwardingResult *prevPtr, *nextPtr;
				/* Links into the list of pending forwarded
				 * results. */
};

/*
 * List of forwarded operations which have not completed yet, plus the mutex
 * to protect the access to this process global list.
 */

static ForwardingResult *forwardList = NULL;
TCL_DECLARE_MUTEX(rcForwardMutex)

/*
 * Function containing the generic code executing a forward, and wrapper
 * macros for the actual operations we wish to forward. Uses ForwardProc as
 * the event function executed by the thread receiving a forwarding event
 * (which executes the appropriate function and collects the result, if any).
 *
 * The two ExitProcs are handlers so that things do not deadlock when either
 * thread involved in the forwarding exits. They also clean things up so that
 * we don't leak resources when threads go away.
 */

static void		ForwardOpToOwnerThread(ReflectedChannel *rcPtr,
			    ForwardedOperation op, CONST VOID *param);
static int		ForwardProc(Tcl_Event *evPtr, int mask);
static void		SrcExitProc(ClientData clientData);
static void		DstExitProc(ClientData clientData);

#define FreeReceivedError(p) \
	if ((p)->base.mustFree) { \
	    ckfree((p)->base.msgStr); \
	}
#define PassReceivedErrorInterp(i,p) \
	if ((i) != NULL) { \
	    Tcl_SetChannelErrorInterp((i), \
		    Tcl_NewStringObj((p)->base.msgStr, -1)); \
	} \
	FreeReceivedError(p)
#define PassReceivedError(c,p) \
	Tcl_SetChannelError((c), Tcl_NewStringObj((p)->base.msgStr, -1)); \
	FreeReceivedError(p)
#define ForwardSetStaticError(p,emsg) \
	(p)->base.code = TCL_ERROR; \
	(p)->base.mustFree = 0; \
	(p)->base.msgStr = (char *) (emsg)
#define ForwardSetDynamicError(p,emsg) \
	(p)->base.code = TCL_ERROR; \
	(p)->base.mustFree = 1; \
	(p)->base.msgStr = (char *) (emsg)

static void		ForwardSetObjError(ForwardParam *p,
			    Tcl_Obj *objPtr);
#endif /* TCL_THREADS */

#define SetChannelErrorStr(c,msgStr) \
	Tcl_SetChannelError((c), Tcl_NewStringObj((msgStr), -1))

static Tcl_Obj *	MarshallError(Tcl_Interp *interp);
static void		UnmarshallErrorResult(Tcl_Interp *interp,
			    Tcl_Obj *msgObj);

/*
 * Static functions for this file:
 */

static int		EncodeEventMask(Tcl_Interp *interp,
			    CONST char *objName, Tcl_Obj *obj, int *mask);
static Tcl_Obj *	DecodeEventMask(int mask);
static ReflectedChannel * NewReflectedChannel(Tcl_Interp *interp,
			    Tcl_Obj *cmdpfxObj, int mode, Tcl_Obj *handleObj);
static Tcl_Obj *	NextHandle(void);
static void		FreeReflectedChannel(ReflectedChannel *rcPtr);
static int		InvokeTclMethod(ReflectedChannel *rcPtr,
			    CONST char *method, Tcl_Obj *argOneObj,
			    Tcl_Obj *argTwoObj, Tcl_Obj **resultObjPtr,
			    int flags);

#define INVOKE_NO_CAPTURE	0x01

/*
 * Global constant strings (messages). ==================
 * These string are used directly as bypass errors, thus they have to be valid
 * Tcl lists where the last element is the message itself. Hence the
 * list-quoting to keep the words of the message together. See also [x].
 */

static CONST char *msg_read_unsup = "{read not supported by Tcl driver}";
static CONST char *msg_read_toomuch = "{read delivered more than requested}";
static CONST char *msg_write_unsup = "{write not supported by Tcl driver}";
static CONST char *msg_write_toomuch = "{write wrote more than requested}";
static CONST char *msg_seek_beforestart = "{Tried to seek before origin}";
#ifdef TCL_THREADS
static CONST char *msg_send_originlost = "{Origin thread lost}";
static CONST char *msg_send_dstlost = "{Destination thread lost}";
#endif /* TCL_THREADS */

/*
 * Main methods to plug into the 'chan' ensemble'. ==================
 */

/*
 *----------------------------------------------------------------------
 *
 * TclChanCreateObjCmd --
 *
 *	This function is invoked to process the "chan create" Tcl command.
 *	See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result. The handle of the new channel is placed in the
 *	interp result.
 *
 * Side effects:
 *	Creates a new channel.
 *
 *----------------------------------------------------------------------
 */

int
TclChanCreateObjCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *CONST *objv)
{
    ReflectedChannel *rcPtr;	/* Instance data of the new channel */
    Tcl_Obj *rcId;		/* Handle of the new channel */
    int mode;			/* R/W mode of new channel. Has to match
				 * abilities of handler commands */
    Tcl_Obj *cmdObj;		/* Command prefix, list of words */
    Tcl_Obj *cmdNameObj;	/* Command name */
    Tcl_Channel chan;		/* Token for the new channel */
    Tcl_Obj *modeObj;		/* mode in obj form for method call */
    int listc;			/* Result of 'initialize', and of */
    Tcl_Obj **listv;		/* its sublist in the 2nd element */
    int methIndex;		/* Encoded method name */
    int result;			/* Result code for 'initialize' */
    Tcl_Obj *resObj;		/* Result data for 'initialize' */
    int methods;		/* Bitmask for supported methods. */
    Channel *chanPtr;		/* 'chan' resolved to internal struct. */

    /*
     * Syntax:   chan create MODE CMDPREFIX
     *           [0]  [1]    [2]  [3]
     *
     * Actually: rCreate MODE CMDPREFIX
     *           [0]     [1]  [2]
     */

#define MODE	(1)
#define CMD	(2)

    /*
     * Number of arguments...
     */

    if (objc != 3) {
	Tcl_WrongNumArgs(interp, 1, objv, "mode cmdprefix");
	return TCL_ERROR;
    }

    /*
     * First argument is a list of modes. Allowed entries are "read", "write".
     * Expect at least one list element. Abbreviations are ok.
     */

    modeObj = objv[MODE];
    if (EncodeEventMask(interp, "mode", objv[MODE], &mode) != TCL_OK) {
	return TCL_ERROR;
    }

    /*
     * Second argument is command prefix, i.e. list of words, first word is
     * name of handler command, other words are fixed arguments. Run
     * 'initialize' method to get the list of supported methods. Validate
     * this.
     */

    cmdObj = objv[CMD];

    /*
     * Basic check that the command prefix truly is a list.
     */

    if (Tcl_ListObjIndex(interp, cmdObj, 0, &cmdNameObj) != TCL_OK) {
	return TCL_ERROR;
    }

    /*
     * Now create the channel.
     */

    rcId = NextHandle();
    rcPtr = NewReflectedChannel(interp, cmdObj, mode, rcId);
    chan = Tcl_CreateChannel(&tclRChannelType, TclGetString(rcId), rcPtr,
	    mode);
    rcPtr->chan = chan;
    chanPtr = (Channel *) chan;

    /*
     * Invoke 'initialize' and validate that the handler is present and ok.
     * Squash the channel if not.
     *
     * Note: The conversion of 'mode' back into a Tcl_Obj ensures that
     * 'initialize' is invoked with canonical mode names, and no
     * abbreviations. Using modeObj directly could feed abbreviations into the
     * handler, and the handler is not specified to handle such.
     */

    modeObj = DecodeEventMask(mode);
    result = InvokeTclMethod(rcPtr, "initialize", modeObj, NULL, &resObj,
	    INVOKE_NO_CAPTURE);
    Tcl_DecrRefCount(modeObj);
    if (result != TCL_OK) {
	Tcl_Obj *err = Tcl_NewStringObj("Initialize failure: ", -1);

	Tcl_AppendObjToObj(err, resObj);
	Tcl_SetObjResult(interp, err);
	Tcl_DecrRefCount(resObj);	/* Remove reference held from invoke */
	goto error;
    }

    /*
     * Verify the result.
     * - List, of method names. Convert to mask.
     *   Check for non-optionals through the mask.
     *   Compare open mode against optional r/w.
     */

    Tcl_AppendResult(interp, "Initialize failure: ", NULL);

    if (Tcl_ListObjGetElements(interp, resObj, &listc, &listv) != TCL_OK) {
	/*
	 * The function above replaces my prefix in case of an error, so more
	 * work for us to get the prefix back into the error message
	 */

	Tcl_Obj *err = Tcl_NewStringObj("Initialize failure: ", -1);

	Tcl_AppendObjToObj(err, Tcl_GetObjResult(interp));
	Tcl_SetObjResult(interp, err);
	goto error;
    }

    methods = 0;
    while (listc > 0) {
	if (Tcl_GetIndexFromObj(interp, listv[listc-1], methodNames,
		"method", TCL_EXACT, &methIndex) != TCL_OK) {
	    Tcl_Obj *err = Tcl_NewStringObj("Initialize failure: ", -1);

	    Tcl_AppendObjToObj(err, Tcl_GetObjResult(interp));
	    Tcl_SetObjResult(interp, err);
	    goto error;
	}

	methods |= FLAG(methIndex);
	listc--;
    }

    if ((REQUIRED_METHODS & methods) != REQUIRED_METHODS) {
	Tcl_AppendResult(interp, "Not all required methods supported", NULL);
	goto error;
    }

    if ((mode & TCL_READABLE) && !HAS(methods, METH_READ)) {
	Tcl_AppendResult(interp, "Reading not supported, but requested", NULL);
	goto error;
    }

    if ((mode & TCL_WRITABLE) && !HAS(methods, METH_WRITE)) {
	Tcl_AppendResult(interp, "Writing not supported, but requested", NULL);
	goto error;
    }

    if (!IMPLIES(HAS(methods, METH_CGET), HAS(methods, METH_CGETALL))) {
	Tcl_AppendResult(interp,
		"'cgetall' not supported, but should be, as 'cget' is", NULL);
	goto error;
    }

    if (!IMPLIES(HAS(methods, METH_CGETALL), HAS(methods, METH_CGET))) {
	Tcl_AppendResult(interp,
		"'cget' not supported, but should be, as 'cgetall' is", NULL);
	goto error;
    }

    Tcl_ResetResult(interp);

    /*
     * Everything is fine now.
     */

    rcPtr->methods = methods;

    if ((methods & NULLABLE_METHODS) != NULLABLE_METHODS) {
	/*
	 * Some of the nullable methods are not supported. We clone the
	 * channel type, null the associated C functions, and use the result
	 * as the actual channel type.
	 */

	Tcl_ChannelType *clonePtr = (Tcl_ChannelType *)
		ckalloc(sizeof(Tcl_ChannelType));

	memcpy(clonePtr, &tclRChannelType, sizeof(Tcl_ChannelType));

	if (!(methods & FLAG(METH_CONFIGURE))) {
	    clonePtr->setOptionProc = NULL;
	}

	if (!(methods & FLAG(METH_CGET)) && !(methods & FLAG(METH_CGETALL))) {
	    clonePtr->getOptionProc = NULL;
	}
	if (!(methods & FLAG(METH_BLOCKING))) {
	    clonePtr->blockModeProc = NULL;
	}
	if (!(methods & FLAG(METH_SEEK))) {
	    clonePtr->seekProc = NULL;
	    clonePtr->wideSeekProc = NULL;
	}

	chanPtr->typePtr = clonePtr;
    }

    Tcl_RegisterChannel(interp, chan);

    /*
     * Return handle as result of command.
     */

    Tcl_SetObjResult(interp, rcId);
    return TCL_OK;

 error:
    /*
     * Signal to ReflectClose to not call 'finalize'.
     */
    rcPtr->methods = 0;
    Tcl_Close(interp, chan);
    return TCL_ERROR;

#undef MODE
#undef CMD
}

/*
 *----------------------------------------------------------------------
 *
 * TclChanPostEventObjCmd --
 *
 *	This function is invoked to process the "chan postevent" Tcl command.
 *	See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	Posts events to a reflected channel, invokes event handlers. The
 *	latter implies that arbitrary side effects are possible.
 *
 *----------------------------------------------------------------------
 */

int
TclChanPostEventObjCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *CONST *objv)
{
    /*
     * Syntax:   chan postevent CHANNEL EVENTSPEC
     *           [0]  [1]       [2]     [3]
     *
     * Actually: rPostevent CHANNEL EVENTSPEC
     *           [0]        [1]     [2]
     *
     * where EVENTSPEC = {read write ...} (Abbreviations allowed as well).
     */

#define CHAN	(1)
#define EVENT	(2)

    CONST char *chanId;		/* Tcl level channel handle */
    Tcl_Channel chan;		/* Channel associated to the handle */
    Tcl_ChannelType *chanTypePtr;
				/* Its associated driver structure */
    ReflectedChannel *rcPtr;	/* Associated instance data */
    int mode;			/* Dummy, r|w mode of the channel */
    int events;			/* Mask of events to post */

    /*
     * Number of arguments...
     */

    if (objc != 3) {
	Tcl_WrongNumArgs(interp, 1, objv, "channel eventspec");
	return TCL_ERROR;
    }

    /*
     * First argument is a channel, a reflected channel, and the call of this
     * command is done from the interp defining the channel handler cmd.
     */

    chanId = TclGetString(objv[CHAN]);
    chan = Tcl_GetChannel(interp, chanId, &mode);

    if (chan == NULL) {
	return TCL_ERROR;
    }

    chanTypePtr = Tcl_GetChannelType(chan);

    /*
     * We use a function referenced by the channel type as our cookie to
     * detect calls to non-reflecting channels. The channel type itself is not
     * suitable, as it might not be the static definition in this file, but a
     * clone thereof. And while we have reserved the name of the type nothing
     * in the core checks against violation, so someone else might have
     * created a channel type using our name, clashing with ourselves.
     */

    if (chanTypePtr->watchProc != &ReflectWatch) {
	Tcl_AppendResult(interp, "channel \"", chanId,
		"\" is not a reflected channel", NULL);
	return TCL_ERROR;
    }

    rcPtr = (ReflectedChannel *) Tcl_GetChannelInstanceData(chan);

    if (rcPtr->interp != interp) {
	Tcl_AppendResult(interp, "postevent for channel \"", chanId,
		"\" called from outside interpreter", NULL);
	return TCL_ERROR;
    }

    /*
     * Second argument is a list of events. Allowed entries are "read",
     * "write". Expect at least one list element. Abbreviations are ok.
     */

    if (EncodeEventMask(interp, "event", objv[EVENT], &events) != TCL_OK) {
	return TCL_ERROR;
    }

    /*
     * Check that the channel is actually interested in the provided events.
     */

    if (events & ~rcPtr->interest) {
	Tcl_AppendResult(interp, "tried to post events channel \"", chanId,
		"\" is not interested in", NULL);
	return TCL_ERROR;
    }

    /*
     * We have the channel and the events to post.
     */

    Tcl_NotifyChannel(chan, events);

    /*
     * Squash interp results left by the event script.
     */

    Tcl_ResetResult(interp);
    return TCL_OK;

#undef CHAN
#undef EVENT
}

/*
 * Channel error message marshalling utilities.
 */

static Tcl_Obj*
MarshallError(
    Tcl_Interp *interp)
{
    /*
     * Capture the result status of the interpreter into a string. => List of
     * options and values, followed by the error message. The result has
     * refCount 0.
     */

    Tcl_Obj *returnOpt = Tcl_GetReturnOptions(interp, TCL_ERROR);

    /*
     * => returnOpt.refCount == 0. We can append directly.
     */

    Tcl_ListObjAppendElement(NULL, returnOpt, Tcl_GetObjResult(interp));
    return returnOpt;
}

static void
UnmarshallErrorResult(
    Tcl_Interp *interp,
    Tcl_Obj *msgObj)
{
    int lc;
    Tcl_Obj **lv;
    int explicitResult;
    int numOptions;

    /*
     * Process the caught message.
     *
     * Syntax = (option value)... ?message?
     *
     * Bad syntax causes a panic. This is OK because the other side uses
     * Tcl_GetReturnOptions and list construction functions to marshall the
     * information; if we panic here, something has gone badly wrong already.
     */

    if (Tcl_ListObjGetElements(interp, msgObj, &lc, &lv) != TCL_OK) {
	Tcl_Panic("TclChanCaughtErrorBypass: Bad syntax of caught result");
    }

    explicitResult = lc & 1;		/* Odd number of values? */
    numOptions = lc - explicitResult;

    if (explicitResult) {
	Tcl_SetObjResult(interp, lv[lc-1]);
    }

    (void) Tcl_SetReturnOptions(interp, Tcl_NewListObj(numOptions, lv));
}

int
TclChanCaughtErrorBypass(
    Tcl_Interp *interp,
    Tcl_Channel chan)
{
    Tcl_Obj *chanMsgObj = NULL;
    Tcl_Obj *interpMsgObj = NULL;
    Tcl_Obj *msgObj = NULL;

    /*
     * Get a bypassed error message from channel and/or interpreter, save the
     * reference, then kill the returned objects, if there were any. If there
     * are messages in both the channel has preference.
     */

    if ((chan == NULL) && (interp == NULL)) {
	return 0;
    }

    if (chan != NULL) {
	Tcl_GetChannelError(chan, &chanMsgObj);
    }
    if (interp != NULL) {
	Tcl_GetChannelErrorInterp(interp, &interpMsgObj);
    }

    if (chanMsgObj != NULL) {
	msgObj = chanMsgObj;
    } else if (interpMsgObj != NULL) {
	msgObj = interpMsgObj;
    }
    if (msgObj != NULL) {
	Tcl_IncrRefCount(msgObj);
    }

    if (chanMsgObj != NULL) {
	Tcl_DecrRefCount(chanMsgObj);
    }
    if (interpMsgObj != NULL) {
	Tcl_DecrRefCount(interpMsgObj);
    }

    /*
     * No message returned, nothing caught.
     */

    if (msgObj == NULL) {
	return 0;
    }

    UnmarshallErrorResult(interp, msgObj);

    Tcl_DecrRefCount(msgObj);
    return 1;
}

/*
 * Driver functions. ================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * ReflectClose --
 *
 *	This function is invoked when the channel is closed, to delete the
 *	driver specific instance data.
 *
 * Results:
 *	A posix error.
 *
 * Side effects:
 *	Releases memory. Arbitrary, as it calls upon a script.
 *
 *----------------------------------------------------------------------
 */

static int
ReflectClose(
    ClientData clientData,
    Tcl_Interp *interp)
{
    ReflectedChannel *rcPtr = (ReflectedChannel *) clientData;
    int result;			/* Result code for 'close' */
    Tcl_Obj *resObj;		/* Result data for 'close' */

    if (interp == NULL) {
	/*
	 * This call comes from TclFinalizeIOSystem. There are no
	 * interpreters, and therefore we cannot call upon the handler command
	 * anymore. Threading is irrelevant as well. We simply clean up all
	 * our C level data structures and leave the Tcl level to the other
	 * finalization functions.
	 */

	/*
	 * THREADED => Forward this to the origin thread
	 *
	 * Note: Have a thread delete handler for the origin thread. Use this
	 * to clean up the structure!
	 */

#ifdef TCL_THREADS
	if (rcPtr->thread != Tcl_GetCurrentThread()) {
	    ForwardParam p;

	    ForwardOpToOwnerThread(rcPtr, ForwardedClose, &p);
	    result = p.base.code;

	    /*
	     * FreeReflectedChannel is done in the forwarded operation!, in
	     * the other thread. rcPtr here is gone!
	     */

	    if (result != TCL_OK) {
		FreeReceivedError(&p);
	    }
	    return EOK;
	}
#endif

	FreeReflectedChannel(rcPtr);
	return EOK;
    }

    /*
     * -- No -- ASSERT rcPtr->methods & FLAG(METH_FINAL)
     *
     * A cleaned method mask here implies that the channel creation was
     * aborted, and "finalize" must not be called.
     */

    if (rcPtr->methods == 0) {
	FreeReflectedChannel(rcPtr);
	return EOK;
    }

    /*
     * Are we in the correct thread?
     */

#ifdef TCL_THREADS
    if (rcPtr->thread != Tcl_GetCurrentThread()) {
	ForwardParam p;

	ForwardOpToOwnerThread(rcPtr, ForwardedClose, &p);
	result = p.base.code;

	/*
	 * FreeReflectedChannel is done in the forwarded operation!, in the
	 * other thread. rcPtr here is gone!
	 */

	if (result != TCL_OK) {
	    PassReceivedErrorInterp(interp, &p);
	}
    } else {
#endif
	result = InvokeTclMethod(rcPtr, "finalize", NULL, NULL, &resObj, 0);
	if ((result != TCL_OK) && (interp != NULL)) {
	    Tcl_SetChannelErrorInterp(interp, resObj);
	}

	Tcl_DecrRefCount(resObj);	/* Remove reference we held from the
					 * invoke */
#ifdef TCL_THREADS
	FreeReflectedChannel(rcPtr);
    }
#endif
    return (result == TCL_OK) ? EOK : EINVAL;
}

/*
 *----------------------------------------------------------------------
 *
 * ReflectInput --
 *
 *	This function is invoked when more data is requested from the channel.
 *
 * Results:
 *	The number of bytes read.
 *
 * Side effects:
 *	Allocates memory. Arbitrary, as it calls upon a script.
 *
 *----------------------------------------------------------------------
 */

static int
ReflectInput(
    ClientData clientData,
    char *buf,
    int toRead,
    int *errorCodePtr)
{
    ReflectedChannel *rcPtr = (ReflectedChannel *) clientData;
    Tcl_Obj *toReadObj;
    int bytec;			/* Number of returned bytes */
    unsigned char *bytev;	/* Array of returned bytes */
    Tcl_Obj *resObj;		/* Result data for 'read' */

    /*
     * The following check can be done before thread redirection, because we
     * are reading from an item which is readonly, i.e. will never change
     * during the lifetime of the channel.
     */

    if (!(rcPtr->methods & FLAG(METH_READ))) {
	SetChannelErrorStr(rcPtr->chan, msg_read_unsup);
	*errorCodePtr = EINVAL;
	return -1;
    }

    /*
     * Are we in the correct thread?
     */

#ifdef TCL_THREADS
    if (rcPtr->thread != Tcl_GetCurrentThread()) {
	ForwardParam p;

	p.input.buf = buf;
	p.input.toRead = toRead;

	ForwardOpToOwnerThread(rcPtr, ForwardedInput, &p);

	if (p.base.code != TCL_OK) {
	    PassReceivedError(rcPtr->chan, &p);
	    *errorCodePtr = EINVAL;
	} else {
	    *errorCodePtr = EOK;
	}

	return p.input.toRead;
    }
#endif

    /* ASSERT: rcPtr->method & FLAG(METH_READ) */
    /* ASSERT: rcPtr->mode & TCL_READABLE */

    toReadObj = Tcl_NewIntObj(toRead);
    if (InvokeTclMethod(rcPtr, "read", toReadObj, NULL, &resObj, 0)!=TCL_OK) {
	Tcl_SetChannelError(rcPtr->chan, resObj);
	Tcl_DecrRefCount(resObj);	/* Remove reference held from invoke */
	*errorCodePtr = EINVAL;
	return -1;
    }

    bytev = Tcl_GetByteArrayFromObj(resObj, &bytec);

    if (toRead < bytec) {
	Tcl_DecrRefCount(resObj);	/* Remove reference held from invoke */
	SetChannelErrorStr(rcPtr->chan, msg_read_toomuch);
	*errorCodePtr = EINVAL;
	return -1;
    }

    *errorCodePtr = EOK;

    if (bytec > 0) {
	memcpy(buf, bytev, bytec);
    }

    Tcl_DecrRefCount(resObj);		/* Remove reference held from invoke */
    return bytec;
}

/*
 *----------------------------------------------------------------------
 *
 * ReflectOutput --
 *
 *	This function is invoked when data is writen to the channel.
 *
 * Results:
 *	The number of bytes actually written.
 *
 * Side effects:
 *	Allocates memory. Arbitrary, as it calls upon a script.
 *
 *----------------------------------------------------------------------
 */

static int
ReflectOutput(
    ClientData clientData,
    CONST char *buf,
    int toWrite,
    int *errorCodePtr)
{
    ReflectedChannel *rcPtr = (ReflectedChannel *) clientData;
    Tcl_Obj *bufObj;
    Tcl_Obj *resObj;		/* Result data for 'write' */
    int written;

    /*
     * The following check can be done before thread redirection, because we
     * are reading from an item which is readonly, i.e. will never change
     * during the lifetime of the channel.
     */

    if (!(rcPtr->methods & FLAG(METH_WRITE))) {
	SetChannelErrorStr(rcPtr->chan, msg_write_unsup);
	*errorCodePtr = EINVAL;
	return -1;
    }

    /*
     * Are we in the correct thread?
     */

#ifdef TCL_THREADS
    if (rcPtr->thread != Tcl_GetCurrentThread()) {
	ForwardParam p;

	p.output.buf = buf;
	p.output.toWrite = toWrite;

	ForwardOpToOwnerThread(rcPtr, ForwardedOutput, &p);

	if (p.base.code != TCL_OK) {
	    PassReceivedError(rcPtr->chan, &p);
	    *errorCodePtr = EINVAL;
	} else {
	    *errorCodePtr = EOK;
	}

	return p.output.toWrite;
    }
#endif

    /* ASSERT: rcPtr->method & FLAG(METH_WRITE) */
    /* ASSERT: rcPtr->mode & TCL_WRITABLE */

    bufObj = Tcl_NewByteArrayObj((unsigned char *) buf, toWrite);
    if (InvokeTclMethod(rcPtr, "write", bufObj, NULL, &resObj, 0) != TCL_OK) {
	Tcl_SetChannelError(rcPtr->chan, resObj);
	Tcl_DecrRefCount(resObj);	/* Remove reference held from invoke */
	*errorCodePtr = EINVAL;
	return -1;
    }

    if (Tcl_GetIntFromObj(rcPtr->interp, resObj, &written) != TCL_OK) {
	Tcl_DecrRefCount(resObj);	/* Remove reference held from invoke */
	Tcl_SetChannelError(rcPtr->chan, MarshallError(rcPtr->interp));
	*errorCodePtr = EINVAL;
	return -1;
    }

    Tcl_DecrRefCount(resObj);		/* Remove reference held from invoke */

    if ((written == 0) || (toWrite < written)) {
	/*
	 * The handler claims to have written more than it was given. That is
	 * bad. Note that the I/O core would crash if we were to return this
	 * information, trying to write -nnn bytes in the next iteration.
	 */

	SetChannelErrorStr(rcPtr->chan, msg_write_toomuch);
	*errorCodePtr = EINVAL;
	return -1;
    }

    *errorCodePtr = EOK;
    return written;
}

/*
 *----------------------------------------------------------------------
 *
 * ReflectSeekWide / ReflectSeek --
 *
 *	This function is invoked when the user wishes to seek on the channel.
 *
 * Results:
 *	The new location of the access point.
 *
 * Side effects:
 *	Allocates memory. Arbitrary, as it calls upon a script.
 *
 *----------------------------------------------------------------------
 */

static Tcl_WideInt
ReflectSeekWide(
    ClientData clientData,
    Tcl_WideInt offset,
    int seekMode,
    int *errorCodePtr)
{
    ReflectedChannel *rcPtr = (ReflectedChannel *) clientData;
    Tcl_Obj *offObj;
    Tcl_Obj *baseObj;
    Tcl_Obj *resObj;		/* Result for 'seek' */
    Tcl_WideInt newLoc;

    /*
     * Are we in the correct thread?
     */

#ifdef TCL_THREADS
    if (rcPtr->thread != Tcl_GetCurrentThread()) {
	ForwardParam p;

	p.seek.seekMode = seekMode;
	p.seek.offset = offset;

	ForwardOpToOwnerThread(rcPtr, ForwardedSeek, &p);

	if (p.base.code != TCL_OK) {
	    PassReceivedError(rcPtr->chan, &p);
	    *errorCodePtr = EINVAL;
	} else {
	    *errorCodePtr = EOK;
	}

	return p.seek.offset;
    }
#endif

    /* ASSERT: rcPtr->method & FLAG(METH_SEEK) */

    offObj = Tcl_NewWideIntObj(offset);
    baseObj = Tcl_NewStringObj((seekMode == SEEK_SET) ? "start" :
	    ((seekMode == SEEK_CUR) ? "current" : "end"), -1);
    if (InvokeTclMethod(rcPtr, "seek", offObj, baseObj, &resObj, 0)!=TCL_OK) {
	Tcl_SetChannelError(rcPtr->chan, resObj);
	Tcl_DecrRefCount(resObj);	/* Remove reference held from invoke */
	*errorCodePtr = EINVAL;
	return -1;
    }

    if (Tcl_GetWideIntFromObj(rcPtr->interp, resObj, &newLoc) != TCL_OK) {
	Tcl_DecrRefCount(resObj);	/* Remove reference held from invoke */
	Tcl_SetChannelError(rcPtr->chan, MarshallError(rcPtr->interp));
	*errorCodePtr = EINVAL;
	return -1;
    }

    Tcl_DecrRefCount(resObj);		/* Remove reference held from invoke */

    if (newLoc < Tcl_LongAsWide(0)) {
	SetChannelErrorStr(rcPtr->chan, msg_seek_beforestart);
	*errorCodePtr = EINVAL;
	return -1;
    }

    *errorCodePtr = EOK;
    return newLoc;
}

static int
ReflectSeek(
    ClientData clientData,
    long offset,
    int seekMode,
    int *errorCodePtr)
{
    /*
     * This function can be invoked from a transformation which is based on
     * standard seeking, i.e. non-wide. Because of this we have to implement
     * it, a dummy is not enough. We simply delegate the call to the wide
     * routine.
     */

    return (int) ReflectSeekWide(clientData, Tcl_LongAsWide(offset), seekMode,
	    errorCodePtr);
}

/*
 *----------------------------------------------------------------------
 *
 * ReflectWatch --
 *
 *	This function is invoked to tell the channel what events the I/O
 *	system is interested in.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Allocates memory. Arbitrary, as it calls upon a script.
 *
 *----------------------------------------------------------------------
 */

static void
ReflectWatch(
    ClientData clientData,
    int mask)
{
    ReflectedChannel *rcPtr = (ReflectedChannel *) clientData;
    Tcl_Obj *maskObj;

    /* ASSERT rcPtr->methods & FLAG(METH_WATCH) */

    /*
     * We restrict the interest to what the channel can support. IOW there
     * will never be write events for a channel which is not writable.
     * Analoguously for read events and non-readable channels.
     */

    mask &= rcPtr->mode;

    if (mask == rcPtr->interest) {
	/*
	 * Same old, same old, why should we do something?
	 */

	return;
    }

    rcPtr->interest = mask;

    /*
     * Are we in the correct thread?
     */

#ifdef TCL_THREADS
    if (rcPtr->thread != Tcl_GetCurrentThread()) {
	ForwardParam p;

	p.watch.mask = mask;
	ForwardOpToOwnerThread(rcPtr, ForwardedWatch, &p);

	/*
	 * Any failure from the forward is ignored. We have no place to put
	 * this.
	 */

	return;
    }
#endif

    maskObj = DecodeEventMask(mask);
    (void) InvokeTclMethod(rcPtr, "watch", maskObj, NULL, NULL,
	    INVOKE_NO_CAPTURE);
    Tcl_DecrRefCount(maskObj);
}

/*
 *----------------------------------------------------------------------
 *
 * ReflectBlock --
 *
 *	This function is invoked to tell the channel which blocking behaviour
 *	is required of it.
 *
 * Results:
 *	A posix error number.
 *
 * Side effects:
 *	Allocates memory. Arbitrary, as it calls upon a script.
 *
 *----------------------------------------------------------------------
 */

static int
ReflectBlock(
    ClientData clientData,
    int nonblocking)
{
    ReflectedChannel *rcPtr = (ReflectedChannel *) clientData;
    Tcl_Obj *blockObj;
    int errorNum;		/* EINVAL or EOK (success). */
    Tcl_Obj *resObj;		/* Result data for 'blocking' */

    /*
     * Are we in the correct thread?
     */

#ifdef TCL_THREADS
    if (rcPtr->thread != Tcl_GetCurrentThread()) {
	ForwardParam p;

	p.block.nonblocking = nonblocking;

	ForwardOpToOwnerThread(rcPtr, ForwardedBlock, &p);

	if (p.base.code != TCL_OK) {
	    PassReceivedError(rcPtr->chan, &p);
	    return EINVAL;
	}

	return EOK;
    }
#endif

    blockObj = Tcl_NewBooleanObj(!nonblocking);

    if (InvokeTclMethod(rcPtr, "blocking", blockObj, NULL, &resObj,
	    0) != TCL_OK) {
	Tcl_SetChannelError(rcPtr->chan, resObj);
	errorNum = EINVAL;
    } else {
	errorNum = EOK;
    }

    Tcl_DecrRefCount(resObj);		/* Remove reference held from invoke */
    return errorNum;
}

/*
 *----------------------------------------------------------------------
 *
 * ReflectSetOption --
 *
 *	This function is invoked to configure a channel option.
 *
 * Results:
 *	A standard Tcl result code.
 *
 * Side effects:
 *	Arbitrary, as it calls upon a Tcl script.
 *
 *----------------------------------------------------------------------
 */

static int
ReflectSetOption(
    ClientData clientData,	/* Channel to query */
    Tcl_Interp *interp,		/* Interpreter to leave error messages in */
    CONST char *optionName,	/* Name of requested option */
    CONST char *newValue)	/* The new value */
{
    ReflectedChannel *rcPtr = (ReflectedChannel *) clientData;
    Tcl_Obj *optionObj;
    Tcl_Obj *valueObj;
    int result;			/* Result code for 'configure' */
    Tcl_Obj *resObj;		/* Result data for 'configure' */

    /*
     * Are we in the correct thread?
     */

#ifdef TCL_THREADS
    if (rcPtr->thread != Tcl_GetCurrentThread()) {
	ForwardParam p;

	p.setOpt.name = optionName;
	p.setOpt.value = newValue;

	ForwardOpToOwnerThread(rcPtr, ForwardedSetOpt, &p);

	if (p.base.code != TCL_OK) {
	    Tcl_Obj *err = Tcl_NewStringObj(p.base.msgStr, -1);

	    UnmarshallErrorResult(interp, err);
	    Tcl_DecrRefCount(err);
	    FreeReceivedError(&p);
	}

	return p.base.code;
    }
#endif

    optionObj = Tcl_NewStringObj(optionName, -1);
    valueObj = Tcl_NewStringObj(newValue, -1);
    result = InvokeTclMethod(rcPtr, "configure",optionObj,valueObj, &resObj,0);
    if (result != TCL_OK) {
	UnmarshallErrorResult(interp, resObj);
    }

    Tcl_DecrRefCount(resObj);		/* Remove reference held from invoke */
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ReflectGetOption --
 *
 *	This function is invoked to retrieve all or a channel option.
 *
 * Results:
 *	A standard Tcl result code.
 *
 * Side effects:
 *	Arbitrary, as it calls upon a Tcl script.
 *
 *----------------------------------------------------------------------
 */

static int
ReflectGetOption(
    ClientData clientData,	/* Channel to query */
    Tcl_Interp *interp,		/* Interpreter to leave error messages in */
    CONST char *optionName,	/* Name of reuqested option */
    Tcl_DString *dsPtr)		/* String to place the result into */
{
    /*
     * This code is special. It has regular passing of Tcl result, and errors.
     * The bypass functions are not required.
     */

    ReflectedChannel *rcPtr = (ReflectedChannel*) clientData;
    Tcl_Obj *optionObj;
    Tcl_Obj *resObj;		/* Result data for 'configure' */
    int listc;
    Tcl_Obj **listv;
    const char *method;

    /*
     * Are we in the correct thread?
     */

#ifdef TCL_THREADS
    if (rcPtr->thread != Tcl_GetCurrentThread()) {
	int opcode;
	ForwardParam p;

	p.getOpt.name = optionName;
	p.getOpt.value = dsPtr;

	if (optionName == NULL) {
	    opcode = ForwardedGetOptAll;
	} else {
	    opcode = ForwardedGetOpt;
	}

	ForwardOpToOwnerThread(rcPtr, opcode, &p);

	if (p.base.code != TCL_OK) {
	    Tcl_Obj *err = Tcl_NewStringObj(p.base.msgStr, -1);

	    UnmarshallErrorResult(interp, err);
	    Tcl_DecrRefCount(err);
	    FreeReceivedError(&p);
	}

	return p.base.code;
    }
#endif

    if (optionName == NULL) {
	/*
	 * Retrieve all options.
	 */

	method = "cgetall";
	optionObj = NULL;
    } else {
	/*
	 * Retrieve the value of one option.
	 */

	method = "cget";
	optionObj = Tcl_NewStringObj(optionName, -1);
    }

    if (InvokeTclMethod(rcPtr, method, optionObj, NULL, &resObj, 0)!=TCL_OK) {
	UnmarshallErrorResult(interp, resObj);
	Tcl_DecrRefCount(resObj);	/* Remove reference held from invoke */
	return TCL_ERROR;
    }

    /*
     * The result has to go into the 'dsPtr' for propagation to the caller of
     * the driver.
     */

    if (optionObj != NULL) {
	Tcl_DStringAppend(dsPtr, TclGetString(resObj), -1);
	Tcl_DecrRefCount(resObj);	/* Remove reference held from invoke */
	return TCL_OK;
    }

    /*
     * Extract the list and append each item as element.
     */

    /*
     * NOTE (4): If we extract the string rep we can assume a properly quoted
     * string. Together with a separating space this way of simply appending
     * the whole string rep might be faster. It also doesn't check if the
     * result is a valid list. Nor that the list has an even number elements.
     */

    if (Tcl_ListObjGetElements(interp, resObj, &listc, &listv) != TCL_OK) {
	Tcl_DecrRefCount(resObj);	/* Remove reference held from invoke */
	return TCL_ERROR;
    }

    if ((listc % 2) == 1) {
	/*
	 * Odd number of elements is wrong.
	 */

	Tcl_Obj *objPtr = Tcl_NewObj();

	Tcl_ResetResult(interp);
	TclObjPrintf(NULL, objPtr, "Expected list with even number of "
		"elements, got %d element%s instead", listc,
		(listc == 1 ? "" : "s"));
	Tcl_SetObjResult(interp, objPtr);
	Tcl_DecrRefCount(resObj);	/* Remove reference held from invoke */
	return TCL_ERROR;
    } else {
	int len;
	char *str = Tcl_GetStringFromObj(resObj, &len);

	if (len) {
	    Tcl_DStringAppend(dsPtr, " ", 1);
	    Tcl_DStringAppend(dsPtr, str, len);
	}
	Tcl_DecrRefCount(resObj);	/* Remove reference held from invoke */
	return TCL_OK;
    }
}

/*
 * Helpers. =========================================================
 */

/*
 *----------------------------------------------------------------------
 *
 * EncodeEventMask --
 *
 *	This function takes a list of event items and constructs the
 *	equivalent internal bitmask. The list has to contain at least one
 *	element. Elements are "read", "write", or any unique abbreviation
 *	thereof. Note that the bitmask is not changed if problems are
 *	encountered.
 *
 * Results:
 *	A standard Tcl error code. A bitmask where TCL_READABLE and/or
 *	TCL_WRITABLE can be set.
 *
 * Side effects:
 *	May shimmer 'obj' to a list representation. May place an error message
 *	into the interp result.
 *
 *----------------------------------------------------------------------
 */

static int
EncodeEventMask(
    Tcl_Interp *interp,
    CONST char *objName,
    Tcl_Obj *obj,
    int *mask)
{
    int events;			/* Mask of events to post */
    int listc;			/* #elements in eventspec list */
    Tcl_Obj **listv;		/* Elements of eventspec list */
    int evIndex;		/* Id of event for an element of the eventspec
				 * list. */

    if (Tcl_ListObjGetElements(interp, obj, &listc, &listv) != TCL_OK) {
	return TCL_ERROR;
    }

    if (listc < 1) {
	Tcl_AppendResult(interp, "bad ", objName, " list: is empty", NULL);
	return TCL_ERROR;
    }

    events = 0;
    while (listc > 0) {
	if (Tcl_GetIndexFromObj(interp, listv[listc-1], eventOptions,
		objName, 0, &evIndex) != TCL_OK) {
	    return TCL_ERROR;
	}
	switch (evIndex) {
	case EVENT_READ:
	    events |= TCL_READABLE;
	    break;
	case EVENT_WRITE:
	    events |= TCL_WRITABLE;
	    break;
	}
	listc --;
    }

    *mask = events;
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * DecodeEventMask --
 *
 *	This function takes an internal bitmask of events and constructs the
 *	equivalent list of event items.
 *
 * Results:
 *	A Tcl_Obj reference. The object will have a refCount of one. The user
 *	has to decrement it to release the object.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj *
DecodeEventMask(
    int mask)
{
    register CONST char *eventStr;
    Tcl_Obj *evObj;

    switch (mask & RANDW) {
    case RANDW:
	eventStr = "read write";
	break;
    case TCL_READABLE:
	eventStr = "read";
	break;
    case TCL_WRITABLE:
	eventStr = "write";
	break;
    default:
	eventStr = "";
	break;
    }

    evObj = Tcl_NewStringObj(eventStr, -1);
    Tcl_IncrRefCount(evObj);
    return evObj;
}

/*
 *----------------------------------------------------------------------
 *
 * NewReflectedChannel --
 *
 *	This function is invoked to allocate and initialize the instance data
 *	of a new reflected channel.
 *
 * Results:
 *	A heap-allocated channel instance.
 *
 * Side effects:
 *	Allocates memory.
 *
 *----------------------------------------------------------------------
 */

static ReflectedChannel *
NewReflectedChannel(
    Tcl_Interp *interp,
    Tcl_Obj *cmdpfxObj,
    int mode,
    Tcl_Obj *handleObj)
{
    ReflectedChannel *rcPtr;
    int listc;
    Tcl_Obj **listv;
    int i;

    rcPtr = (ReflectedChannel *) ckalloc(sizeof(ReflectedChannel));

    /* rcPtr->chan: Assigned by caller. Dummy data here. */
    /* rcPtr->methods: Assigned by caller. Dummy data here. */

    rcPtr->chan = NULL;
    rcPtr->methods = 0;
    rcPtr->interp = interp;
#ifdef TCL_THREADS
    rcPtr->thread = Tcl_GetCurrentThread();
#endif
    rcPtr->mode = mode;
    rcPtr->interest = 0;		/* Initially no interest registered */

    /*
     * Method placeholder.
     */

    /* ASSERT: cmdpfxObj is a Tcl List */

    Tcl_ListObjGetElements(interp, cmdpfxObj, &listc, &listv);

    /*
     * See [==] as well.
     * Storage for the command prefix and the additional words required for
     * the invocation of methods in the command handler.
     *
     * listv [0] [listc-1] | [listc]  [listc+1] |
     * argv  [0]   ... [.] | [argc-2] [argc-1]  | [argc]  [argc+2]
     *       cmd   ... pfx | method   chan      | detail1 detail2
     */

    rcPtr->argc = listc + 2;
    rcPtr->argv = (Tcl_Obj**) ckalloc(sizeof(Tcl_Obj*) * (listc+4));

    /*
     * Duplicate object references.
     */

    for (i=0; i<listc ; i++) {
	Tcl_Obj *word = rcPtr->argv[i] = listv[i];
	Tcl_IncrRefCount(word);
    }

    i++;				/* Skip placeholder for method */

    rcPtr->argv[i] = handleObj;
    Tcl_IncrRefCount(handleObj);

    /*
     * The next two objects are kept empty, varying arguments.
     */

    /*
     * Initialization complete.
     */

    return rcPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * NextHandle --
 *
 *	This function is invoked to generate a channel handle for a new
 *	reflected channel.
 *
 * Results:
 *	A Tcl_Obj containing the string of the new channel handle. The
 *	refcount of the returned object is -- zero --.
 *
 * Side effects:
 *	May allocate memory. Mutex protected critical section locks out other
 *	threads for a short time.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj *
NextHandle(void)
{
    /*
     * Count number of generated reflected channels. Used for id generation.
     * Ids are never reclaimed and there is no dealing with wrap around. On
     * the other hand, "unsigned long" should be big enough except for
     * absolute longrunners (generate a 100 ids per second => overflow will
     * occur in 1 1/3 years).
     */

    TCL_DECLARE_MUTEX(rcCounterMutex)
    static unsigned long rcCounter = 0;
    Tcl_Obj *resObj;

    TclNewObj(resObj);
    Tcl_MutexLock(&rcCounterMutex);
    TclObjPrintf(NULL, resObj, "rc%lu", rcCounter);
    rcCounter++;
    Tcl_MutexUnlock(&rcCounterMutex);

    return resObj;
}

static void
FreeReflectedChannel(rcPtr)
    ReflectedChannel *rcPtr;
{
    Channel *chanPtr = (Channel *) rcPtr->chan;
    int i, n;

    if (chanPtr->typePtr != &tclRChannelType) {
	/*
	 * Delete a cloned ChannelType structure.
	 */

	ckfree((char*) chanPtr->typePtr);
    }

    n = rcPtr->argc - 2;
    for (i=0; i<n; i++) {
	Tcl_DecrRefCount(rcPtr->argv[i]);
    }

    ckfree((char*) rcPtr->argv);
    ckfree((char*) rcPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * InvokeTclMethod --
 *
 *	This function is used to invoke the Tcl level of a reflected channel.
 *	It handles all the command assembly, invokation, and generic state and
 *	result mgmt. It does *not* handle thread redirection; that is the
 *	responsibility of clients of this function.
 *
 * Results:
 *	Result code and data as returned by the method.
 *
 * Side effects:
 *	Arbitrary, as it calls upon a Tcl script.
 *
 *----------------------------------------------------------------------
 */

static int
InvokeTclMethod(
    ReflectedChannel *rcPtr,
    CONST char *method,
    Tcl_Obj *argOneObj,		/* NULL'able */
    Tcl_Obj *argTwoObj,		/* NULL'able */
    Tcl_Obj **resultObjPtr,	/* NULL'able */
    int flags)
{
    int cmdc;			/* #words in constructed command */
    Tcl_Obj *methObj = NULL;	/* Method name in object form */
    Tcl_InterpState sr;		/* State of handler interp */
    int result;			/* Result code of method invokation */
    Tcl_Obj *resObj = NULL;	/* Result of method invokation. */

    /*
     * NOTE (5): Decide impl. issue: Cache objects with method names?
     * Requires TSD data as reflections can be created in many different
     * threads.
     */

    /*
     * Insert method into the pre-allocated area, after the command prefix,
     * before the channel id.
     */

    methObj = Tcl_NewStringObj(method, -1);
    Tcl_IncrRefCount(methObj);
    rcPtr->argv[rcPtr->argc - 2] = methObj;

    /*
     * Append the additional argument containing method specific details
     * behind the channel id. If specified.
     */

    cmdc = rcPtr->argc;
    if (argOneObj) {
	Tcl_IncrRefCount(argOneObj);
	rcPtr->argv[cmdc] = argOneObj;
	cmdc++;
	if (argTwoObj) {
	    Tcl_IncrRefCount(argTwoObj);
	    rcPtr->argv[cmdc] = argTwoObj;
	    cmdc++;
	}
    }

    /*
     * And run the handler... This is done in auch a manner which leaves any
     * existing state intact.
     */

    sr = Tcl_SaveInterpState(rcPtr->interp, 0 /* Dummy */);
    Tcl_Preserve(rcPtr->interp);
    result = Tcl_EvalObjv(rcPtr->interp, cmdc, rcPtr->argv, TCL_EVAL_GLOBAL);

    /*
     * We do not try to extract the result information if the caller has no
     * interest in it. I.e. there is no need to put effort into creating
     * something which is discarded immediately after.
     */

    if (resultObjPtr) {
	if ((result == TCL_OK) || (flags & INVOKE_NO_CAPTURE)) {
	    /*
	     * Ok result taken as is, also if the caller requests that there
	     * is no capture.
	     */

	    resObj = Tcl_GetObjResult(rcPtr->interp);
	} else {
	    /*
	     * Non-ok result is always treated as an error. We have to capture
	     * the full state of the result, including additional options.
	     */

	    result = TCL_ERROR;
	    resObj = MarshallError(rcPtr->interp);
	}
	Tcl_IncrRefCount(resObj);
    }
    Tcl_RestoreInterpState(rcPtr->interp, sr);
    Tcl_Release(rcPtr->interp);

    /*
     * Cleanup of the dynamic parts of the command.
     */

    Tcl_DecrRefCount(methObj);
    if (argOneObj) {
	Tcl_DecrRefCount(argOneObj);
	if (argTwoObj) {
	    Tcl_DecrRefCount(argTwoObj);
	}
    }

    /*
     * The resObj has a ref count of 1 at this location. This means that the
     * caller of InvokeTclMethod has to dispose of it (but only if it was
     * returned to it).
     */

    if (resultObjPtr != NULL) {
	*resultObjPtr = resObj;
    }

    /*
     * There no need to handle the case where nothing is returned, because for
     * that case resObj was not set anyway.
     */

    return result;
}

#ifdef TCL_THREADS
static void
ForwardOpToOwnerThread(
    ReflectedChannel *rcPtr,	/* Channel instance */
    ForwardedOperation op,	/* Forwarded driver operation */
    CONST VOID *param)		/* Arguments */
{
    Tcl_ThreadId dst = rcPtr->thread;
    ForwardingEvent *evPtr;
    ForwardingResult *resultPtr;
    int result;

    /*
     * Create and initialize the event and data structures.
     */

    evPtr = (ForwardingEvent *) ckalloc(sizeof(ForwardingEvent));
    resultPtr = (ForwardingResult *) ckalloc(sizeof(ForwardingResult));

    evPtr->event.proc = ForwardProc;
    evPtr->resultPtr = resultPtr;
    evPtr->op = op;
    evPtr->rcPtr = rcPtr;
    evPtr->param = (ForwardParam *) param;

    resultPtr->src = Tcl_GetCurrentThread();
    resultPtr->dst = dst;
    resultPtr->done = NULL;
    resultPtr->result = -1;
    resultPtr->evPtr = evPtr;

    /*
     * Now execute the forward.
     */

    Tcl_MutexLock(&rcForwardMutex);
    TclSpliceIn(resultPtr, forwardList);

    /*
     * Ensure cleanup of the event if any of the two involved threads exits
     * while this event is pending or in progress.
     */

    Tcl_CreateThreadExitHandler(SrcExitProc, (ClientData) evPtr);
    Tcl_CreateThreadExitHandler(DstExitProc, (ClientData) evPtr);

    /*
     * Queue the event and poke the other thread's notifier.
     */

    Tcl_ThreadQueueEvent(dst, (Tcl_Event *)evPtr, TCL_QUEUE_TAIL);
    Tcl_ThreadAlert(dst);

    /*
     * (*) Block until the other thread has either processed the transfer or
     * rejected it.
     */

    while (resultPtr->result < 0) {
	/*
	 * NOTE (1): Is it possible that the current thread goes away while
	 * waiting here? IOW Is it possible that "SrcExitProc" is called
	 * while we are here? See complementary note (2) in "SrcExitProc"
	 */

	Tcl_ConditionWait(&resultPtr->done, &rcForwardMutex, NULL);
    }

    /*
     * Unlink result from the forwarder list.
     */

    TclSpliceOut(resultPtr, forwardList);

    resultPtr->nextPtr = NULL;
    resultPtr->prevPtr = NULL;

    Tcl_MutexUnlock(&rcForwardMutex);
    Tcl_ConditionFinalize(&resultPtr->done);

    /*
     * Kill the cleanup handlers now, and the result structure as well, before
     * returning the success code.
     *
     * Note: The event structure has already been deleted.
     */

    Tcl_DeleteThreadExitHandler(SrcExitProc, (ClientData) evPtr);
    Tcl_DeleteThreadExitHandler(DstExitProc, (ClientData) evPtr);

    result = resultPtr->result;
    ckfree((char*) resultPtr);
}

static int
ForwardProc(
    Tcl_Event *evGPtr,
    int mask)
{
    /*
     * Notes regarding access to the referenced data.
     *
     * In principle the data belongs to the originating thread (see
     * evPtr->src), however this thread is currently blocked at (*), i.e.
     * quiescent. Because of this we can treat the data as belonging to us,
     * without fear of race conditions. I.e. we can read and write as we like.
     *
     * The only thing we cannot be sure of is the resultPtr. This can be be
     * NULLed if the originating thread went away while the event is handled
     * here now.
     */

    ForwardingEvent *evPtr = (ForwardingEvent *) evGPtr;
    ForwardingResult *resultPtr = evPtr->resultPtr;
    ReflectedChannel *rcPtr = evPtr->rcPtr;
    Tcl_Interp *interp = rcPtr->interp;
    ForwardParam *paramPtr = evPtr->param;
    Tcl_Obj *resObj = NULL;	/* Interp result of InvokeTclMethod */

    /*
     * Ignore the event if no one is waiting for its result anymore.
     */

    if (!resultPtr) {
	return 1;
    }

    paramPtr->base.code = TCL_OK;
    paramPtr->base.msgStr = NULL;
    paramPtr->base.mustFree = 0;

    switch (evPtr->op) {
	/*
	 * The destination thread for the following operations is
	 * rcPtr->thread, which contains rcPtr->interp, the interp we have to
	 * call upon for the driver.
	 */

    case ForwardedClose:
	/*
	 * No parameters/results.
	 */

	if (InvokeTclMethod(rcPtr, "finalize", NULL, NULL, &resObj,
		0) != TCL_OK) {
	    ForwardSetObjError(paramPtr, resObj);
	}

	/*
	 * Freeing is done here, in the origin thread, because the argv[]
	 * objects belong to this thread. Deallocating them in a different
	 * thread is not allowed
	 */

	FreeReflectedChannel(rcPtr);
	break;

    case ForwardedInput: {
	Tcl_Obj *toReadObj = Tcl_NewIntObj(paramPtr->input.toRead);

	if (InvokeTclMethod(rcPtr, "read", toReadObj, NULL, &resObj,
		0) != TCL_OK) {
	    ForwardSetObjError(paramPtr, resObj);
	    paramPtr->input.toRead = -1;
	} else {
	    /*
	     * Process a regular result.
	     */

	    int bytec;		/* Number of returned bytes */
	    unsigned char *bytev; /* Array of returned bytes */

	    bytev = Tcl_GetByteArrayFromObj(resObj, &bytec);

	    if (paramPtr->input.toRead < bytec) {
		ForwardSetStaticError(paramPtr, msg_read_toomuch);
		paramPtr->input.toRead = -1;
	    } else {
		if (bytec > 0) {
		    memcpy(paramPtr->input.buf, bytev, bytec);
		}
		paramPtr->input.toRead = bytec;
	    }
	}
	break;
    }

    case ForwardedOutput: {
	Tcl_Obj *bufObj = Tcl_NewByteArrayObj((unsigned char *)
		paramPtr->output.buf, paramPtr->output.toWrite);

	if (InvokeTclMethod(rcPtr, "write", bufObj, NULL, &resObj,
		0) != TCL_OK) {
	    ForwardSetObjError(paramPtr, resObj);
	    paramPtr->output.toWrite = -1;
	} else {
	    /*
	     * Process a regular result.
	     */

	    int written;

	    if (Tcl_GetIntFromObj(interp, resObj, &written) != TCL_OK) {
		ForwardSetObjError(paramPtr, MarshallError(interp));
		paramPtr->output.toWrite = -1;
	    } else if (written==0 || paramPtr->output.toWrite<written) {
		ForwardSetStaticError(paramPtr, msg_write_toomuch);
		paramPtr->output.toWrite = -1;
	    } else {
		paramPtr->output.toWrite = written;
	    }
	}
	break;
    }

    case ForwardedSeek: {
	Tcl_Obj *offObj = Tcl_NewWideIntObj(paramPtr->seek.offset);
	Tcl_Obj *baseObj = Tcl_NewStringObj(
		(paramPtr->seek.seekMode==SEEK_SET) ? "start" :
		(paramPtr->seek.seekMode==SEEK_CUR) ? "current" : "end", -1);

	if (InvokeTclMethod(rcPtr, "seek", offObj, baseObj, &resObj,
		0) != TCL_OK) {
	    ForwardSetObjError(paramPtr, resObj);
	    paramPtr->seek.offset = -1;
	} else {
	    /*
	     * Process a regular result. If the type is wrong this may change
	     * into an error.
	     */

	    Tcl_WideInt newLoc;

	    if (Tcl_GetWideIntFromObj(interp, resObj, &newLoc) == TCL_OK) {
		if (newLoc < Tcl_LongAsWide(0)) {
		    ForwardSetStaticError(paramPtr, msg_seek_beforestart);
		    paramPtr->seek.offset = -1;
		} else {
		    paramPtr->seek.offset = newLoc;
		}
	    } else {
		ForwardSetObjError(paramPtr, MarshallError(interp));
		paramPtr->seek.offset = -1;
	    }
	}
	break;
    }

    case ForwardedWatch: {
	Tcl_Obj *maskObj = DecodeEventMask(paramPtr->watch.mask);

	(void) InvokeTclMethod(rcPtr, "watch", maskObj, NULL, NULL,
		INVOKE_NO_CAPTURE);
	Tcl_DecrRefCount(maskObj);
	break;
    }

    case ForwardedBlock: {
	Tcl_Obj *blockObj = Tcl_NewBooleanObj(!paramPtr->block.nonblocking);

	if (InvokeTclMethod(rcPtr, "blocking", blockObj, NULL, &resObj,
		0) != TCL_OK) {
	    ForwardSetObjError(paramPtr, resObj);
	}
	break;
    }

    case ForwardedSetOpt: {
	Tcl_Obj *optionObj = Tcl_NewStringObj(paramPtr->setOpt.name, -1);
	Tcl_Obj *valueObj = Tcl_NewStringObj(paramPtr->setOpt.value, -1);

	if (InvokeTclMethod(rcPtr, "configure", optionObj, valueObj, &resObj,
		0) != TCL_OK) {
	    ForwardSetObjError(paramPtr, resObj);
	}
	break;
    }

    case ForwardedGetOpt: {
	/*
	 * Retrieve the value of one option.
	 */

	Tcl_Obj *optionObj = Tcl_NewStringObj(paramPtr->getOpt.name, -1);

	if (InvokeTclMethod(rcPtr, "cget", optionObj, NULL, &resObj,
		0) != TCL_OK) {
	    ForwardSetObjError(paramPtr, resObj);
	} else {
	    Tcl_DStringAppend(paramPtr->getOpt.value, TclGetString(resObj),-1);
	}
	break;
    }

    case ForwardedGetOptAll:
	/*
	 * Retrieve all options.
	 */

	if (InvokeTclMethod(rcPtr, "cgetall", NULL, NULL, &resObj,
		0) != TCL_OK) {
	    ForwardSetObjError(paramPtr, resObj);
	} else {
	    /*
	     * Extract list, validate that it is a list, and #elements. See
	     * NOTE (4) as well.
	     */

	    int listc;
	    Tcl_Obj** listv;

	    if (Tcl_ListObjGetElements(interp, resObj, &listc,
		    &listv) != TCL_OK) {
		ForwardSetObjError(paramPtr, MarshallError(interp));
	    } else if ((listc % 2) == 1) {
		/*
		 * Odd number of elements is wrong. [x].
		 */

		char *buf = ckalloc(200);
		sprintf(buf,
			"{Expected list with even number of elements, got %d %s instead}",
			listc, (listc == 1 ? "element" : "elements"));

		ForwardSetDynamicError(paramPtr, buf);
	    } else {
		int len;
		CONST char *str = Tcl_GetStringFromObj(resObj, &len);

		if (len) {
		    Tcl_DStringAppend(paramPtr->getOpt.value, " ", 1);
		    Tcl_DStringAppend(paramPtr->getOpt.value, str, len);
		}
	    }
	}
	break;

    default:
	/*
	 * Bad operation code.
	 */
	Tcl_Panic("Bad operation code in ForwardProc");
	break;
    }

    /*
     * Remove the reference we held on the result of the invoke, if we had
     * such.
     */

    if (resObj != NULL) {
	Tcl_DecrRefCount(resObj);
    }

    if (resultPtr) {
	/*
	 * Report the forwarding result synchronously to the waiting caller.
	 * This unblocks (*) as well. This is wrapped into a conditional
	 * because the caller may have exited in the mean time.
	 */

	Tcl_MutexLock(&rcForwardMutex);
	resultPtr->result = TCL_OK;
	Tcl_ConditionNotify(&resultPtr->done);
	Tcl_MutexUnlock(&rcForwardMutex);
    }

    return 1;
}

static void
SrcExitProc(
    ClientData clientData)
{
    ForwardingEvent *evPtr = (ForwardingEvent *) clientData;
    ForwardingResult *resultPtr;
    ForwardParam *paramPtr;

    /*
     * NOTE (2): Can this handler be called with the originator blocked?
     */

    /*
     * The originator for the event exited. It is not sure if this can happen,
     * as the originator should be blocked at (*) while the event is in
     * transit/pending.
     *
     * We make sure that the event cannot refer to the result anymore, remove
     * it from the list of pending results and free the structure. Locking the
     * access ensures that we cannot get in conflict with "ForwardProc",
     * should it already execute the event.
     */

    Tcl_MutexLock(&rcForwardMutex);

    resultPtr = evPtr->resultPtr;
    paramPtr = evPtr->param;

    evPtr->resultPtr = NULL;
    resultPtr->evPtr = NULL;
    resultPtr->result = TCL_ERROR;

    ForwardSetStaticError(paramPtr, msg_send_originlost);

    /*
     * See below: TclSpliceOut(resultPtr, forwardList);
     */

    Tcl_MutexUnlock(&rcForwardMutex);

    /*
     * This unlocks (*). The structure will be spliced out and freed by
     * "ForwardProc". Maybe.
     */

    Tcl_ConditionNotify(&resultPtr->done);
}

static void
DstExitProc(
    ClientData clientData)
{
    ForwardingEvent *evPtr = (ForwardingEvent *) clientData;
    ForwardingResult *resultPtr = evPtr->resultPtr;
    ForwardParam *paramPtr = evPtr->param;

    /*
     * NOTE (3): It is not clear if the event still exists when this handler
     * is called. We might have to use 'resultPtr' as our clientData instead.
     */

    /*
     * The receiver for the event exited, before processing the event. We
     * detach the result now, wake the originator up and signal failure.
     */

    evPtr->resultPtr = NULL;
    resultPtr->evPtr = NULL;
    resultPtr->result = TCL_ERROR;

    ForwardSetStaticError(paramPtr, msg_send_dstlost);

    Tcl_ConditionNotify(&resultPtr->done);
}

static void
ForwardSetObjError(
    ForwardParam *paramPtr,
    Tcl_Obj *obj)
{
    int len;
    CONST char *msgStr = Tcl_GetStringFromObj(obj, &len);

    len++;
    ForwardSetDynamicError(paramPtr, ckalloc((unsigned) len));
    memcpy(paramPtr->base.msgStr, msgStr, (unsigned) len);
}
#endif

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
