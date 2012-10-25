/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=79 ft=cpp:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsscript_h___
#define jsscript_h___
/*
 * JS script descriptor.
 */
#include "jsprvtd.h"
#include "jsdbgapi.h"
#include "jsclist.h"
#include "jsinfer.h"
#include "jsopcode.h"
#include "jsscope.h"

#include "gc/Barrier.h"

namespace js {

struct Shape;
class BindingIter;

namespace mjit {
struct JITScript;
class CallCompiler;
}

namespace analyze {
class ScriptAnalysis;
}

}

/*
 * Type of try note associated with each catch or finally block, and also with
 * for-in loops.
 */
typedef enum JSTryNoteKind {
    JSTRY_CATCH,
    JSTRY_FINALLY,
    JSTRY_ITER
} JSTryNoteKind;

/*
 * Exception handling record.
 */
struct JSTryNote {
    uint8_t         kind;       /* one of JSTryNoteKind */
    uint8_t         padding;    /* explicit padding on uint16_t boundary */
    uint16_t        stackDepth; /* stack depth upon exception handler entry */
    uint32_t        start;      /* start of the try statement or for-in loop
                                   relative to script->main */
    uint32_t        length;     /* length of the try statement or for-in loop */
};

namespace js {

struct ConstArray {
    js::HeapValue   *vector;    /* array of indexed constant values */
    uint32_t        length;
};

struct ObjectArray {
    js::HeapPtrObject *vector;  /* array of indexed objects */
    uint32_t        length;     /* count of indexed objects */
};

struct TryNoteArray {
    JSTryNote       *vector;    /* array of indexed try notes */
    uint32_t        length;     /* count of indexed try notes */
};

/*
 * A "binding" is a formal, 'var' or 'const' declaration. A function's lexical
 * scope is composed of these three kinds of bindings.
 */

enum BindingKind { ARGUMENT, VARIABLE, CONSTANT };

class Binding
{
    /*
     * One JSScript stores one Binding per formal/variable so we use a
     * packed-word representation.
     */
    uintptr_t bits_;

    static const uintptr_t KIND_MASK = 0x3;
    static const uintptr_t ALIASED_BIT = 0x4;
    static const uintptr_t NAME_MASK = ~(KIND_MASK | ALIASED_BIT);

  public:
    explicit Binding() : bits_(0) {}

    Binding(PropertyName *name, BindingKind kind, bool aliased) {
        JS_STATIC_ASSERT(CONSTANT <= KIND_MASK);
        JS_ASSERT((uintptr_t(name) & ~NAME_MASK) == 0);
        JS_ASSERT((uintptr_t(kind) & ~KIND_MASK) == 0);
        bits_ = uintptr_t(name) | uintptr_t(kind) | (aliased ? ALIASED_BIT : 0);
    }

    PropertyName *name() const {
        return (PropertyName *)(bits_ & NAME_MASK);
    }

    BindingKind kind() const {
        return BindingKind(bits_ & KIND_MASK);
    }

    bool aliased() const {
        return bool(bits_ & ALIASED_BIT);
    }
};

JS_STATIC_ASSERT(sizeof(Binding) == sizeof(uintptr_t));

/*
 * Formal parameters and local variables are stored in a shape tree
 * path encapsulated within this class.  This class represents bindings for
 * both function and top-level scripts (the latter is needed to track names in
 * strict mode eval code, to give such code its own lexical environment).
 */
class Bindings
{
    friend class BindingIter;
    friend class AliasedFormalIter;

    HeapPtr<Shape> callObjShape_;
    uintptr_t bindingArrayAndFlag_;
    uint16_t numArgs_;
    uint16_t numVars_;

    /*
     * During parsing, bindings are allocated out of a temporary LifoAlloc.
     * After parsing, a JSScript object is created and the bindings are
     * permanently transferred to it. On error paths, the JSScript object may
     * end up with bindings that still point to the (new released) LifoAlloc
     * memory. To avoid tracing these bindings during GC, we keep track of
     * whether the bindings are temporary or permanent in the low bit of
     * bindingArrayAndFlag_.
     */
    static const uintptr_t TEMPORARY_STORAGE_BIT = 0x1;
    bool bindingArrayUsingTemporaryStorage() const {
        return bindingArrayAndFlag_ & TEMPORARY_STORAGE_BIT;
    }
    Binding *bindingArray() const {
        return reinterpret_cast<Binding *>(bindingArrayAndFlag_ & ~TEMPORARY_STORAGE_BIT);
    }

  public:
    inline Bindings();

    /*
     * Initialize a Bindings with a pointer into temporary storage.
     * bindingArray must have length numArgs+numVars. Before the temporary
     * storage is release, switchToScriptStorage must be called, providing a
     * pointer into the Binding array stored in script->data.
     */
    bool initWithTemporaryStorage(JSContext *cx, unsigned numArgs, unsigned numVars, Binding *bindingArray);
    uint8_t *switchToScriptStorage(Binding *newStorage);

    /*
     * Clone srcScript's bindings (as part of js::CloneScript). dstScriptData
     * is the pointer to what will eventually be dstScript->data.
     */
    bool clone(JSContext *cx, uint8_t *dstScriptData, HandleScript srcScript);

