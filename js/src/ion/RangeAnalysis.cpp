/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/MathAlgorithms.h"

#include <math.h>
#include <stdio.h>

#include "vm/NumericConversions.h"

#include "Ion.h"
#include "IonAnalysis.h"
#include "MIR.h"
#include "MIRGraph.h"
#include "RangeAnalysis.h"
#include "IonSpewer.h"

using namespace js;
using namespace js::ion;

using mozilla::Abs;
using mozilla::ExponentComponent;
using mozilla::IsInfinite;
using mozilla::IsNaN;
using mozilla::IsNegative;

// This algorithm is based on the paper "Eliminating Range Checks Using
// Static Single Assignment Form" by Gough and Klaren.
//
// We associate a range object with each SSA name, and the ranges are consulted
// in order to determine whether overflow is possible for arithmetic
// computations.
//
// An important source of range information that requires care to take
// advantage of is conditional control flow. Consider the code below:
//
// if (x < 0) {
//   y = x + 2000000000;
// } else {
//   if (x < 1000000000) {
//     y = x * 2;
//   } else {
//     y = x - 3000000000;
//   }
// }
//
// The arithmetic operations in this code cannot overflow, but it is not
// sufficient to simply associate each name with a range, since the information
// differs between basic blocks. The traditional dataflow approach would be
// associate ranges with (name, basic block) pairs. This solution is not
// satisfying, since we lose the benefit of SSA form: in SSA form, each
// definition has a unique name, so there is no need to track information about
// the control flow of the program.
//
// The approach used here is to add a new form of pseudo operation called a
// beta node, which associates range information with a value. These beta
// instructions take one argument and additionally have an auxiliary constant
// range associated with them. Operationally, beta nodes are just copies, but
// the invariant expressed by beta node copies is that the output will fall
// inside the range given by the beta node.  Gough and Klaeren refer to SSA
// extended with these beta nodes as XSA form. The following shows the example
// code transformed into XSA form:
//
// if (x < 0) {
//   x1 = Beta(x, [INT_MIN, -1]);
//   y1 = x1 + 2000000000;
// } else {
//   x2 = Beta(x, [0, INT_MAX]);
//   if (x2 < 1000000000) {
//     x3 = Beta(x2, [INT_MIN, 999999999]);
//     y2 = x3*2;
//   } else {
//     x4 = Beta(x2, [1000000000, INT_MAX]);
//     y3 = x4 - 3000000000;
//   }
//   y4 = Phi(y2, y3);
// }
// y = Phi(y1, y4);
//
// We insert beta nodes for the purposes of range analysis (they might also be
// usefully used for other forms of bounds check elimination) and remove them
// after range analysis is performed. The remaining compiler phases do not ever
// encounter beta nodes.

RangeAnalysis::RangeAnalysis(MIRGraph &graph)
  : graph_(graph)
{
}

static bool
IsDominatedUse(MBasicBlock *block, MUse *use)
{
    MNode *n = use->consumer();
    bool isPhi = n->isDefinition() && n->toDefinition()->isPhi();

    if (isPhi)
        return block->dominates(n->block()->getPredecessor(use->index()));

    return block->dominates(n->block());
}

static inline void
SpewRange(MDefinition *def)
{
#ifdef DEBUG
    if (IonSpewEnabled(IonSpew_Range) && def->range()) {
        Sprinter sp(GetIonContext()->cx);
        sp.init();
        def->range()->print(sp);
        IonSpew(IonSpew_Range, "%d has range %s", def->id(), sp.string());
    }
#endif
}

void
RangeAnalysis::replaceDominatedUsesWith(MDefinition *orig, MDefinition *dom,
                                            MBasicBlock *block)
{
    for (MUseIterator i(orig->usesBegin()); i != orig->usesEnd(); ) {
        if (i->consumer() != dom && IsDominatedUse(block, *i))
            i = i->consumer()->replaceOperand(i, dom);
        else
            i++;
    }
}

bool
RangeAnalysis::addBetaNobes()
{
    IonSpew(IonSpew_Range, "Adding beta nobes");

    for (PostorderIterator i(graph_.poBegin()); i != graph_.poEnd(); i++) {
        MBasicBlock *block = *i;
        IonSpew(IonSpew_Range, "Looking at block %d", block->id());

        BranchDirection branch_dir;
        MTest *test = block->immediateDominatorBranch(&branch_dir);

        if (!test || !test->getOperand(0)->isCompare())
            continue;

        MCompare *compare = test->getOperand(0)->toCompare();

        // TODO: support unsigned comparisons
        if (compare->compareType() == MCompare::Compare_UInt32)
            continue;

        MDefinition *left = compare->getOperand(0);
        MDefinition *right = compare->getOperand(1);
        int32_t bound;
        MDefinition *val = NULL;

        JSOp jsop = compare->jsop();

        if (branch_dir == FALSE_BRANCH)
            jsop = analyze::NegateCompareOp(jsop);

        if (left->isConstant() && left->toConstant()->value().isInt32()) {
            bound = left->toConstant()->value().toInt32();
            val = right;
            jsop = analyze::ReverseCompareOp(jsop);
        } else if (right->isConstant() && right->toConstant()->value().isInt32()) {
            bound = right->toConstant()->value().toInt32();
            val = left;
        } else {
            MDefinition *smaller = NULL;
            MDefinition *greater = NULL;
            if (jsop == JSOP_LT) {
                smaller = left;
                greater = right;
            } else if (jsop == JSOP_GT) {
                smaller = right;
                greater = left;
            }
            if (smaller && greater) {
                MBeta *beta;
                beta = MBeta::New(smaller, new Range(JSVAL_INT_MIN, JSVAL_INT_MAX-1,
                                                     smaller->type() != MIRType_Int32,
                                                     Range::MaxDoubleExponent));
                block->insertBefore(*block->begin(), beta);
                replaceDominatedUsesWith(smaller, beta, block);
                IonSpew(IonSpew_Range, "Adding beta node for smaller %d", smaller->id());
                beta = MBeta::New(greater, new Range(JSVAL_INT_MIN+1, JSVAL_INT_MAX,
                                                     greater->type() != MIRType_Int32,
                                                     Range::MaxDoubleExponent));
                block->insertBefore(*block->begin(), beta);
                replaceDominatedUsesWith(greater, beta, block);
                IonSpew(IonSpew_Range, "Adding beta node for greater %d", greater->id());
            }
            continue;
        }

        JS_ASSERT(val);


        Range comp;
        if (val->type() == MIRType_Int32)
            comp.setInt32();
        switch (jsop) {
          case JSOP_LE:
            comp.setUpper(bound);
            break;
          case JSOP_LT:
            if (!SafeSub(bound, 1, &bound))
                break;
            comp.setUpper(bound);
            break;
          case JSOP_GE:
            comp.setLower(bound);
            break;
          case JSOP_GT:
            if (!SafeAdd(bound, 1, &bound))
                break;
            comp.setLower(bound);
            break;
          case JSOP_EQ:
            comp.setLower(bound);
            comp.setUpper(bound);
          default:
            break; // well, for neq we could have
                   // [-\inf, bound-1] U [bound+1, \inf] but we only use contiguous ranges.
        }

        IonSpew(IonSpew_Range, "Adding beta node for %d", val->id());
        MBeta *beta = MBeta::New(val, new Range(comp));
        block->insertBefore(*block->begin(), beta);
        replaceDominatedUsesWith(val, beta, block);
    }

    return true;
}

