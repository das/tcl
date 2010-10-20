/*
 * tclCompCmds.c --
 *
 *	This file contains compilation procedures that compile various Tcl
 *	commands into a sequence of instructions ("bytecodes").
 *
 * Copyright (c) 1997-1998 Sun Microsystems, Inc.
 * Copyright (c) 2001 by Kevin B. Kenny.  All rights reserved.
 * Copyright (c) 2002 ActiveState Corporation.
 * Copyright (c) 2004-2006 by Donal K. Fellows.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id$
 */

#include "tclInt.h"
#include "tclCompile.h"

/*
 * Prototypes for procedures defined later in this file:
 */

static ClientData	DupDictUpdateInfo(ClientData clientData);
static void		FreeDictUpdateInfo(ClientData clientData);
static void		PrintDictUpdateInfo(ClientData clientData,
			    Tcl_Obj *appendObj, ByteCode *codePtr,
			    unsigned int pcOffset);
static ClientData	DupForeachInfo(ClientData clientData);
static void		FreeForeachInfo(ClientData clientData);
static void		PrintForeachInfo(ClientData clientData,
			    Tcl_Obj *appendObj, ByteCode *codePtr,
			    unsigned int pcOffset);
static void		CompileReturnInternal(CompileEnv *envPtr,
			    unsigned char op, int code, int level,
			    Tcl_Obj *returnOpts);
static int		IndexTailVarIfKnown(Tcl_Interp *interp,
			    Tcl_Token *varTokenPtr, CompileEnv *envPtr);
static int		PushVarName(Tcl_Interp *interp,
			    Tcl_Token *varTokenPtr, CompileEnv *envPtr,
			    int flags, int *localIndexPtr,
			    int *simpleVarNamePtr, int *isScalarPtr,
			    int line, int *clNext);

/*
 * Macro that encapsulates an efficiency trick that avoids a function call for
 * the simplest of compiles. The ANSI C "prototype" for this macro is:
 *
 * static void		CompileWord(CompileEnv *envPtr, Tcl_Token *tokenPtr,
 *			    Tcl_Interp *interp, int word);
 */

#define CompileWord(envPtr, tokenPtr, interp, word) \
    if ((tokenPtr)->type == TCL_TOKEN_SIMPLE_WORD) {			\
	TclEmitPush(TclRegisterNewLiteral((envPtr), (tokenPtr)[1].start, \
		(tokenPtr)[1].size), (envPtr));				\
    } else {								\
	envPtr->line = mapPtr->loc[eclIndex].line[word];		\
	envPtr->clNext = mapPtr->loc[eclIndex].next[word];		\
	TclCompileTokens((interp), (tokenPtr)+1, (tokenPtr)->numComponents, \
		(envPtr));						\
    }

/*
 * TIP #280: Remember the per-word line information of the current command. An
 * index is used instead of a pointer as recursive compilation may reallocate,
 * i.e. move, the array. This is also the reason to save the nuloc now, it may
 * change during the course of the function.
 *
 * Macro to encapsulate the variable definition and setup.
 */

#define DefineLineInformation \
    ExtCmdLoc *mapPtr = envPtr->extCmdMapPtr;				\
    int eclIndex = mapPtr->nuloc - 1

#define SetLineInformation(word) \
    envPtr->line = mapPtr->loc[eclIndex].line[(word)];			\
    envPtr->clNext = mapPtr->loc[eclIndex].next[(word)]

#define PushVarNameWord(i,v,e,f,l,s,sc,word) \
    PushVarName(i,v,e,f,l,s,sc,						\
	    mapPtr->loc[eclIndex].line[(word)],				\
	    mapPtr->loc[eclIndex].next[(word)])

/*
 * Flags bits used by PushVarName.
 */

#define TCL_NO_LARGE_INDEX 1	/* Do not return localIndex value > 255 */

/*
 * The structures below define the AuxData types defined in this file.
 */

const AuxDataType tclForeachInfoType = {
    "ForeachInfo",		/* name */
    DupForeachInfo,		/* dupProc */
    FreeForeachInfo,		/* freeProc */
    PrintForeachInfo		/* printProc */
};

const AuxDataType tclDictUpdateInfoType = {
    "DictUpdateInfo",		/* name */
    DupDictUpdateInfo,		/* dupProc */
    FreeDictUpdateInfo,		/* freeProc */
    PrintDictUpdateInfo		/* printProc */
};

