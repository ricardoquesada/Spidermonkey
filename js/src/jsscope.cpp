/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=78:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS symbol tables.
 */
#include <new>
#include <stdlib.h>
#include <string.h>
#include "jstypes.h"
#include "jsclist.h"
#include "jsutil.h"
#include "jsapi.h"
#include "jsatom.h"
#include "jscntxt.h"
#include "jsdbgapi.h"
#include "jslock.h"
#include "jsnum.h"
#include "jsobj.h"
#include "jsscope.h"
#include "jsstr.h"

#include "js/HashTable.h"
#include "js/MemoryMetrics.h"

#include "jsatominlines.h"
#include "jscntxtinlines.h"
#include "jsobjinlines.h"
#include "jsscopeinlines.h"

using namespace js;
using namespace js::gc;

bool
ShapeTable::init(JSRuntime *rt, Shape *lastProp)
{
    /*
     * Either we're creating a table for a large scope that was populated
     * via property cache hit logic under JSOP_INITPROP, JSOP_SETNAME, or
     * JSOP_SETPROP; or else calloc failed at least once already. In any
     * event, let's try to grow, overallocating to hold at least twice the
     * current population.
     */
    uint32_t sizeLog2 = JS_CEILING_LOG2W(2 * entryCount);
    if (sizeLog2 < MIN_SIZE_LOG2)
        sizeLog2 = MIN_SIZE_LOG2;

    /*
     * Use rt->calloc_ for memory accounting and overpressure handling
     * without OOM reporting. See ShapeTable::change.
     */
    entries = (Shape **) rt->calloc_(sizeOfEntries(JS_BIT(sizeLog2)));
    if (!entries)
        return false;

    hashShift = HASH_BITS - sizeLog2;
    for (Shape::Range r = lastProp->all(); !r.empty(); r.popFront()) {
        Shape &shape = r.front();
        Shape **spp = search(shape.propid(), true);

        /*
         * Beware duplicate args and arg vs. var conflicts: the youngest shape
         * (nearest to lastProp) must win. See bug 600067.
         */
        if (!SHAPE_FETCH(spp))
            SHAPE_STORE_PRESERVING_COLLISION(spp, &shape);
    }
    return true;
}

bool
Shape::makeOwnBaseShape(JSContext *cx)
{
    JS_ASSERT(!base()->isOwned());
    assertSameCompartment(cx, compartment());

    RootedShape self(cx, this);

    BaseShape *nbase = js_NewGCBaseShape(cx);
    if (!nbase)
        return false;

    new (nbase) BaseShape(StackBaseShape(self));
    nbase->setOwned(self->base()->toUnowned());

    self->base_ = nbase;

    return true;
}

void
Shape::handoffTableTo(Shape *shape)
{
    JS_ASSERT(inDictionary() && shape->inDictionary());

    if (this == shape)
        return;

    JS_ASSERT(base()->isOwned() && !shape->base()->isOwned());

    BaseShape *nbase = base();

    JS_ASSERT_IF(shape->hasSlot(), nbase->slotSpan() > shape->slot());

    this->base_ = nbase->baseUnowned();
    nbase->adoptUnowned(shape->base()->toUnowned());

    shape->base_ = nbase;
}

bool
Shape::hashify(JSContext *cx)
{
    JS_ASSERT(!hasTable());

    RootedShape self(cx, this);

    if (!ensureOwnBaseShape(cx))
        return false;

    JSRuntime *rt = cx->runtime;
    ShapeTable *table = rt->new_<ShapeTable>(self->entryCount());
    if (!table)
        return false;

    if (!table->init(rt, self)) {
        rt->free_(table);
        return false;
    }

    self->base()->setTable(table);
    return true;
}

/*
 * Double hashing needs the second hash code to be relatively prime to table
 * size, so we simply make hash2 odd.
 */
#define HASH1(hash0,shift)      ((hash0) >> (shift))
#define HASH2(hash0,log2,shift) ((((hash0) << (log2)) >> (shift)) | 1)

Shape **
ShapeTable::search(jsid id, bool adding)
{
    js::HashNumber hash0, hash1, hash2;
    int sizeLog2;
    Shape *stored, *shape, **spp, **firstRemoved;
    uint32_t sizeMask;

    JS_ASSERT(entries);
    JS_ASSERT(!JSID_IS_EMPTY(id));

    /* Compute the primary hash address. */
    hash0 = HashId(id);
    hash1 = HASH1(hash0, hashShift);
    spp = entries + hash1;

    /* Miss: return space for a new entry. */
    stored = *spp;
    if (SHAPE_IS_FREE(stored))
        return spp;

    /* Hit: return entry. */
    shape = SHAPE_CLEAR_COLLISION(stored);
    if (shape && shape->propid() == id)
        return spp;

    /* Collision: double hash. */
    sizeLog2 = HASH_BITS - hashShift;
    hash2 = HASH2(hash0, sizeLog2, hashShift);
    sizeMask = JS_BITMASK(sizeLog2);

#ifdef DEBUG
    uintptr_t collision_flag = SHAPE_COLLISION;
#endif

    /* Save the first removed entry pointer so we can recycle it if adding. */
    if (SHAPE_IS_REMOVED(stored)) {
        firstRemoved = spp;
    } else {
        firstRemoved = NULL;
        if (adding && !SHAPE_HAD_COLLISION(stored))
            SHAPE_FLAG_COLLISION(spp, shape);
#ifdef DEBUG
        collision_flag &= uintptr_t(*spp) & SHAPE_COLLISION;
#endif
    }

    for (;;) {
        hash1 -= hash2;
        hash1 &= sizeMask;
        spp = entries + hash1;

        stored = *spp;
        if (SHAPE_IS_FREE(stored))
            return (adding && firstRemoved) ? firstRemoved : spp;

        shape = SHAPE_CLEAR_COLLISION(stored);
        if (shape && shape->propid() == id) {
            JS_ASSERT(collision_flag);
            return spp;
        }

        if (SHAPE_IS_REMOVED(stored)) {
            if (!firstRemoved)
                firstRemoved = spp;
        } else {
            if (adding && !SHAPE_HAD_COLLISION(stored))
                SHAPE_FLAG_COLLISION(spp, shape);
#ifdef DEBUG
            collision_flag &= uintptr_t(*spp) & SHAPE_COLLISION;
#endif
        }
    }

    /* NOTREACHED */
    return NULL;
}

