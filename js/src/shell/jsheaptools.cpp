/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <string.h>

#include "jsapi.h"

#include "jsalloc.h"
#include "jscntxt.h"
#include "jscompartment.h"
#include "jsfun.h"
#include "jsobj.h"
#include "jsprf.h"
#include "jsutil.h"

#include "jsobjinlines.h"

using namespace js;

#ifdef DEBUG


/*** class HeapReverser **************************************************************************/

/*
 * A class for constructing a map of the JavaScript heap, with all
 * reference edges reversed.
 *
 * Unfortunately, it's not possible to build the results for findReferences
 * while visiting things solely in the order that JS_TraceRuntime and
 * JS_TraceChildren reaches them. For example, as you work outward from the
 * roots, suppose an edge from thing T reaches a "gray" thing G --- G being gray
 * because you're still in the midst of traversing its descendants. At this
 * point, you don't know yet whether G will be a referrer or not, and so you
 * can't tell whether T should be a referrer either. And you won't visit T
 * again.
 *
 * So we take a brute-force approach. We reverse the entire graph, and then walk
 * outward from |target| to the representable objects that refer to it, stopping
 * at such objects.
 */

/*
 * A JSTracer that produces a map of the heap with edges reversed.
 *
 * HeapReversers must be allocated in a stack frame. (They contain an AutoArrayRooter,
 * and those must be allocated and destroyed in a stack-like order.)
 *
 * HeapReversers keep all the roots they find in their traversal alive until
 * they are destroyed. So you don't need to worry about nodes going away while
 * you're using them.
 */
class HeapReverser : public JSTracer {
  public:
    struct Edge;

    /* Metadata for a given Cell we have visited. */
    class Node {
      public:
        Node() { }
        Node(JSGCTraceKind kind)
          : kind(kind), incoming(), marked(false) { }

        /*
         * Move constructor and move assignment. These allow us to store our
         * incoming edge Vector in the hash table: Vectors support moves, but
         * not assignments or copy construction.
         */
        Node(MoveRef<Node> rhs)
          : kind(rhs->kind), incoming(Move(rhs->incoming)), marked(rhs->marked) { }
        Node &operator=(MoveRef<Node> rhs) {
            this->~Node();
            new(this) Node(rhs);
            return *this;
        }

        /* What kind of Cell this is. */
        JSGCTraceKind kind;

        /*
         * A vector of this Cell's incoming edges.
         * This must use SystemAllocPolicy because HashMap requires its elements to
         * be constructible with no arguments.
         */
        Vector<Edge, 0, SystemAllocPolicy> incoming;

        /* A mark bit, for other traversals. */
        bool marked;

      private:
        Node(const Node &);
        Node &operator=(const Node &);
    };

    /* Metadata for a heap edge we have traversed. */
    struct Edge {
      public:
        Edge(char *name, void *origin) : name(name), origin(origin) { }
        ~Edge() { js_free(name); }

        /*
         * Move constructor and move assignment. These allow us to live in
         * Vectors without needing to copy our name string when the vector is
         * resized.
         */
        Edge(MoveRef<Edge> rhs) : name(rhs->name), origin(rhs->origin) {
            rhs->name = NULL;
        }
        Edge &operator=(MoveRef<Edge> rhs) {
            this->~Edge();
            new(this) Edge(rhs);
            return *this;
        }

        /* The name of this heap edge. Owned by this Edge. */
        char *name;

        /*
         * The Cell from which this edge originates. NULL means a root. This is
         * a cell address instead of a Node * because Nodes live in HashMap
         * table entries; if the HashMap reallocates its table, all pointers to
         * the Nodes it contains would become invalid. You should look up the
         * address here in |map| to find its Node.
         */
        void *origin;
    };

    /*
     * The result of a reversal is a map from Cells' addresses to Node
     * structures describing their incoming edges.
     */
    typedef HashMap<void *, Node, DefaultHasher<void *>, SystemAllocPolicy> Map;
    Map map;

    /* Construct a HeapReverser for |context|'s heap. */
    HeapReverser(JSContext *cx) : rooter(cx, 0, NULL), parent(NULL) {
        JS_TracerInit(this, JS_GetRuntime(cx), traverseEdgeWithThis);
    }

    bool init() { return map.init(); }

    /* Build a reversed map of the heap in |map|. */
    bool reverseHeap();

