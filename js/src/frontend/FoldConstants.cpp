/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/FloatingPoint.h"

#include "jslibmath.h"
#if JS_HAS_XML_SUPPORT
#include "jsxml.h"
#endif

#include "frontend/FoldConstants.h"
#include "frontend/ParseNode.h"
#include "frontend/Parser.h"
#include "vm/NumericConversions.h"

#include "jsatominlines.h"

#include "vm/String-inl.h"

using namespace js;
using namespace js::frontend;

static ParseNode *
ContainsVarOrConst(ParseNode *pn)
{
    if (!pn)
        return NULL;
    if (pn->isKind(PNK_VAR) || pn->isKind(PNK_CONST))
        return pn;
    switch (pn->getArity()) {
      case PN_LIST:
        for (ParseNode *pn2 = pn->pn_head; pn2; pn2 = pn2->pn_next) {
            if (ParseNode *pnt = ContainsVarOrConst(pn2))
                return pnt;
        }
        break;
      case PN_TERNARY:
        if (ParseNode *pnt = ContainsVarOrConst(pn->pn_kid1))
            return pnt;
        if (ParseNode *pnt = ContainsVarOrConst(pn->pn_kid2))
            return pnt;
        return ContainsVarOrConst(pn->pn_kid3);
      case PN_BINARY:
        /*
         * Limit recursion if pn is a binary expression, which can't contain a
         * var statement.
         */
        if (!pn->isOp(JSOP_NOP))
            return NULL;
        if (ParseNode *pnt = ContainsVarOrConst(pn->pn_left))
            return pnt;
        return ContainsVarOrConst(pn->pn_right);
      case PN_UNARY:
        if (!pn->isOp(JSOP_NOP))
            return NULL;
        return ContainsVarOrConst(pn->pn_kid);
      case PN_NAME:
        return ContainsVarOrConst(pn->maybeExpr());
      default:;
    }
    return NULL;
}

/*
 * Fold from one constant type to another.
 * XXX handles only strings and numbers for now
 */
static bool
FoldType(JSContext *cx, ParseNode *pn, ParseNodeKind kind)
{
    if (!pn->isKind(kind)) {
        switch (kind) {
          case PNK_NUMBER:
            if (pn->isKind(PNK_STRING)) {
                double d;
                if (!ToNumber(cx, StringValue(pn->pn_atom), &d))
                    return false;
                pn->pn_dval = d;
                pn->setKind(PNK_NUMBER);
                pn->setOp(JSOP_DOUBLE);
            }
            break;

          case PNK_STRING:
            if (pn->isKind(PNK_NUMBER)) {
                JSString *str = js_NumberToString(cx, pn->pn_dval);
                if (!str)
                    return false;
                pn->pn_atom = AtomizeString(cx, str);
                if (!pn->pn_atom)
                    return false;
                pn->setKind(PNK_STRING);
                pn->setOp(JSOP_STRING);
            }
            break;

          default:;
        }
    }
    return true;
}

/*
 * Fold two numeric constants.  Beware that pn1 and pn2 are recycled, unless
 * one of them aliases pn, so you can't safely fetch pn2->pn_next, e.g., after
 * a successful call to this function.
 */
static bool
FoldBinaryNumeric(JSContext *cx, JSOp op, ParseNode *pn1, ParseNode *pn2,
                  ParseNode *pn, Parser *parser)
{
    double d, d2;
    int32_t i, j;

    JS_ASSERT(pn1->isKind(PNK_NUMBER) && pn2->isKind(PNK_NUMBER));
    d = pn1->pn_dval;
    d2 = pn2->pn_dval;
    switch (op) {
      case JSOP_LSH:
      case JSOP_RSH:
        i = ToInt32(d);
        j = ToInt32(d2);
        j &= 31;
        d = (op == JSOP_LSH) ? i << j : i >> j;
        break;

      case JSOP_URSH:
        j = ToInt32(d2);
        j &= 31;
        d = ToUint32(d) >> j;
        break;

      case JSOP_ADD:
        d += d2;
        break;

      case JSOP_SUB:
        d -= d2;
        break;

      case JSOP_MUL:
        d *= d2;
        break;

      case JSOP_DIV:
        if (d2 == 0) {
#if defined(XP_WIN)
            /* XXX MSVC miscompiles such that (NaN == 0) */
            if (MOZ_DOUBLE_IS_NaN(d2))
                d = js_NaN;
            else
#endif
            if (d == 0 || MOZ_DOUBLE_IS_NaN(d))
                d = js_NaN;
            else if (MOZ_DOUBLE_IS_NEGATIVE(d) != MOZ_DOUBLE_IS_NEGATIVE(d2))
                d = js_NegativeInfinity;
            else
                d = js_PositiveInfinity;
        } else {
            d /= d2;
        }
        break;

      case JSOP_MOD:
        if (d2 == 0) {
            d = js_NaN;
        } else {
            d = js_fmod(d, d2);
        }
        break;

      default:;
    }

    /* Take care to allow pn1 or pn2 to alias pn. */
    if (pn1 != pn)
        parser->freeTree(pn1);
    if (pn2 != pn)
        parser->freeTree(pn2);
    pn->setKind(PNK_NUMBER);
    pn->setOp(JSOP_DOUBLE);
    pn->setArity(PN_NULLARY);
    pn->pn_dval = d;
    return true;
}

