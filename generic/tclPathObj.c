/* 
 * tclPathObj.c --
 *
 *	This file contains the implementation of Tcl's "path" object
 *	type used to represent and manipulate a general (virtual)
 *	filesystem entity in an efficient manner.
 *
 * Copyright (c) 2003 Vince Darley.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id$
 */

#include "tclInt.h"
#include "tclPort.h"
#ifdef MAC_TCL
#include "tclMacInt.h"
#endif
#include "tclFileSystem.h"

/*
 * Prototypes for procedures defined later in this file.
 */

static void	DupFsPathInternalRep _ANSI_ARGS_((Tcl_Obj *srcPtr,
						  Tcl_Obj *copyPtr));
static void	FreeFsPathInternalRep _ANSI_ARGS_((Tcl_Obj *pathPtr));
static void	UpdateStringOfFsPath  _ANSI_ARGS_((Tcl_Obj *pathPtr));
static int	SetFsPathFromAny _ANSI_ARGS_((Tcl_Interp *interp,
					      Tcl_Obj *pathPtr));
static int	FindSplitPos _ANSI_ARGS_((CONST char *path, int separator));
static int      IsSeparatorOrNull _ANSI_ARGS_((int ch));
static Tcl_Obj* GetExtension _ANSI_ARGS_((Tcl_Obj *pathPtr));


/*
 * Define the 'path' object type, which Tcl uses to represent
 * file paths internally.
 */
Tcl_ObjType tclFsPathType = {
    "path",				/* name */
    FreeFsPathInternalRep,		/* freeIntRepProc */
    DupFsPathInternalRep,	        /* dupIntRepProc */
    UpdateStringOfFsPath,		/* updateStringProc */
    SetFsPathFromAny			/* setFromAnyProc */
};

/* 
 * struct FsPath --
 * 
 * Internal representation of a Tcl_Obj of "path" type.  This
 * can be used to represent relative or absolute paths, and has
 * certain optimisations when used to represent paths which are
 * already normalized and absolute.
 * 
 * Note that both 'translatedPathPtr' and 'normPathPtr' can be a
 * circular reference to the container Tcl_Obj of this FsPath.
 * 
 * There are two cases, with the first being the most common:
 * 
 * (i) flags == 0, => Ordinary path.  
 * 
 * translatedPathPtr contains the translated path (which may be
 * a circular reference to the object itself).  If it is NULL
 * then the path is pure normalized (and the normPathPtr will be
 * a circular reference).  cwdPtr is null for an absolute path,
 * and non-null for a relative path (unless the cwd has never been
 * set, in which case the cwdPtr may also be null for a relative path).
 * 
 * (ii) flags != 0, => Special path, see TclNewFSPathObj
 * 
 * Now, this is a path like 'file join $dir $tail' where, cwdPtr is
 * the $dir and normPathPtr is the $tail.
 * 
 */
typedef struct FsPath {
    Tcl_Obj *translatedPathPtr; /* Name without any ~user sequences.
				 * If this is NULL, then this is a 
				 * pure normalized, absolute path
				 * object, in which the parent Tcl_Obj's
				 * string rep is already both translated
				 * and normalized. */
    Tcl_Obj *normPathPtr;       /* Normalized absolute path, without 
				 * ., .. or ~user sequences. If the 
				 * Tcl_Obj containing 
				 * this FsPath is already normalized, 
				 * this may be a circular reference back
				 * to the container.  If that is NOT the
				 * case, we have a refCount on the object. */
    Tcl_Obj *cwdPtr;            /* If null, path is absolute, else
				 * this points to the cwd object used
				 * for this path.  We have a refCount
				 * on the object. */
    int flags;                  /* Flags to describe interpretation -
                                 * see below. */
    ClientData nativePathPtr;   /* Native representation of this path,
				 * which is filesystem dependent. */
    int filesystemEpoch;        /* Used to ensure the path representation
				 * was generated during the correct
				 * filesystem epoch.  The epoch changes
				 * when filesystem-mounts are changed. */ 
    struct FilesystemRecord *fsRecPtr;
				/* Pointer to the filesystem record 
				 * entry to use for this path. */
} FsPath;

/*
 * Flag values for FsPath->flags.
 */
#define TCLPATH_APPENDED 1

/* 
 * Define some macros to give us convenient access to path-object
 * specific fields.
 */
#define PATHOBJ(pathPtr) (pathPtr->internalRep.otherValuePtr)
#define PATHFLAGS(pathPtr) \
 (((FsPath*)(pathPtr->internalRep.otherValuePtr))->flags)


/*
 *---------------------------------------------------------------------------
 *
 * TclFSNormalizeAbsolutePath --
 *
 * Description:
 *	Takes an absolute path specification and computes a 'normalized'
 *	path from it.
 *	
 *	A normalized path is one which has all '../', './' removed.
 *	Also it is one which is in the 'standard' format for the native
 *	platform.  On MacOS, Unix, this means the path must be free of
 *	symbolic links/aliases, and on Windows it means we want the
 *	long form, with that long form's case-dependence (which gives
 *	us a unique, case-dependent path).
 *	
 *	The behaviour of this function if passed a non-absolute path
 *	is NOT defined.
 *	
 *	pathPtr may have a refCount of zero, or may be a shared
 *	object.
 *
 * Results:
 *	The result is returned in a Tcl_Obj with a refCount of 1,
 *	which is therefore owned by the caller.  It must be
 *	freed (with Tcl_DecrRefCount) by the caller when no longer needed.
 *
 * Side effects:
 *	None (beyond the memory allocation for the result).
 *
 * Special note:
 *	This code was originally based on code from Matt Newman and
 *	Jean-Claude Wippler, but has since been totally rewritten by
 *	Vince Darley to deal with symbolic links.
 *
 *---------------------------------------------------------------------------
 */