  private:
    /*
     * Once we've produced a reversed map of the heap, we need to keep the
     * engine from freeing the objects we've found in it, until we're done using
     * the map. Even if we're only using the map to construct a result object,
     * and not rearranging the heap ourselves, any allocation could cause a
     * garbage collection, which could free objects held internally by the
     * engine (for example, JaegerMonkey object templates, used by jit scripts).
     *
     * So, each time reverseHeap reaches any object, we add it to 'roots', which
     * is cited by 'rooter', so the object will stay alive long enough for us to
     * include it in the results, if needed.
     *
     * Note that AutoArrayRooters must be constructed and destroyed in a
     * stack-like order, so the same rule applies to this HeapReverser. The
     * easiest way to satisfy this requirement is to only allocate HeapReversers
     * as local variables in functions, or in types that themselves follow that
     * rule. This is kind of dumb, but JSAPI doesn't provide any less restricted
     * way to register arrays of roots.
     */
    Vector<jsval, 0, SystemAllocPolicy> roots;
    AutoArrayRooter rooter;

    /*
     * Return the name of the most recent edge this JSTracer has traversed. The
     * result is allocated with malloc; if we run out of memory, raise an error
     * in this HeapReverser's context and return NULL.
     *
     * This may not be called after that edge's call to traverseEdge has
     * returned.
     */
    char *getEdgeDescription();

    /* Class for setting new parent, and then restoring the original. */
    class AutoParent {
      public:
        AutoParent(HeapReverser *reverser, void *newParent) : reverser(reverser) {
            savedParent = reverser->parent;
            reverser->parent = newParent;
        }
        ~AutoParent() {
            reverser->parent = savedParent;
        }
      private:
        HeapReverser *reverser;
        void *savedParent;
    };

    /* A work item in the stack of nodes whose children we need to traverse. */
    struct Child {
        Child(void *cell, JSGCTraceKind kind) : cell(cell), kind(kind) { }
        void *cell;
        JSGCTraceKind kind;
    };

    /*
     * A stack of work items. We represent the stack explicitly to avoid
     * overflowing the C++ stack when traversing long chains of objects.
     */
    Vector<Child, 0, SystemAllocPolicy> work;

    /* When traverseEdge is called, the Cell and kind at which the edge originated. */
    void *parent;

    /* Traverse an edge. */
    bool traverseEdge(void *cell, JSGCTraceKind kind);

    /*
     * JS_TraceRuntime and JS_TraceChildren don't propagate error returns,
     * and out-of-memory errors, by design, don't establish an exception in
     * |context|, so traverseEdgeWithThis uses this to communicate the
     * result of the traversal to reverseHeap.
     */
    bool traversalStatus;

    /* Static member function wrapping 'traverseEdge'. */
    static void traverseEdgeWithThis(JSTracer *tracer, void **thingp, JSGCTraceKind kind) {
        HeapReverser *reverser = static_cast<HeapReverser *>(tracer);
        if (!reverser->traverseEdge(*thingp, kind))
            reverser->traversalStatus = false;
    }

    /* Return a jsval representing a node, if possible; otherwise, return JSVAL_VOID. */
    jsval nodeToValue(void *cell, int kind) {
        if (kind != JSTRACE_OBJECT)
            return JSVAL_VOID;
        JSObject *object = static_cast<JSObject *>(cell);
        return OBJECT_TO_JSVAL(object);
    }
};

bool
HeapReverser::traverseEdge(void *cell, JSGCTraceKind kind)
{
    jsval v = nodeToValue(cell, kind);
    if (v.isObject()) {
        if (!roots.append(v))
            return false;
        rooter.changeArray(roots.begin(), roots.length());
    }

    /* Capture this edge before the JSTracer members get overwritten. */
    char *edgeDescription = getEdgeDescription();
    if (!edgeDescription)
        return false;
    Edge e(edgeDescription, parent);

    Map::AddPtr a = map.lookupForAdd(cell);
    if (!a) {
        /*
         * We've never visited this cell before. Add it to the map (thus
         * marking it as visited), and put it on the work stack, to be
         * visited from the main loop.
         */
        Node n(kind);
        uint32_t generation = map.generation();
        if (!map.add(a, cell, Move(n)) ||
            !work.append(Child(cell, kind)))
            return false;
        /* If the map has been resized, re-check the pointer. */
        if (map.generation() != generation)
            a = map.lookupForAdd(cell);
    }

    /* Add this edge to the reversed map. */
    return a->value.incoming.append(Move(e));
}