bool
ShapeTable::change(int log2Delta, JSContext *cx)
{
    JS_ASSERT(entries);

    /*
     * Grow, shrink, or compress by changing this->entries.
     */
    int oldlog2 = HASH_BITS - hashShift;
    int newlog2 = oldlog2 + log2Delta;
    uint32_t oldsize = JS_BIT(oldlog2);
    uint32_t newsize = JS_BIT(newlog2);
    Shape **newTable = (Shape **) cx->calloc_(sizeOfEntries(newsize));
    if (!newTable)
        return false;

    /* Now that we have newTable allocated, update members. */
    hashShift = HASH_BITS - newlog2;
    removedCount = 0;
    Shape **oldTable = entries;
    entries = newTable;

    /* Copy only live entries, leaving removed and free ones behind. */
    for (Shape **oldspp = oldTable; oldsize != 0; oldspp++) {
        Shape *shape = SHAPE_FETCH(oldspp);
        if (shape) {
            Shape **spp = search(shape->propid(), true);
            JS_ASSERT(SHAPE_IS_FREE(*spp));
            *spp = shape;
        }
        oldsize--;
    }

    /* Finally, free the old entries storage. */
    cx->free_(oldTable);
    return true;
}

bool
ShapeTable::grow(JSContext *cx)
{
    JS_ASSERT(needsToGrow());

    uint32_t size = capacity();
    int delta = removedCount < size >> 2;

    if (!change(delta, cx) && entryCount + removedCount == size - 1) {
        JS_ReportOutOfMemory(cx);
        return false;
    }
    return true;
}

Shape *
Shape::getChildBinding(JSContext *cx, const StackShape &child)
{
    JS_ASSERT(!inDictionary());

    /* Try to allocate all slots inline. */
    uint32_t slots = child.slotSpan();
    gc::AllocKind kind = gc::GetGCObjectKind(slots);
    uint32_t nfixed = gc::GetGCKindSlots(kind);

    return cx->propertyTree().getChild(cx, this, nfixed, child);
}

/* static */ Shape *
Shape::replaceLastProperty(JSContext *cx, const StackBaseShape &base, JSObject *proto, Shape *shape_)
{
    RootedShape shape(cx, shape_);

    JS_ASSERT(!shape->inDictionary());

    if (!shape->parent) {
        /* Treat as resetting the initial property of the shape hierarchy. */
        AllocKind kind = gc::GetGCObjectKind(shape->numFixedSlots());
        return EmptyShape::getInitialShape(cx, base.clasp, proto,
                                           base.parent, kind,
                                           base.flags & BaseShape::OBJECT_FLAG_MASK);
    }

    UnownedBaseShape *nbase = BaseShape::getUnowned(cx, base);
    if (!nbase)
        return NULL;

    StackShape child(shape);
    child.base = nbase;

    return cx->propertyTree().getChild(cx, shape->parent, shape->numFixedSlots(), child);
}

/*
 * Get or create a property-tree or dictionary child property of |parent|,
 * which must be lastProperty() if inDictionaryMode(), else parent must be
 * one of lastProperty() or lastProperty()->parent.
 */
Shape *
JSObject::getChildProperty(JSContext *cx, Shape *parent, StackShape &child)
{
    /*
     * Shared properties have no slot, but slot_ will reflect that of parent.
     * Unshared properties allocate a slot here but may lose it due to a
     * JS_ClearScope call.
     */
    if (!child.hasSlot()) {
        child.setSlot(parent->maybeSlot());
    } else {
        if (child.hasMissingSlot()) {
            uint32_t slot;
            if (!allocSlot(cx, &slot))
                return NULL;
            child.setSlot(slot);
        } else {
            /* Slots can only be allocated out of order on objects in dictionary mode. */
            JS_ASSERT(inDictionaryMode() ||
                      parent->hasMissingSlot() ||
                      child.slot() == parent->maybeSlot() + 1);
        }
    }

    Shape *shape;

    RootedObject self(cx, this);

    if (inDictionaryMode()) {
        JS_ASSERT(parent == lastProperty());
        StackShape::AutoRooter childRoot(cx, &child);
        shape = js_NewGCShape(cx);
        if (!shape)
            return NULL;
        if (child.hasSlot() && child.slot() >= self->lastProperty()->base()->slotSpan()) {
            if (!self->setSlotSpan(cx, child.slot() + 1))
                return NULL;
        }
        shape->initDictionaryShape(child, self->numFixedSlots(), &self->shape_);
    } else {
        shape = cx->propertyTree().getChild(cx, parent, self->numFixedSlots(), child);
        if (!shape)
            return NULL;
        //JS_ASSERT(shape->parent == parent);
        //JS_ASSERT_IF(parent != lastProperty(), parent == lastProperty()->parent);
        if (!self->setLastProperty(cx, shape))
            return NULL;
    }

    return shape;
}

