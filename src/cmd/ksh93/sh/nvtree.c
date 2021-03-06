/***********************************************************************
 *                                                                      *
 *               This software is part of the ast package               *
 *          Copyright (c) 1982-2014 AT&T Intellectual Property          *
 *                      and is licensed under the                       *
 *                 Eclipse Public License, Version 1.0                  *
 *                    by AT&T Intellectual Property                     *
 *                                                                      *
 *                A copy of the License is available at                 *
 *          http://www.eclipse.org/org/documents/epl-v10.html           *
 *         (with md5 checksum b35adb5213ca9657e911e9befb180842)         *
 *                                                                      *
 *              Information and Software Systems Research               *
 *                            AT&T Research                             *
 *                           Florham Park NJ                            *
 *                                                                      *
 *                    David Korn <dgkorn@gmail.com>                     *
 *                                                                      *
 ***********************************************************************/
//
// Code for tree nodes and name walking.
//
//   David Korn
//   AT&T Labs
//
#include "config_ast.h"  // IWYU pragma: keep

#include <float.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "defs.h"

#include "argnod.h"
#include "ast_assert.h"
#include "cdt.h"
#include "fault.h"
#include "lexstates.h"
#include "name.h"
#include "sfio.h"
#include "shtable.h"
#include "stk.h"
#include "variables.h"

struct nvdir {
    Dt_t *root;
    Namval_t *hp;
    Namval_t *table;
    Namval_t *otable;
    Namval_t *(*nextnode)(Namval_t *, Dt_t *, Namfun_t *);
    Namfun_t *fun;
    struct nvdir *prev;
    size_t len;
    char *data;
};

static int Indent;
char *nv_getvtree(Namval_t *, Namfun_t *);
static_fn void put_tree(Namval_t *, const void *, int, Namfun_t *);
static_fn char *walk_tree(Namval_t *, Namval_t *, int);

static_fn int read_tree(Namval_t *np, Sfio_t *in, int n, Namfun_t *dp) {
    if (n >= 0) return -1;

    Shell_t *shp = sh_ptr(np);
    Sfio_t *sp, *iop;
    char *cp;
    int c;
    typedef int (*Shread_t)(Shell_t *, Sfio_t *, Sfio_t *);
    Shread_t fun;

    fun = *(void **)(dp + 1);
    if (fun) {
        iop = sftmp(SF_BUFSIZE * sizeof(char *));
        sfputr(iop, nv_name(np), '=');
        (*fun)(shp, in, iop);
        sfseek(iop, (Sfoff_t)0, SEEK_SET);
        goto done;
    }
    iop = in;
    while ((c = sfgetc(iop)) && isblank(c)) {
        ;  // empty loop
    }
    sfungetc(iop, c);
    sfprintf(shp->strbuf, "%s=%c", nv_name(np), 0);
    cp = sfstruse(shp->strbuf);
    sp = sfopen(NULL, cp, "s");
    sfstack(iop, sp);
done:
    c = sh_eval(shp, iop, SH_READEVAL);
    if (iop != in) sfclose(in);
    return c;
}

static_fn Namval_t *create_tree(Namval_t *np, const void *name, int flag, Namfun_t *dp) {
    Namfun_t *fp = dp;

    fp->dsize = 0;
    while ((fp = fp->next)) {
        if (fp->disc && fp->disc->createf) {
            np = (*fp->disc->createf)(np, name, flag, fp);
            if (np) dp->last = fp->last;
            return np;
        }
    }
    return (flag & NV_NOADD) ? 0 : np;
}

static_fn Namfun_t *clone_tree(Namval_t *np, Namval_t *mp, int flags, Namfun_t *fp) {
    Namfun_t *dp;

    if ((flags & NV_MOVE) && nv_type(np)) return fp;
    dp = nv_clone_disc(fp, flags);
    if ((flags & NV_COMVAR) && !(flags & NV_RAW)) {
        walk_tree(np, mp, flags);
        if ((flags & NV_MOVE) && !(fp->nofree & 1)) free(fp);
    }
    return dp;
}

static const Namdisc_t treedisc = {0,          put_tree, nv_getvtree, NULL, NULL,      create_tree,
                                   clone_tree, NULL,     NULL,        NULL, read_tree, NULL};

static_fn char *nextdot(const char *str, void *context) {
    char *cp;
    int c;

    if (*str == '.') str++;
    for (cp = (char *)str; *cp; cp++) {
        c = *cp;
        if (c == '[') {
            cp = nv_endsubscript(NULL, (char *)cp, 0, context);
            return *cp == '.' ? cp : 0;
        }
        if (c == '.') return cp;
    }
    return NULL;
}

static_fn Namfun_t *nextdisc(Namval_t *np) {
    Namfun_t *fp;

    if (nv_isref(np)) return NULL;
    for (fp = np->nvfun; fp; fp = fp->next) {
        if (fp && fp->disc && fp->disc->nextf) return fp;
    }
    return NULL;
}

