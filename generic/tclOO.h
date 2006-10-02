/*
 * tclOO.h --
 *
 *	This file contains the structure definitions and some of the function
 *	declarations for the object-system (NB: not Tcl_Obj, but ::oo).
 *
 * Copyright (c) 2006 by Donal K. Fellows
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id$
 */

// vvvvvvvvvvvvvvvvvvvvvv MOVE TO TCL.DECLS vvvvvvvvvvvvvvvvvvvvvv
Tcl_Object		Tcl_GetClassAsObject(Tcl_Class clazz);
Tcl_Class		Tcl_GetObjectAsClass(Tcl_Object object);
Tcl_Command		Tcl_GetObjectCommand(Tcl_Object object);
Tcl_Object		Tcl_GetObjectFromObj(Tcl_Interp *interp,
			    Tcl_Obj *objPtr);
Tcl_Namespace *		Tcl_GetObjectNamespace(Tcl_Object object);
Tcl_Class		Tcl_MethodDeclarerClass(Tcl_Method method);
Tcl_Object		Tcl_MethodDeclarerObject(Tcl_Method method);
int			Tcl_MethodIsPublic(Tcl_Method method);
int			Tcl_MethodIsType(Tcl_Method method,
			    const Tcl_MethodType *typePtr,
			    ClientData *clientDataPtr);
Tcl_Obj *		Tcl_MethodName(Tcl_Method method);
Tcl_Method		Tcl_NewMethod(Tcl_Interp *interp, Tcl_Object object,
			    Tcl_Obj *nameObj, int isPublic,
			    const Tcl_MethodType *typePtr,
			    ClientData clientData);
Tcl_Method		Tcl_NewClassMethod(Tcl_Interp *interp, Tcl_Class cls,
			    Tcl_Obj *nameObj, int isPublic,
			    const Tcl_MethodType *typePtr,
			    ClientData clientData);
Tcl_Object		Tcl_NewObjectInstance(Tcl_Interp *interp,
			    Tcl_Class cls, const char *name, int objc,
			    Tcl_Obj *const *objv, int skip);
int			Tcl_ObjectDeleted(Tcl_Object object);
int			Tcl_ObjectContextIsFiltering(
			    Tcl_ObjectContext context);
Tcl_Method		Tcl_ObjectContextMethod(Tcl_ObjectContext context);
Tcl_Object		Tcl_ObjectContextObject(Tcl_ObjectContext context);
int			Tcl_ObjectContextSkippedArgs(
			    Tcl_ObjectContext context);
// ^^^^^^^^^^^^^^^^^^^^^^ MOVE TO TCL.DECLS ^^^^^^^^^^^^^^^^^^^^^^

/*
 * Forward declarations.
 */

struct Class;
struct Object;
struct Method;
struct CallContext;

/*
 * The data that needs to be stored per method. This record is used to collect
 * information about all sorts of methods, including forwards, constructors
 * and destructors.
 */

typedef struct Method {
    const Tcl_MethodType *typePtr;
				/* The type of method. If NULL, this is a
				 * special flag record which is just used for
				 * the setting of the flags field. */
    ClientData clientData;	/* Type-specific data. */
    Tcl_Obj *namePtr;		/* Name of the method. */
    struct Object *declaringObjectPtr;
				/* The object that declares this method, or
				 * NULL if it was declared by a class. */
    struct Class *declaringClassPtr;
				/* The class that declares this method, or
				 * NULL if it was declared directly on an
				 * object. */
    int flags;			/* Assorted flags. Includes whether this
				 * method is public/exported or not. */
} Method;

/*
 * Procedure-like methods have the following extra information. It is a
 * single-field structure because this allows for future expansion without
 * changing vast amounts of code.
 */

typedef struct ProcedureMethod {
    Proc *procPtr;
} ProcedureMethod;

/*
 * Forwarded methods have the following extra information. It is a
 * single-field structure because this allows for future expansion without
 * changing vast amounts of code.
 */

typedef struct ForwardMethod {
    Tcl_Obj *prefixObj;
} ForwardMethod;

/*
 * Now, the definition of what an object actually is.
 */

