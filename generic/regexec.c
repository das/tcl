/*
 * re_*exec and friends - match REs
 */

#include "regguts.h"



/* internal variables, bundled for easy passing around */
struct vars {
	regex_t *re;
	struct guts *g;
	int eflags;		/* copies of arguments */
	size_t nmatch;
	regmatch_t *pmatch;
	chr *start;		/* start of string */
	chr *stop;		/* just past end of string */
	int err;		/* error code if any (0 none) */
	regoff_t *mem;		/* memory vector for backtracking */
};
#define	VISERR(vv)	((vv)->err != 0)	/* have we seen an error yet? */
#define	ISERR()	VISERR(v)
#define	VERR(vv,e)	(((vv)->err) ? (vv)->err : ((vv)->err = (e)))
#define	ERR(e)	VERR(v, e)		/* record an error */
#define	NOERR()	{if (ISERR()) return;}	/* if error seen, return */
#define	OFF(p)	((p) - v->start)
#define	LOFF(p)	((long)OFF(p))



/* lazy-DFA representation */
struct arcp {			/* "pointer" to an outarc */
	struct sset *ss;
	color co;
};

struct sset {			/* state set */
	unsigned *states;	/* pointer to bitvector */
	unsigned hash;		/* hash of bitvector */
#		define	HASH(bv, nw)	(((nw) == 1) ? *(bv) : hash(bv, nw))
#	define	HIT(h,bv,ss,nw)	((ss)->hash == (h) && ((nw) == 1 || \
		memcmp(VS(bv), VS((ss)->states), (nw)*sizeof(unsigned)) == 0))
	int flags;
#		define	STARTER		01	/* the initial state set */
#		define	POSTSTATE	02	/* includes the goal state */
#		define	LOCKED		04	/* locked in cache */
#		define	NOPROGRESS	010	/* zero-progress state set */
	struct arcp ins;	/* chain of inarcs pointing here */
	chr *lastseen;		/* last entered on arrival here */
	struct sset **outs;	/* outarc vector indexed by color */
	struct arcp *inchain;	/* chain-pointer vector for outarcs */
};

struct dfa {
	int nssets;		/* size of cache */
	int nssused;		/* how many entries occupied yet */
	int nstates;		/* number of states */
	int ncolors;		/* length of outarc and inchain vectors */
	int wordsper;		/* length of state-set bitvectors */
	struct sset *ssets;	/* state-set cache */
	unsigned *statesarea;	/* bitvector storage */
	unsigned *work;		/* pointer to work area within statesarea */
	struct sset **outsarea;	/* outarc-vector storage */
	struct arcp *incarea;	/* inchain storage */
	struct cnfa *cnfa;
	struct colormap *cm;
	chr *lastpost;		/* location of last cache-flushed success */
	chr *lastnopr;		/* location of last cache-flushed NOPROGRESS */
	struct sset *search;	/* replacement-search-pointer memory */
	int cptsmalloced;	/* were the areas individually malloced? */
	char *mallocarea;	/* self, or master malloced area, or NULL */
};

#define	WORK	1		/* number of work bitvectors needed */

/* setup for non-malloc allocation for small cases */
#define	FEWSTATES	20	/* must be less than UBITS */
#define	FEWCOLORS	15
struct smalldfa {
	struct dfa dfa;
	struct sset ssets[FEWSTATES*2];
	unsigned statesarea[FEWSTATES*2 + WORK];
	struct sset *outsarea[FEWSTATES*2 * FEWCOLORS];
	struct arcp incarea[FEWSTATES*2 * FEWCOLORS];
};




/*
 * forward declarations
 */