bool
JSObject::toDictionaryMode(JSContext *cx)
{
    JS_ASSERT(!inDictionaryMode());

    /* We allocate the shapes from cx->compartment, so make sure it's right. */
    JS_ASSERT(compartment() == cx->compartment);

    uint32_t span = slotSpan();

    RootedObject self(cx, this);

    /*
     * Clone the shapes into a new dictionary list. Don't update the
     * last property of this object until done, otherwise a GC
     * triggered while creating the dictionary will get the wrong
     * slot span for this object.
     */
    RootedShape root(cx);
    RootedShape dictionaryShape(cx);

    RootedShape shape(cx, lastProperty());
    while (shape) {
        JS_ASSERT(!shape->inDictionary());

        Shape *dprop = js_NewGCShape(cx);
        if (!dprop) {
            js_ReportOutOfMemory(cx);
            return false;
        }

        HeapPtrShape *listp = dictionaryShape
                              ? &dictionaryShape->parent
                              : (HeapPtrShape *) root.address();

        StackShape child(shape);
        dprop->initDictionaryShape(child, self->numFixedSlots(), listp);

        JS_ASSERT(!dprop->hasTable());
        dictionaryShape = dprop;
        shape = shape->previous();
    }

    if (!root->hashify(cx)) {
        js_ReportOutOfMemory(cx);
        return false;
    }

    JS_ASSERT((Shape **) root->listp == root.address());
    root->listp = &self->shape_;
    self->shape_ = root;

    JS_ASSERT(self->inDictionaryMode());
    root->base()->setSlotSpan(span);

    return true;
}

/*
 * Normalize stub getter and setter values for faster is-stub testing in the
 * SHAPE_CALL_[GS]ETTER macros.
 */
static inline bool
NormalizeGetterAndSetter(JSContext *cx, JSObject *obj,
                         jsid id, unsigned attrs, unsigned flags,
                         PropertyOp &getter,
                         StrictPropertyOp &setter)
{
    if (setter == JS_StrictPropertyStub) {
        JS_ASSERT(!(attrs & JSPROP_SETTER));
        setter = NULL;
    }
    if (getter == JS_PropertyStub) {
        JS_ASSERT(!(attrs & JSPROP_GETTER));
        getter = NULL;
    }

    return true;
}

Shape *
JSObject::addProperty(JSContext *cx, jsid id,
                      PropertyOp getter, StrictPropertyOp setter,
                      uint32_t slot, unsigned attrs,
                      unsigned flags, int shortid, bool allowDictionary)
{
    JS_ASSERT(!JSID_IS_VOID(id));

    if (!isExtensible()) {
        reportNotExtensible(cx);
        return NULL;
    }

    NormalizeGetterAndSetter(cx, this, id, attrs, flags, getter, setter);

    RootedObject self(cx, this);

    Shape **spp = NULL;
    if (inDictionaryMode())
        spp = lastProperty()->table().search(id, true);

    return self->addPropertyInternal(cx, id, getter, setter, slot, attrs, flags, shortid,
                                     spp, allowDictionary);
}

Shape *
JSObject::addPropertyInternal(JSContext *cx, jsid id_,
                              PropertyOp getter, StrictPropertyOp setter,
                              uint32_t slot, unsigned attrs,
                              unsigned flags, int shortid, Shape **spp,
                              bool allowDictionary)
{
    JS_ASSERT_IF(!allowDictionary, !inDictionaryMode());

    RootedId id(cx, id_);
    RootedObject self(cx, this);

    AutoRooterGetterSetter gsRoot(cx, attrs, &getter, &setter);

    ShapeTable *table = NULL;
    if (!inDictionaryMode()) {
        bool stableSlot =
            (slot == SHAPE_INVALID_SLOT) ||
            lastProperty()->hasMissingSlot() ||
            (slot == lastProperty()->maybeSlot() + 1);
        JS_ASSERT_IF(!allowDictionary, stableSlot);
        if (allowDictionary &&
            (!stableSlot || lastProperty()->entryCount() >= PropertyTree::MAX_HEIGHT)) {
            if (!toDictionaryMode(cx))
                return NULL;
            table = &self->lastProperty()->table();
            spp = table->search(id, true);
        }
    } else {
        table = &lastProperty()->table();
        if (table->needsToGrow()) {
            if (!table->grow(cx))
                return NULL;
            spp = table->search(id, true);
            JS_ASSERT(!SHAPE_FETCH(spp));
        }
    }

    JS_ASSERT(!!table == !!spp);

    /* Find or create a property tree node labeled by our arguments. */
    Shape *shape;
    {
        shape = self->lastProperty();

        uint32_t index;
        bool indexed = js_IdIsIndex(id, &index);
        UnownedBaseShape *nbase;
        if (shape->base()->matchesGetterSetter(getter, setter) && !indexed) {
            nbase = shape->base()->unowned();
        } else {
            StackBaseShape base(shape->base());
            base.updateGetterSetter(attrs, getter, setter);
            if (indexed)
                base.flags |= BaseShape::INDEXED;
            nbase = BaseShape::getUnowned(cx, base);
            if (!nbase)
                return NULL;
        }

        StackShape child(nbase, id, slot, self->numFixedSlots(), attrs, flags, shortid);
        shape = self->getChildProperty(cx, self->lastProperty(), child);
    }

    if (shape) {
        JS_ASSERT(shape == self->lastProperty());

        if (table) {
            /* Store the tree node pointer in the table entry for id. */
            SHAPE_STORE_PRESERVING_COLLISION(spp, shape);
            ++table->entryCount;

            /* Pass the table along to the new last property, namely shape. */
            JS_ASSERT(&shape->parent->table() == table);
            shape->parent->handoffTableTo(shape);
        }

        self->checkShapeConsistency();
        return shape;
    }

    self->checkShapeConsistency();
    return NULL;
}

