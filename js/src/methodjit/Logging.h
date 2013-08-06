/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined jsjaeger_logging_h__
#define jsjaeger_logging_h__

#include "assembler/wtf/Platform.h"
#include "prmjtime.h"

namespace js {

#define JSPEW_CHAN_MAP(_)   \
    _(Abort)                \
    _(Scripts)              \
    _(Prof)                 \
    _(JSOps)                \
    _(Insns)                \
    _(VMFrame)              \
    _(PICs)                 \
    _(SlowCalls)            \
    _(Analysis)             \
    _(Regalloc)             \
    _(Inlining)             \
    _(Recompile)

enum JaegerSpewChannel {
#define _(name) JSpew_##name,
    JSPEW_CHAN_MAP(_)
#undef  _
    JSpew_Terminator
};

#ifdef JS_METHODJIT_SPEW

void JMCheckLogging();
bool IsJaegerSpewChannelActive(JaegerSpewChannel channel);

#ifdef __GNUC__
void JaegerSpew(JaegerSpewChannel channel, const char *fmt, ...) __attribute__ ((format (printf, 2, 3)));
#else
void JaegerSpew(JaegerSpewChannel channel, const char *fmt, ...);
#endif

#else

static inline void JMCheckLogging() {}
static inline bool IsJaegerSpewChannelActive(JaegerSpewChannel channel) { return false; }
static inline void JaegerSpew(JaegerSpewChannel channel, const char *fmt, ...) {}

#endif // JS_METHODJIT_SPEW

#if defined(JS_METHODJIT_SPEW)

struct ConditionalLog {
    uint32_t oldBits;
    bool logging;
    ConditionalLog(bool logging);
    ~ConditionalLog();
};

struct Profiler {
    int64_t t_start;
    int64_t t_stop;

    static inline int64_t now() {
        return PRMJ_Now();
    }

    inline void start() {
        t_start = now();
    }

    inline void stop() {
        t_stop = now();
    }

    inline uint32_t time_ms() {
        return uint32_t((t_stop - t_start) / PRMJ_USEC_PER_MSEC);
    }

    inline uint32_t time_us() {
        return uint32_t(t_stop - t_start);
    }
};

#endif // JS_METHODJIT_SPEW

} // namespace js

#endif
