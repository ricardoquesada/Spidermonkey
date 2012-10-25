/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=79:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BytecodeEmitter_h__
#define BytecodeEmitter_h__

/*
 * JS bytecode generation.
 */
#include "jstypes.h"
#include "jsatom.h"
#include "jsopcode.h"
#include "jsscript.h"
#include "jsprvtd.h"
#include "jspubtd.h"

#include "frontend/Parser.h"
#include "frontend/ParseMaps.h"
#include "frontend/SharedContext.h"

#include "vm/ScopeObject.h"

namespace js {
namespace frontend {

struct TryNode {
    JSTryNote       note;
    TryNode       *prev;
};

struct CGObjectList {
    uint32_t            length;     /* number of emitted so far objects */
    ObjectBox           *lastbox;   /* last emitted object */

    CGObjectList() : length(0), lastbox(NULL) {}

    unsigned add(ObjectBox *objbox);
    unsigned indexOf(JSObject *obj);
    void finish(ObjectArray *array);
};

class GCConstList {
    Vector<Value> list;
  public:
    GCConstList(JSContext *cx) : list(cx) {}
    bool append(Value v) { JS_ASSERT_IF(v.isString(), v.toString()->isAtom()); return list.append(v); }
    size_t length() const { return list.length(); }
    void finish(ConstArray *array);
};

struct StmtInfoBCE;

struct BytecodeEmitter
{
    typedef StmtInfoBCE StmtInfo;

    SharedContext   *const sc;      /* context shared between parsing and bytecode generation */

    BytecodeEmitter *const parent;  /* enclosing function or global context */

    Rooted<JSScript*> script;       /* the JSScript we're ultimately producing */

    struct {
        jsbytecode  *base;          /* base of JS bytecode vector */
        jsbytecode  *limit;         /* one byte beyond end of bytecode */
        jsbytecode  *next;          /* pointer to next free bytecode */
        jssrcnote   *notes;         /* source notes, see below */
        unsigned    noteCount;      /* number of source notes so far */
        unsigned    noteLimit;      /* limit number for source notes in notePool */
        ptrdiff_t   lastNoteOffset; /* code offset for last source note */
        unsigned    currentLine;    /* line number for tree-based srcnote gen */
        unsigned    lastColumn;     /* zero-based column index on currentLine of
                                       last SRC_COLSPAN-annotated opcode */
    } prolog, main, *current;

    Parser          *const parser;  /* the parser */

    StackFrame      *const callerFrame; /* scripted caller frame for eval and dbgapi */

    StmtInfoBCE     *topStmt;       /* top of statement info stack */
    StmtInfoBCE     *topScopeStmt;  /* top lexical scope statement */
    Rooted<StaticBlockObject *> blockChain;
                                    /* compile time block scope chain */

    OwnedAtomIndexMapPtr atomIndices; /* literals indexed for mapping */
    unsigned        firstLine;      /* first line, for JSScript::initFromEmitter */

    int             stackDepth;     /* current stack depth in script frame */
    unsigned        maxStackDepth;  /* maximum stack depth so far */

    unsigned        ntrynotes;      /* number of allocated so far try notes */
    TryNode         *lastTryNode;   /* the last allocated try node */

    unsigned        arrayCompDepth; /* stack depth of array in comprehension */

    unsigned        emitLevel;      /* js::frontend::EmitTree recursion level */

    typedef HashMap<JSAtom *, Value> ConstMap;
    ConstMap        constMap;       /* compile time constants */

    GCConstList     constList;      /* constants to be included with the script */

    CGObjectList    objectList;     /* list of emitted objects */
    CGObjectList    regexpList;     /* list of emitted regexp that will be
                                       cloned during execution */

    uint16_t        typesetCount;   /* Number of JOF_TYPESET opcodes generated */

    bool            hasSingletons:1;    /* script contains singleton initializer JSOP_OBJECT */

    bool            emittingForInit:1;  /* true while emitting init expr of for; exclude 'in' */

    const bool      hasGlobalScope:1;   /* frontend::CompileScript's scope chain is the
                                           global object */

    const bool      selfHostingMode:1;  /* Emit JSOP_CALLINTRINSIC instead of JSOP_NAME
                                           and assert that JSOP_NAME and JSOP_*GNAME
                                           don't ever get emitted. See the comment for
                                           the field |selfHostingMode| in Parser.h for details. */

    BytecodeEmitter(BytecodeEmitter *parent, Parser *parser, SharedContext *sc,
                    HandleScript script, StackFrame *callerFrame, bool hasGlobalScope,
                    unsigned lineno, bool selfHostingMode = false);
    bool init();

