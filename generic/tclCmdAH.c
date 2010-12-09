/*
 * tclCmdAH.c --
 *
 *	This file contains the top-level command routines for most of the Tcl
 *	built-in commands whose names begin with the letters A to H.
 *
 * Copyright (c) 1987-1993 The Regents of the University of California.
 * Copyright (c) 1994-1997 Sun Microsystems, Inc.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id$
 */

#include "tclInt.h"
#include <locale.h>

/*
 * The state structure used by [foreach]. Note that the actual structure has
 * all its working arrays appended afterwards so they can be allocated and
 * freed in a single step.
 */

struct ForeachState {
    Tcl_Obj *bodyPtr;		/* The script body of the command. */
    int bodyIdx;		/* The argument index of the body. */
    int j, maxj;		/* Number of loop iterations. */
    int numLists;		/* Count of value lists. */
    int *index;			/* Array of value list indices. */
    int *varcList;		/* # loop variables per list. */
    Tcl_Obj ***varvList;	/* Array of var name lists. */
    Tcl_Obj **vCopyList;	/* Copies of var name list arguments. */
    int *argcList;		/* Array of value list sizes. */
    Tcl_Obj ***argvList;	/* Array of value lists. */
    Tcl_Obj **aCopyList;	/* Copies of value list arguments. */
};

/*
 * Prototypes for local procedures defined in this file:
 */

static int		CheckAccess(Tcl_Interp *interp, Tcl_Obj *pathPtr,
			    int mode);
static int		EncodingDirsObjCmd(ClientData dummy,
			    Tcl_Interp *interp, int objc,
			    Tcl_Obj *const objv[]);
static inline int	ForeachAssignments(Tcl_Interp *interp,
			    struct ForeachState *statePtr);
static inline void	ForeachCleanup(Tcl_Interp *interp,
			    struct ForeachState *statePtr);
static int		GetStatBuf(Tcl_Interp *interp, Tcl_Obj *pathPtr,
			    Tcl_FSStatProc *statProc, Tcl_StatBuf *statPtr);
static const char *	GetTypeFromMode(int mode);
static int		StoreStatData(Tcl_Interp *interp, Tcl_Obj *varName,
			    Tcl_StatBuf *statPtr);
static Tcl_NRPostProc	CatchObjCmdCallback;
static Tcl_NRPostProc	ExprCallback;
static Tcl_NRPostProc	ForSetupCallback;
static Tcl_NRPostProc	ForCondCallback;
static Tcl_NRPostProc	ForNextCallback;
static Tcl_NRPostProc	ForPostNextCallback;
static Tcl_NRPostProc	ForeachLoopStep;
static Tcl_NRPostProc	EvalCmdErrMsg;

static Tcl_ObjCmdProc FileAttrAccessTimeCmd;
static Tcl_ObjCmdProc FileAttrIsDirectoryCmd;
static Tcl_ObjCmdProc FileAttrIsExecutableCmd;
static Tcl_ObjCmdProc FileAttrIsExistingCmd;
static Tcl_ObjCmdProc FileAttrIsFileCmd;
static Tcl_ObjCmdProc FileAttrIsOwnedCmd;
static Tcl_ObjCmdProc FileAttrIsReadableCmd;
static Tcl_ObjCmdProc FileAttrIsWritableCmd;
static Tcl_ObjCmdProc FileAttrLinkStatCmd;
static Tcl_ObjCmdProc FileAttrModifyTimeCmd;
static Tcl_ObjCmdProc FileAttrSizeCmd;
static Tcl_ObjCmdProc FileAttrStatCmd;
static Tcl_ObjCmdProc FileAttrTypeCmd;
static Tcl_ObjCmdProc FilesystemSeparatorCmd;
static Tcl_ObjCmdProc FilesystemVolumesCmd;
static Tcl_ObjCmdProc PathDirNameCmd;
static Tcl_ObjCmdProc PathExtensionCmd;
static Tcl_ObjCmdProc PathFilesystemCmd;
static Tcl_ObjCmdProc PathJoinCmd;
static Tcl_ObjCmdProc PathNativeNameCmd;
static Tcl_ObjCmdProc PathNormalizeCmd;
static Tcl_ObjCmdProc PathRootNameCmd;
static Tcl_ObjCmdProc PathSplitCmd;
static Tcl_ObjCmdProc PathTailCmd;
static Tcl_ObjCmdProc PathTypeCmd;

/*
 *----------------------------------------------------------------------
 *
 * Tcl_BreakObjCmd --
 *
 *	This procedure is invoked to process the "break" Tcl command. See the
 *	user documentation for details on what it does.
 *
 *	With the bytecode compiler, this procedure is only called when a
 *	command name is computed at runtime, and is "break" or the name to
 *	which "break" was renamed: e.g., "set z break; $z"
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
Tcl_BreakObjCmd(
    ClientData dummy,		/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    if (objc != 1) {
	Tcl_WrongNumArgs(interp, 1, objv, NULL);
	return TCL_ERROR;
    }
    return TCL_BREAK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_CaseObjCmd --
 *
 *	This procedure is invoked to process the "case" Tcl command. See the
 *	user documentation for details on what it does. THIS COMMAND IS
 *	OBSOLETE AND DEPRECATED. SLATED FOR REMOVAL IN TCL 9.0.
 *
 * Results:
 *	A standard Tcl object result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
Tcl_CaseObjCmd(
    ClientData dummy,		/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    register int i;
    int body, result, caseObjc;
    const char *stringPtr, *arg;
    Tcl_Obj *const *caseObjv;
    Tcl_Obj *armPtr;

    if (objc < 3) {
	Tcl_WrongNumArgs(interp, 1, objv,
		"string ?in? ?pattern body ...? ?default body?");
	return TCL_ERROR;
    }

    stringPtr = TclGetString(objv[1]);
    body = -1;

    arg = TclGetString(objv[2]);
    if (strcmp(arg, "in") == 0) {
	i = 3;
    } else {
	i = 2;
    }
    caseObjc = objc - i;
    caseObjv = objv + i;

    /*
     * If all of the pattern/command pairs are lumped into a single argument,
     * split them out again.
     */

    if (caseObjc == 1) {
	Tcl_Obj **newObjv;

	TclListObjGetElements(interp, caseObjv[0], &caseObjc, &newObjv);
	caseObjv = newObjv;
    }

    for (i = 0;  i < caseObjc;  i += 2) {
	int patObjc, j;
	const char **patObjv;
	const char *pat;
	unsigned char *p;

	if (i == caseObjc-1) {
	    Tcl_ResetResult(interp);
	    Tcl_AppendResult(interp, "extra case pattern with no body", NULL);
	    return TCL_ERROR;
	}

	/*
	 * Check for special case of single pattern (no list) with no
	 * backslash sequences.
	 */

	pat = TclGetString(caseObjv[i]);
	for (p = (unsigned char *) pat; *p != '\0'; p++) {
	    if (isspace(*p) || (*p == '\\')) {	/* INTL: ISO space, UCHAR */
		break;
	    }
	}
	if (*p == '\0') {
	    if ((*pat == 'd') && (strcmp(pat, "default") == 0)) {
		body = i + 1;
	    }
	    if (Tcl_StringMatch(stringPtr, pat)) {
		body = i + 1;
		goto match;
	    }
	    continue;
	}

	/*
	 * Break up pattern lists, then check each of the patterns in the
	 * list.
	 */

	result = Tcl_SplitList(interp, pat, &patObjc, &patObjv);
	if (result != TCL_OK) {
	    return result;
	}
	for (j = 0; j < patObjc; j++) {
	    if (Tcl_StringMatch(stringPtr, patObjv[j])) {
		body = i + 1;
		break;
	    }
	}
	ckfree((char *) patObjv);
	if (j < patObjc) {
	    break;
	}
    }

  match:
    if (body != -1) {
	armPtr = caseObjv[body - 1];
	result = Tcl_EvalObjEx(interp, caseObjv[body], 0);
	if (result == TCL_ERROR) {
	    Tcl_AppendObjToErrorInfo(interp, Tcl_ObjPrintf(
		    "\n    (\"%.50s\" arm line %d)",
		    TclGetString(armPtr), Tcl_GetErrorLine(interp)));
	}
	return result;
    }

    /*
     * Nothing matched: return nothing.
     */

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_CatchObjCmd --
 *
 *	This object-based procedure is invoked to process the "catch" Tcl
 *	command. See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl object result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
