# tdbc.tcl --
#
#	Definitions of base classes from which TDBC drivers' connections,
#	statements and result sets may inherit.
#
# Copyright (c) 2008 by Kevin B. Kenny
# See the file "license.terms" for information on usage and redistribution
# of this file, and for a DISCLAIMER OF ALL WARRANTIES.
#
# RCS: @(#) $Id$
#
#------------------------------------------------------------------------------

package require TclOO

namespace eval ::tdbc {
    namespace export connection statement resultset
    variable generalError [list TDBC GENERAL_ERROR HY000 {}]
}

#------------------------------------------------------------------------------
#
# tdbc::ParseConvenienceArgs --
#
#	Parse the convenience arguments to a TDBC 'execute', 
#	'executewithdictionary', or 'foreach' call.
#
# Parameters:
#	argv - Arguments to the call
#	optsVar -- Name of a variable in caller's scope that will receive
#		   a dictionary of the supplied options
#
# Results:
#	Returns any args remaining after parsing the options.
#
# Side effects:
#	Sets the 'opts' dictionary to the options.
#
#------------------------------------------------------------------------------

proc tdbc::ParseConvenienceArgs {argv optsVar} {

    variable generalError
    upvar 1 $optsVar opts

    set opts [dict create -as dicts]
    set i 0
    
    # Munch keyword options off the front of the command arguments
    
    foreach {key value} $argv {
	if {[string index $key 0] eq {-}} {
	    switch -regexp -- $key {
		-as? {
		    if {$value ne {dicts} && $value ne {lists}} {
			set errorcode $generalError
			lappend errorcode badVarType $value
			return -code error \
			    -errorcode $errorcode \
			    "bad variable type \"$value\":\
                             must be lists or dicts"
		    }
		    dict set opts -as $value
		}
		-c(?:o(?:l(?:u(?:m(?:n(?:s(?:v(?:a(?:r(?:i(?:a(?:b(?:le?)?)?)?)?)?)?)?)?)?)?)?)?) {
		    dict set opts -columnsvariable $value
		}
		-- {
		    incr i
		    break
		}
		default {
		    set errorcode $generalError
		    lappend errorcode badOption $key
		    return -code error \
			-errorcode $errorcode \
			"bad option \"$key\":\
                             must be -as or -columnsvariable"
		}
	    }
	} else {
	    break
	}
	incr i 2
    }

    return [lrange $argv[set argv {}] $i end]
    
}



#------------------------------------------------------------------------------
#
# tdbc::connection --
#
#	Class that represents a generic connection to a database.
#
#-----------------------------------------------------------------------------

