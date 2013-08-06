/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/Module.h"
#include "frontend/ParseNode.h"
#include "frontend/Parser.h"

#include "jsscriptinlines.h"

#include "frontend/ParseMaps-inl.h"
#include "frontend/ParseNode-inl.h"
#include "frontend/Parser-inl.h"

using namespace js;
using namespace js::frontend;

using mozilla::IsFinite;

/*
 * Asserts to verify assumptions behind pn_ macros.
 */
#define pn_offsetof(m)  offsetof(ParseNode, m)

JS_STATIC_ASSERT(pn_offsetof(pn_link) == pn_offsetof(dn_uses));

#undef pn_offsetof

#ifdef DEBUG
void
ParseNode::checkListConsistency()
{
    JS_ASSERT(isArity(PN_LIST));
    ParseNode **tail;
    uint32_t count = 0;
    if (pn_head) {
        ParseNode *pn, *last;
        for (pn = last = pn_head; pn; last = pn, pn = pn->pn_next, count++)
            ;
        tail = &last->pn_next;
    } else {
        tail = &pn_head;
    }
    JS_ASSERT(pn_tail == tail);
    JS_ASSERT(pn_count == count);
}
#endif

/* Add |node| to |parser|'s free node list. */
void
ParseNodeAllocator::freeNode(ParseNode *pn)
{
    /* Catch back-to-back dup recycles. */
    JS_ASSERT(pn != freelist);

    /*
     * It's too hard to clear these nodes from the AtomDefnMaps, etc. that
     * hold references to them, so we never free them. It's our caller's job to
     * recognize and process these, since their children do need to be dealt
     * with.
     */
    JS_ASSERT(!pn->isUsed());
    JS_ASSERT(!pn->isDefn());

#ifdef DEBUG
    /* Poison the node, to catch attempts to use it without initializing it. */
    memset(pn, 0xab, sizeof(*pn));
#endif

    pn->pn_next = freelist;
    freelist = pn;
}

/*
 * A work pool of ParseNodes. The work pool is a stack, chained together
 * by nodes' pn_next fields. We use this to avoid creating deep C++ stacks
 * when recycling deep parse trees.
 *
 * Since parse nodes are probably allocated in something close to the order
 * they appear in a depth-first traversal of the tree, making the work pool
 * a stack should give us pretty good locality.
 */
class NodeStack {
  public:
    NodeStack() : top(NULL) { }
    bool empty() { return top == NULL; }
    void push(ParseNode *pn) {
        pn->pn_next = top;
        top = pn;
    }
    void pushUnlessNull(ParseNode *pn) { if (pn) push(pn); }
    /* Push the children of the PN_LIST node |pn| on the stack. */
    void pushList(ParseNode *pn) {
        /* This clobbers pn->pn_head if the list is empty; should be okay. */
        *pn->pn_tail = top;
        top = pn->pn_head;
    }
    ParseNode *pop() {
        JS_ASSERT(!empty());
        ParseNode *hold = top; /* my kingdom for a prog1 */
        top = top->pn_next;
        return hold;
    }
  private:
    ParseNode *top;
};

/*
 * Push the children of |pn| on |stack|. Return true if |pn| itself could be
 * safely recycled, or false if it must be cleaned later (pn_used and pn_defn
 * nodes, and all function nodes; see comments for CleanFunctionList in
 * SemanticAnalysis.cpp). Some callers want to free |pn|; others
 * (js::ParseNodeAllocator::prepareNodeForMutation) don't care about |pn|, and
 * just need to take care of its children.
 */