Tcl_CatchObjCmd(
    ClientData dummy,		/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    return Tcl_NRCallObjProc(interp, TclNRCatchObjCmd, dummy, objc, objv);
}

int
TclNRCatchObjCmd(
    ClientData dummy,		/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    Tcl_Obj *varNamePtr = NULL;
    Tcl_Obj *optionVarNamePtr = NULL;
    Interp *iPtr = (Interp *) interp;

    if ((objc < 2) || (objc > 4)) {
	Tcl_WrongNumArgs(interp, 1, objv,
		"script ?resultVarName? ?optionVarName?");
	return TCL_ERROR;
    }

    if (objc >= 3) {
	varNamePtr = objv[2];
    }
    if (objc == 4) {
	optionVarNamePtr = objv[3];
    }

    TclNRAddCallback(interp, CatchObjCmdCallback, INT2PTR(objc),
	    varNamePtr, optionVarNamePtr, NULL);

    /*
     * TIP #280. Make invoking context available to caught script.
     */

    return TclNREvalObjEx(interp, objv[1], 0, iPtr->cmdFramePtr, 1);
}

static int
CatchObjCmdCallback(
    ClientData data[],
    Tcl_Interp *interp,
    int result)
{
    Interp *iPtr = (Interp *) interp;
    int objc = PTR2INT(data[0]);
    Tcl_Obj *varNamePtr = data[1];
    Tcl_Obj *optionVarNamePtr = data[2];
    int rewind = iPtr->execEnvPtr->rewind;

    /*
     * We disable catch in interpreters where the limit has been exceeded.
     */

    if (rewind || Tcl_LimitExceeded(interp)) {
	Tcl_AppendObjToErrorInfo(interp, Tcl_ObjPrintf(
		"\n    (\"catch\" body line %d)", Tcl_GetErrorLine(interp)));
	return TCL_ERROR;
    }

    if (objc >= 3) {
	if (NULL == Tcl_ObjSetVar2(interp, varNamePtr, NULL,
		Tcl_GetObjResult(interp), 0)) {
	    Tcl_ResetResult(interp);
	    Tcl_AppendResult(interp,
		    "couldn't save command result in variable", NULL);
	    return TCL_ERROR;
	}
    }
    if (objc == 4) {
	Tcl_Obj *options = Tcl_GetReturnOptions(interp, result);

	if (NULL == Tcl_ObjSetVar2(interp, optionVarNamePtr, NULL,
		options, 0)) {
	    Tcl_DecrRefCount(options);
	    Tcl_ResetResult(interp);
	    Tcl_AppendResult(interp,
		    "couldn't save return options in variable", NULL);
	    return TCL_ERROR;
	}
    }

    Tcl_ResetResult(interp);
    Tcl_SetObjResult(interp, Tcl_NewIntObj(result));
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_CdObjCmd --
 *
 *	This procedure is invoked to process the "cd" Tcl command. See the
 *	user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
Tcl_CdObjCmd(
    ClientData dummy,		/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    Tcl_Obj *dir;
    int result;

    if (objc > 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "?dirName?");
	return TCL_ERROR;
    }

    if (objc == 2) {
	dir = objv[1];
    } else {
	TclNewLiteralStringObj(dir, "~");
	Tcl_IncrRefCount(dir);
    }
    if (Tcl_FSConvertToPathType(interp, dir) != TCL_OK) {
	result = TCL_ERROR;
    } else {
	result = Tcl_FSChdir(dir);
	if (result != TCL_OK) {
	    Tcl_AppendResult(interp, "couldn't change working directory to \"",
		    TclGetString(dir), "\": ", Tcl_PosixError(interp), NULL);
	    result = TCL_ERROR;
	}
    }
    if (objc != 2) {
	Tcl_DecrRefCount(dir);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ConcatObjCmd --
 *
 *	This object-based procedure is invoked to process the "concat" Tcl
 *	command. See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl object result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
Tcl_ConcatObjCmd(
    ClientData dummy,		/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    if (objc >= 2) {
	Tcl_SetObjResult(interp, Tcl_ConcatObj(objc-1, objv+1));
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ContinueObjCmd --
 *
 *	This procedure is invoked to process the "continue" Tcl command. See
 *	the user documentation for details on what it does.
 *
 *	With the bytecode compiler, this procedure is only called when a
 *	command name is computed at runtime, and is "continue" or the name to
 *	which "continue" was renamed: e.g., "set z continue; $z"
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
Tcl_ContinueObjCmd(
    ClientData dummy,		/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    if (objc != 1) {
	Tcl_WrongNumArgs(interp, 1, objv, NULL);
	return TCL_ERROR;
    }
    return TCL_CONTINUE;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_EncodingObjCmd --
 *
 *	This command manipulates encodings.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_EncodingObjCmd(
    ClientData dummy,		/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    int index;

    static const char *const optionStrings[] = {
	"convertfrom", "convertto", "dirs", "names", "system",
	NULL
    };
    enum options {
	ENC_CONVERTFROM, ENC_CONVERTTO, ENC_DIRS, ENC_NAMES, ENC_SYSTEM
    };

    if (objc < 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "option ?arg ...?");
	return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], optionStrings, "option", 0,
	    &index) != TCL_OK) {
	return TCL_ERROR;
    }

    switch ((enum options) index) {
    case ENC_CONVERTTO:
    case ENC_CONVERTFROM: {
	Tcl_Obj *data;
	Tcl_DString ds;
	Tcl_Encoding encoding;
	int length;
	const char *stringPtr;

	if (objc == 3) {
	    encoding = Tcl_GetEncoding(interp, NULL);
	    data = objv[2];
	} else if (objc == 4) {
	    if (Tcl_GetEncodingFromObj(interp, objv[2], &encoding) != TCL_OK) {
		return TCL_ERROR;
	    }
	    data = objv[3];
	} else {
	    Tcl_WrongNumArgs(interp, 2, objv, "?encoding? data");
	    return TCL_ERROR;
	}

	if ((enum options) index == ENC_CONVERTFROM) {
	    /*
	     * Treat the string as binary data.
	     */

	    stringPtr = (char *) Tcl_GetByteArrayFromObj(data, &length);
	    Tcl_ExternalToUtfDString(encoding, stringPtr, length, &ds);

	    /*
	     * Note that we cannot use Tcl_DStringResult here because it will
	     * truncate the string at the first null byte.
	     */

	    Tcl_SetObjResult(interp, Tcl_NewStringObj(
		    Tcl_DStringValue(&ds), Tcl_DStringLength(&ds)));
	    Tcl_DStringFree(&ds);
	} else {
	    /*
	     * Store the result as binary data.
	     */

	    stringPtr = TclGetStringFromObj(data, &length);
	    Tcl_UtfToExternalDString(encoding, stringPtr, length, &ds);
	    Tcl_SetObjResult(interp, Tcl_NewByteArrayObj(
		    (unsigned char *) Tcl_DStringValue(&ds),
		    Tcl_DStringLength(&ds)));
	    Tcl_DStringFree(&ds);
	}

	Tcl_FreeEncoding(encoding);
	break;
    }
    case ENC_DIRS:
	return EncodingDirsObjCmd(dummy, interp, objc-1, objv+1);
    case ENC_NAMES:
	if (objc > 2) {
	    Tcl_WrongNumArgs(interp, 2, objv, NULL);
	    return TCL_ERROR;
	}
	Tcl_GetEncodingNames(interp);
	break;
    case ENC_SYSTEM:
	if (objc > 3) {
	    Tcl_WrongNumArgs(interp, 2, objv, "?encoding?");
	    return TCL_ERROR;
	}
	if (objc == 2) {
	    Tcl_SetObjResult(interp, Tcl_NewStringObj(
		    Tcl_GetEncodingName(NULL), -1));
	} else {
	    return Tcl_SetSystemEncoding(interp, TclGetString(objv[2]));
	}
	break;
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * EncodingDirsObjCmd --
 *
 *	This command manipulates the encoding search path.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	Can set the encoding search path.
 *
 *----------------------------------------------------------------------
 */

int
EncodingDirsObjCmd(
    ClientData dummy,		/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    if (objc > 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "?dirList?");
	return TCL_ERROR;
    }
    if (objc == 1) {
	Tcl_SetObjResult(interp, Tcl_GetEncodingSearchPath());
	return TCL_OK;
    }
    if (Tcl_SetEncodingSearchPath(objv[1]) == TCL_ERROR) {
	Tcl_AppendResult(interp, "expected directory list but got \"",
		TclGetString(objv[1]), "\"", NULL);
	return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, objv[1]);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ErrorObjCmd --
 *
 *	This procedure is invoked to process the "error" Tcl command. See the
 *	user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl object result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
Tcl_ErrorObjCmd(
    ClientData dummy,		/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    Tcl_Obj *options, *optName;

    if ((objc < 2) || (objc > 4)) {
	Tcl_WrongNumArgs(interp, 1, objv, "message ?errorInfo? ?errorCode?");
	return TCL_ERROR;
    }

    TclNewLiteralStringObj(options, "-code error -level 0");

    if (objc >= 3) {		/* Process the optional info argument */
	TclNewLiteralStringObj(optName, "-errorinfo");
	Tcl_ListObjAppendElement(NULL, options, optName);
	Tcl_ListObjAppendElement(NULL, options, objv[2]);
    }

    if (objc >= 4) {		/* Process the optional code argument */
	TclNewLiteralStringObj(optName, "-errorcode");
	Tcl_ListObjAppendElement(NULL, options, optName);
	Tcl_ListObjAppendElement(NULL, options, objv[3]);
    }

    Tcl_SetObjResult(interp, objv[1]);
    return Tcl_SetReturnOptions(interp, options);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_EvalObjCmd --
 *
 *	This object-based procedure is invoked to process the "eval" Tcl
 *	command. See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl object result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
static int
EvalCmdErrMsg(
    ClientData data[],
    Tcl_Interp *interp,
    int result)
{
    if (result == TCL_ERROR) {
	Tcl_AppendObjToErrorInfo(interp, Tcl_ObjPrintf(
		"\n    (\"eval\" body line %d)", Tcl_GetErrorLine(interp)));
    }
    return result;
}

int
Tcl_EvalObjCmd(
    ClientData dummy,		/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    register Tcl_Obj *objPtr;
    Interp *iPtr = (Interp *) interp;
    CmdFrame *invoker = NULL;
    int word = 0;

    if (objc < 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "arg ?arg ...?");
	return TCL_ERROR;
    }

    if (objc == 2) {
	/*
	 * TIP #280. Make argument location available to eval'd script.
	 */

	invoker = iPtr->cmdFramePtr;
	word = 1;
	objPtr = objv[1];
	TclArgumentGet(interp, objPtr, &invoker, &word);
    } else {
	/*
	 * More than one argument: concatenate them together with spaces
	 * between, then evaluate the result. Tcl_EvalObjEx will delete the
	 * object when it decrements its refcount after eval'ing it.
	 *
	 * TIP #280. Make invoking context available to eval'd script, done
	 * with the default values.
	 */

	objPtr = Tcl_ConcatObj(objc-1, objv+1);
    }
    TclNRAddCallback(interp, EvalCmdErrMsg, NULL, NULL, NULL, NULL);
    return TclNREvalObjEx(interp, objPtr, 0, invoker, word);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ExitObjCmd --
 *
 *	This procedure is invoked to process the "exit" Tcl command. See the
 *	user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl object result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
Tcl_ExitObjCmd(
    ClientData dummy,		/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    int value;

    if ((objc != 1) && (objc != 2)) {
	Tcl_WrongNumArgs(interp, 1, objv, "?returnCode?");
	return TCL_ERROR;
    }

    if (objc == 1) {
	value = 0;
    } else if (Tcl_GetIntFromObj(interp, objv[1], &value) != TCL_OK) {
	return TCL_ERROR;
    }
    Tcl_Exit(value);
    /*NOTREACHED*/
    return TCL_OK;		/* Better not ever reach this! */
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ExprObjCmd --
 *
 *	This object-based procedure is invoked to process the "expr" Tcl
 *	command. See the user documentation for details on what it does.
 *
 *	With the bytecode compiler, this procedure is called in two
 *	circumstances: 1) to execute expr commands that are too complicated or
 *	too unsafe to try compiling directly into an inline sequence of
 *	instructions, and 2) to execute commands where the command name is
 *	computed at runtime and is "expr" or the name to which "expr" was
 *	renamed (e.g., "set z expr; $z 2+3")
 *
 * Results:
 *	A standard Tcl object result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
Tcl_ExprObjCmd(
    ClientData dummy,		/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    return Tcl_NRCallObjProc(interp, TclNRExprObjCmd, dummy, objc, objv);
}

int
TclNRExprObjCmd(
    ClientData dummy,		/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    Tcl_Obj *resultPtr, *objPtr;

    if (objc < 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "arg ?arg ...?");
	return TCL_ERROR;
    }

    TclNewObj(resultPtr);
    Tcl_IncrRefCount(resultPtr);
    if (objc == 2) {
	objPtr = objv[1];
	TclNRAddCallback(interp, ExprCallback, resultPtr, NULL, NULL, NULL);
    } else {
	objPtr = Tcl_ConcatObj(objc-1, objv+1);
	TclNRAddCallback(interp, ExprCallback, resultPtr, objPtr, NULL, NULL);
    }

    return Tcl_NRExprObj(interp, objPtr, resultPtr);
}

static int
ExprCallback(
    ClientData data[],
    Tcl_Interp *interp,
    int result)
{
    Tcl_Obj *resultPtr = data[0];
    Tcl_Obj *objPtr = data[1];

    if (objPtr != NULL) {
	Tcl_DecrRefCount(objPtr);
    }

    if (result == TCL_OK) {
	Tcl_SetObjResult(interp, resultPtr);
    }
    Tcl_DecrRefCount(resultPtr);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * TclInitFileCmd --
 *
 *	This function builds the "file" Tcl command ensemble. See the user
 *	documentation for details on what that ensemble does.
 *
 *	PLEASE NOTE THAT THIS FAILS WITH FILENAMES AND PATHS WITH EMBEDDED
 *	NULLS. With the object-based Tcl_FS APIs, the above NOTE may no longer
 *	be true. In any case this assertion should be tested.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *----------------------------------------------------------------------
 */

Tcl_Command
TclInitFileCmd(
    Tcl_Interp *interp)
{
    /*
     * Note that most subcommands are unsafe because either they manipulate
     * the native filesystem or because they reveal information about the
     * native filesystem.
     */

    static const EnsembleImplMap initMap[] = {
	{"atime",	FileAttrAccessTimeCmd},
	{"attributes",	TclFileAttrsCmd},
	{"channels",	TclChannelNamesCmd},
	{"copy",	TclFileCopyCmd},
	{"delete",	TclFileDeleteCmd},
	{"dirname",	PathDirNameCmd},
	{"executable",	FileAttrIsExecutableCmd},
	{"exists",	FileAttrIsExistingCmd},
	{"extension",	PathExtensionCmd},
	{"isdirectory",	FileAttrIsDirectoryCmd},
	{"isfile",	FileAttrIsFileCmd},
	{"join",	PathJoinCmd},
	{"link",	TclFileLinkCmd},
	{"lstat",	FileAttrLinkStatCmd},
	{"mtime",	FileAttrModifyTimeCmd},
	{"mkdir",	TclFileMakeDirsCmd},
	{"nativename",	PathNativeNameCmd},
	{"normalize",	PathNormalizeCmd},
	{"owned",	FileAttrIsOwnedCmd},
	{"pathtype",	PathTypeCmd},
	{"readable",	FileAttrIsReadableCmd},
	{"readlink",	TclFileReadLinkCmd},
	{"rename",	TclFileRenameCmd},
	{"rootname",	PathRootNameCmd},
	{"separator",	FilesystemSeparatorCmd},
	{"size",	FileAttrSizeCmd},
	{"split",	PathSplitCmd},
	{"stat",	FileAttrStatCmd},
	{"system",	PathFilesystemCmd},
	{"tail",	PathTailCmd},
	{"tempfile",	TclFileTemporaryCmd},
	{"type",	FileAttrTypeCmd},
	{"volumes",	FilesystemVolumesCmd},
	{"writable",	FileAttrIsWritableCmd},
	{NULL}
    };
    return TclMakeEnsemble(interp, "file", initMap);
}

/*
 *----------------------------------------------------------------------
 *
 * TclMakeFileCommandSafe --
 *
 *	This function hides the unsafe subcommands of the "file" Tcl command
 *	ensemble. It must only be called from TclHideUnsafeCommands.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	Adds commands to the table of hidden commands.
 *
 *----------------------------------------------------------------------
 */

int
TclMakeFileCommandSafe(
    Tcl_Interp *interp)
{
    static const struct {
	const char *cmdName;
	int unsafe;
    } unsafeInfo[] = {
	{"atime",	 1},
	{"attributes",	 1},
	{"channels",	 0},
	{"copy",	 1},
	{"delete",	 1},
	{"dirname",	 1},
	{"executable",	 1},
	{"exists",	 1},
	{"extension",	 1},
	{"isdirectory",	 1},
	{"isfile",	 1},
	{"join",	 0},
	{"link",	 1},
	{"lstat",	 1},
	{"mtime",	 1},
	{"mkdir",	 1},
	{"nativename",	 1},
	{"normalize",	 1},
	{"owned",	 1},
	{"pathtype",	 0},
	{"readable",	 1},
	{"readlink",	 1},
	{"rename",	 1},
	{"rootname",	 1},
	{"separator",	 0},
	{"size",	 1},
	{"split",	 0},
	{"stat",	 1},
	{"system",	 0},
	{"tail",	 1},
	{"tempfile",	 1},
	{"type",	 1},
	{"volumes",	 1},
	{"writable",	 1},
	{NULL}
    };
    int i;
    Tcl_DString oldBuf, newBuf;

    Tcl_DStringInit(&oldBuf);
    Tcl_DStringAppend(&oldBuf, "::tcl::file::", -1);
    Tcl_DStringInit(&newBuf);
    Tcl_DStringAppend(&newBuf, "tcl:file:", -1);
    for (i=0 ; unsafeInfo[i].cmdName != NULL ; i++) {
	if (unsafeInfo[i].unsafe) {
	    const char *oldName, *newName;

	    Tcl_DStringSetLength(&oldBuf, 13);
	    oldName = Tcl_DStringAppend(&oldBuf, unsafeInfo[i].cmdName, -1);
	    Tcl_DStringSetLength(&newBuf, 9);
	    newName = Tcl_DStringAppend(&newBuf, unsafeInfo[i].cmdName, -1);
	    if (TclRenameCommand(interp, oldName, "___tmp") != TCL_OK
		    || Tcl_HideCommand(interp, "___tmp", newName) != TCL_OK) {
		Tcl_Panic("problem making 'file %s' safe: %s",
			unsafeInfo[i].cmdName,
			Tcl_GetString(Tcl_GetObjResult(interp)));
	    }
	}
    }
    Tcl_DStringFree(&oldBuf);
    Tcl_DStringFree(&newBuf);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * FileAttrAccessTimeCmd --
 *
 *	This function is invoked to process the "file atime" Tcl command. See
 *	the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	May update the access time on the file, if requested by the user.
 *
 *----------------------------------------------------------------------
 */

static int
FileAttrAccessTimeCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    Tcl_StatBuf buf;
    struct utimbuf tval;

    if (objc < 2 || objc > 3) {
	Tcl_WrongNumArgs(interp, 1, objv, "name ?time?");
	return TCL_ERROR;
    }
    if (GetStatBuf(interp, objv[1], Tcl_FSStat, &buf) != TCL_OK) {
	return TCL_ERROR;
    }
    if (objc == 3) {
	/*
	 * Need separate variable for reading longs from an object on 64-bit
	 * platforms. [Bug 698146]
	 */

	long newTime;

	if (TclGetLongFromObj(interp, objv[2], &newTime) != TCL_OK) {
	    return TCL_ERROR;
	}

	tval.actime = newTime;
	tval.modtime = buf.st_mtime;

	if (Tcl_FSUtime(objv[1], &tval) != 0) {
	    Tcl_AppendResult(interp, "could not set access time for file \"",
		    TclGetString(objv[1]), "\": ", Tcl_PosixError(interp),
		    NULL);
	    return TCL_ERROR;
	}

	/*
	 * Do another stat to ensure that the we return the new recognized
	 * atime - hopefully the same as the one we sent in. However, fs's
	 * like FAT don't even know what atime is.
	 */

	if (GetStatBuf(interp, objv[1], Tcl_FSStat, &buf) != TCL_OK) {
	    return TCL_ERROR;
	}
    }

    Tcl_SetObjResult(interp, Tcl_NewLongObj((long) buf.st_atime));
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * FileAttrModifyTimeCmd --
 *
 *	This function is invoked to process the "file mtime" Tcl command. See
 *	the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	May update the modification time on the file, if requested by the
 *	user.
 *
 *----------------------------------------------------------------------
 */

static int
FileAttrModifyTimeCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    Tcl_StatBuf buf;
    struct utimbuf tval;

    if (objc < 2 || objc > 3) {
	Tcl_WrongNumArgs(interp, 1, objv, "name ?time?");
	return TCL_ERROR;
    }
    if (GetStatBuf(interp, objv[1], Tcl_FSStat, &buf) != TCL_OK) {
	return TCL_ERROR;
    }
    if (objc == 3) {
	/*
	 * Need separate variable for reading longs from an object on 64-bit
	 * platforms. [Bug 698146]
	 */

	long newTime;

	if (TclGetLongFromObj(interp, objv[2], &newTime) != TCL_OK) {
	    return TCL_ERROR;
	}

	tval.actime = buf.st_atime;
	tval.modtime = newTime;

	if (Tcl_FSUtime(objv[1], &tval) != 0) {
	    Tcl_AppendResult(interp, "could not set modification time for "
		    "file \"", TclGetString(objv[1]), "\": ",
		    Tcl_PosixError(interp), NULL);
	    return TCL_ERROR;
	}

	/*
	 * Do another stat to ensure that the we return the new recognized
	 * mtime - hopefully the same as the one we sent in.
	 */

	if (GetStatBuf(interp, objv[1], Tcl_FSStat, &buf) != TCL_OK) {
	    return TCL_ERROR;
	}
    }

    Tcl_SetObjResult(interp, Tcl_NewLongObj((long) buf.st_mtime));
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * FileAttrLinkStatCmd --
 *
 *	This function is invoked to process the "file lstat" Tcl command. See
 *	the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	Writes to an array named by the user.
 *
 *----------------------------------------------------------------------
 */

static int
FileAttrLinkStatCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    Tcl_StatBuf buf;

    if (objc != 3) {
	Tcl_WrongNumArgs(interp, 1, objv, "name varName");
	return TCL_ERROR;
    }
    if (GetStatBuf(interp, objv[1], Tcl_FSLstat, &buf) != TCL_OK) {
	return TCL_ERROR;
    }
    return StoreStatData(interp, objv[2], &buf);
}

/*
 *----------------------------------------------------------------------
 *
 * FileAttrStatCmd --
 *
 *	This function is invoked to process the "file stat" Tcl command. See
 *	the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	Writes to an array named by the user.
 *
 *----------------------------------------------------------------------
 */

static int
FileAttrStatCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    Tcl_StatBuf buf;

    if (objc != 3) {
	Tcl_WrongNumArgs(interp, 1, objv, "name varName");
	return TCL_ERROR;
    }
    if (GetStatBuf(interp, objv[1], Tcl_FSStat, &buf) != TCL_OK) {
	return TCL_ERROR;
    }
    return StoreStatData(interp, objv[2], &buf);
}

/*
 *----------------------------------------------------------------------
 *
 * FileAttrTypeCmd --
 *
 *	This function is invoked to process the "file type" Tcl command. See
 *	the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
FileAttrTypeCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    Tcl_StatBuf buf;

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "name");
	return TCL_ERROR;
    }
    if (GetStatBuf(interp, objv[1], Tcl_FSLstat, &buf) != TCL_OK) {
	return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, Tcl_NewStringObj(
	    GetTypeFromMode((unsigned short) buf.st_mode), -1));
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * FileAttrSizeCmd --
 *
 *	This function is invoked to process the "file size" Tcl command. See
 *	the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
FileAttrSizeCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    Tcl_StatBuf buf;

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "name");
	return TCL_ERROR;
    }
    if (GetStatBuf(interp, objv[1], Tcl_FSStat, &buf) != TCL_OK) {
	return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, Tcl_NewWideIntObj((Tcl_WideInt) buf.st_size));
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * FileAttrIsDirectoryCmd --
 *
 *	This function is invoked to process the "file isdirectory" Tcl
 *	command. See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
FileAttrIsDirectoryCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    Tcl_StatBuf buf;
    int value = 0;

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "name");
	return TCL_ERROR;
    }
    if (GetStatBuf(NULL, objv[1], Tcl_FSStat, &buf) == TCL_OK) {
	value = S_ISDIR(buf.st_mode);
    }
    Tcl_SetObjResult(interp, Tcl_NewBooleanObj(value));
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * FileAttrIsExecutableCmd --
 *
 *	This function is invoked to process the "file executable" Tcl command.
 *	See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
FileAttrIsExecutableCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "name");
	return TCL_ERROR;
    }
    return CheckAccess(interp, objv[1], X_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * FileAttrIsExistingCmd --
 *
 *	This function is invoked to process the "file exists" Tcl command. See
 *	the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
FileAttrIsExistingCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "name");
	return TCL_ERROR;
    }
    return CheckAccess(interp, objv[1], F_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * FileAttrIsFileCmd --
 *
 *	This function is invoked to process the "file isfile" Tcl command. See
 *	the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
FileAttrIsFileCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    Tcl_StatBuf buf;
    int value = 0;

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "name");
	return TCL_ERROR;
    }
    if (GetStatBuf(NULL, objv[1], Tcl_FSStat, &buf) == TCL_OK) {
	value = S_ISREG(buf.st_mode);
    }
    Tcl_SetObjResult(interp, Tcl_NewBooleanObj(value));
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * FileAttrIsOwnedCmd --
 *
 *	This function is invoked to process the "file owned" Tcl command. See
 *	the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
FileAttrIsOwnedCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    Tcl_StatBuf buf;
    int value = 0;

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "name");
	return TCL_ERROR;
    }
    if (GetStatBuf(NULL, objv[1], Tcl_FSStat, &buf) == TCL_OK) {
	/*
	 * For Windows, there are no user ids associated with a file, so we
	 * always return 1.
	 *
	 * TODO: use GetSecurityInfo to get the real owner of the file and
	 * test for equivalence to the current user.
	 */

#ifdef __WIN32__
	value = 1;
#else
	value = (geteuid() == buf.st_uid);
#endif
    }
    Tcl_SetObjResult(interp, Tcl_NewBooleanObj(value));
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * FileAttrIsReadableCmd --
 *
 *	This function is invoked to process the "file readable" Tcl command.
 *	See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
FileAttrIsReadableCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "name");
	return TCL_ERROR;
    }
    return CheckAccess(interp, objv[1], R_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * FileAttrIsWritableCmd --
 *
 *	This function is invoked to process the "file writable" Tcl command.
 *	See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
FileAttrIsWritableCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "name");
	return TCL_ERROR;
    }
    return CheckAccess(interp, objv[1], W_OK);
}

