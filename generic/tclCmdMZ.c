/* 
 * tclCmdMZ.c --
 *
 *	This file contains the top-level command routines for most of
 *	the Tcl built-in commands whose names begin with the letters
 *	M to Z.  It contains only commands in the generic core (i.e.
 *	those that don't depend much upon UNIX facilities).
 *
 * Copyright (c) 1987-1993 The Regents of the University of California.
 * Copyright (c) 1994-1997 Sun Microsystems, Inc.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id$
 */

#include "tclInt.h"
#include "tclPort.h"
#include "tclCompile.h"
#include "tclRegexp.h"

/*
 * Flag values used by Tcl_ScanObjCmd.
 */

#define SCAN_NOSKIP	0x1		  /* Don't skip blanks. */
#define SCAN_SUPPRESS	0x2		  /* Suppress assignment. */
#define SCAN_UNSIGNED	0x4		  /* Read an unsigned value. */
#define SCAN_WIDTH	0x8		  /* A width value was supplied. */

#define SCAN_SIGNOK	0x10		  /* A +/- character is allowed. */
#define SCAN_NODIGITS	0x20		  /* No digits have been scanned. */
#define SCAN_NOZERO	0x40		  /* No zero digits have been scanned. */
#define SCAN_XOK	0x80		  /* An 'x' is allowed. */
#define SCAN_PTOK	0x100		  /* Decimal point is allowed. */
#define SCAN_EXPOK	0x200		  /* An exponent is allowed. */

/*
 * Structure used to hold information about variable traces:
 */

typedef struct {
    int flags;			/* Operations for which Tcl command is
				 * to be invoked. */
    char *errMsg;		/* Error message returned from Tcl command,
				 * or NULL.  Malloc'ed. */
    size_t length;		/* Number of non-NULL chars. in command. */
    char command[4];		/* Space for Tcl command to invoke.  Actual
				 * size will be as large as necessary to
				 * hold command.  This field must be the
				 * last in the structure, so that it can
				 * be larger than 4 bytes. */
} TraceVarInfo;

/*
 * Forward declarations for procedures defined in this file:
 */

static char *		TraceVarProc _ANSI_ARGS_((ClientData clientData,
			    Tcl_Interp *interp, char *name1, char *name2,
			    int flags));