    /*
     * Note that BytecodeEmitters are magic: they own the arena "top-of-stack"
     * space above their tempMark points. This means that you cannot alloc from
     * tempLifoAlloc and save the pointer beyond the next BytecodeEmitter
     * destructor call.
     */
    ~BytecodeEmitter();

    bool isAliasedName(ParseNode *pn);

    JS_ALWAYS_INLINE
    bool makeAtomIndex(JSAtom *atom, jsatomid *indexp) {
        AtomIndexAddPtr p = atomIndices->lookupForAdd(atom);
        if (p) {
            *indexp = p.value();
            return true;
        }

        jsatomid index = atomIndices->count();
        if (!atomIndices->add(p, atom, index))
            return false;

        *indexp = index;
        return true;
    }

    bool checkSingletonContext();

    bool needsImplicitThis();

    void tellDebuggerAboutCompiledScript(JSContext *cx);

    TokenStream *tokenStream() { return &parser->tokenStream; }

    jsbytecode *base() const { return current->base; }
    jsbytecode *limit() const { return current->limit; }
    jsbytecode *next() const { return current->next; }
    jsbytecode *code(ptrdiff_t offset) const { return base() + offset; }
    ptrdiff_t offset() const { return next() - base(); }
    jsbytecode *prologBase() const { return prolog.base; }
    ptrdiff_t prologOffset() const { return prolog.next - prolog.base; }
    void switchToMain() { current = &main; }
    void switchToProlog() { current = &prolog; }

    jssrcnote *notes() const { return current->notes; }
    unsigned noteCount() const { return current->noteCount; }
    unsigned noteLimit() const { return current->noteLimit; }
    ptrdiff_t lastNoteOffset() const { return current->lastNoteOffset; }
    unsigned currentLine() const { return current->currentLine; }
    unsigned lastColumn() const { return current->lastColumn; }

    inline ptrdiff_t countFinalSourceNotes();