static bool
PushNodeChildren(ParseNode *pn, NodeStack *stack)
{
    switch (pn->getArity()) {
      case PN_CODE:
        /*
         * Function nodes are linked into the function box tree, and may appear
         * on method lists. Both of those lists are singly-linked, so trying to
         * update them now could result in quadratic behavior when recycling
         * trees containing many functions; and the lists can be very long. So
         * we put off cleaning the lists up until just before function
         * analysis, when we call CleanFunctionList.
         *
         * In fact, we can't recycle the parse node yet, either: it may appear
         * on a method list, and reusing the node would corrupt that. Instead,
         * we clear its pn_funbox pointer to mark it as deleted;
         * CleanFunctionList recycles it as well.
         *
         * We do recycle the nodes around it, though, so we must clear pointers
         * to them to avoid leaving dangling references where someone can find
         * them.
         */
        pn->pn_funbox = NULL;
        stack->pushUnlessNull(pn->pn_body);
        pn->pn_body = NULL;
        return false;

      case PN_NAME:
        /*
         * Because used/defn nodes appear in AtomDefnMaps and elsewhere, we
         * don't recycle them. (We'll recover their storage when we free the
         * temporary arena.) However, we do recycle the nodes around them, so
         * clean up the pointers to avoid dangling references. The top-level
         * decls table carries references to them that later iterations through
         * the compileScript loop may find, so they need to be neat.
         *
         * pn_expr and pn_lexdef share storage; the latter isn't an owning
         * reference.
         */
        if (!pn->isUsed()) {
            stack->pushUnlessNull(pn->pn_expr);
            pn->pn_expr = NULL;
        }
        return !pn->isUsed() && !pn->isDefn();

      case PN_LIST:
        pn->checkListConsistency();
        stack->pushList(pn);
        break;
      case PN_TERNARY:
        stack->pushUnlessNull(pn->pn_kid1);
        stack->pushUnlessNull(pn->pn_kid2);
        stack->pushUnlessNull(pn->pn_kid3);
        break;
      case PN_BINARY:
        if (pn->pn_left != pn->pn_right)
            stack->pushUnlessNull(pn->pn_left);
        stack->pushUnlessNull(pn->pn_right);
        break;
      case PN_UNARY:
        stack->pushUnlessNull(pn->pn_kid);
        break;
      case PN_NULLARY:
        return !pn->isUsed() && !pn->isDefn();
      default:
        ;
    }

    return true;
}

/*
 * Prepare |pn| to be mutated in place into a new kind of node. Recycle all
 * |pn|'s recyclable children (but not |pn| itself!), and disconnect it from
 * metadata structures (the function box tree).
 */
void
ParseNodeAllocator::prepareNodeForMutation(ParseNode *pn)
{
    if (!pn->isArity(PN_NULLARY)) {
        /* Put |pn|'s children (but not |pn| itself) on a work stack. */
        NodeStack stack;
        PushNodeChildren(pn, &stack);
        /*
         * For each node on the work stack, push its children on the work stack,
         * and free the node if we can.
         */
        while (!stack.empty()) {
            pn = stack.pop();
            if (PushNodeChildren(pn, &stack))
                freeNode(pn);
        }
    }
}

/*
 * Return the nodes in the subtree |pn| to the parser's free node list, for
 * reallocation.
 */
ParseNode *
ParseNodeAllocator::freeTree(ParseNode *pn)
{
    if (!pn)
        return NULL;

    ParseNode *savedNext = pn->pn_next;

    NodeStack stack;
    for (;;) {
        if (PushNodeChildren(pn, &stack))
            freeNode(pn);
        if (stack.empty())
            break;
        pn = stack.pop();
    }

    return savedNext;
}

/*
 * Allocate a ParseNode from parser's node freelist or, failing that, from
 * cx's temporary arena.
 */
void *
ParseNodeAllocator::allocNode()
{
    if (ParseNode *pn = freelist) {
        freelist = pn->pn_next;
        return pn;
    }

    void *p = cx->tempLifoAlloc().alloc(sizeof (ParseNode));
    if (!p)
        js_ReportOutOfMemory(cx);
    return p;
}

/* used only by static create methods of subclasses */

ParseNode *
ParseNode::create(ParseNodeKind kind, ParseNodeArity arity, FullParseHandler *handler)
{
    const Token &tok = handler->currentToken();
    return handler->new_<ParseNode>(kind, JSOP_NOP, arity, tok.pos);
}

