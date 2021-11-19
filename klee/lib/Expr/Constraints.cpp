//===-- Constraints.cpp ---------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Constraints.h"

#include "klee/util/ExprPPrinter.h"
#include "klee/util/ExprVisitor.h"
#include "llvm/Support/CommandLine.h"

#include <iostream>
#include <map>

namespace {
  llvm::cl::opt<bool>
  SimplifyConstraints("simplify-constraints",
        llvm::cl::desc("Apply more aggressive constraints simplifications"),
        llvm::cl::init(true));

}

using namespace klee;

class ExprReplaceVisitor : public ExprVisitor {
private:
  ref<Expr> src, dst;

public:
  ExprReplaceVisitor(ref<Expr> _src, ref<Expr> _dst) : src(_src), dst(_dst) {}

  Action visitExpr(const Expr &e) {
    if (e == *src.get()) {
      return Action::changeTo(dst);
    } else {
      return Action::doChildren();
    }
  }

  Action visitExprPost(const Expr &e) {
    if (e == *src.get()) {
      return Action::changeTo(dst);
    } else {
      return Action::doChildren();
    }
  }
};

class ExprReplaceVisitor2 : public ExprVisitor {
private:
  const std::map< ref<Expr>, ref<Expr> > &replacements;

public:
  ExprReplaceVisitor2(const std::map< ref<Expr>, ref<Expr> > &_replacements) 
    : ExprVisitor(true),
      replacements(_replacements) {}

  Action visitExprPost(const Expr &e) {
    std::map< ref<Expr>, ref<Expr> >::const_iterator it =
      replacements.find(ref<Expr>(const_cast<Expr*>(&e)));
    if (it!=replacements.end()) {
      return Action::changeTo(it->second);
    } else {
      return Action::doChildren();
    }
  }
};

class ExprReplaceVisitor3 : public ExprVisitor {
private:
  typedef ConstraintManager::SRange SRange;
  typedef ConstraintManager::ranges_ty ranges_ty;
  const ranges_ty &ranges;

  bool getRange(const ref<Expr>& e, SRange *r) {
    if (ConstantExpr *ce = dyn_cast<ConstantExpr>(e)) {
      *r = SRange(ce->getWidth(), ce->getZExtValue()); return true;
    }
    ranges_ty::const_iterator it = ranges.find(e);
    if (it != ranges.end()) {
      *r = it->second; return true;
    }
    return false;
  }

public:
  ExprReplaceVisitor3(const ranges_ty &_ranges)
      : ExprVisitor(true), ranges(_ranges) {}

  Action visitExpr(const Expr &e) {
    SRange r;
    if (getRange(ref<Expr>(const_cast<Expr*>(&e)), &r))
      if (r.isSingle()) // e can only have a single value
        return Action::changeTo(ConstantExpr::create(r.getSingle(), e.getWidth()));
    return Action::doChildren();
  }

  Action visitEq(const EqExpr &e) {
    SRange lr, rr;
    if (getRange(e.left, &lr) && getRange(e.right, &rr))
        if (lr.intersection(rr).empty())
          return Action::changeTo(ConstantExpr::create(0, Expr::Bool));
    return Action::doChildren();
  }

  Action visitUlt(const UltExpr &e) {
    SRange lr, rr;
    if (getRange(e.left, &lr) && getRange(e.right, &rr)) {
      if (lr.umax() < rr.umin())
        return Action::changeTo(ConstantExpr::create(1, Expr::Bool));
      if (rr.umax() <= lr.umin())
        return Action::changeTo(ConstantExpr::create(0, Expr::Bool));
    }
    return Action::doChildren();
  }

  Action visitUle(const UleExpr &e) {
    SRange lr, rr;
    if (getRange(e.left, &lr) && getRange(e.right, &rr)) {
      if (lr.umax() <= rr.umin())
        return Action::changeTo(ConstantExpr::create(1, Expr::Bool));
      if (rr.umax() < lr.umin())
        return Action::changeTo(ConstantExpr::create(0, Expr::Bool));
    }
    return Action::doChildren();
  }