Tcl_Obj*
TclFSNormalizeAbsolutePath(interp, pathPtr, clientDataPtr)
    Tcl_Interp* interp;        /* Interpreter to use */
    Tcl_Obj *pathPtr;          /* Absolute path to normalize */
    ClientData *clientDataPtr; /* If non-NULL, then may be set to the
                                * fs-specific clientData for this path.
                                * This will happen when that extra
                                * information can be calculated efficiently
                                * as a side-effect of normalization. */
{
    ClientData clientData = NULL;
    CONST char *dirSep, *oldDirSep;
    int first = 1;   /* Set to zero once we've passed the first
                      * directory separator - we can't use '..' to 
                      * remove the volume in a path. */
    Tcl_Obj *retVal = NULL;
    dirSep = Tcl_GetString(pathPtr);
    
    if (tclPlatform == TCL_PLATFORM_WINDOWS) {
        if (dirSep[0] != 0 && dirSep[1] == ':' && 
	    (dirSep[2] == '/' || dirSep[2] == '\\')) {
	    /* Do nothing */
	} else if ((dirSep[0] == '/' || dirSep[0] == '\\') 
	    && (dirSep[1] == '/' || dirSep[1] == '\\')) {
	    /* 
	     * UNC style path, where we must skip over the
	     * first separator, since the first two segments
	     * are actually inseparable.
	     */
	    dirSep += 2;
	    dirSep += FindSplitPos(dirSep, '/');
	    if (*dirSep != 0) {
	        dirSep++;
	    }
	}
    }
    
    /* 
     * Scan forward from one directory separator to the next,
     * checking for '..' and '.' sequences which must be handled
     * specially.  In particular handling of '..' can be complicated
     * if the directory before is a link, since we will have to
     * expand the link to be able to back up one level.
     */
    while (*dirSep != 0) {
	oldDirSep = dirSep;
	if (!first) {
	    dirSep++;
	}
        dirSep += FindSplitPos(dirSep, '/');
	if (dirSep[0] == 0 || dirSep[1] == 0) {
	    if (retVal != NULL) {
		Tcl_AppendToObj(retVal, oldDirSep, dirSep - oldDirSep);
	    }
	    break;
	}
	if (dirSep[1] == '.') {
	    if (retVal != NULL) {
		Tcl_AppendToObj(retVal, oldDirSep, dirSep - oldDirSep);
		oldDirSep = dirSep;
	    }
	  again:
	    if (IsSeparatorOrNull(dirSep[2])) {
		/* Need to skip '.' in the path */
		if (retVal == NULL) {
		    CONST char *path = Tcl_GetString(pathPtr);
		    retVal = Tcl_NewStringObj(path, dirSep - path);
		    Tcl_IncrRefCount(retVal);
		}
		dirSep += 2;
		oldDirSep = dirSep;
		if (dirSep[0] != 0 && dirSep[1] == '.') {
		    goto again;
		}
		continue;
	    }
	    if (dirSep[2] == '.' && IsSeparatorOrNull(dirSep[3])) {
		Tcl_Obj *link;
		int curLen;
		char *linkStr;
		/* Have '..' so need to skip previous directory */
		if (retVal == NULL) {
		    CONST char *path = Tcl_GetString(pathPtr);
		    retVal = Tcl_NewStringObj(path, dirSep - path);
		    Tcl_IncrRefCount(retVal);
		}
		if (!first || (tclPlatform == TCL_PLATFORM_UNIX)) {
		    link = Tcl_FSLink(retVal, NULL, 0);
		    if (link != NULL) {
			/* 
			 * Got a link.  Need to check if the link
			 * is relative or absolute, for those platforms
			 * where relative links exist.
			 */
			if ((tclPlatform != TCL_PLATFORM_WINDOWS)
			   && (Tcl_FSGetPathType(link) == TCL_PATH_RELATIVE)) {
			    /* 
			     * We need to follow this link which is
			     * relative to retVal's directory.  This
			     * means concatenating the link onto
			     * the directory of the path so far.
			     */
			    CONST char *path = Tcl_GetStringFromObj(retVal, 
								    &curLen);
			    while (--curLen >= 0) {
			        if (IsSeparatorOrNull(path[curLen])) {
			            break;
			        }
			    }
			    if (Tcl_IsShared(retVal)) {
				Tcl_DecrRefCount(retVal);
				retVal = Tcl_DuplicateObj(retVal);
				Tcl_IncrRefCount(retVal);
			    }
			    /* We want the trailing slash */
			    Tcl_SetObjLength(retVal, curLen+1);
			    Tcl_AppendObjToObj(retVal, link);
			    Tcl_DecrRefCount(link);
			    linkStr = Tcl_GetStringFromObj(retVal, &curLen);
			} else {
			    /* Absolute link */
			    Tcl_DecrRefCount(retVal);
			    retVal = link;
			    linkStr = Tcl_GetStringFromObj(retVal, &curLen);
			    /* Convert to forward-slashes on windows */
			    if (tclPlatform == TCL_PLATFORM_WINDOWS) {
				int i;
				for (i = 0; i < curLen; i++) {
				    if (linkStr[i] == '\\') {
					linkStr[i] = '/';
				    }
				}
			    }
			}
		    } else {
			linkStr = Tcl_GetStringFromObj(retVal, &curLen);
		    }
		    /* Either way, we now remove the last path element */
		    while (--curLen >= 0) {
			if (IsSeparatorOrNull(linkStr[curLen])) {
			    Tcl_SetObjLength(retVal, curLen);
			    break;
			}
		    }
		}
		dirSep += 3;
		oldDirSep = dirSep;
		if (dirSep[0] != 0 && dirSep[1] == '.') {
		    goto again;
		}
		continue;
	    }
	}
	first = 0;
	if (retVal != NULL) {
	    Tcl_AppendToObj(retVal, oldDirSep, dirSep - oldDirSep);
	}
    }
    
    /* 
     * If we didn't make any changes, just use the input path 
     */
    if (retVal == NULL) {
	retVal = pathPtr;
	Tcl_IncrRefCount(retVal);
	
	if (Tcl_IsShared(retVal)) {
	    /* 
	     * Unfortunately, the platform-specific normalization code
	     * which will be called below has no way of dealing with the
	     * case where an object is shared.  It is expecting to
	     * modify an object in place.  So, we must duplicate this
	     * here to ensure an object with a single ref-count.
	     * 
	     * If that changes in the future (e.g. the normalize proc is
	     * given one object and is able to return a different one),
	     * then we could remove this code.
	     */
	    Tcl_DecrRefCount(retVal);
	    retVal = Tcl_DuplicateObj(pathPtr);
	    Tcl_IncrRefCount(retVal);
	}
    }

    /* 
     * Ensure a windows drive like C:/ has a trailing separator 
     */
    if (tclPlatform == TCL_PLATFORM_WINDOWS) {
	int len;
	CONST char *path = Tcl_GetStringFromObj(retVal, &len);
	if (len == 2 && path[0] != 0 && path[1] == ':') {
	    if (Tcl_IsShared(retVal)) {
		Tcl_DecrRefCount(retVal);
		retVal = Tcl_DuplicateObj(retVal);
		Tcl_IncrRefCount(retVal);
	    }
	    Tcl_AppendToObj(retVal, "/", 1);
	}
    }

    /* 
     * Now we have an absolute path, with no '..', '.' sequences,
     * but it still may not be in 'unique' form, depending on the
     * platform.  For instance, Unix is case-sensitive, so the
     * path is ok.  Windows is case-insensitive, and also has the
     * weird 'longname/shortname' thing (e.g. C:/Program Files/ and
     * C:/Progra~1/ are equivalent).  MacOS is case-insensitive.
     * 
     * Virtual file systems which may be registered may have
     * other criteria for normalizing a path.
     */
    TclFSNormalizeToUniquePath(interp, retVal, 0, &clientData);
    /* 
     * Since we know it is a normalized path, we can
     * actually convert this object into an FsPath for
     * greater efficiency 
     */
    TclFSMakePathFromNormalized(interp, retVal, clientData);
    if (clientDataPtr != NULL) {
	*clientDataPtr = clientData;
    }
    /* This has a refCount of 1 for the caller */
    return retVal;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FSGetPathType --
 *
 *	Determines whether a given path is relative to the current
 *	directory, relative to the current volume, or absolute.  
 *
 * Results:
 *	Returns one of TCL_PATH_ABSOLUTE, TCL_PATH_RELATIVE, or
 *	TCL_PATH_VOLUME_RELATIVE.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_PathType
Tcl_FSGetPathType(pathPtr)
    Tcl_Obj *pathPtr;
{
    return TclFSGetPathType(pathPtr, NULL, NULL);
}

/*
 *----------------------------------------------------------------------
 *
 * TclFSGetPathType --
 *
 *	Determines whether a given path is relative to the current
 *	directory, relative to the current volume, or absolute.  If the
 *	caller wishes to know which filesystem claimed the path (in the
 *	case for which the path is absolute), then a reference to a
 *	filesystem pointer can be passed in (but passing NULL is
 *	acceptable).
 *
 * Results:
 *	Returns one of TCL_PATH_ABSOLUTE, TCL_PATH_RELATIVE, or
 *	TCL_PATH_VOLUME_RELATIVE.  The filesystem reference will
 *	be set if and only if it is non-NULL and the function's 
 *	return value is TCL_PATH_ABSOLUTE.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_PathType
TclFSGetPathType(pathPtr, filesystemPtrPtr, driveNameLengthPtr)
    Tcl_Obj *pathPtr;
    Tcl_Filesystem **filesystemPtrPtr;
    int *driveNameLengthPtr;
{
    if (Tcl_FSConvertToPathType(NULL, pathPtr) != TCL_OK) {
	return TclGetPathType(pathPtr, filesystemPtrPtr, 
		driveNameLengthPtr, NULL);
    } else {
	FsPath *fsPathPtr = (FsPath*) PATHOBJ(pathPtr);
	if (fsPathPtr->cwdPtr != NULL) {
	    if (PATHFLAGS(pathPtr) == 0) {
		return TCL_PATH_RELATIVE;
	    }
	    return TclFSGetPathType(fsPathPtr->cwdPtr, filesystemPtrPtr, 
		    driveNameLengthPtr);
	} else {
	    return TclGetPathType(pathPtr, filesystemPtrPtr, 
		    driveNameLengthPtr, NULL);
	}
    }
}

/*
 *---------------------------------------------------------------------------
 *
 * TclPathPart
 *
 *	This procedure calculates the requested part of the the given
 *	path, which can be:
 *	
 *	- the directory above ('file dirname')
 *	- the tail            ('file tail')
 *	- the extension       ('file extension')
 *	- the root            ('file root')
 *	
 *	The 'portion' parameter dictates which of these to calculate.
 *	There are a number of special cases both to be more efficient,
 *	and because the behaviour when given a path with only a single
 *	element is defined to require the expansion of that single
 *	element, where possible.
 *
 *      Should look into integrating 'FileBasename' in tclFCmd.c into
 *      this function.
 *      
 * Results:
 *	NULL if an error occurred, otherwise a Tcl_Obj owned by
 *	the caller (i.e. most likely with refCount 1).
 *
 * Side effects:
 *      None.
 *
 *---------------------------------------------------------------------------
 */

Tcl_Obj*
TclPathPart(interp, pathPtr, portion)
    Tcl_Interp *interp;		/* Used for error reporting */
    Tcl_Obj *pathPtr;           /* Path to take dirname of */
    Tcl_PathPart portion;       /* Requested portion of name */
{
    if (pathPtr->typePtr == &tclFsPathType) {
	FsPath *fsPathPtr = (FsPath*) PATHOBJ(pathPtr);
	if (PATHFLAGS(pathPtr) != 0) {
	    switch (portion) {
		case TCL_PATH_DIRNAME: {
		    Tcl_IncrRefCount(fsPathPtr->cwdPtr);
		    return fsPathPtr->cwdPtr;
		}
		case TCL_PATH_TAIL: {
		    Tcl_IncrRefCount(fsPathPtr->normPathPtr);
		    return fsPathPtr->normPathPtr;
		}
		case TCL_PATH_EXTENSION: {
		    return GetExtension(fsPathPtr->normPathPtr);
		}
		case TCL_PATH_ROOT: {
		    /* Unimplemented */
		    CONST char *fileName, *extension;
		    int length;
		    fileName = Tcl_GetStringFromObj(fsPathPtr->normPathPtr, 
						    &length);
		    extension = TclGetExtension(fileName);
		    if (extension == NULL) {
			/* 
			 * There is no extension so the root is the
			 * same as the path we were given.
			 */
			Tcl_IncrRefCount(pathPtr);
			return pathPtr;
		    } else {
			/*
			 * Duplicate the object we were given and
			 * then trim off the extension of the
			 * tail component of the path.
			 */
			Tcl_Obj *root;
			FsPath *fsDupPtr;
			root = Tcl_DuplicateObj(pathPtr);
			Tcl_IncrRefCount(root);
			fsDupPtr = (FsPath*) PATHOBJ(root);
			if (Tcl_IsShared(fsDupPtr->normPathPtr)) {
			    Tcl_DecrRefCount(fsDupPtr->normPathPtr);
			    fsDupPtr->normPathPtr = Tcl_NewStringObj(fileName,
					     (int)(length - strlen(extension)));
			    Tcl_IncrRefCount(fsDupPtr->normPathPtr);
			} else {
			    Tcl_SetObjLength(fsDupPtr->normPathPtr, 
					     (int)(length - strlen(extension)));
			}
			return root;
		    }
		}
		default: {
		    /* We should never get here */
		    Tcl_Panic("Bad portion to TclPathPart");
		    /* For less clever compilers */
		    return NULL;
		}
	    }
	} else if (fsPathPtr->cwdPtr != NULL) {
	    /* Relative path */
	    goto standardPath;
	} else {
	    /* Absolute path */
	    goto standardPath;
	}
    } else {
	int splitElements;
	Tcl_Obj *splitPtr;
	Tcl_Obj *resultPtr = NULL;
      standardPath:

        if (portion == TCL_PATH_EXTENSION) {
	    return GetExtension(pathPtr);
        } else if (portion == TCL_PATH_ROOT) {
	    int length;
	    CONST char *fileName, *extension;
	    
	    fileName = Tcl_GetStringFromObj(pathPtr, &length);
	    extension = TclGetExtension(fileName);
	    if (extension == NULL) {
		Tcl_IncrRefCount(pathPtr);
		return pathPtr;
	    } else {
		Tcl_Obj *root = Tcl_NewStringObj(fileName, 
			(int) (length - strlen(extension)));
		Tcl_IncrRefCount(root);
		return root;
	    }
        }
        
	/* 
	 * The behaviour we want here is slightly different to
	 * the standard Tcl_FSSplitPath in the handling of home
	 * directories; Tcl_FSSplitPath preserves the "~" while 
	 * this code computes the actual full path name, if we
	 * had just a single component.
	 */    
	splitPtr = Tcl_FSSplitPath(pathPtr, &splitElements);
	Tcl_IncrRefCount(splitPtr);
	if ((splitElements == 1) && (Tcl_GetString(pathPtr)[0] == '~')) {
	    Tcl_Obj *norm;
	    
	    Tcl_DecrRefCount(splitPtr);
	    norm = Tcl_FSGetNormalizedPath(interp, pathPtr);
	    if (norm == NULL) {
		return NULL;
	    }
	    splitPtr = Tcl_FSSplitPath(norm, &splitElements);
	    Tcl_IncrRefCount(splitPtr);
	}
	if (portion == TCL_PATH_TAIL) {
	    /*
	     * Return the last component, unless it is the only component,
	     * and it is the root of an absolute path.
	     */

	    if ((splitElements > 0) && ((splitElements > 1)
	      || (Tcl_FSGetPathType(pathPtr) == TCL_PATH_RELATIVE))) {
		Tcl_ListObjIndex(NULL, splitPtr, splitElements-1, &resultPtr);
	    } else {
		resultPtr = Tcl_NewObj();
	    }
	} else {
	    /*
	     * Return all but the last component.  If there is only one
	     * component, return it if the path was non-relative, otherwise
	     * return the current directory.
	     */

	    if (splitElements > 1) {
		resultPtr = Tcl_FSJoinPath(splitPtr, splitElements - 1);
	    } else if (splitElements == 0 || 
	      (Tcl_FSGetPathType(pathPtr) == TCL_PATH_RELATIVE)) {
		resultPtr = Tcl_NewStringObj(
			((tclPlatform == TCL_PLATFORM_MAC) ? ":" : "."), 1);
	    } else {
		Tcl_ListObjIndex(NULL, splitPtr, 0, &resultPtr);
	    }
	}
	Tcl_IncrRefCount(resultPtr);
	Tcl_DecrRefCount(splitPtr);
	return resultPtr;
    }
}

/*
 * Simple helper function 
 */
static Tcl_Obj*
GetExtension(pathPtr) 
    Tcl_Obj *pathPtr;
{
    CONST char *tail, *extension;
    Tcl_Obj *ret;
    
    tail = Tcl_GetString(pathPtr);
    extension = TclGetExtension(tail);
    if (extension == NULL) {
	ret = Tcl_NewObj();
    } else {
	ret = Tcl_NewStringObj(extension, -1);
    }
    Tcl_IncrRefCount(ret);
    return ret;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_FSJoinPath --
 *
 *      This function takes the given Tcl_Obj, which should be a valid
 *      list, and returns the path object given by considering the
 *      first 'elements' elements as valid path segments.  If elements < 0,
 *      we use the entire list.
 *      
 *      It is possible that the returned object is actually an element
 *      of the given list, so the caller should be careful to store a
 *      refCount to it before freeing the list.
 *      
 * Results:
 *      Returns object with refCount of zero, (or if non-zero, it has
 *      references elsewhere in Tcl).  Either way, the caller must
 *      increment its refCount before use.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */
Tcl_Obj* 
Tcl_FSJoinPath(listObj, elements)
    Tcl_Obj *listObj;  /* Path elements to join, may have refCount 0 */
    int elements;      /* Number of elements to use (-1 = all) */
{
    Tcl_Obj *res;
    int i;
    Tcl_Filesystem *fsPtr = NULL;
    
    if (elements < 0) {
	if (Tcl_ListObjLength(NULL, listObj, &elements) != TCL_OK) {
	    return NULL;
	}
    } else {
	/* Just make sure it is a valid list */
	int listTest;
	if (Tcl_ListObjLength(NULL, listObj, &listTest) != TCL_OK) {
	    return NULL;
	}
	/* 
	 * Correct this if it is too large, otherwise we will
	 * waste our time joining null elements to the path 
	 */
	if (elements > listTest) {
	    elements = listTest;
	}
    }
    
    res = NULL;
    
    for (i = 0; i < elements; i++) {
	Tcl_Obj *elt;
	int driveNameLength;
	Tcl_PathType type;
	char *strElt;
	int strEltLen;
	int length;
	char *ptr;
	Tcl_Obj *driveName = NULL;
	
	Tcl_ListObjIndex(NULL, listObj, i, &elt);
	
	/* 
	 * This is a special case where we can be much more
	 * efficient, where we are joining a single relative path
	 * onto an object that is already of path type.  The 
	 * 'TclNewFSPathObj' call below creates an object which
	 * can be normalized more efficiently.  Currently we only
	 * use the special case when we have exactly two elements,
	 * but we could expand that in the future.
	 */
	if ((i == (elements-2)) && (i == 0) && (elt->typePtr == &tclFsPathType)
	  && !(elt->bytes != NULL && (elt->bytes[0] == '\0'))) {
	    Tcl_Obj *tail;
	    Tcl_PathType type;
	    Tcl_ListObjIndex(NULL, listObj, i+1, &tail);
	    type = TclGetPathType(tail, NULL, NULL, NULL);
	    if (type == TCL_PATH_RELATIVE) {
		CONST char *str;
		int len;
		str = Tcl_GetStringFromObj(tail,&len);
		if (len == 0) {
		    /* 
		     * This happens if we try to handle the root volume
		     * '/'.  There's no need to return a special path
		     * object, when the base itself is just fine!
		     */
		    if (res != NULL) Tcl_DecrRefCount(res);
		    return elt;
		}
		/* 
		 * If it doesn't begin with '.'  and is a mac or unix
		 * path or it a windows path without backslashes, then we
		 * can be very efficient here.  (In fact even a windows
		 * path with backslashes can be joined efficiently, but
		 * the path object would not have forward slashes only,
		 * and this would therefore contradict our 'file join'
		 * documentation).
		 */
		if (str[0] != '.' && ((tclPlatform != TCL_PLATFORM_WINDOWS) 
				      || (strchr(str, '\\') == NULL))) {
		    if (res != NULL) Tcl_DecrRefCount(res);
		    return TclNewFSPathObj(elt, str, len);
		}
		/* 
		 * Otherwise we don't have an easy join, and
		 * we must let the more general code below handle
		 * things
		 */
	    } else {
		if (tclPlatform == TCL_PLATFORM_UNIX) {
		    if (res != NULL) Tcl_DecrRefCount(res);
		    return tail;
		} else {
		    CONST char *str;
		    int len;
		    str = Tcl_GetStringFromObj(tail,&len);
		    if (tclPlatform == TCL_PLATFORM_WINDOWS) {
			if (strchr(str, '\\') == NULL) {
			    if (res != NULL) Tcl_DecrRefCount(res);
			    return tail;
			}
		    } else if (tclPlatform == TCL_PLATFORM_MAC) {
			if (strchr(str, '/') == NULL) {
			    if (res != NULL) Tcl_DecrRefCount(res);
			    return tail;
			}
		    }
		}
	    }
	}
	strElt = Tcl_GetStringFromObj(elt, &strEltLen);
	type = TclGetPathType(elt, &fsPtr, &driveNameLength, &driveName);
	if (type != TCL_PATH_RELATIVE) {
	    /* Zero out the current result */
	    if (res != NULL) Tcl_DecrRefCount(res);

	    if (driveName != NULL) {
		/*
		 * We've been given a separate drive-name object,
		 * because the prefix in 'elt' is not in a suitable
		 * format for us (e.g. it may contain irrelevant
		 * multiple separators, like C://///foo).
		 */
		res = Tcl_DuplicateObj(driveName);
		Tcl_DecrRefCount(driveName);
		/* 
		 * Do not set driveName to NULL, because we will check
		 * its value below (but we won't access the contents,
		 * since those have been cleaned-up).
		 */
	    } else {
		res = Tcl_NewStringObj(strElt, driveNameLength);
	    }
	    strElt += driveNameLength;
	}
	
	/* 
	 * Optimisation block: if this is the last element to be
	 * examined, and it is absolute or the only element, and the
	 * drive-prefix was ok (if there is one), it might be that the
	 * path is already in a suitable form to be returned.  Then we
	 * can short-cut the rest of this procedure.
	 */
	if ((driveName == NULL) && (i == (elements - 1)) 
	  && (type != TCL_PATH_RELATIVE || res == NULL)) {
	    /* 
	     * It's the last path segment.  Perform a quick check if
	     * the path is already in a suitable form.
	     */
	    int equal = 1;
	    
	    if (tclPlatform == TCL_PLATFORM_WINDOWS) {
		if (strchr(strElt, '\\') != NULL) {
		    equal = 0;
		}
	    }
	    if (equal && (tclPlatform != TCL_PLATFORM_MAC)) {
		ptr = strElt;
		while (*ptr != '\0') {
		    if (*ptr == '/' && (ptr[1] == '/' || ptr[1] == '\0')) {
			equal = 0;
			break;
		    }
		    ptr++;
		}
	    }
	    if (equal && (tclPlatform == TCL_PLATFORM_MAC)) {
		/*
		 * If it contains any colons, then it mustn't contain
		 * any duplicates.  Otherwise, the path is in unix-form
		 * and is no good.
		 */
		if (strchr(strElt, ':') != NULL) {
		    ptr = strElt;
		    while (*ptr != '\0') {
			if (*ptr == ':' && (ptr[1] == ':' || ptr[1] == '\0')) {
			    equal = 0;
			    break;
			}
			ptr++;
		    }
		} else {
		    equal = 0;
		}
	    }
	    if (equal) {
		if (res != NULL) Tcl_DecrRefCount(res);
		/* 
		 * This element is just what we want to return already -
		 * no further manipulation is requred.
		 */
		return elt;
	    }
	}
	
	if (res == NULL) {
	    res = Tcl_NewObj();
	    ptr = Tcl_GetStringFromObj(res, &length);
	} else {
	    ptr = Tcl_GetStringFromObj(res, &length);
	}
	
	/* 
	 * Strip off any './' before a tilde, unless this is the
	 * beginning of the path.
	 */
	if (length > 0 && strEltLen > 0 
	  && (strElt[0] == '.') && (strElt[1] == '/') && (strElt[2] == '~')) {
	    strElt += 2;
	}

	/* 
	 * A NULL value for fsPtr at this stage basically means
	 * we're trying to join a relative path onto something
	 * which is also relative (or empty).  There's nothing
	 * particularly wrong with that.
	 */
	if (*strElt == '\0') continue;
	
	if (fsPtr == &tclNativeFilesystem || fsPtr == NULL) {
	    TclpNativeJoinPath(res, strElt);
	} else {
	    char separator = '/';
	    int needsSep = 0;
	    
	    if (fsPtr->filesystemSeparatorProc != NULL) {
		Tcl_Obj *sep = (*fsPtr->filesystemSeparatorProc)(res);
		if (sep != NULL) {
		    separator = Tcl_GetString(sep)[0];
		}
	    }

	    if (length > 0 && ptr[length -1] != '/') {
		Tcl_AppendToObj(res, &separator, 1);
		length++;
	    }
	    Tcl_SetObjLength(res, length + (int) strlen(strElt));
	    
	    ptr = Tcl_GetString(res) + length;
	    for (; *strElt != '\0'; strElt++) {
		if (*strElt == separator) {
		    while (strElt[1] == separator) {
			strElt++;
		    }
		    if (strElt[1] != '\0') {
			if (needsSep) {
			    *ptr++ = separator;
			}
		    }
		} else {
		    *ptr++ = *strElt;
		    needsSep = 1;
		}
	    }
	    length = ptr - Tcl_GetString(res);
	    Tcl_SetObjLength(res, length);
	}
    }
    return res;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_FSConvertToPathType --
 *
 *      This function tries to convert the given Tcl_Obj to a valid
 *      Tcl path type, taking account of the fact that the cwd may
 *      have changed even if this object is already supposedly of
 *      the correct type.
 *      
 *      The filename may begin with "~" (to indicate current user's
 *      home directory) or "~<user>" (to indicate any user's home
 *      directory).
 *
 * Results:
 *      Standard Tcl error code.
 *
 * Side effects:
 *	The old representation may be freed, and new memory allocated.
 *
 *---------------------------------------------------------------------------
 */
int 
Tcl_FSConvertToPathType(interp, pathPtr)
    Tcl_Interp *interp;		/* Interpreter in which to store error
				 * message (if necessary). */
    Tcl_Obj *pathPtr;		/* Object to convert to a valid, current
				 * path type. */
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&tclFsDataKey);

    /* 
     * While it is bad practice to examine an object's type directly,
     * this is actually the best thing to do here.  The reason is that
     * if we are converting this object to FsPath type for the first
     * time, we don't need to worry whether the 'cwd' has changed.
     * On the other hand, if this object is already of FsPath type,
     * and is a relative path, we do have to worry about the cwd.
     * If the cwd has changed, we must recompute the path.
     */
    if (pathPtr->typePtr == &tclFsPathType) {
	FsPath *fsPathPtr = (FsPath*) PATHOBJ(pathPtr);
	if (fsPathPtr->filesystemEpoch != tsdPtr->filesystemEpoch) {
	    if (pathPtr->bytes == NULL) {
		UpdateStringOfFsPath(pathPtr);
	    }
	    FreeFsPathInternalRep(pathPtr);
	    pathPtr->typePtr = NULL;
	    return Tcl_ConvertToType(interp, pathPtr, &tclFsPathType);
	}
	return TCL_OK;
	/* 
	 * We used to have more complex code here:
	 * 
	 * if (fsPathPtr->cwdPtr == NULL || PATHFLAGS(pathPtr) != 0) {
	 *     return TCL_OK;
	 * } else {
	 *     if (TclFSCwdPointerEquals(&fsPathPtr->cwdPtr)) {
	 *         return TCL_OK;
	 *     } else {
	 *         if (pathPtr->bytes == NULL) {
	 *             UpdateStringOfFsPath(pathPtr);
	 *         }
	 *         FreeFsPathInternalRep(pathPtr);
	 *         pathPtr->typePtr = NULL;
	 *         return Tcl_ConvertToType(interp, pathPtr, &tclFsPathType);
	 *     }
	 * }
	 * 
	 * But we no longer believe this is necessary.
	 */
    } else {
	return Tcl_ConvertToType(interp, pathPtr, &tclFsPathType);
    }
}

/* 
 * Helper function for normalization.
 */
static int
IsSeparatorOrNull(ch)
    int ch;
{
    if (ch == 0) {
        return 1;
    }
    switch (tclPlatform) {
	case TCL_PLATFORM_UNIX: {
	    return (ch == '/' ? 1 : 0);
	}
	case TCL_PLATFORM_MAC: {
	    return (ch == ':' ? 1 : 0);
	}
	case TCL_PLATFORM_WINDOWS: {
	    return ((ch == '/' || ch == '\\') ? 1 : 0);
	}
    }
    return 0;
}

/* 
 * Helper function for SetFsPathFromAny.  Returns position of first
 * directory delimiter in the path.  If no separator is found, then
 * returns the position of the end of the string.
 */
static int
FindSplitPos(path, separator)
    CONST char *path;
    int separator;
{
    int count = 0;
    switch (tclPlatform) {
	case TCL_PLATFORM_UNIX:
	case TCL_PLATFORM_MAC:
	    while (path[count] != 0) {
		if (path[count] == separator) {
		    return count;
		}
		count++;
	    }
	    break;

	case TCL_PLATFORM_WINDOWS:
	    while (path[count] != 0) {
		if (path[count] == separator || path[count] == '\\') {
		    return count;
		}
		count++;
	    }
	    break;
    }
    return count;
}

/*
 *---------------------------------------------------------------------------
 *
 * TclNewFSPathObj --
 *
 *      Creates a path object whose string representation is '[file join
 *      dirPtr addStrRep]', but does so in a way that allows for more
 *      efficient creation and caching of normalized paths, and more
 *      efficient 'file dirname', 'file tail', etc.
 *      
 * Assumptions:
 *      'dirPtr' must be an absolute path.  
 *      'len' may not be zero.
 *      
 * Results:
 *      The new Tcl object, with refCount zero.
 *
 * Side effects:
 *	Memory is allocated.  'dirPtr' gets an additional refCount.
 *
 *---------------------------------------------------------------------------
 */

Tcl_Obj*
TclNewFSPathObj(Tcl_Obj *dirPtr, CONST char *addStrRep, int len)
{
    FsPath *fsPathPtr;
    Tcl_Obj *pathPtr;
    ThreadSpecificData *tsdPtr;
    
    tsdPtr = TCL_TSD_INIT(&tclFsDataKey);
    
    pathPtr = Tcl_NewObj();
    fsPathPtr = (FsPath*)ckalloc((unsigned)sizeof(FsPath));
    
    if (tclPlatform == TCL_PLATFORM_MAC) { 
	/* 
	 * Mac relative paths may begin with a directory separator ':'. 
	 * If present, we need to skip this ':' because we assume that 
	 * we can join dirPtr and addStrRep by concatenating them as 
	 * strings (and we ensure that dirPtr is terminated by a ':'). 
	 */ 
	if (addStrRep[0] == ':') { 
	    addStrRep++; 
	    len--; 
	} 
    }
    /* Setup the path */
    fsPathPtr->translatedPathPtr = NULL;
    fsPathPtr->normPathPtr = Tcl_NewStringObj(addStrRep, len);
    Tcl_IncrRefCount(fsPathPtr->normPathPtr);
    fsPathPtr->cwdPtr = dirPtr;
    Tcl_IncrRefCount(dirPtr);
    fsPathPtr->nativePathPtr = NULL;
    fsPathPtr->fsRecPtr = NULL;
    fsPathPtr->filesystemEpoch = tsdPtr->filesystemEpoch;

    PATHOBJ(pathPtr) = (VOID *) fsPathPtr;
    PATHFLAGS(pathPtr) = TCLPATH_APPENDED;
    pathPtr->typePtr = &tclFsPathType;
    pathPtr->bytes = NULL;
    pathPtr->length = 0;

    return pathPtr;
}

/*
 *---------------------------------------------------------------------------
 *
 * TclFSMakePathRelative --
 *
 *      Only for internal use.
 *      
 *      Takes a path and a directory, where we _assume_ both path and
 *      directory are absolute, normalized and that the path lies
 *      inside the directory.  Returns a Tcl_Obj representing filename 
 *      of the path relative to the directory.
 *      
 * Results:
 *      NULL on error, otherwise a valid object, typically with
 *      refCount of zero, which it is assumed the caller will
 *      increment.
 *
 * Side effects:
 *	The old representation may be freed, and new memory allocated.
 *
 *---------------------------------------------------------------------------
 */

Tcl_Obj*
TclFSMakePathRelative(interp, pathPtr, cwdPtr)
    Tcl_Interp *interp;		/* Used for error reporting if not NULL. */
    Tcl_Obj *pathPtr;		/* The object we have. */
    Tcl_Obj *cwdPtr;		/* Make it relative to this. */
{
    int cwdLen, len;
    CONST char *tempStr;
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&tclFsDataKey);
    
    if (pathPtr->typePtr == &tclFsPathType) {
	FsPath* fsPathPtr = (FsPath*) PATHOBJ(pathPtr);
	if (PATHFLAGS(pathPtr) != 0 
		&& fsPathPtr->cwdPtr == cwdPtr) {
	    pathPtr = fsPathPtr->normPathPtr;
	    /* Free old representation */
	    if (pathPtr->typePtr != NULL) {
		if (pathPtr->bytes == NULL) {
		    if (pathPtr->typePtr->updateStringProc == NULL) {
			if (interp != NULL) {
			    Tcl_ResetResult(interp);
			    Tcl_AppendResult(interp, "can't find object",
				   "string representation", (char *) NULL);
			}
			return NULL;
		    }
		    pathPtr->typePtr->updateStringProc(pathPtr);
		}
		if ((pathPtr->typePtr->freeIntRepProc) != NULL) {
		    (*pathPtr->typePtr->freeIntRepProc)(pathPtr);
		}
	    }

	    fsPathPtr = (FsPath*)ckalloc((unsigned)sizeof(FsPath));

	    /* Circular reference, by design */
	    fsPathPtr->translatedPathPtr = pathPtr;
	    fsPathPtr->normPathPtr = NULL;
	    fsPathPtr->cwdPtr = cwdPtr;
	    Tcl_IncrRefCount(cwdPtr);
	    fsPathPtr->nativePathPtr = NULL;
	    fsPathPtr->fsRecPtr = NULL;
	    fsPathPtr->filesystemEpoch = tsdPtr->filesystemEpoch;

	    PATHOBJ(pathPtr) = (VOID *) fsPathPtr;
	    PATHFLAGS(pathPtr) = 0;
	    pathPtr->typePtr = &tclFsPathType;

	    return pathPtr;
	}
    }
    /* 
     * We know the cwd is a normalised object which does
     * not end in a directory delimiter, unless the cwd
     * is the name of a volume, in which case it will
     * end in a delimiter!  We handle this situation here.
     * A better test than the '!= sep' might be to simply
     * check if 'cwd' is a root volume.
     * 
     * Note that if we get this wrong, we will strip off
     * either too much or too little below, leading to
     * wrong answers returned by glob.
     */
    tempStr = Tcl_GetStringFromObj(cwdPtr, &cwdLen);
    /* 
     * Should we perhaps use 'Tcl_FSPathSeparator'?
     * But then what about the Windows special case?
     * Perhaps we should just check if cwd is a root
     * volume.
     */
    switch (tclPlatform) {
	case TCL_PLATFORM_UNIX:
	    if (tempStr[cwdLen-1] != '/') {
		cwdLen++;
	    }
	    break;
	case TCL_PLATFORM_WINDOWS:
	    if (tempStr[cwdLen-1] != '/' 
		    && tempStr[cwdLen-1] != '\\') {
		cwdLen++;
	    }
	    break;
	case TCL_PLATFORM_MAC:
	    if (tempStr[cwdLen-1] != ':') {
		cwdLen++;
	    }
	    break;
    }
    tempStr = Tcl_GetStringFromObj(pathPtr, &len);

    return Tcl_NewStringObj(tempStr + cwdLen, len - cwdLen);
}