    bool reportError(ParseNode *pn, unsigned errorNumber, ...);
    bool reportStrictWarning(ParseNode *pn, unsigned errorNumber, ...);
    bool reportStrictModeError(ParseNode *pn, unsigned errorNumber, ...);
};

/*
 * Emit one bytecode.
 */
ptrdiff_t
Emit1(JSContext *cx, BytecodeEmitter *bce, JSOp op);

/*
 * Emit two bytecodes, an opcode (op) with a byte of immediate operand (op1).
 */
ptrdiff_t
Emit2(JSContext *cx, BytecodeEmitter *bce, JSOp op, jsbytecode op1);

/*
 * Emit three bytecodes, an opcode with two bytes of immediate operands.
 */
ptrdiff_t
Emit3(JSContext *cx, BytecodeEmitter *bce, JSOp op, jsbytecode op1, jsbytecode op2);

/*
 * Emit (1 + extra) bytecodes, for N bytes of op and its immediate operand.
 */
ptrdiff_t
EmitN(JSContext *cx, BytecodeEmitter *bce, JSOp op, size_t extra);

/*
 * Define and lookup a primitive jsval associated with the const named by atom.
 * DefineCompileTimeConstant analyzes the constant-folded initializer at pn
 * and saves the const's value in bce->constList, if it can be used at compile
 * time. It returns true unless an error occurred.
 *
 * If the initializer's value could not be saved, DefineCompileTimeConstant
 * calls will return the undefined value. DefineCompileTimeConstant tries
 * to find a const value memorized for atom, returning true with *vp set to a
 * value other than undefined if the constant was found, true with *vp set to
 * JSVAL_VOID if not found, and false on error.
 */
bool
DefineCompileTimeConstant(JSContext *cx, BytecodeEmitter *bce, JSAtom *atom, ParseNode *pn);

/*
 * Emit code into bce for the tree rooted at pn.
 */
bool
EmitTree(JSContext *cx, BytecodeEmitter *bce, ParseNode *pn);

/*
 * Emit function code using bce for the tree rooted at body.
 */
bool
EmitFunctionScript(JSContext *cx, BytecodeEmitter *bce, ParseNode *body);

} /* namespace frontend */

/*
 * Source notes generated along with bytecode for decompiling and debugging.
 * A source note is a uint8_t with 5 bits of type and 3 of offset from the pc
 * of the previous note. If 3 bits of offset aren't enough, extended delta
 * notes (SRC_XDELTA) consisting of 2 set high order bits followed by 6 offset
 * bits are emitted before the next note. Some notes have operand offsets
 * encoded immediately after them, in note bytes or byte-triples.
 *
 *                 Source Note               Extended Delta
 *              +7-6-5-4-3+2-1-0+           +7-6-5+4-3-2-1-0+
 *              |note-type|delta|           |1 1| ext-delta |
 *              +---------+-----+           +---+-----------+
 *
 * At most one "gettable" note (i.e., a note of type other than SRC_NEWLINE,
 * SRC_COLSPAN, SRC_SETLINE, and SRC_XDELTA) applies to a given bytecode.
 *
 * NB: the js_SrcNoteSpec array in BytecodeEmitter.cpp is indexed by this
 * enum, so its initializers need to match the order here.
 *
 * Note on adding new source notes: every pair of bytecodes (A, B) where A and
 * B have disjoint sets of source notes that could apply to each bytecode may
 * reuse the same note type value for two notes (snA, snB) that have the same
 * arity in JSSrcNoteSpec. This is why SRC_IF and SRC_INITPROP have the same
 * value below.
 *
 * Don't forget to update XDR_BYTECODE_VERSION in vm/Xdr.h for all such
 * incompatible source note or other bytecode changes.
 */
enum SrcNoteType {
    SRC_NULL        = 0,        /* terminates a note vector */
    SRC_IF          = 1,        /* JSOP_IFEQ bytecode is from an if-then */
    SRC_BREAK       = 1,        /* JSOP_GOTO is a break */
    SRC_INITPROP    = 1,        /* disjoint meaning applied to JSOP_INITELEM or
                                   to an index label in a regular (structuring)
                                   or a destructuring object initialiser */
    SRC_GENEXP      = 1,        /* JSOP_LAMBDA from generator expression */
    SRC_IF_ELSE     = 2,        /* JSOP_IFEQ bytecode is from an if-then-else */
    SRC_FOR_IN      = 2,        /* JSOP_GOTO to for-in loop condition from
                                   before loop (same arity as SRC_IF_ELSE) */
    SRC_FOR         = 3,        /* JSOP_NOP or JSOP_POP in for(;;) loop head */
    SRC_WHILE       = 4,        /* JSOP_GOTO to for or while loop condition
                                   from before loop, else JSOP_NOP at top of
                                   do-while loop */
    SRC_CONTINUE    = 5,        /* JSOP_GOTO is a continue, not a break;
                                   JSOP_ENDINIT needs extra comma at end of
                                   array literal: [1,2,,];
                                   JSOP_DUP continuing destructuring pattern;
                                   JSOP_POP at end of for-in */
    SRC_DECL        = 6,        /* type of a declaration (var, const, let*) */
    SRC_DESTRUCT    = 6,        /* JSOP_DUP starting a destructuring assignment
                                   operation, with SRC_DECL_* offset operand */
    SRC_PCDELTA     = 7,        /* distance forward from comma-operator to
                                   next POP, or from CONDSWITCH to first CASE
                                   opcode, etc. -- always a forward delta */
    SRC_GROUPASSIGN = 7,        /* SRC_DESTRUCT variant for [a, b] = [c, d] */
    SRC_DESTRUCTLET = 7,        /* JSOP_DUP starting a destructuring let
                                   operation, with offset to JSOP_ENTERLET0 */
    SRC_ASSIGNOP    = 8,        /* += or another assign-op follows */
    SRC_COND        = 9,        /* JSOP_IFEQ is from conditional ?: operator */
    SRC_BRACE       = 10,       /* mandatory brace, for scope or to avoid
                                   dangling else */
    SRC_HIDDEN      = 11,       /* opcode shouldn't be decompiled */
    SRC_PCBASE      = 12,       /* distance back from annotated getprop or
                                   setprop op to left-most obj.prop.subprop
                                   bytecode -- always a backward delta */
    SRC_LABEL       = 13,       /* JSOP_LABEL for label: with atomid immediate */
    SRC_LABELBRACE  = 14,       /* JSOP_LABEL for label: {...} begin brace */
    SRC_ENDBRACE    = 15,       /* JSOP_NOP for label: {...} end brace */
    SRC_BREAK2LABEL = 16,       /* JSOP_GOTO for 'break label' with atomid */
    SRC_CONT2LABEL  = 17,       /* JSOP_GOTO for 'continue label' with atomid */
    SRC_SWITCH      = 18,       /* JSOP_*SWITCH with offset to end of switch,
                                   2nd off to first JSOP_CASE if condswitch */
    SRC_SWITCHBREAK = 18,       /* JSOP_GOTO is a break in a switch */
    SRC_FUNCDEF     = 19,       /* JSOP_NOP for function f() with atomid */
    SRC_CATCH       = 20,       /* catch block has guard */
    SRC_COLSPAN     = 21,       /* number of columns this opcode spans */
    SRC_NEWLINE     = 22,       /* bytecode follows a source newline */
    SRC_SETLINE     = 23,       /* a file-absolute source line number note */
    SRC_XDELTA      = 24        /* 24-31 are for extended delta notes */
};

/*
 * Constants for the SRC_DECL source note.
 *
 * NB: the var_prefix array in jsopcode.c depends on these dense indexes from
 * SRC_DECL_VAR through SRC_DECL_LET.
 */
#define SRC_DECL_VAR            0
#define SRC_DECL_CONST          1
#define SRC_DECL_LET            2
#define SRC_DECL_NONE           3

#define SN_TYPE_BITS            5
#define SN_DELTA_BITS           3
#define SN_XDELTA_BITS          6
#define SN_TYPE_MASK            (JS_BITMASK(SN_TYPE_BITS) << SN_DELTA_BITS)
#define SN_DELTA_MASK           ((ptrdiff_t)JS_BITMASK(SN_DELTA_BITS))
#define SN_XDELTA_MASK          ((ptrdiff_t)JS_BITMASK(SN_XDELTA_BITS))

#define SN_MAKE_NOTE(sn,t,d)    (*(sn) = (jssrcnote)                          \
                                          (((t) << SN_DELTA_BITS)             \
                                           | ((d) & SN_DELTA_MASK)))
