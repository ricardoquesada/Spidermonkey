/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef FoldConstants_h__
#define FoldConstants_h__

#include "jsprvtd.h"

namespace js {
namespace frontend {

// Perform constant folding on the given AST. For example, the program
// `print(2 + 2)` would become `print(4)`.
//
// pnp is the address of a pointer variable that points to the root node of the
// AST. On success, *pnp points to the root node of the new tree, which may be
// the same node (unchanged or modified in place) or a new node.
//
// Usage:
//    pn = parser->statement();
//    if (!pn)
//        return false;
//    if (!FoldConstants(cx, &pn, parser))
//        return false;
template <typename ParseHandler>
bool
FoldConstants(JSContext *cx, typename ParseHandler::Node *pnp,
              Parser<ParseHandler> *parser,
              bool inGenexpLambda = false, bool inCond = false);

} /* namespace frontend */
} /* namespace js */

#endif /* FoldConstants_h__ */