#if JS_HAS_XML_SUPPORT

static bool
FoldXMLConstants(JSContext *cx, ParseNode *pn, Parser *parser)
{
    JS_ASSERT(pn->isArity(PN_LIST));
    ParseNodeKind kind = pn->getKind();
    ParseNode **pnp = &pn->pn_head;
    ParseNode *pn1 = *pnp;
    RootedString accum(cx);
    RootedString str(cx);
    if ((pn->pn_xflags & PNX_CANTFOLD) == 0) {
        if (kind == PNK_XMLETAGO)
            accum = cx->runtime->atomState.etagoAtom;
        else if (kind == PNK_XMLSTAGO || kind == PNK_XMLPTAGC)
            accum = cx->runtime->atomState.stagoAtom;
    }

    /*
     * GC Rooting here is tricky: for most of the loop, |accum| is safe via
     * the newborn string root. However, when |pn2->getKind()| is PNK_XMLCDATA,
     * PNK_XMLCOMMENT, or PNK_XMLPI it is knocked out of the newborn root.
     * Therefore, we have to add additonal protection from GC nesting under
     * js_ConcatStrings.
     */
    ParseNode *pn2;
    uint32_t i, j;
    for (pn2 = pn1, i = j = 0; pn2; pn2 = pn2->pn_next, i++) {
        /* The parser already rejected end-tags with attributes. */
        JS_ASSERT(kind != PNK_XMLETAGO || i == 0);
        switch (pn2->getKind()) {
          case PNK_XMLATTR:
            if (!accum)
                goto cantfold;
            /* FALL THROUGH */
          case PNK_XMLNAME:
          case PNK_XMLSPACE:
          case PNK_XMLTEXT:
          case PNK_STRING:
            if (pn2->isArity(PN_LIST))
                goto cantfold;
            str = pn2->pn_atom;
            break;

          case PNK_XMLCDATA:
            str = js_MakeXMLCDATAString(cx, pn2->pn_atom);
            if (!str)
                return false;
            break;

          case PNK_XMLCOMMENT:
            str = js_MakeXMLCommentString(cx, pn2->pn_atom);
            if (!str)
                return false;
            break;

          case PNK_XMLPI: {
            XMLProcessingInstruction &pi = pn2->asXMLProcessingInstruction();
            str = js_MakeXMLPIString(cx, pi.target(), pi.data());
            if (!str)
                return false;
            break;
          }

          cantfold:
          default:
            JS_ASSERT(*pnp == pn1);
            if ((kind == PNK_XMLSTAGO || kind == PNK_XMLPTAGC) &&
                (i & 1) ^ (j & 1)) {
#ifdef DEBUG_brendanXXX
                printf("1: %d, %d => ", i, j);
                if (accum)
                    FileEscapedString(stdout, accum, 0);
                else
                    fputs("NULL", stdout);
                fputc('\n', stdout);
#endif
            } else if (accum && pn1 != pn2) {
                while (pn1->pn_next != pn2) {
                    pn1 = parser->freeTree(pn1);
                    --pn->pn_count;
                }
                pn1->setKind(PNK_XMLTEXT);
                pn1->setOp(JSOP_STRING);
                pn1->setArity(PN_NULLARY);
                pn1->pn_atom = AtomizeString(cx, accum);
                if (!pn1->pn_atom)
                    return false;
                JS_ASSERT(pnp != &pn1->pn_next);
                *pnp = pn1;
            }
            pnp = &pn2->pn_next;
            pn1 = *pnp;
            accum = NULL;
            continue;
        }

        if (accum) {
            {
                str = ((kind == PNK_XMLSTAGO || kind == PNK_XMLPTAGC) && i != 0)
                      ? js_AddAttributePart(cx, i & 1, accum, str)
                      : js_ConcatStrings(cx, accum, str);
            }
            if (!str)
                return false;
#ifdef DEBUG_brendanXXX
            printf("2: %d, %d => ", i, j);
            FileEscapedString(stdout, str, 0);
            printf(" (%u)\n", str->length());
#endif
            ++j;
        }
        accum = str;
    }

    if (accum) {
        str = NULL;
        if ((pn->pn_xflags & PNX_CANTFOLD) == 0) {
            if (kind == PNK_XMLPTAGC)
                str = cx->runtime->atomState.ptagcAtom;
            else if (kind == PNK_XMLSTAGO || kind == PNK_XMLETAGO)
                str = cx->runtime->atomState.tagcAtom;
        }
        if (str) {
            accum = js_ConcatStrings(cx, accum, str);
            if (!accum)
                return false;
        }

        JS_ASSERT(*pnp == pn1);
        while (pn1->pn_next) {
            pn1 = parser->freeTree(pn1);
            --pn->pn_count;
        }
        pn1->setKind(PNK_XMLTEXT);
        pn1->setOp(JSOP_STRING);
        pn1->setArity(PN_NULLARY);
        pn1->pn_atom = AtomizeString(cx, accum);
        if (!pn1->pn_atom)
            return false;
        JS_ASSERT(pnp != &pn1->pn_next);
        *pnp = pn1;
    }

    if (pn1 && pn->pn_count == 1) {
        /*
         * Only one node under pn, and it has been folded: move pn1 onto pn
         * unless pn is an XML root (in which case we need it to tell the code
         * generator to emit a JSOP_TOXML or JSOP_TOXMLLIST op).  If pn is an
         * XML root *and* it's a point-tag, rewrite it to PNK_XMLELEM to avoid
         * extra "<" and "/>" bracketing at runtime.
         */
        if (!(pn->pn_xflags & PNX_XMLROOT)) {
            pn->become(pn1);
        } else if (kind == PNK_XMLPTAGC) {
            pn->setKind(PNK_XMLELEM);
            pn->setOp(JSOP_TOXML);
        }
    }
    return true;
}

