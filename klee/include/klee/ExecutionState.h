//===-- ExecutionState.h ----------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_EXECUTIONSTATE_H
#define KLEE_EXECUTIONSTATE_H

#include "klee/Constraints.h"
#include "klee/Expr.h"
#include "klee/Internal/ADT/TreeStream.h"

// FIXME: We do not want to be exposing these? :(
#include "../../lib/Core/AddressSpace.h"
#include "klee/Internal/Module/KInstIterator.h"

#if (LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 9)
#include "llvm/System/TimeValue.h"
#else
#include "llvm/Support/TimeValue.h"
#endif

#include "klee/Threading.h"
#include "klee/MultiProcess.h"
#include "klee/AddressPool.h"
#include "klee/StackTrace.h"

#include <map>
#include <set>
#include <vector>
#include <list>

using namespace llvm;

namespace cloud9 {
namespace worker {
class SymbolicState;
}
}

namespace llvm {
class Instruction;
}

namespace klee {
  class Array;
  class CallPathNode;
  struct Cell;
  struct KFunction;
  struct KInstruction;
  class MemoryObject;
  class PTreeNode;
  struct InstructionInfo;
  class Executor;

  class ExecutionState;

namespace c9 {
std::ostream &printStateStack(std::ostream &os, const ExecutionState &state);
std::ostream &printStateTopoIndex(std::ostream &os, const ExecutionState &state);
std::ostream &printStateConstraints(std::ostream &os, const ExecutionState &state);
std::ostream &printStateMemorySummary(std::ostream &os, const ExecutionState &state);
}

std::ostream &operator<<(std::ostream &os, const ExecutionState &state); // XXX Cloud9 hack
std::ostream &operator<<(std::ostream &os, const MemoryMap &mm);

typedef uint64_t wlist_id_t;

/*
struct StackFrame {
  KInstIterator caller;
  KFunction *kf;
  CallPathNode *callPathNode;

  std::vector<const MemoryObject*> allocas;
  Cell *locals;

  /// Minimum distance to an uncovered instruction once the function
  /// returns. This is not a good place for this but is used to
  /// quickly compute the context sensitive minimum distance to an
  /// uncovered instruction. This value is updated by the StatsTracker
  /// periodically.
  unsigned minDistToUncoveredOnReturn;

  // For vararg functions: arguments not passed via parameter are
  // stored (packed tightly) in a local (alloca) memory object. This
  // is setup to match the way the front-end generates vaarg code (it
  // does not pass vaarg through as expected). VACopy is lowered inside
  // of intrinsic lowering.
  MemoryObject *varargs;

  /// A stack of execution indexes. An item at index 0 corresponds to the
  /// non-loop function code, each next item corresponds to one loop level.
  /// This is updated by special function handlers for loop instrumentation.
  std::vector<LoopExecIndex> execIndexStack;

  StackFrame(KInstIterator caller, uint32_t callerExecIndex, KFunction *kf);
  StackFrame(const StackFrame &s);
  ~StackFrame();
};*/

class ExecutionState {
	friend class ObjectState;

public:
  typedef std::vector<StackFrame> stack_ty;

  typedef std::map<thread_uid_t, Thread> threads_ty;
  typedef std::map<process_id_t, Process> processes_ty;
  typedef std::map<wlist_id_t, std::set<thread_uid_t> > wlists_ty;

private:
  // unsupported, use copy constructor
  ExecutionState &operator=(const ExecutionState&); 
  std::map< std::string, std::string > fnAliases;

  cloud9::worker::SymbolicState *c9State;

  void setupMain(KFunction *kf);
  void setupTime();
  void setupAddressPool();
public:
  /* System-level parameters */
  Executor *executor;

  bool fakeState;
  // Are we currently underconstrained?  Hack: value is size to make fake
  // objects.
  unsigned depth;

  // An upper bound of the number of paths merged in this one
  uint64_t multiplicity;
  // Exact number of paths merged in this one (requires KeepMergedDuplicates)
  uint64_t multiplicityExact;

  /// Disables forking, set by user code.
  bool forkDisabled;

  mutable double queryCost;
  double weight;

  TreeOStream pathOS, symPathOS;
  unsigned instsSinceCovNew;
  unsigned instsSinceFork;
  unsigned instsTotal;

  bool coveredNew;
  sys::TimeValue lastCoveredTime;

  void setCoveredNew() {
	  coveredNew = true;
	  lastCoveredTime = sys::TimeValue::now();
  }

  std::map<const std::string*, std::set<unsigned> > coveredLines;

  PTreeNode *ptreeNode;

  int crtForkReason;
  Instruction *crtSpecialFork;


  /// ordered list of symbolics: used to generate test cases. 
  //
  // FIXME: Move to a shared list structure (not critical).
  std::vector< std::pair<const MemoryObject*, const Array*> > symbolics;
  uint64_t symbolicsHash;

  ConstraintManager globalConstraints;


  // For a multi threaded ExecutionState
  threads_ty threads;
  processes_ty processes;

  wlists_ty waitingLists;
  wlist_id_t wlistCounter;