void *nv_diropen(Namval_t *np, const char *name, void *context) {
    Shell_t *shp = (Shell_t *)context;
    const char *last;
    char *next;
    size_t c, len = strlen(name);
    struct nvdir *save, *dp;
    Namval_t *nq = 0, fake;
    Namfun_t *nfp = 0;

    dp = calloc(1, sizeof(struct nvdir) + len + 1);
    if (!dp) return NULL;

    memset(&fake, 0, sizeof(fake));
    dp->data = (char *)(dp + 1);
    if (name[len - 1] == '*' || name[len - 1] == '@') len -= 1;
    name = memcpy(dp->data, name, len);
    dp->data[len] = 0;
    dp->len = len;
    dp->root = shp->last_root ? shp->last_root : shp->var_tree;

    last = &name[len];
    if (!np) np = nv_search(name, dp->root, 0);
    if (!np || !nv_isvtree(np)) {
        while (1) {
            dp->table = shp->last_table;
            shp->last_table = 0;
            if (*(last = (char *)name) == 0) break;
            if (!(next = nextdot(last, (void *)shp))) break;
            *next = 0;
            np = nv_open(name, dp->root, NV_NOFAIL);
            *next = '.';
            if (!np || !nv_istable(np)) break;
            dp->root = nv_dict(np);
            name = next + 1;
        }
    }
    if (*name) {
        memset(&fake, 0, sizeof(fake));
        fake.nvname = (char *)name;
        dp->hp = (Namval_t *)dtprev(dp->root, &fake);
        if (dp->hp) {
            char *cp = nv_name(dp->hp);
            c = strlen(cp);
            if (strncmp(name, cp, c) || name[c] != '[') {
                dp->hp = (Namval_t *)dtnext(dp->root, dp->hp);
            } else {
                np = dp->hp;
                last = 0;
            }
        } else {
            dp->hp = (Namval_t *)dtfirst(dp->root);
        }
    } else {
        dp->hp = (Namval_t *)dtfirst(dp->root);
    }
    while (1) {
        if (!last) {
            next = 0;
        } else if ((next = nextdot(last, (void *)shp))) {
            c = *next;
            *next = 0;
        }
        if (!np) {
            if (nfp && nfp->disc && nfp->disc->createf) {
                np = (*nfp->disc->createf)(nq, last, 0, nfp);
                if (*nfp->last == '[') {
                    nv_endsubscript(np, nfp->last, NV_NOADD, np->nvshell);
                    nq = nv_opensub(np);
                    if (nq) np = nq;
                }
            } else {
                np = nv_search(last, dp->root, 0);
            }
        }
        if (next) *next = c;
        if (np == dp->hp && !next) dp->hp = (Namval_t *)dtnext(dp->root, dp->hp);
        if (np && ((nfp = nextdisc(np)) || nv_istable(np))) {
            save = calloc(1, sizeof(struct nvdir));
            if (!save) return NULL;
            *save = *dp;
            dp->prev = save;
            if (nv_istable(np)) {
                dp->root = nv_dict(np);
            } else {
                Namarr_t *ap;
                Namval_t *mp = nv_open(name, shp->var_tree, NV_VARNAME | NV_NOADD | NV_NOFAIL);
                int sub;
                if (mp && (ap = nv_arrayptr(mp)) && !ap->fun && !ap->flags &&
                    (sub = nv_aindex(mp)) >= 0) {
                    nv_putsub(np, NULL, sub, 0);
                }
                dp->root = (Dt_t *)np;
            }
            if (nfp) {
                dp->nextnode = nfp->disc->nextf;
                dp->table = np;
                dp->otable = shp->last_table;
                dp->fun = nfp;
                dp->hp = (*dp->nextnode)(np, NULL, nfp);
            } else {
                dp->nextnode = 0;
            }
        } else {
            break;
        }
        if (!next || next[1] == 0) break;
        last = next + 1;
        nq = np;
        np = 0;
    }
    return (void *)dp;
}

static_fn Namval_t *nextnode(struct nvdir *dp) {
    if (dp->nextnode) return (*dp->nextnode)(dp->hp, dp->root, dp->fun);
    if (dp->len && strncmp(dp->data, dp->hp->nvname, dp->len)) return NULL;
    return (Namval_t *)dtnext(dp->root, dp->hp);
}

char *nv_dirnext(void *dir) {
    Shell_t *shp = 0;
    struct nvdir *save, *dp = (struct nvdir *)dir;
    Namval_t *np, *last_table;
    char *cp;
    Namfun_t *nfp;
    Namval_t *nq;
    Namarr_t *ap = dp->table ? nv_arrayptr(dp->table) : 0;
    int dot = -1, xdot, flags;

    if (ap && !ap->fun && nv_type(dp->table) && (ap->flags & ARRAY_SCAN)) {
        dot = nv_aindex(dp->table);
        flags = ap->flags;
    }
    while (1) {
        if (!shp && dp->hp) shp = sh_ptr(dp->hp);
        while ((np = dp->hp)) {
#if 0
			char *sptr;
#endif
            ap = nv_arrayptr(np);
            if (ap) nv_putsub(np, NULL, 0, ARRAY_UNDEF);
            dp->hp = nextnode(dp);
            if (nv_isnull(np) && !nv_isarray(np) && !nv_isattr(np, NV_INTEGER)) continue;
            last_table = shp->last_table;
#if 0
			if(dp->table && dp->otable && !nv_isattr(dp->table,NV_MINIMAL))
			{
				sptr = dp->table->nvenv;
				dp->table->nvenv = (char*)dp->otable;
			}
#endif
            shp->last_table = dp->table;
            if (!dp->table) dot = -1;
            if (dot >= 0) {
                xdot = nv_aindex(dp->table);
                nv_putsub(dp->table, NULL, dot, flags);
            }
            cp = nv_name(np);
            if (dot >= 0) nv_putsub(dp->table, NULL, xdot, xdot < dot ? 0 : flags);

#if 0
			if(dp->table && dp->otable && !nv_isattr(dp->table,NV_MINIMAL))
				dp->table->nvenv = sptr;
#endif
            if (dp->nextnode && !dp->hp && (nq = (Namval_t *)dp->table)) {
                Namarr_t *aq = nv_arrayptr(nq);
                if (aq && (aq->flags & ARRAY_SCAN) && nv_nextsub(nq)) {
                    dp->hp = (*dp->nextnode)(np, NULL, dp->fun);
                }
            }
            shp->last_table = last_table;

            if (dp->len && strncmp(cp, dp->data, dp->len) != 0) continue;

            if ((nfp = nextdisc(np)) && (nfp->disc->getval || nfp->disc->getnum) &&
                nv_isvtree(np) && strcmp(cp, dp->data)) {
                nfp = 0;
            }

            if (!nfp && !nv_istable(np)) return cp;

            Dt_t *root;
            size_t len;
            if (nv_istable(np)) {
                root = nv_dict(np);
            } else {
                root = (Dt_t *)np;
            }
            // Check for recursive walk.
            for (save = dp; save; save = save->prev) {
                if (save->root == root) break;
            }
            if (save) return cp;
            len = strlen(cp);
            save = calloc(1, sizeof(struct nvdir) + len + 1);
            if (!save) return 0;
            *save = *dp;
            dp->prev = save;
            dp->root = root;
            dp->len = len - 1;
            dp->data = (char *)(save + 1);
            memcpy(dp->data, cp, len + 1);
            if (nfp && np->nvfun) {
#if 0
                Namarr_t *ap = nv_arrayptr(np);
                if(ap && (ap->flags&ARRAY_UNDEF))
                        nv_putsub(np,(char*)0,0,ARRAY_SCAN);
#endif
                dp->nextnode = nfp->disc->nextf;
                dp->otable = dp->table;
                dp->table = np;
                dp->fun = nfp;
                dp->hp = (*dp->nextnode)(np, NULL, nfp);
            } else {
                dp->nextnode = 0;
            }
            return cp;
        }
        if (!(save = dp->prev)) break;
        *dp = *save;
        free(save);
    }
    return NULL;
}

