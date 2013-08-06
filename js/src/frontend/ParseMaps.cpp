/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jscntxt.h"
#include "jscompartment.h"
#include "FullParseHandler.h"
#include "SyntaxParseHandler.h"

#include "ParseMaps-inl.h"
#include "vm/String-inl.h"

using namespace js;
using namespace js::frontend;

void
ParseMapPool::checkInvariants()
{
    /*
     * Having all values be of the same size permits us to easily reuse the
     * allocated space for each of the map types.
     */
    JS_STATIC_ASSERT(sizeof(Definition *) == sizeof(jsatomid));
    JS_STATIC_ASSERT(sizeof(Definition *) == sizeof(DefinitionList));
    JS_STATIC_ASSERT(sizeof(AtomDefnMap::Entry) == sizeof(AtomIndexMap::Entry));
    JS_STATIC_ASSERT(sizeof(AtomDefnMap::Entry) == sizeof(AtomDefnListMap::Entry));
    JS_STATIC_ASSERT(sizeof(AtomMapT::Entry) == sizeof(AtomDefnListMap::Entry));
    /* Ensure that the HasTable::clear goes quickly via memset. */
    JS_STATIC_ASSERT(mozilla::IsPod<AtomIndexMap::WordMap::Entry>::value);
    JS_STATIC_ASSERT(mozilla::IsPod<AtomDefnListMap::WordMap::Entry>::value);
    JS_STATIC_ASSERT(mozilla::IsPod<AtomDefnMap::WordMap::Entry>::value);
}

void
ParseMapPool::purgeAll()
{
    for (void **it = all.begin(), **end = all.end(); it != end; ++it)
        js_delete<AtomMapT>(asAtomMap(*it));

    all.clearAndFree();
    recyclable.clearAndFree();
}

void *
ParseMapPool::allocateFresh()
{
    size_t newAllLength = all.length() + 1;
    if (!all.reserve(newAllLength) || !recyclable.reserve(newAllLength))
        return NULL;

    AtomMapT *map = cx->new_<AtomMapT>(cx);
    if (!map)
        return NULL;

    all.infallibleAppend(map);
    return (void *) map;
}

DefinitionList::Node *
DefinitionList::allocNode(JSContext *cx, uintptr_t head, Node *tail)
{
    Node *result = cx->tempLifoAlloc().new_<Node>(head, tail);
    if (!result)
        js_ReportOutOfMemory(cx);
    return result;
}

#ifdef DEBUG
template <typename ParseHandler>
void
AtomDecls<ParseHandler>::dump()
{
    for (AtomDefnListRange r = map->all(); !r.empty(); r.popFront()) {
        fprintf(stderr, "atom: ");
        js_DumpAtom(r.front().key());
        const DefinitionList &dlist = r.front().value();
        for (DefinitionList::Range dr = dlist.all(); !dr.empty(); dr.popFront()) {
            fprintf(stderr, "    defn: %p\n", (void *) dr.front<ParseHandler>());
        }
    }
}

void
DumpAtomDefnMap(const AtomDefnMapPtr &map)
{
    if (map->empty()) {
        fprintf(stderr, "empty\n");
        return;
    }

    for (AtomDefnRange r = map->all(); !r.empty(); r.popFront()) {
        fprintf(stderr, "atom: ");
        js_DumpAtom(r.front().key());
        fprintf(stderr, "defn: %p\n", (void *) r.front().value().get<FullParseHandler>());
    }
}
#endif

template <typename ParseHandler>
bool
AtomDecls<ParseHandler>::addShadow(JSAtom *atom, typename ParseHandler::DefinitionNode defn)
{
    AtomDefnListAddPtr p = map->lookupForAdd(atom);
    if (!p)
        return map->add(p, atom, DefinitionList(ParseHandler::definitionToBits(defn)));

    return p.value().pushFront<ParseHandler>(cx, defn);
}

void
frontend::InitAtomMap(JSContext *cx, frontend::AtomIndexMap *indices, HeapPtrAtom *atoms)
{
    if (indices->isMap()) {
        typedef AtomIndexMap::WordMap WordMap;
        const WordMap &wm = indices->asMap();
        for (WordMap::Range r = wm.all(); !r.empty(); r.popFront()) {
            JSAtom *atom = r.front().key;
            jsatomid index = r.front().value;
            JS_ASSERT(index < indices->count());
            atoms[index].init(atom);
        }
    } else {
        for (const AtomIndexMap::InlineElem *it = indices->asInline(), *end = indices->inlineEnd();
             it != end; ++it) {
            JSAtom *atom = it->key;
            if (!atom)
                continue;
            JS_ASSERT(it->value < indices->count());
            atoms[it->value].init(atom);
        }
    }
}

template class AtomDecls<FullParseHandler>;
template class AtomDecls<SyntaxParseHandler>;