  uint64_t stateTime;

  AddressPool addressPool;
  AddressSpace::cow_domain_t cowDomain;

  Thread& createThread(thread_id_t tid, KFunction *kf);
  Process& forkProcess(process_id_t pid);
  void terminateThread(threads_ty::iterator it);
  void terminateProcess(processes_ty::iterator it);

  threads_ty::iterator nextThread(threads_ty::iterator it) {
    if (it == threads.end())
      it = threads.begin();
    else {
      it++;
      if (it == threads.end())
        it = threads.begin();
    }

    crtProcessIt = processes.find(crtThreadIt->second.getPid());

    return it;
  }

  void scheduleNext(threads_ty::iterator it) {
    //CLOUD9_DEBUG("New thread scheduled: " << it->second.tid << " (pid: " << it->second.pid << ")");
    assert(it != threads.end());

    crtThreadIt = it;
    crtProcessIt = processes.find(crtThreadIt->second.getPid());
  }

  wlist_id_t getWaitingList() { return wlistCounter++; }
  void sleepThread(wlist_id_t wlist);
  void notifyOne(wlist_id_t wlist, thread_uid_t tid);
  void notifyAll(wlist_id_t wlist);

  threads_ty::iterator crtThreadIt;
  processes_ty::iterator crtProcessIt;

  unsigned int preemptions;


  /* Shortcut methods */

  Thread &crtThread() { return crtThreadIt->second; }
  const Thread &crtThread() const { return crtThreadIt->second; }

  Process &crtProcess() { return crtProcessIt->second; }
  const Process &crtProcess() const { return crtProcessIt->second; }

  ConstraintManager &constraints() { return globalConstraints; }
  const ConstraintManager &constraints() const { return globalConstraints; }

  AddressSpace &addressSpace() { return crtProcess().addressSpace; }
  const AddressSpace &addressSpace() const { return crtProcess().addressSpace; }

  const KInstIterator& pc() const { return crtThread().pc; }
  void setPC(const KInstIterator& newPC);

  const KInstIterator& prevPC() const { return crtThread().prevPC; }
  void setPrevPC(const KInstIterator& newPrevPC) { crtThread().prevPC = newPrevPC; }

  stack_ty& stack() { return crtThread().stack; }
  const stack_ty& stack() const { return crtThread().stack; }

  std::string getFnAlias(std::string fn);
  void addFnAlias(std::string old_fn, std::string new_fn);
  void removeFnAlias(std::string fn);

  /* State merging */
  uint64_t interleavedMergeIndex;

  bool isPCCompatible(const ExecutionState &b) const;
  bool areQCEMemoryTrackMapsCompatible(const ExecutionState &b) const;

  /* Duplicate states management */
  std::set<ExecutionState*> duplicates;
  bool isDuplicate;

  const MemoryObject* lastResolveResult;

public:
  ExecutionState(Executor *_executor, KFunction *kf);

  // XXX total hack, just used to make a state so solver can
  // use on structure
  ExecutionState(Executor *_executor, const std::vector<ref<Expr> > &assumptions);

  ~ExecutionState();
  
  ExecutionState *branch(bool copy = false);

  void pushFrame(Thread &t, KInstIterator caller, KFunction *kf) {
    t.stack.push_back(StackFrame(caller, t.mergeIndex, kf,
                                 !t.stack.empty() ? &t.stack.back() : NULL));
    t.topoIndex.push_back(TopoFrame(uint64_t(-1), 0));
  }
  void pushFrame(KInstIterator caller, KFunction *kf) {
    pushFrame(crtThread(), caller, kf);
  }

  void popFrame(Thread &t) {
    StackFrame &sf = t.stack.back();
    for (std::vector<const MemoryObject*>::iterator it = sf.allocas.begin(),
           ie = sf.allocas.end(); it != ie; ++it) {
      processes.find(t.getPid())->second.addressSpace.unbindObject(*it);
    }
    t.stack.pop_back();
    t.topoIndex.pop_back();
  }
  void popFrame() {
    popFrame(crtThread());
  }

  void addSymbolic(const MemoryObject *mo, const Array *array) { 
    symbolics.push_back(std::make_pair(mo, array));
    symbolicsHash = hashUpdate(symbolicsHash, (uintptr_t) mo);
  }
  void addConstraint(ref<Expr> e) { 
    constraints().addConstraint(e);
  }

  ExecutionState* merge(const ExecutionState &b, bool copy = false);
  bool mergeDisabled() const;

  cloud9::worker::SymbolicState *getCloud9State() const { return c9State; }
  void setCloud9State(cloud9::worker::SymbolicState *state) { c9State = state; }

  StackTrace getStackTrace() const;

  uint64_t getMergeIndex() const { return interleavedMergeIndex; }
  const TopoIndex& getTopoIndex() const { return crtThread().topoIndex; }

  // Merge blacklist functionality
  void updateMemoryUseFrequency(llvm::Instruction *inst,
                               ref<ConstantExpr> address, uint64_t size,
                               uint64_t useFreq, uint64_t totalUseFreq);

};

}

#endif
