/*
 * zle_tricky.c - expansion and completion
 *
 * This file is part of zsh, the Z shell.
 *
 * Copyright (c) 1992-1997 Paul Falstad
 * All rights reserved.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and to distribute modified versions of this software for any
 * purpose, provided that the above copyright notice and the following
 * two paragraphs appear in all copies of this software.
 *
 * In no event shall Paul Falstad or the Zsh Development Group be liable
 * to any party for direct, indirect, special, incidental, or consequential
 * damages arising out of the use of this software and its documentation,
 * even if Paul Falstad and the Zsh Development Group have been advised of
 * the possibility of such damage.
 *
 * Paul Falstad and the Zsh Development Group specifically disclaim any
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose.  The software
 * provided hereunder is on an "as is" basis, and Paul Falstad and the
 * Zsh Development Group have no obligation to provide maintenance,
 * support, updates, enhancements, or modifications.
 *
 */

#include "zle.mdh"
#include "zle_tricky.pro"

/*
 * The main part of ZLE maintains the line being edited as binary data,
 * but here, where we interface with the lexer and other bits of zsh, we
 * need the line metafied and, if necessary, converted from wide
 * characters into multibyte strings.  On entry to the
 * expansion/completion system, we metafy the line from zleline into
 * zlemetaline, with zlell and zlecs adjusted into zlemetall zlemetacs
 * to match.  zlemetall and zlemetacs refer to raw character positions,
 * in other words a metafied character contributes 2 to each.  All
 * completion and expansion is done on the metafied line.  Immediately
 * before returning, the line is unmetafied again, so that zleline,
 * zlell and zlecs are once again valid.  (zlell and zlecs might have
 * changed during completion, so they can't be merely saved and
 * restored.)  The various indexes into the line that are used in this
 * file only are not translated: they remain indexes into the metafied
 * line.
 *
 * zlemetaline is always NULL when not in use and non-NULL when in use.
 * This can be used to test if the line is metafied.  It would be
 * possible to use zlecs and zlell directly, updated as appropriate when
 * metafying and unmetafying, instead of zlemetacs and zlemetall,
 * however the current system seems clearer.
 */

#define inststr(X) inststrlen((X),1,-1)

/*
 * The state of the line being edited between metafy_line()
 * unmetafy_line().
 *
 * zlemetacs and zlemetall are defined in lex.c.
 */
/**/
mod_export char *zlemetaline;
/**/
mod_export int metalinesz;

/* The line before completion was tried. */

/**/
mod_export char *origline;
/**/
mod_export int origcs, origll;

/* Words on the command line, for use in completion */
 
/**/
mod_export int clwsize, clwnum, clwpos;
/**/
mod_export char **clwords;

/* offs is the cursor position within the tokenized *
 * current word after removing nulargs.             */

/**/
mod_export int offs;

/* These control the type of completion that will be done.  They are       *
 * affected by the choice of ZLE command and by relevant shell options.    *
 * usemenu is set to 2 if we have to start automenu and 3 if we have to    *
 * insert a match as if for menucompletion but without really starting it. */

/**/
mod_export int usemenu, useglob;

/* != 0 if we would insert a TAB if we weren't calling a completion widget. */

/**/
mod_export int wouldinstab;

/* != 0 if we are in the middle of a menu completion. May be == 2 to force *
 * menu completion even if using different widgets.                        */

/**/
mod_export int menucmp;

/* Lists of brace-infos before/after cursor (first and last for each). */

/**/
mod_export Brinfo brbeg, lastbrbeg, brend, lastbrend;

/**/
mod_export int nbrbeg, nbrend;

/**/
mod_export char *lastprebr, *lastpostbr;

/* !=0 if we have a valid completion list. */

/**/
mod_export int validlist;

/* Non-zero if we have to redisplay the list of matches. */

/**/
mod_export int showagain = 0;

/* This holds the word we are working on without braces removed. */

static char *origword;

/* The quoted prefix/suffix and a flag saying if we want to add the
 * closing quote. */

/**/
mod_export char *qipre, *qisuf, *autoq;

/* This contains the name of the function to call if this is for a new  *
 * style completion. */

/**/
mod_export char *compfunc = NULL;

/* Non-zero if the last completion done was ambiguous (used to find   *
 * out if AUTOMENU should start).  More precisely, it's nonzero after *
 * successfully doing any completion, unless the completion was       *
 * unambiguous and did not cause the display of a completion list.    *
 * From the other point of view, it's nonzero iff AUTOMENU (if set)   *
 * should kick in on another completion.                              *
 *                                                                    *
 * If both AUTOMENU and BASHAUTOLIST are set, then we get a listing   *
 * on the second tab, a` la bash, and then automenu kicks in when     *
 * lastambig == 2.                                                    */

/**/
mod_export int lastambig, bashlistfirst;

/* Arguments for and return value of completion widget. */

/**/
mod_export char **cfargs;
/**/
mod_export int cfret;

/* != 0 if recursive calls to completion are (temporarily) allowed */

/**/
mod_export int comprecursive;

/* != 0 if there are any defined completion widgets. */

/**/
int hascompwidgets;

/*
 * Find out if we have to insert a tab (instead of trying to complete).
 * The line is not metafied here.
 */

/**/
static int
usetab(void)
{
    ZLE_STRING_T s = zleline + zlecs - 1;

    if (keybuf[0] != '\t' || keybuf[1])
	return 0;
    for (; s >= zleline && *s != ZWC('\n'); s--)
	if (*s != ZWC('\t') && *s != ZWC(' '))
	    return 0;
    if (compfunc) {
	wouldinstab = 1;

	return 0;
    }
    return 1;
}

/**/
int
completecall(char **args)
{
    cfargs = args;
    cfret = 0;
    compfunc = compwidget->u.comp.func;
    if (compwidget->u.comp.fn(zlenoargs) && !cfret)
	cfret = 1;
    compfunc = NULL;

    return cfret;
}

/**/
int
completeword(char **args)
{
    usemenu = !!isset(MENUCOMPLETE);
    useglob = isset(GLOBCOMPLETE);
    wouldinstab = 0;
    if (lastchar == '\t' && usetab())
	return selfinsert(args);
    else {
	int ret;
	if (lastambig == 1 && isset(BASHAUTOLIST) && !usemenu && !menucmp) {
	    bashlistfirst = 1;
	    ret = docomplete(COMP_LIST_COMPLETE);
	    bashlistfirst = 0;
	    lastambig = 2;
	} else
	    ret = docomplete(COMP_COMPLETE);
	return ret;
    }
}

/**/
mod_export int
menucomplete(char **args)
{
    usemenu = 1;
    useglob = isset(GLOBCOMPLETE);
    wouldinstab = 0;
    if (lastchar == '\t' && usetab())
	return selfinsert(args);
    else
	return docomplete(COMP_COMPLETE);
}

/**/
int
listchoices(UNUSED(char **args))
{
    usemenu = !!isset(MENUCOMPLETE);
    useglob = isset(GLOBCOMPLETE);
    wouldinstab = 0;
    return docomplete(COMP_LIST_COMPLETE);
}

/**/
int
spellword(UNUSED(char **args))
{
    usemenu = useglob = 0;
    wouldinstab = 0;
    return docomplete(COMP_SPELL);
}

/**/
int
deletecharorlist(char **args)
{
    usemenu = !!isset(MENUCOMPLETE);
    useglob = isset(GLOBCOMPLETE);
    wouldinstab = 0;

    /* Line not yet metafied */
    if (zlecs != zlell) {
	fixsuffix();
	invalidatelist();
	return deletechar(args);
    }
    return docomplete(COMP_LIST_COMPLETE);
}

/**/
int
expandword(char **args)
{
    usemenu = useglob = 0;
    wouldinstab = 0;
    if (lastchar == '\t' && usetab())
	return selfinsert(args);
    else
	return docomplete(COMP_EXPAND);
}

/**/
int
expandorcomplete(char **args)
{
    usemenu = !!isset(MENUCOMPLETE);
    useglob = isset(GLOBCOMPLETE);
    wouldinstab = 0;
    if (lastchar == '\t' && usetab())
	return selfinsert(args);
    else {
	int ret;
	if (lastambig == 1 && isset(BASHAUTOLIST) && !usemenu && !menucmp) {
	    bashlistfirst = 1;
	    ret = docomplete(COMP_LIST_COMPLETE);
	    bashlistfirst = 0;
	    lastambig = 2;
	} else
	    ret = docomplete(COMP_EXPAND_COMPLETE);
	return ret;
    }
}

/**/
int
menuexpandorcomplete(char **args)
{
    usemenu = 1;
    useglob = isset(GLOBCOMPLETE);
    wouldinstab = 0;
    if (lastchar == '\t' && usetab())
	return selfinsert(args);
    else
	return docomplete(COMP_EXPAND_COMPLETE);
}

/**/
int
listexpand(UNUSED(char **args))
{
    usemenu = !!isset(MENUCOMPLETE);
    useglob = isset(GLOBCOMPLETE);
    wouldinstab = 0;
    return docomplete(COMP_LIST_EXPAND);
}

/**/
mod_export int
reversemenucomplete(char **args)
{
    wouldinstab = 0;
    if (!menucmp)
	return menucomplete(args);

    runhookdef(REVERSEMENUHOOK, NULL);
    return 0;
}

/**/
int
acceptandmenucomplete(char **args)
{
    wouldinstab = 0;
    if (!menucmp)
	return 1;
    runhookdef(ACCEPTCOMPHOOK, NULL);
    return menucomplete(args);
}

/* These are flags saying if we are completing in the command *
 * position, in a redirection, or in a parameter expansion.   */