oo::class create ::tdbc::connection {

    # statementSeq is the sequence number of the last statement created.
    # statementClass is the name of the class that implements the
    #	'statement' API.

    variable statementSeq

    # The base class constructor accepts no arguments.  It sets up the
    # machinery to do the bookkeeping to keep track of what statements
    # are associated with the connection.  The derived class constructor
    # is expected to set the variable, 'statementClass' to the name
    # of the class that represents statements, so that the 'prepare'
    # method can invoke it.

    constructor {} {
	set statementSeq 0
	namespace eval Stmt {}
    }

    # The 'close' method is simply an alternative syntax for destroying
    # the connection.

    method close {} {
	my destroy
    }

    # The 'prepare' method creates a new statement against the connection,
    # giving its constructor the current statement and the SQL code to
    # prepare.  It uses the 'statementClass' variable set by the constructor
    # to get the class to instantiate.

    method prepare {sqlcode} {
	return [my statementCreate Stmt::[incr statementSeq] [self] $sqlcode]
    }

    # The 'statementCreate' method delegates to the constructor
    # of the class specified by the 'statementClass' variable. It's
    # intended for drivers designed before tdbc 1.0b10. Current ones
    # should forward this method to the constructor directly.

    method statementCreate {name instance sqlcode} {
	my variable statementClass
	return [$statementClass create $name $instance $sqlcode]
    }

    # Derived classes are expected to implement the 'prepareCall' method,
    # and have it call 'prepare' as needed (or do something else and
    # install the resulting statement)

    # The 'statements' method lists the statements active against this 
    # connection.

    method statements {} {
	info commands Stmt::*
    }

    # The 'resultsets' method lists the result sets active against this
    # connection.

    method resultsets {} {
	set retval {}
	foreach statement [my statements] {
	    foreach resultset [$statement resultsets] {
		lappend retval $resultset
	    }
	}
	return $retval
    }

    # The 'transaction' method executes a block of Tcl code as an
    # ACID transaction against the database.

    method transaction {script} {
	my begintransaction
	set status [catch {uplevel 1 $script} result options]
	switch -exact -- $status {
	    0 {
		my commit
	    }
	    2 - 3 - 4 {
		set options [dict merge {-level 1} $options[set options {}]]
		dict incr options -level
		my commit
	    }
	    default {
		my rollback
	    }
	}
	return -options $options $result
    }

    # The 'allrows' method prepares a statement, then executes it with
    # a given set of substituents, returning a list of all the rows
    # that the statement returns. Optionally, it stores the names of
    # the columns in '-columnsvariable'.
    # Usage:
    #     $db allrows ?-as lists|dicts? ?-columnsvariable varName? ?--?
    #	      sql ?dictionary?

    method allrows args {

	variable ::tdbc::generalError

	# Grab keyword-value parameters

	set args [::tdbc::ParseConvenienceArgs $args[set args {}] opts]

	# Check postitional parameters 

	set cmd [list [self] prepare]
	if {[llength $args] == 1} {
	    set sqlcode [lindex $args 0]
	} elseif {[llength $args] == 2} {
	    lassign $args sqlcode dict
	} else {
	    set errorcode $generalError
	    lappend errorcode wrongNumArgs
	    return -code error -errorcode $errorcode \
		"wrong # args: should be [lrange [info level 0] 0 1]\
                 ?-option value?... ?--? sqlcode ?dictionary?"
	}
	lappend cmd $sqlcode

	# Prepare the statement

	set stmt [uplevel 1 $cmd]

	# Delegate to the statement to accumulate the results

	set cmd [list $stmt allrows {*}$opts --]
	if {[info exists dict]} {
	    lappend cmd $dict
	}
	set status [catch {
	    uplevel 1 $cmd
	} result options]

	# Destroy the statement

	catch {
	    $stmt close
	}

	return -options $options $result
    }

    # The 'foreach' method prepares a statement, then executes it with
    # a supplied set of substituents.  For each row of the result,
    # it sets a variable to the row and invokes a script in the caller's
    # scope.
    #
    # Usage: 
    #     $db foreach ?-as lists|dicts? ?-columnsVariable varName? ?--?
    #         varName sql ?dictionary? script

    method foreach args {

	variable ::tdbc::generalError

	# Grab keyword-value parameters

	set args [::tdbc::ParseConvenienceArgs $args[set args {}] opts]

	# Check postitional parameters 

	set cmd [list [self] prepare]
	if {[llength $args] == 3} {
	    lassign $args varname sqlcode script
	} elseif {[llength $args] == 4} {
	    lassign $args varname sqlcode dict script
	} else {
	    set errorcode $generalError
	    lappend errorcode wrongNumArgs
	    return -code error -errorcode $errorcode \
		"wrong # args: should be [lrange [info level 0] 0 1]\
                 ?-option value?... ?--? varname sqlcode ?dictionary? script"
	}
	lappend cmd $sqlcode

	# Prepare the statement

	set stmt [uplevel 1 $cmd]

	# Delegate to the statement to iterate over the results

	set cmd [list $stmt foreach {*}$opts -- $varname]
	if {[info exists dict]} {
	    lappend cmd $dict
	}
	lappend cmd $script
	set status [catch {
	    uplevel 1 $cmd
	} result options]

	# Destroy the statement

	catch {
	    $stmt close
	}

	# Adjust return level in the case that the script [return]s

	if {$status == 2} {
	    set options [dict merge {-level 1} $options[set options {}]]
	    dict incr options -level
	}
	return -options $options $result
    }

    # Derived classes are expected to implement the 'begintransaction',
    # 'commit', and 'rollback' methods.
	
    # Derived classes are expected to implement 'tables' and 'columns' method.

}

#------------------------------------------------------------------------------
#
# Class: tdbc::statement
#
#	Class that represents a SQL statement in a generic database
#
#------------------------------------------------------------------------------

