/*
 * tclTomMath.h --
 *
 *	Interface information that comes in at the head of
 *	<tommath.h> to adapt the API to Tcl's linkage conventions.
 *
 * Copyright (c) 2005 by Kevin B. Kenny.  All rights reserved.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id$
 */

#ifndef TCLTOMMATH_H
#define TCLTOMMATH_H 1

#include <tcl.h>
#include <stdlib.h>


/* Define TOMMATH_DLLIMPORT and TOMMATH_DLLEXPORT to suit the compiler */

#ifdef STATIC_BUILD
#   define TOMMATH_DLLIMPORT
#   define TOMMATH_DLLEXPORT
#else
#   if (defined(__WIN32__) && (defined(_MSC_VER) || (__BORLANDC__ >= 0x0550) || defined(__LCC__) || defined(__WATCOMC__) || (defined(__GNUC__) && defined(__declspec))))
#	define TOMMATH_DLLIMPORT __declspec(dllimport)
#	define TOMMATH_DLLEXPORT __declspec(dllexport)
#   else
#	define TOMMATH_DLLIMPORT
#	define TOMMATH_DLLEXPORT
#   endif
#endif

/* Define TOMMATH_STORAGE_CLASS according to the build options. */

#undef TOMMATH_STORAGE_CLASS
#ifdef BUILD_tcl
#   define TOMMATH_STORAGE_CLASS TOMMATH_DLLEXPORT
#else
#   ifdef USE_TCL_STUBS
#      define TOMMATH_STORAGE_CLASS
#   else
#      define TOMMATH_STORAGE_CLASS TOMMATH_DLLIMPORT
#   endif
#endif

/* Define custom memory allocation for libtommath */

#define XMALLOC(x) TclBNAlloc(x)
#define XFREE(x) TclBNFree(x)
#define XREALLOC(x,n) TclBNRealloc(x,n)
#define XCALLOC(n,x) TclBNCalloc(n,x)
void* TclBNAlloc( size_t );
void* TclBNRealloc( void*, size_t );
void TclBNFree( void* );
void* TclBNCalloc( size_t, size_t );

/* Rename all global symboles in libtommath to avoid linkage conflicts */

#define KARATSUBA_MUL_CUTOFF TclBNKaratsubaMulCutoff
#define KARATSUBA_SQR_CUTOFF TclBNKaratsubaSqrCutoff
#define TOOM_MUL_CUTOFF TclBNToomMulCutoff
#define TOOM_SQR_CUTOFF TclBNToomSqrCutoff

#define mp_s_rmap TclBNMpSRmap

#define bn_reverse TclBN_reverse
#define fast_s_mp_mul_digs TclBN_fast_s_mp_mul_digs
#define fast_s_mp_sqr TclBN_fast_s_mp_sqr
#define mp_add TclBN_mp_add
#define mp_add_d TclBN_mp_add_d
#define mp_and TclBN_mp_and
#define mp_clamp TclBN_mp_clamp
#define mp_clear TclBN_mp_clear
#define mp_clear_multi TclBN_mp_clear_multi
#define mp_cmp TclBN_mp_cmp
#define mp_cmp_d TclBN_mp_cmp_d
#define mp_cmp_mag TclBN_mp_cmp_mag
#define mp_copy TclBN_mp_copy
#define mp_count_bits TclBN_mp_count_bits
#define mp_div TclBN_mp_div
#define mp_div_d TclBN_mp_div_d
#define mp_div_2 TclBN_mp_div_2
#define mp_div_2d TclBN_mp_div_2d
#define mp_div_3 TclBN_mp_div_3
#define mp_exch TclBN_mp_exch
#define mp_expt_d TclBN_mp_expt_d
#define mp_grow TclBN_mp_grow
#define mp_init TclBN_mp_init
#define mp_init_copy TclBN_mp_init_copy
#define mp_init_multi TclBN_mp_init_multi
#define mp_init_set TclBN_mp_init_set
#define mp_init_size TclBN_mp_init_size
#define mp_karatsuba_mul TclBN_mp_karatsuba_mul
#define mp_karatsuba_sqr TclBN_mp_karatsuba_sqr
#define mp_lshd TclBN_mp_lshd
#define mp_mod TclBN_mp_mod
#define mp_mod_2d TclBN_mp_mod_2d
#define mp_mul TclBN_mp_mul
#define mp_mul_2 TclBN_mp_mul_2
#define mp_mul_2d TclBN_mp_mul_2d
#define mp_mul_d TclBN_mp_mul_d
#define mp_neg TclBN_mp_neg
#define mp_or TclBN_mp_or
#define mp_radix_size TclBN_mp_radix_size
#define mp_read_radix TclBN_mp_read_radix
#define mp_rshd TclBN_mp_rshd
#define mp_shrink TclBN_mp_shrink
#define mp_set TclBN_mp_set
#define mp_sqr TclBN_mp_sqr
#define mp_sqrt TclBN_mp_sqrt
#define mp_sub TclBN_mp_sub
#define mp_sub_d TclBN_mp_sub_d
#define mp_to_unsigned_bin TclBN_mp_to_unsigned_bin
#define mp_to_unsigned_bin_n TclBN_mp_to_unsigned_bin_n
#define mp_toom_mul TclBN_mp_toom_mul
#define mp_toom_sqr TclBN_mp_toom_sqr
#define mp_toradix_n TclBN_mp_toradix_n
#define mp_unsigned_bin_size TclBN_mp_unsigned_bin_size
#define mp_xor TclBN_mp_xor
#define mp_zero TclBN_mp_zero
#define s_mp_add TclBN_s_mp_add
#define s_mp_mul_digs TclBN_s_mp_mul_digs
#define s_mp_sqr TclBN_s_mp_sqr
#define s_mp_sub TclBN_s_mp_sub

#endif
