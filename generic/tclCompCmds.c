/* 
 * tclCompCmds.c --
 *
 *	This file contains compilation procedures that compile various
 *	Tcl commands into a sequence of instructions ("bytecodes"). 
 *
 * Copyright (c) 1997-1998 Sun Microsystems, Inc.
 * Copyright (c) 2001 by Kevin B. Kenny.  All rights reserved.
 * Copyright (c) 2002 ActiveState Corporation.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id$
 */

#include "tclInt.h"
#include "tclCompile.h"

/*
 * Prototypes for procedures defined later in this file:
 */

static ClientData	DupForeachInfo _ANSI_ARGS_((ClientData clientData));
static void		FreeForeachInfo _ANSI_ARGS_((ClientData clientData));
static int		TclPushVarName _ANSI_ARGS_((Tcl_Interp *interp,
	Tcl_Token *varTokenPtr, CompileEnv *envPtr, int flags,
	int *localIndexPtr, int *simpleVarNamePtr, int *isScalarPtr));

/*
 * Flags bits used by TclPushVarName.
 */

#define TCL_CREATE_VAR     1 /* Create a compiled local if none is found */
#define TCL_NO_LARGE_INDEX 2 /* Do not return localIndex value > 255 */

/*
 * The structures below define the AuxData types defined in this file.
 */

AuxDataType tclForeachInfoType = {
    "ForeachInfo",				/* name */
    DupForeachInfo,				/* dupProc */
    FreeForeachInfo				/* freeProc */
};