    unsigned numArgs() const { return numArgs_; }
    unsigned numVars() const { return numVars_; }
    unsigned count() const { return numArgs() + numVars(); }

    /* Return the initial shape of call objects created for this scope. */
    Shape *callObjShape() const { return callObjShape_; }

    /* Convenience method to get the var index of 'arguments'. */
    unsigned argumentsVarIndex(JSContext *cx) const;

    /* Return whether the binding at bindingIndex is aliased. */
    bool bindingIsAliased(unsigned bindingIndex);

    /* Return whether this scope has any aliased bindings. */
    bool hasAnyAliasedBindings() const { return !callObjShape_->isEmptyShape(); }

    void trace(JSTracer *trc);
    class AutoRooter;
};

class Bindings::AutoRooter : private AutoGCRooter
{
  public:
    explicit AutoRooter(JSContext *cx, Bindings *bindings_
                        JS_GUARD_OBJECT_NOTIFIER_PARAM)
      : AutoGCRooter(cx, BINDINGS), bindings(bindings_), skip(cx, bindings_)
    {
        JS_GUARD_OBJECT_NOTIFIER_INIT;
    }

    friend void AutoGCRooter::trace(JSTracer *trc);
    void trace(JSTracer *trc);

  private:
    Bindings *bindings;
    SkipRoot skip;
    JS_DECL_USE_GUARD_OBJECT_NOTIFIER
};

class ScriptCounts
{
    friend struct ::JSScript;
    friend struct ScriptAndCounts;
    /*
     * This points to a single block that holds an array of PCCounts followed
     * by an array of doubles.  Each element in the PCCounts array has a
     * pointer into the array of doubles.
     */
    PCCounts *pcCountsVector;

 public:
    ScriptCounts() : pcCountsVector(NULL) { }

    inline void destroy(FreeOp *fop);

    void set(js::ScriptCounts counts) {
        pcCountsVector = counts.pcCountsVector;
    }
};

typedef HashMap<JSScript *,
                ScriptCounts,
                DefaultHasher<JSScript *>,
                SystemAllocPolicy> ScriptCountsMap;

class DebugScript
{
    friend struct ::JSScript;

    /*
     * When non-zero, compile script in single-step mode. The top bit is set and
     * cleared by setStepMode, as used by JSD. The lower bits are a count,
     * adjusted by changeStepModeCount, used by the Debugger object. Only
     * when the bit is clear and the count is zero may we compile the script
     * without single-step support.
     */
    uint32_t        stepMode;

    /* Number of breakpoint sites at opcodes in the script. */
    uint32_t        numSites;

    /*
     * Array with all breakpoints installed at opcodes in the script, indexed
     * by the offset of the opcode into the script.
     */
    BreakpointSite  *breakpoints[1];
};

typedef HashMap<JSScript *,
                DebugScript *,
                DefaultHasher<JSScript *>,
                SystemAllocPolicy> DebugScriptMap;

struct ScriptSource;

} /* namespace js */

struct JSScript : public js::gc::Cell
{
  private:
    static const uint32_t stepFlagMask = 0x80000000U;
    static const uint32_t stepCountMask = 0x7fffffffU;

  public:
#ifdef JS_METHODJIT
    // This type wraps JITScript.  It has three possible states.
    // - "Empty": no compilation has been attempted and there is no JITScript.
    // - "Unjittable": compilation failed and there is no JITScript.
    // - "Valid": compilation succeeded and there is a JITScript.
    class JITScriptHandle
    {
        // CallCompiler must be a friend because it generates code that uses
        // UNJITTABLE.
        friend class js::mjit::CallCompiler;

        // The exact representation:
        // - NULL means "empty".
        // - UNJITTABLE means "unjittable".
        // - Any other value means "valid".
        // UNJITTABLE = 1 so that we can check that a JITScript is valid
        // with a single |> 1| test.  It's defined outside the class because
        // non-integral static const fields can't be defined in the class.
        static const js::mjit::JITScript *UNJITTABLE;   // = (JITScript *)1;
        js::mjit::JITScript *value;

      public:
        JITScriptHandle()       { value = NULL; }

        bool isEmpty()          { return value == NULL; }
        bool isUnjittable()     { return value == UNJITTABLE; }
        bool isValid()          { return value  > UNJITTABLE; }

        js::mjit::JITScript *getValid() {
            JS_ASSERT(isValid());
            return value;
        }

        void setEmpty()         { value = NULL; }
        void setUnjittable()    { value = const_cast<js::mjit::JITScript *>(UNJITTABLE); }
        void setValid(js::mjit::JITScript *jit) {
            value = jit;
            JS_ASSERT(isValid());
        }

        static void staticAsserts();
    };

    // All the possible JITScripts that can simultaneously exist for a script.
    struct JITScriptSet
    {
        JITScriptHandle jitHandleNormal;          // JIT info for normal scripts
        JITScriptHandle jitHandleNormalBarriered; // barriered JIT info for normal scripts
        JITScriptHandle jitHandleCtor;            // JIT info for constructors
        JITScriptHandle jitHandleCtorBarriered;   // barriered JIT info for constructors