void nv_dirclose(void *dir) {
    struct nvdir *dp = (struct nvdir *)dir;
    if (dp->prev) nv_dirclose((void *)dp->prev);
    free(dir);
}

static_fn void outtype(Namval_t *np, Namfun_t *fp, Sfio_t *out, const char *prefix) {
    char *type = 0;
    Namval_t *tp = fp->type;

    assert(fp->disc);
    assert(fp->disc->typef);
    if (!tp) tp = (*fp->disc->typef)(np, fp);
    for (fp = fp->next; fp; fp = fp->next) {
        if (fp->type || (fp->disc && fp->disc->typef && (*fp->disc->typef)(np, fp))) {
            outtype(np, fp, out, prefix);
            break;
        }
    }
    if (prefix && *prefix == 't') {
        type = "-T";
    } else if (!prefix) {
        type = "type";
    }
    if (type) {
        char *cp = tp->nvname;
        cp = strrchr(cp, '.');
        if (cp) {
            cp++;
        } else {
            cp = tp->nvname;
        }
        sfprintf(out, "%s %s ", type, cp);
    }
}

//
// Print the attributes of name value pair give by <np>.
//
void nv_attribute(Namval_t *np, Sfio_t *out, char *prefix, int noname) {
    const Shtable_t *tp;
    char *cp;
    unsigned val, mask, attr;
    char *ip = 0;
    Namfun_t *fp = 0;
    Namval_t *typep = 0;

    for (fp = np->nvfun; fp; fp = fp->next) {
        if ((typep = fp->type) ||
            (fp->disc && fp->disc->typef && (typep = (*fp->disc->typef)(np, fp))))
            break;
    }
    if (np == typep) {
        fp = 0;
        typep = 0;
    }
    if (!fp && !nv_isattr(np, ~(NV_MINIMAL | NV_NOFREE))) {
        if (prefix && *prefix) {
            if (nv_isvtree(np)) {
                sfprintf(out, "%s -C ", prefix);
            } else if ((!np->nvalue.cp || np->nvalue.cp == Empty) &&
                       nv_isattr(np, ~NV_NOFREE) == NV_MINIMAL && strcmp(np->nvname, "_")) {
                sfputr(out, prefix, ' ');
            }
        }
        return;
    }

    if ((attr = nv_isattr(np, ~NV_NOFREE)) || fp) {
        if ((attr & (NV_NOPRINT | NV_INTEGER)) == NV_NOPRINT) attr &= ~NV_NOPRINT;
        if (!attr && !fp) return;
        if (fp) {
            prefix = Empty;
            attr &= NV_RDONLY | NV_ARRAY;
            if (nv_isattr(np, NV_REF | NV_TAGGED) == (NV_REF | NV_TAGGED)) {
                attr |= (NV_REF | NV_TAGGED);
            }
            if (typep) {
                cp = typep->nvname;
                cp = strrchr(cp, '.');
                if (cp) {
                    cp++;
                } else {
                    cp = typep->nvname;
                }
                sfputr(out, cp, ' ');
                fp = 0;
            }
        } else if (prefix && *prefix) {
            sfputr(out, prefix, ' ');
        }
        for (tp = shtab_attributes; *tp->sh_name; tp++) {
            val = tp->sh_number;
            mask = val;
            if (fp && (val & NV_INTEGER)) break;
            // The following test is needed to prevent variables with E attribute from being given
            // the F attribute as well.
            if (val == NV_DOUBLE && (attr & (NV_EXPNOTE | NV_HEXFLOAT))) continue;
            if (val & NV_INTEGER) {
                mask |= NV_DOUBLE;
            } else if (val & NV_HOST) {
                mask = NV_HOST;
            }
            if ((attr & mask) == val) {
                if (val == NV_ARRAY) {
                    Namarr_t *ap = nv_arrayptr(np);
                    char **xp = 0;
                    if (ap && is_associative(ap)) {
                        if (tp->sh_name[1] != 'A') continue;
                    } else if (tp->sh_name[1] == 'A') {
                        continue;
                    }
                    if ((ap && (ap->flags & ARRAY_TREE)) || (!ap && nv_isattr(np, NV_NOFREE))) {
                        if (prefix && *prefix) sfwrite(out, "-C ", 3);
                    }
                    if (ap && !is_associative(ap) && (xp = (char **)(ap + 1)) && *xp) {
                        ip = nv_namptr(*xp, 0)->nvname;
                    }
                }
                if (val == NV_UTOL || val == NV_LTOU) {
                    cp = (char *)nv_mapchar(np, NULL);
                    if (cp && strcmp(cp, tp->sh_name + 2)) {
                        sfprintf(out, "-M %s ", cp);
                        continue;
                    }
                }
                if (prefix) {
                    if (*tp->sh_name == '-') sfprintf(out, "%.2s ", tp->sh_name);
                    if (ip) {
                        sfprintf(out, "[%s] ", ip);
                        ip = 0;
                    }
                } else {
                    sfputr(out, tp->sh_name + 2, ' ');
                }
                if ((val & (NV_LJUST | NV_RJUST | NV_ZFILL)) && !(val & NV_INTEGER) &&
                    val != NV_HOST) {
                    sfprintf(out, "%d ", nv_size(np));
                }
                if (val == (NV_REF | NV_TAGGED)) attr &= ~(NV_REF | NV_TAGGED);
            }

            if (val != NV_INTEGER) continue;
            if (!nv_isattr(np, NV_INTEGER)) continue;

            int size = 10;
            if (nv_isattr(np, NV_DOUBLE | NV_EXPNOTE) == (NV_DOUBLE | NV_EXPNOTE)) {
                size = DBL_DIG;
                if (nv_isattr(np, NV_LONG)) {
                    size = LDBL_DIG;
                } else if (nv_isattr(np, NV_SHORT)) {
                    size = FLT_DIG;
                }
                size -= 2;
            }
            if (nv_size(np) != size) {
                if (nv_isattr(np, NV_DOUBLE) == NV_DOUBLE) {
                    cp = "precision";
                } else {
                    cp = "base";
                }
                if (!prefix) sfputr(out, cp, ' ');
                sfprintf(out, "%d ", nv_size(np));
            }
            break;
        }
        if (fp) outtype(np, fp, out, prefix);
        if (noname) return;
        sfputr(out, nv_name(np), '\n');
    }
}