/* =====^!^===== begin forwards =====^!^===== */
/* automatically gathered by fwd; do not hand-edit */
/* === regexec.c === */
int exec _ANSI_ARGS_((regex_t *, CONST chr *, size_t, rm_detail_t *, size_t, regmatch_t [], int));
static int find _ANSI_ARGS_((struct vars *, struct cnfa *, struct colormap *));
static int cfind _ANSI_ARGS_((struct vars *, struct cnfa *, struct colormap *));
static VOID zapsubs _ANSI_ARGS_((regmatch_t *, size_t));
static VOID zapmem _ANSI_ARGS_((struct vars *, struct subre *));
static VOID subset _ANSI_ARGS_((struct vars *, struct subre *, chr *, chr *));
static int dissect _ANSI_ARGS_((struct vars *, struct subre *, chr *, chr *));
static int condissect _ANSI_ARGS_((struct vars *, struct subre *, chr *, chr *));
static int altdissect _ANSI_ARGS_((struct vars *, struct subre *, chr *, chr *));
static int cdissect _ANSI_ARGS_((struct vars *, struct subre *, chr *, chr *));
static int ccondissect _ANSI_ARGS_((struct vars *, struct subre *, chr *, chr *));
static int crevdissect _ANSI_ARGS_((struct vars *, struct subre *, chr *, chr *));
static int cbrdissect _ANSI_ARGS_((struct vars *, struct subre *, chr *, chr *));
static int caltdissect _ANSI_ARGS_((struct vars *, struct subre *, chr *, chr *));
/* === rege_dfa.c === */
static chr *longest _ANSI_ARGS_((struct vars *, struct dfa *, chr *, chr *));
static chr *shortest _ANSI_ARGS_((struct vars *, struct dfa *, chr *, chr *, chr *, chr **));
static struct dfa *newdfa _ANSI_ARGS_((struct vars *, struct cnfa *, struct colormap *, struct smalldfa *));
static VOID freedfa _ANSI_ARGS_((struct dfa *));
static unsigned hash _ANSI_ARGS_((unsigned *, int));
static struct sset *initialize _ANSI_ARGS_((struct vars *, struct dfa *, chr *));
static struct sset *miss _ANSI_ARGS_((struct vars *, struct dfa *, struct sset *, pcolor, chr *, chr *));
static int lacon _ANSI_ARGS_((struct vars *, struct cnfa *, chr *, pcolor));
static struct sset *getvacant _ANSI_ARGS_((struct vars *, struct dfa *, chr *, chr *));
static struct sset *pickss _ANSI_ARGS_((struct vars *, struct dfa *, chr *, chr *));
/* automatically gathered by fwd; do not hand-edit */
/* =====^!^===== end forwards =====^!^===== */



/*
 - exec - match regular expression
 ^ int exec(regex_t *, CONST chr *, size_t, rm_detail_t *,
 ^					size_t, regmatch_t [], int);
 */