  Action visitSlt(const SltExpr &e) {
    SRange lr, rr;
    if (getRange(e.left, &lr) && getRange(e.right, &rr)) {
      if (lr.smax() < rr.smin())
        return Action::changeTo(ConstantExpr::create(1, Expr::Bool));
      if (rr.smax() <= lr.smin())
        return Action::changeTo(ConstantExpr::create(0, Expr::Bool));
    }
    return Action::doChildren();
  }

  Action visitSle(const SleExpr &e) {
    SRange lr, rr;
    if (getRange(e.left, &lr) && getRange(e.right, &rr)) {
      if (lr.smax() <= rr.smin())
        return Action::changeTo(ConstantExpr::create(1, Expr::Bool));
      if (rr.smax() < lr.smin())
        return Action::changeTo(ConstantExpr::create(0, Expr::Bool));
    }
    return Action::doChildren();
  }
};

ConstraintManager::ConstraintManager(const std::vector<ref<Expr> > &_constraints)
    : constraints(_constraints) {
  if (SimplifyConstraints)
    recomputeAllRanges();
}

bool ConstraintManager::rewriteConstraints(ExprVisitor &visitor,
                                           bool canBeFalse, bool *ok) {
  ConstraintManager::constraints_ty old;
  bool changed = false;

  constraints.swap(old);
  for (ConstraintManager::constraints_ty::iterator 
         it = old.begin(), ie = old.end(); it != ie; ++it) {
    ref<Expr> &ce = *it;
    ref<Expr> e = visitor.visit(ce);

    if (e!=ce) {
      if (!addConstraintInternal(e, canBeFalse)) { // enable further reductions
        if (ok) *ok = false;
        return true;
      }
      changed = true;
    } else {
      constraints.push_back(ce);
    }
  }

  if (ok) *ok = true;
  return changed;
}

void ConstraintManager::simplifyConstraints(const ref<Expr> &expr,
                                            bool canBeFalse, bool *ok) {
  if (ok) *ok = true;
  constraints_ty old;
  constraints.swap(old);

  ranges.clear();
  computeRanges(expr, ok);
  if (ok && !*ok)
    return;

  ExprReplaceVisitor3 visitor(ranges);

  for (ConstraintManager::constraints_ty::iterator
         it = old.begin(), ie = old.end(); it != ie; ++it) {
    ref<Expr> &ce = *it;

    // Simplify assuming e and all previously added constraints
    ref<Expr> e = visitor.visit(ce);

    if (e != ce) {
      if (!addConstraintInternal(e, canBeFalse)) {
        if (ok) *ok = false;
        return;
      }
      // this might clean out ranges, we have to update them
      computeRanges(expr, ok);
      if (ok && !*ok)
        return;
    } else {
      constraints.push_back(ce);
      computeRanges(ce, ok);
      if (ok && !*ok)
        return;
    }
  }
}

void ConstraintManager::simplifyForValidConstraint(ref<Expr> e) {
  // XXX 
}

ref<Expr> ConstraintManager::simplifyExpr(ref<Expr> e) const {
  if (isa<ConstantExpr>(e))
    return e;

  if (SimplifyConstraints) {
    return ExprReplaceVisitor3(ranges).visit(e);
  } else {
    std::map< ref<Expr>, ref<Expr> > equalities;

    for (ConstraintManager::constraints_ty::const_iterator
           it = constraints.begin(), ie = constraints.end(); it != ie; ++it) {
      if (const EqExpr *ee = dyn_cast<EqExpr>(*it)) {
        if (isa<ConstantExpr>(ee->left)) {
          equalities.insert(std::make_pair(ee->right,
                                           ee->left));
        } else {
          equalities.insert(std::make_pair(*it,
                                           ConstantExpr::alloc(1, Expr::Bool)));
        }
      } else {
        equalities.insert(std::make_pair(*it,
                                         ConstantExpr::alloc(1, Expr::Bool)));
      }
    }

    return ExprReplaceVisitor2(equalities).visit(e);
  }
}