struct Walk {
    Shell_t *shp;
    Sfio_t *out;
    Dt_t *root;
    int noscope;
    int indent;
    int nofollow;
    int array;
    int flags;
};

void nv_outnode(Namval_t *np, Sfio_t *out, int indent, int special) {
    char *fmtq = "";
    char *ep, *xp;
    Namval_t *mp;
    Namarr_t *ap = nv_arrayptr(np);
    int scan, tabs = 0, c, more;
    bool associative = false;
    int saveI = Indent, dot = -1;
    bool json = (special & NV_JSON);
    bool json_last = (special & NV_JSON_LAST);
    Shell_t *shp = (Shell_t *)np->nvshell;

    special &= ~(NV_JSON | NV_JSON_LAST);
    Indent = indent;
    if (ap) {
        sfputc(out, json ? '[' : '(');
        if (array_elem(ap) == 0) return;
        if (!(ap->flags & ARRAY_SCAN)) nv_putsub(np, NULL, 0, ARRAY_SCAN);
        if (indent >= 0) {
            sfputc(out, '\n');
            tabs = 1;
        }
        associative = (is_associative(ap) != 0);
        if (!associative && array_elem(ap) < nv_aimax(np) + 1) associative = true;
    }
    mp = nv_opensub(np);
    while (1) {
        if (mp && mp->nvalue.cp == Empty && !mp->nvfun) {
            more = nv_nextsub(np);
            goto skip;
        }
        if (mp && special && nv_isvtree(mp) && !nv_isarray(mp)) {
            if (!nv_nextsub(np)) break;
            mp = nv_opensub(np);
            continue;
        }
        if (tabs) sfnputc(out, '\t', Indent = ++indent);
        tabs = 0;
        if (associative || special) {
            Namarr_t *aq;
            if (mp && (aq = nv_arrayptr(mp)) && !aq->fun && array_elem(aq) < nv_aimax(mp) + 1) {
                sfwrite(out, "typeset -a ", 11);
            }
            if (!(fmtq = nv_getsub(np))) break;
            if (!json) {
                sfprintf(out, "[%s]=", sh_fmtstr(fmtq, '['));
            } else if (associative) {
                sfprintf(out, "%s: ", sh_fmtj(fmtq));
            }
        }
        if (ap && !is_associative(ap)) scan = ap->flags & ARRAY_SCAN;
        if (mp && nv_isarray(mp)) {
            nv_outnode(mp, out, indent, 0);
            if (indent > 0) sfnputc(out, '\t', indent);
            if (nv_arrayptr(mp)) sfputc(out, json ? ']' : ')');
            sfputc(out, indent >= 0 ? '\n' : ' ');
            if (ap && !is_associative(ap)) ap->flags |= scan;
            more = nv_nextsub(np);
            goto skip;
        }
        if (mp && nv_isvtree(mp)) {
            if (indent < 0) nv_onattr(mp, NV_EXPORT);
            nv_onattr(mp, NV_TABLE);
        }
        if (ap) dot = nv_aindex(np);
        ep = nv_getval(mp ? mp : np);
        if (dot >= 0) {
            nv_putsub(np, NULL, dot, 0);
        } else if (mp && associative) {
            nv_putsub(np, mp->nvname, 0, ARRAY_SCAN);
        }
        if (ep == Empty && !ap) ep = NULL;
        xp = 0;
        if (!ap && nv_isattr(np, NV_INTEGER | NV_LJUST) == NV_LJUST) {
            xp = ep + nv_size(np);
            while (--xp > ep && *xp == ' ') {
                ;  // empty loop
            }
            if (xp > ep || *xp != ' ') xp++;
            if (xp < (ep + nv_size(np))) {
                *xp = 0;
            } else {
                xp = 0;
            }
        }
        if (mp && nv_isvtree(mp)) {
            fmtq = ep;
        } else if (json) {
            if (nv_isattr(np, NV_INTEGER)) {
                Namval_t *tp;
                if ((tp = nv_type(np)) && strcmp(tp->nvname, "_Bool") == 0) {
                    fmtq = nv_getval(np);
                } else {
                    Sfdouble_t d = nv_getnum(np);
                    sfprintf(shp->strbuf, "%.*Lg", sizeof(d), d);
                    fmtq = sfstruse(shp->strbuf);
                }
            } else if (!(fmtq = sh_fmtj(ep))) {
                fmtq = "\"\"";
            }
        } else if (!ep && !mp && nv_isarray(np)) {
            fmtq = " ()";
        } else if (!(fmtq = sh_fmtq(ep))) {
            fmtq = "";
        } else if (!associative && (ep = strchr(fmtq, '='))) {
            char *qp = strchr(fmtq, '\'');
            if (!qp || qp > ep) {
                sfwrite(out, fmtq, ep - fmtq);
                sfputc(out, '\\');
                fmtq = ep;
            }
        }
        if (ap && !is_associative(ap)) ap->flags |= scan;
        more = nv_nextsub(np);
        if (json_last || (ap && !more)) json = 0;
        c = json ? ',' : '\n';
        if (indent < 0) {
            c = indent < -1 ? -1 : ';';
            if (ap || nv_isarray(np)) c = more ? ' ' : -1;
        }
        sfputr(out, fmtq, c);
        if (json) sfputc(out, '\n');
        if (xp) *xp = ' ';
    skip:
        if (!more) break;
        mp = nv_opensub(np);
        if (indent > 0 && !(mp && special && nv_isvtree(mp))) sfnputc(out, '\t', indent);
    }
    Indent = saveI;
}