int
exec(re, string, len, details, nmatch, pmatch, flags)
regex_t *re;
CONST chr *string;
size_t len;
rm_detail_t *details;		/* hook for future elaboration */
size_t nmatch;
regmatch_t pmatch[];
int flags;
{
	struct vars var;
	register struct vars *v = &var;
	int st;
	size_t n;
	int complications;
#	define	LOCALMAT	20
	regmatch_t mat[LOCALMAT];
#	define	LOCALMEM	40
	regoff_t mem[LOCALMEM];

	/* sanity checks */
	if (re == NULL || string == NULL || re->re_magic != REMAGIC)
		return REG_INVARG;
	if (re->re_csize != sizeof(chr))
		return REG_MIXED;

	/* setup */
	v->re = re;
	v->g = (struct guts *)re->re_guts;
	if (v->g->unmatchable)
		return REG_NOMATCH;
	complications = (v->g->info&REG_UBACKREF) ? 1 : 0;
	if (v->g->usedshorter)
		complications = 1;
	v->eflags = flags;
	if (v->g->cflags&REG_NOSUB)
		nmatch = 0;		/* override client */
	v->nmatch = nmatch;
	if (complications && v->nmatch < v->g->nsub + 1) {
		/* need work area bigger than what user gave us */
		if (v->g->nsub + 1 <= LOCALMAT)
			v->pmatch = mat;
		else
			v->pmatch = (regmatch_t *)MALLOC((v->g->nsub + 1) *
							sizeof(regmatch_t));
		if (v->pmatch == NULL)
			return REG_ESPACE;
		v->nmatch = v->g->nsub + 1;
	} else
		v->pmatch = pmatch;
	v->start = (chr *)string;
	v->stop = (chr *)string + len;
	v->err = 0;
	if (complications) {
		assert(v->g->ntree >= 0);
		n = (size_t)v->g->ntree;
		if (n <= LOCALMEM)
			v->mem = mem;
		else
			v->mem = (regoff_t *)MALLOC(n*sizeof(regoff_t));
		if (v->mem == NULL) {
			if (v->pmatch != pmatch && v->pmatch != mat)
				FREE(v->pmatch);
			return REG_ESPACE;
		}
	} else
		v->mem = NULL;

	/* do it */
	assert(v->g->tree != NULL);
	if (complications)
		st = cfind(v, &v->g->tree->cnfa, &v->g->cmap);
	else
		st = find(v, &v->g->tree->cnfa, &v->g->cmap);

	/* copy (portion of) match vector over if necessary */
	if (st == REG_OKAY && v->pmatch != pmatch && nmatch > 0) {
		zapsubs(pmatch, nmatch);
		n = (nmatch < v->nmatch) ? nmatch : v->nmatch;
		memcpy(VS(pmatch), VS(v->pmatch), n*sizeof(regmatch_t));
	}

	/* clean up */
	if (v->pmatch != pmatch && v->pmatch != mat)
		FREE(v->pmatch);
	if (v->mem != NULL && v->mem != mem)
		FREE(v->mem);
	return st;
}

/*
 - find - find a match for the main NFA (no-complications case)
 ^ static int find(struct vars *, struct cnfa *, struct colormap *);
 */
static int
find(v, cnfa, cm)
struct vars *v;
struct cnfa *cnfa;
struct colormap *cm;
{
	struct smalldfa da;
	struct dfa *d = newdfa(v, cnfa, cm, &da);
	struct smalldfa sa;
	struct dfa *s = newdfa(v, &v->g->search, cm, &sa);
	chr *begin;
	chr *end;
	chr *open;		/* open and close of range of possible starts */
	chr *close;

	if (d == NULL)
		return v->err;
	if (s == NULL) {
		freedfa(d);
		return v->err;
	}

	close = v->start;
	do {
		MDEBUG(("\nsearch at %ld\n", LOFF(close)));
		close = shortest(v, s, close, close, v->stop, &open);
		if (close == NULL)
			break;				/* NOTE BREAK */
		MDEBUG(("between %ld and %ld\n", LOFF(open), LOFF(close)));
		for (begin = open; begin <= close; begin++) {
			MDEBUG(("\nfind trying at %ld\n", LOFF(begin)));
			end = longest(v, d, begin, v->stop);
			if (end != NULL) {
				if (v->nmatch > 0) {
					v->pmatch[0].rm_so = OFF(begin);
					v->pmatch[0].rm_eo = OFF(end);
				}
				freedfa(d);
				freedfa(s);
				if (v->nmatch > 1) {
					zapsubs(v->pmatch, v->nmatch);
					return dissect(v, v->g->tree, begin,
									end);
				}
				if (ISERR())
					return v->err;
				return REG_OKAY;
			}
		}
	} while (close < v->stop);

	freedfa(d);
	freedfa(s);
	if (ISERR())
		return v->err;
	return REG_NOMATCH;
}

/*
 - cfind - find a match for the main NFA (with complications)
 ^ static int cfind(struct vars *, struct cnfa *, struct colormap *);
 */
static int
cfind(v, cnfa, cm)
struct vars *v;
struct cnfa *cnfa;
struct colormap *cm;
{
	struct smalldfa da;
	struct dfa *d = newdfa(v, cnfa, cm, &da);
	struct smalldfa sa;
	struct dfa *s = newdfa(v, &v->g->search, cm, &sa);
	chr *begin;
	chr *end;
	chr *open;		/* open and close of range of possible starts */
	chr *close;
	chr *estart;
	chr *estop;
	int er;
	int shorter = v->g->tree->flags&SHORTER;

