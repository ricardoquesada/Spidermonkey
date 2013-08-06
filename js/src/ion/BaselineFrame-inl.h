/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(jsion_baseline_frame_inl_h__) && defined(JS_ION)
#define jsion_baseline_frame_inl_h__

#include "jscntxt.h"
#include "jscompartment.h"

#include "IonFrames.h"
#include "vm/ScopeObject-inl.h"

namespace js {
namespace ion {

inline void
BaselineFrame::pushOnScopeChain(ScopeObject &scope)
{
    JS_ASSERT(*scopeChain() == scope.enclosingScope() ||
              *scopeChain() == scope.asCall().enclosingScope().asDeclEnv().enclosingScope());
    scopeChain_ = &scope;
}

inline void
BaselineFrame::popOffScopeChain()
{
    scopeChain_ = &scopeChain_->asScope().enclosingScope();
}

inline bool
BaselineFrame::pushBlock(JSContext *cx, Handle<StaticBlockObject *> block)
{
    JS_ASSERT_IF(hasBlockChain(), blockChain() == *block->enclosingBlock());

    if (block->needsClone()) {
        ClonedBlockObject *clone = ClonedBlockObject::create(cx, block, this);
        if (!clone)
            return false;

        pushOnScopeChain(*clone);
    }

    setBlockChain(*block);
    return true;
}

inline void
BaselineFrame::popBlock(JSContext *cx)
{
    JS_ASSERT(hasBlockChain());

    if (cx->compartment->debugMode())
        DebugScopes::onPopBlock(cx, this);

    if (blockChain_->needsClone()) {
        JS_ASSERT(scopeChain_->asClonedBlock().staticBlock() == *blockChain_);
        popOffScopeChain();
    }

    setBlockChain(*blockChain_->enclosingBlock());
}

inline CallObject &
BaselineFrame::callObj() const
{
    JS_ASSERT(hasCallObj());
    JS_ASSERT(fun()->isHeavyweight());

    JSObject *obj = scopeChain();
    while (!obj->isCall())
        obj = obj->enclosingScope();
    return obj->asCall();
}

} // namespace ion
} // namespace js

#endif