/*
 *---------------------------------------------------------------------------
 *
 * TclFSMakePathFromNormalized --
 *
 *      Like SetFsPathFromAny, but assumes the given object is an
 *      absolute normalized path. Only for internal use.
 *      
 * Results:
 *      Standard Tcl error code.
 *
 * Side effects:
 *	The old representation may be freed, and new memory allocated.
 *
 *---------------------------------------------------------------------------
 */

int
TclFSMakePathFromNormalized(interp, pathPtr, nativeRep)
    Tcl_Interp *interp;		/* Used for error reporting if not NULL. */
    Tcl_Obj *pathPtr;		/* The object to convert. */
    ClientData nativeRep;	/* The native rep for the object, if known
				 * else NULL. */
{
    FsPath *fsPathPtr;
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&tclFsDataKey);

    if (pathPtr->typePtr == &tclFsPathType) {
	return TCL_OK;
    }
    
    /* Free old representation */
    if (pathPtr->typePtr != NULL) {
	if (pathPtr->bytes == NULL) {
	    if (pathPtr->typePtr->updateStringProc == NULL) {
		if (interp != NULL) {
		    Tcl_ResetResult(interp);
		    Tcl_AppendResult(interp, "can't find object",
				     "string representation", (char *) NULL);
		}
		return TCL_ERROR;
	    }
	    pathPtr->typePtr->updateStringProc(pathPtr);
	}
	if ((pathPtr->typePtr->freeIntRepProc) != NULL) {
	    (*pathPtr->typePtr->freeIntRepProc)(pathPtr);
	}
    }

    fsPathPtr = (FsPath*)ckalloc((unsigned)sizeof(FsPath));
    /* It's a pure normalized absolute path */
    fsPathPtr->translatedPathPtr = NULL;
    /* Circular reference by design */
    fsPathPtr->normPathPtr = pathPtr;
    fsPathPtr->cwdPtr = NULL;
    fsPathPtr->nativePathPtr = nativeRep;
    fsPathPtr->fsRecPtr = NULL;
    fsPathPtr->filesystemEpoch = tsdPtr->filesystemEpoch;

    PATHOBJ(pathPtr) = (VOID *) fsPathPtr;
    PATHFLAGS(pathPtr) = 0;
    pathPtr->typePtr = &tclFsPathType;

    return TCL_OK;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_FSNewNativePath --
 *
 *      This function performs the something like that reverse of the 
 *      usual obj->path->nativerep conversions.  If some code retrieves
 *      a path in native form (from, e.g. readlink or a native dialog),
 *      and that path is to be used at the Tcl level, then calling
 *      this function is an efficient way of creating the appropriate
 *      path object type.
 *      
 *      Any memory which is allocated for 'clientData' should be retained
 *      until clientData is passed to the filesystem's freeInternalRepProc
 *      when it can be freed.  The built in platform-specific filesystems
 *      use 'ckalloc' to allocate clientData, and ckfree to free it.
 *
 * Results:
 *      NULL or a valid path object pointer, with refCount zero.
 *
 * Side effects:
 *	New memory may be allocated.
 *
 *---------------------------------------------------------------------------
 */