	if (d == NULL)
		return v->err;
	if (s == NULL) {
		freedfa(d);
		return v->err;
	}

	close = v->start;
	do {
		MDEBUG(("\ncsearch at %ld\n", LOFF(close)));
		close = shortest(v, s, close, close, v->stop, &open);
		if (close == NULL)
			break;				/* NOTE BREAK */
		MDEBUG(("cbetween %ld and %ld\n", LOFF(open), LOFF(close)));
		for (begin = open; begin <= close; begin++) {
			MDEBUG(("\ncfind trying at %ld\n", LOFF(begin)));
			estart = begin;
			estop = v->stop;
			for (;;) {
				if (shorter)
					end = shortest(v, d, begin, estart,
							estop, (chr **)NULL);
				else
					end = longest(v, d, begin, estop);
				if (end == NULL)
					break;		/* NOTE BREAK OUT */
				MDEBUG(("tentative end %ld\n", LOFF(end)));
				zapsubs(v->pmatch, v->nmatch);
				zapmem(v, v->g->tree);
				er = cdissect(v, v->g->tree, begin, end);
				switch (er) {
				case REG_OKAY:
					if (v->nmatch > 0) {
						v->pmatch[0].rm_so = OFF(begin);
						v->pmatch[0].rm_eo = OFF(end);
					}
					freedfa(d);
					freedfa(s);
					if (ISERR())
						return v->err;
					return REG_OKAY;
					break;
				case REG_NOMATCH:
					/* go around and try again */
					if ((shorter) ? end == estop :
								end == begin) {
						/* no point in trying again */
						freedfa(s);
						freedfa(d);
						return REG_NOMATCH;
					}
					if (shorter)
						estart = end + 1;
					else
						estop = end - 1;
					break;
				default:
					freedfa(d);
					freedfa(s);
					return er;
					break;
				}
			}
		}
	} while (close < v->stop);

	freedfa(d);
	freedfa(s);
	if (ISERR())
		return v->err;
	return REG_NOMATCH;
}

/*
 - zapsubs - initialize the subexpression matches to "no match"
 ^ static VOID zapsubs(regmatch_t *, size_t);
 */
static VOID
zapsubs(p, n)
regmatch_t *p;
size_t n;
{
	size_t i;

	for (i = n-1; i > 0; i--) {
		p[i].rm_so = -1;
		p[i].rm_eo = -1;
	}
}

/*
 - zapmem - initialize the retry memory of a subtree to zeros
 ^ static VOID zapmem(struct vars *, struct subre *);
 */
static VOID
zapmem(v, t)
struct vars *v;
struct subre *t;
{
	if (t == NULL)
		return;

	assert(v->mem != NULL);
	v->mem[t->retry] = 0;
	if (t->op == '(') {
		assert(t->subno > 0);
		v->pmatch[t->subno].rm_so = -1;
		v->pmatch[t->subno].rm_eo = -1;
	}

	if (t->left != NULL)
		zapmem(v, t->left);
	if (t->right != NULL)
		zapmem(v, t->right);
}

/*
 - subset - set any subexpression relevant to a successful subre
 ^ static VOID subset(struct vars *, struct subre *, chr *, chr *);
 */
static VOID
subset(v, sub, begin, end)
struct vars *v;
struct subre *sub;
chr *begin;
chr *end;
{
	int n = sub->subno;

	assert(n > 0);
	if ((size_t)n >= v->nmatch)
		return;

	MDEBUG(("setting %d\n", n));
	v->pmatch[n].rm_so = OFF(begin);
	v->pmatch[n].rm_eo = OFF(end);
}

/*
 - dissect - determine subexpression matches (uncomplicated case)
 ^ static int dissect(struct vars *, struct subre *, chr *, chr *);
 */
