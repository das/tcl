/* 
 * tclIOUtil.c --
 *
 *	This file contains the implementation of Tcl's generic
 *	filesystem code, which supports a pluggable filesystem
 *	architecture allowing both platform specific filesystems and
 *	'virtual filesystems'.  All filesystem access should go through
 *	the functions defined in this file.  Most of this code was
 *	contributed by Vince Darley.
 *
 *	Parts of this file are based on code contributed by Karl
 *	Lehenbauer, Mark Diekhans and Peter da Silva.
 *
 * Copyright (c) 1991-1994 The Regents of the University of California.
 * Copyright (c) 1994-1997 Sun Microsystems, Inc.
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id$
 */

#include "tclInt.h"
#include "tclPort.h"

/*
 * Prototypes for procedures defined later in this file.  The last
 * of these could perhaps be exported in the future, if extensions
 * require it.
 */

static void		DupFsPathInternalRep _ANSI_ARGS_((Tcl_Obj *srcPtr,
			    Tcl_Obj *copyPtr));
static void		FreeFsPathInternalRep _ANSI_ARGS_((Tcl_Obj *listPtr));
static int		SetFsPathFromAny _ANSI_ARGS_((Tcl_Interp *interp,
			    Tcl_Obj *objPtr));
static Tcl_Obj*         FSNormalizeAbsolutePath 
                            _ANSI_ARGS_((Tcl_Interp* interp, char *path));
static int              TclNormalizeToUniquePath 
                            _ANSI_ARGS_((Tcl_Interp *interp, Tcl_Obj *pathPtr));
static int		SetFsPathFromAbsoluteNormalized 
                            _ANSI_ARGS_((Tcl_Interp *interp, Tcl_Obj *objPtr));
static int 		FindSplitPos _ANSI_ARGS_((char *path, char *separator));
static Tcl_Filesystem*  Tcl_FSGetFileSystemForPath 
			    _ANSI_ARGS_((Tcl_Obj* pathObjPtr));

/*
 * Define the 'path' object type, which Tcl uses to represent
 * file paths internally.
 */
Tcl_ObjType tclFsPathType = {
    "path",				/* name */
    FreeFsPathInternalRep,		/* freeIntRepProc */
    DupFsPathInternalRep,	        /* dupIntRepProc */
    NULL,				/* updateStringProc */
    SetFsPathFromAny			/* setFromAnyProc */
};

/* 
 * These form part of the native filesystem support.  They are needed
 * here because we have a few native filesystem functions (which are
 * the same for mac/win/unix) in this file.  There is no need to place
 * them in tclInt.h, because they are not (and should not be) used
 * anywhere else.
 */
extern char *			tclpFileAttrStrings[];
extern CONST TclFileAttrProcs	tclpFileAttrProcs[];

/* 
 * The following functions are obsolete string based APIs, and should
 * be removed in a future release.
 */

/* Obsolete */
int
TclStat(path, buf)
    CONST char *path;		/* Path of file to stat (in current CP). */
    struct stat *buf;		/* Filled with results of stat call. */
{
    return Tcl_Stat(path,buf);
}

/* Obsolete */
int
TclAccess(path, mode)
    CONST char *path;		/* Path of file to access (in current CP). */
    int mode;                   /* Permission setting. */
{
    return Tcl_Access(path, mode);
}

/* Obsolete */
int
Tcl_Stat(path, buf)
    CONST char *path;		/* Path of file to stat (in current CP). */
    struct stat *buf;		/* Filled with results of stat call. */
{
    int ret;
    Tcl_Obj *pathPtr = Tcl_NewStringObj(path,-1);
    Tcl_IncrRefCount(pathPtr);
    ret = Tcl_FSStat(pathPtr,buf);
    Tcl_DecrRefCount(pathPtr);
    return ret;
}

/* Obsolete */
int
Tcl_Access(path, mode)
    CONST char *path;		/* Path of file to access (in current CP). */
    int mode;                   /* Permission setting. */
{
    int ret;
    Tcl_Obj *pathPtr = Tcl_NewStringObj(path,-1);
    Tcl_IncrRefCount(pathPtr);
    ret = Tcl_FSAccess(pathPtr,mode);
    Tcl_DecrRefCount(pathPtr);
    return ret;
}

/* Obsolete */
Tcl_Channel
Tcl_OpenFileChannel(interp, path, modeString, permissions)
    Tcl_Interp *interp;                 /* Interpreter for error reporting;
					 * can be NULL. */
    char *path;                         /* Name of file to open. */
    char *modeString;                   /* A list of POSIX open modes or
					 * a string such as "rw". */
    int permissions;                    /* If the open involves creating a
					 * file, with what modes to create
					 * it? */
{
    Tcl_Channel ret;
    Tcl_Obj *pathPtr = Tcl_NewStringObj(path,-1);
    Tcl_IncrRefCount(pathPtr);
    ret = Tcl_FSOpenFileChannel(interp, pathPtr, modeString, permissions);
    Tcl_DecrRefCount(pathPtr);
    return ret;

}

/* Obsolete */
int
Tcl_Chdir(dirName)
    CONST char *dirName;
{
    int ret;
    Tcl_Obj *pathPtr = Tcl_NewStringObj(dirName,-1);
    Tcl_IncrRefCount(pathPtr);
    ret = Tcl_FSChdir(pathPtr);
    Tcl_DecrRefCount(pathPtr);
    return ret;
}

/* Obsolete */
char *
Tcl_GetCwd(interp, cwdPtr)
    Tcl_Interp *interp;
    Tcl_DString *cwdPtr;
{
    Tcl_Obj *cwd;
    cwd = Tcl_FSGetCwd(interp);
    if (cwd == NULL) {
	return NULL;
    } else {
	Tcl_DStringInit(cwdPtr);
	Tcl_DStringAppend(cwdPtr, Tcl_GetString(cwd), -1);
	Tcl_DecrRefCount(cwd);
	return Tcl_DStringValue(cwdPtr);
    }
}

/* Obsolete */
int
Tcl_EvalFile(interp, fileName)
    Tcl_Interp *interp;		/* Interpreter in which to process file. */
    char *fileName;		/* Name of file to process.  Tilde-substitution
				 * will be performed on this name. */
{
    int ret;
    Tcl_Obj *pathPtr = Tcl_NewStringObj(fileName,-1);
    Tcl_IncrRefCount(pathPtr);
    ret = Tcl_FSEvalFile(interp, pathPtr);
    Tcl_DecrRefCount(pathPtr);
    return ret;
}


/* 
 * The 3 hooks for Stat, Access and OpenFileChannel are obsolete.  The
 * complete, general hooked filesystem APIs should be used instead.
 * This define decides whether to include the obsolete hooks and
 * related code.  If these are removed, we'll also want to remove them
 * from stubs/tclInt.  The only known users of these APIs are prowrap
 * and mktclapp.  New code/extensions should not use them, since they
 * do not provide as full support as the full filesystem API.
 */
#define USE_OBSOLETE_FS_HOOKS


#ifdef USE_OBSOLETE_FS_HOOKS
/*
 * The following typedef declarations allow for hooking into the chain
 * of functions maintained for 'Tcl_Stat(...)', 'Tcl_Access(...)' &
 * 'Tcl_OpenFileChannel(...)'.  Basically for each hookable function
 * a linked list is defined.
 */

typedef struct StatProc {
    TclStatProc_ *proc;		 /* Function to process a 'stat()' call */
    struct StatProc *nextPtr;    /* The next 'stat()' function to call */
} StatProc;

typedef struct AccessProc {
    TclAccessProc_ *proc;	 /* Function to process a 'access()' call */
    struct AccessProc *nextPtr;  /* The next 'access()' function to call */
} AccessProc;

typedef struct OpenFileChannelProc {
    TclOpenFileChannelProc_ *proc;  /* Function to process a
				     * 'Tcl_OpenFileChannel()' call */
    struct OpenFileChannelProc *nextPtr;
				    /* The next 'Tcl_OpenFileChannel()'
				     * function to call */
} OpenFileChannelProc;

/*
 * For each type of (obsolete) hookable function, a static node is
 * declared to hold the function pointer for the "built-in" routine
 * (e.g. 'TclpStat(...)') and the respective list is initialized as a
 * pointer to that node.
 * 
 * The "delete" functions (e.g. 'TclStatDeleteProc(...)') ensure that
 * these statically declared list entry cannot be inadvertently removed.
 *
 * This method avoids the need to call any sort of "initialization"
 * function.
 *
 * All three lists are protected by a global obsoleteFsHookMutex.
 */

static StatProc *statProcList = NULL;
static AccessProc *accessProcList = NULL;
static OpenFileChannelProc *openFileChannelProcList = NULL;

TCL_DECLARE_MUTEX(obsoleteFsHookMutex)

#endif /* USE_OBSOLETE_FS_HOOKS */

/* 
 * A filesystem record is used to keep track of each
 * filesystem currently registered with the core,
 * in a linked list.
 */
typedef struct FilesystemRecord {
    ClientData	     clientData;  /* Client specific data for the new
				   * filesystem (can be NULL) */
    Tcl_Filesystem *fsPtr;        /* Pointer to filesystem dispatch
                                   * table. */
    int refCount;                 /* How many Tcl_Obj's use this
                                   * filesystem. */
    struct FilesystemRecord *nextPtr;  
                                  /* The next filesystem registered
                                   * to Tcl, or NULL if no more. */
} FilesystemRecord;

/* 
 * Declare the native filesystem support.  These functions should
 * be considered private to Tcl, and should really not be called
 * directly by any code other than this file (i.e. neither by
 * Tcl's core nor by extensions).  Similarly, the old string-based
 * Tclp... native filesystem functions should not be called.
 * 
 * The correct API to use now is the Tcl_FS... set of functions,
 * which ensure correct and complete virtual filesystem support.
 * 
 * We cannot make all of these static, since some of them
 * are implemented in the platform-specific directories.
 */
static Tcl_FSPathInFilesystemProc NativePathInFilesystem;
static Tcl_FSFilesystemPathTypeProc NativeFilesystemPathType;
static Tcl_FSFilesystemSeparatorProc NativeFilesystemSeparator;
static Tcl_FSFreeInternalRepProc NativeFreeInternalRep;
static Tcl_FSDupInternalRepProc NativeDupInternalRep;
static Tcl_FSCreateInternalRepProc NativeCreateNativeRep;
static Tcl_FSFileAttrStringsProc NativeFileAttrStrings;
static Tcl_FSFileAttrsGetProc NativeFileAttrsGet;
static Tcl_FSFileAttrsSetProc NativeFileAttrsSet;
static Tcl_FSLoadFileProc NativeLoadFile; 
static Tcl_FSOpenFileChannelProc NativeOpenFileChannel; 
static Tcl_FSUtimeProc NativeUtime;

/* 
 * The only reason these functions are not static is that they
 * are either called by code in the native (win/unix/mac) directories
 * or they are actually implemented in those directories.  They
 * should simply not be called by code outside Tcl's native
 * filesystem core.  i.e. they should be considered 'static' to
 * Tcl's filesystem code (if we ever built the native filesystem
 * support into a separate code library, this could actually be
 * enforced).
 */
Tcl_FSInternalToNormalizedProc TclpNativeToNormalized;
Tcl_FSStatProc TclpObjStat;
Tcl_FSAccessProc TclpObjAccess;	    
Tcl_FSMatchInDirectoryProc TclpMatchInDirectory;  
Tcl_FSGetCwdProc TclpObjGetCwd;     
Tcl_FSChdirProc TclpObjChdir;	    
Tcl_FSLstatProc TclpObjLstat;	    
Tcl_FSCopyFileProc TclpObjCopyFile; 
Tcl_FSDeleteFileProc TclpObjDeleteFile;	    
Tcl_FSRenameFileProc TclpObjRenameFile;	    
Tcl_FSCreateDirectoryProc TclpObjCreateDirectory;	    
Tcl_FSCopyDirectoryProc TclpObjCopyDirectory;	    
Tcl_FSRemoveDirectoryProc TclpObjRemoveDirectory;	    
Tcl_FSUnloadFileProc TclpUnloadFile;	    
Tcl_FSReadlinkProc TclpObjReadlink; 
Tcl_FSListVolumesProc TclpListVolumes;	    

/* Define the native filesystem dispatch table */
static Tcl_Filesystem nativeFilesystem = {
    "native",
    sizeof(Tcl_Filesystem),
    TCL_FILESYSTEM_VERSION_1,
    &NativePathInFilesystem,
    &NativeDupInternalRep,
    &NativeFreeInternalRep,
    &TclpNativeToNormalized,
    &NativeCreateNativeRep,
    &TclpObjNormalizePath,
    &NativeFilesystemPathType,
    &NativeFilesystemSeparator,
    &TclpObjStat,
    &TclpObjAccess,
    &NativeOpenFileChannel,
    &TclpMatchInDirectory,
    &NativeUtime,
#ifndef S_IFLNK
    NULL,
#else
    &TclpObjReadlink,
#endif /* S_IFLNK */
    &TclpListVolumes,
    &NativeFileAttrStrings,
    &NativeFileAttrsGet,
    &NativeFileAttrsSet,
    &TclpObjCreateDirectory,
    &TclpObjRemoveDirectory, 
    &TclpObjDeleteFile,
    &TclpObjLstat,
    &TclpObjCopyFile,
    &TclpObjRenameFile,
    &TclpObjCopyDirectory, 
    &NativeLoadFile,
    &TclpUnloadFile,
    &TclpObjGetCwd,
    &TclpObjChdir
};

/* 
 * Define the tail of the linked list.  Note that for unconventional
 * uses of Tcl without a native filesystem, we may in the future wish
 * to modify the current approach of hard-coding the native filesystem
 * in the lookup list 'filesystemList' below.
 */
static FilesystemRecord nativeFilesystemRecord = {
    NULL,
    &nativeFilesystem,
    1,
    NULL
};

/* 
 * The following few variables are protected by the 
 * filesystemMutex just below.
 */

/* 
 * This is incremented each time we modify the linked list of
 * filesystems.  Any time it changes, all cached filesystem
 * representations are suspect and must be freed.
 */
int filesystemEpoch = 0;
/* Stores the linked list of filesystems.*/
static FilesystemRecord *filesystemList = &nativeFilesystemRecord;
/* 
 * The number of loops which are currently iterating over the linked
 * list.  If this is greater than zero, we can't modify the list.
 */
int filesystemIteratorsInProgress = 0;
/* Someone wants to modify the list of filesystems if this is set. */
int filesystemWantToModify = 0;

Tcl_Condition filesystemOkToModify = NULL;

TCL_DECLARE_MUTEX(filesystemMutex)

/* 
 * struct FsPath --
 * 
 * Internal representation of a Tcl_Obj of "path" type.  This
 * can be used to represent relative or absolute paths, and has
 * certain optimisations when used to represent paths which are
 * already normalized and absolute.
 * 
 * Note that 'normPathPtr' can be a circular reference to the
 * container Tcl_Obj of this FsPath.
 */