oo::class create tdbc::statement {

    # resultSetSeq is the sequence number of the last result set created.
    # resultSetClass is the name of the class that implements the 'resultset'
    #	API.

    variable resultSetClass resultSetSeq

    # The base class constructor accepts no arguments.  It initializes
    # the machinery for tracking the ownership of result sets. The derived
    # constructor is expected to invoke the base constructor, and to
    # set a variable 'resultSetClass' to the fully-qualified name of the
    # class that represents result sets.

    constructor {} {
	set resultSetSeq 0
	namespace eval ResultSet {}
    }

    # The 'execute' method on a statement runs the statement with
    # a particular set of substituted variables.  It actually works
    # by creating the result set object and letting that objects
    # constructor do the work of running the statement.  The creation
    # is wrapped in an [uplevel] call because the substitution proces
    # may need to access variables in the caller's scope.

    # WORKAROUND: Take out the '0 &&' from the next line when 
    # Bug 2649975 is fixed
    if {0 && [package vsatisfies [package provide Tcl] 8.6]} {
	method execute args {
	    tailcall my resultSetCreate \
		[namespace current]::ResultSet::[incr resultSetSeq]  \
		[self] {*}$args
	}
    } else {
	method execute args {
	    return \
		[uplevel 1 \
		     [list \
			  [self] resultSetCreate \
			  [namespace current]::ResultSet::[incr resultSetSeq] \
			  [self] {*}$args]]
	}
    }

    # The 'ResultSetCreate' method is expected to be a forward to the
    # appropriate result set constructor. If it's missing, the driver must
    # have been designed for tdbc 1.0b9 and earlier, and the 'resultSetClass'
    # variable holds the class name.

    method resultSetCreate {name instance args} {
	return [uplevel 1 [list $resultSetClass create \
			       $name $instance {*}$args]]
    }

    # The 'resultsets' method returns a list of result sets produced by
    # the current statement

    method resultsets {} {
	info commands ResultSet::*
    }

    # The 'allrows' method executes a statement with a given set of
    # substituents, and returns a list of all the rows that the statement
    # returns.  Optionally, it stores the names of columns in
    # '-columnsvariable'.
    #
    # Usage:
    #	$statement allrows ?-as lists|dicts? ?-columnsvariable varName? ?--?
    #		?dictionary?


    method allrows args {

	variable ::tdbc::generalError

	# Grab keyword-value parameters

	set args [::tdbc::ParseConvenienceArgs $args[set args {}] opts]

	# Check postitional parameters 

	set cmd [list [self] execute]
	if {[llength $args] == 0} {
	    # do nothing
	} elseif {[llength $args] == 1} {
	    lappend cmd [lindex $args 0]
	} else {
	    set errorcode $generalError
	    lappend errorcode wrongNumArgs
	    return -code error -errorcode $errorcode \
		"wrong # args: should be [lrange [info level 0] 0 1]\
                 ?-option value?... ?--? ?dictionary?"
	}

	# Get the result set

	set resultSet [uplevel 1 $cmd]

	# Delegate to the result set's [allrows] method to accumulate
	# the rows of the result.

	set cmd [list $resultSet allrows {*}$opts]
	set status [catch {
	    uplevel 1 $cmd
	} result options]

	# Destroy the result set

	catch {
	    rename $resultSet {}
	}

	# Adjust return level in the case that the script [return]s

	if {$status == 2} {
	    set options [dict merge {-level 1} $options[set options {}]]
	    dict incr options -level
	}
	return -options $options $result
    }

    # The 'foreach' method executes a statement with a given set of
    # substituents.  It runs the supplied script, substituting the supplied
    # named variable. Optionally, it stores the names of columns in
    # '-columnsvariable'.
    #
    # Usage:
    #	$statement foreach ?-as lists|dicts? ?-columnsvariable varName? ?--?
    #		variableName ?dictionary? script

    method foreach args {

	variable ::tdbc::generalError

	# Grab keyword-value parameters

	set args [::tdbc::ParseConvenienceArgs $args[set args {}] opts]
	
	# Check positional parameters

	set cmd [list [self] execute]
	if {[llength $args] == 2} {
	    lassign $args varname script
	} elseif {[llength $args] == 3} {
	    lassign $args varname dict script
	    lappend cmd $dict
	} else {
	    set errorcode $generalError
	    lappend errorcode wrongNumArgs
	    return -code error -errorcode $errorcode \
		"wrong # args: should be [lrange [info level 0] 0 1]\
                 ?-option value?... ?--? varName ?dictionary? script"
	}

	# Get the result set

	set resultSet [uplevel 1 $cmd]

	# Delegate to the result set's [foreach] method to evaluate
	# the script for each row of the result.

	set cmd [list $resultSet foreach {*}$opts -- $varname $script]
	set status [catch {
	    uplevel 1 $cmd
	} result options]

	# Destroy the result set

	catch {
	    rename $resultSet {}
	}

	# Adjust return level in the case that the script [return]s

	if {$status == 2} {
	    set options [dict merge {-level 1} $options[set options {}]]
	    dict incr options -level
	}
	return -options $options $result
    }

    # The 'close' method is syntactic sugar for invoking the destructor

    method close {} {
	my destroy
    }

    # Derived classes are expected to implement their own constructors,
    # plus the following methods:

    # paramtype paramName ?direction? type ?scale ?precision??
    #     Declares the type of a parameter in the statement

}

#------------------------------------------------------------------------------
#
# Class: tdbc::resultset
#
#	Class that represents a result set in a generic database.
#
#------------------------------------------------------------------------------