static_fn void outname(Shell_t *shp, Sfio_t *out, char *name, int len, bool json) {
    if (json) {
        if (len < 0) len = strlen(name);
        sfputc(out, '"');
        if (*name == '[') {
            len -= 2;
            if (*++name == '\'') len--;
        }
    } else if (*name == '[' && name[-1] == '.') {
        name--;
    }
    sh_outname(shp, out, name, len);
    if (json) sfwrite(out, "\": ", 3);
}

static_fn void outval(char *name, const char *vname, struct Walk *wp) {
    Namval_t *tp = 0, *np, *nq, *last_table = wp->shp->last_table;
    Namfun_t *fp;
    int isarray = 0, special = 0, mode = 0;
    bool json = (wp->flags & NV_JSON);
    Dt_t *root = wp->root ? wp->root : wp->shp->var_base;

    if (*name != '.' || vname[strlen(vname) - 1] == ']') mode = NV_ARRAY;
    if (!(np = nv_open(vname, root, mode | NV_VARNAME | NV_NOADD | NV_NOFAIL | wp->noscope))) {
        wp->shp->last_table = last_table;
        wp->flags &= ~NV_COMVAR;
        return;
    }
    if (!wp->out) wp->shp->last_table = last_table;
    if (wp->shp->last_table) tp = nv_type(wp->shp->last_table);
    last_table = wp->shp->last_table;
    fp = nv_hasdisc(np, &treedisc);
    if (*name == '.') {
        if (nv_isattr(np, NV_BINARY) || nv_type(np)) return;
        if (fp && np->nvalue.cp && np->nvalue.cp != Empty) {
            nv_local = true;
            fp = 0;
        }
        if (fp) return;
        if (nv_isarray(np)) return;
    }
    if (!special && fp && !nv_isarray(np)) {
        Namfun_t *xp;
        if (!wp->out) {
            // TODO: Is this correct? Why would `nv_stack()` be called twice like this?
            (void)nv_stack(np, fp);
            fp = nv_stack(np, NULL);
            if (fp) free(fp);
            np->nvfun = 0;
            if (!nv_isattr(np, NV_MINIMAL)) np->nvenv = 0;
            return;
        }
        for (xp = fp->next; xp; xp = xp->next) {
            if (xp->disc && (xp->disc->getval || xp->disc->getnum)) break;
        }
        if (!xp) {
            if (nv_type(np) || !(wp->flags & NV_COMVAR)) {
                wp->flags &= ~NV_COMVAR;
                return;
            }
            if (wp->indent > 0) sfnputc(wp->out, '\t', wp->indent);
            nv_attribute(np, wp->out, "typeset", ' ');
            sfputr(wp->out, name, wp->indent > 0 ? '\n' : -1);
            return;
        }
    }
    wp->flags &= ~NV_COMVAR;
#if 0
	if(nv_isnull(np) && !nv_isarray(np) && !nv_isattr(np,NV_INTEGER))
		return;
#else
    if (!nv_isarray(np) && !nv_isattr(np, NV_INTEGER)) {
        if (nv_isnull(np)) return;
        if (np->nvalue.cp == Empty && tp &&
            (last_table->nvname[0] != '_' || last_table->nvname[1])) {
            for (fp = np->nvfun; fp; fp = fp->next) {
                if (fp->disc && (fp->disc->getval || fp->disc->getnum)) break;
            }
            if (!fp) return;
        }
    }
#endif
    if (special || (nv_isarray(np) && nv_arrayptr(np))) {
        isarray = 1;
        if (array_elem(nv_arrayptr(np)) == 0) {
            isarray = 2;
            if (tp && (last_table->nvname[0] != '_' || last_table->nvname[1])) return;
        } else {
            nq = nv_putsub(np, NULL, 0, ARRAY_SCAN | (wp->out && !nv_type(np) ? ARRAY_NOCHILD : 0));
        }
    }
    if (!wp->out) {
        _nv_unset(np, NV_RDONLY);
        if (wp->shp->subshell || (wp->flags != NV_RDONLY) ||
            nv_isattr(np, NV_MINIMAL | NV_NOFREE)) {
            wp->root = 0;
        }
        nv_delete(np, wp->root, nv_isattr(np, NV_MINIMAL) ? NV_NOFREE : 0);
        return;
    }
    if (isarray == 1 && !nq) {
        int c = json ? ':' : '=';
        if (wp->out->_next[-1] != c) return;
        if (json) sfputc(wp->out, ' ');
        sfputc(wp->out, json ? '[' : '(');
        if (wp->indent >= 0) sfputc(wp->out, '\n');
        return;
    }
    if (isarray == 0 && nv_isarray(np) && (nv_isnull(np) || np->nvalue.cp == Empty)) {
        isarray = 2;  // empty array
    }
    special |= wp->nofollow;
    if (!wp->array && wp->indent > 0) sfnputc(wp->out, '\t', wp->indent);
    if (!special) {
        if (*name != '.') {
            if (!json) nv_attribute(np, wp->out, "typeset", '=');
        }
        outname(wp->shp, wp->out, name, -1, json);
        if ((np->nvalue.cp && np->nvalue.cp != Empty) || nv_isattr(np, ~(NV_MINIMAL | NV_NOFREE)) ||
            nv_isvtree(np)) {
            if (!json) sfputc(wp->out, (isarray == 2 ? (wp->indent >= 0 ? '\n' : ';') : '='));
        }
        if (isarray == 2) return;
    }
    fp = np->nvfun;
    if (*name == '.' && !isarray) np->nvfun = 0;
    nv_outnode(np, wp->out, wp->indent, special | (wp->flags & (NV_JSON | NV_JSON_LAST)));
    if (*name == '.' && !isarray) np->nvfun = fp;
    if (isarray && !special) {
        if (wp->indent > 0) {
            sfnputc(wp->out, '\t', wp->indent);
            if (json && !(wp->flags & NV_JSON_LAST)) {
                sfwrite(wp->out, "],\n", 3);
            } else {
                sfwrite(wp->out, json ? "]\n" : ")\n", 2);
            }
        } else {
            sfwrite(wp->out, ");", 2);
        }
    }
}