/**/
mod_export int lincmd, linredir, linarr;

/* The string for the redirection operator. */

/**/
mod_export char *rdstr;

static char rdstrbuf[20];

/* The list of redirections on the line. */

/**/
mod_export LinkList rdstrs;

/* This holds the name of the current command (used to find the right *
 * compctl).                                                          */

/**/
mod_export char *cmdstr;

/* This hold the name of the variable we are working on. */

/**/
mod_export char *varname;

/* != 0 if we are in a subscript */

/**/
mod_export int insubscr;

/* Parameter pointer for completing keys of an assoc array. */

/**/
mod_export Param keypm;

/* 1 if we are completing in a quoted string (or inside `...`) */

/**/
mod_export int instring, inbackt;

/* Convenience macro for calling bslashquote() (formerly quotename()). *
 * This uses the instring variable above.                              */

#define quotename(s, e) bslashquote(s, e, instring)

/* Check if the given string is the name of a parameter and if this *
 * parameter is one worth expanding.                                */

/**/
static int
checkparams(char *p)
{
    int t0, n, l = strlen(p), e = 0;
    struct hashnode *hn;

    for (t0 = paramtab->hsize - 1, n = 0; n < 2 && t0 >= 0; t0--)
	for (hn = paramtab->nodes[t0]; n < 2 && hn; hn = hn->next)
	    if (pfxlen(p, hn->nam) == l) {
		n++;
		if ((int)strlen(hn->nam) == l)
		    e = 1;
	    }
    return (n == 1) ? (getsparam(p) != NULL) :
	(!menucmp && e && (!hascompmod || isset(RECEXACT)));
}

/* Check if the given string has wildcards.  The difficulty is that we *
 * have to treat things like job specifications (%...) and parameter   *
 * expressions correctly.                                              */

/**/
static int
cmphaswilds(char *str)
{
    if ((*str == Inbrack || *str == Outbrack) && !str[1])
	return 0;

    /* If a leading % is immediately followed by ?, then don't *
     * treat that ? as a wildcard.  This is so you don't have  *
     * to escape job references such as %?foo.                 */
    if (str[0] == '%' && str[1] ==Quest)
	str += 2;

    for (; *str;) {
	if (*str == String || *str == Qstring) {
	    /* A parameter expression. */

	    if (*++str == Inbrace)
		skipparens(Inbrace, Outbrace, &str);
	    else if (*str == String || *str == Qstring)
		str++;
	    else {
		/* Skip all the things a parameter expression might start *
		 * with (before we come to the parameter name).           */
		for (; *str; str++)
		    if (*str != '^' && *str != Hat &&
			*str != '=' && *str != Equals &&
			*str != '~' && *str != Tilde)
			break;
		if (*str == '#' || *str == Pound)
		    str++;
		/* Star and Quest are parameter names here, not wildcards */
		if (*str == Star || *str == Quest)
		    str++;
	    }
	} else {
	    /* Not a parameter expression so we check for wildcards */
	    if (((*str == Pound || *str == Hat) && isset(EXTENDEDGLOB)) ||
		*str == Star || *str == Bar || *str == Quest ||
		!skipparens(Inbrack, Outbrack, &str) ||
		!skipparens(Inang,   Outang,   &str) ||
		(unset(IGNOREBRACES) &&
		 !skipparens(Inbrace, Outbrace, &str)) ||
		(*str == Inpar && str[1] == ':' &&
		 !skipparens(Inpar, Outpar, &str)))
		return 1;
	    if (*str)
		str++;
	}
    }
    return 0;
}

/* Check if we have to complete a parameter name. */

/**/
char *
parambeg(char *s)
{
    char *p;

    /* Try to find a `$'. */
    for (p = s + offs; p > s && *p != String && *p != Qstring; p--);
    if (*p == String || *p == Qstring) {
	/* Handle $$'s */
	while (p > s && (p[-1] == String || p[-1] == Qstring))
	    p--;
	while ((p[1] == String || p[1] == Qstring) &&
	       (p[2] == String || p[2] == Qstring))
	    p += 2;
    }
    if ((*p == String || *p == Qstring) && p[1] != Inpar && p[1] != Inbrack) {
	/* This is really a parameter expression (not $(...) or $[...]). */
	char *b = p + 1, *e = b;
	int n = 0, br = 1, nest = 0;

	if (*b == Inbrace) {
	    char *tb = b;

	    /* If this is a ${...}, see if we are before the '}'. */
	    if (!skipparens(Inbrace, Outbrace, &tb))
		return NULL;

	    /* Ignore the possible (...) flags. */
	    b++, br++;
	    n = skipparens(Inpar, Outpar, &b);

	    for (tb = p - 1; tb > s && *tb != Outbrace && *tb != Inbrace; tb--);
	    if (tb > s && *tb == Inbrace && (tb[-1] == String || *tb == Qstring))
		nest = 1;
	}

	/* Ignore the stuff before the parameter name. */
	for (; *b; b++)
	    if (*b != '^' && *b != Hat &&
		*b != '=' && *b != Equals &&
		*b != '~' && *b != Tilde)
		break;
	if (*b == '#' || *b == Pound || *b == '+')
	    b++;

	e = b;
	if (br) {
	    while (*e == Dnull)
		e++;
	}
	/* Find the end of the name. */
	if (*e == Quest || *e == Star || *e == String || *e == Qstring ||
	    *e == '?'   || *e == '*'  || *e == '$'    ||
	    *e == '-'   || *e == '!'  || *e == '@')
	    e++;
	else if (idigit(*e))
	    while (idigit(*e))
		e++;
	else
	    e = itype_end(e, IIDENT, 0);

	/* Now make sure that the cursor is inside the name. */
	if (offs <= e - s && offs >= b - s && n <= 0) {
	    if (br) {
		p = e;
		while (*p == Dnull)
		    p++;
	    }
	    /* It is. */
	    return b;
	}
    }
    return NULL;
}

/* The main entry point for completion. */

