/* 
 * tclResult.c --
 *
 *	This file contains code to manage the interpreter result.
 *
 * Copyright (c) 1997 by Sun Microsystems, Inc.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id$
 */

#include "tclInt.h"

/*
 * Function prototypes for local procedures in this file:
 */

static void             ResetObjResult _ANSI_ARGS_((Interp *iPtr));
static void		SetupAppendBuffer _ANSI_ARGS_((Interp *iPtr,
			    int newSpace));

/*
 *  This structure is used to take a snapshot of the interpreter
 *  state in TclSaveInterpState.  You can snapshot the state,
 *  execute a command, and then back up to the result or the
 *  error that was previously in progress.
 */
typedef struct InterpState {
    int status;			/* return code status */
    int flags;			/* Each remaining field saves */
    int returnLevel;		/* the corresponding field of */
    int returnCode;		/* the Interp struct.  These */
    Tcl_Obj *errorInfo;		/* fields take together are the */
    Tcl_Obj *errorCode;		/* "state" of the interp. */
    Tcl_Obj *returnOpts;
    Tcl_Obj *objResult;
} InterpState;


/*
 *----------------------------------------------------------------------
 *
 * TclSaveInterpState --
 *
 *      Fills a token with a snapshot of the current state of the
 *      interpreter.  The snapshot can be restored at any point by
 *      TclRestoreInterpState. 
 *
 *      The token returned must be eventally passed to one of the
 *      routines TclRestoreInterpState or TclDiscardInterpState,
 *      or there will be a memory leak.
 *
 * Results:
 *	Returns a token representing the interp state.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

TclInterpState
TclSaveInterpState(interp, status)
    Tcl_Interp* interp;     /* Interpreter's state to be saved */
    int status;             /* status code for current operation */
{
    Interp *iPtr = (Interp *)interp;
    InterpState *statePtr = (InterpState *)ckalloc(sizeof(InterpState));

    statePtr->status = status;
    statePtr->flags = iPtr->flags & ERR_ALREADY_LOGGED;
    statePtr->returnLevel = iPtr->returnLevel;
    statePtr->returnCode = iPtr->returnCode;
    statePtr->errorInfo = iPtr->errorInfo;
    if (statePtr->errorInfo) {
	Tcl_IncrRefCount(statePtr->errorInfo);
    }
    statePtr->errorCode = iPtr->errorCode;
    if (statePtr->errorCode) {
	Tcl_IncrRefCount(statePtr->errorCode);
    }
    statePtr->returnOpts = iPtr->returnOpts;
    if (statePtr->returnOpts) {
	Tcl_IncrRefCount(statePtr->returnOpts);
    }
    statePtr->objResult = Tcl_GetObjResult(interp);
    Tcl_IncrRefCount(statePtr->objResult);
    return (TclInterpState) statePtr;
}

/*
 *----------------------------------------------------------------------
 *
 * TclRestoreInterpState --
 *
 *      Accepts an interp and a token previously returned by
 *      TclSaveInterpState.  Restore the state of the interp
 *      to what it was at the time of the TclSaveInterpState call.
 *
 * Results:
 *	Returns the status value originally passed in to TclSaveInterpState.
 *
 * Side effects:
 *	Restores the interp state and frees memory held by token.
 *
 *----------------------------------------------------------------------
 */

int
TclRestoreInterpState(interp, state)
    Tcl_Interp* interp;		/* Interpreter's state to be restored*/
    TclInterpState state;	/* saved interpreter state */
{
    Interp *iPtr = (Interp *)interp;
    InterpState *statePtr = (InterpState *)state;
    int status = statePtr->status;

    iPtr->flags &= ~ERR_ALREADY_LOGGED;
    iPtr->flags |= (statePtr->flags & ERR_ALREADY_LOGGED);

    iPtr->returnLevel = statePtr->returnLevel;
    iPtr->returnCode = statePtr->returnCode;
    iPtr->errorInfo = statePtr->errorInfo;
    if (iPtr->errorInfo) {
	Tcl_IncrRefCount(iPtr->errorInfo);
    }
    iPtr->errorCode = statePtr->errorCode;
    if (iPtr->errorCode) {
	Tcl_IncrRefCount(iPtr->errorCode);
    }
    iPtr->returnOpts = statePtr->returnOpts;
    if (iPtr->returnOpts) {
	Tcl_IncrRefCount(iPtr->returnOpts);
    }
    Tcl_SetObjResult(interp, statePtr->objResult);
    TclDiscardInterpState(state);
    return status;
}