/*
 * Check and adjust the new attributes for the shape to make sure that our
 * slot access optimizations are sound. It is responsibility of the callers to
 * enforce all restrictions from ECMA-262 v5 8.12.9 [[DefineOwnProperty]].
 */
inline bool
CheckCanChangeAttrs(JSContext *cx, JSObject *obj, Shape *shape, unsigned *attrsp)
{
    if (shape->configurable())
        return true;

    /* A permanent property must stay permanent. */
    *attrsp |= JSPROP_PERMANENT;

    /* Reject attempts to remove a slot from the permanent data property. */
    if (shape->isDataDescriptor() && shape->hasSlot() &&
        (*attrsp & (JSPROP_GETTER | JSPROP_SETTER | JSPROP_SHARED))) {
        obj->reportNotConfigurable(cx, shape->propid());
        return false;
    }

    return true;
}

Shape *
JSObject::putProperty(JSContext *cx, jsid id_,
                      PropertyOp getter, StrictPropertyOp setter,
                      uint32_t slot, unsigned attrs,
                      unsigned flags, int shortid)
{
    RootedId id(cx, id_);
    JS_ASSERT(!JSID_IS_VOID(id));

    NormalizeGetterAndSetter(cx, this, id, attrs, flags, getter, setter);

    RootedObject self(cx, this);
    AutoRooterGetterSetter gsRoot(cx, attrs, &getter, &setter);

    /* Search for id in order to claim its entry if table has been allocated. */
    Shape **spp;
    RootedShape shape(cx, Shape::search(cx, lastProperty(), id, &spp, true));
    if (!shape) {
        /*
         * You can't add properties to a non-extensible object, but you can change
         * attributes of properties in such objects.
         */
        if (!self->isExtensible()) {
            self->reportNotExtensible(cx);
            return NULL;
        }

        return self->addPropertyInternal(cx, id, getter, setter, slot, attrs, flags, shortid, spp, true);
    }

    /* Property exists: search must have returned a valid *spp. */
    JS_ASSERT_IF(spp, !SHAPE_IS_REMOVED(*spp));

    if (!CheckCanChangeAttrs(cx, self, shape, &attrs))
        return NULL;

    /*
     * If the caller wants to allocate a slot, but doesn't care which slot,
     * copy the existing shape's slot into slot so we can match shape, if all
     * other members match.
     */
    bool hadSlot = shape->hasSlot();
    uint32_t oldSlot = shape->maybeSlot();
    if (!(attrs & JSPROP_SHARED) && slot == SHAPE_INVALID_SLOT && hadSlot)
        slot = oldSlot;

    Rooted<UnownedBaseShape*> nbase(cx);
    {
        uint32_t index;
        bool indexed = js_IdIsIndex(id, &index);
        StackBaseShape base(self->lastProperty()->base());
        base.updateGetterSetter(attrs, getter, setter);
        if (indexed)
            base.flags |= BaseShape::INDEXED;
        nbase = BaseShape::getUnowned(cx, base);
        if (!nbase)
            return NULL;
    }

    /*
     * Now that we've possibly preserved slot, check whether all members match.
     * If so, this is a redundant "put" and we can return without more work.
     */
    if (shape->matchesParamsAfterId(nbase, slot, attrs, flags, shortid))
        return shape;

    /*
     * Overwriting a non-last property requires switching to dictionary mode.
     * The shape tree is shared immutable, and we can't removeProperty and then
     * addPropertyInternal because a failure under add would lose data.
     */
    if (shape != self->lastProperty() && !self->inDictionaryMode()) {
        if (!self->toDictionaryMode(cx))
            return NULL;
        spp = self->lastProperty()->table().search(shape->propid(), false);
        shape = SHAPE_FETCH(spp);
    }

    JS_ASSERT_IF(shape->hasSlot() && !(attrs & JSPROP_SHARED), shape->slot() == slot);

    if (self->inDictionaryMode()) {
        /*
         * Updating some property in a dictionary-mode object. Create a new
         * shape for the existing property, and also generate a new shape for
         * the last property of the dictionary (unless the modified property
         * is also the last property).
         */
        bool updateLast = (shape == self->lastProperty());
        shape = self->replaceWithNewEquivalentShape(cx, shape);
        if (!shape)
            return NULL;
        if (!updateLast && !self->generateOwnShape(cx))
            return NULL;

        /* FIXME bug 593129 -- slot allocation and JSObject *this must move out of here! */
        if (slot == SHAPE_INVALID_SLOT && !(attrs & JSPROP_SHARED)) {
            if (!self->allocSlot(cx, &slot))
                return NULL;
        }

        if (updateLast)
            shape->base()->adoptUnowned(nbase);
        else
            shape->base_ = nbase;

        shape->setSlot(slot);
        shape->attrs = uint8_t(attrs);
        shape->flags = flags | Shape::IN_DICTIONARY;
        shape->shortid_ = int16_t(shortid);
    } else {
        /*
         * Updating the last property in a non-dictionary-mode object. Find an
         * alternate shared child of the last property's previous shape.
         */
        StackBaseShape base(self->lastProperty()->base());
        base.updateGetterSetter(attrs, getter, setter);
        UnownedBaseShape *nbase = BaseShape::getUnowned(cx, base);
        if (!nbase)
            return NULL;

        JS_ASSERT(shape == self->lastProperty());

        /* Find or create a property tree node labeled by our arguments. */
        StackShape child(nbase, id, slot, self->numFixedSlots(), attrs, flags, shortid);
        Shape *newShape = self->getChildProperty(cx, shape->parent, child);

        if (!newShape) {
            self->checkShapeConsistency();
            return NULL;
        }

        shape = newShape;
    }

    /*
     * Can't fail now, so free the previous incarnation's slot if the new shape
     * has no slot. But we do not need to free oldSlot (and must not, as trying
     * to will botch an assertion in JSObject::freeSlot) if the new last
     * property (shape here) has a slotSpan that does not cover it.
     */
    if (hadSlot && !shape->hasSlot()) {
        if (oldSlot < self->slotSpan())
            self->freeSlot(cx, oldSlot);
        JS_ATOMIC_INCREMENT(&cx->runtime->propertyRemovals);
    }

    self->checkShapeConsistency();

    return shape;
}

