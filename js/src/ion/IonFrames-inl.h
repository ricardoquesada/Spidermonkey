/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsion_frames_inl_h__
#define jsion_frames_inl_h__

#include "ion/IonFrames.h"
#include "ion/IonFrameIterator.h"
#include "ion/LIR.h"

namespace js {
namespace ion {

inline void
SafepointIndex::resolve()
{
    JS_ASSERT(!resolved);
    safepointOffset_ = safepoint_->offset();
    resolved = true;
}

static inline size_t
SizeOfFramePrefix(FrameType type)
{
    switch (type) {
      case IonFrame_Entry:
        return IonEntryFrameLayout::Size();
      case IonFrame_OptimizedJS:
      case IonFrame_Unwound_OptimizedJS:
        return IonJSFrameLayout::Size();
      case IonFrame_Rectifier:
        return IonRectifierFrameLayout::Size();
      case IonFrame_Unwound_Rectifier:
        return IonUnwoundRectifierFrameLayout::Size();
      case IonFrame_Exit:
        return IonExitFrameLayout::Size();
      case IonFrame_Osr:
        return IonOsrFrameLayout::Size();
      default:
        JS_NOT_REACHED("unknown frame type");
    }
    return 0;
}

inline IonCommonFrameLayout *
IonFrameIterator::current() const
{
    return (IonCommonFrameLayout *)current_;
}

inline uint8_t *
IonFrameIterator::returnAddress() const
{
    IonCommonFrameLayout *current = (IonCommonFrameLayout *) current_;
    return current->returnAddress();
}

inline size_t
IonFrameIterator::prevFrameLocalSize() const
{
    IonCommonFrameLayout *current = (IonCommonFrameLayout *) current_;
    return current->prevFrameLocalSize();
}

inline FrameType
IonFrameIterator::prevType() const
{
    IonCommonFrameLayout *current = (IonCommonFrameLayout *) current_;
    return current->prevType();
}

size_t
IonFrameIterator::frameSize() const
{
    JS_ASSERT(type_ != IonFrame_Exit);
    return frameSize_;
}

// Returns the JSScript associated with the topmost Ion frame.
inline RawScript
GetTopIonJSScript(JSContext *cx, const SafepointIndex **safepointIndexOut, void **returnAddrOut)
{
    IonFrameIterator iter(cx->mainThread().ionTop);
    JS_ASSERT(iter.type() == IonFrame_Exit);
    ++iter;

    // If needed, grab the safepoint index.
    if (safepointIndexOut)
        *safepointIndexOut = iter.safepoint();

    JS_ASSERT(iter.returnAddressToFp() != NULL);
    if (returnAddrOut)
        *returnAddrOut = (void *) iter.returnAddressToFp();

    JS_ASSERT(iter.isScripted());
    return iter.script();
}

} // namespace ion
} // namespace js

#endif // jsion_frames_inl_h__

