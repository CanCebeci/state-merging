//===-- Solver.cpp --------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Solver.h"
#include "klee/SolverImpl.h"

#include "SolverStats.h"
#include "STPBuilder.h"

#include "klee/Constraints.h"
#include "klee/Expr.h"
#include "klee/TimerStatIncrementer.h"
#include "klee/util/Assignment.h"
#include "klee/util/ExprPPrinter.h"
#include "klee/util/ExprUtil.h"
#include "klee/Internal/Support/Timer.h"
#include "../Core/Common.h"

#if (LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 9)
#include "llvm/System/Process.h"
#else
#include "llvm/Support/Process.h"
#endif

#include "cloud9/instrum/Timing.h"
#include "cloud9/instrum/InstrumentationManager.h"
#include "cloud9/Logger.h"


#define vc_bvBoolExtract IAMTHESPAWNOFSATAN

#include <cassert>
#include <cstdio>
#include <map>
#include <vector>

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <signal.h>
#include <pthread.h>

#include <iostream>
#include <fstream>

using namespace klee;
using namespace llvm;

using cloud9::instrum::Timer;

#define SHARED_MEM_SIZE	(1<<20)

const ConstraintManager Query::emptyConstraintManager;

Query Query::asOneExpr() const {
  ref<Expr> newExpr = expr;
  for (ConstraintManager::const_iterator it = constraints.begin(),
              ie = constraints.end(); it != ie; ++it) {
    newExpr = OrExpr::create(newExpr, Expr::createIsZero(*it));
  }
  return Query(emptyConstraintManager, newExpr);
}

/***/

const char *Solver::validity_to_str(Validity v) {
  switch (v) {
  default:    return "Unknown";
  case True:  return "True";
  case False: return "False";
  }
}

Solver::~Solver() { 
  delete impl; 
}

SolverImpl::~SolverImpl() {
}

bool Solver::evaluate(const Query& query, Validity &result) {
  assert(query.expr->getWidth() == Expr::Bool && "Invalid expression type!");

  // Maintain invariants implementations expect.
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(query.expr)) {
    result = CE->isTrue() ? True : False;
    return true;
  }

  return impl->computeValidity(query, result);
}

bool SolverImpl::computeValidity(const Query& query, Solver::Validity &result) {
  bool isTrue, isFalse;
  if (!computeTruth(query, isTrue))
    return false;
  if (isTrue) {
    result = Solver::True;
  } else {
    if (!computeTruth(query.negateExpr(), isFalse))
      return false;
    result = isFalse ? Solver::False : Solver::Unknown;
  }
  return true;
}

bool Solver::mustBeTrue(const Query& query, bool &result) {
  assert(query.expr->getWidth() == Expr::Bool && "Invalid expression type!");

  // Maintain invariants implementations expect.
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(query.expr)) {
    result = CE->isTrue() ? true : false;
    return true;
  }

  return impl->computeTruth(query, result);
}

bool Solver::mustBeFalse(const Query& query, bool &result) {
  return mustBeTrue(query.negateExpr(), result);
}

bool Solver::mayBeTrue(const Query& query, bool &result) {
  bool res;
  if (!mustBeFalse(query, res))
    return false;
  result = !res;
  return true;
}

bool Solver::mayBeFalse(const Query& query, bool &result) {
  bool res;
  if (!mustBeTrue(query, res))
    return false;
  result = !res;
  return true;
}

bool Solver::getValue(const Query& query, ref<ConstantExpr> &result) {
  // Maintain invariants implementation expect.
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(query.expr)) {
    result = CE;
    return true;
  }

  // FIXME: Push ConstantExpr requirement down.
  ref<Expr> tmp;
  if (!impl->computeValue(query, tmp))
    return false;
  
  result = cast<ConstantExpr>(tmp);
  return true;
}

bool 
Solver::getInitialValues(const Query& query,
                         const std::vector<const Array*> &objects,
                         std::vector< std::vector<unsigned char> > &values) {
  bool hasSolution;
  bool success =
    impl->computeInitialValues(query, objects, values, hasSolution);
  // FIXME: Propogate this out.
  if (!hasSolution)
    return false;
    
  return success;
}