bool
RangeAnalysis::removeBetaNobes()
{
    IonSpew(IonSpew_Range, "Removing beta nobes");

    for (PostorderIterator i(graph_.poBegin()); i != graph_.poEnd(); i++) {
        MBasicBlock *block = *i;
        for (MDefinitionIterator iter(*i); iter; ) {
            MDefinition *def = *iter;
            if (def->isBeta()) {
                MDefinition *op = def->getOperand(0);
                IonSpew(IonSpew_Range, "Removing beta node %d for %d",
                        def->id(), op->id());
                def->replaceAllUsesWith(op);
                iter = block->discardDefAt(iter);
            } else {
                // We only place Beta nodes at the beginning of basic
                // blocks, so if we see something else, we can move on
                // to the next block.
                break;
            }
        }
    }
    return true;
}

void
SymbolicBound::print(Sprinter &sp) const
{
    if (loop)
        sp.printf("[loop] ");
    sum.print(sp);
}

void
Range::print(Sprinter &sp) const
{
    JS_ASSERT_IF(lower_infinite_, lower_ == JSVAL_INT_MIN);
    JS_ASSERT_IF(upper_infinite_, upper_ == JSVAL_INT_MAX);

    // Real or Natural subset.
    if (decimal_)
        sp.printf("R");
    else
        sp.printf("N");

    sp.printf("[");

    if (lower_infinite_)
        sp.printf("-inf");
    else
        sp.printf("%d", lower_);
    if (symbolicLower_) {
        sp.printf(" {");
        symbolicLower_->print(sp);
        sp.printf("}");
    }

    sp.printf(", ");

    if (upper_infinite_)
        sp.printf("inf");
    else
        sp.printf("%d", upper_);
    if (symbolicUpper_) {
        sp.printf(" {");
        symbolicUpper_->print(sp);
        sp.printf("}");
    }

    sp.printf("]");
    sp.printf(" (%db)", numBits());
}

Range *
Range::intersect(const Range *lhs, const Range *rhs, bool *emptyRange)
{
    *emptyRange = false;

    if (!lhs && !rhs)
        return NULL;

    if (!lhs)
        return new Range(*rhs);
    if (!rhs)
        return new Range(*lhs);

    Range *r = new Range(
        Max(lhs->lower_, rhs->lower_),
        Min(lhs->upper_, rhs->upper_),
        lhs->decimal_ && rhs->decimal_,
        Min(lhs->max_exponent_, rhs->max_exponent_));

    r->lower_infinite_ = lhs->lower_infinite_ && rhs->lower_infinite_;
    r->upper_infinite_ = lhs->upper_infinite_ && rhs->upper_infinite_;

    // :TODO: This information could be used better. If upper < lower, then we
    // have conflicting constraints. Consider:
    //
    // if (x < 0) {
    //   if (x > 0) {
    //     [Some code.]
    //   }
    // }
    //
    // In this case, the block is dead. Right now, we just disregard this fact
    // and make the range infinite, rather than empty.
    //
    // Instead, we should use it to eliminate the dead block.
    // (Bug 765127)
    if (r->upper_ < r->lower_) {
        *emptyRange = true;
        r->makeRangeInfinite();
    }

    return r;
}

void
Range::unionWith(const Range *other)
{
   lower_infinite_ |= other->lower_infinite_;
   upper_infinite_ |= other->upper_infinite_;
   decimal_ |= other->decimal_;
   max_exponent_ = Max(max_exponent_, other->max_exponent_);
   setLower(Min(lower_, other->lower_));
   setUpper(Max(upper_, other->upper_));
}

static const Range emptyRange;

Range::Range(const MDefinition *def)
  : symbolicLower_(NULL),
    symbolicUpper_(NULL)
{
    const Range *other = def->range();
    if (!other)
        other = &emptyRange;

    lower_ = other->lower_;
    lower_infinite_ = other->lower_infinite_;
    upper_ = other->upper_;
    upper_infinite_ = other->upper_infinite_;
    decimal_ = other->decimal_;
    max_exponent_ = other->max_exponent_;

    if (def->type() == MIRType_Int32)
        truncate();
}

const int64_t RANGE_INF_MAX = (int64_t) JSVAL_INT_MAX + 1;
const int64_t RANGE_INF_MIN = (int64_t) JSVAL_INT_MIN - 1;

