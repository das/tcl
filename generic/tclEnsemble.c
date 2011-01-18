/*
 * tclEnsemble.c --
 *
 *	Contains support for ensembles (see TIP#112), which provide simple
 *	mechanism for creating composite commands on top of namespaces.
 *
 * Copyright (c) 2005-2010 Donal K. Fellows.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id$
 */

#include "tclInt.h"
#include "tclCompile.h"

/*
 * Declarations for functions local to this file:
 */

static inline int	EnsembleUnknownCallback(Tcl_Interp *interp,
			    EnsembleConfig *ensemblePtr, int objc,
			    Tcl_Obj *const objv[], Tcl_Obj **prefixObjPtr);
static int		NsEnsembleImplementationCmd(ClientData clientData,
			    Tcl_Interp *interp,int objc,Tcl_Obj *const objv[]);
static int		NsEnsembleImplementationCmdNR(ClientData clientData,
			    Tcl_Interp *interp,int objc,Tcl_Obj *const objv[]);
static void		BuildEnsembleConfig(EnsembleConfig *ensemblePtr);
static int		NsEnsembleStringOrder(const void *strPtr1,
			    const void *strPtr2);
static void		DeleteEnsembleConfig(ClientData clientData);
static void		MakeCachedEnsembleCommand(Tcl_Obj *objPtr,
			    EnsembleConfig *ensemblePtr,
			    const char *subcmdName, Tcl_Obj *prefixObjPtr);
static void		FreeEnsembleCmdRep(Tcl_Obj *objPtr);
static void		DupEnsembleCmdRep(Tcl_Obj *objPtr, Tcl_Obj *copyPtr);
static void		StringOfEnsembleCmdRep(Tcl_Obj *objPtr);

/*
 * The lists of subcommands and options for the [namespace ensemble] command.
 */

static const char *const ensembleSubcommands[] = {
    "configure", "create", "exists", NULL
};
enum EnsSubcmds {
    ENS_CONFIG, ENS_CREATE, ENS_EXISTS
};

static const char *const ensembleCreateOptions[] = {
    "-command", "-map", "-parameters", "-prefixes", "-subcommands",
    "-unknown", NULL
};
enum EnsCreateOpts {
    CRT_CMD, CRT_MAP, CRT_PARAM, CRT_PREFIX, CRT_SUBCMDS, CRT_UNKNOWN
};

static const char *const ensembleConfigOptions[] = {
    "-map", "-namespace", "-parameters", "-prefixes", "-subcommands",
    "-unknown", NULL
};
enum EnsConfigOpts {
    CONF_MAP, CONF_NAMESPACE, CONF_PARAM, CONF_PREFIX, CONF_SUBCMDS,
    CONF_UNKNOWN
};

/*
 * This structure defines a Tcl object type that contains a reference to an
 * ensemble subcommand (e.g. the "length" in [string length ab]). It is used
 * to cache the mapping between the subcommand itself and the real command
 * that implements it.
 */

const Tcl_ObjType tclEnsembleCmdType = {
    "ensembleCommand",		/* the type's name */
    FreeEnsembleCmdRep,		/* freeIntRepProc */
    DupEnsembleCmdRep,		/* dupIntRepProc */
    StringOfEnsembleCmdRep,	/* updateStringProc */
    NULL			/* setFromAnyProc */
};

/*
 *----------------------------------------------------------------------
 *
 * TclNamespaceEnsembleCmd --
 *
 *	Invoked to implement the "namespace ensemble" command that creates and
 *	manipulates ensembles built on top of namespaces. Handles the
 *	following syntax:
 *
 *	    namespace ensemble name ?dictionary?
 *
 * Results:
 *	Returns TCL_OK if successful, and TCL_ERROR if anything goes wrong.
 *
 * Side effects:
 *	Creates the ensemble for the namespace if one did not previously
 *	exist. Alternatively, alters the way that the ensemble's subcommand =>
 *	implementation prefix is configured.
 *
 *----------------------------------------------------------------------
 */

