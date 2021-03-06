//===-- Executor.h ----------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Class to perform actual execution, hides implementation details from external
// interpreter.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_EXECUTOR_H
#define KLEE_EXECUTOR_H

#include "klee/ExecutionState.h"
#include "klee/Interpreter.h"
#include "klee/Expr.h"
#include "klee/ForkTag.h"
#include "klee/util/Ref.h"
#include "klee/Internal/Module/Cell.h"
#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/KModule.h"
#include "llvm/Support/CallSite.h"
#include "cloud9/worker/SymbolicEngine.h"
#include "cloud9/Logger.h"
#include <vector>
#include <string>
#include <map>
#include <set>

//#define VERIFY_QCE_MAPS

struct KTest;

namespace llvm {
  class BasicBlock;
  class BranchInst;
  class CallInst;
  class Constant;
  class ConstantExpr;
  class Function;
  class GlobalValue;
  class Instruction;
  class TargetData;
  class Twine;
  class Value;
}

namespace klee {  
  class Array;
  struct Cell;
  class ExecutionState;
  class ExternalDispatcher;
  class Expr;
  class InstructionInfoTable;
  struct KFunction;
  struct KInstruction;
  class KInstIterator;
  class KModule;
  class MemoryManager;
  class MemoryObject;
  class ObjectState;
  class PTree;
  class Searcher;
  class SeedInfo;
  class SpecialFunctionHandler;
  struct StackFrame;
  class StatsTracker;
  class TimingSolver;
  class Solver;
  class STPSolver;
  class PCLoggingSolver;
  class TreeStreamWriter;

  /// \todo Add a context object to keep track of data only live
  /// during an instruction step. Should contain addedStates,
  /// removedStates, and haltExecution, among others.

class Executor : public Interpreter, public ::cloud9::worker::SymbolicEngine {
  friend class LazyMergingSearcher;
  friend class BumpMergingSearcher;
  friend class StaticMergingSearcher;
  friend class MergingSearcher;
  friend class RandomPathSearcher;
  friend class OwningSearcher;
  friend class WeightedRandomSearcher;
  friend class ForkCapSearcher;
  friend class SpecialFunctionHandler;
  friend class StatsTracker;

  friend class ObjectState;

public:
  class Timer {
  public:
    Timer();
    virtual ~Timer();

    /// The event callback.
    virtual void run() = 0;
  };

  typedef std::pair<ExecutionState*,ExecutionState*> StatePair;

private:
  class TimerInfo;

  KModule *kmodule;
  InterpreterHandler *interpreterHandler;
  Searcher *searcher;

  ExternalDispatcher *externalDispatcher;
  TimingSolver *solver;
  std::vector<Solver*> loggingSolvers;
  MemoryManager *memory;
  std::set<ExecutionState*> states;
  StatsTracker *statsTracker;
  TreeStreamWriter *pathWriter, *symPathWriter;
  SpecialFunctionHandler *specialFunctionHandler;
  std::vector<TimerInfo*> timers;
  PTree *processTree;

  /// Used to track states that have been added during the current
  /// instructions step. 
  /// \invariant \ref addedStates is a subset of \ref states. 
  /// \invariant \ref addedStates and \ref removedStates are disjoint.
  std::set<ExecutionState*> addedStates;
  /// Used to track states that have been removed during the current
  /// instructions step. 
  /// \invariant \ref removedStates is a subset of \ref states. 
  /// \invariant \ref addedStates and \ref removedStates are disjoint.
  std::set<ExecutionState*> removedStates;

  /// When non-empty the Executor is running in "seed" mode. The
  /// states in this map will be executed in an arbitrary order
  /// (outside the normal search interface) until they terminate. When
  /// the states reach a symbolic branch then either direction that
  /// satisfies one or more seeds will be added to this map. What
  /// happens with other states (that don't satisfy the seeds) depends
  /// on as-yet-to-be-determined flags.
  std::map<ExecutionState*, std::vector<SeedInfo> > seedMap;
  