#endif /* JS_HAS_XML_SUPPORT */

enum Truthiness { Truthy, Falsy, Unknown };

static Truthiness
Boolish(ParseNode *pn)
{
    switch (pn->getOp()) {
      case JSOP_DOUBLE:
        return (pn->pn_dval != 0 && !MOZ_DOUBLE_IS_NaN(pn->pn_dval)) ? Truthy : Falsy;

      case JSOP_STRING:
        return (pn->pn_atom->length() > 0) ? Truthy : Falsy;

#if JS_HAS_GENERATOR_EXPRS
      case JSOP_CALL:
      {
        /*
         * A generator expression as an if or loop condition has no effects, it
         * simply results in a truthy object reference. This condition folding
         * is needed for the decompiler. See bug 442342 and bug 443074.
         */
        if (pn->pn_count != 1)
            return Unknown;
        ParseNode *pn2 = pn->pn_head;
        if (!pn2->isKind(PNK_FUNCTION))
            return Unknown;
        if (!(pn2->pn_funbox->inGenexpLambda))
            return Unknown;
        return Truthy;
      }
#endif

      case JSOP_DEFFUN:
      case JSOP_LAMBDA:
      case JSOP_TRUE:
        return Truthy;

      case JSOP_NULL:
      case JSOP_FALSE:
        return Falsy;

      default:
        return Unknown;
    }
}