Tcl_Obj *
Tcl_FSNewNativePath(fromFilesystem, clientData)
    Tcl_Filesystem* fromFilesystem;
    ClientData clientData;
{
    Tcl_Obj *pathPtr;
    FsPath *fsPathPtr;

    FilesystemRecord *fsFromPtr;
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&tclFsDataKey);
    
    pathPtr = TclFSInternalToNormalized(fromFilesystem, clientData,
                                       &fsFromPtr);
    if (pathPtr == NULL) {
	return NULL;
    }
    
    /* 
     * Free old representation; shouldn't normally be any,
     * but best to be safe. 
     */
    if (pathPtr->typePtr != NULL) {
	if (pathPtr->bytes == NULL) {
	    if (pathPtr->typePtr->updateStringProc == NULL) {
		return NULL;
	    }
	    pathPtr->typePtr->updateStringProc(pathPtr);
	}
	if ((pathPtr->typePtr->freeIntRepProc) != NULL) {
	    (*pathPtr->typePtr->freeIntRepProc)(pathPtr);
	}
    }
    
    fsPathPtr = (FsPath*)ckalloc((unsigned)sizeof(FsPath));

    fsPathPtr->translatedPathPtr = NULL;
    /* Circular reference, by design */
    fsPathPtr->normPathPtr = pathPtr;
    fsPathPtr->cwdPtr = NULL;
    fsPathPtr->nativePathPtr = clientData;
    fsPathPtr->fsRecPtr = fsFromPtr;
    fsPathPtr->fsRecPtr->fileRefCount++;
    fsPathPtr->filesystemEpoch = tsdPtr->filesystemEpoch;  

    PATHOBJ(pathPtr) = (VOID *) fsPathPtr;
    PATHFLAGS(pathPtr) = 0;
    pathPtr->typePtr = &tclFsPathType;

    return pathPtr;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_FSGetTranslatedPath --
 *
 *      This function attempts to extract the translated path
 *      from the given Tcl_Obj.  If the translation succeeds (i.e. the
 *      object is a valid path), then it is returned.  Otherwise NULL
 *      will be returned, and an error message may be left in the
 *      interpreter (if it is non-NULL)
 *
 * Results:
 *      NULL or a valid Tcl_Obj pointer.
 *
 * Side effects:
 *	Only those of 'Tcl_FSConvertToPathType'
 *
 *---------------------------------------------------------------------------
 */