  /// Map of globals to their representative memory object.
  std::map<const llvm::GlobalValue*, MemoryObject*> globalObjects;

  /// Map of globals to their bound address. This also includes
  /// globals that have no representative object (i.e. functions).
  std::map<const llvm::GlobalValue*, ref<ConstantExpr> > globalAddresses;

  /// The set of legal function addresses, used to validate function
  /// pointers. We use the actual Function* address as the function address.
  std::set<uint64_t> legalFunctions;

  /// When non-null the bindings that will be used for calls to
  /// klee_make_symbolic in order replay.
  const struct KTest *replayOut;
  /// When non-null a list of branch decisions to be used for replay.
  const std::vector<bool> *replayPath;
  /// The index into the current \ref replayOut or \ref replayPath
  /// object.
  unsigned replayPosition;

  /// When non-null a list of "seed" inputs which will be used to
  /// drive execution.
  const std::vector<struct KTest *> *usingSeeds;  

  /// Disables forking, instead a random path is chosen. Enabled as
  /// needed to control memory usage. \see fork()
  bool atMemoryLimit;

  /// Disables forking, set by client. \see setInhibitForking()
  bool inhibitForking;

  /// Signals the executor to halt execution at the next instruction
  /// step.
  bool haltExecution;  

  /// Whether implied-value concretization is enabled. Currently
  /// false, it is buggy (it needs to validate its writes).
  bool ivcEnabled;

  /// The maximum time to allow for a single stp query.
  double stpTimeout;

  std::ostream* constraintsLog;

  /// Hack to try the same concretization in duplicates
  mutable std::vector< ref<Expr> > getValuePreferences;

  Solver *constructSolverChain(STPSolver *stpSolver,
                               std::string queryLogPath,
                               std::string stpQueryLogPath,
                               std::string queryPCLogPath,
                               std::string stpQueryPCLogPath);

  llvm::Function* getCalledFunction(llvm::CallSite &cs, ExecutionState &state);
  
  void executeInstruction(ExecutionState &state, KInstruction *ki);

  void printFileLine(ExecutionState &state, KInstruction *ki);

  void run(ExecutionState &initialState);

  // QCE tracking function
  void dumpQceMap(ExecutionState &state);

#ifdef VERIFY_QCE_MAPS
  void verifyQceMap(ExecutionState &state);
#else
  void verifyQceMap(ExecutionState &state) {}
#endif

  void updateQceMapOnExec(ExecutionState &state);
  void updateQceMapBeforeCall(ExecutionState &state);
  void updateQceMapOnFramePush(ExecutionState &state);
  void updateQceMapOnFramePop(ExecutionState &state);
  void updateQceMapOnFree(ExecutionState &state,
                          const MemoryObject *mo, KInstruction *ki = NULL);

  bool modifyQceMemoryTrackMap(ExecutionState &state, const HotValue &hotValue,
                               int vnumber, bool inVhAdd,
                               const char *reason = NULL,
                               KInstruction *ki = NULL);

  void updateQceMemoryValue(ExecutionState &state,
                            const MemoryObject *mo, ObjectState *os,
                            ref<Expr> offset, ref<Expr> newValue,
                            KInstruction *ki = NULL);

  bool modifyQceLocalsTrackMap(ExecutionState &state, const HotValue &hotValue,
                               StackFrame &sf, int vnumber, bool inVhAdd,
                               const char *reason = NULL,
                               KInstruction *ki = NULL);

  void updateQceLocalsValue(ExecutionState &state,
                            int vnumber, ref<Expr> &newValue,
                            KInstruction *ki = NULL);

  // Given a concrete object in our [klee's] address space, add it to
  // objects checked code can reference.
  MemoryObject *addExternalObject(ExecutionState &state, void *addr, 
                                  unsigned size, bool isReadOnly, const char* name);

  void initializeGlobalObject(ExecutionState &state, ObjectState *os, 
			      llvm::Constant *c,
			      unsigned offset);
  void initializeGlobals(ExecutionState &state);