/*
 *----------------------------------------------------------------------
 *
 * TclDiscardInterpState --
 *
 *      Accepts a token previously returned by TclSaveInterpState.
 *      Frees the memory it uses.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Frees memory.
 *
 *----------------------------------------------------------------------
 */

void
TclDiscardInterpState(state)
    TclInterpState state;	/* saved interpreter state */
{
    InterpState *statePtr = (InterpState *)state;

    if (statePtr->errorInfo) {
        Tcl_DecrRefCount(statePtr->errorInfo);
    }
    if (statePtr->errorCode) {
        Tcl_DecrRefCount(statePtr->errorCode);
    }
    if (statePtr->returnOpts) {
        Tcl_DecrRefCount(statePtr->returnOpts);
    }
    Tcl_DecrRefCount(statePtr->objResult);
    ckfree((char*) statePtr);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SaveResult --
 *
 *      Takes a snapshot of the current result state of the interpreter.
 *      The snapshot can be restored at any point by
 *      Tcl_RestoreResult. Note that this routine does not 
 *	preserve the errorCode, errorInfo, or flags fields so it
 *	should not be used if an error is in progress.
 *
 *      Once a snapshot is saved, it must be restored by calling
 *      Tcl_RestoreResult, or discarded by calling
 *      Tcl_DiscardResult.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Resets the interpreter result.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_SaveResult(interp, statePtr)
    Tcl_Interp *interp;		/* Interpreter to save. */
    Tcl_SavedResult *statePtr;	/* Pointer to state structure. */
{
    Interp *iPtr = (Interp *) interp;

    /*
     * Move the result object into the save state.  Note that we don't need
     * to change its refcount because we're moving it, not adding a new
     * reference.  Put an empty object into the interpreter.
     */

    statePtr->objResultPtr = iPtr->objResultPtr;
    iPtr->objResultPtr = Tcl_NewObj(); 
    Tcl_IncrRefCount(iPtr->objResultPtr); 

    /*
     * Save the string result. 
     */

    statePtr->freeProc = iPtr->freeProc;
    if (iPtr->result == iPtr->resultSpace) {
	/*
	 * Copy the static string data out of the interp buffer.
	 */

	statePtr->result = statePtr->resultSpace;
	strcpy(statePtr->result, iPtr->result);
	statePtr->appendResult = NULL;
    } else if (iPtr->result == iPtr->appendResult) {
	/*
	 * Move the append buffer out of the interp.
	 */

	statePtr->appendResult = iPtr->appendResult;
	statePtr->appendAvl = iPtr->appendAvl;
	statePtr->appendUsed = iPtr->appendUsed;
	statePtr->result = statePtr->appendResult;
	iPtr->appendResult = NULL;
	iPtr->appendAvl = 0;
	iPtr->appendUsed = 0;
    } else {
	/*
	 * Move the dynamic or static string out of the interpreter.
	 */

	statePtr->result = iPtr->result;
	statePtr->appendResult = NULL;
    }

    iPtr->result = iPtr->resultSpace;
    iPtr->resultSpace[0] = 0;
    iPtr->freeProc = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_RestoreResult --
 *
 *      Restores the state of the interpreter to a snapshot taken
 *      by Tcl_SaveResult.  After this call, the token for
 *      the interpreter state is no longer valid.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Restores the interpreter result.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_RestoreResult(interp, statePtr)
    Tcl_Interp* interp;		/* Interpreter being restored. */
    Tcl_SavedResult *statePtr;	/* State returned by Tcl_SaveResult. */
{
    Interp *iPtr = (Interp *) interp;

    Tcl_ResetResult(interp);

    /*
     * Restore the string result.
     */

    iPtr->freeProc = statePtr->freeProc;
    if (statePtr->result == statePtr->resultSpace) {
	/*
	 * Copy the static string data into the interp buffer.
	 */

	iPtr->result = iPtr->resultSpace;
	strcpy(iPtr->result, statePtr->result);
    } else if (statePtr->result == statePtr->appendResult) {
	/*
	 * Move the append buffer back into the interp.
	 */

	if (iPtr->appendResult != NULL) {
	    ckfree((char *)iPtr->appendResult);
	}

	iPtr->appendResult = statePtr->appendResult;
	iPtr->appendAvl = statePtr->appendAvl;
	iPtr->appendUsed = statePtr->appendUsed;
	iPtr->result = iPtr->appendResult;
    } else {
	/*
	 * Move the dynamic or static string back into the interpreter.
	 */

	iPtr->result = statePtr->result;
    }

    /*
     * Restore the object result.
     */

    Tcl_DecrRefCount(iPtr->objResultPtr);
    iPtr->objResultPtr = statePtr->objResultPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_DiscardResult --
 *
 *      Frees the memory associated with an interpreter snapshot
 *      taken by Tcl_SaveResult.  If the snapshot is not
 *      restored, this procedure must be called to discard it,
 *      or the memory will be lost.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_DiscardResult(statePtr)
    Tcl_SavedResult *statePtr;	/* State returned by Tcl_SaveResult. */
{
    TclDecrRefCount(statePtr->objResultPtr);

    if (statePtr->result == statePtr->appendResult) {
	ckfree(statePtr->appendResult);
    } else if (statePtr->freeProc) {
	if (statePtr->freeProc == TCL_DYNAMIC) {
	    ckfree(statePtr->result);
	} else {
	    (*statePtr->freeProc)(statePtr->result);
	}
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetResult --
 *
 *	Arrange for "string" to be the Tcl return value.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	interp->result is left pointing either to "string" (if "copy" is 0)
 *	or to a copy of string. Also, the object result is reset.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_SetResult(interp, stringPtr, freeProc)
    Tcl_Interp *interp;		/* Interpreter with which to associate the
				 * return value. */
    register char *stringPtr;	/* Value to be returned.  If NULL, the
				 * result is set to an empty string. */
    Tcl_FreeProc *freeProc;	/* Gives information about the string:
				 * TCL_STATIC, TCL_VOLATILE, or the address
				 * of a Tcl_FreeProc such as free. */
{
    Interp *iPtr = (Interp *) interp;
    int length;
    register Tcl_FreeProc *oldFreeProc = iPtr->freeProc;
    char *oldResult = iPtr->result;

    if (stringPtr == NULL) {
	iPtr->resultSpace[0] = 0;
	iPtr->result = iPtr->resultSpace;
	iPtr->freeProc = 0;
    } else if (freeProc == TCL_VOLATILE) {
	length = strlen(stringPtr);
	if (length > TCL_RESULT_SIZE) {
	    iPtr->result = (char *) ckalloc((unsigned) length+1);
	    iPtr->freeProc = TCL_DYNAMIC;
	} else {
	    iPtr->result = iPtr->resultSpace;
	    iPtr->freeProc = 0;
	}
	strcpy(iPtr->result, stringPtr);
    } else {
	iPtr->result = stringPtr;
	iPtr->freeProc = freeProc;
    }

    /*
     * If the old result was dynamically-allocated, free it up.  Do it
     * here, rather than at the beginning, in case the new result value
     * was part of the old result value.
     */

    if (oldFreeProc != 0) {
	if (oldFreeProc == TCL_DYNAMIC) {
	    ckfree(oldResult);
	} else {
	    (*oldFreeProc)(oldResult);
	}
    }

    /*
     * Reset the object result since we just set the string result.
     */

    ResetObjResult(iPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetStringResult --
 *
 *	Returns an interpreter's result value as a string.
 *
 * Results:
 *	The interpreter's result as a string.
 *
 * Side effects:
 *	If the string result is empty, the object result is moved to the
 *	string result, then the object result is reset.
 *
 *----------------------------------------------------------------------
 */

CONST char *
Tcl_GetStringResult(interp)
     register Tcl_Interp *interp; /* Interpreter whose result to return. */
{
    /*
     * If the string result is empty, move the object result to the
     * string result, then reset the object result.
     */
    
    if (*(interp->result) == 0) {
	Tcl_SetResult(interp, TclGetString(Tcl_GetObjResult(interp)),
	        TCL_VOLATILE);
    }
    return interp->result;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetObjResult --
 *
 *	Arrange for objPtr to be an interpreter's result value.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	interp->objResultPtr is left pointing to the object referenced
 *	by objPtr. The object's reference count is incremented since
 *	there is now a new reference to it. The reference count for any
 *	old objResultPtr value is decremented. Also, the string result
 *	is reset.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_SetObjResult(interp, objPtr)
    Tcl_Interp *interp;		/* Interpreter with which to associate the
				 * return object value. */
    register Tcl_Obj *objPtr;	/* Tcl object to be returned. If NULL, the
				 * obj result is made an empty string
				 * object. */
{
    register Interp *iPtr = (Interp *) interp;
    register Tcl_Obj *oldObjResult = iPtr->objResultPtr;

    iPtr->objResultPtr = objPtr;
    Tcl_IncrRefCount(objPtr);	/* since interp result is a reference */

    /*
     * We wait until the end to release the old object result, in case
     * we are setting the result to itself.
     */
    
    TclDecrRefCount(oldObjResult);

    /*
     * Reset the string result since we just set the result object.
     */

    if (iPtr->freeProc != NULL) {
	if (iPtr->freeProc == TCL_DYNAMIC) {
	    ckfree(iPtr->result);
	} else {
	    (*iPtr->freeProc)(iPtr->result);
	}
	iPtr->freeProc = 0;
    }
    iPtr->result = iPtr->resultSpace;
    iPtr->resultSpace[0] = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetObjResult --
 *
 *	Returns an interpreter's result value as a Tcl object. The object's
 *	reference count is not modified; the caller must do that if it
 *	needs to hold on to a long-term reference to it.
 *
 * Results:
 *	The interpreter's result as an object.
 *
 * Side effects:
 *	If the interpreter has a non-empty string result, the result object
 *	is either empty or stale because some procedure set interp->result
 *	directly. If so, the string result is moved to the result object
 *	then the string result is reset.
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj *
Tcl_GetObjResult(interp)
    Tcl_Interp *interp;		/* Interpreter whose result to return. */
{
    register Interp *iPtr = (Interp *) interp;
    Tcl_Obj *objResultPtr;
    int length;

    /*
     * If the string result is non-empty, move the string result to the
     * object result, then reset the string result.
     */
    
    if (*(iPtr->result) != 0) {
	ResetObjResult(iPtr);
	
	objResultPtr = iPtr->objResultPtr;
	length = strlen(iPtr->result);
	TclInitStringRep(objResultPtr, iPtr->result, length);
	
	if (iPtr->freeProc != NULL) {
	    if (iPtr->freeProc == TCL_DYNAMIC) {
		ckfree(iPtr->result);
	    } else {
		(*iPtr->freeProc)(iPtr->result);
	    }
	    iPtr->freeProc = 0;
	}
	iPtr->result = iPtr->resultSpace;
	iPtr->resultSpace[0] = 0;
    }
    return iPtr->objResultPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_AppendResultVA --
 *
 *	Append a variable number of strings onto the interpreter's
 *	result.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The result of the interpreter given by the first argument is
 *	extended by the strings in the va_list (up to a terminating
 *	NULL argument).
 *
 *	If the string result is non-empty, the object result forced to
 *	be a duplicate of it first. There will be a string result
 *	afterwards.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_AppendResultVA(interp, argList)
    Tcl_Interp *interp;		/* Interpreter with which to associate the
				 * return value. */
    va_list argList;		/* Variable argument list. */
{
    Tcl_Obj *objPtr = Tcl_GetObjResult(interp);

    if (Tcl_IsShared(objPtr)) {
	objPtr = Tcl_DuplicateObj(objPtr);
    }
    Tcl_AppendStringsToObjVA(objPtr, argList);
    Tcl_SetObjResult(interp, objPtr);
    /*
     * Ensure that the interp->result is legal so old Tcl 7.* code
     * still works. There's still embarrasingly much of it about...
     */
    (void) Tcl_GetStringResult(interp);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_AppendResult --
 *
 *	Append a variable number of strings onto the interpreter's
 *	result.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The result of the interpreter given by the first argument is
 *	extended by the strings given by the second and following
 *	arguments (up to a terminating NULL argument).
 *
 *	If the string result is non-empty, the object result forced to
 *	be a duplicate of it first. There will be a string result
 *	afterwards.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_AppendResult TCL_VARARGS_DEF(Tcl_Interp *,arg1)
{
    Tcl_Interp *interp;
    va_list argList;

    interp = TCL_VARARGS_START(Tcl_Interp *,arg1,argList);
    Tcl_AppendResultVA(interp, argList);
    va_end(argList);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_AppendElement --
 *
 *	Convert a string to a valid Tcl list element and append it to the
 *	result (which is ostensibly a list).
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The result in the interpreter given by the first argument is
 *	extended with a list element converted from string. A separator
 *	space is added before the converted list element unless the current
 *	result is empty, contains the single character "{", or ends in " {".
 *
 *	If the string result is empty, the object result is moved to the
 *	string result, then the object result is reset.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_AppendElement(interp, stringPtr)
    Tcl_Interp *interp;		/* Interpreter whose result is to be
				 * extended. */
    CONST char *stringPtr;	/* String to convert to list element and
				 * add to result. */
{
    Interp *iPtr = (Interp *) interp;
    char *dst;
    int size;
    int flags;

    /*
     * If the string result is empty, move the object result to the
     * string result, then reset the object result.
     */

    (void) Tcl_GetStringResult(interp);

    /*
     * See how much space is needed, and grow the append buffer if
     * needed to accommodate the list element.
     */

    size = Tcl_ScanElement(stringPtr, &flags) + 1;
    if ((iPtr->result != iPtr->appendResult)
	    || (iPtr->appendResult[iPtr->appendUsed] != 0)
	    || ((size + iPtr->appendUsed) >= iPtr->appendAvl)) {
       SetupAppendBuffer(iPtr, size+iPtr->appendUsed);
    }

    /*
     * Convert the string into a list element and copy it to the
     * buffer that's forming, with a space separator if needed.
     */

    dst = iPtr->appendResult + iPtr->appendUsed;
    if (TclNeedSpace(iPtr->appendResult, dst)) {
	iPtr->appendUsed++;
	*dst = ' ';
	dst++;
	/*
	 * If we need a space to separate this element from preceding
	 * stuff, then this element will not lead a list, and need not
	 * have it's leading '#' quoted.
	 */
	flags |= TCL_DONT_QUOTE_HASH;
    }
    iPtr->appendUsed += Tcl_ConvertElement(stringPtr, dst, flags);
}

/*
 *----------------------------------------------------------------------
 *
 * SetupAppendBuffer --
 *
 *	This procedure makes sure that there is an append buffer properly
 *	initialized, if necessary, from the interpreter's result, and
 *	that it has at least enough room to accommodate newSpace new
 *	bytes of information.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
SetupAppendBuffer(iPtr, newSpace)
    Interp *iPtr;		/* Interpreter whose result is being set up. */
    int newSpace;		/* Make sure that at least this many bytes
				 * of new information may be added. */
{
    int totalSpace;

    /*
     * Make the append buffer larger, if that's necessary, then copy the
     * result into the append buffer and make the append buffer the official
     * Tcl result.
     */

    if (iPtr->result != iPtr->appendResult) {
	/*
	 * If an oversized buffer was used recently, then free it up
	 * so we go back to a smaller buffer.  This avoids tying up
	 * memory forever after a large operation.
	 */

	if (iPtr->appendAvl > 500) {
	    ckfree(iPtr->appendResult);
	    iPtr->appendResult = NULL;
	    iPtr->appendAvl = 0;
	}
	iPtr->appendUsed = strlen(iPtr->result);
    } else if (iPtr->result[iPtr->appendUsed] != 0) {
	/*
	 * Most likely someone has modified a result created by
	 * Tcl_AppendResult et al. so that it has a different size.
	 * Just recompute the size.
	 */

	iPtr->appendUsed = strlen(iPtr->result);
    }
    
    totalSpace = newSpace + iPtr->appendUsed;
    if (totalSpace >= iPtr->appendAvl) {
	char *new;

	if (totalSpace < 100) {
	    totalSpace = 200;
	} else {
	    totalSpace *= 2;
	}
	new = (char *) ckalloc((unsigned) totalSpace);
	strcpy(new, iPtr->result);
	if (iPtr->appendResult != NULL) {
	    ckfree(iPtr->appendResult);
	}
	iPtr->appendResult = new;
	iPtr->appendAvl = totalSpace;
    } else if (iPtr->result != iPtr->appendResult) {
	strcpy(iPtr->appendResult, iPtr->result);
    }
    
    Tcl_FreeResult((Tcl_Interp *) iPtr);
    iPtr->result = iPtr->appendResult;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FreeResult --
 *
 *	This procedure frees up the memory associated with an interpreter's
 *	string result. It also resets the interpreter's result object.
 *	Tcl_FreeResult is most commonly used when a procedure is about to
 *	replace one result value with another.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Frees the memory associated with interp's string result and sets
 *	interp->freeProc to zero, but does not change interp->result or
 *	clear error state. Resets interp's result object to an unshared
 *	empty object.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_FreeResult(interp)
    register Tcl_Interp *interp; /* Interpreter for which to free result. */
{
    register Interp *iPtr = (Interp *) interp;
    
    if (iPtr->freeProc != NULL) {
	if (iPtr->freeProc == TCL_DYNAMIC) {
	    ckfree(iPtr->result);
	} else {
	    (*iPtr->freeProc)(iPtr->result);
	}
	iPtr->freeProc = 0;
    }
    
    ResetObjResult(iPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ResetResult --
 *
 *	This procedure resets both the interpreter's string and object
 *	results.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	It resets the result object to an unshared empty object. It
 *	then restores the interpreter's string result area to its default
 *	initialized state, freeing up any memory that may have been
 *	allocated. It also clears any error information for the interpreter.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_ResetResult(interp)
    register Tcl_Interp *interp; /* Interpreter for which to clear result. */
{
    register Interp *iPtr = (Interp *) interp;

    ResetObjResult(iPtr);
    if (iPtr->freeProc != NULL) {
	if (iPtr->freeProc == TCL_DYNAMIC) {
	    ckfree(iPtr->result);
	} else {
	    (*iPtr->freeProc)(iPtr->result);
	}
	iPtr->freeProc = 0;
    }
    iPtr->result = iPtr->resultSpace;
    iPtr->resultSpace[0] = 0;
    if (iPtr->errorCode) {
	/* Legacy support */
	Tcl_ObjSetVar2(interp, iPtr->ecVar, NULL,
		iPtr->errorCode, TCL_GLOBAL_ONLY);
	Tcl_DecrRefCount(iPtr->errorCode);
	iPtr->errorCode = NULL;
    }
    if (iPtr->errorInfo) {
	/* Legacy support */
	Tcl_ObjSetVar2(interp, iPtr->eiVar, NULL,
		iPtr->errorInfo, TCL_GLOBAL_ONLY);
	Tcl_DecrRefCount(iPtr->errorInfo);
	iPtr->errorInfo = NULL;
    }
    iPtr->flags &= ~ERR_ALREADY_LOGGED;
}

/*
 *----------------------------------------------------------------------
 *
 * ResetObjResult --
 *
 *	Procedure used to reset an interpreter's Tcl result object.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Resets the interpreter's result object to an unshared empty string
 *	object with ref count one. It does not clear any error information
 *	in the interpreter.
 *
 *----------------------------------------------------------------------
 */

static void
ResetObjResult(iPtr)
    register Interp *iPtr;	/* Points to the interpreter whose result
				 * object should be reset. */
{
    register Tcl_Obj *objResultPtr = iPtr->objResultPtr;

    if (Tcl_IsShared(objResultPtr)) {
	TclDecrRefCount(objResultPtr);
	TclNewObj(objResultPtr);
	Tcl_IncrRefCount(objResultPtr);
	iPtr->objResultPtr = objResultPtr;
    } else {
	if ((objResultPtr->bytes != NULL)
	        && (objResultPtr->bytes != tclEmptyStringRep)) {
	    ckfree((char *) objResultPtr->bytes);
	}
	objResultPtr->bytes  = tclEmptyStringRep;
	objResultPtr->length = 0;
	TclFreeIntRep(objResultPtr);
	objResultPtr->typePtr = (Tcl_ObjType *) NULL;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetErrorCodeVA --
 *
 *	This procedure is called to record machine-readable information
 *	about an error that is about to be returned.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The errorCode field of the interp is modified to hold all of the
 *	arguments to this procedure, in a list form with each argument
 *	becoming one element of the list.  
 *
 *----------------------------------------------------------------------
 */

void
Tcl_SetErrorCodeVA (interp, argList)
    Tcl_Interp *interp;		/* Interpreter in which to set errorCode */
    va_list argList;		/* Variable argument list. */
{
    Tcl_Obj *errorObj = Tcl_NewObj();

    /*
     * Scan through the arguments one at a time, appending them to
     * the errorCode field as list elements.
     */

    while (1) {
	char *elem = va_arg(argList, char *);
	if (elem == NULL) {
	    break;
	}
	Tcl_ListObjAppendElement(NULL, errorObj, Tcl_NewStringObj(elem, -1));
    }
    Tcl_SetObjErrorCode(interp, errorObj);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetErrorCode --
 *
 *	This procedure is called to record machine-readable information
 *	about an error that is about to be returned.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The errorCode field of the interp is modified to hold all of the
 *	arguments to this procedure, in a list form with each argument
 *	becoming one element of the list.  
 *
 *----------------------------------------------------------------------
 */
	/* VARARGS2 */
void
Tcl_SetErrorCode TCL_VARARGS_DEF(Tcl_Interp *,arg1)
{
    Tcl_Interp *interp;
    va_list argList;

    /*
     * Scan through the arguments one at a time, appending them to
     * the errorCode field as list elements.
     */

    interp = TCL_VARARGS_START(Tcl_Interp *,arg1,argList);
    Tcl_SetErrorCodeVA(interp, argList);
    va_end(argList);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetObjErrorCode --
 *
 *	This procedure is called to record machine-readable information
 *	about an error that is about to be returned. The caller should
 *	build a list object up and pass it to this routine.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The errorCode field of the interp is set to the new value.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_SetObjErrorCode(interp, errorObjPtr)
    Tcl_Interp *interp;
    Tcl_Obj *errorObjPtr;
{
    Interp *iPtr = (Interp *) interp;
    
    if (iPtr->errorCode) {
	Tcl_DecrRefCount(iPtr->errorCode);
    }
    iPtr->errorCode = errorObjPtr;
    Tcl_IncrRefCount(iPtr->errorCode);
}

/*
 *-------------------------------------------------------------------------
 *
 * TclTransferResult --
 *
 *	Copy the result (and error information) from one interp to 
 *	another.  Used when one interp has caused another interp to 
 *	evaluate a script and then wants to transfer the results back
 *	to itself.
 *
 *	This routine copies the string reps of the result and error 
 *	information.  It does not simply increment the refcounts of the
 *	result and error information objects themselves.
 *	It is not legal to exchange objects between interps, because an
 *	object may be kept alive by one interp, but have an internal rep 
 *	that is only valid while some other interp is alive.  
 *
 * Results:
 *	The target interp's result is set to a copy of the source interp's
 *	result.  The source's errorInfo field may be transferred to the
 *	target's errorInfo field, and the source's errorCode field may be
 *	transferred to the target's errorCode field.
 *
 * Side effects:
 *	None.
 *
 *-------------------------------------------------------------------------
 */
	
void
TclTransferResult(sourceInterp, result, targetInterp)
    Tcl_Interp *sourceInterp;	/* Interp whose result and error information
				 * should be moved to the target interp.  
				 * After moving result, this interp's result 
				 * is reset. */
    int result;			/* TCL_OK if just the result should be copied, 
				 * TCL_ERROR if both the result and error 
				 * information should be copied. */
    Tcl_Interp *targetInterp;	/* Interp where result and error information 
				 * should be stored.  If source and target
				 * are the same, nothing is done. */
{
    Interp *iPtr;

    if (sourceInterp == targetInterp) {
	return;
    }

    if (result == TCL_ERROR) {
	/*
	 * An error occurred, so transfer error information from the source
	 * interpreter to the target interpreter.  Setting the flags tells
	 * the target interp that it has inherited a partial traceback
	 * chain, not just a simple error message.
	 */

	iPtr = (Interp *) sourceInterp;
        if ((iPtr->flags & ERR_ALREADY_LOGGED) == 0) {
            Tcl_AddErrorInfo(sourceInterp, "");
        }
        iPtr->flags &= ~(ERR_ALREADY_LOGGED);
        
        Tcl_ResetResult(targetInterp);
        
	if (iPtr->errorInfo) {
	    ((Interp *) targetInterp)->errorInfo = iPtr->errorInfo;
	    Tcl_IncrRefCount(((Interp *) targetInterp)->errorInfo);
	}

	if (iPtr->errorCode) {
	    Tcl_SetObjErrorCode(targetInterp, iPtr->errorCode);
	}
    }

    /* This may need examination for safety */
    Tcl_DecrRefCount( ((Interp *) targetInterp)->returnOpts );
    ((Interp *) targetInterp)->returnOpts = 
	    ((Interp *) sourceInterp)->returnOpts;
    Tcl_IncrRefCount( ((Interp *) targetInterp)->returnOpts );

    Tcl_SetObjResult(targetInterp, Tcl_GetObjResult(sourceInterp));
    Tcl_ResetResult(sourceInterp);
}