std::pair< ref<Expr>, ref<Expr> > Solver::getRange(const Query& query) {
  ref<Expr> e = query.expr;
  Expr::Width width = e->getWidth();
  uint64_t min, max;

  if (width==1) {
    Solver::Validity result;
    if (!evaluate(query, result))
      assert(0 && "computeValidity failed");
    switch (result) {
    case Solver::True: 
      min = max = 1; break;
    case Solver::False: 
      min = max = 0; break;
    default:
      min = 0, max = 1; break;
    }
  } else if (ConstantExpr *CE = dyn_cast<ConstantExpr>(e)) {
    min = max = CE->getZExtValue();
  } else {
    // binary search for # of useful bits
    uint64_t lo=0, hi=width, mid, bits=0;
    while (lo<hi) {
      mid = lo + (hi - lo)/2;
      bool res;
      bool success = 
        mustBeTrue(query.withExpr(
                     EqExpr::create(LShrExpr::create(e,
                                                     ConstantExpr::create(mid, 
                                                                          width)),
                                    ConstantExpr::create(0, width))),
                   res);

      assert(success && "FIXME: Unhandled solver failure");
      (void) success;

      if (res) {
        hi = mid;
      } else {
        lo = mid+1;
      }

      bits = lo;
    }
    
    // could binary search for training zeros and offset
    // min max but unlikely to be very useful

    // check common case
    bool res = false;
    bool success = 
      mayBeTrue(query.withExpr(EqExpr::create(e, ConstantExpr::create(0, 
                                                                      width))), 
                res);

    assert(success && "FIXME: Unhandled solver failure");      
    (void) success;

    if (res) {
      min = 0;
    } else {
      // binary search for min
      lo=0, hi=bits64::maxValueOfNBits(bits);
      while (lo<hi) {
        mid = lo + (hi - lo)/2;
        bool res = false;
        bool success = 
          mayBeTrue(query.withExpr(UleExpr::create(e, 
                                                   ConstantExpr::create(mid, 
                                                                        width))),
                    res);

        assert(success && "FIXME: Unhandled solver failure");      
        (void) success;

        if (res) {
          hi = mid;
        } else {
          lo = mid+1;
        }
      }

      min = lo;
    }

    // binary search for max
    lo=min, hi=bits64::maxValueOfNBits(bits);
    while (lo<hi) {
      mid = lo + (hi - lo)/2;
      bool res;
      bool success = 
        mustBeTrue(query.withExpr(UleExpr::create(e, 
                                                  ConstantExpr::create(mid, 
                                                                       width))),
                   res);

      assert(success && "FIXME: Unhandled solver failure");      
      (void) success;

      if (res) {
        hi = mid;
      } else {
        lo = mid+1;
      }
    }

    max = lo;
  }

  return std::make_pair(ConstantExpr::create(min, width),
                        ConstantExpr::create(max, width));
}

/***/

class ValidatingSolver : public SolverImpl {
private:
  Solver *solver, *oracle;

public: 
  ValidatingSolver(Solver *_solver, Solver *_oracle) 
    : solver(_solver), oracle(_oracle) {}
  ~ValidatingSolver() { delete solver; }
  
  bool computeValidity(const Query&, Solver::Validity &result);
  bool computeTruth(const Query&, bool &isValid);
  bool computeValue(const Query&, ref<Expr> &result);
  bool computeInitialValues(const Query&,
                            const std::vector<const Array*> &objects,
                            std::vector< std::vector<unsigned char> > &values,
                            bool &hasSolution);

  void cancelPendingJobs() { assert(0); }
};

bool ValidatingSolver::computeTruth(const Query& query,
                                    bool &isValid) {
  bool answer;
  
  if (!solver->impl->computeTruth(query, isValid))
    return false;
  if (!oracle->impl->computeTruth(query, answer))
    return false;
  
  if (isValid != answer)
    assert(0 && "invalid solver result (computeTruth)");
  
  return true;
}

bool ValidatingSolver::computeValidity(const Query& query,
                                       Solver::Validity &result) {
  Solver::Validity answer;
  
  if (!solver->impl->computeValidity(query, result))
    return false;
  if (!oracle->impl->computeValidity(query, answer))
    return false;
  
  if (result != answer)
    assert(0 && "invalid solver result (computeValidity)");
  
  return true;
}

bool ValidatingSolver::computeValue(const Query& query,
                                    ref<Expr> &result) {  
  bool answer;

  if (!solver->impl->computeValue(query, result))
    return false;
  // We don't want to compare, but just make sure this is a legal
  // solution.
  if (!oracle->impl->computeTruth(query.withExpr(NeExpr::create(query.expr, 
                                                                result)),
                                  answer))
    return false;

  if (answer)
    assert(0 && "invalid solver result (computeValue)");

  return true;
}

