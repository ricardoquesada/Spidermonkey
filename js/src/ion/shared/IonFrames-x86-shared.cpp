/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ion/Ion.h"
#include "ion/IonFrames.h"

#include "jsscriptinlines.h"

using namespace js;
using namespace js::ion;

IonJSFrameLayout *
InvalidationBailoutStack::fp() const
{
    return (IonJSFrameLayout *) (sp() + ionScript_->frameSize());
}

void
InvalidationBailoutStack::checkInvariants() const
{
#ifdef DEBUG
    IonJSFrameLayout *frame = fp();
    CalleeToken token = frame->calleeToken();
    JS_ASSERT(token);

    uint8_t *rawBase = ionScript()->method()->raw();
    uint8_t *rawLimit = rawBase + ionScript()->method()->instructionsSize();
    uint8_t *osiPoint = osiPointReturnAddress();
    JS_ASSERT(rawBase <= osiPoint && osiPoint <= rawLimit);
#endif
}