bool ConstraintManager::addConstraintInternal(ref<Expr> e, bool canBeFalse) {
  // rewrite any known equalities 

  // XXX should profile the effects of this and the overhead.
  // traversing the constraints looking for equalities is hardly the
  // slowest thing we do, but it is probably nicer to have a
  // ConstraintSet ADT which efficiently remembers obvious patterns
  // (byte-constant comparison).

  bool ok = true;

  switch (e->getKind()) {
  case Expr::Constant:
    if (canBeFalse) {
      constraints.push_back(e);
      if (cast<ConstantExpr>(e)->isFalse())
        ok = false;
    } else {
      assert(cast<ConstantExpr>(e)->isTrue() &&
             "attempt to add invalid (false) constraint");
    }
    break;
    
    // split to enable finer grained independence and other optimizations
  case Expr::And: {
    BinaryExpr *be = cast<BinaryExpr>(e);
    ok &= addConstraintInternal(be->left, canBeFalse);
    ok &= addConstraintInternal(be->right, canBeFalse);
    break;
  }

  case Expr::Eq:
    if (/*1 || */!SimplifyConstraints) {
      BinaryExpr *be = cast<BinaryExpr>(e);
      if (isa<ConstantExpr>(be->left)) {
        ExprReplaceVisitor visitor(be->right, be->left);
        rewriteConstraints(visitor, canBeFalse, &ok);
      }
      constraints.push_back(e);
      //computeRanges(e);
      break;
    }

  default:
    if (/*0 && */SimplifyConstraints) {
      if (computeRanges(e, &ok) && ok)
        simplifyConstraints(e, canBeFalse, &ok);
    } else {
      //computeRanges(e);
    }
    constraints.push_back(e);
    break;
  }
  return ok;
}

void ConstraintManager::addConstraint(ref<Expr> e) {
  e = simplifyExpr(e);
  addConstraintInternal(e, false);
}

bool ConstraintManager::checkAddConstraint(ref<Expr> e) {
  e = simplifyExpr(e);
  return addConstraintInternal(e, true);
}

bool ConstraintManager::intersectRange(const ref<Expr> &e, const SRange &r, bool *ok) {
  std::pair<ranges_ty::iterator, bool> p = ranges.insert(std::make_pair(e, r));
  if (p.second)
    return true;

  // compute the intersection
  SRange s = p.first->second.intersection(r);
  if (p.first->second != s) {
    if (ok)
      *ok = !s.empty();
    else
      assert(!s.empty());
    p.first->second = s;
    return true;
  }
  return false;
}