static int			/* regexec return code */
dissect(v, t, begin, end)
struct vars *v;
struct subre *t;
chr *begin;			/* beginning of relevant substring */
chr *end;			/* end of same */
{
	assert(t != NULL);
	MDEBUG(("dissect %ld-%ld\n", LOFF(begin), LOFF(end)));

	switch (t->op) {
	case '=':		/* terminal node */
		assert(t->left == NULL && t->right == NULL);
		return REG_OKAY;	/* no action, parent did the work */
		break;
	case '|':		/* alternation */
		assert(t->left != NULL);
		return altdissect(v, t, begin, end);
		break;
	case 'b':		/* back ref -- shouldn't be calling us! */
		return REG_ASSERT;
		break;
	case '.':		/* concatenation */
		assert(t->left != NULL && t->right != NULL);
		return condissect(v, t, begin, end);
		break;
	case '(':		/* capturing */
		assert(t->left != NULL && t->right == NULL);
		assert(t->subno > 0);
		subset(v, t, begin, end);
		return dissect(v, t->left, begin, end);
		break;
	default:
		return REG_ASSERT;
		break;
	}
}

/*
 - condissect - determine concatenation subexpression matches (uncomplicated)
 ^ static int condissect(struct vars *, struct subre *, chr *, chr *);
 */
static int			/* regexec return code */
condissect(v, t, begin, end)
struct vars *v;
struct subre *t;
chr *begin;			/* beginning of relevant substring */
chr *end;			/* end of same */
{
	struct smalldfa da;
	struct dfa *d;
	struct smalldfa d2a;
	struct dfa *d2;
	chr *mid;
	int i;

	assert(t->op == '.');
	assert(t->left != NULL && t->left->cnfa.nstates > 0);
	assert(t->right != NULL && t->right->cnfa.nstates > 0);

	d = newdfa(v, &t->left->cnfa, &v->g->cmap, &da);
	if (ISERR())
		return v->err;
	d2 = newdfa(v, &t->right->cnfa, &v->g->cmap, &d2a);
	if (ISERR()) {
		freedfa(d);
		return v->err;
	}

	/* pick a tentative midpoint */
	mid = longest(v, d, begin, end);
	if (mid == NULL) {
		freedfa(d);
		freedfa(d2);
		return REG_ASSERT;
	}
	MDEBUG(("tentative midpoint %ld\n", LOFF(mid)));

	/* iterate until satisfaction or failure */
	while (longest(v, d2, mid, end) != end) {
		/* that midpoint didn't work, find a new one */
		if (mid == begin) {
			/* all possibilities exhausted! */
			MDEBUG(("no midpoint!\n"));
			freedfa(d);
			freedfa(d2);
			return REG_ASSERT;
		}
		mid = longest(v, d, begin, mid-1);
		if (mid == NULL) {
			/* failed to find a new one! */
			MDEBUG(("failed midpoint!\n"));
			freedfa(d);
			freedfa(d2);
			return REG_ASSERT;
		}
		MDEBUG(("new midpoint %ld\n", LOFF(mid)));
	}

	/* satisfaction */
	MDEBUG(("successful\n"));
	freedfa(d);
	freedfa(d2);
	i = dissect(v, t->left, begin, mid);
	if (i != REG_OKAY)
		return i;
	return dissect(v, t->right, mid, end);
}

/*
 - altdissect - determine alternative subexpression matches (uncomplicated)
 ^ static int altdissect(struct vars *, struct subre *, chr *, chr *);
 */
static int			/* regexec return code */
altdissect(v, t, begin, end)
struct vars *v;
struct subre *t;
chr *begin;			/* beginning of relevant substring */
chr *end;			/* end of same */
{
	struct smalldfa da;
	struct dfa *d;
	int i;

	assert(t != NULL);
	assert(t->op == '|');

	for (i = 0; t != NULL; t = t->right, i++) {
		MDEBUG(("trying %dth\n", i));
		assert(t->left != NULL && t->left->cnfa.nstates > 0);
		d = newdfa(v, &t->left->cnfa, &v->g->cmap, &da);
		if (ISERR())
			return v->err;
		if (longest(v, d, begin, end) == end) {
			MDEBUG(("success\n"));
			freedfa(d);
			return dissect(v, t->left, begin, end);
		}
		freedfa(d);
	}
	return REG_ASSERT;	/* none of them matched?!? */
}

