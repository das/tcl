/* 
 * tclCompExpr.c --
 *
 *	This file contains the code to compile Tcl expressions.
 *
 * Copyright (c) 1997 Sun Microsystems, Inc.
 * Copyright (c) 1998-2000 by Scriptics Corporation.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id$
 */

#include "tclInt.h"
#include "tclCompile.h"

/*
 * The stuff below is a bit of a hack so that this file can be used in
 * environments that include no UNIX, i.e. no errno: just arrange to use
 * the errno from tclExecute.c here.
 */

#ifdef TCL_GENERIC_ONLY
#define NO_ERRNO_H
#endif

#ifdef NO_ERRNO_H
extern int errno;			/* Use errno from tclExecute.c. */
#define ERANGE 34
#endif

/*
 * Boolean variable that controls whether expression compilation tracing
 * is enabled.
 */

#ifdef TCL_COMPILE_DEBUG
static int traceExprComp = 0;
#endif /* TCL_COMPILE_DEBUG */

/*
 * The ExprInfo structure describes the state of compiling an expression.
 * A pointer to an ExprInfo record is passed among the routines in
 * this module.
 */

typedef struct ExprInfo {
    Tcl_Interp *interp;		/* Used for error reporting. */
    Tcl_Parse *parsePtr;	/* Structure filled with information about
				 * the parsed expression. */
    CONST char *expr;		/* The expression that was originally passed
				 * to TclCompileExpr. */
    CONST char *lastChar;	/* Points just after last byte of expr. */
    int hasOperators;		/* Set 1 if the expr has operators; 0 if
				 * expr is only a primary. If 1 after
				 * compiling an expr, a tryCvtToNumeric
				 * instruction is emitted to convert the
				 * primary to a number if possible. */
} ExprInfo;

/*
 * Definitions of numeric codes representing each expression operator.
 * The order of these must match the entries in the operatorTable below.
 * Also the codes for the relational operators (OP_LESS, OP_GREATER, 
 * OP_LE, OP_GE, OP_EQ, and OP_NE) must be consecutive and in that order.
 * Note that OP_PLUS and OP_MINUS represent both unary and binary operators.
 */

#define OP_MULT		0
#define OP_DIVIDE	1
#define OP_MOD		2
#define OP_PLUS		3
#define OP_MINUS	4
#define OP_LSHIFT	5
#define OP_RSHIFT	6
#define OP_LESS		7
#define OP_GREATER	8
#define OP_LE		9
#define OP_GE		10
#define OP_EQ		11
#define OP_NEQ		12
#define OP_BITAND	13
#define OP_BITXOR	14
#define OP_BITOR	15
#define OP_LAND		16
#define OP_LOR		17
#define OP_QUESTY	18
#define OP_LNOT		19
#define OP_BITNOT	20
#define OP_STREQ	21
#define OP_STRNEQ	22
#define OP_EXPON	23
#define OP_IN_LIST	24
#define OP_NOT_IN_LIST	25

/*
 * Table describing the expression operators. Entries in this table must
 * correspond to the definitions of numeric codes for operators just above.
 */

static int opTableInitialized = 0; /* 0 means not yet initialized. */

TCL_DECLARE_MUTEX(opMutex)

typedef struct OperatorDesc {
    char *name;			/* Name of the operator. */
    int numOperands;		/* Number of operands. 0 if the operator
				 * requires special handling. */
    int instruction;		/* Instruction opcode for the operator.
				 * Ignored if numOperands is 0. */
} OperatorDesc;

static OperatorDesc operatorTable[] = {
    {"*",   2,  INST_MULT},
    {"/",   2,  INST_DIV},
    {"%",   2,  INST_MOD},
    {"+",   0}, 
    {"-",   0},
    {"<<",  2,  INST_LSHIFT},
    {">>",  2,  INST_RSHIFT},
    {"<",   2,  INST_LT},
    {">",   2,  INST_GT},
    {"<=",  2,  INST_LE},
    {">=",  2,  INST_GE},
    {"==",  2,  INST_EQ},
    {"!=",  2,  INST_NEQ},
    {"&",   2,  INST_BITAND},
    {"^",   2,  INST_BITXOR},
    {"|",   2,  INST_BITOR},
    {"&&",  0},
    {"||",  0},
    {"?",   0},
    {"!",   1,  INST_LNOT},
    {"~",   1,  INST_BITNOT},
    {"eq",  2,  INST_STR_EQ},
    {"ne",  2,  INST_STR_NEQ},
    {"**",  2,	INST_EXPON},
    {"in",  2,	INST_LIST_IN},
    {"ni",  2,	INST_LIST_NOT_IN},
    {NULL}
};