        static size_t jitHandleOffset(bool constructing, bool barriers) {
            return constructing
                ? (barriers
                   ? offsetof(JITScriptSet, jitHandleCtorBarriered)
                   : offsetof(JITScriptSet, jitHandleCtor))
                : (barriers
                   ? offsetof(JITScriptSet, jitHandleNormalBarriered)
                   : offsetof(JITScriptSet, jitHandleNormal));
        }
    };

#endif  // JS_METHODJIT

    //
    // We order fields according to their size in order to avoid wasting space
    // for alignment.
    //

    // Larger-than-word-sized fields.

  public:
    js::Bindings    bindings;   /* names of top-level variables in this script
                                   (and arguments if this is a function script) */

    // Word-sized fields.

  public:
    jsbytecode      *code;      /* bytecodes and their immediate operands */
    uint8_t         *data;      /* pointer to variable-length data array (see
                                   comment above Create() for details) */

    const char      *filename;  /* source filename or null */
    js::HeapPtrAtom *atoms;     /* maps immediate index to literal struct */

    JSPrincipals    *principals;/* principals for this script */
    JSPrincipals    *originPrincipals; /* see jsapi.h 'originPrincipals' comment */

    /* Persistent type information retained across GCs. */
    js::types::TypeScript *types;

  private:
    js::ScriptSource *scriptSource_; /* source code */
#ifdef JS_METHODJIT
    JITScriptSet *mJITInfo;
#endif
    js::HeapPtrFunction function_;
    js::HeapPtrObject   enclosingScope_;

    // 32-bit fields.

  public:
    uint32_t        length;     /* length of code vector */

    uint32_t        lineno;     /* base line number of script */

    uint32_t        mainOffset; /* offset of main entry point from code, after
                                   predef'ing prolog */

    uint32_t        natoms;     /* length of atoms array */

    uint32_t        sourceStart;
    uint32_t        sourceEnd;


  private:
    uint32_t        useCount;   /* Number of times the script has been called
                                 * or has had backedges taken. Reset if the
                                 * script's JIT code is forcibly discarded. */

#if !defined(JS_METHODJIT) && JS_BITS_PER_WORD == 32
    uint32_t        pad32;
#endif

#ifdef DEBUG
    // Unique identifier within the compartment for this script, used for
    // printing analysis information.
    uint32_t        id_;
  private:
    uint32_t        idpad;
#endif

    uint32_t        PADDING;

    // 16-bit fields.

  private:
    uint16_t        version;    /* JS version under which script was compiled */

  public:
    uint16_t        nfixed;     /* number of slots besides stack operands in
                                   slot array */

    uint16_t        nTypeSets;  /* number of type sets used in this script for
                                   dynamic type monitoring */

    uint16_t        nslots;     /* vars plus maximum stack depth */
    uint16_t        staticLevel;/* static level for display maintenance */

    // 8-bit fields.

  public:
    // The kinds of the optional arrays.
    enum ArrayKind {
        CONSTS,
        OBJECTS,
        REGEXPS,
        TRYNOTES,
        LIMIT
    };

    typedef uint8_t ArrayBitsT;

  private:
    // The bits in this field indicate the presence/non-presence of several
    // optional arrays in |data|.  See the comments above Create() for details.
    ArrayBitsT      hasArrayBits;

    // 1-bit fields.

  public:
    bool            noScriptRval:1; /* no need for result value of last
                                       expression statement */
    bool            savedCallerFun:1; /* can call getCallerFunction() */
    bool            strictModeCode:1; /* code is in strict mode */
    bool            explicitUseStrict:1; /* code has "use strict"; explicitly */
    bool            compileAndGo:1;   /* see Parser::compileAndGo */
    bool            bindingsAccessedDynamically:1; /* see ContextFlags' field of the same name */
    bool            funHasExtensibleScope:1;       /* see ContextFlags' field of the same name */
    bool            funHasAnyAliasedFormal:1;      /* true if any formalIsAliased(i) */
    bool            warnedAboutTwoArgumentEval:1; /* have warned about use of
                                                     obsolete eval(s, o) in
                                                     this script */
    bool            warnedAboutUndefinedProp:1; /* have warned about uses of
                                                   undefined properties in this
                                                   script */
    bool            hasSingletons:1;  /* script has singleton objects */
    bool            isActiveEval:1;   /* script came from eval(), and is still active */
    bool            isCachedEval:1;   /* script came from eval(), and is in eval cache */
    bool            uninlineable:1;   /* script is considered uninlineable by analysis */
#ifdef JS_METHODJIT
    bool            debugMode:1;      /* script was compiled in debug mode */
    bool            failedBoundsCheck:1; /* script has had hoisted bounds checks fail */
#endif
    bool            isGenerator:1;    /* is a generator */
    bool            isGeneratorExp:1; /* is a generator expression */
    bool            hasScriptCounts:1;/* script has an entry in
                                         JSCompartment::scriptCountsMap */
    bool            hasDebugScript:1; /* script has an entry in
                                         JSCompartment::debugScriptMap */
    bool            hasFreezeConstraints:1; /* freeze constraints for stack
                                             * type sets have been generated */

  private:
    /* See comments below. */
    bool            argsHasVarBinding_:1;
    bool            needsArgsAnalysis_:1;
    bool            needsArgsObj_:1;

    //
    // End of fields.  Start methods.
    //