oo::class create tdbc::resultset {

    constructor {} { }

    # The 'allrows' method returns a list of all rows that a given
    # result set returns.

    method allrows args {

	variable ::tdbc::generalError

	# Parse args

	set args [::tdbc::ParseConvenienceArgs $args[set args {}] opts]
	if {[llength $args] != 0} {
	    set errorcode $generalError
	    lappend errorcode wrongNumArgs
	    return -code error -errorcode $errorcode \
		"wrong # args: should be [lrange [info level 0] 0 1]\
                 ?-option value?... ?--? varName script"
	}

	# Do -columnsvariable if requested

	if {[dict exists $opts -columnsvariable]} {
	    upvar 1 [dict get $opts -columnsvariable] columns
	    set columns [my columns]
	}

	# Assemble the results

	if {[dict get $opts -as] eq {lists}} {
	    set delegate nextlist
	} else {
	    set delegate nextdict
	}
	set results [list]
	while {[my $delegate row]} {
	    lappend results $row
	}
	return $results
	    
    }

    # The 'foreach' method runs a script on each row from a result set.
    # TODO - Implement in C for speed?
    # Note that there are performance issues with anything involving an
    # iterated 'uplevel'; Miguel Sofer has ideas regarding how to 
    # make 'uplevel' as fast as evaluating a 'proc'.

    method foreach args {

	variable ::tdbc::generalError

	# Grab keyword-value parameters

	set args [::tdbc::ParseConvenienceArgs $args[set args {}] opts]

	# Check positional parameters

	if {[llength $args] != 2} {
	    set errorcode $generalError
	    lappend errorcode wrongNumArgs
	    return -code error -errorcode $errorcode \
		"wrong # args: should be [lrange [info level 0] 0 1]\
                 ?-option value?... ?--? varName script"
	}

	# Do -columnsvariable if requested

	if {[dict exists $opts -columnsvariable]} {
	    upvar 1 [dict get $opts -columnsvariable] columns
	    set columns [my columns]
	}

	# Run the loop over the rows of the query

	upvar 1 [lindex $args 0] row
	if {[dict get $opts -as] eq {lists}} {
	    set delegate nextlist
	} else {
	    set delegate nextdict
	}
	while {[my $delegate row]} {
	    set status [catch {
		uplevel 1 [lindex $args 1]
	    } result options]
	    switch -exact -- $status {
		0 - 4 {		# OK or CONTINUE
		}
		2 {		# RETURN
		    set options [dict merge {-level 1} $options[set options {}]]
		    dict incr options -level
		    return -options $options $result
		}
		3 {		# BREAK
		    break
		}
		default {	# ERROR or unknown status
		    return -options $options $result
		}
	    }
	}

	return
    }

    
    # The 'nextrow' method retrieves a row in the form of either
    # a list or a dictionary.

    method nextrow {args} {

	variable ::tdbc::generalError

	set opts [dict create -as dicts]
	set i 0
    
	# Munch keyword options off the front of the command arguments
	
	foreach {key value} $args {
	    if {[string index $key 0] eq {-}} {
		switch -regexp -- $key {
		    -as? {
			dict set opts -as $value
		    }
		    -- {
			incr i
			break
		    }
		    default {
			set errorcode $generalError
			lappend errorcode badOption $key
			return -code error -errorcode $errorcode \
			    "bad option \"$key\":\
                             must be -as or -columnsvariable"
		    }
		}
	    } else {
		break
	    }
	    incr i 2
	}

	set args [lrange $args $i end]
	if {[llength $args] != 1} {
	    set errorcode $generalError
	    lappend errorcode wrongNumArgs
	    return -code error -errorcode $errorcode \
		"wrong # args: should be [lrange [info level 0] 0 1]\
                 ?-option value?... ?--? varName"
	}
	upvar 1 [lindex $args 0] row
	if {[dict get $opts -as] eq {lists}} {
	    set delegate nextlist
	} else {
	    set delegate nextdict
	}
	return [my $delegate row]
    }

    # The 'close' method is syntactic sugar for destroying the result set.

    method close {} {
	my destroy
    }

    # Derived classes are expected to implement the following methods:

    # constructor and destructor.  
    #        Constructor accepts a statement and an optional
    #        a dictionary of substituted parameters  and
    #        executes the statement against the database. If
    #	     the dictionary is not supplied, then the default
    #	     is to get params from variables in the caller's scope).
    # columns
    #     -- Returns a list of the names of the columns in the result.
    # nextdict variableName
    #     -- Stores the next row of the result set in the given variable
    #        in caller's scope, in the form of a dictionary that maps
    #	     column names to values.
    # nextlist variableName
    #     -- Stores the next row of the result set in the given variable
    #        in caller's scope, in the form of a list of cells.
    # rowcount
    #     -- Returns a count of rows affected by the statement, or -1
    #        if the count of rows has not been determined.

}