#define SN_MAKE_XDELTA(sn,d)    (*(sn) = (jssrcnote)                          \
                                          ((SRC_XDELTA << SN_DELTA_BITS)      \
                                           | ((d) & SN_XDELTA_MASK)))

#define SN_IS_XDELTA(sn)        ((*(sn) >> SN_DELTA_BITS) >= SRC_XDELTA)
#define SN_TYPE(sn)             ((js::SrcNoteType)(SN_IS_XDELTA(sn)           \
                                                   ? SRC_XDELTA               \
                                                   : *(sn) >> SN_DELTA_BITS))
#define SN_SET_TYPE(sn,type)    SN_MAKE_NOTE(sn, type, SN_DELTA(sn))
#define SN_IS_GETTABLE(sn)      (SN_TYPE(sn) < SRC_COLSPAN)

#define SN_DELTA(sn)            ((ptrdiff_t)(SN_IS_XDELTA(sn)                 \
                                             ? *(sn) & SN_XDELTA_MASK         \
                                             : *(sn) & SN_DELTA_MASK))
#define SN_SET_DELTA(sn,delta)  (SN_IS_XDELTA(sn)                             \
                                 ? SN_MAKE_XDELTA(sn, delta)                  \
                                 : SN_MAKE_NOTE(sn, SN_TYPE(sn), delta))

#define SN_DELTA_LIMIT          ((ptrdiff_t)JS_BIT(SN_DELTA_BITS))
#define SN_XDELTA_LIMIT         ((ptrdiff_t)JS_BIT(SN_XDELTA_BITS))

/*
 * Offset fields follow certain notes and are frequency-encoded: an offset in
 * [0,0x7f] consumes one byte, an offset in [0x80,0x7fffff] takes three, and
 * the high bit of the first byte is set.
 */
#define SN_3BYTE_OFFSET_FLAG    0x80
#define SN_3BYTE_OFFSET_MASK    0x7f

/*
 * Negative SRC_COLSPAN offsets are rare, but can arise with for(;;) loops and
 * other constructs that generate code in non-source order. They can also arise
 * due to failure to update pn->pn_pos.end to be the last child's end -- such
 * failures are bugs to fix.
 *
 * Source note offsets in general must be non-negative and less than 0x800000,
 * per the above SN_3BYTE_* definitions. To encode negative colspans, we bias
 * them by the offset domain size and restrict non-negative colspans to less
 * than half this domain.
 */
#define SN_COLSPAN_DOMAIN       ptrdiff_t(SN_3BYTE_OFFSET_FLAG << 16)

#define SN_MAX_OFFSET ((size_t)((ptrdiff_t)SN_3BYTE_OFFSET_FLAG << 16) - 1)

#define SN_LENGTH(sn)           ((js_SrcNoteSpec[SN_TYPE(sn)].arity == 0) ? 1 \
                                 : js_SrcNoteLength(sn))