ParseNode *
ParseNode::append(ParseNodeKind kind, JSOp op, ParseNode *left, ParseNode *right,
                  FullParseHandler *handler)
{
    if (!left || !right)
        return NULL;

    JS_ASSERT(left->isKind(kind) && left->isOp(op) && (js_CodeSpec[op].format & JOF_LEFTASSOC));

    ListNode *list;
    if (left->pn_arity == PN_LIST) {
        list = &left->as<ListNode>();
    } else {
        ParseNode *pn1 = left->pn_left, *pn2 = left->pn_right;
        list = handler->new_<ListNode>(kind, op, pn1);
        if (!list)
            return NULL;
        list->append(pn2);
        if (kind == PNK_ADD) {
            if (pn1->isKind(PNK_STRING))
                list->pn_xflags |= PNX_STRCAT;
            else if (!pn1->isKind(PNK_NUMBER))
                list->pn_xflags |= PNX_CANTFOLD;
            if (pn2->isKind(PNK_STRING))
                list->pn_xflags |= PNX_STRCAT;
            else if (!pn2->isKind(PNK_NUMBER))
                list->pn_xflags |= PNX_CANTFOLD;
        }
    }

    list->append(right);
    list->pn_pos.end = right->pn_pos.end;
    if (kind == PNK_ADD) {
        if (right->isKind(PNK_STRING))
            list->pn_xflags |= PNX_STRCAT;
        else if (!right->isKind(PNK_NUMBER))
            list->pn_xflags |= PNX_CANTFOLD;
    }

    return list;
}

ParseNode *
ParseNode::newBinaryOrAppend(ParseNodeKind kind, JSOp op, ParseNode *left, ParseNode *right,
                             FullParseHandler *handler, ParseContext<FullParseHandler> *pc,
                             bool foldConstants)
{
    if (!left || !right)
        return NULL;

    /*
     * Ensure that the parse tree is faithful to the source when "use asm" (for
     * the purpose of type checking).
     */
    if (pc->useAsmOrInsideUseAsm())
        return handler->new_<BinaryNode>(kind, op, left, right);

    /*
     * Flatten a left-associative (left-heavy) tree of a given operator into
     * a list to reduce js::FoldConstants and js::frontend::EmitTree recursion.
     */
    if (left->isKind(kind) && left->isOp(op) && (js_CodeSpec[op].format & JOF_LEFTASSOC))
        return append(kind, op, left, right, handler);

    /*
     * Fold constant addition immediately, to conserve node space and, what's
     * more, so js::FoldConstants never sees mixed addition and concatenation
     * operations with more than one leading non-string operand in a PN_LIST
     * generated for expressions such as 1 + 2 + "pt" (which should evaluate
     * to "3pt", not "12pt").
     */
    if (kind == PNK_ADD &&
        left->isKind(PNK_NUMBER) &&
        right->isKind(PNK_NUMBER) &&
        foldConstants)
    {
        left->pn_dval += right->pn_dval;
        left->pn_pos.end = right->pn_pos.end;
        handler->freeTree(right);
        return left;
    }

    return handler->new_<BinaryNode>(kind, op, left, right);
}

// Note: the parse context passed into this may not equal the associated
// parser's current context.
NameNode *
NameNode::create(ParseNodeKind kind, JSAtom *atom, FullParseHandler *handler,
                 ParseContext<FullParseHandler> *pc)
{
    ParseNode *pn = ParseNode::create(kind, PN_NAME, handler);
    if (pn) {
        pn->pn_atom = atom;
        ((NameNode *)pn)->initCommon(pc);
    }
    return (NameNode *)pn;
}

const char *
Definition::kindString(Kind kind)
{
    static const char *table[] = {
        "", js_var_str, js_const_str, js_let_str, js_function_str, "argument", "unknown"
    };

    JS_ASSERT(unsigned(kind) <= unsigned(ARG));
    return table[kind];
}