static inline bool
HasInfinite(const Range *lhs, const Range *rhs)
{
    return lhs->isLowerInfinite() || lhs->isUpperInfinite() ||
           rhs->isLowerInfinite() || rhs->isUpperInfinite();
}

Range *
Range::add(const Range *lhs, const Range *rhs)
{
    int64_t l = (int64_t) lhs->lower_ + (int64_t) rhs->lower_;
    if (lhs->isLowerInfinite() || rhs->isLowerInfinite())
        l = RANGE_INF_MIN;

    int64_t h = (int64_t) lhs->upper_ + (int64_t) rhs->upper_;
    if (lhs->isUpperInfinite() || rhs->isUpperInfinite())
        h = RANGE_INF_MAX;

    return new Range(l, h, lhs->isDecimal() || rhs->isDecimal(),
                     Max(lhs->exponent(), rhs->exponent()) + 1);
}

Range *
Range::sub(const Range *lhs, const Range *rhs)
{
    int64_t l = (int64_t) lhs->lower_ - (int64_t) rhs->upper_;
    if (lhs->isLowerInfinite() || rhs->isUpperInfinite())
        l = RANGE_INF_MIN;

    int64_t h = (int64_t) lhs->upper_ - (int64_t) rhs->lower_;
    if (lhs->isUpperInfinite() || rhs->isLowerInfinite())
        h = RANGE_INF_MAX;

    return new Range(l, h, lhs->isDecimal() || rhs->isDecimal(),
                     Max(lhs->exponent(), rhs->exponent()) + 1);
}

Range *
Range::and_(const Range *lhs, const Range *rhs)
{
    int64_t lower;
    int64_t upper;

    // If both numbers can be negative, result can be negative in the whole range
    if (lhs->lower_ < 0 && rhs->lower_ < 0) {
        lower = INT_MIN;
        upper = Max(lhs->upper_, rhs->upper_);
        return new Range(lower, upper);
    }

    // Only one of both numbers can be negative.
    // - result can't be negative
    // - Upper bound is minimum of both upper range,
    lower = 0;
    upper = Min(lhs->upper_, rhs->upper_);

    // EXCEPT when upper bound of non negative number is max value,
    // because negative value can return the whole max value.
    // -1 & 5 = 5
    if (lhs->lower_ < 0)
       upper = rhs->upper_;
    if (rhs->lower_ < 0)
        upper = lhs->upper_;

    return new Range(lower, upper);
}

Range *
Range::mul(const Range *lhs, const Range *rhs)
{
    bool decimal = lhs->isDecimal() || rhs->isDecimal();
    uint16_t exponent = lhs->numBits() + rhs->numBits() - 1;
    if (HasInfinite(lhs, rhs))
        return new Range(RANGE_INF_MIN, RANGE_INF_MAX, decimal, exponent);
    int64_t a = (int64_t)lhs->lower_ * (int64_t)rhs->lower_;
    int64_t b = (int64_t)lhs->lower_ * (int64_t)rhs->upper_;
    int64_t c = (int64_t)lhs->upper_ * (int64_t)rhs->lower_;
    int64_t d = (int64_t)lhs->upper_ * (int64_t)rhs->upper_;
    return new Range(
        Min( Min(a, b), Min(c, d) ),
        Max( Max(a, b), Max(c, d) ),
        decimal, exponent);
}

Range *
Range::shl(const Range *lhs, int32_t c)
{
    int32_t shift = c & 0x1f;
    return new Range(
        (int64_t)lhs->lower_ << shift,
        (int64_t)lhs->upper_ << shift);
}

Range *
Range::shr(const Range *lhs, int32_t c)
{
    int32_t shift = c & 0x1f;
    return new Range(
        (int64_t)lhs->lower_ >> shift,
        (int64_t)lhs->upper_ >> shift);
}

bool
Range::negativeZeroMul(const Range *lhs, const Range *rhs)
{
    // Both values are positive
    if (lhs->lower_ >= 0 && rhs->lower_ >= 0)
        return false;

    // Both values are negative (non zero)
    if (lhs->upper_ < 0 && rhs->upper_ < 0)
        return false;

    // One operand is positive (non zero)
    if (lhs->lower_ > 0 || rhs->lower_ > 0)
        return false;

    return true;
}

bool
Range::update(const Range *other)
{
    bool changed =
        lower_ != other->lower_ ||
        lower_infinite_ != other->lower_infinite_ ||
        upper_ != other->upper_ ||
        upper_infinite_ != other->upper_infinite_ ||
        decimal_ != other->decimal_ ||
        max_exponent_ != other->max_exponent_;
    if (changed) {
        lower_ = other->lower_;
        lower_infinite_ = other->lower_infinite_;
        upper_ = other->upper_;
        upper_infinite_ = other->upper_infinite_;
        decimal_ = other->decimal_;
        max_exponent_ = other->max_exponent_;
    }

    return changed;
}

///////////////////////////////////////////////////////////////////////////////
// Range Computation for MIR Nodes
///////////////////////////////////////////////////////////////////////////////

void
MPhi::computeRange()
{
    if (type() != MIRType_Int32 && type() != MIRType_Double)
        return;

    Range *range = NULL;
    JS_ASSERT(getOperand(0)->op() != MDefinition::Op_OsrValue);
    for (size_t i = 0; i < numOperands(); i++) {
        if (getOperand(i)->block()->earlyAbort()) {
            IonSpew(IonSpew_Range, "Ignoring unreachable input %d", getOperand(i)->id());
            continue;
        }

        if (isOSRLikeValue(getOperand(i)))
            continue;

        Range *input = getOperand(i)->range();

        if (!input) {
            range = NULL;
            break;
        }

        if (range)
            range->unionWith(input);
        else
            range = new Range(*input);
    }

    setRange(range);

    if (block()->isLoopHeader()) {
    }
}