bool 
ValidatingSolver::computeInitialValues(const Query& query,
                                       const std::vector<const Array*>
                                         &objects,
                                       std::vector< std::vector<unsigned char> >
                                         &values,
                                       bool &hasSolution) {
  bool answer;

  if (!solver->impl->computeInitialValues(query, objects, values, 
                                          hasSolution))
    return false;

  if (hasSolution) {
    // Assert the bindings as constraints, and verify that the
    // conjunction of the actual constraints is satisfiable.
    std::vector< ref<Expr> > bindings;
    for (unsigned i = 0; i != values.size(); ++i) {
      const Array *array = objects[i];
      for (unsigned j=0; j<array->size; j++) {
        unsigned char value = values[i][j];
        bindings.push_back(EqExpr::create(ReadExpr::create(UpdateList(array, 0),
                                                           ConstantExpr::alloc(j, Expr::Int32)),
                                          ConstantExpr::alloc(value, Expr::Int8)));
      }
    }
    ConstraintManager tmp(bindings);
    ref<Expr> constraints = Expr::createIsZero(query.expr);
    for (ConstraintManager::const_iterator it = query.constraints.begin(), 
           ie = query.constraints.end(); it != ie; ++it)
      constraints = AndExpr::create(constraints, *it);
    
    if (!oracle->impl->computeTruth(Query(tmp, constraints), answer))
      return false;
    if (!answer)
      assert(0 && "invalid solver result (computeInitialValues)");
  } else {
    if (!oracle->impl->computeTruth(query, answer))
      return false;
    if (!answer)
      assert(0 && "invalid solver result (computeInitialValues)");    
  }

  return true;
}

Solver *klee::createValidatingSolver(Solver *s, Solver *oracle) {
  return new Solver(new ValidatingSolver(s, oracle));
}

/***/

class DummySolverImpl : public SolverImpl {
public: 
  DummySolverImpl() {}
  
  bool computeValidity(const Query&, Solver::Validity &result) { 
    ++stats::queries;
    // FIXME: We should have stats::queriesFail;
    return false; 
  }
  bool computeTruth(const Query&, bool &isValid) { 
    ++stats::queries;
    // FIXME: We should have stats::queriesFail;
    return false; 
  }
  bool computeValue(const Query&, ref<Expr> &result) { 
    ++stats::queries;
    ++stats::queryCounterexamples;
    return false; 
  }
  bool computeInitialValues(const Query&,
                            const std::vector<const Array*> &objects,
                            std::vector< std::vector<unsigned char> > &values,
                            bool &hasSolution) { 
    ++stats::queries;
    ++stats::queryCounterexamples;
    return false; 
  }
  void cancelPendingJobs() {}
};

Solver *klee::createDummySolver() {
  return new Solver(new DummySolverImpl());
}

/***/

class STPSolverImpl : public SolverImpl {
private:
  /// The solver we are part of, for access to public information.
  STPSolver *solver;
  VC vc;
  STPBuilder *builder;
  double timeout;
  bool useForkedSTP;
  bool enableLogging;

  //pthread_mutex_t mutex;
  pthread_mutex_t mutex;
  std::set<pid_t> solverInstances;

  pthread_key_t shmSegmentKey;

public:
  STPSolverImpl(STPSolver *_solver, bool _useForkedSTP, bool _optimizeDivides,
      bool _enabledLogging);
  ~STPSolverImpl();

  char *getConstraintLog(const Query&);
  void setTimeout(double _timeout) { timeout = _timeout; }
  double getTimeout() { return timeout; }

  bool computeTruth(const Query&, bool &isValid);
  bool computeValue(const Query&, ref<Expr> &result);
  bool computeInitialValues(const Query&,
                            const std::vector<const Array*> &objects,
                            std::vector< std::vector<unsigned char> > &values,
                            bool &hasSolution);

  void cancelPendingJobs();

  void printSimplified(const Query&);
};

static void stp_error_handler(const char* err_msg) {
  fprintf(stderr, "error: STP Error: %s\n", err_msg);
  abort();
}

