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
 * Copyright (c) 1998-2000 Scriptics Corporation.
 * Copyright (c) 2002 ActiveState Corporation.
 * Copyright (c) 2003 Donal K. Fellows.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id$
 */

#include "tclInt.h"
#include "tclRegexp.h"

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
    Tcl_Obj *retVal;

    if (objc != 1) {
	Tcl_WrongNumArgs(interp, 1, objv, NULL);
	return TCL_ERROR;
    }

    retVal = Tcl_FSGetCwd(interp);
    if (retVal == NULL) {
	return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, retVal);
    Tcl_DecrRefCount(retVal);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_RegexpObjCmd --
 *
 *	This procedure is invoked to process the "regexp" Tcl command.
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
Tcl_RegexpObjCmd(dummy, interp, objc, objv)
    ClientData dummy;			/* Not used. */
    Tcl_Interp *interp;			/* Current interpreter. */
    int objc;				/* Number of arguments. */
    Tcl_Obj *CONST objv[];		/* Argument objects. */
{
    int i, indices, match, about, offset, all, doinline, numMatchesSaved;
    int cflags, eflags, stringLength;
    Tcl_RegExp regExpr;
    Tcl_Obj *objPtr, *resultPtr;
    Tcl_RegExpInfo info;
    static CONST char *options[] = {
	"-all",		"-about",	"-indices",	"-inline",
	"-expanded",	"-line",	"-linestop",	"-lineanchor",
	"-nocase",	"-start",	"--",		(char *) NULL
    };
    enum options {
	REGEXP_ALL,	REGEXP_ABOUT,	REGEXP_INDICES,	REGEXP_INLINE,
	REGEXP_EXPANDED,REGEXP_LINE,	REGEXP_LINESTOP,REGEXP_LINEANCHOR,
	REGEXP_NOCASE,	REGEXP_START,	REGEXP_LAST
    };

    indices	= 0;
    about	= 0;
    cflags	= TCL_REG_ADVANCED;
    eflags	= 0;
    offset	= 0;
    all		= 0;
    doinline	= 0;
    
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
	    case REGEXP_ALL: {
		all = 1;
		break;
	    }
	    case REGEXP_INDICES: {
		indices = 1;
		break;
	    }
	    case REGEXP_INLINE: {
		doinline = 1;
		break;
	    }
	    case REGEXP_NOCASE: {
		cflags |= TCL_REG_NOCASE;
		break;
	    }
	    case REGEXP_ABOUT: {
		about = 1;
		break;
	    }
	    case REGEXP_EXPANDED: {
		cflags |= TCL_REG_EXPANDED;
		break;
	    }
	    case REGEXP_LINE: {
		cflags |= TCL_REG_NEWLINE;
		break;
	    }
	    case REGEXP_LINESTOP: {
		cflags |= TCL_REG_NLSTOP;
		break;
	    }
	    case REGEXP_LINEANCHOR: {
		cflags |= TCL_REG_NLANCH;
		break;
	    }
	    case REGEXP_START: {
		if (++i >= objc) {
		    goto endOfForLoop;
		}
		if (Tcl_GetIntFromObj(interp, objv[i], &offset) != TCL_OK) {
		    return TCL_ERROR;
		}
		if (offset < 0) {
		    offset = 0;
		}
		break;
	    }
	    case REGEXP_LAST: {
		i++;
		goto endOfForLoop;
	    }
	}
    }

    endOfForLoop:
    if ((objc - i) < (2 - about)) {
	Tcl_WrongNumArgs(interp, 1, objv, 
	  "?switches? exp string ?matchVar? ?subMatchVar subMatchVar ...?");
	return TCL_ERROR;
    }
    objc -= i;
    objv += i;

    if (doinline && ((objc - 2) != 0)) {
	/*
	 * User requested -inline, but specified match variables - a no-no.
	 */
	Tcl_AppendResult(interp, "regexp match variables not allowed",
		" when using -inline", (char *) NULL);
	return TCL_ERROR;
    }

    /*
     * Handle the odd about case separately.
     */
    if (about) {
	regExpr = Tcl_GetRegExpFromObj(interp, objv[0], cflags);
	if ((regExpr == NULL) || (TclRegAbout(interp, regExpr) < 0)) {
	    return TCL_ERROR;
	}
	return TCL_OK;
    }

    /*
     * Get the length of the string that we are matching against so
     * we can do the termination test for -all matches.  Do this before
     * getting the regexp to avoid shimmering problems.
     */
    objPtr = objv[1];
    stringLength = Tcl_GetCharLength(objPtr);

    regExpr = Tcl_GetRegExpFromObj(interp, objv[0], cflags);
    if (regExpr == NULL) {
	return TCL_ERROR;
    }

    if (offset > 0) {
	/*
	 * Add flag if using offset (string is part of a larger string),
	 * so that "^" won't match.
	 */
	eflags |= TCL_REG_NOTBOL;
    }

    objc -= 2;
    objv += 2;
    resultPtr = Tcl_GetObjResult(interp);

    if (doinline) {
	/*
	 * Save all the subexpressions, as we will return them as a list
	 */
	numMatchesSaved = -1;
    } else {
	/*
	 * Save only enough subexpressions for matches we want to keep,
	 * expect in the case of -all, where we need to keep at least
	 * one to know where to move the offset.
	 */
	numMatchesSaved = (objc == 0) ? all : objc;
    }

    /*
     * The following loop is to handle multiple matches within the
     * same source string;  each iteration handles one match.  If "-all"
     * hasn't been specified then the loop body only gets executed once.
     * We terminate the loop when the starting offset is past the end of the
     * string.
     */

    while (1) {
	match = Tcl_RegExpExecObj(interp, regExpr, objPtr,
		offset /* offset */, numMatchesSaved, eflags 
		| ((offset > 0 &&
		   (Tcl_GetUniChar(objPtr,offset-1) != (Tcl_UniChar)'\n'))
		   ? TCL_REG_NOTBOL : 0));

	if (match < 0) {
	    return TCL_ERROR;
	}

	if (match == 0) {
	    /*
	     * We want to set the value of the intepreter result only when
	     * this is the first time through the loop.
	     */
	    if (all <= 1) {
		/*
		 * If inlining, set the interpreter's object result to an
		 * empty list, otherwise set it to an integer object w/
		 * value 0.
		 */
		if (doinline) {
		    Tcl_SetListObj(resultPtr, 0, NULL);
		} else {
		    Tcl_SetIntObj(resultPtr, 0);
		}
		return TCL_OK;
	    }
	    break;
	}

	/*
	 * If additional variable names have been specified, return
	 * index information in those variables.
	 */

	Tcl_RegExpGetInfo(regExpr, &info);
	if (doinline) {
	    /*
	     * It's the number of substitutions, plus one for the matchVar
	     * at index 0
	     */
	    objc = info.nsubs + 1;
	}
	for (i = 0; i < objc; i++) {
	    Tcl_Obj *newPtr;

	    if (indices) {
		int start, end;
		Tcl_Obj *objs[2];

		/*
		 * Only adjust the match area if there was a match for
		 * that area.  (Scriptics Bug 4391/SF Bug #219232)
		 */
		if (i <= info.nsubs && info.matches[i].start >= 0) {
		    start = offset + info.matches[i].start;
		    end   = offset + info.matches[i].end;

		    /*
		     * Adjust index so it refers to the last character in the
		     * match instead of the first character after the match.
		     */

		    if (end >= offset) {
			end--;
		    }
		} else {
		    start = -1;
		    end   = -1;
		}

		objs[0] = Tcl_NewLongObj(start);
		objs[1] = Tcl_NewLongObj(end);

		newPtr = Tcl_NewListObj(2, objs);
	    } else {
		if (i <= info.nsubs) {
		    newPtr = Tcl_GetRange(objPtr,
			    offset + info.matches[i].start,
			    offset + info.matches[i].end - 1);
		} else {
		    newPtr = Tcl_NewObj();
		}
	    }
	    if (doinline) {
		if (Tcl_ListObjAppendElement(interp, resultPtr, newPtr)
			!= TCL_OK) {
		    Tcl_DecrRefCount(newPtr);
		    return TCL_ERROR;
		}
	    } else {
		Tcl_Obj *valuePtr;
		valuePtr = Tcl_ObjSetVar2(interp, objv[i], NULL, newPtr, 0);
		if (valuePtr == NULL) {
		    Tcl_DecrRefCount(newPtr);
		    Tcl_AppendResult(interp, "couldn't set variable \"",
			    Tcl_GetString(objv[i]), "\"", (char *) NULL);
		    return TCL_ERROR;
		}
	    }
	}

	if (all == 0) {
	    break;
	}
	/*
	 * Adjust the offset to the character just after the last one
	 * in the matchVar and increment all to count how many times
	 * we are making a match.  We always increment the offset by at least
	 * one to prevent endless looping (as in the case:
	 * regexp -all {a*} a).  Otherwise, when we match the NULL string at
	 * the end of the input string, we will loop indefinately (because the
	 * length of the match is 0, so offset never changes).
	 */
	if (info.matches[0].end == 0) {
	    offset++;
	}
	offset += info.matches[0].end;
	all++;
	eflags |= TCL_REG_NOTBOL;
	if (offset >= stringLength) {
	    break;
	}
    }

    /*
     * Set the interpreter's object result to an integer object
     * with value 1 if -all wasn't specified, otherwise it's all-1
     * (the number of times through the while - 1).
     * Get the resultPtr again as the Tcl_ObjSetVar2 above may have
     * cause the result to change. [Patch #558324] (watson).
     */

    if (!doinline) {
	resultPtr = Tcl_GetObjResult(interp);
	Tcl_SetIntObj(resultPtr, (all ? all-1 : 1));
    }
    return TCL_OK;
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
    int idx, result, cflags, all, wlen, wsublen, numMatches, offset;
    int start, end, subStart, subEnd, match;
    Tcl_RegExp regExpr;
    Tcl_RegExpInfo info;
    Tcl_Obj *resultPtr, *subPtr, *objPtr;
    Tcl_UniChar ch, *wsrc, *wfirstChar, *wstring, *wsubspec, *wend;

    static CONST char *options[] = {
	"-all",		"-nocase",	"-expanded",
	"-line",	"-linestop",	"-lineanchor",	"-start",
	"--",		NULL
    };
    enum options {
	REGSUB_ALL,	REGSUB_NOCASE,	REGSUB_EXPANDED,
	REGSUB_LINE,	REGSUB_LINESTOP, REGSUB_LINEANCHOR,	REGSUB_START,
	REGSUB_LAST
    };

    cflags = TCL_REG_ADVANCED;
    all = 0;
    offset = 0;
    resultPtr = NULL;

    for (idx = 1; idx < objc; idx++) {
	char *name;
	int index;
	
	name = Tcl_GetString(objv[idx]);
	if (name[0] != '-') {
	    break;
	}
	if (Tcl_GetIndexFromObj(interp, objv[idx], options, "switch",
		TCL_EXACT, &index) != TCL_OK) {
	    return TCL_ERROR;
	}
	switch ((enum options) index) {
	    case REGSUB_ALL: {
		all = 1;
		break;
	    }
	    case REGSUB_NOCASE: {
		cflags |= TCL_REG_NOCASE;
		break;
	    }
	    case REGSUB_EXPANDED: {
		cflags |= TCL_REG_EXPANDED;
		break;
	    }
	    case REGSUB_LINE: {
		cflags |= TCL_REG_NEWLINE;
		break;
	    }
	    case REGSUB_LINESTOP: {
		cflags |= TCL_REG_NLSTOP;
		break;
	    }
	    case REGSUB_LINEANCHOR: {
		cflags |= TCL_REG_NLANCH;
		break;
	    }
	    case REGSUB_START: {
		if (++idx >= objc) {
		    goto endOfForLoop;
		}
		if (Tcl_GetIntFromObj(interp, objv[idx], &offset) != TCL_OK) {
		    return TCL_ERROR;
		}
		if (offset < 0) {
		    offset = 0;
		}
		break;
	    }
	    case REGSUB_LAST: {
		idx++;
		goto endOfForLoop;
	    }
	}
    }
    endOfForLoop:
    if (objc-idx < 3 || objc-idx > 4) {
	Tcl_WrongNumArgs(interp, 1, objv,
		"?switches? exp string subSpec ?varName?");
	return TCL_ERROR;
    }

    objc -= idx;
    objv += idx;

    if (all && (offset == 0)
	    && (strpbrk(Tcl_GetString(objv[2]), "&\\") == NULL)
	    && (strpbrk(Tcl_GetString(objv[0]), "*+?{}()[].\\|^$") == NULL)) {
	/*
	 * This is a simple one pair string map situation.  We make use of
	 * a slightly modified version of the one pair STR_MAP code.
	 */
	int slen, nocase;
	int (*strCmpFn)_ANSI_ARGS_((CONST Tcl_UniChar *, CONST Tcl_UniChar *,
		unsigned long));
	Tcl_UniChar *p, wsrclc;

	numMatches = 0;
	nocase     = (cflags & TCL_REG_NOCASE);
	strCmpFn   = nocase ? Tcl_UniCharNcasecmp : Tcl_UniCharNcmp;

	wsrc     = Tcl_GetUnicodeFromObj(objv[0], &slen);
	wstring  = Tcl_GetUnicodeFromObj(objv[1], &wlen);
	wsubspec = Tcl_GetUnicodeFromObj(objv[2], &wsublen);
	wend     = wstring + wlen - (slen ? slen - 1 : 0);
	result   = TCL_OK;

	if (slen == 0) {
	    /*
	     * regsub behavior for "" matches between each character.
	     * 'string map' skips the "" case.
	     */
	    if (wstring < wend) {
		resultPtr = Tcl_NewUnicodeObj(wstring, 0);
		Tcl_IncrRefCount(resultPtr);
		for (; wstring < wend; wstring++) {
		    Tcl_AppendUnicodeToObj(resultPtr, wsubspec, wsublen);
		    Tcl_AppendUnicodeToObj(resultPtr, wstring, 1);
		    numMatches++;
		}
		wlen = 0;
	    }
	} else {
	    wsrclc = Tcl_UniCharToLower(*wsrc);
	    for (p = wfirstChar = wstring; wstring < wend; wstring++) {
		if (((*wstring == *wsrc) ||
			(nocase && (Tcl_UniCharToLower(*wstring) ==
				wsrclc))) &&
			((slen == 1) || (strCmpFn(wstring, wsrc,
				(unsigned long) slen) == 0))) {
		    if (numMatches == 0) {
			resultPtr = Tcl_NewUnicodeObj(wstring, 0);
			Tcl_IncrRefCount(resultPtr);
		    }
		    if (p != wstring) {
			Tcl_AppendUnicodeToObj(resultPtr, p, wstring - p);
			p = wstring + slen;
		    } else {
			p += slen;
		    }
		    wstring = p - 1;

		    Tcl_AppendUnicodeToObj(resultPtr, wsubspec, wsublen);
		    numMatches++;
		}
	    }
	    if (numMatches) {
		wlen    = wfirstChar + wlen - p;
		wstring = p;
	    }
	}
	objPtr = NULL;
	subPtr = NULL;
	goto regsubDone;
    }

    regExpr = Tcl_GetRegExpFromObj(interp, objv[0], cflags);
    if (regExpr == NULL) {
	return TCL_ERROR;
    }

    /*
     * Make sure to avoid problems where the objects are shared.  This
     * can cause RegExpObj <> UnicodeObj shimmering that causes data
     * corruption.  [Bug #461322]
     */

    if (objv[1] == objv[0]) {
	objPtr = Tcl_DuplicateObj(objv[1]);
    } else {
	objPtr = objv[1];
    }
    wstring = Tcl_GetUnicodeFromObj(objPtr, &wlen);
    if (objv[2] == objv[0]) {
	subPtr = Tcl_DuplicateObj(objv[2]);
    } else {
	subPtr = objv[2];
    }
    wsubspec = Tcl_GetUnicodeFromObj(subPtr, &wsublen);

    result = TCL_OK;

    /*
     * The following loop is to handle multiple matches within the
     * same source string;  each iteration handles one match and its
     * corresponding substitution.  If "-all" hasn't been specified
     * then the loop body only gets executed once.  We must use
     * 'offset <= wlen' in particular for the case where the regexp
     * pattern can match the empty string - this is useful when
     * doing, say, 'regsub -- ^ $str ...' when $str might be empty.
     */

    numMatches = 0;
    for ( ; offset <= wlen; ) {

	/*
	 * The flags argument is set if string is part of a larger string,
	 * so that "^" won't match.
	 */

	match = Tcl_RegExpExecObj(interp, regExpr, objPtr, offset,
		10 /* matches */, ((offset > 0 &&
		   (wstring[offset-1] != (Tcl_UniChar)'\n'))
		   ? TCL_REG_NOTBOL : 0));

	if (match < 0) {
	    result = TCL_ERROR;
	    goto done;
	}
	if (match == 0) {
	    break;
	}
	if (numMatches == 0) {
	    resultPtr = Tcl_NewUnicodeObj(wstring, 0);
	    Tcl_IncrRefCount(resultPtr);
	    if (offset > 0) {
		/*
		 * Copy the initial portion of the string in if an offset
		 * was specified.
		 */
		Tcl_AppendUnicodeToObj(resultPtr, wstring, offset);
	    }
	}
	numMatches++;

	/*
	 * Copy the portion of the source string before the match to the
	 * result variable.
	 */

	Tcl_RegExpGetInfo(regExpr, &info);
	start = info.matches[0].start;
	end = info.matches[0].end;
	Tcl_AppendUnicodeToObj(resultPtr, wstring + offset, start);

	/*
	 * Append the subSpec argument to the variable, making appropriate
	 * substitutions.  This code is a bit hairy because of the backslash
	 * conventions and because the code saves up ranges of characters in
	 * subSpec to reduce the number of calls to Tcl_SetVar.
	 */

	wsrc = wfirstChar = wsubspec;
	wend = wsubspec + wsublen;
	for (ch = *wsrc; wsrc != wend; wsrc++, ch = *wsrc) {
	    if (ch == '&') {
		idx = 0;
	    } else if (ch == '\\') {
		ch = wsrc[1];
		if ((ch >= '0') && (ch <= '9')) {
		    idx = ch - '0';
		} else if ((ch == '\\') || (ch == '&')) {
		    *wsrc = ch;
		    Tcl_AppendUnicodeToObj(resultPtr, wfirstChar,
			    wsrc - wfirstChar + 1);
		    *wsrc = '\\';
		    wfirstChar = wsrc + 2;
		    wsrc++;
		    continue;
		} else {
		    continue;
		}
	    } else {
		continue;
	    }
	    if (wfirstChar != wsrc) {
		Tcl_AppendUnicodeToObj(resultPtr, wfirstChar,
			wsrc - wfirstChar);
	    }
	    if (idx <= info.nsubs) {
		subStart = info.matches[idx].start;
		subEnd = info.matches[idx].end;
		if ((subStart >= 0) && (subEnd >= 0)) {
		    Tcl_AppendUnicodeToObj(resultPtr,
			    wstring + offset + subStart, subEnd - subStart);
		}
	    }
	    if (*wsrc == '\\') {
		wsrc++;
	    }
	    wfirstChar = wsrc + 1;
	}
	if (wfirstChar != wsrc) {
	    Tcl_AppendUnicodeToObj(resultPtr, wfirstChar, wsrc - wfirstChar);
	}
	if (end == 0) {
	    /*
	     * Always consume at least one character of the input string
	     * in order to prevent infinite loops.
	     */

	    if (offset < wlen) {
		Tcl_AppendUnicodeToObj(resultPtr, wstring + offset, 1);
	    }
	    offset++;
	} else {
	    offset += end;
	    if (start == end) {
		/*
		 * We matched an empty string, which means we must go 
		 * forward one more step so we don't match again at the
		 * same spot.
		 */
		if (offset < wlen) {
		    Tcl_AppendUnicodeToObj(resultPtr, wstring + offset, 1);
		}
		offset++;
	    }
	}
	if (!all) {
	    break;
	}
    }

    /*
     * Copy the portion of the source string after the last match to the
     * result variable.
     */
    regsubDone:
    if (numMatches == 0) {
	/*
	 * On zero matches, just ignore the offset, since it shouldn't
	 * matter to us in this case, and the user may have skewed it.
	 */
	resultPtr = objv[1];
	Tcl_IncrRefCount(resultPtr);
    } else if (offset < wlen) {
	Tcl_AppendUnicodeToObj(resultPtr, wstring + offset, wlen - offset);
    }
    if (objc == 4) {
	if (Tcl_ObjSetVar2(interp, objv[3], NULL, resultPtr, 0) == NULL) {
	    Tcl_AppendResult(interp, "couldn't set variable \"",
		    Tcl_GetString(objv[3]), "\"", (char *) NULL);
	    result = TCL_ERROR;
	} else {
	    /*
	     * Set the interpreter's object result to an integer object
	     * holding the number of matches. 
	     */

	    Tcl_SetIntObj(Tcl_GetObjResult(interp), numMatches);
	}
    } else {
	/*
	 * No varname supplied, so just return the modified string.
	 */
	Tcl_SetObjResult(interp, resultPtr);
    }

    done:
    if (objPtr && (objv[1] == objv[0])) { Tcl_DecrRefCount(objPtr); }
    if (subPtr && (objv[2] == objv[0])) { Tcl_DecrRefCount(subPtr); }
    if (resultPtr) { Tcl_DecrRefCount(resultPtr); }
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
    int code, level;
    Tcl_Obj *returnOpts;

    /*
     * General syntax: [return ?-option value ...? ?result?]
     * An even number of words means an explicit result argument is present.
     */
    int explicitResult = (0 == (objc % 2));
    int numOptionWords = objc - 1 - explicitResult;

    if (TCL_ERROR == TclMergeReturnOptions(interp, numOptionWords, objv+1,
	    &returnOpts, &code, &level)) {
	return TCL_ERROR;
    }

    code = TclProcessReturn(interp, code, level, returnOpts);
    if (explicitResult) {
	Tcl_SetObjResult(interp, objv[objc-1]);
    }
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * TclProcessReturn --
 *
 *	Does the work of the [return] command based on the code,
 *	level, and returnOpts arguments.  Note that the code argument
 *	must agree with the -code entry in returnOpts and the level
 *	argument must agree with the -level entry in returnOpts, as
 *	is the case for values returned from TclMergeReturnOptions.
 *
 * Results:
 *	Returns the return code the [return] command should return.
 *
 * Side effects:
 *	When the return code is TCL_ERROR, the values of ::errorInfo
 *	and ::errorCode may be updated.
 *
 *----------------------------------------------------------------------
 */