//
// Format initialization list given a list of assignments <argp>.
//
static_fn char **genvalue(char **argv, const char *prefix, int n, struct Walk *wp) {
    char *cp, *nextcp, *arg;
    Sfio_t *outfile = wp->out;
    int r;
    Shell_t *shp = wp->shp;
    Namarr_t *ap;
    Namval_t *np, *tp;
    size_t m, l;
    bool json = (wp->flags & NV_JSON);
    bool array_parent = (wp->flags & NV_ARRAY);
    char endchar = json ? '}' : ')';

    wp->flags &= ~NV_ARRAY;
    if (n == 0) {
        m = strlen(prefix);
    } else if ((cp = nextdot(prefix, (void *)shp))) {
        m = cp - prefix;
    } else {
        m = strlen(prefix) - 1;
    }
    m++;
    wp->flags &= ~NV_COMVAR;
    if (outfile && !wp->array) {
        sfputc(outfile, json ? '{' : '(');
        if (wp->indent >= 0) {
            wp->indent++;
            sfputc(outfile, '\n');
        }
    }
    for (; (arg = *argv); argv++) {
        cp = arg + n;
        if (n == 0 && cp[m - 1] != '.') continue;
        if (n && cp[m - 1] == 0) break;
        if (n == 0 || strncmp(arg, prefix - n, m + n) == 0) {
            cp += m;
            r = 0;
            if (*cp == '.') cp++, r++;
            if (wp->indent < 0 && argv[1] == 0) wp->indent--;
            nextcp = nextdot(cp, (void *)shp);
            if (nextcp) {
                if (outfile) {
                    *nextcp = 0;
                    np = nv_open(arg, wp->root, NV_VARNAME | NV_NOADD | NV_NOFAIL | wp->noscope);
                    if (!np || (nv_isarray(np) && (!(tp = nv_opensub(np)) || !nv_isvtree(tp)))) {
                        *nextcp = '.';
                        continue;
                    }
                    if (*cp != '[' && (tp = nv_type(np)) && (ap = nv_arrayptr(np)) && !ap->fun) {
                        continue;
                    }
                    if (wp->indent >= 0) sfnputc(outfile, '\t', wp->indent);
                    if (!json && *cp != '[' && tp) {
                        char *sp;
                        sp = strrchr(tp->nvname, '.');
                        if (sp) {
                            sp++;
                        } else {
                            sp = tp->nvname;
                        }
                        sfputr(outfile, sp, ' ');
                    } else if (*cp != '[' && wp->indent >= 0 && nv_isvtree(np)) {
                        if (!json) nv_attribute(np, outfile, "typeset", ' ');
                    }
                    if (!array_parent) {
                        outname(shp, outfile, cp, nextcp - cp, json);
                        if (!json) sfputc(outfile, '=');
                    }
                    *nextcp = '.';
                } else {
                    outval(cp, arg, wp);
                    continue;
                }
                argv = genvalue(argv, cp, n + m + r, wp);
                if (wp->indent >= 0) sfputc(outfile, '\n');
                if (*argv) continue;
                break;
            } else if (outfile && !wp->nofollow && argv[1] &&
                       strncmp(arg, argv[1], l = strlen(arg)) == 0 && argv[1][l] == '[') {
                int k = 1;
                Namarr_t *aq = 0;
                np = nv_open(arg, wp->root, NV_VARNAME | NV_NOADD | wp->noscope);
                if (!np) continue;
                if ((wp->array = nv_isarray(np)) && (aq = nv_arrayptr(np))) k = array_elem(aq);
                if (wp->indent > 0) sfnputc(outfile, '\t', wp->indent);
                if (json) {
                    sfputc(outfile, '"');
                } else {
                    nv_attribute(np, outfile, "typeset", 1);
                }
                nv_close(np);
                sfputr(outfile, arg + m + r + (n ? n : 0), (k ? (json ? '"' : '=') : '\n'));
                if (json) sfputc(outfile, ':');
                if (!k) {
                    wp->array = 0;
                    continue;
                }
                wp->nofollow = 1;
                if (json && aq && !aq->fun) wp->flags |= NV_ARRAY;
                argv = genvalue(argv, cp, cp - arg, wp);
                if (wp->indent > 0) sfputc(outfile, '\n');
            } else if (outfile && *cp == '[' && cp[-1] != '.') {
                // Skip multi-dimensional arrays.
                if (*nv_endsubscript(NULL, cp, 0, (void *)shp) == '[') continue;
                if (wp->indent > 0) sfnputc(outfile, '\t', wp->indent);
                if (cp[-1] == '.') cp--;
                sfputr(outfile, cp, '=');
                if (*cp == '.') cp++;
                argv = genvalue(++argv, cp, cp - arg, wp);
                sfputc(outfile, wp->indent > 0 ? '\n' : ';');
            } else {
                if (n && *cp && cp[-1] != '.' && cp[-1] != '[') break;
                if (outfile && wp->indent < 0 && (wp->flags & NV_COMVAR)) sfputc(outfile, ';');
                wp->flags |= NV_COMVAR;
                if (argv[1]) {
                    ssize_t r0 = (cp - argv[0]) + strlen(cp);
                    ssize_t r1 = strlen(argv[1]);
                    if (r0 <= r1 && argv[1][r0] == '.' && strncmp(argv[0], argv[1], r0) == 0) {
                        wp->flags &= ~NV_COMVAR;
                    }
                }
                if ((wp->flags & NV_JSON) &&
                    (!argv[1] || strlen(argv[1]) < m + n || strncmp(argv[1], arg, m + n - 1))) {
                    wp->flags |= NV_JSON_LAST;
                }
                outval(cp, arg, wp);
                if (wp->array) {
                    if (wp->indent >= 0) {
                        wp->indent++;
                        if (json) endchar = ']';
                    } else {
                        assert(outfile);
                        sfputc(outfile, ' ');
                    }
                    wp->array = 0;
                }
            }
        } else {
            break;
        }
        wp->nofollow = 0;
    }
    wp->array = 0;
    wp->flags &= ~NV_COMVAR;
    if (outfile) {
        int c = prefix[m - 1];
        cp = (char *)prefix;
        if (c == '.') cp[m - 1] = 0;
        outval(".", prefix - n, wp);
        if (c == '.') cp[m - 1] = c;
        if (wp->indent > 0) sfnputc(outfile, '\t', --wp->indent);
        sfputc(outfile, endchar);
        if (json && wp->indent > 0 && *argv && strncmp(arg, argv[-1], n) == 0) sfputc(outfile, ',');
        if (*argv && n && wp->indent < 0) sfputc(outfile, ';');
    }
    wp->flags &= ~NV_JSON_LAST;
    return --argv;
}