bool
HeapReverser::reverseHeap()
{
    traversalStatus = true;

    /* Prime the work stack with the roots of collection. */
    JS_TraceRuntime(this);
    if (!traversalStatus)
        return false;

    /* Traverse children until the stack is empty. */
    while (!work.empty()) {
        const Child child = work.popCopy();
        AutoParent autoParent(this, child.cell);
        JS_TraceChildren(this, child.cell, child.kind);
        if (!traversalStatus)
            return false;
    }

    return true;
}

char *
HeapReverser::getEdgeDescription()
{
    if (!debugPrinter && debugPrintIndex == (size_t) -1) {
        const char *arg = static_cast<const char *>(debugPrintArg);
        char *name = js_pod_malloc<char>(strlen(arg) + 1);
        if (!name)
            return NULL;
        strcpy(name, arg);
        return name;
    }

    /* Lovely; but a fixed size is required by JSTraceNamePrinter. */
    static const int nameSize = 200;
    char *name = js_pod_malloc<char>(nameSize);
    if (!name)
        return NULL;
    if (debugPrinter)
        debugPrinter(this, name, nameSize);
    else
        JS_snprintf(name, nameSize, "%s[%lu]",
                    static_cast<const char *>(debugPrintArg), debugPrintIndex);

    /* Shrink storage to fit. */
    return static_cast<char *>(js_realloc(name, strlen(name) + 1));
}


/*** class ReferenceFinder ***********************************************************************/

/* A class for finding an object's referrers, given a reversed heap map. */
class ReferenceFinder {
  public:
    ReferenceFinder(JSContext *cx, const HeapReverser &reverser)
      : context(cx), reverser(reverser), result(cx) { }

    /* Produce an object describing all references to |target|. */
    JSObject *findReferences(HandleObject target);

  private:
    /* The context in which to do allocation and error-handling. */
    JSContext *context;

    /* A reversed map of the current heap. */
    const HeapReverser &reverser;

    /* The results object we're currently building. */
    RootedObject result;

    /* A list of edges we've traversed to get to a certain point. */
    class Path {
      public:
        Path(const HeapReverser::Edge &edge, Path *next) : edge(edge), next(next) { }

        /*
         * Compute the full path represented by this Path. The result is
         * owned by the caller.
         */
        char *computeName(JSContext *cx);

      private:
        const HeapReverser::Edge &edge;
        Path *next;
    };

    struct AutoNodeMarker {
        AutoNodeMarker(HeapReverser::Node *node) : node(node) { node->marked = true; }
        ~AutoNodeMarker() { node->marked = false; }
      private:
        HeapReverser::Node *node;
    };

    /*
     * Given that we've reached |cell| via |path|, with all Nodes along that
     * path marked, add paths from all reportable objects reachable from cell
     * to |result|.
     */
    bool visit(void *cell, Path *path);

    /*
     * If |cell|, of |kind|, is representable as a JavaScript value, return that
     * value; otherwise, return JSVAL_VOID.
     */
    jsval representable(void *cell, int kind) {
        if (kind == JSTRACE_OBJECT) {
            JSObject *object = static_cast<JSObject *>(cell);

            /* Certain classes of object are for internal use only. */
            if (object->isBlock() ||
                object->isCall() ||
                object->isWith() ||
                object->isDeclEnv()) {
                return JSVAL_VOID;
            }

            /* Internal function objects should also not be revealed. */
            if (JS_ObjectIsFunction(context, object) && IsInternalFunctionObject(object))
                return JSVAL_VOID;

            return OBJECT_TO_JSVAL(object);
        }

        return JSVAL_VOID;
    }

    /* Add |referrer| as something that refers to |target| via |path|. */
    bool addReferrer(jsval referrer, Path *path);
};