/**/
static int
docomplete(int lst)
{
    static int active = 0;

    char *s, *ol;
    int olst = lst, chl = 0, ne = noerrs, ocs, ret = 0, dat[2];

    if (active && !comprecursive) {
	zwarn("completion cannot be used recursively (yet)");
	return 1;
    }
    active = 1;
    comprecursive = 0;
    makecommaspecial(0);
    if (undoing)
	setlastline();

    /* From the C-code's point of view, we can only use compctl as a default
     * type of completion. Load it if it hasn't been loaded already and
     * no completion widgets are defined. */

    if (!module_loaded("zsh/compctl") && !hascompwidgets)
	load_module("zsh/compctl");

    if (runhookdef(BEFORECOMPLETEHOOK, (void *) &lst)) {
	active = 0;
	return 0;
    }
    /* Expand history references before starting completion.  If anything *
     * changed, do no more.                                               */

    if (doexpandhist()) {
	active = 0;
	return 0;
    }

    metafy_line();

    ocs = zlemetacs;
    origline = dupstring(zlemetaline);
    origcs = zlemetacs;
    origll = zlemetall;
    if (!isfirstln && chline != NULL) {
	/* If we are completing in a multi-line buffer (which was not  *
	 * taken from the history), we have to prepend the stuff saved *
	 * in chline to the contents of line.                          */

	ol = dupstring(zlemetaline);
	/* Make sure that chline is zero-terminated. */
	*hptr = '\0';
	zlemetacs = 0;
	inststr(chline);
	chl = zlemetacs;
	zlemetacs += ocs;
    } else
	ol = NULL;
    inwhat = IN_NOTHING;
    zsfree(qipre);
    qipre = ztrdup("");
    zsfree(qisuf);
    qisuf = ztrdup("");
    zsfree(autoq);
    autoq = NULL;
    /* Get the word to complete.
     * NOTE: get_comp_string() calls pushheap(), but not popheap(). */
    noerrs = 1;
    s = get_comp_string();
    DPUTS(wb < 0 || zlemetacs < wb || zlemetacs > we,
	  "BUG: 0 <= wb <= zlemetacs <= we is not true!");
    noerrs = ne;
    /* For vi mode, reset the start-of-insertion pointer to the beginning *
     * of the word being completed, if it is currently later.  Vi itself  *
     * would never change the pointer in the middle of an insertion, but  *
     * then vi doesn't have completion.  More to the point, this is only  *
     * an emulation.                                                      */
    if (viinsbegin > ztrsub(zlemetaline + wb, zlemetaline))
	viinsbegin = ztrsub(zlemetaline + wb, zlemetaline);
    /* If we added chline to the line buffer, reset the original contents. */
    if (ol) {
	zlemetacs -= chl;
	wb -= chl;
	we -= chl;
	if (wb < 0) {
	    strcpy(zlemetaline, ol);
	    zlemetall = strlen(zlemetaline);
	    zlemetacs = ocs;
	    popheap();
	    unmetafy_line();
	    zsfree(s);
	    active = 0;
	    makecommaspecial(0);
	    return 1;
	}
	ocs = zlemetacs;
	zlemetacs = 0;
	foredel(chl);
	zlemetacs = ocs;
    }
    freeheap();
    /* Save the lexer state, in case the completion code uses the lexer *
     * somewhere (e.g. when processing a compctl -s flag).              */
    lexsave();
    if (inwhat == IN_ENV)
	lincmd = 0;
    if (s) {
	if (lst == COMP_EXPAND_COMPLETE) {
	    /* Check if we have to do expansion or completion. */
	    char *q = s;

	    if (*q == Equals) {
		/* The word starts with `=', see if we can expand it. */
		q = s + 1;
		if (cmdnamtab->getnode(cmdnamtab, q) || hashcmd(q, pathchecked)) {
		    if (!hascompmod || isset(RECEXACT))
			lst = COMP_EXPAND;
		    else {
			int t0, n = 0;
			struct hashnode *hn;

			for (t0 = cmdnamtab->hsize - 1; t0 >= 0; t0--)
			    for (hn = cmdnamtab->nodes[t0]; hn;
				 hn = hn->next) {
				if (strpfx(q, hn->nam) && findcmd(hn->nam, 0))
				    n++;
				if (n == 2)
				    break;
			    }

			if (n == 1)
			    lst = COMP_EXPAND;
		    }
		}
	    }
	    if (lst == COMP_EXPAND_COMPLETE)
		do {
		    /* Check if there is a parameter expression. */
		    for (; *q && *q != String; q++);
		    if (*q == String && q[1] != Inpar && q[1] != Inbrack) {
			if (*++q == Inbrace) {
			    if (! skipparens(Inbrace, Outbrace, &q) &&
				q == s + zlemetacs - wb)
				lst = COMP_EXPAND;
			} else {
			    char *t, sav, sav2;

			    /* Skip the things parameter expressions might *
			     * start with (the things before the parameter *
			     * name).                                      */
			    for (; *q; q++)
				if (*q != '^' && *q != Hat &&
				    *q != '=' && *q != Equals &&
				    *q != '~' && *q != Tilde)
				    break;
			    if ((*q == '#' || *q == Pound || *q == '+') &&
				q[1] != String)
				q++;

			    sav2 = *(t = q);
			    if (*q == Quest || *q == Star || *q == String ||
				*q == Qstring)
				*q = ztokens[*q - Pound], ++q;
			    else if (*q == '?' || *q == '*' || *q == '$' ||
				     *q == '-' || *q == '!' || *q == '@')
				q++;
			    else if (idigit(*q))
				do q++; while (idigit(*q));
			    else
				q = itype_end(q, IIDENT, 0);
			    sav = *q;
			    *q = '\0';
			    if (zlemetacs - wb == q - s &&
				(idigit(sav2) || checkparams(t)))
				lst = COMP_EXPAND;
			    *q = sav;
			    *t = sav2;
			}
			if (lst != COMP_EXPAND)
			    lst = COMP_COMPLETE;
		    } else
			break;
		} while (q < s + zlemetacs - wb);
	    if (lst == COMP_EXPAND_COMPLETE) {
		/* If it is still not clear if we should use expansion or   *
		 * completion and there is a `$' or a backtick in the word, *
		 * than do expansion.                                       */
		for (q = s; *q; q++)
		    if (*q == Tick || *q == Qtick ||
			*q == String || *q == Qstring)
			break;
		lst = *q ? COMP_EXPAND : COMP_COMPLETE;
	    }
	    /* And do expansion if there are wildcards and globcomplete is *
	     * not used.                                                   */
	    if (unset(GLOBCOMPLETE) && cmphaswilds(s))
		lst = COMP_EXPAND;
	}
	if (lincmd && (inwhat == IN_NOTHING))
	    inwhat = IN_CMD;

	if (lst == COMP_SPELL) {
	    char *w = dupstring(origword), *x, *q, *ox;

	    for (q = w; *q; q++)
		if (inull(*q))
		    *q = Nularg;
	    zlemetacs = wb;
	    foredel(we - wb);

	    untokenize(x = ox = dupstring(w));
	    if (*w == Tilde || *w == Equals || *w == String)
		*x = *w;
	    spckword(&x, 0, lincmd, 0);
	    ret = !strcmp(x, ox);

	    untokenize(x);
	    inststr(x);
	} else if (COMP_ISEXPAND(lst)) {
	    /* Do expansion. */
	    char *ol = (olst == COMP_EXPAND ||
                        olst == COMP_EXPAND_COMPLETE) ?
		dupstring(zlemetaline) : zlemetaline;
	    int ocs = zlemetacs, ne = noerrs;

	    noerrs = 1;
	    ret = doexpansion(origword, lst, olst, lincmd);
	    lastambig = 0;
	    noerrs = ne;

	    /* If expandorcomplete was invoked and the expansion didn't *
	     * change the command line, do completion.                  */
	    if (olst == COMP_EXPAND_COMPLETE &&
		!strcmp(ol, zlemetaline)) {
		zlemetacs = ocs;
		errflag = 0;

		if (!compfunc) {
		    char *p;

		    p = s;
		    if (*p == Tilde || *p == Equals)
			p++;
		    for (; *p; p++)
			if (itok(*p)) {
			    if (*p != String && *p != Qstring)
				*p = ztokens[*p - Pound];
			    else if (p[1] == Inbrace)
				p++, skipparens(Inbrace, Outbrace, &p);
			}
		}
		ret = docompletion(s, lst, lincmd);
            } else {
                if (ret)
                    clearlist = 1;
                if (!strcmp(ol, zlemetaline)) {
                    /* We may have removed some quotes. For completion, other
                     * parts of the code re-install them, but for expansion
                     * we have to do it here. */
                    zlemetacs = 0;
                    foredel(zlemetall);
                    spaceinline(origll);
                    memcpy(zlemetaline, origline, origll);
                    zlemetacs = origcs;
                }
            }
	} else
	    /* Just do completion. */
	    ret = docompletion(s, lst, lincmd);
	zsfree(s);
    } else
	ret = 1;
    /* Reset the lexer state, pop the heap. */
    lexrestore();
    popheap();

    dat[0] = lst;
    dat[1] = ret;
    runhookdef(AFTERCOMPLETEHOOK, (void *) dat);
    unmetafy_line();

    active = 0;
    makecommaspecial(0);
    return dat[1];
}

/* 1 if we are completing the prefix */
static int comppref;

/* This function inserts an `x' in the command line at the cursor position. *
 *                                                                          *
 * Oh, you want to know why?  Well, if completion is tried somewhere on an  *
 * empty part of the command line, the lexer code would normally not be     *
 * able to give us the `word' we want to complete, since there is no word.  *
 * But we need to call the lexer to find out where we are (and for which    *
 * command we are completing and such things).  So we temporarily add a `x' *
 * (any character without special meaning would do the job) at the cursor   *
 * position, than the lexer gives us the word `x' and its beginning and end *
 * positions and we can remove the `x'.                                     *
 *									    *
 * If we are just completing the prefix (comppref set), we also insert a    *
 * space after the x to end the word.  We never need to remove the space:   *
 * anywhere we are able to retrieve a word for completion it will be	    *
 * discarded as whitespace.  It has the effect of making any suffix	    *
 * referrable to as the next word on the command line when indexing	    *
 * from a completion function.                                              */

/**/
static void
addx(char **ptmp)
{
    int addspace = 0;

    if (!zlemetaline[zlemetacs] || zlemetaline[zlemetacs] == '\n' ||
	(iblank(zlemetaline[zlemetacs]) &&
	 (!zlemetacs || zlemetaline[zlemetacs-1] != '\\')) ||
	zlemetaline[zlemetacs] == ')' || zlemetaline[zlemetacs] == '`' ||
	zlemetaline[zlemetacs] == '}' ||
	zlemetaline[zlemetacs] == ';' || zlemetaline[zlemetacs] == '|' ||
	zlemetaline[zlemetacs] == '&' ||
	zlemetaline[zlemetacs] == '>' || zlemetaline[zlemetacs] == '<' ||
	(instring && (zlemetaline[zlemetacs] == '"' ||
		      zlemetaline[zlemetacs] == '\'')) ||
	(addspace = (comppref && !iblank(zlemetaline[zlemetacs])))) {
	*ptmp = zlemetaline;
	zlemetaline = zhalloc(strlen(zlemetaline) + 3 + addspace);
	memcpy(zlemetaline, *ptmp, zlemetacs);
	zlemetaline[zlemetacs] = 'x';
	if (addspace)
	    zlemetaline[zlemetacs+1] = ' ';
	strcpy(zlemetaline + zlemetacs + 1 + addspace, (*ptmp) + zlemetacs);
	addedx = 1 + addspace;
    } else {
	addedx = 0;
	*ptmp = NULL;
    }
}

/* Like dupstring, but add an extra space at the end of the string. */

/**/
mod_export char *
dupstrspace(const char *str)
{
    int len = strlen(str);
    char *t = (char *) hcalloc(len + 2);
    strcpy(t, str);
    strcpy(t+len, " ");
    return t;
}

/*
 * These functions metafy and unmetafy the ZLE buffer, as described at
 * the top of this file.  They *must* be called in matching pairs,
 * around all the expansion/completion code.
 *
 * The variables zleline, zlell and zlecs are metafied into
 * zlemetaline, zlemetall and zlemetacs.  Only the latter variables
 * should be referred to from above zle (i.e. in the main shell),
 * or when using the completion API (if that's not too strong a
 * way of referring to it).
 */

/**/
mod_export void
metafy_line(void)
{
    UNMETACHECK();

    zlemetaline = zlelineasstring(zleline, zlell, zlecs,
				  &zlemetall, &zlemetacs, 0);
    metalinesz = zlemetall;

    /*
     * We will always allocate a new zleline based on zlemetaline.
     */
    free(zleline);
    zleline = NULL;
}

/**/
mod_export void
unmetafy_line(void)
{
    METACHECK();

    /* paranoia */
    zlemetaline[zlemetall] = '\0';
    zleline = stringaszleline(zlemetaline, zlemetacs, &zlell, &linesz, &zlecs);

    free(zlemetaline);
    zlemetaline = NULL;
}

/* Free a brinfo list. */

/**/
mod_export void
freebrinfo(Brinfo p)
{
    Brinfo n;

    while (p) {
	n = p->next;
	zsfree(p->str);
	zfree(p, sizeof(*p));

	p = n;
    }
}