bool
frontend::FoldConstants(JSContext *cx, ParseNode *pn, Parser *parser, bool inGenexpLambda,
                        bool inCond)
{
    ParseNode *pn1 = NULL, *pn2 = NULL, *pn3 = NULL;

    JS_CHECK_RECURSION(cx, return false);

    switch (pn->getArity()) {
      case PN_FUNC:
        if (!FoldConstants(cx, pn->pn_body, parser, pn->pn_funbox->inGenexpLambda))
            return false;
        break;

      case PN_LIST:
      {
        /* Propagate inCond through logical connectives. */
        bool cond = inCond && (pn->isKind(PNK_OR) || pn->isKind(PNK_AND));

        /* Don't fold a parenthesized call expression. See bug 537673. */
        pn1 = pn2 = pn->pn_head;
        if ((pn->isKind(PNK_LP) || pn->isKind(PNK_NEW)) && pn2->isInParens())
            pn2 = pn2->pn_next;

        /* Save the list head in pn1 for later use. */
        for (; pn2; pn2 = pn2->pn_next) {
            if (!FoldConstants(cx, pn2, parser, inGenexpLambda, cond))
                return false;
        }
        break;
      }

      case PN_TERNARY:
        /* Any kid may be null (e.g. for (;;)). */
        pn1 = pn->pn_kid1;
        pn2 = pn->pn_kid2;
        pn3 = pn->pn_kid3;
        if (pn1 && !FoldConstants(cx, pn1, parser, inGenexpLambda, pn->isKind(PNK_IF)))
            return false;
        if (pn2) {
            if (!FoldConstants(cx, pn2, parser, inGenexpLambda, pn->isKind(PNK_FORHEAD)))
                return false;
            if (pn->isKind(PNK_FORHEAD) && pn2->isOp(JSOP_TRUE)) {
                parser->freeTree(pn2);
                pn->pn_kid2 = NULL;
            }
        }
        if (pn3 && !FoldConstants(cx, pn3, parser, inGenexpLambda))
            return false;
        break;

      case PN_BINARY:
        pn1 = pn->pn_left;
        pn2 = pn->pn_right;

        /* Propagate inCond through logical connectives. */
        if (pn->isKind(PNK_OR) || pn->isKind(PNK_AND)) {
            if (!FoldConstants(cx, pn1, parser, inGenexpLambda, inCond))
                return false;
            if (!FoldConstants(cx, pn2, parser, inGenexpLambda, inCond))
                return false;
            break;
        }

        /* First kid may be null (for default case in switch). */
        if (pn1 && !FoldConstants(cx, pn1, parser, inGenexpLambda, pn->isKind(PNK_WHILE)))
            return false;
        if (!FoldConstants(cx, pn2, parser, inGenexpLambda, pn->isKind(PNK_DOWHILE)))
            return false;
        break;

      case PN_UNARY:
        pn1 = pn->pn_kid;

        /*
         * Kludge to deal with typeof expressions: because constant folding
         * can turn an expression into a name node, we have to check here,
         * before folding, to see if we should throw undefined name errors.
         *
         * NB: We know that if pn->pn_op is JSOP_TYPEOF, pn1 will not be
         * null. This assumption does not hold true for other unary
         * expressions.
         */
        if (pn->isOp(JSOP_TYPEOF) && !pn1->isKind(PNK_NAME))
            pn->setOp(JSOP_TYPEOFEXPR);

        if (pn1 && !FoldConstants(cx, pn1, parser, inGenexpLambda, pn->isOp(JSOP_NOT)))
            return false;
        break;

      case PN_NAME:
        /*
         * Skip pn1 down along a chain of dotted member expressions to avoid
         * excessive recursion.  Our only goal here is to fold constants (if
         * any) in the primary expression operand to the left of the first
         * dot in the chain.
         */
        if (!pn->isUsed()) {
            pn1 = pn->pn_expr;
            while (pn1 && pn1->isArity(PN_NAME) && !pn1->isUsed())
                pn1 = pn1->pn_expr;
            if (pn1 && !FoldConstants(cx, pn1, parser, inGenexpLambda))
                return false;
        }
        break;

      case PN_NULLARY:
        break;
    }

    switch (pn->getKind()) {
      case PNK_IF:
        if (ContainsVarOrConst(pn2) || ContainsVarOrConst(pn3))
            break;
        /* FALL THROUGH */

      case PNK_CONDITIONAL:
        /* Reduce 'if (C) T; else E' into T for true C, E for false. */
        switch (pn1->getKind()) {
          case PNK_NUMBER:
            if (pn1->pn_dval == 0 || MOZ_DOUBLE_IS_NaN(pn1->pn_dval))
                pn2 = pn3;
            break;
          case PNK_STRING:
            if (pn1->pn_atom->length() == 0)
                pn2 = pn3;
            break;
          case PNK_TRUE:
            break;
          case PNK_FALSE:
          case PNK_NULL:
            pn2 = pn3;
            break;
          default:
            /* Early return to dodge common code that copies pn2 to pn. */
            return true;
        }

#if JS_HAS_GENERATOR_EXPRS
        /* Don't fold a trailing |if (0)| in a generator expression. */
        if (!pn2 && inGenexpLambda)
            break;
#endif

        if (pn2 && !pn2->isDefn())
            pn->become(pn2);
        if (!pn2 || (pn->isKind(PNK_SEMI) && !pn->pn_kid)) {
            /*
             * False condition and no else, or an empty then-statement was
             * moved up over pn.  Either way, make pn an empty block (not an
             * empty statement, which does not decompile, even when labeled).
             * NB: pn must be a PNK_IF as PNK_CONDITIONAL can never have a null
             * kid or an empty statement for a child.
             */
            pn->setKind(PNK_STATEMENTLIST);
            pn->setArity(PN_LIST);
            pn->makeEmpty();
        }
        parser->freeTree(pn2);
        if (pn3 && pn3 != pn2)
            parser->freeTree(pn3);
        break;

      case PNK_OR:
      case PNK_AND:
        if (inCond) {
            if (pn->isArity(PN_LIST)) {
                ParseNode **pnp = &pn->pn_head;
                JS_ASSERT(*pnp == pn1);
                uint32_t orig = pn->pn_count;
                do {
                    Truthiness t = Boolish(pn1);
                    if (t == Unknown) {
                        pnp = &pn1->pn_next;
                        continue;
                    }
                    if ((t == Truthy) == pn->isKind(PNK_OR)) {
                        for (pn2 = pn1->pn_next; pn2; pn2 = pn3) {
                            pn3 = pn2->pn_next;
                            parser->freeTree(pn2);
                            --pn->pn_count;
                        }
                        pn1->pn_next = NULL;
                        break;
                    }
                    JS_ASSERT((t == Truthy) == pn->isKind(PNK_AND));
                    if (pn->pn_count == 1)
                        break;
                    *pnp = pn1->pn_next;
                    parser->freeTree(pn1);
                    --pn->pn_count;
                } while ((pn1 = *pnp) != NULL);

                // We may have to change arity from LIST to BINARY.
                pn1 = pn->pn_head;
                if (pn->pn_count == 2) {
                    pn2 = pn1->pn_next;
                    pn1->pn_next = NULL;
                    JS_ASSERT(!pn2->pn_next);
                    pn->setArity(PN_BINARY);
                    pn->pn_left = pn1;
                    pn->pn_right = pn2;
                } else if (pn->pn_count == 1) {
                    pn->become(pn1);
                    parser->freeTree(pn1);
                } else if (orig != pn->pn_count) {
                    // Adjust list tail.
                    pn2 = pn1->pn_next;
                    for (; pn1; pn2 = pn1, pn1 = pn1->pn_next)
                        ;
                    pn->pn_tail = &pn2->pn_next;
                }
            } else {
                Truthiness t = Boolish(pn1);
                if (t != Unknown) {
                    if ((t == Truthy) == pn->isKind(PNK_OR)) {
                        parser->freeTree(pn2);
                        pn->become(pn1);
                    } else {
                        JS_ASSERT((t == Truthy) == pn->isKind(PNK_AND));
                        parser->freeTree(pn1);
                        pn->become(pn2);
                    }
                }
            }
        }
        break;

      case PNK_SUBASSIGN:
      case PNK_BITORASSIGN:
      case PNK_BITXORASSIGN:
      case PNK_BITANDASSIGN:
      case PNK_LSHASSIGN:
      case PNK_RSHASSIGN:
      case PNK_URSHASSIGN:
      case PNK_MULASSIGN:
      case PNK_DIVASSIGN:
      case PNK_MODASSIGN:
        /*
         * Compound operators such as *= should be subject to folding, in case
         * the left-hand side is constant, and so that the decompiler produces
         * the same string that you get from decompiling a script or function
         * compiled from that same string.  += is special and so must be
         * handled below.
         */
        goto do_binary_op;

      case PNK_ADDASSIGN:
        JS_ASSERT(pn->isOp(JSOP_ADD));
        /* FALL THROUGH */
      case PNK_ADD:
        if (pn->isArity(PN_LIST)) {
            /*
             * Any string literal term with all others number or string means
             * this is a concatenation.  If any term is not a string or number
             * literal, we can't fold.
             */
            JS_ASSERT(pn->pn_count > 2);
            if (pn->pn_xflags & PNX_CANTFOLD)
                return true;
            if (pn->pn_xflags != PNX_STRCAT)
                goto do_binary_op;

            /* Ok, we're concatenating: convert non-string constant operands. */
            size_t length = 0;
            for (pn2 = pn1; pn2; pn2 = pn2->pn_next) {
                if (!FoldType(cx, pn2, PNK_STRING))
                    return false;
                /* XXX fold only if all operands convert to string */
                if (!pn2->isKind(PNK_STRING))
                    return true;
                length += pn2->pn_atom->length();
            }

            /* Allocate a new buffer and string descriptor for the result. */
            jschar *chars = (jschar *) cx->malloc_((length + 1) * sizeof(jschar));
            if (!chars)
                return false;
            chars[length] = 0;
            JSString *str = js_NewString(cx, chars, length);
            if (!str) {
                cx->free_(chars);
                return false;
            }

            /* Fill the buffer, advancing chars and recycling kids as we go. */
            for (pn2 = pn1; pn2; pn2 = parser->freeTree(pn2)) {
                JSAtom *atom = pn2->pn_atom;
                size_t length2 = atom->length();
                js_strncpy(chars, atom->chars(), length2);
                chars += length2;
            }
            JS_ASSERT(*chars == 0);

            /* Atomize the result string and mutate pn to refer to it. */
            pn->pn_atom = AtomizeString(cx, str);
            if (!pn->pn_atom)
                return false;
            pn->setKind(PNK_STRING);
            pn->setOp(JSOP_STRING);
            pn->setArity(PN_NULLARY);
            break;
        }

        /* Handle a binary string concatenation. */
        JS_ASSERT(pn->isArity(PN_BINARY));
        if (pn1->isKind(PNK_STRING) || pn2->isKind(PNK_STRING)) {
            if (!FoldType(cx, !pn1->isKind(PNK_STRING) ? pn1 : pn2, PNK_STRING))
                return false;
            if (!pn1->isKind(PNK_STRING) || !pn2->isKind(PNK_STRING))
                return true;
            RootedString left(cx, pn1->pn_atom);
            RootedString right(cx, pn2->pn_atom);
            RootedString str(cx, js_ConcatStrings(cx, left, right));
            if (!str)
                return false;
            pn->pn_atom = AtomizeString(cx, str);
            if (!pn->pn_atom)
                return false;
            pn->setKind(PNK_STRING);
            pn->setOp(JSOP_STRING);
            pn->setArity(PN_NULLARY);
            parser->freeTree(pn1);
            parser->freeTree(pn2);
            break;
        }

        /* Can't concatenate string literals, let's try numbers. */
        goto do_binary_op;

      case PNK_SUB:
      case PNK_STAR:
      case PNK_LSH:
      case PNK_RSH:
      case PNK_URSH:
      case PNK_DIV:
      case PNK_MOD:
      do_binary_op:
        if (pn->isArity(PN_LIST)) {
            JS_ASSERT(pn->pn_count > 2);
            for (pn2 = pn1; pn2; pn2 = pn2->pn_next) {
                if (!FoldType(cx, pn2, PNK_NUMBER))
                    return false;
            }
            for (pn2 = pn1; pn2; pn2 = pn2->pn_next) {
                /* XXX fold only if all operands convert to number */
                if (!pn2->isKind(PNK_NUMBER))
                    break;
            }
            if (!pn2) {
                JSOp op = pn->getOp();

                pn2 = pn1->pn_next;
                pn3 = pn2->pn_next;
                if (!FoldBinaryNumeric(cx, op, pn1, pn2, pn, parser))
                    return false;
                while ((pn2 = pn3) != NULL) {
                    pn3 = pn2->pn_next;
                    if (!FoldBinaryNumeric(cx, op, pn, pn2, pn, parser))
                        return false;
                }
            }
        } else {
            JS_ASSERT(pn->isArity(PN_BINARY));
            if (!FoldType(cx, pn1, PNK_NUMBER) ||
                !FoldType(cx, pn2, PNK_NUMBER)) {
                return false;
            }
            if (pn1->isKind(PNK_NUMBER) && pn2->isKind(PNK_NUMBER)) {
                if (!FoldBinaryNumeric(cx, pn->getOp(), pn1, pn2, pn, parser))
                    return false;
            }
        }
        break;

      case PNK_TYPEOF:
      case PNK_VOID:
      case PNK_NOT:
      case PNK_BITNOT:
      case PNK_POS:
      case PNK_NEG:
        if (pn1->isKind(PNK_NUMBER)) {
            double d;

            /* Operate on one numeric constant. */
            d = pn1->pn_dval;
            switch (pn->getOp()) {
              case JSOP_BITNOT:
                d = ~ToInt32(d);
                break;

              case JSOP_NEG:
                d = -d;
                break;

              case JSOP_POS:
                break;

              case JSOP_NOT:
                if (d == 0 || MOZ_DOUBLE_IS_NaN(d)) {
                    pn->setKind(PNK_TRUE);
                    pn->setOp(JSOP_TRUE);
                } else {
                    pn->setKind(PNK_FALSE);
                    pn->setOp(JSOP_FALSE);
                }
                pn->setArity(PN_NULLARY);
                /* FALL THROUGH */

              default:
                /* Return early to dodge the common PNK_NUMBER code. */
                return true;
            }
            pn->setKind(PNK_NUMBER);
            pn->setOp(JSOP_DOUBLE);
            pn->setArity(PN_NULLARY);
            pn->pn_dval = d;
            parser->freeTree(pn1);
        } else if (pn1->isKind(PNK_TRUE) || pn1->isKind(PNK_FALSE)) {
            if (pn->isOp(JSOP_NOT)) {
                pn->become(pn1);
                if (pn->isKind(PNK_TRUE)) {
                    pn->setKind(PNK_FALSE);
                    pn->setOp(JSOP_FALSE);
                } else {
                    pn->setKind(PNK_TRUE);
                    pn->setOp(JSOP_TRUE);
                }
                parser->freeTree(pn1);
            }
        }
        break;

#if JS_HAS_XML_SUPPORT
      case PNK_XMLELEM:
      case PNK_XMLLIST:
      case PNK_XMLPTAGC:
      case PNK_XMLSTAGO:
      case PNK_XMLETAGO:
      case PNK_XMLNAME:
        if (pn->isArity(PN_LIST)) {
            JS_ASSERT(pn->isKind(PNK_XMLLIST) || pn->pn_count != 0);
            if (!FoldXMLConstants(cx, pn, parser))
                return false;
        }
        break;

      case PNK_AT:
        if (pn1->isKind(PNK_XMLNAME)) {
            Value v = StringValue(pn1->pn_atom);
            if (!js_ToAttributeName(cx, &v))
                return false;
            JS_ASSERT(v.isObject());

            ObjectBox *xmlbox = parser->newObjectBox(&v.toObject());
            if (!xmlbox)
                return false;

            pn->setKind(PNK_XMLNAME);
            pn->setOp(JSOP_OBJECT);
            pn->setArity(PN_NULLARY);
            pn->pn_objbox = xmlbox;
            parser->freeTree(pn1);
        }
        break;
#endif /* JS_HAS_XML_SUPPORT */

      default:;
    }

    if (inCond) {
        Truthiness t = Boolish(pn);
        if (t != Unknown) {
            /*
             * We can turn function nodes into constant nodes here, but mutating function
             * nodes is tricky --- in particular, mutating a function node that appears on
             * a method list corrupts the method list. However, methods are M's in
             * statements of the form 'this.foo = M;', which we never fold, so we're okay.
             */
            parser->allocator.prepareNodeForMutation(pn);
            if (t == Truthy) {
                pn->setKind(PNK_TRUE);
                pn->setOp(JSOP_TRUE);
            } else {
                pn->setKind(PNK_FALSE);
                pn->setOp(JSOP_FALSE);
            }
            pn->setArity(PN_NULLARY);
        }
    }

    return true;
}