STPSolverImpl::STPSolverImpl(STPSolver *_solver, bool _useForkedSTP,
    bool _optimizeDivides, bool _enabledLogging)
  : solver(_solver),
    vc(vc_createValidityChecker()),
    builder(new STPBuilder(vc, _optimizeDivides)),
    timeout(0.0),
    useForkedSTP(_useForkedSTP),
    enableLogging(_enabledLogging)
{
  assert(vc && "unable to create validity checker");
  assert(builder && "unable to create STPBuilder");

#ifdef HAVE_EXT_STP
  // In newer versions of STP, a memory management mechanism has been
  // introduced that automatically invalidates certain C interface
  // pointers at vc_Destroy time.  This caused double-free errors
  // due to the ExprHandle destructor also attempting to invalidate
  // the pointers using vc_DeleteExpr.  By setting EXPRDELETE to 0
  // we restore the old behaviour.
  vc_setInterfaceFlags(vc, EXPRDELETE, 0);
#endif
  vc_registerErrorHandler(::stp_error_handler);

  /*
  if (useForkedSTP)
    defaultShMem = solver->getSharedMemSegment();
  else
    defaultShMem = NULL;
  */

  if (useForkedSTP) {
    pthread_mutex_init(&mutex, NULL);
  }

  pthread_key_create(&shmSegmentKey, (void (*)(void*)) shmdt);
}

STPSolverImpl::~STPSolverImpl() {
  delete builder;

  //shmdt(defaultShMem);

  pthread_mutex_destroy(&mutex);

  vc_Destroy(vc);
}

/***/

STPSolver::STPSolver(bool useForkedSTP, bool optimizeDivides, bool enabledLogging)
  : Solver(new STPSolverImpl(this, useForkedSTP, optimizeDivides, enabledLogging))
{
}

char *STPSolver::getConstraintLog(const Query &query) {
  return static_cast<STPSolverImpl*>(impl)->getConstraintLog(query);  
}

void STPSolver::setTimeout(double timeout) {
  static_cast<STPSolverImpl*>(impl)->setTimeout(timeout);
}

/*
char *STPSolver::getSharedMemSegment() {
	int shmID = shmget(IPC_PRIVATE, SHARED_MEM_SIZE, IPC_CREAT | 0700);
	assert(shmID>=0 && "shmget failed");

	char *shmPtr = (char*) shmat(shmID, NULL, 0);
	assert(shmPtr!=(void*)-1 && "shmat failed");
	shmctl(shmID, IPC_RMID, NULL);

	return shmPtr;
}

void STPSolver::releaseSharedMemSegment(char *shMem) {
	int res = shmdt(shMem);

	assert(res == 0);
}

bool STPSolver::getInitialValues(const Query& query,
                      const std::vector<const Array*> &objects,
                      std::vector< std::vector<unsigned char> > &result,
                      bool &hasSolution) {
	return dynamic_cast<STPSolverImpl*>(impl)->computeInitialValues(query,
			objects, result, hasSolution);
}

bool STPSolver::getInitialValues(const Query& query,
                      const std::vector<const Array*> &objects,
                      std::vector< std::vector<unsigned char> > &result,
                      bool &hasSolution,
                      char *shMemBuffer) {
	return dynamic_cast<STPSolverImpl*>(impl)->computeInitialValues(query,
				objects, result, hasSolution, shMemBuffer);
}

void STPSolver::cancelPendingJobs() {
  dynamic_cast<STPSolverImpl*>(impl)->cancelPendingJobs();
}
*/

/***/

char *STPSolverImpl::getConstraintLog(const Query &query) {
  vc_push(vc);
  for (std::vector< ref<Expr> >::const_iterator it = query.constraints.begin(), 
         ie = query.constraints.end(); it != ie; ++it)
    vc_assertFormula(vc, builder->construct(*it));
  assert(query.expr == ConstantExpr::alloc(0, Expr::Bool) &&
         "Unexpected expression in query!");

  char *buffer;
  unsigned long length;
  vc_printQueryStateToBuffer(vc, builder->getFalse(), 
                             &buffer, &length, false);
  vc_pop(vc);

  return buffer;
}

bool STPSolverImpl::computeTruth(const Query& query,
                                 bool &isValid) {
  std::vector<const Array*> objects;
  std::vector< std::vector<unsigned char> > values;
  bool hasSolution;

  if (!computeInitialValues(query, objects, values, hasSolution))
    return false;

  isValid = !hasSolution;
  return true;
}