//
// Walk the virtual tree and print or delete name-value pairs.
//
static_fn char *walk_tree(Namval_t *np, Namval_t *xp, int flags) {
    Shell_t *shp = sh_ptr(np);
    static Sfio_t *out;
    struct Walk walk;
    Sfio_t *outfile;
    Sfoff_t off = 0;
    int len, savtop = stktell(shp->stk);
    char *savptr = stkfreeze(shp->stk, 0);
    struct argnod *ap = 0;
    struct argnod *arglist = 0;
    char *name, *cp, **argv;
    char *subscript = 0;
    void *dir;
    int n = 0, noscope = (flags & NV_NOSCOPE);
    Namarr_t *arp = nv_arrayptr(np);
    Dt_t *save_tree = shp->var_tree, *last_root;
    Namval_t *mp = 0, *table;
    char *xpname = xp ? stkcopy(shp->stk, nv_name(xp)) : 0;

    memset(&walk, 0, sizeof(walk));
    walk.shp = shp;
    if (xp) {
        if (!(last_root = shp->last_root)) last_root = shp->var_tree;
        shp->last_root = shp->prev_root;
        shp->last_table = shp->prev_table;
    }
    if (shp->last_table) shp->last_root = nv_dict(shp->last_table);
    if (shp->last_root) shp->var_tree = shp->last_root;
    table = shp->last_table;
    sfputr(shp->stk, nv_name(np), -1);
    shp->last_table = table;
    if (arp && !(arp->flags & ARRAY_SCAN) && (subscript = nv_getsub(np))) {
        mp = nv_opensub(np);
        sfputc(shp->stk, '[');
        sfputr(shp->stk, subscript, ']');
        sfputc(shp->stk, '.');
    } else if (*stkptr(shp->stk, stktell(shp->stk) - 1) == ']') {
        mp = np;
    }
    name = stkfreeze(shp->stk, 1);
    shp->last_root = 0;
    if (shp->last_table && !nv_type(shp->last_table) && (cp = nv_name(shp->last_table)) &&
        strcmp(cp, ".sh") && (len = strlen(cp)) && strncmp(name, cp, len) == 0 &&
        name[len] == '.') {
        name += len + 1;
    }
    len = strlen(name);
    dir = nv_diropen(mp, name, (void *)shp);
    walk.root = shp->last_root ? shp->last_root : shp->var_tree;
    if (subscript) name[strlen(name) - 1] = 0;

    struct nvdir *odir = NULL;
    Namval_t *nq = NULL;
    while (true) {
        cp = nv_dirnext(dir);
        if (odir) {
            // TODO: Figure out if we should even be freeing the entity pointed to by `odir`.
            // If we do free it one of the `treemove` tests that involves `typeset -m` fails.
            // free(odir);
            odir = NULL;
        }
        if (nq) {
            nv_delete(nq, walk.root, 0);
            nq = NULL;
        }
        if (!cp) break;
        if (cp[len] != '.') continue;

        if (xp) {
            Dt_t *dp = shp->var_tree;
            Namval_t *mq;
            if (strlen(cp) <= len) continue;
            nq = nv_open(cp, walk.root, NV_VARNAME | NV_NOADD | NV_NOFAIL);
            if (!nq && (flags & NV_MOVE)) nq = nv_search(cp, walk.root, NV_NOADD);
            stkseek(shp->stk, 0);
            sfputr(shp->stk, xpname, -1);
            sfputr(shp->stk, cp + len, 0);
            shp->var_tree = save_tree;
            mq = nv_open(stkptr(shp->stk, 0), last_root, NV_VARNAME | NV_NOFAIL);
            shp->var_tree = dp;
            if (nq && mq) {
                struct nvdir *dp = (struct nvdir *)dir;
                char *nvenv = mq->nvenv;
                // Related to the TODO above this condition appears to only ever be true if
                // flags&NV_MOVE is true.
                if (dp->table == nq) {
                    dp = dp->prev;
                    odir = dir;
                    dir = dp;
                }
                nv_clone(nq, mq, flags | NV_RAW);
                mq->nvenv = nvenv;
                if (!(flags & NV_MOVE)) nq = NULL;
            }
            continue;
        }
        stkseek(shp->stk, ARGVAL);
        sfputr(shp->stk, cp, -1);
        ap = (struct argnod *)stkfreeze(shp->stk, 1);
        ap->argflag = ARG_RAW;
        ap->argchn.ap = arglist;
        n++;
        arglist = ap;
    }

    nv_dirclose(dir);
    if (xp) {
        shp->var_tree = save_tree;
        return NULL;
    }
    argv = (char **)stkalloc(shp->stk, (n + 1) * sizeof(char *));
    argv += n;
    *argv = 0;
    for (; ap; ap = ap->argchn.ap) *--argv = ap->argval;
    if (flags & 1) {
        outfile = 0;
    } else if (!(outfile = out)) {
        outfile = out = sfnew(NULL, NULL, -1, -1, SF_WRITE | SF_STRING);
    } else if (flags & NV_TABLE) {
        off = sftell(outfile);
    } else {
        sfseek(outfile, 0L, SEEK_SET);
    }
    walk.out = outfile;
    walk.indent = (flags & NV_EXPORT) ? -1 : Indent;
    walk.nofollow = 0;
    walk.noscope = noscope;
    walk.array = 0;
    walk.flags = flags;
    genvalue(argv, name, 0, &walk);
    stkset(shp->stk, savptr, savtop);
    shp->var_tree = save_tree;
    if (!outfile) return NULL;
    sfputc(out, 0);
    sfseek(out, off, SEEK_SET);
    return (char *)out->_data + off;
}