  void stepInstruction(ExecutionState &state, bool trackInstr);
  void updateStates(ExecutionState *current);
  void transferToBasicBlock(llvm::BasicBlock *dst, 
			    llvm::BasicBlock *src,
			    ExecutionState &state);

  void callExternalFunction(ExecutionState &state,
                            KInstruction *target,
                            llvm::Function *function,
                            std::vector< ref<Expr> > &arguments);

  void callUnmodelledFunction(ExecutionState &state,
                            KInstruction *target,
                            llvm::Function *function,
                            std::vector<ref<Expr> > &arguments);

  ObjectState *bindObjectInState(ExecutionState &state, const MemoryObject *mo,
                                 bool isLocal, const Array *array = 0);

  /// Resolve a pointer to the memory objects it could point to the
  /// start of, forking execution when necessary and generating errors
  /// for pointers to invalid locations (either out of bounds or
  /// address inside the middle of objects).
  ///
  /// \param results[out] A list of ((MemoryObject,ObjectState),
  /// state) pairs for each object the given address can point to the
  /// beginning of.
  typedef std::vector< std::pair<std::pair<const MemoryObject*, const ObjectState*>, 
                                 ExecutionState*> > ExactResolutionList;

  void resolveExact(ExecutionState &state,
                    ref<Expr> p,
                    ExactResolutionList &results,
                    const std::string &name);

  /// Allocate and bind a new object in a particular state. NOTE: This
  /// function may fork.
  ///
  /// \param isLocal Flag to indicate if the object should be
  /// automatically deallocated on function return (this also makes it
  /// illegal to free directly).
  ///
  /// \param target Value at which to bind the base address of the new
  /// object.
  ///
  /// \param reallocFrom If non-zero and the allocation succeeds,
  /// initialize the new object from the given one and unbind it when
  /// done (realloc semantics). The initialized bytes will be the
  /// minimum of the size of the old and new objects, with remaining
  /// bytes initialized as specified by zeroMemory.
  void executeAlloc(ExecutionState &state,
                    ref<Expr> size,
                    bool isLocal,
                    KInstruction *target,
                    bool zeroMemory=false,
                    const ObjectState *reallocFrom=0);

  /// Free the given address with checking for errors. If target is
  /// given it will be bound to 0 in the resulting states (this is a
  /// convenience for realloc). Note that this function can cause the
  /// state to fork and that \ref state cannot be safely accessed
  /// afterwards.
  void executeFree(ExecutionState &state,
                   ref<Expr> address,
                   KInstruction *target = 0);
  
  void executeCall(ExecutionState &state, 
                   KInstruction *ki,
                   llvm::Function *f,
                   std::vector< ref<Expr> > &arguments);
                   
  // do address resolution / object binding / out of bounds checking
  // and perform the operation
  void executeMemoryOperation(ExecutionState &state,
                              bool isWrite,
                              ref<Expr> address,
                              ref<Expr> value /* undef if read */,
                              KInstruction *target /* undef if write */);

  void executeMakeSymbolic(ExecutionState &state, const MemoryObject *mo,
      bool shared=false);

  void executeEvent(ExecutionState &state, unsigned int type,
      long int value);

  /// Create a new state where each input condition has been added as
  /// a constraint and return the results. The input state is included
  /// as one of the results. Note that the output vector may included
  /// NULL pointers for states which were unable to be created.
  void branch(ExecutionState &state, 
              const std::vector< ref<Expr> > &conditions,
              std::vector<ExecutionState*> &result, int reason);

  // Fork current and return states in which condition holds / does
  // not hold, respectively. One of the states is necessarily the
  // current state, and one of the states may be null.
  StatePair fork(ExecutionState &current, ref<Expr> condition, bool isInternal, int reason);
  StatePair fork(ExecutionState &current, int reason);
  ForkTag getForkTag(ExecutionState &current, int reason);

  void addDuplicates(ExecutionState *main, ExecutionState *other);

  /// Add the given (boolean) condition as a constraint on state. This
  /// function is a wrapper around the state's addConstraint function
  /// which also manages manages propogation of implied values,
  /// validity checks, and seed patching.
  void addConstraint(ExecutionState &state, ref<Expr> condition);