/*
 * Hashtable used to map the names of expression operators to the index
 * of their OperatorDesc description.
 */

static Tcl_HashTable opHashTable;

/*
 * Declarations for local procedures to this file:
 */

static int		CompileCondExpr _ANSI_ARGS_((
			    Tcl_Token *exprTokenPtr, ExprInfo *infoPtr,
			    CompileEnv *envPtr, Tcl_Token **endPtrPtr));
static int		CompileLandOrLorExpr _ANSI_ARGS_((
			    Tcl_Token *exprTokenPtr, int opIndex,
			    ExprInfo *infoPtr, CompileEnv *envPtr,
			    Tcl_Token **endPtrPtr));
static int		CompileMathFuncCall _ANSI_ARGS_((
			    Tcl_Token *exprTokenPtr, CONST char *funcName,
			    ExprInfo *infoPtr, CompileEnv *envPtr,
			    Tcl_Token **endPtrPtr));
static int		CompileSubExpr _ANSI_ARGS_((
			    Tcl_Token *exprTokenPtr, ExprInfo *infoPtr,
			    CompileEnv *envPtr));
static void		LogSyntaxError _ANSI_ARGS_((ExprInfo *infoPtr));

/*
 * Macro used to debug the execution of the expression compiler.
 */

#ifdef TCL_COMPILE_DEBUG
#define TRACE(exprBytes, exprLength, tokenBytes, tokenLength) \
    if (traceExprComp) { \
	fprintf(stderr, "CompileSubExpr: \"%.*s\", token \"%.*s\"\n", \
	        (exprLength), (exprBytes), (tokenLength), (tokenBytes)); \
    }
#else
#define TRACE(exprBytes, exprLength, tokenBytes, tokenLength)
#endif /* TCL_COMPILE_DEBUG */