/* Duplicate a brinfo list. */

/**/
mod_export Brinfo
dupbrinfo(Brinfo p, Brinfo *last, int heap)
{
    Brinfo ret = NULL, *q = &ret, n = NULL;

    while (p) {
	n = *q = (heap ? (Brinfo) zhalloc(sizeof(*n)) :
		  (Brinfo) zalloc(sizeof(*n)));
	q = &(n->next);

	n->next = NULL;
	n->str = (heap ? dupstring(p->str) : ztrdup(p->str));
	n->pos = p->pos;
	n->qpos = p->qpos;
	n->curpos = p->curpos;

	p = p->next;
    }
    if (last)
	*last = n;

    return ret;
}

/* This is a bit like has_token(), but ignores nulls. */

static int
has_real_token(const char *s)
{
    while (*s) {
	if (itok(*s) && !inull(*s))
	    return 1;
	s++;
    }
    return 0;
}


/* Lasciate ogni speranza.                                                  *
 * This function is a nightmare.  It works, but I'm sure that nobody really *
 * understands why.  The problem is: to make it cleaner we would need       *
 * changes in the lexer code (and then in the parser, and then...).         */

/**/
static char *
get_comp_string(void)
{
    int t0, tt0, i, j, k, cp, rd, sl, ocs, ins, oins, ia, parct, varq = 0;
    int ona = noaliases, qsub;
    char *s = NULL, *tmp, *p, *tt = NULL, rdop[20];
    char *linptr, *u;

    METACHECK();

    freebrinfo(brbeg);
    freebrinfo(brend);
    brbeg = lastbrbeg = brend = lastbrend = NULL;
    nbrbeg = nbrend = 0;
    zsfree(lastprebr);
    zsfree(lastpostbr);
    lastprebr = lastpostbr = NULL;
    if (rdstrs)
        freelinklist(rdstrs, freestr);
    rdstrs = znewlinklist();
    rdop[0] = '\0';
    rdstr = NULL;

    /* This global flag is used to signal the lexer code if it should *
     * expand aliases or not.                                         */
    noaliases = isset(COMPLETEALIASES);

    /* Find out if we are somewhere in a `string', i.e. inside '...', *
     * "...", `...`, or ((...)). Nowadays this is only used to find   *
     * out if we are inside `...`.                                    */

    for (i = j = k = 0, u = zlemetaline; u < zlemetaline + zlemetacs; u++) {
	if (*u == '`' && !(k & 1))
	    i++;
	else if (*u == '\"' && !(k & 1) && !(i & 1))
	    j++;
	else if (*u == '\'' && !(j & 1))
	    k++;
	else if (*u == '\\' && u[1] && !(k & 1))
	    u++;
    }
    inbackt = (i & 1);
    instring = 0;
    addx(&tmp);
    linptr = zlemetaline;
    pushheap();

 start:
    inwhat = IN_NOTHING;
    /* Now set up the lexer and start it. */
    parbegin = parend = -1;
    lincmd = incmdpos;
    linredir = inredir;
    zsfree(cmdstr);
    cmdstr = NULL;
    zsfree(varname);
    varname = NULL;
    insubscr = 0;
    zleparse = 1;
    clwpos = -1;
    lexsave();
    inpush(dupstrspace(linptr), 0, NULL);
    strinbeg(0);
    i = tt0 = cp = rd = ins = oins = linarr = parct = ia = 0;

    /* This loop is possibly the wrong way to do this.  It goes through *
     * the previously massaged command line using the lexer.  It stores *
     * each token in each command (commands being regarded, roughly, as *
     * being separated by tokens | & &! |& || &&).  The loop stops when *
     * the end of the command containing the cursor is reached.  What   *
     * makes this messy is checking for things like redirections, loops *
    * and whatnot. */

    do {
        qsub = 0;

	lincmd = ((incmdpos && !ins && !incond) || (oins == 2 && i == 2) ||
		  (ins == 3 && i == 1));
	linredir = (inredir && !ins);
	oins = ins;
	/* Get the next token. */
	if (linarr)
	    incmdpos = 0;
	ctxtlex();

	if (tok == LEXERR) {
	    if (!tokstr)
		break;
	    for (j = 0, p = tokstr; *p; p++)
		if (*p == Snull || *p == Dnull)
		    j++;
	    if (j & 1) {
		if (lincmd && strchr(tokstr, '=')) {
		    varq = 1;
		    tok = ENVSTRING;
		} else
		    tok = STRING;
	    }
	} else if (tok == ENVSTRING)
	    varq = 0;
	if (tok == ENVARRAY) {
	    linarr = 1;
	    zsfree(varname);
	    varname = ztrdup(tokstr);
	} else if (tok == INPAR)
	    parct++;
	else if (tok == OUTPAR) {
	    if (parct)
		parct--;
	    else
		linarr = 0;
	}
	if (inredir && IS_REDIROP(tok)) {
            rdstr = rdstrbuf;
            if (tokfd >= 0)
                sprintf(rdop, "%d%s", tokfd, tokstrings[tok]);
            else
                strcpy(rdop, tokstrings[tok]);
            strcpy(rdstr, rdop);
        }
	if (tok == DINPAR)
	    tokstr = NULL;

	/* We reached the end. */
	if (tok == ENDINPUT)
	    break;
	if ((ins && (tok == DOLOOP || tok == SEPER)) ||
	    (ins == 2 && i == 2) || (ins == 3 && i == 3) ||
	    tok == BAR    || tok == AMPER     ||
	    tok == BARAMP || tok == AMPERBANG ||
	    ((tok == DBAR || tok == DAMPER) && !incond)) {
	    /* This is one of the things that separate commands.  If we  *
	     * already have the things we need (e.g. the token strings), *
	     * leave the loop.                                           */
	    if (tt)
		break;
	    /* Otherwise reset the variables we are collecting data in. */
	    i = tt0 = cp = rd = ins = 0;
	}
	if (lincmd && (tok == STRING || tok == FOR || tok == FOREACH ||
		       tok == SELECT || tok == REPEAT || tok == CASE)) {
	    /* The lexer says, this token is in command position, so *
	     * store the token string (to find the right compctl).   */
	    ins = (tok == REPEAT ? 2 : (tok != STRING));
	    zsfree(cmdstr);
	    cmdstr = ztrdup(tokstr);
	    i = 0;
	}
	if (!zleparse && !tt0) {
	    /* This is done when the lexer reached the word the cursor is on. */
	    tt = tokstr ? dupstring(tokstr) : NULL;

            if (isset(RCQUOTES) && *tt == Snull) {
                char *p, *e = tt + zlemetacs - wb;
                for (p = tt; *p && p < e; p++)
                    if (*p == '\'')
                        qsub++;
            }
	    /* If we added a `x', remove it. */
	    if (addedx && tt)
		chuck(tt + zlemetacs - wb - qsub);
	    tt0 = tok;
	    /* Store the number of this word. */
	    clwpos = i;
	    cp = lincmd;
	    rd = linredir;
	    ia = linarr;
	    if (inwhat == IN_NOTHING && incond)
		inwhat = IN_COND;
	} else if (linredir) {
            if (rdop[0] && tokstr)
                zaddlinknode(rdstrs, tricat(rdop, ":", tokstr));
	    continue;
        }
	if (incond) {
	    if (tok == DBAR)
		tokstr = "||";
	    else if (tok == DAMPER)
		tokstr = "&&";
	}
	if (!tokstr)
	    continue;
	/* Hack to allow completion after `repeat n do'. */
	if (oins == 2 && !i && !strcmp(tokstr, "do"))
	    ins = 3;
	/* We need to store the token strings of all words (for some of *
	 * the more complicated compctl -x things).  They are stored in *
	 * the clwords array.  Make this array big enough.              */
	if (i + 1 == clwsize) {
	    int n;
	    clwords = (char **)realloc(clwords,
				       (clwsize *= 2) * sizeof(char *));
	    for(n = clwsize; --n > i; )
		clwords[n] = NULL;
	}
	zsfree(clwords[i]);
	/* And store the current token string. */
	clwords[i] = ztrdup(tokstr);
	sl = strlen(tokstr);
	/* Sometimes the lexer gives us token strings ending with *
	 * spaces we delete the spaces.                           */
	while (sl && clwords[i][sl - 1] == ' ' &&
	       (sl < 2 || (clwords[i][sl - 2] != Bnull &&
			   clwords[i][sl - 2] != Meta)))
	    clwords[i][--sl] = '\0';
	/* If this is the word the cursor is in and we added a `x', *
	 * remove it.                                               */
	if (clwpos == i++ && addedx)
	    chuck(&clwords[i - 1][((zlemetacs - wb - qsub) >= sl) ?
				 (sl - 1) : (zlemetacs - wb - qsub)]);
    } while (tok != LEXERR && tok != ENDINPUT &&
	     (tok != SEPER || (zleparse && !tt0)));
    /* Calculate the number of words stored in the clwords array. */
    clwnum = (tt || !i) ? i : i - 1;
    zsfree(clwords[clwnum]);
    clwords[clwnum] = NULL;
    t0 = tt0;
    if (ia) {
	lincmd = linredir = 0;
	inwhat = IN_ENV;
    } else {
	lincmd = cp;
	linredir = rd;
    }
    strinend();
    inpop();
    errflag = zleparse = 0;
    if (parbegin != -1) {
	/* We are in command or process substitution if we are not in
	 * a $((...)). */
	if (parend >= 0 && !tmp)
	    zlemetaline = dupstring(tmp = zlemetaline);
	linptr = zlemetaline + zlemetall + addedx - parbegin + 1;
	if ((linptr - zlemetaline) < 3 || *linptr != '(' ||
	    linptr[-1] != '(' || linptr[-2] != '$') {
	    if (parend >= 0) {
		zlemetall -= parend;
		zlemetaline[zlemetall + addedx] = '\0';
	    }
	    lexrestore();
	    tt = NULL;
	    goto start;
	}
    }

    if (inwhat == IN_MATH)
	s = NULL;
    else if (!t0 || t0 == ENDINPUT) {
	/* There was no word (empty line). */
	s = ztrdup("");
	we = wb = zlemetacs;
	clwpos = clwnum;
	t0 = STRING;
    } else if (t0 == STRING) {
	/* We found a simple string. */
	s = ztrdup(clwords[clwpos]);
    } else if (t0 == ENVSTRING) {
	char sav;
	/* The cursor was inside a parameter assignment. */

	if (varq)
	    tt = clwords[clwpos];

	s = itype_end(tt, IIDENT, 0);
	sav = *s;
	*s = '\0';
	zsfree(varname);
	varname = ztrdup(tt);
	*s = sav;
        if (*s == '+')
            s++;
	if (skipparens(Inbrack, Outbrack, &s) > 0 || s > tt + zlemetacs - wb) {
	    s = NULL;
	    inwhat = IN_MATH;
	    if ((keypm = (Param) paramtab->getnode(paramtab, varname)) &&
		(keypm->node.flags & PM_HASHED))
		insubscr = 2;
	    else
		insubscr = 1;
	} else if (*s == '=') {
            if (zlemetacs > wb + (s - tt)) {
                s++;
                wb += s - tt;
                s = ztrdup(s);
                inwhat = IN_ENV;
            } else {
                char *p = s;

                if (p[-1] == '+')
                    p--;
                sav = *p;
                *p = '\0';
                inwhat = IN_PAR;
                s = ztrdup(tt);
                *p = sav;
                we = wb + p - tt;
            }
            t0 = STRING;
	}
	lincmd = 1;
    }
    if (we > zlemetall)
	we = zlemetall;
    tt = zlemetaline;
    if (tmp) {
	zlemetaline = tmp;
	zlemetall = strlen(zlemetaline);
    }
    if (t0 != STRING && inwhat != IN_MATH) {
	if (tmp) {
	    tmp = NULL;
	    linptr = zlemetaline;
	    lexrestore();
	    addedx = 0;
	    goto start;
	}
	noaliases = ona;
	lexrestore();
	return NULL;
    }

    noaliases = ona;

    /* Check if we are in an array subscript.  We simply assume that  *
     * we are in a subscript if we are in brackets.  Correct solution *
     * is very difficult.  This is quite close, but gets things like  *
     * foo[_ wrong (note no $).  If we are in a subscript, treat it   *
     * as being in math.                                              */
    if (inwhat != IN_MATH) {
	int i = 0;
	char *nnb, *nb = NULL, *ne = NULL;

	MB_METACHARINIT();
	if (itype_end(s, IIDENT, 1) == s)
	    nnb = s + MB_METACHARLEN(s);
	else
	    nnb = s;
	for (tt = s; tt < s + zlemetacs - wb;) {
	    if (*tt == Inbrack) {
		i++;
		nb = nnb;
		ne = tt;
		tt++;
	    } else if (i && *tt == Outbrack) {
		i--;
		tt++;
	    } else {
		int nclen = MB_METACHARLEN(tt);
		if (itype_end(tt, IIDENT, 1) == tt)
		    nnb = tt + nclen;
		tt += nclen;
	    }
	}
	if (i) {
	    inwhat = IN_MATH;
	    insubscr = 1;
	    if (nb < ne) {
		char sav = *ne;
		*ne = '\0';
		zsfree(varname);
		varname = ztrdup(nb);
		*ne = sav;
		if ((keypm = (Param) paramtab->getnode(paramtab, varname)) &&
		    (keypm->node.flags & PM_HASHED))
		    insubscr = 2;
	    }
	}
    }
    if (inwhat == IN_MATH) {
	if (compfunc || insubscr == 2) {
	    int lev;
	    char *p;

	    for (wb = zlemetacs - 1, lev = 0; wb > 0; wb--)
		if (zlemetaline[wb] == ']' || zlemetaline[wb] == ')')
		    lev++;
		else if (zlemetaline[wb] == '[') {
		    if (!lev--)
			break;
		} else if (zlemetaline[wb] == '(') {
		    if (!lev && zlemetaline[wb - 1] == '(')
			break;
		    if (lev)
			lev--;
		}
	    p = zlemetaline + wb;
	    wb++;
	    if (wb && (*p == '[' || *p == '(') &&
		!skipparens(*p, (*p == '[' ? ']' : ')'), &p)) {
		we = (p - zlemetaline) - 1;
		if (insubscr == 2)
		    insubscr = 3;
	    }
	} else {
	    /* In mathematical expression, we complete parameter names  *
	     * (even if they don't have a `$' in front of them).  So we *
	     * have to find that name.                                  */
	    char *cspos = zlemetaline + zlemetacs, *wptr, *cptr;
	    we = itype_end(cspos, IIDENT, 0) - cspos;

	    /*
	     * With multibyte characters we need to go forwards,
	     * so start at the beginning of the line and continue
	     * until cspos.
	     */
	    wptr = cptr = zlemetaline;
	    for (;;) {
		cptr = itype_end(wptr, IIDENT, 0);
		if (cptr == wptr) {
		    /* not an ident character */
		    wptr = (cptr += MB_METACHARLEN(cptr));
		}
		if (cptr >= cspos) {
		    wb = wptr - zlemetaline;
		    break;
		}
	    }
	}
	zsfree(s);
	s = zalloc(we - wb + 1);
	strncpy(s, zlemetaline + wb, we - wb);
	s[we - wb] = '\0';

	if (wb > 2 && zlemetaline[wb - 1] == '[') {
	    char *sqbr = zlemetaline + wb - 1, *cptr, *wptr;

	    /* Need to search forward for word characters */
	    cptr = wptr = zlemetaline;
	    for (;;) {
		cptr = itype_end(wptr, IIDENT, 0);
		if (cptr == wptr) {
		    /* not an ident character */
		    wptr = (cptr += MB_METACHARLEN(cptr));
		}
		if (cptr >= sqbr)
		    break;
	    }

	    if (wptr < sqbr) {
		zsfree(varname);
		varname = ztrduppfx(wptr, sqbr - wptr);
		if ((keypm = (Param) paramtab->getnode(paramtab, varname)) &&
		    (keypm->node.flags & PM_HASHED)) {
		    if (insubscr != 3)
			insubscr = 2;
		} else
		    insubscr = 1;
	    }
	}

	parse_subst_string(s);
    }
    /* This variable will hold the current word in quoted form. */
    offs = zlemetacs - wb;
    if ((p = parambeg(s))) {
	for (p = s; *p; p++)
	    if (*p == Dnull)
		*p = '"';
	    else if (*p == Snull)
		*p = '\'';
    } else {
        int level = 0;

        for (p = s; *p; p++) {
            if (level && *p == Snull)
                *p = '\'';
            else if (level && *p == Dnull)
                *p = '"';
            else if ((*p == String || *p == Qstring) && p[1] == Inbrace)
                level++;
            else if (*p == Outbrace)
                level--;
        }
    }
    if ((*s == Snull || *s == Dnull) && !has_real_token(s + 1)) {
	char *q = (*s == Snull ? "'" : "\""), *n = tricat(qipre, q, "");
	int sl = strlen(s);

	instring = (*s == Snull ? 1 : 2);
	zsfree(qipre);
	qipre = n;
	if (sl > 1 && s[sl - 1] == *s) {
	    n = tricat(q, qisuf, "");
	    zsfree(qisuf);
	    qisuf = n;
	}
	autoq = ztrdup(q);

        if (instring == 2) {
            for (q = s; *q; q++)
                if (*q == '\\' && q[1] == '!')
                    *q = Bnull;
        }
    }
    /* While building the quoted form, we also clean up the command line. */
    for (p = s, i = wb, j = 0; *p; p++, i++)
	if (inull(*p)) {
	    if (i < zlemetacs)
		offs--;
	    if (*p == Snull && isset(RCQUOTES))
		j = 1-j;
	    if (p[1] || *p != Bnull) {
		if (*p == Bnull) {
		    if (zlemetacs == i + 1)
			zlemetacs++, offs++;
		} else {
		    ocs = zlemetacs;
		    zlemetacs = i;
		    foredel(1);
		    if ((zlemetacs = ocs) > i--)
			zlemetacs--;
		    we--;
		}
	    } else {
		ocs = zlemetacs;
		zlemetacs = we;
		backdel(1);
		if (ocs == we)
		    zlemetacs = we - 1;
		else
		    zlemetacs = ocs;
		we--;
	    }
	    chuck(p--);
	} else if (j && *p == '\'' && i < zlemetacs)
	    offs--;

    zsfree(origword);
    origword = ztrdup(s);

    if (!isset(IGNOREBRACES)) {
	/* Try and deal with foo{xxx etc. */
	char *curs = s + (isset(COMPLETEINWORD) ? offs : (int)strlen(s));
	char *predup = dupstring(s), *dp = predup;
	char *bbeg = NULL, *bend = NULL, *dbeg = NULL;
	char *lastp = NULL, *firsts = NULL;
	int cant = 0, begi = 0, boffs = offs, hascom = 0;

	for (i = 0, p = s; *p; p++, dp++, i++) {
	    /* careful, ${... is not a brace expansion...
	     * we try to get braces after a parameter expansion right,
	     * but this may fail sometimes. sorry.
	     */
	    if (*p == String || *p == Qstring) {
		if (p[1] == Inbrace || p[1] == Inpar || p[1] == Inbrack) {
		    char *tp = p + 1;

		    if (skipparens(*tp, (*tp == Inbrace ? Outbrace :
					 (*tp == Inpar ? Outpar : Outbrack)),
				   &tp)) {
			tt = NULL;
			break;
		    }
		    i += tp - p;
		    dp += tp - p;
		    p = tp;
		} else {
		    char *tp = p + 1;

		    for (; *tp == '^' || *tp == Hat ||
			     *tp == '=' || *tp == Equals ||
			     *tp == '~' || *tp == Tilde ||
			     *tp == '#' || *tp == Pound || *tp == '+';
			 tp++);
		    if (*tp == Quest || *tp == Star || *tp == String ||
			*tp == Qstring || *tp == '?' || *tp == '*' ||
			*tp == '$' || *tp == '-' || *tp == '!' ||
			*tp == '@')
			p++, i++;
		    else {
			char *ie;
			if (idigit(*tp))
			    while (idigit(*tp))
				tp++;
			else if ((ie = itype_end(tp, IIDENT, 0)) != tp)
			    tp = ie;
			else {
			    tt = NULL;
			    break;
			}
			if (*tp == Inbrace) {
			    cant = 1;
			    break;
			}
			tp--;
			i += tp - p;
			dp += tp - p;
			p = tp;
		    }
		}
	    } else if (p < curs) {
		if (*p == Outbrace) {
		    cant = 1;
		    break;
		}
		if (*p == Inbrace) {
		    char *tp = p;

		    if (!skipparens(Inbrace, Outbrace, &tp)) {
			i += tp - p - 1;
			dp += tp - p - 1;
			p = tp - 1;
			continue;
		    }
		    makecommaspecial(1);
		    if (bbeg) {
			Brinfo new;
			int len = bend - bbeg;

			new = (Brinfo) zalloc(sizeof(*new));
			nbrbeg++;

			new->next = NULL;
			if (lastbrbeg)
			    lastbrbeg->next = new;
			else
			    brbeg = new;
			lastbrbeg = new;

			new->next = NULL;
			new->str = dupstrpfx(bbeg, len);
			new->str = ztrdup(bslashquote(new->str, NULL, instring));
			untokenize(new->str);
			new->pos = begi;
			*dbeg = '\0';
			new->qpos = strlen(bslashquote(predup, NULL, instring));
			*dbeg = '{';
			i -= len;
			boffs -= len;
			strcpy(dbeg, dbeg + len);
			dp -= len;
		    }
		    bbeg = lastp = p;
		    dbeg = dp;
		    bend = p + 1;
		    begi = i;
		} else if (*p == Comma && bbeg) {
		    bend = p + 1;
		    hascom = 1;
		}
	    } else {
		if (*p == Inbrace) {
		    char *tp = p;

		    if (!skipparens(Inbrace, Outbrace, &tp)) {
			i += tp - p - 1;
			dp += tp - p - 1;
			p = tp - 1;
			continue;
		    }
		    cant = 1;
		    makecommaspecial(1);
		    break;
		}
		if (p == curs) {
		    if (bbeg) {
			Brinfo new;
			int len = bend - bbeg;

			new = (Brinfo) zalloc(sizeof(*new));
			nbrbeg++;

			new->next = NULL;
			if (lastbrbeg)
			    lastbrbeg->next = new;
			else
			    brbeg = new;
			lastbrbeg = new;

			new->str = dupstrpfx(bbeg, len);
			new->str = ztrdup(bslashquote(new->str, NULL, instring));
			untokenize(new->str);
			new->pos = begi;
			*dbeg = '\0';
			new->qpos = strlen(bslashquote(predup, NULL, instring));
			*dbeg = '{';
			i -= len;
			boffs -= len;
			strcpy(dbeg, dbeg + len);
			dp -= len;
		    }
		    bbeg = NULL;
		}
		if (*p == Comma) {
		    if (!bbeg)
			bbeg = p;
		    hascom = 2;
		} else if (*p == Outbrace) {
		    Brinfo new;
		    int len;

		    if (!bbeg)
			bbeg = p;
		    len = p + 1 - bbeg;
		    if (!firsts)
			firsts = p + 1;

		    new = (Brinfo) zalloc(sizeof(*new));
		    nbrend++;

		    if (!lastbrend)
			lastbrend = new;

		    new->next = brend;
		    brend = new;

		    new->str = dupstrpfx(bbeg, len);
		    new->str = ztrdup(bslashquote(new->str, NULL, instring));
		    untokenize(new->str);
		    new->pos = dp - predup - len + 1;
		    new->qpos = len;
		    bbeg = NULL;
		}
	    }
	}
	if (cant) {
	    freebrinfo(brbeg);
	    freebrinfo(brend);
	    brbeg = lastbrbeg = brend = lastbrend = NULL;
	    nbrbeg = nbrend = 0;
	} else {
	    if (p == curs && bbeg) {
		Brinfo new;
		int len = bend - bbeg;

		new = (Brinfo) zalloc(sizeof(*new));
		nbrbeg++;

		new->next = NULL;
		if (lastbrbeg)
		    lastbrbeg->next = new;
		else
		    brbeg = new;
		lastbrbeg = new;

		new->str = dupstrpfx(bbeg, len);
		new->str = ztrdup(bslashquote(new->str, NULL, instring));
		untokenize(new->str);
		new->pos = begi;
		*dbeg = '\0';
		new->qpos = strlen(bslashquote(predup, NULL, instring));
		*dbeg = '{';
		boffs -= len;
		strcpy(dbeg, dbeg + len);
	    }
	    if (brend) {
		Brinfo bp, prev = NULL;
		int p, l;

		for (bp = brend; bp; bp = bp->next) {
		    bp->prev = prev;
		    prev = bp;
		    p = bp->pos;
		    l = bp->qpos;
		    bp->pos = strlen(predup + p + l);
		    bp->qpos = strlen(bslashquote(predup + p + l, NULL, instring));
		    strcpy(predup + p, predup + p + l);
		}
	    }
	    if (hascom) {
		if (lastp) {
		    char sav = *lastp;

		    *lastp = '\0';
		    untokenize(lastprebr = ztrdup(s));
		    *lastp = sav;
		}
		if ((lastpostbr = ztrdup(firsts)))
		    untokenize(lastpostbr);
	    }
	    zsfree(s);
	    s = ztrdup(predup);
	    offs = boffs;
	}
    }
    lexrestore();

    return (char *)s;
}