Tcl_Obj* 
Tcl_FSGetTranslatedPath(interp, pathPtr)
    Tcl_Interp *interp;
    Tcl_Obj* pathPtr;
{
    Tcl_Obj *retObj = NULL;
    FsPath *srcFsPathPtr;

    if (Tcl_FSConvertToPathType(interp, pathPtr) != TCL_OK) {
	return NULL;
    }
    srcFsPathPtr = (FsPath*) PATHOBJ(pathPtr);
    if (srcFsPathPtr->translatedPathPtr == NULL) {
	if (PATHFLAGS(pathPtr) != 0) {
	    retObj = Tcl_FSGetNormalizedPath(interp, pathPtr);
	} else {
	    /* 
	     * It is a pure absolute, normalized path object.
	     * This is something like being a 'pure list'.  The
	     * object's string, translatedPath and normalizedPath
	     * are all identical.
	     */
	    retObj = srcFsPathPtr->normPathPtr;
	}
    } else {
	/* It is an ordinary path object */
	retObj = srcFsPathPtr->translatedPathPtr;
    }

    Tcl_IncrRefCount(retObj);
    return retObj;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_FSGetTranslatedStringPath --
 *
 *      This function attempts to extract the translated path
 *      from the given Tcl_Obj.  If the translation succeeds (i.e. the
 *      object is a valid path), then the path is returned.  Otherwise NULL
 *      will be returned, and an error message may be left in the
 *      interpreter (if it is non-NULL)
 *
 * Results:
 *      NULL or a valid string.
 *
 * Side effects:
 *	Only those of 'Tcl_FSConvertToPathType'
 *
 *---------------------------------------------------------------------------
 */
CONST char*
Tcl_FSGetTranslatedStringPath(interp, pathPtr)
    Tcl_Interp *interp;
    Tcl_Obj* pathPtr;
{
    Tcl_Obj *transPtr = Tcl_FSGetTranslatedPath(interp, pathPtr);

    if (transPtr != NULL) {
	int len;
	CONST char *result, *orig;
	orig = Tcl_GetStringFromObj(transPtr, &len);
	result = (char*) ckalloc((unsigned)(len+1));
	memcpy((VOID*) result, (VOID*) orig, (size_t) (len+1));
	Tcl_DecrRefCount(transPtr);
	return result;
    }

    return NULL;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_FSGetNormalizedPath --
 *
 *      This important function attempts to extract from the given Tcl_Obj
 *      a unique normalised path representation, whose string value can
 *      be used as a unique identifier for the file.
 *
 * Results:
 *      NULL or a valid path object pointer.
 *
 * Side effects:
 *	New memory may be allocated.  The Tcl 'errno' may be modified
 *      in the process of trying to examine various path possibilities.
 *
 *---------------------------------------------------------------------------
 */

Tcl_Obj* 
Tcl_FSGetNormalizedPath(interp, pathPtr)
    Tcl_Interp *interp;
    Tcl_Obj* pathPtr;
{

    FsPath *fsPathPtr;

    if (Tcl_FSConvertToPathType(interp, pathPtr) != TCL_OK) {
	return NULL;
    }
    fsPathPtr = (FsPath*) PATHOBJ(pathPtr);

    if (PATHFLAGS(pathPtr) != 0) {
	/* 
	 * This is a special path object which is the result of
	 * something like 'file join' 
	 */
	Tcl_Obj *dir, *copy;
	int cwdLen;
	int pathType;
	CONST char *cwdStr;
	ClientData clientData = NULL;
	
	pathType = Tcl_FSGetPathType(fsPathPtr->cwdPtr);
	dir = Tcl_FSGetNormalizedPath(interp, fsPathPtr->cwdPtr);
	if (dir == NULL) {
	    return NULL;
	}
	if (pathPtr->bytes == NULL) {
	    UpdateStringOfFsPath(pathPtr);
	}
	copy = Tcl_DuplicateObj(dir);
	Tcl_IncrRefCount(copy);
	Tcl_IncrRefCount(dir);
	/* We now own a reference on both 'dir' and 'copy' */
	
	cwdStr = Tcl_GetStringFromObj(copy, &cwdLen);
	/* 
	 * Should we perhaps use 'Tcl_FSPathSeparator'?
	 * But then what about the Windows special case?
	 * Perhaps we should just check if cwd is a root volume.
	 * We should never get cwdLen == 0 in this code path.
	 */
	switch (tclPlatform) {
	    case TCL_PLATFORM_UNIX:
		if (cwdStr[cwdLen-1] != '/') {
		    Tcl_AppendToObj(copy, "/", 1);
		    cwdLen++;
		}
		break;
	    case TCL_PLATFORM_WINDOWS:
		if (cwdStr[cwdLen-1] != '/' 
			&& cwdStr[cwdLen-1] != '\\') {
		    Tcl_AppendToObj(copy, "/", 1);
		    cwdLen++;
		}
		break;
	    case TCL_PLATFORM_MAC:
		if (cwdStr[cwdLen-1] != ':') {
		    Tcl_AppendToObj(copy, ":", 1);
		    cwdLen++;
		}
		break;
	}
	Tcl_AppendObjToObj(copy, fsPathPtr->normPathPtr);
	/* 
	 * Normalize the combined string, but only starting after
	 * the end of the previously normalized 'dir'.  This should
	 * be much faster!  We use 'cwdLen-1' so that we are
	 * already pointing at the dir-separator that we know about.
	 * The normalization code will actually start off directly
	 * after that separator.
	 */
	TclFSNormalizeToUniquePath(interp, copy, cwdLen-1, 
	  (fsPathPtr->nativePathPtr == NULL ? &clientData : NULL));
	/* Now we need to construct the new path object */
	
	if (pathType == TCL_PATH_RELATIVE) {
	    FsPath* origDirFsPathPtr;
	    Tcl_Obj *origDir = fsPathPtr->cwdPtr;
	    origDirFsPathPtr = (FsPath*) PATHOBJ(origDir);
	    
	    fsPathPtr->cwdPtr = origDirFsPathPtr->cwdPtr;
	    Tcl_IncrRefCount(fsPathPtr->cwdPtr);
	    
	    Tcl_DecrRefCount(fsPathPtr->normPathPtr);
	    fsPathPtr->normPathPtr = copy;
	    /* That's our reference to copy used */
	    Tcl_DecrRefCount(dir);
	    Tcl_DecrRefCount(origDir);
	} else {
	    Tcl_DecrRefCount(fsPathPtr->cwdPtr);
	    fsPathPtr->cwdPtr = NULL;
	    Tcl_DecrRefCount(fsPathPtr->normPathPtr);
	    fsPathPtr->normPathPtr = copy;
	    /* That's our reference to copy used */
	    Tcl_DecrRefCount(dir);
	}
	if (clientData != NULL) {
	    fsPathPtr->nativePathPtr = clientData;
	}
	PATHFLAGS(pathPtr) = 0;
    }
    /* Ensure cwd hasn't changed */
    if (fsPathPtr->cwdPtr != NULL) {
	if (!TclFSCwdPointerEquals(&fsPathPtr->cwdPtr)) {
	    if (pathPtr->bytes == NULL) {
		UpdateStringOfFsPath(pathPtr);
	    }
	    FreeFsPathInternalRep(pathPtr);
	    pathPtr->typePtr = NULL;
	    if (Tcl_ConvertToType(interp, pathPtr, 
				  &tclFsPathType) != TCL_OK) {
		return NULL;
	    }
	    fsPathPtr = (FsPath*) PATHOBJ(pathPtr);
	} else if (fsPathPtr->normPathPtr == NULL) {
	    int cwdLen;
	    Tcl_Obj *copy;
	    CONST char *cwdStr;
	    ClientData clientData = NULL;
	    
	    copy = Tcl_DuplicateObj(fsPathPtr->cwdPtr);
	    Tcl_IncrRefCount(copy);
	    cwdStr = Tcl_GetStringFromObj(copy, &cwdLen);
	    /* 
	     * Should we perhaps use 'Tcl_FSPathSeparator'?
	     * But then what about the Windows special case?
	     * Perhaps we should just check if cwd is a root volume.
	     * We should never get cwdLen == 0 in this code path.
	     */
	    switch (tclPlatform) {
		case TCL_PLATFORM_UNIX:
		    if (cwdStr[cwdLen-1] != '/') {
			Tcl_AppendToObj(copy, "/", 1);
			cwdLen++;
		    }
		    break;
		case TCL_PLATFORM_WINDOWS:
		    if (cwdStr[cwdLen-1] != '/' 
			    && cwdStr[cwdLen-1] != '\\') {
			Tcl_AppendToObj(copy, "/", 1);
			cwdLen++;
		    }
		    break;
		case TCL_PLATFORM_MAC:
		    if (cwdStr[cwdLen-1] != ':') {
			Tcl_AppendToObj(copy, ":", 1);
			cwdLen++;
		    }
		    break;
	    }
	    Tcl_AppendObjToObj(copy, pathPtr);
	    /* 
	     * Normalize the combined string, but only starting after
	     * the end of the previously normalized 'dir'.  This should
	     * be much faster!
	     */
	    TclFSNormalizeToUniquePath(interp, copy, cwdLen-1, 
	      (fsPathPtr->nativePathPtr == NULL ? &clientData : NULL));
	    fsPathPtr->normPathPtr = copy;
	    if (clientData != NULL) {
		fsPathPtr->nativePathPtr = clientData;
	    }
	}
    }
    if (fsPathPtr->normPathPtr == NULL) {
	ClientData clientData = NULL;
	Tcl_Obj *useThisCwd = NULL;
	/* 
	 * Since normPathPtr is NULL, but this is a valid path
	 * object, we know that the translatedPathPtr cannot be NULL.
	 */
	Tcl_Obj *absolutePath = fsPathPtr->translatedPathPtr;
	char *path = Tcl_GetString(absolutePath);
	
	/* 
	 * We have to be a little bit careful here to avoid infinite loops
	 * we're asking Tcl_FSGetPathType to return the path's type, but
	 * that call can actually result in a lot of other filesystem
	 * action, which might loop back through here.
	 */
	if (path[0] != '\0') {
	    Tcl_PathType type = Tcl_FSGetPathType(pathPtr);
	    if (type == TCL_PATH_RELATIVE) {
		useThisCwd = Tcl_FSGetCwd(interp);

		if (useThisCwd == NULL) return NULL;

		absolutePath = Tcl_FSJoinToPath(useThisCwd, 1, &absolutePath);
		Tcl_IncrRefCount(absolutePath);
		/* We have a refCount on the cwd */
#ifdef __WIN32__
	    } else if (type == TCL_PATH_VOLUME_RELATIVE) {
		/* 
		 * Only Windows has volume-relative paths.  These
		 * paths are rather rare, but is is nice if Tcl can
		 * handle them.  It is much better if we can
		 * handle them here, rather than in the native fs code,
		 * because we really need to have a real absolute path
		 * just below.
		 * 
		 * We do not let this block compile on non-Windows
		 * platforms because the test suite's manual forcing
		 * of tclPlatform can otherwise cause this code path
		 * to be executed, causing various errors because
		 * volume-relative paths really do not exist.
		 */
		useThisCwd = Tcl_FSGetCwd(interp);
		if (useThisCwd == NULL) return NULL;
		
		if (path[0] == '/') {
		    /* 
		     * Path of form /foo/bar which is a path in the
		     * root directory of the current volume.
		     */
		    CONST char *drive = Tcl_GetString(useThisCwd);
		    absolutePath = Tcl_NewStringObj(drive,2);
		    Tcl_AppendToObj(absolutePath, path, -1);
		    Tcl_IncrRefCount(absolutePath);
		    /* We have a refCount on the cwd */
		} else {
		    /* 
		     * Path of form C:foo/bar, but this only makes
		     * sense if the cwd is also on drive C.
		     */
		    int cwdLen;
		    CONST char *drive = Tcl_GetStringFromObj(useThisCwd, 
							     &cwdLen);
		    char drive_cur = path[0];
		    if (drive_cur >= 'a') {
			drive_cur -= ('a' - 'A');
		    }
		    if (drive[0] == drive_cur) {
			absolutePath = Tcl_DuplicateObj(useThisCwd);
			/* We have a refCount on the cwd */
		    } else {
			Tcl_DecrRefCount(useThisCwd);
			useThisCwd = NULL;
			/* 
			 * The path is not in the current drive, but
			 * is volume-relative.  The way Tcl 8.3 handles
			 * this is that it treats such a path as
			 * relative to the root of the drive.  We
			 * therefore behave the same here.
			 */
			absolutePath = Tcl_NewStringObj(path, 2);
		    }
		    Tcl_IncrRefCount(absolutePath);
		    if (drive[cwdLen-1] != '/') {
			/* Only add a trailing '/' if needed */
		        Tcl_AppendToObj(absolutePath, "/", 1);
		    }
		    Tcl_AppendToObj(absolutePath, path+2, -1);
		}
#endif /* __WIN32__ */
	    }
	}
	/* Already has refCount incremented */
	fsPathPtr->normPathPtr = TclFSNormalizeAbsolutePath(interp, absolutePath, 
		       (fsPathPtr->nativePathPtr == NULL ? &clientData : NULL));
	if (0 && (clientData != NULL)) {
	    fsPathPtr->nativePathPtr = 
	      (*fsPathPtr->fsRecPtr->fsPtr->dupInternalRepProc)(clientData);
	}
	/* 
	 * Check if path is pure normalized (this can only be the case
	 * if it is an absolute path).
	 */
	if (useThisCwd == NULL) {
	    if (!strcmp(Tcl_GetString(fsPathPtr->normPathPtr),
			Tcl_GetString(pathPtr))) {
		/* 
		 * The path was already normalized.  
		 * Get rid of the duplicate.
		 */
		Tcl_DecrRefCount(fsPathPtr->normPathPtr);
		/* 
		 * We do *not* increment the refCount for 
		 * this circular reference 
		 */
		fsPathPtr->normPathPtr = pathPtr;
	    }
	} else {
	    /* 
	     * We just need to free an object we allocated above for
	     * relative paths (this was returned by Tcl_FSJoinToPath
	     * above), and then of course store the cwd.
	     */
	    Tcl_DecrRefCount(absolutePath);
	    fsPathPtr->cwdPtr = useThisCwd;
	}
    }

    return fsPathPtr->normPathPtr;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_FSGetInternalRep --
 *
 *      Extract the internal representation of a given path object,
 *      in the given filesystem.  If the path object belongs to a
 *      different filesystem, we return NULL.
 *      
 *      If the internal representation is currently NULL, we attempt
 *      to generate it, by calling the filesystem's 
 *      'Tcl_FSCreateInternalRepProc'.
 *
 * Results:
 *      NULL or a valid internal representation.
 *
 * Side effects:
 *	An attempt may be made to convert the object.
 *
 *---------------------------------------------------------------------------
 */

ClientData 
Tcl_FSGetInternalRep(pathPtr, fsPtr)
    Tcl_Obj* pathPtr;
    Tcl_Filesystem *fsPtr;
{
    FsPath* srcFsPathPtr;
    
    if (Tcl_FSConvertToPathType(NULL, pathPtr) != TCL_OK) {
	return NULL;
    }
    srcFsPathPtr = (FsPath*) PATHOBJ(pathPtr);
    
    /* 
     * We will only return the native representation for the caller's
     * filesystem.  Otherwise we will simply return NULL. This means
     * that there must be a unique bi-directional mapping between paths
     * and filesystems, and that this mapping will not allow 'remapped'
     * files -- files which are in one filesystem but mapped into
     * another.  Another way of putting this is that 'stacked'
     * filesystems are not allowed.  We recognise that this is a
     * potentially useful feature for the future.
     * 
     * Even something simple like a 'pass through' filesystem which
     * logs all activity and passes the calls onto the native system
     * would be nice, but not easily achievable with the current
     * implementation.
     */
    if (srcFsPathPtr->fsRecPtr == NULL) {
	/* 
	 * This only usually happens in wrappers like TclpStat which
	 * create a string object and pass it to TclpObjStat.  Code
	 * which calls the Tcl_FS..  functions should always have a
	 * filesystem already set.  Whether this code path is legal or
	 * not depends on whether we decide to allow external code to
	 * call the native filesystem directly.  It is at least safer
	 * to allow this sub-optimal routing.
	 */
	Tcl_FSGetFileSystemForPath(pathPtr);
	
	/* 
	 * If we fail through here, then the path is probably not a
	 * valid path in the filesystsem, and is most likely to be a
	 * use of the empty path "" via a direct call to one of the
	 * objectified interfaces (e.g. from the Tcl testsuite).
	 */
	srcFsPathPtr = (FsPath*) PATHOBJ(pathPtr);
	if (srcFsPathPtr->fsRecPtr == NULL) {
	    return NULL;
	}
    }

    if (fsPtr != srcFsPathPtr->fsRecPtr->fsPtr) {
	/* 
	 * There is still one possibility we should consider; if the
	 * file belongs to a different filesystem, perhaps it is
	 * actually linked through to a file in our own filesystem
	 * which we do care about.  The way we can check for this
	 * is we ask what filesystem this path belongs to.
	 */
	Tcl_Filesystem *actualFs = Tcl_FSGetFileSystemForPath(pathPtr);
	if (actualFs == fsPtr) {
	    return Tcl_FSGetInternalRep(pathPtr, fsPtr);
	}
	return NULL;
    }

    if (srcFsPathPtr->nativePathPtr == NULL) {
	Tcl_FSCreateInternalRepProc *proc;
	proc = srcFsPathPtr->fsRecPtr->fsPtr->createInternalRepProc;

	if (proc == NULL) {
	    return NULL;
	}
	srcFsPathPtr->nativePathPtr = (*proc)(pathPtr);
    }

    return srcFsPathPtr->nativePathPtr;
}

/*
 *---------------------------------------------------------------------------
 *
 * TclFSEnsureEpochOk --
 *
 *      This will ensure the pathPtr is up to date and can be
 *      converted into a "path" type, and that we are able to generate a
 *      complete normalized path which is used to determine the
 *      filesystem match.
 *
 * Results:
 *      Standard Tcl return code.
 *
 * Side effects:
 *	An attempt may be made to convert the object.
 *
 *---------------------------------------------------------------------------
 */

int 
TclFSEnsureEpochOk(pathPtr, fsPtrPtr)
    Tcl_Obj* pathPtr;
    Tcl_Filesystem **fsPtrPtr;
{
    FsPath* srcFsPathPtr;
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&tclFsDataKey);

    if (pathPtr->typePtr != &tclFsPathType) {
	return TCL_OK;
    }

    srcFsPathPtr = (FsPath*) PATHOBJ(pathPtr);

    /* 
     * Check if the filesystem has changed in some way since
     * this object's internal representation was calculated.
     */
    if (srcFsPathPtr->filesystemEpoch != tsdPtr->filesystemEpoch) {
	/* 
	 * We have to discard the stale representation and 
	 * recalculate it 
	 */
	if (pathPtr->bytes == NULL) {
	    UpdateStringOfFsPath(pathPtr);
	}
	FreeFsPathInternalRep(pathPtr);
	pathPtr->typePtr = NULL;
	if (SetFsPathFromAny(NULL, pathPtr) != TCL_OK) {
	    return TCL_ERROR;
	}
	srcFsPathPtr = (FsPath*) PATHOBJ(pathPtr);
    }
    /* Check whether the object is already assigned to a fs */
    if (srcFsPathPtr->fsRecPtr != NULL) {
	*fsPtrPtr = srcFsPathPtr->fsRecPtr->fsPtr;
    }

    return TCL_OK;
}

void 
TclFSSetPathDetails(pathPtr, fsRecPtr, clientData) 
    Tcl_Obj *pathPtr;
    FilesystemRecord *fsRecPtr;
    ClientData clientData;
{
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&tclFsDataKey);
    FsPath* srcFsPathPtr;
    
    /* Make sure pathPtr is of the correct type */
    if (pathPtr->typePtr != &tclFsPathType) {
	if (SetFsPathFromAny(NULL, pathPtr) != TCL_OK) {
	    return;
	}
    }
    
    srcFsPathPtr = (FsPath*) PATHOBJ(pathPtr);
    srcFsPathPtr->fsRecPtr = fsRecPtr;
    srcFsPathPtr->nativePathPtr = clientData;
    srcFsPathPtr->filesystemEpoch = tsdPtr->filesystemEpoch; 
    fsRecPtr->fileRefCount++;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_FSEqualPaths --
 *
 *      This function tests whether the two paths given are equal path
 *      objects.  If either or both is NULL, 0 is always returned.
 *
 * Results:
 *      1 or 0.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */

int 
Tcl_FSEqualPaths(firstPtr, secondPtr)
    Tcl_Obj* firstPtr;
    Tcl_Obj* secondPtr;
{
    if (firstPtr == secondPtr) {
	return 1;
    } else {
	char *firstStr, *secondStr;
	int firstLen, secondLen, tempErrno;

	if (firstPtr == NULL || secondPtr == NULL) {
	    return 0;
	}
	firstStr  = Tcl_GetStringFromObj(firstPtr, &firstLen);
	secondStr = Tcl_GetStringFromObj(secondPtr, &secondLen);
	if ((firstLen == secondLen) && (strcmp(firstStr, secondStr) == 0)) {
	    return 1;
	}
	/* 
	 * Try the most thorough, correct method of comparing fully
	 * normalized paths
	 */

	tempErrno = Tcl_GetErrno();
	firstPtr = Tcl_FSGetNormalizedPath(NULL, firstPtr);
	secondPtr = Tcl_FSGetNormalizedPath(NULL, secondPtr);
	Tcl_SetErrno(tempErrno);

	if (firstPtr == NULL || secondPtr == NULL) {
	    return 0;
	}
	firstStr  = Tcl_GetStringFromObj(firstPtr, &firstLen);
	secondStr = Tcl_GetStringFromObj(secondPtr, &secondLen);
	if ((firstLen == secondLen) && (strcmp(firstStr, secondStr) == 0)) {
	    return 1;
	}
    }

    return 0;
}

/*
 *---------------------------------------------------------------------------
 *
 * SetFsPathFromAny --
 *
 *      This function tries to convert the given Tcl_Obj to a valid
 *      Tcl path type.
 *      
 *      The filename may begin with "~" (to indicate current user's
 *      home directory) or "~<user>" (to indicate any user's home
 *      directory).
 *
 * Results:
 *      Standard Tcl error code.
 *
 * Side effects:
 *	The old representation may be freed, and new memory allocated.
 *
 *---------------------------------------------------------------------------
 */

static int
SetFsPathFromAny(interp, pathPtr)
    Tcl_Interp *interp;		/* Used for error reporting if not NULL. */
    Tcl_Obj *pathPtr;		/* The object to convert. */
{
    int len;
    FsPath *fsPathPtr;
    Tcl_Obj *transPtr;
    char *name;
    ThreadSpecificData *tsdPtr = TCL_TSD_INIT(&tclFsDataKey);
    
    if (pathPtr->typePtr == &tclFsPathType) {
	return TCL_OK;
    }
    
    /* 
     * First step is to translate the filename.  This is similar to
     * Tcl_TranslateFilename, but shouldn't convert everything to
     * windows backslashes on that platform.  The current
     * implementation of this piece is a slightly optimised version
     * of the various Tilde/Split/Join stuff to avoid multiple
     * split/join operations.
     * 
     * We remove any trailing directory separator.
     * 
     * However, the split/join routines are quite complex, and
     * one has to make sure not to break anything on Unix, Win
     * or MacOS (fCmd.test, fileName.test and cmdAH.test exercise
     * most of the code).
     */
    name = Tcl_GetStringFromObj(pathPtr,&len);

    /*
     * Handle tilde substitutions, if needed.
     */
    if (name[0] == '~') {
	char *expandedUser;
	Tcl_DString temp;
	int split;
	char separator='/';
	
	if (tclPlatform==TCL_PLATFORM_MAC) {
	    if (strchr(name, ':') != NULL) separator = ':';
	}
	
	split = FindSplitPos(name, separator);
	if (split != len) {
	    /* We have multiple pieces '~user/foo/bar...' */
	    name[split] = '\0';
	}
	/* Do some tilde substitution */
	if (name[1] == '\0') {
	    /* We have just '~' */
	    CONST char *dir;
	    Tcl_DString dirString;
	    if (split != len) { name[split] = separator; }
	    
	    dir = TclGetEnv("HOME", &dirString);
	    if (dir == NULL) {
		if (interp) {
		    Tcl_ResetResult(interp);
		    Tcl_AppendResult(interp, "couldn't find HOME environment ",
			    "variable to expand path", (char *) NULL);
		}
		return TCL_ERROR;
	    }
	    Tcl_DStringInit(&temp);
	    Tcl_JoinPath(1, &dir, &temp);
	    Tcl_DStringFree(&dirString);
	} else {
	    /* We have a user name '~user' */
	    Tcl_DStringInit(&temp);
	    if (TclpGetUserHome(name+1, &temp) == NULL) {	
		if (interp != NULL) {
		    Tcl_ResetResult(interp);
		    Tcl_AppendResult(interp, "user \"", (name+1), 
				     "\" doesn't exist", (char *) NULL);
		}
		Tcl_DStringFree(&temp);
		if (split != len) { name[split] = separator; }
		return TCL_ERROR;
	    }
	    if (split != len) { name[split] = separator; }
	}
	
	expandedUser = Tcl_DStringValue(&temp);
	transPtr = Tcl_NewStringObj(expandedUser, Tcl_DStringLength(&temp));

	if (split != len) {
	    /* Join up the tilde substitution with the rest */
	    if (name[split+1] == separator) {

		/*
		 * Somewhat tricky case like ~//foo/bar.
		 * Make use of Split/Join machinery to get it right.
		 * Assumes all paths beginning with ~ are part of the
		 * native filesystem.
		 */

		int objc;
		Tcl_Obj **objv;
		Tcl_Obj *parts = TclpNativeSplitPath(pathPtr, NULL);
		Tcl_ListObjGetElements(NULL, parts, &objc, &objv);
		/* Skip '~'.  It's replaced by its expansion */
		objc--; objv++;
		while (objc--) {
		    TclpNativeJoinPath(transPtr, Tcl_GetString(*objv++));
		}
		Tcl_DecrRefCount(parts);
	    } else {
		/* 
		 * Simple case. "rest" is relative path.  Just join it. 
		 * The "rest" object will be freed when
		 * Tcl_FSJoinToPath returns (unless something else
		 * claims a refCount on it).
		 */
		Tcl_Obj *joined;
		Tcl_Obj *rest = Tcl_NewStringObj(name+split+1,-1);
		Tcl_IncrRefCount(transPtr);
		joined = Tcl_FSJoinToPath(transPtr, 1, &rest);
		Tcl_DecrRefCount(transPtr);
		transPtr = joined;
	    }
	}
	Tcl_DStringFree(&temp);
    } else {
	transPtr = Tcl_FSJoinToPath(pathPtr,0,NULL);
    }

#if defined(__CYGWIN__) && defined(__WIN32__)
    {
    extern int cygwin_conv_to_win32_path 
	_ANSI_ARGS_((CONST char *, char *));
    char winbuf[MAX_PATH+1];

    /*
     * In the Cygwin world, call conv_to_win32_path in order to use the
     * mount table to translate the file name into something Windows will
     * understand.  Take care when converting empty strings!
     */
    name = Tcl_GetStringFromObj(transPtr, &len);
    if (len > 0) {
	cygwin_conv_to_win32_path(name, winbuf);
	TclWinNoBackslash(winbuf);
	Tcl_SetStringObj(transPtr, winbuf, -1);
    }
    }
#endif /* __CYGWIN__ && __WIN32__ */

    /* 
     * Now we have a translated filename in 'transPtr'.  This will have
     * forward slashes on Windows, and will not contain any ~user
     * sequences.
     */
    
    fsPathPtr = (FsPath*)ckalloc((unsigned)sizeof(FsPath));

    fsPathPtr->translatedPathPtr = transPtr;
    if (transPtr != pathPtr) {
        Tcl_IncrRefCount(fsPathPtr->translatedPathPtr);
    }
    fsPathPtr->normPathPtr = NULL;
    fsPathPtr->cwdPtr = NULL;
    fsPathPtr->nativePathPtr = NULL;
    fsPathPtr->fsRecPtr = NULL;
    fsPathPtr->filesystemEpoch = tsdPtr->filesystemEpoch;

    /*
     * Free old representation before installing our new one.
     */
    if (pathPtr->typePtr != NULL && pathPtr->typePtr->freeIntRepProc != NULL) {
	(pathPtr->typePtr->freeIntRepProc)(pathPtr);
    }
    PATHOBJ(pathPtr) = (VOID *) fsPathPtr;
    PATHFLAGS(pathPtr) = 0;
    pathPtr->typePtr = &tclFsPathType;

    return TCL_OK;
}

static void
FreeFsPathInternalRep(pathPtr)
    Tcl_Obj *pathPtr;	/* Path object with internal rep to free. */
{
    FsPath* fsPathPtr = (FsPath*) PATHOBJ(pathPtr);

    if (fsPathPtr->translatedPathPtr != NULL) {
	if (fsPathPtr->translatedPathPtr != pathPtr) {
	    Tcl_DecrRefCount(fsPathPtr->translatedPathPtr);
	}
    }
    if (fsPathPtr->normPathPtr != NULL) {
	if (fsPathPtr->normPathPtr != pathPtr) {
	    Tcl_DecrRefCount(fsPathPtr->normPathPtr);
	}
	fsPathPtr->normPathPtr = NULL;
    }
    if (fsPathPtr->cwdPtr != NULL) {
	Tcl_DecrRefCount(fsPathPtr->cwdPtr);
    }
    if (fsPathPtr->nativePathPtr != NULL) {
	if (fsPathPtr->fsRecPtr != NULL) {
	    if (fsPathPtr->fsRecPtr->fsPtr->freeInternalRepProc != NULL) {
		(*fsPathPtr->fsRecPtr->fsPtr
		   ->freeInternalRepProc)(fsPathPtr->nativePathPtr);
		fsPathPtr->nativePathPtr = NULL;
	    }
	}
    }
    if (fsPathPtr->fsRecPtr != NULL) {
	fsPathPtr->fsRecPtr->fileRefCount--;
	if (fsPathPtr->fsRecPtr->fileRefCount <= 0) {
	    /* It has been unregistered already */
	    ckfree((char *)fsPathPtr->fsRecPtr);
	}
    }

    ckfree((char*) fsPathPtr);
}

static void
DupFsPathInternalRep(srcPtr, copyPtr)
    Tcl_Obj *srcPtr;		/* Path obj with internal rep to copy. */
    Tcl_Obj *copyPtr;		/* Path obj with internal rep to set. */
{
    FsPath* srcFsPathPtr = (FsPath*) PATHOBJ(srcPtr);
    FsPath* copyFsPathPtr = (FsPath*) ckalloc((unsigned)sizeof(FsPath));

    Tcl_FSDupInternalRepProc *dupProc;
    
    PATHOBJ(copyPtr) = (VOID *) copyFsPathPtr;

    if (srcFsPathPtr->translatedPathPtr != NULL) {
	copyFsPathPtr->translatedPathPtr = srcFsPathPtr->translatedPathPtr;
	if (copyFsPathPtr->translatedPathPtr != copyPtr) {
	    Tcl_IncrRefCount(copyFsPathPtr->translatedPathPtr);
	}
    } else {
	copyFsPathPtr->translatedPathPtr = NULL;
    }
    
    if (srcFsPathPtr->normPathPtr != NULL) {
	copyFsPathPtr->normPathPtr = srcFsPathPtr->normPathPtr;
	if (copyFsPathPtr->normPathPtr != copyPtr) {
	    Tcl_IncrRefCount(copyFsPathPtr->normPathPtr);
	}
    } else {
	copyFsPathPtr->normPathPtr = NULL;
    }
    
    if (srcFsPathPtr->cwdPtr != NULL) {
	copyFsPathPtr->cwdPtr = srcFsPathPtr->cwdPtr;
	Tcl_IncrRefCount(copyFsPathPtr->cwdPtr);
    } else {
	copyFsPathPtr->cwdPtr = NULL;
    }

    copyFsPathPtr->flags = srcFsPathPtr->flags;
    
    if (srcFsPathPtr->fsRecPtr != NULL 
      && srcFsPathPtr->nativePathPtr != NULL) {
	dupProc = srcFsPathPtr->fsRecPtr->fsPtr->dupInternalRepProc;
	if (dupProc != NULL) {
	    copyFsPathPtr->nativePathPtr = 
	      (*dupProc)(srcFsPathPtr->nativePathPtr);
	} else {
	    copyFsPathPtr->nativePathPtr = NULL;
	}
    } else {
	copyFsPathPtr->nativePathPtr = NULL;
    }
    copyFsPathPtr->fsRecPtr = srcFsPathPtr->fsRecPtr;
    copyFsPathPtr->filesystemEpoch = srcFsPathPtr->filesystemEpoch;
    if (copyFsPathPtr->fsRecPtr != NULL) {
	copyFsPathPtr->fsRecPtr->fileRefCount++;
    }

    copyPtr->typePtr = &tclFsPathType;
}

/*
 *---------------------------------------------------------------------------
 *
 * UpdateStringOfFsPath --
 *
 *      Gives an object a valid string rep.
 *      
 * Results:
 *      None.
 *
 * Side effects:
 *	Memory may be allocated.
 *
 *---------------------------------------------------------------------------
 */

static void
UpdateStringOfFsPath(pathPtr)
    register Tcl_Obj *pathPtr;	/* path obj with string rep to update. */
{
    FsPath* fsPathPtr = (FsPath*) PATHOBJ(pathPtr);
    CONST char *cwdStr;
    int cwdLen;
    Tcl_Obj *copy;
    
    if (PATHFLAGS(pathPtr) == 0 || fsPathPtr->cwdPtr == NULL) {
	Tcl_Panic("Called UpdateStringOfFsPath with invalid object");
    }
    
    copy = Tcl_DuplicateObj(fsPathPtr->cwdPtr);
    Tcl_IncrRefCount(copy);
    
    cwdStr = Tcl_GetStringFromObj(copy, &cwdLen);
    /* 
     * Should we perhaps use 'Tcl_FSPathSeparator'?
     * But then what about the Windows special case?
     * Perhaps we should just check if cwd is a root volume.
     * We should never get cwdLen == 0 in this code path.
     */
    switch (tclPlatform) {
	case TCL_PLATFORM_UNIX:
	    if (cwdStr[cwdLen-1] != '/') {
		Tcl_AppendToObj(copy, "/", 1);
		cwdLen++;
	    }
	    break;
	case TCL_PLATFORM_WINDOWS:
	    /* 
	     * We need the extra 'cwdLen != 2', and ':' checks because 
	     * a volume relative path doesn't get a '/'.  For example 
	     * 'glob C:*cat*.exe' will return 'C:cat32.exe'
	     */
	    if (cwdStr[cwdLen-1] != '/'
		    && cwdStr[cwdLen-1] != '\\') {
		if (cwdLen != 2 || cwdStr[1] != ':') {
		    Tcl_AppendToObj(copy, "/", 1);
		    cwdLen++;
		}
	    }
	    break;
	case TCL_PLATFORM_MAC:
	    if (cwdStr[cwdLen-1] != ':') {
		Tcl_AppendToObj(copy, ":", 1);
		cwdLen++;
	    }
	    break;
    }
    Tcl_AppendObjToObj(copy, fsPathPtr->normPathPtr);
    pathPtr->bytes = Tcl_GetStringFromObj(copy, &cwdLen);
    pathPtr->length = cwdLen;
    copy->bytes = tclEmptyStringRep;
    copy->length = 0;
    Tcl_DecrRefCount(copy);
}

/*
 *---------------------------------------------------------------------------
 *
 * TclNativePathInFilesystem --
 *
 *      Any path object is acceptable to the native filesystem, by
 *      default (we will throw errors when illegal paths are actually
 *      tried to be used).
 *      
 *      However, this behavior means the native filesystem must be
 *      the last filesystem in the lookup list (otherwise it will
 *      claim all files belong to it, and other filesystems will
 *      never get a look in).
 *
 * Results:
 *      TCL_OK, to indicate 'yes', -1 to indicate no.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */
int 
TclNativePathInFilesystem(pathPtr, clientDataPtr)
    Tcl_Obj *pathPtr;
    ClientData *clientDataPtr;
{
    /* 
     * A special case is required to handle the empty path "". 
     * This is a valid path (i.e. the user should be able
     * to do 'file exists ""' without throwing an error), but
     * equally the path doesn't exist.  Those are the semantics
     * of Tcl (at present anyway), so we have to abide by them
     * here.
     */
    if (pathPtr->typePtr == &tclFsPathType) {
	if (pathPtr->bytes != NULL && pathPtr->bytes[0] == '\0') {
	    /* We reject the empty path "" */
	    return -1;
	}
	/* Otherwise there is no way this path can be empty */
    } else {
	/* 
	 * It is somewhat unusual to reach this code path without
	 * the object being of tclFsPathType.  However, we do
	 * our best to deal with the situation.
	 */
	int len;
	Tcl_GetStringFromObj(pathPtr,&len);
	if (len == 0) {
	    /* We reject the empty path "" */
	    return -1;
	}
    }
    /* 
     * Path is of correct type, or is of non-zero length, 
     * so we accept it.
     */
    return TCL_OK;
}