typedef struct Object {
    Tcl_Namespace *namespacePtr;/* This object's tame namespace. */
    Tcl_Command command;	/* Reference to this object's public
				 * command. */
    Tcl_Command myCommand;	/* Reference to this object's internal
				 * command. */
    struct Class *selfCls;	/* This object's class. */
    Tcl_HashTable methods;	/* Object-local Tcl_Obj (method name) to
				 * Method* mapping. */
    struct {			/* Classes mixed into this object; list length
				 * = allocated space = num field. */
	int num;
	struct Class **list;
    } mixins;
    struct {			/* List of filter names; length=space=num. */
	int num;
	Tcl_Obj **list;
    } filters;
    struct Class *classPtr;	/* All classes have this non-NULL; it points
				 * to the class structure. Everything else has
				 * this NULL. */
    int flags;
    int epoch;			/* Per-object epoch, incremented when the way
				 * an object should resolve call chains is
				 * changed. */
    Tcl_HashTable publicContextCache;	/* Place to keep unused contexts. */
    Tcl_HashTable privateContextCache;	/* Place to keep unused contexts. */
} Object;

#define OBJECT_DELETED	1	/* Flag to say that an object has been
				 * destroyed. */
#define ROOT_OBJECT 0x1000	/* Flag to say that this object is the root of
				 * the class hierarchy and should be treated
				 * specially during teardown. */

/*
 * And the definition of a class. Note that every class also has an associated
 * object, through which it is manipulated.
 */

typedef struct Class {
    Object *thisPtr;		/* Reference to the object associated with
				 * this class. */
    int flags;			/* Assorted flags. */
    struct {			/* List of superclasses; length=space=num. */
	int num;
	struct Class **list;
    } superclasses;
    struct {			/* List of subclasses; length=num,space=size */
	int num, size;
	struct Class **list;
    } subclasses;
    struct {			/* List of instances; length=num,space=size */
	int num, size;
	Object **list;
    } instances;
    struct {			/* List of filter names; length=space=num. */
	int num;
	Tcl_Obj **list;
    } filters;
    Tcl_HashTable classMethods;	/* Hash table of all methods. Hash maps from
				 * the (Tcl_Obj*) method name to the (Method*)
				 * method record. */
    Method *constructorPtr;	/* Method record of the class constructor (if
				 * any). */
    Method *destructorPtr;	/* Method record of the class destructor (if
				 * any). */
} Class;

/*
 * The foundation of the object system within an interpreter contains
 * references to the key classes and namespaces, together with a few other
 * useful bits and pieces. Probably ought to eventually go in the Interp
 * structure itself.
 */

typedef struct Foundation {
    struct Class *objectCls;	/* The root of the object system. */
    struct Class *classCls;	/* The class of all classes. */
    struct Class *definerCls;	/* A metaclass that includes methods that make
				 * classes more convenient to work with at a
				 * cost of bloat. */
    struct Class *structCls;	/* A metaclass that includes methods that make
				 * it easier to build data-oriented
				 * classes. */
    Tcl_Namespace *ooNs;	/* Master ::oo namespace. */
    Tcl_Namespace *defineNs;	/* Namespace containing special commands for
				 * manipulating objects and classes. The
				 * "oo::define" command acts as a special kind
				 * of ensemble for this namespace. */
    Tcl_Namespace *helpersNs;	/* Namespace containing the commands that are
				 * only valid when executing inside a
				 * procedural method. */
    int epoch;			/* Used to invalidate method chains when the
				 * class structure changes. */
    int nsCount;		/* Counter so we can allocate a unique
				 * namespace to each object. */
    Tcl_Obj *unknownMethodNameObj;
				/* Shared object containing the name of the
				 * unknown method handler method. */
} Foundation;

/*
 * A call context structure is built when a method is called. They contain the
 * chain of method implementations that are to be invoked by a particular
 * call, and the process of calling walks the chain, with the [next] command
 * proceeding to the next entry in the chain.
 */

#define CALL_CHAIN_STATIC_SIZE 4

struct MInvoke {
    Method *mPtr;		/* Reference to the method implementation
				 * record. */
    int isFilter;		/* Whether this is a filter invokation. */
};
typedef struct CallContext {
    Object *oPtr;		/* The object associated with this call. */
    int globalEpoch;		/* Global (class) epoch counter snapshot. */
    int localEpoch;		/* Local (single object) epoch counter
				 * snapshot. */
    int flags;			/* Assorted flags, see below. */
    int index;			/* Index into the call chain of the currently
				 * executing method implementation. */
    int skip;
    int numCallChain;		/* Size of the call chain. */
    struct MInvoke *callChain;	/* Array of call chain entries. May point to
				 * staticCallChain if the number of entries is
				 * small. */
    struct MInvoke staticCallChain[CALL_CHAIN_STATIC_SIZE];
    int filterLength;		/* Number of entries in the call chain that
				 * are due to processing filters and not the
				 * main call chain. */
} CallContext;