  public:
    static JSScript *Create(JSContext *cx, js::HandleObject enclosingScope, bool savedCallerFun,
                            const JS::CompileOptions &options, unsigned staticLevel,
                            js::ScriptSource *ss, uint32_t sourceStart, uint32_t sourceEnd);

    // Three ways ways to initialize a JSScript.  Callers of partiallyInit()
    // and fullyInitTrivial() are responsible for notifying the debugger after
    // successfully creating any kind (function or other) of new JSScript.
    // However, callers of fullyInitFromEmitter() do not need to do this.
    static bool partiallyInit(JSContext *cx, JS::Handle<JSScript*> script,
                              uint32_t length, uint32_t nsrcnotes, uint32_t natoms,
                              uint32_t nobjects, uint32_t nregexps, uint32_t ntrynotes, uint32_t nconsts,
                              uint32_t nTypeSets);
    static bool fullyInitTrivial(JSContext *cx, JS::Handle<JSScript*> script);  // inits a JSOP_STOP-only script
    static bool fullyInitFromEmitter(JSContext *cx, JS::Handle<JSScript*> script,
                                     js::frontend::BytecodeEmitter *bce);

    void setVersion(JSVersion v) { version = v; }

    /* See ContextFlags::funArgumentsHasLocalBinding comment. */
    bool argumentsHasVarBinding() const { return argsHasVarBinding_; }
    jsbytecode *argumentsBytecode() const { JS_ASSERT(code[0] == JSOP_ARGUMENTS); return code; }
    void setArgumentsHasVarBinding();

    /*
     * As an optimization, even when argsHasLocalBinding, the function prologue
     * may not need to create an arguments object. This is determined by
     * needsArgsObj which is set by ScriptAnalysis::analyzeSSA before running
     * the script the first time. When !needsArgsObj, the prologue may simply
     * write MagicValue(JS_OPTIMIZED_ARGUMENTS) to 'arguments's slot and any
     * uses of 'arguments' will be guaranteed to handle this magic value.
     * So avoid spurious arguments object creation, we maintain the invariant
     * that needsArgsObj is only called after the script has been analyzed.
     */
    bool analyzedArgsUsage() const { return !needsArgsAnalysis_; }
    bool needsArgsObj() const { JS_ASSERT(analyzedArgsUsage()); return needsArgsObj_; }
    void setNeedsArgsObj(bool needsArgsObj);
    static bool argumentsOptimizationFailed(JSContext *cx, JSScript *script);

    /*
     * Arguments access (via JSOP_*ARG* opcodes) must access the canonical
     * location for the argument. If an arguments object exists AND this is a
     * non-strict function (where 'arguments' aliases formals), then all access
     * must go through the arguments object. Otherwise, the local slot is the
     * canonical location for the arguments. Note: if a formal is aliased
     * through the scope chain, then script->formalIsAliased and JSOP_*ARG*
     * opcodes won't be emitted at all.
     */
    bool argsObjAliasesFormals() const {
        return needsArgsObj() && !strictModeCode;
    }

    /*
     * Original compiled function for the script, if it has a function.
     * NULL for global and eval scripts.
     */
    JSFunction *function() const { return function_; }
    void setFunction(JSFunction *fun);

    JSFixedString *sourceData(JSContext *cx);

    bool loadSource(JSContext *cx, bool *worked);

    js::ScriptSource *scriptSource() {
        return scriptSource_;
    }

    void setScriptSource(JSContext *cx, js::ScriptSource *ss);

  public:

    /* Return whether this script was compiled for 'eval' */
    bool isForEval() { return isCachedEval || isActiveEval; }

#ifdef DEBUG
    unsigned id();
#else
    unsigned id() { return 0; }
#endif

    /* Ensure the script has a TypeScript. */
    inline bool ensureHasTypes(JSContext *cx);

    /*
     * Ensure the script has bytecode analysis information. Performed when the
     * script first runs, or first runs after a TypeScript GC purge.
     */
    inline bool ensureRanAnalysis(JSContext *cx);

    /* Ensure the script has type inference analysis information. */
    inline bool ensureRanInference(JSContext *cx);

    inline bool hasAnalysis();
    inline void clearAnalysis();
    inline js::analyze::ScriptAnalysis *analysis();

    inline void clearPropertyReadTypes();

    inline bool hasGlobal() const;
    inline bool hasClearedGlobal() const;

    inline js::GlobalObject &global() const;

    /* See StaticScopeIter comment. */
    JSObject *enclosingStaticScope() const {
        JS_ASSERT(enclosingScriptsCompiledSuccessfully());
        return enclosingScope_;
    }

    /*
     * If a compile error occurs in an enclosing function after parsing a
     * nested function, the enclosing function's JSFunction, which appears on
     * the nested function's enclosingScope chain, will be invalid. Normal VM
     * operation only sees scripts where all enclosing scripts have been
     * successfully compiled. Any path that may look at scripts left over from
     * unsuccessful compilation (e.g., by iterating over all scripts in the
     * compartment) should check this predicate before doing any operation that
     * uses enclosingScope (e.g., ScopeCoordinateName).
     */
    bool enclosingScriptsCompiledSuccessfully() const;

  private:
    bool makeTypes(JSContext *cx);
    bool makeAnalysis(JSContext *cx);

#ifdef JS_METHODJIT
  private:
    // CallCompiler must be a friend because it generates code that directly
    // accesses jitHandleNormal/jitHandleCtor, via jitHandleOffset().
    friend class js::mjit::CallCompiler;