  // Called on [for now] concrete reads, replaces constant with a symbolic
  // Used for testing.
  ref<Expr> replaceReadWithSymbolic(ExecutionState &state, ref<Expr> e);

  const Cell& evalV(int vnumber, ExecutionState &state) const;
  const Cell& eval(KInstruction *ki, unsigned index,
                   ExecutionState &state) const;

  Cell& getArgumentCell(ExecutionState &state,
                        KFunction *kf,
                        unsigned index) {
    return state.stack().back().locals[kf->getArgRegister(index)];
  }

  Cell& getArgumentCell(StackFrame &sf, 
			KFunction *kf, 
			unsigned index) {
    return sf.locals[kf->getArgRegister(index)];
  }

  Cell& getDestCell(ExecutionState &state,
                    KInstruction *target) {
    return state.stack().back().locals[target->dest];
  }

  void bindLocal(KInstruction *target, 
                 ExecutionState &state, 
                 ref<Expr> value);
  void bindArgument(KFunction *kf, 
                    unsigned index,
                    ExecutionState &state,
                    ref<Expr> value);

  ref<klee::ConstantExpr> evalConstantExpr(llvm::ConstantExpr *ce);

  /// Return a unique constant value for the given expression in the
  /// given state, if it has one (i.e. it provably only has a single
  /// value). Otherwise return the original expression.
  ref<Expr> toUnique(const ExecutionState &state, ref<Expr> &e);

  /// Return a constant value for the given expression, forcing it to
  /// be constant in the given state by adding a constraint if
  /// necessary. Note that this function breaks completeness and
  /// should generally be avoided.
  ///
  /// \param purpose An identify string to printed in case of concretization.
  ref<klee::ConstantExpr> toConstant(ExecutionState &state, ref<Expr> e, 
                                     const char *purpose);

  /// Bind a constant value for e to the given target. NOTE: This
  /// function may fork state if the state has multiple seeds.
  void executeGetValue(ExecutionState &state, ref<Expr> e, KInstruction *target);

  /// Get textual information regarding a memory address.
  std::string getAddressInfo(ExecutionState &state, ref<Expr> address) const;

  // remove state from queue and delete
  bool terminateState(ExecutionState &state, bool silenced);
  // call exit handler and terminate state
  void terminateStateEarly(ExecutionState &state, const llvm::Twine &message);
  // call exit handler and terminate state
  void terminateStateOnExit(ExecutionState &state);
  // call error handler and terminate state
  void terminateStateOnError(ExecutionState &state, 
                             const llvm::Twine &message,
                             const char *suffix,
                             const llvm::Twine &longMessage="");

  // call error handler and terminate state, for execution errors
  // (things that should not be possible, like illegal instruction or
  // unlowered instrinsic, or are unsupported, like inline assembly)
  void terminateStateOnExecError(ExecutionState &state, 
                                 const llvm::Twine &message,
                                 const llvm::Twine &info="") {
    terminateStateOnError(state, message, "exec.err", info);
  }

  /// bindModuleConstants - Initialize the module constant table.
  void bindModuleConstants();

  template <typename TypeIt>
  void computeOffsets(KGEPInstruction *kgepi, TypeIt ib, TypeIt ie);

  /// bindInstructionConstants - Initialize any necessary per instruction
  /// constant values.
  void bindInstructionConstants(KInstruction *KI);

  void handlePointsToObj(ExecutionState &state, 
                         KInstruction *target, 
                         const std::vector<ref<Expr> > &arguments);

  void doImpliedValueConcretization(ExecutionState &state,
                                    ref<Expr> e,
                                    ref<ConstantExpr> value);

  /// Add a timer to be executed periodically.
  ///
  /// \param timer The timer object to run on firings.
  /// \param rate The approximate delay (in seconds) between firings.
  void addTimer(Timer *timer, double rate);

  void initTimers();
  void processTimers(ExecutionState *current,
                     double maxInstTime);
  void resetTimers();