/*
 * Bits for the 'flags' field of the call context.
 */

#define OO_UNKNOWN_METHOD	1 /* This is an unknown method. */
#define PUBLIC_METHOD		2 /* This is a public (exported) method. */
#define CONSTRUCTOR		4 /* This is a constructor. */
#define DESTRUCTOR		8 /* This is a destructor. */

/*
 * Private definitions, some of which perhaps ought to be exposed properly or
 * maybe just put in the internal stubs table.
 */

MODULE_SCOPE Method *	TclNewProcMethod(Tcl_Interp *interp, Object *oPtr,
			    int isPublic, Tcl_Obj *nameObj, Tcl_Obj *argsObj,
			    Tcl_Obj *bodyObj);
MODULE_SCOPE Method *	TclNewForwardMethod(Tcl_Interp *interp, Object *oPtr,
			    int isPublic, Tcl_Obj *nameObj,
			    Tcl_Obj *prefixObj);
MODULE_SCOPE Method *	TclNewProcClassMethod(Tcl_Interp *interp,
			    Class *clsPtr, int isPublic, Tcl_Obj *nameObj,
			    Tcl_Obj *argsObj, Tcl_Obj *bodyObj);
MODULE_SCOPE Method *	TclNewForwardClassMethod(Tcl_Interp *interp,
			    Class *clsPtr, int isPublic, Tcl_Obj *nameObj,
			    Tcl_Obj *prefixObj);
MODULE_SCOPE void	TclDeleteMethod(Method *method);
MODULE_SCOPE int	TclObjInterpProcCore(register Tcl_Interp *interp,
			    CallFrame *framePtr, Tcl_Obj *procNameObj,
			    int skip);
// Expose this one?
MODULE_SCOPE void	TclOOAddToInstances(Object *oPtr, Class *clsPtr);
MODULE_SCOPE void	TclOOAddToSubclasses(Class *subPtr, Class *superPtr);
MODULE_SCOPE Proc *	TclOOGetProcFromMethod(Method *mPtr);
MODULE_SCOPE int	TclOOIsReachable(Class *targetPtr, Class *startPtr);
MODULE_SCOPE void	TclOORemoveFromInstances(Object *oPtr, Class *clsPtr);
MODULE_SCOPE void	TclOORemoveFromSubclasses(Class *subPtr,
			    Class *superPtr);

/*
 * A convenience macro for iterating through the lists used in the internal
 * memory management of objects. This is a bit gnarly because we want to do
 * the assignment of the picked-out value only when the body test succeeds,
 * but we cannot rely on the assigned value being useful, forcing us to do
 * some nasty stuff with the comma operator. The compiler's optimizer should
 * be able to sort it all out!
 */

#define FOREACH(var,ary) \
	for(i=0 ; (i<(ary).num?((var=(ary).list[i]),1):0) ; i++)

/*
 * Convenience macros for iterating through hash tables. FOREACH_HASH_DECLS
 * sets up the declarations needed for the main macro, FOREACH_HASH, which
 * does the actual iteration. FOREACH_HASH_VALUE is a restricted version that
 * only iterates over values.
 */

#define FOREACH_HASH_DECLS \
    Tcl_HashEntry *hPtr;Tcl_HashSearch search
#define FOREACH_HASH(key,val,tablePtr) \
    for(hPtr=Tcl_FirstHashEntry((tablePtr),&search); hPtr!=NULL ? \
	    ((key)=(void *)Tcl_GetHashKey((tablePtr),hPtr),\
	    (val)=Tcl_GetHashValue(hPtr),1):0; hPtr=Tcl_NextHashEntry(&search))
#define FOREACH_HASH_VALUE(val,tablePtr) \
    for(hPtr=Tcl_FirstHashEntry((tablePtr),&search); hPtr!=NULL ? \
	    ((val)=Tcl_GetHashValue(hPtr),1):0;hPtr=Tcl_NextHashEntry(&search))

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