/*
 *----------------------------------------------------------------------
 *
 * PathDirNameCmd --
 *
 *	This function is invoked to process the "file dirname" Tcl command.
 *	See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
PathDirNameCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    Tcl_Obj *dirPtr;

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "name");
	return TCL_ERROR;
    }
    dirPtr = TclPathPart(interp, objv[1], TCL_PATH_DIRNAME);
    if (dirPtr == NULL) {
	return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, dirPtr);
    Tcl_DecrRefCount(dirPtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * PathExtensionCmd --
 *
 *	This function is invoked to process the "file extension" Tcl command.
 *	See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
PathExtensionCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    Tcl_Obj *dirPtr;

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "name");
	return TCL_ERROR;
    }
    dirPtr = TclPathPart(interp, objv[1], TCL_PATH_EXTENSION);
    if (dirPtr == NULL) {
	return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, dirPtr);
    Tcl_DecrRefCount(dirPtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * PathRootNameCmd --
 *
 *	This function is invoked to process the "file root" Tcl command. See
 *	the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
PathRootNameCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    Tcl_Obj *dirPtr;

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "name");
	return TCL_ERROR;
    }
    dirPtr = TclPathPart(interp, objv[1], TCL_PATH_ROOT);
    if (dirPtr == NULL) {
	return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, dirPtr);
    Tcl_DecrRefCount(dirPtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * PathTailCmd --
 *
 *	This function is invoked to process the "file tail" Tcl command. See
 *	the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
PathTailCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    Tcl_Obj *dirPtr;

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "name");
	return TCL_ERROR;
    }
    dirPtr = TclPathPart(interp, objv[1], TCL_PATH_TAIL);
    if (dirPtr == NULL) {
	return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, dirPtr);
    Tcl_DecrRefCount(dirPtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * PathFilesystemCmd --
 *
 *	This function is invoked to process the "file system" Tcl command. See
 *	the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
PathFilesystemCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    Tcl_Obj *fsInfo;

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "name");
	return TCL_ERROR;
    }
    fsInfo = Tcl_FSFileSystemInfo(objv[1]);
    if (fsInfo == NULL) {
	Tcl_SetResult(interp, "unrecognised path", TCL_STATIC);
	return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, fsInfo);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * PathJoinCmd --
 *
 *	This function is invoked to process the "file join" Tcl command. See
 *	the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
PathJoinCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    if (objc < 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "name ?name ...?");
	return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, Tcl_FSJoinToPath(NULL, objc - 1, objv + 1));
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * PathNativeNameCmd --
 *
 *	This function is invoked to process the "file nativename" Tcl command.
 *	See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
PathNativeNameCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    const char *fileName;
    Tcl_DString ds;

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "name");
	return TCL_ERROR;
    }
    fileName = Tcl_TranslateFileName(interp, TclGetString(objv[1]), &ds);
    if (fileName == NULL) {
	return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, Tcl_NewStringObj(fileName,
	    Tcl_DStringLength(&ds)));
    Tcl_DStringFree(&ds);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * PathNormalizeCmd --
 *
 *	This function is invoked to process the "file normalize" Tcl command.
 *	See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
PathNormalizeCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    Tcl_Obj *fileName;

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "name");
	return TCL_ERROR;
    }
    fileName = Tcl_FSGetNormalizedPath(interp, objv[1]);
    if (fileName == NULL) {
	return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, fileName);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * PathSplitCmd --
 *
 *	This function is invoked to process the "file split" Tcl command. See
 *	the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
PathSplitCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    Tcl_Obj *res;

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "name");
	return TCL_ERROR;
    }
    res = Tcl_FSSplitPath(objv[1], NULL);
    if (res == NULL) {
	Tcl_AppendResult(interp, "could not read \"", TclGetString(objv[1]),
		"\": no such file or directory", NULL);
	return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, res);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * PathTypeCmd --
 *
 *	This function is invoked to process the "file pathtype" Tcl command.
 *	See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
PathTypeCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    Tcl_Obj *typeName;

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "name");
	return TCL_ERROR;
    }
    switch (Tcl_FSGetPathType(objv[1])) {
    case TCL_PATH_ABSOLUTE:
	TclNewLiteralStringObj(typeName, "absolute");
	break;
    case TCL_PATH_RELATIVE:
	TclNewLiteralStringObj(typeName, "relative");
	break;
    case TCL_PATH_VOLUME_RELATIVE:
	TclNewLiteralStringObj(typeName, "volumerelative");
	break;
    default:
	/* Should be unreachable */
	return TCL_OK;
    }
    Tcl_SetObjResult(interp, typeName);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * FilesystemSeparatorCmd --
 *
 *	This function is invoked to process the "file separator" Tcl command.
 *	See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
FilesystemSeparatorCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    if (objc < 1 || objc > 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "?name?");
	return TCL_ERROR;
    }
    if (objc == 1) {
	const char *separator = NULL; /* lint */

	switch (tclPlatform) {
	case TCL_PLATFORM_UNIX:
	    separator = "/";
	    break;
	case TCL_PLATFORM_WINDOWS:
	    separator = "\\";
	    break;
	}
	Tcl_SetObjResult(interp, Tcl_NewStringObj(separator, 1));
    } else {
	Tcl_Obj *separatorObj = Tcl_FSPathSeparator(objv[1]);

	if (separatorObj == NULL) {
	    Tcl_SetResult(interp, "unrecognised path", TCL_STATIC);
	    return TCL_ERROR;
	}
	Tcl_SetObjResult(interp, separatorObj);
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * FilesystemVolumesCmd --
 *
 *	This function is invoked to process the "file volumes" Tcl command.
 *	See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
FilesystemVolumesCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    if (objc != 1) {
	Tcl_WrongNumArgs(interp, 1, objv, NULL);
	return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, Tcl_FSListVolumes());
    return TCL_OK;
}

/*
 *---------------------------------------------------------------------------
 *
 * CheckAccess --
 *
 *	Utility procedure used by Tcl_FileObjCmd() to query file attributes
 *	available through the access() system call.
 *
 * Results:
 *	Always returns TCL_OK. Sets interp's result to boolean true or false
 *	depending on whether the file has the specified attribute.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */

static int
CheckAccess(
    Tcl_Interp *interp,		/* Interp for status return. Must not be
				 * NULL. */
    Tcl_Obj *pathPtr,		/* Name of file to check. */
    int mode)			/* Attribute to check; passed as argument to
				 * access(). */
{
    int value;

    if (Tcl_FSConvertToPathType(interp, pathPtr) != TCL_OK) {
	value = 0;
    } else {
	value = (Tcl_FSAccess(pathPtr, mode) == 0);
    }
    Tcl_SetObjResult(interp, Tcl_NewBooleanObj(value));

    return TCL_OK;
}

/*
 *---------------------------------------------------------------------------
 *
 * GetStatBuf --
 *
 *	Utility procedure used by Tcl_FileObjCmd() to query file attributes
 *	available through the stat() or lstat() system call.
 *
 * Results:
 *	The return value is TCL_OK if the specified file exists and can be
 *	stat'ed, TCL_ERROR otherwise. If TCL_ERROR is returned, an error
 *	message is left in interp's result. If TCL_OK is returned, *statPtr is
 *	filled with information about the specified file.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */

static int
GetStatBuf(
    Tcl_Interp *interp,		/* Interp for error return. May be NULL. */
    Tcl_Obj *pathPtr,		/* Path name to examine. */
    Tcl_FSStatProc *statProc,	/* Either stat() or lstat() depending on
				 * desired behavior. */
    Tcl_StatBuf *statPtr)	/* Filled with info about file obtained by
				 * calling (*statProc)(). */
{
    int status;

    if (Tcl_FSConvertToPathType(interp, pathPtr) != TCL_OK) {
	return TCL_ERROR;
    }

    status = statProc(pathPtr, statPtr);

    if (status < 0) {
	if (interp != NULL) {
	    Tcl_AppendResult(interp, "could not read \"",
		    TclGetString(pathPtr), "\": ",
		    Tcl_PosixError(interp), NULL);
	}
	return TCL_ERROR;
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * StoreStatData --
 *
 *	This is a utility procedure that breaks out the fields of a "stat"
 *	structure and stores them in textual form into the elements of an
 *	associative array.
 *
 * Results:
 *	Returns a standard Tcl return value. If an error occurs then a message
 *	is left in interp's result.
 *
 * Side effects:
 *	Elements of the associative array given by "varName" are modified.
 *
 *----------------------------------------------------------------------
 */

static int
StoreStatData(
    Tcl_Interp *interp,		/* Interpreter for error reports. */
    Tcl_Obj *varName,		/* Name of associative array variable in which
				 * to store stat results. */
    Tcl_StatBuf *statPtr)	/* Pointer to buffer containing stat data to
				 * store in varName. */
{
    Tcl_Obj *field, *value;
    register unsigned short mode;

    /*
     * Assume Tcl_ObjSetVar2() does not keep a copy of the field name!
     *
     * Might be a better idea to call Tcl_SetVar2Ex() instead, except we want
     * to have an object (i.e. possibly cached) array variable name but a
     * string element name, so no API exists. Messy.
     */

#define STORE_ARY(fieldName, object) \
    TclNewLiteralStringObj(field, fieldName);				\
    Tcl_IncrRefCount(field);						\
    value = (object);							\
    if (Tcl_ObjSetVar2(interp,varName,field,value,TCL_LEAVE_ERR_MSG)==NULL) { \
	TclDecrRefCount(field);						\
	return TCL_ERROR;						\
    }									\
    TclDecrRefCount(field);

    /*
     * Watch out porters; the inode is meant to be an *unsigned* value, so the
     * cast might fail when there isn't a real arithmetic 'long long' type...
     */

    STORE_ARY("dev",	Tcl_NewLongObj((long)statPtr->st_dev));
    STORE_ARY("ino",	Tcl_NewWideIntObj((Tcl_WideInt)statPtr->st_ino));
    STORE_ARY("nlink",	Tcl_NewLongObj((long)statPtr->st_nlink));
    STORE_ARY("uid",	Tcl_NewLongObj((long)statPtr->st_uid));
    STORE_ARY("gid",	Tcl_NewLongObj((long)statPtr->st_gid));
    STORE_ARY("size",	Tcl_NewWideIntObj((Tcl_WideInt)statPtr->st_size));
#ifdef HAVE_STRUCT_STAT_ST_BLOCKS
    STORE_ARY("blocks",	Tcl_NewWideIntObj((Tcl_WideInt)statPtr->st_blocks));
#endif
#ifdef HAVE_STRUCT_STAT_ST_BLKSIZE
    STORE_ARY("blksize", Tcl_NewLongObj((long)statPtr->st_blksize));
#endif
    STORE_ARY("atime",	Tcl_NewLongObj((long)statPtr->st_atime));
    STORE_ARY("mtime",	Tcl_NewLongObj((long)statPtr->st_mtime));
    STORE_ARY("ctime",	Tcl_NewLongObj((long)statPtr->st_ctime));
    mode = (unsigned short) statPtr->st_mode;
    STORE_ARY("mode",	Tcl_NewIntObj(mode));
    STORE_ARY("type",	Tcl_NewStringObj(GetTypeFromMode(mode), -1));
#undef STORE_ARY

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * GetTypeFromMode --
 *
 *	Given a mode word, returns a string identifying the type of a file.
 *
 * Results:
 *	A static text string giving the file type from mode.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static const char *
GetTypeFromMode(
    int mode)
{
    if (S_ISREG(mode)) {
	return "file";
    } else if (S_ISDIR(mode)) {
	return "directory";
    } else if (S_ISCHR(mode)) {
	return "characterSpecial";
    } else if (S_ISBLK(mode)) {
	return "blockSpecial";
    } else if (S_ISFIFO(mode)) {
	return "fifo";
#ifdef S_ISLNK
    } else if (S_ISLNK(mode)) {
	return "link";
#endif
#ifdef S_ISSOCK
    } else if (S_ISSOCK(mode)) {
	return "socket";
#endif
    }
    return "unknown";
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ForObjCmd --
 *
 *	This procedure is invoked to process the "for" Tcl command. See the
 *	user documentation for details on what it does.
 *
 *	With the bytecode compiler, this procedure is only called when a
 *	command name is computed at runtime, and is "for" or the name to which
 *	"for" was renamed: e.g.,
 *	"set z for; $z {set i 0} {$i<100} {incr i} {puts $i}"
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	See the user documentation.
 *
 * Notes:
 *	This command is split into a lot of pieces so that it can avoid doing
 *	reentrant TEBC calls. This makes things rather hard to follow, but
 *	here's the plan:
 *
 *	NR:	---------------_\
 *	Direct:	Tcl_ForObjCmd -> TclNRForObjCmd
 *					|
 *				ForSetupCallback
 *					|
 *	[while] ------------> TclNRForIterCallback <---------.
 *					|		     |
 *				 ForCondCallback	     |
 *					|		     |
 *				 ForNextCallback ------------|
 *					|		     |
 *			       ForPostNextCallback	     |
 *					|____________________|
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
Tcl_ForObjCmd(
    ClientData dummy,		/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    return Tcl_NRCallObjProc(interp, TclNRForObjCmd, dummy, objc, objv);
}

int
TclNRForObjCmd(
    ClientData dummy,		/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    Interp *iPtr = (Interp *) interp;
    ForIterData *iterPtr;

    if (objc != 5) {
	Tcl_WrongNumArgs(interp, 1, objv, "start test next command");
	return TCL_ERROR;
    }

    TclSmallAllocEx(interp, sizeof(ForIterData), iterPtr);
    iterPtr->cond = objv[2];
    iterPtr->body = objv[4];
    iterPtr->next = objv[3];
    iterPtr->msg  = "\n    (\"for\" body line %d)";
    iterPtr->word = 4;

    TclNRAddCallback(interp, ForSetupCallback, iterPtr, NULL, NULL, NULL);

    /*
     * TIP #280. Make invoking context available to initial script.
     */

    return TclNREvalObjEx(interp, objv[1], 0, iPtr->cmdFramePtr, 1);
}

static int
ForSetupCallback(
    ClientData data[],
    Tcl_Interp *interp,
    int result)
{
    ForIterData *iterPtr = data[0];

    if (result != TCL_OK) {
	if (result == TCL_ERROR) {
	    Tcl_AddErrorInfo(interp, "\n    (\"for\" initial command)");
	}
	TclSmallFreeEx(interp, iterPtr);
	return result;
    }
    TclNRAddCallback(interp, TclNRForIterCallback, iterPtr, NULL, NULL, NULL);
    return TCL_OK;
}

int
TclNRForIterCallback(
    ClientData data[],
    Tcl_Interp *interp,
    int result)
{
    ForIterData *iterPtr = data[0];
    Tcl_Obj *boolObj;

    switch (result) {
    case TCL_OK:
    case TCL_CONTINUE:
	/*
	 * We need to reset the result before evaluating the expression.
	 * Otherwise, any error message will be appended to the result of the
	 * last evaluation.
	 */

	Tcl_ResetResult(interp);
	TclNewObj(boolObj);
	TclNRAddCallback(interp, ForCondCallback, iterPtr, boolObj, NULL,
		NULL);
	return Tcl_NRExprObj(interp, iterPtr->cond, boolObj);
    case TCL_BREAK:
	result = TCL_OK;
	Tcl_ResetResult(interp);
	break;
    case TCL_ERROR:
	Tcl_AppendObjToErrorInfo(interp,
		Tcl_ObjPrintf(iterPtr->msg, Tcl_GetErrorLine(interp)));
    }
    TclSmallFreeEx(interp, iterPtr);
    return result;
}

static int
ForCondCallback(
    ClientData data[],
    Tcl_Interp *interp,
    int result)
{
    Interp *iPtr = (Interp *) interp;
    ForIterData *iterPtr = data[0];
    Tcl_Obj *boolObj = data[1];
    int value;

    if (result != TCL_OK) {
	Tcl_DecrRefCount(boolObj);
	TclSmallFreeEx(interp, iterPtr);
	return result;
    } else if (Tcl_GetBooleanFromObj(interp, boolObj, &value) != TCL_OK) {
	Tcl_DecrRefCount(boolObj);
	TclSmallFreeEx(interp, iterPtr);
	return TCL_ERROR;
    }
    Tcl_DecrRefCount(boolObj);

    if (value) {
	/* TIP #280. */
	if (iterPtr->next) {
	    TclNRAddCallback(interp, ForNextCallback, iterPtr, NULL, NULL,
		    NULL);
	} else {
	    TclNRAddCallback(interp, TclNRForIterCallback, iterPtr, NULL,
		    NULL, NULL);
	}
	return TclNREvalObjEx(interp, iterPtr->body, 0, iPtr->cmdFramePtr,
		iterPtr->word);
    }
    TclSmallFreeEx(interp, iterPtr);
    return result;
}

static int
ForNextCallback(
    ClientData data[],
    Tcl_Interp *interp,
    int result)
{
    Interp *iPtr = (Interp *) interp;
    ForIterData *iterPtr = data[0];
    Tcl_Obj *next = iterPtr->next;

    if ((result == TCL_OK) || (result == TCL_CONTINUE)) {
	TclNRAddCallback(interp, ForPostNextCallback, iterPtr, NULL, NULL,
		NULL);

	/*
	 * TIP #280. Make invoking context available to next script.
	 */

	return TclNREvalObjEx(interp, next, 0, iPtr->cmdFramePtr, 3);
    }

    TclNRAddCallback(interp, TclNRForIterCallback, iterPtr, NULL, NULL, NULL);
    return result;
}

static int
ForPostNextCallback(
    ClientData data[],
    Tcl_Interp *interp,
    int result)
{
    ForIterData *iterPtr = data[0];

    if ((result != TCL_BREAK) && (result != TCL_OK)) {
	if (result == TCL_ERROR) {
	    Tcl_AddErrorInfo(interp, "\n    (\"for\" loop-end command)");
	    TclSmallFreeEx(interp, iterPtr);
	}
	return result;
    }
    TclNRAddCallback(interp, TclNRForIterCallback, iterPtr, NULL, NULL, NULL);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ForeachObjCmd, TclNRForeachCmd --
 *
 *	This object-based procedure is invoked to process the "foreach" Tcl
 *	command. See the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl object result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
Tcl_ForeachObjCmd(
    ClientData dummy,		/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    return Tcl_NRCallObjProc(interp, TclNRForeachCmd, dummy, objc, objv);
}

int
TclNRForeachCmd(
    ClientData dummy,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    int numLists = (objc-2) / 2;
    register struct ForeachState *statePtr;
    int i, j, result;

    if (objc < 4 || (objc%2 != 0)) {
	Tcl_WrongNumArgs(interp, 1, objv,
		"varList list ?varList list ...? command");
	return TCL_ERROR;
    }

    /*
     * Manage numList parallel value lists.
     * statePtr->argvList[i] is a value list counted by statePtr->argcList[i];
     * statePtr->varvList[i] is the list of variables associated with the
     *		value list;
     * statePtr->varcList[i] is the number of variables associated with the
     *		value list;
     * statePtr->index[i] is the current pointer into the value list
     *		statePtr->argvList[i].
     *
     * The setting up of all of these pointers is moderately messy, but allows
     * the rest of this code to be simple and for us to use a single memory
     * allocation for better performance.
     */

    statePtr = TclStackAlloc(interp,
	    sizeof(struct ForeachState) + 3 * numLists * sizeof(int)
	    + 2 * numLists * (sizeof(Tcl_Obj **) + sizeof(Tcl_Obj *)));
    memset(statePtr, 0,
	    sizeof(struct ForeachState) + 3 * numLists * sizeof(int)
	    + 2 * numLists * (sizeof(Tcl_Obj **) + sizeof(Tcl_Obj *)));
    statePtr->varvList = (Tcl_Obj ***) (statePtr + 1);
    statePtr->argvList = statePtr->varvList + numLists;
    statePtr->vCopyList = (Tcl_Obj **) (statePtr->argvList + numLists);
    statePtr->aCopyList = statePtr->vCopyList + numLists;
    statePtr->index = (int *) (statePtr->aCopyList + numLists);
    statePtr->varcList = statePtr->index + numLists;
    statePtr->argcList = statePtr->varcList + numLists;

    statePtr->numLists = numLists;
    statePtr->bodyPtr = objv[objc - 1];
    statePtr->bodyIdx = objc - 1;

    /*
     * Break up the value lists and variable lists into elements.
     */

    for (i=0 ; i<numLists ; i++) {
	statePtr->vCopyList[i] = TclListObjCopy(interp, objv[1+i*2]);
	if (statePtr->vCopyList[i] == NULL) {
	    result = TCL_ERROR;
	    goto done;
	}
	TclListObjGetElements(NULL, statePtr->vCopyList[i],
		&statePtr->varcList[i], &statePtr->varvList[i]);
	if (statePtr->varcList[i] < 1) {
	    Tcl_AppendResult(interp, "foreach varlist is empty", NULL);
	    result = TCL_ERROR;
	    goto done;
	}

	statePtr->aCopyList[i] = TclListObjCopy(interp, objv[2+i*2]);
	if (statePtr->aCopyList[i] == NULL) {
	    result = TCL_ERROR;
	    goto done;
	}
	TclListObjGetElements(NULL, statePtr->aCopyList[i],
		&statePtr->argcList[i], &statePtr->argvList[i]);

	j = statePtr->argcList[i] / statePtr->varcList[i];
	if ((statePtr->argcList[i] % statePtr->varcList[i]) != 0) {
	    j++;
	}
	if (j > statePtr->maxj) {
	    statePtr->maxj = j;
	}
    }

    /*
     * If there is any work to do, assign the variables and set things going
     * non-recursively.
     */

    if (statePtr->maxj > 0) {
	result = ForeachAssignments(interp, statePtr);
	if (result == TCL_ERROR) {
	    goto done;
	}

	TclNRAddCallback(interp, ForeachLoopStep, statePtr, NULL, NULL, NULL);
	return TclNREvalObjEx(interp, objv[objc-1], 0,
		((Interp *) interp)->cmdFramePtr, objc-1);
    }

    /*
     * This cleanup stage is only used when an error occurs during setup or if
     * there is no work to do.
     */

    result = TCL_OK;
  done:
    ForeachCleanup(interp, statePtr);
    return result;
}

/*
 * Post-body processing handler.
 */

static int
ForeachLoopStep(
    ClientData data[],
    Tcl_Interp *interp,
    int result)
{
    register struct ForeachState *statePtr = data[0];

    /*
     * Process the result code from this run of the [foreach] body. Note that
     * this switch uses fallthroughs in several places. Maintainer aware!
     */

    switch (result) {
    case TCL_CONTINUE:
	result = TCL_OK;
    case TCL_OK:
	break;
    case TCL_BREAK:
	result = TCL_OK;
	goto done;
    case TCL_ERROR:
	Tcl_AppendObjToErrorInfo(interp, Tcl_ObjPrintf(
		"\n    (\"foreach\" body line %d)", Tcl_GetErrorLine(interp)));
    default:
	goto done;
    }

    /*
     * Test if there is work still to be done. If so, do the next round of
     * variable assignments, reschedule ourselves and run the body again.
     */

    if (statePtr->maxj > ++statePtr->j) {
	result = ForeachAssignments(interp, statePtr);
	if (result == TCL_ERROR) {
	    goto done;
	}

	TclNRAddCallback(interp, ForeachLoopStep, statePtr, NULL, NULL, NULL);
	return TclNREvalObjEx(interp, statePtr->bodyPtr, 0,
		((Interp *) interp)->cmdFramePtr, statePtr->bodyIdx);
    }

    /*
     * We're done. Tidy up our work space and finish off.
     */

    Tcl_ResetResult(interp);
  done:
    ForeachCleanup(interp, statePtr);
    return result;
}

/*
 * Factored out code to do the assignments in [foreach].
 */

static inline int
ForeachAssignments(
    Tcl_Interp *interp,
    struct ForeachState *statePtr)
{
    int i, v, k;
    Tcl_Obj *valuePtr, *varValuePtr;

    for (i=0 ; i<statePtr->numLists ; i++) {
	for (v=0 ; v<statePtr->varcList[i] ; v++) {
	    k = statePtr->index[i]++;

	    if (k < statePtr->argcList[i]) {
		valuePtr = statePtr->argvList[i][k];
	    } else {
		TclNewObj(valuePtr);	/* Empty string */
	    }

	    varValuePtr = Tcl_ObjSetVar2(interp, statePtr->varvList[i][v],
		    NULL, valuePtr, TCL_LEAVE_ERR_MSG);

	    if (varValuePtr == NULL) {
		Tcl_AppendObjToErrorInfo(interp, Tcl_ObjPrintf(
			"\n    (setting foreach loop variable \"%s\")",
			TclGetString(statePtr->varvList[i][v])));
		return TCL_ERROR;
	    }
	}
    }

    return TCL_OK;
}

/*
 * Factored out code for cleaning up the state of the foreach.
 */

static inline void
ForeachCleanup(
    Tcl_Interp *interp,
    struct ForeachState *statePtr)
{
    int i;

    for (i=0 ; i<statePtr->numLists ; i++) {
	if (statePtr->vCopyList[i]) {
	    TclDecrRefCount(statePtr->vCopyList[i]);
	}
	if (statePtr->aCopyList[i]) {
	    TclDecrRefCount(statePtr->aCopyList[i]);
	}
    }
    TclStackFree(interp, statePtr);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FormatObjCmd --
 *
 *	This procedure is invoked to process the "format" Tcl command. See
 *	the user documentation for details on what it does.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
int
Tcl_FormatObjCmd(
    ClientData dummy,		/* Not used. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    Tcl_Obj *resultPtr;		/* Where result is stored finally. */

    if (objc < 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "formatString ?arg ...?");
	return TCL_ERROR;
    }

    resultPtr = Tcl_Format(interp, TclGetString(objv[1]), objc-2, objv+2);
    if (resultPtr == NULL) {
	return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, resultPtr);
    return TCL_OK;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
