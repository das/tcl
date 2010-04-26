/*
 * tdbc.h --
 *
 *	Declarations of the public API for Tcl DataBase Connectivity (TDBC)
 *
 * Copyright (c) 2006 by Kevin B. Kenny
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id$
 *
 *-----------------------------------------------------------------------------
 */

#ifndef TDBC_H_INCLUDED
#define TDBC_H_INCLUDED 1

#include <tcl.h>

#if defined(BUILD_tdbc)
#    define	TDBCAPI 	DLLEXPORT
#    undef	USE_TDBC_STUBS
#else
#    define	TDBCAPI		DLLIMPORT
#endif

/*
 * TDBC_VERSION and TDBC_PATCHLEVEL here must match the ones that
 * appear near the top of configure.in.
 */

#define	TDBC_VERSION	"1.0b14"
#define TDBC_PATCHLEVEL "1.0b14"

/*
 * Include the Stubs declarations for the public API, generated from
 * tdbc.decls.
 */

#include "tdbcDecls.h"

#endif

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
