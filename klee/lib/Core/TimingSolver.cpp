//===-- TimingSolver.cpp --------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "TimingSolver.h"

#include "klee/ExecutionState.h"
#include "klee/Solver.h"
#include "klee/Statistics.h"

#include "CoreStats.h"

#if (LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 9)
#include "llvm/System/Process.h"
#else
#include "llvm/Support/Process.h"
#endif

#include "cloud9/instrum/Timing.h"
#include "cloud9/instrum/InstrumentationManager.h"

using namespace klee;
using namespace llvm;

using cloud9::instrum::Timer;

static void recordStateInfo(cloud9::instrum::EventClass instrumEvent, const ExecutionState &state) {
  cloud9::instrum::theInstrManager.recordEventAttribute(
        instrumEvent, cloud9::instrum::StateDepth, state.depth);
  cloud9::instrum::theInstrManager.recordEventAttribute(
        instrumEvent, cloud9::instrum::StateMultiplicity, uint64_t(state.multiplicity));
}

static void recordTiming(const Timer &t, const ExecutionState &state) {
  recordStateInfo(cloud9::instrum::ConstraintSolve, state);

  cloud9::instrum::theInstrManager.recordEventTiming(
      cloud9::instrum::ConstraintSolve, t);

  cloud9::instrum::theInstrManager.recordEvent(cloud9::instrum::ConstraintSolve);
}

static void clearStateInfo(cloud9::instrum::EventClass instrumEvent) {
  cloud9::instrum::theInstrManager.clearEventAttribute(instrumEvent,
      cloud9::instrum::StateDepth);
  cloud9::instrum::theInstrManager.clearEventAttribute(instrumEvent,
      cloud9::instrum::StateMultiplicity);
}

/***/

bool TimingSolver::evaluate(const ExecutionState& state, ref<Expr> expr,
                            Solver::Validity &result) {
  // Fast path, to avoid timer and OS overhead.
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(expr)) {
    result = CE->isTrue() ? Solver::True : Solver::False;
    return true;
  }

  sys::TimeValue now(0,0),user(0,0),delta(0,0),sys(0,0);
  sys::Process::GetTimeUsage(now,user,sys);

  Timer t;
  recordStateInfo(cloud9::instrum::SMTSolve, state);
  recordStateInfo(cloud9::instrum::SATSolve, state);
  t.start();

  if (simplifyExprs)
    expr = state.constraints().simplifyExpr(expr);

  bool success = solver->evaluate(Query(state.constraints(), expr), result);

  t.stop();
  recordTiming(t, state);
  clearStateInfo(cloud9::instrum::SMTSolve);
  clearStateInfo(cloud9::instrum::SATSolve);

  sys::Process::GetTimeUsage(delta,user,sys);
  delta -= now;
  stats::solverTime += delta.usec();
  state.queryCost += delta.usec()/1000000.;

  return success;
}

bool TimingSolver::mustBeTrue(const ExecutionState& state, ref<Expr> expr, 
                              bool &result) {
  // Fast path, to avoid timer and OS overhead.
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(expr)) {
    result = CE->isTrue() ? true : false;
    return true;
  }

  sys::TimeValue now(0,0),user(0,0),delta(0,0),sys(0,0);
  sys::Process::GetTimeUsage(now,user,sys);

  Timer t;
  recordStateInfo(cloud9::instrum::SMTSolve, state);
  recordStateInfo(cloud9::instrum::SATSolve, state);
  t.start();

  if (simplifyExprs)
    expr = state.constraints().simplifyExpr(expr);

  bool success = solver->mustBeTrue(Query(state.constraints(), expr), result);

  t.stop();
  recordTiming(t, state);
  clearStateInfo(cloud9::instrum::SMTSolve);
  clearStateInfo(cloud9::instrum::SATSolve);

  sys::Process::GetTimeUsage(delta,user,sys);
  delta -= now;
  stats::solverTime += delta.usec();
  state.queryCost += delta.usec()/1000000.;

  return success;
}

bool TimingSolver::mustBeFalse(const ExecutionState& state, ref<Expr> expr,
                               bool &result) {
  return mustBeTrue(state, Expr::createIsZero(expr), result);
}

bool TimingSolver::mayBeTrue(const ExecutionState& state, ref<Expr> expr, 
                             bool &result) {
  bool res;
  if (!mustBeFalse(state, expr, res))
    return false;
  result = !res;
  return true;
}

bool TimingSolver::mayBeFalse(const ExecutionState& state, ref<Expr> expr, 
                              bool &result) {
  bool res;
  if (!mustBeTrue(state, expr, res))
    return false;
  result = !res;
  return true;
}

bool TimingSolver::getValue(const ExecutionState& state, ref<Expr> expr, 
                            ref<ConstantExpr> &result) {
  // Fast path, to avoid timer and OS overhead.
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(expr)) {
    result = CE;
    return true;
  }
  
  sys::TimeValue now(0,0),user(0,0),delta(0,0),sys(0,0);
  sys::Process::GetTimeUsage(now,user,sys);

  Timer t;
  recordStateInfo(cloud9::instrum::SMTSolve, state);
  recordStateInfo(cloud9::instrum::SATSolve, state);
  t.start();

  if (simplifyExprs)
    expr = state.constraints().simplifyExpr(expr);

  bool success = solver->getValue(Query(state.constraints(), expr), result);

  t.stop();
  recordTiming(t, state);
  clearStateInfo(cloud9::instrum::SMTSolve);
  clearStateInfo(cloud9::instrum::SATSolve);

  sys::Process::GetTimeUsage(delta,user,sys);
  delta -= now;
  stats::solverTime += delta.usec();
  state.queryCost += delta.usec()/1000000.;

  return success;
}

bool 
TimingSolver::getInitialValues(const ExecutionState& state, 
                               const std::vector<const Array*>
                                 &objects,
                               std::vector< std::vector<unsigned char> >
                                 &result) {
  if (objects.empty())
    return true;

  sys::TimeValue now(0,0),user(0,0),delta(0,0),sys(0,0);
  sys::Process::GetTimeUsage(now,user,sys);

  Timer t;
  recordStateInfo(cloud9::instrum::SMTSolve, state);
  recordStateInfo(cloud9::instrum::SATSolve, state);
  t.start();

  bool success = solver->getInitialValues(Query(state.constraints(),
                                                ConstantExpr::alloc(0, Expr::Bool)), 
                                          objects, result);
  
  t.stop();
  recordTiming(t, state);
  clearStateInfo(cloud9::instrum::SMTSolve);
  clearStateInfo(cloud9::instrum::SATSolve);

  sys::Process::GetTimeUsage(delta,user,sys);
  delta -= now;
  stats::solverTime += delta.usec();
  state.queryCost += delta.usec()/1000000.;
  
  return success;
}

std::pair< ref<Expr>, ref<Expr> >
TimingSolver::getRange(const ExecutionState& state, ref<Expr> expr) {
  return solver->getRange(Query(state.constraints(), expr));
}