  public:
    bool hasMJITInfo() {
        return mJITInfo != NULL;
    }

    static size_t offsetOfMJITInfo() { return offsetof(JSScript, mJITInfo); }

    inline bool ensureHasMJITInfo(JSContext *cx);
    inline void destroyMJITInfo(js::FreeOp *fop);

    JITScriptHandle *jitHandle(bool constructing, bool barriers) {
        JS_ASSERT(mJITInfo);
        return constructing
               ? (barriers ? &mJITInfo->jitHandleCtorBarriered : &mJITInfo->jitHandleCtor)
               : (barriers ? &mJITInfo->jitHandleNormalBarriered : &mJITInfo->jitHandleNormal);
    }

    js::mjit::JITScript *getJIT(bool constructing, bool barriers) {
        if (!mJITInfo)
            return NULL;
        JITScriptHandle *jith = jitHandle(constructing, barriers);
        return jith->isValid() ? jith->getValid() : NULL;
    }

    static void ReleaseCode(js::FreeOp *fop, JITScriptHandle *jith);

    // These methods are implemented in MethodJIT.h.
    inline void **nativeMap(bool constructing);
    inline void *nativeCodeForPC(bool constructing, jsbytecode *pc);

    uint32_t getUseCount() const  { return useCount; }
    uint32_t incUseCount() { return ++useCount; }
    uint32_t *addressOfUseCount() { return &useCount; }
    void resetUseCount() { useCount = 0; }

    /*
     * Size of the JITScript and all sections.  If |mallocSizeOf| is NULL, the
     * size is computed analytically.  (This method is implemented in
     * MethodJIT.cpp.)
     */
    size_t sizeOfJitScripts(JSMallocSizeOfFun mallocSizeOf);
#endif

  public:
    bool initScriptCounts(JSContext *cx);
    js::PCCounts getPCCounts(jsbytecode *pc);
    js::ScriptCounts releaseScriptCounts();
    void destroyScriptCounts(js::FreeOp *fop);

    jsbytecode *main() {
        return code + mainOffset;
    }

    /*
     * computedSizeOfData() is the in-use size of all the data sections.
     * sizeOfData() is the size of the block allocated to hold all the data sections
     * (which can be larger than the in-use size).
     */
    size_t computedSizeOfData();
    size_t sizeOfData(JSMallocSizeOfFun mallocSizeOf);

    uint32_t numNotes();  /* Number of srcnote slots in the srcnotes section */

    /* Script notes are allocated right after the code. */
    jssrcnote *notes() { return (jssrcnote *)(code + length); }

    bool hasArray(ArrayKind kind)           { return (hasArrayBits & (1 << kind)); }
    void setHasArray(ArrayKind kind)        { hasArrayBits |= (1 << kind); }
    void cloneHasArray(JSScript *script)    { hasArrayBits = script->hasArrayBits; }

    bool hasConsts()        { return hasArray(CONSTS);      }
    bool hasObjects()       { return hasArray(OBJECTS);     }
    bool hasRegexps()       { return hasArray(REGEXPS);     }
    bool hasTrynotes()      { return hasArray(TRYNOTES);    }

    #define OFF(fooOff, hasFoo, t)   (fooOff() + (hasFoo() ? sizeof(t) : 0))

    size_t constsOffset()     { return 0; }
    size_t objectsOffset()    { return OFF(constsOffset,     hasConsts,     js::ConstArray);      }
    size_t regexpsOffset()    { return OFF(objectsOffset,    hasObjects,    js::ObjectArray);     }
    size_t trynotesOffset()   { return OFF(regexpsOffset,    hasRegexps,    js::ObjectArray);     }

    js::ConstArray *consts() {
        JS_ASSERT(hasConsts());
        return reinterpret_cast<js::ConstArray *>(data + constsOffset());
    }

    js::ObjectArray *objects() {
        JS_ASSERT(hasObjects());
        return reinterpret_cast<js::ObjectArray *>(data + objectsOffset());
    }

    js::ObjectArray *regexps() {
        JS_ASSERT(hasRegexps());
        return reinterpret_cast<js::ObjectArray *>(data + regexpsOffset());
    }

    js::TryNoteArray *trynotes() {
        JS_ASSERT(hasTrynotes());
        return reinterpret_cast<js::TryNoteArray *>(data + trynotesOffset());
    }

    js::HeapPtrAtom &getAtom(size_t index) const {
        JS_ASSERT(index < natoms);
        return atoms[index];
    }

    js::HeapPtrAtom &getAtom(jsbytecode *pc) const {
        JS_ASSERT(pc >= code && pc + sizeof(uint32_t) < code + length);
        return getAtom(GET_UINT32_INDEX(pc));
    }

    js::PropertyName *getName(size_t index) {
        return getAtom(index)->asPropertyName();
    }

    js::PropertyName *getName(jsbytecode *pc) const {
        JS_ASSERT(pc >= code && pc + sizeof(uint32_t) < code + length);
        return getAtom(GET_UINT32_INDEX(pc))->asPropertyName();
    }

