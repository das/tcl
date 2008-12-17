/*
 * tdbc.c --
 *
 *	Basic services for TDBC (Tcl DataBase Connectivity)
 *
 * Copyright (c) 2008 by Kevin B. Kenny.
 *
 * Please refer to the file, 'license.terms' for the conditions on
 * redistribution of this file and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id$
 *
 *-----------------------------------------------------------------------------
 */

#include <tcl.h>
#include "tdbcInt.h"

MODULE_SCOPE TdbcStubs tdbcStubs;

/* Table of commands to create for TDBC */

static struct TdbcCommand {
    const char* name;		/* Name of the command */
    Tcl_ObjCmdProc* proc;	/* Command procedure */
} commandTable[] = {
    { "::tdbc::tokenize", TdbcTokenizeObjCmd },
    { NULL, 		  NULL               },
};

/* Initialization script to run once the base commands are created */

static const char initScript[] = 
    "tcl_findLibrary tdbc " PACKAGE_VERSION " " PACKAGE_VERSION
    " tdbc.tcl TDBC_LIBRARY ::tdbc::Library";


/*
 *-----------------------------------------------------------------------------
 *
 * Tdbc_Init --
 *
 *	Initializes the TDBC framework when this library is loaded.
 *
 * Side effects:
 *
 *	Creates a ::tdbc namespace and a ::tdbc::Connection class
 *	from which the connection objects created by a TDBC driver
 *	may inherit.
 *
 *-----------------------------------------------------------------------------
 */

TDBCAPI int
Tdbc_Init(
    Tcl_Interp* interp		/* Tcl interpreter */
) {

    int i;

    /* Require Tcl and Tcl_OO */

    if (Tcl_InitStubs(interp, TCL_VERSION, 0) == NULL) {
	return TCL_ERROR;
    }

    /* Create the provided commands */

    for (i = 0; commandTable[i].name != NULL; ++i) {
	Tcl_CreateObjCommand(interp, commandTable[i].name, commandTable[i].proc,
			     (ClientData) NULL, (Tcl_CmdDeleteProc*) NULL);
    }

    /* Evaluate the initialization script */

    if (Tcl_EvalEx(interp, initScript, -1, TCL_EVAL_GLOBAL) != TCL_OK) {
	return TCL_ERROR;
    }

    /* Provide the TDBC package */

    if (Tcl_PkgProvideEx(interp, PACKAGE_NAME, PACKAGE_VERSION,
			 (ClientData) &tdbcStubs) == TCL_ERROR) {
	return TCL_ERROR;
    }

    /* 
     * TODO - need (a) to export a Stubs table for (among others)
     * the parser entry point, and (b) to create the parse command at
     * Tcl level.
     */

    return TCL_OK;

}