bool ConstraintManager::computeRanges(const ref<Expr>& expr, bool *ok) {
  bool changed = intersectRange(expr, SRange(1, 1), ok);
  if (ok && !*ok)
    return changed;
  if (const CmpExpr *e = dyn_cast<const CmpExpr>(expr)) {
    switch (e->getKind()) {
    case Expr::Eq:
      if (ConstantExpr *ce = dyn_cast<ConstantExpr>(e->left)) {
        changed |=
            intersectRange(e->right, SRange(ce->getWidth(), ce->getZExtValue()), ok);
        if (ok && !*ok)
          return changed;
        if (ce->getWidth() == Expr::Bool) {
          if (ce->isTrue()) {
            return computeRanges(e->right, ok) | changed;
          } else if (const CmpExpr *ne = dyn_cast<const CmpExpr>(e->right)) {
            switch (ne->getKind()) {
            case Expr::Eq:
              return computeRanges(NeExpr::alloc(ne->left, ne->right), ok) | changed;
            case Expr::Ult:
              return computeRanges(UleExpr::alloc(ne->right, ne->left), ok) | changed;
            case Expr::Ule:
              return computeRanges(UltExpr::alloc(ne->right, ne->left), ok) | changed;
            case Expr::Slt:
              return computeRanges(SleExpr::alloc(ne->right, ne->left), ok) | changed;
            case Expr::Sle:
              return computeRanges(SltExpr::alloc(ne->right, ne->left), ok) | changed;
            default:
              break;
            }
          }
        }
        return changed;
      }
      break;
    case Expr::Ne:
      if (ConstantExpr *ce = dyn_cast<ConstantExpr>(e->left)) {
        if (ce->getWidth() == 1) {
          return intersectRange(e->right, SRange(1, 1ull-ce->getZExtValue(1)), ok) | changed;
        } else {
          uint64_t val = ce->getZExtValue();
          uint64_t max = bits64::maxValueOfNBits(ce->getWidth());
          if (val == 0)
            return intersectRange(e->right, SRange(ce->getWidth(), 1ull, max), ok) | changed;
          else if (val == (max>>1)) // maximum positive
            return intersectRange(e->right, SRange(ce->getWidth(), (max>>1)+1, (max>>1)-1), ok) | changed;
          else if (val == (max>>1)+1) // minimum negative
            return intersectRange(e->right, SRange(ce->getWidth(), (max>>1)+2, (max>>1)), ok) | changed;
          else if (val == max)
            return intersectRange(e->right, SRange(ce->getWidth(), 0, max-1), ok) | changed;
        }
      }
      break;
    case Expr::Ult:
      if (ConstantExpr *ce = dyn_cast<ConstantExpr>(e->left)) {
        assert(ce->getZExtValue() < bits64::maxValueOfNBits(ce->getWidth()));
        return intersectRange(e->right, SRange(ce->getWidth(),
                                     ce->getZExtValue()+1, ULLONG_MAX), ok) | changed;
      } else if (ConstantExpr *ce = dyn_cast<ConstantExpr>(e->right)) {
        assert(ce->getZExtValue() > 0);
        return intersectRange(e->left, SRange(ce->getWidth(), 0ull, ce->getZExtValue()-1), ok) | changed;
      }
      break;
    case Expr::Ule:
      if (ConstantExpr *ce = dyn_cast<ConstantExpr>(e->left)) {
        return intersectRange(e->right, SRange(ce->getWidth(),
                                     ce->getZExtValue(), ULLONG_MAX), ok) | changed;
      } else if (ConstantExpr *ce = dyn_cast<ConstantExpr>(e->right)) {
        return intersectRange(e->left, SRange(ce->getWidth(), 0ull, ce->getZExtValue()), ok) | changed;
      }
      break;
    case Expr::Slt:
      if (ConstantExpr *ce = dyn_cast<ConstantExpr>(e->left)) {
        uint64_t max = bits64::maxValueOfNBits(ce->getWidth()-1);
        assert(ce->getZExtValue() != max); // +1 won't wrap
        return intersectRange(e->right, SRange(ce->getWidth(), ce->getZExtValue()+1, max), ok) | changed;
      } else if (ConstantExpr *ce = dyn_cast<ConstantExpr>(e->right)) {
        uint64_t min = 1 << (ce->getWidth()-1);
        assert(ce->getZExtValue() != min); // -1 won't wrap
        return intersectRange(e->left, SRange(ce->getWidth(), min, ce->getZExtValue()-1), ok) | changed;
      }
      break;
    case Expr::Sle:
      if (ConstantExpr *ce = dyn_cast<ConstantExpr>(e->left)) {
        return intersectRange(e->right, SRange(ce->getWidth(), ce->getZExtValue(),
                                     bits64::maxValueOfNBits(ce->getWidth()-1)), ok) | changed;
      } else if (ConstantExpr *ce = dyn_cast<ConstantExpr>(e->right)) {
        return intersectRange(e->left, SRange(ce->getWidth(), 1 << (ce->getWidth()-1),
                                    ce->getZExtValue()), ok) | changed;
      }
      break;
    default:
      break;
    }
  }
  return changed;
}

void ConstraintManager::recomputeAllRanges() {
  ranges.clear();

  for (ConstraintManager::constraints_ty::iterator
         it = constraints.begin(), ie = constraints.end(); it != ie; ++it) {
    computeRanges(*it, NULL);
  }
}