    JSObject *getObject(size_t index) {
        js::ObjectArray *arr = objects();
        JS_ASSERT(index < arr->length);
        return arr->vector[index];
    }

    JSObject *getObject(jsbytecode *pc) {
        JS_ASSERT(pc >= code && pc + sizeof(uint32_t) < code + length);
        return getObject(GET_UINT32_INDEX(pc));
    }

    JSVersion getVersion() const {
        return JSVersion(version);
    }

    inline JSFunction *getFunction(size_t index);
    inline JSFunction *getCallerFunction();

    inline JSObject *getRegExp(size_t index);

    const js::Value &getConst(size_t index) {
        js::ConstArray *arr = consts();
        JS_ASSERT(index < arr->length);
        return arr->vector[index];
    }

    /*
     * The isEmpty method tells whether this script has code that computes any
     * result (not return value, result AKA normal completion value) other than
     * JSVAL_VOID, or any other effects.
     */
    inline bool isEmpty() const;

    bool varIsAliased(unsigned varSlot);
    bool formalIsAliased(unsigned argSlot);
    bool formalLivesInArgumentsObject(unsigned argSlot);

  private:
    /*
     * Recompile with or without single-stepping support, as directed
     * by stepModeEnabled().
     */
    void recompileForStepMode(js::FreeOp *fop);

    /* Attempt to change this->stepMode to |newValue|. */
    bool tryNewStepMode(JSContext *cx, uint32_t newValue);

    bool ensureHasDebugScript(JSContext *cx);
    js::DebugScript *debugScript();
    js::DebugScript *releaseDebugScript();
    void destroyDebugScript(js::FreeOp *fop);

  public:
    bool hasBreakpointsAt(jsbytecode *pc) { return !!getBreakpointSite(pc); }
    bool hasAnyBreakpointsOrStepMode() { return hasDebugScript; }

    js::BreakpointSite *getBreakpointSite(jsbytecode *pc)
    {
        JS_ASSERT(size_t(pc - code) < length);
        return hasDebugScript ? debugScript()->breakpoints[pc - code] : NULL;
    }

    js::BreakpointSite *getOrCreateBreakpointSite(JSContext *cx, jsbytecode *pc);

    void destroyBreakpointSite(js::FreeOp *fop, jsbytecode *pc);

    void clearBreakpointsIn(js::FreeOp *fop, js::Debugger *dbg, JSObject *handler);
    void clearTraps(js::FreeOp *fop);

    void markTrapClosures(JSTracer *trc);

    /*
     * Set or clear the single-step flag. If the flag is set or the count
     * (adjusted by changeStepModeCount) is non-zero, then the script is in
     * single-step mode. (JSD uses an on/off-style interface; Debugger uses a
     * count-style interface.)
     */
    bool setStepModeFlag(JSContext *cx, bool step);

    /*
     * Increment or decrement the single-step count. If the count is non-zero or
     * the flag (set by setStepModeFlag) is set, then the script is in
     * single-step mode. (JSD uses an on/off-style interface; Debugger uses a
     * count-style interface.)
     */
    bool changeStepModeCount(JSContext *cx, int delta);

    bool stepModeEnabled() { return hasDebugScript && !!debugScript()->stepMode; }

#ifdef DEBUG
    uint32_t stepModeCount() { return hasDebugScript ? (debugScript()->stepMode & stepCountMask) : 0; }
#endif

    void finalize(js::FreeOp *fop);

    static inline void writeBarrierPre(JSScript *script);
    static inline void writeBarrierPost(JSScript *script, void *addr);

    static inline js::ThingRootKind rootKind() { return js::THING_ROOT_SCRIPT; }

    static JSPrincipals *normalizeOriginPrincipals(JSPrincipals *principals,
                                                   JSPrincipals *originPrincipals) {
        return originPrincipals ? originPrincipals : principals;
    }

    void markChildren(JSTracer *trc);
};

JS_STATIC_ASSERT(sizeof(JSScript::ArrayBitsT) * 8 >= JSScript::LIMIT);

/* If this fails, add/remove padding within JSScript. */
JS_STATIC_ASSERT(sizeof(JSScript) % js::gc::Cell::CellSize == 0);

namespace js {

/*
 * Iterator over a script's bindings (formals and variables).
 * The order of iteration is:
 *  - first, formal arguments, from index 0 to numArgs
 *  - next, variables, from index 0 to numVars
 */
class BindingIter
{
    const Bindings *bindings_;
    unsigned i_;

    friend class Bindings;
    BindingIter(const Bindings &bindings, unsigned i) : bindings_(&bindings), i_(i) {}

  public:
    explicit BindingIter(const Bindings &bindings) : bindings_(&bindings), i_(0) {}

    bool done() const { return i_ == bindings_->count(); }
    operator bool() const { return !done(); }
    void operator++(int) { JS_ASSERT(!done()); i_++; }
    BindingIter &operator++() { (*this)++; return *this; }

    unsigned frameIndex() const {
        JS_ASSERT(!done());
        return i_ < bindings_->numArgs() ? i_ : i_ - bindings_->numArgs();
    }