namespace js {
namespace frontend {

#if JS_HAS_DESTRUCTURING

/*
 * This function assumes the cloned tree is for use in the same statement and
 * binding context as the original tree.
 */
template <>
ParseNode *
Parser<FullParseHandler>::cloneParseTree(ParseNode *opn)
{
    JS_CHECK_RECURSION(context, return NULL);

    ParseNode *pn = handler.new_<ParseNode>(opn->getKind(), opn->getOp(), opn->getArity(),
                                            opn->pn_pos);
    if (!pn)
        return NULL;
    pn->setInParens(opn->isInParens());
    pn->setDefn(opn->isDefn());
    pn->setUsed(opn->isUsed());

    switch (pn->getArity()) {
#define NULLCHECK(e)    JS_BEGIN_MACRO if (!(e)) return NULL; JS_END_MACRO

      case PN_CODE:
        if (pn->getKind() == PNK_MODULE) {
            JS_NOT_REACHED("module nodes cannot be cloned");
            return NULL;
        } else {
            NULLCHECK(pn->pn_funbox =
                      newFunctionBox(opn->pn_funbox->function(), pc, opn->pn_funbox->strict));
            NULLCHECK(pn->pn_body = cloneParseTree(opn->pn_body));
            pn->pn_cookie = opn->pn_cookie;
            pn->pn_dflags = opn->pn_dflags;
            pn->pn_blockid = opn->pn_blockid;
        }
        break;

      case PN_LIST:
        pn->makeEmpty();
        for (ParseNode *opn2 = opn->pn_head; opn2; opn2 = opn2->pn_next) {
            ParseNode *pn2;
            NULLCHECK(pn2 = cloneParseTree(opn2));
            pn->append(pn2);
        }
        pn->pn_xflags = opn->pn_xflags;
        break;

      case PN_TERNARY:
        NULLCHECK(pn->pn_kid1 = cloneParseTree(opn->pn_kid1));
        NULLCHECK(pn->pn_kid2 = cloneParseTree(opn->pn_kid2));
        NULLCHECK(pn->pn_kid3 = cloneParseTree(opn->pn_kid3));
        break;

      case PN_BINARY:
        NULLCHECK(pn->pn_left = cloneParseTree(opn->pn_left));
        if (opn->pn_right != opn->pn_left)
            NULLCHECK(pn->pn_right = cloneParseTree(opn->pn_right));
        else
            pn->pn_right = pn->pn_left;
        pn->pn_iflags = opn->pn_iflags;
        break;

      case PN_UNARY:
        NULLCHECK(pn->pn_kid = cloneParseTree(opn->pn_kid));
        pn->pn_hidden = opn->pn_hidden;
        break;

      case PN_NAME:
        // PN_NAME could mean several arms in pn_u, so copy the whole thing.
        pn->pn_u = opn->pn_u;
        if (opn->isUsed()) {
            /*
             * The old name is a use of its pn_lexdef. Make the clone also be a
             * use of that definition.
             */
            Definition *dn = pn->pn_lexdef;

            pn->pn_link = dn->dn_uses;
            dn->dn_uses = pn;
        } else if (opn->pn_expr) {
            NULLCHECK(pn->pn_expr = cloneParseTree(opn->pn_expr));

            /*
             * If the old name is a definition, the new one has pn_defn set.
             * Make the old name a use of the new node.
             */
            if (opn->isDefn()) {
                opn->setDefn(false);
                handler.linkUseToDef(opn, (Definition *) pn);
            }
        }
        break;

      case PN_NULLARY:
        pn->pn_u = opn->pn_u;
        break;

#undef NULLCHECK
    }
    return pn;
}

#endif /* JS_HAS_DESTRUCTURING */

/*
 * Used by Parser::forStatement and comprehensionTail to clone the TARGET in
 *   for (var/const/let TARGET in EXPR)
 *
 * opn must be the pn_head of a node produced by Parser::variables, so its form
 * is known to be LHS = NAME | [LHS] | {id:LHS}.
 *
 * The cloned tree is for use only in the same statement and binding context as
 * the original tree.
 */
template <>
ParseNode *
Parser<FullParseHandler>::cloneLeftHandSide(ParseNode *opn)
{
    ParseNode *pn = handler.new_<ParseNode>(opn->getKind(), opn->getOp(), opn->getArity(),
                                            opn->pn_pos);
    if (!pn)
        return NULL;
    pn->setInParens(opn->isInParens());
    pn->setDefn(opn->isDefn());
    pn->setUsed(opn->isUsed());

#if JS_HAS_DESTRUCTURING
    if (opn->isArity(PN_LIST)) {
        JS_ASSERT(opn->isKind(PNK_ARRAY) || opn->isKind(PNK_OBJECT));
        pn->makeEmpty();
        for (ParseNode *opn2 = opn->pn_head; opn2; opn2 = opn2->pn_next) {
            ParseNode *pn2;
            if (opn->isKind(PNK_OBJECT)) {
                JS_ASSERT(opn2->isArity(PN_BINARY));
                JS_ASSERT(opn2->isKind(PNK_COLON));

                ParseNode *tag = cloneParseTree(opn2->pn_left);
                if (!tag)
                    return NULL;
                ParseNode *target = cloneLeftHandSide(opn2->pn_right);
                if (!target)
                    return NULL;

                pn2 = handler.new_<BinaryNode>(PNK_COLON, JSOP_INITPROP, opn2->pn_pos, tag, target);
            } else if (opn2->isArity(PN_NULLARY)) {
                JS_ASSERT(opn2->isKind(PNK_COMMA));
                pn2 = cloneParseTree(opn2);
            } else {
                pn2 = cloneLeftHandSide(opn2);
            }

            if (!pn2)
                return NULL;
            pn->append(pn2);
        }
        pn->pn_xflags = opn->pn_xflags;
        return pn;
    }
#endif

    JS_ASSERT(opn->isArity(PN_NAME));
    JS_ASSERT(opn->isKind(PNK_NAME));

    /* If opn is a definition or use, make pn a use. */
    pn->pn_u.name = opn->pn_u.name;
    pn->setOp(JSOP_SETNAME);
    if (opn->isUsed()) {
        Definition *dn = pn->pn_lexdef;

        pn->pn_link = dn->dn_uses;
        dn->dn_uses = pn;
    } else {
        pn->pn_expr = NULL;
        if (opn->isDefn()) {
            /* We copied some definition-specific state into pn. Clear it out. */
            pn->pn_cookie.makeFree();
            pn->pn_dflags &= ~PND_BOUND;
            pn->setDefn(false);

            handler.linkUseToDef(pn, (Definition *) opn);
        }
    }
    return pn;
}

} /* namespace frontend */
} /* namespace js */