/*
 - cdissect - determine subexpression matches (with complications)
 * The retry memory stores the offset of the trial midpoint from begin, 
 * plus 1 so that 0 uniquely means "clean slate".
 ^ static int cdissect(struct vars *, struct subre *, chr *, chr *);
 */
static int			/* regexec return code */
cdissect(v, t, begin, end)
struct vars *v;
struct subre *t;
chr *begin;			/* beginning of relevant substring */
chr *end;			/* end of same */
{
	int er;

	assert(t != NULL);
	MDEBUG(("cdissect %ld-%ld\n", LOFF(begin), LOFF(end)));

	switch (t->op) {
	case '=':		/* terminal node */
		assert(t->left == NULL && t->right == NULL);
		return REG_OKAY;	/* no action, parent did the work */
		break;
	case '|':		/* alternation */
		assert(t->left != NULL);
		return caltdissect(v, t, begin, end);
		break;
	case 'b':		/* back ref -- shouldn't be calling us! */
		assert(t->left == NULL && t->right == NULL);
		return cbrdissect(v, t, begin, end);
		break;
	case '.':		/* concatenation */
		assert(t->left != NULL && t->right != NULL);
		return ccondissect(v, t, begin, end);
		break;
	case '(':		/* capturing */
		assert(t->left != NULL && t->right == NULL);
		assert(t->subno > 0);
		er = cdissect(v, t->left, begin, end);
		if (er == REG_OKAY)
			subset(v, t, begin, end);
		return er;
		break;
	default:
		return REG_ASSERT;
		break;
	}
}

/*
 - ccondissect - concatenation subexpression matches (with complications)
 * The retry memory stores the offset of the trial midpoint from begin, 
 * plus 1 so that 0 uniquely means "clean slate".
 ^ static int ccondissect(struct vars *, struct subre *, chr *, chr *);
 */
static int			/* regexec return code */
ccondissect(v, t, begin, end)
struct vars *v;
struct subre *t;
chr *begin;			/* beginning of relevant substring */
chr *end;			/* end of same */
{
	struct smalldfa da;
	struct dfa *d;
	struct smalldfa d2a;
	struct dfa *d2;
	chr *mid;
	int er;

	assert(t->op == '.');
	assert(t->left != NULL && t->left->cnfa.nstates > 0);
	assert(t->right != NULL && t->right->cnfa.nstates > 0);

	if (t->left->flags&SHORTER)		/* reverse scan */
		return crevdissect(v, t, begin, end);

	d = newdfa(v, &t->left->cnfa, &v->g->cmap, &da);
	if (ISERR())
		return v->err;
	d2 = newdfa(v, &t->right->cnfa, &v->g->cmap, &d2a);
	if (ISERR()) {
		freedfa(d);
		return v->err;
	}
	MDEBUG(("cconcat %d\n", t->retry));

	/* pick a tentative midpoint */
	if (v->mem[t->retry] == 0) {
		mid = longest(v, d, begin, end);
		if (mid == NULL) {
			freedfa(d);
			freedfa(d2);
			return REG_NOMATCH;
		}
		MDEBUG(("tentative midpoint %ld\n", LOFF(mid)));
		v->mem[t->retry] = (mid - begin) + 1;
	} else {
		mid = begin + (v->mem[t->retry] - 1);
		MDEBUG(("working midpoint %ld\n", LOFF(mid)));
	}

	/* iterate until satisfaction or failure */
	for (;;) {
		/* try this midpoint on for size */
		er = cdissect(v, t->left, begin, mid);
		if (er == REG_OKAY && longest(v, d2, mid, end) == end &&
				(er = cdissect(v, t->right, mid, end)) == 
								REG_OKAY)
			break;			/* NOTE BREAK OUT */
		if (er != REG_OKAY && er != REG_NOMATCH) {
			freedfa(d);
			freedfa(d2);
			return er;
		}

		/* that midpoint didn't work, find a new one */
		if (mid == begin) {
			/* all possibilities exhausted */
			MDEBUG(("%d no midpoint\n", t->retry));
			freedfa(d);
			freedfa(d2);
			return REG_NOMATCH;
		}
		mid = longest(v, d, begin, mid-1);
		if (mid == NULL) {
			/* failed to find a new one */
			MDEBUG(("%d failed midpoint\n", t->retry));
			freedfa(d);
			freedfa(d2);
			return REG_NOMATCH;
		}
		MDEBUG(("%d: new midpoint %ld\n", t->retry, LOFF(mid)));
		v->mem[t->retry] = (mid - begin) + 1;
		zapmem(v, t->left);
		zapmem(v, t->right);
	}

	/* satisfaction */
	MDEBUG(("successful\n"));
	freedfa(d);
	freedfa(d2);
	return REG_OKAY;
}