    const Binding &operator*() const { JS_ASSERT(!done()); return bindings_->bindingArray()[i_]; }
    const Binding *operator->() const { JS_ASSERT(!done()); return &bindings_->bindingArray()[i_]; }
};

/*
 * This helper function fills the given BindingVector with the sequential
 * values of BindingIter.
 */

typedef Vector<Binding, 32> BindingVector;

extern bool
FillBindingVector(Bindings &bindings, BindingVector *vec);

/*
 * Iterator over the aliased formal bindings in ascending index order. This can
 * be veiwed as a filtering of BindingIter with predicate
 *   bi->aliased() && bi->kind() == ARGUMENT
 */
class AliasedFormalIter
{
    const Binding *begin_, *p_, *end_;
    unsigned slot_;

    void settle() {
        while (p_ != end_ && !p_->aliased())
            p_++;
    }

  public:
    explicit inline AliasedFormalIter(JSScript *script);

    bool done() const { return p_ == end_; }
    operator bool() const { return !done(); }
    void operator++(int) { JS_ASSERT(!done()); p_++; slot_++; settle(); }

    const Binding &operator*() const { JS_ASSERT(!done()); return *p_; }
    const Binding *operator->() const { JS_ASSERT(!done()); return p_; }
    unsigned frameIndex() const { JS_ASSERT(!done()); return p_ - begin_; }
    unsigned scopeSlot() const { JS_ASSERT(!done()); return slot_; }
};

}  /* namespace js */

/*
 * New-script-hook calling is factored from JSScript::fullyInitFromEmitter() so
 * that it and callers of XDRScript() can share this code.  In the case of
 * callers of XDRScript(), the hook should be invoked only after successful
 * decode of any owning function (the fun parameter) or script object (null
 * fun).
 */
extern JS_FRIEND_API(void)
js_CallNewScriptHook(JSContext *cx, JSScript *script, JSFunction *fun);

namespace js {

struct SourceCompressionToken;

struct ScriptSource
{
    friend class SourceCompressorThread;
  private:
    union {
        // When the script source is ready, compressedLength_ != 0 implies
        // compressed holds the compressed data; otherwise, source holds the
        // uncompressed source.
        jschar *source;
        unsigned char *compressed;
    } data;
    uint32_t refs;
    uint32_t length_;
    uint32_t compressedLength_;
    jschar *sourceMap_;

    // True if we can call JSRuntime::sourceHook to load the source on
    // demand. If sourceRetrievable_ and hasSourceData() are false, it is not
    // possible to get source at all.
    bool sourceRetrievable_:1;
    bool argumentsNotIncluded_:1;
#ifdef DEBUG
    bool ready_:1;
#endif

  public:
    ScriptSource()
      : refs(0),
        length_(0),
        compressedLength_(0),
        sourceMap_(NULL),
        sourceRetrievable_(false),
        argumentsNotIncluded_(false)
#ifdef DEBUG
       ,ready_(true)
#endif
    {
        data.source = NULL;
    }
    void incref() { refs++; }
    void decref(JSRuntime *rt) {
        JS_ASSERT(refs != 0);
        if (--refs == 0)
            destroy(rt);
    }
    bool setSourceCopy(JSContext *cx,
                       const jschar *src,
                       uint32_t length,
                       bool argumentsNotIncluded,
                       SourceCompressionToken *tok);
    void setSource(const jschar *src, uint32_t length);
#ifdef DEBUG
    bool ready() const { return ready_; }
#endif
    void setSourceRetrievable() { sourceRetrievable_ = true; }
    bool sourceRetrievable() const { return sourceRetrievable_; }
    bool hasSourceData() const { return !!data.source; }
    uint32_t length() const {
        JS_ASSERT(hasSourceData());
        return length_;
    }
    bool argumentsNotIncluded() const {
        JS_ASSERT(hasSourceData());
        return argumentsNotIncluded_;
    }
    JSFixedString *substring(JSContext *cx, uint32_t start, uint32_t stop);
    size_t sizeOfIncludingThis(JSMallocSizeOfFun mallocSizeOf);

    // XDR handling
    template <XDRMode mode>
    bool performXDR(XDRState<mode> *xdr);

    // Source maps
    bool setSourceMap(JSContext *cx, jschar *sourceMapURL, const char *filename);
    const jschar *sourceMap();
    bool hasSourceMap() const { return sourceMap_ != NULL; }

  private:
    void destroy(JSRuntime *rt);
    bool compressed() { return compressedLength_ != 0; }
};

class ScriptSourceHolder
{
    JSRuntime *rt;
    ScriptSource *ss;
  public:
    ScriptSourceHolder(JSRuntime *rt, ScriptSource *ss)
      : rt(rt),
        ss(ss)
    {
        ss->incref();
    }
    ~ScriptSourceHolder()
    {
        ss->decref(rt);
    }
};

#ifdef JS_THREADSAFE
/*
 * Background thread to compress JS source code. This happens only while parsing
 * and bytecode generation is happening in the main thread. If needed, the
 * compiler waits for compression to complete before returning.
 *
 * To use it, you have to have a SourceCompressionToken, tok, with tok.ss and
 * tok.chars set to the proper values. When the SourceCompressionToken is
 * destroyed, it makes sure the compression is complete. At this point tok.ss is
 * ready to be attached to the runtime.
 */
class SourceCompressorThread
{
  private:
    enum {
        // The compression thread is in the process of compression some source.
        COMPRESSING,
        // The compression thread is not doing anything and available to
        // compress source.
        IDLE,
        // Set by finish() to tell the compression thread to exit.
        SHUTDOWN
    } state;
    SourceCompressionToken *tok;
    PRThread *thread;
    // Protects |state| and |tok| when it's non-NULL.
    PRLock *lock;
    // When it's idling, the compression thread blocks on this. The main thread
    // uses it to notify the compression thread when it has source to be
    // compressed.
    PRCondVar *wakeup;
    // The main thread can block on this to wait for compression to finish.
    PRCondVar *done;
    // Flag which can be set by the main thread to ask compression to abort.
    volatile bool stop;