/*
 *----------------------------------------------------------------------
 *
 * Tcl_PwdObjCmd --
 *
 *	This procedure is invoked to process the "pwd" Tcl command.
 *	See the user documentation for details on what it does.
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
Tcl_PwdObjCmd(dummy, interp, objc, objv)
    ClientData dummy;			/* Not used. */
    Tcl_Interp *interp;			/* Current interpreter. */
    int objc;				/* Number of arguments. */
    Tcl_Obj *CONST objv[];		/* Argument objects. */
{
    Tcl_DString ds;

    if (objc != 1) {
	Tcl_WrongNumArgs(interp, 1, objv, NULL);
	return TCL_ERROR;
    }

    if (TclpGetCwd(interp, &ds) == NULL) {
	return TCL_ERROR;
    }
    Tcl_DStringResult(interp, &ds);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_RegexpObjCmd --
 *
 *	This procedure is invoked to process the "regexp" Tcl command.
 *	See the user documentation for details on what it does.  The
 *	REGEXP_TEST stuff is to minimize code differences between this
 *	and the "testregexp" command.
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
Tcl_RegexpObjCmd(dummy, interp, objc, objv)
    ClientData dummy;			/* Not used. */
    Tcl_Interp *interp;			/* Current interpreter. */
    int objc;				/* Number of arguments. */
    Tcl_Obj *CONST objv[];		/* Argument objects. */
{
    int i, result, indices, stringLength, wLen, match, about;
    int cflags, eflags;
    Tcl_RegExp regExpr;
    char *string;
    Tcl_DString stringBuffer, valueBuffer;
    Tcl_UniChar *wStart;
    static char *options[] = {
	"-indices",	"-nocase",	"-about",	"-expanded",
	"-line",	"-linestop",	"-lineanchor",
	"--",		(char *) NULL
    };
    enum options {
	REGEXP_INDICES, REGEXP_NOCASE,	REGEXP_ABOUT,	REGEXP_EXPANDED,
	REGEXP_LINE,	REGEXP_LINESTOP, REGEXP_LINEANCHOR,
	REGEXP_LAST
    };

    indices = 0;
    about = 0;
    cflags = REG_ADVANCED;
    eflags = 0;
    
    for (i = 1; i < objc; i++) {
	char *name;
	int index;

	name = Tcl_GetString(objv[i]);
	if (name[0] != '-') {
	    break;
	}
	if (Tcl_GetIndexFromObj(interp, objv[i], options, "switch", TCL_EXACT,
		&index) != TCL_OK) {
	    return TCL_ERROR;
	}
	switch ((enum options) index) {
	    case REGEXP_INDICES: {
		indices = 1;
		break;
	    }
	    case REGEXP_NOCASE: {
		cflags |= REG_ICASE;
		break;
	    }
	    case REGEXP_ABOUT: {
		about = 1;
		break;
	    }
	    case REGEXP_EXPANDED: {
		cflags |= REG_EXPANDED;
		break;
	    }
	    case REGEXP_LINE: {
		cflags |= REG_NEWLINE;
		break;
	    }
	    case REGEXP_LINESTOP: {
		cflags |= REG_NLSTOP;
		break;
	    }
	    case REGEXP_LINEANCHOR: {
		cflags |= REG_NLANCH;
		break;
	    }
	    case REGEXP_LAST: {
		i++;
		goto endOfForLoop;
	    }
	}
    }

    endOfForLoop:
    if (objc - i < 2 - about) {
	Tcl_WrongNumArgs(interp, 1, objv,
		"?switches? exp string ?matchVar? ?subMatchVar subMatchVar ...?");
	return TCL_ERROR;
    }
    objc -= i;
    objv += i;

    regExpr = TclRegCompObj(interp, objv[0], cflags);
    if (regExpr == NULL) {
	return TCL_ERROR;
    }

    if (about) {
	if (TclRegAbout(interp, regExpr) < 0) {
	    return TCL_ERROR;
	}
	return TCL_OK;
    }

    result = TCL_OK;
    string = Tcl_GetStringFromObj(objv[1], &stringLength);

    Tcl_DStringInit(&valueBuffer);
    
    Tcl_DStringInit(&stringBuffer);
    wStart = TclUtfToUniCharDString(string, stringLength, &stringBuffer);
    wLen = Tcl_DStringLength(&stringBuffer) / sizeof(Tcl_UniChar);

    match = TclRegExpExecUniChar(interp, regExpr, wStart, wLen, objc-2, eflags);
    if (match < 0) {
	result = TCL_ERROR;
	goto done;
    }
    if (match == 0) {
	/*
	 * Set the interpreter's object result to an integer object w/ value 0. 
	 */
	
	Tcl_SetIntObj(Tcl_GetObjResult(interp), 0);
	goto done;
    }

    /*
     * If additional variable names have been specified, return
     * index information in those variables.
     */

    objc -= 2;
    objv += 2;

    for (i = 0; i < objc; i++) {
	char *varName, *value;
	int start, end;
	
	varName = Tcl_GetString(objv[i]);

	TclRegExpRangeUniChar(regExpr, i, &start, &end);
	if (start < 0) {
	    if (indices) {
		value = Tcl_SetVar(interp, varName, "-1 -1", 0);
	    } else {
		value = Tcl_SetVar(interp, varName, "", 0);
	    }
	} else {
	    if (indices) {
		char info[TCL_INTEGER_SPACE * 2];
		
		sprintf(info, "%d %d", start, end - 1);
		value = Tcl_SetVar(interp, varName, info, 0);
	    } else {
		value = TclUniCharToUtfDString(wStart + start, end - start,
			&valueBuffer);
		value = Tcl_SetVar(interp, varName, value, 0);
		Tcl_DStringSetLength(&valueBuffer, 0);
	    }
	}
	if (value == NULL) {
	    Tcl_AppendResult(interp, "couldn't set variable \"",
		    varName, "\"", (char *) NULL);
	    result = TCL_ERROR;
	    goto done;
	}
    }

    /*
     * Set the interpreter's object result to an integer object w/ value 1. 
     */
	
    Tcl_SetIntObj(Tcl_GetObjResult(interp), 1);

    done:
    Tcl_DStringFree(&stringBuffer);
    Tcl_DStringFree(&valueBuffer);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_RegsubObjCmd --
 *
 *	This procedure is invoked to process the "regsub" Tcl command.
 *	See the user documentation for details on what it does.
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
Tcl_RegsubObjCmd(dummy, interp, objc, objv)
    ClientData dummy;			/* Not used. */
    Tcl_Interp *interp;			/* Current interpreter. */
    int objc;				/* Number of arguments. */
    Tcl_Obj *CONST objv[];		/* Argument objects. */
{
    int i, result, flags, all, stringLength, numMatches;
    Tcl_RegExp regExpr;
    Tcl_DString resultBuffer, stringBuffer;
    CONST Tcl_UniChar *w, *wStart, *wEnd;
    char *string, *subspec, *varname;
    static char *options[] = {
	"-all",		"-nocase",	"--",		NULL
    };
    enum options {
	REGSUB_ALL,	REGSUB_NOCASE,	REGSUB_LAST
    };

    flags = 0;
    all = 0;

    for (i = 1; i < objc; i++) {
	char *name;
	int index;
	
	name = Tcl_GetString(objv[i]);
	if (name[0] != '-') {
	    break;
	}
	if (Tcl_GetIndexFromObj(interp, objv[i], options, "switch", TCL_EXACT,
		&index) != TCL_OK) {
	    return TCL_ERROR;
	}
	switch ((enum options) index) {
	    case REGSUB_ALL: {
		all = 1;
		break;
	    }
	    case REGSUB_NOCASE: {
		flags |= REG_ICASE;
		break;
	    }
	    case REGSUB_LAST: {
		i++;
		goto endOfForLoop;
	    }
	}
    }
    endOfForLoop:
    if (objc - i != 4) {
	Tcl_WrongNumArgs(interp, 1, objv,
		"?switches? exp string subSpec varName");
	return TCL_ERROR;
    }

    objv += i;
    regExpr = TclRegCompObj(interp, objv[0], flags | REG_ADVANCED);
    if (regExpr == NULL) {
	return TCL_ERROR;
    }

    result = TCL_OK;
    string = Tcl_GetStringFromObj(objv[1], &stringLength);
    subspec = Tcl_GetString(objv[2]);
    varname = Tcl_GetString(objv[3]);

    Tcl_DStringInit(&resultBuffer);

    /*
     * The following loop is to handle multiple matches within the
     * same source string;  each iteration handles one match and its
     * corresponding substitution.  If "-all" hasn't been specified
     * then the loop body only gets executed once.
     */

    Tcl_DStringInit(&stringBuffer);
    wStart = TclUtfToUniCharDString(string, stringLength, &stringBuffer);
    wEnd = wStart + Tcl_DStringLength(&stringBuffer) / sizeof(Tcl_UniChar);

    numMatches = 0;
    for (w = wStart; w < wEnd; ) {
	int start, end, subStart, subEnd, match;
	char *src, *firstChar;
	char c;

	/*
	 * The flags argument is set if string is part of a larger string,
	 * so that "^" won't match.
	 */

	match = TclRegExpExecUniChar(interp, regExpr, w, wEnd - w, 10,
		((w > wStart) ? REG_NOTBOL : 0));
	if (match < 0) {
	    result = TCL_ERROR;
	    goto done;
	}
	if (match == 0) {
	    break;
	}
	numMatches++;

	/*
	 * Copy the portion of the source string before the match to the
	 * result variable.
	 */

	TclRegExpRangeUniChar(regExpr, 0, &start, &end);
	TclUniCharToUtfDString(w, start, &resultBuffer);
    
	/*
	 * Append the subSpec argument to the variable, making appropriate
	 * substitutions.  This code is a bit hairy because of the backslash
	 * conventions and because the code saves up ranges of characters in
	 * subSpec to reduce the number of calls to Tcl_SetVar.
	 */

	src = subspec;
	firstChar = subspec;
	for (c = *src; c != '\0'; src++, c = *src) {
	    int index;
    
	    if (c == '&') {
		index = 0;
	    } else if (c == '\\') {
		c = src[1];
		if ((c >= '0') && (c <= '9')) {
		    index = c - '0';
		} else if ((c == '\\') || (c == '&')) {
		    Tcl_DStringAppend(&resultBuffer, firstChar,
			    src - firstChar);
		    Tcl_DStringAppend(&resultBuffer, &c, 1);
		    firstChar = src + 2;
		    src++;
		    continue;
		} else {
		    continue;
		}
	    } else {
		continue;
	    }
	    if (firstChar != src) {
		Tcl_DStringAppend(&resultBuffer, firstChar, src - firstChar);
	    }
	    TclRegExpRangeUniChar(regExpr, index, &subStart, &subEnd);
	    if ((subStart >= 0) && (subEnd >= 0)) {
		TclUniCharToUtfDString(w + subStart, subEnd - subStart,
			&resultBuffer);
	    }
	    if (*src == '\\') {
		src++;
	    }
	    firstChar = src + 1;
	}
	if (firstChar != src) {
	    Tcl_DStringAppend(&resultBuffer, firstChar, src - firstChar);
	}
	if (end == 0) {
	    /*
	     * Always consume at least one character of the input string
	     * in order to prevent infinite loops.
	     */

	    TclUniCharToUtfDString(w, 1, &resultBuffer);
	    w++;
	}
	w += end;
	if (!all) {
	    break;
	}
    }

    /*
     * Copy the portion of the source string after the last match to the
     * result variable.
     */

    if ((w < wEnd) || (numMatches == 0)) {
	TclUniCharToUtfDString(w, wEnd - w, &resultBuffer);
    }
    if (Tcl_SetVar(interp, varname, Tcl_DStringValue(&resultBuffer),
	    0) == NULL) {
	Tcl_AppendResult(interp, "couldn't set variable \"", varname, "\"",
		(char *) NULL);
	result = TCL_ERROR;
    } else {
	/*
	 * Set the interpreter's object result to an integer object holding the
	 * number of matches. 
	 */
	
	Tcl_SetIntObj(Tcl_GetObjResult(interp), numMatches);
    }

    done:
    Tcl_DStringFree(&stringBuffer);
    Tcl_DStringFree(&resultBuffer);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_RenameObjCmd --
 *
 *	This procedure is invoked to process the "rename" Tcl command.
 *	See the user documentation for details on what it does.
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
Tcl_RenameObjCmd(dummy, interp, objc, objv)
    ClientData dummy;		/* Arbitrary value passed to the command. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    char *oldName, *newName;
    
    if (objc != 3) {
	Tcl_WrongNumArgs(interp, 1, objv, "oldName newName");
	return TCL_ERROR;
    }

    oldName = Tcl_GetString(objv[1]);
    newName = Tcl_GetString(objv[2]);
    return TclRenameCommand(interp, oldName, newName);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ReturnObjCmd --
 *
 *	This object-based procedure is invoked to process the "return" Tcl
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
Tcl_ReturnObjCmd(dummy, interp, objc, objv)
    ClientData dummy;		/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    Interp *iPtr = (Interp *) interp;
    int optionLen, argLen, code, result;

    if (iPtr->errorInfo != NULL) {
	ckfree(iPtr->errorInfo);
	iPtr->errorInfo = NULL;
    }
    if (iPtr->errorCode != NULL) {
	ckfree(iPtr->errorCode);
	iPtr->errorCode = NULL;
    }
    code = TCL_OK;
    
    for (objv++, objc--;  objc > 1;  objv += 2, objc -= 2) {
	char *option = Tcl_GetStringFromObj(objv[0], &optionLen);
	char *arg = Tcl_GetStringFromObj(objv[1], &argLen);
    	
	if (strcmp(option, "-code") == 0) {
	    register int c = arg[0];
	    if ((c == 'o') && (strcmp(arg, "ok") == 0)) {
		code = TCL_OK;
	    } else if ((c == 'e') && (strcmp(arg, "error") == 0)) {
		code = TCL_ERROR;
	    } else if ((c == 'r') && (strcmp(arg, "return") == 0)) {
		code = TCL_RETURN;
	    } else if ((c == 'b') && (strcmp(arg, "break") == 0)) {
		code = TCL_BREAK;
	    } else if ((c == 'c') && (strcmp(arg, "continue") == 0)) {
		code = TCL_CONTINUE;
	    } else {
		result = Tcl_GetIntFromObj((Tcl_Interp *) NULL, objv[1],
		        &code);
		if (result != TCL_OK) {
		    Tcl_ResetResult(interp);
		    Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
			    "bad completion code \"",
			    Tcl_GetString(objv[1]),
			    "\": must be ok, error, return, break, ",
			    "continue, or an integer", (char *) NULL);
		    return result;
		}
	    }
	} else if (strcmp(option, "-errorinfo") == 0) {
	    iPtr->errorInfo =
		(char *) ckalloc((unsigned) (strlen(arg) + 1));
	    strcpy(iPtr->errorInfo, arg);
	} else if (strcmp(option, "-errorcode") == 0) {
	    iPtr->errorCode =
		(char *) ckalloc((unsigned) (strlen(arg) + 1));
	    strcpy(iPtr->errorCode, arg);
	} else {
	    Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
		    "bad option \"", option,
		    "\": must be -code, -errorcode, or -errorinfo",
		    (char *) NULL);
	    return TCL_ERROR;
	}
    }
    
    if (objc == 1) {
	/*
	 * Set the interpreter's object result. An inline version of
	 * Tcl_SetObjResult.
	 */

	Tcl_SetObjResult(interp, objv[0]);
    }
    iPtr->returnCode = code;
    return TCL_RETURN;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SourceObjCmd --
 *
 *	This procedure is invoked to process the "source" Tcl command.
 *	See the user documentation for details on what it does.
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
Tcl_SourceObjCmd(dummy, interp, objc, objv)
    ClientData dummy;		/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    char *bytes;
    int result;
    
    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "fileName");
	return TCL_ERROR;
    }

    bytes = Tcl_GetString(objv[1]);
    result = Tcl_EvalFile(interp, bytes);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SplitObjCmd --
 *
 *	This procedure is invoked to process the "split" Tcl command.
 *	See the user documentation for details on what it does.
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
Tcl_SplitObjCmd(dummy, interp, objc, objv)
    ClientData dummy;		/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    Tcl_UniChar ch;
    int len;
    char *splitChars, *string, *end;
    int splitCharLen, stringLen;
    Tcl_Obj *listPtr, *objPtr;

    if (objc == 2) {
	splitChars = " \n\t\r";
	splitCharLen = 4;
    } else if (objc == 3) {
	splitChars = Tcl_GetStringFromObj(objv[2], &splitCharLen);
    } else {
	Tcl_WrongNumArgs(interp, 1, objv, "string ?splitChars?");
	return TCL_ERROR;
    }

    string = Tcl_GetStringFromObj(objv[1], &stringLen);
    end = string + stringLen;
    listPtr = Tcl_GetObjResult(interp);
    
    if (stringLen == 0) {
	/*
	 * Do nothing.
	 */
    } else if (splitCharLen == 0) {
	/*
	 * Handle the special case of splitting on every character.
	 */

	for ( ; string < end; string += len) {
	    len = Tcl_UtfToUniChar(string, &ch);
	    objPtr = Tcl_NewStringObj(string, len);
	    Tcl_ListObjAppendElement(NULL, listPtr, objPtr);
	}
    } else {
	char *element, *p, *splitEnd;
	int splitLen;
	Tcl_UniChar splitChar;
	
	/*
	 * Normal case: split on any of a given set of characters.
	 * Discard instances of the split characters.
	 */

	splitEnd = splitChars + splitCharLen;

	for (element = string; string < end; string += len) {
	    len = Tcl_UtfToUniChar(string, &ch);
	    for (p = splitChars; p < splitEnd; p += splitLen) {
		splitLen = Tcl_UtfToUniChar(p, &splitChar);
		if (ch == splitChar) {
		    objPtr = Tcl_NewStringObj(element, string - element);
		    Tcl_ListObjAppendElement(NULL, listPtr, objPtr);
		    element = string + len;
		    break;
		}
	    }
	}
	objPtr = Tcl_NewStringObj(element, string - element);
	Tcl_ListObjAppendElement(NULL, listPtr, objPtr);
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_StringObjCmd --
 *
 *	This procedure is invoked to process the "string" Tcl command.
 *	See the user documentation for details on what it does.  Note
 *	that this command only functions correctly on properly formed
 *	Tcl UTF strings.
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
Tcl_StringObjCmd(dummy, interp, objc, objv)
    ClientData dummy;		/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    int index, left, right;
    Tcl_Obj *resultPtr;
    char *string1, *string2;
    int length1, length2;
    static char *options[] = {
	"compare",	"first",	"index",	"last",
	"length",	"match",	"range",	"tolower",
	"toupper",	"totitle",	"trim",		"trimleft",
	"trimright",	"wordend",	"wordstart",	(char *) NULL
    };
    enum options {
	STR_COMPARE,	STR_FIRST,	STR_INDEX,	STR_LAST,
	STR_LENGTH,	STR_MATCH,	STR_RANGE,	STR_TOLOWER,
	STR_TOUPPER,	STR_TOTITLE,	STR_TRIM,	STR_TRIMLEFT,
	STR_TRIMRIGHT,	STR_WORDEND,	STR_WORDSTART
    };	  
	    
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "option arg ?arg ...?");
	return TCL_ERROR;
    }
    
    if (Tcl_GetIndexFromObj(interp, objv[1], options, "option", 0,
	    &index) != TCL_OK) {
	return TCL_ERROR;
    }

    resultPtr = Tcl_GetObjResult(interp);
    switch ((enum options) index) {
	case STR_COMPARE: {
	    int match, length;

	    if (objc != 4) {
	        Tcl_WrongNumArgs(interp, 2, objv, "string1 string2");
		return TCL_ERROR;
	    }

	    string1 = Tcl_GetStringFromObj(objv[2], &length1);
	    string2 = Tcl_GetStringFromObj(objv[3], &length2);

	    length = (length1 < length2) ? length1 : length2;
	    match = memcmp(string1, string2, (unsigned) length);
	    if (match == 0) {
	        match = length1 - length2;
	    }
	    Tcl_SetIntObj(resultPtr, (match > 0) ? 1 : (match < 0) ? -1 : 0);
	    break;
	}
	case STR_FIRST: {
	    register char *p, *end;
	    int match;

	    if (objc != 4) {
	        badFirstLastArgs:
	        Tcl_WrongNumArgs(interp, 2, objv, "string1 string2");
		return TCL_ERROR;
	    }

	    /*
	     * This algorithm fails on improperly formed UTF strings.
	     */

	    match = -1;
	    string1 = Tcl_GetStringFromObj(objv[2], &length1);
	    string2 = Tcl_GetStringFromObj(objv[3], &length2);
	    if (length1 > 0) {
		end = string2 + length2 - length1 + 1;
		for (p = string2;  p < end;  p++) {
		    /*
		     * Scan forward to find the first character.
		     */

		    p = memchr(p, *string1, (unsigned) (end - p));
		    if (p == NULL) {
			break;
		    }
		    if (memcmp(string1, p, (unsigned) length1) == 0) {
			match = p - string2;
			break;
		    }
		}
	    }

	    /*
	     * Compute the character index of the matching string by counting
	     * the number of characters before the match.
	     */

	    if (match != -1) {
		match = Tcl_NumUtfChars(string2, match);
	    }
	    Tcl_SetIntObj(resultPtr, match);
	    break;
	}
	case STR_INDEX: {
	    int index;
	    Tcl_UniChar ch;
	    char buf[TCL_UTF_MAX];
	    char *start, *end;

	    if (objc != 4) {
	        Tcl_WrongNumArgs(interp, 2, objv, "string charIndex");
		return TCL_ERROR;
	    }

	    if (Tcl_GetIntFromObj(interp, objv[3], &index) != TCL_OK) {
		return TCL_ERROR;
	    }
	    if (index >= 0) {
		start = Tcl_GetStringFromObj(objv[2], &length1);
		end = start + length1;
		for ( ; start < end; index--) {
		    start += Tcl_UtfToUniChar(start, &ch);
		    if (index == 0) {
			Tcl_SetStringObj(resultPtr, buf,
				Tcl_UniCharToUtf(ch, buf));
			break;
		    }
		}
	    }
	    break;
	}
	case STR_LAST: {
	    register char *p;
	    int match;

	    if (objc != 4) {
	        goto badFirstLastArgs;
	    }

	    /*
	     * This algorithm fails on improperly formed UTF strings.
	     */

	    match = -1;
	    string1 = Tcl_GetStringFromObj(objv[2], &length1);
	    string2 = Tcl_GetStringFromObj(objv[3], &length2);
	    if (length1 > 0) {
		for (p = string2 + length2 - length1;  p >= string2;  p--) {
		    /*
		     * Scan backwards to find the first character.
		     */
		    
		    while ((p != string2) && (*p != *string1)) {
			p--;
		    }
		    if (memcmp(string1, p, (unsigned) length1) == 0) {
			match = p - string2;
			break;
		    }
		}
	    }

	    /*
	     * Compute the character index of the matching string by counting
	     * the number of characters before the match.
	     */

	    if (match != -1) {
		match = Tcl_NumUtfChars(string2, match);
	    }
	    Tcl_SetIntObj(resultPtr, match);
	    break;
	}
	case STR_LENGTH: {
	    if (objc != 3) {
	        Tcl_WrongNumArgs(interp, 2, objv, "string");
		return TCL_ERROR;
	    }

	    string1 = Tcl_GetStringFromObj(objv[2], &length1);
	    Tcl_SetIntObj(resultPtr, Tcl_NumUtfChars(string1, length1));
	    break;
	}
	case STR_MATCH: {
	    if (objc != 4) {
	        Tcl_WrongNumArgs(interp, 2, objv, "pattern string");
		return TCL_ERROR;
	    }

	    string1 = Tcl_GetStringFromObj(objv[2], &length1);
	    string2 = Tcl_GetStringFromObj(objv[3], &length2);
	    Tcl_SetBooleanObj(resultPtr, Tcl_StringMatch(string2, string1));
	    break;
	}
	case STR_RANGE: {
	    int first, last;

	    if (objc != 5) {
	        Tcl_WrongNumArgs(interp, 2, objv, "string first last");
		return TCL_ERROR;
	    }

	    string1 = Tcl_GetStringFromObj(objv[2], &length1);
	    length1 = Tcl_NumUtfChars(string1, length1);
	    if (TclGetIntForIndex(interp, objv[3], length1 - 1,
		    &first) != TCL_OK) {
		return TCL_ERROR;
	    }
	    if (TclGetIntForIndex(interp, objv[4], length1 - 1,
		    &last) != TCL_OK) {
		return TCL_ERROR;
	    }
	    if (first < 0) {
		first = 0;
	    }
	    if (last >= length1 - 1) {
		last = length1 - 1;
	    }
	    if (last >= first) {
		char *start, *end;

		start = Tcl_UtfAtIndex(string1, first);
		end = Tcl_UtfAtIndex(start, last - first + 1);
	        Tcl_SetStringObj(resultPtr, start, end - start);
	    }
	    break;
	}
	case STR_TOLOWER:
	case STR_TOUPPER:
	case STR_TOTITLE:
	    if (objc != 3) {
	        Tcl_WrongNumArgs(interp, 2, objv, "string");
		return TCL_ERROR;
	    }

	    string1 = Tcl_GetStringFromObj(objv[2], &length1);

	    /*
	     * Since the result object is not a shared object, it is
	     * safe to copy the string into the result and do the
	     * conversion in place.  The conversion may change the length
	     * of the string, so reset the length after conversion.
	     */

	    Tcl_SetStringObj(resultPtr, string1, length1);
	    if ((enum options) index == STR_TOLOWER) {
		length1 = Tcl_UtfToLower(Tcl_GetStringFromObj(resultPtr, NULL));
	    } else if ((enum options) index == STR_TOUPPER) {
		length1 = Tcl_UtfToUpper(Tcl_GetStringFromObj(resultPtr, NULL));
	    } else {
		length1 = Tcl_UtfToTitle(Tcl_GetStringFromObj(resultPtr, NULL));
	    }
	    Tcl_SetObjLength(resultPtr, length1);
	    break;

	case STR_TRIM: {
	    Tcl_UniChar ch, trim;
	    register char *p, *end;
	    char *check, *checkEnd;
	    int offset;

	    left = 1;
	    right = 1;

	    dotrim:
	    if (objc == 4) {
		string2 = Tcl_GetStringFromObj(objv[3], &length2);
	    } else if (objc == 3) {
		string2 = " \t\n\r";
		length2 = strlen(string2);
	    } else {
	        Tcl_WrongNumArgs(interp, 2, objv, "string ?chars?");
		return TCL_ERROR;
	    }
	    string1 = Tcl_GetStringFromObj(objv[2], &length1);
	    checkEnd = string2 + length2;

	    if (left) {
		end = string1 + length1;
		/*
		 * The outer loop iterates over the string.  The inner
		 * loop iterates over the trim characters.  The loops
		 * terminate as soon as a non-trim character is discovered
		 * and string1 is left pointing at the first non-trim
		 * character.
		 */

		for (p = string1; p < end; p += offset) {
		    offset = Tcl_UtfToUniChar(p, &ch);
		    
		    for (check = string2; ; ) {
			if (check >= checkEnd) {
			    p = end;
			    break;
			}
			check += Tcl_UtfToUniChar(check, &trim);
			if (ch == trim) {
			    length1 -= offset;
			    string1 += offset;
			    break;
			}
		    }
		}
	    }
	    if (right) {
	        end = string1;

		/*
		 * The outer loop iterates over the string.  The inner
		 * loop iterates over the trim characters.  The loops
		 * terminate as soon as a non-trim character is discovered
		 * and length1 marks the last non-trim character.
		 */

		for (p = string1 + length1; p > end; ) {
		    p = Tcl_UtfPrev(p, string1);
		    offset = Tcl_UtfToUniChar(p, &ch);
		    for (check = string2; ; ) {
		        if (check >= checkEnd) {
			    p = end;
			    break;
			}
			check += Tcl_UtfToUniChar(check, &trim);
			if (ch == trim) {
			    length1 -= offset;
			    break;
			}
		    }
		}
	    }
	    Tcl_SetStringObj(resultPtr, string1, length1);
	    break;
	}
	case STR_TRIMLEFT: {
	    left = 1;
	    right = 0;
	    goto dotrim;
	}
	case STR_TRIMRIGHT: {
	    left = 0;
	    right = 1;
	    goto dotrim;
	}
	case STR_WORDEND: {
	    int cur;
	    Tcl_UniChar ch;
	    char *p, *end;
	    int numChars;
	    
	    if (objc != 4) {
	        Tcl_WrongNumArgs(interp, 2, objv, "string index");
		return TCL_ERROR;
	    }

	    string1 = Tcl_GetStringFromObj(objv[2], &length1);
	    if (Tcl_GetIntFromObj(interp, objv[3], &index) != TCL_OK) {
	        return TCL_ERROR;
	    }
	    if (index < 0) {
		index = 0;
	    }
	    numChars = Tcl_NumUtfChars(string1, length1);
	    if (index < numChars) {
		p = Tcl_UtfAtIndex(string1, index);
		end = string1+length1;
		for (cur = index; p < end; cur++) {
		    p += Tcl_UtfToUniChar(p, &ch);
		    if (!TclUniCharIsWordChar(ch)) {
			break;
		    }
		}
		if (cur == index) {
		    cur++;
		}
	    } else {
		cur = numChars;
	    }
	    Tcl_SetIntObj(resultPtr, cur);
	    break;
	}
	case STR_WORDSTART: {
	    int cur;
	    Tcl_UniChar ch;
	    char *p;
	    int numChars;
	    
	    if (objc != 4) {
	        Tcl_WrongNumArgs(interp, 2, objv, "string index");
		return TCL_ERROR;
	    }

	    string1 = Tcl_GetStringFromObj(objv[2], &length1);
	    if (Tcl_GetIntFromObj(interp, objv[3], &index) != TCL_OK) {
		return TCL_ERROR;
	    }
	    numChars = Tcl_NumUtfChars(string1, length1);
	    if (index >= numChars) {
		index = numChars - 1;
	    }
	    cur = 0;
	    if (index > 0) {
		p = Tcl_UtfAtIndex(string1, index);
	        for (cur = index; cur >= 0; cur--) {
		    Tcl_UtfToUniChar(p, &ch);
		    if (!TclUniCharIsWordChar(ch)) {
			break;
		    }
		    p = Tcl_UtfPrev(p, string1);
		}
		if (cur != index) {
		    cur += 1;
		}
	    }
	    Tcl_SetIntObj(resultPtr, cur);
	    break;
	}
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SubstObjCmd --
 *
 *	This procedure is invoked to process the "subst" Tcl command.
 *	See the user documentation for details on what it does.  This
 *	command is an almost direct copy of an implementation by
 *	Andrew Payne.
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
Tcl_SubstObjCmd(dummy, interp, objc, objv)
    ClientData dummy;			/* Not used. */
    Tcl_Interp *interp;			/* Current interpreter. */
    int objc;				/* Number of arguments. */
    Tcl_Obj *CONST objv[];       	/* Argument objects. */
{
    static char *substOptions[] = {
	"-nobackslashes", "-nocommands", "-novariables", (char *) NULL
    };
    enum substOptions {
	SUBST_NOBACKSLASHES,      SUBST_NOCOMMANDS,       SUBST_NOVARS
    };
    Interp *iPtr = (Interp *) interp;
    Tcl_DString result;
    char *p, *old, *value;
    int optionIndex, code, count, doVars, doCmds, doBackslashes, i;

    /*
     * Parse command-line options.
     */

    doVars = doCmds = doBackslashes = 1;
    for (i = 1; i < (objc-1); i++) {
	p = Tcl_GetString(objv[i]);
	if (*p != '-') {
	    break;
	}
	if (Tcl_GetIndexFromObj(interp, objv[i], substOptions,
		"switch", 0, &optionIndex) != TCL_OK) {

	    return TCL_ERROR;
	}
	switch (optionIndex) {
	    case SUBST_NOBACKSLASHES: {
		doBackslashes = 0;
		break;
	    }
	    case SUBST_NOCOMMANDS: {
		doCmds = 0;
		break;
	    }
	    case SUBST_NOVARS: {
		doVars = 0;
		break;
	    }
	    default: {
		panic("Tcl_SubstObjCmd: bad option index to SubstOptions");
	    }
	}
    }
    if (i != (objc-1)) {
	Tcl_WrongNumArgs(interp, 1, objv,
		"?-nobackslashes? ?-nocommands? ?-novariables? string");
	return TCL_ERROR;
    }

    /*
     * Scan through the string one character at a time, performing
     * command, variable, and backslash substitutions.
     */

    Tcl_DStringInit(&result);
    old = p = Tcl_GetString(objv[i]);
    while (*p != 0) {
	switch (*p) {
	    case '\\':
		if (doBackslashes) {
		    char buf[TCL_UTF_MAX];

		    if (p != old) {
			Tcl_DStringAppend(&result, old, p-old);
		    }
		    Tcl_DStringAppend(&result, buf,
			    Tcl_UtfBackslash(p, &count, buf));
		    p += count;
		    old = p;
		} else {
		    p++;
		}
		break;

	    case '$':
		if (doVars) {
		    if (p != old) {
			Tcl_DStringAppend(&result, old, p-old);
		    }
		    value = Tcl_ParseVar(interp, p, &p);
		    if (value == NULL) {
			Tcl_DStringFree(&result);
			return TCL_ERROR;
		    }
		    Tcl_DStringAppend(&result, value, -1);
		    old = p;
		} else {
		    p++;
		}
		break;

	    case '[':
		if (doCmds) {
		    if (p != old) {
			Tcl_DStringAppend(&result, old, p-old);
		    }
		    iPtr->evalFlags = TCL_BRACKET_TERM;
		    code = Tcl_Eval(interp, p+1);
		    if (code == TCL_ERROR) {
			Tcl_DStringFree(&result);
			return code;
		    }
		    old = p = (p+1 + iPtr->termOffset+1);
		    Tcl_DStringAppend(&result, iPtr->result, -1);
		    Tcl_ResetResult(interp);
		} else {
		    p++;
		}
		break;

	    default:
		p++;
		break;
	}
    }
    if (p != old) {
	Tcl_DStringAppend(&result, old, p-old);
    }
    Tcl_DStringResult(interp, &result);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SwitchObjCmd --
 *
 *	This object-based procedure is invoked to process the "switch" Tcl
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
Tcl_SwitchObjCmd(dummy, interp, objc, objv)
    ClientData dummy;		/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    int i, j, index, mode, matched, result;
    char *string, *pattern;
    static char *options[] = {
	"-exact",	"-glob",	"-regexp",	"--", 
	NULL
    };
    enum options {
	OPT_EXACT,	OPT_GLOB,	OPT_REGEXP,	OPT_LAST
    };

    mode = OPT_EXACT;
    for (i = 1; i < objc; i++) {
	string = Tcl_GetString(objv[i]);
	if (string[0] != '-') {
	    break;
	}
	if (Tcl_GetIndexFromObj(interp, objv[i], options, "option", 0, 
		&index) != TCL_OK) {
	    return TCL_ERROR;
	}
	if (index == OPT_LAST) {
	    i++;
	    break;
	}
	mode = index;
    }

    if (objc - i < 2) {
	Tcl_WrongNumArgs(interp, 1, objv,
		"?switches? string pattern body ... ?default body?");
	return TCL_ERROR;
    }

    string = Tcl_GetString(objv[i]);
    objc -= i + 1;
    objv += i + 1;

    /*
     * If all of the pattern/command pairs are lumped into a single
     * argument, split them out again.
     */

    if (objc == 1) {
	Tcl_Obj **listv;

	if (Tcl_ListObjGetElements(interp, objv[0], &objc, &listv) != TCL_OK) {
	    return TCL_ERROR;
	}
	objv = listv;
    }

    for (i = 0; i < objc; i += 2) {
	if (i == objc - 1) {
	    Tcl_ResetResult(interp);
	    Tcl_AppendToObj(Tcl_GetObjResult(interp),
	            "extra switch pattern with no body", -1);
	    return TCL_ERROR;
	}

	/*
	 * See if the pattern matches the string.
	 */

	pattern = Tcl_GetString(objv[i]);
	matched = 0;
	if ((i == objc - 2) 
		&& (*pattern == 'd') 
		&& (strcmp(pattern, "default") == 0)) {
	    matched = 1;
	} else {
	    switch (mode) {
		case OPT_EXACT:
		    matched = (strcmp(string, pattern) == 0);
		    break;
		case OPT_GLOB:
		    matched = Tcl_StringMatch(string, pattern);
		    break;
		case OPT_REGEXP:
		    matched = TclRegExpMatchObj(interp, string, objv[i]);
		    if (matched < 0) {
			return TCL_ERROR;
		    }
		    break;
	    }
	}
	if (matched == 0) {
	    continue;
	}

	/*
	 * We've got a match. Find a body to execute, skipping bodies
	 * that are "-".
	 */

	for (j = i + 1; ; j += 2) {
	    if (j >= objc) {
		Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
			"no body specified for pattern \"", pattern,
			"\"", (char *) NULL);
		return TCL_ERROR;
	    }
	    if (strcmp(Tcl_GetString(objv[j]), "-") != 0) {
		break;
	    }
	}
	result = Tcl_EvalObjEx(interp, objv[j], 0);
	if (result == TCL_ERROR) {
	    char msg[100 + TCL_INTEGER_SPACE];

	    sprintf(msg, "\n    (\"%.50s\" arm line %d)", pattern,
		    interp->errorLine);
	    Tcl_AddObjErrorInfo(interp, msg, -1);
	}
	return result;
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_TimeObjCmd --
 *
 *	This object-based procedure is invoked to process the "time" Tcl
 *	command.  See the user documentation for details on what it does.
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
Tcl_TimeObjCmd(dummy, interp, objc, objv)
    ClientData dummy;		/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    register Tcl_Obj *objPtr;
    register int i, result;
    int count;
    double totalMicroSec;
    Tcl_Time start, stop;
    char buf[100];

    if (objc == 2) {
	count = 1;
    } else if (objc == 3) {
	result = Tcl_GetIntFromObj(interp, objv[2], &count);
	if (result != TCL_OK) {
	    return result;
	}
    } else {
	Tcl_WrongNumArgs(interp, 1, objv, "command ?count?");
	return TCL_ERROR;
    }
    
    objPtr = objv[1];
    i = count;
    TclpGetTime(&start);
    while (i-- > 0) {
	result = Tcl_EvalObjEx(interp, objPtr, 0);
	if (result != TCL_OK) {
	    return result;
	}
    }
    TclpGetTime(&stop);
    
    totalMicroSec =
	(stop.sec - start.sec)*1000000 + (stop.usec - start.usec);
    sprintf(buf, "%.0f microseconds per iteration",
	((count <= 0) ? 0 : totalMicroSec/count));
    Tcl_ResetResult(interp);
    Tcl_AppendToObj(Tcl_GetObjResult(interp), buf, -1);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_TraceObjCmd --
 *
 *	This procedure is invoked to process the "trace" Tcl command.
 *	See the user documentation for details on what it does.
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
Tcl_TraceObjCmd(dummy, interp, objc, objv)
    ClientData dummy;			/* Not used. */
    Tcl_Interp *interp;			/* Current interpreter. */
    int objc;				/* Number of arguments. */
    Tcl_Obj *CONST objv[];		/* Argument objects. */
{
    int optionIndex, commandLength;
    char *name, *rwuOps, *command, *p;
    size_t length;
    static char *traceOptions[] = {
	"variable", "vdelete", "vinfo", (char *) NULL
    };
    enum traceOptions {
	TRACE_VARIABLE,       TRACE_VDELETE,      TRACE_VINFO
    };

    if (objc < 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "option [arg arg ...]");
	return TCL_ERROR;
    }

    if (Tcl_GetIndexFromObj(interp, objv[1], traceOptions,
		"option", 0, &optionIndex) != TCL_OK) {
	return TCL_ERROR;
    }
    switch ((enum traceOptions) optionIndex) {
	    case TRACE_VARIABLE: {
		int flags;
		TraceVarInfo *tvarPtr;
		if (objc != 5) {
		    Tcl_WrongNumArgs(interp, 2, objv, "name ops command");
		    return TCL_ERROR;
		}

		flags = 0;
		rwuOps = Tcl_GetString(objv[3]);
		for (p = rwuOps; *p != 0; p++) {
		    if (*p == 'r') {
			flags |= TCL_TRACE_READS;
		    } else if (*p == 'w') {
			flags |= TCL_TRACE_WRITES;
		    } else if (*p == 'u') {
			flags |= TCL_TRACE_UNSETS;
		    } else {
			goto badOps;
		    }
		}
		if (flags == 0) {
		    goto badOps;
		}

		command = Tcl_GetStringFromObj(objv[4], &commandLength);
		length = (size_t) commandLength;
		tvarPtr = (TraceVarInfo *) ckalloc((unsigned)
			(sizeof(TraceVarInfo) - sizeof(tvarPtr->command)
				+ length + 1));
		tvarPtr->flags = flags;
		tvarPtr->errMsg = NULL;
		tvarPtr->length = length;
		flags |= TCL_TRACE_UNSETS;
		strcpy(tvarPtr->command, command);
		name = Tcl_GetString(objv[2]);
		if (Tcl_TraceVar(interp, name, flags, TraceVarProc,
			(ClientData) tvarPtr) != TCL_OK) {
		    ckfree((char *) tvarPtr);
		    return TCL_ERROR;
		}
		break;
	    }
	    case TRACE_VDELETE: {
		int flags;
		TraceVarInfo *tvarPtr;
		ClientData clientData;

		if (objc != 5) {
		    Tcl_WrongNumArgs(interp, 2, objv, "name ops command");
		    return TCL_ERROR;
		}

		flags = 0;
		rwuOps = Tcl_GetString(objv[3]);
		for (p = rwuOps; *p != 0; p++) {
		    if (*p == 'r') {
			flags |= TCL_TRACE_READS;
		    } else if (*p == 'w') {
			flags |= TCL_TRACE_WRITES;
		    } else if (*p == 'u') {
			flags |= TCL_TRACE_UNSETS;
		    } else {
			goto badOps;
		    }
		}
		if (flags == 0) {
		    goto badOps;
		}

		/*
		 * Search through all of our traces on this variable to
		 * see if there's one with the given command.  If so, then
		 * delete the first one that matches.
		 */
		
		command = Tcl_GetStringFromObj(objv[4], &commandLength);
		length = (size_t) commandLength;
		clientData = 0;
		name = Tcl_GetString(objv[2]);
		while ((clientData = Tcl_VarTraceInfo(interp, name, 0,
			TraceVarProc, clientData)) != 0) {
		    tvarPtr = (TraceVarInfo *) clientData;
		    if ((tvarPtr->length == length) && (tvarPtr->flags == flags)
			    && (strncmp(command, tvarPtr->command,
				    (size_t) length) == 0)) {
			Tcl_UntraceVar(interp, name, flags | TCL_TRACE_UNSETS,
				TraceVarProc, clientData);
			if (tvarPtr->errMsg != NULL) {
			    ckfree(tvarPtr->errMsg);
			}
			ckfree((char *) tvarPtr);
			break;
		    }
		}
		break;
	    }
	    case TRACE_VINFO: {
		ClientData clientData;
		char ops[4];
		Tcl_Obj *resultListPtr, *pairObjPtr, *elemObjPtr;

		if (objc != 3) {
		    Tcl_WrongNumArgs(interp, 2, objv, "name");
		    return TCL_ERROR;
		}
		resultListPtr = Tcl_GetObjResult(interp);
		clientData = 0;
		name = Tcl_GetString(objv[2]);
		while ((clientData = Tcl_VarTraceInfo(interp, name, 0,
			TraceVarProc, clientData)) != 0) {

		    TraceVarInfo *tvarPtr = (TraceVarInfo *) clientData;

		    pairObjPtr = Tcl_NewListObj(0, (Tcl_Obj **) NULL);
		    p = ops;
		    if (tvarPtr->flags & TCL_TRACE_READS) {
			*p = 'r';
			p++;
		    }
		    if (tvarPtr->flags & TCL_TRACE_WRITES) {
			*p = 'w';
			p++;
		    }
		    if (tvarPtr->flags & TCL_TRACE_UNSETS) {
			*p = 'u';
			p++;
		    }
		    *p = '\0';

		    /*
		     * Build a pair (2-item list) with the ops string as
		     * the first obj element and the tvarPtr->command string
		     * as the second obj element.  Append the pair (as an
		     * element) to the end of the result object list.
		     */

		    elemObjPtr = Tcl_NewStringObj(ops, -1);
		    Tcl_ListObjAppendElement(NULL, pairObjPtr, elemObjPtr);
		    elemObjPtr = Tcl_NewStringObj(tvarPtr->command, -1);
		    Tcl_ListObjAppendElement(NULL, pairObjPtr, elemObjPtr);
		    Tcl_ListObjAppendElement(interp, resultListPtr, pairObjPtr);
		}
		Tcl_SetObjResult(interp, resultListPtr);
		break;
	    }
	default: {
		panic("Tcl_TraceObjCmd: bad option index to TraceOptions");
	    }
    }
    return TCL_OK;

    badOps:
    Tcl_AppendResult(interp, "bad operations \"", rwuOps,
	    "\": should be one or more of rwu", (char *) NULL);
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * TraceVarProc --
 *
 *	This procedure is called to handle variable accesses that have
 *	been traced using the "trace" command.
 *
 * Results:
 *	Normally returns NULL.  If the trace command returns an error,
 *	then this procedure returns an error string.
 *
 * Side effects:
 *	Depends on the command associated with the trace.
 *
 *----------------------------------------------------------------------
 */

	/* ARGSUSED */
static char *
TraceVarProc(clientData, interp, name1, name2, flags)
    ClientData clientData;	/* Information about the variable trace. */
    Tcl_Interp *interp;		/* Interpreter containing variable. */
    char *name1;		/* Name of variable or array. */
    char *name2;		/* Name of element within array;  NULL means
				 * scalar variable is being referenced. */
    int flags;			/* OR-ed bits giving operation and other
				 * information. */
{
    Tcl_SavedResult state;
    TraceVarInfo *tvarPtr = (TraceVarInfo *) clientData;
    char *result;
    int code;
    Tcl_DString cmd;

    result = NULL;
    if (tvarPtr->errMsg != NULL) {
	ckfree(tvarPtr->errMsg);
	tvarPtr->errMsg = NULL;
    }
    if ((tvarPtr->flags & flags) && !(flags & TCL_INTERP_DESTROYED)) {

	/*
	 * Generate a command to execute by appending list elements
	 * for the two variable names and the operation.  The five
	 * extra characters are for three space, the opcode character,
	 * and the terminating null.
	 */

	if (name2 == NULL) {
	    name2 = "";
	}
	Tcl_DStringInit(&cmd);
	Tcl_DStringAppend(&cmd, tvarPtr->command, (int) tvarPtr->length);
	Tcl_DStringAppendElement(&cmd, name1);
	Tcl_DStringAppendElement(&cmd, name2);
	if (flags & TCL_TRACE_READS) {
	    Tcl_DStringAppend(&cmd, " r", 2);
	} else if (flags & TCL_TRACE_WRITES) {
	    Tcl_DStringAppend(&cmd, " w", 2);
	} else if (flags & TCL_TRACE_UNSETS) {
	    Tcl_DStringAppend(&cmd, " u", 2);
	}

	/*
	 * Execute the command.  Save the interp's result used for
	 * the command. We discard any object result the command returns.
	 */

	Tcl_SaveResult(interp, &state);

	code = Tcl_Eval(interp, Tcl_DStringValue(&cmd));
	if (code != TCL_OK) {	     /* copy error msg to result */
	    char *string;
	    int length;
	    
	    string = Tcl_GetStringFromObj(Tcl_GetObjResult(interp), &length);
	    tvarPtr->errMsg = (char *) ckalloc((unsigned) (length + 1));
	    memcpy(tvarPtr->errMsg, string, (size_t) (length + 1));
	    result = tvarPtr->errMsg;
	}

	Tcl_RestoreResult(interp, &state);

	Tcl_DStringFree(&cmd);
    }
    if (flags & TCL_TRACE_DESTROYED) {
	result = NULL;
	if (tvarPtr->errMsg != NULL) {
	    ckfree(tvarPtr->errMsg);
	}
	ckfree((char *) tvarPtr);
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_WhileObjCmd --
 *
 *      This procedure is invoked to process the "while" Tcl command.
 *      See the user documentation for details on what it does.
 *
 *	With the bytecode compiler, this procedure is only called when
 *	a command name is computed at runtime, and is "while" or the name
 *	to which "while" was renamed: e.g., "set z while; $z {$i<100} {}"
 *
 * Results:
 *      A standard Tcl result.
 *
 * Side effects:
 *      See the user documentation.
 *
 *----------------------------------------------------------------------
 */

        /* ARGSUSED */
int
Tcl_WhileObjCmd(dummy, interp, objc, objv)
    ClientData dummy;                   /* Not used. */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];       	/* Argument objects. */
{
    int result, value;

    if (objc != 3) {
	Tcl_WrongNumArgs(interp, 1, objv, "test command");
        return TCL_ERROR;
    }

    while (1) {
        result = Tcl_ExprBooleanObj(interp, objv[1], &value);
        if (result != TCL_OK) {
            return result;
        }
        if (!value) {
            break;
        }
        result = Tcl_EvalObjEx(interp, objv[2], 0);
        if ((result != TCL_OK) && (result != TCL_CONTINUE)) {
            if (result == TCL_ERROR) {
                char msg[32 + TCL_INTEGER_SPACE];

                sprintf(msg, "\n    (\"while\" body line %d)",
                        interp->errorLine);
                Tcl_AddErrorInfo(interp, msg);
            }
            break;
        }
    }
    if (result == TCL_BREAK) {
        result = TCL_OK;
    }
    if (result == TCL_OK) {
        Tcl_ResetResult(interp);
    }
    return result;
}