/*
 - crevdissect - determine shortest-first subexpression matches
 * The retry memory stores the offset of the trial midpoint from begin, 
 * plus 1 so that 0 uniquely means "clean slate".
 ^ static int crevdissect(struct vars *, struct subre *, chr *, chr *);
 */
static int			/* regexec return code */
crevdissect(v, t, begin, end)
struct vars *v;
struct subre *t;
chr *begin;			/* beginning of relevant substring */
chr *end;			/* end of same */
{
	struct smalldfa da;
	struct dfa *d;
	struct smalldfa d2a;
	struct dfa *d2;
	chr *mid;
	int er;

	assert(t->op == '.');
	assert(t->left != NULL && t->left->cnfa.nstates > 0);
	assert(t->right != NULL && t->right->cnfa.nstates > 0);
	assert(t->left->flags&SHORTER);

	/* concatenation -- need to split the substring between parts */
	d = newdfa(v, &t->left->cnfa, &v->g->cmap, &da);
	if (ISERR())
		return v->err;
	d2 = newdfa(v, &t->right->cnfa, &v->g->cmap, &d2a);
	if (ISERR()) {
		freedfa(d);
		return v->err;
	}
	MDEBUG(("crev %d\n", t->retry));

	/* pick a tentative midpoint */
	if (v->mem[t->retry] == 0) {
		mid = shortest(v, d, begin, begin, end, (chr **)NULL);
		if (mid == NULL) {
			freedfa(d);
			freedfa(d2);
			return REG_NOMATCH;
		}
		MDEBUG(("tentative midpoint %ld\n", LOFF(mid)));
		v->mem[t->retry] = (mid - begin) + 1;
	} else {
		mid = begin + (v->mem[t->retry] - 1);
		MDEBUG(("working midpoint %ld\n", LOFF(mid)));
	}

	/* iterate until satisfaction or failure */
	for (;;) {
		/* try this midpoint on for size */
		er = cdissect(v, t->left, begin, mid);
		if (er == REG_OKAY && longest(v, d2, mid, end) == end &&
				(er = cdissect(v, t->right, mid, end)) == 
								REG_OKAY)
			break;			/* NOTE BREAK OUT */
		if (er != REG_OKAY && er != REG_NOMATCH) {
			freedfa(d);
			freedfa(d2);
			return er;
		}

		/* that midpoint didn't work, find a new one */
		if (mid == end) {
			/* all possibilities exhausted */
			MDEBUG(("%d no midpoint\n", t->retry));
			freedfa(d);
			freedfa(d2);
			return REG_NOMATCH;
		}
		mid = shortest(v, d, begin, mid+1, end, (chr **)NULL);
		if (mid == NULL) {
			/* failed to find a new one */
			MDEBUG(("%d failed midpoint\n", t->retry));
			freedfa(d);
			freedfa(d2);
			return REG_NOMATCH;
		}
		MDEBUG(("%d: new midpoint %ld\n", t->retry, LOFF(mid)));
		v->mem[t->retry] = (mid - begin) + 1;
		zapmem(v, t->left);
		zapmem(v, t->right);
	}

	/* satisfaction */
	MDEBUG(("successful\n"));
	freedfa(d);
	freedfa(d2);
	return REG_OKAY;
}