/*
 *----------------------------------------------------------------------
 *
 * TclCompileAppendCmd --
 *
 *	Procedure called to compile the "append" command.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "append" command at
 *	runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileAppendCmd(
    Tcl_Interp *interp,		/* Used for error reporting. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    Tcl_Token *varTokenPtr, *valueTokenPtr;
    int simpleVarName, isScalar, localIndex, numWords;
    DefineLineInformation;	/* TIP #280 */

    numWords = parsePtr->numWords;
    if (numWords == 1) {
	return TCL_ERROR;
    } else if (numWords == 2) {
	/*
	 * append varName == set varName
	 */

	return TclCompileSetCmd(interp, parsePtr, cmdPtr, envPtr);
    } else if (numWords > 3) {
	/*
	 * APPEND instructions currently only handle one value.
	 */

	return TCL_ERROR;
    }

    /*
     * Decide if we can use a frame slot for the var/array name or if we need
     * to emit code to compute and push the name at runtime. We use a frame
     * slot (entry in the array of local vars) if we are compiling a procedure
     * body and if the name is simple text that does not include namespace
     * qualifiers.
     */

    varTokenPtr = TokenAfter(parsePtr->tokenPtr);

    PushVarNameWord(interp, varTokenPtr, envPtr, 0,
	    &localIndex, &simpleVarName, &isScalar, 1);

    /*
     * We are doing an assignment, otherwise TclCompileSetCmd was called, so
     * push the new value. This will need to be extended to push a value for
     * each argument.
     */

    if (numWords > 2) {
	valueTokenPtr = TokenAfter(varTokenPtr);
	CompileWord(envPtr, valueTokenPtr, interp, 2);
    }

    /*
     * Emit instructions to set/get the variable.
     */

    if (simpleVarName) {
	if (isScalar) {
	    if (localIndex < 0) {
		TclEmitOpcode(INST_APPEND_STK, envPtr);
	    } else if (localIndex <= 255) {
		TclEmitInstInt1(INST_APPEND_SCALAR1, localIndex, envPtr);
	    } else {
		TclEmitInstInt4(INST_APPEND_SCALAR4, localIndex, envPtr);
	    }
	} else {
	    if (localIndex < 0) {
		TclEmitOpcode(INST_APPEND_ARRAY_STK, envPtr);
	    } else if (localIndex <= 255) {
		TclEmitInstInt1(INST_APPEND_ARRAY1, localIndex, envPtr);
	    } else {
		TclEmitInstInt4(INST_APPEND_ARRAY4, localIndex, envPtr);
	    }
	}
    } else {
	TclEmitOpcode(INST_APPEND_STK, envPtr);
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileBreakCmd --
 *
 *	Procedure called to compile the "break" command.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "break" command at
 *	runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileBreakCmd(
    Tcl_Interp *interp,		/* Used for error reporting. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    if (parsePtr->numWords != 1) {
	return TCL_ERROR;
    }

    /*
     * Emit a break instruction.
     */

    TclEmitOpcode(INST_BREAK, envPtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileCatchCmd --
 *
 *	Procedure called to compile the "catch" command.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "catch" command at
 *	runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileCatchCmd(
    Tcl_Interp *interp,		/* Used for error reporting. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    JumpFixup jumpFixup;
    Tcl_Token *cmdTokenPtr, *resultNameTokenPtr, *optsNameTokenPtr;
    const char *name;
    int resultIndex, optsIndex, nameChars, range;
    int savedStackDepth = envPtr->currStackDepth;
    DefineLineInformation;	/* TIP #280 */

    /*
     * If syntax does not match what we expect for [catch], do not compile.
     * Let runtime checks determine if syntax has changed.
     */

    if ((parsePtr->numWords < 2) || (parsePtr->numWords > 4)) {
	return TCL_ERROR;
    }

    /*
     * If variables were specified and the catch command is at global level
     * (not in a procedure), don't compile it inline: the payoff is too small.
     */

    if ((parsePtr->numWords >= 3) && !EnvHasLVT(envPtr)) {
	return TCL_ERROR;
    }

    /*
     * Make sure the variable names, if any, have no substitutions and just
     * refer to local scalars.
     */

    resultIndex = optsIndex = -1;
    cmdTokenPtr = TokenAfter(parsePtr->tokenPtr);
    if (parsePtr->numWords >= 3) {
	resultNameTokenPtr = TokenAfter(cmdTokenPtr);
	/* DGP */
	if (resultNameTokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
	    return TCL_ERROR;
	}

	name = resultNameTokenPtr[1].start;
	nameChars = resultNameTokenPtr[1].size;
	if (!TclIsLocalScalar(name, nameChars)) {
	    return TCL_ERROR;
	}
	resultIndex = TclFindCompiledLocal(resultNameTokenPtr[1].start,
		resultNameTokenPtr[1].size, /*create*/ 1, envPtr);
	if (resultIndex < 0) {
	    return TCL_ERROR;
	}

	/* DKF */
	if (parsePtr->numWords == 4) {
	    optsNameTokenPtr = TokenAfter(resultNameTokenPtr);
	    if (optsNameTokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
		return TCL_ERROR;
	    }
	    name = optsNameTokenPtr[1].start;
	    nameChars = optsNameTokenPtr[1].size;
	    if (!TclIsLocalScalar(name, nameChars)) {
		return TCL_ERROR;
	    }
	    optsIndex = TclFindCompiledLocal(optsNameTokenPtr[1].start,
		    optsNameTokenPtr[1].size, /*create*/ 1, envPtr);
	    if (optsIndex < 0) {
		return TCL_ERROR;
	    }
	}
    }

    /*
     * We will compile the catch command. Emit a beginCatch instruction at the
     * start of the catch body: the subcommand it controls.
     */

    range = DeclareExceptionRange(envPtr, CATCH_EXCEPTION_RANGE);
    TclEmitInstInt4(INST_BEGIN_CATCH4, range, envPtr);

    /*
     * If the body is a simple word, compile the instructions to eval it.
     * Otherwise, compile instructions to substitute its text without
     * catching, a catch instruction that resets the stack to what it was
     * before substituting the body, and then an instruction to eval the body.
     * Care has to be taken to register the correct startOffset for the catch
     * range so that errors in the substitution are not caught. [Bug 219184]
     */

    SetLineInformation(1);
    if (cmdTokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
	ExceptionRangeStarts(envPtr, range);
	CompileBody(envPtr, cmdTokenPtr, interp);
	ExceptionRangeEnds(envPtr, range);
    } else {
	CompileTokens(envPtr, cmdTokenPtr, interp);
	ExceptionRangeStarts(envPtr, range);
	TclEmitOpcode(INST_EVAL_STK, envPtr);
	ExceptionRangeEnds(envPtr, range);
    }

    /*
     * The "no errors" epilogue code: store the body's result into the
     * variable (if any), push "0" (TCL_OK) as the catch's "no error" result,
     * and jump around the "error case" code. Note that we issue the push of
     * the return options first so that if alterations happen to the current
     * interpreter state during the writing of the variable, we won't see
     * them; this results in a slightly complex instruction issuing flow
     * (can't exchange, only duplicate and pop).
     */

    if (resultIndex != -1) {
	if (optsIndex != -1) {
	    TclEmitOpcode(INST_PUSH_RETURN_OPTIONS, envPtr);
	    TclEmitInstInt4(INST_OVER, 1, envPtr);
	}
	if (resultIndex <= 255) {
	    TclEmitInstInt1(INST_STORE_SCALAR1, resultIndex, envPtr);
	} else {
	    TclEmitInstInt4(INST_STORE_SCALAR4, resultIndex, envPtr);
	}
	if (optsIndex != -1) {
	    TclEmitOpcode(INST_POP, envPtr);
	    if (optsIndex <= 255) {
		TclEmitInstInt1(INST_STORE_SCALAR1, optsIndex, envPtr);
	    } else {
		TclEmitInstInt4(INST_STORE_SCALAR4, optsIndex, envPtr);
	    }
	    TclEmitOpcode(INST_POP, envPtr);
	}
    }
    TclEmitOpcode(INST_POP, envPtr);
    PushLiteral(envPtr, "0", 1);
    TclEmitForwardJump(envPtr, TCL_UNCONDITIONAL_JUMP, &jumpFixup);

    /*
     * The "error case" code: store the body's result into the variable (if
     * any), then push the error result code. The initial PC offset here is
     * the catch's error target. Note that if we are saving the return
     * options, we do that first so the preservation cannot get affected by
     * any intermediate result handling.
     */

    envPtr->currStackDepth = savedStackDepth;
    ExceptionRangeTarget(envPtr, range, catchOffset);
    if (resultIndex != -1) {
	if (optsIndex != -1) {
	    TclEmitOpcode(INST_PUSH_RETURN_OPTIONS, envPtr);
	}
	TclEmitOpcode(INST_PUSH_RESULT, envPtr);
	if (resultIndex <= 255) {
	    TclEmitInstInt1(INST_STORE_SCALAR1, resultIndex, envPtr);
	} else {
	    TclEmitInstInt4(INST_STORE_SCALAR4, resultIndex, envPtr);
	}
	TclEmitOpcode(INST_POP, envPtr);
	if (optsIndex != -1) {
	    if (optsIndex <= 255) {
		TclEmitInstInt1(INST_STORE_SCALAR1, optsIndex, envPtr);
	    } else {
		TclEmitInstInt4(INST_STORE_SCALAR4, optsIndex, envPtr);
	    }
	    TclEmitOpcode(INST_POP, envPtr);
	}
    }
    TclEmitOpcode(INST_PUSH_RETURN_CODE, envPtr);

    /*
     * Update the target of the jump after the "no errors" code, then emit an
     * endCatch instruction at the end of the catch command.
     */

    if (TclFixupForwardJumpToHere(envPtr, &jumpFixup, 127)) {
	Tcl_Panic("TclCompileCatchCmd: bad jump distance %d",
		CurrentOffset(envPtr) - jumpFixup.codeOffset);
    }
    TclEmitOpcode(INST_END_CATCH, envPtr);

    envPtr->currStackDepth = savedStackDepth + 1;
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileContinueCmd --
 *
 *	Procedure called to compile the "continue" command.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "continue" command at
 *	runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileContinueCmd(
    Tcl_Interp *interp,		/* Used for error reporting. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    /*
     * There should be no argument after the "continue".
     */

    if (parsePtr->numWords != 1) {
	return TCL_ERROR;
    }

    /*
     * Emit a continue instruction.
     */

    TclEmitOpcode(INST_CONTINUE, envPtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileDict*Cmd --
 *
 *	Functions called to compile "dict" sucommands.
 *
 * Results:
 *	All return TCL_OK for a successful compile, and TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "dict" subcommand at
 *	runtime.
 *
 * Notes:
 *	The following commands are in fairly common use and are possibly worth
 *	bytecoding:
 *		dict append
 *		dict create	[*]
 *		dict exists	[*]
 *		dict for
 *		dict get	[*]
 *		dict incr
 *		dict keys	[*]
 *		dict lappend
 *		dict set
 *		dict unset
 *
 *	In practice, those that are pure-value operators (marked with [*]) can
 *	probably be left alone (except perhaps [dict get] which is very very
 *	common) and [dict update] should be considered instead (really big
 *	win!)
 *
 *----------------------------------------------------------------------
 */

int
TclCompileDictSetCmd(
    Tcl_Interp *interp,		/* Used for looking up stuff. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    Tcl_Token *tokenPtr;
    int numWords, i;
    DefineLineInformation;	/* TIP #280 */
    Tcl_Token *varTokenPtr;
    int dictVarIndex, nameChars;
    const char *name;

    /*
     * There must be at least one argument after the command.
     */

    if (parsePtr->numWords < 4) {
	return TCL_ERROR;
    }

    /*
     * The dictionary variable must be a local scalar that is knowable at
     * compile time; anything else exceeds the complexity of the opcode. So
     * discover what the index is.
     */

    varTokenPtr = TokenAfter(parsePtr->tokenPtr);
    if (varTokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
	return TCL_ERROR;
    }
    name = varTokenPtr[1].start;
    nameChars = varTokenPtr[1].size;
    if (!TclIsLocalScalar(name, nameChars)) {
	return TCL_ERROR;
    }
    dictVarIndex = TclFindCompiledLocal(name, nameChars, 1, envPtr);
    if (dictVarIndex < 0) {
	return TCL_ERROR;
    }

    /*
     * Remaining words (key path and value to set) can be handled normally.
     */

    tokenPtr = TokenAfter(varTokenPtr);
    numWords = parsePtr->numWords-1;
    for (i=1 ; i<numWords ; i++) {
	CompileWord(envPtr, tokenPtr, interp, i);
	tokenPtr = TokenAfter(tokenPtr);
    }

    /*
     * Now emit the instruction to do the dict manipulation.
     */

    TclEmitInstInt4( INST_DICT_SET, numWords-2,		envPtr);
    TclEmitInt4(     dictVarIndex,			envPtr);
    return TCL_OK;
}

int
TclCompileDictIncrCmd(
    Tcl_Interp *interp,		/* Used for looking up stuff. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    DefineLineInformation;	/* TIP #280 */
    Tcl_Token *varTokenPtr, *keyTokenPtr;
    int dictVarIndex, nameChars, incrAmount;
    const char *name;

    /*
     * There must be at least two arguments after the command.
     */

    if (parsePtr->numWords < 3 || parsePtr->numWords > 4) {
	return TCL_ERROR;
    }
    varTokenPtr = TokenAfter(parsePtr->tokenPtr);
    keyTokenPtr = TokenAfter(varTokenPtr);

    /*
     * Parse the increment amount, if present.
     */

    if (parsePtr->numWords == 4) {
	const char *word;
	int numBytes, code;
	Tcl_Token *incrTokenPtr;
	Tcl_Obj *intObj;

	incrTokenPtr = TokenAfter(keyTokenPtr);
	if (incrTokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
	    return TCL_ERROR;
	}
	word = incrTokenPtr[1].start;
	numBytes = incrTokenPtr[1].size;

	intObj = Tcl_NewStringObj(word, numBytes);
	Tcl_IncrRefCount(intObj);
	code = TclGetIntFromObj(NULL, intObj, &incrAmount);
	TclDecrRefCount(intObj);
	if (code != TCL_OK) {
	    return TCL_ERROR;
	}
    } else {
	incrAmount = 1;
    }

    /*
     * The dictionary variable must be a local scalar that is knowable at
     * compile time; anything else exceeds the complexity of the opcode. So
     * discover what the index is.
     */

    if (varTokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
	return TCL_ERROR;
    }
    name = varTokenPtr[1].start;
    nameChars = varTokenPtr[1].size;
    if (!TclIsLocalScalar(name, nameChars)) {
	return TCL_ERROR;
    }
    dictVarIndex = TclFindCompiledLocal(name, nameChars, 1, envPtr);
    if (dictVarIndex < 0) {
	return TCL_ERROR;
    }

    /*
     * Emit the key and the code to actually do the increment.
     */

    CompileWord(envPtr, keyTokenPtr, interp, 3);
    TclEmitInstInt4( INST_DICT_INCR_IMM, incrAmount,	envPtr);
    TclEmitInt4(     dictVarIndex,			envPtr);
    return TCL_OK;
}

int
TclCompileDictGetCmd(
    Tcl_Interp *interp,		/* Used for looking up stuff. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    Tcl_Token *tokenPtr;
    int numWords, i;
    DefineLineInformation;	/* TIP #280 */

    /*
     * There must be at least two arguments after the command (the single-arg
     * case is legal, but too special and magic for us to deal with here).
     */

    if (parsePtr->numWords < 3) {
	return TCL_ERROR;
    }
    tokenPtr = TokenAfter(parsePtr->tokenPtr);
    numWords = parsePtr->numWords-1;

    /*
     * Only compile this because we need INST_DICT_GET anyway.
     */

    for (i=0 ; i<numWords ; i++) {
	CompileWord(envPtr, tokenPtr, interp, i);
	tokenPtr = TokenAfter(tokenPtr);
    }
    TclEmitInstInt4(INST_DICT_GET, numWords-1, envPtr);
    return TCL_OK;
}

int
TclCompileDictForCmd(
    Tcl_Interp *interp,		/* Used for looking up stuff. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    DefineLineInformation;	/* TIP #280 */
    Tcl_Token *varsTokenPtr, *dictTokenPtr, *bodyTokenPtr;
    int keyVarIndex, valueVarIndex, nameChars, loopRange, catchRange;
    int infoIndex, jumpDisplacement, bodyTargetOffset, emptyTargetOffset;
    int numVars, endTargetOffset;
    int savedStackDepth = envPtr->currStackDepth;
				/* Needed because jumps confuse the stack
				 * space calculator. */
    const char **argv;
    Tcl_DString buffer;

    /*
     * There must be at least three argument after the command.
     */

    if (parsePtr->numWords != 4) {
	return TCL_ERROR;
    }

    varsTokenPtr = TokenAfter(parsePtr->tokenPtr);
    dictTokenPtr = TokenAfter(varsTokenPtr);
    bodyTokenPtr = TokenAfter(dictTokenPtr);
    if (varsTokenPtr->type != TCL_TOKEN_SIMPLE_WORD ||
	    bodyTokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
	return TCL_ERROR;
    }

    /*
     * Check we've got a pair of variables and that they are local variables.
     * Then extract their indices in the LVT.
     */

    Tcl_DStringInit(&buffer);
    Tcl_DStringAppend(&buffer, varsTokenPtr[1].start, varsTokenPtr[1].size);
    if (Tcl_SplitList(NULL, Tcl_DStringValue(&buffer), &numVars,
	    &argv) != TCL_OK) {
	Tcl_DStringFree(&buffer);
	return TCL_ERROR;
    }
    Tcl_DStringFree(&buffer);
    if (numVars != 2) {
	ckfree((char *) argv);
	return TCL_ERROR;
    }

    nameChars = strlen(argv[0]);
    if (!TclIsLocalScalar(argv[0], nameChars)) {
	ckfree((char *) argv);
	return TCL_ERROR;
    }
    keyVarIndex = TclFindCompiledLocal(argv[0], nameChars, 1, envPtr);

    nameChars = strlen(argv[1]);
    if (!TclIsLocalScalar(argv[1], nameChars)) {
	ckfree((char *) argv);
	return TCL_ERROR;
    }
    valueVarIndex = TclFindCompiledLocal(argv[1], nameChars, 1, envPtr);
    ckfree((char *) argv);

    if ((keyVarIndex < 0) || (valueVarIndex < 0)) {
	return TCL_ERROR;
    }

    /*
     * Allocate a temporary variable to store the iterator reference. The
     * variable will contain a Tcl_DictSearch reference which will be
     * allocated by INST_DICT_FIRST and disposed when the variable is unset
     * (at which point it should also have been finished with).
     */

    infoIndex = TclFindCompiledLocal(NULL, 0, 1, envPtr);
    if (infoIndex < 0) {
	return TCL_ERROR;
    }

    /*
     * Preparation complete; issue instructions. Note that this code issues
     * fixed-sized jumps. That simplifies things a lot!
     *
     * First up, get the dictionary and start the iteration. No catching of
     * errors at this point.
     */

    CompileWord(envPtr, dictTokenPtr, interp, 3);
    TclEmitInstInt4( INST_DICT_FIRST, infoIndex,		envPtr);
    emptyTargetOffset = CurrentOffset(envPtr);
    TclEmitInstInt4( INST_JUMP_TRUE4, 0,			envPtr);

    /*
     * Now we catch errors from here on so that we can finalize the search
     * started by Tcl_DictObjFirst above.
     */

    catchRange = DeclareExceptionRange(envPtr, CATCH_EXCEPTION_RANGE);
    TclEmitInstInt4( INST_BEGIN_CATCH4, catchRange,		envPtr);
    ExceptionRangeStarts(envPtr, catchRange);

    /*
     * Inside the iteration, write the loop variables.
     */

    bodyTargetOffset = CurrentOffset(envPtr);
    TclEmitInstInt4( INST_STORE_SCALAR4, keyVarIndex,		envPtr);
    TclEmitOpcode(   INST_POP,					envPtr);
    TclEmitInstInt4( INST_STORE_SCALAR4, valueVarIndex,		envPtr);
    TclEmitOpcode(   INST_POP,					envPtr);

    /*
     * Set up the loop exception targets.
     */

    loopRange = DeclareExceptionRange(envPtr, LOOP_EXCEPTION_RANGE);
    ExceptionRangeStarts(envPtr, loopRange);

    /*
     * Compile the loop body itself. It should be stack-neutral.
     */

    SetLineInformation(4);
    CompileBody(envPtr, bodyTokenPtr, interp);
    TclEmitOpcode(   INST_POP,					envPtr);

    /*
     * Both exception target ranges (error and loop) end here.
     */

    ExceptionRangeEnds(envPtr, loopRange);
    ExceptionRangeEnds(envPtr, catchRange);

    /*
     * Continue (or just normally process) by getting the next pair of items
     * from the dictionary and jumping back to the code to write them into
     * variables if there is another pair.
     */

    ExceptionRangeTarget(envPtr, loopRange, continueOffset);
    TclEmitInstInt4( INST_DICT_NEXT, infoIndex,			envPtr);
    jumpDisplacement = bodyTargetOffset - CurrentOffset(envPtr);
    TclEmitInstInt4( INST_JUMP_FALSE4, jumpDisplacement,	envPtr);
    TclEmitOpcode(   INST_POP,					envPtr);
    TclEmitOpcode(   INST_POP,					envPtr);

    /*
     * Now do the final cleanup for the no-error case (this is where we break
     * out of the loop to) by force-terminating the iteration (if not already
     * terminated), ditching the exception info and jumping to the last
     * instruction for this command. In theory, this could be done using the
     * "finally" clause (next generated) but this is faster.
     */

    ExceptionRangeTarget(envPtr, loopRange, breakOffset);
    TclEmitInstInt1( INST_UNSET_SCALAR, 0,			envPtr);
    TclEmitInt4(     infoIndex,					envPtr);
    TclEmitOpcode(   INST_END_CATCH,				envPtr);
    endTargetOffset = CurrentOffset(envPtr);
    TclEmitInstInt4( INST_JUMP4, 0,				envPtr);

    /*
     * Error handler "finally" clause, which force-terminates the iteration
     * and rethrows the error.
     */

    ExceptionRangeTarget(envPtr, catchRange, catchOffset);
    TclEmitOpcode(   INST_PUSH_RETURN_OPTIONS,			envPtr);
    TclEmitOpcode(   INST_PUSH_RESULT,				envPtr);
    TclEmitInstInt1( INST_UNSET_SCALAR, 0,			envPtr);
    TclEmitInt4(     infoIndex,					envPtr);
    TclEmitOpcode(   INST_END_CATCH,				envPtr);
    TclEmitOpcode(   INST_RETURN_STK,				envPtr);

    /*
     * Otherwise we're done (the jump after the DICT_FIRST points here) and we
     * need to pop the bogus key/value pair (pushed to keep stack calculations
     * easy!) Note that we skip the END_CATCH. [Bug 1382528]
     */

    envPtr->currStackDepth = savedStackDepth+2;
    jumpDisplacement = CurrentOffset(envPtr) - emptyTargetOffset;
    TclUpdateInstInt4AtPc(INST_JUMP_TRUE4, jumpDisplacement,
	    envPtr->codeStart + emptyTargetOffset);
    TclEmitOpcode(   INST_POP,					envPtr);
    TclEmitOpcode(   INST_POP,					envPtr);
    TclEmitInstInt1( INST_UNSET_SCALAR, 0,			envPtr);
    TclEmitInt4(     infoIndex,					envPtr);

    /*
     * Final stage of the command (normal case) is that we push an empty
     * object. This is done last to promote peephole optimization when it's
     * dropped immediately.
     */

    jumpDisplacement = CurrentOffset(envPtr) - endTargetOffset;
    TclUpdateInstInt4AtPc(INST_JUMP4, jumpDisplacement,
	    envPtr->codeStart + endTargetOffset);
    PushLiteral(envPtr, "", 0);
    return TCL_OK;
}

int
TclCompileDictUpdateCmd(
    Tcl_Interp *interp,		/* Used for looking up stuff. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    DefineLineInformation;	/* TIP #280 */
    const char *name;
    int i, nameChars, dictIndex, numVars, range, infoIndex;
    Tcl_Token **keyTokenPtrs, *dictVarTokenPtr, *bodyTokenPtr, *tokenPtr;
    int savedStackDepth = envPtr->currStackDepth;
    DictUpdateInfo *duiPtr;
    JumpFixup jumpFixup;

    /*
     * There must be at least one argument after the command.
     */

    if (parsePtr->numWords < 5) {
	return TCL_ERROR;
    }

    /*
     * Parse the command. Expect the following:
     *   dict update <lit(eral)> <any> <lit> ?<any> <lit> ...? <lit>
     */

    if ((parsePtr->numWords - 1) & 1) {
	return TCL_ERROR;
    }
    numVars = (parsePtr->numWords - 3) / 2;

    /*
     * The dictionary variable must be a local scalar that is knowable at
     * compile time; anything else exceeds the complexity of the opcode. So
     * discover what the index is.
     */

    dictVarTokenPtr = TokenAfter(parsePtr->tokenPtr);
    if (dictVarTokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
	return TCL_ERROR;
    }
    name = dictVarTokenPtr[1].start;
    nameChars = dictVarTokenPtr[1].size;
    if (!TclIsLocalScalar(name, nameChars)) {
	return TCL_ERROR;
    }
    dictIndex = TclFindCompiledLocal(name, nameChars, 1, envPtr);
    if (dictIndex < 0) {
	return TCL_ERROR;
    }

    /*
     * Assemble the instruction metadata. This is complex enough that it is
     * represented as auxData; it holds an ordered list of variable indices
     * that are to be used.
     */

    duiPtr = (DictUpdateInfo *)
	    ckalloc(sizeof(DictUpdateInfo) + sizeof(int) * (numVars - 1));
    duiPtr->length = numVars;
    keyTokenPtrs = TclStackAlloc(interp,
	    sizeof(Tcl_Token *) * numVars);
    tokenPtr = TokenAfter(dictVarTokenPtr);

    for (i=0 ; i<numVars ; i++) {
	/*
	 * Put keys to one side for later compilation to bytecode.
	 */

	keyTokenPtrs[i] = tokenPtr;

	/*
	 * Variables first need to be checked for sanity.
	 */

	tokenPtr = TokenAfter(tokenPtr);
	if (tokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
	    goto failedUpdateInfoAssembly;
	}
	name = tokenPtr[1].start;
	nameChars = tokenPtr[1].size;
	if (!TclIsLocalScalar(name, nameChars)) {
	    goto failedUpdateInfoAssembly;
	}

	/*
	 * Stash the index in the auxiliary data.
	 */

	duiPtr->varIndices[i] =
		TclFindCompiledLocal(name, nameChars, 1, envPtr);
	if (duiPtr->varIndices[i] < 0) {
	    goto failedUpdateInfoAssembly;
	}
	tokenPtr = TokenAfter(tokenPtr);
    }
    if (tokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
    failedUpdateInfoAssembly:
	ckfree((char *) duiPtr);
	TclStackFree(interp, keyTokenPtrs);
	return TCL_ERROR;
    }
    bodyTokenPtr = tokenPtr;

    /*
     * The list of variables to bind is stored in auxiliary data so that it
     * can't be snagged by literal sharing and forced to shimmer dangerously.
     */

    infoIndex = TclCreateAuxData(duiPtr, &tclDictUpdateInfoType, envPtr);

    for (i=0 ; i<numVars ; i++) {
	CompileWord(envPtr, keyTokenPtrs[i], interp, i);
    }
    TclEmitInstInt4( INST_LIST, numVars,			envPtr);
    TclEmitInstInt4( INST_DICT_UPDATE_START, dictIndex,		envPtr);
    TclEmitInt4(     infoIndex,					envPtr);

    range = DeclareExceptionRange(envPtr, CATCH_EXCEPTION_RANGE);
    TclEmitInstInt4( INST_BEGIN_CATCH4, range,			envPtr);

    ExceptionRangeStarts(envPtr, range);
    envPtr->currStackDepth++;
    CompileBody(envPtr, bodyTokenPtr, interp);
    envPtr->currStackDepth = savedStackDepth;
    ExceptionRangeEnds(envPtr, range);

    /*
     * Normal termination code: the stack has the key list below the result of
     * the body evaluation: swap them and finish the update code.
     */

    TclEmitOpcode(   INST_END_CATCH,				envPtr);
    TclEmitInstInt4( INST_REVERSE, 2,				envPtr);
    TclEmitInstInt4( INST_DICT_UPDATE_END, dictIndex,		envPtr);
    TclEmitInt4(     infoIndex,					envPtr);

    /*
     * Jump around the exceptional termination code.
     */

    TclEmitForwardJump(envPtr, TCL_UNCONDITIONAL_JUMP, &jumpFixup);

    /*
     * Termination code for non-ok returns: stash the result and return
     * options in the stack, bring up the key list, finish the update code,
     * and finally return with the catched return data
     */

    ExceptionRangeTarget(envPtr, range, catchOffset);
    TclEmitOpcode(   INST_PUSH_RESULT,				envPtr);
    TclEmitOpcode(   INST_PUSH_RETURN_OPTIONS,			envPtr);
    TclEmitOpcode(   INST_END_CATCH,				envPtr);
    TclEmitInstInt4( INST_REVERSE, 3,				envPtr);

    TclEmitInstInt4( INST_DICT_UPDATE_END, dictIndex,		envPtr);
    TclEmitInt4(     infoIndex,					envPtr);
    TclEmitOpcode(   INST_RETURN_STK,				envPtr);

    if (TclFixupForwardJumpToHere(envPtr, &jumpFixup, 127)) {
	Tcl_Panic("TclCompileDictCmd(update): bad jump distance %d",
		CurrentOffset(envPtr) - jumpFixup.codeOffset);
    }
    TclStackFree(interp, keyTokenPtrs);
    return TCL_OK;
}

int
TclCompileDictAppendCmd(
    Tcl_Interp *interp,		/* Used for looking up stuff. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    DefineLineInformation;	/* TIP #280 */
    Tcl_Token *tokenPtr;
    int i, dictVarIndex;

    /*
     * There must be at least two argument after the command. And we impose an
     * (arbirary) safe limit; anyone exceeding it should stop worrying about
     * speed quite so much. ;-)
     */

    if (parsePtr->numWords<4 || parsePtr->numWords>100) {
	return TCL_ERROR;
    }

    /*
     * Get the index of the local variable that we will be working with.
     */

    tokenPtr = TokenAfter(parsePtr->tokenPtr);
    if (tokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
	return TCL_ERROR;
    } else {
	register const char *name = tokenPtr[1].start;
	register int nameChars = tokenPtr[1].size;

	if (!TclIsLocalScalar(name, nameChars)) {
	    return TCL_ERROR;
	}
	dictVarIndex = TclFindCompiledLocal(name, nameChars, 1, envPtr);
	if (dictVarIndex < 0) {
	    return TCL_ERROR;
	}
    }

    /*
     * Produce the string to concatenate onto the dictionary entry.
     */

    tokenPtr = TokenAfter(tokenPtr);
    for (i=2 ; i<parsePtr->numWords ; i++) {
	CompileWord(envPtr, tokenPtr, interp, i);
	tokenPtr = TokenAfter(tokenPtr);
    }
    if (parsePtr->numWords > 4) {
	TclEmitInstInt1(INST_CONCAT1, parsePtr->numWords-3, envPtr);
    }

    /*
     * Do the concatenation.
     */

    TclEmitInstInt4(INST_DICT_APPEND, dictVarIndex, envPtr);
    return TCL_OK;
}

int
TclCompileDictLappendCmd(
    Tcl_Interp *interp,		/* Used for looking up stuff. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    DefineLineInformation;	/* TIP #280 */
    Tcl_Token *varTokenPtr, *keyTokenPtr, *valueTokenPtr;
    int dictVarIndex, nameChars;
    const char *name;

    /*
     * There must be three arguments after the command.
     */

    if (parsePtr->numWords != 4) {
	return TCL_ERROR;
    }

    varTokenPtr = TokenAfter(parsePtr->tokenPtr);
    keyTokenPtr = TokenAfter(varTokenPtr);
    valueTokenPtr = TokenAfter(keyTokenPtr);
    if (varTokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
	return TCL_ERROR;
    }
    name = varTokenPtr[1].start;
    nameChars = varTokenPtr[1].size;
    if (!TclIsLocalScalar(name, nameChars)) {
	return TCL_ERROR;
    }
    dictVarIndex = TclFindCompiledLocal(name, nameChars, 1, envPtr);
    if (dictVarIndex < 0) {
	return TCL_ERROR;
    }
    CompileWord(envPtr, keyTokenPtr, interp, 3);
    CompileWord(envPtr, valueTokenPtr, interp, 4);
    TclEmitInstInt4( INST_DICT_LAPPEND, dictVarIndex, envPtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * DupDictUpdateInfo, FreeDictUpdateInfo --
 *
 *	Functions to duplicate, release and print the aux data created for use
 *	with the INST_DICT_UPDATE_START and INST_DICT_UPDATE_END instructions.
 *
 * Results:
 *	DupDictUpdateInfo: a copy of the auxiliary data
 *	FreeDictUpdateInfo: none
 *	PrintDictUpdateInfo: none
 *
 * Side effects:
 *	DupDictUpdateInfo: allocates memory
 *	FreeDictUpdateInfo: releases memory
 *	PrintDictUpdateInfo: none
 *
 *----------------------------------------------------------------------
 */

static ClientData
DupDictUpdateInfo(
    ClientData clientData)
{
    DictUpdateInfo *dui1Ptr, *dui2Ptr;
    unsigned len;

    dui1Ptr = clientData;
    len = sizeof(DictUpdateInfo) + sizeof(int) * (dui1Ptr->length - 1);
    dui2Ptr = (DictUpdateInfo *) ckalloc(len);
    memcpy(dui2Ptr, dui1Ptr, len);
    return dui2Ptr;
}

static void
FreeDictUpdateInfo(
    ClientData clientData)
{
    ckfree(clientData);
}

static void
PrintDictUpdateInfo(
    ClientData clientData,
    Tcl_Obj *appendObj,
    ByteCode *codePtr,
    unsigned int pcOffset)
{
    DictUpdateInfo *duiPtr = clientData;
    int i;

    for (i=0 ; i<duiPtr->length ; i++) {
	if (i) {
	    Tcl_AppendToObj(appendObj, ", ", -1);
	}
	Tcl_AppendPrintfToObj(appendObj, "%%v%u", duiPtr->varIndices[i]);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileErrorCmd --
 *
 *	Procedure called to compile the "error" command.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "error" command at
 *	runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileErrorCmd(
    Tcl_Interp *interp,		/* Used for context. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    /*
     * General syntax: [error message ?errorInfo? ?errorCode?]
     * However, we only deal with the case where there is just a message.
     */
    Tcl_Token *messageTokenPtr;
    DefineLineInformation;	/* TIP #280 */

    if (parsePtr->numWords != 2) {
	return TCL_ERROR;
    }
    messageTokenPtr = TokenAfter(parsePtr->tokenPtr);

    PushLiteral(envPtr, "-code error -level 0", 20);
    CompileWord(envPtr, messageTokenPtr, interp, 1);
    TclEmitOpcode(INST_RETURN_STK, envPtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileExprCmd --
 *
 *	Procedure called to compile the "expr" command.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "expr" command at
 *	runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileExprCmd(
    Tcl_Interp *interp,		/* Used for error reporting. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    Tcl_Token *firstWordPtr;

    if (parsePtr->numWords == 1) {
	return TCL_ERROR;
    }

    /*
     * TIP #280: Use the per-word line information of the current command.
     */

    envPtr->line = envPtr->extCmdMapPtr->loc[
	    envPtr->extCmdMapPtr->nuloc-1].line[1];

    firstWordPtr = TokenAfter(parsePtr->tokenPtr);
    TclCompileExprWords(interp, firstWordPtr, parsePtr->numWords-1, envPtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileForCmd --
 *
 *	Procedure called to compile the "for" command.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "for" command at
 *	runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileForCmd(
    Tcl_Interp *interp,		/* Used for error reporting. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    Tcl_Token *startTokenPtr, *testTokenPtr, *nextTokenPtr, *bodyTokenPtr;
    JumpFixup jumpEvalCondFixup;
    int testCodeOffset, bodyCodeOffset, nextCodeOffset, jumpDist;
    int bodyRange, nextRange;
    int savedStackDepth = envPtr->currStackDepth;
    DefineLineInformation;	/* TIP #280 */

    if (parsePtr->numWords != 5) {
	return TCL_ERROR;
    }

    /*
     * If the test expression requires substitutions, don't compile the for
     * command inline. E.g., the expression might cause the loop to never
     * execute or execute forever, as in "for {} "$x > 5" {incr x} {}".
     */

    startTokenPtr = TokenAfter(parsePtr->tokenPtr);
    testTokenPtr = TokenAfter(startTokenPtr);
    if (testTokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
	return TCL_ERROR;
    }

    /*
     * Bail out also if the body or the next expression require substitutions
     * in order to insure correct behaviour [Bug 219166]
     */

    nextTokenPtr = TokenAfter(testTokenPtr);
    bodyTokenPtr = TokenAfter(nextTokenPtr);
    if ((nextTokenPtr->type != TCL_TOKEN_SIMPLE_WORD)
	    || (bodyTokenPtr->type != TCL_TOKEN_SIMPLE_WORD)) {
	return TCL_ERROR;
    }

    /*
     * Create ExceptionRange records for the body and the "next" command. The
     * "next" command's ExceptionRange supports break but not continue (and
     * has a -1 continueOffset).
     */

    bodyRange = DeclareExceptionRange(envPtr, LOOP_EXCEPTION_RANGE);
    nextRange = TclCreateExceptRange(LOOP_EXCEPTION_RANGE, envPtr);

    /*
     * Inline compile the initial command.
     */

    SetLineInformation(1);
    CompileBody(envPtr, startTokenPtr, interp);
    TclEmitOpcode(INST_POP, envPtr);

    /*
     * Jump to the evaluation of the condition. This code uses the "loop
     * rotation" optimisation (which eliminates one branch from the loop).
     * "for start cond next body" produces then:
     *       start
     *       goto A
     *    B: body                : bodyCodeOffset
     *       next                : nextCodeOffset, continueOffset
     *    A: cond -> result      : testCodeOffset
     *       if (result) goto B
     */

    TclEmitForwardJump(envPtr, TCL_UNCONDITIONAL_JUMP, &jumpEvalCondFixup);

    /*
     * Compile the loop body.
     */

    bodyCodeOffset = ExceptionRangeStarts(envPtr, bodyRange);
    SetLineInformation(4);
    CompileBody(envPtr, bodyTokenPtr, interp);
    ExceptionRangeEnds(envPtr, bodyRange);
    envPtr->currStackDepth = savedStackDepth + 1;
    TclEmitOpcode(INST_POP, envPtr);

    /*
     * Compile the "next" subcommand.
     */

    envPtr->currStackDepth = savedStackDepth;
    nextCodeOffset = ExceptionRangeStarts(envPtr, nextRange);
    SetLineInformation(3);
    CompileBody(envPtr, nextTokenPtr, interp);
    ExceptionRangeEnds(envPtr, nextRange);
    envPtr->currStackDepth = savedStackDepth + 1;
    TclEmitOpcode(INST_POP, envPtr);
    envPtr->currStackDepth = savedStackDepth;

    /*
     * Compile the test expression then emit the conditional jump that
     * terminates the for.
     */

    testCodeOffset = CurrentOffset(envPtr);

    jumpDist = testCodeOffset - jumpEvalCondFixup.codeOffset;
    if (TclFixupForwardJump(envPtr, &jumpEvalCondFixup, jumpDist, 127)) {
	bodyCodeOffset += 3;
	nextCodeOffset += 3;
	testCodeOffset += 3;
    }

    SetLineInformation(2);
    envPtr->currStackDepth = savedStackDepth;
    TclCompileExprWords(interp, testTokenPtr, 1, envPtr);
    envPtr->currStackDepth = savedStackDepth + 1;

    jumpDist = CurrentOffset(envPtr) - bodyCodeOffset;
    if (jumpDist > 127) {
	TclEmitInstInt4(INST_JUMP_TRUE4, -jumpDist, envPtr);
    } else {
	TclEmitInstInt1(INST_JUMP_TRUE1, -jumpDist, envPtr);
    }

    /*
     * Fix the starting points of the exception ranges (may have moved due to
     * jump type modification) and set where the exceptions target.
     */

    envPtr->exceptArrayPtr[bodyRange].codeOffset = bodyCodeOffset;
    envPtr->exceptArrayPtr[bodyRange].continueOffset = nextCodeOffset;

    envPtr->exceptArrayPtr[nextRange].codeOffset = nextCodeOffset;

    ExceptionRangeTarget(envPtr, bodyRange, breakOffset);
    ExceptionRangeTarget(envPtr, nextRange, breakOffset);

    /*
     * The for command's result is an empty string.
     */

    envPtr->currStackDepth = savedStackDepth;
    PushLiteral(envPtr, "", 0);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileForeachCmd --
 *
 *	Procedure called to compile the "foreach" command.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "foreach" command at
 *	runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileForeachCmd(
    Tcl_Interp *interp,		/* Used for error reporting. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    Proc *procPtr = envPtr->procPtr;
    ForeachInfo *infoPtr;	/* Points to the structure describing this
				 * foreach command. Stored in a AuxData
				 * record in the ByteCode. */
    int firstValueTemp;		/* Index of the first temp var in the frame
				 * used to point to a value list. */
    int loopCtTemp;		/* Index of temp var holding the loop's
				 * iteration count. */
    Tcl_Token *tokenPtr, *bodyTokenPtr;
    unsigned char *jumpPc;
    JumpFixup jumpFalseFixup;
    int jumpBackDist, jumpBackOffset, infoIndex, range, bodyIndex;
    int numWords, numLists, numVars, loopIndex, tempVar, i, j, code;
    int savedStackDepth = envPtr->currStackDepth;
    DefineLineInformation;	/* TIP #280 */

    /*
     * We parse the variable list argument words and create two arrays:
     *    varcList[i] is number of variables in i-th var list.
     *    varvList[i] points to array of var names in i-th var list.
     */

    int *varcList;
    const char ***varvList;

    /*
     * If the foreach command isn't in a procedure, don't compile it inline:
     * the payoff is too small.
     */

    if (procPtr == NULL) {
	return TCL_ERROR;
    }

    numWords = parsePtr->numWords;
    if ((numWords < 4) || (numWords%2 != 0)) {
	return TCL_ERROR;
    }

    /*
     * Bail out if the body requires substitutions in order to insure correct
     * behaviour. [Bug 219166]
     */

    for (i = 0, tokenPtr = parsePtr->tokenPtr; i < numWords-1; i++) {
	tokenPtr = TokenAfter(tokenPtr);
    }
    bodyTokenPtr = tokenPtr;
    if (bodyTokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
	return TCL_ERROR;
    }

    bodyIndex = i-1;

    /*
     * Allocate storage for the varcList and varvList arrays if necessary.
     */

    numLists = (numWords - 2)/2;
    varcList = TclStackAlloc(interp, numLists * sizeof(int));
    memset(varcList, 0, numLists * sizeof(int));
    varvList = (const char ***) TclStackAlloc(interp,
	    numLists * sizeof(const char **));
    memset((char*) varvList, 0, numLists * sizeof(const char **));

    /*
     * Break up each var list and set the varcList and varvList arrays. Don't
     * compile the foreach inline if any var name needs substitutions or isn't
     * a scalar, or if any var list needs substitutions.
     */

    loopIndex = 0;
    for (i = 0, tokenPtr = parsePtr->tokenPtr;
	    i < numWords-1;
	    i++, tokenPtr = TokenAfter(tokenPtr)) {
	Tcl_DString varList;

	if (i%2 != 1) {
	    continue;
	}
	if (tokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
	    code = TCL_ERROR;
	    goto done;
	}

	/*
	 * Lots of copying going on here. Need a ListObj wizard to show a
	 * better way.
	 */

	Tcl_DStringInit(&varList);
	Tcl_DStringAppend(&varList, tokenPtr[1].start, tokenPtr[1].size);
	code = Tcl_SplitList(interp, Tcl_DStringValue(&varList),
		&varcList[loopIndex], &varvList[loopIndex]);
	Tcl_DStringFree(&varList);
	if (code != TCL_OK) {
	    code = TCL_ERROR;
	    goto done;
	}
	numVars = varcList[loopIndex];

	/*
	 * If the variable list is empty, we can enter an infinite loop when
	 * the interpreted version would not. Take care to ensure this does
	 * not happen. [Bug 1671138]
	 */

	if (numVars == 0) {
	    code = TCL_ERROR;
	    goto done;
	}

	for (j = 0;  j < numVars;  j++) {
	    const char *varName = varvList[loopIndex][j];

	    if (!TclIsLocalScalar(varName, (int) strlen(varName))) {
		code = TCL_ERROR;
		goto done;
	    }
	}
	loopIndex++;
    }

    /*
     * We will compile the foreach command. Reserve (numLists + 1) temporary
     * variables:
     *    - numLists temps to hold each value list
     *    - 1 temp for the loop counter (index of next element in each list)
     *
     * At this time we don't try to reuse temporaries; if there are two
     * nonoverlapping foreach loops, they don't share any temps.
     */

    code = TCL_OK;
    firstValueTemp = -1;
    for (loopIndex = 0;  loopIndex < numLists;  loopIndex++) {
	tempVar = TclFindCompiledLocal(NULL, /*nameChars*/ 0,
		/*create*/ 1, envPtr);
	if (loopIndex == 0) {
	    firstValueTemp = tempVar;
	}
    }
    loopCtTemp = TclFindCompiledLocal(NULL, /*nameChars*/ 0,
	    /*create*/ 1, envPtr);

    /*
     * Create and initialize the ForeachInfo and ForeachVarList data
     * structures describing this command. Then create a AuxData record
     * pointing to the ForeachInfo structure.
     */

    infoPtr = (ForeachInfo *) ckalloc((unsigned)
	    sizeof(ForeachInfo) + numLists*sizeof(ForeachVarList *));
    infoPtr->numLists = numLists;
    infoPtr->firstValueTemp = firstValueTemp;
    infoPtr->loopCtTemp = loopCtTemp;
    for (loopIndex = 0;  loopIndex < numLists;  loopIndex++) {
	ForeachVarList *varListPtr;

	numVars = varcList[loopIndex];
	varListPtr = (ForeachVarList *) ckalloc((unsigned)
		sizeof(ForeachVarList) + numVars*sizeof(int));
	varListPtr->numVars = numVars;
	for (j = 0;  j < numVars;  j++) {
	    const char *varName = varvList[loopIndex][j];
	    int nameChars = strlen(varName);

	    varListPtr->varIndexes[j] = TclFindCompiledLocal(varName,
		    nameChars, /*create*/ 1, envPtr);
	}
	infoPtr->varLists[loopIndex] = varListPtr;
    }
    infoIndex = TclCreateAuxData(infoPtr, &tclForeachInfoType, envPtr);

    /*
     * Create an exception record to handle [break] and [continue].
     */

    range = DeclareExceptionRange(envPtr, LOOP_EXCEPTION_RANGE);

    /*
     * Evaluate then store each value list in the associated temporary.
     */

    loopIndex = 0;
    for (i = 0, tokenPtr = parsePtr->tokenPtr;
	    i < numWords-1;
	    i++, tokenPtr = TokenAfter(tokenPtr)) {
	if ((i%2 == 0) && (i > 0)) {
	    SetLineInformation(i);
	    CompileTokens(envPtr, tokenPtr, interp);
	    tempVar = (firstValueTemp + loopIndex);
	    if (tempVar <= 255) {
		TclEmitInstInt1(INST_STORE_SCALAR1, tempVar, envPtr);
	    } else {
		TclEmitInstInt4(INST_STORE_SCALAR4, tempVar, envPtr);
	    }
	    TclEmitOpcode(INST_POP, envPtr);
	    loopIndex++;
	}
    }

    /*
     * Initialize the temporary var that holds the count of loop iterations.
     */

    TclEmitInstInt4(INST_FOREACH_START4, infoIndex, envPtr);

    /*
     * Top of loop code: assign each loop variable and check whether
     * to terminate the loop.
     */

    ExceptionRangeTarget(envPtr, range, continueOffset);
    TclEmitInstInt4(INST_FOREACH_STEP4, infoIndex, envPtr);
    TclEmitForwardJump(envPtr, TCL_FALSE_JUMP, &jumpFalseFixup);

    /*
     * Inline compile the loop body.
     */

    SetLineInformation(bodyIndex);
    ExceptionRangeStarts(envPtr, range);
    CompileBody(envPtr, bodyTokenPtr, interp);
    ExceptionRangeEnds(envPtr, range);
    envPtr->currStackDepth = savedStackDepth + 1;
    TclEmitOpcode(INST_POP, envPtr);

    /*
     * Jump back to the test at the top of the loop. Generate a 4 byte jump if
     * the distance to the test is > 120 bytes. This is conservative and
     * ensures that we won't have to replace this jump if we later need to
     * replace the ifFalse jump with a 4 byte jump.
     */

    jumpBackOffset = CurrentOffset(envPtr);
    jumpBackDist = jumpBackOffset-envPtr->exceptArrayPtr[range].continueOffset;
    if (jumpBackDist > 120) {
	TclEmitInstInt4(INST_JUMP4, -jumpBackDist, envPtr);
    } else {
	TclEmitInstInt1(INST_JUMP1, -jumpBackDist, envPtr);
    }

    /*
     * Fix the target of the jump after the foreach_step test.
     */

    if (TclFixupForwardJumpToHere(envPtr, &jumpFalseFixup, 127)) {
	/*
	 * Update the loop body's starting PC offset since it moved down.
	 */

	envPtr->exceptArrayPtr[range].codeOffset += 3;

	/*
	 * Update the jump back to the test at the top of the loop since it
	 * also moved down 3 bytes.
	 */

	jumpBackOffset += 3;
	jumpPc = (envPtr->codeStart + jumpBackOffset);
	jumpBackDist += 3;
	if (jumpBackDist > 120) {
	    TclUpdateInstInt4AtPc(INST_JUMP4, -jumpBackDist, jumpPc);
	} else {
	    TclUpdateInstInt1AtPc(INST_JUMP1, -jumpBackDist, jumpPc);
	}
    }

    /*
     * Set the loop's break target.
     */

    ExceptionRangeTarget(envPtr, range, breakOffset);

    /*
     * The foreach command's result is an empty string.
     */

    envPtr->currStackDepth = savedStackDepth;
    PushLiteral(envPtr, "", 0);
    envPtr->currStackDepth = savedStackDepth + 1;

  done:
    for (loopIndex = 0;  loopIndex < numLists;  loopIndex++) {
	if (varvList[loopIndex] != NULL) {
	    ckfree((char *) varvList[loopIndex]);
	}
    }
    TclStackFree(interp, (void *)varvList);
    TclStackFree(interp, varcList);
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * DupForeachInfo --
 *
 *	This procedure duplicates a ForeachInfo structure created as auxiliary
 *	data during the compilation of a foreach command.
 *
 * Results:
 *	A pointer to a newly allocated copy of the existing ForeachInfo
 *	structure is returned.
 *
 * Side effects:
 *	Storage for the copied ForeachInfo record is allocated. If the
 *	original ForeachInfo structure pointed to any ForeachVarList records,
 *	these structures are also copied and pointers to them are stored in
 *	the new ForeachInfo record.
 *
 *----------------------------------------------------------------------
 */

static ClientData
DupForeachInfo(
    ClientData clientData)	/* The foreach command's compilation auxiliary
				 * data to duplicate. */
{
    register ForeachInfo *srcPtr = clientData;
    ForeachInfo *dupPtr;
    register ForeachVarList *srcListPtr, *dupListPtr;
    int numVars, i, j, numLists = srcPtr->numLists;

    dupPtr = (ForeachInfo *) ckalloc((unsigned)
	    sizeof(ForeachInfo) + numLists*sizeof(ForeachVarList *));
    dupPtr->numLists = numLists;
    dupPtr->firstValueTemp = srcPtr->firstValueTemp;
    dupPtr->loopCtTemp = srcPtr->loopCtTemp;

    for (i = 0;  i < numLists;  i++) {
	srcListPtr = srcPtr->varLists[i];
	numVars = srcListPtr->numVars;
	dupListPtr = (ForeachVarList *) ckalloc((unsigned)
		sizeof(ForeachVarList) + numVars*sizeof(int));
	dupListPtr->numVars = numVars;
	for (j = 0;  j < numVars;  j++) {
	    dupListPtr->varIndexes[j] =	srcListPtr->varIndexes[j];
	}
	dupPtr->varLists[i] = dupListPtr;
    }
    return dupPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * FreeForeachInfo --
 *
 *	Procedure to free a ForeachInfo structure created as auxiliary data
 *	during the compilation of a foreach command.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Storage for the ForeachInfo structure pointed to by the ClientData
 *	argument is freed as is any ForeachVarList record pointed to by the
 *	ForeachInfo structure.
 *
 *----------------------------------------------------------------------
 */

static void
FreeForeachInfo(
    ClientData clientData)	/* The foreach command's compilation auxiliary
				 * data to free. */
{
    register ForeachInfo *infoPtr = clientData;
    register ForeachVarList *listPtr;
    int numLists = infoPtr->numLists;
    register int i;

    for (i = 0;  i < numLists;  i++) {
	listPtr = infoPtr->varLists[i];
	ckfree((char *) listPtr);
    }
    ckfree((char *) infoPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * PrintForeachInfo --
 *
 *	Function to write a human-readable representation of a ForeachInfo
 *	structure to stdout for debugging.
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
PrintForeachInfo(
    ClientData clientData,
    Tcl_Obj *appendObj,
    ByteCode *codePtr,
    unsigned int pcOffset)
{
    register ForeachInfo *infoPtr = clientData;
    register ForeachVarList *varsPtr;
    int i, j;

    Tcl_AppendToObj(appendObj, "data=[", -1);

    for (i=0 ; i<infoPtr->numLists ; i++) {
	if (i) {
	    Tcl_AppendToObj(appendObj, ", ", -1);
	}
	Tcl_AppendPrintfToObj(appendObj, "%%v%u",
		(unsigned) (infoPtr->firstValueTemp + i));
    }
    Tcl_AppendPrintfToObj(appendObj, "], loop=%%v%u",
	    (unsigned) infoPtr->loopCtTemp);
    for (i=0 ; i<infoPtr->numLists ; i++) {
	if (i) {
	    Tcl_AppendToObj(appendObj, ",", -1);
	}
	Tcl_AppendPrintfToObj(appendObj, "\n\t\t it%%v%u\t[",
		(unsigned) (infoPtr->firstValueTemp + i));
	varsPtr = infoPtr->varLists[i];
	for (j=0 ; j<varsPtr->numVars ; j++) {
	    if (j) {
		Tcl_AppendToObj(appendObj, ", ", -1);
	    }
	    Tcl_AppendPrintfToObj(appendObj, "%%v%u",
		    (unsigned) varsPtr->varIndexes[j]);
	}
	Tcl_AppendToObj(appendObj, "]", -1);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileGlobalCmd --
 *
 *	Procedure called to compile the "global" command.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "global" command at
 *	runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileGlobalCmd(
    Tcl_Interp *interp,		/* Used for error reporting. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    Tcl_Token *varTokenPtr;
    int localIndex, numWords, i;
    DefineLineInformation;	/* TIP #280 */

    numWords = parsePtr->numWords;
    if (numWords < 2) {
	return TCL_ERROR;
    }

    /*
     * 'global' has no effect outside of proc bodies; handle that at runtime
     */

    if (envPtr->procPtr == NULL) {
	return TCL_ERROR;
    }

    /*
     * Push the namespace
     */

    PushLiteral(envPtr, "::", 2);

    /*
     * Loop over the variables.
     */

    varTokenPtr = TokenAfter(parsePtr->tokenPtr);
    for (i=2; i<=numWords; varTokenPtr = TokenAfter(varTokenPtr),i++) {
	localIndex = IndexTailVarIfKnown(interp, varTokenPtr, envPtr);

	if (localIndex < 0) {
	    return TCL_ERROR;
	}

	CompileWord(envPtr, varTokenPtr, interp, 1);
	TclEmitInstInt4(INST_NSUPVAR, localIndex, envPtr);
    }

    /*
     * Pop the namespace, and set the result to empty
     */

    TclEmitOpcode(INST_POP, envPtr);
    PushLiteral(envPtr, "", 0);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileIfCmd --
 *
 *	Procedure called to compile the "if" command.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "if" command at
 *	runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileIfCmd(
    Tcl_Interp *interp,		/* Used for error reporting. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    JumpFixupArray jumpFalseFixupArray;
				/* Used to fix the ifFalse jump after each
				 * test when its target PC is determined. */
    JumpFixupArray jumpEndFixupArray;
				/* Used to fix the jump after each "then" body
				 * to the end of the "if" when that PC is
				 * determined. */
    Tcl_Token *tokenPtr, *testTokenPtr;
    int jumpIndex = 0;		/* Avoid compiler warning. */
    int jumpFalseDist, numWords, wordIdx, numBytes, j, code;
    const char *word;
    int savedStackDepth = envPtr->currStackDepth;
				/* Saved stack depth at the start of the first
				 * test; the envPtr current depth is restored
				 * to this value at the start of each test. */
    int realCond = 1;		/* Set to 0 for static conditions:
				 * "if 0 {..}" */
    int boolVal;		/* Value of static condition. */
    int compileScripts = 1;
    DefineLineInformation;	/* TIP #280 */

    /*
     * Only compile the "if" command if all arguments are simple words, in
     * order to insure correct substitution [Bug 219166]
     */

    tokenPtr = parsePtr->tokenPtr;
    wordIdx = 0;
    numWords = parsePtr->numWords;

    for (wordIdx = 0; wordIdx < numWords; wordIdx++) {
	if (tokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
	    return TCL_ERROR;
	}
	tokenPtr = TokenAfter(tokenPtr);
    }

    TclInitJumpFixupArray(&jumpFalseFixupArray);
    TclInitJumpFixupArray(&jumpEndFixupArray);
    code = TCL_OK;

    /*
     * Each iteration of this loop compiles one "if expr ?then? body" or
     * "elseif expr ?then? body" clause.
     */

    tokenPtr = parsePtr->tokenPtr;
    wordIdx = 0;
    while (wordIdx < numWords) {
	/*
	 * Stop looping if the token isn't "if" or "elseif".
	 */

	word = tokenPtr[1].start;
	numBytes = tokenPtr[1].size;
	if ((tokenPtr == parsePtr->tokenPtr)
		|| ((numBytes == 6) && (strncmp(word, "elseif", 6) == 0))) {
	    tokenPtr = TokenAfter(tokenPtr);
	    wordIdx++;
	} else {
	    break;
	}
	if (wordIdx >= numWords) {
	    code = TCL_ERROR;
	    goto done;
	}

	/*
	 * Compile the test expression then emit the conditional jump around
	 * the "then" part.
	 */

	envPtr->currStackDepth = savedStackDepth;
	testTokenPtr = tokenPtr;

	if (realCond) {
	    /*
	     * Find out if the condition is a constant.
	     */

	    Tcl_Obj *boolObj = Tcl_NewStringObj(testTokenPtr[1].start,
		    testTokenPtr[1].size);

	    Tcl_IncrRefCount(boolObj);
	    code = Tcl_GetBooleanFromObj(NULL, boolObj, &boolVal);
	    TclDecrRefCount(boolObj);
	    if (code == TCL_OK) {
		/*
		 * A static condition.
		 */

		realCond = 0;
		if (!boolVal) {
		    compileScripts = 0;
		}
	    } else {
		SetLineInformation(wordIdx);
		Tcl_ResetResult(interp);
		TclCompileExprWords(interp, testTokenPtr, 1, envPtr);
		if (jumpFalseFixupArray.next >= jumpFalseFixupArray.end) {
		    TclExpandJumpFixupArray(&jumpFalseFixupArray);
		}
		jumpIndex = jumpFalseFixupArray.next;
		jumpFalseFixupArray.next++;
		TclEmitForwardJump(envPtr, TCL_FALSE_JUMP,
			jumpFalseFixupArray.fixup+jumpIndex);
	    }
	    code = TCL_OK;
	}

	/*
	 * Skip over the optional "then" before the then clause.
	 */

	tokenPtr = TokenAfter(testTokenPtr);
	wordIdx++;
	if (wordIdx >= numWords) {
	    code = TCL_ERROR;
	    goto done;
	}
	if (tokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
	    word = tokenPtr[1].start;
	    numBytes = tokenPtr[1].size;
	    if ((numBytes == 4) && (strncmp(word, "then", 4) == 0)) {
		tokenPtr = TokenAfter(tokenPtr);
		wordIdx++;
		if (wordIdx >= numWords) {
		    code = TCL_ERROR;
		    goto done;
		}
	    }
	}

	/*
	 * Compile the "then" command body.
	 */

	if (compileScripts) {
	    SetLineInformation(wordIdx);
	    envPtr->currStackDepth = savedStackDepth;
	    CompileBody(envPtr, tokenPtr, interp);
	}

	if (realCond) {
	    /*
	     * Jump to the end of the "if" command. Both jumpFalseFixupArray
	     * and jumpEndFixupArray are indexed by "jumpIndex".
	     */

	    if (jumpEndFixupArray.next >= jumpEndFixupArray.end) {
		TclExpandJumpFixupArray(&jumpEndFixupArray);
	    }
	    jumpEndFixupArray.next++;
	    TclEmitForwardJump(envPtr, TCL_UNCONDITIONAL_JUMP,
		    jumpEndFixupArray.fixup+jumpIndex);

	    /*
	     * Fix the target of the jumpFalse after the test. Generate a 4
	     * byte jump if the distance is > 120 bytes. This is conservative,
	     * and ensures that we won't have to replace this jump if we later
	     * also need to replace the proceeding jump to the end of the "if"
	     * with a 4 byte jump.
	     */

	    if (TclFixupForwardJumpToHere(envPtr,
		    jumpFalseFixupArray.fixup+jumpIndex, 120)) {
		/*
		 * Adjust the code offset for the proceeding jump to the end
		 * of the "if" command.
		 */

		jumpEndFixupArray.fixup[jumpIndex].codeOffset += 3;
	    }
	} else if (boolVal) {
	    /*
	     * We were processing an "if 1 {...}"; stop compiling scripts.
	     */

	    compileScripts = 0;
	} else {
	    /*
	     * We were processing an "if 0 {...}"; reset so that the rest
	     * (elseif, else) is compiled correctly.
	     */

	    realCond = 1;
	    compileScripts = 1;
	}

	tokenPtr = TokenAfter(tokenPtr);
	wordIdx++;
    }

    /*
     * Restore the current stack depth in the environment; the "else" clause
     * (or its default) will add 1 to this.
     */

    envPtr->currStackDepth = savedStackDepth;

    /*
     * Check for the optional else clause. Do not compile anything if this was
     * an "if 1 {...}" case.
     */

    if ((wordIdx < numWords) && (tokenPtr->type == TCL_TOKEN_SIMPLE_WORD)) {
	/*
	 * There is an else clause. Skip over the optional "else" word.
	 */

	word = tokenPtr[1].start;
	numBytes = tokenPtr[1].size;
	if ((numBytes == 4) && (strncmp(word, "else", 4) == 0)) {
	    tokenPtr = TokenAfter(tokenPtr);
	    wordIdx++;
	    if (wordIdx >= numWords) {
		code = TCL_ERROR;
		goto done;
	    }
	}

	if (compileScripts) {
	    /*
	     * Compile the else command body.
	     */

	    SetLineInformation(wordIdx);
	    CompileBody(envPtr, tokenPtr, interp);
	}

	/*
	 * Make sure there are no words after the else clause.
	 */

	wordIdx++;
	if (wordIdx < numWords) {
	    code = TCL_ERROR;
	    goto done;
	}
    } else {
	/*
	 * No else clause: the "if" command's result is an empty string.
	 */

	if (compileScripts) {
	    PushLiteral(envPtr, "", 0);
	}
    }

    /*
     * Fix the unconditional jumps to the end of the "if" command.
     */

    for (j = jumpEndFixupArray.next;  j > 0;  j--) {
	jumpIndex = (j - 1);	/* i.e. process the closest jump first. */
	if (TclFixupForwardJumpToHere(envPtr,
		jumpEndFixupArray.fixup+jumpIndex, 127)) {
	    /*
	     * Adjust the immediately preceeding "ifFalse" jump. We moved it's
	     * target (just after this jump) down three bytes.
	     */

	    unsigned char *ifFalsePc = envPtr->codeStart
		    + jumpFalseFixupArray.fixup[jumpIndex].codeOffset;
	    unsigned char opCode = *ifFalsePc;

	    if (opCode == INST_JUMP_FALSE1) {
		jumpFalseDist = TclGetInt1AtPtr(ifFalsePc + 1);
		jumpFalseDist += 3;
		TclStoreInt1AtPtr(jumpFalseDist, (ifFalsePc + 1));
	    } else if (opCode == INST_JUMP_FALSE4) {
		jumpFalseDist = TclGetInt4AtPtr(ifFalsePc + 1);
		jumpFalseDist += 3;
		TclStoreInt4AtPtr(jumpFalseDist, (ifFalsePc + 1));
	    } else {
		Tcl_Panic("TclCompileIfCmd: unexpected opcode \"%d\" updating ifFalse jump", (int) opCode);
	    }
	}
    }

    /*
     * Free the jumpFixupArray array if malloc'ed storage was used.
     */

  done:
    envPtr->currStackDepth = savedStackDepth + 1;
    TclFreeJumpFixupArray(&jumpFalseFixupArray);
    TclFreeJumpFixupArray(&jumpEndFixupArray);
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileIncrCmd --
 *
 *	Procedure called to compile the "incr" command.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "incr" command at
 *	runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileIncrCmd(
    Tcl_Interp *interp,		/* Used for error reporting. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    Tcl_Token *varTokenPtr, *incrTokenPtr;
    int simpleVarName, isScalar, localIndex, haveImmValue, immValue;
    DefineLineInformation;	/* TIP #280 */

    if ((parsePtr->numWords != 2) && (parsePtr->numWords != 3)) {
	return TCL_ERROR;
    }

    varTokenPtr = TokenAfter(parsePtr->tokenPtr);

    PushVarNameWord(interp, varTokenPtr, envPtr, TCL_NO_LARGE_INDEX,
	    &localIndex, &simpleVarName, &isScalar, 1);

    /*
     * If an increment is given, push it, but see first if it's a small
     * integer.
     */

    haveImmValue = 0;
    immValue = 1;
    if (parsePtr->numWords == 3) {
	incrTokenPtr = TokenAfter(varTokenPtr);
	if (incrTokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
	    const char *word = incrTokenPtr[1].start;
	    int numBytes = incrTokenPtr[1].size;
	    int code;
	    Tcl_Obj *intObj = Tcl_NewStringObj(word, numBytes);

	    Tcl_IncrRefCount(intObj);
	    code = TclGetIntFromObj(NULL, intObj, &immValue);
	    TclDecrRefCount(intObj);
	    if ((code == TCL_OK) && (-127 <= immValue) && (immValue <= 127)) {
		haveImmValue = 1;
	    }
	    if (!haveImmValue) {
		PushLiteral(envPtr, word, numBytes);
	    }
	} else {
	    SetLineInformation(2);
	    CompileTokens(envPtr, incrTokenPtr, interp);
	}
    } else {			/* No incr amount given so use 1. */
	haveImmValue = 1;
    }

    /*
     * Emit the instruction to increment the variable.
     */

    if (simpleVarName) {
	if (isScalar) {
	    if (localIndex >= 0) {
		if (haveImmValue) {
		    TclEmitInstInt1(INST_INCR_SCALAR1_IMM, localIndex, envPtr);
		    TclEmitInt1(immValue, envPtr);
		} else {
		    TclEmitInstInt1(INST_INCR_SCALAR1, localIndex, envPtr);
		}
	    } else {
		if (haveImmValue) {
		    TclEmitInstInt1(INST_INCR_SCALAR_STK_IMM, immValue, envPtr);
		} else {
		    TclEmitOpcode(INST_INCR_SCALAR_STK, envPtr);
		}
	    }
	} else {
	    if (localIndex >= 0) {
		if (haveImmValue) {
		    TclEmitInstInt1(INST_INCR_ARRAY1_IMM, localIndex, envPtr);
		    TclEmitInt1(immValue, envPtr);
		} else {
		    TclEmitInstInt1(INST_INCR_ARRAY1, localIndex, envPtr);
		}
	    } else {
		if (haveImmValue) {
		    TclEmitInstInt1(INST_INCR_ARRAY_STK_IMM, immValue, envPtr);
		} else {
		    TclEmitOpcode(INST_INCR_ARRAY_STK, envPtr);
		}
	    }
	}
    } else {			/* Non-simple variable name. */
	if (haveImmValue) {
	    TclEmitInstInt1(INST_INCR_STK_IMM, immValue, envPtr);
	} else {
	    TclEmitOpcode(INST_INCR_STK, envPtr);
	}
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileInfoExistsCmd --
 *
 *	Procedure called to compile the "info exists" subcommand.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "info exists"
 *	subcommand at runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileInfoExistsCmd(
    Tcl_Interp *interp,		/* Used for error reporting. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    Tcl_Token *tokenPtr;
    int isScalar, simpleVarName, localIndex;
    DefineLineInformation;	/* TIP #280 */

    if (parsePtr->numWords != 2) {
	return TCL_ERROR;
    }

    /*
     * Decide if we can use a frame slot for the var/array name or if we need
     * to emit code to compute and push the name at runtime. We use a frame
     * slot (entry in the array of local vars) if we are compiling a procedure
     * body and if the name is simple text that does not include namespace
     * qualifiers.
     */

    tokenPtr = TokenAfter(parsePtr->tokenPtr);
    PushVarNameWord(interp, tokenPtr, envPtr, 0, &localIndex,
	    &simpleVarName, &isScalar, 1);

    /*
     * Emit instruction to check the variable for existence.
     */

    if (simpleVarName) {
	if (isScalar) {
	    if (localIndex < 0) {
		TclEmitOpcode(INST_EXIST_STK, envPtr);
	    } else {
		TclEmitInstInt4(INST_EXIST_SCALAR, localIndex, envPtr);
	    }
	} else {
	    if (localIndex < 0) {
		TclEmitOpcode(INST_EXIST_ARRAY_STK, envPtr);
	    } else {
		TclEmitInstInt4(INST_EXIST_ARRAY, localIndex, envPtr);
	    }
	}
    } else {
	TclEmitOpcode(INST_EXIST_STK, envPtr);
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileLappendCmd --
 *
 *	Procedure called to compile the "lappend" command.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "lappend" command at
 *	runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileLappendCmd(
    Tcl_Interp *interp,		/* Used for error reporting. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    Tcl_Token *varTokenPtr;
    int simpleVarName, isScalar, localIndex, numWords;
    DefineLineInformation;	/* TIP #280 */

    /*
     * If we're not in a procedure, don't compile.
     */

    if (envPtr->procPtr == NULL) {
	return TCL_ERROR;
    }

    numWords = parsePtr->numWords;
    if (numWords == 1) {
	return TCL_ERROR;
    }
    if (numWords != 3) {
	/*
	 * LAPPEND instructions currently only handle one value appends.
	 */

	return TCL_ERROR;
    }

    /*
     * Decide if we can use a frame slot for the var/array name or if we
     * need to emit code to compute and push the name at runtime. We use a
     * frame slot (entry in the array of local vars) if we are compiling a
     * procedure body and if the name is simple text that does not include
     * namespace qualifiers.
     */

    varTokenPtr = TokenAfter(parsePtr->tokenPtr);

    PushVarNameWord(interp, varTokenPtr, envPtr, 0,
	    &localIndex, &simpleVarName, &isScalar, 1);

    /*
     * If we are doing an assignment, push the new value. In the no values
     * case, create an empty object.
     */

    if (numWords > 2) {
	Tcl_Token *valueTokenPtr = TokenAfter(varTokenPtr);

	CompileWord(envPtr, valueTokenPtr, interp, 2);
    }

    /*
     * Emit instructions to set/get the variable.
     */

    /*
     * The *_STK opcodes should be refactored to make better use of existing
     * LOAD/STORE instructions.
     */

    if (simpleVarName) {
	if (isScalar) {
	    if (localIndex < 0) {
		TclEmitOpcode(INST_LAPPEND_STK, envPtr);
	    } else if (localIndex <= 255) {
		TclEmitInstInt1(INST_LAPPEND_SCALAR1, localIndex, envPtr);
	    } else {
		TclEmitInstInt4(INST_LAPPEND_SCALAR4, localIndex, envPtr);
	    }
	} else {
	    if (localIndex < 0) {
		TclEmitOpcode(INST_LAPPEND_ARRAY_STK, envPtr);
	    } else if (localIndex <= 255) {
		TclEmitInstInt1(INST_LAPPEND_ARRAY1, localIndex, envPtr);
	    } else {
		TclEmitInstInt4(INST_LAPPEND_ARRAY4, localIndex, envPtr);
	    }
	}
    } else {
	TclEmitOpcode(INST_LAPPEND_STK, envPtr);
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileLassignCmd --
 *
 *	Procedure called to compile the "lassign" command.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "lassign" command at
 *	runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileLassignCmd(
    Tcl_Interp *interp,		/* Used for error reporting. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    Tcl_Token *tokenPtr;
    int simpleVarName, isScalar, localIndex, numWords, idx;
    DefineLineInformation;	/* TIP #280 */

    numWords = parsePtr->numWords;

    /*
     * Check for command syntax error, but we'll punt that to runtime.
     */

    if (numWords < 3) {
	return TCL_ERROR;
    }

    /*
     * Generate code to push list being taken apart by [lassign].
     */

    tokenPtr = TokenAfter(parsePtr->tokenPtr);
    CompileWord(envPtr, tokenPtr, interp, 1);

    /*
     * Generate code to assign values from the list to variables.
     */

    for (idx=0 ; idx<numWords-2 ; idx++) {
	tokenPtr = TokenAfter(tokenPtr);

	/*
	 * Generate the next variable name.
	 */

	PushVarNameWord(interp, tokenPtr, envPtr, 0, &localIndex,
		&simpleVarName, &isScalar, idx+2);

	/*
	 * Emit instructions to get the idx'th item out of the list value on
	 * the stack and assign it to the variable.
	 */

	if (simpleVarName) {
	    if (isScalar) {
		if (localIndex >= 0) {
		    TclEmitOpcode(INST_DUP, envPtr);
		    TclEmitInstInt4(INST_LIST_INDEX_IMM, idx, envPtr);
		    if (localIndex <= 255) {
			TclEmitInstInt1(INST_STORE_SCALAR1,localIndex,envPtr);
		    } else {
			TclEmitInstInt4(INST_STORE_SCALAR4,localIndex,envPtr);
		    }
		} else {
		    TclEmitInstInt4(INST_OVER, 1, envPtr);
		    TclEmitInstInt4(INST_LIST_INDEX_IMM, idx, envPtr);
		    TclEmitOpcode(INST_STORE_SCALAR_STK, envPtr);
		}
	    } else {
		if (localIndex >= 0) {
		    TclEmitInstInt4(INST_OVER, 1, envPtr);
		    TclEmitInstInt4(INST_LIST_INDEX_IMM, idx, envPtr);
		    if (localIndex <= 255) {
			TclEmitInstInt1(INST_STORE_ARRAY1, localIndex, envPtr);
		    } else {
			TclEmitInstInt4(INST_STORE_ARRAY4, localIndex, envPtr);
		    }
		} else {
		    TclEmitInstInt4(INST_OVER, 2, envPtr);
		    TclEmitInstInt4(INST_LIST_INDEX_IMM, idx, envPtr);
		    TclEmitOpcode(INST_STORE_ARRAY_STK, envPtr);
		}
	    }
	} else {
	    TclEmitInstInt4(INST_OVER, 1, envPtr);
	    TclEmitInstInt4(INST_LIST_INDEX_IMM, idx, envPtr);
	    TclEmitOpcode(INST_STORE_STK, envPtr);
	}
	TclEmitOpcode(INST_POP, envPtr);
    }

    /*
     * Generate code to leave the rest of the list on the stack.
     */

    TclEmitInstInt4(INST_LIST_RANGE_IMM, idx, envPtr);
    TclEmitInt4(-2, envPtr);	/* -2 == "end" */

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileLindexCmd --
 *
 *	Procedure called to compile the "lindex" command.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "lindex" command at
 *	runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileLindexCmd(
    Tcl_Interp *interp,		/* Used for error reporting. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    Tcl_Token *idxTokenPtr, *valTokenPtr;
    int i, numWords = parsePtr->numWords;
    DefineLineInformation;	/* TIP #280 */

    /*
     * Quit if too few args.
     */

    if (numWords <= 1) {
	return TCL_ERROR;
    }

    valTokenPtr = TokenAfter(parsePtr->tokenPtr);
    if (numWords != 3) {
	goto emitComplexLindex;
    }

    idxTokenPtr = TokenAfter(valTokenPtr);
    if (idxTokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
	Tcl_Obj *tmpObj;
	int idx, result;

	tmpObj = Tcl_NewStringObj(idxTokenPtr[1].start, idxTokenPtr[1].size);
	result = TclGetIntFromObj(NULL, tmpObj, &idx);
	TclDecrRefCount(tmpObj);

	if (result == TCL_OK && idx >= 0) {
	    /*
	     * All checks have been completed, and we have exactly this
	     * construct:
	     *	 lindex <arbitraryValue> <posInt>
	     * This is best compiled as a push of the arbitrary value followed
	     * by an "immediate lindex" which is the most efficient variety.
	     */

	    CompileWord(envPtr, valTokenPtr, interp, 1);
	    TclEmitInstInt4(INST_LIST_INDEX_IMM, idx, envPtr);
	    return TCL_OK;
	}

	/*
	 * If the conversion failed or the value was negative, we just keep on
	 * going with the more complex compilation.
	 */
    }

    /*
     * Push the operands onto the stack.
     */

  emitComplexLindex:
    for (i=1 ; i<numWords ; i++) {
	CompileWord(envPtr, valTokenPtr, interp, i);
	valTokenPtr = TokenAfter(valTokenPtr);
    }

    /*
     * Emit INST_LIST_INDEX if objc==3, or INST_LIST_INDEX_MULTI if there are
     * multiple index args.
     */

    if (numWords == 3) {
	TclEmitOpcode(INST_LIST_INDEX, envPtr);
    } else {
	TclEmitInstInt4(INST_LIST_INDEX_MULTI, numWords-1, envPtr);
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileListCmd --
 *
 *	Procedure called to compile the "list" command.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "list" command at
 *	runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileListCmd(
    Tcl_Interp *interp,		/* Used for error reporting. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    DefineLineInformation;	/* TIP #280 */

    /*
     * If we're not in a procedure, don't compile.
     */

    if (envPtr->procPtr == NULL) {
	return TCL_ERROR;
    }

    if (parsePtr->numWords == 1) {
	/*
	 * [list] without arguments just pushes an empty object.
	 */

	PushLiteral(envPtr, "", 0);
    } else {
	/*
	 * Push the all values onto the stack.
	 */

	Tcl_Token *valueTokenPtr;
	int i, numWords;

	numWords = parsePtr->numWords;

	valueTokenPtr = TokenAfter(parsePtr->tokenPtr);
	for (i = 1; i < numWords; i++) {
	    CompileWord(envPtr, valueTokenPtr, interp, i);
	    valueTokenPtr = TokenAfter(valueTokenPtr);
	}
	TclEmitInstInt4(INST_LIST, numWords - 1, envPtr);
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileLlengthCmd --
 *
 *	Procedure called to compile the "llength" command.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "llength" command at
 *	runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileLlengthCmd(
    Tcl_Interp *interp,		/* Used for error reporting. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    Tcl_Token *varTokenPtr;
    DefineLineInformation;	/* TIP #280 */

    if (parsePtr->numWords != 2) {
	return TCL_ERROR;
    }
    varTokenPtr = TokenAfter(parsePtr->tokenPtr);

    CompileWord(envPtr, varTokenPtr, interp, 1);
    TclEmitOpcode(INST_LIST_LENGTH, envPtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileLsetCmd --
 *
 *	Procedure called to compile the "lset" command.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "lset" command at
 *	runtime.
 *
 * The general template for execution of the "lset" command is:
 *	(1) Instructions to push the variable name, unless the variable is
 *	    local to the stack frame.
 *	(2) If the variable is an array element, instructions to push the
 *	    array element name.
 *	(3) Instructions to push each of zero or more "index" arguments to the
 *	    stack, followed with the "newValue" element.
 *	(4) Instructions to duplicate the variable name and/or array element
 *	    name onto the top of the stack, if either was pushed at steps (1)
 *	    and (2).
 *	(5) The appropriate INST_LOAD_* instruction to place the original
 *	    value of the list variable at top of stack.
 *	(6) At this point, the stack contains:
 *		varName? arrayElementName? index1 index2 ... newValue oldList
 *	    The compiler emits one of INST_LSET_FLAT or INST_LSET_LIST
 *	    according as whether there is exactly one index element (LIST) or
 *	    either zero or else two or more (FLAT). This instruction removes
 *	    everything from the stack except for the two names and pushes the
 *	    new value of the variable.
 *	(7) Finally, INST_STORE_* stores the new value in the variable and
 *	    cleans up the stack.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileLsetCmd(
    Tcl_Interp *interp,		/* Tcl interpreter for error reporting. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the
				 * command. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds the resulting instructions. */
{
    int tempDepth;		/* Depth used for emitting one part of the
				 * code burst. */
    Tcl_Token *varTokenPtr;	/* Pointer to the Tcl_Token representing the
				 * parse of the variable name. */
    int localIndex;		/* Index of var in local var table. */
    int simpleVarName;		/* Flag == 1 if var name is simple. */
    int isScalar;		/* Flag == 1 if scalar, 0 if array. */
    int i;
    DefineLineInformation;	/* TIP #280 */

    /*
     * Check argument count.
     */

    if (parsePtr->numWords < 3) {
	/*
	 * Fail at run time, not in compilation.
	 */

	return TCL_ERROR;
    }

    /*
     * Decide if we can use a frame slot for the var/array name or if we need
     * to emit code to compute and push the name at runtime. We use a frame
     * slot (entry in the array of local vars) if we are compiling a procedure
     * body and if the name is simple text that does not include namespace
     * qualifiers.
     */

    varTokenPtr = TokenAfter(parsePtr->tokenPtr);
    PushVarNameWord(interp, varTokenPtr, envPtr, 0,
	    &localIndex, &simpleVarName, &isScalar, 1);

    /*
     * Push the "index" args and the new element value.
     */

    for (i=2 ; i<parsePtr->numWords ; ++i) {
	varTokenPtr = TokenAfter(varTokenPtr);
	CompileWord(envPtr, varTokenPtr, interp, i);
    }

    /*
     * Duplicate the variable name if it's been pushed.
     */

    if (!simpleVarName || localIndex < 0) {
	if (!simpleVarName || isScalar) {
	    tempDepth = parsePtr->numWords - 2;
	} else {
	    tempDepth = parsePtr->numWords - 1;
	}
	TclEmitInstInt4(INST_OVER, tempDepth, envPtr);
    }

    /*
     * Duplicate an array index if one's been pushed.
     */

    if (simpleVarName && !isScalar) {
	if (localIndex < 0) {
	    tempDepth = parsePtr->numWords - 1;
	} else {
	    tempDepth = parsePtr->numWords - 2;
	}
	TclEmitInstInt4(INST_OVER, tempDepth, envPtr);
    }

    /*
     * Emit code to load the variable's value.
     */

    if (!simpleVarName) {
	TclEmitOpcode(INST_LOAD_STK, envPtr);
    } else if (isScalar) {
	if (localIndex < 0) {
	    TclEmitOpcode(INST_LOAD_SCALAR_STK, envPtr);
	} else if (localIndex < 0x100) {
	    TclEmitInstInt1(INST_LOAD_SCALAR1, localIndex, envPtr);
	} else {
	    TclEmitInstInt4(INST_LOAD_SCALAR4, localIndex, envPtr);
	}
    } else {
	if (localIndex < 0) {
	    TclEmitOpcode(INST_LOAD_ARRAY_STK, envPtr);
	} else if (localIndex < 0x100) {
	    TclEmitInstInt1(INST_LOAD_ARRAY1, localIndex, envPtr);
	} else {
	    TclEmitInstInt4(INST_LOAD_ARRAY4, localIndex, envPtr);
	}
    }

    /*
     * Emit the correct variety of 'lset' instruction.
     */

    if (parsePtr->numWords == 4) {
	TclEmitOpcode(INST_LSET_LIST, envPtr);
    } else {
	TclEmitInstInt4(INST_LSET_FLAT, parsePtr->numWords-1, envPtr);
    }

    /*
     * Emit code to put the value back in the variable.
     */

    if (!simpleVarName) {
	TclEmitOpcode(INST_STORE_STK, envPtr);
    } else if (isScalar) {
	if (localIndex < 0) {
	    TclEmitOpcode(INST_STORE_SCALAR_STK, envPtr);
	} else if (localIndex < 0x100) {
	    TclEmitInstInt1(INST_STORE_SCALAR1, localIndex, envPtr);
	} else {
	    TclEmitInstInt4(INST_STORE_SCALAR4, localIndex, envPtr);
	}
    } else {
	if (localIndex < 0) {
	    TclEmitOpcode(INST_STORE_ARRAY_STK, envPtr);
	} else if (localIndex < 0x100) {
	    TclEmitInstInt1(INST_STORE_ARRAY1, localIndex, envPtr);
	} else {
	    TclEmitInstInt4(INST_STORE_ARRAY4, localIndex, envPtr);
	}
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileNamespaceCmd --
 *
 *	Procedure called to compile the "namespace" command; currently, only
 *	the subcommand "namespace upvar" is compiled to bytecodes.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "namespace upvar"
 *	command at runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileNamespaceCmd(
    Tcl_Interp *interp,		/* Used for error reporting. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    Tcl_Token *tokenPtr, *otherTokenPtr, *localTokenPtr;
    int simpleVarName, isScalar, localIndex, numWords, i;
    DefineLineInformation;	/* TIP #280 */

    if (envPtr->procPtr == NULL) {
	return TCL_ERROR;
    }

    /*
     * Only compile [namespace upvar ...]: needs an odd number of args, >=5
     */

    numWords = parsePtr->numWords;
    if (!(numWords%2) || (numWords < 5)) {
	return TCL_ERROR;
    }

    /*
     * Check if the second argument is "upvar"
     */

    tokenPtr = TokenAfter(parsePtr->tokenPtr);
    if ((tokenPtr->size != 5)  /* 5 == strlen("upvar") */
	    || strncmp(tokenPtr->start, "upvar", 5)) {
	return TCL_ERROR;
    }

    /*
     * Push the namespace
     */

    tokenPtr = TokenAfter(tokenPtr);
    CompileWord(envPtr, tokenPtr, interp, 1);

    /*
     * Loop over the (otherVar, thisVar) pairs. If any of the thisVar is not a
     * local variable, return an error so that the non-compiled command will
     * be called at runtime.
     */

    localTokenPtr = tokenPtr;
    for (i=4; i<=numWords; i+=2) {
	otherTokenPtr = TokenAfter(localTokenPtr);
	localTokenPtr = TokenAfter(otherTokenPtr);

	CompileWord(envPtr, otherTokenPtr, interp, 1);
	PushVarNameWord(interp, localTokenPtr, envPtr, 0,
		&localIndex, &simpleVarName, &isScalar, 1);

	if ((localIndex < 0) || !isScalar) {
	    return TCL_ERROR;
	}
	TclEmitInstInt4(INST_NSUPVAR, localIndex, envPtr);
    }

    /*
     * Pop the namespace, and set the result to empty
     */

    TclEmitOpcode(INST_POP, envPtr);
    PushLiteral(envPtr, "", 0);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileRegexpCmd --
 *
 *	Procedure called to compile the "regexp" command.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "regexp" command at
 *	runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileRegexpCmd(
    Tcl_Interp *interp,		/* Tcl interpreter for error reporting. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the
				 * command. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds the resulting instructions. */
{
    Tcl_Token *varTokenPtr;	/* Pointer to the Tcl_Token representing the
				 * parse of the RE or string. */
    int i, len, nocase, exact, sawLast, simple;
    const char *str;
    DefineLineInformation;	/* TIP #280 */

    /*
     * We are only interested in compiling simple regexp cases. Currently
     * supported compile cases are:
     *   regexp ?-nocase? ?--? staticString $var
     *   regexp ?-nocase? ?--? {^staticString$} $var
     */

    if (parsePtr->numWords < 3) {
	return TCL_ERROR;
    }

    simple = 0;
    nocase = 0;
    sawLast = 0;
    varTokenPtr = parsePtr->tokenPtr;

    /*
     * We only look for -nocase and -- as options. Everything else gets pushed
     * to runtime execution. This is different than regexp's runtime option
     * handling, but satisfies our stricter needs.
     */

    for (i = 1; i < parsePtr->numWords - 2; i++) {
	varTokenPtr = TokenAfter(varTokenPtr);
	if (varTokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
	    /*
	     * Not a simple string, so punt to runtime.
	     */

	    return TCL_ERROR;
	}
	str = varTokenPtr[1].start;
	len = varTokenPtr[1].size;
	if ((len == 2) && (str[0] == '-') && (str[1] == '-')) {
	    sawLast++;
	    i++;
	    break;
	} else if ((len > 1) && (strncmp(str,"-nocase",(unsigned)len) == 0)) {
	    nocase = 1;
	} else {
	    /*
	     * Not an option we recognize.
	     */

	    return TCL_ERROR;
	}
    }

    if ((parsePtr->numWords - i) != 2) {
	/*
	 * We don't support capturing to variables.
	 */

	return TCL_ERROR;
    }

    /*
     * Get the regexp string. If it is not a simple string or can't be
     * converted to a glob pattern, push the word for the INST_REGEXP.
     * Keep changes here in sync with TclCompileSwitchCmd Switch_Regexp.
     */

    varTokenPtr = TokenAfter(varTokenPtr);

    if (varTokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
	Tcl_DString ds;

	str = varTokenPtr[1].start;
	len = varTokenPtr[1].size;

	/*
	 * If it has a '-', it could be an incorrectly formed regexp command.
	 */

	if ((*str == '-') && !sawLast) {
	    return TCL_ERROR;
	}

	if (len == 0) {
	    /*
	     * The semantics of regexp are always match on re == "".
	     */

	    PushLiteral(envPtr, "1", 1);
	    return TCL_OK;
	}

	/*
	 * Attempt to convert pattern to glob.  If successful, push the
	 * converted pattern as a literal.
	 */

	if (TclReToGlob(NULL, varTokenPtr[1].start, len, &ds, &exact)
		== TCL_OK) {
	    simple = 1;
	    PushLiteral(envPtr, Tcl_DStringValue(&ds),Tcl_DStringLength(&ds));
	    Tcl_DStringFree(&ds);
	}
    }

    if (!simple) {
	CompileWord(envPtr, varTokenPtr, interp, parsePtr->numWords-2);
    }

    /*
     * Push the string arg.
     */

    varTokenPtr = TokenAfter(varTokenPtr);
    CompileWord(envPtr, varTokenPtr, interp, parsePtr->numWords-1);

    if (simple) {
	if (exact && !nocase) {
	    TclEmitOpcode(INST_STR_EQ, envPtr);
	} else {
	    TclEmitInstInt1(INST_STR_MATCH, nocase, envPtr);
	}
    } else {
	/*
	 * Pass correct RE compile flags.  We use only Int1 (8-bit), but
	 * that handles all the flags we want to pass.
	 * Don't use TCL_REG_NOSUB as we may have backrefs.
	 */

	int cflags = TCL_REG_ADVANCED | (nocase ? TCL_REG_NOCASE : 0);

	TclEmitInstInt1(INST_REGEXP, cflags, envPtr);
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileReturnCmd --
 *
 *	Procedure called to compile the "return" command.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "return" command at
 *	runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileReturnCmd(
    Tcl_Interp *interp,		/* Used for error reporting. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    /*
     * General syntax: [return ?-option value ...? ?result?]
     * An even number of words means an explicit result argument is present.
     */
    int level, code, objc, size, status = TCL_OK;
    int numWords = parsePtr->numWords;
    int explicitResult = (0 == (numWords % 2));
    int numOptionWords = numWords - 1 - explicitResult;
    Tcl_Obj *returnOpts, **objv;
    Tcl_Token *wordTokenPtr = TokenAfter(parsePtr->tokenPtr);
    DefineLineInformation;	/* TIP #280 */

    /*
     * Check for special case which can always be compiled:
     *	    return -options <opts> <msg>
     * Unlike the normal [return] compilation, this version does everything at
     * runtime so it can handle arbitrary words and not just literals. Note
     * that if INST_RETURN_STK wasn't already needed for something else
     * ('finally' clause processing) this piece of code would not be present.
     */

    if ((numWords == 4) && (wordTokenPtr->type == TCL_TOKEN_SIMPLE_WORD)
	    && (wordTokenPtr[1].size == 8)
	    && (strncmp(wordTokenPtr[1].start, "-options", 8) == 0)) {
	Tcl_Token *optsTokenPtr = TokenAfter(wordTokenPtr);
	Tcl_Token *msgTokenPtr = TokenAfter(optsTokenPtr);

	CompileWord(envPtr, optsTokenPtr, interp, 2);
	CompileWord(envPtr, msgTokenPtr,  interp, 3);
	TclEmitOpcode(INST_RETURN_STK, envPtr);
	return TCL_OK;
    }

    /*
     * Allocate some working space.
     */

    objv = TclStackAlloc(interp, numOptionWords * sizeof(Tcl_Obj *));

    /*
     * Scan through the return options. If any are unknown at compile time,
     * there is no value in bytecompiling. Save the option values known in an
     * objv array for merging into a return options dictionary.
     */

    for (objc = 0; objc < numOptionWords; objc++) {
	objv[objc] = Tcl_NewObj();
	Tcl_IncrRefCount(objv[objc]);
	if (!TclWordKnownAtCompileTime(wordTokenPtr, objv[objc])) {
	    objc++;
	    status = TCL_ERROR;
	    goto cleanup;
	}
	wordTokenPtr = TokenAfter(wordTokenPtr);
    }
    status = TclMergeReturnOptions(interp, objc, objv,
	    &returnOpts, &code, &level);
  cleanup:
    while (--objc >= 0) {
	TclDecrRefCount(objv[objc]);
    }
    TclStackFree(interp, objv);
    if (TCL_ERROR == status) {
	/*
	 * Something was bogus in the return options. Clear the error message,
	 * and report back to the compiler that this must be interpreted at
	 * runtime.
	 */

	Tcl_ResetResult(interp);
	return TCL_ERROR;
    }

    /*
     * All options are known at compile time, so we're going to bytecompile.
     * Emit instructions to push the result on the stack.
     */

    if (explicitResult) {
	 CompileWord(envPtr, wordTokenPtr, interp, numWords-1);
    } else {
	/*
	 * No explict result argument, so default result is empty string.
	 */

	PushLiteral(envPtr, "", 0);
    }

    /*
     * Check for optimization: When [return] is in a proc, and there's no
     * enclosing [catch], and there are no return options, then the INST_DONE
     * instruction is equivalent, and may be more efficient.
     */

    if (numOptionWords == 0 && envPtr->procPtr != NULL) {
	/*
	 * We have default return options and we're in a proc ...
	 */

	int index = envPtr->exceptArrayNext - 1;
	int enclosingCatch = 0;

	while (index >= 0) {
	    ExceptionRange range = envPtr->exceptArrayPtr[index];

	    if ((range.type == CATCH_EXCEPTION_RANGE)
		    && (range.catchOffset == -1)) {
		enclosingCatch = 1;
		break;
	    }
	    index--;
	}
	if (!enclosingCatch) {
	    /*
	     * ... and there is no enclosing catch. Issue the maximally
	     * efficient exit instruction.
	     */

	    Tcl_DecrRefCount(returnOpts);
	    TclEmitOpcode(INST_DONE, envPtr);
	    return TCL_OK;
	}
    }

    /* Optimize [return -level 0 $x]. */
    Tcl_DictObjSize(NULL, returnOpts, &size);
    if (size == 0 && level == 0 && code == TCL_OK) {
	Tcl_DecrRefCount(returnOpts);
	return TCL_OK;
    }

    /*
     * Could not use the optimization, so we push the return options dict, and
     * emit the INST_RETURN_IMM instruction with code and level as operands.
     */

    CompileReturnInternal(envPtr, INST_RETURN_IMM, code, level, returnOpts);
    return TCL_OK;
}

static void
CompileReturnInternal(
    CompileEnv *envPtr,
    unsigned char op,
    int code,
    int level,
    Tcl_Obj *returnOpts)
{
    TclEmitPush(TclAddLiteralObj(envPtr, returnOpts, NULL), envPtr);
    TclEmitInstInt4(op, code, envPtr);
    TclEmitInt4(level, envPtr);
}

void
TclCompileSyntaxError(
    Tcl_Interp *interp,
    CompileEnv *envPtr)
{
    Tcl_Obj *msg = Tcl_GetObjResult(interp);
    int numBytes;
    const char *bytes = TclGetStringFromObj(msg, &numBytes);

    TclEmitPush(TclRegisterNewLiteral(envPtr, bytes, numBytes), envPtr);
    CompileReturnInternal(envPtr, INST_SYNTAX, TCL_ERROR, 0,
	    Tcl_GetReturnOptions(interp, TCL_ERROR));
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileUpvarCmd --
 *
 *	Procedure called to compile the "upvar" command.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "upvar" command at
 *	runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileUpvarCmd(
    Tcl_Interp *interp,		/* Used for error reporting. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    Tcl_Token *tokenPtr, *otherTokenPtr, *localTokenPtr;
    int simpleVarName, isScalar, localIndex, numWords, i;
    DefineLineInformation;	/* TIP #280 */
    Tcl_Obj *objPtr = Tcl_NewObj();

    if (envPtr->procPtr == NULL) {
	Tcl_DecrRefCount(objPtr);
	return TCL_ERROR;
    }

    numWords = parsePtr->numWords;
    if (numWords < 3) {
	Tcl_DecrRefCount(objPtr);
	return TCL_ERROR;
    }

    /*
     * Push the frame index if it is known at compile time
     */

    tokenPtr = TokenAfter(parsePtr->tokenPtr);
    if (TclWordKnownAtCompileTime(tokenPtr, objPtr)) {
	CallFrame *framePtr;
	const Tcl_ObjType *newTypePtr, *typePtr = objPtr->typePtr;

	/*
	 * Attempt to convert to a level reference. Note that TclObjGetFrame
	 * only changes the obj type when a conversion was successful.
	 */

	TclObjGetFrame(interp, objPtr, &framePtr);
	newTypePtr = objPtr->typePtr;
	Tcl_DecrRefCount(objPtr);

	if (newTypePtr != typePtr) {
	    if (numWords%2) {
		return TCL_ERROR;
	    }
	    CompileWord(envPtr, tokenPtr, interp, 1);
	    otherTokenPtr = TokenAfter(tokenPtr);
	    i = 4;
	} else {
	    if (!(numWords%2)) {
		return TCL_ERROR;
	    }
	    PushLiteral(envPtr, "1", 1);
	    otherTokenPtr = tokenPtr;
	    i = 3;
	}
    } else {
	Tcl_DecrRefCount(objPtr);
	return TCL_ERROR;
    }

    /*
     * Loop over the (otherVar, thisVar) pairs. If any of the thisVar is not a
     * local variable, return an error so that the non-compiled command will
     * be called at runtime.
     */

    for (; i<=numWords; i+=2, otherTokenPtr = TokenAfter(localTokenPtr)) {
	localTokenPtr = TokenAfter(otherTokenPtr);

	CompileWord(envPtr, otherTokenPtr, interp, 1);
	PushVarNameWord(interp, localTokenPtr, envPtr, 0,
		&localIndex, &simpleVarName, &isScalar, 1);

	if ((localIndex < 0) || !isScalar) {
	    return TCL_ERROR;
	}
	TclEmitInstInt4(INST_UPVAR, localIndex, envPtr);
    }

    /*
     * Pop the frame index, and set the result to empty
     */

    TclEmitOpcode(INST_POP, envPtr);
    PushLiteral(envPtr, "", 0);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileVariableCmd --
 *
 *	Procedure called to compile the "variable" command.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "variable" command at
 *	runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileVariableCmd(
    Tcl_Interp *interp,		/* Used for error reporting. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    Tcl_Token *varTokenPtr, *valueTokenPtr;
    int localIndex, numWords, i;
    DefineLineInformation;	/* TIP #280 */

    numWords = parsePtr->numWords;
    if (numWords < 2) {
	return TCL_ERROR;
    }

    /*
     * Bail out if not compiling a proc body
     */

    if (envPtr->procPtr == NULL) {
	return TCL_ERROR;
    }

    /*
     * Loop over the (var, value) pairs.
     */

    valueTokenPtr = parsePtr->tokenPtr;
    for (i=2; i<=numWords; i+=2) {
	varTokenPtr = TokenAfter(valueTokenPtr);
	valueTokenPtr = TokenAfter(varTokenPtr);

	localIndex = IndexTailVarIfKnown(interp, varTokenPtr, envPtr);

	if (localIndex < 0) {
	    return TCL_ERROR;
	}

	CompileWord(envPtr, varTokenPtr, interp, 1);
	TclEmitInstInt4(INST_VARIABLE, localIndex, envPtr);

	if (i != numWords) {
	    /*
	     * A value has been given: set the variable, pop the value
	     */

	    CompileWord(envPtr, valueTokenPtr, interp, 1);
	    if (localIndex < 0x100) {
		TclEmitInstInt1(INST_STORE_SCALAR1, localIndex, envPtr);
	    } else {
		TclEmitInstInt4(INST_STORE_SCALAR4, localIndex, envPtr);
	    }
	    TclEmitOpcode(INST_POP, envPtr);
	}
    }

    /*
     * Set the result to empty
     */

    PushLiteral(envPtr, "", 0);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * IndexTailVarIfKnown --
 *
 *	Procedure used in compiling [global] and [variable] commands. It
 *	inspects the variable name described by varTokenPtr and, if the tail
 *	is known at compile time, defines a corresponding local variable.
 *
 * Results:
 *	Returns the variable's index in the table of compiled locals if the
 *	tail is known at compile time, or -1 otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
IndexTailVarIfKnown(
    Tcl_Interp *interp,
    Tcl_Token *varTokenPtr,	/* Token representing the variable name */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    Tcl_Obj *tailPtr;
    const char *tailName, *p;
    int len, n = varTokenPtr->numComponents;
    Tcl_Token *lastTokenPtr;
    int full, localIndex;

    /*
     * Determine if the tail is (a) known at compile time, and (b) not an
     * array element. Should any of these fail, return an error so that the
     * non-compiled command will be called at runtime.
     *
     * In order for the tail to be known at compile time, the last token in
     * the word has to be constant and contain "::" if it is not the only one.
     */

    if (!EnvHasLVT(envPtr)) {
	return -1;
    }

    TclNewObj(tailPtr);
    if (TclWordKnownAtCompileTime(varTokenPtr, tailPtr)) {
	full = 1;
	lastTokenPtr = varTokenPtr;
    } else {
	full = 0;
	lastTokenPtr = varTokenPtr + n;
	if (!TclWordKnownAtCompileTime(lastTokenPtr, tailPtr)) {
	    Tcl_DecrRefCount(tailPtr);
	    return -1;
	}
    }

    tailName = TclGetStringFromObj(tailPtr, &len);

    if (len) {
	if (*(tailName+len-1) == ')') {
	    /*
	     * Possible array: bail out
	     */

	    Tcl_DecrRefCount(tailPtr);
	    return -1;
	}

	/*
	 * Get the tail: immediately after the last '::'
	 */

	for (p = tailName + len -1; p > tailName; p--) {
	    if ((*p == ':') && (*(p-1) == ':')) {
		p++;
		break;
	    }
	}
	if (!full && (p == tailName)) {
	    /*
	     * No :: in the last component.
	     */

	    Tcl_DecrRefCount(tailPtr);
	    return -1;
	}
	len -= p - tailName;
	tailName = p;
    }

    localIndex = TclFindCompiledLocal(tailName, len, 1, envPtr);
    Tcl_DecrRefCount(tailPtr);
    return localIndex;
}

/*
 *----------------------------------------------------------------------
 *
 * PushVarName --
 *
 *	Procedure used in the compiling where pushing a variable name is
 *	necessary (append, lappend, set).
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "set" command at
 *	runtime.
 *
 *----------------------------------------------------------------------
 */

static int
PushVarName(
    Tcl_Interp *interp,		/* Used for error reporting. */
    Tcl_Token *varTokenPtr,	/* Points to a variable token. */
    CompileEnv *envPtr,		/* Holds resulting instructions. */
    int flags,			/* TCL_NO_LARGE_INDEX. */
    int *localIndexPtr,		/* Must not be NULL. */
    int *simpleVarNamePtr,	/* Must not be NULL. */
    int *isScalarPtr,		/* Must not be NULL. */
    int line,			/* Line the token starts on. */
    int *clNext)		/* Reference to offset of next hidden cont.
				 * line. */
{
    register const char *p;
    const char *name, *elName;
    register int i, n;
    Tcl_Token *elemTokenPtr = NULL;
    int nameChars, elNameChars, simpleVarName, localIndex;
    int elemTokenCount = 0, allocedTokens = 0, removedParen = 0;

    /*
     * Decide if we can use a frame slot for the var/array name or if we need
     * to emit code to compute and push the name at runtime. We use a frame
     * slot (entry in the array of local vars) if we are compiling a procedure
     * body and if the name is simple text that does not include namespace
     * qualifiers.
     */

    simpleVarName = 0;
    name = elName = NULL;
    nameChars = elNameChars = 0;
    localIndex = -1;

    /*
     * Check not only that the type is TCL_TOKEN_SIMPLE_WORD, but whether
     * curly braces surround the variable name. This really matters for array
     * elements to handle things like
     *    set {x($foo)} 5
     * which raises an undefined var error if we are not careful here.
     */

    if ((varTokenPtr->type == TCL_TOKEN_SIMPLE_WORD) &&
	    (varTokenPtr->start[0] != '{')) {
	/*
	 * A simple variable name. Divide it up into "name" and "elName"
	 * strings. If it is not a local variable, look it up at runtime.
	 */

	simpleVarName = 1;

	name = varTokenPtr[1].start;
	nameChars = varTokenPtr[1].size;
	if (name[nameChars-1] == ')') {
	    /*
	     * last char is ')' => potential array reference.
	     */

	    for (i=0,p=name ; i<nameChars ; i++,p++) {
		if (*p == '(') {
		    elName = p + 1;
		    elNameChars = nameChars - i - 2;
		    nameChars = i;
		    break;
		}
	    }

	    if ((elName != NULL) && elNameChars) {
		/*
		 * An array element, the element name is a simple string:
		 * assemble the corresponding token.
		 */

		elemTokenPtr = TclStackAlloc(interp, sizeof(Tcl_Token));
		allocedTokens = 1;
		elemTokenPtr->type = TCL_TOKEN_TEXT;
		elemTokenPtr->start = elName;
		elemTokenPtr->size = elNameChars;
		elemTokenPtr->numComponents = 0;
		elemTokenCount = 1;
	    }
	}
    } else if (((n = varTokenPtr->numComponents) > 1)
	    && (varTokenPtr[1].type == TCL_TOKEN_TEXT)
	    && (varTokenPtr[n].type == TCL_TOKEN_TEXT)
	    && (varTokenPtr[n].start[varTokenPtr[n].size - 1] == ')')) {
	/*
	 * Check for parentheses inside first token.
	 */

	simpleVarName = 0;
	for (i = 0, p = varTokenPtr[1].start;
		i < varTokenPtr[1].size; i++, p++) {
	    if (*p == '(') {
		simpleVarName = 1;
		break;
	    }
	}
	if (simpleVarName) {
	    int remainingChars;

	    /*
	     * Check the last token: if it is just ')', do not count it.
	     * Otherwise, remove the ')' and flag so that it is restored at
	     * the end.
	     */

	    if (varTokenPtr[n].size == 1) {
		n--;
	    } else {
		varTokenPtr[n].size--;
		removedParen = n;
	    }

	    name = varTokenPtr[1].start;
	    nameChars = p - varTokenPtr[1].start;
	    elName = p + 1;
	    remainingChars = (varTokenPtr[2].start - p) - 1;
	    elNameChars = (varTokenPtr[n].start-p) + varTokenPtr[n].size - 2;

	    if (remainingChars) {
		/*
		 * Make a first token with the extra characters in the first
		 * token.
		 */

		elemTokenPtr = TclStackAlloc(interp, n * sizeof(Tcl_Token));
		allocedTokens = 1;
		elemTokenPtr->type = TCL_TOKEN_TEXT;
		elemTokenPtr->start = elName;
		elemTokenPtr->size = remainingChars;
		elemTokenPtr->numComponents = 0;
		elemTokenCount = n;

		/*
		 * Copy the remaining tokens.
		 */

		memcpy(elemTokenPtr+1, varTokenPtr+2,
			(n-1) * sizeof(Tcl_Token));
	    } else {
		/*
		 * Use the already available tokens.
		 */

		elemTokenPtr = &varTokenPtr[2];
		elemTokenCount = n - 1;
	    }
	}
    }

    if (simpleVarName) {
	/*
	 * See whether name has any namespace separators (::'s).
	 */

	int hasNsQualifiers = 0;

	for (i = 0, p = name;  i < nameChars;  i++, p++) {
	    if ((*p == ':') && ((i+1) < nameChars) && (*(p+1) == ':')) {
		hasNsQualifiers = 1;
		break;
	    }
	}

	/*
	 * Look up the var name's index in the array of local vars in the proc
	 * frame. If retrieving the var's value and it doesn't already exist,
	 * push its name and look it up at runtime.
	 */

	if (!hasNsQualifiers) {
	    localIndex = TclFindCompiledLocal(name, nameChars,
		    1, envPtr);
	    if ((flags & TCL_NO_LARGE_INDEX) && (localIndex > 255)) {
		/*
		 * We'll push the name.
		 */

		localIndex = -1;
	    }
	}
	if (localIndex < 0) {
	    PushLiteral(envPtr, name, nameChars);
	}

	/*
	 * Compile the element script, if any.
	 */

	if (elName != NULL) {
	    if (elNameChars) {
		envPtr->line = line;
		envPtr->clNext = clNext;
		TclCompileTokens(interp, elemTokenPtr, elemTokenCount,
			envPtr);
	    } else {
		PushLiteral(envPtr, "", 0);
	    }
	}
    } else {
	/*
	 * The var name isn't simple: compile and push it.
	 */

	envPtr->line = line;
	envPtr->clNext = clNext;
	CompileTokens(envPtr, varTokenPtr, interp);
    }

    if (removedParen) {
	varTokenPtr[removedParen].size++;
    }
    if (allocedTokens) {
	TclStackFree(interp, elemTokenPtr);
    }
    *localIndexPtr = localIndex;
    *simpleVarNamePtr = simpleVarName;
    *isScalarPtr = (elName == NULL);
    return TCL_OK;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
