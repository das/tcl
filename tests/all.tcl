# all.tcl --
#
# This file contains a top-level script to run all of the Tcl
# tests.  Execute it by invoking "source all.test" when running tcltest
# in this directory.
#
# Copyright (c) 1998-1999 by Scriptics Corporation.
# Copyright (c) 2000 by Ajuba Solutions
# All rights reserved.
# 
# RCS: @(#) $Id$

set tcltestVersion [package require tcltest]
namespace import -force tcltest::*

tcltest::testsDirectory [file dir [info script]]
tcltest::runAllTests

return