#ifdef DEBUG

static const char *parseNodeNames[] = {
#define STRINGIFY(name) #name,
    FOR_EACH_PARSE_NODE_KIND(STRINGIFY)
#undef STRINGIFY
};

void
frontend::DumpParseTree(ParseNode *pn, int indent)
{
    if (pn == NULL)
        fprintf(stderr, "#NULL");
    else
        pn->dump(indent);
}

static void
IndentNewLine(int indent)
{
    fputc('\n', stderr);
    for (int i = 0; i < indent; ++i)
        fputc(' ', stderr);
}

void
ParseNode::dump()
{
    dump(0);
    fputc('\n', stderr);
}

void
ParseNode::dump(int indent)
{
    switch (pn_arity) {
      case PN_NULLARY:
        ((NullaryNode *) this)->dump();
        break;
      case PN_UNARY:
        ((UnaryNode *) this)->dump(indent);
        break;
      case PN_BINARY:
        ((BinaryNode *) this)->dump(indent);
        break;
      case PN_TERNARY:
        ((TernaryNode *) this)->dump(indent);
        break;
      case PN_CODE:
        ((CodeNode *) this)->dump(indent);
        break;
      case PN_LIST:
        ((ListNode *) this)->dump(indent);
        break;
      case PN_NAME:
        ((NameNode *) this)->dump(indent);
        break;
      default:
        fprintf(stderr, "#<BAD NODE %p, kind=%u, arity=%u>",
                (void *) this, unsigned(getKind()), unsigned(pn_arity));
        break;
    }
}

void
NullaryNode::dump()
{
    switch (getKind()) {
      case PNK_TRUE:  fprintf(stderr, "#true");  break;
      case PNK_FALSE: fprintf(stderr, "#false"); break;
      case PNK_NULL:  fprintf(stderr, "#null");  break;

      case PNK_NUMBER: {
        ToCStringBuf cbuf;
        const char *cstr = NumberToCString(NULL, &cbuf, pn_dval);
        if (!IsFinite(pn_dval))
            fputc('#', stderr);
        if (cstr)
            fprintf(stderr, "%s", cstr);
        else
            fprintf(stderr, "%g", pn_dval);
        break;
      }

      case PNK_STRING:
        JSString::dumpChars(pn_atom->chars(), pn_atom->length());
        break;

      default:
        fprintf(stderr, "(%s)", parseNodeNames[getKind()]);
    }
}

void
UnaryNode::dump(int indent)
{
    const char *name = parseNodeNames[getKind()];
    fprintf(stderr, "(%s ", name);
    indent += strlen(name) + 2;
    DumpParseTree(pn_kid, indent);
    fprintf(stderr, ")");
}