void
MConstant::computeRange()
{
    if (type() == MIRType_Int32) {
        setRange(new Range(value().toInt32(), value().toInt32()));
        return;
    }

    if (type() != MIRType_Double)
        return;

    double d = value().toDouble();
    int exp = Range::MaxDoubleExponent;

    // NaN is estimated as a Double which covers everything.
    if (IsNaN(d)) {
        setRange(new Range(RANGE_INF_MIN, RANGE_INF_MAX, true, exp));
        return;
    }

    // Infinity is used to set both lower and upper to the range boundaries.
    if (IsInfinite(d)) {
        if (IsNegative(d))
            setRange(new Range(RANGE_INF_MIN, RANGE_INF_MIN, false, exp));
        else
            setRange(new Range(RANGE_INF_MAX, RANGE_INF_MAX, false, exp));
        return;
    }

    // Extract the exponent, to approximate it with the range analysis.
    exp = ExponentComponent(d);
    if (exp < 0) {
        // This double only has a decimal part.
        if (IsNegative(d))
            setRange(new Range(-1, 0, true, 0));
        else
            setRange(new Range(0, 1, true, 0));
    } else if (exp < Range::MaxTruncatableExponent) {
        // Extract the integral part.
        int64_t integral = ToInt64(d);
        // Extract the decimal part.
        double rest = d - (double) integral;
        // Estimate the smallest integral boundaries.
        //   Safe double comparisons, because there is no precision loss.
        int64_t l = integral - ((rest < 0) ? 1 : 0);
        int64_t h = integral + ((rest > 0) ? 1 : 0);
        setRange(new Range(l, h, (rest != 0), exp));
    } else {
        // This double has a precision loss. This also mean that it cannot
        // encode any decimals.
        if (IsNegative(d))
            setRange(new Range(RANGE_INF_MIN, RANGE_INF_MIN, false, exp));
        else
            setRange(new Range(RANGE_INF_MAX, RANGE_INF_MAX, false, exp));
    }
}

void
MCharCodeAt::computeRange()
{
    setRange(new Range(0, 65535)); //ECMA 262 says that the integer will be
                                   //non-negative and at most 65535.
}

void
MClampToUint8::computeRange()
{
    setRange(new Range(0, 255));
}

void
MBitAnd::computeRange()
{
    Range left(getOperand(0));
    Range right(getOperand(1));
    setRange(Range::and_(&left, &right));
}

void
MLsh::computeRange()
{
    MDefinition *right = getOperand(1);
    if (!right->isConstant())
        return;

    int32_t c = right->toConstant()->value().toInt32();
    Range other(getOperand(0));
    setRange(Range::shl(&other, c));
}

void
MRsh::computeRange()
{
    MDefinition *right = getOperand(1);
    if (!right->isConstant())
        return;

    int32_t c = right->toConstant()->value().toInt32();
    Range other(getOperand(0));
    setRange(Range::shr(&other, c));
}

void
MAbs::computeRange()
{
    if (specialization_ != MIRType_Int32 && specialization_ != MIRType_Double)
        return;

    Range other(getOperand(0));

    Range *range = new Range(0,
                             Max(Abs<int64_t>(other.lower()), Abs<int64_t>(other.upper())),
                             other.isDecimal(),
                             other.exponent());
    setRange(range);
}

void
MAdd::computeRange()
{
    if (specialization() != MIRType_Int32 && specialization() != MIRType_Double)
        return;
    Range left(getOperand(0));
    Range right(getOperand(1));
    Range *next = Range::add(&left, &right);
    setRange(next);
}

void
MSub::computeRange()
{
    if (specialization() != MIRType_Int32 && specialization() != MIRType_Double)
        return;
    Range left(getOperand(0));
    Range right(getOperand(1));
    Range *next = Range::sub(&left, &right);
    setRange(next);
}

void
MMul::computeRange()
{
    if ((specialization() != MIRType_Int32 && specialization() != MIRType_Double) || isTruncated())
        return;
    Range left(getOperand(0));
    Range right(getOperand(1));
    if (canBeNegativeZero())
        canBeNegativeZero_ = Range::negativeZeroMul(&left, &right);
    setRange(Range::mul(&left, &right));
}

void
MMod::computeRange()
{
    if (specialization() != MIRType_Int32 && specialization() != MIRType_Double)
        return;
    Range lhs(getOperand(0));
    Range rhs(getOperand(1));
    int64_t a = Abs<int64_t>(rhs.lower());
    int64_t b = Abs<int64_t>(rhs.upper());
    if (a == 0 && b == 0)
        return;
    int64_t bound = Max(1-a, b-1);
    setRange(new Range(-bound, bound, lhs.isDecimal() || rhs.isDecimal()));
}

void
MToDouble::computeRange()
{
    setRange(new Range(getOperand(0)));
}

void
MTruncateToInt32::computeRange()
{
    Range input(getOperand(0));
    setRange(new Range(input.lower(), input.upper()));
}

void
MToInt32::computeRange()
{
    Range input(getOperand(0));
    setRange(new Range(input.lower(), input.upper()));
}

void
MLoadTypedArrayElementStatic::computeRange()
{
    setRange(new Range(this));
}

///////////////////////////////////////////////////////////////////////////////
// Range Analysis
///////////////////////////////////////////////////////////////////////////////

void
RangeAnalysis::markBlocksInLoopBody(MBasicBlock *header, MBasicBlock *current)
{
    // Visited.
    current->mark();

    // If we haven't reached the loop header yet, recursively explore predecessors
    // if we haven't seen them already.
    if (current != header) {
        for (size_t i = 0; i < current->numPredecessors(); i++) {
            if (current->getPredecessor(i)->isMarked())
                continue;
            markBlocksInLoopBody(header, current->getPredecessor(i));
        }
    }
}

