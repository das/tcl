/* 
 * tclUnixFile.c --
 *
 *      This file contains wrappers around UNIX file handling functions.
 *      These wrappers mask differences between Windows and UNIX.
 *
 * Copyright (c) 1995-1998 Sun Microsystems, Inc.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id$
 */

#include "tclInt.h"
#include "tclPort.h"

static int NativeMatchType(CONST char* nativeName, Tcl_GlobTypeData *types);


/*
 *---------------------------------------------------------------------------
 *
 * TclpFindExecutable --
 *
 *	This procedure computes the absolute path name of the current
 *	application, given its argv[0] value.
 *
 * Results:
 *	A dirty UTF string that is the path to the executable.  At this
 *	point we may not know the system encoding.  Convert the native
 *	string value to UTF using the default encoding.  The assumption
 *	is that we will still be able to parse the path given the path
 *	name contains ASCII string and '/' chars do not conflict with
 *	other UTF chars.
 *
 * Side effects:
 *	The variable tclNativeExecutableName gets filled in with the file
 *	name for the application, if we figured it out.  If we couldn't
 *	figure it out, tclNativeExecutableName is set to NULL.
 *
 *---------------------------------------------------------------------------
 */

char *
TclpFindExecutable(argv0)
    CONST char *argv0;		/* The value of the application's argv[0]
				 * (native). */
{
    CONST char *name, *p;
    Tcl_StatBuf statBuf;
    int length;
    Tcl_DString buffer, nameString;

    if (argv0 == NULL) {
	return NULL;
    }
    if (tclNativeExecutableName != NULL) {
	return tclNativeExecutableName;
    }

    Tcl_DStringInit(&buffer);

    name = argv0;
    for (p = name; *p != '\0'; p++) {
	if (*p == '/') {
	    /*
	     * The name contains a slash, so use the name directly
	     * without doing a path search.
	     */

	    goto gotName;
	}
    }

    p = getenv("PATH");					/* INTL: Native. */
    if (p == NULL) {
	/*
	 * There's no PATH environment variable; use the default that
	 * is used by sh.
	 */

	p = ":/bin:/usr/bin";
    } else if (*p == '\0') {
	/*
	 * An empty path is equivalent to ".".
	 */

	p = "./";
    }

    /*
     * Search through all the directories named in the PATH variable
     * to see if argv[0] is in one of them.  If so, use that file
     * name.
     */

    while (1) {
	while (isspace(UCHAR(*p))) {		/* INTL: BUG */
	    p++;
	}
	name = p;
	while ((*p != ':') && (*p != 0)) {
	    p++;
	}
	Tcl_DStringSetLength(&buffer, 0);
	if (p != name) {
	    Tcl_DStringAppend(&buffer, name, p - name);
	    if (p[-1] != '/') {
		Tcl_DStringAppend(&buffer, "/", 1);
	    }
	}
	name = Tcl_DStringAppend(&buffer, argv0, -1);

	/*
	 * INTL: The following calls to access() and stat() should not be
	 * converted to Tclp routines because they need to operate on native
	 * strings directly.
	 */

	if ((access(name, X_OK) == 0)			   /* INTL: Native. */
		&& (Tcl_PlatformStat(name, &statBuf) == 0) /* INTL: Native. */
		&& S_ISREG(statBuf.st_mode)) {
	    goto gotName;
	}
	if (*p == '\0') {
	    break;
	} else if (*(p+1) == 0) {
	    p = "./";
	} else {
	    p++;
	}
    }
    goto done;

    /*
     * If the name starts with "/" then just copy it to tclExecutableName.
     */

gotName:
#ifdef DJGPP
    if (name[1] == ':')  {
#else
    if (name[0] == '/')  {
#endif
	Tcl_ExternalToUtfDString(NULL, name, -1, &nameString);
	tclNativeExecutableName = (char *)
		ckalloc((unsigned) (Tcl_DStringLength(&nameString) + 1));
	strcpy(tclNativeExecutableName, Tcl_DStringValue(&nameString));
	Tcl_DStringFree(&nameString);
	goto done;
    }

    /*
     * The name is relative to the current working directory.  First
     * strip off a leading "./", if any, then add the full path name of
     * the current working directory.
     */

    if ((name[0] == '.') && (name[1] == '/')) {
	name += 2;
    }

    Tcl_ExternalToUtfDString(NULL, name, -1, &nameString);

    Tcl_DStringFree(&buffer);
    TclpGetCwd(NULL, &buffer);

    length = Tcl_DStringLength(&buffer) + Tcl_DStringLength(&nameString) + 2;
    tclNativeExecutableName = (char *) ckalloc((unsigned) length);
    strcpy(tclNativeExecutableName, Tcl_DStringValue(&buffer));
    tclNativeExecutableName[Tcl_DStringLength(&buffer)] = '/';
    strcpy(tclNativeExecutableName + Tcl_DStringLength(&buffer) + 1,
	    Tcl_DStringValue(&nameString));
    Tcl_DStringFree(&nameString);
    
done:
    Tcl_DStringFree(&buffer);
    return tclNativeExecutableName;
}

/*
 *----------------------------------------------------------------------
 *
 * TclpMatchInDirectory --
 *
 *	This routine is used by the globbing code to search a
 *	directory for all files which match a given pattern.
 *
 * Results: 
 *	The return value is a standard Tcl result indicating whether an
 *	error occurred in globbing.  Errors are left in interp, good
 *	results are lappended to resultPtr (which must be a valid object)
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------- */

int
TclpMatchInDirectory(interp, resultPtr, pathPtr, pattern, types)
    Tcl_Interp *interp;		/* Interpreter to receive errors. */
    Tcl_Obj *resultPtr;		/* List object to lappend results. */
    Tcl_Obj *pathPtr;	        /* Contains path to directory to search. */
    CONST char *pattern;	/* Pattern to match against. */
    Tcl_GlobTypeData *types;	/* Object containing list of acceptable types.
				 * May be NULL. In particular the directory
				 * flag is very important. */
{
    CONST char *native;
    Tcl_Obj *fileNamePtr;

    fileNamePtr = Tcl_FSGetTranslatedPath(interp, pathPtr);
    if (fileNamePtr == NULL) {
	return TCL_ERROR;
    }
    
    if (pattern == NULL || (*pattern == '\0')) {
	/* Match a file directly */
	CONST char *native = (CONST char*) Tcl_FSGetNativePath(pathPtr);
	if (NativeMatchType(native, types)) {
	    Tcl_ListObjAppendElement(interp, resultPtr, pathPtr);
	}
	return TCL_OK;
    } else {
	CONST char *fname, *dirName;
	DIR *d;
	Tcl_DString ds;
	Tcl_StatBuf statBuf;
	int matchHidden;
	int nativeDirLen;
	int result = TCL_OK;
	Tcl_DString dsOrig;
	int baseLength;
	
	Tcl_DStringInit(&dsOrig);
	Tcl_DStringAppend(&dsOrig, Tcl_GetString(fileNamePtr), -1);
	baseLength = Tcl_DStringLength(&dsOrig);
	
	/*
	 * Make sure that the directory part of the name really is a
	 * directory.  If the directory name is "", use the name "."
	 * instead, because some UNIX systems don't treat "" like "."
	 * automatically.  Keep the "" for use in generating file names,
	 * otherwise "glob foo.c" would return "./foo.c".
	 */

	if (baseLength == 0) {
	    dirName = ".";
	} else {
	    dirName = Tcl_DStringValue(&dsOrig);
	    /* Make sure we have a trailing directory delimiter */
	    if (dirName[baseLength-1] != '/') {
		dirName = Tcl_DStringAppend(&dsOrig, "/", 1);
		baseLength++;
	    }
	}
	
	/*
	 * Check to see if the pattern needs to compare with hidden files.
	 */

	if ((pattern[0] == '.')
		|| ((pattern[0] == '\\') && (pattern[1] == '.'))) {
	    matchHidden = 1;
	} else {
	    matchHidden = 0;
	}

	/*
	 * Now open the directory for reading and iterate over the contents.
	 */

	native = Tcl_UtfToExternalDString(NULL, dirName, -1, &ds);

	if ((Tcl_PlatformStat(native, &statBuf) != 0)		/* INTL: UTF-8. */
		|| !S_ISDIR(statBuf.st_mode)) {
	    Tcl_DStringFree(&dsOrig);
	    Tcl_DStringFree(&ds);
	    return TCL_OK;
	}

	d = opendir(native);				/* INTL: Native. */
	if (d == NULL) {
	    char savedChar = '\0';
	    Tcl_ResetResult(interp);
	    Tcl_DStringFree(&ds);

	    /*
	     * Strip off a trailing '/' if necessary, before reporting the error.
	     */

	    if (baseLength > 0) {
		savedChar = (Tcl_DStringValue(&dsOrig))[baseLength-1];
		if (savedChar == '/') {
		    (Tcl_DStringValue(&dsOrig))[baseLength-1] = '\0';
		}
	    }
	    Tcl_AppendResult(interp, "couldn't read directory \"",
		    Tcl_DStringValue(&dsOrig), "\": ",
		    Tcl_PosixError(interp), (char *) NULL);
	    if (baseLength > 0) {
		(Tcl_DStringValue(&dsOrig))[baseLength-1] = savedChar;
	    }
	    Tcl_DStringFree(&dsOrig);
	    return TCL_ERROR;
	}

	nativeDirLen = Tcl_DStringLength(&ds);

	while (1) {
	    Tcl_DString utfDs;
	    CONST char *utf;
	    Tcl_DirEntry *entryPtr;
	    
	    entryPtr = Tcl_PlatformReaddir(d);		/* INTL: Native. */
	    if (entryPtr == NULL) {
		break;
	    }
	    if (types != NULL && (types->perm & TCL_GLOB_PERM_HIDDEN)) {
		/* 
		 * We explicitly asked for hidden files, so turn around
		 * and ignore any file which isn't hidden.
		 */
		if (*entryPtr->d_name != '.') {
		    continue;
		}
	    } else if (!matchHidden && (*entryPtr->d_name == '.')) {
		/*
		 * Don't match names starting with "." unless the "." is
		 * present in the pattern.
		 */
		continue;
	    }

	    /*
	     * Now check to see if the file matches, according to both type
	     * and pattern.  If so, add the file to the result.
	     */

	    utf = Tcl_ExternalToUtfDString(NULL, entryPtr->d_name, -1, &utfDs);
	    if (Tcl_StringMatch(utf, pattern) != 0) {
		int typeOk = 1;

		Tcl_DStringSetLength(&dsOrig, baseLength);
		Tcl_DStringAppend(&dsOrig, utf, -1);
		fname = Tcl_DStringValue(&dsOrig);
		if (types != NULL) {
		    char *nativeEntry;
		    Tcl_DStringSetLength(&ds, nativeDirLen);
		    nativeEntry = Tcl_DStringAppend(&ds, entryPtr->d_name, -1);
		    typeOk = NativeMatchType(nativeEntry, types);
		}
		if (typeOk) {
		    Tcl_ListObjAppendElement(interp, resultPtr, 
			    Tcl_NewStringObj(fname, Tcl_DStringLength(&dsOrig)));
		}
	    }
	    Tcl_DStringFree(&utfDs);
	}

	closedir(d);
	Tcl_DStringFree(&ds);
	Tcl_DStringFree(&dsOrig);
	return result;
    }
}
static int 
NativeMatchType(
    CONST char* nativeEntry,  /* Native path to check */
    Tcl_GlobTypeData *types)  /* Type description to match against */
{
    Tcl_StatBuf buf;
    if (types == NULL) {
	/* 
	 * Simply check for the file's existence, but do it
	 * with lstat, in case it is a link to a file which
	 * doesn't exist (since that case would not show up
	 * if we used 'access' or 'stat')
	 */
	if (Tcl_PlatformLStat(nativeEntry, &buf) != 0) {
	    return 0;
	}
    } else {
	if (types->perm != 0) {
	    if (Tcl_PlatformStat(nativeEntry, &buf) != 0) {
		/* 
		 * Either the file has disappeared between the
		 * 'readdir' call and the 'stat' call, or
		 * the file is a link to a file which doesn't
		 * exist (which we could ascertain with
		 * lstat), or there is some other strange
		 * problem.  In all these cases, we define this
		 * to mean the file does not match any defined
		 * permission, and therefore it is not 
		 * added to the list of files to return.
		 */
		return 0;
	    }
	    
	    /* 
	     * readonly means that there are NO write permissions
	     * (even for user), but execute is OK for anybody
	     */
	    if (((types->perm & TCL_GLOB_PERM_RONLY) &&
			(buf.st_mode & (S_IWOTH|S_IWGRP|S_IWUSR))) ||
		((types->perm & TCL_GLOB_PERM_R) &&
			(access(nativeEntry, R_OK) != 0)) ||
		((types->perm & TCL_GLOB_PERM_W) &&
			(access(nativeEntry, W_OK) != 0)) ||
		((types->perm & TCL_GLOB_PERM_X) &&
			(access(nativeEntry, X_OK) != 0))
		) {
		return 0;
	    }
	}
	if (types->type != 0) {
	    if (types->perm == 0) {
		/* We haven't yet done a stat on the file */
		if (Tcl_PlatformStat(nativeEntry, &buf) != 0) {
		    /* Posix error occurred */
		    return 0;
		}
	    }
	    /*
	     * In order bcdpfls as in 'find -t'
	     */
	    if (
		((types->type & TCL_GLOB_TYPE_BLOCK) &&
			S_ISBLK(buf.st_mode)) ||
		((types->type & TCL_GLOB_TYPE_CHAR) &&
			S_ISCHR(buf.st_mode)) ||
		((types->type & TCL_GLOB_TYPE_DIR) &&
			S_ISDIR(buf.st_mode)) ||
		((types->type & TCL_GLOB_TYPE_PIPE) &&
			S_ISFIFO(buf.st_mode)) ||
		((types->type & TCL_GLOB_TYPE_FILE) &&
			S_ISREG(buf.st_mode))
    #ifdef S_ISSOCK
		|| ((types->type & TCL_GLOB_TYPE_SOCK) &&
			S_ISSOCK(buf.st_mode))
    #endif
		) {
		/* Do nothing -- this file is ok */
	    } else {
    #ifdef S_ISLNK
		if (types->type & TCL_GLOB_TYPE_LINK) {
		    if (Tcl_PlatformLStat(nativeEntry, &buf) == 0) {
			if (S_ISLNK(buf.st_mode)) {
			    return 1;
			}
		    }
		}
    #endif
		return 0;
	    }
	}
    }
    return 1;
}

/*
 *---------------------------------------------------------------------------
 *
 * TclpGetUserHome --
 *
 *	This function takes the specified user name and finds their
 *	home directory.
 *
 * Results:
 *	The result is a pointer to a string specifying the user's home
 *	directory, or NULL if the user's home directory could not be
 *	determined.  Storage for the result string is allocated in
 *	bufferPtr; the caller must call Tcl_DStringFree() when the result
 *	is no longer needed.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

char *
TclpGetUserHome(name, bufferPtr)
    CONST char *name;		/* User name for desired home directory. */
    Tcl_DString *bufferPtr;	/* Uninitialized or free DString filled
				 * with name of user's home directory. */
{
    struct passwd *pwPtr;
    Tcl_DString ds;
    CONST char *native;

    native = Tcl_UtfToExternalDString(NULL, name, -1, &ds);
    pwPtr = getpwnam(native);				/* INTL: Native. */
    Tcl_DStringFree(&ds);
    
    if (pwPtr == NULL) {
	endpwent();
	return NULL;
    }
    Tcl_ExternalToUtfDString(NULL, pwPtr->pw_dir, -1, bufferPtr);
    endpwent();
    return Tcl_DStringValue(bufferPtr);
}

/*
 *---------------------------------------------------------------------------
 *
 * TclpObjAccess --
 *
 *	This function replaces the library version of access().
 *
 * Results:
 *	See access() documentation.
 *
 * Side effects:
 *	See access() documentation.
 *
 *---------------------------------------------------------------------------
 */

int 
TclpObjAccess(pathPtr, mode)
    Tcl_Obj *pathPtr;        /* Path of file to access */
    int mode;                /* Permission setting. */
{
    CONST char *path = Tcl_FSGetNativePath(pathPtr);
    if (path == NULL) {
	return -1;
    } else {
	return access(path, mode);
    }
}

/*
 *---------------------------------------------------------------------------
 *
 * TclpObjChdir --
 *
 *	This function replaces the library version of chdir().
 *
 * Results:
 *	See chdir() documentation.
 *
 * Side effects:
 *	See chdir() documentation.  
 *
 *---------------------------------------------------------------------------
 */

int 
TclpObjChdir(pathPtr)
    Tcl_Obj *pathPtr;          /* Path to new working directory */
{
    CONST char *path = Tcl_FSGetNativePath(pathPtr);
    if (path == NULL) {
	return -1;
    } else {
	return chdir(path);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TclpObjLstat --
 *
 *	This function replaces the library version of lstat().
 *
 * Results:
 *	See lstat() documentation.
 *
 * Side effects:
 *	See lstat() documentation.
 *
 *----------------------------------------------------------------------
 */

int 
TclpObjLstat(pathPtr, bufPtr)
    Tcl_Obj *pathPtr;		/* Path of file to stat */
    Tcl_StatBuf *bufPtr;	/* Filled with results of stat call. */
{
    return Tcl_PlatformLStat(Tcl_FSGetNativePath(pathPtr), bufPtr);
}

/*
 *---------------------------------------------------------------------------
 *
 * TclpObjGetCwd --
 *
 *	This function replaces the library version of getcwd().
 *
 * Results:
 *	The result is a pointer to a string specifying the current
 *	directory, or NULL if the current directory could not be
 *	determined.  If NULL is returned, an error message is left in the
 *	interp's result.  Storage for the result string is allocated in
 *	bufferPtr; the caller must call Tcl_DStringFree() when the result
 *	is no longer needed.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj* 
TclpObjGetCwd(interp)
    Tcl_Interp *interp;
{
    Tcl_DString ds;
    if (TclpGetCwd(interp, &ds) != NULL) {
	Tcl_Obj *cwdPtr = Tcl_NewStringObj(Tcl_DStringValue(&ds), -1);
	Tcl_IncrRefCount(cwdPtr);
	Tcl_DStringFree(&ds);
	return cwdPtr;
    } else {
	return NULL;
    }
}

/* Older string based version */
CONST char *
TclpGetCwd(interp, bufferPtr)
    Tcl_Interp *interp;		/* If non-NULL, used for error reporting. */
    Tcl_DString *bufferPtr;	/* Uninitialized or free DString filled
				 * with name of current directory. */
{
    char buffer[MAXPATHLEN+1];

#ifdef USEGETWD
    if (getwd(buffer) == NULL) {			/* INTL: Native. */
#else
    if (getcwd(buffer, MAXPATHLEN + 1) == NULL) {	/* INTL: Native. */
#endif
	if (interp != NULL) {
	    Tcl_AppendResult(interp,
		    "error getting working directory name: ",
		    Tcl_PosixError(interp), (char *) NULL);
	}
	return NULL;
    }
    return Tcl_ExternalToUtfDString(NULL, buffer, -1, bufferPtr);
}

/*
 *---------------------------------------------------------------------------
 *
 * TclpReadlink --
 *
 *	This function replaces the library version of readlink().
 *
 * Results:
 *	The result is a pointer to a string specifying the contents
 *	of the symbolic link given by 'path', or NULL if the symbolic
 *	link could not be read.  Storage for the result string is
 *	allocated in bufferPtr; the caller must call Tcl_DStringFree()
 *	when the result is no longer needed.
 *
 * Side effects:
 *	See readlink() documentation.
 *
 *---------------------------------------------------------------------------
 */

char *
TclpReadlink(path, linkPtr)
    CONST char *path;		/* Path of file to readlink (UTF-8). */
    Tcl_DString *linkPtr;	/* Uninitialized or free DString filled
				 * with contents of link (UTF-8). */
{
#ifndef DJGPP
    char link[MAXPATHLEN];
    int length;
    CONST char *native;
    Tcl_DString ds;

    native = Tcl_UtfToExternalDString(NULL, path, -1, &ds);
    length = readlink(native, link, sizeof(link));	/* INTL: Native. */
    Tcl_DStringFree(&ds);
    
    if (length < 0) {
	return NULL;
    }

    Tcl_ExternalToUtfDString(NULL, link, length, linkPtr);
    return Tcl_DStringValue(linkPtr);
#else
	return NULL;
#endif
}

/*
 *----------------------------------------------------------------------
 *
 * TclpObjStat --
 *
 *	This function replaces the library version of stat().
 *
 * Results:
 *	See stat() documentation.
 *
 * Side effects:
 *	See stat() documentation.
 *
 *----------------------------------------------------------------------
 */

int 
TclpObjStat(pathPtr, bufPtr)
    Tcl_Obj *pathPtr;		/* Path of file to stat */
    Tcl_StatBuf *bufPtr;	/* Filled with results of stat call. */
{
    CONST char *path = Tcl_FSGetNativePath(pathPtr);
    if (path == NULL) {
	return -1;
    } else {
	return Tcl_PlatformStat(path, bufPtr);
    }
}


#ifdef S_IFLNK

Tcl_Obj* 
TclpObjLink(pathPtr, toPtr)
    Tcl_Obj *pathPtr;
    Tcl_Obj *toPtr;
{
    extern Tcl_Filesystem nativeFilesystem;

    if (toPtr != NULL) {
	CONST char *src = Tcl_FSGetNativePath(pathPtr);
	CONST char *target = Tcl_FSGetNativePath(toPtr);
	
	if (src == NULL || target == NULL) {
	    return NULL;
	}
	if (symlink(src, target) != 0) {
	    return NULL;
	} else {
	    return toPtr;
	}
    } else {
	Tcl_Obj* linkPtr = NULL;

	char link[MAXPATHLEN];
	int length;
	char *native;

	if (Tcl_FSGetTranslatedPath(NULL, pathPtr) == NULL) {
	    return NULL;
	}
	length = readlink(Tcl_FSGetNativePath(pathPtr), link, sizeof(link));
	if (length < 0) {
	    return NULL;
	}
	
	/* 
	 * Allocate and copy the name, taking care since the
	 * name need not be null terminated. 
	 */
	native = (char*)ckalloc((unsigned)(1+length));
	strncpy(native, link, (unsigned)length);
	native[length] = '\0';
	
	linkPtr = Tcl_FSNewNativePath(&nativeFilesystem, native);
	if (linkPtr != NULL) {
	    Tcl_IncrRefCount(linkPtr);
	}
	return linkPtr;
    }
}

#endif


/*
 *---------------------------------------------------------------------------
 *
 * TclpFilesystemPathType --
 *
 *      This function is part of the native filesystem support, and
 *      returns the path type of the given path.  Right now it simply
 *      returns NULL.  In the future it could return specific path
 *      types, like 'nfs', 'samba', 'FAT32', etc.
 *
 * Results:
 *      NULL at present.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */
Tcl_Obj*
TclpFilesystemPathType(pathObjPtr)
    Tcl_Obj* pathObjPtr;
{
    /* All native paths are of the same type */
    return NULL;
}