/* Insert the given string into the command line.  If move is non-zero, *
 * the cursor position is changed and len is the length of the string   *
 * to insert (if it is -1, the length is calculated here).              *
 * The last argument says if we should quote the string.                */

/**/
mod_export int
inststrlen(char *str, int move, int len)
{
    if (!len || !str)
	return 0;
    if (len == -1)
	len = strlen(str);
    spaceinline(len);
    if (zlemetaline != NULL) {
	strncpy(zlemetaline + zlemetacs, str, len);
	if (move)
	    zlemetacs += len;
    } else {
	char *instr;
	ZLE_STRING_T zlestr;
	int zlelen;

	instr = ztrduppfx(str, len);
	zlestr = stringaszleline(instr, 0, &zlelen, NULL, NULL);
	ZS_strncpy(zleline + zlecs, zlestr, zlelen);
	free(zlestr);
	zsfree(instr);
	if (move)
	    zlecs += len;
    }
    return len;
}

/* Expand the current word. */

/**/
static int
doexpansion(char *s, int lst, int olst, int explincmd)
{
    int ret = 1, first = 1;
    LinkList vl;
    char *ss, *ts;

    pushheap();
    vl = newlinklist();
    ss = dupstring(s);
    /* get_comp_string() leaves these quotes unchanged when they are
     * inside parameter expansions. */
    for (ts = ss; *ts; ts++)
        if (*ts == '"')
            *ts = Dnull;
        else if (*ts == '\'')
            *ts = Snull;
    addlinknode(vl, ss);
    prefork(vl, 0);
    if (errflag)
	goto end;
    if (lst == COMP_LIST_EXPAND || lst == COMP_EXPAND) {
	int ng = opts[NULLGLOB];

	opts[NULLGLOB] = 1;
	globlist(vl, 1);
	opts[NULLGLOB] = ng;
    }
    if (errflag)
	goto end;
    if (empty(vl) || !*(char *)peekfirst(vl))
	goto end;
    if (peekfirst(vl) == (void *) ss ||
	(olst == COMP_EXPAND_COMPLETE &&
	 !nextnode(firstnode(vl)) && *s == Tilde &&
	 (ss = dupstring(s), filesubstr(&ss, 0)) &&
	 !strcmp(ss, (char *)peekfirst(vl)))) {
	/* If expansion didn't change the word, try completion if *
	 * expandorcomplete was called, otherwise, just beep.     */
	if (lst == COMP_EXPAND_COMPLETE)
	    docompletion(s, COMP_COMPLETE, explincmd);
	goto end;
    }
    if (lst == COMP_LIST_EXPAND) {
	/* Only the list of expansions was requested. Restore the 
         * command line. */
        zlemetacs = 0;
        foredel(zlemetall);
        spaceinline(origll);
        memcpy(zlemetaline, origline, origll);
        zlemetacs = origcs;
        ret = listlist(vl);
        showinglist = 0;
	goto end;
    }
    /* Remove the current word and put the expansions there. */
    zlemetacs = wb;
    foredel(we - wb);
    while ((ss = (char *)ugetnode(vl))) {
	ret = 0;
	ss = quotename(ss, NULL);
	untokenize(ss);
	inststr(ss);
#if 0
	if (olst != COMP_EXPAND_COMPLETE || nonempty(vl) ||
	    (zlemetacs && zlemetaline[zlemetacs-1] != '/')) {
#endif
	if (nonempty(vl) || !first) {
	    spaceinline(1);
	    zlemetaline[zlemetacs++] = ' ';
	}
	first = 0;
    }
    end:
    popheap();

    return ret;
}

/**/
static int
docompletion(char *s, int lst, int incmd)
{
    struct compldat dat;

    dat.s = s;
    dat.lst = lst;
    dat.incmd = incmd;

    return runhookdef(COMPLETEHOOK, (void *) &dat);
}

/*
 * Return the length of the common prefix of s and t.
 * s and t are both metafied; the length returned is a raw byte count
 * into both strings, excluding any common bytes that form less than
 * a complete wide character.
 */

/**/
mod_export int
pfxlen(char *s, char *t)
{
    int i = 0;

#ifdef MULTIBYTE_SUPPORT
    wchar_t wc;
    mbstate_t mbs;
    size_t cnt;
    int lasti = 0;
    char inc;

    memset(&mbs, 0, sizeof mbs);
    while (*s) {
	if (*s == Meta) {
	    if (*t != Meta || t[1] != s[1])
		break;
	    inc = s[1] ^ 32;
	    i += 2;
	    s += 2;
	    t += 2;
	} else {
	    if (*s != *t)
		break;
	    inc = *s;
	    i++;
	    s++;
	    t++;
	}

	cnt = mbrtowc(&wc, &inc, 1, &mbs);
	if (cnt == MB_INVALID) {
	    /* error */
	    break;
	}
	if (cnt != MB_INCOMPLETE) {
	    /* successfully found complete character, record position */
	    lasti = i;
	}
	/* Otherwise, not found a complete character: keep trying. */
    }
    return lasti;
#else
    while (*s && *s == *t)
	s++, t++, i++;
    return i;
#endif
}

/* Return the length of the common suffix of s and t. */

#if 0
static int
sfxlen(char *s, char *t)
{
    if (*s && *t) {
	int i = 0;
	char *s2 = s + strlen(s) - 1, *t2 = t + strlen(t) - 1;

	while (s2 >= s && t2 >= t && *s2 == *t2)
	    s2--, t2--, i++;

	return i;
    } else
	return 0;
}
#endif

/* This is strcmp with ignoring backslashes. */

/**/
mod_export int
strbpcmp(char **aa, char **bb)
{
    char *a = *aa, *b = *bb;

    while (*a && *b) {
	if (*a == '\\')
	    a++;
	if (*b == '\\')
	    b++;
	if (*a != *b)
	    break;
	if (*a)
	    a++;
	if (*b)
	    b++;
    }
    if (isset(NUMERICGLOBSORT) && (idigit(*a) || idigit(*b))) {
	for (; a > *aa && idigit(a[-1]); a--, b--);
	if (idigit(*a) && idigit(*b)) {
	    while (*a == '0')
		a++;
	    while (*b == '0')
		b++;
	    for (; idigit(*a) && *a == *b; a++, b++);
	    if (idigit(*a) || idigit(*b)) {
		int cmp = (int) STOUC(*a) - (int) STOUC(*b);

		while (idigit(*a) && idigit(*b))
		    a++, b++;
		if (idigit(*a) && !idigit(*b))
		    return 1;
		if (idigit(*b) && !idigit(*a))
		    return -1;

		return cmp;
	    }
	}
    }
#ifndef HAVE_STRCOLL
    return (int)(*a - *b);
#else
    return strcoll(a,b);
#endif
}

/* This is used to print the strings (e.g. explanations). *
 * It returns the number of lines printed.       */

/**/
mod_export int
printfmt(char *fmt, int n, int dopr, int doesc)
{
    char *p = fmt, nc[DIGBUFSIZE];
    int l = 0, cc = 0, b = 0, s = 0, u = 0, m;

    MB_METACHARINIT();
    for (; *p; ) {
	/* Handle the `%' stuff (%% == %, %n == <number of matches>). */
	if (doesc && *p == '%') {
	    if (*++p) {
		m = 0;
		switch (*p) {
		case '%':
		    if (dopr)
			putc('%', shout);
		    cc++;
		    break;
		case 'n':
		    sprintf(nc, "%d", n);
		    if (dopr)
			fprintf(shout, nc);
		    cc += MB_METASTRWIDTH(nc);
		    break;
		case 'B':
		    b = 1;
		    if (dopr)
			tcout(TCBOLDFACEBEG);
		    break;
		case 'b':
		    b = 0; m = 1;
		    if (dopr)
			tcout(TCALLATTRSOFF);
		    break;
		case 'S':
		    s = 1;
		    if (dopr)
			tcout(TCSTANDOUTBEG);
		    break;
		case 's':
		    s = 0; m = 1;
		    if (dopr)
			tcout(TCSTANDOUTEND);
		    break;
		case 'U':
		    u = 1;
		    if (dopr)
			tcout(TCUNDERLINEBEG);
		    break;
		case 'u':
		    u = 0; m = 1;
		    if (dopr)
			tcout(TCUNDERLINEEND);
		    break;
		case '{':
		    for (p++; *p && (*p != '%' || p[1] != '}'); p++)
			if (dopr)
			    putc(*p, shout);
		    if (*p)
			p++;
		    else
			p--;
		    break;
		}
		if (dopr && m) {
		    if (b)
			tcout(TCBOLDFACEBEG);
		    if (s)
			tcout(TCSTANDOUTBEG);
		    if (u)
			tcout(TCUNDERLINEBEG);
		}
	    } else
		break;
	    p++;
	} else {
	    if (*p == '\n') {
		cc++;
		if (dopr) {
		    if (tccan(TCCLEAREOL))
			tcout(TCCLEAREOL);
		    else {
			int s = columns - 1 - (cc % columns);

			while (s-- > 0)
			    putc(' ', shout);
		    }
		}
		l += 1 + ((cc - 1) / columns);
		cc = 0;
		putc('\n', shout);
		p++;
	    } else {
		convchar_t cchar;
		int clen = MB_METACHARLENCONV(p, &cchar);
		if (dopr) {
		    while (clen--)
			putc(*p++, shout);
		} else
		    p += clen;
		cc += WCWIDTH(cchar);
		if (dopr && !(cc % columns))
			fputs(" \010", shout);
	    }
	}
    }
    if (dopr) {
        if (!(cc % columns))
            fputs(" \010", shout);
	if (tccan(TCCLEAREOL))
	    tcout(TCCLEAREOL);
	else {
	    int s = columns - 1 - (cc % columns);

	    while (s-- > 0)
		putc(' ', shout);
	}
    }
    /*
     * Experiments suggest that at this point not subtracting 1 from
     * cc is correct, i.e. if just misses wrapping we still add 1.
     * (Why?)
     */
    return l + (cc / columns);
}

/* This is used to print expansions. */

/**/
int
listlist(LinkList l)
{
    int num = countlinknodes(l);
    VARARR(char *, data, (num + 1));
    LinkNode node;
    char **p;
    VARARR(int, lens, num);
    VARARR(int, widths, columns);
    int longest = 0, shortest = columns, totl = 0;
    int len, ncols, nlines, tolast, col, i, max, pack = 0, *lenp;

    for (node = firstnode(l), p = data; node; incnode(node), p++)
	*p = (char *) getdata(node);
    *p = NULL;

    qsort((void *) data, num, sizeof(char *),
	  (int (*) _((const void *, const void *))) strbpcmp);

    for (p = data, lenp = lens; *p; p++, lenp++) {
	len = *lenp = ZMB_nicewidth(*p) + 2;
	if (len > longest)
	    longest = len;
	if (len < shortest)
	    shortest = len;
	totl += len;
    }
    if ((ncols = ((columns + 2) / longest))) {
	int tlines = 0, tline, tcols = 0, maxlen, nth, width;

	nlines = (num + ncols - 1) / ncols;

	if (isset(LISTPACKED)) {
	    if (isset(LISTROWSFIRST)) {
		int count, tcol, first, maxlines = 0, llines;

		for (tcols = columns / shortest; tcols > ncols;
		     tcols--) {
		    for (nth = first = maxlen = width = maxlines =
			     llines = tcol = 0,
			     count = num;
			 count > 0; count--) {
			if (!(nth % tcols))
			    llines++;
			if (lens[nth] > maxlen)
			    maxlen = lens[nth];
			nth += tcols;
			tlines++;
			if (nth >= num) {
			    if ((width += maxlen) >= columns)
				break;
			    widths[tcol++] = maxlen;
			    maxlen = 0;
			    nth = ++first;
			    if (llines > maxlines)
				maxlines = llines;
			    llines = 0;
			}
		    }
		    if (nth < num) {
			widths[tcol++] = maxlen;
			width += maxlen;
		    }
		    if (!count && width < columns)
			break;
		}
		if (tcols > ncols)
		    tlines = maxlines;
	    } else {
		for (tlines = ((totl + columns) / columns);
		     tlines < nlines; tlines++) {
		    for (p = data, nth = tline = width =
			     maxlen = tcols = 0;
			 *p; nth++, p++) {
			if (lens[nth] > maxlen)
			    maxlen = lens[nth];
			if (++tline == tlines) {
			    if ((width += maxlen) >= columns)
				break;
			    widths[tcols++] = maxlen;
			    maxlen = tline = 0;
			}
		    }
		    if (tline) {
			widths[tcols++] = maxlen;
			width += maxlen;
		    }
		    if (nth == num && width < columns)
			break;
		}
	    }
	    if ((pack = (tlines < nlines))) {
		nlines = tlines;
		ncols = tcols;
	    }
	}
    } else {
	nlines = 0;
	for (p = data; *p; p++)
	    nlines += 1 + (strlen(*p) / columns);
    }
    /* Set the cursor below the prompt. */
    trashzle();

    tolast = ((zmult == 1) == !!isset(ALWAYSLASTPROMPT));
    clearflag = (isset(USEZLE) && !termflags && tolast);

    max = getiparam("LISTMAX");
    if ((max && num > max) || (!max && nlines > lines)) {
	int qup, l;

	zsetterm();
	l = (num > 0 ?
	     fprintf(shout, "zsh: do you wish to see all %d possibilities (%d lines)? ",
		     num, nlines) :
	     fprintf(shout, "zsh: do you wish to see all %d lines? ", nlines));
	qup = ((l + columns - 1) / columns) - 1;
	fflush(shout);
	if (!getzlequery()) {
	    if (clearflag) {
		putc('\r', shout);
		tcmultout(TCUP, TCMULTUP, qup);
		if (tccan(TCCLEAREOD))
		    tcout(TCCLEAREOD);
		tcmultout(TCUP, TCMULTUP, nlnct);
	    } else
		putc('\n', shout);
	    return 1;
	}
	if (clearflag) {
	    putc('\r', shout);
	    tcmultout(TCUP, TCMULTUP, qup);
	    if (tccan(TCCLEAREOD))
		tcout(TCCLEAREOD);
	} else
	    putc('\n', shout);
	settyinfo(&shttyinfo);
    }
    lastlistlen = (clearflag ? nlines : 0);

    if (ncols) {
	if (isset(LISTROWSFIRST)) {
	    for (col = 1, p = data, lenp = lens; *p;
		 p++, lenp++, col++) {
		nicezputs(*p, shout);
		if (col == ncols) {
		    col = 0;
		    if (p[1])
			putc('\n', shout);
		} else {
		    if ((i = (pack ? widths[col - 1] : longest) - *lenp + 2) > 0)
			while (i--)
			    putc(' ', shout);
		}
	    }
	} else {
	    char **f;
	    int *fl, line;

	    for (f = data, fl = lens, line = 0; line < nlines;
		 f++, fl++, line++) {
		for (col = 1, p = f, lenp = fl; *p; col++) {
		    nicezputs(*p, shout);
		    if (col == ncols)
			break;
		    if ((i = (pack ? widths[col - 1] : longest) - *lenp + 2) > 0)
			while (i--)
			    putc(' ', shout);
		    for (i = nlines; i && *p; i--, p++, lenp++);
		}
		if (line + 1 < nlines)
		    putc('\n', shout);
	    }
	}
    } else {
	for (p = data; *p; p++) {
	    nicezputs(*p, shout);
	    putc('\n', shout);
	}
    }
    if (clearflag) {
	if ((nlines += nlnct - 1) < lines) {
	    tcmultout(TCUP, TCMULTUP, nlines);
	    showinglist = -1;
	} else
	    clearflag = 0, putc('\n', shout);
    } else
	putc('\n', shout);

    if (listshown)
	showagain = 1;

    return !num;
}

/* Expand the history references. */

/**/
int
doexpandhist(void)
{
    char *ol;
    int oll, ocs, ne = noerrs, err, ona = noaliases;

    UNMETACHECK();

    pushheap();
    metafy_line();
    oll = zlemetall;
    ocs = zlemetacs;
    ol = dupstring(zlemetaline);
    expanding = 1;
    excs = zlemetacs;
    zlemetall = zlemetacs = 0;
    lexsave();
    /* We push ol as it will remain unchanged */
    inpush(ol, 0, NULL);
    strinbeg(1);
    noaliases = 1;
    noerrs = 1;
    exlast = inbufct;
    do {
	ctxtlex();
    } while (tok != ENDINPUT && tok != LEXERR);
    while (!lexstop)
	hgetc();
    /* We have to save errflags because it's reset in lexrestore. Since  *
     * noerrs was set to 1 errflag is true if there was a habort() which *
     * means that the expanded string is unusable.                       */
    err = errflag;
    noerrs = ne;
    noaliases = ona;
    strinend();
    inpop();
    zleparse = 0;
    lexrestore();
    expanding = 0;

    if (!err) {
	zlemetacs = excs;
	if (strcmp(zlemetaline, ol)) {
	    unmetafy_line();
	    /* For vi mode -- reset the beginning-of-insertion pointer   *
	     * to the beginning of the line.  This seems a little silly, *
	     * if we are, for example, expanding "exec !!".              */
	    if (viinsbegin > findbol())
		viinsbegin = findbol();
	    popheap();
	    return 1;
	}
    }

    strcpy(zlemetaline, ol);
    zlemetall = oll;
    zlemetacs = ocs;
    unmetafy_line();

    popheap();

    return 0;
}

/**/
void
fixmagicspace(void)
{
    lastchar = ' ';
#ifdef MULTIBYTE_SUPPORT
    /*
     * This is redundant if the multibyte encoding extends ASCII,
     * since lastchar is a full character, but it's safer anyway...
     */
    lastchar_wide = L' ';
    lastchar_wide_valid = 1;
#endif
}

/**/
int
magicspace(char **args)
{
    ZLE_STRING_T bangq;
    ZLE_CHAR_T zlebangchar[1];
    int ret;
#ifdef MULTIBYTE_SUPPORT
    mbstate_t mbs;
#endif

    fixmagicspace();

#ifdef MULTIBYTE_SUPPORT
    /*
     * Use mbrtowc() here for consistency and to ensure the
     * state is initialised properly.  bangchar is unsigned char,
     * but must be ASCII, so we simply cast the pointer.
     */
    memset(&mbs, 0, sizeof(mbs));
    if (mbrtowc(zlebangchar, (char *)&bangchar, 1, &mbs) < 0)
	return selfinsert(args);
#else
    zlebangchar[0] = bangchar;
#endif
    for (bangq = zleline; bangq < zleline + zlell; bangq++) {
	if (*bangq != zlebangchar[0])
	    continue;
	if (bangq[1] == ZWC('"') &&
	    (bangq == zleline || bangq[-1] == ZWC('\\')))
	    break;
    }

    if (!(ret = selfinsert(args)) &&
	(!bangq || bangq + 2 > zleline + zlecs))
	doexpandhist();
    return ret;
}

/**/
int
expandhistory(UNUSED(char **args))
{
    if (!doexpandhist())
	return 1;
    return 0;
}

static int cmdwb, cmdwe;

/**/
static char *
getcurcmd(void)
{
    int curlincmd;
    char *s = NULL;

    zleparse = 2;
    lexsave();
    metafy_line();
    inpush(dupstrspace(zlemetaline), 0, NULL);
    strinbeg(1);
    pushheap();
    do {
	curlincmd = incmdpos;
	ctxtlex();
	if (tok == ENDINPUT || tok == LEXERR)
	    break;
	if (tok == STRING && curlincmd) {
	    zsfree(s);
	    s = ztrdup(tokstr);
	    cmdwb = zlemetall - wordbeg;
	    cmdwe = zlemetall + 1 - inbufct;
	}
    }
    while (tok != ENDINPUT && tok != LEXERR && zleparse);
    popheap();
    strinend();
    inpop();
    errflag = zleparse = 0;
    unmetafy_line();
    lexrestore();

    return s;
}

/**/
int
processcmd(UNUSED(char **args))
{
    char *s;
    int m = zmult, na = noaliases;

    if (!strcmp(bindk->nam, "which-command"))
	noaliases = 1;
    s = getcurcmd();
    noaliases = na;
    if (!s)
	return 1;
    zmult = 1;
    pushline(zlenoargs);
    zmult = m;
    inststr(bindk->nam);
    inststr(" ");
    untokenize(s);

    inststr(quotename(s, NULL));

    zsfree(s);
    done = 1;
    return 0;
}

/**/
int
expandcmdpath(UNUSED(char **args))
{
    /*
     * zleline is not metafied for most of this function
     * (that happens within getcurcmd()).
     */
    int oldcs = zlecs, na = noaliases, strll;
    char *s, *str;
    ZLE_STRING_T zlestr;

    noaliases = 1;
    s = getcurcmd();
    noaliases = na;
    if (!s || cmdwb < 0 || cmdwe < cmdwb)
	return 1;
    str = findcmd(s, 1);
    zsfree(s);
    if (!str)
	return 1;
    zlecs = cmdwb;
    foredel(cmdwe - cmdwb);
    zlestr = stringaszleline(str, 0, &strll, NULL, NULL);
    spaceinline(strll);
    ZS_strncpy(zleline + zlecs, zlestr, strll);
    free(zlestr);
    zlecs = oldcs;
    if (zlecs >= cmdwe - 1)
	zlecs += cmdwe - cmdwb + strlen(str);
    if (zlecs > zlell)
	zlecs = zlell;
    return 0;
}

/* Extra function added by AR Iano-Fletcher. */
/* This is a expand/complete in the vein of wash. */

/**/
int
expandorcompleteprefix(char **args)
{
    int ret;

    comppref = 1;
    ret = expandorcomplete(args);
    if (zlecs && zleline[zlecs - 1] == ZWC(' '))
        makesuffixstr(NULL, "\\-", 0);
    comppref = 0;
    return ret;
}

/**/
int
endoflist(UNUSED(char **args))
{
    if (lastlistlen > 0) {
	int i;

	clearflag = 0;
	trashzle();

	for (i = lastlistlen; i > 0; i--)
	    putc('\n', shout);

	showinglist = lastlistlen = 0;

	if (sfcontext)
	    zrefresh();

	return 0;
    }
    return 1;
}
