# fixtommath.tcl --
#
#	Changes to 'tommath.h' to make it conform with Tcl's linking
#	conventions.
#
# Copyright (c) 2005 by Kevin B. Kenny.  All rights reserved.
#
# See the file "license.terms" for information on usage and redistribution
# of this file, and for a DISCLAIMER OF ALL WARRANTIES.
#
# RCS: @(#) $Id$
#
#----------------------------------------------------------------------

set f [open [lindex $argv 0] r]
set data [read $f]
close $f

foreach line [split $data \n] {
    switch -regexp -- $line {
	{#define BN_H_} {
	    puts $line
	    puts {}
	    puts "\#ifdef TCL_TOMMATH"
	    puts "\#include <tclTomMath.h>"
	    puts "\#endif"
	    puts "\#ifndef TOMMATH_STORAGE_CLASS"
	    puts "\#define TOMMATH_STORAGE_CLASS extern"
	    puts "\#endif"
	    puts "\#ifndef MODULE_SCOPE"
	    puts "\#define MODULE_SCOPE extern"
	    puts "\#endif"
	}
	{typedef.*mp_digit;} {
	    puts "\#ifndef MP_DIGIT_DECLARED"
	    puts $line
	    puts "\#define MP_DIGIT_DECLARED"
	    puts "\#endif"
	}
	{typedef struct} {
	    puts "\#ifndef MP_INT_DECLARED"
	    puts "\#define MP_INT_DECLARED"
	    puts "typedef struct mp_int mp_int;"
	    puts "\#endif"
	    puts "struct mp_int \{"
	}
	\}\ mp_int\; {
	    puts "\};"
	}
	{^(char|int|void) mp_(div_d|mul_d|clear|init|read_radix)\(} {
	    puts "TOMMATH_STORAGE_CLASS $line"
	}
	{^(char|int|void)} {
	    puts "TOMMATH_STORAGE_CLASS MODULE_SCOPE $line"
	}
	{^extern (int|const)} {
	    puts [regsub {^extern} $line "MODULE_SCOPE"]
	}
	default {
	    puts $line
	}
    }
}