bool STPSolverImpl::computeValue(const Query& query,
                                 ref<Expr> &result) {
  std::vector<const Array*> objects;
  std::vector< std::vector<unsigned char> > values;
  bool hasSolution;

  // Find the object used in the expression, and compute an assignment
  // for them.
  findSymbolicObjects(query.expr, objects);
  if (!computeInitialValues(query.withFalse(), objects, values, hasSolution))
    return false;
  assert(hasSolution && "state has invalid constraint set");
  
  // Evaluate the expression with the computed assignment.
  Assignment a(objects, values);
  result = a.evaluate(query.expr);

  return true;
}

static void stpTimeoutHandler(int x) {
  _exit(52);
}

bool
STPSolverImpl::computeInitialValues(const Query& query,
                            const std::vector<const Array*> &objects,
                            std::vector< std::vector<unsigned char> > &values,
                            bool &hasSolution) {
  TimerStatIncrementer t(stats::queryTime);

  ++stats::queries;
  ++stats::queryCounterexamples;

  unsigned char *pos = (unsigned char*) pthread_getspecific(shmSegmentKey);
  if (pos == NULL) {
    int shmID = shmget(IPC_PRIVATE, SHARED_MEM_SIZE, IPC_CREAT | 0700);
    assert(shmID>=0 && "shmget failed");

    pos = (unsigned char*) shmat(shmID, NULL, 0);
    assert(pos!=(void*)-1 && "shmat failed");
    shmctl(shmID, IPC_RMID, NULL);

    pthread_setspecific(shmSegmentKey, pos);
  }

  bool success = false;
  int pid = 0;
  Timer timer;

  // Do some checks before forking...
  if (useForkedSTP) {
    unsigned sum = 0;
    for (std::vector<const Array*>::const_iterator
                   it = objects.begin(), ie = objects.end(); it != ie; ++it)
          sum += (*it)->size;

    assert(sum<SHARED_MEM_SIZE && "not enough shared memory for counterexample");

    // Now fork...
    fflush(stdout);
    fflush(stderr);

    pid = fork();
  }

  // Handle error cases
  if (pid==-1) {
    // This can happen only if we forked
    if(errno == EAGAIN)
      fprintf(stderr, "error: fork failed (for STP) - reached system limit (EAGAIN)\n");
    else
      if(errno == ENOMEM) {
              unsigned mbs = sys::Process::GetTotalMemoryUsage() >> 20;
              fprintf(stderr, "error: fork failed (for STP) - cannot allocate kernel structures (ENOMEM) - Klee mem = %u\n", mbs);
      }
      else
              fprintf(stderr, "error: fork failed (for STP) - unknown errno = %d\n", errno);

    std::string line;
    std::ifstream *f;
    f = new std::ifstream("/proc/meminfo");
    if (!f) {
      fprintf(stderr, "out of memory");
    } else if (!f->good()) {
      fprintf(stderr, "error opening /proc/meminfo");
      delete f;
      f = NULL;
    }

    std::getline(*f, line);
    fprintf(stderr,  "%s\n", line.c_str());
    std::getline(*f, line);
    fprintf(stderr, "%s\n", line.c_str());

    f->close();

    return false;
  }

  if (pid == 0 || !useForkedSTP) {
    // We are now in the child...
    vc_push(vc);

    // Construct the data structures for STP...
    for (ConstraintManager::const_iterator it = query.constraints.begin(),
             ie = query.constraints.end(); it != ie; ++it)
    vc_assertFormula(vc, builder->construct(*it));

    ExprHandle stp_e = builder->construct(query.expr);


    if (timeout && useForkedSTP) {
      ::alarm(0); /* Turn off alarm so we can safely set signal handler */
      ::signal(SIGALRM, stpTimeoutHandler);
      ::alarm(std::max(1, (int)timeout));
    }

    if (enableLogging && !useForkedSTP) timer.start();

    unsigned res = vc_query(vc, stp_e);

    if (enableLogging && !useForkedSTP) timer.stop();


    if (!res) {
      if (useForkedSTP) {
        // Use the shared memory mechanism...
        for (std::vector<const Array*>::const_iterator
                   it = objects.begin(), ie = objects.end(); it != ie; ++it) {
          const Array *array = *it;
          for (unsigned offset = 0; offset < array->size; offset++) {
            ExprHandle counter =
                  vc_getCounterExample(vc, builder->getInitialRead(array, offset));
            *pos++ = getBVUnsigned(counter);
          }
        }
      } else {
        // Write the solution directly...
        hasSolution = true;

        values.reserve(objects.size());
        for (std::vector<const Array*>::const_iterator
               it = objects.begin(), ie = objects.end(); it != ie; ++it) {
          const Array *array = *it;
          std::vector<unsigned char> data;

          data.reserve(array->size);
          for (unsigned offset = 0; offset < array->size; offset++) {
            ExprHandle counter =
              vc_getCounterExample(vc, builder->getInitialRead(array, offset));
            unsigned char val = getBVUnsigned(counter);
            data.push_back(val);
          }

          values.push_back(data);
        }
      }
    } else if (!useForkedSTP) {
      hasSolution = false;
    }

    vc_pop(vc);

    if (useForkedSTP) {
      // Nothing to do in the child... pass the control back to the parent
      _exit(res);
    }
  } else {
    int status;
    pid_t res;
    int exitcode;

    pthread_mutex_lock(&mutex);
    solverInstances.insert(pid);
    pthread_mutex_unlock(&mutex);

    if (enableLogging) timer.start();

    do {
      res = waitpid(pid, &status, 0);
    } while (res < 0 && errno == EINTR);

    if (enableLogging) timer.stop();

#warning There is a subtle race condition here: you cannot call kill() on \
         a PID after you called waitpid() on it before

    pthread_mutex_lock(&mutex);
    solverInstances.erase(pid);
    pthread_mutex_unlock(&mutex);

    if (res < 0) {
      fprintf(stderr, "error: waitpid() for STP failed\n");
      success = false;
      goto finalize;
    }

    // From timed_run.py: It appears that linux at least will on
    // "occasion" return a status when the process was terminated by a
    // signal, so test signal first.
    if (WIFSIGNALED(status) || !WIFEXITED(status)) {
      if (WIFSIGNALED(status) && WTERMSIG(status) == 9) {
        // The solver was canceled
      } else {
        fprintf(stderr, "error: STP did not return successfully\n");
      }
      success = false;
      goto finalize;
    }

    exitcode = WEXITSTATUS(status);
    if (exitcode==0) {
      hasSolution = true;
    } else if (exitcode==1) {
      hasSolution = false;
    } else if (exitcode==52) {
      fprintf(stderr, "error: STP timed out\n");
      success = false;
      goto finalize;
    } else {
      fprintf(stderr, "error: STP did not return a recognized code\n");
      success = false;
      goto finalize;
    }

    if (hasSolution) {
      values = std::vector< std::vector<unsigned char> >(objects.size());
      unsigned i=0;
      for (std::vector<const Array*>::const_iterator
                     it = objects.begin(), ie = objects.end(); it != ie; ++it) {
            const Array *array = *it;
            std::vector<unsigned char> &data = values[i++];
            data.insert(data.begin(), pos, pos + array->size);
            pos += array->size;
      }
    }
  }

  // Control flow joins here - this is code executed both when forking and
  // when solving in the same process
  if (enableLogging) {
    cloud9::instrum::theInstrManager.recordEventAttribute(cloud9::instrum::SMTSolve,
        cloud9::instrum::SolvingResult, (int)hasSolution);
    cloud9::instrum::theInstrManager.recordEvent(cloud9::instrum::SMTSolve, timer);
  }


  success = true;

finalize:

  if (success) {
        if (hasSolution)
          ++stats::queriesInvalid;
        else
          ++stats::queriesValid;
  }

  return success;
}

void STPSolverImpl::cancelPendingJobs() {
  if (!useForkedSTP)
    return; // Really nothing to do, a single instance is running at a time

  pthread_mutex_lock(&mutex);

  for (std::set<pid_t>::iterator it = solverInstances.begin();
      it != solverInstances.end(); it++) {
    int res = kill(*it, SIGKILL);
    if (res == -1) {
      assert(errno == ESRCH);
    }
  }

  pthread_mutex_unlock(&mutex);
}

/*
bool
STPSolverImpl::computeInitialValues(const Query &query,
                                    const std::vector<const Array*>
                                      &objects,
                                    std::vector< std::vector<unsigned char> >
                                      &values,
                                    bool &hasSolution) {
	return computeInitialValues(query, objects, values, hasSolution,
			defaultShMem);
}
*/