/* static */ Shape *
JSObject::changeProperty(JSContext *cx, HandleObject obj, Shape *shape, unsigned attrs, unsigned mask,
                         PropertyOp getter, StrictPropertyOp setter)
{
    JS_ASSERT(obj->nativeContainsNoAllocation(*shape));

    attrs |= shape->attrs & mask;

    /* Allow only shared (slotless) => unshared (slotful) transition. */
    JS_ASSERT(!((attrs ^ shape->attrs) & JSPROP_SHARED) ||
              !(attrs & JSPROP_SHARED));

    types::MarkTypePropertyConfigured(cx, obj, shape->propid());
    if (attrs & (JSPROP_GETTER | JSPROP_SETTER))
        types::AddTypePropertyId(cx, obj, shape->propid(), types::Type::UnknownType());

    if (getter == JS_PropertyStub)
        getter = NULL;
    if (setter == JS_StrictPropertyStub)
        setter = NULL;

    if (!CheckCanChangeAttrs(cx, obj, shape, &attrs))
        return NULL;

    if (shape->attrs == attrs && shape->getter() == getter && shape->setter() == setter)
        return shape;

    /*
     * Let JSObject::putProperty handle this |overwriting| case, including
     * the conservation of shape->slot (if it's valid). We must not call
     * removeProperty because it will free an allocated shape->slot, and
     * putProperty won't re-allocate it.
     */
    Shape *newShape = obj->putProperty(cx, shape->propid(), getter, setter, shape->maybeSlot(),
                                       attrs, shape->flags, shape->maybeShortid());

    obj->checkShapeConsistency();
    return newShape;
}

bool
JSObject::removeProperty(JSContext *cx, jsid id_)
{
    RootedId id(cx, id_);
    RootedObject self(cx, this);

    Shape **spp;
    RootedShape shape(cx, Shape::search(cx, lastProperty(), id, &spp));
    if (!shape)
        return true;

    /*
     * If shape is not the last property added, or the last property cannot
     * be removed, switch to dictionary mode.
     */
    if (!self->inDictionaryMode() && (shape != self->lastProperty() || !self->canRemoveLastProperty())) {
        if (!self->toDictionaryMode(cx))
            return false;
        spp = self->lastProperty()->table().search(shape->propid(), false);
        shape = SHAPE_FETCH(spp);
    }

    /*
     * If in dictionary mode, get a new shape for the last property after the
     * removal. We need a fresh shape for all dictionary deletions, even of
     * the last property. Otherwise, a shape could replay and caches might
     * return deleted DictionaryShapes! See bug 595365. Do this before changing
     * the object or table, so the remaining removal is infallible.
     */
    RootedShape spare(cx);
    if (self->inDictionaryMode()) {
        spare = js_NewGCShape(cx);
        if (!spare)
            return false;
        new (spare) Shape(shape->base()->unowned(), 0);
        if (shape == self->lastProperty()) {
            /*
             * Get an up to date unowned base shape for the new last property
             * when removing the dictionary's last property. Information in
             * base shapes for non-last properties may be out of sync with the
             * object's state.
             */
            RootedShape previous(cx, self->lastProperty()->parent);
            StackBaseShape base(self->lastProperty()->base());
            base.updateGetterSetter(previous->attrs, previous->getter(), previous->setter());
            BaseShape *nbase = BaseShape::getUnowned(cx, base);
            if (!nbase)
                return false;
            previous->base_ = nbase;
        }
    }

    /* If shape has a slot, free its slot number. */
    if (shape->hasSlot()) {
        self->freeSlot(cx, shape->slot());
        JS_ATOMIC_INCREMENT(&cx->runtime->propertyRemovals);
    }

    /*
     * A dictionary-mode object owns mutable, unique shapes on a non-circular
     * doubly linked list, hashed by lastProperty()->table. So we can edit the
     * list and hash in place.
     */
    if (self->inDictionaryMode()) {
        ShapeTable &table = self->lastProperty()->table();

        if (SHAPE_HAD_COLLISION(*spp)) {
            *spp = SHAPE_REMOVED;
            ++table.removedCount;
            --table.entryCount;
        } else {
            *spp = NULL;
            --table.entryCount;

#ifdef DEBUG
            /*
             * Check the consistency of the table but limit the number of
             * checks not to alter significantly the complexity of the
             * delete in debug builds, see bug 534493.
             */
            Shape *aprop = self->lastProperty();
            for (int n = 50; --n >= 0 && aprop->parent; aprop = aprop->parent)
                JS_ASSERT_IF(aprop != shape, self->nativeContainsNoAllocation(*aprop));
#endif
        }

        /* Remove shape from its non-circular doubly linked list. */
        Shape *oldLastProp = self->lastProperty();
        shape->removeFromDictionary(self);

        /* Hand off table from the old to new last property. */
        oldLastProp->handoffTableTo(self->lastProperty());

        /* Generate a new shape for the object, infallibly. */
        JS_ALWAYS_TRUE(self->generateOwnShape(cx, spare));

        /* Consider shrinking table if its load factor is <= .25. */
        uint32_t size = table.capacity();
        if (size > ShapeTable::MIN_SIZE && table.entryCount <= size >> 2)
            (void) table.change(-1, cx);
    } else {
        /*
         * Non-dictionary-mode shape tables are shared immutables, so all we
         * need do is retract the last property and we'll either get or else
         * lazily make via a later hashify the exact table for the new property
         * lineage.
         */
        JS_ASSERT(shape == self->lastProperty());
        self->removeLastProperty(cx);
    }

    self->checkShapeConsistency();
    return true;
}