/*
 *----------------------------------------------------------------------
 *
 * TclCompileAppendCmd --
 *
 *	Procedure called to compile the "append" command.
 *
 * Results:
 *	The return value is a standard Tcl result, which is normally TCL_OK
 *	unless there was an error while parsing string. If an error occurs
 *	then the interpreter's result contains a standard error message. If
 *	complation fails because the command requires a second level of
 *	substitutions, TCL_OUT_LINE_COMPILE is returned indicating that the
 *	command should be compiled "out of line" by emitting code to
 *	invoke its command procedure (Tcl_AppendObjCmd) at runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "append" command
 *	at runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileAppendCmd(interp, parsePtr, envPtr)
    Tcl_Interp *interp;		/* Used for error reporting. */
    Tcl_Parse *parsePtr;	/* Points to a parse structure for the
				 * command created by Tcl_ParseCommand. */
    CompileEnv *envPtr;		/* Holds resulting instructions. */
{
    Tcl_Token *varTokenPtr, *valueTokenPtr;
    int simpleVarName, isScalar, localIndex, numWords;
    int code = TCL_OK;

    numWords = parsePtr->numWords;
    if (numWords == 1) {
	Tcl_ResetResult(interp);
	Tcl_AppendToObj(Tcl_GetObjResult(interp),
		"wrong # args: should be \"append varName ?value value ...?\"",
		-1);
	return TCL_ERROR;
    } else if (numWords == 2) {
	/*
	 * append varName === set varName
	 */
        return TclCompileSetCmd(interp, parsePtr, envPtr);
    } else if (numWords > 3) {
	/*
	 * APPEND instructions currently only handle one value
	 */
        return TCL_OUT_LINE_COMPILE;
    }

    /*
     * Decide if we can use a frame slot for the var/array name or if we
     * need to emit code to compute and push the name at runtime. We use a
     * frame slot (entry in the array of local vars) if we are compiling a
     * procedure body and if the name is simple text that does not include
     * namespace qualifiers. 
     */

    varTokenPtr = parsePtr->tokenPtr
	    + (parsePtr->tokenPtr->numComponents + 1);

    code = TclPushVarName(interp, varTokenPtr, envPtr, TCL_CREATE_VAR,
	    &localIndex, &simpleVarName, &isScalar);
    if (code != TCL_OK) {
	goto done;
    }

    /*
     * We are doing an assignment, otherwise TclCompileSetCmd was called,
     * so push the new value.  This will need to be extended to push a
     * value for each argument.
     */

    if (numWords > 2) {
	valueTokenPtr = varTokenPtr + (varTokenPtr->numComponents + 1);
	if (valueTokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
	    TclEmitPush(TclRegisterLiteral(envPtr, valueTokenPtr[1].start,
		    valueTokenPtr[1].size, /*onHeap*/ 0), envPtr);
	} else {
	    code = TclCompileTokens(interp, valueTokenPtr+1,
	            valueTokenPtr->numComponents, envPtr);
	    if (code != TCL_OK) {
		goto done;
	    }
	}
    }

    /*
     * Emit instructions to set/get the variable.
     */

    if (simpleVarName) {
	if (isScalar) {
	    if (localIndex >= 0) {
		if (localIndex <= 255) {
		    TclEmitInstInt1(INST_APPEND_SCALAR1, localIndex, envPtr);
		} else {
		    TclEmitInstInt4(INST_APPEND_SCALAR4, localIndex, envPtr);
		}
	    } else {
		TclEmitOpcode(INST_APPEND_STK, envPtr);
	    }
	} else {
	    if (localIndex >= 0) {
		if (localIndex <= 255) {
		    TclEmitInstInt1(INST_APPEND_ARRAY1, localIndex, envPtr);
		} else {
		    TclEmitInstInt4(INST_APPEND_ARRAY4, localIndex, envPtr);
		}
	    } else {
		TclEmitOpcode(INST_APPEND_ARRAY_STK, envPtr);
	    }
	}
    } else {
	TclEmitOpcode(INST_APPEND_STK, envPtr);
    }

    done:
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileBreakCmd --
 *
 *	Procedure called to compile the "break" command.
 *
 * Results:
 *	The return value is a standard Tcl result, which is TCL_OK unless
 *	there was an error during compilation. If an error occurs then
 *	the interpreter's result contains a standard error message.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "break" command
 *	at runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileBreakCmd(interp, parsePtr, envPtr)
    Tcl_Interp *interp;		/* Used for error reporting. */
    Tcl_Parse *parsePtr;	/* Points to a parse structure for the
				 * command created by Tcl_ParseCommand. */
    CompileEnv *envPtr;		/* Holds resulting instructions. */
{
    if (parsePtr->numWords != 1) {
	Tcl_ResetResult(interp);
	Tcl_AppendToObj(Tcl_GetObjResult(interp),
	        "wrong # args: should be \"break\"", -1);
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
 *	The return value is a standard Tcl result, which is TCL_OK if
 *	compilation was successful. If an error occurs then the
 *	interpreter's result contains a standard error message and TCL_ERROR
 *	is returned. If the command is too complex for TclCompileCatchCmd,
 *	TCL_OUT_LINE_COMPILE is returned indicating that the catch command
 *	should be compiled "out of line" by emitting code to invoke its
 *	command procedure at runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "catch" command
 *	at runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileCatchCmd(interp, parsePtr, envPtr)
    Tcl_Interp *interp;		/* Used for error reporting. */
    Tcl_Parse *parsePtr;	/* Points to a parse structure for the
				 * command created by Tcl_ParseCommand. */
    CompileEnv *envPtr;		/* Holds resulting instructions. */
{
    JumpFixup jumpFixup;
    Tcl_Token *cmdTokenPtr, *nameTokenPtr;
    char *name;
    int localIndex, nameChars, range, startOffset, jumpDist;
    int code;
    char buffer[32 + TCL_INTEGER_SPACE];
    int savedStackDepth = envPtr->currStackDepth;

    if ((parsePtr->numWords != 2) && (parsePtr->numWords != 3)) {
	Tcl_ResetResult(interp);
	Tcl_AppendToObj(Tcl_GetObjResult(interp),
	        "wrong # args: should be \"catch command ?varName?\"", -1);
	return TCL_ERROR;
    }

    /*
     * If a variable was specified and the catch command is at global level
     * (not in a procedure), don't compile it inline: the payoff is
     * too small.
     */

    if ((parsePtr->numWords == 3) && (envPtr->procPtr == NULL)) {
	return TCL_OUT_LINE_COMPILE;
    }

    /*
     * Make sure the variable name, if any, has no substitutions and just
     * refers to a local scaler.
     */

    localIndex = -1;
    cmdTokenPtr = parsePtr->tokenPtr
	    + (parsePtr->tokenPtr->numComponents + 1);
    if (parsePtr->numWords == 3) {
	nameTokenPtr = cmdTokenPtr + (cmdTokenPtr->numComponents + 1);
	if (nameTokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
	    name = nameTokenPtr[1].start;
	    nameChars = nameTokenPtr[1].size;
	    if (!TclIsLocalScalar(name, nameChars)) {
		return TCL_OUT_LINE_COMPILE;
	    }
	    localIndex = TclFindCompiledLocal(nameTokenPtr[1].start,
		    nameTokenPtr[1].size, /*create*/ 1, 
		    /*flags*/ VAR_SCALAR, envPtr->procPtr);
	} else {
	   return TCL_OUT_LINE_COMPILE;
	}
    }

    /*
     * We will compile the catch command. Emit a beginCatch instruction at
     * the start of the catch body: the subcommand it controls.
     */
    
    envPtr->exceptDepth++;
    envPtr->maxExceptDepth =
	TclMax(envPtr->exceptDepth, envPtr->maxExceptDepth);
    range = TclCreateExceptRange(CATCH_EXCEPTION_RANGE, envPtr);
    TclEmitInstInt4(INST_BEGIN_CATCH4, range, envPtr);

    /*
     * If the body is a simple word, compile the instructions to
     * eval it. Otherwise, compile instructions to substitute its
     * text without catching, a catch instruction that resets the 
     * stack to what it was before substituting the body, and then 
     * an instruction to eval the body. Care has to be taken to 
     * register the correct startOffset for the catch range so that
     * errors in the substitution are not catched [Bug 219184]
     */

    if (cmdTokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
	startOffset = (envPtr->codeNext - envPtr->codeStart);
	code = TclCompileCmdWord(interp, cmdTokenPtr+1, 1, envPtr);
    } else {
	code = TclCompileTokens(interp, cmdTokenPtr+1,
	        cmdTokenPtr->numComponents, envPtr);
	startOffset = (envPtr->codeNext - envPtr->codeStart);
	TclEmitOpcode(INST_EVAL_STK, envPtr);
    }
    envPtr->exceptArrayPtr[range].codeOffset = startOffset;

    if (code != TCL_OK) {
	if (code == TCL_ERROR) {
	    sprintf(buffer, "\n    (\"catch\" body line %d)",
		    interp->errorLine);
            Tcl_AddObjErrorInfo(interp, buffer, -1);
        }
	goto done;
    }
    envPtr->exceptArrayPtr[range].numCodeBytes =
	    (envPtr->codeNext - envPtr->codeStart) - startOffset;
		    
    /*
     * The "no errors" epilogue code: store the body's result into the
     * variable (if any), push "0" (TCL_OK) as the catch's "no error"
     * result, and jump around the "error case" code.
     */

    if (localIndex != -1) {
	if (localIndex <= 255) {
	    TclEmitInstInt1(INST_STORE_SCALAR1, localIndex, envPtr);
	} else {
	    TclEmitInstInt4(INST_STORE_SCALAR4, localIndex, envPtr);
	}
    }
    TclEmitOpcode(INST_POP, envPtr);
    TclEmitPush(TclRegisterLiteral(envPtr, "0", 1, /*onHeap*/ 0),
	    envPtr);
    TclEmitForwardJump(envPtr, TCL_UNCONDITIONAL_JUMP, &jumpFixup);

    /*
     * The "error case" code: store the body's result into the variable (if
     * any), then push the error result code. The initial PC offset here is
     * the catch's error target.
     */

    envPtr->currStackDepth = savedStackDepth;
    envPtr->exceptArrayPtr[range].catchOffset =
	    (envPtr->codeNext - envPtr->codeStart);
    if (localIndex != -1) {
	TclEmitOpcode(INST_PUSH_RESULT, envPtr);
	if (localIndex <= 255) {
	    TclEmitInstInt1(INST_STORE_SCALAR1, localIndex, envPtr);
	} else {
	    TclEmitInstInt4(INST_STORE_SCALAR4, localIndex, envPtr);
	}
	TclEmitOpcode(INST_POP, envPtr);
    }
    TclEmitOpcode(INST_PUSH_RETURN_CODE, envPtr);


    /*
     * Update the target of the jump after the "no errors" code, then emit
     * an endCatch instruction at the end of the catch command.
     */

    jumpDist = (envPtr->codeNext - envPtr->codeStart)
	    - jumpFixup.codeOffset;
    if (TclFixupForwardJump(envPtr, &jumpFixup, jumpDist, 127)) {
	panic("TclCompileCatchCmd: bad jump distance %d\n", jumpDist);
    }
    TclEmitOpcode(INST_END_CATCH, envPtr);

    done:
    envPtr->currStackDepth = savedStackDepth + 1;
    envPtr->exceptDepth--;
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileContinueCmd --
 *
 *	Procedure called to compile the "continue" command.
 *
 * Results:
 *	The return value is a standard Tcl result, which is TCL_OK unless
 *	there was an error while parsing string. If an error occurs then
 *	the interpreter's result contains a standard error message.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "continue" command
 *	at runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileContinueCmd(interp, parsePtr, envPtr)
    Tcl_Interp *interp;		/* Used for error reporting. */
    Tcl_Parse *parsePtr;	/* Points to a parse structure for the
				 * command created by Tcl_ParseCommand. */
    CompileEnv *envPtr;		/* Holds resulting instructions. */
{
    /*
     * There should be no argument after the "continue".
     */

    if (parsePtr->numWords != 1) {
	Tcl_ResetResult(interp);
	Tcl_AppendToObj(Tcl_GetObjResult(interp),
	        "wrong # args: should be \"continue\"", -1);
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
 * TclCompileExprCmd --
 *
 *	Procedure called to compile the "expr" command.
 *
 * Results:
 *	The return value is a standard Tcl result, which is TCL_OK
 *	unless there was an error while parsing string. If an error occurs
 *	then the interpreter's result contains a standard error message.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "expr" command
 *	at runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileExprCmd(interp, parsePtr, envPtr)
    Tcl_Interp *interp;		/* Used for error reporting. */
    Tcl_Parse *parsePtr;	/* Points to a parse structure for the
				 * command created by Tcl_ParseCommand. */
    CompileEnv *envPtr;		/* Holds resulting instructions. */
{
    Tcl_Token *firstWordPtr;

    if (parsePtr->numWords == 1) {
	Tcl_ResetResult(interp);
	Tcl_AppendToObj(Tcl_GetObjResult(interp),
	        "wrong # args: should be \"expr arg ?arg ...?\"", -1);
        return TCL_ERROR;
    }

    firstWordPtr = parsePtr->tokenPtr
	    + (parsePtr->tokenPtr->numComponents + 1);
    return TclCompileExprWords(interp, firstWordPtr, (parsePtr->numWords-1),
	    envPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileForCmd --
 *
 *	Procedure called to compile the "for" command.
 *
 * Results:
 *	The return value is a standard Tcl result, which is TCL_OK unless
 *	there was an error while parsing string. If an error occurs then
 *	the interpreter's result contains a standard error message.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "for" command
 *	at runtime.
 *
 *----------------------------------------------------------------------
 */
int
TclCompileForCmd(interp, parsePtr, envPtr)
    Tcl_Interp *interp;		/* Used for error reporting. */
    Tcl_Parse *parsePtr;	/* Points to a parse structure for the
				 * command created by Tcl_ParseCommand. */
    CompileEnv *envPtr;		/* Holds resulting instructions. */
{
    Tcl_Token *startTokenPtr, *testTokenPtr, *nextTokenPtr, *bodyTokenPtr;
    JumpFixup jumpEvalCondFixup;
    int testCodeOffset, bodyCodeOffset, nextCodeOffset, jumpDist;
    int bodyRange, nextRange, code;
    char buffer[32 + TCL_INTEGER_SPACE];
    int savedStackDepth = envPtr->currStackDepth;

    if (parsePtr->numWords != 5) {
	Tcl_ResetResult(interp);
	Tcl_AppendToObj(Tcl_GetObjResult(interp),
	        "wrong # args: should be \"for start test next command\"", -1);
	return TCL_ERROR;
    }

    /*
     * If the test expression requires substitutions, don't compile the for
     * command inline. E.g., the expression might cause the loop to never
     * execute or execute forever, as in "for {} "$x > 5" {incr x} {}".
     */

    startTokenPtr = parsePtr->tokenPtr
	    + (parsePtr->tokenPtr->numComponents + 1);
    testTokenPtr = startTokenPtr + (startTokenPtr->numComponents + 1);
    if (testTokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
	return TCL_OUT_LINE_COMPILE;
    }

    /*
     * Bail out also if the body or the next expression require substitutions
     * in order to insure correct behaviour [Bug 219166]
     */

    nextTokenPtr = testTokenPtr + (testTokenPtr->numComponents + 1);
    bodyTokenPtr = nextTokenPtr + (nextTokenPtr->numComponents + 1);
    if ((nextTokenPtr->type != TCL_TOKEN_SIMPLE_WORD) 
	    || (bodyTokenPtr->type != TCL_TOKEN_SIMPLE_WORD)) {
	return TCL_OUT_LINE_COMPILE;
    }

    /*
     * Create ExceptionRange records for the body and the "next" command.
     * The "next" command's ExceptionRange supports break but not continue
     * (and has a -1 continueOffset).
     */

    envPtr->exceptDepth++;
    envPtr->maxExceptDepth =
	    TclMax(envPtr->exceptDepth, envPtr->maxExceptDepth);
    bodyRange = TclCreateExceptRange(LOOP_EXCEPTION_RANGE, envPtr);
    nextRange = TclCreateExceptRange(LOOP_EXCEPTION_RANGE, envPtr);

    /*
     * Inline compile the initial command.
     */

    code = TclCompileCmdWord(interp, startTokenPtr+1,
	    startTokenPtr->numComponents, envPtr);
    if (code != TCL_OK) {
	if (code == TCL_ERROR) {
            Tcl_AddObjErrorInfo(interp,
	            "\n    (\"for\" initial command)", -1);
        }
	goto done;
    }
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

    bodyCodeOffset = (envPtr->codeNext - envPtr->codeStart);

    code = TclCompileCmdWord(interp, bodyTokenPtr+1,
	    bodyTokenPtr->numComponents, envPtr);
    envPtr->currStackDepth = savedStackDepth + 1;
    if (code != TCL_OK) {
	if (code == TCL_ERROR) {
	    sprintf(buffer, "\n    (\"for\" body line %d)",
		    interp->errorLine);
            Tcl_AddObjErrorInfo(interp, buffer, -1);
        }
	goto done;
    }
    envPtr->exceptArrayPtr[bodyRange].numCodeBytes =
	    (envPtr->codeNext - envPtr->codeStart) - bodyCodeOffset;
    TclEmitOpcode(INST_POP, envPtr);


    /*
     * Compile the "next" subcommand.
     */

    envPtr->currStackDepth = savedStackDepth;
    nextCodeOffset = (envPtr->codeNext - envPtr->codeStart);
    code = TclCompileCmdWord(interp, nextTokenPtr+1,
	    nextTokenPtr->numComponents, envPtr);
    envPtr->currStackDepth = savedStackDepth + 1;
    if (code != TCL_OK) {
	if (code == TCL_ERROR) {
	    Tcl_AddObjErrorInfo(interp,
	            "\n    (\"for\" loop-end command)", -1);
	}
	goto done;
    }
    envPtr->exceptArrayPtr[nextRange].numCodeBytes =
	    (envPtr->codeNext - envPtr->codeStart)
	    - envPtr->exceptArrayPtr[nextRange].codeOffset;
    TclEmitOpcode(INST_POP, envPtr);
    envPtr->currStackDepth = savedStackDepth;

    /*
     * Compile the test expression then emit the conditional jump that
     * terminates the for.
     */

    testCodeOffset = (envPtr->codeNext - envPtr->codeStart);
    jumpDist = testCodeOffset - jumpEvalCondFixup.codeOffset;
    if (TclFixupForwardJump(envPtr, &jumpEvalCondFixup, jumpDist, 127)) {
	bodyCodeOffset += 3;
	nextCodeOffset += 3;
	testCodeOffset += 3;
    }
    
    envPtr->currStackDepth = savedStackDepth;
    code = TclCompileExprWords(interp, testTokenPtr, 1, envPtr);
    if (code != TCL_OK) {
	if (code == TCL_ERROR) {
	    Tcl_AddObjErrorInfo(interp,
				"\n    (\"for\" test expression)", -1);
	}
	goto done;
    }
    envPtr->currStackDepth = savedStackDepth + 1;
    
    jumpDist = (envPtr->codeNext - envPtr->codeStart) - bodyCodeOffset;
    if (jumpDist > 127) {
	TclEmitInstInt4(INST_JUMP_TRUE4, -jumpDist, envPtr);
    } else {
	TclEmitInstInt1(INST_JUMP_TRUE1, -jumpDist, envPtr);
    }
    
    /*
     * Set the loop's offsets and break target.
     */

    envPtr->exceptArrayPtr[bodyRange].codeOffset = bodyCodeOffset;
    envPtr->exceptArrayPtr[bodyRange].continueOffset = nextCodeOffset;

    envPtr->exceptArrayPtr[nextRange].codeOffset = nextCodeOffset;

    envPtr->exceptArrayPtr[bodyRange].breakOffset =
            envPtr->exceptArrayPtr[nextRange].breakOffset =
	    (envPtr->codeNext - envPtr->codeStart);
    
    /*
     * The for command's result is an empty string.
     */

    envPtr->currStackDepth = savedStackDepth;
    TclEmitPush(TclRegisterLiteral(envPtr, "", 0, /*onHeap*/ 0), envPtr);
    code = TCL_OK;

    done:
    envPtr->exceptDepth--;
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileForeachCmd --
 *
 *	Procedure called to compile the "foreach" command.
 *
 * Results:
 *	The return value is a standard Tcl result, which is TCL_OK if
 *	compilation was successful. If an error occurs then the
 *	interpreter's result contains a standard error message and TCL_ERROR
 *	is returned. If the command is too complex for TclCompileForeachCmd,
 *	TCL_OUT_LINE_COMPILE is returned indicating that the foreach command
 *	should be compiled "out of line" by emitting code to invoke its
 *	command procedure at runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "foreach" command
 *	at runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileForeachCmd(interp, parsePtr, envPtr)
    Tcl_Interp *interp;		/* Used for error reporting. */
    Tcl_Parse *parsePtr;	/* Points to a parse structure for the
				 * command created by Tcl_ParseCommand. */
    CompileEnv *envPtr;		/* Holds resulting instructions. */
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
    char *varList;
    unsigned char *jumpPc;
    JumpFixup jumpFalseFixup;
    int jumpDist, jumpBackDist, jumpBackOffset, infoIndex, range;
    int numWords, numLists, numVars, loopIndex, tempVar, i, j, code;
    char savedChar;
    char buffer[32 + TCL_INTEGER_SPACE];
    int savedStackDepth = envPtr->currStackDepth;


    /*
     * We parse the variable list argument words and create two arrays:
     *    varcList[i] is number of variables in i-th var list
     *    varvList[i] points to array of var names in i-th var list
     */

#define STATIC_VAR_LIST_SIZE 5
    int varcListStaticSpace[STATIC_VAR_LIST_SIZE];
    CONST char **varvListStaticSpace[STATIC_VAR_LIST_SIZE];
    int *varcList = varcListStaticSpace;
    CONST char ***varvList = varvListStaticSpace;

    /*
     * If the foreach command isn't in a procedure, don't compile it inline:
     * the payoff is too small.
     */

    if (procPtr == NULL) {
	return TCL_OUT_LINE_COMPILE;
    }

    numWords = parsePtr->numWords;
    if ((numWords < 4) || (numWords%2 != 0)) {
	Tcl_ResetResult(interp);
	Tcl_AppendToObj(Tcl_GetObjResult(interp),
	        "wrong # args: should be \"foreach varList list ?varList list ...? command\"", -1);
        return TCL_ERROR;
    }

    /*
     * Bail out if the body requires substitutions
     * in order to insure correct behaviour [Bug 219166]
     */
    for (i = 0, tokenPtr = parsePtr->tokenPtr;
	    i < numWords-1;
	    i++, tokenPtr += (tokenPtr->numComponents + 1)) {
    }
    bodyTokenPtr = tokenPtr;
    if (bodyTokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
	return TCL_OUT_LINE_COMPILE;
    }

    /*
     * Allocate storage for the varcList and varvList arrays if necessary.
     */

    numLists = (numWords - 2)/2;
    if (numLists > STATIC_VAR_LIST_SIZE) {
        varcList = (int *) ckalloc(numLists * sizeof(int));
        varvList = (CONST char ***) ckalloc(numLists * sizeof(char **));
    }
    for (loopIndex = 0;  loopIndex < numLists;  loopIndex++) {
        varcList[loopIndex] = 0;
        varvList[loopIndex] = NULL;
    }
    
    /*
     * Set the exception stack depth.
     */ 

    envPtr->exceptDepth++;
    envPtr->maxExceptDepth =
	TclMax(envPtr->exceptDepth, envPtr->maxExceptDepth);

    /*
     * Break up each var list and set the varcList and varvList arrays.
     * Don't compile the foreach inline if any var name needs substitutions
     * or isn't a scalar, or if any var list needs substitutions.
     */

    loopIndex = 0;
    for (i = 0, tokenPtr = parsePtr->tokenPtr;
	    i < numWords-1;
	    i++, tokenPtr += (tokenPtr->numComponents + 1)) {
	if (i%2 == 1) {
	    if (tokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
		code = TCL_OUT_LINE_COMPILE;
		goto done;
	    }
	    varList = tokenPtr[1].start;
	    savedChar = varList[tokenPtr[1].size];

	    /*
	     * Note there is a danger that modifying the string could have
	     * undesirable side effects.  In this case, Tcl_SplitList does
	     * not have any dependencies on shared strings so we should be
	     * safe.
	     */

	    varList[tokenPtr[1].size] = '\0';
	    code = Tcl_SplitList(interp, varList,
		    &varcList[loopIndex], &varvList[loopIndex]);
	    varList[tokenPtr[1].size] = savedChar;
	    if (code != TCL_OK) {
		goto done;
	    }

	    numVars = varcList[loopIndex];
	    for (j = 0;  j < numVars;  j++) {
		CONST char *varName = varvList[loopIndex][j];
		if (!TclIsLocalScalar(varName, (int) strlen(varName))) {
		    code = TCL_OUT_LINE_COMPILE;
		    goto done;
		}
	    }
	    loopIndex++;
	}
    }

    /*
     * We will compile the foreach command.
     * Reserve (numLists + 1) temporary variables:
     *    - numLists temps to hold each value list
     *    - 1 temp for the loop counter (index of next element in each list)
     * At this time we don't try to reuse temporaries; if there are two
     * nonoverlapping foreach loops, they don't share any temps.
     */

    firstValueTemp = -1;
    for (loopIndex = 0;  loopIndex < numLists;  loopIndex++) {
	tempVar = TclFindCompiledLocal(NULL, /*nameChars*/ 0,
		/*create*/ 1, /*flags*/ VAR_SCALAR, procPtr);
	if (loopIndex == 0) {
	    firstValueTemp = tempVar;
	}
    }
    loopCtTemp = TclFindCompiledLocal(NULL, /*nameChars*/ 0,
	    /*create*/ 1, /*flags*/ VAR_SCALAR, procPtr);
    
    /*
     * Create and initialize the ForeachInfo and ForeachVarList data
     * structures describing this command. Then create a AuxData record
     * pointing to the ForeachInfo structure.
     */

    infoPtr = (ForeachInfo *) ckalloc((unsigned)
	    (sizeof(ForeachInfo) + (numLists * sizeof(ForeachVarList *))));
    infoPtr->numLists = numLists;
    infoPtr->firstValueTemp = firstValueTemp;
    infoPtr->loopCtTemp = loopCtTemp;
    for (loopIndex = 0;  loopIndex < numLists;  loopIndex++) {
	ForeachVarList *varListPtr;
	numVars = varcList[loopIndex];
	varListPtr = (ForeachVarList *) ckalloc((unsigned)
	        sizeof(ForeachVarList) + (numVars * sizeof(int)));
	varListPtr->numVars = numVars;
	for (j = 0;  j < numVars;  j++) {
	    CONST char *varName = varvList[loopIndex][j];
	    int nameChars = strlen(varName);
	    varListPtr->varIndexes[j] = TclFindCompiledLocal(varName,
		    nameChars, /*create*/ 1, /*flags*/ VAR_SCALAR, procPtr);
	}
	infoPtr->varLists[loopIndex] = varListPtr;
    }
    infoIndex = TclCreateAuxData((ClientData) infoPtr, &tclForeachInfoType, envPtr);

    /*
     * Evaluate then store each value list in the associated temporary.
     */

    range = TclCreateExceptRange(LOOP_EXCEPTION_RANGE, envPtr);
    
    loopIndex = 0;
    for (i = 0, tokenPtr = parsePtr->tokenPtr;
	    i < numWords-1;
	    i++, tokenPtr += (tokenPtr->numComponents + 1)) {
	if ((i%2 == 0) && (i > 0)) {
	    code = TclCompileTokens(interp, tokenPtr+1,
		    tokenPtr->numComponents, envPtr);
	    if (code != TCL_OK) {
		goto done;
	    }

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

    envPtr->exceptArrayPtr[range].continueOffset =
	    (envPtr->codeNext - envPtr->codeStart);
    TclEmitInstInt4(INST_FOREACH_STEP4, infoIndex, envPtr);
    TclEmitForwardJump(envPtr, TCL_FALSE_JUMP, &jumpFalseFixup);
    
    /*
     * Inline compile the loop body.
     */

    envPtr->exceptArrayPtr[range].codeOffset =
	    (envPtr->codeNext - envPtr->codeStart);
    code = TclCompileCmdWord(interp, bodyTokenPtr+1,
	    bodyTokenPtr->numComponents, envPtr);
    envPtr->currStackDepth = savedStackDepth + 1;
    if (code != TCL_OK) {
	if (code == TCL_ERROR) {
	    sprintf(buffer, "\n    (\"foreach\" body line %d)",
		    interp->errorLine);
            Tcl_AddObjErrorInfo(interp, buffer, -1);
        }
	goto done;
    }
    envPtr->exceptArrayPtr[range].numCodeBytes =
	    (envPtr->codeNext - envPtr->codeStart)
	    - envPtr->exceptArrayPtr[range].codeOffset;
    TclEmitOpcode(INST_POP, envPtr);
	
    /*
     * Jump back to the test at the top of the loop. Generate a 4 byte jump
     * if the distance to the test is > 120 bytes. This is conservative and
     * ensures that we won't have to replace this jump if we later need to
     * replace the ifFalse jump with a 4 byte jump.
     */

    jumpBackOffset = (envPtr->codeNext - envPtr->codeStart);
    jumpBackDist =
	(jumpBackOffset - envPtr->exceptArrayPtr[range].continueOffset);
    if (jumpBackDist > 120) {
	TclEmitInstInt4(INST_JUMP4, -jumpBackDist, envPtr);
    } else {
	TclEmitInstInt1(INST_JUMP1, -jumpBackDist, envPtr);
    }

    /*
     * Fix the target of the jump after the foreach_step test.
     */

    jumpDist = (envPtr->codeNext - envPtr->codeStart)
	    - jumpFalseFixup.codeOffset;
    if (TclFixupForwardJump(envPtr, &jumpFalseFixup, jumpDist, 127)) {
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

    envPtr->exceptArrayPtr[range].breakOffset =
	    (envPtr->codeNext - envPtr->codeStart);
    
    /*
     * The foreach command's result is an empty string.
     */

    envPtr->currStackDepth = savedStackDepth;
    TclEmitPush(TclRegisterLiteral(envPtr, "", 0, /*onHeap*/ 0), envPtr);
    envPtr->currStackDepth = savedStackDepth + 1;

    done:
    for (loopIndex = 0;  loopIndex < numLists;  loopIndex++) {
        if (varvList[loopIndex] != NULL) {
            ckfree((char *) varvList[loopIndex]);
        }
    }
    if (varcList != varcListStaticSpace) {
	ckfree((char *) varcList);
        ckfree((char *) varvList);
    }
    envPtr->exceptDepth--;
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * DupForeachInfo --
 *
 *	This procedure duplicates a ForeachInfo structure created as
 *	auxiliary data during the compilation of a foreach command.
 *
 * Results:
 *	A pointer to a newly allocated copy of the existing ForeachInfo
 *	structure is returned.
 *
 * Side effects:
 *	Storage for the copied ForeachInfo record is allocated. If the
 *	original ForeachInfo structure pointed to any ForeachVarList
 *	records, these structures are also copied and pointers to them
 *	are stored in the new ForeachInfo record.
 *
 *----------------------------------------------------------------------
 */

static ClientData
DupForeachInfo(clientData)
    ClientData clientData;	/* The foreach command's compilation
				 * auxiliary data to duplicate. */
{
    register ForeachInfo *srcPtr = (ForeachInfo *) clientData;
    ForeachInfo *dupPtr;
    register ForeachVarList *srcListPtr, *dupListPtr;
    int numLists = srcPtr->numLists;
    int numVars, i, j;
    
    dupPtr = (ForeachInfo *) ckalloc((unsigned)
	    (sizeof(ForeachInfo) + (numLists * sizeof(ForeachVarList *))));
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
    return (ClientData) dupPtr;
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
FreeForeachInfo(clientData)
    ClientData clientData;	/* The foreach command's compilation
				 * auxiliary data to free. */
{
    register ForeachInfo *infoPtr = (ForeachInfo *) clientData;
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
 * TclCompileIfCmd --
 *
 *	Procedure called to compile the "if" command.
 *
 * Results:
 *	The return value is a standard Tcl result, which is TCL_OK if
 *	compilation was successful. If an error occurs then the
 *	interpreter's result contains a standard error message and TCL_ERROR
 *	is returned. If the command is too complex for TclCompileIfCmd,
 *	TCL_OUT_LINE_COMPILE is returned indicating that the if command
 *	should be compiled "out of line" by emitting code to invoke its
 *	command procedure at runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "if" command
 *	at runtime.
 *
 *----------------------------------------------------------------------
 */
int
TclCompileIfCmd(interp, parsePtr, envPtr)
    Tcl_Interp *interp;		/* Used for error reporting. */
    Tcl_Parse *parsePtr;	/* Points to a parse structure for the
				 * command created by Tcl_ParseCommand. */
    CompileEnv *envPtr;		/* Holds resulting instructions. */
{
    JumpFixupArray jumpFalseFixupArray;
    				/* Used to fix the ifFalse jump after each
				 * test when its target PC is determined. */
    JumpFixupArray jumpEndFixupArray;
				/* Used to fix the jump after each "then"
				 * body to the end of the "if" when that PC
				 * is determined. */
    Tcl_Token *tokenPtr, *testTokenPtr;
    int jumpDist, jumpFalseDist;
    int jumpIndex = 0;          /* avoid compiler warning. */
    int numWords, wordIdx, numBytes, j, code;
    char *word;
    char buffer[100];
    int savedStackDepth = envPtr->currStackDepth;
                                /* Saved stack depth at the start of the first
				 * test; the envPtr current depth is restored
				 * to this value at the start of each test. */
    char *condStart, *savedPos, savedChar;
    int realCond = 1;           /* set to 0 for static conditions: "if 0 {..}" */
    int boolVal;                /* value of static condition */
    int compileScripts = 1;            

    /*
     * Only compile the "if" command if all arguments are simple
     * words, in order to insure correct substitution [Bug 219166]
     */

    tokenPtr = parsePtr->tokenPtr;
    wordIdx = 0;
    numWords = parsePtr->numWords;

    for (wordIdx = 0; wordIdx < numWords; wordIdx++) {
	if (tokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
	    return TCL_OUT_LINE_COMPILE;
	}
	tokenPtr += 2;
    }


    TclInitJumpFixupArray(&jumpFalseFixupArray);
    TclInitJumpFixupArray(&jumpEndFixupArray);
    code = TCL_OK;

    /*
     * Each iteration of this loop compiles one "if expr ?then? body"
     * or "elseif expr ?then? body" clause. 
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
	    tokenPtr += (tokenPtr->numComponents + 1);
	    wordIdx++;
	} else {
	    break;
	}
	if (wordIdx >= numWords) {
	    sprintf(buffer,
	            "wrong # args: no expression after \"%.30s\" argument",
		    word);
	    Tcl_ResetResult(interp);
	    Tcl_AppendToObj(Tcl_GetObjResult(interp), buffer, -1);
	    code = TCL_ERROR;
	    goto done;
	}

	/*
	 * Compile the test expression then emit the conditional jump
	 * around the "then" part. 
	 */
	
	envPtr->currStackDepth = savedStackDepth;
	testTokenPtr = tokenPtr;


	if (realCond) {
	    /*
	     * Find out if the condition is a constant. 
	     */
	
	    condStart = testTokenPtr[1].start;
	    savedPos = condStart + testTokenPtr[1].size - 1;
	    
	    while (*condStart == ' ') {
		condStart++;
	    }
	    while (*savedPos == ' ') {
		savedPos--;
	    }
	    savedPos++;
	    
	    savedChar = *savedPos;
	    *savedPos = '\0';
	    
	    if (Tcl_GetBoolean(interp, condStart, &boolVal) != TCL_ERROR) { 
		/*
		 * A static condition
		 */
		*savedPos = savedChar;
		realCond = 0;
		if (!boolVal) {
		    compileScripts = 0;
		}
	    } else {
		*savedPos = savedChar;
		Tcl_ResetResult(interp);
		code = TclCompileExprWords(interp, testTokenPtr, 1, envPtr);
		if (code != TCL_OK) {
		    if (code == TCL_ERROR) {
			Tcl_AddObjErrorInfo(interp,
			        "\n    (\"if\" test expression)", -1);
		    }
		    goto done;
		}
		if (jumpFalseFixupArray.next >= jumpFalseFixupArray.end) {
		    TclExpandJumpFixupArray(&jumpFalseFixupArray);
		}
		jumpIndex = jumpFalseFixupArray.next;
		jumpFalseFixupArray.next++;
		TclEmitForwardJump(envPtr, TCL_FALSE_JUMP,
			       &(jumpFalseFixupArray.fixup[jumpIndex]));	    
	    }
	}


	/*
	 * Skip over the optional "then" before the then clause.
	 */

	tokenPtr = testTokenPtr + (testTokenPtr->numComponents + 1);
	wordIdx++;
	if (wordIdx >= numWords) {
	    sprintf(buffer, "wrong # args: no script following \"%.20s\" argument", testTokenPtr->start);
	    Tcl_ResetResult(interp);
	    Tcl_AppendToObj(Tcl_GetObjResult(interp), buffer, -1);
	    code = TCL_ERROR;
	    goto done;
	}
	if (tokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
	    word = tokenPtr[1].start;
	    numBytes = tokenPtr[1].size;
	    if ((numBytes == 4) && (strncmp(word, "then", 4) == 0)) {
		tokenPtr += (tokenPtr->numComponents + 1);
		wordIdx++;
		if (wordIdx >= numWords) {
		    Tcl_ResetResult(interp);
		    Tcl_AppendToObj(Tcl_GetObjResult(interp),
		            "wrong # args: no script following \"then\" argument", -1);
		    code = TCL_ERROR;
		    goto done;
		}
	    }
	}

	/*
	 * Compile the "then" command body.
	 */

	if (compileScripts) {
	    envPtr->currStackDepth = savedStackDepth;
	    code = TclCompileCmdWord(interp, tokenPtr+1,
	            tokenPtr->numComponents, envPtr);
	    if (code != TCL_OK) {
		if (code == TCL_ERROR) {
		    sprintf(buffer, "\n    (\"if\" then script line %d)",
		            interp->errorLine);
		    Tcl_AddObjErrorInfo(interp, buffer, -1);
		}
		goto done;
	    }	
	}

	if (realCond) {
	    /*
	     * Jump to the end of the "if" command. Both jumpFalseFixupArray and
	     * jumpEndFixupArray are indexed by "jumpIndex".
	     */
	    
	    if (jumpEndFixupArray.next >= jumpEndFixupArray.end) {
		TclExpandJumpFixupArray(&jumpEndFixupArray);
	    }
	    jumpEndFixupArray.next++;
	    TclEmitForwardJump(envPtr, TCL_UNCONDITIONAL_JUMP,
	            &(jumpEndFixupArray.fixup[jumpIndex]));
	    
	    /*
	     * Fix the target of the jumpFalse after the test. Generate a 4 byte
	     * jump if the distance is > 120 bytes. This is conservative, and
	     * ensures that we won't have to replace this jump if we later also
	     * need to replace the proceeding jump to the end of the "if" with a
	     * 4 byte jump.
	     */

	    jumpDist = (envPtr->codeNext - envPtr->codeStart)
	            - jumpFalseFixupArray.fixup[jumpIndex].codeOffset;
	    if (TclFixupForwardJump(envPtr,
	            &(jumpFalseFixupArray.fixup[jumpIndex]), jumpDist, 120)) {
		/*
		 * Adjust the code offset for the proceeding jump to the end
		 * of the "if" command.
		 */
		
		jumpEndFixupArray.fixup[jumpIndex].codeOffset += 3;
	    }
	} else if (boolVal) {
	    /* 
	     *We were processing an "if 1 {...}"; stop compiling
	     * scripts
	     */

	    compileScripts = 0;
	} else {
	    /* 
	     *We were processing an "if 0 {...}"; reset so that
	     * the rest (elseif, else) is compiled correctly
	     */

	    realCond = 1;
	    compileScripts = 1;
	} 

	tokenPtr += (tokenPtr->numComponents + 1);
	wordIdx++;
    }

    /*
     * Restore the current stack depth in the environment; the 
     * "else" clause (or its default) will add 1 to this.
     */

    envPtr->currStackDepth = savedStackDepth;

    /*
     * Check for the optional else clause. Do not compile
     * anything if this was an "if 1 {...}" case.
     */

    if ((wordIdx < numWords)
	    && (tokenPtr->type == TCL_TOKEN_SIMPLE_WORD)) {
	/*
	 * There is an else clause. Skip over the optional "else" word.
	 */

	word = tokenPtr[1].start;
	numBytes = tokenPtr[1].size;
	if ((numBytes == 4) && (strncmp(word, "else", 4) == 0)) {
	    tokenPtr += (tokenPtr->numComponents + 1);
	    wordIdx++;
	    if (wordIdx >= numWords) {
		Tcl_ResetResult(interp);
		Tcl_AppendToObj(Tcl_GetObjResult(interp),
		        "wrong # args: no script following \"else\" argument", -1);
		code = TCL_ERROR;
		goto done;
	    }
	}

	if (compileScripts) {
	    /*
	     * Compile the else command body.
	     */
	    
	    code = TclCompileCmdWord(interp, tokenPtr+1,
		    tokenPtr->numComponents, envPtr);
	    if (code != TCL_OK) {
		if (code == TCL_ERROR) {
		    sprintf(buffer, "\n    (\"if\" else script line %d)",
			    interp->errorLine);
		    Tcl_AddObjErrorInfo(interp, buffer, -1);
		}
		goto done;
	    }
	}

	/*
	 * Make sure there are no words after the else clause.
	 */
	
	wordIdx++;
	if (wordIdx < numWords) {
	    Tcl_ResetResult(interp);
	    Tcl_AppendToObj(Tcl_GetObjResult(interp),
		    "wrong # args: extra words after \"else\" clause in \"if\" command", -1);
	    code = TCL_ERROR;
	    goto done;
	}
    } else {
	/*
	 * No else clause: the "if" command's result is an empty string.
	 */

	if (compileScripts) {
	    TclEmitPush(TclRegisterLiteral(envPtr, "", 0,/*onHeap*/ 0), envPtr);
	}
    }

    /*
     * Fix the unconditional jumps to the end of the "if" command.
     */
    
    for (j = jumpEndFixupArray.next;  j > 0;  j--) {
	jumpIndex = (j - 1);	/* i.e. process the closest jump first */
	jumpDist = (envPtr->codeNext - envPtr->codeStart)
	        - jumpEndFixupArray.fixup[jumpIndex].codeOffset;
	if (TclFixupForwardJump(envPtr,
	        &(jumpEndFixupArray.fixup[jumpIndex]), jumpDist, 127)) {
	    /*
	     * Adjust the immediately preceeding "ifFalse" jump. We moved
	     * it's target (just after this jump) down three bytes.
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
		panic("TclCompileIfCmd: unexpected opcode updating ifFalse jump");
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
 *	The return value is a standard Tcl result, which is TCL_OK if
 *	compilation was successful. If an error occurs then the
 *	interpreter's result contains a standard error message and TCL_ERROR
 *	is returned. If the command is too complex for TclCompileIncrCmd,
 *	TCL_OUT_LINE_COMPILE is returned indicating that the incr command
 *	should be compiled "out of line" by emitting code to invoke its
 *	command procedure at runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "incr" command
 *	at runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileIncrCmd(interp, parsePtr, envPtr)
    Tcl_Interp *interp;		/* Used for error reporting. */
    Tcl_Parse *parsePtr;	/* Points to a parse structure for the
				 * command created by Tcl_ParseCommand. */
    CompileEnv *envPtr;		/* Holds resulting instructions. */
{
    Tcl_Token *varTokenPtr, *incrTokenPtr;
    int simpleVarName, isScalar, localIndex, haveImmValue, immValue;
    int code = TCL_OK;

    if ((parsePtr->numWords != 2) && (parsePtr->numWords != 3)) {
	Tcl_ResetResult(interp);
	Tcl_AppendToObj(Tcl_GetObjResult(interp),
	        "wrong # args: should be \"incr varName ?increment?\"", -1);
	return TCL_ERROR;
    }

    varTokenPtr = parsePtr->tokenPtr
	    + (parsePtr->tokenPtr->numComponents + 1);

    code = TclPushVarName(interp, varTokenPtr, envPtr, TCL_NO_LARGE_INDEX,
	    &localIndex, &simpleVarName, &isScalar);
    if (code != TCL_OK) {
	goto done;
    }

    /*
     * If an increment is given, push it, but see first if it's a small
     * integer.
     */

    haveImmValue = 0;
    immValue = 0;
    if (parsePtr->numWords == 3) {
	incrTokenPtr = varTokenPtr + (varTokenPtr->numComponents + 1);
	if (incrTokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
	    char *word = incrTokenPtr[1].start;
	    int numBytes = incrTokenPtr[1].size;
	    char savedChar = word[numBytes];
	    long n;

	    /*
	     * Note there is a danger that modifying the string could have
	     * undesirable side effects.  In this case, TclLooksLikeInt and
	     * TclGetLong do not have any dependencies on shared strings so we
	     * should be safe.
	     */

	    word[numBytes] = '\0';
	    if (TclLooksLikeInt(word, numBytes)
		    && (TclGetLong((Tcl_Interp *) NULL, word, &n) == TCL_OK)) {
		if ((-127 <= n) && (n <= 127)) {
		    haveImmValue = 1;
		    immValue = n;
		}
	    }
	    word[numBytes] = savedChar;
	    if (!haveImmValue) {
		TclEmitPush(TclRegisterLiteral(envPtr, word, numBytes,
	               /*onHeap*/ 0), envPtr);
	    }
	} else {
	    code = TclCompileTokens(interp, incrTokenPtr+1, 
	            incrTokenPtr->numComponents, envPtr);
	    if (code != TCL_OK) {
		if (code == TCL_ERROR) {
		    Tcl_AddObjErrorInfo(interp,
	                    "\n    (increment expression)", -1);
		}
		goto done;
	    }
	}
    } else {			/* no incr amount given so use 1 */
	haveImmValue = 1;
	immValue = 1;
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
    } else {			/* non-simple variable name */
	if (haveImmValue) {
	    TclEmitInstInt1(INST_INCR_STK_IMM, immValue, envPtr);
	} else {
	    TclEmitOpcode(INST_INCR_STK, envPtr);
	}
    }
	
    done:
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileLappendCmd --
 *
 *	Procedure called to compile the "lappend" command.
 *
 * Results:
 *	The return value is a standard Tcl result, which is normally TCL_OK
 *	unless there was an error while parsing string. If an error occurs
 *	then the interpreter's result contains a standard error message. If
 *	complation fails because the command requires a second level of
 *	substitutions, TCL_OUT_LINE_COMPILE is returned indicating that the
 *	command should be compiled "out of line" by emitting code to
 *	invoke its command procedure (Tcl_LappendObjCmd) at runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "lappend" command
 *	at runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileLappendCmd(interp, parsePtr, envPtr)
    Tcl_Interp *interp;		/* Used for error reporting. */
    Tcl_Parse *parsePtr;	/* Points to a parse structure for the
				 * command created by Tcl_ParseCommand. */
    CompileEnv *envPtr;		/* Holds resulting instructions. */
{
    Tcl_Token *varTokenPtr, *valueTokenPtr;
    int numValues, simpleVarName, isScalar, localIndex, numWords;
    int code = TCL_OK;

    /*
     * If we're not in a procedure, don't compile.
     */
    if (envPtr->procPtr == NULL) {
	return TCL_OUT_LINE_COMPILE;
    }

    numWords = parsePtr->numWords;
    if (numWords == 1) {
	Tcl_ResetResult(interp);
	Tcl_AppendToObj(Tcl_GetObjResult(interp),
		"wrong # args: should be \"lappend varName ?value value ...?\"", -1);
	return TCL_ERROR;
    }
    if (numWords != 3) {
	/*
	 * LAPPEND instructions currently only handle one value appends
	 */
        return TCL_OUT_LINE_COMPILE;
    }
    numValues = (numWords - 2);

    /*
     * Decide if we can use a frame slot for the var/array name or if we
     * need to emit code to compute and push the name at runtime. We use a
     * frame slot (entry in the array of local vars) if we are compiling a
     * procedure body and if the name is simple text that does not include
     * namespace qualifiers. 
     */

    varTokenPtr = parsePtr->tokenPtr
	    + (parsePtr->tokenPtr->numComponents + 1);

    code = TclPushVarName(interp, varTokenPtr, envPtr, TCL_CREATE_VAR,
	    &localIndex, &simpleVarName, &isScalar);
    if (code != TCL_OK) {
	goto done;
    }

    /*
     * If we are doing an assignment, push the new value.
     * In the no values case, create an empty object.
     */

    if (numWords > 2) {
	valueTokenPtr = varTokenPtr + (varTokenPtr->numComponents + 1);
	if (valueTokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
	    TclEmitPush(TclRegisterLiteral(envPtr, valueTokenPtr[1].start,
		    valueTokenPtr[1].size, /*onHeap*/ 0), envPtr);
	} else {
	    code = TclCompileTokens(interp, valueTokenPtr+1,
	            valueTokenPtr->numComponents, envPtr);
	    if (code != TCL_OK) {
		goto done;
	    }
	}
#if 0
    } else {
	/*
	 * We need to carefully handle the two arg case, as lappend
	 * always creates the variable.
	 */

	TclEmitPush(TclRegisterLiteral(envPtr, "", 0, /*onHeap*/ 0), envPtr);
	numValues = 1;
#endif
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
	    if (localIndex >= 0) {
		if (localIndex <= 255) {
		    TclEmitInstInt1(INST_LAPPEND_SCALAR1, localIndex, envPtr);
		} else {
		    TclEmitInstInt4(INST_LAPPEND_SCALAR4, localIndex, envPtr);
		}
	    } else {
		TclEmitOpcode(INST_LAPPEND_STK, envPtr);
	    }
	} else {
	    if (localIndex >= 0) {
		if (localIndex <= 255) {
		    TclEmitInstInt1(INST_LAPPEND_ARRAY1, localIndex, envPtr);
		} else {
		    TclEmitInstInt4(INST_LAPPEND_ARRAY4, localIndex, envPtr);
		}
	    } else {
		TclEmitOpcode(INST_LAPPEND_ARRAY_STK, envPtr);
	    }
	}
    } else {
	TclEmitOpcode(INST_LAPPEND_STK, envPtr);
    }

    done:
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileLindexCmd --
 *
 *	Procedure called to compile the "lindex" command.
 *
 * Results:
 *	The return value is a standard Tcl result, which is TCL_OK if the
 *	compilation was successful.  If the command cannot be byte-compiled,
 *	TCL_OUT_LINE_COMPILE is returned.  If an error occurs then the
 *	interpreter's result contains an error message, and TCL_ERROR is
 *	returned.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "lindex" command
 *	at runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileLindexCmd(interp, parsePtr, envPtr)
    Tcl_Interp *interp;		/* Used for error reporting. */
    Tcl_Parse *parsePtr;	/* Points to a parse structure for the
				 * command created by Tcl_ParseCommand. */
    CompileEnv *envPtr;		/* Holds resulting instructions. */
{
    Tcl_Token *varTokenPtr;
    int code, i;

    int numWords;
    numWords = parsePtr->numWords;

    /*
     * Quit if too few args
     */

    if ( numWords <= 1 ) {
	return TCL_OUT_LINE_COMPILE;
    }

    varTokenPtr = parsePtr->tokenPtr
	+ (parsePtr->tokenPtr->numComponents + 1);
    
    /*
     * Push the operands onto the stack.
     */
	
    for ( i = 1 ; i < numWords ; i++ ) {
	if (varTokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
	    TclEmitPush( TclRegisterLiteral( envPtr,
					     varTokenPtr[1].start,
					     varTokenPtr[1].size,
					     0),
			 envPtr);
	} else {
	    code = TclCompileTokens(interp, varTokenPtr+1,
				    varTokenPtr->numComponents, envPtr);
	    if (code != TCL_OK) {
		return code;
	    }
	}
	varTokenPtr = varTokenPtr + (varTokenPtr->numComponents + 1);
    }
	
    /*
     * Emit INST_LIST_INDEX if objc==3, or INST_LIST_INDEX_MULTI
     * if there are multiple index args.
     */

    if ( numWords == 3 ) {
	TclEmitOpcode( INST_LIST_INDEX, envPtr );
    } else {
 	TclEmitInstInt4( INST_LIST_INDEX_MULTI, numWords-1, envPtr );
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
 *	The return value is a standard Tcl result, which is normally TCL_OK
 *	unless there was an error while parsing string. If an error occurs
 *	then the interpreter's result contains a standard error message. If
 *	complation fails because the command requires a second level of
 *	substitutions, TCL_OUT_LINE_COMPILE is returned indicating that the
 *	command should be compiled "out of line" by emitting code to
 *	invoke its command procedure (Tcl_ListObjCmd) at runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "list" command
 *	at runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileListCmd(interp, parsePtr, envPtr)
    Tcl_Interp *interp;		/* Used for error reporting. */
    Tcl_Parse *parsePtr;	/* Points to a parse structure for the
				 * command created by Tcl_ParseCommand. */
    CompileEnv *envPtr;		/* Holds resulting instructions. */
{
    /*
     * If we're not in a procedure, don't compile.
     */
    if (envPtr->procPtr == NULL) {
	return TCL_OUT_LINE_COMPILE;
    }

    if (parsePtr->numWords == 1) {
	/*
	 * Empty args case
	 */

	TclEmitPush(TclRegisterLiteral(envPtr, "", 0, 0), envPtr);
    } else {
	/*
	 * Push the all values onto the stack.
	 */
	Tcl_Token *valueTokenPtr;
	int i, code, numWords;

	numWords = parsePtr->numWords;

	valueTokenPtr = parsePtr->tokenPtr
	    + (parsePtr->tokenPtr->numComponents + 1);
	for (i = 1; i < numWords; i++) {
	    if (valueTokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
		TclEmitPush(TclRegisterLiteral(envPtr,
			valueTokenPtr[1].start, valueTokenPtr[1].size,
			/*onHeap*/ 0), envPtr);
	    } else {
		code = TclCompileTokens(interp, valueTokenPtr+1,
			valueTokenPtr->numComponents, envPtr);
		if (code != TCL_OK) {
		    return code;
		}
	    }
	    valueTokenPtr = valueTokenPtr + (valueTokenPtr->numComponents + 1);
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
 *	The return value is a standard Tcl result, which is TCL_OK if the
 *	compilation was successful.  If the command cannot be byte-compiled,
 *	TCL_OUT_LINE_COMPILE is returned.  If an error occurs then the
 *	interpreter's result contains an error message, and TCL_ERROR is
 *	returned.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "llength" command
 *	at runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileLlengthCmd(interp, parsePtr, envPtr)
    Tcl_Interp *interp;		/* Used for error reporting. */
    Tcl_Parse *parsePtr;	/* Points to a parse structure for the
				 * command created by Tcl_ParseCommand. */
    CompileEnv *envPtr;		/* Holds resulting instructions. */
{
    Tcl_Token *varTokenPtr;
    int code;

    if (parsePtr->numWords != 2) {
	Tcl_SetResult(interp, "wrong # args: should be \"llength list\"",
		TCL_STATIC);
	return TCL_ERROR;
    }
    varTokenPtr = parsePtr->tokenPtr
	+ (parsePtr->tokenPtr->numComponents + 1);

    if (varTokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
	/*
	 * We could simply count the number of elements here and push
	 * that value, but that is too rare a case to waste the code space.
	 */
	TclEmitPush(TclRegisterLiteral(envPtr, varTokenPtr[1].start,
		varTokenPtr[1].size, 0), envPtr);
    } else {
	code = TclCompileTokens(interp, varTokenPtr+1,
		varTokenPtr->numComponents, envPtr);
	if (code != TCL_OK) {
	    return code;
	}
    }
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
 *	The return value is a standard Tcl result, which is TCL_OK if
 *	the compilation was successful.  If the "lset" command is too
 *	complex for this function, then TCL_OUT_LINE_COMPILE is returned,
 *	indicating that the command should be compiled "out of line"
 *	(that is, not byte-compiled).  If an error occurs, TCL_ERROR is
 *	returned, and the interpreter result contains an error message.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "lset" command
 *	at runtime.
 *
 * The general template for execution of the "lset" command is:
 *	(1) Instructions to push the variable name, unless the
 *	    variable is local to the stack frame.
 *	(2) If the variable is an array element, instructions
 *	    to push the array element name.
 *	(3) Instructions to push each of zero or more "index" arguments
 *	    to the stack, followed with the "newValue" element.
 *	(4) Instructions to duplicate the variable name and/or array
 *	    element name onto the top of the stack, if either was
 *	    pushed at steps (1) and (2).
 *	(5) The appropriate INST_LOAD_* instruction to place the
 *	    original value of the list variable at top of stack.
 *	(6) At this point, the stack contains:
 *	     varName? arrayElementName? index1 index2 ... newValue oldList
 *	    The compiler emits one of INST_LSET_FLAT or INST_LSET_LIST
 *	    according as whether there is exactly one index element (LIST)
 *	    or either zero or else two or more (FLAT).  This instruction
 *	    removes everything from the stack except for the two names
 *	    and pushes the new value of the variable.
 *	(7) Finally, INST_STORE_* stores the new value in the variable
 *	    and cleans up the stack.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileLsetCmd( interp, parsePtr, envPtr )
    Tcl_Interp* interp;		/* Tcl interpreter for error reporting */
    Tcl_Parse* parsePtr;	/* Points to a parse structure for
				 * the command */
    CompileEnv* envPtr;		/* Holds the resulting instructions */
{

    int tempDepth;		/* Depth used for emitting one part
				 * of the code burst. */
    Tcl_Token* varTokenPtr;	/* Pointer to the Tcl_Token representing
				 * the parse of the variable name */

    int result;			/* Status return from library calls */

    int localIndex;		/* Index of var in local var table */
    int simpleVarName;		/* Flag == 1 if var name is simple */
    int isScalar;		/* Flag == 1 if scalar, 0 if array */

    int i;

    /* Check argument count */

    if ( parsePtr->numWords < 3 ) {
	/* Fail at run time, not in compilation */
	return TCL_OUT_LINE_COMPILE;
    }

    /*
     * Decide if we can use a frame slot for the var/array name or if we
     * need to emit code to compute and push the name at runtime. We use a
     * frame slot (entry in the array of local vars) if we are compiling a
     * procedure body and if the name is simple text that does not include
     * namespace qualifiers. 
     */

    varTokenPtr = parsePtr->tokenPtr
	    + (parsePtr->tokenPtr->numComponents + 1);
    result = TclPushVarName( interp, varTokenPtr, envPtr, 0,
			     &localIndex, &simpleVarName, &isScalar );
    if (result != TCL_OK) {
	return result;
    }

    /* Push the "index" args and the new element value. */

    for ( i = 2; i < parsePtr->numWords; ++i ) {

	/* Advance to next arg */

	varTokenPtr = varTokenPtr + (varTokenPtr->numComponents + 1);

	/* Push an arg */

	if (varTokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
	    TclEmitPush( TclRegisterLiteral( envPtr,
					     varTokenPtr[1].start,
					     varTokenPtr[1].size,
					     0),
			 envPtr);
	} else {
	    result = TclCompileTokens(interp, varTokenPtr+1,
				      varTokenPtr->numComponents, envPtr);
	    if ( result != TCL_OK ) {
		return result;
	    }
	}
    }

    /*
     * Duplicate the variable name if it's been pushed.  
     */

    if ( !simpleVarName || localIndex < 0 ) {
	if ( !simpleVarName || isScalar ) {
	    tempDepth = parsePtr->numWords - 2;
	} else {
	    tempDepth = parsePtr->numWords - 1;
	}
	TclEmitInstInt4( INST_OVER, tempDepth, envPtr );
    }

    /*
     * Duplicate an array index if one's been pushed
     */

    if ( simpleVarName && !isScalar ) {
	if ( localIndex < 0 ) {
	    tempDepth = parsePtr->numWords - 1;
	} else {
	    tempDepth = parsePtr->numWords - 2;
	}
	TclEmitInstInt4( INST_OVER, tempDepth, envPtr );
    }

    /*
     * Emit code to load the variable's value.
     */

    if ( !simpleVarName ) {
	TclEmitOpcode( INST_LOAD_STK, envPtr );
    } else if ( isScalar ) {
	if ( localIndex < 0 ) {
	    TclEmitOpcode( INST_LOAD_SCALAR_STK, envPtr );
	} else if ( localIndex < 0x100 ) {
	    TclEmitInstInt1( INST_LOAD_SCALAR1, localIndex, envPtr );
	} else {
	    TclEmitInstInt4( INST_LOAD_SCALAR4, localIndex, envPtr );
	}
    } else {
	if ( localIndex < 0 ) {
	    TclEmitOpcode( INST_LOAD_ARRAY_STK, envPtr );
	} else if ( localIndex < 0x100 ) {
	    TclEmitInstInt1( INST_LOAD_ARRAY1, localIndex, envPtr );
	} else {
	    TclEmitInstInt4( INST_LOAD_ARRAY4, localIndex, envPtr );
	}
    }

    /*
     * Emit the correct variety of 'lset' instruction
     */

    if ( parsePtr->numWords == 4 ) {
	TclEmitOpcode( INST_LSET_LIST, envPtr );
    } else {
	TclEmitInstInt4( INST_LSET_FLAT, (parsePtr->numWords - 1), envPtr );
    }

    /*
     * Emit code to put the value back in the variable
     */

    if ( !simpleVarName ) {
	TclEmitOpcode( INST_STORE_STK, envPtr );
    } else if ( isScalar ) {
	if ( localIndex < 0 ) {
	    TclEmitOpcode( INST_STORE_SCALAR_STK, envPtr );
	} else if ( localIndex < 0x100 ) {
	    TclEmitInstInt1( INST_STORE_SCALAR1, localIndex, envPtr );
	} else {
	    TclEmitInstInt4( INST_STORE_SCALAR4, localIndex, envPtr );
	}
    } else {
	if ( localIndex < 0 ) {
	    TclEmitOpcode( INST_STORE_ARRAY_STK, envPtr );
	} else if ( localIndex < 0x100 ) {
	    TclEmitInstInt1( INST_STORE_ARRAY1, localIndex, envPtr );
	} else {
	    TclEmitInstInt4( INST_STORE_ARRAY4, localIndex, envPtr );
	}
    }
    
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
 *	The return value is a standard Tcl result, which is TCL_OK if
 *	the compilation was successful.  If the "regexp" command is too
 *	complex for this function, then TCL_OUT_LINE_COMPILE is returned,
 *	indicating that the command should be compiled "out of line"
 *	(that is, not byte-compiled).  If an error occurs, TCL_ERROR is
 *	returned, and the interpreter result contains an error message.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "regexp" command
 *	at runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileRegexpCmd(interp, parsePtr, envPtr)
    Tcl_Interp* interp;		/* Tcl interpreter for error reporting */
    Tcl_Parse* parsePtr;	/* Points to a parse structure for
				 * the command */
    CompileEnv* envPtr;		/* Holds the resulting instructions */
{
    Tcl_Token *varTokenPtr;	/* Pointer to the Tcl_Token representing
				 * the parse of the RE or string */
    int i, len, code, exactMatch, nocase;
    char c, *str;

    /*
     * We are only interested in compiling simple regexp cases.
     * Currently supported compile cases are:
     *   regexp ?-nocase? ?--? staticString $var
     *   regexp ?-nocase? ?--? {^staticString$} $var
     */
    if (parsePtr->numWords < 3) {
	return TCL_OUT_LINE_COMPILE;
    }

    nocase = 0;
    varTokenPtr = parsePtr->tokenPtr;

    /*
     * We only look for -nocase and -- as options.  Everything else
     * gets pushed to runtime execution.  This is different than regexp's
     * runtime option handling, but satisfies our stricter needs.
     */
    for (i = 1; i < parsePtr->numWords - 2; i++) {
	varTokenPtr = varTokenPtr + (varTokenPtr->numComponents + 1);
	if (varTokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
	    /* Not a simple string - punt to runtime. */
	    return TCL_OUT_LINE_COMPILE;
	}
	str = varTokenPtr[1].start;
	len = varTokenPtr[1].size;
	if ((len == 2) && (str[0] == '-') && (str[1] == '-')) {
	    i++;
	    break;
	} else if ((len > 1)
		&& (strncmp(str, "-nocase", (unsigned) len) == 0)) {
	    nocase = 1;
	} else {
	    /* Not an option we recognize. */
	    return TCL_OUT_LINE_COMPILE;
	}
    }

    if ((parsePtr->numWords - i) != 2) {
	/* We don't support capturing to variables */
	return TCL_OUT_LINE_COMPILE;
    }

    /*
     * Get the regexp string.  If it is not a simple string, punt to runtime.
     * If it has a '-', it could be an incorrectly formed regexp command.
     */
    varTokenPtr = varTokenPtr + (varTokenPtr->numComponents + 1);
    str = varTokenPtr[1].start;
    len = varTokenPtr[1].size;
    if ((varTokenPtr->type != TCL_TOKEN_SIMPLE_WORD) || (*str == '-')) {
	return TCL_OUT_LINE_COMPILE;
    }

    if (len == 0) {
	/*
	 * The semantics of regexp are always match on re == "".
	 */
	TclEmitPush(TclRegisterLiteral(envPtr, "1", 1, 0), envPtr);
	return TCL_OK;
    }

    /*
     * On the first (pattern) arg, check to see if any RE special characters
     * are in the word.  If not, this is the same as 'string equal'.
     * We can use strchr here because the glob chars are all in the ascii-7
     * range.  If -nocase was specified, we can't do this because INST_STR_EQ
     * has no support for nocase.
     */
    
    if (Tcl_RegExpCompile(NULL, str) == NULL) {
	/*
	 * This is a bad RE.  Let it complain at runtime.
	 */
	return TCL_OUT_LINE_COMPILE;	
    }
#if 0
    if ((len > 2) && (*str == '.') && (str[1] == '*')) {
	str += 2; len -= 2;
    }
    if ((len > 2) && (str[len-3] != '\\')
	    && (str[len-2] == '.') && (str[len-1] == '*')) {
	len -= 2;
    }
#endif
    if ((len > 1) && (str[0] == '^') && (str[len-1] == '$')
	    && (str[len-2] != '\\')) {
	/*
	 * It appears and exact search was requested (ie ^foo$), so strip
	 * off the special chars and signal exactMatch.
	 */
	str++; len -= 2;
	exactMatch = 1;
    } else {
	exactMatch = 0;
    }
    c = str[len];
    str[len] = '\0';
    if (strpbrk(str, "*+?{}()[].\\|^$") != NULL) {
	str[len] = c;
	/* We don't do anything with REs with special chars yet. */
	return TCL_OUT_LINE_COMPILE;
    }
    str[len] = c;
    if (exactMatch) {
	TclEmitPush(TclRegisterLiteral(envPtr, str, len, 0), envPtr);
    } else {
	/*
	 * This needs to find the substring anywhere in the string, so
	 * use string match and *foo*.
	 */
	char *newStr  = ckalloc((unsigned) len + 3);
	newStr[0]     = '*';
	strncpy(newStr + 1, str, (size_t) len);
	newStr[len+1] = '*';
	newStr[len+2] = '\0';
	TclEmitPush(TclRegisterLiteral(envPtr, newStr, len+2, 0), envPtr);
	ckfree((char *) newStr);
    }

    /*
     * Push the string arg
     */
    varTokenPtr = varTokenPtr + (varTokenPtr->numComponents + 1);
    if (varTokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
	TclEmitPush(TclRegisterLiteral(envPtr,
		varTokenPtr[1].start, varTokenPtr[1].size, 0), envPtr);
    } else {
	code = TclCompileTokens(interp, varTokenPtr+1,
		varTokenPtr->numComponents, envPtr);
	if (code != TCL_OK) {
	    return code;
	}
    }

    if (exactMatch && !nocase) {
	TclEmitOpcode(INST_STR_EQ, envPtr);
    } else {
	TclEmitInstInt1(INST_STR_MATCH, nocase, envPtr);
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
 *	The return value is a standard Tcl result, which is TCL_OK if the
 *	compilation was successful.  If the particular return command is
 *	too complex for this function (ie, return with any flags like "-code"
 *	or "-errorinfo"), TCL_OUT_LINE_COMPILE is returned, indicating that
 *	the command should be compiled "out of line" (eg, not byte compiled).
 *	If an error occurs then the interpreter's result contains a standard
 *	error message.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "return" command
 *	at runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileReturnCmd(interp, parsePtr, envPtr)
    Tcl_Interp *interp;		/* Used for error reporting. */
    Tcl_Parse *parsePtr;	/* Points to a parse structure for the
				 * command created by Tcl_ParseCommand. */
    CompileEnv *envPtr;		/* Holds resulting instructions. */
{
    Tcl_Token *varTokenPtr;
    int code;

    /*
     * If we're not in a procedure, don't compile.
     */

    if (envPtr->procPtr == NULL) {
	return TCL_OUT_LINE_COMPILE;
    }

    switch (parsePtr->numWords) {
	case 1: {
	    /*
	     * Simple case:  [return]
	     * Just push the literal string "".
	     */
	    TclEmitPush(TclRegisterLiteral(envPtr, "", 0, 0), envPtr);
	    break;
	}
	case 2: {
	    /*
	     * More complex cases:
	     * [return "foo"]
	     * [return $value]
	     * [return [otherCmd]]
	     */
	    varTokenPtr = parsePtr->tokenPtr
		+ (parsePtr->tokenPtr->numComponents + 1);
	    if (varTokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
		/*
		 * [return "foo"] case:  the parse token is a simple word,
		 * so just push it.
		 */
		TclEmitPush(TclRegisterLiteral(envPtr, varTokenPtr[1].start,
			varTokenPtr[1].size, /*onHeap*/ 0), envPtr);
	    } else {
		/*
		 * Parse token is more complex, so compile it; this handles the
		 * variable reference and nested command cases.  If the
		 * parse token can be byte-compiled, then this instance of
		 * "return" will be byte-compiled; otherwise it will be
		 * out line compiled.
		 */
		code = TclCompileTokens(interp, varTokenPtr+1,
			varTokenPtr->numComponents, envPtr);
		if (code != TCL_OK) {
		    return code;
		}
	    }
	    break;
	}
	default: {
	    /*
	     * Most complex return cases: everything else, including
	     * [return -code error], etc.
	     */
	    return TCL_OUT_LINE_COMPILE;
	}
    }

    /*
     * The INST_DONE opcode actually causes the branching out of the
     * subroutine, and takes the top stack item as the return result
     * (which is why we pushed the value above).
     */
    TclEmitOpcode(INST_DONE, envPtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileSetCmd --
 *
 *	Procedure called to compile the "set" command.
 *
 * Results:
 *	The return value is a standard Tcl result, which is normally TCL_OK
 *	unless there was an error while parsing string. If an error occurs
 *	then the interpreter's result contains a standard error message. If
 *	complation fails because the set command requires a second level of
 *	substitutions, TCL_OUT_LINE_COMPILE is returned indicating that the
 *	set command should be compiled "out of line" by emitting code to
 *	invoke its command procedure (Tcl_SetCmd) at runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "set" command
 *	at runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileSetCmd(interp, parsePtr, envPtr)
    Tcl_Interp *interp;		/* Used for error reporting. */
    Tcl_Parse *parsePtr;	/* Points to a parse structure for the
				 * command created by Tcl_ParseCommand. */
    CompileEnv *envPtr;		/* Holds resulting instructions. */
{
    Tcl_Token *varTokenPtr, *valueTokenPtr;
    int isAssignment, isScalar, simpleVarName, localIndex, numWords;
    int code = TCL_OK;

    numWords = parsePtr->numWords;
    if ((numWords != 2) && (numWords != 3)) {
	Tcl_ResetResult(interp);
	Tcl_AppendToObj(Tcl_GetObjResult(interp),
	        "wrong # args: should be \"set varName ?newValue?\"", -1);
        return TCL_ERROR;
    }
    isAssignment = (numWords == 3);

    /*
     * Decide if we can use a frame slot for the var/array name or if we
     * need to emit code to compute and push the name at runtime. We use a
     * frame slot (entry in the array of local vars) if we are compiling a
     * procedure body and if the name is simple text that does not include
     * namespace qualifiers. 
     */

    varTokenPtr = parsePtr->tokenPtr
	    + (parsePtr->tokenPtr->numComponents + 1);

    code = TclPushVarName(interp, varTokenPtr, envPtr,
	    (isAssignment ? TCL_CREATE_VAR : 0),
	    &localIndex, &simpleVarName, &isScalar);
    if (code != TCL_OK) {
	goto done;
    }

    /*
     * If we are doing an assignment, push the new value.
     */

    if (isAssignment) {
	valueTokenPtr = varTokenPtr + (varTokenPtr->numComponents + 1);
	if (valueTokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
	    TclEmitPush(TclRegisterLiteral(envPtr, valueTokenPtr[1].start,
		    valueTokenPtr[1].size, /*onHeap*/ 0), envPtr);
	} else {
	    code = TclCompileTokens(interp, valueTokenPtr+1,
	            valueTokenPtr->numComponents, envPtr);
	    if (code != TCL_OK) {
		goto done;
	    }
	}
    }

    /*
     * Emit instructions to set/get the variable.
     */

    if (simpleVarName) {
	if (isScalar) {
	    if (localIndex >= 0) {
		if (localIndex <= 255) {
		    TclEmitInstInt1((isAssignment?
		            INST_STORE_SCALAR1 : INST_LOAD_SCALAR1),
			    localIndex, envPtr);
		} else {
		    TclEmitInstInt4((isAssignment?
			    INST_STORE_SCALAR4 : INST_LOAD_SCALAR4),
			    localIndex, envPtr);
		}
	    } else {
		TclEmitOpcode((isAssignment?
		        INST_STORE_SCALAR_STK : INST_LOAD_SCALAR_STK), envPtr);
	    }
	} else {
	    if (localIndex >= 0) {
		if (localIndex <= 255) {
		    TclEmitInstInt1((isAssignment?
		            INST_STORE_ARRAY1 : INST_LOAD_ARRAY1),
			    localIndex, envPtr);
		} else {
		    TclEmitInstInt4((isAssignment?
			    INST_STORE_ARRAY4 : INST_LOAD_ARRAY4),
			    localIndex, envPtr);
		}
	    } else {
		TclEmitOpcode((isAssignment?
		        INST_STORE_ARRAY_STK : INST_LOAD_ARRAY_STK), envPtr);
	    }
	}
    } else {
	TclEmitOpcode((isAssignment? INST_STORE_STK : INST_LOAD_STK), envPtr);
    }
	
    done:
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileStringCmd --
 *
 *	Procedure called to compile the "string" command.
 *
 * Results:
 *	The return value is a standard Tcl result, which is TCL_OK if the
 *	compilation was successful.  If the command cannot be byte-compiled,
 *	TCL_OUT_LINE_COMPILE is returned.  If an error occurs then the
 *	interpreter's result contains an error message, and TCL_ERROR is
 *	returned.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "string" command
 *	at runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileStringCmd(interp, parsePtr, envPtr)
    Tcl_Interp *interp;		/* Used for error reporting. */
    Tcl_Parse *parsePtr;	/* Points to a parse structure for the
				 * command created by Tcl_ParseCommand. */
    CompileEnv *envPtr;		/* Holds resulting instructions. */
{
    Tcl_Token *opTokenPtr, *varTokenPtr;
    Tcl_Obj *opObj;
    int index;
    int code;
    
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

    if (parsePtr->numWords < 2) {
	/* Fail at run time, not in compilation */
	return TCL_OUT_LINE_COMPILE;
    }
    opTokenPtr = parsePtr->tokenPtr
	+ (parsePtr->tokenPtr->numComponents + 1);

    opObj = Tcl_NewStringObj(opTokenPtr->start, opTokenPtr->size);
    if (Tcl_GetIndexFromObj(interp, opObj, options, "option", 0,
	    &index) != TCL_OK) {
	Tcl_DecrRefCount(opObj);
	Tcl_ResetResult(interp);
	return TCL_OUT_LINE_COMPILE;
    }
    Tcl_DecrRefCount(opObj);

    varTokenPtr = opTokenPtr + (opTokenPtr->numComponents + 1);

    switch ((enum options) index) {
	case STR_BYTELENGTH:
	case STR_FIRST:
	case STR_IS:
	case STR_LAST:
	case STR_MAP:
	case STR_RANGE:
	case STR_REPEAT:
	case STR_REPLACE:
	case STR_TOLOWER:
	case STR_TOUPPER:
	case STR_TOTITLE:
	case STR_TRIM:
	case STR_TRIMLEFT:
	case STR_TRIMRIGHT:
	case STR_WORDEND:
	case STR_WORDSTART:
	    /*
	     * All other cases: compile out of line.
	     */
	    return TCL_OUT_LINE_COMPILE;

	case STR_COMPARE: 
	case STR_EQUAL: {
	    int i;
	    /*
	     * If there are any flags to the command, we can't byte compile it
	     * because the INST_STR_EQ bytecode doesn't support flags.
	     */

	    if (parsePtr->numWords != 4) {
		return TCL_OUT_LINE_COMPILE;
	    }

	    /*
	     * Push the two operands onto the stack.
	     */

	    for (i = 0; i < 2; i++) {
		if (varTokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
		    TclEmitPush(TclRegisterLiteral(envPtr,
			    varTokenPtr[1].start, varTokenPtr[1].size,
			    0), envPtr);
		} else {
		    code = TclCompileTokens(interp, varTokenPtr+1,
			    varTokenPtr->numComponents, envPtr);
		    if (code != TCL_OK) {
			return code;
		    }
		}
		varTokenPtr = varTokenPtr + (varTokenPtr->numComponents + 1);
	    }

	    TclEmitOpcode(((((enum options) index) == STR_COMPARE) ?
		    INST_STR_CMP : INST_STR_EQ), envPtr);
	    return TCL_OK;
	}
	case STR_INDEX: {
	    int i;

	    if (parsePtr->numWords != 4) {
		/* Fail at run time, not in compilation */
		return TCL_OUT_LINE_COMPILE;
	    }

	    /*
	     * Push the two operands onto the stack.
	     */

	    for (i = 0; i < 2; i++) {
		if (varTokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
		    TclEmitPush(TclRegisterLiteral(envPtr,
			    varTokenPtr[1].start, varTokenPtr[1].size,
			    0), envPtr);
		} else {
		    code = TclCompileTokens(interp, varTokenPtr+1,
			    varTokenPtr->numComponents, envPtr);
		    if (code != TCL_OK) {
			return code;
		    }
		}
		varTokenPtr = varTokenPtr + (varTokenPtr->numComponents + 1);
	    }

	    TclEmitOpcode(INST_STR_INDEX, envPtr);
	    return TCL_OK;
	}
	case STR_LENGTH: {
	    if (parsePtr->numWords != 3) {
		/* Fail at run time, not in compilation */
		return TCL_OUT_LINE_COMPILE;
	    }

	    if (varTokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
		/*
		 * Here someone is asking for the length of a static string.
		 * Just push the actual character (not byte) length.
		 */
		char buf[TCL_INTEGER_SPACE];
		int len = Tcl_NumUtfChars(varTokenPtr[1].start,
			varTokenPtr[1].size);
		len = sprintf(buf, "%d", len);
		TclEmitPush(TclRegisterLiteral(envPtr, buf, len, 0), envPtr);
		return TCL_OK;
	    } else {
		code = TclCompileTokens(interp, varTokenPtr+1,
			varTokenPtr->numComponents, envPtr);
		if (code != TCL_OK) {
		    return code;
		}
	    }
	    TclEmitOpcode(INST_STR_LEN, envPtr);
	    return TCL_OK;
	}
	case STR_MATCH: {
	    int i, length, exactMatch = 0, nocase = 0;
	    char c, *str;

	    if (parsePtr->numWords < 4 || parsePtr->numWords > 5) {
		/* Fail at run time, not in compilation */
		return TCL_OUT_LINE_COMPILE;
	    }

	    if (parsePtr->numWords == 5) {
		if (varTokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
		    return TCL_OUT_LINE_COMPILE;
		}
		str    = varTokenPtr[1].start;
		length = varTokenPtr[1].size;
		if ((length > 1) &&
			strncmp(str, "-nocase", (size_t) length) == 0) {
		    nocase = 1;
		} else {
		    c = str[length];
		    str[length] = '\0';
		    Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
			    "bad option \"", str, "\": must be -nocase",
			    (char *) NULL);
		    str[length] = c;
		    /* Fail at run time, not in compilation */
		    return TCL_OUT_LINE_COMPILE;
		}
		varTokenPtr = varTokenPtr + (varTokenPtr->numComponents + 1);
	    }

	    for (i = 0; i < 2; i++) {
		if (varTokenPtr->type == TCL_TOKEN_SIMPLE_WORD) {
		    str = varTokenPtr[1].start;
		    length = varTokenPtr[1].size;
		    if (!nocase && (i == 0)) {
			/*
			 * On the first (pattern) arg, check to see if any
			 * glob special characters are in the word '*[]?\\'.
			 * If not, this is the same as 'string equal'.  We
			 * can use strchr here because the glob chars are all
			 * in the ascii-7 range.  If -nocase was specified,
			 * we can't do this because INST_STR_EQ has no support
			 * for nocase.
			 */
			c = str[length];
			str[length] = '\0';
			exactMatch = (strpbrk(str, "*[]?\\") == NULL);
			str[length] = c;
		    }
		    TclEmitPush(TclRegisterLiteral(envPtr, str, length,
			    0), envPtr);
		} else {
		    code = TclCompileTokens(interp, varTokenPtr+1,
			    varTokenPtr->numComponents, envPtr);
		    if (code != TCL_OK) {
			return code;
		    }
		}
		varTokenPtr = varTokenPtr + (varTokenPtr->numComponents + 1);
	    }

	    if (exactMatch) {
		TclEmitOpcode(INST_STR_EQ, envPtr);
	    } else {
		TclEmitInstInt1(INST_STR_MATCH, nocase, envPtr);
	    }
	    return TCL_OK;
	}
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileWhileCmd --
 *
 *	Procedure called to compile the "while" command.
 *
 * Results:
 *	The return value is a standard Tcl result, which is TCL_OK if
 *	compilation was successful. If an error occurs then the
 *	interpreter's result contains a standard error message and TCL_ERROR
 *	is returned. If compilation failed because the command is too
 *	complex for TclCompileWhileCmd, TCL_OUT_LINE_COMPILE is returned
 *	indicating that the while command should be compiled "out of line"
 *	by emitting code to invoke its command procedure at runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "while" command
 *	at runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileWhileCmd(interp, parsePtr, envPtr)
    Tcl_Interp *interp;		/* Used for error reporting. */
    Tcl_Parse *parsePtr;	/* Points to a parse structure for the
				 * command created by Tcl_ParseCommand. */
    CompileEnv *envPtr;		/* Holds resulting instructions. */
{
    Tcl_Token *testTokenPtr, *bodyTokenPtr;
    JumpFixup jumpEvalCondFixup;
    int testCodeOffset, bodyCodeOffset, jumpDist;
    int range, code;
    char buffer[32 + TCL_INTEGER_SPACE];
    int savedStackDepth = envPtr->currStackDepth;
    int loopMayEnd = 1;         /* This is set to 0 if it is recognized as
				 * an infinite loop. */
    int boolVal;
    char *condStart;
    char savedChar, *savedPos;

    if (parsePtr->numWords != 3) {
	Tcl_ResetResult(interp);
	Tcl_AppendToObj(Tcl_GetObjResult(interp),
	        "wrong # args: should be \"while test command\"", -1);
	return TCL_ERROR;
    }

    /*
     * If the test expression requires substitutions, don't compile the
     * while command inline. E.g., the expression might cause the loop to
     * never execute or execute forever, as in "while "$x < 5" {}".
     *
     * Bail out also if the body expression requires substitutions
     * in order to insure correct behaviour [Bug 219166]
     */

    testTokenPtr = parsePtr->tokenPtr
	    + (parsePtr->tokenPtr->numComponents + 1);
    bodyTokenPtr = testTokenPtr + (testTokenPtr->numComponents + 1);
    if ((testTokenPtr->type != TCL_TOKEN_SIMPLE_WORD)
	    || (bodyTokenPtr->type != TCL_TOKEN_SIMPLE_WORD)) {
	return TCL_OUT_LINE_COMPILE;
    }

    /*
     * Find out if the condition is a constant. 
     */

    condStart = testTokenPtr[1].start;
    savedPos = condStart + testTokenPtr[1].size - 1;

    while (*condStart == ' ') {
	condStart++;
    }
    while (*savedPos == ' ') {
	savedPos--;
    }
    savedPos++;

    savedChar = *savedPos;
    *savedPos = '\0';
    
    if (Tcl_GetBoolean(interp, condStart, &boolVal) != TCL_ERROR) { 
	if (boolVal) {
	    /*
	     * it is an infinite loop 
	     */

	    loopMayEnd = 0;  
	} else {
	    /*
	     * This is an empty loop: "while 0 {...}" or such.
	     * Compile no bytecodes.
	     */

	    *savedPos = savedChar;
	    goto pushResult;
	}
    } else {
	Tcl_ResetResult(interp);	
    }
    *savedPos = savedChar;
	
    /* 
     * Create a ExceptionRange record for the loop body. This is used to
     * implement break and continue.
     */

    envPtr->exceptDepth++;
    envPtr->maxExceptDepth =
	TclMax(envPtr->exceptDepth, envPtr->maxExceptDepth);
    range = TclCreateExceptRange(LOOP_EXCEPTION_RANGE, envPtr);

    /*
     * Jump to the evaluation of the condition. This code uses the "loop
     * rotation" optimisation (which eliminates one branch from the loop).
     * "while cond body" produces then:
     *       goto A
     *    B: body                : bodyCodeOffset
     *    A: cond -> result      : testCodeOffset, continueOffset
     *       if (result) goto B
     *
     * The infinite loop "while 1 body" produces:
     *    B: body                : all three offsets here
     *       goto B
     */

    if (loopMayEnd) {
	TclEmitForwardJump(envPtr, TCL_UNCONDITIONAL_JUMP, &jumpEvalCondFixup);
	testCodeOffset = 0; /* avoid compiler warning */
    } else {
	testCodeOffset = (envPtr->codeNext - envPtr->codeStart);
    }
    

    /*
     * Compile the loop body.
     */

    bodyCodeOffset = (envPtr->codeNext - envPtr->codeStart);
    code = TclCompileCmdWord(interp, bodyTokenPtr+1,
	    bodyTokenPtr->numComponents, envPtr);
    envPtr->currStackDepth = savedStackDepth + 1;
    if (code != TCL_OK) {
	if (code == TCL_ERROR) {
	    sprintf(buffer, "\n    (\"while\" body line %d)",
		    interp->errorLine);
            Tcl_AddObjErrorInfo(interp, buffer, -1);
        }
	goto error;
    }
    envPtr->exceptArrayPtr[range].numCodeBytes =
	    (envPtr->codeNext - envPtr->codeStart) - bodyCodeOffset;
    TclEmitOpcode(INST_POP, envPtr);

    /*
     * Compile the test expression then emit the conditional jump that
     * terminates the while. We already know it's a simple word.
     */

    if (loopMayEnd) {
	testCodeOffset = (envPtr->codeNext - envPtr->codeStart);
	jumpDist = testCodeOffset - jumpEvalCondFixup.codeOffset;
	if (TclFixupForwardJump(envPtr, &jumpEvalCondFixup, jumpDist, 127)) {
	    bodyCodeOffset += 3;
	    testCodeOffset += 3;
	}
	envPtr->currStackDepth = savedStackDepth;
	code = TclCompileExprWords(interp, testTokenPtr, 1, envPtr);
	if (code != TCL_OK) {
	    if (code == TCL_ERROR) {
		Tcl_AddObjErrorInfo(interp,
				    "\n    (\"while\" test expression)", -1);
	    }
	    goto error;
	}
	envPtr->currStackDepth = savedStackDepth + 1;
    
	jumpDist = (envPtr->codeNext - envPtr->codeStart) - bodyCodeOffset;
	if (jumpDist > 127) {
	    TclEmitInstInt4(INST_JUMP_TRUE4, -jumpDist, envPtr);
	} else {
	    TclEmitInstInt1(INST_JUMP_TRUE1, -jumpDist, envPtr);
	}
    } else {
	jumpDist = (envPtr->codeNext - envPtr->codeStart) - bodyCodeOffset;
	if (jumpDist > 127) {
	    TclEmitInstInt4(INST_JUMP4, -jumpDist, envPtr);
	} else {
	    TclEmitInstInt1(INST_JUMP1, -jumpDist, envPtr);
	}	
    }


    /*
     * Set the loop's body, continue and break offsets.
     */

    envPtr->exceptArrayPtr[range].continueOffset = testCodeOffset;
    envPtr->exceptArrayPtr[range].codeOffset = bodyCodeOffset;
    envPtr->exceptArrayPtr[range].breakOffset =
	    (envPtr->codeNext - envPtr->codeStart);
    
    /*
     * The while command's result is an empty string.
     */

    pushResult:
    envPtr->currStackDepth = savedStackDepth;
    TclEmitPush(TclRegisterLiteral(envPtr, "", 0, /*onHeap*/ 0), envPtr);
    envPtr->exceptDepth--;
    return TCL_OK;

    error:
    envPtr->exceptDepth--;
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * TclPushVarName --
 *
 *	Procedure used in the compiling where pushing a variable name
 *	is necessary (append, lappend, set).
 *
 * Results:
 *	The return value is a standard Tcl result, which is normally TCL_OK
 *	unless there was an error while parsing string. If an error occurs
 *	then the interpreter's result contains a standard error message.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the "set" command
 *	at runtime.
 *
 *----------------------------------------------------------------------
 */

static int
TclPushVarName(interp, varTokenPtr, envPtr, flags, localIndexPtr,
	simpleVarNamePtr, isScalarPtr)
    Tcl_Interp *interp;		/* Used for error reporting. */
    Tcl_Token *varTokenPtr;	/* Points to a variable token. */
    CompileEnv *envPtr;		/* Holds resulting instructions. */
    int flags;			/* takes TCL_CREATE_VAR or
				 * TCL_LARGE_INDEX_OK */
    int *localIndexPtr;		/* must not be NULL */
    int *simpleVarNamePtr;	/* must not be NULL */
    int *isScalarPtr;		/* must not be NULL */
{
    Tcl_Parse elemParse;
    int gotElemParse = 0;
    register char *p;
    char *name, *elName;
    register int i, n;
    int nameChars, elNameChars, simpleVarName, localIndex;
    int code = TCL_OK;

    /*
     * Decide if we can use a frame slot for the var/array name or if we
     * need to emit code to compute and push the name at runtime. We use a
     * frame slot (entry in the array of local vars) if we are compiling a
     * procedure body and if the name is simple text that does not include
     * namespace qualifiers. 
     */

    simpleVarName = 0;
    name = elName = NULL;
    nameChars = elNameChars = 0;
    localIndex = -1;

    /*
     * Check not only that the type is TCL_TOKEN_SIMPLE_WORD, but whether
     * curly braces surround the variable name.
     * This really matters for array elements to handle things like
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
	/* last char is ')' => potential array reference */
	if ( *(name + nameChars - 1) == ')') {
	    for (i = 0, p = name;  i < nameChars;  i++, p++) {
		if (*p == '(') {
		    elName = p + 1;
		    elNameChars = nameChars - i - 2;
		    nameChars = i ;
		    break;
		}
	    }
	}

	/*
	 * If elName contains any double quotes ("), we can't inline
	 * compile the element script using the replace '()' by '"'
	 * technique below.
	 */

	for (i = 0, p = elName;  i < elNameChars;  i++, p++) {
	    if (*p == '"') {
		simpleVarName = 0;
		break;
	    }
	}
    } else if (((n = varTokenPtr->numComponents) > 1)
	    && (varTokenPtr[1].type == TCL_TOKEN_TEXT)
            && (varTokenPtr[n].type == TCL_TOKEN_TEXT)
            && (varTokenPtr[n].start[varTokenPtr[n].size - 1] == ')')) {
        simpleVarName = 0;

        /*
	 * Check for parentheses inside first token
	 */
        for (i = 0, p = varTokenPtr[1].start; 
	     i < varTokenPtr[1].size; i++, p++) {
            if (*p == '(') {
                simpleVarName = 1;
                break;
            }
        }
        if (simpleVarName) {
            name = varTokenPtr[1].start;
            nameChars = p - varTokenPtr[1].start;
            elName = p + 1;
            elNameChars = (varTokenPtr[n].start - p) + varTokenPtr[n].size - 2;

            /*
             * If elName contains any double quotes ("), we can't inline
             * compile the element script using the replace '()' by '"'
             * technique below.
             */

            for (i = 0, p = elName;  i < elNameChars;  i++, p++) {
                if (*p == '"') {
                    simpleVarName = 0;
                    break;
                }
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
	 * Look up the var name's index in the array of local vars in the
	 * proc frame. If retrieving the var's value and it doesn't already
	 * exist, push its name and look it up at runtime.
	 */

	if ((envPtr->procPtr != NULL) && !hasNsQualifiers) {
	    localIndex = TclFindCompiledLocal(name, nameChars,
		    /*create*/ (flags & TCL_CREATE_VAR),
                    /*flags*/ ((elName==NULL)? VAR_SCALAR : VAR_ARRAY),
		    envPtr->procPtr);
	    if ((flags & TCL_NO_LARGE_INDEX) && (localIndex > 255)) {
		/* we'll push the name */
		localIndex = -1;
	    }
	}
	if (localIndex < 0) {
	    TclEmitPush(TclRegisterLiteral(envPtr, name, nameChars,
		    /*onHeap*/ 0), envPtr);
	}

	/*
	 * Compile the element script, if any.
	 */

	if (elName != NULL) {
	    /*
	     * Temporarily replace the '(' and ')' by '"'s.
	     */

	    *(elName-1) = '"';
	    *(elName+elNameChars) = '"';
	    code = Tcl_ParseCommand(interp, elName-1, elNameChars+2,
                    /*nested*/ 0, &elemParse);
	    *(elName-1) = '(';
	    *(elName+elNameChars) = ')';
	    gotElemParse = 1;
	    if ((code != TCL_OK) || (elemParse.numWords > 1)) {
		char buffer[160];
		sprintf(buffer, "\n    (parsing index for array \"%.*s\")",
		        TclMin(nameChars, 100), name);
		Tcl_AddObjErrorInfo(interp, buffer, -1);
		code = TCL_ERROR;
		goto done;
	    } else if (elemParse.numWords == 1) {
		code = TclCompileTokens(interp, elemParse.tokenPtr+1,
                        elemParse.tokenPtr->numComponents, envPtr);
		if (code != TCL_OK) {
		    goto done;
		}
	    } else {
		TclEmitPush(TclRegisterLiteral(envPtr, "", 0,
                        /*alreadyAlloced*/ 0), envPtr);
	    }
	}
    } else {
	/*
	 * The var name isn't simple: compile and push it.
	 */

	code = TclCompileTokens(interp, varTokenPtr+1,
		varTokenPtr->numComponents, envPtr);
	if (code != TCL_OK) {
	    goto done;
	}
    }

    done:
    if (gotElemParse) {
        Tcl_FreeParse(&elemParse);
    }
    *localIndexPtr	= localIndex;
    *simpleVarNamePtr	= simpleVarName;
    *isScalarPtr	= (elName == NULL);
    return code;
}