  /// Pthread create needs a specific StackFrame instead of the one of the current state
  void bindArgumentToPthreadCreate(KFunction *kf, unsigned index, 
				   StackFrame &sf, ref<Expr> value);

  /// Finds the functions coresponding to an address.
  /// For now, it only support concrete values for the thread and function pointer argument.
  /// Can be extended easily to take care of symbolic function pointers.
  /// \param address address of the function pointer
  KFunction* resolveFunction(ref<Expr> address);


  //pthread handlers
  void executeThreadCreate(ExecutionState &state, thread_id_t tid,
      ref<Expr> start_function, ref<Expr> arg);

  void executeThreadExit(ExecutionState &state);
  
  void executeProcessExit(ExecutionState &state);
  
  void executeProcessFork(ExecutionState &state, KInstruction *ki,
      process_id_t pid);
  
  bool schedule(ExecutionState &state, bool yield);
  
  void executeThreadNotifyOne(ExecutionState &state, wlist_id_t wlist);

  void executeFork(ExecutionState &state, KInstruction *ki, int reason);

  void dumpProcessTree();
public:
  Executor(const InterpreterOptions &opts, InterpreterHandler *ie);
  virtual ~Executor();

  const InterpreterHandler& getHandler() {
    return *interpreterHandler;
  }

  // XXX should just be moved out to utility module
  ref<ConstantExpr> evalConstant(llvm::Constant *c);

  virtual void setPathWriter(TreeStreamWriter *tsw) {
    pathWriter = tsw;
  }
  virtual void setSymbolicPathWriter(TreeStreamWriter *tsw) {
    symPathWriter = tsw;
  }      

  virtual void setReplayOut(const struct KTest *out) {
    assert(!replayPath && "cannot replay both buffer and path");
    replayOut = out;
    replayPosition = 0;
  }

  virtual void setReplayPath(const std::vector<bool> *path) {
    assert(!replayOut && "cannot replay both buffer and path");
    replayPath = path;
    replayPosition = 0;
  }

  virtual const llvm::Module *
  setModule(llvm::Module *module, const ModuleOptions &opts);
  
  const KModule* getKModule() const {return kmodule;} 

  virtual void useSeeds(const std::vector<struct KTest *> *seeds) { 
    usingSeeds = seeds;
  }

  virtual void runFunctionAsMain(llvm::Function *f,
                                 int argc,
                                 char **argv,
                                 char **envp);

  /*** Runtime options ***/
  
  virtual void setHaltExecution(bool value) {
    haltExecution = value;
  }

  virtual void setInhibitForking(bool value) {
    inhibitForking = value;
  }

  /*** State accessor methods ***/

  virtual unsigned getPathStreamID(const ExecutionState &state);

  virtual unsigned getSymbolicPathStreamID(const ExecutionState &state);

  virtual void getConstraintLog(const ExecutionState &state,
                                std::string &res,
                                bool asCVC = false);

  virtual bool getSymbolicSolution(const ExecutionState &state, 
                                   std::vector< 
                                   std::pair<std::string,
                                   std::vector<unsigned char> > >
                                   &res);

  virtual void getCoveredLines(const ExecutionState &state,
                               std::map<const std::string*, std::set<unsigned> > &res);

  Expr::Width getWidthForLLVMType(const llvm::Type *type) const;

  /*** Cloud9 symbolic execution engine methods ***/

  virtual ExecutionState *createRootState(llvm::Function *f);
  virtual void initRootState(ExecutionState *state, int argc, char **argv, char **envp);

  virtual void stepInState(ExecutionState *state);

  virtual void destroyStates();

  virtual void destroyState(klee::ExecutionState *state);

  virtual klee::Searcher *initSearcher(klee::Searcher *base);

  virtual klee::KModule *getModule() {
	  return kmodule;
  }

  virtual ExecutionState* merge(ExecutionState &current, ExecutionState &other);

  //Hack for dynamic cast in CoreStrategies, TODO Solve it as soon as possible
  static bool classof(const SymbolicEngine* engine){ return true; }
};
  
} // End klee namespace

#endif
