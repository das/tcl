# std.tcl --
#
#  Test support package for pkg_mkIndex.
#  This is referenced by pkgIndex.tcl as a -direct script.
#
# Copyright (c) 1998 by Scriptics Corporation.
# All rights reserved.
# 
# RCS: @(#) $Id$

package provide direct1 1.0

namespace eval direct1 {
    namespace export pd1 pd2
}

proc direct1::pd1 { stg } {
    return [string tolower $stg]
}

proc direct1::pd2 { stg } {
    return [string toupper $stg]
}