void
JSObject::clear(JSContext *cx)
{
    Shape *shape = lastProperty();
    JS_ASSERT(inDictionaryMode() == shape->inDictionary());

    while (shape->parent) {
        shape = shape->parent;
        JS_ASSERT(inDictionaryMode() == shape->inDictionary());
    }
    JS_ASSERT(shape->isEmptyShape());

    if (inDictionaryMode())
        shape->listp = &shape_;

    JS_ALWAYS_TRUE(setLastProperty(cx, shape));

    JS_ATOMIC_INCREMENT(&cx->runtime->propertyRemovals);
    checkShapeConsistency();
}

void
JSObject::rollbackProperties(JSContext *cx, uint32_t slotSpan)
{
    /*
     * Remove properties from this object until it has a matching slot span.
     * The object cannot have escaped in a way which would prevent safe
     * removal of the last properties.
     */
    JS_ASSERT(!inDictionaryMode() && slotSpan <= this->slotSpan());
    while (this->slotSpan() != slotSpan) {
        JS_ASSERT(lastProperty()->hasSlot() && getSlot(lastProperty()->slot()).isUndefined());
        removeLastProperty(cx);
    }
}

Shape *
JSObject::replaceWithNewEquivalentShape(JSContext *cx, Shape *oldShape, Shape *newShape)
{
    JS_ASSERT_IF(oldShape != lastProperty(),
                 inDictionaryMode() &&
                 nativeLookupNoAllocation(oldShape->propidRef()) == oldShape);

    JSObject *self = this;

    if (!inDictionaryMode()) {
        RootedObject selfRoot(cx, self);
        RootedShape newRoot(cx, newShape);
        if (!toDictionaryMode(cx))
            return NULL;
        oldShape = selfRoot->lastProperty();
        self = selfRoot;
        newShape = newRoot;
    }

    if (!newShape) {
        RootedObject selfRoot(cx, self);
        RootedShape oldRoot(cx, oldShape);
        newShape = js_NewGCShape(cx);
        if (!newShape)
            return NULL;
        new (newShape) Shape(oldRoot->base()->unowned(), 0);
        self = selfRoot;
        oldShape = oldRoot;
    }

    ShapeTable &table = self->lastProperty()->table();
    Shape **spp = oldShape->isEmptyShape()
                  ? NULL
                  : table.search(oldShape->propidRef(), false);

    /*
     * Splice the new shape into the same position as the old shape, preserving
     * enumeration order (see bug 601399).
     */
    StackShape nshape(oldShape);
    newShape->initDictionaryShape(nshape, self->numFixedSlots(), oldShape->listp);

    JS_ASSERT(newShape->parent == oldShape);
    oldShape->removeFromDictionary(self);

    if (newShape == self->lastProperty())
        oldShape->handoffTableTo(newShape);

    if (spp)
        SHAPE_STORE_PRESERVING_COLLISION(spp, newShape);
    return newShape;
}

bool
JSObject::shadowingShapeChange(JSContext *cx, const Shape &shape)
{
    return generateOwnShape(cx);
}

/* static */ bool
JSObject::clearParent(JSContext *cx, HandleObject obj)
{
    return setParent(cx, obj, NullPtr());
}

/* static */ bool
JSObject::setParent(JSContext *cx, HandleObject obj, HandleObject parent)
{
    if (parent && !parent->setDelegate(cx))
        return false;

    if (obj->inDictionaryMode()) {
        StackBaseShape base(obj->lastProperty());
        base.parent = parent;
        UnownedBaseShape *nbase = BaseShape::getUnowned(cx, base);
        if (!nbase)
            return false;

        obj->lastProperty()->base()->adoptUnowned(nbase);
        return true;
    }

    Shape *newShape = Shape::setObjectParent(cx, parent, obj->getProto(), obj->shape_);
    if (!newShape)
        return false;

    obj->shape_ = newShape;
    return true;
}