void
RangeAnalysis::analyzeLoop(MBasicBlock *header)
{
    // Try to compute an upper bound on the number of times the loop backedge
    // will be taken. Look for tests that dominate the backedge and which have
    // an edge leaving the loop body.
    MBasicBlock *backedge = header->backedge();

    // Ignore trivial infinite loops.
    if (backedge == header)
        return;

    markBlocksInLoopBody(header, backedge);

    LoopIterationBound *iterationBound = NULL;

    MBasicBlock *block = backedge;
    do {
        BranchDirection direction;
        MTest *branch = block->immediateDominatorBranch(&direction);

        if (block == block->immediateDominator())
            break;

        block = block->immediateDominator();

        if (branch) {
            direction = NegateBranchDirection(direction);
            MBasicBlock *otherBlock = branch->branchSuccessor(direction);
            if (!otherBlock->isMarked()) {
                iterationBound =
                    analyzeLoopIterationCount(header, branch, direction);
                if (iterationBound)
                    break;
            }
        }
    } while (block != header);

    if (!iterationBound) {
        graph_.unmarkBlocks();
        return;
    }

#ifdef DEBUG
    if (IonSpewEnabled(IonSpew_Range)) {
        Sprinter sp(GetIonContext()->cx);
        sp.init();
        iterationBound->sum.print(sp);
        IonSpew(IonSpew_Range, "computed symbolic bound on backedges: %s",
                sp.string());
    }
#endif

    // Try to compute symbolic bounds for the phi nodes at the head of this
    // loop, expressed in terms of the iteration bound just computed.

    for (MDefinitionIterator iter(header); iter; iter++) {
        MDefinition *def = *iter;
        if (def->isPhi())
            analyzeLoopPhi(header, iterationBound, def->toPhi());
    }

    // Try to hoist any bounds checks from the loop using symbolic bounds.

    Vector<MBoundsCheck *, 0, IonAllocPolicy> hoistedChecks;

    for (ReversePostorderIterator iter(graph_.rpoBegin()); iter != graph_.rpoEnd(); iter++) {
        MBasicBlock *block = *iter;
        if (!block->isMarked())
            continue;

        for (MDefinitionIterator iter(block); iter; iter++) {
            MDefinition *def = *iter;
            if (def->isBoundsCheck() && def->isMovable()) {
                if (tryHoistBoundsCheck(header, def->toBoundsCheck()))
                    hoistedChecks.append(def->toBoundsCheck());
            }
        }
    }

    // Note: replace all uses of the original bounds check with the
    // actual index. This is usually done during bounds check elimination,
    // but in this case it's safe to do it here since the load/store is
    // definitely not loop-invariant, so we will never move it before
    // one of the bounds checks we just added.
    for (size_t i = 0; i < hoistedChecks.length(); i++) {
        MBoundsCheck *ins = hoistedChecks[i];
        ins->replaceAllUsesWith(ins->index());
        ins->block()->discard(ins);
    }

    graph_.unmarkBlocks();
}

LoopIterationBound *
RangeAnalysis::analyzeLoopIterationCount(MBasicBlock *header,
                                         MTest *test, BranchDirection direction)
{
    SimpleLinearSum lhs(NULL, 0);
    MDefinition *rhs;
    bool lessEqual;
    if (!ExtractLinearInequality(test, direction, &lhs, &rhs, &lessEqual))
        return NULL;

    // Ensure the rhs is a loop invariant term.
    if (rhs && rhs->block()->isMarked()) {
        if (lhs.term && lhs.term->block()->isMarked())
            return NULL;
        MDefinition *temp = lhs.term;
        lhs.term = rhs;
        rhs = temp;
        if (!SafeSub(0, lhs.constant, &lhs.constant))
            return NULL;
        lessEqual = !lessEqual;
    }

    JS_ASSERT_IF(rhs, !rhs->block()->isMarked());

    // Ensure the lhs is a phi node from the start of the loop body.
    if (!lhs.term || !lhs.term->isPhi() || lhs.term->block() != header)
        return NULL;

    // Check that the value of the lhs changes by a constant amount with each
    // loop iteration. This requires that the lhs be written in every loop
    // iteration with a value that is a constant difference from its value at
    // the start of the iteration.

    if (lhs.term->toPhi()->numOperands() != 2)
        return NULL;

    // The first operand of the phi should be the lhs' value at the start of
    // the first executed iteration, and not a value written which could
    // replace the second operand below during the middle of execution.
    MDefinition *lhsInitial = lhs.term->toPhi()->getOperand(0);
    if (lhsInitial->block()->isMarked())
        return NULL;

    // The second operand of the phi should be a value written by an add/sub
    // in every loop iteration, i.e. in a block which dominates the backedge.
    MDefinition *lhsWrite = lhs.term->toPhi()->getOperand(1);
    if (lhsWrite->isBeta())
        lhsWrite = lhsWrite->getOperand(0);
    if (!lhsWrite->isAdd() && !lhsWrite->isSub())
        return NULL;
    if (!lhsWrite->block()->isMarked())
        return NULL;
    MBasicBlock *bb = header->backedge();
    for (; bb != lhsWrite->block() && bb != header; bb = bb->immediateDominator()) {}
    if (bb != lhsWrite->block())
        return NULL;

    SimpleLinearSum lhsModified = ExtractLinearSum(lhsWrite);

    // Check that the value of the lhs at the backedge is of the form
    // 'old(lhs) + N'. We can be sure that old(lhs) is the value at the start
    // of the iteration, and not that written to lhs in a previous iteration,
    // as such a previous value could not appear directly in the addition:
    // it could not be stored in lhs as the lhs add/sub executes in every
    // iteration, and if it were stored in another variable its use here would
    // be as an operand to a phi node for that variable.
    if (lhsModified.term != lhs.term)
        return NULL;

    LinearSum bound;

    if (lhsModified.constant == 1 && !lessEqual) {
        // The value of lhs is 'initial(lhs) + iterCount' and this will end
        // execution of the loop if 'lhs + lhsN >= rhs'. Thus, an upper bound
        // on the number of backedges executed is:
        //
        // initial(lhs) + iterCount + lhsN == rhs
        // iterCount == rhsN - initial(lhs) - lhsN

        if (rhs) {
            if (!bound.add(rhs, 1))
                return NULL;
        }
        if (!bound.add(lhsInitial, -1))
            return NULL;

        int32_t lhsConstant;
        if (!SafeSub(0, lhs.constant, &lhsConstant))
            return NULL;
        if (!bound.add(lhsConstant))
            return NULL;
    } else if (lhsModified.constant == -1 && lessEqual) {
        // The value of lhs is 'initial(lhs) - iterCount'. Similar to the above
        // case, an upper bound on the number of backedges executed is:
        //
        // initial(lhs) - iterCount + lhsN == rhs
        // iterCount == initial(lhs) - rhs + lhsN

        if (!bound.add(lhsInitial, 1))
            return NULL;
        if (rhs) {
            if (!bound.add(rhs, -1))
                return NULL;
        }
        if (!bound.add(lhs.constant))
            return NULL;
    } else {
        return NULL;
    }

    return new LoopIterationBound(header, test, bound);
}