/*
 - cbrdissect - determine backref subexpression matches
 ^ static int cbrdissect(struct vars *, struct subre *, chr *, chr *);
 */
static int			/* regexec return code */
cbrdissect(v, t, begin, end)
struct vars *v;
struct subre *t;
chr *begin;			/* beginning of relevant substring */
chr *end;			/* end of same */
{
	int i;
	int n = t->subno;
	size_t len;
	chr *paren;
	chr *p;
	chr *stop;
	int min = t->min;
	int max = t->max;

	assert(t != NULL);
	assert(t->op == 'b');
	assert(n >= 0);
	assert((size_t)n < v->nmatch);

	MDEBUG(("cbackref n%d %d{%d-%d}\n", t->retry, n, min, max));

	if (v->pmatch[n].rm_so == -1)
		return REG_NOMATCH;
	paren = v->start + v->pmatch[n].rm_so;
	len = v->pmatch[n].rm_eo - v->pmatch[n].rm_so;

	/* no room to maneuver -- retries are pointless */
	if (v->mem[t->retry])
		return REG_NOMATCH;
	v->mem[t->retry] = 1;

	/* special-case zero-length string */
	if (len == 0) {
		if (begin == end)
			return REG_OKAY;
		return REG_NOMATCH;
	}

	/* and too-short string */
	assert(end >= begin);
	if ((size_t)(end - begin) < len)
		return REG_NOMATCH;
	stop = end - len;

	/* count occurrences */
	i = 0;
	for (p = begin; p <= stop && (i < max || max == INFINITY); p += len) {
		if ((*v->g->compare)(paren, p, len) != 0)
				break;
		i++;
	}
	MDEBUG(("cbackref found %d\n", i));

	/* and sort it out */
	if (p != end)			/* didn't consume all of it */
		return REG_NOMATCH;
	if (min <= i && (i <= max || max == INFINITY))
		return REG_OKAY;
	return REG_NOMATCH;		/* out of range */
}

/*
 - caltdissect - determine alternative subexpression matches (w. complications)
 ^ static int caltdissect(struct vars *, struct subre *, chr *, chr *);
 */
static int			/* regexec return code */
caltdissect(v, t, begin, end)
struct vars *v;
struct subre *t;
chr *begin;			/* beginning of relevant substring */
chr *end;			/* end of same */
{
	struct smalldfa da;
	struct dfa *d;
	int er;
#	define	UNTRIED	0	/* not yet tried at all */
#	define	TRYING	1	/* top matched, trying submatches */
#	define	TRIED	2	/* top didn't match or submatches exhausted */

	if (t == NULL)
		return REG_NOMATCH;
	assert(t->op == '|');
	if (v->mem[t->retry] == TRIED)
		return caltdissect(v, t->right, begin, end);

	MDEBUG(("calt n%d\n", t->retry));
	assert(t->left != NULL);

	if (v->mem[t->retry] == UNTRIED) {
		d = newdfa(v, &t->left->cnfa, &v->g->cmap, &da);
		if (ISERR())
			return v->err;
		if (longest(v, d, begin, end) != end) {
			freedfa(d);
			v->mem[t->retry] = TRIED;
			return caltdissect(v, t->right, begin, end);
		}
		freedfa(d);
		MDEBUG(("calt matched\n"));
		v->mem[t->retry] = TRYING;
	}

	er = cdissect(v, t->left, begin, end);
	if (er != REG_NOMATCH)
		return er;

	v->mem[t->retry] = TRIED;
	return caltdissect(v, t->right, begin, end);
}



#include "rege_dfa.c"