int
TclProcessReturn(interp, code, level, returnOpts)
    Tcl_Interp *interp;
    int code;
    int level;
    Tcl_Obj *returnOpts;
{
    Interp *iPtr = (Interp *) interp;
    Tcl_Obj *valuePtr;

    /* Store the merged return options */
    if (iPtr->returnOpts != returnOpts) {
	Tcl_DecrRefCount(iPtr->returnOpts);
	iPtr->returnOpts = returnOpts;
	Tcl_IncrRefCount(iPtr->returnOpts);
    }

    if (level == 0) {
	if (code == TCL_ERROR) {
	    valuePtr = NULL;
	    Tcl_DictObjGet(NULL, iPtr->returnOpts,
		    iPtr->returnErrorinfoKey, &valuePtr);
	    if (valuePtr != NULL) {
		int infoLen;
		CONST char *info = Tcl_GetStringFromObj(valuePtr,&infoLen);
		if (infoLen) {
		    Tcl_AddObjErrorInfo(interp, info, infoLen);
		    iPtr->flags |= ERR_ALREADY_LOGGED;
		}
	    }
	    valuePtr = NULL;
	    Tcl_DictObjGet(NULL, iPtr->returnOpts,
		    iPtr->returnErrorcodeKey, &valuePtr);
	    if (valuePtr != NULL) {
		Tcl_SetVar2Ex(interp, "errorCode", NULL,
			valuePtr, TCL_GLOBAL_ONLY);
		iPtr->flags |= ERROR_CODE_SET;
	    }
	}
    } else {
	code = TCL_RETURN;
    }
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * TclMergeReturnOptions --
 *
 *	Parses, checks, and stores the options to the [return] command.
 *
 * Results:
 *	Returns TCL_ERROR is any of the option values are invalid.
 *	Otherwise, returns TCL_OK, and writes the returnOpts, code,
 *	and level values to the pointers provided.
 *
 * Side effects:
 * 	None.
 *
 *----------------------------------------------------------------------
 */

int
TclMergeReturnOptions(interp, objc, objv, optionsPtrPtr, codePtr, levelPtr)
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
    Tcl_Obj **optionsPtrPtr;	/* If not NULL, points to space for a
				 * (Tcl_Obj *) where the pointer to the
				 * merged return options dictionary should
				 * be written */
    int *codePtr;		/* If not NULL, points to space where the
				 * -code value should be written */
    int *levelPtr;		/* If not NULL, points to space where the
				 * -level value should be written */
{
    Interp *iPtr = (Interp *) interp;
    int code, level, size;
    Tcl_Obj *valuePtr;
    Tcl_Obj *returnOpts = Tcl_DuplicateObj(iPtr->defaultReturnOpts);

    for (;  objc > 1;  objv += 2, objc -= 2) {
	int optLen;
	CONST char *opt = Tcl_GetStringFromObj(objv[0], &optLen);
	int compareLen;
	CONST char *compare =
		Tcl_GetStringFromObj(iPtr->returnOptionsKey, &compareLen);

	if ((optLen == compareLen) && (strcmp(opt, compare) == 0)) {
	    Tcl_DictSearch search;
	    int done = 0;
	    Tcl_Obj *keyPtr;
	    Tcl_Obj *dict = objv[1];

	    nestedOptions:
	    if (TCL_ERROR == Tcl_DictObjFirst(NULL, dict,
		    &search, &keyPtr, &valuePtr, &done)) {
		/* Value is not a legal dictionary */
		Tcl_ResetResult(interp);
		Tcl_AppendStringsToObj(Tcl_GetObjResult(interp), "bad ",
			compare, " value: expected dictionary but got \"",
			Tcl_GetString(objv[1]), "\"", (char *) NULL);
		return TCL_ERROR;
	    }

	    while (!done) {
		Tcl_DictObjPut(NULL, returnOpts, keyPtr, valuePtr);
		Tcl_DictObjNext(&search, &keyPtr, &valuePtr, &done);
	    }

	    valuePtr = NULL;
	    Tcl_DictObjGet(NULL, returnOpts, iPtr->returnOptionsKey, &valuePtr);
	    if (valuePtr != NULL) {
		dict = valuePtr;
		Tcl_DictObjRemove(NULL, returnOpts, iPtr->returnOptionsKey);
		goto nestedOptions;
	    }

	} else {
	    Tcl_DictObjPut(NULL, returnOpts, objv[0], objv[1]);
	}
    }

    /* Check for bogus -code value */
    Tcl_DictObjGet(NULL, returnOpts, iPtr->returnCodeKey, &valuePtr);
    if (TCL_ERROR == Tcl_GetIntFromObj(NULL, valuePtr, &code)) {
	static CONST char *returnCodes[] = {
	    "ok", "error", "return", "break", "continue", NULL
	};

	if (TCL_ERROR == Tcl_GetIndexFromObj(NULL, valuePtr, returnCodes,
		NULL, TCL_EXACT, &code)) {
	    /* Value is not a legal return code */
	    Tcl_ResetResult(interp);
	    Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
		    "bad completion code \"",
		    Tcl_GetString(valuePtr),
		    "\": must be ok, error, return, break, ",
		    "continue, or an integer", (char *) NULL);
	    return TCL_ERROR;
	}
	/* Have a legal string value for a return code; convert to integer */
	Tcl_DictObjPut(NULL, returnOpts,
		iPtr->returnCodeKey, Tcl_NewIntObj(code));
    }

    /* Check for bogus -level value */
    Tcl_DictObjGet(NULL, returnOpts, iPtr->returnLevelKey, &valuePtr);
    if (TCL_ERROR == Tcl_GetIntFromObj(NULL, valuePtr, &level) || (level < 0)) {
	/* Value is not a legal level */
	Tcl_ResetResult(interp);
	Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
		"bad -level value: expected non-negative integer but got \"",
		Tcl_GetString(valuePtr), "\"", (char *) NULL);
	return TCL_ERROR;
    }

    /* 
     * Convert [return -code return -level X] to
     * [return -code ok -level X+1]
     */
    if (code == TCL_RETURN) {
	level++;
	Tcl_DictObjPut(NULL, returnOpts,
		iPtr->returnLevelKey, Tcl_NewIntObj(level));
	Tcl_DictObjPut(NULL, returnOpts,
		iPtr->returnCodeKey, Tcl_NewIntObj(TCL_OK));
    }

    /*
     * Check if we just have the default options.  If so, use them.
     * A dictionary equality test would be more robust, but seems
     * tricky, to say the least.
     */
    Tcl_DictObjSize(NULL, returnOpts, &size);
    if (size == 2 && code == TCL_OK && level == 1) {
	Tcl_DecrRefCount(returnOpts);
	returnOpts = iPtr->defaultReturnOpts;
    }
    if (codePtr != NULL) {
	*codePtr = code;
    }
    if (levelPtr != NULL) {
	*levelPtr = level;
    }
    if ((optionsPtrPtr == NULL) && (returnOpts != iPtr->defaultReturnOpts)) {
	/* not passing back the options (?!), so clean them up */
	Tcl_DecrRefCount(returnOpts);
    } else {
	*optionsPtrPtr = returnOpts;
    }
    return TCL_OK;
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
    CONST char *encodingName = NULL;
    Tcl_Obj *fileName;

    if (objc != 2 && objc !=4) {
	Tcl_WrongNumArgs(interp, 1, objv, "?-encoding name? fileName");
	return TCL_ERROR;
    }
    fileName = objv[objc-1];
    if (objc == 4) {
	static CONST char *options[] = {
	    "-encoding", (char *) NULL
	};
	int index;
	if (TCL_ERROR == Tcl_GetIndexFromObj(interp, objv[1],
		options, "option", TCL_EXACT, &index)) {
	    return TCL_ERROR;
	}
	encodingName = Tcl_GetString(objv[2]);
    }
    return Tcl_FSEvalFileEx(interp, fileName, encodingName);
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
	Tcl_HashTable charReuseTable;
	Tcl_HashEntry *hPtr;
	int isNew;

	/*
	 * Handle the special case of splitting on every character.
	 *
	 * Uses a hash table to ensure that each kind of character has
	 * only one Tcl_Obj instance (multiply-referenced) in the
	 * final list.  This is a *major* win when splitting on a long
	 * string (especially in the megabyte range!) - DKF
	 */

	Tcl_InitHashTable(&charReuseTable, TCL_ONE_WORD_KEYS);
	for ( ; string < end; string += len) {
	    len = TclUtfToUniChar(string, &ch);
	    /* Assume Tcl_UniChar is an integral type... */
	    hPtr = Tcl_CreateHashEntry(&charReuseTable, (char*)0 + ch, &isNew);
	    if (isNew) {
		objPtr = Tcl_NewStringObj(string, len);
		/* Don't need to fiddle with refcount... */
		Tcl_SetHashValue(hPtr, (ClientData) objPtr);
	    } else {
		objPtr = (Tcl_Obj*) Tcl_GetHashValue(hPtr);
	    }
	    Tcl_ListObjAppendElement(NULL, listPtr, objPtr);
	}
	Tcl_DeleteHashTable(&charReuseTable);
    } else if (splitCharLen == 1) {
	char *p;

	/*
	 * Handle the special case of splitting on a single character.
	 * This is only true for the one-char ASCII case, as one unicode
	 * char is > 1 byte in length.
	 */

	while (*string && (p = strchr(string, (int) *splitChars)) != NULL) {
	    objPtr = Tcl_NewStringObj(string, p - string);
	    Tcl_ListObjAppendElement(NULL, listPtr, objPtr);
	    string = p + 1;
	}
	objPtr = Tcl_NewStringObj(string, end - string);
	Tcl_ListObjAppendElement(NULL, listPtr, objPtr);
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
	    len = TclUtfToUniChar(string, &ch);
	    for (p = splitChars; p < splitEnd; p += splitLen) {
		splitLen = TclUtfToUniChar(p, &splitChar);
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
 *	Note that the primary methods here (equal, compare, match, ...)
 *	have bytecode equivalents.  You will find the code for those in
 *	tclExecute.c.  The code here will only be used in the non-bc
 *	case (like in an 'eval').
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
    static CONST char *options[] = {
	"bytelength",	"compare",	"equal",	"first",
	"index",	"is",		"last",		"length",
	"map",		"match",	"range",	"repeat",
	"replace",	"tolower",	"toupper",	"totitle",
	"trim",		"trimleft",	"trimright",
	"wordend",	"wordstart",	(char *) NULL
    };
    enum options {
	STR_BYTELENGTH,	STR_COMPARE,	STR_EQUAL,	STR_FIRST,
	STR_INDEX,	STR_IS,		STR_LAST,	STR_LENGTH,
	STR_MAP,	STR_MATCH,	STR_RANGE,	STR_REPEAT,
	STR_REPLACE,	STR_TOLOWER,	STR_TOUPPER,	STR_TOTITLE,
	STR_TRIM,	STR_TRIMLEFT,	STR_TRIMRIGHT,
	STR_WORDEND,	STR_WORDSTART
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
	case STR_EQUAL:
	case STR_COMPARE: {
	    /*
	     * Remember to keep code here in some sync with the
	     * byte-compiled versions in tclExecute.c (INST_STR_EQ,
	     * INST_STR_NEQ and INST_STR_CMP as well as the expr string
	     * comparison in INST_EQ/INST_NEQ/INST_LT/...).
	     */
	    int i, match, length, nocase = 0, reqlength = -1;
	    int (*strCmpFn)();

	    if (objc < 4 || objc > 7) {
	    str_cmp_args:
	        Tcl_WrongNumArgs(interp, 2, objv,
				 "?-nocase? ?-length int? string1 string2");
		return TCL_ERROR;
	    }

	    for (i = 2; i < objc-2; i++) {
		string2 = Tcl_GetStringFromObj(objv[i], &length2);
		if ((length2 > 1)
			&& strncmp(string2, "-nocase", (size_t)length2) == 0) {
		    nocase = 1;
		} else if ((length2 > 1)
			&& strncmp(string2, "-length", (size_t)length2) == 0) {
		    if (i+1 >= objc-2) {
			goto str_cmp_args;
		    }
		    if (Tcl_GetIntFromObj(interp, objv[++i],
			    &reqlength) != TCL_OK) {
			return TCL_ERROR;
		    }
		} else {
		    Tcl_AppendStringsToObj(resultPtr, "bad option \"",
			    string2, "\": must be -nocase or -length",
			    (char *) NULL);
		    return TCL_ERROR;
		}
	    }

	    /*
	     * From now on, we only access the two objects at the end
	     * of the argument array.
	     */
	    objv += objc-2;

	    if ((reqlength == 0) || (objv[0] == objv[1])) {
		/*
		 * Alway match at 0 chars of if it is the same obj.
		 */

		Tcl_SetBooleanObj(resultPtr,
			((enum options) index == STR_EQUAL));
		break;
	    } else if (!nocase && objv[0]->typePtr == &tclByteArrayType &&
		    objv[1]->typePtr == &tclByteArrayType) {
		/*
		 * Use binary versions of comparisons since that won't
		 * cause undue type conversions and it is much faster.
		 * Only do this if we're case-sensitive (which is all
		 * that really makes sense with byte arrays anyway, and
		 * we have no memcasecmp() for some reason... :^)
		 */
		string1 = (char*) Tcl_GetByteArrayFromObj(objv[0], &length1);
		string2 = (char*) Tcl_GetByteArrayFromObj(objv[1], &length2);
		strCmpFn = memcmp;
	    } else if ((objv[0]->typePtr == &tclStringType)
		    && (objv[1]->typePtr == &tclStringType)) {
		/*
		 * Do a unicode-specific comparison if both of the args
		 * are of String type.  In benchmark testing this proved
		 * the most efficient check between the unicode and
		 * string comparison operations.
		 */
		string1 = (char*) Tcl_GetUnicodeFromObj(objv[0], &length1);
		string2 = (char*) Tcl_GetUnicodeFromObj(objv[1], &length2);
		strCmpFn = nocase ? Tcl_UniCharNcasecmp : Tcl_UniCharNcmp;
	    } else {
		/*
		 * As a catch-all we will work with UTF-8.  We cannot use
		 * memcmp() as that is unsafe with any string containing
		 * NULL (\xC0\x80 in Tcl's utf rep).  We can use the more
		 * efficient TclpUtfNcmp2 if we are case-sensitive and no
		 * specific length was requested.
		 */
		string1 = (char*) Tcl_GetStringFromObj(objv[0], &length1);
		string2 = (char*) Tcl_GetStringFromObj(objv[1], &length2);
		if ((reqlength < 0) && !nocase) {
		    strCmpFn = TclpUtfNcmp2;
		} else {
		    length1 = Tcl_NumUtfChars(string1, length1);
		    length2 = Tcl_NumUtfChars(string2, length2);
		    strCmpFn = nocase ? Tcl_UtfNcasecmp : Tcl_UtfNcmp;
		}
	    }

	    if (((enum options) index == STR_EQUAL)
		    && (reqlength < 0) && (length1 != length2)) {
		match = 1; /* this will be reversed below */
	    } else {
		length = (length1 < length2) ? length1 : length2;
		if (reqlength > 0 && reqlength < length) {
		    length = reqlength;
		} else if (reqlength < 0) {
		    /*
		     * The requested length is negative, so we ignore it by
		     * setting it to length + 1 so we correct the match var.
		     */
		    reqlength = length + 1;
		}
		match = strCmpFn(string1, string2, (unsigned) length);
		if ((match == 0) && (reqlength > length)) {
		    match = length1 - length2;
		}
	    }

	    if ((enum options) index == STR_EQUAL) {
		Tcl_SetBooleanObj(resultPtr, (match) ? 0 : 1);
	    } else {
		Tcl_SetIntObj(resultPtr, ((match > 0) ? 1 :
					  (match < 0) ? -1 : 0));
	    }
	    break;
	}
	case STR_FIRST: {
	    Tcl_UniChar *ustring1, *ustring2;
	    int match, start;

	    if (objc < 4 || objc > 5) {
	        Tcl_WrongNumArgs(interp, 2, objv,
				 "subString string ?startIndex?");
		return TCL_ERROR;
	    }

	    /*
	     * We are searching string2 for the sequence string1.
	     */

	    match = -1;
	    start = 0;
	    length2 = -1;

	    ustring1 = Tcl_GetUnicodeFromObj(objv[2], &length1);
	    ustring2 = Tcl_GetUnicodeFromObj(objv[3], &length2);

	    if (objc == 5) {
		/*
		 * If a startIndex is specified, we will need to fast
		 * forward to that point in the string before we think
		 * about a match
		 */
		if (TclGetIntForIndex(interp, objv[4], length2 - 1,
			&start) != TCL_OK) {
		    return TCL_ERROR;
		}
		if (start >= length2) {
		    goto str_first_done;
		} else if (start > 0) {
		    ustring2 += start;
		    length2  -= start;
		} else if (start < 0) {
		    /*
		     * Invalid start index mapped to string start;
		     * Bug #423581
		     */
		    start = 0;
		}
	    }

	    if (length1 > 0) {
		register Tcl_UniChar *p, *end;

		end = ustring2 + length2 - length1 + 1;
		for (p = ustring2;  p < end;  p++) {
		    /*
		     * Scan forward to find the first character.
		     */
		    if ((*p == *ustring1) &&
			    (TclUniCharNcmp(ustring1, p,
				    (unsigned long) length1) == 0)) {
			match = p - ustring2;
			break;
		    }
		}
	    }
	    /*
	     * Compute the character index of the matching string by
	     * counting the number of characters before the match.
	     */
	    if ((match != -1) && (objc == 5)) {
		match += start;
	    }

	    str_first_done:
	    Tcl_SetIntObj(resultPtr, match);
	    break;
	}
	case STR_INDEX: {
	    if (objc != 4) {
	        Tcl_WrongNumArgs(interp, 2, objv, "string charIndex");
		return TCL_ERROR;
	    }

	    /*
	     * If we have a ByteArray object, avoid indexing in the
	     * Utf string since the byte array contains one byte per
	     * character.  Otherwise, use the Unicode string rep to
	     * get the index'th char.
	     */

	    if (objv[2]->typePtr == &tclByteArrayType) {
		string1 = (char *) Tcl_GetByteArrayFromObj(objv[2], &length1);

		if (TclGetIntForIndex(interp, objv[3], length1 - 1,
			&index) != TCL_OK) {
		    return TCL_ERROR;
		}
		if ((index >= 0) && (index < length1)) {
		    Tcl_SetByteArrayObj(resultPtr,
			    (unsigned char *)(&string1[index]), 1);
		}
	    } else {
		/*
		 * Get Unicode char length to calulate what 'end' means.
		 */
		length1 = Tcl_GetCharLength(objv[2]);

		if (TclGetIntForIndex(interp, objv[3], length1 - 1,
			&index) != TCL_OK) {
		    return TCL_ERROR;
		}
		if ((index >= 0) && (index < length1)) {
		    char buf[TCL_UTF_MAX];
		    Tcl_UniChar ch;

		    ch      = Tcl_GetUniChar(objv[2], index);
		    length1 = Tcl_UniCharToUtf(ch, buf);
		    Tcl_SetStringObj(resultPtr, buf, length1);
		}
	    }
	    break;
	}
	case STR_IS: {
	    char *end;
	    Tcl_UniChar ch;

            /*
	     * The UniChar comparison function
	     */

	    int (*chcomp)_ANSI_ARGS_((int)) = NULL; 
	    int i, failat = 0, result = 1, strict = 0;
	    Tcl_Obj *objPtr, *failVarObj = NULL;

	    static CONST char *isOptions[] = {
		"alnum",	"alpha",	"ascii",	"control",
		"boolean",	"digit",	"double",	"false",
		"graph",	"integer",	"lower",	"print",
		"punct",	"space",	"true",		"upper",
		"wordchar",	"xdigit",	(char *) NULL
	    };
	    enum isOptions {
		STR_IS_ALNUM,	STR_IS_ALPHA,	STR_IS_ASCII,	STR_IS_CONTROL,
		STR_IS_BOOL,	STR_IS_DIGIT,	STR_IS_DOUBLE,	STR_IS_FALSE,
		STR_IS_GRAPH,	STR_IS_INT,	STR_IS_LOWER,	STR_IS_PRINT,
		STR_IS_PUNCT,	STR_IS_SPACE,	STR_IS_TRUE,	STR_IS_UPPER,
		STR_IS_WORD,	STR_IS_XDIGIT
	    };

	    if (objc < 4 || objc > 7) {
		Tcl_WrongNumArgs(interp, 2, objv,
				 "class ?-strict? ?-failindex var? str");
		return TCL_ERROR;
	    }
	    if (Tcl_GetIndexFromObj(interp, objv[2], isOptions, "class", 0,
				    &index) != TCL_OK) {
		return TCL_ERROR;
	    }
	    if (objc != 4) {
		for (i = 3; i < objc-1; i++) {
		    string2 = Tcl_GetStringFromObj(objv[i], &length2);
		    if ((length2 > 1) &&
			strncmp(string2, "-strict", (size_t) length2) == 0) {
			strict = 1;
		    } else if ((length2 > 1) &&
			    strncmp(string2, "-failindex",
				    (size_t) length2) == 0) {
			if (i+1 >= objc-1) {
			    Tcl_WrongNumArgs(interp, 3, objv,
					     "?-strict? ?-failindex var? str");
			    return TCL_ERROR;
			}
			failVarObj = objv[++i];
		    } else {
			Tcl_AppendStringsToObj(resultPtr, "bad option \"",
				string2, "\": must be -strict or -failindex",
				(char *) NULL);
			return TCL_ERROR;
		    }
		}
	    }

	    /*
	     * We get the objPtr so that we can short-cut for some classes
	     * by checking the object type (int and double), but we need
	     * the string otherwise, because we don't want any conversion
	     * of type occuring (as, for example, Tcl_Get*FromObj would do
	     */
	    objPtr = objv[objc-1];
	    string1 = Tcl_GetStringFromObj(objPtr, &length1);
	    if (length1 == 0) {
		if (strict) {
		    result = 0;
		}
		goto str_is_done;
	    }
	    end = string1 + length1;

	    /*
	     * When entering here, result == 1 and failat == 0
	     */
	    switch ((enum isOptions) index) {
		case STR_IS_ALNUM:
		    chcomp = Tcl_UniCharIsAlnum;
		    break;
		case STR_IS_ALPHA:
		    chcomp = Tcl_UniCharIsAlpha;
		    break;
		case STR_IS_ASCII:
		    for (; string1 < end; string1++, failat++) {
			/*
			 * This is a valid check in unicode, because all
			 * bytes < 0xC0 are single byte chars (but isascii
			 * limits that def'n to 0x80).
			 */
			if (*((unsigned char *)string1) >= 0x80) {
			    result = 0;
			    break;
			}
		    }
		    break;
		case STR_IS_BOOL:
		case STR_IS_TRUE:
		case STR_IS_FALSE:
		    if (objPtr->typePtr == &tclBooleanType) {
			if ((((enum isOptions) index == STR_IS_TRUE) &&
			     objPtr->internalRep.longValue == 0) ||
			    (((enum isOptions) index == STR_IS_FALSE) &&
			     objPtr->internalRep.longValue != 0)) {
			    result = 0;
			}
		    } else if ((Tcl_GetBoolean(NULL, string1, &i)
				== TCL_ERROR) ||
			       (((enum isOptions) index == STR_IS_TRUE) &&
				i == 0) ||
			       (((enum isOptions) index == STR_IS_FALSE) &&
				i != 0)) {
			result = 0;
		    }
		    break;
		case STR_IS_CONTROL:
		    chcomp = Tcl_UniCharIsControl;
		    break;
		case STR_IS_DIGIT:
		    chcomp = Tcl_UniCharIsDigit;
		    break;
		case STR_IS_DOUBLE: {
		    char *stop;

		    if ((objPtr->typePtr == &tclDoubleType) ||
			(objPtr->typePtr == &tclIntType)) {
			break;
		    }
		    /*
		     * This is adapted from Tcl_GetDouble
		     *
		     * The danger in this function is that
		     * "12345678901234567890" is an acceptable 'double',
		     * but will later be interp'd as an int by something
		     * like [expr].  Therefore, we check to see if it looks
		     * like an int, and if so we do a range check on it.
		     * If strtoul gets to the end, we know we either
		     * received an acceptable int, or over/underflow
		     */
		    if (TclLooksLikeInt(string1, length1)) {
			errno = 0;
#ifdef TCL_WIDE_INT_IS_LONG
			strtoul(string1, &stop, 0); /* INTL: Tcl source. */
#else
			strtoull(string1, &stop, 0); /* INTL: Tcl source. */
#endif
			if (stop == end) {
			    if (errno == ERANGE) {
				result = 0;
				failat = -1;
			    }
			    break;
			}
		    }
		    errno = 0;
		    strtod(string1, &stop); /* INTL: Tcl source. */
		    if (errno == ERANGE) {
			/*
			 * if (errno == ERANGE), then it was an over/underflow
			 * problem, but in this method, we only want to know
			 * yes or no, so bad flow returns 0 (false) and sets
			 * the failVarObj to the string length.
			 */
			result = 0;
			failat = -1;
		    } else if (stop == string1) {
			/*
			 * In this case, nothing like a number was found
			 */
			result = 0;
			failat = 0;
		    } else {
			/*
			 * Assume we sucked up one char per byte
			 * and then we go onto SPACE, since we are
			 * allowed trailing whitespace
			 */
			failat = stop - string1;
			string1 = stop;
			chcomp = Tcl_UniCharIsSpace;
		    }
		    break;
		}
		case STR_IS_GRAPH:
		    chcomp = Tcl_UniCharIsGraph;
		    break;
		case STR_IS_INT: {
		    char *stop;
		    long int l = 0;

		    if (TCL_OK == Tcl_GetIntFromObj(NULL, objPtr, &i)) {
			break;
		    }
		    /*
		     * Like STR_IS_DOUBLE, but we use strtoul.
		     * Since Tcl_GetIntFromObj already failed,
		     * we set result to 0.
		     */
		    result = 0;
		    errno = 0;
		    l = strtol(string1, &stop, 0); /* INTL: Tcl source. */
		    if ((errno == ERANGE) || (l > INT_MAX) || (l < INT_MIN)) {
			/*
			 * if (errno == ERANGE), then it was an over/underflow
			 * problem, but in this method, we only want to know
			 * yes or no, so bad flow returns 0 (false) and sets
			 * the failVarObj to the string length.
			 */
			failat = -1;

		    } else if (stop == string1) {
			/*
			 * In this case, nothing like a number was found
			 */
			failat = 0;
		    } else {
			/*
			 * Assume we sucked up one char per byte
			 * and then we go onto SPACE, since we are
			 * allowed trailing whitespace
			 */
			failat = stop - string1;
			string1 = stop;
			chcomp = Tcl_UniCharIsSpace;
		    }
		    break;
		}
		case STR_IS_LOWER:
		    chcomp = Tcl_UniCharIsLower;
		    break;
		case STR_IS_PRINT:
		    chcomp = Tcl_UniCharIsPrint;
		    break;
		case STR_IS_PUNCT:
		    chcomp = Tcl_UniCharIsPunct;
		    break;
		case STR_IS_SPACE:
		    chcomp = Tcl_UniCharIsSpace;
		    break;
		case STR_IS_UPPER:
		    chcomp = Tcl_UniCharIsUpper;
		    break;
		case STR_IS_WORD:
		    chcomp = Tcl_UniCharIsWordChar;
		    break;
		case STR_IS_XDIGIT: {
		    for (; string1 < end; string1++, failat++) {
			/* INTL: We assume unicode is bad for this class */
			if ((*((unsigned char *)string1) >= 0xC0) ||
			    !isxdigit(*(unsigned char *)string1)) {
			    result = 0;
			    break;
			}
		    }
		    break;
		}
	    }
	    if (chcomp != NULL) {
		for (; string1 < end; string1 += length2, failat++) {
		    length2 = TclUtfToUniChar(string1, &ch);
		    if (!chcomp(ch)) {
			result = 0;
			break;
		    }
		}
	    }
	str_is_done:
	    /*
	     * Only set the failVarObj when we will return 0
	     * and we have indicated a valid fail index (>= 0)
	     */
	    if ((result == 0) && (failVarObj != NULL) &&
		Tcl_ObjSetVar2(interp, failVarObj, NULL, Tcl_NewIntObj(failat),
			       TCL_LEAVE_ERR_MSG) == NULL) {
		return TCL_ERROR;
	    }
	    Tcl_SetBooleanObj(resultPtr, result);
	    break;
	}
	case STR_LAST: {
	    Tcl_UniChar *ustring1, *ustring2, *p;
	    int match, start;

	    if (objc < 4 || objc > 5) {
	        Tcl_WrongNumArgs(interp, 2, objv,
				 "subString string ?startIndex?");
		return TCL_ERROR;
	    }

	    /*
	     * We are searching string2 for the sequence string1.
	     */

	    match = -1;
	    start = 0;
	    length2 = -1;

	    ustring1 = Tcl_GetUnicodeFromObj(objv[2], &length1);
	    ustring2 = Tcl_GetUnicodeFromObj(objv[3], &length2);

	    if (objc == 5) {
		/*
		 * If a startIndex is specified, we will need to restrict
		 * the string range to that char index in the string
		 */
		if (TclGetIntForIndex(interp, objv[4], length2 - 1,
			&start) != TCL_OK) {
		    return TCL_ERROR;
		}
		if (start < 0) {
		    goto str_last_done;
		} else if (start < length2) {
		    p = ustring2 + start + 1 - length1;
		} else {
		    p = ustring2 + length2 - length1;
		}
	    } else {
		p = ustring2 + length2 - length1;
	    }

	    if (length1 > 0) {
		for (; p >= ustring2;  p--) {
		    /*
		     * Scan backwards to find the first character.
		     */
		    if ((*p == *ustring1) &&
			    (memcmp((char *) ustring1, (char *) p, (size_t)
				    (length1 * sizeof(Tcl_UniChar))) == 0)) {
			match = p - ustring2;
			break;
		    }
		}
	    }

	    str_last_done:
	    Tcl_SetIntObj(resultPtr, match);
	    break;
	}
	case STR_BYTELENGTH:
	case STR_LENGTH: {
	    if (objc != 3) {
	        Tcl_WrongNumArgs(interp, 2, objv, "string");
		return TCL_ERROR;
	    }

	    if ((enum options) index == STR_BYTELENGTH) {
		(void) Tcl_GetStringFromObj(objv[2], &length1);
	    } else {
		/*
		 * If we have a ByteArray object, avoid recomputing the
		 * string since the byte array contains one byte per
		 * character.  Otherwise, use the Unicode string rep to
		 * calculate the length.
		 */

		if (objv[2]->typePtr == &tclByteArrayType) {
		    (void) Tcl_GetByteArrayFromObj(objv[2], &length1);
		} else {
		    length1 = Tcl_GetCharLength(objv[2]);
		}
	    }
	    Tcl_SetIntObj(resultPtr, length1);
	    break;
	}
	case STR_MAP: {
	    int mapElemc, nocase = 0, mapWithDict = 0;
	    Tcl_Obj **mapElemv;
	    Tcl_UniChar *ustring1, *ustring2, *p, *end;
	    int (*strCmpFn)_ANSI_ARGS_((CONST Tcl_UniChar*,
		    CONST Tcl_UniChar*, unsigned long));

	    if (objc < 4 || objc > 5) {
	        Tcl_WrongNumArgs(interp, 2, objv, "?-nocase? charMap string");
		return TCL_ERROR;
	    }

	    if (objc == 5) {
		string2 = Tcl_GetStringFromObj(objv[2], &length2);
		if ((length2 > 1) &&
		    strncmp(string2, "-nocase", (size_t) length2) == 0) {
		    nocase = 1;
		} else {
		    Tcl_AppendStringsToObj(resultPtr, "bad option \"",
			    string2, "\": must be -nocase", (char *) NULL);
		    return TCL_ERROR;
		}
	    }

	    /*
	     * This test is tricky, but has to be that way or you get
	     * other strange inconsistencies (see test string-10.20
	     * for illustration why!)
	     */
	    if (objv[objc-2]->typePtr == &tclDictType &&
		    objv[objc-2]->bytes == NULL) {
		int i, done;
		Tcl_DictSearch search;

		/*
		 * We know the type exactly, so all dict operations
		 * will succeed for sure.  This shortens this code
		 * quite a bit.
		 */
		Tcl_DictObjSize(interp, objv[objc-2], &mapElemc);
		if (mapElemc == 0) {
		    /*
		     * empty charMap, just return whatever string was given
		     */
		    Tcl_SetObjResult(interp, objv[objc-1]);
		    return TCL_OK;
		}
		mapElemc *= 2;
		mapWithDict = 1;
		/*
		 * Copy the dictionary out into an array; that's the
		 * easiest way to adapt this code...
		 */
		mapElemv = (Tcl_Obj **) ckalloc(sizeof(Tcl_Obj *) * mapElemc);
		Tcl_DictObjFirst(interp, objv[objc-2], &search,
			mapElemv+0, mapElemv+1, &done);
		for (i=2 ; i<mapElemc ; i+=2) {
		    Tcl_DictObjNext(&search, mapElemv+i, mapElemv+i+1, &done);
		}
	    } else {
		if (Tcl_ListObjGetElements(interp, objv[objc-2],
			&mapElemc, &mapElemv) != TCL_OK) {
		    return TCL_ERROR;
		}
		if (mapElemc == 0) {
		    /*
		     * empty charMap, just return whatever string was given
		     */
		    Tcl_SetObjResult(interp, objv[objc-1]);
		    return TCL_OK;
		} else if (mapElemc & 1) {
		    /*
		     * The charMap must be an even number of key/value items
		     */
		    Tcl_SetStringObj(resultPtr, "char map list unbalanced", -1);
		    return TCL_ERROR;
		}
	    }
	    objc--;

	    ustring1 = Tcl_GetUnicodeFromObj(objv[objc], &length1);
	    if (length1 == 0) {
		/*
		 * Empty input string, just stop now
		 */
		if (mapWithDict) {
		    ckfree((char *) mapElemv);
		}
		break;
	    }
	    end = ustring1 + length1;

	    strCmpFn = nocase ? Tcl_UniCharNcasecmp : Tcl_UniCharNcmp;

	    /*
	     * Force result to be Unicode
	     */
	    Tcl_SetUnicodeObj(resultPtr, ustring1, 0);

	    if (mapElemc == 2) {
		/*
		 * Special case for one map pair which avoids the extra
		 * for loop and extra calls to get Unicode data.  The
		 * algorithm is otherwise identical to the multi-pair case.
		 * This will be >30% faster on larger strings.
		 */
		int mapLen;
		Tcl_UniChar *mapString, u2lc;

		ustring2 = Tcl_GetUnicodeFromObj(mapElemv[0], &length2);
		p = ustring1;
		if (length2 == 0) {
		    ustring1 = end;
		} else {
		    mapString = Tcl_GetUnicodeFromObj(mapElemv[1], &mapLen);
		    u2lc = (nocase ? Tcl_UniCharToLower(*ustring2) : 0);
		    for (; ustring1 < end; ustring1++) {
			if (((*ustring1 == *ustring2) ||
				(nocase && (Tcl_UniCharToLower(*ustring1) ==
					u2lc))) &&
				((length2 == 1) || strCmpFn(ustring1, ustring2,
					(unsigned long) length2) == 0)) {
			    if (p != ustring1) {
				Tcl_AppendUnicodeToObj(resultPtr, p,
					ustring1 - p);
				p = ustring1 + length2;
			    } else {
				p += length2;
			    }
			    ustring1 = p - 1;

			    Tcl_AppendUnicodeToObj(resultPtr, mapString,
				    mapLen);
			}
		    }
		}
	    } else {
		Tcl_UniChar **mapStrings, *u2lc = NULL;
		int *mapLens;
		/*
		 * Precompute pointers to the unicode string and length.
		 * This saves us repeated function calls later,
		 * significantly speeding up the algorithm.  We only need
		 * the lowercase first char in the nocase case.
		 */
		mapStrings = (Tcl_UniChar **) ckalloc((mapElemc * 2)
			* sizeof(Tcl_UniChar *));
		mapLens = (int *) ckalloc((mapElemc * 2) * sizeof(int));
		if (nocase) {
		    u2lc = (Tcl_UniChar *)
			    ckalloc((mapElemc) * sizeof(Tcl_UniChar));
		}
		for (index = 0; index < mapElemc; index++) {
		    mapStrings[index] = Tcl_GetUnicodeFromObj(mapElemv[index],
			    &(mapLens[index]));
		    if (nocase && ((index % 2) == 0)) {
			u2lc[index/2] = Tcl_UniCharToLower(*mapStrings[index]);
		    }
		}
		for (p = ustring1; ustring1 < end; ustring1++) {
		    for (index = 0; index < mapElemc; index += 2) {
			/*
			 * Get the key string to match on.
			 */
			ustring2 = mapStrings[index];
			length2  = mapLens[index];
			if ((length2 > 0) && ((*ustring1 == *ustring2) ||
				(nocase && (Tcl_UniCharToLower(*ustring1) ==
					u2lc[index/2]))) &&
				((length2 == 1) || strCmpFn(ustring2, ustring1,
					(unsigned long) length2) == 0)) {
			    if (p != ustring1) {
				/*
				 * Put the skipped chars onto the result first
				 */
				Tcl_AppendUnicodeToObj(resultPtr, p,
					ustring1 - p);
				p = ustring1 + length2;
			    } else {
				p += length2;
			    }
			    /*
			     * Adjust len to be full length of matched string
			     */
			    ustring1 = p - 1;

			    /*
			     * Append the map value to the unicode string
			     */
			    Tcl_AppendUnicodeToObj(resultPtr,
				    mapStrings[index+1], mapLens[index+1]);
			    break;
			}
		    }
		}
		ckfree((char *) mapStrings);
		ckfree((char *) mapLens);
		if (nocase) {
		    ckfree((char *) u2lc);
		}
	    }
	    if (p != ustring1) {
		/*
		 * Put the rest of the unmapped chars onto result
		 */
		Tcl_AppendUnicodeToObj(resultPtr, p, ustring1 - p);
	    }
	    if (mapWithDict) {
		ckfree((char *) mapElemv);
	    }
	    break;
	}
	case STR_MATCH: {
	    Tcl_UniChar *ustring1, *ustring2;
	    int nocase = 0;

	    if (objc < 4 || objc > 5) {
	        Tcl_WrongNumArgs(interp, 2, objv, "?-nocase? pattern string");
		return TCL_ERROR;
	    }

	    if (objc == 5) {
		string2 = Tcl_GetStringFromObj(objv[2], &length2);
		if ((length2 > 1) &&
		    strncmp(string2, "-nocase", (size_t) length2) == 0) {
		    nocase = 1;
		} else {
		    Tcl_AppendStringsToObj(resultPtr, "bad option \"",
					   string2, "\": must be -nocase",
					   (char *) NULL);
		    return TCL_ERROR;
		}
	    }
	    ustring1 = Tcl_GetUnicodeFromObj(objv[objc-1], &length1);
	    ustring2 = Tcl_GetUnicodeFromObj(objv[objc-2], &length2);
	    Tcl_SetBooleanObj(resultPtr, TclUniCharMatch(ustring1, length1,
		    ustring2, length2, nocase));
	    break;
	}
	case STR_RANGE: {
	    int first, last;

	    if (objc != 5) {
	        Tcl_WrongNumArgs(interp, 2, objv, "string first last");
		return TCL_ERROR;
	    }

	    /*
	     * If we have a ByteArray object, avoid indexing in the
	     * Utf string since the byte array contains one byte per
	     * character.  Otherwise, use the Unicode string rep to
	     * get the range.
	     */

	    if (objv[2]->typePtr == &tclByteArrayType) {
		string1 = (char *)Tcl_GetByteArrayFromObj(objv[2], &length1);
		length1--;
	    } else {
		/*
		 * Get the length in actual characters.
		 */
		string1 = NULL;
		length1 = Tcl_GetCharLength(objv[2]) - 1;
	    }

	    if ((TclGetIntForIndex(interp, objv[3], length1, &first) != TCL_OK)
		    || (TclGetIntForIndex(interp, objv[4], length1,
			    &last) != TCL_OK)) {
		return TCL_ERROR;
	    }

	    if (first < 0) {
		first = 0;
	    }
	    if (last >= length1) {
		last = length1;
	    }
	    if (last >= first) {
		if (string1 != NULL) {
		    int numBytes = last - first + 1;
		    resultPtr = Tcl_NewByteArrayObj(
			(unsigned char *) &string1[first], numBytes);
		    Tcl_SetObjResult(interp, resultPtr);
		} else {
		    Tcl_SetObjResult(interp,
			    Tcl_GetRange(objv[2], first, last));
		}
	    }
	    break;
	}
	case STR_REPEAT: {
	    int count;

	    if (objc != 4) {
		Tcl_WrongNumArgs(interp, 2, objv, "string count");
		return TCL_ERROR;
	    }

	    if (Tcl_GetIntFromObj(interp, objv[3], &count) != TCL_OK) {
		return TCL_ERROR;
	    }

	    if (count == 1) {
		Tcl_SetObjResult(interp, objv[2]);
	    } else if (count > 1) {
		string1 = Tcl_GetStringFromObj(objv[2], &length1);
		if (length1 > 0) {
		    /*
		     * Only build up a string that has data.  Instead of
		     * building it up with repeated appends, we just allocate
		     * the necessary space once and copy the string value in.
		     * Check for overflow with back-division. [Bug #714106]
		     */
		    length2		= length1 * count;
		    if ((length2 / count) != length1) {
			char buf[TCL_INTEGER_SPACE+1];
			sprintf(buf, "%d", INT_MAX);
			Tcl_AppendStringsToObj(resultPtr,
				"string size overflow, must be less than ",
				buf, (char *) NULL);
			return TCL_ERROR;
		    }
		    /*
		     * Include space for the NULL
		     */
		    string2		= (char *) ckalloc((size_t) length2+1);
		    for (index = 0; index < count; index++) {
			memcpy(string2 + (length1 * index), string1,
				(size_t) length1);
		    }
		    string2[length2]	= '\0';
		    /*
		     * We have to directly assign this instead of using
		     * Tcl_SetStringObj (and indirectly TclInitStringRep)
		     * because that makes another copy of the data.
		     */
		    resultPtr		= Tcl_NewObj();
		    resultPtr->bytes	= string2;
		    resultPtr->length	= length2;
		    Tcl_SetObjResult(interp, resultPtr);
		}
	    }
	    break;
	}
	case STR_REPLACE: {
	    Tcl_UniChar *ustring1;
	    int first, last;

	    if (objc < 5 || objc > 6) {
	        Tcl_WrongNumArgs(interp, 2, objv,
				 "string first last ?string?");
		return TCL_ERROR;
	    }

	    ustring1 = Tcl_GetUnicodeFromObj(objv[2], &length1);
	    length1--;

	    if ((TclGetIntForIndex(interp, objv[3], length1, &first) != TCL_OK)
		    || (TclGetIntForIndex(interp, objv[4], length1,
			    &last) != TCL_OK)) {
		return TCL_ERROR;
	    }

	    if ((last < first) || (last < 0) || (first > length1)) {
		Tcl_SetObjResult(interp, objv[2]);
	    } else {
		if (first < 0) {
		    first = 0;
		}

		Tcl_SetUnicodeObj(resultPtr, ustring1, first);
		if (objc == 6) {
		    Tcl_AppendObjToObj(resultPtr, objv[5]);
		}
		if (last < length1) {
		    Tcl_AppendUnicodeToObj(resultPtr, ustring1 + last + 1,
			    length1 - last);
		}
	    }
	    break;
	}
	case STR_TOLOWER:
	case STR_TOUPPER:
	case STR_TOTITLE:
	    if (objc < 3 || objc > 5) {
	        Tcl_WrongNumArgs(interp, 2, objv, "string ?first? ?last?");
		return TCL_ERROR;
	    }

	    string1 = Tcl_GetStringFromObj(objv[2], &length1);

	    if (objc == 3) {
		/*
		 * Since the result object is not a shared object, it is
		 * safe to copy the string into the result and do the
		 * conversion in place.  The conversion may change the length
		 * of the string, so reset the length after conversion.
		 */

		Tcl_SetStringObj(resultPtr, string1, length1);
		if ((enum options) index == STR_TOLOWER) {
		    length1 = Tcl_UtfToLower(Tcl_GetString(resultPtr));
		} else if ((enum options) index == STR_TOUPPER) {
		    length1 = Tcl_UtfToUpper(Tcl_GetString(resultPtr));
		} else {
		    length1 = Tcl_UtfToTitle(Tcl_GetString(resultPtr));
		}
		Tcl_SetObjLength(resultPtr, length1);
	    } else {
		int first, last;
		CONST char *start, *end;

		length1 = Tcl_NumUtfChars(string1, length1) - 1;
		if (TclGetIntForIndex(interp, objv[3], length1,
				      &first) != TCL_OK) {
		    return TCL_ERROR;
		}
		if (first < 0) {
		    first = 0;
		}
		last = first;
		if ((objc == 5) && (TclGetIntForIndex(interp, objv[4], length1,
						      &last) != TCL_OK)) {
		    return TCL_ERROR;
		}
		if (last >= length1) {
		    last = length1;
		}
		if (last < first) {
		    Tcl_SetObjResult(interp, objv[2]);
		    break;
		}
		start = Tcl_UtfAtIndex(string1, first);
		end = Tcl_UtfAtIndex(start, last - first + 1);
		length2 = end-start;
		string2 = ckalloc((size_t) length2+1);
		memcpy(string2, start, (size_t) length2);
		string2[length2] = '\0';
		if ((enum options) index == STR_TOLOWER) {
		    length2 = Tcl_UtfToLower(string2);
		} else if ((enum options) index == STR_TOUPPER) {
		    length2 = Tcl_UtfToUpper(string2);
		} else {
		    length2 = Tcl_UtfToTitle(string2);
		}
		Tcl_SetStringObj(resultPtr, string1, start - string1);
		Tcl_AppendToObj(resultPtr, string2, length2);
		Tcl_AppendToObj(resultPtr, end, -1);
		ckfree(string2);
	    }
	    break;

	case STR_TRIM: {
	    Tcl_UniChar ch, trim;
	    register CONST char *p, *end;
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
		    offset = TclUtfToUniChar(p, &ch);
		    
		    for (check = string2; ; ) {
			if (check >= checkEnd) {
			    p = end;
			    break;
			}
			check += TclUtfToUniChar(check, &trim);
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
		    offset = TclUtfToUniChar(p, &ch);
		    for (check = string2; ; ) {
		        if (check >= checkEnd) {
			    p = end;
			    break;
			}
			check += TclUtfToUniChar(check, &trim);
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
	    CONST char *p, *end;
	    int numChars;
	    
	    if (objc != 4) {
	        Tcl_WrongNumArgs(interp, 2, objv, "string index");
		return TCL_ERROR;
	    }

	    string1 = Tcl_GetStringFromObj(objv[2], &length1);
	    numChars = Tcl_NumUtfChars(string1, length1);
	    if (TclGetIntForIndex(interp, objv[3], numChars-1,
				  &index) != TCL_OK) {
		return TCL_ERROR;
	    }
	    if (index < 0) {
		index = 0;
	    }
	    if (index < numChars) {
		p = Tcl_UtfAtIndex(string1, index);
		end = string1+length1;
		for (cur = index; p < end; cur++) {
		    p += TclUtfToUniChar(p, &ch);
		    if (!Tcl_UniCharIsWordChar(ch)) {
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
	    CONST char *p;
	    int numChars;
	    
	    if (objc != 4) {
	        Tcl_WrongNumArgs(interp, 2, objv, "string index");
		return TCL_ERROR;
	    }

	    string1 = Tcl_GetStringFromObj(objv[2], &length1);
	    numChars = Tcl_NumUtfChars(string1, length1);
	    if (TclGetIntForIndex(interp, objv[3], numChars-1,
				  &index) != TCL_OK) {
		return TCL_ERROR;
	    }
	    if (index >= numChars) {
		index = numChars - 1;
	    }
	    cur = 0;
	    if (index > 0) {
		p = Tcl_UtfAtIndex(string1, index);
	        for (cur = index; cur >= 0; cur--) {
		    TclUtfToUniChar(p, &ch);
		    if (!Tcl_UniCharIsWordChar(ch)) {
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
 *	command relies on Tcl_SubstObj() for its implementation.
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
    static CONST char *substOptions[] = {
	"-nobackslashes", "-nocommands", "-novariables", (char *) NULL
    };
    enum substOptions {
	SUBST_NOBACKSLASHES,      SUBST_NOCOMMANDS,       SUBST_NOVARS
    };
    Tcl_Obj *resultPtr;
    int optionIndex, flags, i;

    /*
     * Parse command-line options.
     */

    flags = TCL_SUBST_ALL;
    for (i = 1; i < (objc-1); i++) {
	if (Tcl_GetIndexFromObj(interp, objv[i], substOptions,
		"switch", 0, &optionIndex) != TCL_OK) {

	    return TCL_ERROR;
	}
	switch (optionIndex) {
	    case SUBST_NOBACKSLASHES: {
		flags &= ~TCL_SUBST_BACKSLASHES;
		break;
	    }
	    case SUBST_NOCOMMANDS: {
		flags &= ~TCL_SUBST_COMMANDS;
		break;
	    }
	    case SUBST_NOVARS: {
		flags &= ~TCL_SUBST_VARIABLES;
		break;
	    }
	    default: {
		Tcl_Panic("Tcl_SubstObjCmd: bad option index to SubstOptions");
	    }
	}
    }
    if (i != (objc-1)) {
	Tcl_WrongNumArgs(interp, 1, objv,
		"?-nobackslashes? ?-nocommands? ?-novariables? string");
	return TCL_ERROR;
    }

    /*
     * Perform the substitution.
     */
    resultPtr = Tcl_SubstObj(interp, objv[i], flags);

    if (resultPtr == NULL) {
	return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, resultPtr);
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
    int i, j, index, mode, matched, result, splitObjs, numMatchesSaved;
    char *string, *pattern;
    Tcl_Obj *stringObj, *indexVarObj, *matchVarObj;
    Tcl_Obj *CONST *savedObjv = objv;
    Tcl_RegExp regExpr = NULL;
    static CONST char *options[] = {
	"-exact", "-glob", "-indexvar", "-matchvar", "-regexp", "--", 
	NULL
    };
    enum options {
	OPT_EXACT, OPT_GLOB, OPT_INDEXV, OPT_MATCHV, OPT_REGEXP, OPT_LAST
    };

    mode = OPT_EXACT;
    indexVarObj = NULL;
    matchVarObj = NULL;
    numMatchesSaved = 0;
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

	/*
	 * Check for TIP#75 options specifying the variables to write
	 * regexp information into.
	 */

	if (index == OPT_INDEXV) {
	    i++;
	    if (i == objc) {
		Tcl_AppendResult(interp,
			"missing variable name argument to -indexvar option",
			(char *) NULL);
		return TCL_ERROR;
	    }
	    indexVarObj = objv[i];
	    numMatchesSaved = -1;
	} else if (index == OPT_MATCHV) {
	    i++;
	    if (i == objc) {
		Tcl_AppendResult(interp,
			"missing variable name argument to -matchvar option",
			(char *) NULL);
		return TCL_ERROR;
	    }
	    matchVarObj = objv[i];
	    numMatchesSaved = -1;
	} else {
	    mode = index;
	}
    }

    if (objc - i < 2) {
	Tcl_WrongNumArgs(interp, 1, objv,
		"?switches? string pattern body ... ?default body?");
	return TCL_ERROR;
    }
    if (indexVarObj != NULL && mode != OPT_REGEXP) {
	Tcl_AppendResult(interp,
		"-indexvar option requires -regexp option", (char *) NULL);
	return TCL_ERROR;
    }
    if (matchVarObj != NULL && mode != OPT_REGEXP) {
	Tcl_AppendResult(interp,
		"-matchvar option requires -regexp option", (char *) NULL);
	return TCL_ERROR;
    }

    stringObj = objv[i];
    objc -= i + 1;
    objv += i + 1;

    /*
     * If all of the pattern/command pairs are lumped into a single
     * argument, split them out again.
     */

    splitObjs = 0;
    if (objc == 1) {
	Tcl_Obj **listv;

	if (Tcl_ListObjGetElements(interp, objv[0], &objc, &listv) != TCL_OK) {
	    return TCL_ERROR;
	}

	/*
	 * Ensure that the list is non-empty.
	 */

	if (objc < 1) {
	    Tcl_WrongNumArgs(interp, 1, savedObjv,
		    "?switches? string {pattern body ... ?default body?}");
	    return TCL_ERROR;
	}
	objv = listv;
	splitObjs = 1;
    }

    /*
     * Complain if there is an odd number of words in the list of
     * patterns and bodies.
     */

    if (objc % 2) {
	Tcl_ResetResult(interp);
	Tcl_AppendResult(interp, "extra switch pattern with no body", NULL);

	/*
	 * Check if this can be due to a badly placed comment
	 * in the switch block.
	 *
	 * The following is an heuristic to detect the infamous
	 * "comment in switch" error: just check if a pattern
	 * begins with '#'.
	 */

	if (splitObjs) {
	    for (i=0 ; i<objc ; i+=2) {
		if (Tcl_GetString(objv[i])[0] == '#') {
		    Tcl_AppendResult(interp, ", this may be due to a ",
			    "comment incorrectly placed outside of a ",
			    "switch body - see the \"switch\" ",
			    "documentation", NULL);
		    break;
		}
	    }
	}

	return TCL_ERROR;
    }

    /*
     * Complain if the last body is a continuation.  Note that this
     * check assumes that the list is non-empty!
     */

    if (strcmp(Tcl_GetString(objv[objc-1]), "-") == 0) {
	Tcl_ResetResult(interp);
	Tcl_AppendResult(interp, "no body specified for pattern \"",
		Tcl_GetString(objv[objc-2]), "\"", NULL);
	return TCL_ERROR;
    }

    for (i = 0; i < objc; i += 2) {
	/*
	 * See if the pattern matches the string.
	 */

	pattern = Tcl_GetString(objv[i]);

	matched = 0;
	if ((i == objc - 2) 
		&& (*pattern == 'd') 
		&& (strcmp(pattern, "default") == 0)) {
	    Tcl_Obj *emptyObj = NULL;

	    matched = 1;
	    /*
	     * If either indexVarObj or matchVarObj are non-NULL,
	     * we're in REGEXP mode but have reached the default
	     * clause anyway.  TIP#75 specifies that we set the
	     * variables to empty lists (== empty objects) in that
	     * case.
	     */
	    if (indexVarObj != NULL) {
		TclNewObj(emptyObj);
		if (Tcl_ObjSetVar2(interp, indexVarObj, NULL, emptyObj,
			TCL_LEAVE_ERR_MSG) == NULL) {
		    Tcl_DecrRefCount(emptyObj);
		    return TCL_ERROR;
		}
	    }
	    if (matchVarObj != NULL) {
		if (emptyObj == NULL) {
		    TclNewObj(emptyObj);
		}
		if (Tcl_ObjSetVar2(interp, matchVarObj, NULL, emptyObj,
			TCL_LEAVE_ERR_MSG) == NULL) {
		    if (indexVarObj == NULL) {
			Tcl_DecrRefCount(emptyObj);
		    }
		    return TCL_ERROR;
		}
	    }
	    numMatchesSaved = 0;
	} else {
	    switch (mode) {
	    case OPT_EXACT:
		matched = (strcmp(Tcl_GetString(stringObj), pattern) == 0);
		break;
	    case OPT_GLOB:
		matched = Tcl_StringMatch(Tcl_GetString(stringObj), pattern);
		break;
	    case OPT_REGEXP:
		regExpr = Tcl_GetRegExpFromObj(interp, objv[i],
			TCL_REG_ADVANCED);
		if (regExpr == NULL) {
		    return TCL_ERROR;
		}
		matched = Tcl_RegExpExecObj(interp, regExpr, stringObj, 0,
			numMatchesSaved, 0);
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
	 * We are operating in REGEXP mode and we need to store
	 * information about what we matched in some user-nominated
	 * arrays.  So build the lists of values and indices to write
	 * here.  [TIP#75]
	 */

	if (numMatchesSaved) {
	    Tcl_RegExpInfo info;
	    Tcl_Obj *matchesObj, *indicesObj = NULL;

	    Tcl_RegExpGetInfo(regExpr, &info);
	    if (matchVarObj != NULL) {
		TclNewObj(matchesObj);
	    } else {
		matchesObj = NULL;
	    }
	    if (indexVarObj != NULL) {
		TclNewObj(indicesObj);
	    }
	    for (j=0 ; j<=info.nsubs ; j++) {
		if (indexVarObj != NULL) {
		    Tcl_Obj *rangeObjAry[2];

		    rangeObjAry[0] = Tcl_NewLongObj(info.matches[j].start);
		    rangeObjAry[1] = Tcl_NewLongObj(info.matches[j].end);
		    /*
		     * Never fails; the object is always clean at this point.
		     */
		    Tcl_ListObjAppendElement(NULL, indicesObj,
			    Tcl_NewListObj(2, rangeObjAry));
		}
		if (matchVarObj != NULL) {
		    Tcl_Obj *substringObj;

		    substringObj = Tcl_GetRange(stringObj,
			    info.matches[j].start, info.matches[j].end-1);
		    /*
		     * Never fails; the object is always clean at this point.
		     */
		    Tcl_ListObjAppendElement(NULL, matchesObj, substringObj);
		}
	    }
	    if (indexVarObj != NULL) {
		if (Tcl_ObjSetVar2(interp, indexVarObj, NULL, indicesObj,
			TCL_LEAVE_ERR_MSG) == NULL) {
		    Tcl_DecrRefCount(indicesObj);
		    /*
		     * Careful!  Check to see if we have allocated the
		     * list of matched strings; if so (but there was
		     * an error assigning the indices list) we have a
		     * potential memory leak because the match list
		     * has not been written to a variable.  Except
		     * that we'll clean that up right now.
		     */
		    if (matchesObj != NULL) {
			Tcl_DecrRefCount(matchesObj);
		    }
		    return TCL_ERROR;
		}
	    }
	    if (matchVarObj != NULL) {
		if (Tcl_ObjSetVar2(interp, matchVarObj, NULL, matchesObj,
			TCL_LEAVE_ERR_MSG) == NULL) {
		    Tcl_DecrRefCount(matchesObj);
		    /*
		     * Unlike above, if indicesObj is non-NULL at this
		     * point, it will have been written to a variable
		     * already and will hence not be leaked.
		     */
		    return TCL_ERROR;
		}
	    }
	}

	/*
	 * We've got a match. Find a body to execute, skipping bodies
	 * that are "-".
	 */

	for (j = i + 1; ; j += 2) {
	    if (j >= objc) {
		/*
		 * This shouldn't happen since we've checked that the
		 * last body is not a continuation...
		 */
		Tcl_Panic("fall-out when searching for body to match pattern");
	    }
	    if (strcmp(Tcl_GetString(objv[j]), "-") != 0) {
		break;
	    }
	}
	result = Tcl_EvalObjEx(interp, objv[j], 0);
	if (result == TCL_ERROR) {
	    Tcl_Obj *msg = Tcl_NewStringObj("\n    (\"", -1);
	    Tcl_Obj *errorLine = Tcl_NewIntObj(interp->errorLine);
	    Tcl_IncrRefCount(msg);
	    Tcl_IncrRefCount(errorLine);
	    TclAppendLimitedToObj(msg, pattern, -1, 50, "");
	    Tcl_AppendToObj(msg,"\" arm line ", -1);
	    Tcl_AppendObjToObj(msg, errorLine);
	    Tcl_DecrRefCount(errorLine);
	    Tcl_AppendToObj(msg,")", -1);
	    TclAppendObjToErrorInfo(interp, msg);
	    Tcl_DecrRefCount(msg);
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
    Tcl_GetTime(&start);
    while (i-- > 0) {
	result = Tcl_EvalObjEx(interp, objPtr, 0);
	if (result != TCL_OK) {
	    return result;
	}
    }
    Tcl_GetTime(&stop);
    
    totalMicroSec = ( ( (double) ( stop.sec - start.sec ) ) * 1.0e6
		      + ( stop.usec - start.usec ) );
    sprintf(buf, "%.0f microseconds per iteration",
	((count <= 0) ? 0 : totalMicroSec/count));
    Tcl_ResetResult(interp);
    Tcl_AppendToObj(Tcl_GetObjResult(interp), buf, -1);
    return TCL_OK;
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