void
RangeAnalysis::analyzeLoopPhi(MBasicBlock *header, LoopIterationBound *loopBound, MPhi *phi)
{
    // Given a bound on the number of backedges taken, compute an upper and
    // lower bound for a phi node that may change by a constant amount each
    // iteration. Unlike for the case when computing the iteration bound
    // itself, the phi does not need to change the same amount every iteration,
    // but is required to change at most N and be either nondecreasing or
    // nonincreasing.

    if (phi->numOperands() != 2)
        return;

    MBasicBlock *preLoop = header->loopPredecessor();
    JS_ASSERT(!preLoop->isMarked() && preLoop->successorWithPhis() == header);

    MBasicBlock *backedge = header->backedge();
    JS_ASSERT(backedge->isMarked() && backedge->successorWithPhis() == header);

    MDefinition *initial = phi->getOperand(preLoop->positionInPhiSuccessor());
    if (initial->block()->isMarked())
        return;

    SimpleLinearSum modified = ExtractLinearSum(phi->getOperand(backedge->positionInPhiSuccessor()));

    if (modified.term != phi || modified.constant == 0)
        return;

    if (!phi->range())
        phi->setRange(new Range());

    LinearSum initialSum;
    if (!initialSum.add(initial, 1))
        return;

    // The phi may change by N each iteration, and is either nondecreasing or
    // nonincreasing. initial(phi) is either a lower or upper bound for the
    // phi, and initial(phi) + loopBound * N is either an upper or lower bound,
    // at all points within the loop, provided that loopBound >= 0.
    //
    // We are more interested, however, in the bound for phi at points
    // dominated by the loop bound's test; if the test dominates e.g. a bounds
    // check we want to hoist from the loop, using the value of the phi at the
    // head of the loop for this will usually be too imprecise to hoist the
    // check. These points will execute only if the backedge executes at least
    // one more time (as the test passed and the test dominates the backedge),
    // so we know both that loopBound >= 1 and that the phi's value has changed
    // at most loopBound - 1 times. Thus, another upper or lower bound for the
    // phi is initial(phi) + (loopBound - 1) * N, without requiring us to
    // ensure that loopBound >= 0.

    LinearSum limitSum(loopBound->sum);
    if (!limitSum.multiply(modified.constant) || !limitSum.add(initialSum))
        return;

    int32_t negativeConstant;
    if (!SafeSub(0, modified.constant, &negativeConstant) || !limitSum.add(negativeConstant))
        return;

    Range *initRange = initial->range();
    if (modified.constant > 0) {
        if (initRange && !initRange->isLowerInfinite())
            phi->range()->setLower(initRange->lower());
        phi->range()->setSymbolicLower(new SymbolicBound(NULL, initialSum));
        phi->range()->setSymbolicUpper(new SymbolicBound(loopBound, limitSum));
    } else {
        if (initRange && !initRange->isUpperInfinite())
            phi->range()->setUpper(initRange->upper());
        phi->range()->setSymbolicUpper(new SymbolicBound(NULL, initialSum));
        phi->range()->setSymbolicLower(new SymbolicBound(loopBound, limitSum));
    }

    IonSpew(IonSpew_Range, "added symbolic range on %d", phi->id());
    SpewRange(phi);
}

// Whether bound is valid at the specified bounds check instruction in a loop,
// and may be used to hoist ins.
static inline bool
SymbolicBoundIsValid(MBasicBlock *header, MBoundsCheck *ins, const SymbolicBound *bound)
{
    if (!bound->loop)
        return true;
    if (ins->block() == header)
        return false;
    MBasicBlock *bb = ins->block()->immediateDominator();
    while (bb != header && bb != bound->loop->test->block())
        bb = bb->immediateDominator();
    return bb == bound->loop->test->block();
}

// Convert all components of a linear sum *except* its constant to a definition,
// adding any necessary instructions to the end of block.
static inline MDefinition *
ConvertLinearSum(MBasicBlock *block, const LinearSum &sum)
{
    MDefinition *def = NULL;

    for (size_t i = 0; i < sum.numTerms(); i++) {
        LinearTerm term = sum.term(i);
        JS_ASSERT(!term.term->isConstant());
        if (term.scale == 1) {
            if (def) {
                def = MAdd::New(def, term.term);
                def->toAdd()->setInt32();
                block->insertBefore(block->lastIns(), def->toInstruction());
            } else {
                def = term.term;
            }
        } else {
            if (!def) {
                def = MConstant::New(Int32Value(0));
                block->insertBefore(block->lastIns(), def->toInstruction());
            }
            if (term.scale == -1) {
                def = MSub::New(def, term.term);
                def->toSub()->setInt32();
                block->insertBefore(block->lastIns(), def->toInstruction());
            } else {
                MConstant *factor = MConstant::New(Int32Value(term.scale));
                block->insertBefore(block->lastIns(), factor);
                MMul *mul = MMul::New(term.term, factor);
                mul->setInt32();
                block->insertBefore(block->lastIns(), mul);
                def = MAdd::New(def, mul);
                def->toAdd()->setInt32();
                block->insertBefore(block->lastIns(), def->toInstruction());
            }
        }
    }

    if (!def) {
        def = MConstant::New(Int32Value(0));
        block->insertBefore(block->lastIns(), def->toInstruction());
    }

    return def;
}