int
TclNamespaceEnsembleCmd(
    ClientData dummy,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    Tcl_Namespace *namespacePtr;
    Namespace *nsPtr = (Namespace *) TclGetCurrentNamespace(interp);
    Tcl_Command token;
    Tcl_DictSearch search;
    Tcl_Obj *listObj;
    int index, done;

    if (nsPtr == NULL || nsPtr->flags & NS_DYING) {
	if (!Tcl_InterpDeleted(interp)) {
	    Tcl_AppendResult(interp,
		    "tried to manipulate ensemble of deleted namespace",
		    NULL);
	}
	return TCL_ERROR;
    }

    if (objc < 3) {
	Tcl_WrongNumArgs(interp, 2, objv, "subcommand ?arg ...?");
	return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[2], ensembleSubcommands,
	    "subcommand", 0, &index) != TCL_OK) {
	return TCL_ERROR;
    }

    switch ((enum EnsSubcmds) index) {
    case ENS_CREATE: {
	const char *name;
	int len, allocatedMapFlag = 0;
	/*
	 * Defaults
	 */
	Tcl_Obj *subcmdObj = NULL;
	Tcl_Obj *mapObj = NULL;
	int permitPrefix = 1;
	Tcl_Obj *unknownObj = NULL;
	Tcl_Obj *paramObj = NULL;

	/*
	 * Check that we've got option-value pairs... [Bug 1558654]
	 */

	if ((objc & 1) == 0) {
	    Tcl_WrongNumArgs(interp, 3, objv, "?option value ...?");
	    return TCL_ERROR;
	}
	objv += 3;
	objc -= 3;

	/*
	 * Work out what name to use for the command to create. If supplied,
	 * it is either fully specified or relative to the current namespace.
	 * If not supplied, it is exactly the name of the current namespace.
	 */

	name = nsPtr->fullName;

	/*
	 * Parse the option list, applying type checks as we go. Note that we
	 * are not incrementing any reference counts in the objects at this
	 * stage, so the presence of an option multiple times won't cause any
	 * memory leaks.
	 */

	for (; objc>1 ; objc-=2,objv+=2) {
	    if (Tcl_GetIndexFromObj(interp, objv[0], ensembleCreateOptions,
		    "option", 0, &index) != TCL_OK) {
		if (allocatedMapFlag) {
		    Tcl_DecrRefCount(mapObj);
		}
		return TCL_ERROR;
	    }
	    switch ((enum EnsCreateOpts) index) {
	    case CRT_CMD:
		name = TclGetString(objv[1]);
		continue;
	    case CRT_SUBCMDS:
		if (TclListObjLength(interp, objv[1], &len) != TCL_OK) {
		    if (allocatedMapFlag) {
			Tcl_DecrRefCount(mapObj);
		    }
		    return TCL_ERROR;
		}
		subcmdObj = (len > 0 ? objv[1] : NULL);
		continue;
	    case CRT_PARAM:
		if (TclListObjLength(interp, objv[1], &len) != TCL_OK) {
		    if (allocatedMapFlag) {
			Tcl_DecrRefCount(mapObj);
		    }
		    return TCL_ERROR;
		}
		paramObj = (len > 0 ? objv[1] : NULL);
		continue;
	    case CRT_MAP: {
		Tcl_Obj *patchedDict = NULL, *subcmdWordsObj;

		/*
		 * Verify that the map is sensible.
		 */

		if (Tcl_DictObjFirst(interp, objv[1], &search,
			&subcmdWordsObj, &listObj, &done) != TCL_OK) {
		    if (allocatedMapFlag) {
			Tcl_DecrRefCount(mapObj);
		    }
		    return TCL_ERROR;
		}
		if (done) {
		    mapObj = NULL;
		    continue;
		}
		do {
		    Tcl_Obj **listv;
		    const char *cmd;

		    if (TclListObjGetElements(interp, listObj, &len,
			    &listv) != TCL_OK) {
			Tcl_DictObjDone(&search);
			if (patchedDict) {
			    Tcl_DecrRefCount(patchedDict);
			}
			if (allocatedMapFlag) {
			    Tcl_DecrRefCount(mapObj);
			}
			return TCL_ERROR;
		    }
		    if (len < 1) {
			Tcl_SetResult(interp,
				"ensemble subcommand implementations "
				"must be non-empty lists", TCL_STATIC);
			Tcl_DictObjDone(&search);
			if (patchedDict) {
			    Tcl_DecrRefCount(patchedDict);
			}
			if (allocatedMapFlag) {
			    Tcl_DecrRefCount(mapObj);
			}
			return TCL_ERROR;
		    }
		    cmd = TclGetString(listv[0]);
		    if (!(cmd[0] == ':' && cmd[1] == ':')) {
			Tcl_Obj *newList = Tcl_NewListObj(len, listv);
			Tcl_Obj *newCmd = Tcl_NewStringObj(nsPtr->fullName,-1);

			if (nsPtr->parentPtr) {
			    Tcl_AppendStringsToObj(newCmd, "::", NULL);
			}
			Tcl_AppendObjToObj(newCmd, listv[0]);
			Tcl_ListObjReplace(NULL, newList, 0, 1, 1, &newCmd);
			if (patchedDict == NULL) {
			    patchedDict = Tcl_DuplicateObj(objv[1]);
			}
			Tcl_DictObjPut(NULL, patchedDict, subcmdWordsObj,
				newList);
		    }
		    Tcl_DictObjNext(&search, &subcmdWordsObj,&listObj, &done);
		} while (!done);

		if (allocatedMapFlag) {
		    Tcl_DecrRefCount(mapObj);
		}
		mapObj = (patchedDict ? patchedDict : objv[1]);
		if (patchedDict) {
		    allocatedMapFlag = 1;
		}
		continue;
	    }
	    case CRT_PREFIX:
		if (Tcl_GetBooleanFromObj(interp, objv[1],
			&permitPrefix) != TCL_OK) {
		    if (allocatedMapFlag) {
			Tcl_DecrRefCount(mapObj);
		    }
		    return TCL_ERROR;
		}
		continue;
	    case CRT_UNKNOWN:
		if (TclListObjLength(interp, objv[1], &len) != TCL_OK) {
		    if (allocatedMapFlag) {
			Tcl_DecrRefCount(mapObj);
		    }
		    return TCL_ERROR;
		}
		unknownObj = (len > 0 ? objv[1] : NULL);
		continue;
	    }
	}

	/*
	 * Create the ensemble. Note that this might delete another ensemble
	 * linked to the same namespace, so we must be careful. However, we
	 * should be OK because we only link the namespace into the list once
	 * we've created it (and after any deletions have occurred.)
	 */

	token = Tcl_CreateEnsemble(interp, name, NULL,
		(permitPrefix ? TCL_ENSEMBLE_PREFIX : 0));
	Tcl_SetEnsembleSubcommandList(interp, token, subcmdObj);
	Tcl_SetEnsembleMappingDict(interp, token, mapObj);
	Tcl_SetEnsembleUnknownHandler(interp, token, unknownObj);
	Tcl_SetEnsembleParameterList(interp, token, paramObj);

	/*
	 * Tricky! Must ensure that the result is not shared (command delete
	 * traces could have corrupted the pristine object that we started
	 * with). [Snit test rename-1.5]
	 */

	Tcl_ResetResult(interp);
	Tcl_GetCommandFullName(interp, token, Tcl_GetObjResult(interp));
	return TCL_OK;
    }

    case ENS_EXISTS:
	if (objc != 4) {
	    Tcl_WrongNumArgs(interp, 3, objv, "cmdname");
	    return TCL_ERROR;
	}
	Tcl_SetObjResult(interp, Tcl_NewBooleanObj(
		Tcl_FindEnsemble(interp, objv[3], 0) != NULL));
	return TCL_OK;

    case ENS_CONFIG:
	if (objc < 4 || (objc != 5 && objc & 1)) {
	    Tcl_WrongNumArgs(interp, 3, objv,
		    "cmdname ?-option value ...? ?arg ...?");
	    return TCL_ERROR;
	}
	token = Tcl_FindEnsemble(interp, objv[3], TCL_LEAVE_ERR_MSG);
	if (token == NULL) {
	    return TCL_ERROR;
	}

	if (objc == 5) {
	    Tcl_Obj *resultObj = NULL;		/* silence gcc 4 warning */

	    if (Tcl_GetIndexFromObj(interp, objv[4], ensembleConfigOptions,
		    "option", 0, &index) != TCL_OK) {
		return TCL_ERROR;
	    }
	    switch ((enum EnsConfigOpts) index) {
	    case CONF_SUBCMDS:
		Tcl_GetEnsembleSubcommandList(NULL, token, &resultObj);
		if (resultObj != NULL) {
		    Tcl_SetObjResult(interp, resultObj);
		}
		break;
	    case CONF_PARAM:
		Tcl_GetEnsembleParameterList(NULL, token, &resultObj);
		if (resultObj != NULL) {
		    Tcl_SetObjResult(interp, resultObj);
		}
		break;
	    case CONF_MAP:
		Tcl_GetEnsembleMappingDict(NULL, token, &resultObj);
		if (resultObj != NULL) {
		    Tcl_SetObjResult(interp, resultObj);
		}
		break;
	    case CONF_NAMESPACE:
		namespacePtr = NULL;		/* silence gcc 4 warning */
		Tcl_GetEnsembleNamespace(NULL, token, &namespacePtr);
		Tcl_SetResult(interp, ((Namespace *) namespacePtr)->fullName,
			TCL_VOLATILE);
		break;
	    case CONF_PREFIX: {
		int flags = 0;			/* silence gcc 4 warning */

		Tcl_GetEnsembleFlags(NULL, token, &flags);
		Tcl_SetObjResult(interp,
			Tcl_NewBooleanObj(flags & TCL_ENSEMBLE_PREFIX));
		break;
	    }
	    case CONF_UNKNOWN:
		Tcl_GetEnsembleUnknownHandler(NULL, token, &resultObj);
		if (resultObj != NULL) {
		    Tcl_SetObjResult(interp, resultObj);
		}
		break;
	    }
	} else if (objc == 4) {
	    /*
	     * Produce list of all information.
	     */

	    Tcl_Obj *resultObj, *tmpObj = NULL;	/* silence gcc 4 warning */
	    int flags = 0;			/* silence gcc 4 warning */

	    TclNewObj(resultObj);

	    /* -map option */
	    Tcl_ListObjAppendElement(NULL, resultObj,
		    Tcl_NewStringObj(ensembleConfigOptions[CONF_MAP], -1));
	    Tcl_GetEnsembleMappingDict(NULL, token, &tmpObj);
	    Tcl_ListObjAppendElement(NULL, resultObj,
		    (tmpObj != NULL) ? tmpObj : Tcl_NewObj());

	    /* -namespace option */
	    Tcl_ListObjAppendElement(NULL, resultObj,
		    Tcl_NewStringObj(ensembleConfigOptions[CONF_NAMESPACE],
			    -1));
	    namespacePtr = NULL;		/* silence gcc 4 warning */
	    Tcl_GetEnsembleNamespace(NULL, token, &namespacePtr);
	    Tcl_ListObjAppendElement(NULL, resultObj,
		    Tcl_NewStringObj(((Namespace *) namespacePtr)->fullName,
			    -1));

	    /* -parameters option */
	    Tcl_ListObjAppendElement(NULL, resultObj,
		    Tcl_NewStringObj(ensembleConfigOptions[CONF_PARAM], -1));
	    Tcl_GetEnsembleParameterList(NULL, token, &tmpObj);
	    Tcl_ListObjAppendElement(NULL, resultObj,
		    (tmpObj != NULL) ? tmpObj : Tcl_NewObj());

	    /* -prefix option */
	    Tcl_ListObjAppendElement(NULL, resultObj,
		    Tcl_NewStringObj(ensembleConfigOptions[CONF_PREFIX], -1));
	    Tcl_GetEnsembleFlags(NULL, token, &flags);
	    Tcl_ListObjAppendElement(NULL, resultObj,
		    Tcl_NewBooleanObj(flags & TCL_ENSEMBLE_PREFIX));

	    /* -subcommands option */
	    Tcl_ListObjAppendElement(NULL, resultObj,
		    Tcl_NewStringObj(ensembleConfigOptions[CONF_SUBCMDS],-1));
	    Tcl_GetEnsembleSubcommandList(NULL, token, &tmpObj);
	    Tcl_ListObjAppendElement(NULL, resultObj,
		    (tmpObj != NULL) ? tmpObj : Tcl_NewObj());

	    /* -unknown option */
	    Tcl_ListObjAppendElement(NULL, resultObj,
		    Tcl_NewStringObj(ensembleConfigOptions[CONF_UNKNOWN],-1));
	    Tcl_GetEnsembleUnknownHandler(NULL, token, &tmpObj);
	    Tcl_ListObjAppendElement(NULL, resultObj,
		    (tmpObj != NULL) ? tmpObj : Tcl_NewObj());

	    Tcl_SetObjResult(interp, resultObj);
	} else {
	    int len, allocatedMapFlag = 0;
	    Tcl_Obj *subcmdObj = NULL, *mapObj = NULL, *paramObj = NULL,
		    *unknownObj = NULL; /* Defaults, silence gcc 4 warnings */
	    int permitPrefix, flags = 0;	/* silence gcc 4 warning */

	    Tcl_GetEnsembleSubcommandList(NULL, token, &subcmdObj);
	    Tcl_GetEnsembleMappingDict(NULL, token, &mapObj);
	    Tcl_GetEnsembleParameterList(NULL, token, &paramObj);
	    Tcl_GetEnsembleUnknownHandler(NULL, token, &unknownObj);
	    Tcl_GetEnsembleFlags(NULL, token, &flags);
	    permitPrefix = (flags & TCL_ENSEMBLE_PREFIX) != 0;

	    objv += 4;
	    objc -= 4;

	    /*
	     * Parse the option list, applying type checks as we go. Note that
	     * we are not incrementing any reference counts in the objects at
	     * this stage, so the presence of an option multiple times won't
	     * cause any memory leaks.
	     */

	    for (; objc>0 ; objc-=2,objv+=2) {
		if (Tcl_GetIndexFromObj(interp, objv[0],ensembleConfigOptions,
			"option", 0, &index) != TCL_OK) {
		freeMapAndError:
		    if (allocatedMapFlag) {
			Tcl_DecrRefCount(mapObj);
		    }
		    return TCL_ERROR;
		}
		switch ((enum EnsConfigOpts) index) {
		case CONF_SUBCMDS:
		    if (TclListObjLength(interp, objv[1], &len) != TCL_OK) {
			goto freeMapAndError;
		    }
		    subcmdObj = (len > 0 ? objv[1] : NULL);
		    continue;
		case CONF_PARAM:
		    if (TclListObjLength(interp, objv[1], &len) != TCL_OK) {
			goto freeMapAndError;
		    }
		    paramObj = (len > 0 ? objv[1] : NULL);
		    continue;
		case CONF_MAP: {
		    Tcl_Obj *patchedDict = NULL, *subcmdWordsObj, **listv;
		    const char *cmd;

		    /*
		     * Verify that the map is sensible.
		     */

		    if (Tcl_DictObjFirst(interp, objv[1], &search,
			    &subcmdWordsObj, &listObj, &done) != TCL_OK) {
			goto freeMapAndError;
		    }
		    if (done) {
			mapObj = NULL;
			continue;
		    }
		    do {
			if (TclListObjGetElements(interp, listObj, &len,
				&listv) != TCL_OK) {
			    Tcl_DictObjDone(&search);
			    if (patchedDict) {
				Tcl_DecrRefCount(patchedDict);
			    }
			    goto freeMapAndError;
			}
			if (len < 1) {
			    Tcl_SetResult(interp,
				    "ensemble subcommand implementations "
				    "must be non-empty lists", TCL_STATIC);
			    Tcl_DictObjDone(&search);
			    if (patchedDict) {
				Tcl_DecrRefCount(patchedDict);
			    }
			    goto freeMapAndError;
			}
			cmd = TclGetString(listv[0]);
			if (!(cmd[0] == ':' && cmd[1] == ':')) {
			    Tcl_Obj *newList = Tcl_DuplicateObj(listObj);
			    Tcl_Obj *newCmd =
				    Tcl_NewStringObj(nsPtr->fullName, -1);

			    if (nsPtr->parentPtr) {
				Tcl_AppendStringsToObj(newCmd, "::", NULL);
			    }
			    Tcl_AppendObjToObj(newCmd, listv[0]);
			    Tcl_ListObjReplace(NULL, newList, 0,1, 1,&newCmd);
			    if (patchedDict == NULL) {
				patchedDict = Tcl_DuplicateObj(objv[1]);
			    }
			    Tcl_DictObjPut(NULL, patchedDict, subcmdWordsObj,
				    newList);
			}
			Tcl_DictObjNext(&search, &subcmdWordsObj, &listObj,
				&done);
		    } while (!done);
		    if (allocatedMapFlag) {
			Tcl_DecrRefCount(mapObj);
		    }
		    mapObj = (patchedDict ? patchedDict : objv[1]);
		    if (patchedDict) {
			allocatedMapFlag = 1;
		    }
		    continue;
		}
		case CONF_NAMESPACE:
		    Tcl_AppendResult(interp, "option -namespace is read-only",
			    NULL);
		    goto freeMapAndError;
		case CONF_PREFIX:
		    if (Tcl_GetBooleanFromObj(interp, objv[1],
			    &permitPrefix) != TCL_OK) {
			goto freeMapAndError;
		    }
		    continue;
		case CONF_UNKNOWN:
		    if (TclListObjLength(interp, objv[1], &len) != TCL_OK) {
			goto freeMapAndError;
		    }
		    unknownObj = (len > 0 ? objv[1] : NULL);
		    continue;
		}
	    }

	    /*
	     * Update the namespace now that we've finished the parsing stage.
	     */

	    flags = (permitPrefix ? flags|TCL_ENSEMBLE_PREFIX
		    : flags&~TCL_ENSEMBLE_PREFIX);
	    Tcl_SetEnsembleSubcommandList(interp, token, subcmdObj);
	    Tcl_SetEnsembleMappingDict(interp, token, mapObj);
	    Tcl_SetEnsembleParameterList(interp, token, paramObj);
	    Tcl_SetEnsembleUnknownHandler(interp, token, unknownObj);
	    Tcl_SetEnsembleFlags(interp, token, flags);
	}
	return TCL_OK;

    default:
	Tcl_Panic("unexpected ensemble command");
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_CreateEnsemble --
 *
 *	Create a simple ensemble attached to the given namespace.
 *
 * Results:
 *	The token for the command created.
 *
 * Side effects:
 *	The ensemble is created and marked for compilation.
 *
 *----------------------------------------------------------------------
 */

Tcl_Command
Tcl_CreateEnsemble(
    Tcl_Interp *interp,
    const char *name,
    Tcl_Namespace *namespacePtr,
    int flags)
{
    Namespace *nsPtr = (Namespace *) namespacePtr;
    EnsembleConfig *ensemblePtr = (EnsembleConfig *)
	    ckalloc(sizeof(EnsembleConfig));
    Tcl_Obj *nameObj = NULL;

    if (nsPtr == NULL) {
	nsPtr = (Namespace *) TclGetCurrentNamespace(interp);
    }

    /*
     * Make the name of the ensemble into a fully qualified name. This might
     * allocate a temporary object.
     */

    if (!(name[0] == ':' && name[1] == ':')) {
	nameObj = Tcl_NewStringObj(nsPtr->fullName, -1);
	if (nsPtr->parentPtr == NULL) {
	    Tcl_AppendStringsToObj(nameObj, name, NULL);
	} else {
	    Tcl_AppendStringsToObj(nameObj, "::", name, NULL);
	}
	Tcl_IncrRefCount(nameObj);
	name = TclGetString(nameObj);
    }

    ensemblePtr->nsPtr = nsPtr;
    ensemblePtr->epoch = 0;
    Tcl_InitHashTable(&ensemblePtr->subcommandTable, TCL_STRING_KEYS);
    ensemblePtr->subcommandArrayPtr = NULL;
    ensemblePtr->subcmdList = NULL;
    ensemblePtr->subcommandDict = NULL;
    ensemblePtr->flags = flags;
    ensemblePtr->numParameters = 0;
    ensemblePtr->parameterList = NULL;
    ensemblePtr->unknownHandler = NULL;
    ensemblePtr->token = Tcl_NRCreateCommand(interp, name,
	    NsEnsembleImplementationCmd, NsEnsembleImplementationCmdNR,
	    ensemblePtr, DeleteEnsembleConfig);
    ensemblePtr->next = (EnsembleConfig *) nsPtr->ensembles;
    nsPtr->ensembles = (Tcl_Ensemble *) ensemblePtr;

    /*
     * Trigger an eventual recomputation of the ensemble command set. Note
     * that this is slightly tricky, as it means that we are not actually
     * counting the number of namespace export actions, but it is the simplest
     * way to go!
     */

    nsPtr->exportLookupEpoch++;

    if (flags & ENSEMBLE_COMPILE) {
	((Command *) ensemblePtr->token)->compileProc = TclCompileEnsemble;
    }

    if (nameObj != NULL) {
	TclDecrRefCount(nameObj);
    }
    return ensemblePtr->token;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetEnsembleSubcommandList --
 *
 *	Set the subcommand list for a particular ensemble.
 *
 * Results:
 *	Tcl result code (error if command token does not indicate an ensemble
 *	or the subcommand list - if non-NULL - is not a list).
 *
 * Side effects:
 *	The ensemble is updated and marked for recompilation.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_SetEnsembleSubcommandList(
    Tcl_Interp *interp,
    Tcl_Command token,
    Tcl_Obj *subcmdList)
{
    Command *cmdPtr = (Command *) token;
    EnsembleConfig *ensemblePtr;
    Tcl_Obj *oldList;

    if (cmdPtr->objProc != NsEnsembleImplementationCmd) {
	Tcl_AppendResult(interp, "command is not an ensemble", NULL);
	return TCL_ERROR;
    }
    if (subcmdList != NULL) {
	int length;

	if (TclListObjLength(interp, subcmdList, &length) != TCL_OK) {
	    return TCL_ERROR;
	}
	if (length < 1) {
	    subcmdList = NULL;
	}
    }

    ensemblePtr = cmdPtr->objClientData;
    oldList = ensemblePtr->subcmdList;
    ensemblePtr->subcmdList = subcmdList;
    if (subcmdList != NULL) {
	Tcl_IncrRefCount(subcmdList);
    }
    if (oldList != NULL) {
	TclDecrRefCount(oldList);
    }

    /*
     * Trigger an eventual recomputation of the ensemble command set. Note
     * that this is slightly tricky, as it means that we are not actually
     * counting the number of namespace export actions, but it is the simplest
     * way to go!
     */

    ensemblePtr->nsPtr->exportLookupEpoch++;

    /*
     * Special hack to make compiling of [info exists] work when the
     * dictionary is modified.
     */

    if (cmdPtr->compileProc != NULL) {
	((Interp *) interp)->compileEpoch++;
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetEnsembleParameterList --
 *
 *	Set the parameter list for a particular ensemble.
 *
 * Results:
 *	Tcl result code (error if command token does not indicate an ensemble
 *	or the parameter list - if non-NULL - is not a list).
 *
 * Side effects:
 *	The ensemble is updated and marked for recompilation.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_SetEnsembleParameterList(
    Tcl_Interp *interp,
    Tcl_Command token,
    Tcl_Obj *paramList)
{
    Command *cmdPtr = (Command *) token;
    EnsembleConfig *ensemblePtr;
    Tcl_Obj *oldList;
    int length;

    if (cmdPtr->objProc != NsEnsembleImplementationCmd) {
	Tcl_AppendResult(interp, "command is not an ensemble", NULL);
	return TCL_ERROR;
    }
    if (paramList == NULL) {
	length = 0;
    } else {
	if (TclListObjLength(interp, paramList, &length) != TCL_OK) {
	    return TCL_ERROR;
	}
	if (length < 1) {
	    paramList = NULL;
	}
    }

    ensemblePtr = cmdPtr->objClientData;
    oldList = ensemblePtr->parameterList;
    ensemblePtr->parameterList = paramList;
    if (paramList != NULL) {
	Tcl_IncrRefCount(paramList);
    }
    if (oldList != NULL) {
	TclDecrRefCount(oldList);
    }
    ensemblePtr->numParameters = length;

    /*
     * Trigger an eventual recomputation of the ensemble command set. Note
     * that this is slightly tricky, as it means that we are not actually
     * counting the number of namespace export actions, but it is the simplest
     * way to go!
     */

    ensemblePtr->nsPtr->exportLookupEpoch++;

    /*
     * Special hack to make compiling of [info exists] work when the
     * dictionary is modified.
     */

    if (cmdPtr->compileProc != NULL) {
	((Interp *) interp)->compileEpoch++;
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetEnsembleMappingDict --
 *
 *	Set the mapping dictionary for a particular ensemble.
 *
 * Results:
 *	Tcl result code (error if command token does not indicate an ensemble
 *	or the mapping - if non-NULL - is not a dict).
 *
 * Side effects:
 *	The ensemble is updated and marked for recompilation.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_SetEnsembleMappingDict(
    Tcl_Interp *interp,
    Tcl_Command token,
    Tcl_Obj *mapDict)
{
    Command *cmdPtr = (Command *) token;
    EnsembleConfig *ensemblePtr;
    Tcl_Obj *oldDict;

    if (cmdPtr->objProc != NsEnsembleImplementationCmd) {
	Tcl_AppendResult(interp, "command is not an ensemble", NULL);
	return TCL_ERROR;
    }
    if (mapDict != NULL) {
	int size, done;
	Tcl_DictSearch search;
	Tcl_Obj *valuePtr;

	if (Tcl_DictObjSize(interp, mapDict, &size) != TCL_OK) {
	    return TCL_ERROR;
	}

	for (Tcl_DictObjFirst(NULL, mapDict, &search, NULL, &valuePtr, &done);
		!done; Tcl_DictObjNext(&search, NULL, &valuePtr, &done)) {
	    Tcl_Obj *cmdObjPtr;
	    const char *bytes;

	    if (Tcl_ListObjIndex(interp, valuePtr, 0, &cmdObjPtr) != TCL_OK) {
		Tcl_DictObjDone(&search);
		return TCL_ERROR;
	    }
	    bytes = TclGetString(cmdObjPtr);
	    if (bytes[0] != ':' || bytes[1] != ':') {
		Tcl_AppendResult(interp,
			"ensemble target is not a fully-qualified command",
			NULL);
		Tcl_DictObjDone(&search);
		return TCL_ERROR;
	    }
	}

	if (size < 1) {
	    mapDict = NULL;
	}
    }

    ensemblePtr = cmdPtr->objClientData;
    oldDict = ensemblePtr->subcommandDict;
    ensemblePtr->subcommandDict = mapDict;
    if (mapDict != NULL) {
	Tcl_IncrRefCount(mapDict);
    }
    if (oldDict != NULL) {
	TclDecrRefCount(oldDict);
    }

    /*
     * Trigger an eventual recomputation of the ensemble command set. Note
     * that this is slightly tricky, as it means that we are not actually
     * counting the number of namespace export actions, but it is the simplest
     * way to go!
     */

    ensemblePtr->nsPtr->exportLookupEpoch++;

    /*
     * Special hack to make compiling of [info exists] work when the
     * dictionary is modified.
     */

    if (cmdPtr->compileProc != NULL) {
	((Interp *) interp)->compileEpoch++;
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetEnsembleUnknownHandler --
 *
 *	Set the unknown handler for a particular ensemble.
 *
 * Results:
 *	Tcl result code (error if command token does not indicate an ensemble
 *	or the unknown handler - if non-NULL - is not a list).
 *
 * Side effects:
 *	The ensemble is updated and marked for recompilation.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_SetEnsembleUnknownHandler(
    Tcl_Interp *interp,
    Tcl_Command token,
    Tcl_Obj *unknownList)
{
    Command *cmdPtr = (Command *) token;
    EnsembleConfig *ensemblePtr;
    Tcl_Obj *oldList;

    if (cmdPtr->objProc != NsEnsembleImplementationCmd) {
	Tcl_AppendResult(interp, "command is not an ensemble", NULL);
	return TCL_ERROR;
    }
    if (unknownList != NULL) {
	int length;

	if (TclListObjLength(interp, unknownList, &length) != TCL_OK) {
	    return TCL_ERROR;
	}
	if (length < 1) {
	    unknownList = NULL;
	}
    }

    ensemblePtr = cmdPtr->objClientData;
    oldList = ensemblePtr->unknownHandler;
    ensemblePtr->unknownHandler = unknownList;
    if (unknownList != NULL) {
	Tcl_IncrRefCount(unknownList);
    }
    if (oldList != NULL) {
	TclDecrRefCount(oldList);
    }

    /*
     * Trigger an eventual recomputation of the ensemble command set. Note
     * that this is slightly tricky, as it means that we are not actually
     * counting the number of namespace export actions, but it is the simplest
     * way to go!
     */

    ensemblePtr->nsPtr->exportLookupEpoch++;

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_SetEnsembleFlags --
 *
 *	Set the flags for a particular ensemble.
 *
 * Results:
 *	Tcl result code (error if command token does not indicate an
 *	ensemble).
 *
 * Side effects:
 *	The ensemble is updated and marked for recompilation.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_SetEnsembleFlags(
    Tcl_Interp *interp,
    Tcl_Command token,
    int flags)
{
    Command *cmdPtr = (Command *) token;
    EnsembleConfig *ensemblePtr;
    int wasCompiled;

    if (cmdPtr->objProc != NsEnsembleImplementationCmd) {
	Tcl_AppendResult(interp, "command is not an ensemble", NULL);
	return TCL_ERROR;
    }

    ensemblePtr = cmdPtr->objClientData;
    wasCompiled = ensemblePtr->flags & ENSEMBLE_COMPILE;

    /*
     * This API refuses to set the ENSEMBLE_DEAD flag...
     */

    ensemblePtr->flags &= ENSEMBLE_DEAD;
    ensemblePtr->flags |= flags & ~ENSEMBLE_DEAD;

    /*
     * Trigger an eventual recomputation of the ensemble command set. Note
     * that this is slightly tricky, as it means that we are not actually
     * counting the number of namespace export actions, but it is the simplest
     * way to go!
     */

    ensemblePtr->nsPtr->exportLookupEpoch++;

    /*
     * If the ENSEMBLE_COMPILE flag status was changed, install or remove the
     * compiler function and bump the interpreter's compilation epoch so that
     * bytecode gets regenerated.
     */

    if (flags & ENSEMBLE_COMPILE) {
	if (!wasCompiled) {
	    ((Command*) ensemblePtr->token)->compileProc = TclCompileEnsemble;
	    ((Interp *) interp)->compileEpoch++;
	}
    } else {
	if (wasCompiled) {
	    ((Command *) ensemblePtr->token)->compileProc = NULL;
	    ((Interp *) interp)->compileEpoch++;
	}
    }

    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetEnsembleSubcommandList --
 *
 *	Get the list of subcommands associated with a particular ensemble.
 *
 * Results:
 *	Tcl result code (error if command token does not indicate an
 *	ensemble). The list of subcommands is returned by updating the
 *	variable pointed to by the last parameter (NULL if this is to be
 *	derived from the mapping dictionary or the associated namespace's
 *	exported commands).
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

int
Tcl_GetEnsembleSubcommandList(
    Tcl_Interp *interp,
    Tcl_Command token,
    Tcl_Obj **subcmdListPtr)
{
    Command *cmdPtr = (Command *) token;
    EnsembleConfig *ensemblePtr;

    if (cmdPtr->objProc != NsEnsembleImplementationCmd) {
	if (interp != NULL) {
	    Tcl_AppendResult(interp, "command is not an ensemble", NULL);
	}
	return TCL_ERROR;
    }

    ensemblePtr = cmdPtr->objClientData;
    *subcmdListPtr = ensemblePtr->subcmdList;
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetEnsembleParameterList --
 *
 *	Get the list of parameters associated with a particular ensemble.
 *
 * Results:
 *	Tcl result code (error if command token does not indicate an
 *	ensemble). The list of parameters is returned by updating the
 *	variable pointed to by the last parameter (NULL if there are
 *	no parameters).
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

int
Tcl_GetEnsembleParameterList(
    Tcl_Interp *interp,
    Tcl_Command token,
    Tcl_Obj **paramListPtr)
{
    Command *cmdPtr = (Command *) token;
    EnsembleConfig *ensemblePtr;

    if (cmdPtr->objProc != NsEnsembleImplementationCmd) {
	if (interp != NULL) {
	    Tcl_AppendResult(interp, "command is not an ensemble", NULL);
	}
	return TCL_ERROR;
    }

    ensemblePtr = cmdPtr->objClientData;
    *paramListPtr = ensemblePtr->parameterList;
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetEnsembleMappingDict --
 *
 *	Get the command mapping dictionary associated with a particular
 *	ensemble.
 *
 * Results:
 *	Tcl result code (error if command token does not indicate an
 *	ensemble). The mapping dict is returned by updating the variable
 *	pointed to by the last parameter (NULL if none is installed).
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

int
Tcl_GetEnsembleMappingDict(
    Tcl_Interp *interp,
    Tcl_Command token,
    Tcl_Obj **mapDictPtr)
{
    Command *cmdPtr = (Command *) token;
    EnsembleConfig *ensemblePtr;

    if (cmdPtr->objProc != NsEnsembleImplementationCmd) {
	if (interp != NULL) {
	    Tcl_AppendResult(interp, "command is not an ensemble", NULL);
	}
	return TCL_ERROR;
    }

    ensemblePtr = cmdPtr->objClientData;
    *mapDictPtr = ensemblePtr->subcommandDict;
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetEnsembleUnknownHandler --
 *
 *	Get the unknown handler associated with a particular ensemble.
 *
 * Results:
 *	Tcl result code (error if command token does not indicate an
 *	ensemble). The unknown handler is returned by updating the variable
 *	pointed to by the last parameter (NULL if no handler is installed).
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

int
Tcl_GetEnsembleUnknownHandler(
    Tcl_Interp *interp,
    Tcl_Command token,
    Tcl_Obj **unknownListPtr)
{
    Command *cmdPtr = (Command *) token;
    EnsembleConfig *ensemblePtr;

    if (cmdPtr->objProc != NsEnsembleImplementationCmd) {
	if (interp != NULL) {
	    Tcl_AppendResult(interp, "command is not an ensemble", NULL);
	}
	return TCL_ERROR;
    }

    ensemblePtr = cmdPtr->objClientData;
    *unknownListPtr = ensemblePtr->unknownHandler;
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetEnsembleFlags --
 *
 *	Get the flags for a particular ensemble.
 *
 * Results:
 *	Tcl result code (error if command token does not indicate an
 *	ensemble). The flags are returned by updating the variable pointed to
 *	by the last parameter.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

int
Tcl_GetEnsembleFlags(
    Tcl_Interp *interp,
    Tcl_Command token,
    int *flagsPtr)
{
    Command *cmdPtr = (Command *) token;
    EnsembleConfig *ensemblePtr;

    if (cmdPtr->objProc != NsEnsembleImplementationCmd) {
	if (interp != NULL) {
	    Tcl_AppendResult(interp, "command is not an ensemble", NULL);
	}
	return TCL_ERROR;
    }

    ensemblePtr = cmdPtr->objClientData;
    *flagsPtr = ensemblePtr->flags;
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetEnsembleNamespace --
 *
 *	Get the namespace associated with a particular ensemble.
 *
 * Results:
 *	Tcl result code (error if command token does not indicate an
 *	ensemble). Namespace is returned by updating the variable pointed to
 *	by the last parameter.
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

int
Tcl_GetEnsembleNamespace(
    Tcl_Interp *interp,
    Tcl_Command token,
    Tcl_Namespace **namespacePtrPtr)
{
    Command *cmdPtr = (Command *) token;
    EnsembleConfig *ensemblePtr;

    if (cmdPtr->objProc != NsEnsembleImplementationCmd) {
	if (interp != NULL) {
	    Tcl_AppendResult(interp, "command is not an ensemble", NULL);
	}
	return TCL_ERROR;
    }

    ensemblePtr = cmdPtr->objClientData;
    *namespacePtrPtr = (Tcl_Namespace *) ensemblePtr->nsPtr;
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FindEnsemble --
 *
 *	Given a command name, get the ensemble token for it, allowing for
 *	[namespace import]s. [Bug 1017022]
 *
 * Results:
 *	The token for the ensemble command with the given name, or NULL if the
 *	command either does not exist or is not an ensemble (when an error
 *	message will be written into the interp if thats non-NULL).
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

Tcl_Command
Tcl_FindEnsemble(
    Tcl_Interp *interp,		/* Where to do the lookup, and where to write
				 * the errors if TCL_LEAVE_ERR_MSG is set in
				 * the flags. */
    Tcl_Obj *cmdNameObj,	/* Name of command to look up. */
    int flags)			/* Either 0 or TCL_LEAVE_ERR_MSG; other flags
				 * are probably not useful. */
{
    Command *cmdPtr;

    cmdPtr = (Command *)
	    Tcl_FindCommand(interp, TclGetString(cmdNameObj), NULL, flags);
    if (cmdPtr == NULL) {
	return NULL;
    }

    if (cmdPtr->objProc != NsEnsembleImplementationCmd) {
	/*
	 * Reuse existing infrastructure for following import link chains
	 * rather than duplicating it.
	 */

	cmdPtr = (Command *) TclGetOriginalCommand((Tcl_Command) cmdPtr);

	if (cmdPtr == NULL || cmdPtr->objProc != NsEnsembleImplementationCmd){
	    if (flags & TCL_LEAVE_ERR_MSG) {
		Tcl_AppendResult(interp, "\"", TclGetString(cmdNameObj),
			"\" is not an ensemble command", NULL);
		Tcl_SetErrorCode(interp, "TCL", "LOOKUP", "ENSEMBLE",
			TclGetString(cmdNameObj), NULL);
	    }
	    return NULL;
	}
    }

    return (Tcl_Command) cmdPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_IsEnsemble --
 *
 *	Simple test for ensemble-hood that takes into account imported
 *	ensemble commands as well.
 *
 * Results:
 *	Boolean value
 *
 * Side effects:
 *	None
 *
 *----------------------------------------------------------------------
 */

int
Tcl_IsEnsemble(
    Tcl_Command token)
{
    Command *cmdPtr = (Command *) token;

    if (cmdPtr->objProc == NsEnsembleImplementationCmd) {
	return 1;
    }
    cmdPtr = (Command *) TclGetOriginalCommand((Tcl_Command) cmdPtr);
    if (cmdPtr == NULL || cmdPtr->objProc != NsEnsembleImplementationCmd) {
	return 0;
    }
    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * TclMakeEnsemble --
 *
 *	Create an ensemble from a table of implementation commands. The
 *	ensemble will be subject to (limited) compilation if any of the
 *	implementation commands are compilable.
 *
 *	The 'name' parameter may be a single command name or a list if
 *	creating an ensemble subcommand (see the binary implementation).
 *
 *	Currently, the TCL_ENSEMBLE_PREFIX ensemble flag is only used on
 *	top-level ensemble commands.
 *
 * Results:
 *	Handle for the new ensemble, or NULL on failure.
 *
 * Side effects:
 *	May advance the bytecode compilation epoch.
 *
 *----------------------------------------------------------------------
 */

Tcl_Command
TclMakeEnsemble(
    Tcl_Interp *interp,
    const char *name,		 /* The ensemble name (as explained above) */
    const EnsembleImplMap map[]) /* The subcommands to create */
{
    Tcl_Command ensemble;
    Tcl_Namespace *ns;
    Tcl_DString buf, hiddenBuf;
    const char **nameParts = NULL;
    const char *cmdName = NULL;
    int i, nameCount = 0, ensembleFlags = 0, hiddenLen;

    /*
     * Construct the path for the ensemble namespace and create it.
     */

    Tcl_DStringInit(&buf);
    Tcl_DStringInit(&hiddenBuf);
    Tcl_DStringAppend(&hiddenBuf, "tcl:", -1);
    Tcl_DStringAppend(&hiddenBuf, name, -1);
    Tcl_DStringAppend(&hiddenBuf, ":", -1);
    hiddenLen = Tcl_DStringLength(&hiddenBuf);
    if (name[0] == ':' && name[1] == ':') {
	/*
	 * An absolute name, so use it directly.
	 */

	cmdName = name;
	Tcl_DStringAppend(&buf, name, -1);
	ensembleFlags = TCL_ENSEMBLE_PREFIX;
    } else {
	/*
	 * Not an absolute name, so do munging of it. Note that this treats a
	 * multi-word list differently to a single word.
	 */

	Tcl_DStringAppend(&buf, "::tcl", -1);

	if (Tcl_SplitList(NULL, name, &nameCount, &nameParts) != TCL_OK) {
	    Tcl_Panic("invalid ensemble name '%s'", name);
	}

	for (i = 0; i < nameCount; ++i) {
	    Tcl_DStringAppend(&buf, "::", 2);
	    Tcl_DStringAppend(&buf, nameParts[i], -1);
	}
    }

    ns = Tcl_FindNamespace(interp, Tcl_DStringValue(&buf), NULL,
	    TCL_CREATE_NS_IF_UNKNOWN);
    if (!ns) {
	Tcl_Panic("unable to find or create %s namespace!",
		Tcl_DStringValue(&buf));
    }

    /*
     * Create the named ensemble in the correct namespace
     */

    if (cmdName == NULL) {
	if (nameCount == 1) {
	    ensembleFlags = TCL_ENSEMBLE_PREFIX;
	    cmdName = Tcl_DStringValue(&buf) + 5;
	} else {
	    ns = ns->parentPtr;
	    cmdName = nameParts[nameCount - 1];
	}
    }
    ensemble = Tcl_CreateEnsemble(interp, cmdName, ns, ensembleFlags);

    /*
     * Create the ensemble mapping dictionary and the ensemble command procs.
     */

    if (ensemble != NULL) {
	Tcl_Obj *mapDict, *fromObj, *toObj;
	Command *cmdPtr;

	Tcl_DStringAppend(&buf, "::", 2);
	TclNewObj(mapDict);
	for (i=0 ; map[i].name != NULL ; i++) {
	    fromObj = Tcl_NewStringObj(map[i].name, -1);
	    TclNewStringObj(toObj, Tcl_DStringValue(&buf),
		    Tcl_DStringLength(&buf));
	    Tcl_AppendToObj(toObj, map[i].name, -1);
	    Tcl_DictObjPut(NULL, mapDict, fromObj, toObj);

	    if (map[i].proc || map[i].nreProc) {
		/*
		 * If the command is unsafe, hide it when we're in a safe
		 * interpreter. The code to do this is really hokey! It also
		 * doesn't work properly yet; this function is always
		 * currently called before the safe-interp flag is set so the
		 * Tcl_IsSafe check fails.
		 */

		if (map[i].unsafe && Tcl_IsSafe(interp)) {
		    cmdPtr = (Command *)
			    Tcl_NRCreateCommand(interp, "___tmp", map[i].proc,
			    map[i].nreProc, map[i].clientData, NULL);
		    Tcl_DStringSetLength(&hiddenBuf, hiddenLen);
		    if (Tcl_HideCommand(interp, "___tmp",
			    Tcl_DStringAppend(&hiddenBuf, map[i].name, -1))) {
			Tcl_Panic("%s", Tcl_GetString(Tcl_GetObjResult(interp)));
		    }
		} else {
		    /*
		     * Not hidden, so just create it. Yay!
		     */

		    cmdPtr = (Command *)
			    Tcl_NRCreateCommand(interp, TclGetString(toObj),
			    map[i].proc, map[i].nreProc, map[i].clientData,
			    NULL);
		}
		cmdPtr->compileProc = map[i].compileProc;
		if (map[i].compileProc != NULL) {
		    ensembleFlags |= ENSEMBLE_COMPILE;
		}
	    }
	}
	Tcl_SetEnsembleMappingDict(interp, ensemble, mapDict);
	if (ensembleFlags & ENSEMBLE_COMPILE) {
	    Tcl_SetEnsembleFlags(interp, ensemble, ensembleFlags);
	}
    }

    Tcl_DStringFree(&buf);
    Tcl_DStringFree(&hiddenBuf);
    if (nameParts != NULL) {
	Tcl_Free((char *) nameParts);
    }
    return ensemble;
}

/*
 *----------------------------------------------------------------------
 *
 * NsEnsembleImplementationCmd --
 *
 *	Implements an ensemble of commands (being those exported by a
 *	namespace other than the global namespace) as a command with the same
 *	(short) name as the namespace in the parent namespace.
 *
 * Results:
 *	A standard Tcl result code. Will be TCL_ERROR if the command is not an
 *	unambiguous prefix of any command exported by the ensemble's
 *	namespace.
 *
 * Side effects:
 *	Depends on the command within the namespace that gets executed. If the
 *	ensemble itself returns TCL_ERROR, a descriptive error message will be
 *	placed in the interpreter's result.
 *
 *----------------------------------------------------------------------
 */

static int
NsEnsembleImplementationCmd(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    return Tcl_NRCallObjProc(interp, NsEnsembleImplementationCmdNR,
	    clientData, objc, objv);
}

static int
NsEnsembleImplementationCmdNR(
    ClientData clientData,
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    EnsembleConfig *ensemblePtr = clientData;
				/* The ensemble itself. */
    Tcl_Obj *prefixObj;		/* An object containing the prefix words of
				 * the command that implements the
				 * subcommand. */
    Tcl_HashEntry *hPtr;	/* Used for efficient lookup of fully
				 * specified but not yet cached command
				 * names. */
    int reparseCount = 0;	/* Number of reparses. */

    /*
     * Must recheck objc, since numParameters might have changed. Cf. test
     * namespace-53.9.
     */

  restartEnsembleParse:
    if (objc < 2 + ensemblePtr->numParameters) {
	/*
	 * We don't have a subcommand argument. Make error message.
	 */

	Tcl_DString buf;	/* Message being built */
	Tcl_Obj **elemPtrs;	/* Parameter names */
	int len;		/* Number of parameters to append */

	Tcl_DStringInit(&buf);
	if (ensemblePtr->parameterList == NULL) {
	    len = 0;
	} else if (TclListObjGetElements(NULL, ensemblePtr->parameterList,
		&len, &elemPtrs) != TCL_OK) {
	    Tcl_Panic("List of ensemble parameters is not a list");
	}
	for (; len>0; len--,elemPtrs++) {
	    Tcl_DStringAppend(&buf, Tcl_GetString(*elemPtrs), -1);
	    Tcl_DStringAppend(&buf, " ", -1);
	}
	Tcl_DStringAppend(&buf, "subcommand ?arg ...?", -1);
	Tcl_WrongNumArgs(interp, 1, objv, Tcl_DStringValue(&buf));
	Tcl_DStringFree(&buf);

	return TCL_ERROR;
    }

    if (ensemblePtr->nsPtr->flags & NS_DYING) {
	/*
	 * Don't know how we got here, but make things give up quickly.
	 */

	if (!Tcl_InterpDeleted(interp)) {
	    Tcl_AppendResult(interp,
		    "ensemble activated for deleted namespace", NULL);
	}
	return TCL_ERROR;
    }

    /*
     * Determine if the table of subcommands is right. If so, we can just look
     * up in there and go straight to dispatch.
     */

    if (ensemblePtr->epoch == ensemblePtr->nsPtr->exportLookupEpoch) {
	/*
	 * Table of subcommands is still valid; therefore there might be a
	 * valid cache of discovered information which we can reuse. Do the
	 * check here, and if we're still valid, we can jump straight to the
	 * part where we do the invocation of the subcommand.
	 */

	if (objv[1+ensemblePtr->numParameters]->typePtr==&tclEnsembleCmdType){
	    EnsembleCmdRep *ensembleCmd = objv[1+ensemblePtr->numParameters]
		    ->internalRep.otherValuePtr;

	    if (ensembleCmd->nsPtr == ensemblePtr->nsPtr &&
		    ensembleCmd->epoch == ensemblePtr->epoch &&
		    ensembleCmd->token == ensemblePtr->token) {
		prefixObj = ensembleCmd->realPrefixObj;
		Tcl_IncrRefCount(prefixObj);
		goto runResultingSubcommand;
	    }
	}
    } else {
	BuildEnsembleConfig(ensemblePtr);
	ensemblePtr->epoch = ensemblePtr->nsPtr->exportLookupEpoch;
    }

    /*
     * Look in the hashtable for the subcommand name; this is the fastest way
     * of all if there is no cache in operation.
     */

    hPtr = Tcl_FindHashEntry(&ensemblePtr->subcommandTable,
	    TclGetString(objv[1 + ensemblePtr->numParameters]));
    if (hPtr != NULL) {
	char *fullName = Tcl_GetHashKey(&ensemblePtr->subcommandTable, hPtr);

	prefixObj = Tcl_GetHashValue(hPtr);

	/*
	 * Cache for later in the subcommand object.
	 */

	MakeCachedEnsembleCommand(objv[1 + ensemblePtr->numParameters],
		ensemblePtr, fullName, prefixObj);
    } else if (!(ensemblePtr->flags & TCL_ENSEMBLE_PREFIX)) {
	/*
	 * Could not map, no prefixing, go to unknown/error handling.
	 */

	goto unknownOrAmbiguousSubcommand;
    } else {
	/*
	 * If we've not already confirmed the command with the hash as part of
	 * building our export table, we need to scan the sorted array for
	 * matches.
	 */

	const char *subcmdName; /* Name of the subcommand, or unique prefix of
				 * it (will be an error for a non-unique
				 * prefix). */
	char *fullName = NULL;	/* Full name of the subcommand. */
	int stringLength, i;
	int tableLength = ensemblePtr->subcommandTable.numEntries;

	subcmdName = TclGetString(objv[1 + ensemblePtr->numParameters]);
	stringLength = objv[1 + ensemblePtr->numParameters]->length;
	for (i=0 ; i<tableLength ; i++) {
	    register int cmp = strncmp(subcmdName,
		    ensemblePtr->subcommandArrayPtr[i],
		    (unsigned) stringLength);

	    if (cmp == 0) {
		if (fullName != NULL) {
		    /*
		     * Since there's never the exact-match case to worry about
		     * (hash search filters this), getting here indicates that
		     * our subcommand is an ambiguous prefix of (at least) two
		     * exported subcommands, which is an error case.
		     */

		    goto unknownOrAmbiguousSubcommand;
		}
		fullName = ensemblePtr->subcommandArrayPtr[i];
	    } else if (cmp < 0) {
		/*
		 * Because we are searching a sorted table, we can now stop
		 * searching because we have gone past anything that could
		 * possibly match.
		 */

		break;
	    }
	}
	if (fullName == NULL) {
	    /*
	     * The subcommand is not a prefix of anything, so bail out!
	     */

	    goto unknownOrAmbiguousSubcommand;
	}
	hPtr = Tcl_FindHashEntry(&ensemblePtr->subcommandTable, fullName);
	if (hPtr == NULL) {
	    Tcl_Panic("full name %s not found in supposedly synchronized hash",
		    fullName);
	}
	prefixObj = Tcl_GetHashValue(hPtr);

	/*
	 * Cache for later in the subcommand object.
	 */

	MakeCachedEnsembleCommand(objv[1 + ensemblePtr->numParameters],
		ensemblePtr, fullName, prefixObj);
    }

    Tcl_IncrRefCount(prefixObj);
  runResultingSubcommand:

    /*
     * Do the real work of execution of the subcommand by building an array of
     * objects (note that this is potentially not the same length as the
     * number of arguments to this ensemble command), populating it and then
     * feeding it back through the main command-lookup engine. In theory, we
     * could look up the command in the namespace ourselves, as we already
     * have the namespace in which it is guaranteed to exist,
     *
     *   ((Q: That's not true if the -map option is used, is it?))
     *
     * but we don't do that (the cacheing of the command object used should
     * help with that.)
     */

    {
	Tcl_Obj **prefixObjv;	/* The list of objects to substitute in as the
				 * target command prefix. */
	Tcl_Obj *copyPtr;	/* The actual list of words to dispatch to.
				 * Will be freed by the dispatch engine. */
	int prefixObjc, copyObjc;
	Interp *iPtr = (Interp *) interp;

	/*
	 * Get the prefix that we're rewriting to. To do this we need to
	 * ensure that the internal representation of the list does not change
	 * so that we can safely keep the internal representations of the
	 * elements in the list.
	 *
	 * TODO: Use conventional list operations to make this code sane!
	 */

	TclListObjGetElements(NULL, prefixObj, &prefixObjc, &prefixObjv);

	copyObjc = objc - 2 + prefixObjc;
	copyPtr = Tcl_NewListObj(copyObjc, NULL);
	if (copyObjc > 0) {
	    register Tcl_Obj **copyObjv;
				/* Space used to construct the list of
				 * arguments to pass to the command that
				 * implements the ensemble subcommand. */
	    register List *listRepPtr = copyPtr->internalRep.twoPtrValue.ptr1;
	    register int i;

	    listRepPtr->elemCount = copyObjc;
	    copyObjv = &listRepPtr->elements;
	    memcpy(copyObjv, prefixObjv, sizeof(Tcl_Obj *) * prefixObjc);
	    memcpy(copyObjv+prefixObjc, objv+1,
		    sizeof(Tcl_Obj *) * ensemblePtr->numParameters);
	    memcpy(copyObjv+prefixObjc+ensemblePtr->numParameters,
		    objv+ensemblePtr->numParameters+2,
		    sizeof(Tcl_Obj *) * (objc-ensemblePtr->numParameters-2));

	    for (i=0; i < copyObjc; i++) {
		Tcl_IncrRefCount(copyObjv[i]);
	    }
	}
	TclDecrRefCount(prefixObj);

	/*
	 * Record what arguments the script sent in so that things like
	 * Tcl_WrongNumArgs can give the correct error message. Parameters
	 * count both as inserted and removed arguments.
	 */

#if 0
	if (TclInitRewriteEnsemble(interp, 2 + ensemblePtr->numParameters, prefixObjc + ensemblePtr->numParameters, objv)) {
	    TclNRAddCallback(interp, TclClearRootEnsemble, NULL, NULL, NULL, NULL);
	}
#else
	if (iPtr->ensembleRewrite.sourceObjs == NULL) {
	    iPtr->ensembleRewrite.sourceObjs = objv;
	    iPtr->ensembleRewrite.numRemovedObjs =
		    2 + ensemblePtr->numParameters;
	    iPtr->ensembleRewrite.numInsertedObjs =
		    prefixObjc + ensemblePtr->numParameters;
	    TclNRAddCallback(interp, TclClearRootEnsemble, NULL, NULL, NULL,
		    NULL);
	} else {
	    register int ni = 2 + ensemblePtr->numParameters
		    - iPtr->ensembleRewrite.numInsertedObjs;
				/* Position in objv of new front of insertion
				 * relative to old one. */
	    if (ni > 0) {
		iPtr->ensembleRewrite.numRemovedObjs += ni;
		iPtr->ensembleRewrite.numInsertedObjs += prefixObjc-1;
	    } else {
		iPtr->ensembleRewrite.numInsertedObjs += prefixObjc-2;
	    }
	}
#endif

	/*
	 * Hand off to the target command.
	 */

	iPtr->evalFlags |= TCL_EVAL_REDIRECT;
	return Tcl_NREvalObj(interp, copyPtr, TCL_EVAL_INVOKE);
    }

  unknownOrAmbiguousSubcommand:
    /*
     * Have not been able to match the subcommand asked for with a real
     * subcommand that we export. See whether a handler has been registered
     * for dealing with this situation. Will only call (at most) once for any
     * particular ensemble invocation.
     */

    if (ensemblePtr->unknownHandler != NULL && reparseCount++ < 1) {
	switch (EnsembleUnknownCallback(interp, ensemblePtr, objc, objv,
		&prefixObj)) {
	case TCL_OK:
	    goto runResultingSubcommand;
	case TCL_ERROR:
	    return TCL_ERROR;
	case TCL_CONTINUE:
	    goto restartEnsembleParse;
	}
    }

    /*
     * We cannot determine what subcommand to hand off to, so generate a
     * (standard) failure message. Note the one odd case compared with
     * standard ensemble-like command, which is where a namespace has no
     * exported commands at all...
     */

    Tcl_ResetResult(interp);
    Tcl_SetErrorCode(interp, "TCL", "LOOKUP", "ENSEMBLE",
	    TclGetString(objv[1+ensemblePtr->numParameters]), NULL);
    if (ensemblePtr->subcommandTable.numEntries == 0) {
	Tcl_AppendResult(interp, "unknown subcommand \"",
		TclGetString(objv[1+ensemblePtr->numParameters]),
		"\": namespace ", ensemblePtr->nsPtr->fullName,
		" does not export any commands", NULL);
	Tcl_SetErrorCode(interp, "TCL", "LOOKUP", "SUBCOMMAND",
		TclGetString(objv[1+ensemblePtr->numParameters]), NULL);
	return TCL_ERROR;
    }
    Tcl_AppendResult(interp, "unknown ",
	    (ensemblePtr->flags & TCL_ENSEMBLE_PREFIX ? "or ambiguous " : ""),
	    "subcommand \"", TclGetString(objv[1+ensemblePtr->numParameters]),
	    "\": must be ", NULL);
    if (ensemblePtr->subcommandTable.numEntries == 1) {
	Tcl_AppendResult(interp, ensemblePtr->subcommandArrayPtr[0], NULL);
    } else {
	int i;

	for (i=0 ; i<ensemblePtr->subcommandTable.numEntries-1 ; i++) {
	    Tcl_AppendResult(interp,
		    ensemblePtr->subcommandArrayPtr[i], ", ", NULL);
	}
	Tcl_AppendResult(interp, "or ",
		ensemblePtr->subcommandArrayPtr[i], NULL);
    }
    Tcl_SetErrorCode(interp, "TCL", "LOOKUP", "SUBCOMMAND",
	    TclGetString(objv[1+ensemblePtr->numParameters]), NULL);
    return TCL_ERROR;
}

int
TclClearRootEnsemble(
    ClientData data[],
    Tcl_Interp *interp,
    int result)
{
    TclResetRewriteEnsemble(interp, 1);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * TclInitRewriteEnsemble --
 *
 *	Applies a rewrite of arguments so that an ensemble subcommand will
 *	report error messages correctly for the overall command.
 *
 * Results:
 *	Whether this is the first rewrite applied, a value which must be
 *	passed to TclResetRewriteEnsemble when undoing this command's
 *	behaviour.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
TclInitRewriteEnsemble(
    Tcl_Interp *interp,
    int numRemoved,
    int numInserted,
    Tcl_Obj *const *objv)
{
    Interp *iPtr = (Interp *) interp;

    int isRootEnsemble = (iPtr->ensembleRewrite.sourceObjs == NULL);

    if (isRootEnsemble) {
	iPtr->ensembleRewrite.sourceObjs = objv;
	iPtr->ensembleRewrite.numRemovedObjs = numRemoved;
	iPtr->ensembleRewrite.numInsertedObjs = numInserted;
    } else {
	int numIns = iPtr->ensembleRewrite.numInsertedObjs;

	if (numIns < numRemoved) {
	    iPtr->ensembleRewrite.numRemovedObjs += numRemoved - numIns;
	    iPtr->ensembleRewrite.numInsertedObjs += numInserted - 1;
	} else {
	    iPtr->ensembleRewrite.numInsertedObjs += numInserted - numRemoved;
	}
    }
    return isRootEnsemble;
}

/*
 *----------------------------------------------------------------------
 *
 * TclResetRewriteEnsemble --
 *
 *	Removes any rewrites applied to support proper reporting of error
 *	messages used in ensembles. Should be paired with
 *	TclInitRewriteEnsemble.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
TclResetRewriteEnsemble(
    Tcl_Interp *interp,
    int isRootEnsemble)
{
    Interp *iPtr = (Interp *) interp;

    if (isRootEnsemble) {
	iPtr->ensembleRewrite.sourceObjs = NULL;
	iPtr->ensembleRewrite.numRemovedObjs = 0;
	iPtr->ensembleRewrite.numInsertedObjs = 0;
    }
}

/*
 * ----------------------------------------------------------------------
 *
 * EnsmebleUnknownCallback --
 *
 *	Helper for the ensemble engine that handles the procesing of unknown
 *	callbacks. See the user documentation of the ensemble unknown handler
 *	for details; this function is only ever called when such a function is
 *	defined, and is only ever called once per ensemble dispatch (i.e. if a
 *	reparse still fails, this isn't called again).
 *
 * Results:
 *	TCL_OK -	*prefixObjPtr contains the command words to dispatch
 *			to.
 *	TCL_CONTINUE -	Need to reparse (*prefixObjPtr is invalid).
 *	TCL_ERROR -	Something went wrong! Error message in interpreter.
 *
 * Side effects:
 *	Calls the Tcl interpreter, so arbitrary.
 *
 * ----------------------------------------------------------------------
 */

static inline int
EnsembleUnknownCallback(
    Tcl_Interp *interp,
    EnsembleConfig *ensemblePtr,
    int objc,
    Tcl_Obj *const objv[],
    Tcl_Obj **prefixObjPtr)
{
    int paramc, i, result, prefixObjc;
    Tcl_Obj **paramv, *unknownCmd, *ensObj;
    char buf[TCL_INTEGER_SPACE];

    /*
     * Create the unknown command callback to determine what to do.
     */

    unknownCmd = Tcl_DuplicateObj(ensemblePtr->unknownHandler);
    TclNewObj(ensObj);
    Tcl_GetCommandFullName(interp, ensemblePtr->token, ensObj);
    Tcl_ListObjAppendElement(NULL, unknownCmd, ensObj);
    for (i=1 ; i<objc ; i++) {
	Tcl_ListObjAppendElement(NULL, unknownCmd, objv[i]);
    }
    TclListObjGetElements(NULL, unknownCmd, &paramc, &paramv);
    Tcl_IncrRefCount(unknownCmd);

    /*
     * Now call the unknown handler. (We don't bother NRE-enabling this; deep
     * recursing through unknown handlers is horribly perverse.) Note that it
     * is always an error for an unknown handler to delete its ensemble; don't
     * do that!
     */

    Tcl_Preserve(ensemblePtr);
    ((Interp *) interp)->evalFlags |= TCL_EVAL_REDIRECT;
    result = Tcl_EvalObjv(interp, paramc, paramv, 0);
    if ((result == TCL_OK) && (ensemblePtr->flags & ENSEMBLE_DEAD)) {
	Tcl_SetResult(interp,
		"unknown subcommand handler deleted its ensemble",
		TCL_STATIC);
	result = TCL_ERROR;
    }
    Tcl_Release(ensemblePtr);

    /*
     * If we succeeded, we should either have a list of words that form the
     * command to be executed, or an empty list. In the empty-list case, the
     * ensemble is believed to be updated so we should ask the ensemble engine
     * to reparse the original command.
     */

    if (result == TCL_OK) {
	*prefixObjPtr = Tcl_GetObjResult(interp);
	Tcl_IncrRefCount(*prefixObjPtr);
	TclDecrRefCount(unknownCmd);
	Tcl_ResetResult(interp);

	/*
	 * Namespace is still there. Check if the result is a valid list. If
	 * it is, and it is non-empty, that list is what we are using as our
	 * replacement.
	 */

	if (TclListObjLength(interp, *prefixObjPtr, &prefixObjc) != TCL_OK) {
	    TclDecrRefCount(*prefixObjPtr);
	    Tcl_AddErrorInfo(interp, "\n    while parsing result of "
		    "ensemble unknown subcommand handler");
	    return TCL_ERROR;
	}
	if (prefixObjc > 0) {
	    return TCL_OK;
	}

	/*
	 * Namespace alive & empty result => reparse.
	 */

	TclDecrRefCount(*prefixObjPtr);
	return TCL_CONTINUE;
    }

    /*
     * Oh no! An exceptional result. Convert to an error.
     */

    if (!Tcl_InterpDeleted(interp)) {
	if (result != TCL_ERROR) {
	    Tcl_ResetResult(interp);
	    Tcl_SetResult(interp,
		    "unknown subcommand handler returned bad code: ",
		    TCL_STATIC);
	    switch (result) {
	    case TCL_RETURN:
		Tcl_AppendResult(interp, "return", NULL);
		break;
	    case TCL_BREAK:
		Tcl_AppendResult(interp, "break", NULL);
		break;
	    case TCL_CONTINUE:
		Tcl_AppendResult(interp, "continue", NULL);
		break;
	    default:
		sprintf(buf, "%d", result);
		Tcl_AppendResult(interp, buf, NULL);
	    }
	    Tcl_AddErrorInfo(interp, "\n    result of "
		    "ensemble unknown subcommand handler: ");
	    Tcl_AddErrorInfo(interp, TclGetString(unknownCmd));
	} else {
	    Tcl_AddErrorInfo(interp,
		    "\n    (ensemble unknown subcommand handler)");
	}
    }
    TclDecrRefCount(unknownCmd);
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * MakeCachedEnsembleCommand --
 *
 *	Cache what we've computed so far; it's not nice to repeatedly copy
 *	strings about. Note that to do this, we start by deleting any old
 *	representation that there was (though if it was an out of date
 *	ensemble rep, we can skip some of the deallocation process.)
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Alters the internal representation of the first object parameter.
 *
 *----------------------------------------------------------------------
 */

static void
MakeCachedEnsembleCommand(
    Tcl_Obj *objPtr,
    EnsembleConfig *ensemblePtr,
    const char *subcommandName,
    Tcl_Obj *prefixObjPtr)
{
    register EnsembleCmdRep *ensembleCmd;
    int length;

    if (objPtr->typePtr == &tclEnsembleCmdType) {
	ensembleCmd = objPtr->internalRep.otherValuePtr;
	Tcl_DecrRefCount(ensembleCmd->realPrefixObj);
	TclNsDecrRefCount(ensembleCmd->nsPtr);
	ckfree(ensembleCmd->fullSubcmdName);
    } else {
	/*
	 * Kill the old internal rep, and replace it with a brand new one of
	 * our own.
	 */

	TclFreeIntRep(objPtr);
	ensembleCmd = (EnsembleCmdRep *) ckalloc(sizeof(EnsembleCmdRep));
	objPtr->internalRep.otherValuePtr = ensembleCmd;
	objPtr->typePtr = &tclEnsembleCmdType;
    }

    /*
     * Populate the internal rep.
     */

    ensembleCmd->nsPtr = ensemblePtr->nsPtr;
    ensembleCmd->epoch = ensemblePtr->epoch;
    ensembleCmd->token = ensemblePtr->token;
    ensemblePtr->nsPtr->refCount++;
    ensembleCmd->realPrefixObj = prefixObjPtr;
    length = strlen(subcommandName)+1;
    ensembleCmd->fullSubcmdName = ckalloc((unsigned) length);
    memcpy(ensembleCmd->fullSubcmdName, subcommandName, (unsigned) length);
    Tcl_IncrRefCount(ensembleCmd->realPrefixObj);
}

/*
 *----------------------------------------------------------------------
 *
 * DeleteEnsembleConfig --
 *
 *	Destroys the data structure used to represent an ensemble. This is
 *	called when the ensemble's command is deleted (which happens
 *	automatically if the ensemble's namespace is deleted.) Maintainers
 *	should note that ensembles should be deleted by deleting their
 *	commands.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Memory is (eventually) deallocated.
 *
 *----------------------------------------------------------------------
 */

static void
DeleteEnsembleConfig(
    ClientData clientData)
{
    EnsembleConfig *ensemblePtr = clientData;
    Namespace *nsPtr = ensemblePtr->nsPtr;
    Tcl_HashSearch search;
    Tcl_HashEntry *hEnt;

    /*
     * Unlink from the ensemble chain if it has not been marked as having been
     * done already.
     */

    if (ensemblePtr->next != ensemblePtr) {
	EnsembleConfig *ensPtr = (EnsembleConfig *) nsPtr->ensembles;

	if (ensPtr == ensemblePtr) {
	    nsPtr->ensembles = (Tcl_Ensemble *) ensemblePtr->next;
	} else {
	    while (ensPtr != NULL) {
		if (ensPtr->next == ensemblePtr) {
		    ensPtr->next = ensemblePtr->next;
		    break;
		}
		ensPtr = ensPtr->next;
	    }
	}
    }

    /*
     * Mark the namespace as dead so code that uses Tcl_Preserve() can tell
     * whether disaster happened anyway.
     */

    ensemblePtr->flags |= ENSEMBLE_DEAD;

    /*
     * Kill the pointer-containing fields.
     */

    if (ensemblePtr->subcommandTable.numEntries != 0) {
	ckfree((char *) ensemblePtr->subcommandArrayPtr);
    }
    hEnt = Tcl_FirstHashEntry(&ensemblePtr->subcommandTable, &search);
    while (hEnt != NULL) {
	Tcl_Obj *prefixObj = Tcl_GetHashValue(hEnt);

	Tcl_DecrRefCount(prefixObj);
	hEnt = Tcl_NextHashEntry(&search);
    }
    Tcl_DeleteHashTable(&ensemblePtr->subcommandTable);
    if (ensemblePtr->subcmdList != NULL) {
	Tcl_DecrRefCount(ensemblePtr->subcmdList);
    }
    if (ensemblePtr->parameterList != NULL) {
	Tcl_DecrRefCount(ensemblePtr->parameterList);
    }
    if (ensemblePtr->subcommandDict != NULL) {
	Tcl_DecrRefCount(ensemblePtr->subcommandDict);
    }
    if (ensemblePtr->unknownHandler != NULL) {
	Tcl_DecrRefCount(ensemblePtr->unknownHandler);
    }

    /*
     * Arrange for the structure to be reclaimed. Note that this is complex
     * because we have to make sure that we can react sensibly when an
     * ensemble is deleted during the process of initialising the ensemble
     * (especially the unknown callback.)
     */

    Tcl_EventuallyFree(ensemblePtr, TCL_DYNAMIC);
}

/*
 *----------------------------------------------------------------------
 *
 * BuildEnsembleConfig --
 *
 *	Create the internal data structures that describe how an ensemble
 *	looks, being a hash mapping from the full command name to the Tcl list
 *	that describes the implementation prefix words, and a sorted array of
 *	all the full command names to allow for reasonably efficient
 *	unambiguous prefix handling.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Reallocates and rebuilds the hash table and array stored at the
 *	ensemblePtr argument. For large ensembles or large namespaces, this is
 *	a potentially expensive operation.
 *
 *----------------------------------------------------------------------
 */

static void
BuildEnsembleConfig(
    EnsembleConfig *ensemblePtr)
{
    Tcl_HashSearch search;	/* Used for scanning the set of commands in
				 * the namespace that backs up this
				 * ensemble. */
    int i, j, isNew;
    Tcl_HashTable *hash = &ensemblePtr->subcommandTable;
    Tcl_HashEntry *hPtr;

    if (hash->numEntries != 0) {
	/*
	 * Remove pre-existing table.
	 */

	ckfree((char *) ensemblePtr->subcommandArrayPtr);
	hPtr = Tcl_FirstHashEntry(hash, &search);
	while (hPtr != NULL) {
	    Tcl_Obj *prefixObj = Tcl_GetHashValue(hPtr);

	    Tcl_DecrRefCount(prefixObj);
	    hPtr = Tcl_NextHashEntry(&search);
	}
	Tcl_DeleteHashTable(hash);
	Tcl_InitHashTable(hash, TCL_STRING_KEYS);
    }

    /*
     * See if we've got an export list. If so, we will only export exactly
     * those commands, which may be either implemented by the prefix in the
     * subcommandDict or mapped directly onto the namespace's commands.
     */

    if (ensemblePtr->subcmdList != NULL) {
	Tcl_Obj **subcmdv, *target, *cmdObj, *cmdPrefixObj;
	int subcmdc;

	TclListObjGetElements(NULL, ensemblePtr->subcmdList, &subcmdc,
		&subcmdv);
	for (i=0 ; i<subcmdc ; i++) {
	    const char *name = TclGetString(subcmdv[i]);

	    hPtr = Tcl_CreateHashEntry(hash, name, &isNew);

	    /*
	     * Skip non-unique cases.
	     */

	    if (!isNew) {
		continue;
	    }

	    /*
	     * Look in our dictionary (if present) for the command.
	     */

	    if (ensemblePtr->subcommandDict != NULL) {
		Tcl_DictObjGet(NULL, ensemblePtr->subcommandDict, subcmdv[i],
			&target);
		if (target != NULL) {
		    Tcl_SetHashValue(hPtr, target);
		    Tcl_IncrRefCount(target);
		    continue;
		}
	    }

	    /*
	     * Not there, so map onto the namespace. Note in this case that we
	     * do not guarantee that the command is actually there; that is
	     * the programmer's responsibility (or [::unknown] of course).
	     */

	    cmdObj = Tcl_NewStringObj(ensemblePtr->nsPtr->fullName, -1);
	    if (ensemblePtr->nsPtr->parentPtr != NULL) {
		Tcl_AppendStringsToObj(cmdObj, "::", name, NULL);
	    } else {
		Tcl_AppendStringsToObj(cmdObj, name, NULL);
	    }
	    cmdPrefixObj = Tcl_NewListObj(1, &cmdObj);
	    Tcl_SetHashValue(hPtr, cmdPrefixObj);
	    Tcl_IncrRefCount(cmdPrefixObj);
	}
    } else if (ensemblePtr->subcommandDict != NULL) {
	/*
	 * No subcmd list, but we do have a mapping dictionary so we should
	 * use the keys of that. Convert the dictionary's contents into the
	 * form required for the ensemble's internal hashtable.
	 */

	Tcl_DictSearch dictSearch;
	Tcl_Obj *keyObj, *valueObj;
	int done;

	Tcl_DictObjFirst(NULL, ensemblePtr->subcommandDict, &dictSearch,
		&keyObj, &valueObj, &done);
	while (!done) {
	    const char *name = TclGetString(keyObj);

	    hPtr = Tcl_CreateHashEntry(hash, name, &isNew);
	    Tcl_SetHashValue(hPtr, valueObj);
	    Tcl_IncrRefCount(valueObj);
	    Tcl_DictObjNext(&dictSearch, &keyObj, &valueObj, &done);
	}
    } else {
	/*
	 * Discover what commands are actually exported by the namespace.
	 * What we have is an array of patterns and a hash table whose keys
	 * are the command names exported by the namespace (the contents do
	 * not matter here.) We must find out what commands are actually
	 * exported by filtering each command in the namespace against each of
	 * the patterns in the export list. Note that we use an intermediate
	 * hash table to make memory management easier, and because that makes
	 * exact matching far easier too.
	 *
	 * Suggestion for future enhancement: compute the unique prefixes and
	 * place them in the hash too, which should make for even faster
	 * matching.
	 */

	hPtr = Tcl_FirstHashEntry(&ensemblePtr->nsPtr->cmdTable, &search);
	for (; hPtr!= NULL ; hPtr=Tcl_NextHashEntry(&search)) {
	    char *nsCmdName =		/* Name of command in namespace. */
		    Tcl_GetHashKey(&ensemblePtr->nsPtr->cmdTable, hPtr);

	    for (i=0 ; i<ensemblePtr->nsPtr->numExportPatterns ; i++) {
		if (Tcl_StringMatch(nsCmdName,
			ensemblePtr->nsPtr->exportArrayPtr[i])) {
		    hPtr = Tcl_CreateHashEntry(hash, nsCmdName, &isNew);

		    /*
		     * Remember, hash entries have a full reference to the
		     * substituted part of the command (as a list) as their
		     * content!
		     */

		    if (isNew) {
			Tcl_Obj *cmdObj, *cmdPrefixObj;

			TclNewObj(cmdObj);
			Tcl_AppendStringsToObj(cmdObj,
				ensemblePtr->nsPtr->fullName,
				(ensemblePtr->nsPtr->parentPtr ? "::" : ""),
				nsCmdName, NULL);
			cmdPrefixObj = Tcl_NewListObj(1, &cmdObj);
			Tcl_SetHashValue(hPtr, cmdPrefixObj);
			Tcl_IncrRefCount(cmdPrefixObj);
		    }
		    break;
		}
	    }
	}
    }

    if (hash->numEntries == 0) {
	ensemblePtr->subcommandArrayPtr = NULL;
	return;
    }

    /*
     * Create a sorted array of all subcommands in the ensemble; hash tables
     * are all very well for a quick look for an exact match, but they can't
     * determine things like whether a string is a prefix of another (not
     * without lots of preparation anyway) and they're no good for when we're
     * generating the error message either.
     *
     * We do this by filling an array with the names (we use the hash keys
     * directly to save a copy, since any time we change the array we change
     * the hash too, and vice versa) and running quicksort over the array.
     */

    ensemblePtr->subcommandArrayPtr = (char **)
	    ckalloc(sizeof(char *) * hash->numEntries);

    /*
     * Fill array from both ends as this makes us less likely to end up with
     * performance problems in qsort(), which is good. Note that doing this
     * makes this code much more opaque, but the naive alternatve:
     *
     * for (hPtr=Tcl_FirstHashEntry(hash,&search),i=0 ;
     *	       hPtr!=NULL ; hPtr=Tcl_NextHashEntry(&search),i++) {
     *     ensemblePtr->subcommandArrayPtr[i] = Tcl_GetHashKey(hash, &hPtr);
     * }
     *
     * can produce long runs of precisely ordered table entries when the
     * commands in the namespace are declared in a sorted fashion (an ordering
     * some people like) and the hashing functions (or the command names
     * themselves) are fairly unfortunate. By filling from both ends, it
     * requires active malice (and probably a debugger) to get qsort() to have
     * awful runtime behaviour.
     */

    i = 0;
    j = hash->numEntries;
    hPtr = Tcl_FirstHashEntry(hash, &search);
    while (hPtr != NULL) {
	ensemblePtr->subcommandArrayPtr[i++] = Tcl_GetHashKey(hash, hPtr);
	hPtr = Tcl_NextHashEntry(&search);
	if (hPtr == NULL) {
	    break;
	}
	ensemblePtr->subcommandArrayPtr[--j] = Tcl_GetHashKey(hash, hPtr);
	hPtr = Tcl_NextHashEntry(&search);
    }
    if (hash->numEntries > 1) {
	qsort(ensemblePtr->subcommandArrayPtr, (unsigned) hash->numEntries,
		sizeof(char *), NsEnsembleStringOrder);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * NsEnsembleStringOrder --
 *
 *	Helper function to compare two pointers to two strings for use with
 *	qsort().
 *
 * Results:
 *	-1 if the first string is smaller, 1 if the second string is smaller,
 *	and 0 if they are equal.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static int
NsEnsembleStringOrder(
    const void *strPtr1,
    const void *strPtr2)
{
    return strcmp(*(const char **)strPtr1, *(const char **)strPtr2);
}

/*
 *----------------------------------------------------------------------
 *
 * FreeEnsembleCmdRep --
 *
 *	Destroys the internal representation of a Tcl_Obj that has been
 *	holding information about a command in an ensemble.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Memory is deallocated. If this held the last reference to a
 *	namespace's main structure, that main structure will also be
 *	destroyed.
 *
 *----------------------------------------------------------------------
 */

static void
FreeEnsembleCmdRep(
    Tcl_Obj *objPtr)
{
    EnsembleCmdRep *ensembleCmd = objPtr->internalRep.otherValuePtr;

    Tcl_DecrRefCount(ensembleCmd->realPrefixObj);
    ckfree(ensembleCmd->fullSubcmdName);
    TclNsDecrRefCount(ensembleCmd->nsPtr);
    ckfree((char *) ensembleCmd);
    objPtr->typePtr = NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * DupEnsembleCmdRep --
 *
 *	Makes one Tcl_Obj into a copy of another that is a subcommand of an
 *	ensemble.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Memory is allocated, and the namespace that the ensemble is built on
 *	top of gains another reference.
 *
 *----------------------------------------------------------------------
 */

static void
DupEnsembleCmdRep(
    Tcl_Obj *objPtr,
    Tcl_Obj *copyPtr)
{
    EnsembleCmdRep *ensembleCmd = objPtr->internalRep.otherValuePtr;
    EnsembleCmdRep *ensembleCopy = (EnsembleCmdRep *)
	    ckalloc(sizeof(EnsembleCmdRep));
    int length = strlen(ensembleCmd->fullSubcmdName);

    copyPtr->typePtr = &tclEnsembleCmdType;
    copyPtr->internalRep.otherValuePtr = ensembleCopy;
    ensembleCopy->nsPtr = ensembleCmd->nsPtr;
    ensembleCopy->epoch = ensembleCmd->epoch;
    ensembleCopy->token = ensembleCmd->token;
    ensembleCopy->nsPtr->refCount++;
    ensembleCopy->realPrefixObj = ensembleCmd->realPrefixObj;
    Tcl_IncrRefCount(ensembleCopy->realPrefixObj);
    ensembleCopy->fullSubcmdName = ckalloc((unsigned) length+1);
    memcpy(ensembleCopy->fullSubcmdName, ensembleCmd->fullSubcmdName,
	    (unsigned) length+1);
}

/*
 *----------------------------------------------------------------------
 *
 * StringOfEnsembleCmdRep --
 *
 *	Creates a string representation of a Tcl_Obj that holds a subcommand
 *	of an ensemble.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The object gains a string (UTF-8) representation.
 *
 *----------------------------------------------------------------------
 */

static void
StringOfEnsembleCmdRep(
    Tcl_Obj *objPtr)
{
    EnsembleCmdRep *ensembleCmd = objPtr->internalRep.otherValuePtr;
    int length = strlen(ensembleCmd->fullSubcmdName);

    objPtr->length = length;
    objPtr->bytes = ckalloc((unsigned) length+1);
    memcpy(objPtr->bytes, ensembleCmd->fullSubcmdName, (unsigned) length+1);
}

/*
 *----------------------------------------------------------------------
 *
 * TclCompileEnsemble --
 *
 *	Procedure called to compile an ensemble command. Note that most
 *	ensembles are not compiled, since modifying a compiled ensemble causes
 *	a invalidation of all existing bytecode (expensive!) which is not
 *	normally warranted.
 *
 * Results:
 *	Returns TCL_OK for a successful compile. Returns TCL_ERROR to defer
 *	evaluation to runtime.
 *
 * Side effects:
 *	Instructions are added to envPtr to execute the subcommands of the
 *	ensemble at runtime if a compile-time mapping is possible.
 *
 *----------------------------------------------------------------------
 */

int
TclCompileEnsemble(
    Tcl_Interp *interp,		/* Used for error reporting. */
    Tcl_Parse *parsePtr,	/* Points to a parse structure for the command
				 * created by Tcl_ParseCommand. */
    Command *cmdPtr,		/* Points to defintion of command being
				 * compiled. */
    CompileEnv *envPtr)		/* Holds resulting instructions. */
{
    Tcl_Token *tokenPtr;
    Tcl_Obj *mapObj, *subcmdObj, *targetCmdObj, *listObj, **elems;
    Tcl_Command ensemble = (Tcl_Command) cmdPtr;
    Tcl_Parse synthetic;
    int len, result, flags = 0, i;
    unsigned numBytes;
    const char *word;

    if (parsePtr->numWords < 2) {
	return TCL_ERROR;
    }

    tokenPtr = TokenAfter(parsePtr->tokenPtr);
    if (tokenPtr->type != TCL_TOKEN_SIMPLE_WORD) {
	/*
	 * Too hard.
	 */

	return TCL_ERROR;
    }

    word = tokenPtr[1].start;
    numBytes = tokenPtr[1].size;

    /*
     * There's a sporting chance we'll be able to compile this. But now we
     * must check properly. To do that, check that we're compiling an ensemble
     * that has a compilable command as its appropriate subcommand.
     */

    if (Tcl_GetEnsembleMappingDict(NULL, ensemble, &mapObj) != TCL_OK
	    || mapObj == NULL) {
	/*
	 * Either not an ensemble or a mapping isn't installed. Crud. Too hard
	 * to proceed.
	 */

	return TCL_ERROR;
    }

    /*
     * Also refuse to compile anything that uses a formal parameter list for
     * now, on the grounds that it is too complex.
     */

    if (Tcl_GetEnsembleParameterList(NULL, ensemble, &listObj) != TCL_OK
	    || listObj != NULL) {
	/*
	 * Figuring out how to compile this has become too much. Bail out.
	 */

	return TCL_ERROR;
    }

    /*
     * Next, get the flags. We need them on several code paths so that we can
     * know whether we're to do prefix matching.
     */

    (void) Tcl_GetEnsembleFlags(NULL, ensemble, &flags);

    /*
     * Check to see if there's also a subcommand list; must check to see if
     * the subcommand we are calling is in that list if it exists, since that
     * list filters the entries in the map.
     */

    (void) Tcl_GetEnsembleSubcommandList(NULL, ensemble, &listObj);
    if (listObj != NULL) {
	int sclen;
	const char *str;
	Tcl_Obj *matchObj = NULL;

	if (Tcl_ListObjGetElements(NULL, listObj, &len, &elems) != TCL_OK) {
	    return TCL_ERROR;
	}
	for (i=0 ; i<len ; i++) {
	    str = Tcl_GetStringFromObj(elems[i], &sclen);
	    if ((sclen == (int) numBytes) && !memcmp(word, str, numBytes)) {
		/*
		 * Exact match! Excellent!
		 */

		result = Tcl_DictObjGet(NULL, mapObj,elems[i], &targetCmdObj);
		if (result != TCL_OK || targetCmdObj == NULL) {
		    return TCL_ERROR;
		}
		goto doneMapLookup;
	    }

	    /*
	     * Check to see if we've got a prefix match. A single prefix match
	     * is fine, and allows us to refine our dictionary lookup, but
	     * multiple prefix matches is a Bad Thing and will prevent us from
	     * making progress. Note that we cannot do the lookup immediately
	     * in the prefix case; might be another entry later in the list
	     * that causes things to fail.
	     */

	    if ((flags & TCL_ENSEMBLE_PREFIX)
		    && strncmp(word, str, numBytes) == 0) {
		if (matchObj != NULL) {
		    return TCL_ERROR;
		}
		matchObj = elems[i];
	    }
	}
	if (matchObj == NULL) {
	    return TCL_ERROR;
	}
	result = Tcl_DictObjGet(NULL, mapObj, matchObj, &targetCmdObj);
	if (result != TCL_OK || targetCmdObj == NULL) {
	    return TCL_ERROR;
	}
    } else {
	Tcl_DictSearch s;
	int done, matched;
	Tcl_Obj *tmpObj;

	/*
	 * No map, so check the dictionary directly.
	 */

	TclNewStringObj(subcmdObj, word, (int) numBytes);
	result = Tcl_DictObjGet(NULL, mapObj, subcmdObj, &targetCmdObj);
	TclDecrRefCount(subcmdObj);
	if (result == TCL_OK && targetCmdObj != NULL) {
	    /*
	     * Got it. Skip the fiddling around with prefixes.
	     */

	    goto doneMapLookup;
	}

	/*
	 * We've not literally got a valid subcommand. But maybe we have a
	 * prefix. Check if prefix matches are allowed.
	 */

	if (!(flags & TCL_ENSEMBLE_PREFIX)) {
	    return TCL_ERROR;
	}

	/*
	 * Iterate over the keys in the dictionary, checking to see if we're a
	 * prefix.
	 */

	Tcl_DictObjFirst(NULL, mapObj, &s, &subcmdObj, &tmpObj, &done);
	matched = 0;
	while (!done) {
	    if (strncmp(TclGetString(subcmdObj), word, numBytes) == 0) {
		if (matched++) {
		    /*
		     * Must have matched twice! Not unique, so no point
		     * looking further.
		     */

		    break;
		}
		targetCmdObj = tmpObj;
	    }
	    Tcl_DictObjNext(&s, &subcmdObj, &tmpObj, &done);
	}
	Tcl_DictObjDone(&s);

	/*
	 * If we have anything other than a single match, we've failed the
	 * unique prefix check.
	 */

	if (matched != 1) {
	    return TCL_ERROR;
	}
    }

    /*
     * OK, we definitely map to something. But what?
     *
     * The command we map to is the first word out of the map element. Note
     * that we also reject dealing with multi-element rewrites if we are in a
     * safe interpreter, as there is otherwise a (highly gnarly!) way to make
     * Tcl crash open to exploit.
     */

  doneMapLookup:
    if (Tcl_ListObjGetElements(NULL, targetCmdObj, &len, &elems) != TCL_OK) {
	return TCL_ERROR;
    }
    if (len > 1 && Tcl_IsSafe(interp)) {
	return TCL_ERROR;
    }
    targetCmdObj = elems[0];

    Tcl_IncrRefCount(targetCmdObj);
    cmdPtr = (Command *) Tcl_GetCommandFromObj(interp, targetCmdObj);
    TclDecrRefCount(targetCmdObj);
    if (cmdPtr == NULL || cmdPtr->compileProc == NULL) {
	/*
	 * Maps to an undefined command or a command without a compiler.
	 * Cannot compile.
	 */

	return TCL_ERROR;
    }

    /*
     * Now we've done the mapping process, can now actually try to compile.
     * We do this by handing off to the subcommand's actual compiler. But to
     * do that, we have to perform some trickery to rewrite the arguments.
     */

    TclParseInit(interp, NULL, 0, &synthetic);
    synthetic.numWords = parsePtr->numWords - 2 + len;
    TclGrowParseTokenArray(&synthetic, 2*len);
    synthetic.numTokens = 2*len;

    /*
     * Now we have the space to work in, install something rewritten. Note
     * that we are here praying for all our might that none of these words are
     * a script; the error detection code will crash if that happens and there
     * is nothing we can do to avoid it!
     */

    for (i=0 ; i<len ; i++) {
	int sclen;
	const char *str = Tcl_GetStringFromObj(elems[i], &sclen);

	synthetic.tokenPtr[2*i].type = TCL_TOKEN_SIMPLE_WORD;
	synthetic.tokenPtr[2*i].start = str;
	synthetic.tokenPtr[2*i].size = sclen;
	synthetic.tokenPtr[2*i].numComponents = 1;

	synthetic.tokenPtr[2*i+1].type = TCL_TOKEN_TEXT;
	synthetic.tokenPtr[2*i+1].start = str;
	synthetic.tokenPtr[2*i+1].size = sclen;
	synthetic.tokenPtr[2*i+1].numComponents = 0;
    }

    /*
     * Copy over the real argument tokens.
     */

    for (i=len; i<synthetic.numWords; i++) {
	int toCopy;

	tokenPtr = TokenAfter(tokenPtr);
	toCopy = tokenPtr->numComponents + 1;
	TclGrowParseTokenArray(&synthetic, toCopy);
	memcpy(synthetic.tokenPtr + synthetic.numTokens, tokenPtr,
		sizeof(Tcl_Token) * toCopy);
	synthetic.numTokens += toCopy;
    }

    /*
     * Hand off compilation to the subcommand compiler. At last!
     */

    result = cmdPtr->compileProc(interp, &synthetic, cmdPtr, envPtr);

    /*
     * Clean up if necessary.
     */

    Tcl_FreeParse(&synthetic);
    return result;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
