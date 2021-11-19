//===-- Constraints.h -------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_CONSTRAINTS_H
#define KLEE_CONSTRAINTS_H

#include "klee/Expr.h"
#include <map>

// FIXME: Currently we use ConstraintManager for two things: to pass
// sets of constraints around, and to optimize constraints. We should
// move the first usage into a separate data structure
// (ConstraintSet?) which ConstraintManager could embed if it likes.
namespace klee {

class ExprVisitor;
  
class ConstraintManager {
public:
  typedef std::vector< ref<Expr> > constraints_ty;
  typedef constraints_ty::iterator iterator;
  typedef constraints_ty::const_iterator const_iterator;

  ConstraintManager() {}

  // create from constraints with no optimization
  explicit
  ConstraintManager(const std::vector< ref<Expr> > &_constraints);

  ConstraintManager(const ConstraintManager &cs)
      : constraints(cs.constraints),
        ranges(cs.ranges) {}

  typedef std::vector< ref<Expr> >::const_iterator constraint_iterator;

  // given a constraint which is known to be valid, attempt to 
  // simplify the existing constraint set
  void simplifyForValidConstraint(ref<Expr> e);

  ref<Expr> simplifyExpr(ref<Expr> e) const;

  void addConstraint(ref<Expr> e);

  // Add constraint that might be unsatisfiable. Return false if
  // unsatisfiability is detected. In this case, the ConstraintManager
  // is in an undefined state. NOTE: true return value does not guarantees
  // that the constraint is satisfiable.
  bool checkAddConstraint(ref<Expr> e);
  
  bool empty() const {
    return constraints.empty();
  }
  ref<Expr> back() const {
    return constraints.back();
  }
  constraint_iterator begin() const {
    return constraints.begin();
  }
  constraint_iterator end() const {
    return constraints.end();
  }
  size_t size() const {
    return constraints.size();
  }

  bool operator==(const ConstraintManager &other) const {
    return constraints == other.constraints;
  }
  
private:
  std::vector< ref<Expr> > constraints;

public:

  // Signed range represented as a union of two ranges (positive and negative)
  struct SRange {
  private:
    uint64_t minL, maxL;
    uint64_t minU, maxU;
    unsigned width;

    void init(unsigned width, uint64_t min, uint64_t max) {
      this->width = width; assert(width != 0);
      uint64_t maxV = bits64::maxValueOfNBits(width);
      uint64_t midV = maxV > 1 ? maxV >> 1 : maxV;
      if (min > midV && max <= midV) {
        minL = 0; maxU = maxV;
      } else {
        minL = min; maxU = std::min(max, maxV);
      }
      maxL = std::min(max, midV);
      minU = std::max(min, midV+1);
    }

    int64_t sext(uint64_t v) { return int64_t(v<<(64-width)) >> (64-width); }

  public:
    SRange(): width(0) {}

    SRange(unsigned width) { init(width, 0ull, ULLONG_MAX); }

    SRange(unsigned width, uint64_t value) { init(width, value, value); }
    SRange(unsigned width, uint64_t min, uint64_t max) { init(width, min, max); }

    bool empty() { return minL > maxL && minU > maxU; }

    SRange intersection(const SRange &o) {
      SRange r = *this; assert(r.width == o.width);
      if (r.minL < o.minL) r.minL = o.minL; if (r.maxL > o.maxL) r.maxL = o.maxL;
      if (r.minU < o.minU) r.minU = o.minU; if (r.maxU > o.maxU) r.maxU = o.maxU;
      return r;
    }

    bool isSingle() {
      return (minL == maxL && minU > maxU) || (minU == maxU && minL > maxL);
    }

    uint64_t getSingle() {
      if (minL == maxL) { assert(minU > maxU); return minL; }
      assert(minL > maxL && minU == maxU); return minU;
    }

    uint64_t umin() { assert(!empty()); return minL <= maxL ? minL : minU; }
    uint64_t umax() { assert(!empty()); return minU <= maxU ? maxU : maxL; }

    int64_t smin() { assert(!empty()); return sext(minU <= maxU ? minU : minL); }
    int64_t smax() { assert(!empty()); return sext(minL <= maxL ? maxL : maxU); }

    bool operator==(const SRange& o) {
      return ((minL > maxL && o.minL > o.maxL) ||
              (minL == o.minL && maxL == o.maxL)) &&
             ((minU > maxU && o.minU > o.maxU) ||
              (minU == o.minU && maxU == o.maxU)) && width == o.width;
    }

    bool operator!=(const SRange& o) { return !(*this == o); }
  };

  typedef std::map<ref<Expr>, SRange> ranges_ty;

  ConstraintManager(const constraints_ty &_constraints,
                    const ranges_ty &_ranges)
    : constraints(_constraints),
      ranges(_ranges) {}

  ranges_ty ranges;

private:
  // returns true iff the constraints were modified
  bool rewriteConstraints(ExprVisitor &visitor, bool canBeFalse, bool *ok);

  // simplify constraints assuming e is true
  void simplifyConstraints(const ref<Expr>& e, bool canBeFalse, bool *ok);

  bool addConstraintInternal(ref<Expr> e, bool canBeFalse);

  // return true of the range info already existed
  bool intersectRange(const ref<Expr>& e, const SRange& r, bool *ok);

  // update ranges assuming e is true,
  // return true if already existing ranges might be changed
  bool computeRanges(const ref<Expr>& e, bool *ok);
  void recomputeAllRanges();
};

}

#endif /* KLEE_CONSTRAINTS_H */