/* static */ Shape *
Shape::setObjectParent(JSContext *cx, JSObject *parent, JSObject *proto, Shape *last)
{
    if (last->getObjectParent() == parent)
        return last;

    StackBaseShape base(last);
    base.parent = parent;

    return replaceLastProperty(cx, base, proto, last);
}

bool
JSObject::preventExtensions(JSContext *cx)
{
    JS_ASSERT(isExtensible());

    RootedObject self(cx, this);

    /*
     * Force lazy properties to be resolved by iterating over the objects' own
     * properties.
     */
    AutoIdVector props(cx);
    if (!js::GetPropertyNames(cx, self, JSITER_HIDDEN | JSITER_OWNONLY, &props))
        return false;

    if (self->isDenseArray())
        self->makeDenseArraySlow(cx, self);

    return self->setFlag(cx, BaseShape::NOT_EXTENSIBLE, GENERATE_SHAPE);
}

bool
JSObject::setFlag(JSContext *cx, /*BaseShape::Flag*/ uint32_t flag_, GenerateShape generateShape)
{
    BaseShape::Flag flag = (BaseShape::Flag) flag_;

    if (lastProperty()->getObjectFlags() & flag)
        return true;

    RootedObject self(cx, this);

    if (inDictionaryMode()) {
        if (generateShape == GENERATE_SHAPE && !generateOwnShape(cx))
            return false;
        StackBaseShape base(self->lastProperty());
        base.flags |= flag;
        UnownedBaseShape *nbase = BaseShape::getUnowned(cx, base);
        if (!nbase)
            return false;

        self->lastProperty()->base()->adoptUnowned(nbase);
        return true;
    }

    Shape *newShape = Shape::setObjectFlag(cx, flag, getProto(), lastProperty());
    if (!newShape)
        return false;

    self->shape_ = newShape;
    return true;
}

/* static */ Shape *
Shape::setObjectFlag(JSContext *cx, BaseShape::Flag flag, JSObject *proto, Shape *last)
{
    if (last->getObjectFlags() & flag)
        return last;

    StackBaseShape base(last);
    base.flags |= flag;

    return replaceLastProperty(cx, base, proto, last);
}

/* static */ inline HashNumber
StackBaseShape::hash(const StackBaseShape *base)
{
    HashNumber hash = base->flags;
    hash = JS_ROTATE_LEFT32(hash, 4) ^ (uintptr_t(base->clasp) >> 3);
    hash = JS_ROTATE_LEFT32(hash, 4) ^ (uintptr_t(base->parent) >> 3);
    hash = JS_ROTATE_LEFT32(hash, 4) ^ uintptr_t(base->rawGetter);
    hash = JS_ROTATE_LEFT32(hash, 4) ^ uintptr_t(base->rawSetter);
    return hash;
}

/* static */ inline bool
StackBaseShape::match(UnownedBaseShape *key, const StackBaseShape *lookup)
{
    return key->flags == lookup->flags
        && key->clasp == lookup->clasp
        && key->parent == lookup->parent
        && key->rawGetter == lookup->rawGetter
        && key->rawSetter == lookup->rawSetter;
}

/* static */ UnownedBaseShape *
BaseShape::getUnowned(JSContext *cx, const StackBaseShape &base)
{
    BaseShapeSet &table = cx->compartment->baseShapes;

    if (!table.initialized() && !table.init())
        return NULL;

    BaseShapeSet::AddPtr p = table.lookupForAdd(&base);

    if (p)
        return *p;

    StackBaseShape::AutoRooter root(cx, &base);

    BaseShape *nbase_ = js_NewGCBaseShape(cx);
    if (!nbase_)
        return NULL;
    new (nbase_) BaseShape(base);

    UnownedBaseShape *nbase = static_cast<UnownedBaseShape *>(nbase_);

    if (!table.relookupOrAdd(p, &base, nbase))
        return NULL;

    return nbase;
}

void
JSCompartment::sweepBaseShapeTable()
{
    if (baseShapes.initialized()) {
        for (BaseShapeSet::Enum e(baseShapes); !e.empty(); e.popFront()) {
            UnownedBaseShape *base = e.front();
            if (!base->isMarked())
                e.removeFront();
        }
    }
}

void
BaseShape::finalize(FreeOp *fop)
{
    if (table_) {
        fop->delete_(table_);
        table_ = NULL;
    }
}

inline
InitialShapeEntry::InitialShapeEntry() : shape(NULL), proto(NULL)
{
}

inline
InitialShapeEntry::InitialShapeEntry(const ReadBarriered<Shape> &shape, JSObject *proto)
  : shape(shape), proto(proto)
{
}

inline InitialShapeEntry::Lookup
InitialShapeEntry::getLookup()
{
    return Lookup(shape->getObjectClass(), proto, shape->getObjectParent(),
                  shape->numFixedSlots(), shape->getObjectFlags());
}

/* static */ inline HashNumber
InitialShapeEntry::hash(const Lookup &lookup)
{
    HashNumber hash = uintptr_t(lookup.clasp) >> 3;
    hash = JS_ROTATE_LEFT32(hash, 4) ^ (uintptr_t(lookup.proto) >> 3);
    hash = JS_ROTATE_LEFT32(hash, 4) ^ (uintptr_t(lookup.parent) >> 3);
    return hash + lookup.nfixed;
}