bool
RangeAnalysis::tryHoistBoundsCheck(MBasicBlock *header, MBoundsCheck *ins)
{
    // The bounds check's length must be loop invariant.
    if (ins->length()->block()->isMarked())
        return false;

    // The bounds check's index should not be loop invariant (else we would
    // already have hoisted it during LICM).
    SimpleLinearSum index = ExtractLinearSum(ins->index());
    if (!index.term || !index.term->block()->isMarked())
        return false;

    // Check for a symbolic lower and upper bound on the index. If either
    // condition depends on an iteration bound for the loop, only hoist if
    // the bounds check is dominated by the iteration bound's test.
    if (!index.term->range())
        return false;
    const SymbolicBound *lower = index.term->range()->symbolicLower();
    if (!lower || !SymbolicBoundIsValid(header, ins, lower))
        return false;
    const SymbolicBound *upper = index.term->range()->symbolicUpper();
    if (!upper || !SymbolicBoundIsValid(header, ins, upper))
        return false;

    MBasicBlock *preLoop = header->loopPredecessor();
    JS_ASSERT(!preLoop->isMarked());

    MDefinition *lowerTerm = ConvertLinearSum(preLoop, lower->sum);
    if (!lowerTerm)
        return false;

    MDefinition *upperTerm = ConvertLinearSum(preLoop, upper->sum);
    if (!upperTerm)
        return false;

    // We are checking that index + indexConstant >= 0, and know that
    // index >= lowerTerm + lowerConstant. Thus, check that:
    //
    // lowerTerm + lowerConstant + indexConstant >= 0
    // lowerTerm >= -lowerConstant - indexConstant

    int32_t lowerConstant = 0;
    if (!SafeSub(lowerConstant, index.constant, &lowerConstant))
        return false;
    if (!SafeSub(lowerConstant, lower->sum.constant(), &lowerConstant))
        return false;
    MBoundsCheckLower *lowerCheck = MBoundsCheckLower::New(lowerTerm);
    lowerCheck->setMinimum(lowerConstant);

    // We are checking that index < boundsLength, and know that
    // index <= upperTerm + upperConstant. Thus, check that:
    //
    // upperTerm + upperConstant < boundsLength

    int32_t upperConstant = index.constant;
    if (!SafeAdd(upper->sum.constant(), upperConstant, &upperConstant))
        return false;
    MBoundsCheck *upperCheck = MBoundsCheck::New(upperTerm, ins->length());
    upperCheck->setMinimum(upperConstant);
    upperCheck->setMaximum(upperConstant);

    // Hoist the loop invariant upper and lower bounds checks.
    preLoop->insertBefore(preLoop->lastIns(), lowerCheck);
    preLoop->insertBefore(preLoop->lastIns(), upperCheck);

    return true;
}

bool
RangeAnalysis::analyze()
{
    IonSpew(IonSpew_Range, "Doing range propagation");

    for (ReversePostorderIterator iter(graph_.rpoBegin()); iter != graph_.rpoEnd(); iter++) {
        MBasicBlock *block = *iter;

        for (MDefinitionIterator iter(block); iter; iter++) {
            MDefinition *def = *iter;

            def->computeRange();
            IonSpew(IonSpew_Range, "computing range on %d", def->id());
            SpewRange(def);
        }

        if (block->isLoopHeader())
            analyzeLoop(block);
    }

    return true;
}

///////////////////////////////////////////////////////////////////////////////
// Range based Truncation
///////////////////////////////////////////////////////////////////////////////

void
Range::truncate()
{
    if (isInt32())
        return;
    int64_t l = isLowerInfinite() ? JSVAL_INT_MIN : lower();
    int64_t h = isUpperInfinite() ? JSVAL_INT_MAX : upper();
    set(l, h, false, 32);
}

bool
MDefinition::truncate()
{
    // No procedure defined for truncating this instruction.
    return false;
}

bool
MConstant::truncate()
{
    if (!value_.isDouble())
        return false;

    // Truncate the double to int, since all uses truncates it.
    value_.setInt32(ToInt32(value_.toDouble()));
    setResultType(MIRType_Int32);
    if (range())
        range()->truncate();
    return true;
}

bool
MAdd::truncate()
{
    // Remember analysis, needed for fallible checks.
    setTruncated(true);

    // Modify the instruction if needed.
    if (type() != MIRType_Double)
        return false;

    specialization_ = MIRType_Int32;
    setResultType(MIRType_Int32);
    if (range())
        range()->truncate();
    return true;
}

bool
MSub::truncate()
{
    // Remember analysis, needed for fallible checks.
    setTruncated(true);

    // Modify the instruction if needed.
    if (type() != MIRType_Double)
        return false;

    specialization_ = MIRType_Int32;
    setResultType(MIRType_Int32);
    if (range())
        range()->truncate();
    return true;
}

bool
MMul::truncate()
{
    // Remember analysis, needed to remove negative zero checks.
    setTruncated(true);

    // Modify the instruction.
    bool truncated = type() == MIRType_Int32;
    if (type() == MIRType_Double) {
        specialization_ = MIRType_Int32;
        setResultType(MIRType_Int32);
        truncated = true;
        JS_ASSERT(range());
    }

    if (truncated && range()) {
        range()->truncate();
        setTruncated(true);
        setCanBeNegativeZero(false);
    }

    return truncated;
}

bool
MDiv::truncate()
{
    // Remember analysis, needed to remove negative zero checks.
    setTruncated(true);

    // No modifications.
    return false;
}