#define SN_NEXT(sn)             ((sn) + SN_LENGTH(sn))

/* A source note array is terminated by an all-zero element. */
#define SN_MAKE_TERMINATOR(sn)  (*(sn) = SRC_NULL)
#define SN_IS_TERMINATOR(sn)    (*(sn) == SRC_NULL)

namespace frontend {

/*
 * Append a new source note of the given type (and therefore size) to bce's
 * notes dynamic array, updating bce->noteCount. Return the new note's index
 * within the array pointed at by bce->current->notes. Return -1 if out of
 * memory.
 */
int
NewSrcNote(JSContext *cx, BytecodeEmitter *bce, SrcNoteType type);

int
NewSrcNote2(JSContext *cx, BytecodeEmitter *bce, SrcNoteType type, ptrdiff_t offset);

int
NewSrcNote3(JSContext *cx, BytecodeEmitter *bce, SrcNoteType type, ptrdiff_t offset1,
               ptrdiff_t offset2);

/*
 * NB: this function can add at most one extra extended delta note.
 */
jssrcnote *
AddToSrcNoteDelta(JSContext *cx, BytecodeEmitter *bce, jssrcnote *sn, ptrdiff_t delta);

bool
FinishTakingSrcNotes(JSContext *cx, BytecodeEmitter *bce, jssrcnote *notes);

void
FinishTakingTryNotes(BytecodeEmitter *bce, TryNoteArray *array);

/*
 * Finish taking source notes in cx's notePool, copying final notes to the new
 * stable store allocated by the caller and passed in via notes. Return false
 * on malloc failure, which means this function reported an error.
 *
 * Use this to compute the number of jssrcnotes to allocate and pass in via
 * notes. This method knows a lot about details of FinishTakingSrcNotes, so
 * DON'T CHANGE js::frontend::FinishTakingSrcNotes WITHOUT CHECKING WHETHER
 * THIS METHOD NEEDS CORRESPONDING CHANGES!
 */
inline ptrdiff_t
BytecodeEmitter::countFinalSourceNotes()
{
    ptrdiff_t diff = prologOffset() - prolog.lastNoteOffset;
    ptrdiff_t cnt = prolog.noteCount + main.noteCount + 1;
    if (prolog.noteCount && prolog.currentLine != firstLine) {
        if (diff > SN_DELTA_MASK)
            cnt += JS_HOWMANY(diff - SN_DELTA_MASK, SN_XDELTA_MASK);
        cnt += 2 + ((firstLine > SN_3BYTE_OFFSET_MASK) << 1);
    } else if (diff > 0) {
        if (main.noteCount) {
            jssrcnote *sn = main.notes;
            diff -= SN_IS_XDELTA(sn)
                    ? SN_XDELTA_MASK - (*sn & SN_XDELTA_MASK)
                    : SN_DELTA_MASK - (*sn & SN_DELTA_MASK);
        }
        if (diff > 0)
            cnt += JS_HOWMANY(diff, SN_XDELTA_MASK);
    }
    return cnt;
}

/*
 * To avoid offending js_SrcNoteSpec[SRC_DECL].arity, pack the two data needed
 * to decompile let into one ptrdiff_t:
 *   offset: offset to the LEAVEBLOCK(EXPR) op (not including ENTER/LEAVE)
 *   groupAssign: whether this was an optimized group assign ([x,y] = [a,b])
 */
inline ptrdiff_t PackLetData(size_t offset, bool groupAssign)
{
    JS_ASSERT(offset <= (size_t(-1) >> 1));
    return ptrdiff_t(offset << 1) | ptrdiff_t(groupAssign);
}

inline size_t LetDataToOffset(ptrdiff_t w)
{
    return size_t(w) >> 1;
}

inline bool LetDataToGroupAssign(ptrdiff_t w)
{
    return size_t(w) & 1;
}

} /* namespace frontend */
} /* namespace js */

struct JSSrcNoteSpec {
    const char      *name;      /* name for disassembly/debugging output */
    int8_t          arity;      /* number of offset operands */
};

extern JS_FRIEND_DATA(JSSrcNoteSpec)  js_SrcNoteSpec[];
extern JS_FRIEND_API(unsigned)         js_SrcNoteLength(jssrcnote *sn);

/*
 * Get and set the offset operand identified by which (0 for the first, etc.).
 */
extern JS_FRIEND_API(ptrdiff_t)
js_GetSrcNoteOffset(jssrcnote *sn, unsigned which);

#endif /* BytecodeEmitter_h__ */