bool
ReferenceFinder::visit(void *cell, Path *path)
{
    /* In ReferenceFinder, paths will almost certainly fit on the C++ stack. */
    JS_CHECK_RECURSION(context, return false);

    /* Have we reached a root? Always report that. */
    if (!cell)
        return addReferrer(JSVAL_NULL, path);

    HeapReverser::Map::Ptr p = reverser.map.lookup(cell);
    JS_ASSERT(p);
    HeapReverser::Node *node = &p->value;

    /* Is |cell| a representable cell, reached via a non-empty path? */
    if (path != NULL) {
        jsval representation = representable(cell, node->kind);
        if (!JSVAL_IS_VOID(representation))
            return addReferrer(representation, path);
    }

    /*
     * If we've made a cycle, don't traverse further. We *do* want to include
     * paths from the target to itself, so we don't want to do this check until
     * after we've possibly reported this cell as a referrer.
     */
    if (node->marked)
        return true;
    AutoNodeMarker marker(node);

    /* Visit the origins of all |cell|'s incoming edges. */
    for (size_t i = 0; i < node->incoming.length(); i++) {
        const HeapReverser::Edge &edge = node->incoming[i];
        Path extendedPath(edge, path);
        if (!visit(edge.origin, &extendedPath))
            return false;
    }

    return true;
}

char *
ReferenceFinder::Path::computeName(JSContext *cx)
{
    /* Walk the edge list and compute the total size of the path. */
    size_t size = 6;
    for (Path *l = this; l; l = l->next)
        size += strlen(l->edge.name) + (l->next ? 2 : 0);
    size += 1;

    char *path = cx->pod_malloc<char>(size);
    if (!path)
        return NULL;

    /*
     * Walk the edge list again, and copy the edge names into place, with
     * appropriate separators. Note that we constructed the edge list from
     * target to referrer, which means that the list links point *towards* the
     * target, so we can walk the list and build the path from left to right.
     */
    strcpy(path, "edge: ");
    char *next = path + 6;
    for (Path *l = this; l; l = l->next) {
        strcpy(next, l->edge.name);
        next += strlen(next);
        if (l->next) {
            strcpy(next, "; ");
            next += 2;
        }
    }
    JS_ASSERT(next + 1 == path + size);

    return path;
}

bool
ReferenceFinder::addReferrer(jsval referrerArg, Path *path)
{
    RootedValue referrer(context, referrerArg);

    if (!context->compartment->wrap(context, &referrer))
        return false;

    ScopedJSFreePtr<char> pathName(path->computeName(context));
    if (!pathName)
        return false;

    /* Find the property of the results object named |pathName|. */
    RootedValue valRoot(context);
    Value &v = valRoot.get();

    if (!JS_GetProperty(context, result, pathName, &v))
        return false;
    if (v.isUndefined()) {
        /* Create an array to accumulate referents under this path. */
        JSObject *array = JS_NewArrayObject(context, 1, referrer.address());
        if (!array)
            return false;
        v.setObject(*array);
        return !!JS_SetProperty(context, result, pathName, &v);
    }

    /* The property's value had better be an array. */
    RootedObject array(context, &v.toObject());
    JS_ASSERT(JS_IsArrayObject(context, array));

    /* Append our referrer to this array. */
    uint32_t length;
    return JS_GetArrayLength(context, array, &length) &&
           JS_SetElement(context, array, length, referrer.address());
}

JSObject *
ReferenceFinder::findReferences(HandleObject target)
{
    result = JS_NewObject(context, NULL, NULL, NULL);
    if (!result)
        return NULL;
    if (!visit(target, NULL))
        return NULL;

    return result;
}

/* See help(findReferences). */
JSBool
FindReferences(JSContext *cx, unsigned argc, jsval *vp)
{
    if (argc < 1) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_MORE_ARGS_NEEDED,
                             "findReferences", "0", "s");
        return false;
    }

    RootedValue target(cx, JS_ARGV(cx, vp)[0]);
    if (!target.isObject()) {
        JS_ReportErrorNumber(cx, js_GetErrorMessage, NULL, JSMSG_UNEXPECTED_TYPE,
                             "argument", "not an object");
        return false;
    }

    /* Walk the JSRuntime, producing a reversed map of the heap. */
    HeapReverser reverser(cx);
    if (!reverser.init() || !reverser.reverseHeap())
        return false;

    /* Given the reversed map, find the referents of target. */
    ReferenceFinder finder(cx, reverser);
    Rooted<JSObject*> targetObj(cx, &target.toObject());
    JSObject *references = finder.findReferences(targetObj);
    if (!references)
        return false;

    JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(references));
    return true;
}

#endif /* DEBUG */