    void threadLoop();
    static void compressorThread(void *arg);

  public:
    explicit SourceCompressorThread(JSRuntime *rt)
    : state(IDLE),
      tok(NULL),
      thread(NULL),
      lock(NULL),
      wakeup(NULL),
      done(NULL) {}
    void finish();
    bool init();
    void compress(SourceCompressionToken *tok);
    void waitOnCompression(SourceCompressionToken *userTok);
    void abort(SourceCompressionToken *userTok);
};
#endif

struct SourceCompressionToken
{
    friend struct ScriptSource;
    friend class SourceCompressorThread;
  private:
    JSContext *cx;
    ScriptSource *ss;
    const jschar *chars;
  public:
    SourceCompressionToken(JSContext *cx)
      : cx(cx), ss(NULL), chars(NULL) {}
    ~SourceCompressionToken()
    {
        JS_ASSERT_IF(!ss, !chars);
        if (ss)
            ensureReady();
    }

    void ensureReady();
    void abort();
};

extern void
CallDestroyScriptHook(FreeOp *fop, JSScript *script);

extern const char *
SaveScriptFilename(JSContext *cx, const char *filename);

struct ScriptFilenameEntry
{
    bool marked;
    char filename[1];

    static ScriptFilenameEntry *fromFilename(const char *filename) {
        return (ScriptFilenameEntry *)(filename - offsetof(ScriptFilenameEntry, filename));
    }
};

struct ScriptFilenameHasher
{
    typedef const char *Lookup;
    static HashNumber hash(const char *l) { return mozilla::HashString(l); }
    static bool match(const ScriptFilenameEntry *e, const char *l) {
        return strcmp(e->filename, l) == 0;
    }
};

typedef HashSet<ScriptFilenameEntry *,
                ScriptFilenameHasher,
                SystemAllocPolicy> ScriptFilenameTable;

inline void
MarkScriptFilename(JSRuntime *rt, const char *filename);

extern void
SweepScriptFilenames(JSRuntime *rt);

extern void
FreeScriptFilenames(JSRuntime *rt);

struct ScriptAndCounts
{
    JSScript *script;
    ScriptCounts scriptCounts;

    PCCounts &getPCCounts(jsbytecode *pc) const {
        JS_ASSERT(unsigned(pc - script->code) < script->length);
        return scriptCounts.pcCountsVector[pc - script->code];
    }
};

} /* namespace js */

/*
 * To perturb as little code as possible, we introduce a js_GetSrcNote lookup
 * cache without adding an explicit cx parameter.  Thus js_GetSrcNote becomes
 * a macro that uses cx from its calls' lexical environments.
 */
#define js_GetSrcNote(script,pc) js_GetSrcNoteCached(cx, script, pc)

extern jssrcnote *
js_GetSrcNoteCached(JSContext *cx, JSScript *script, jsbytecode *pc);

extern jsbytecode *
js_LineNumberToPC(JSScript *script, unsigned lineno);

extern JS_FRIEND_API(unsigned)
js_GetScriptLineExtent(JSScript *script);

namespace js {

extern unsigned
PCToLineNumber(JSScript *script, jsbytecode *pc, unsigned *columnp = NULL);

extern unsigned
PCToLineNumber(unsigned startLine, jssrcnote *notes, jsbytecode *code, jsbytecode *pc,
               unsigned *columnp = NULL);

extern unsigned
CurrentLine(JSContext *cx);

/*
 * This function returns the file and line number of the script currently
 * executing on cx. If there is no current script executing on cx (e.g., a
 * native called directly through JSAPI (e.g., by setTimeout)), NULL and 0 are
 * returned as the file and line. Additionally, this function avoids the full
 * linear scan to compute line number when the caller guarnatees that the
 * script compilation occurs at a JSOP_EVAL.
 */

enum LineOption {
    CALLED_FROM_JSOP_EVAL,
    NOT_CALLED_FROM_JSOP_EVAL
};

inline void
CurrentScriptFileLineOrigin(JSContext *cx, unsigned *linenop, LineOption = NOT_CALLED_FROM_JSOP_EVAL);

extern JSScript *
CloneScript(JSContext *cx, HandleObject enclosingScope, HandleFunction fun, HandleScript script);

/*
 * NB: after a successful XDR_DECODE, XDRScript callers must do any required
 * subsequent set-up of owning function or script object and then call
 * js_CallNewScriptHook.
 */
template<XDRMode mode>
bool
XDRScript(XDRState<mode> *xdr, HandleObject enclosingScope, HandleScript enclosingScript,
          HandleFunction fun, JSScript **scriptp);

} /* namespace js */

#endif /* jsscript_h___ */