/* static */ inline bool
InitialShapeEntry::match(const InitialShapeEntry &key, const Lookup &lookup)
{
    return lookup.clasp == key.shape->getObjectClass()
        && lookup.proto == key.proto
        && lookup.parent == key.shape->getObjectParent()
        && lookup.nfixed == key.shape->numFixedSlots()
        && lookup.baseFlags == key.shape->getObjectFlags();
}

/* static */ Shape *
EmptyShape::getInitialShape(JSContext *cx, Class *clasp, JSObject *proto, JSObject *parent,
                            AllocKind kind, uint32_t objectFlags)
{
    InitialShapeSet &table = cx->compartment->initialShapes;

    if (!table.initialized() && !table.init())
        return NULL;

    size_t nfixed = GetGCKindSlots(kind, clasp);
    InitialShapeEntry::Lookup lookup(clasp, proto, parent, nfixed, objectFlags);

    InitialShapeSet::AddPtr p = table.lookupForAdd(lookup);

    if (p)
        return p->shape;

    RootedObject protoRoot(cx, lookup.proto);
    RootedObject parentRoot(cx, lookup.parent);

    StackBaseShape base(clasp, parent, objectFlags);
    Rooted<UnownedBaseShape*> nbase(cx, BaseShape::getUnowned(cx, base));
    if (!nbase)
        return NULL;

    Shape *shape = cx->propertyTree().newShape(cx);
    if (!shape)
        return NULL;
    new (shape) EmptyShape(nbase, nfixed);

    lookup.proto = protoRoot;
    lookup.parent = parentRoot;

    if (!table.relookupOrAdd(p, lookup, InitialShapeEntry(shape, lookup.proto)))
        return NULL;

    return shape;
}

void
NewObjectCache::invalidateEntriesForShape(JSContext *cx, Shape *shape, JSObject *proto_)
{
    Class *clasp = shape->getObjectClass();

    gc::AllocKind kind = gc::GetGCObjectKind(shape->numFixedSlots());
    if (CanBeFinalizedInBackground(kind, clasp))
        kind = GetBackgroundAllocKind(kind);

    Rooted<GlobalObject *> global(cx, &shape->getObjectParent()->global());
    RootedObject proto(cx, proto_);
    types::TypeObject *type = proto->getNewType(cx);

    EntryIndex entry;
    if (lookupGlobal(clasp, global, kind, &entry))
        PodZero(&entries[entry]);
    if (!proto->isGlobal() && lookupProto(clasp, proto, kind, &entry))
        PodZero(&entries[entry]);
    if (lookupType(clasp, type, kind, &entry))
        PodZero(&entries[entry]);
}

/* static */ void
EmptyShape::insertInitialShape(JSContext *cx, Shape *shape, JSObject *proto)
{
    InitialShapeEntry::Lookup lookup(shape->getObjectClass(), proto, shape->getObjectParent(),
                                     shape->numFixedSlots(), shape->getObjectFlags());

    InitialShapeSet::Ptr p = cx->compartment->initialShapes.lookup(lookup);
    JS_ASSERT(p);

    InitialShapeEntry &entry = const_cast<InitialShapeEntry &>(*p);
    JS_ASSERT(entry.shape->isEmptyShape());

    /* The new shape had better be rooted at the old one. */
#ifdef DEBUG
    Shape *nshape = shape;
    while (!nshape->isEmptyShape())
        nshape = nshape->previous();
    JS_ASSERT(nshape == entry.shape);
#endif

    entry.shape = shape;

    /*
     * This affects the shape that will be produced by the various NewObject
     * methods, so clear any cache entry referring to the old shape. This is
     * not required for correctness (though it may bust on the above asserts):
     * the NewObject must always check for a nativeEmpty() result and generate
     * the appropriate properties if found. Clearing the cache entry avoids
     * this duplicate regeneration.
     */
    cx->runtime->newObjectCache.invalidateEntriesForShape(cx, shape, proto);
}

void
JSCompartment::sweepInitialShapeTable()
{
    if (initialShapes.initialized()) {
        for (InitialShapeSet::Enum e(initialShapes); !e.empty(); e.popFront()) {
            const InitialShapeEntry &entry = e.front();
            Shape *shape = entry.shape;
            JSObject *proto = entry.proto;
            if (!IsShapeMarked(&shape) || (proto && !IsObjectMarked(&proto))) {
                e.removeFront();
            } else {
#ifdef DEBUG
                JSObject *parent = shape->getObjectParent();
                JS_ASSERT(!parent || IsObjectMarked(&parent));
                JS_ASSERT(parent == shape->getObjectParent());
#endif
                if (shape != entry.shape || proto != entry.proto) {
                    InitialShapeEntry newKey(shape, proto);
                    e.rekeyFront(newKey.getLookup(), newKey);
                }
            }
        }
    }
}

/*
 * Property lookup hooks on non-native objects are required to return a non-NULL
 * shape to signify that the property has been found. The actual shape returned
 * is arbitrary, and it should never be read from. We use the non-native
 * object's shape_ field, since it is readily available.
 */
void
js::MarkNonNativePropertyFound(HandleObject obj, MutableHandleShape propp)
{
    propp.set(obj->lastProperty());
}