/*
 *----------------------------------------------------------------------
 *
 * TclCompileExpr --
 *
 *	This procedure compiles a string containing a Tcl expression into
 *	Tcl bytecodes. This procedure is the top-level interface to the
 *	the expression compilation module, and is used by such public
 *	procedures as Tcl_ExprString, Tcl_ExprStringObj, Tcl_ExprLong,
 *	Tcl_ExprDouble, Tcl_ExprBoolean, and Tcl_ExprBooleanObj.
 *
 * Results:
 *	The return value is TCL_OK on a successful compilation and TCL_ERROR
 *	on failure. If TCL_ERROR is returned, then the interpreter's result
 *	contains an error message.
 *
 * Side effects:
 *	Adds instructions to envPtr to evaluate the expression at runtime.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileExpr(interp, script, numBytes, envPtr)
    Tcl_Interp *interp;		/* Used for error reporting. */
    CONST char *script;		/* The source script to compile. */
    int numBytes;		/* Number of bytes in script. If < 0, the
				 * string consists of all bytes up to the
				 * first null character. */
    CompileEnv *envPtr;		/* Holds resulting instructions. */
{
    ExprInfo info;
    Tcl_Parse parse;
    Tcl_HashEntry *hPtr;
    int new, i, code;

    /*
     * If this is the first time we've been called, initialize the table
     * of expression operators.
     */

    if (numBytes < 0) {
	numBytes = (script? strlen(script) : 0);
    }
    if (!opTableInitialized) {
	Tcl_MutexLock(&opMutex);
	if (!opTableInitialized) {
	    Tcl_InitHashTable(&opHashTable, TCL_STRING_KEYS);
	    for (i = 0;  operatorTable[i].name != NULL;  i++) {
		hPtr = Tcl_CreateHashEntry(&opHashTable,
			operatorTable[i].name, &new);
		if (new) {
		    Tcl_SetHashValue(hPtr, (ClientData) i);
		}
	    }
	    opTableInitialized = 1;
	}
	Tcl_MutexUnlock(&opMutex);
    }

    /*
     * Initialize the structure containing information abvout this
     * expression compilation.
     */

    info.interp = interp;
    info.parsePtr = &parse;
    info.expr = script;
    info.lastChar = (script + numBytes); 
    info.hasOperators = 0;

    /*
     * Parse the expression then compile it.
     */

    code = Tcl_ParseExpr(interp, script, numBytes, &parse);
    if (code != TCL_OK) {
	goto done;
    }

    code = CompileSubExpr(parse.tokenPtr, &info, envPtr);
    if (code != TCL_OK) {
	Tcl_FreeParse(&parse);
	goto done;
    }
    
    if (!info.hasOperators) {
	/*
	 * Attempt to convert the primary's object to an int or double.
	 * This is done in order to support Tcl's policy of interpreting
	 * operands if at all possible as first integers, else
	 * floating-point numbers.
	 */
	
	TclEmitOpcode(INST_TRY_CVT_TO_NUMERIC, envPtr);
    }
    Tcl_FreeParse(&parse);

    done:
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * TclFinalizeCompilation --
 *
 *	Clean up the compilation environment so it can later be
 *	properly reinitialized. This procedure is called by
 *	TclFinalizeCompExecEnv() in tclObj.c, which in turn is called
 *	by Tcl_Finalize().
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Cleans up the compilation environment. At the moment, just the
 *	table of expression operators is freed.
 *
 *----------------------------------------------------------------------
 */

void
TclFinalizeCompilation()
{
    Tcl_MutexLock(&opMutex);
    if (opTableInitialized) {
        Tcl_DeleteHashTable(&opHashTable);
        opTableInitialized = 0;
    }
    Tcl_MutexUnlock(&opMutex);
}

/*
 *----------------------------------------------------------------------
 *
 * CompileSubExpr --
 *
 *	Given a pointer to a TCL_TOKEN_SUB_EXPR token describing a
 *	subexpression, this procedure emits instructions to evaluate the
 *	subexpression at runtime.
 *
 * Results:
 *	The return value is TCL_OK on a successful compilation and TCL_ERROR
 *	on failure. If TCL_ERROR is returned, then the interpreter's result
 *	contains an error message.
 *
 * Side effects:
 *	Adds instructions to envPtr to evaluate the subexpression.
 *
 *----------------------------------------------------------------------
 */

static int
CompileSubExpr(exprTokenPtr, infoPtr, envPtr)
    Tcl_Token *exprTokenPtr;	/* Points to TCL_TOKEN_SUB_EXPR token
				 * to compile. */
    ExprInfo *infoPtr;		/* Describes the compilation state for the
				 * expression being compiled. */
    CompileEnv *envPtr;		/* Holds resulting instructions. */
{
    Tcl_Interp *interp = infoPtr->interp;
    Tcl_Token *tokenPtr, *endPtr, *afterSubexprPtr;
    OperatorDesc *opDescPtr;
    Tcl_HashEntry *hPtr;
    CONST char *operator;
    Tcl_DString opBuf;
    int objIndex, opIndex, length, code;
    char buffer[TCL_UTF_MAX];

    if (exprTokenPtr->type != TCL_TOKEN_SUB_EXPR) {
	Tcl_Panic("CompileSubExpr: token type %d not TCL_TOKEN_SUB_EXPR\n",
	        exprTokenPtr->type);
    }
    code = TCL_OK;

    /*
     * Switch on the type of the first token after the subexpression token.
     * After processing it, advance tokenPtr to point just after the
     * subexpression's last token.
     */
    
    tokenPtr = exprTokenPtr+1;
    TRACE(exprTokenPtr->start, exprTokenPtr->size,
	    tokenPtr->start, tokenPtr->size);
    switch (tokenPtr->type) {
        case TCL_TOKEN_WORD:
	    TclCompileTokens(interp, tokenPtr+1,
	            tokenPtr->numComponents, envPtr);
	    tokenPtr += (tokenPtr->numComponents + 1);
	    break;
	    
        case TCL_TOKEN_TEXT:
	    if (tokenPtr->size > 0) {
		objIndex = TclRegisterNewLiteral(envPtr, tokenPtr->start,
	                tokenPtr->size);
	    } else {
		objIndex = TclRegisterNewLiteral(envPtr, "", 0);
	    }
	    TclEmitPush(objIndex, envPtr);
	    tokenPtr += 1;
	    break;
	    
        case TCL_TOKEN_BS:
	    length = Tcl_UtfBackslash(tokenPtr->start, (int *) NULL,
		    buffer);
	    if (length > 0) {
		objIndex = TclRegisterNewLiteral(envPtr, buffer, length);
	    } else {
		objIndex = TclRegisterNewLiteral(envPtr, "", 0);
	    }
	    TclEmitPush(objIndex, envPtr);
	    tokenPtr += 1;
	    break;
	    
        case TCL_TOKEN_COMMAND:
	    TclCompileScript(interp, tokenPtr->start+1,
		    tokenPtr->size-2, envPtr);
	    tokenPtr += 1;
	    break;
	    
        case TCL_TOKEN_VARIABLE:
	    TclCompileTokens(interp, tokenPtr, 1, envPtr);
	    tokenPtr += (tokenPtr->numComponents + 1);
	    break;
	    
        case TCL_TOKEN_SUB_EXPR:
	    code = CompileSubExpr(tokenPtr, infoPtr, envPtr);
	    if (code != TCL_OK) {
		goto done;
	    }
	    tokenPtr += (tokenPtr->numComponents + 1);
	    break;
	    
        case TCL_TOKEN_OPERATOR:
	    /*
	     * Look up the operator.  If the operator isn't found, treat it
	     * as a math function.
	     */
	    Tcl_DStringInit(&opBuf);
	    operator = Tcl_DStringAppend(&opBuf, 
		    tokenPtr->start, tokenPtr->size);
	    hPtr = Tcl_FindHashEntry(&opHashTable, operator);
	    if (hPtr == NULL) {
		code = CompileMathFuncCall(exprTokenPtr, operator, infoPtr,
			envPtr, &endPtr);
		Tcl_DStringFree(&opBuf);
		if (code != TCL_OK) {
		    goto done;
		}
		tokenPtr = endPtr;
		break;
	    }
	    Tcl_DStringFree(&opBuf);
	    opIndex = (int) Tcl_GetHashValue(hPtr);
	    opDescPtr = &(operatorTable[opIndex]);

	    /*
	     * If the operator is "normal", compile it using information
	     * from the operator table.
	     */

	    if (opDescPtr->numOperands > 0) {
		tokenPtr++;
		code = CompileSubExpr(tokenPtr, infoPtr, envPtr);
		if (code != TCL_OK) {
		    goto done;
		}
		tokenPtr += (tokenPtr->numComponents + 1);

		if (opDescPtr->numOperands == 2) {
		    code = CompileSubExpr(tokenPtr, infoPtr, envPtr);
		    if (code != TCL_OK) {
			goto done;
		    }
		    tokenPtr += (tokenPtr->numComponents + 1);
		}
		TclEmitOpcode(opDescPtr->instruction, envPtr);
		infoPtr->hasOperators = 1;
		break;
	    }
	    
	    /*
	     * The operator requires special treatment, and is either
	     * "+" or "-", or one of "&&", "||" or "?".
	     */
	    
	    switch (opIndex) {
	        case OP_PLUS:
	        case OP_MINUS:
		    tokenPtr++;
		    code = CompileSubExpr(tokenPtr, infoPtr, envPtr);
		    if (code != TCL_OK) {
			goto done;
		    }
		    tokenPtr += (tokenPtr->numComponents + 1);
		    
		    /*
		     * Check whether the "+" or "-" is unary.
		     */
		    
		    afterSubexprPtr = exprTokenPtr
			    + exprTokenPtr->numComponents+1;
		    if (tokenPtr == afterSubexprPtr) {
			TclEmitOpcode(((opIndex==OP_PLUS)?
			        INST_UPLUS : INST_UMINUS),
			        envPtr);
			break;
		    }
		    
		    /*
		     * The "+" or "-" is binary.
		     */
		    
		    code = CompileSubExpr(tokenPtr, infoPtr, envPtr);
		    if (code != TCL_OK) {
			goto done;
		    }
		    tokenPtr += (tokenPtr->numComponents + 1);
		    TclEmitOpcode(((opIndex==OP_PLUS)? INST_ADD : INST_SUB),
			    envPtr);
		    break;

	        case OP_LAND:
	        case OP_LOR:
		    code = CompileLandOrLorExpr(exprTokenPtr, opIndex,
			    infoPtr, envPtr, &endPtr);
		    if (code != TCL_OK) {
			goto done;
		    }
		    tokenPtr = endPtr;
		    break;
			
	        case OP_QUESTY:
		    code = CompileCondExpr(exprTokenPtr, infoPtr,
			    envPtr, &endPtr);
		    if (code != TCL_OK) {
			goto done;
		    }
		    tokenPtr = endPtr;
		    break;
		    
		default:
		    Tcl_Panic("CompileSubExpr: unexpected operator %d requiring special treatment\n",
		        opIndex);
	    } /* end switch on operator requiring special treatment */
	    infoPtr->hasOperators = 1;
	    break;

        default:
	    Tcl_Panic("CompileSubExpr: unexpected token type %d\n",
	            tokenPtr->type);
    }

    /*
     * Verify that the subexpression token had the required number of
     * subtokens: that we've advanced tokenPtr just beyond the
     * subexpression's last token. For example, a "*" subexpression must
     * contain the tokens for exactly two operands.
     */
    
    if (tokenPtr != (exprTokenPtr + exprTokenPtr->numComponents+1)) {
	LogSyntaxError(infoPtr);
	code = TCL_ERROR;
    }
    
    done:
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * CompileLandOrLorExpr --
 *
 *	This procedure compiles a Tcl logical and ("&&") or logical or
 *	("||") subexpression.
 *
 * Results:
 *	The return value is TCL_OK on a successful compilation and TCL_ERROR
 *	on failure. If TCL_OK is returned, a pointer to the token just after
 *	the last one in the subexpression is stored at the address in
 *	endPtrPtr. If TCL_ERROR is returned, then the interpreter's result
 *	contains an error message.
 *
 * Side effects:
 *	Adds instructions to envPtr to evaluate the expression at runtime.
 *
 *----------------------------------------------------------------------
 */

static int
CompileLandOrLorExpr(exprTokenPtr, opIndex, infoPtr, envPtr, endPtrPtr)
    Tcl_Token *exprTokenPtr;	 /* Points to TCL_TOKEN_SUB_EXPR token
				  * containing the "&&" or "||" operator. */
    int opIndex;		 /* A code describing the expression
				  * operator: either OP_LAND or OP_LOR. */
    ExprInfo *infoPtr;		 /* Describes the compilation state for the
				  * expression being compiled. */
    CompileEnv *envPtr;		 /* Holds resulting instructions. */
    Tcl_Token **endPtrPtr;	 /* If successful, a pointer to the token
				  * just after the last token in the
				  * subexpression is stored here. */
{
    JumpFixup shortCircuitFixup; /* Used to fix up the short circuit jump
				  * after the first subexpression. */
    JumpFixup shortCircuitFixup2;/* Used to fix up the second jump to the
				  * short-circuit target. */
    JumpFixup endFixup;          /* Used to fix up jump to the end. */
    Tcl_Token *tokenPtr;
    int code;
    int savedStackDepth = envPtr->currStackDepth;

    /*
     * Emit code for the first operand.
     */

    tokenPtr = exprTokenPtr+2;
    code = CompileSubExpr(tokenPtr, infoPtr, envPtr);
    if (code != TCL_OK) {
	goto done;
    }
    tokenPtr += (tokenPtr->numComponents + 1);

    /*
     * Emit the short-circuit jump.
     */

    TclEmitForwardJump(envPtr,
	    ((opIndex==OP_LAND)? TCL_FALSE_JUMP : TCL_TRUE_JUMP),
	    &shortCircuitFixup);

    /*
     * Emit code for the second operand.
     */

    code = CompileSubExpr(tokenPtr, infoPtr, envPtr);
    if (code != TCL_OK) {
	goto done;
    }
    tokenPtr += (tokenPtr->numComponents + 1);
	    
    /*
     * The result is the boolean value of the second operand. We 
     * code this in a somewhat contorted manner to be able to reuse
     * the shortCircuit value and save one INST_JUMP.
     */

    TclEmitForwardJump(envPtr,
	    ((opIndex==OP_LAND)? TCL_FALSE_JUMP : TCL_TRUE_JUMP),
	    &shortCircuitFixup2);

    if (opIndex == OP_LAND) {
	TclEmitPush(TclRegisterNewLiteral(envPtr, "1", 1), envPtr);
    } else {
	TclEmitPush(TclRegisterNewLiteral(envPtr, "0", 1), envPtr);
    }
    TclEmitForwardJump(envPtr, TCL_UNCONDITIONAL_JUMP, &endFixup);

    /*
     * Fixup the short-circuit jumps and push the shortCircuit value.
     * Note that shortCircuitFixup2 is always a short jump.
     */

    TclFixupForwardJumpToHere(envPtr, &shortCircuitFixup2, 127);
    if (TclFixupForwardJumpToHere(envPtr, &shortCircuitFixup, 127)) {
	/*
	 * shortCircuit jump grown by 3 bytes: update endFixup.
	 */
	    
	 endFixup.codeOffset += 3;
    }

    if (opIndex == OP_LAND) {
	TclEmitPush(TclRegisterNewLiteral(envPtr, "0", 1), envPtr);
    } else {
	TclEmitPush(TclRegisterNewLiteral(envPtr, "1", 1), envPtr);
    }

    TclFixupForwardJumpToHere(envPtr, &endFixup, 127);
    *endPtrPtr = tokenPtr;

    done:
    envPtr->currStackDepth = savedStackDepth + 1;
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * CompileCondExpr --
 *
 *	This procedure compiles a Tcl conditional expression:
 *	condExpr ::= lorExpr ['?' condExpr ':' condExpr]
 *
 * Results:
 *	The return value is TCL_OK on a successful compilation and TCL_ERROR
 *	on failure. If TCL_OK is returned, a pointer to the token just after
 *	the last one in the subexpression is stored at the address in
 *	endPtrPtr. If TCL_ERROR is returned, then the interpreter's result
 *	contains an error message.
 *
 * Side effects:
 *	Adds instructions to envPtr to evaluate the expression at runtime.
 *
 *----------------------------------------------------------------------
 */

static int
CompileCondExpr(exprTokenPtr, infoPtr, envPtr, endPtrPtr)
    Tcl_Token *exprTokenPtr;	/* Points to TCL_TOKEN_SUB_EXPR token
				 * containing the "?" operator. */
    ExprInfo *infoPtr;		/* Describes the compilation state for the
				 * expression being compiled. */
    CompileEnv *envPtr;		/* Holds resulting instructions. */
    Tcl_Token **endPtrPtr;	/* If successful, a pointer to the token
				 * just after the last token in the
				 * subexpression is stored here. */
{
    JumpFixup jumpAroundThenFixup, jumpAroundElseFixup;
				/* Used to update or replace one-byte jumps
				 * around the then and else expressions when
				 * their target PCs are determined. */
    Tcl_Token *tokenPtr;
    int elseCodeOffset, dist, code;
    int savedStackDepth = envPtr->currStackDepth;

    /*
     * Emit code for the test.
     */

    tokenPtr = exprTokenPtr+2;
    code = CompileSubExpr(tokenPtr, infoPtr, envPtr);
    if (code != TCL_OK) {
	goto done;
    }
    tokenPtr += (tokenPtr->numComponents + 1);
    
    /*
     * Emit the jump to the "else" expression if the test was false.
     */
    
    TclEmitForwardJump(envPtr, TCL_FALSE_JUMP, &jumpAroundThenFixup);

    /*
     * Compile the "then" expression. Note that if a subexpression is only
     * a primary, we need to try to convert it to numeric. We do this to
     * support Tcl's policy of interpreting operands if at all possible as
     * first integers, else floating-point numbers.
     */

    infoPtr->hasOperators = 0;
    code = CompileSubExpr(tokenPtr, infoPtr, envPtr);
    if (code != TCL_OK) {
	goto done;
    }
    tokenPtr += (tokenPtr->numComponents + 1);
    if (!infoPtr->hasOperators) {
	TclEmitOpcode(INST_TRY_CVT_TO_NUMERIC, envPtr);
    }

    /*
     * Emit an unconditional jump around the "else" condExpr.
     */
    
    TclEmitForwardJump(envPtr, TCL_UNCONDITIONAL_JUMP,
	    &jumpAroundElseFixup);

    /*
     * Compile the "else" expression.
     */

    envPtr->currStackDepth = savedStackDepth;
    elseCodeOffset = (envPtr->codeNext - envPtr->codeStart);
    infoPtr->hasOperators = 0;
    code = CompileSubExpr(tokenPtr, infoPtr, envPtr);
    if (code != TCL_OK) {
	goto done;
    }
    tokenPtr += (tokenPtr->numComponents + 1);
    if (!infoPtr->hasOperators) {
	TclEmitOpcode(INST_TRY_CVT_TO_NUMERIC, envPtr);
    }

    /*
     * Fix up the second jump around the "else" expression.
     */

    dist = (envPtr->codeNext - envPtr->codeStart)
	    - jumpAroundElseFixup.codeOffset;
    if (TclFixupForwardJump(envPtr, &jumpAroundElseFixup, dist, 127)) {
	/*
	 * Update the else expression's starting code offset since it
	 * moved down 3 bytes too.
	 */
	
	elseCodeOffset += 3;
    }
	
    /*
     * Fix up the first jump to the "else" expression if the test was false.
     */
    
    dist = (elseCodeOffset - jumpAroundThenFixup.codeOffset);
    TclFixupForwardJump(envPtr, &jumpAroundThenFixup, dist, 127);
    *endPtrPtr = tokenPtr;

    done:
    envPtr->currStackDepth = savedStackDepth + 1;
    return code;
}

/*
 *----------------------------------------------------------------------
 *
 * CompileMathFuncCall --
 *
 *	This procedure compiles a call on a math function in an expression:
 *	mathFuncCall ::= funcName '(' [condExpr {',' condExpr}] ')'
 *
 * Results:
 *	The return value is TCL_OK on a successful compilation and TCL_ERROR
 *	on failure. If TCL_OK is returned, a pointer to the token just after
 *	the last one in the subexpression is stored at the address in
 *	endPtrPtr. If TCL_ERROR is returned, then the interpreter's result
 *	contains an error message.
 *
 * Side effects:
 *	Adds instructions to envPtr to evaluate the math function at
 *	runtime.
 *
 *----------------------------------------------------------------------
 */

static int
CompileMathFuncCall(exprTokenPtr, funcName, infoPtr, envPtr, endPtrPtr)
    Tcl_Token *exprTokenPtr;	/* Points to TCL_TOKEN_SUB_EXPR token
				 * containing the math function call. */
    CONST char *funcName;	/* Name of the math function. */
    ExprInfo *infoPtr;		/* Describes the compilation state for the
				 * expression being compiled. */
    CompileEnv *envPtr;		/* Holds resulting instructions. */
    Tcl_Token **endPtrPtr;	/* If successful, a pointer to the token
				 * just after the last token in the
				 * subexpression is stored here. */
{
    Tcl_DString cmdName;
    int objIndex;
    Tcl_Token *tokenPtr, *afterSubexprPtr;
    int argCount;
    int code = TCL_OK;
		       
    /*
     * Prepend "tcl::mathfunc::" to the function name, to produce the 
     * name of a command that evaluates the function.  Push that
     * command name on the stack, in a literal registered to the
     * namespace so that resolution can be cached.
     */

    Tcl_DStringInit( &cmdName );
    Tcl_DStringAppend( &cmdName, "tcl::mathfunc::", -1 );
    Tcl_DStringAppend( &cmdName, funcName, -1 );
    objIndex = TclRegisterNewNSLiteral( envPtr,
					 Tcl_DStringValue( &cmdName ),
					 Tcl_DStringLength( &cmdName ) );
    TclEmitPush( objIndex, envPtr );
    Tcl_DStringFree( &cmdName );

    /*
     * Compile any arguments for the function.
     */

    argCount = 1;
    tokenPtr = exprTokenPtr+2;
    afterSubexprPtr = exprTokenPtr + (exprTokenPtr->numComponents + 1);
    while (tokenPtr != afterSubexprPtr) {
	++argCount;
	code = CompileSubExpr(tokenPtr, infoPtr, envPtr);
	if (code != TCL_OK) {
	    return code;
	}
	tokenPtr += (tokenPtr->numComponents + 1);
    }
    
    /* Invoke the function */

    if ( argCount < 255 ) {
	TclEmitInstInt1( INST_INVOKE_STK1, argCount, envPtr );
    } else {
	TclEmitInstInt4( INST_INVOKE_STK4, argCount, envPtr );
    }

    *endPtrPtr = afterSubexprPtr;
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * LogSyntaxError --
 *
 *	This procedure is invoked after an error occurs when compiling an
 *	expression. It sets the interpreter result to an error message
 *	describing the error.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Sets the interpreter result to an error message describing the
 *	expression that was being compiled when the error occurred.
 *
 *----------------------------------------------------------------------
 */

static void
LogSyntaxError(infoPtr)
    ExprInfo *infoPtr;		/* Describes the compilation state for the
				 * expression being compiled. */
{
    Tcl_Obj *result =
            Tcl_NewStringObj("syntax error in expression \"", -1);
    TclAppendLimitedToObj(result, infoPtr->expr,
            (int)(infoPtr->lastChar - infoPtr->expr), 60, "");
    Tcl_AppendToObj(result, "\"", -1);
    Tcl_SetObjResult(infoPtr->interp, result);
}