Namfun_t *nv_isvtree(Namval_t *np) {
    if (np == SH_STATS || np == SH_SIG) return (Namfun_t *)1;
    if (np) return nv_hasdisc(np, &treedisc);
    return NULL;
}

//
// Get discipline for compound initializations.
//
char *nv_getvtree(Namval_t *np, Namfun_t *fp) {
    int flags = 0;
    for (; fp && fp->next; fp = fp->next) {
        if (fp->next->disc && (fp->next->disc->getnum || fp->next->disc->getval)) {
            return nv_getv(np, fp);
        }
    }
    if (nv_isattr(np, NV_BINARY) && !nv_isattr(np, NV_RAW)) return nv_getv(np, fp);
    if (nv_isattr(np, NV_ARRAY) && !nv_type(np) && nv_arraychild(np, NULL, 0) == np) {
        return nv_getv(np, fp);
    }
    flags = nv_isattr(np, NV_EXPORT | NV_TAGGED);
    if (flags) nv_offattr(np, NV_EXPORT | NV_TAGGED);
    flags |= nv_isattr(np, NV_TABLE);
    if (flags) nv_offattr(np, NV_TABLE);
    return walk_tree(np, NULL, flags);
}

//
// Put discipline for compound initializations.
//
static_fn void put_tree(Namval_t *np, const void *val, int flags, Namfun_t *fp) {
    struct Namarray *ap;
    int nleft = 0;

    if (!val && !fp->next && nv_isattr(np, NV_NOFREE)) return;
    if (!nv_isattr(np, (NV_INTEGER | NV_BINARY))) {
        Shell_t *shp = sh_ptr(np);
        Namval_t *last_table = shp->last_table;
        Dt_t *last_root = shp->last_root;
        // Note that val may be non-NULL but point to an empty string which will cause mp == NULL;
        Namval_t *mp = nv_open(val, shp->var_tree, NV_VARNAME | NV_NOADD | NV_ARRAY | NV_NOFAIL);
        if (mp && nv_isvtree(mp)) {
            shp->prev_table = shp->last_table;
            shp->prev_root = shp->last_root;
            shp->last_table = last_table;
            shp->last_root = last_root;
            if (!(flags & NV_APPEND)) walk_tree(np, NULL, (flags & NV_NOSCOPE) | 1);
            nv_clone(mp, np, NV_COMVAR);
            return;
        }
        walk_tree(np, NULL, (flags & NV_NOSCOPE) | 1);
    }
    nv_putv(np, val, flags, fp);
    if (val && nv_isattr(np, (NV_INTEGER | NV_BINARY))) return;
    ap = nv_arrayptr(np);
    if (ap) nleft = array_elem(ap);
    if (nleft == 0) {
        // TODO: Is this correct? Why would `nv_stack()` be called twice like this?
        (void)nv_stack(np, fp);
        fp = nv_stack(np, NULL);
        if (fp) free(fp);
    }
}

//
// Insert discipline to cause $x to print current tree.
//
void nv_setvtree(Namval_t *np) {
    Shell_t *shp = sh_ptr(np);
    Namfun_t *nfp;

    if (shp->subshell) sh_assignok(np, 1);
    if (nv_hasdisc(np, &treedisc)) return;
    nfp = calloc(1, sizeof(Namfun_t) + sizeof(void *));
    *(void **)(nfp + 1) = 0;
    nfp->disc = &treedisc;
    nfp->dsize = sizeof(Namfun_t);
    nv_stack(np, nfp);
}