bool
MMod::truncate()
{
    // Remember analysis, needed to remove negative zero checks.
    setTruncated(true);

    // No modifications.
    return false;
}

bool
MToDouble::truncate()
{
    JS_ASSERT(type() == MIRType_Double);

    // We use the return type to flag that this MToDouble sould be replaced by a
    // MTruncateToInt32 when modifying the graph.
    setResultType(MIRType_Int32);
    if (range())
        range()->truncate();

    return true;
}

bool
MLoadTypedArrayElementStatic::truncate()
{
    setInfallible();
    return false;
}

bool
MDefinition::isOperandTruncated(size_t index) const
{
    return false;
}

bool
MTruncateToInt32::isOperandTruncated(size_t index) const
{
    return true;
}

bool
MBinaryBitwiseInstruction::isOperandTruncated(size_t index) const
{
    return true;
}

bool
MAdd::isOperandTruncated(size_t index) const
{
    return isTruncated();
}

bool
MSub::isOperandTruncated(size_t index) const
{
    return isTruncated();
}

bool
MMul::isOperandTruncated(size_t index) const
{
    return isTruncated();
}

bool
MToDouble::isOperandTruncated(size_t index) const
{
    // The return type is used to flag that we are replacing this Double by a
    // Truncate of its operand if needed.
    return type() == MIRType_Int32;
}

// Ensure that all observables uses can work with a truncated
// version of the |candidate|'s result.
static bool
AllUsesTruncate(MInstruction *candidate)
{
    for (MUseIterator use(candidate->usesBegin()); use != candidate->usesEnd(); use++) {
        if (!use->consumer()->isDefinition()) {
            // We can only skip testing resume points, if all original uses are still present.
            // Only than testing all uses is enough to guarantee the truncation isn't observerable.
            if (candidate->isUseRemoved())
                return false;
            continue;
        }

        if (!use->consumer()->toDefinition()->isOperandTruncated(use->index()))
            return false;
    }

    return true;
}

static void
RemoveTruncatesOnOutput(MInstruction *truncated)
{
    JS_ASSERT(truncated->type() == MIRType_Int32);
    JS_ASSERT_IF(truncated->range(), truncated->range()->isInt32());

    for (MUseDefIterator use(truncated); use; use++) {
        MDefinition *def = use.def();
        if (!def->isTruncateToInt32() || !def->isToInt32())
            continue;

        def->replaceAllUsesWith(truncated);
    }
}

void
AdjustTruncatedInputs(MInstruction *truncated)
{
    MBasicBlock *block = truncated->block();
    for (size_t i = 0; i < truncated->numOperands(); i++) {
        if (!truncated->isOperandTruncated(i))
            continue;
        if (truncated->getOperand(i)->type() == MIRType_Int32)
            continue;

        MTruncateToInt32 *op = MTruncateToInt32::New(truncated->getOperand(i));
        block->insertBefore(truncated, op);
        truncated->replaceOperand(i, op);
    }

    if (truncated->isToDouble()) {
        truncated->replaceAllUsesWith(truncated->getOperand(0));
        block->discard(truncated);
    }
}

// Iterate backward on all instruction and attempt to truncate operations for
// each instruction which respect the following list of predicates: Has been
// analyzed by range analysis, the range has no rounding errors, all uses cases
// are truncating the result.
//
// If the truncation of the operation is successful, then the instruction is
// queue for later updating the graph to restore the type correctness by
// converting the operands that need to be truncated.
//
// We iterate backward because it is likely that a truncated operation truncates
// some of its operands.
bool
RangeAnalysis::truncate()
{
    IonSpew(IonSpew_Range, "Do range-base truncation (backward loop)");

    Vector<MInstruction *, 16, SystemAllocPolicy> worklist;
    Vector<MBinaryBitwiseInstruction *, 16, SystemAllocPolicy> bitops;

    for (PostorderIterator block(graph_.poBegin()); block != graph_.poEnd(); block++) {
        for (MInstructionReverseIterator iter(block->rbegin()); iter != block->rend(); iter++) {
            // Remember all bitop instructions for folding after range analysis.
            switch (iter->op()) {
              case MDefinition::Op_BitAnd:
              case MDefinition::Op_BitOr:
              case MDefinition::Op_BitXor:
              case MDefinition::Op_Lsh:
              case MDefinition::Op_Rsh:
              case MDefinition::Op_Ursh:
                if (!bitops.append(static_cast<MBinaryBitwiseInstruction*>(*iter)))
                    return false;
              default:;
            }

            // Set truncated flag if range analysis ensure that it has no
            // rounding errors and no freactional part.
            const Range *r = iter->range();
            if (!r || r->hasRoundingErrors())
                continue;

            // Ensure all observable uses are truncated.
            if (!AllUsesTruncate(*iter))
                continue;

            // Truncate this instruction if possible.
            if (!iter->truncate())
                continue;

            // Delay updates of inputs/outputs to avoid creating node which
            // would be removed by the truncation of the next operations.
            iter->setInWorklist();
            if (!worklist.append(*iter))
                return false;
        }
    }

    // Update inputs/outputs of truncated instructions.
    IonSpew(IonSpew_Range, "Do graph type fixup (dequeue)");
    while (!worklist.empty()) {
        MInstruction *ins = worklist.popCopy();
        ins->setNotInWorklist();
        RemoveTruncatesOnOutput(ins);
        AdjustTruncatedInputs(ins);
    }

    // Fold any unnecessary bitops in the graph, such as (x | 0) on an integer
    // input. This is done after range analysis rather than during GVN as the
    // presence of the bitop can change which instructions are truncated.
    for (size_t i = 0; i < bitops.length(); i++) {
        MBinaryBitwiseInstruction *ins = bitops[i];
        MDefinition *folded = ins->foldUnnecessaryBitop();
        if (folded != ins)
            ins->replaceAllUsesWith(folded);
    }

    return true;
}