void
BinaryNode::dump(int indent)
{
    const char *name = parseNodeNames[getKind()];
    fprintf(stderr, "(%s ", name);
    indent += strlen(name) + 2;
    DumpParseTree(pn_left, indent);
    IndentNewLine(indent);
    DumpParseTree(pn_right, indent);
    fprintf(stderr, ")");
}

void
TernaryNode::dump(int indent)
{
    const char *name = parseNodeNames[getKind()];
    fprintf(stderr, "(%s ", name);
    indent += strlen(name) + 2;
    DumpParseTree(pn_kid1, indent);
    IndentNewLine(indent);
    DumpParseTree(pn_kid2, indent);
    IndentNewLine(indent);
    DumpParseTree(pn_kid3, indent);
    fprintf(stderr, ")");
}

void
CodeNode::dump(int indent)
{
    const char *name = parseNodeNames[getKind()];
    fprintf(stderr, "(%s ", name);
    indent += strlen(name) + 2;
    DumpParseTree(pn_body, indent);
    fprintf(stderr, ")");
}

void
ListNode::dump(int indent)
{
    const char *name = parseNodeNames[getKind()];
    fprintf(stderr, "(%s [", name);
    if (pn_head != NULL) {
        indent += strlen(name) + 3;
        DumpParseTree(pn_head, indent);
        ParseNode *pn = pn_head->pn_next;
        while (pn != NULL) {
            IndentNewLine(indent);
            DumpParseTree(pn, indent);
            pn = pn->pn_next;
        }
    }
    fprintf(stderr, "])");
}

void
NameNode::dump(int indent)
{
    if (isKind(PNK_NAME) || isKind(PNK_DOT)) {
        if (isKind(PNK_DOT))
            fprintf(stderr, "(.");

        if (!pn_atom) {
            fprintf(stderr, "#<null name>");
        } else {
            const jschar *s = pn_atom->chars();
            size_t len = pn_atom->length();
            if (len == 0)
                fprintf(stderr, "#<zero-length name>");
            for (size_t i = 0; i < len; i++) {
                if (s[i] > 32 && s[i] < 127)
                    fputc(s[i], stderr);
                else if (s[i] <= 255)
                    fprintf(stderr, "\\x%02x", (unsigned int) s[i]);
                else
                    fprintf(stderr, "\\u%04x", (unsigned int) s[i]);
            }
        }

        if (isKind(PNK_DOT)) {
            fputc(' ', stderr);
            DumpParseTree(expr(), indent + 2);
            fputc(')', stderr);
        }
        return;
    }

    JS_ASSERT(!isUsed());
    const char *name = parseNodeNames[getKind()];
    if (isUsed())
        fprintf(stderr, "(%s)", name);
    else {
        fprintf(stderr, "(%s ", name);
        indent += strlen(name) + 2;
        DumpParseTree(expr(), indent);
        fprintf(stderr, ")");
    }
}
#endif

ObjectBox::ObjectBox(JSObject *object, ObjectBox* traceLink)
  : object(object),
    traceLink(traceLink),
    emitLink(NULL)
{
    JS_ASSERT(!object->isFunction());
}

ObjectBox::ObjectBox(JSFunction *function, ObjectBox* traceLink)
  : object(function),
    traceLink(traceLink),
    emitLink(NULL)
{
    JS_ASSERT(object->isFunction());
    JS_ASSERT(asFunctionBox()->function() == function);
}

ModuleBox *
ObjectBox::asModuleBox()
{
    JS_ASSERT(isModuleBox());
    return static_cast<ModuleBox *>(this);
}

FunctionBox *
ObjectBox::asFunctionBox()
{
    JS_ASSERT(isFunctionBox());
    return static_cast<FunctionBox *>(this);
}

ObjectBox::ObjectBox(Module *module, ObjectBox* traceLink)
  : object(module),
    traceLink(traceLink),
    emitLink(NULL)
{
    JS_ASSERT(object->isModule());
}

void
ObjectBox::trace(JSTracer *trc)
{
    ObjectBox *box = this;
    while (box) {
        MarkObjectRoot(trc, &box->object, "parser.object");
        if (box->isModuleBox())
            box->asModuleBox()->bindings.trace(trc);
        if (box->isFunctionBox())
            box->asFunctionBox()->bindings.trace(trc);
        box = box->traceLink;
    }
}