typedef struct FsPath {
    char *translatedPathPtr;    /* Name without any ~user sequences.
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
 * Used to implement Tcl_FSGetCwd in a file-system independent way.
 * This is protected by the cwdMutex below.
 */
static Tcl_Obj* cwdPathPtr = NULL;
TCL_DECLARE_MUTEX(cwdMutex)

/* 
 * Declare fallback support function and 
 * information for Tcl_FSLoadFile 
 */
static Tcl_FSUnloadFileProc FSUnloadTempFile;

/*
 * One of these structures is used each time we successfully load a
 * file from a file system by way of making a temporary copy of the
 * file on the native filesystem.  We need to store both the actual
 * unloadProc/clientData combination which was used, and the original
 * and modified filenames, so that we can correctly undo the entire
 * operation when we want to unload the code.
 */
typedef struct FsDivertLoad {
    ClientData clientData;
    Tcl_FSUnloadFileProc *unloadProcPtr;	
    Tcl_Obj *divertedFile;
} FsDivertLoad;

/* Now move on to the basic filesystem implementation */


static int 
FsCwdPointerEquals(objPtr)
    Tcl_Obj* objPtr;
{
    Tcl_MutexLock(&cwdMutex);
    if (cwdPathPtr == objPtr) {
	Tcl_MutexUnlock(&cwdMutex);
	return 1;
    } else {
	Tcl_MutexUnlock(&cwdMutex);
	return 0;
    }
}
        

static FilesystemRecord* 
FsGetIterator(void) {
    Tcl_MutexLock(&filesystemMutex);
    filesystemIteratorsInProgress++;
    Tcl_MutexUnlock(&filesystemMutex);
    /* Now we know the list of filesystems cannot be modified */
    return filesystemList;
}

static void 
FsReleaseIterator(void) {
    Tcl_MutexLock(&filesystemMutex);
    filesystemIteratorsInProgress--;
    if (filesystemIteratorsInProgress == 0) {
        /* Notify any waiting threads that things are ok now */
	if (filesystemWantToModify > 0) {
	    Tcl_ConditionNotify(&filesystemOkToModify);
	}
    }
    Tcl_MutexUnlock(&filesystemMutex);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FSRegister --
 *
 *    Insert the filesystem function table at the head of the list of
 *    functions which are used during calls to all file-system
 *    operations.  The filesystem will be added even if it is 
 *    already in the list.  (You can use TclFilesystemData to
 *    check if it is in the list, provided the ClientData used was
 *    not NULL).
 *    
 *    Note that the filesystem handling is head-to-tail of the list.
 *    Each filesystem is asked in turn whether it can handle a
 *    particular request, _until_ one of them says 'yes'. At that
 *    point no further filesystems are asked.
 *    
 *    In particular this means if you want to add a diagnostic
 *    filesystem (which simply reports all fs activity), it must be 
 *    at the head of the list: i.e. it must be the last registered.
 *
 * Results:
 *    Normally TCL_OK; TCL_ERROR if memory for a new node in the list
 *    could not be allocated.
 *
 * Side effects:
 *    Memory allocataed and modifies the link list for filesystems.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_FSRegister(clientData, fsPtr)
    ClientData clientData;    /* Client specific data for this fs */
    Tcl_Filesystem  *fsPtr;   /* The filesystem record for the new fs. */
{
    FilesystemRecord *newFilesystemPtr;

    if (fsPtr == NULL) {
	return TCL_ERROR;
    }

    newFilesystemPtr = (FilesystemRecord *)
	ckalloc(sizeof(FilesystemRecord));

    newFilesystemPtr->clientData = clientData;
    newFilesystemPtr->fsPtr = fsPtr;
    newFilesystemPtr->refCount = 0;
    
    /* 
     * Is this lock and wait strictly speaking necessary?  Since any
     * iterators out there will have grabbed a copy of the head of
     * the list and be iterating away from that, if we add a new
     * element to the head of the list, it can't possibly have any
     * effect on any of their loops.  In fact it could be better not
     * to wait, since we are adjusting the filesystem epoch, any
     * cached representations calculated by existing iterators are
     * going to have to be thrown away anyway.
     * 
     * However, since registering and unregistering filesystems is
     * a very rare action, this is not a very important point.
     */
    Tcl_MutexLock(&filesystemMutex);
    filesystemWantToModify++;
    Tcl_ConditionWait(&filesystemOkToModify, &filesystemMutex, NULL);
    filesystemWantToModify--;

    newFilesystemPtr->nextPtr = filesystemList;
    filesystemList = newFilesystemPtr;
    /* 
     * Increment the filesystem epoch counter, since existing paths
     * might conceivably now belong to different filesystems.
     */
    filesystemEpoch++;
    Tcl_MutexUnlock(&filesystemMutex);

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FSUnregister --
 *
 *    Remove the passed filesystem from the list of filesystem
 *    function tables.  It also ensures that the built-in
 *    (native) filesystem is not removable, although we may wish
 *    to change that decision in the future to allow a smaller
 *    Tcl core, in which the native filesystem is not used at
 *    all (we could, say, initialise Tcl completely over a network
 *    connection).
 *
 * Results:
 *    TCL_OK if the procedure pointer was successfully removed,
 *    TCL_ERROR otherwise.
 *
 * Side effects:
 *    Memory is deallocated and the respective list updated.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_FSUnregister(fsPtr)
    Tcl_Filesystem  *fsPtr;   /* The filesystem record to remove. */
{
    int retVal = TCL_ERROR;
    FilesystemRecord *tmpFsRecPtr;
    FilesystemRecord *prevFsRecPtr = NULL;

    Tcl_MutexLock(&filesystemMutex);
    filesystemWantToModify++;
    Tcl_ConditionWait(&filesystemOkToModify, &filesystemMutex, NULL);
    filesystemWantToModify--;
    tmpFsRecPtr = filesystemList;
    /*
     * Traverse the 'filesystemList' looking for the particular node
     * whose 'fsPtr' member matches 'fsPtr' and remove that one from
     * the list.  Ensure that the "default" node cannot be removed.
     */

    while ((retVal == TCL_ERROR) && (tmpFsRecPtr != &nativeFilesystemRecord)) {
	if (tmpFsRecPtr->fsPtr == fsPtr) {
	    if (prevFsRecPtr == NULL) {
		filesystemList = filesystemList->nextPtr;
	    } else {
		prevFsRecPtr->nextPtr = tmpFsRecPtr->nextPtr;
	    }
	    /* 
	     * Increment the filesystem epoch counter, since existing
	     * paths might conceivably now belong to different
	     * filesystems.  This should also ensure that paths which
	     * have cached the filesystem which is about to be deleted
	     * do not reference that filesystem (which would of course
	     * lead to memory exceptions).
	     */
	    filesystemEpoch++;

	    ckfree((char *)tmpFsRecPtr);

	    retVal = TCL_OK;
	} else {
	    prevFsRecPtr = tmpFsRecPtr;
	    tmpFsRecPtr = tmpFsRecPtr->nextPtr;
	}
    }

    Tcl_MutexUnlock(&filesystemMutex);
    return (retVal);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FSData --
 *
 *    Retrieve the clientData field for the filesystem given,
 *    or NULL if that filesystem is not registered.
 *
 * Results:
 *    A clientData value, or NULL.  Note that if the filesystem
 *    was registered with a NULL clientData field, this function
 *    will return that NULL value.
 *
 * Side effects:
 *    None.
 *
 *----------------------------------------------------------------------
 */

ClientData
Tcl_FSData(fsPtr)
    Tcl_Filesystem  *fsPtr;   /* The filesystem record to query. */
{
    ClientData retVal = NULL;
    FilesystemRecord *tmpFsRecPtr;

    tmpFsRecPtr = FsGetIterator();
    /*
     * Traverse the 'filesystemList' looking for the particular node
     * whose 'fsPtr' member matches 'fsPtr' and remove that one from
     * the list.  Ensure that the "default" node cannot be removed.
     */

    while ((retVal == NULL) && (tmpFsRecPtr != NULL)) {
	if (tmpFsRecPtr->fsPtr == fsPtr) {
	    retVal = tmpFsRecPtr->clientData;
	}
	tmpFsRecPtr = tmpFsRecPtr->nextPtr;
    }

    FsReleaseIterator();
    return (retVal);
}

/*
 *---------------------------------------------------------------------------
 *
 * FSNormalizeAbsolutePath --
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
 * Results:
 *	The result is returned in a Tcl_Obj with a refCount of 1,
 *	which is therefore owned by the caller.  It must be
 *	freed (with Tcl_DecrRefCount) by the caller when no longer needed.
 *
 * Side effects:
 *	None (beyond the memory allocation for the result).
 *
 * Special note:
 *	This code is based on code from Matt Newman and Jean-Claude
 *	Wippler, with additions from Vince Darley and is copyright 
 *	those respective authors.
 *
 *---------------------------------------------------------------------------
 */
static Tcl_Obj*
FSNormalizeAbsolutePath(interp, path)
    Tcl_Interp* interp;    /* Interpreter to use */
    char *path;            /* Absolute path to normalize (UTF-8) */
{
    char **sp = NULL, *np[BUFSIZ];
    int splen = 0, nplen, i;
    Tcl_Obj *retVal;
    
    Tcl_SplitPath(path, &splen, &sp);
    
    nplen = 0;
    for (i = 0;i < splen;i++) {
	if (strcmp(sp[i], ".") == 0)
	    continue;

	if (strcmp(sp[i], "..") == 0) {
	    if (nplen > 1) nplen--;
	} else {
	    np[nplen++] = sp[i];
	}
    }
    if (nplen > 0) {
	Tcl_DString dtemp;
	Tcl_DStringInit(&dtemp);
	Tcl_JoinPath(nplen, np, &dtemp);
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
	retVal = Tcl_NewStringObj(Tcl_DStringValue(&dtemp),-1);
	Tcl_DStringFree(&dtemp);
	Tcl_IncrRefCount(retVal);
	TclNormalizeToUniquePath(interp, retVal);
	/* 
	 * Since we know it is a normalized path, we can
	 * actually convert this object into an FsPath for
	 * greater efficiency 
	 */
	SetFsPathFromAbsoluteNormalized(interp, retVal);
    } else {
	/* Init to an empty string */
	retVal = Tcl_NewStringObj("",0);
	Tcl_IncrRefCount(retVal);
    }
    ckfree((char*) sp);

    /* This has a refCount of 1 for the caller */
    return retVal;
}

/*
 *---------------------------------------------------------------------------
 *
 * TclNormalizeToUniquePath --
 *
 * Description:
 *	Takes a path specification containing no ../, ./ sequences,
 *	and converts it into a unique path for the given platform.
 *      On MacOS, Unix, this means the path must be free of
 *	symbolic links/aliases, and on Windows it means we want the
 *	long form, with that long form's case-dependence (which gives
 *	us a unique, case-dependent path).
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
 *	This is only used by the above function.  Also if the
 *	filesystem-specific normalizePathProcs can re-introduce
 *	../, ./ sequences into the path, then this function will
 *	not return the correct result.  This may be possible with
 *	symbolic links on unix/macos.
 *
 *---------------------------------------------------------------------------
 */
static int
TclNormalizeToUniquePath(interp, pathPtr)
    Tcl_Interp *interp;
    Tcl_Obj *pathPtr;
{
    FilesystemRecord *fsRecPtr;
    int retVal = 0;

    /*
     * Call each of the "normalise path" functions in succession. This is
     * a special case, in which if we have a native filesystem handler,
     * we call it first.  This is because the root of Tcl's filesystem
     * is always a native filesystem (i.e. '/' on unix is native).
     */

    fsRecPtr = FsGetIterator();
    while (fsRecPtr != NULL) {
        if (fsRecPtr == &nativeFilesystemRecord) {
	    Tcl_FSNormalizePathProc *proc = fsRecPtr->fsPtr->normalizePathProc;
	    if (proc != NULL) {
		retVal = (*proc)(interp, pathPtr, retVal);
	    }
	    break;
        }
	fsRecPtr = fsRecPtr->nextPtr;
    }
    FsReleaseIterator();
    
    fsRecPtr = FsGetIterator();
    while (fsRecPtr != NULL) {
	/* Skip the native system next time through */
	if (fsRecPtr != &nativeFilesystemRecord) {
	    Tcl_FSNormalizePathProc *proc = fsRecPtr->fsPtr->normalizePathProc;
	    if (proc != NULL) {
		retVal = (*proc)(interp, pathPtr, retVal);
	    }
	    /* 
	     * We could add an efficiency check like this:
	     * 
	     *   if (retVal == Tcl_DStringLength(pathPtr)) {break;}
	     * 
	     * but there's not much benefit.
	     */
	}
	fsRecPtr = fsRecPtr->nextPtr;
    }
    FsReleaseIterator();

    return (retVal);
}

/*
 *---------------------------------------------------------------------------
 *
 * TclGetOpenMode --
 *
 * Description:
 *	Computes a POSIX mode mask for opening a file, from a given string,
 *	and also sets a flag to indicate whether the caller should seek to
 *	EOF after opening the file.
 *
 * Results:
 *	On success, returns mode to pass to "open". If an error occurs, the
 *	return value is -1 and if interp is not NULL, sets interp's result
 *	object to an error message.
 *
 * Side effects:
 *	Sets the integer referenced by seekFlagPtr to 1 to tell the caller
 *	to seek to EOF after opening the file.
 *
 * Special note:
 *	This code is based on a prototype implementation contributed
 *	by Mark Diekhans.
 *
 *---------------------------------------------------------------------------
 */

int
TclGetOpenMode(interp, string, seekFlagPtr)
    Tcl_Interp *interp;			/* Interpreter to use for error
					 * reporting - may be NULL. */
    char *string;			/* Mode string, e.g. "r+" or
					 * "RDONLY CREAT". */
    int *seekFlagPtr;			/* Set this to 1 if the caller
                                         * should seek to EOF during the
                                         * opening of the file. */
{
    int mode, modeArgc, c, i, gotRW;
    char **modeArgv, *flag;
#define RW_MODES (O_RDONLY|O_WRONLY|O_RDWR)

    /*
     * Check for the simpler fopen-like access modes (e.g. "r").  They
     * are distinguished from the POSIX access modes by the presence
     * of a lower-case first letter.
     */

    *seekFlagPtr = 0;
    mode = 0;

    /*
     * Guard against international characters before using byte oriented
     * routines.
     */

    if (!(string[0] & 0x80)
	    && islower(UCHAR(string[0]))) { /* INTL: ISO only. */
	switch (string[0]) {
	    case 'r':
		mode = O_RDONLY;
		break;
	    case 'w':
		mode = O_WRONLY|O_CREAT|O_TRUNC;
		break;
	    case 'a':
		mode = O_WRONLY|O_CREAT;
                *seekFlagPtr = 1;
		break;
	    default:
		error:
                if (interp != (Tcl_Interp *) NULL) {
                    Tcl_AppendResult(interp,
                            "illegal access mode \"", string, "\"",
                            (char *) NULL);
                }
		return -1;
	}
	if (string[1] == '+') {
	    mode &= ~(O_RDONLY|O_WRONLY);
	    mode |= O_RDWR;
	    if (string[2] != 0) {
		goto error;
	    }
	} else if (string[1] != 0) {
	    goto error;
	}
        return mode;
    }

    /*
     * The access modes are specified using a list of POSIX modes
     * such as O_CREAT.
     *
     * IMPORTANT NOTE: We rely on Tcl_SplitList working correctly when
     * a NULL interpreter is passed in.
     */

    if (Tcl_SplitList(interp, string, &modeArgc, &modeArgv) != TCL_OK) {
        if (interp != (Tcl_Interp *) NULL) {
            Tcl_AddErrorInfo(interp,
                    "\n    while processing open access modes \"");
            Tcl_AddErrorInfo(interp, string);
            Tcl_AddErrorInfo(interp, "\"");
        }
        return -1;
    }
    
    gotRW = 0;
    for (i = 0; i < modeArgc; i++) {
	flag = modeArgv[i];
	c = flag[0];
	if ((c == 'R') && (strcmp(flag, "RDONLY") == 0)) {
	    mode = (mode & ~RW_MODES) | O_RDONLY;
	    gotRW = 1;
	} else if ((c == 'W') && (strcmp(flag, "WRONLY") == 0)) {
	    mode = (mode & ~RW_MODES) | O_WRONLY;
	    gotRW = 1;
	} else if ((c == 'R') && (strcmp(flag, "RDWR") == 0)) {
	    mode = (mode & ~RW_MODES) | O_RDWR;
	    gotRW = 1;
	} else if ((c == 'A') && (strcmp(flag, "APPEND") == 0)) {
	    mode |= O_APPEND;
            *seekFlagPtr = 1;
	} else if ((c == 'C') && (strcmp(flag, "CREAT") == 0)) {
	    mode |= O_CREAT;
	} else if ((c == 'E') && (strcmp(flag, "EXCL") == 0)) {
	    mode |= O_EXCL;
	} else if ((c == 'N') && (strcmp(flag, "NOCTTY") == 0)) {
#ifdef O_NOCTTY
	    mode |= O_NOCTTY;
#else
	    if (interp != (Tcl_Interp *) NULL) {
                Tcl_AppendResult(interp, "access mode \"", flag,
                        "\" not supported by this system", (char *) NULL);
            }
            ckfree((char *) modeArgv);
	    return -1;
#endif
	} else if ((c == 'N') && (strcmp(flag, "NONBLOCK") == 0)) {
#if defined(O_NDELAY) || defined(O_NONBLOCK)
#   ifdef O_NONBLOCK
	    mode |= O_NONBLOCK;
#   else
	    mode |= O_NDELAY;
#   endif
#else
            if (interp != (Tcl_Interp *) NULL) {
                Tcl_AppendResult(interp, "access mode \"", flag,
                        "\" not supported by this system", (char *) NULL);
            }
            ckfree((char *) modeArgv);
	    return -1;
#endif
	} else if ((c == 'T') && (strcmp(flag, "TRUNC") == 0)) {
	    mode |= O_TRUNC;
	} else {
            if (interp != (Tcl_Interp *) NULL) {
                Tcl_AppendResult(interp, "invalid access mode \"", flag,
                        "\": must be RDONLY, WRONLY, RDWR, APPEND, CREAT",
                        " EXCL, NOCTTY, NONBLOCK, or TRUNC", (char *) NULL);
            }
	    ckfree((char *) modeArgv);
	    return -1;
	}
    }
    ckfree((char *) modeArgv);
    if (!gotRW) {
        if (interp != (Tcl_Interp *) NULL) {
            Tcl_AppendResult(interp, "access mode must include either",
                    " RDONLY, WRONLY, or RDWR", (char *) NULL);
        }
	return -1;
    }
    return mode;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FSEvalFile --
 *
 *	Read in a file and process the entire file as one gigantic
 *	Tcl command.
 *
 * Results:
 *	A standard Tcl result, which is either the result of executing
 *	the file or an error indicating why the file couldn't be read.
 *
 * Side effects:
 *	Depends on the commands in the file.  During the evaluation
 *	of the contents of the file, iPtr->scriptFile is made to
 *	point to fileName (the old value is cached and replaced when
 *	this function returns).
 *
 *----------------------------------------------------------------------
 */

int
Tcl_FSEvalFile(interp, fileName)
    Tcl_Interp *interp;		/* Interpreter in which to process file. */
    Tcl_Obj *fileName;		/* Name of file to process.  Tilde-substitution
				 * will be performed on this name. */
{
    int result, length;
    struct stat statBuf;
    Tcl_Obj *oldScriptFile;
    Interp *iPtr;
    char *string;
    Tcl_Channel chan;
    Tcl_Obj *objPtr;

    if (Tcl_FSGetTranslatedPath(interp, fileName) == NULL) {
	return TCL_ERROR;
    }

    result = TCL_ERROR;
    objPtr = Tcl_NewObj();

    if (Tcl_FSStat(fileName, &statBuf) == -1) {
        Tcl_SetErrno(errno);
	Tcl_AppendResult(interp, "couldn't read file \"", 
		Tcl_GetString(fileName),
		"\": ", Tcl_PosixError(interp), (char *) NULL);
	goto end;
    }
    chan = Tcl_FSOpenFileChannel(interp, fileName, "r", 0644);
    if (chan == (Tcl_Channel) NULL) {
        Tcl_ResetResult(interp);
	Tcl_AppendResult(interp, "couldn't read file \"", 
		Tcl_GetString(fileName),
		"\": ", Tcl_PosixError(interp), (char *) NULL);
	goto end;
    }
    /*
     * The eofchar is \32 (^Z).  This is the usual on Windows, but we
     * effect this cross-platform to allow for scripted documents.
     * [Bug: 2040]
     */
    Tcl_SetChannelOption(interp, chan, "-eofchar", "\32");
    if (Tcl_ReadChars(chan, objPtr, -1, 0) < 0) {
        Tcl_Close(interp, chan);
	Tcl_AppendResult(interp, "couldn't read file \"", 
		Tcl_GetString(fileName),
		"\": ", Tcl_PosixError(interp), (char *) NULL);
	goto end;
    }
    if (Tcl_Close(interp, chan) != TCL_OK) {
        goto end;
    }

    iPtr = (Interp *) interp;
    oldScriptFile = iPtr->scriptFile;
    iPtr->scriptFile = fileName;
    Tcl_IncrRefCount(iPtr->scriptFile);
    string = Tcl_GetStringFromObj(objPtr, &length);
    result = Tcl_EvalEx(interp, string, length, 0);
    /* 
     * Now we have to be careful; the script may have changed the
     * iPtr->scriptFile value, so we must reset it without
     * assuming it still points to 'fileName'.
     */
    if (iPtr->scriptFile != NULL) {
	Tcl_DecrRefCount(iPtr->scriptFile);
    }
    iPtr->scriptFile = oldScriptFile;

    if (result == TCL_RETURN) {
	result = TclUpdateReturnInfo(iPtr);
    } else if (result == TCL_ERROR) {
	char msg[200 + TCL_INTEGER_SPACE];

	/*
	 * Record information telling where the error occurred.
	 */

	sprintf(msg, "\n    (file \"%.150s\" line %d)", Tcl_GetString(fileName),
		interp->errorLine);
	Tcl_AddErrorInfo(interp, msg);
    }

    end:
    Tcl_DecrRefCount(objPtr);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetErrno --
 *
 *	Gets the current value of the Tcl error code variable. This is
 *	currently the global variable "errno" but could in the future
 *	change to something else.
 *
 * Results:
 *	The value of the Tcl error code variable.
 *
 * Side effects:
 *	None. Note that the value of the Tcl error code variable is
 *	UNDEFINED if a call to Tcl_SetErrno did not precede this call.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_GetErrno()
{
    return errno;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetErrno --
 *
 *	Sets the Tcl error code variable to the supplied value.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Modifies the value of the Tcl error code variable.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_SetErrno(err)
    int err;			/* The new value. */
{
    errno = err;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_PosixError --
 *
 *	This procedure is typically called after UNIX kernel calls
 *	return errors.  It stores machine-readable information about
 *	the error in $errorCode returns an information string for
 *	the caller's use.
 *
 * Results:
 *	The return value is a human-readable string describing the
 *	error.
 *
 * Side effects:
 *	The global variable $errorCode is reset.
 *
 *----------------------------------------------------------------------
 */

char *
Tcl_PosixError(interp)
    Tcl_Interp *interp;		/* Interpreter whose $errorCode variable
				 * is to be changed. */
{
    char *id, *msg;

    msg = Tcl_ErrnoMsg(errno);
    id = Tcl_ErrnoId();
    Tcl_SetErrorCode(interp, "POSIX", id, msg, (char *) NULL);
    return msg;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FSStat --
 *
 *	This procedure replaces the library version of stat and lsat.
 *	
 *	The appropriate function for the filesystem to which pathPtr
 *	belongs will be called.
 *
 * Results:
 *      See stat documentation.
 *
 * Side effects:
 *      See stat documentation.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_FSStat(pathPtr, buf)
    Tcl_Obj *pathPtr;		/* Path of file to stat (in current CP). */
    struct stat *buf;		/* Filled with results of stat call. */
{
#ifdef USE_OBSOLETE_FS_HOOKS
    StatProc *statProcPtr;
    int retVal = -1;
#endif /* USE_OBSOLETE_FS_HOOKS */
    Tcl_Filesystem *fsPtr;
    char *path = Tcl_FSGetTranslatedPath(NULL, pathPtr);

    /*
     * Call each of the "stat" function in succession.  A non-return
     * value of -1 indicates the particular function has succeeded.
     */

#ifdef USE_OBSOLETE_FS_HOOKS
    Tcl_MutexLock(&obsoleteFsHookMutex);
    statProcPtr = statProcList;
    while ((retVal == -1) && (statProcPtr != NULL)) {
	retVal = (*statProcPtr->proc)(path, buf);
	statProcPtr = statProcPtr->nextPtr;
    }
    Tcl_MutexUnlock(&obsoleteFsHookMutex);
    if (retVal != -1) {
        return retVal;
    }
#endif /* USE_OBSOLETE_FS_HOOKS */
    fsPtr = Tcl_FSGetFileSystemForPath(pathPtr);
    if (fsPtr != NULL) {
	Tcl_FSStatProc *proc = fsPtr->statProc;
	if (proc != NULL) {
	    return (*proc)(pathPtr, buf);
	}
    }
    Tcl_SetErrno(ENOENT);
    return -1;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FSLstat --
 *
 *	This procedure replaces the library version of lstat.
 *	The appropriate function for the filesystem to which pathPtr
 *	belongs will be called.  If no 'lstat' function is listed,
 *	but a 'stat' function is, then Tcl will fall back on the
 *	stat function.
 *
 * Results:
 *      See lstat documentation.
 *
 * Side effects:
 *      See lstat documentation.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_FSLstat(pathPtr, buf)
    Tcl_Obj *pathPtr;		/* Path of file to stat (in current CP). */
    struct stat *buf;		/* Filled with results of stat call. */
{
    Tcl_Filesystem *fsPtr = Tcl_FSGetFileSystemForPath(pathPtr);
    if (fsPtr != NULL) {
	Tcl_FSLstatProc *proc = fsPtr->lstatProc;
	if (proc != NULL) {
	    return (*proc)(pathPtr, buf);
	} else {
	    Tcl_FSStatProc *sproc = fsPtr->statProc;
	    if (sproc != NULL) {
		return (*sproc)(pathPtr, buf);
	    }
	}
    }
    Tcl_SetErrno(ENOENT);
    return -1;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FSAccess --
 *
 *	This procedure replaces the library version of access.
 *	The appropriate function for the filesystem to which pathPtr
 *	belongs will be called.
 *
 * Results:
 *      See access documentation.
 *
 * Side effects:
 *      See access documentation.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_FSAccess(pathPtr, mode)
    Tcl_Obj *pathPtr;		/* Path of file to access (in current CP). */
    int mode;                   /* Permission setting. */
{
#ifdef USE_OBSOLETE_FS_HOOKS
    AccessProc *accessProcPtr;
    int retVal = -1;
#endif /* USE_OBSOLETE_FS_HOOKS */
    Tcl_Filesystem *fsPtr;
    char *path = Tcl_FSGetTranslatedPath(NULL, pathPtr);

    /*
     * Call each of the "access" function in succession.  A non-return
     * value of -1 indicates the particular function has succeeded.
     */

#ifdef USE_OBSOLETE_FS_HOOKS
    Tcl_MutexLock(&obsoleteFsHookMutex);
    accessProcPtr = accessProcList;
    while ((retVal == -1) && (accessProcPtr != NULL)) {
	retVal = (*accessProcPtr->proc)(path, mode);
	accessProcPtr = accessProcPtr->nextPtr;
    }
    Tcl_MutexUnlock(&obsoleteFsHookMutex);
    if (retVal != -1) {
	return retVal;
    }
#endif /* USE_OBSOLETE_FS_HOOKS */
    fsPtr = Tcl_FSGetFileSystemForPath(pathPtr);
    if (fsPtr != NULL) {
	Tcl_FSAccessProc *proc = fsPtr->accessProc;
	if (proc != NULL) {
	    return (*proc)(pathPtr, mode);
	}
    }

    Tcl_SetErrno(ENOENT);
    return -1;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FSOpenFileChannel --
 *
 *	The appropriate function for the filesystem to which pathPtr
 *	belongs will be called.
 *
 * Results:
 *	The new channel or NULL, if the named file could not be opened.
 *
 * Side effects:
 *	May open the channel and may cause creation of a file on the
 *	file system.
 *
 *----------------------------------------------------------------------
 */
 
Tcl_Channel
Tcl_FSOpenFileChannel(interp, pathPtr, modeString, permissions)
    Tcl_Interp *interp;                 /* Interpreter for error reporting;
                                         * can be NULL. */
    Tcl_Obj *pathPtr;                   /* Name of file to open. */
    char *modeString;                   /* A list of POSIX open modes or
                                         * a string such as "rw". */
    int permissions;                    /* If the open involves creating a
                                         * file, with what modes to create
                                         * it? */
{
#ifdef USE_OBSOLETE_FS_HOOKS
    OpenFileChannelProc *openFileChannelProcPtr;
    Tcl_Channel retVal = NULL;
#endif /* USE_OBSOLETE_FS_HOOKS */
    Tcl_Filesystem *fsPtr;
    char *path = Tcl_FSGetTranslatedPath(interp, pathPtr);
    if (path == NULL) {
	return NULL;
    }

    /*
     * Call each of the "Tcl_OpenFileChannel" function in succession.
     * A non-NULL return value indicates the particular function has
     * succeeded.
     */

#ifdef USE_OBSOLETE_FS_HOOKS
    Tcl_MutexLock(&obsoleteFsHookMutex);
    openFileChannelProcPtr = openFileChannelProcList;
    while ((retVal == NULL) && (openFileChannelProcPtr != NULL)) {
	retVal = (*openFileChannelProcPtr->proc)(interp, path,
		modeString, permissions);
	openFileChannelProcPtr = openFileChannelProcPtr->nextPtr;
    }
    Tcl_MutexUnlock(&obsoleteFsHookMutex);
    if (retVal != NULL) {
	return retVal;
    }
#endif /* USE_OBSOLETE_FS_HOOKS */
    fsPtr = Tcl_FSGetFileSystemForPath(pathPtr);
    if (fsPtr != NULL) {
	Tcl_FSOpenFileChannelProc *proc = fsPtr->openFileChannelProc;
	if (proc != NULL) {
	    return (*proc)(interp, pathPtr, modeString, permissions);
	}
    }
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FSMatchInDirectory --
 *
 *	This routine is used by the globbing code to search a directory
 *	for all files which match a given pattern.  The appropriate
 *	function for the filesystem to which pathPtr belongs will be
 *	called.  If pathPtr does not belong to any filesystem and if it
 *	is NULL or the empty string, then we assume the pattern is to
 *	be matched in the current working directory.  To avoid each
 *	filesystem's Tcl_FSMatchInDirectoryProc having to deal with
 *	this issue, we create a pathPtr on the fly, and then remove it
 *	from the results returned.  This makes filesystems easy to
 *	write, since they can assume the pathPtr passed to them
 *	is an ordinary path.  In fact this means we could remove such
 *	special case handling from Tcl's native filesystems.
 *
 * Results: 
 *	
 *	The return value is a standard Tcl result indicating whether an
 *	error occurred in globbing.  Error messages are placed in
 *	interp, but good results are placed in the resultPtr given.
 *	
 *	Recursive searches, e.g.
 *	
 *	   glob -dir $dir -join * pkgIndex.tcl
 *	   
 *	which must recurse through each directory matching '*' are
 *	handled internally by Tcl, by passing specific flags in a 
 *	modified 'types' parameter.
 *
 * Side effects:
 *	The interpreter may have an error message inserted into it.
 *
 *---------------------------------------------------------------------- 
 */

int
Tcl_FSMatchInDirectory(interp, result, pathPtr, pattern, types)
    Tcl_Interp *interp;		/* Interpreter to receive error messages. */
    Tcl_Obj *result;		/* List object to receive results. */
    Tcl_Obj *pathPtr;	        /* Contains path to directory to search. */
    char *pattern;		/* Pattern to match against. */
    Tcl_GlobTypeData *types;	/* Object containing list of acceptable types.
				 * May be NULL. In particular the directory
				 * flag is very important. */
{
    Tcl_Filesystem *fsPtr = Tcl_FSGetFileSystemForPath(pathPtr);
    if (fsPtr != NULL) {
	Tcl_FSMatchInDirectoryProc *proc = fsPtr->matchInDirectoryProc;
	if (proc != NULL) {
	    return (*proc)(interp, result, pathPtr, pattern, types);
	}
    } else {
	Tcl_Obj* cwd;
	int ret = -1;
	if (pathPtr != NULL) {
	    int len;
	    Tcl_GetStringFromObj(pathPtr,&len);
	    if (len != 0) {
		/* 
		 * We have no idea how to match files in a directory
		 * which belongs to no known filesystem
		 */
		return -1;
	    }
	}
	/* 
	 * We have a null string, this means we must use the 'cwd', and
	 * then manipulate the result.  We must deal with this here,
	 * since if we don't, every single filesystem's implementation
	 * of Tcl_FSMatchInDirectory will have to deal with it for us.
	 */
	cwd = Tcl_FSGetCwd(NULL);
	if (cwd == NULL) {
	    if (interp != NULL) {
	        Tcl_SetResult(interp, "glob couldn't determine"
			  "the current working directory", TCL_STATIC);
	    }
	    return TCL_ERROR;
	}
	fsPtr = Tcl_FSGetFileSystemForPath(cwd);
	if (fsPtr != NULL) {
	    Tcl_FSMatchInDirectoryProc *proc = fsPtr->matchInDirectoryProc;
	    if (proc != NULL) {
		int cwdLen;
		Tcl_Obj *cwdDir;
		Tcl_Obj* tmpResultPtr = Tcl_NewListObj(0, NULL);
		/* 
		 * We know the cwd is a normalised object which does
		 * not end in a directory delimiter.
		 */
		cwdDir = Tcl_DuplicateObj(cwd);
#ifdef MAC_TCL
		Tcl_AppendToObj(cwdDir, ":", 1);
#else
		Tcl_AppendToObj(cwdDir, "/", 1);
#endif
		Tcl_GetStringFromObj(cwdDir, &cwdLen);
		Tcl_IncrRefCount(cwdDir);
		ret = (*proc)(interp, tmpResultPtr, cwdDir, pattern, types);
		Tcl_DecrRefCount(cwdDir);
		if (ret == TCL_OK) {
		    int resLength;

		    ret = Tcl_ListObjLength(interp, tmpResultPtr, &resLength);
		    if (ret == TCL_OK) {
			Tcl_Obj *elt, *cutElt;
			char *eltStr;
			int eltLen, i;

			for (i = 0; i < resLength; i++) {
			    Tcl_ListObjIndex(interp, tmpResultPtr, i, &elt);
			    eltStr = Tcl_GetStringFromObj(elt,&eltLen);
			    cutElt = Tcl_NewStringObj(eltStr + cwdLen,
				    eltLen - cwdLen);
			    Tcl_ListObjAppendElement(interp, result, cutElt);
			}
		    }
		}
		Tcl_DecrRefCount(tmpResultPtr);
	    }
	}
	Tcl_DecrRefCount(cwd);
	return ret;
    }
    return -1;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FSGetCwd --
 *
 *	This function replaces the library version of getcwd().
 *	
 *	Most VFS's will *not* implement a 'cwdProc'.  Tcl now maintains
 *	its own record (in a Tcl_Obj) of the cwd, and an attempt
 *	is made to synchronise this with the cwd's containing filesystem,
 *	if that filesystem provides a cwdProc (e.g. the native filesystem).
 *	
 *	Note that if Tcl's cwd is not in the native filesystem, then of
 *	course Tcl's cwd and the native cwd are different: extensions
 *	should therefore ensure they only access the cwd through this
 *	function to avoid confusion.
 *	
 *	If a global cwdPathPtr already exists, it is returned, subject
 *	to a synchronisation attempt in that cwdPathPtr's fs.
 *	Otherwise, the chain of functions that have been "inserted"
 *	into the filesystem will be called in succession until either a
 *	value other than NULL is returned, or the entire list is
 *	visited.
 *
 * Results:
 *	The result is a pointer to a Tcl_Obj specifying the current
 *	directory, or NULL if the current directory could not be
 *	determined.  If NULL is returned, an error message is left in the
 *	interp's result.  
 *	
 *	The result already has its refCount incremented for the caller.
 *	When it is no longer needed, that refCount should be decremented.
 *	This is needed for thread-safety purposes, to allow multiple
 *	threads to access this and related functions, while ensuring the
 *	results are always valid.
 *	
 *	Of course it is probably a bad idea for multiple threads to
 *	be *setting* the cwd anyway, but we can at least try to 
 *	help the case of multiple reads with occasional sets.
 *
 * Side effects:
 *	Various objects may be freed and allocated.
 *
 *----------------------------------------------------------------------
 */

Tcl_Obj*
Tcl_FSGetCwd(interp)
    Tcl_Interp *interp;
{
    Tcl_Obj *cwdToReturn;
    
    if (FsCwdPointerEquals(NULL)) {
	FilesystemRecord *fsRecPtr;
	Tcl_Obj *retVal = NULL;

        /* 
         * We've never been called before, try to find a cwd.  Call
         * each of the "Tcl_GetCwd" function in succession.  A non-NULL
         * return value indicates the particular function has
         * succeeded.
	 */

	fsRecPtr = FsGetIterator();
	while ((retVal == NULL) && (fsRecPtr != NULL)) {
	    Tcl_FSGetCwdProc *proc = fsRecPtr->fsPtr->getCwdProc;
	    if (proc != NULL) {
		retVal = (*proc)(interp);
	    }
	    fsRecPtr = fsRecPtr->nextPtr;
	}
	FsReleaseIterator();
	/* 
	 * Now the 'cwd' may NOT be normalized, at least on some
	 * platforms.  For the sake of efficiency, we want a completely
	 * normalized cwd at all times.
	 * 
	 * Finally, if retVal is NULL, we do not have a cwd, which
	 * could be problematic.
	 */
	if (retVal != NULL) {
	    Tcl_Obj *norm = FSNormalizeAbsolutePath(interp, 
				Tcl_GetString(retVal));
	    if (norm != NULL) {
		/* 
		 * We found a cwd, which is now in our global storage.
		 * We must make a copy.  Norm already has a refCount of
		 * 1.
		 * 
		 * Threading issue: note that multiple threads at system
		 * startup could in principle call this procedure 
		 * simultaneously.  They will therefore each set the
		 * cwdPathPtr independently.  That behaviour is a bit
		 * peculiar, but should be fine.  Once we have a cwd,
		 * we'll always be in the 'else' branch below which
		 * is simpler.
		 */
		Tcl_MutexLock(&cwdMutex);
		/* Just in case the pointer has been set by another
		 * thread between now and the test above */
		if (cwdPathPtr != NULL) {
		    Tcl_DecrRefCount(cwdPathPtr);
		}
		cwdPathPtr = norm;
		Tcl_MutexUnlock(&cwdMutex);
	    }
	    Tcl_DecrRefCount(retVal);
	}
    } else {
	/* 
	 * We already have a cwd cached, but we want to give the
	 * filesystem it is in a chance to check whether that cwd
	 * has changed, or is perhaps no longer accessible.  This
	 * allows an error to be thrown if, say, the permissions on
	 * that directory have changed.
	 */
	Tcl_Filesystem *fsPtr = Tcl_FSGetFileSystemForPath(cwdPathPtr);
	/* 
	 * If the filesystem couldn't be found, or if no cwd function
	 * exists for this filesystem, then we simply assume the cached
	 * cwd is ok.  If we do call a cwd, we must watch for errors
	 * (if the cwd returns NULL).  This ensures that, say, on Unix
	 * if the permissions of the cwd change, 'pwd' does actually
	 * throw the correct error in Tcl.  (This is tested for in the
	 * test suite on unix).
	 */
	if (fsPtr != NULL) {
	    Tcl_FSGetCwdProc *proc = fsPtr->getCwdProc;
	    if (proc != NULL) {
		Tcl_Obj *retVal = (*proc)(interp);
		if (retVal != NULL) {
		    Tcl_Obj *norm = FSNormalizeAbsolutePath(interp, 
					Tcl_GetString(retVal));
		    /* 
		     * Check whether cwd has changed from the value
		     * previously stored in cwdPathPtr.  Really 'norm'
		     * shouldn't be null, but we are careful.
		     */
		    if (norm == NULL) {
			/* Do nothing */
		    } else if (Tcl_FSEqualPaths(cwdPathPtr, norm)) {
		        /* 
		         * If the paths were equal, we can be more
		         * efficient and retain the old path object
		         * which will probably already be shared.  In
		         * this case we can simply free the normalized
		         * path we just calculated.
		         */
		        Tcl_DecrRefCount(norm);
		    } else {
			/* The cwd has in fact changed, so we must
			 * lock down the cwdMutex to modify. */
			Tcl_MutexLock(&cwdMutex);
			Tcl_DecrRefCount(cwdPathPtr);
			cwdPathPtr = norm;
			Tcl_MutexUnlock(&cwdMutex);
		    }
		    Tcl_DecrRefCount(retVal);
		} else {
		    /* The 'cwd' function returned an error, so we
		     * reset the cwd after locking down the mutex. */
		    Tcl_MutexLock(&cwdMutex);
		    Tcl_DecrRefCount(cwdPathPtr);
		    cwdPathPtr = NULL;
		    Tcl_MutexUnlock(&cwdMutex);
		}
	    }
	}
    }
    
    /* 
     * The paths all eventually fall through to here.  Note that
     * we use a bunch of separate mutex locks throughout this
     * code to help prevent deadlocks between threads.  Really
     * the only weirdness will arise if multiple threads are setting
     * and reading the cwd, and that behaviour is always going to be
     * a little suspect.
     */
    Tcl_MutexLock(&cwdMutex);
    cwdToReturn = cwdPathPtr;
    if (cwdToReturn != NULL) {
        Tcl_IncrRefCount(cwdToReturn);
    }
    Tcl_MutexUnlock(&cwdMutex);
    
    return (cwdToReturn);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FSUtime --
 *
 *	This procedure replaces the library version of utime.
 *	The appropriate function for the filesystem to which pathPtr
 *	belongs will be called.
 *
 * Results:
 *      See utime documentation.
 *
 * Side effects:
 *      See utime documentation.
 *
 *----------------------------------------------------------------------
 */

int 
Tcl_FSUtime (pathPtr, tval)
    Tcl_Obj *pathPtr;       /* File to change access/modification times */
    struct utimbuf *tval;   /* Structure containing access/modification 
                             * times to use.  Should not be modified. */
{
    Tcl_Filesystem *fsPtr = Tcl_FSGetFileSystemForPath(pathPtr);
    if (fsPtr != NULL) {
	Tcl_FSUtimeProc *proc = fsPtr->utimeProc;
	if (proc != NULL) {
	    return (*proc)(pathPtr, tval);
	}
    }
    return -1;
}

/*
 *----------------------------------------------------------------------
 *
 * NativeFileAttrStrings --
 *
 *	This procedure implements the platform dependent 'file
 *	attributes' subcommand, for the native filesystem, for listing
 *	the set of possible attribute strings.  This function is part
 *	of Tcl's native filesystem support, and is placed here because
 *	it is shared by Unix, MacOS and Windows code.
 *
 * Results:
 *      An array of strings
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char**
NativeFileAttrStrings(pathPtr, objPtrRef)
    Tcl_Obj *pathPtr;
    Tcl_Obj** objPtrRef;
{
    return tclpFileAttrStrings;
}

/*
 *----------------------------------------------------------------------
 *
 * NativeFileAttrsGet --
 *
 *	This procedure implements the platform dependent
 *	'file attributes' subcommand, for the native
 *	filesystem, for 'get' operations.  This function is part
 *	of Tcl's native filesystem support, and is placed here
 *	because it is shared by Unix, MacOS and Windows code.
 *
 * Results:
 *      Standard Tcl return code.  The object placed in objPtrRef
 *      (if TCL_OK was returned) is likely to have a refCount of zero.
 *      Either way we must either store it somewhere (e.g. the Tcl 
 *      result), or Incr/Decr its refCount to ensure it is properly
 *      freed.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NativeFileAttrsGet(interp, index, fileName, objPtrRef)
    Tcl_Interp *interp;		/* The interpreter for error reporting. */
    int index;			/* index of the attribute command. */
    Tcl_Obj *fileName;		/* filename we are operating on. */
    Tcl_Obj **objPtrRef;	/* for output. */
{
    return (*tclpFileAttrProcs[index].getProc)(interp, index, 
		    Tcl_FSGetTranslatedPath(NULL, fileName),
		    objPtrRef);
}

/*
 *----------------------------------------------------------------------
 *
 * NativeFileAttrsSet --
 *
 *	This procedure implements the platform dependent
 *	'file attributes' subcommand, for the native
 *	filesystem, for 'set' operations. This function is part
 *	of Tcl's native filesystem support, and is placed here
 *	because it is shared by Unix, MacOS and Windows code.
 *
 * Results:
 *      Standard Tcl return code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
NativeFileAttrsSet(interp, index, fileName, objPtr)
    Tcl_Interp *interp;		/* The interpreter for error reporting. */
    int index;			/* index of the attribute command. */
    Tcl_Obj *fileName;		/* filename we are operating on. */
    Tcl_Obj *objPtr;		/* set to this value. */
{
    return (*tclpFileAttrProcs[index].setProc)(interp, index,
		    Tcl_FSGetTranslatedPath(NULL, fileName),
		    objPtr);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FSFileAttrStrings --
 *
 *	This procedure implements part of the hookable 'file
 *	attributes' subcommand.  The appropriate function for the
 *	filesystem to which pathPtr belongs will be called.
 *
 * Results:
 *      The called procedure may either return an array of strings,
 *      or may instead return NULL and place a Tcl list into the 
 *      given objPtrRef.  Tcl will take that list and first increment
 *      its refCount before using it.  On completion of that use, Tcl
 *      will decrement its refCount.  Hence if the list should be
 *      disposed of by Tcl when done, it should have a refCount of zero,
 *      and if the list should not be disposed of, the filesystem
 *      should ensure it retains a refCount on the object.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

char**
Tcl_FSFileAttrStrings(pathPtr, objPtrRef)
    Tcl_Obj* pathPtr;
    Tcl_Obj** objPtrRef;
{
    Tcl_Filesystem *fsPtr = Tcl_FSGetFileSystemForPath(pathPtr);
    if (fsPtr != NULL) {
	Tcl_FSFileAttrStringsProc *proc = fsPtr->fileAttrStringsProc;
	if (proc != NULL) {
	    return (*proc)(pathPtr, objPtrRef);
	}
    }
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FSFileAttrsGet --
 *
 *	This procedure implements read access for the hookable 'file
 *	attributes' subcommand.  The appropriate function for the
 *	filesystem to which pathPtr belongs will be called.
 *
 * Results:
 *      Standard Tcl return code.  The object placed in objPtrRef
 *      (if TCL_OK was returned) is likely to have a refCount of zero.
 *      Either way we must either store it somewhere (e.g. the Tcl 
 *      result), or Incr/Decr its refCount to ensure it is properly
 *      freed.

 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_FSFileAttrsGet(interp, index, pathPtr, objPtrRef)
    Tcl_Interp *interp;		/* The interpreter for error reporting. */
    int index;			/* index of the attribute command. */
    Tcl_Obj *pathPtr;		/* filename we are operating on. */
    Tcl_Obj **objPtrRef;	/* for output. */
{
    Tcl_Filesystem *fsPtr = Tcl_FSGetFileSystemForPath(pathPtr);
    if (fsPtr != NULL) {
	Tcl_FSFileAttrsGetProc *proc = fsPtr->fileAttrsGetProc;
	if (proc != NULL) {
	    return (*proc)(interp, index, pathPtr, objPtrRef);
	}
    }
    return -1;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FSFileAttrsSet --
 *
 *	This procedure implements write access for the hookable 'file
 *	attributes' subcommand.  The appropriate function for the
 *	filesystem to which pathPtr belongs will be called.
 *
 * Results:
 *      Standard Tcl return code.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_FSFileAttrsSet(interp, index, pathPtr, objPtr)
    Tcl_Interp *interp;		/* The interpreter for error reporting. */
    int index;			/* index of the attribute command. */
    Tcl_Obj *pathPtr;		/* filename we are operating on. */
    Tcl_Obj *objPtr;		/* Input value. */
{
    Tcl_Filesystem *fsPtr = Tcl_FSGetFileSystemForPath(pathPtr);
    if (fsPtr != NULL) {
	Tcl_FSFileAttrsSetProc *proc = fsPtr->fileAttrsSetProc;
	if (proc != NULL) {
	    return (*proc)(interp, index, pathPtr, objPtr);
	}
    }
    return -1;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FSChdir --
 *
 *	This function replaces the library version of chdir().
 *	
 *	The path is normalized and then passed to the filesystem
 *	which claims it.
 *
 * Results:
 *	See chdir() documentation.  If successful, we keep a 
 *	record of the successful path in cwdPathPtr for subsequent 
 *	calls to getcwd.
 *
 * Side effects:
 *	See chdir() documentation.  The global cwdPathPtr may 
 *	change value.
 *
 *----------------------------------------------------------------------
 */
int
Tcl_FSChdir(pathPtr)
    Tcl_Obj *pathPtr;
{
    Tcl_Filesystem *fsPtr;
    int retVal = -1;
    Tcl_Obj *normDirName;
    
    normDirName = Tcl_FSGetNormalizedPath(NULL, pathPtr);
    if (normDirName == NULL) {
        return TCL_ERROR;
    }
    
    fsPtr = Tcl_FSGetFileSystemForPath(pathPtr);
    if (fsPtr != NULL) {
	Tcl_FSChdirProc *proc = fsPtr->chdirProc;
	if (proc != NULL) {
	    retVal = (*proc)(pathPtr);
	} else {
	    /* Fallback on stat-based implementation */
	    struct stat buf;
	    /* If the file can be stat'ed and is a directory and
	     * is readable, then we can chdir. */
	    if ((Tcl_FSStat(pathPtr, &buf) == 0) 
	      && (S_ISDIR(buf.st_mode))
	      && (Tcl_FSAccess(pathPtr, R_OK) == 0)) {
		/* We allow the chdir */
		retVal = 0;
	    }
	}
    }

    if (retVal != -1) {
	/* 
	 * The cwd changed, or an error was thrown.  If an error was
	 * thrown, we can just continue (and that will report the error
	 * to the user).  If there was no error we must assume that the
	 * cwd was actually changed to the normalized value we
	 * calculated above, and we must therefore cache that
	 * information.
	 */
	if (retVal == TCL_OK) {
	    /* Get a lock on the cwd while we modify it */
	    Tcl_MutexLock(&cwdMutex);
	    /* Free up the previous cwd we stored */
	    if (cwdPathPtr != NULL) {
		Tcl_DecrRefCount(cwdPathPtr);
	    }
	    /* Now remember the current cwd */
	    cwdPathPtr = normDirName;
	    Tcl_IncrRefCount(cwdPathPtr);
	    Tcl_MutexUnlock(&cwdMutex);
	}
    }
    
    return (retVal);
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FSLoadFile --
 *
 *	Dynamically loads a binary code file into memory and returns
 *	the addresses of two procedures within that file, if they are
 *	defined.  The appropriate function for the filesystem to which
 *	pathPtr belongs will be called.
 *
 * Results:
 *	A standard Tcl completion code.  If an error occurs, an error
 *	message is left in the interp's result.
 *
 * Side effects:
 *	New code suddenly appears in memory.  We remember which
 *	filesystem loaded the code, so that we can use that filesystem's
 *	unloadProc to unload the code when that occurs.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_FSLoadFile(interp, pathPtr, sym1, sym2, proc1Ptr, proc2Ptr, 
	       clientDataPtr, unloadProcPtr)
    Tcl_Interp *interp;		/* Used for error reporting. */
    Tcl_Obj *pathPtr;		/* Name of the file containing the desired
				 * code. */
    char *sym1, *sym2;		/* Names of two procedures to look up in
				 * the file's symbol table. */
    Tcl_PackageInitProc **proc1Ptr, **proc2Ptr;
				/* Where to return the addresses corresponding
				 * to sym1 and sym2. */
    ClientData *clientDataPtr;	/* Filled with token for dynamically loaded
				 * file which will be passed back to 
				 * (*unloadProcPtr)() to unload the file. */
    Tcl_FSUnloadFileProc **unloadProcPtr;	
                                /* Filled with address of Tcl_FSUnloadFileProc
                                 * function which should be used for
                                 * this file. */
{
    Tcl_Filesystem *fsPtr = Tcl_FSGetFileSystemForPath(pathPtr);
    if (fsPtr != NULL) {
	Tcl_FSLoadFileProc *proc = fsPtr->loadFileProc;
	if (proc != NULL) {
	    int retVal = (*proc)(interp, pathPtr, sym1, sym2,
			     proc1Ptr, proc2Ptr, clientDataPtr);
	    if (retVal != -1) {
		/* 
		 * We handled it.  Remember which unload file 
		 * proc to use. 
		 */
		(*unloadProcPtr) = fsPtr->unloadFileProc;
	    }
	    return retVal;
	} else {
	    Tcl_Filesystem *copyFsPtr;
	    /* Get a temporary filename to use, first to
	     * copy the file into, and then to load. */
	    Tcl_Obj *copyToPtr = TclpTempFileName();
	    if (copyToPtr == NULL) {
	        return -1;
	    }
	    Tcl_IncrRefCount(copyToPtr);
	    
	    copyFsPtr = Tcl_FSGetFileSystemForPath(copyToPtr);
	    if ((copyFsPtr == NULL) || (copyFsPtr == fsPtr)) {
		/* We already know we can't use Tcl_FSLoadFile from 
		 * this filesystem, and we must avoid a possible
		 * infinite loop. */
		Tcl_DecrRefCount(copyToPtr);
		return -1;
	    }
	    
	    if (Tcl_FSCopyFile(pathPtr, copyToPtr) == 0) {
		/* 
		 * Do we need to set appropriate permissions 
		 * on the file?  This may be required on some
		 * systems.  On Unix we could do loop over
		 * the file attributes, and set any that are
		 * called "-permissions" to 0777.  Or directly:
		 * 
		 * Tcl_Obj* perm = Tcl_NewStringObj("0777",-1);
		 * Tcl_IncrRefCount(perm);
		 * Tcl_FSFileAttrsSet(NULL, 2, copyToPtr, perm);
		 * Tcl_DecrRefCount(perm);
		 * 
		 */
		ClientData newClientData = NULL;
		Tcl_FSUnloadFileProc *newUnloadProcPtr = NULL;
		FsDivertLoad *tvdlPtr;
		int retVal;
		
		retVal = Tcl_FSLoadFile(interp, copyToPtr, sym1, sym2, proc1Ptr, 
			       proc2Ptr, &newClientData, &newUnloadProcPtr);
	        if (retVal == -1) {
		    /* The file didn't load successfully */
		    Tcl_FSDeleteFile(copyToPtr);
		    Tcl_DecrRefCount(copyToPtr);
		    return -1;
		}
		/* 
		 * When we unload this file, we need to divert the 
		 * unloading so we can unload and cleanup the 
		 * temporary file correctly.
		 */
		tvdlPtr = (FsDivertLoad*) ckalloc(sizeof(FsDivertLoad));

		/* 
		 * Remember three pieces of information.  This allows
		 * us to cleanup the diverted load completely, on
		 * platforms which allow proper unloading of code.
		 */
		tvdlPtr->clientData = newClientData;
		tvdlPtr->unloadProcPtr = newUnloadProcPtr;
		/* copyToPtr is already incremented for this reference */
		tvdlPtr->divertedFile = copyToPtr;
		copyToPtr = NULL;
		(*clientDataPtr) = (ClientData) tvdlPtr;
		(*unloadProcPtr) = &FSUnloadTempFile;
		
		return retVal;
	    }
	}
    }
    return -1;
}

/*
 *---------------------------------------------------------------------------
 *
 * FSUnloadTempFile --
 *
 *	This function is called when we loaded a library of code via
 *	an intermediate temporary file.  This function ensures
 *	the library is correctly unloaded and the temporary file
 *	is correctly deleted.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The effects of the 'unload' function called, and of course
 *	the temporary file will be deleted.
 *
 *---------------------------------------------------------------------------
 */
static void 
FSUnloadTempFile(clientData)
    ClientData clientData;    /* ClientData returned by a previous call
			       * to Tcl_FSLoadFile().  The clientData is 
			       * a token that represents the loaded 
			       * file. */
{
    FsDivertLoad *tvdlPtr = (FsDivertLoad*)clientData;
    /* 
     * This test should never trigger, since we give
     * the client data in the function above.
     */
    if (tvdlPtr == NULL) { return; }
    
    /* Call the real 'unloadfile' proc we actually used. */
    if (tvdlPtr->unloadProcPtr != NULL) {
	(*tvdlPtr->unloadProcPtr)(tvdlPtr->clientData);
    }
    
    /* Remove the temporary file we created. */
    Tcl_FSDeleteFile(tvdlPtr->divertedFile);
    
    /* And free up the allocations */
    Tcl_DecrRefCount(tvdlPtr->divertedFile);
    ckfree((char*)tvdlPtr);
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_FSReadlink --
 *
 *	This function replaces the library version of readlink().
 *	The appropriate function for the filesystem to which pathPtr
 *	belongs will be called.
 *
 * Results:
 *	The result is a Tcl_Obj specifying the contents
 *	of the symbolic link given by 'path', or NULL if the symbolic
 *	link could not be read.  The result is owned by the caller,
 *	which should call Tcl_DecrRefCount when the result is no longer
 *	needed.
 *
 * Side effects:
 *	See readlink() documentation.
 *
 *---------------------------------------------------------------------------
 */

Tcl_Obj *
Tcl_FSReadlink(pathPtr)
    Tcl_Obj *pathPtr;		/* Path of file to readlink (UTF-8). */
{
    Tcl_Filesystem *fsPtr = Tcl_FSGetFileSystemForPath(pathPtr);
    if (fsPtr != NULL) {
	Tcl_FSReadlinkProc *proc = fsPtr->readlinkProc;
	if (proc != NULL) {
	    return (*proc)(pathPtr);
	}
    }
    /*
     * If S_IFLNK isn't defined it means that the machine doesn't
     * support symbolic links, so the file can't possibly be a
     * symbolic link.  Generate an EINVAL error, which is what
     * happens on machines that do support symbolic links when
     * you invoke readlink on a file that isn't a symbolic link.
     */
#ifndef S_IFLNK
    errno = EINVAL;
#endif /* S_IFLNK */
    return NULL;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_FSListVolumes --
 *
 *	Lists the currently mounted volumes.
 *	The chain of functions that have been "inserted" into the
 *	filesystem will be called in succession; each may add to 
 *	the Tcl result, until all mounted file systems are listed.
 *
 * Results:
 *	A standard Tcl result.  Will always be TCL_OK, since there is no way
 *	that this command can fail.  Also, the interpreter's result is set to 
 *	the list of volumes.
 *
 * Side effects:
 *	None
 *
 *---------------------------------------------------------------------------
 */

int
Tcl_FSListVolumes(interp)
    Tcl_Interp *interp;		/* Interpreter for returning volume list. */
{
    FilesystemRecord *fsRecPtr;

    /*
     * Call each of the "listVolumes" function in succession.
     * A non-NULL return value indicates the particular function has
     * succeeded.  We call all the functions registered, since we want
     * a list of all drives from all filesystems.
     */

    fsRecPtr = FsGetIterator();
    while (fsRecPtr != NULL) {
	Tcl_FSListVolumesProc *proc = fsRecPtr->fsPtr->listVolumesProc;
	if (proc != NULL) {
	    /* Ignore return value */
	    (*proc)(interp);
	}
	fsRecPtr = fsRecPtr->nextPtr;
    }
    FsReleaseIterator();
    
    return TCL_OK;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_FSRenameFile --
 *
 *	If the two paths given belong to the same filesystem, we call
 *	that filesystems rename function.  Otherwise we simply
 *	return the posix error 'EXDEV', and -1.
 *
 * Results:
 *      Standard Tcl error code if a function was called.
 *
 * Side effects:
 *	A file may be renamed.
 *
 *---------------------------------------------------------------------------
 */

int
Tcl_FSRenameFile(srcPathPtr, destPathPtr)
    Tcl_Obj* srcPathPtr;	/* Pathname of file or dir to be renamed
				 * (UTF-8). */
    Tcl_Obj *destPathPtr;	/* New pathname of file or directory
				 * (UTF-8). */
{
    int retVal = -1;
    Tcl_Filesystem *fsPtr, *fsPtr2;
    fsPtr = Tcl_FSGetFileSystemForPath(srcPathPtr);
    fsPtr2 = Tcl_FSGetFileSystemForPath(destPathPtr);

    if (fsPtr == fsPtr2 && fsPtr != NULL) {
	Tcl_FSRenameFileProc *proc = fsPtr->renameFileProc;
	if (proc != NULL) {
	    retVal =  (*proc)(srcPathPtr, destPathPtr);
	}
    }
    if (retVal == -1) {
	Tcl_SetErrno(EXDEV);
    }
    return retVal;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_FSCopyFile --
 *
 *	If the two paths given belong to the same filesystem, we call
 *	that filesystem's copy function.  Otherwise we simply
 *	return the posix error 'EXDEV', and -1.
 *
 * Results:
 *      Standard Tcl error code if a function was called.
 *
 * Side effects:
 *	A file may be copied.
 *
 *---------------------------------------------------------------------------
 */

int 
Tcl_FSCopyFile(srcPathPtr, destPathPtr)
    Tcl_Obj* srcPathPtr;	/* Pathname of file to be copied (UTF-8). */
    Tcl_Obj *destPathPtr;	/* Pathname of file to copy to (UTF-8). */
{
    int retVal = -1;
    Tcl_Filesystem *fsPtr, *fsPtr2;
    fsPtr = Tcl_FSGetFileSystemForPath(srcPathPtr);
    fsPtr2 = Tcl_FSGetFileSystemForPath(destPathPtr);

    if (fsPtr == fsPtr2 && fsPtr != NULL) {
	Tcl_FSCopyFileProc *proc = fsPtr->copyFileProc;
	if (proc != NULL) {
	    retVal = (*proc)(srcPathPtr, destPathPtr);
	}
    }
    if (retVal == -1) {
	Tcl_SetErrno(EXDEV);
    }
    return retVal;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_FSDeleteFile --
 *
 *	The appropriate function for the filesystem to which pathPtr
 *	belongs will be called.
 *
 * Results:
 *      Standard Tcl error code.
 *
 * Side effects:
 *	A file may be deleted.
 *
 *---------------------------------------------------------------------------
 */

int
Tcl_FSDeleteFile(pathPtr)
    Tcl_Obj *pathPtr;		/* Pathname of file to be removed (UTF-8). */
{
    Tcl_Filesystem *fsPtr = Tcl_FSGetFileSystemForPath(pathPtr);
    if (fsPtr != NULL) {
	Tcl_FSDeleteFileProc *proc = fsPtr->deleteFileProc;
	if (proc != NULL) {
	    return (*proc)(pathPtr);
	}
    }
    return -1;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_FSCreateDirectory --
 *
 *	The appropriate function for the filesystem to which pathPtr
 *	belongs will be called.
 *
 * Results:
 *      Standard Tcl error code.
 *
 * Side effects:
 *	A directory may be created.
 *
 *---------------------------------------------------------------------------
 */

int
Tcl_FSCreateDirectory(pathPtr)
    Tcl_Obj *pathPtr;		/* Pathname of directory to create (UTF-8). */
{
    Tcl_Filesystem *fsPtr = Tcl_FSGetFileSystemForPath(pathPtr);
    if (fsPtr != NULL) {
	Tcl_FSCreateDirectoryProc *proc = fsPtr->createDirectoryProc;
	if (proc != NULL) {
	    return (*proc)(pathPtr);
	}
    }
    return -1;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_FSRenameFile --
 *
 *	If the two paths given belong to the same filesystem, we call
 *	that filesystems copy-directory function.  Otherwise we simply
 *	return the posix error 'EXDEV', and -1.
 *
 * Results:
 *      Standard Tcl error code if a function was called.
 *
 * Side effects:
 *	A directory may be copied.
 *
 *---------------------------------------------------------------------------
 */

int
Tcl_FSCopyDirectory(srcPathPtr, destPathPtr, errorPtr)
    Tcl_Obj* srcPathPtr;	/* Pathname of directory to be copied
				 * (UTF-8). */
    Tcl_Obj *destPathPtr;	/* Pathname of target directory (UTF-8). */
    Tcl_Obj **errorPtr;	        /* If non-NULL, then will be set to a
                       	         * new object containing name of file
                       	         * causing error, with refCount 1. */
{
    int retVal = -1;
    Tcl_Filesystem *fsPtr, *fsPtr2;
    fsPtr = Tcl_FSGetFileSystemForPath(srcPathPtr);
    fsPtr2 = Tcl_FSGetFileSystemForPath(destPathPtr);

    if (fsPtr == fsPtr2 && fsPtr != NULL) {
	Tcl_FSCopyDirectoryProc *proc = fsPtr->copyDirectoryProc;
	if (proc != NULL) {
	    retVal = (*proc)(srcPathPtr, destPathPtr, errorPtr);
	}
    }
    if (retVal == -1) {
	Tcl_SetErrno(EXDEV);
    }
    return retVal;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_FSRemoveDirectory --
 *
 *	The appropriate function for the filesystem to which pathPtr
 *	belongs will be called.
 *
 * Results:
 *      Standard Tcl error code.
 *
 * Side effects:
 *	A directory may be deleted.
 *
 *---------------------------------------------------------------------------
 */

int
Tcl_FSRemoveDirectory(pathPtr, recursive, errorPtr)
    Tcl_Obj *pathPtr;		/* Pathname of directory to be removed
				 * (UTF-8). */
    int recursive;		/* If non-zero, removes directories that
				 * are nonempty.  Otherwise, will only remove
				 * empty directories. */
    Tcl_Obj **errorPtr;	        /* If non-NULL, then will be set to a
				 * new object containing name of file
				 * causing error, with refCount 1. */
{
    Tcl_Filesystem *fsPtr = Tcl_FSGetFileSystemForPath(pathPtr);
    if (fsPtr != NULL) {
	Tcl_FSRemoveDirectoryProc *proc = fsPtr->removeDirectoryProc;
	if (proc != NULL) {
	    return (*proc)(pathPtr, recursive, errorPtr);
	}
    }
    return -1;
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
Tcl_FSConvertToPathType(interp, objPtr)
    Tcl_Interp *interp;		/* Interpreter in which to store error
				 * message (if necessary). */
    Tcl_Obj *objPtr;		/* Object to convert to a valid, current
                    		 * path type. */
{
    /* 
     * While it is bad practice to examine an object's type directly,
     * this is actually the best thing to do here.  The reason is that
     * if we are converting this object to FsPath type for the first
     * time, we don't need to worry whether the 'cwd' has changed.
     * On the other hand, if this object is already of FsPath type,
     * and is a relative path, we do have to worry about the cwd.
     * If the cwd has changed, we must recompute the path.
     */
    if (objPtr->typePtr == &tclFsPathType) {
	FsPath *fsPathPtr = (FsPath*) objPtr->internalRep.otherValuePtr;
	if (fsPathPtr->cwdPtr == NULL) {
	    return TCL_OK;
	} else {
	    if (FsCwdPointerEquals(fsPathPtr->cwdPtr)) {
		return TCL_OK;
	    } else {
		FreeFsPathInternalRep(objPtr);
		objPtr->typePtr = NULL;
		return Tcl_ConvertToType(interp, objPtr, &tclFsPathType);
	    }
	}
    } else {
	return Tcl_ConvertToType(interp, objPtr, &tclFsPathType);
    }
}


/* 
 * Helper function for SetFsPathFromAny.  Returns position of first
 * directory delimiter in the path.
 */
static int
FindSplitPos(path, separator)
    char *path;
    char *separator;
{
    int count = 0;
    switch (tclPlatform) {
	case TCL_PLATFORM_UNIX:
	case TCL_PLATFORM_MAC:
	    while (path[count] != 0) {
	        if (path[count] == *separator) {
	            return count;
	        }
	        count++;
	    }
	    break;

	case TCL_PLATFORM_WINDOWS:
	    while (path[count] != 0) {
		if (path[count] == *separator || path[count] == '\\') {
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
 * SetFsPathFromAbsoluteNormalized --
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

static int
SetFsPathFromAbsoluteNormalized(interp, objPtr)
    Tcl_Interp *interp;		/* Used for error reporting if not NULL. */
    Tcl_Obj *objPtr;		/* The object to convert. */
{
    FsPath *fsPathPtr;

    if (objPtr->typePtr == &tclFsPathType) {
        return TCL_OK;
    }
    
    /* Free old representation */
    if (objPtr->typePtr != NULL) {
	if (objPtr->bytes == NULL) {
	    objPtr->typePtr->updateStringProc(objPtr);
	}
	if ((objPtr->typePtr->freeIntRepProc) != NULL) {
	    (*objPtr->typePtr->freeIntRepProc)(objPtr);
	}
    }

    fsPathPtr = (FsPath*)ckalloc((unsigned)sizeof(FsPath));
    /* It's a pure normalized absolute path */
    fsPathPtr->translatedPathPtr = NULL;
    fsPathPtr->normPathPtr = objPtr;
    fsPathPtr->cwdPtr = NULL;
    fsPathPtr->nativePathPtr = NULL;
    fsPathPtr->fsRecPtr = NULL;
    fsPathPtr->filesystemEpoch = -1;

    objPtr->internalRep.otherValuePtr = fsPathPtr;
    objPtr->typePtr = &tclFsPathType;

    return TCL_OK;
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
SetFsPathFromAny(interp, objPtr)
    Tcl_Interp *interp;		/* Used for error reporting if not NULL. */
    Tcl_Obj *objPtr;		/* The object to convert. */
{
    int len;
    FsPath *fsPathPtr;
    Tcl_DString buffer;
    char *name;
    
    if (objPtr->typePtr == &tclFsPathType) {
	return TCL_OK;
    }
    
    /* Free old representation */
    if (objPtr->typePtr != NULL) {
	if (objPtr->bytes == NULL) {
	    objPtr->typePtr->updateStringProc(objPtr);
	}
	if ((objPtr->typePtr->freeIntRepProc) != NULL) {
	    (*objPtr->typePtr->freeIntRepProc)(objPtr);
	}
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
    name = Tcl_GetStringFromObj(objPtr,&len);

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
	
	split = FindSplitPos(name, &separator);
	if (split != len) {
	    /* We have multiple pieces '~user/foo/bar...' */
	    name[split] = '\0';
	}
	/* Do some tilde substitution */
	if (name[1] == '\0') {
	    /* We have just '~' */
	    char *dir;
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
		if (interp) {
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

	Tcl_DStringInit(&buffer);
	if (split == len) {
	    /* We have the result we need in the wrong DString */
	    Tcl_DStringAppend(&buffer, expandedUser, Tcl_DStringLength(&temp));
	} else {
	    /* 
	     * Build a simple 2 element list and join it up with
	     * the tilde substitution in place 
	     */
	    char *argv[2];
            argv[0] = expandedUser;
            argv[1] = name+split+1;
	    Tcl_JoinPath(2, argv, &buffer);
	}
	Tcl_DStringFree(&temp);
    } else {
	Tcl_DStringInit(&buffer);
	Tcl_JoinPath(1, &name, &buffer);
    }

    len = Tcl_DStringLength(&buffer);

    /* 
     * Now we have a translated filename in 'buffer', of
     * length 'len'.  This will have forward slashes on
     * Windows, and will not contain any ~user sequences.
     */
    
    fsPathPtr = (FsPath*)ckalloc((unsigned)sizeof(FsPath));
    fsPathPtr->translatedPathPtr = ckalloc((unsigned)(1+len));
    strcpy(fsPathPtr->translatedPathPtr, Tcl_DStringValue(&buffer));
    Tcl_DStringFree(&buffer);
    fsPathPtr->normPathPtr = NULL;
    fsPathPtr->cwdPtr = NULL;
    fsPathPtr->nativePathPtr = NULL;
    fsPathPtr->fsRecPtr = NULL;
    fsPathPtr->filesystemEpoch = -1;

    objPtr->internalRep.otherValuePtr = fsPathPtr;
    objPtr->typePtr = &tclFsPathType;

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
    Tcl_Obj* fromFilesystem;
    ClientData clientData;
{
    Tcl_Obj *objPtr;
    FsPath *fsPathPtr, *fsFromPtr;
    Tcl_FSInternalToNormalizedProc *proc;
    
    if (Tcl_FSConvertToPathType(NULL, fromFilesystem) != TCL_OK) {
        return NULL;
    }
    
    fsFromPtr = (FsPath*) fromFilesystem->internalRep.otherValuePtr;

    proc = fsFromPtr->fsRecPtr->fsPtr->internalToNormalizedProc;

    if (proc == NULL) {
        return NULL;
    }
    
    objPtr = (*proc)(clientData);
    if (objPtr == NULL) {
        return NULL;
    }
    
    /* 
     * Free old representation; shouldn't normally be any,
     * but best to be safe. 
     */
    if (objPtr->typePtr != NULL) {
	if (objPtr->bytes == NULL) {
	    objPtr->typePtr->updateStringProc(objPtr);
	}
	if ((objPtr->typePtr->freeIntRepProc) != NULL) {
	    (*objPtr->typePtr->freeIntRepProc)(objPtr);
	}
    }
    
    fsPathPtr = (FsPath*)ckalloc((unsigned)sizeof(FsPath));
    fsPathPtr->translatedPathPtr = NULL;
    /* Circular reference, by design */
    fsPathPtr->normPathPtr = objPtr;
    fsPathPtr->cwdPtr = NULL;
    fsPathPtr->nativePathPtr = clientData;
    fsPathPtr->fsRecPtr = fsFromPtr->fsRecPtr;
    fsPathPtr->filesystemEpoch = fsFromPtr->filesystemEpoch;

    objPtr->internalRep.otherValuePtr = fsPathPtr;
    objPtr->typePtr = &tclFsPathType;
    return objPtr;
}

static void
FreeFsPathInternalRep(pathObjPtr)
    Tcl_Obj *pathObjPtr;	/* Path object with internal rep to free. */
{
    register FsPath* fsPathPtr = 
      (FsPath*) pathObjPtr->internalRep.otherValuePtr;

    if (fsPathPtr->translatedPathPtr != NULL) {
	ckfree((char *) fsPathPtr->translatedPathPtr);
    }
    if (fsPathPtr->normPathPtr != NULL) {
	if (fsPathPtr->normPathPtr != pathObjPtr) {
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
        fsPathPtr->fsRecPtr->refCount--;
    }

    ckfree((char*) fsPathPtr);
}

static void
DupFsPathInternalRep(srcPtr, copyPtr)
    Tcl_Obj *srcPtr;		/* Path obj with internal rep to copy. */
    Tcl_Obj *copyPtr;		/* Path obj with internal rep to set. */
{
    register FsPath* srcFsPathPtr = 
      (FsPath*) srcPtr->internalRep.otherValuePtr;
    register FsPath* copyFsPathPtr = 
      (FsPath*) ckalloc((unsigned)sizeof(FsPath));
    Tcl_FSDupInternalRepProc *dupProc;
    
    copyPtr->internalRep.otherValuePtr = copyFsPathPtr;

    if (srcFsPathPtr->translatedPathPtr != NULL) {
	copyFsPathPtr->translatedPathPtr = 
	  ckalloc(1+strlen(srcFsPathPtr->translatedPathPtr));
	strcpy(copyFsPathPtr->translatedPathPtr, 
	       srcFsPathPtr->translatedPathPtr);
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
        copyFsPathPtr->fsRecPtr->refCount++;
    }

    copyPtr->typePtr = &tclFsPathType;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_FSGetTranslatedPath --
 *
 *      This function attempts to extract the translated path string
 *      from the given Tcl_Obj.  If the translation succeeds (i.e. the
 *      object is a valid path), then it is returned.  Otherwise NULL
 *      will be returned, and an error message may be left in the
 *      interpreter.
 *
 * Results:
 *      NULL or a valid string.
 *
 * Side effects:
 *	Only those of 'Tcl_FSConvertToPathType'
 *
 *---------------------------------------------------------------------------
 */

char* 
Tcl_FSGetTranslatedPath(interp, pathPtr)
    Tcl_Interp *interp;
    Tcl_Obj* pathPtr;
{
    register FsPath* srcFsPathPtr;
    if (Tcl_FSConvertToPathType(interp, pathPtr) != TCL_OK) {
	return NULL;
    }
    srcFsPathPtr = (FsPath*) pathPtr->internalRep.otherValuePtr;
    if (srcFsPathPtr->translatedPathPtr == NULL) {
        /* 
         * It is a pure absolute, normalized path object.
         * This is something like being a 'pure list'.  The
         * object's string, translatedPath and normalizedPath
         * are all identical.
         */
	return Tcl_GetString(srcFsPathPtr->normPathPtr);
    } else {
	/* It is an ordinary path object */
	return srcFsPathPtr->translatedPathPtr;
    }
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
Tcl_FSGetNormalizedPath(interp, pathObjPtr)
    Tcl_Interp *interp;
    Tcl_Obj* pathObjPtr;
{
    register FsPath* srcFsPathPtr;
    if (Tcl_FSConvertToPathType(interp, pathObjPtr) != TCL_OK) {
	return NULL;
    }
    srcFsPathPtr = (FsPath*) pathObjPtr->internalRep.otherValuePtr;
    if (srcFsPathPtr->normPathPtr == NULL) {
	int relative = 0;
	char *path = srcFsPathPtr->translatedPathPtr;
	Tcl_DString atemp;
	
	if ((path[0] != '\0') && (Tcl_GetPathType(path) == TCL_PATH_RELATIVE)) {
	    char * pair[2];
	    Tcl_Obj *cwd = Tcl_FSGetCwd(interp);

	    if (cwd == NULL) {
		return NULL;
	    }
		
	    /* 
	     * The efficiency of this piece of code could
	     * be improved, given the new object interfaces.
	     */
	    pair[0] = Tcl_GetString(cwd);
	    pair[1] = path;

	    Tcl_DStringInit(&atemp);
	    Tcl_JoinPath(2, pair, &atemp);
	    path = Tcl_DStringValue(&atemp);
	    Tcl_DecrRefCount(cwd);
	    
	    relative = 1;
	}

	/* Already has refCount incremented */
	srcFsPathPtr->normPathPtr = FSNormalizeAbsolutePath(interp, path);
	if (!strcmp(Tcl_GetString(srcFsPathPtr->normPathPtr),
		    Tcl_GetString(pathObjPtr))) {
	    /* 
	     * The path was already normalized.  
	     * Get rid of the duplicate.
	     */
	    Tcl_DecrRefCount(srcFsPathPtr->normPathPtr);
	    /* 
	     * We do *not* increment the refCount for 
	     * this circular reference 
	     */
	    srcFsPathPtr->normPathPtr = pathObjPtr;
	}
	if (relative) {
	    Tcl_DStringFree(&atemp);

	    /* Get a quick, temporary lock on the cwd while we copy it */
	    Tcl_MutexLock(&cwdMutex);
	    srcFsPathPtr->cwdPtr = cwdPathPtr;
	    Tcl_IncrRefCount(srcFsPathPtr->cwdPtr);
	    Tcl_MutexUnlock(&cwdMutex);
	}
    }
    return srcFsPathPtr->normPathPtr;
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
Tcl_FSGetInternalRep(pathObjPtr, fsPtr)
    Tcl_Obj* pathObjPtr;
    Tcl_Filesystem *fsPtr;
{
    register FsPath* srcFsPathPtr;
    
    if (Tcl_FSConvertToPathType(NULL, pathObjPtr) != TCL_OK) {
	return NULL;
    }
    srcFsPathPtr = (FsPath*) pathObjPtr->internalRep.otherValuePtr;
    
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
	Tcl_FSGetFileSystemForPath(pathObjPtr);
    }

    if (fsPtr != srcFsPathPtr->fsRecPtr->fsPtr) {
	return NULL;
    }

    if (srcFsPathPtr->nativePathPtr == NULL) {
	Tcl_FSCreateInternalRepProc *proc;
	proc = srcFsPathPtr->fsRecPtr->fsPtr->createInternalRepProc;

	if (proc == NULL) {
	    return NULL;
	}
	srcFsPathPtr->nativePathPtr = (*proc)(pathObjPtr);
    }
    return srcFsPathPtr->nativePathPtr;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_FSGetNativePath --
 *
 *      This function is for use by the Win/Unix/MacOS native filesystems,
 *      so that they can easily retrieve the native (char* or TCHAR*)
 *      representation of a path.  Other filesystems will probably
 *      want to implement similar functions.  They basically act as a 
 *      safety net around Tcl_FSGetInternalRep.  Normally your file-
 *      system procedures will always be called with path objects
 *      already converted to the correct filesystem, but if for 
 *      some reason they are called directly (i.e. by procedures 
 *      not in this file), then one cannot necessarily guarantee that
 *      the path object pointer is from the correct filesystem.
 *      
 *      Note: in the future it might be desireable to have separate
 *      versions of this function with different signatures, for
 *      example Tcl_FSGetNativeMacPath, Tcl_FSGetNativeUnixPath etc.
 *      Right now, since native paths are all string based, we use just
 *      one function.  On MacOS we could possibly use an FSSpec or
 *      FSRef as the native representation.
 *
 * Results:
 *      NULL or a valid native path.
 *
 * Side effects:
 *	See Tcl_FSGetInternalRep.
 *
 *---------------------------------------------------------------------------
 */

char* 
Tcl_FSGetNativePath(pathObjPtr)
    Tcl_Obj* pathObjPtr;
{
    return (char*)Tcl_FSGetInternalRep(pathObjPtr, &nativeFilesystem);
}

/*
 *---------------------------------------------------------------------------
 *
 * NativeCreateNativeRep --
 *
 *      Create a native representation for the given path.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */
ClientData 
NativeCreateNativeRep(pathObjPtr)
    Tcl_Obj* pathObjPtr;
{
    char *nativePathPtr;
    Tcl_DString ds;
    Tcl_Obj* normPtr;
    int len;
    char *str;

    /* Make sure the normalized path is set */
    normPtr = Tcl_FSGetNormalizedPath(NULL, pathObjPtr);

    str = Tcl_GetStringFromObj(normPtr,&len);
#ifdef __WIN32__
    Tcl_WinUtfToTChar(str, len, &ds);
    nativePathPtr = ckalloc((unsigned)(2+Tcl_DStringLength(&ds)));
    memcpy((VOID*)nativePathPtr, (VOID*)Tcl_DStringValue(&ds), 
	   (size_t) (2+Tcl_DStringLength(&ds)));
#else
    Tcl_UtfToExternalDString(NULL, str, len, &ds);
    nativePathPtr = ckalloc((unsigned)(1+Tcl_DStringLength(&ds)));
    memcpy((VOID*)nativePathPtr, (VOID*)Tcl_DStringValue(&ds), 
	  (size_t) (1+Tcl_DStringLength(&ds)));
#endif
	  
    Tcl_DStringFree(&ds);
    return (ClientData)nativePathPtr;
}

/*
 *---------------------------------------------------------------------------
 *
 * TclpNativeToNormalized --
 *
 *      Convert native format to a normalized path object, with refCount
 *      of zero.
 *
 * Results:
 *      A valid normalized path.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */
Tcl_Obj* 
TclpNativeToNormalized(clientData)
    ClientData clientData;
{
    Tcl_DString ds;
    Tcl_Obj *objPtr;
    
#ifdef __WIN32__
    Tcl_WinTCharToUtf((char*)clientData, -1, &ds);
#else
    Tcl_ExternalToUtfDString(NULL, (char*)clientData, -1, &ds);
#endif
    objPtr = Tcl_NewStringObj(Tcl_DStringValue(&ds),Tcl_DStringLength(&ds));
    Tcl_DStringFree(&ds);
    
    return objPtr;
}


/*
 *---------------------------------------------------------------------------
 *
 * NativeDupInternalRep --
 *
 *      Duplicate the native representation.
 *
 * Results:
 *      The copied native representation, or NULL if it is not possible
 *      to copy the representation.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */
ClientData 
NativeDupInternalRep(clientData)
    ClientData clientData;
{
#ifdef __WIN32__
    /* Copying internal representations is complicated with multi-byte TChars */
    return NULL;
#else
    if (clientData == NULL) {
        return NULL;
    } else {
	char *native = (char*)clientData;
	char *copy = ckalloc((unsigned)(1+strlen(native)));
	strcpy(copy,native);
	return (ClientData)copy;
    }
#endif
}

/*
 *---------------------------------------------------------------------------
 *
 * NativePathInFilesystem --
 *
 *      Any path object is acceptable to the native filesystem, by
 *      default (we will throw errors when illegal paths are actually
 *      tried to be used).
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
NativePathInFilesystem(pathPtr, clientDataPtr)
    Tcl_Obj *pathPtr;
    ClientData *clientDataPtr;
{
    int len;
    Tcl_GetStringFromObj(pathPtr,&len);
    if (len == 0) {
        return -1;
    } else {
	/* We accept any path as valid */
	return TCL_OK;
    }
}

/*
 *---------------------------------------------------------------------------
 *
 * NativeFreeInternalRep --
 *
 *      Free a native internal representation, which will be non-NULL.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *	Memory is released.
 *
 *---------------------------------------------------------------------------
 */
void 
NativeFreeInternalRep(clientData)
    ClientData clientData;
{
    ckfree((char*)clientData);
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_FSFileSystemInfo --
 *
 *      This function returns a list of two elements.  The first
 *      element is the name of the filesystem (e.g. "native" or "vfs"),
 *      and the second is the particular type of the given path within
 *      that filesystem.
 *
 * Results:
 *      A list of two elements.
 *
 * Side effects:
 *	The object may be converted to a path type.
 *
 *---------------------------------------------------------------------------
 */
Tcl_Obj*
Tcl_FSFileSystemInfo(pathObjPtr)
    Tcl_Obj* pathObjPtr;
{
    Tcl_Obj *resPtr;
    Tcl_FSFilesystemPathTypeProc *proc;
    Tcl_Filesystem *fsPtr = Tcl_FSGetFileSystemForPath(pathObjPtr);
    
    if (fsPtr == NULL) {
	return NULL;
    }
    
    resPtr = Tcl_NewListObj(0,NULL);
    
    Tcl_ListObjAppendElement(NULL, resPtr, 
			     Tcl_NewStringObj(fsPtr->typeName,-1));

    proc = fsPtr->filesystemPathTypeProc;
    if (proc != NULL) {
	Tcl_Obj *typePtr = (*proc)(pathObjPtr);
	if (typePtr != NULL) {
	    Tcl_ListObjAppendElement(NULL, resPtr, typePtr);
	}
    }
    
    return resPtr;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_FSPathSeparator --
 *
 *      This function returns the separator to be used for a given
 *      path.  The object returned should have a refCount of zero
 *
 * Results:
 *      A Tcl object, with a refCount of zero.  If the caller
 *      needs to retain a reference to the object, it should
 *      call Tcl_IncrRefCount.
 *
 * Side effects:
 *	The path object may be converted to a path type.
 *
 *---------------------------------------------------------------------------
 */
Tcl_Obj*
Tcl_FSPathSeparator(pathObjPtr)
    Tcl_Obj* pathObjPtr;
{
    Tcl_Filesystem *fsPtr = Tcl_FSGetFileSystemForPath(pathObjPtr);
    
    if (fsPtr == NULL) {
	return NULL;
    }
    if (fsPtr->filesystemSeparatorProc != NULL) {
	return (*fsPtr->filesystemSeparatorProc)(pathObjPtr);
    }
    
    return NULL;
}

/*
 *---------------------------------------------------------------------------
 *
 * NativeFilesystemSeparator --
 *
 *      This function is part of the native filesystem support, and
 *      returns the separator for the given path.
 *
 * Results:
 *      String object containing the separator character.
 *
 * Side effects:
 *	None.
 *
 *---------------------------------------------------------------------------
 */
Tcl_Obj*
NativeFilesystemSeparator(pathObjPtr)
    Tcl_Obj* pathObjPtr;
{
    char *separator = NULL; /* lint */
    switch (tclPlatform) {
	case TCL_PLATFORM_UNIX:
	    separator = "/";
	    break;
	case TCL_PLATFORM_WINDOWS:
	    separator = "\\";
	    break;
	case TCL_PLATFORM_MAC:
	    separator = ":";
	    break;
    }
    return Tcl_NewStringObj(separator,1);
}

/*
 *---------------------------------------------------------------------------
 *
 * NativeFilesystemPathType --
 *
 *      This function is part of the native filesystem support, and
 *      returns the path type of the given path.  Right now it simply
 *      returns NULL.  In the future it could return specific path
 *      types, like 'network' for a natively-networked path, etc.
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
NativeFilesystemPathType(pathObjPtr)
    Tcl_Obj* pathObjPtr;
{
    /* All native paths are of the same type */
    return NULL;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_FSGetFileSystemForPath --
 *
 *      This function determines which filesystem to use for a
 *      particular path object, and returns the filesystem which
 *      accepts this file.  If no filesystem will accept this object
 *      as a valid file path, then NULL is returned.
 *
 * Results:
 *      NULL or a filesystem which will accept this path.
 *
 * Side effects:
 *	The object may be converted to a path type.
 *
 *---------------------------------------------------------------------------
 */

static Tcl_Filesystem*
Tcl_FSGetFileSystemForPath(pathObjPtr)
    Tcl_Obj* pathObjPtr;
{
    FilesystemRecord *fsRecPtr;
    Tcl_Filesystem* retVal = NULL;
    FsPath* srcFsPathPtr;
    
    /* Make sure pathObjPtr is of our type */

    if (Tcl_FSConvertToPathType(NULL, pathObjPtr) != TCL_OK) {
	return NULL;
    }
    
    if (Tcl_FSGetNormalizedPath(NULL, pathObjPtr) == NULL) {
	return NULL;
    }
    
    /* 
     * Get a lock on filesystemEpoch and the filesystemList
     * 
     * While we don't need the fsRecPtr until the while loop
     * below, we do want to make sure the filesystemEpoch doesn't
     * change between the 'if' and 'while' blocks, getting this
     * iterator will ensure that everything is consistent
     */
    fsRecPtr = FsGetIterator();
    
    /* Make sure pathObjPtr is of the correct epoch */
    
    srcFsPathPtr = (FsPath*) pathObjPtr->internalRep.otherValuePtr;
    
    if (srcFsPathPtr->filesystemEpoch != -1) {
	/* 
	 * Check if the filesystem has changed in some way since
	 * this object's internal representation was calculated.
	 */
	if (srcFsPathPtr->filesystemEpoch != filesystemEpoch) {
	    /* 
	     * We have to discard the stale representation and 
	     * recalculate it 
	     */
	    FreeFsPathInternalRep(pathObjPtr);
	    pathObjPtr->typePtr = NULL;
	    if (SetFsPathFromAny(NULL, pathObjPtr) != TCL_OK) {
	        goto done;
	    }
	    srcFsPathPtr = (FsPath*) pathObjPtr->internalRep.otherValuePtr;
	}
    }
    
    /* Check whether the object is already assigned to a fs */
    if (srcFsPathPtr->fsRecPtr != NULL) {
        retVal = srcFsPathPtr->fsRecPtr->fsPtr;
        goto done;
    }
    
    /*
     * Call each of the "pathInFilesystem" functions in succession.  A
     * non-return value of -1 indicates the particular function has
     * succeeded.
     */

    while ((retVal == NULL) && (fsRecPtr != NULL)) {
	Tcl_FSPathInFilesystemProc *proc = fsRecPtr->fsPtr->pathInFilesystemProc;
	if (proc != NULL) {
	    ClientData clientData = NULL;
	    int ret = (*proc)(pathObjPtr, &clientData);
	    if (ret != -1) {
		/* 
		 * We assume the srcFsPathPtr hasn't been changed 
		 * by the above call to the pathInFilesystemProc.
		 */
		srcFsPathPtr->fsRecPtr = fsRecPtr;
		srcFsPathPtr->nativePathPtr = clientData;
		srcFsPathPtr->filesystemEpoch = filesystemEpoch;
		fsRecPtr->refCount++;
		retVal = fsRecPtr->fsPtr;
	    }
	}
	fsRecPtr = fsRecPtr->nextPtr;
    }

  done:
    FsReleaseIterator();
    return retVal;
}

/*
 *---------------------------------------------------------------------------
 *
 * Tcl_FSEqualPaths --
 *
 *      This function tests whether the two paths given are equal path
 *      objects.
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
        int tempErrno;

	if (firstPtr == NULL || secondPtr == NULL) {
	    return 0;
	}
	if (!(strcmp(Tcl_GetString(firstPtr), Tcl_GetString(secondPtr)))) {
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
	if (!(strcmp(Tcl_GetString(firstPtr), Tcl_GetString(secondPtr)))) {
	    return 1;
	}
    }
    return 0;
}

/* Wrappers */

Tcl_Channel 
NativeOpenFileChannel(interp, pathPtr, modeString, permissions)
    Tcl_Interp *interp;
    Tcl_Obj *pathPtr;
    char *modeString;
    int permissions;
{
    char *trans = Tcl_FSGetTranslatedPath(interp, pathPtr);
    if (trans == NULL) {
	return NULL;
    }
    return TclpOpenFileChannel(interp, trans, modeString, permissions);
}

/* 
 * utime wants a normalized, NOT native path.  I assume a native
 * version of 'utime' doesn't exist (at least under that name) on NT/2000.
 * If a native function does exist somewhere, then we could use:
 * 
 *   return native_utime(Tcl_FSGetNativePath(pathPtr),tval);
 *   
 * This seems rather strange when compared with stat, lstat, access, etc.
 * all of which want a native path.
 */
int 
NativeUtime(pathPtr, tval)
    Tcl_Obj *pathPtr;
    struct utimbuf *tval;
{
#ifdef MAC_TCL
    long gmt_offset=TclpGetGMTOffset();
    struct utimbuf local_tval;
    local_tval.actime=tval->actime+gmt_offset;
    local_tval.modtime=tval->modtime+gmt_offset;
    return utime(Tcl_GetString(Tcl_FSGetNormalizedPath(NULL,pathPtr)),&local_tval);
#else
    return utime(Tcl_GetString(Tcl_FSGetNormalizedPath(NULL,pathPtr)),tval);
#endif
}

int 
NativeLoadFile(interp, pathPtr, sym1, sym2, proc1Ptr, proc2Ptr, clientDataPtr)
    Tcl_Interp * interp; 
    Tcl_Obj *pathPtr;
    char * sym1;
    char * sym2; 
    Tcl_PackageInitProc ** proc1Ptr; 
    Tcl_PackageInitProc ** proc2Ptr; 
    ClientData * clientDataPtr;
{
    return TclpLoadFile(interp, Tcl_FSGetTranslatedPath(NULL, pathPtr), 
	    sym1, sym2, proc1Ptr, proc2Ptr, clientDataPtr);
}

/* Everything from here on is contained in this obsolete ifdef */
#ifdef USE_OBSOLETE_FS_HOOKS

/*
 *----------------------------------------------------------------------
 *
 * TclStatInsertProc --
 *
 *	Insert the passed procedure pointer at the head of the list of
 *	functions which are used during a call to 'TclStat(...)'. The
 *	passed function should be have exactly like 'TclStat' when called
 *	during that time (see 'TclStat(...)' for more informatin).
 *	The function will be added even if it already in the list.
 *
 * Results:
 *      Normally TCL_OK; TCL_ERROR if memory for a new node in the list
 *	could not be allocated.
 *
 * Side effects:
 *      Memory allocataed and modifies the link list for 'TclStat'
 *	functions.
 *
 *----------------------------------------------------------------------
 */

int
TclStatInsertProc (proc)
    TclStatProc_ *proc;
{
    int retVal = TCL_ERROR;

    if (proc != NULL) {
	StatProc *newStatProcPtr;

	newStatProcPtr = (StatProc *)ckalloc(sizeof(StatProc));

	if (newStatProcPtr != NULL) {
	    newStatProcPtr->proc = proc;
	    Tcl_MutexLock(&obsoleteFsHookMutex);
	    newStatProcPtr->nextPtr = statProcList;
	    statProcList = newStatProcPtr;
	    Tcl_MutexUnlock(&obsoleteFsHookMutex);

	    retVal = TCL_OK;
	}
    }

    return (retVal);
}

/*
 *----------------------------------------------------------------------
 *
 * TclStatDeleteProc --
 *
 *	Removed the passed function pointer from the list of 'TclStat'
 *	functions.  Ensures that the built-in stat function is not
 *	removvable.
 *
 * Results:
 *      TCL_OK if the procedure pointer was successfully removed,
 *	TCL_ERROR otherwise.
 *
 * Side effects:
 *      Memory is deallocated and the respective list updated.
 *
 *----------------------------------------------------------------------
 */

int
TclStatDeleteProc (proc)
    TclStatProc_ *proc;
{
    int retVal = TCL_ERROR;
    StatProc *tmpStatProcPtr;
    StatProc *prevStatProcPtr = NULL;

    Tcl_MutexLock(&obsoleteFsHookMutex);
    tmpStatProcPtr = statProcList;
    /*
     * Traverse the 'statProcList' looking for the particular node
     * whose 'proc' member matches 'proc' and remove that one from
     * the list.  Ensure that the "default" node cannot be removed.
     */

    while ((retVal == TCL_ERROR) && (tmpStatProcPtr != NULL)) {
	if (tmpStatProcPtr->proc == proc) {
	    if (prevStatProcPtr == NULL) {
		statProcList = tmpStatProcPtr->nextPtr;
	    } else {
		prevStatProcPtr->nextPtr = tmpStatProcPtr->nextPtr;
	    }

	    ckfree((char *)tmpStatProcPtr);

	    retVal = TCL_OK;
	} else {
	    prevStatProcPtr = tmpStatProcPtr;
	    tmpStatProcPtr = tmpStatProcPtr->nextPtr;
	}
    }

    Tcl_MutexUnlock(&obsoleteFsHookMutex);
    return (retVal);
}

/*
 *----------------------------------------------------------------------
 *
 * TclAccessInsertProc --
 *
 *	Insert the passed procedure pointer at the head of the list of
 *	functions which are used during a call to 'TclAccess(...)'. The
 *	passed function should be have exactly like 'TclAccess' when
 *	called during that time (see 'TclAccess(...)' for more informatin).
 *	The function will be added even if it already in the list.
 *
 * Results:
 *      Normally TCL_OK; TCL_ERROR if memory for a new node in the list
 *	could not be allocated.
 *
 * Side effects:
 *      Memory allocated and modifies the link list for 'TclAccess'
 *	functions.
 *
 *----------------------------------------------------------------------
 */

int
TclAccessInsertProc(proc)
    TclAccessProc_ *proc;
{
    int retVal = TCL_ERROR;

    if (proc != NULL) {
	AccessProc *newAccessProcPtr;

	newAccessProcPtr = (AccessProc *)ckalloc(sizeof(AccessProc));

	if (newAccessProcPtr != NULL) {
	    newAccessProcPtr->proc = proc;
	    Tcl_MutexLock(&obsoleteFsHookMutex);
	    newAccessProcPtr->nextPtr = accessProcList;
	    accessProcList = newAccessProcPtr;
	    Tcl_MutexUnlock(&obsoleteFsHookMutex);

	    retVal = TCL_OK;
	}
    }

    return (retVal);
}

/*
 *----------------------------------------------------------------------
 *
 * TclAccessDeleteProc --
 *
 *	Removed the passed function pointer from the list of 'TclAccess'
 *	functions.  Ensures that the built-in access function is not
 *	removvable.
 *
 * Results:
 *      TCL_OK if the procedure pointer was successfully removed,
 *	TCL_ERROR otherwise.
 *
 * Side effects:
 *      Memory is deallocated and the respective list updated.
 *
 *----------------------------------------------------------------------
 */

int
TclAccessDeleteProc(proc)
    TclAccessProc_ *proc;
{
    int retVal = TCL_ERROR;
    AccessProc *tmpAccessProcPtr;
    AccessProc *prevAccessProcPtr = NULL;

    /*
     * Traverse the 'accessProcList' looking for the particular node
     * whose 'proc' member matches 'proc' and remove that one from
     * the list.  Ensure that the "default" node cannot be removed.
     */

    Tcl_MutexLock(&obsoleteFsHookMutex);
    tmpAccessProcPtr = accessProcList;
    while ((retVal == TCL_ERROR) && (tmpAccessProcPtr != NULL)) {
	if (tmpAccessProcPtr->proc == proc) {
	    if (prevAccessProcPtr == NULL) {
		accessProcList = tmpAccessProcPtr->nextPtr;
	    } else {
		prevAccessProcPtr->nextPtr = tmpAccessProcPtr->nextPtr;
	    }

	    ckfree((char *)tmpAccessProcPtr);

	    retVal = TCL_OK;
	} else {
	    prevAccessProcPtr = tmpAccessProcPtr;
	    tmpAccessProcPtr = tmpAccessProcPtr->nextPtr;
	}
    }
    Tcl_MutexUnlock(&obsoleteFsHookMutex);

    return (retVal);
}

/*
 *----------------------------------------------------------------------
 *
 * TclOpenFileChannelInsertProc --
 *
 *	Insert the passed procedure pointer at the head of the list of
 *	functions which are used during a call to
 *	'Tcl_OpenFileChannel(...)'. The passed function should be have
 *	exactly like 'Tcl_OpenFileChannel' when called during that time
 *	(see 'Tcl_OpenFileChannel(...)' for more informatin). The
 *	function will be added even if it already in the list.
 *
 * Results:
 *      Normally TCL_OK; TCL_ERROR if memory for a new node in the list
 *	could not be allocated.
 *
 * Side effects:
 *      Memory allocataed and modifies the link list for
 *	'Tcl_OpenFileChannel' functions.
 *
 *----------------------------------------------------------------------
 */

int
TclOpenFileChannelInsertProc(proc)
    TclOpenFileChannelProc_ *proc;
{
    int retVal = TCL_ERROR;

    if (proc != NULL) {
	OpenFileChannelProc *newOpenFileChannelProcPtr;

	newOpenFileChannelProcPtr =
		(OpenFileChannelProc *)ckalloc(sizeof(OpenFileChannelProc));

	if (newOpenFileChannelProcPtr != NULL) {
	    newOpenFileChannelProcPtr->proc = proc;
	    Tcl_MutexLock(&obsoleteFsHookMutex);
	    newOpenFileChannelProcPtr->nextPtr = openFileChannelProcList;
	    openFileChannelProcList = newOpenFileChannelProcPtr;
	    Tcl_MutexUnlock(&obsoleteFsHookMutex);

	    retVal = TCL_OK;
	}
    }

    return (retVal);
}

/*
 *----------------------------------------------------------------------
 *
 * TclOpenFileChannelDeleteProc --
 *
 *	Removed the passed function pointer from the list of
 *	'Tcl_OpenFileChannel' functions.  Ensures that the built-in
 *	open file channel function is not removable.
 *
 * Results:
 *      TCL_OK if the procedure pointer was successfully removed,
 *	TCL_ERROR otherwise.
 *
 * Side effects:
 *      Memory is deallocated and the respective list updated.
 *
 *----------------------------------------------------------------------
 */

int
TclOpenFileChannelDeleteProc(proc)
    TclOpenFileChannelProc_ *proc;
{
    int retVal = TCL_ERROR;
    OpenFileChannelProc *tmpOpenFileChannelProcPtr = openFileChannelProcList;
    OpenFileChannelProc *prevOpenFileChannelProcPtr = NULL;

    /*
     * Traverse the 'openFileChannelProcList' looking for the particular
     * node whose 'proc' member matches 'proc' and remove that one from
     * the list.  
     */

    Tcl_MutexLock(&obsoleteFsHookMutex);
    tmpOpenFileChannelProcPtr = openFileChannelProcList;
    while ((retVal == TCL_ERROR) &&
	    (tmpOpenFileChannelProcPtr != NULL)) {
	if (tmpOpenFileChannelProcPtr->proc == proc) {
	    if (prevOpenFileChannelProcPtr == NULL) {
		openFileChannelProcList = tmpOpenFileChannelProcPtr->nextPtr;
	    } else {
		prevOpenFileChannelProcPtr->nextPtr =
			tmpOpenFileChannelProcPtr->nextPtr;
	    }

	    ckfree((char *)tmpOpenFileChannelProcPtr);

	    retVal = TCL_OK;
	} else {
	    prevOpenFileChannelProcPtr = tmpOpenFileChannelProcPtr;
	    tmpOpenFileChannelProcPtr = tmpOpenFileChannelProcPtr->nextPtr;
	}
    }
    Tcl_MutexUnlock(&obsoleteFsHookMutex);

    return (retVal);
}
#endif /* USE_OBSOLETE_FS_HOOKS */
