//===-- Executor.cpp ------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Common.h"

#include "klee/Executor.h"
 
#include "Context.h"
#include "CoreStats.h"
#include "ExternalDispatcher.h"
#include "ImpliedValue.h"
#include "Memory.h"
#include "MemoryManager.h"
#include "PTree.h"
#include "SeedInfo.h"
#include "SpecialFunctionHandler.h"
#include "StatsTracker.h"
#include "TimingSolver.h"
#include "UserSearcher.h"
#include "../Solver/SolverStats.h"

#include "klee/ExecutionState.h"
#include "klee/Searcher.h"
#include "klee/Expr.h"
#include "klee/Interpreter.h"
#include "klee/TimerStatIncrementer.h"
#include "klee/util/Assignment.h"
#include "klee/util/ExprPPrinter.h"
#include "klee/util/ExprUtil.h"
#include "klee/util/GetElementPtrTypeIterator.h"
#include "klee/Config/config.h"
#include "klee/Internal/ADT/KTest.h"
#include "klee/Internal/ADT/RNG.h"
#include "klee/Internal/Module/Cell.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/KModule.h"
#include "klee/Internal/Support/FloatEvaluation.h"
#include "klee/Internal/System/Time.h"

#include "llvm/Attributes.h"
#include "llvm/BasicBlock.h"
#include "llvm/Constants.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/IntrinsicInst.h"
#if !(LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 7)
#include "llvm/LLVMContext.h"
#endif
#include "llvm/Module.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Debug.h"
#if (LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 9)
#include "llvm/System/Process.h"
#else
#include "llvm/Support/Process.h"
#endif
#include "llvm/Target/TargetData.h"

#include "cloud9/instrum/InstrumentationManager.h"

#include <cassert>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>

#include <sys/mman.h>

#include <errno.h>
#include <cxxabi.h>

#include <boost/foreach.hpp>
#define foreach BOOST_FOREACH

//using namespace llvm;
using namespace klee;

namespace {
  cl::opt<bool>
  DumpStatesOnHalt("dump-states-on-halt",
                   cl::init(true));
 
  cl::opt<bool>
  NoPreferCex("no-prefer-cex",
              cl::init(false));
 
  cl::opt<bool>
  UseAsmAddresses("use-asm-addresses",
                  cl::init(false));
 
  cl::opt<bool>
  RandomizeFork("randomize-fork",
                cl::init(false));
 
  cl::opt<bool>
  AllowExternalSymCalls("allow-external-sym-calls",
                        cl::init(false));

  cl::opt<bool>
  DebugPrintInstructions("debug-print-instructions",
                         cl::desc("Print instructions during execution."));

  cl::opt<bool>
  DebugCheckForImpliedValues("debug-check-for-implied-values");


  cl::opt<bool>
  SimplifySymIndices("simplify-sym-indices",
                     cl::init(false));

  cl::opt<unsigned>
  MaxSymArraySize("max-sym-array-size",
                  cl::init(0));

  cl::opt<bool>
  DebugValidateSolver("debug-validate-solver",
		      cl::init(false));

  cl::opt<bool>
  SuppressExternalWarnings("suppress-external-warnings");

  cl::opt<bool>
  AllExternalWarnings("all-external-warnings");

  cl::opt<bool>
  OnlyOutputStatesCoveringNew("only-output-states-covering-new",
                              cl::init(false));

  cl::opt<bool>
  AlwaysOutputSeeds("always-output-seeds",
                              cl::init(true));

  cl::opt<bool>
  UseFastCexSolver("use-fast-cex-solver",
		   cl::init(false));

  cl::opt<bool>
  UseIndependentSolver("use-independent-solver",
                       cl::init(true),
		       cl::desc("Use constraint independence"));

  cl::opt<bool>
  UseParallelSolver("use-parallel-solver",
      cl::init(false), cl::desc("Use parallel solver"));

  cl::opt<unsigned>
  ParallelSubqueriesDelay("parallel-subq-delay",
      cl::init(100), cl::desc("The delay in millisecs before the subqueries start to be computed"));

  cl::opt<bool>
  UseHLParallelSolver("use-hl-parallel-solver",
      cl::init(false), cl::desc("Use high-level parallel solver"));

  cl::opt<bool>
  EmitAllErrors("emit-all-errors",
                cl::init(false),
                cl::desc("Generate tests cases for all errors "
                         "(default=one per (error,instruction) pair)"));

  cl::opt<bool>
  UseCexCache("use-cex-cache",
              cl::init(true),
	      cl::desc("Use counterexample caching"));

  cl::opt<bool>
  UseQueryPCLog("use-query-pc-log",
                cl::init(false));
  
  cl::opt<bool>
  UseSTPQueryPCLog("use-stp-query-pc-log",
                   cl::init(false));

  cl::opt<bool>
  NoExternals("no-externals",
           cl::desc("Do not allow external functin calls"));

  cl::opt<bool>
  UseCache("use-cache",
	   cl::init(true),
	   cl::desc("Use validity caching"));

  cl::opt<bool>
  OnlyReplaySeeds("only-replay-seeds",
                  cl::desc("Discard states that do not have a seed."));
 
  cl::opt<bool>
  OnlySeed("only-seed",
           cl::desc("Stop execution after seeding is done without doing regular search."));
 
  cl::opt<bool>
  AllowSeedExtension("allow-seed-extension",
                     cl::desc("Allow extra (unbound) values to become symbolic during seeding."));
 
  cl::opt<bool>
  ZeroSeedExtension("zero-seed-extension");
 
  cl::opt<bool>
  AllowSeedTruncation("allow-seed-truncation",
                      cl::desc("Allow smaller buffers than in seeds."));
 
  cl::opt<bool>
  NamedSeedMatching("named-seed-matching",
                    cl::desc("Use names to match symbolic objects to inputs."));

  cl::opt<bool>
  DebugCallHistory("debug-call-history", cl::init(false));

  cl::opt<double>
  MaxStaticForkPct("max-static-fork-pct", cl::init(1.));
  cl::opt<double>
  MaxStaticSolvePct("max-static-solve-pct", cl::init(1.));
  cl::opt<double>
  MaxStaticCPForkPct("max-static-cpfork-pct", cl::init(1.));
  cl::opt<double>
  MaxStaticCPSolvePct("max-static-cpsolve-pct", cl::init(1.));

  cl::opt<double>
  MaxInstructionTime("max-instruction-time",
                     cl::desc("Only allow a single instruction to take this much time (default=0 (off))"),
                     cl::init(0));
  
  cl::opt<double>
  SeedTime("seed-time",
           cl::desc("Amount of time to dedicate to seeds, before normal search (default=0 (off))"),
           cl::init(0));
  
  cl::opt<double>
  MaxSTPTime("max-stp-time",
             cl::desc("Maximum amount of time for a single query (default=120s)"),
             cl::init(120.0));
  
  cl::opt<unsigned int>
  StopAfterNInstructions("stop-after-n-instructions",
                         cl::desc("Stop execution after specified number of instructions (0=off)"),
                         cl::init(0));
  
  cl::opt<unsigned>
  MaxForks("max-forks",
           cl::desc("Only fork this many times (-1=off)"),
           cl::init(~0u));
  
  cl::opt<unsigned>
  MaxDepth("max-depth",
           cl::desc("Only allow this many symbolic branches (0=off)"),
           cl::init(0));
  
  cl::opt<unsigned>
  MaxMemory("max-memory",
            cl::desc("Refuse to fork when more above this about of memory (in MB, 0=off)"),
            cl::init(0));

  cl::opt<bool>
  MaxMemoryInhibit("max-memory-inhibit",
            cl::desc("Inhibit forking at memory cap (vs. random terminate)"),
            cl::init(true));

  cl::opt<bool>
  UseForkedSTP("use-forked-stp",
                 cl::desc("Run STP in forked process"));

  cl::opt<bool>
  STPOptimizeDivides("stp-optimize-divides", 
                 cl::desc("Optimize constant divides into add/shift/multiplies before passing to STP"),
                 cl::init(true));

  cl::opt<unsigned int>
  MaxPreemptions("scheduler-preemption-bound",
		 cl::desc("scheduler preemption bound (default=0)"),
		 cl::init(0));
  
  cl::opt<bool>
  ForkOnSchedule("fork-on-schedule", 
		 cl::desc("fork when various schedules are possible (defaul=disabled)"),
		 cl::init(false));

  cl::opt<bool>
  DumpPTreeOnChange("dump-ptree-on-change",
          cl::desc("Dump PTree each time it changes"),
          cl::init(false));

  cl::opt<bool>
  KeepMergedDuplicates("keep-merged-duplicates",
          cl::desc("Keep execuring merged states as duplicates"));

  cl::opt<bool>
  OutputConstraints("output-constraints",
          cl::desc("Output path constratins for each explored state"),
          cl::init(false));

  /*
  cl::opt<bool>
  OutputForkedStatesConstraints("output-finished-states-constraints",
          cl::desc("Output path constratins for each finished state"),
          cl::init(false));
  */

  cl::opt<bool>
  DebugMergeSlowdown("debug-merge-slowdown",
          cl::desc("Debug slow-down of merged states"),
          cl::init(false));

  cl::opt<float>
  QceThreshold("qce-threshold", cl::init(1e-8));

  cl::opt<float>
  QceAbsThreshold("qce-abs-threshold", cl::init(0));

  cl::opt<bool>
  DebugQceMaps("debug-qce-maps", cl::init(false));
}

static void *theMMap = 0;
static unsigned theMMapSize = 0;

namespace klee {
  RNG theRNG;
}

Solver *Executor::constructSolverChain(STPSolver *stpSolver,
                             std::string queryLogPath,
                             std::string stpQueryLogPath,
                             std::string queryPCLogPath,
                             std::string stpQueryPCLogPath) {
  Solver *solver = stpSolver;

  if (UseParallelSolver) {
    assert(!UseHLParallelSolver);
    CLOUD9_DEBUG("Using the parallel solver...");
    solver = createParallelSolver(4, ParallelSubqueriesDelay, STPOptimizeDivides, stpSolver);
  }

  if (UseHLParallelSolver) {
    assert(!UseParallelSolver);
    assert(UseForkedSTP && "HLParallelSolver requires --use-forked-stp!");
    solver = createHLParallelSolver(solver, 0);
  }


  if (UseSTPQueryPCLog) {
    solver = createPCLoggingSolver(solver, 
                                   stpQueryLogPath);
    loggingSolvers.push_back(solver);
  }

  if (UseFastCexSolver)
    solver = createFastCexSolver(solver);

  if (UseCexCache)
    solver = createCexCachingSolver(solver);

  if (UseCache)
    solver = createCachingSolver(solver);

  if (UseIndependentSolver)
    solver = createIndependentSolver(solver);

  if (DebugValidateSolver)
    solver = createValidatingSolver(solver, stpSolver);

  if (UseQueryPCLog) {
    solver = createPCLoggingSolver(solver, 
                                   queryPCLogPath);
    loggingSolvers.push_back(solver);
  }
  
  return solver;
}

//namespace klee {

Executor::Executor(const InterpreterOptions &opts,
                   InterpreterHandler *ih) 
  : Interpreter(opts),
    kmodule(0),
    interpreterHandler(ih),
    searcher(0),
    externalDispatcher(new ExternalDispatcher()),
    statsTracker(0),
    pathWriter(0),
    symPathWriter(0),
    specialFunctionHandler(0),
    processTree(0),
    replayOut(0),
    replayPath(0),    
    usingSeeds(0),
    atMemoryLimit(false),
    inhibitForking(false),
    haltExecution(false),
    ivcEnabled(false),
    stpTimeout(MaxSTPTime != 0 && MaxInstructionTime != 0
	       ? std::min(MaxSTPTime,MaxInstructionTime)
	       : std::max(MaxSTPTime,MaxInstructionTime)) {

  STPSolver *stpSolver = new STPSolver(UseForkedSTP || UseParallelSolver, STPOptimizeDivides,
      !UseParallelSolver);

  Solver *solver = 
    constructSolverChain(stpSolver,
                         interpreterHandler->getOutputFilename("queries.qlog"),
                         interpreterHandler->getOutputFilename("stp-queries.qlog"),
                         interpreterHandler->getOutputFilename("queries.pc"),
                         interpreterHandler->getOutputFilename("stp-queries.pc"));
  
  this->solver = new TimingSolver(solver, stpSolver);

  memory = new MemoryManager();

  if (OutputConstraints) {
    constraintsLog = interpreterHandler->openOutputFile("constraints.log");
    assert(constraintsLog);
  }
}


const Module *Executor::setModule(llvm::Module *module, 
                                  const ModuleOptions &opts) {
  assert(!kmodule && module && "can only register one module"); // XXX gross
  
  kmodule = new KModule(module);

  // Initialize the context.
  TargetData *TD = kmodule->targetData;
  Context::initialize(TD->isLittleEndian(),
                      (Expr::Width) TD->getPointerSizeInBits());

  specialFunctionHandler = new SpecialFunctionHandler(*this);

  specialFunctionHandler->prepare();
  kmodule->prepare(opts, interpreterHandler,
                   userSearcherRequiresMergeAnalysis());
  specialFunctionHandler->bind();

  if (StatsTracker::useStatistics()) {
    statsTracker = 
      new StatsTracker(*this,
                       interpreterHandler->getOutputFilename("assembly.ll"),
                       userSearcherRequiresMD2U());
  }
  
  return module;
}

Executor::~Executor() {
  delete memory;
  delete externalDispatcher;
  if (processTree)
    delete processTree;
  if (specialFunctionHandler)
    delete specialFunctionHandler;
  if (statsTracker)
    delete statsTracker;
  delete solver;
  delete kmodule;
}

/***/

void Executor::initializeGlobalObject(ExecutionState &state, ObjectState *os,
                                      Constant *c, 
                                      unsigned offset) {
  TargetData *targetData = kmodule->targetData;
  if (ConstantVector *cp = dyn_cast<ConstantVector>(c)) {
    unsigned elementSize =
      targetData->getTypeStoreSize(cp->getType()->getElementType());
    for (unsigned i=0, e=cp->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, cp->getOperand(i), 
			     offset + i*elementSize);
  } else if (isa<ConstantAggregateZero>(c)) {
    unsigned i, size = targetData->getTypeStoreSize(c->getType());
    for (i=0; i<size; i++)
      os->write8(offset+i, (uint8_t) 0);
  } else if (ConstantArray *ca = dyn_cast<ConstantArray>(c)) {
    unsigned elementSize =
      targetData->getTypeStoreSize(ca->getType()->getElementType());
    for (unsigned i=0, e=ca->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, ca->getOperand(i), 
			     offset + i*elementSize);
  } else if (ConstantStruct *cs = dyn_cast<ConstantStruct>(c)) {
    const StructLayout *sl =
      targetData->getStructLayout(cast<StructType>(cs->getType()));
    for (unsigned i=0, e=cs->getNumOperands(); i != e; ++i)
      initializeGlobalObject(state, os, cs->getOperand(i), 
			     offset + sl->getElementOffset(i));
  } else {
    unsigned StoreBits = targetData->getTypeStoreSizeInBits(c->getType());
    ref<ConstantExpr> C = evalConstant(c);

    // Extend the constant if necessary;
    assert(StoreBits >= C->getWidth() && "Invalid store size!");
    if (StoreBits > C->getWidth())
      C = C->ZExt(StoreBits);

    os->write(offset, C);
  }
}

MemoryObject * Executor::addExternalObject(ExecutionState &state, 
                                           void *addr, unsigned size, 
                                           bool isReadOnly,
                                           const char* name) {
  MemoryObject *mo = memory->allocateFixed((uint64_t) (unsigned long) addr, 
                                           size, 0, name);
  ObjectState *os = bindObjectInState(state, mo, false);
  for(unsigned i = 0; i < size; i++)
    os->write8(i, ((uint8_t*)addr)[i]);
  if(isReadOnly)
    os->setReadOnly(true);  
  return mo;
}

void Executor::initializeGlobals(ExecutionState &state) {
  Module *m = kmodule->module;

  if (m->getModuleInlineAsm() != "")
    klee_warning("executable has module level assembly (ignoring)");

  //assert(m->lib_begin() == m->lib_end() &&
  //       "XXX do not support dependent libraries");

  // represent function globals using the address of the actual llvm function
  // object. given that we use malloc to allocate memory in states this also
  // ensures that we won't conflict. we don't need to allocate a memory object
  // since reading/writing via a function pointer is unsupported anyway.
  for (Module::iterator i = m->begin(), ie = m->end(); i != ie; ++i) {
    Function *f = i;
    ref<ConstantExpr> addr(0);

    // If the symbol has external weak linkage then it is implicitly
    // not defined in this module; if it isn't resolvable then it
    // should be null.
    if (f->hasExternalWeakLinkage() && 
        !externalDispatcher->resolveSymbol(f->getNameStr())) {
      addr = Expr::createPointer(0);
    } else {
      addr = Expr::createPointer((unsigned long) (void*) f);
      legalFunctions.insert((uint64_t) (unsigned long) (void*) f);
    }
    
    globalAddresses.insert(std::make_pair(f, addr));
  }

  // Disabled, we don't want to promote use of live externals.
#ifdef HAVE_CTYPE_EXTERNALS
#ifndef WINDOWS
#ifndef DARWIN
  /* From /usr/include/errno.h: it [errno] is a per-thread variable. */
  int *errno_addr = __errno_location();
  addExternalObject(state, (void *)errno_addr, sizeof *errno_addr, false, "errno_addr");

  /* from /usr/include/ctype.h:
       These point into arrays of 384, so they can be indexed by any `unsigned
       char' value [0,255]; by EOF (-1); or by any `signed char' value
       [-128,-1).  ISO C requires that the ctype functions work for `unsigned */
  const uint16_t **addr = __ctype_b_loc();
  addExternalObject(state, (void *)(*addr-128), 
                    384 * sizeof **addr, true, "__ctype_b_loc_m128");
  addExternalObject(state, addr, sizeof(*addr), true, "__ctype_b_loc");
    
  const int32_t **lower_addr = __ctype_tolower_loc();
  addExternalObject(state, (void *)(*lower_addr-128), 
                    384 * sizeof **lower_addr, true, "__ctype_tolower_loc_m128");
  addExternalObject(state, lower_addr, sizeof(*lower_addr), true, "__ctype_tolower_loc");
  
  const int32_t **upper_addr = __ctype_toupper_loc();
  addExternalObject(state, (void *)(*upper_addr-128), 
                    384 * sizeof **upper_addr, true, "__ctype_toupper_loc_m128");
  addExternalObject(state, upper_addr, sizeof(*upper_addr), true, "__ctype_toupper_loc");
#endif
#endif
#endif

  // allocate and initialize globals, done in two passes since we may
  // need address of a global in order to initialize some other one.

  // allocate memory objects for all globals
  for (Module::const_global_iterator i = m->global_begin(),
         e = m->global_end();
       i != e; ++i) {
    if (i->isDeclaration()) {
      // FIXME: We have no general way of handling unknown external
      // symbols. If we really cared about making external stuff work
      // better we could support user definition, or use the EXE style
      // hack where we check the object file information.

      const Type *ty = i->getType()->getElementType();
      uint64_t size = kmodule->targetData->getTypeStoreSize(ty);

      // XXX - DWD - hardcode some things until we decide how to fix.
#ifndef WINDOWS
      if (i->getName() == "_ZTVN10__cxxabiv117__class_type_infoE") {
        size = 0x2C;
      } else if (i->getName() == "_ZTVN10__cxxabiv120__si_class_type_infoE") {
        size = 0x2C;
      } else if (i->getName() == "_ZTVN10__cxxabiv121__vmi_class_type_infoE") {
        size = 0x2C;
      }
#endif

      if (size == 0) {
        llvm::errs() << "Unable to find size for global variable: " 
                     << i->getName() 
                     << " (use will result in out of bounds access)\n";
      }

      MemoryObject *mo = memory->allocate(&state, size, false, true, i);
      ObjectState *os = bindObjectInState(state, mo, false);
      globalObjects.insert(std::make_pair(i, mo));
      globalAddresses.insert(std::make_pair(i, mo->getBaseExpr()));

      // Program already running = object already initialized.  Read
      // concrete value and write it to our copy.
      if (size) {
        void *addr;
        if (i->getName() == "__dso_handle") {
          continue; // XXX
          //extern void *__dso_handle __attribute__ ((__weak__));
          //addr = &__dso_handle; // wtf ?
        } else {
          addr = externalDispatcher->resolveSymbol(i->getNameStr());
        }
        if (!addr)
          klee_error("unable to load symbol(%s) while initializing globals.", 
                     i->getName().data());

        for (unsigned offset=0; offset<mo->size; offset++)
          os->write8(offset, ((unsigned char*)addr)[offset]);
      }
    } else {
      const Type *ty = i->getType()->getElementType();
      uint64_t size = kmodule->targetData->getTypeStoreSize(ty);
      MemoryObject *mo = 0;

      if (UseAsmAddresses && i->getName()[0]=='\01') {
        char *end;
        uint64_t address = ::strtoll(i->getNameStr().c_str()+1, &end, 0);

        if (end && *end == '\0') {
          klee_message("NOTE: allocated global at asm specified address: %#08llx"
                       " (%llu bytes)",
                       (long long) address, (unsigned long long) size);
          mo = memory->allocateFixed(address, size, &*i, 0);
          mo->isUserSpecified = true; // XXX hack;
        }
      }

      if (!mo)
        mo = memory->allocate(&state, size, false, true, &*i);
      if(!mo)
	klee_message("cannot allocate memory for global %s", i->getNameStr().c_str());
      assert(mo && "out of memory");
      ObjectState *os = bindObjectInState(state, mo, false);
      globalObjects.insert(std::make_pair(i, mo));
      globalAddresses.insert(std::make_pair(i, mo->getBaseExpr()));

      if (!i->hasInitializer())
          os->initializeToRandom();
    }
  }
  
  // link aliases to their definitions (if bound)
  for (Module::alias_iterator i = m->alias_begin(), ie = m->alias_end(); 
       i != ie; ++i) {
    // Map the alias to its aliasee's address. This works because we have
    // addresses for everything, even undefined functions. 
    globalAddresses.insert(std::make_pair(i, evalConstant(i->getAliasee())));
  }

  // once all objects are allocated, do the actual initialization
  for (Module::const_global_iterator i = m->global_begin(),
         e = m->global_end();
       i != e; ++i) {
    if (i->hasInitializer()) {
      MemoryObject *mo = globalObjects.find(i)->second;
      const ObjectState *os = state.addressSpace().findObject(mo);
      assert(os);
      ObjectState *wos = state.addressSpace().getWriteable(mo, os);
      
      initializeGlobalObject(state, wos, i->getInitializer(), 0);
      // if(i->isConstant()) os->setReadOnly(true);
    }
  }
}

void Executor::branch(ExecutionState &state, 
                      const std::vector< ref<Expr> > &conditions,
                      std::vector<ExecutionState*> &result, int reason) {
  TimerStatIncrementer timer(stats::forkTime);
  unsigned N = conditions.size();
  assert(N);

  stats::forks += N-1;
  stats::forksMult += (N-1) * state.multiplicity;

  ForkTag tag = getForkTag(state, reason);

  // XXX do proper balance or keep random?
  result.push_back(&state);
  for (unsigned i=1; i<N; ++i) {
    ExecutionState *es = result[0]; // TODO: Replace with better code; result[theRNG.getInt32() % i];
    ExecutionState *ns = es->branch();

    addedStates.insert(ns);
    result.push_back(ns);
    es->ptreeNode->data = 0;
    std::pair<PTree::Node*,PTree::Node*> res = 
      processTree->split(es->ptreeNode, ns, es, conditions[i], tag);
    ns->ptreeNode = res.first;
    es->ptreeNode = res.second;

    fireStateBranched(ns, es, 0, tag);
  }

  if(DumpPTreeOnChange)
    dumpProcessTree();

  // If necessary redistribute seeds to match conditions, killing
  // states if necessary due to OnlyReplaySeeds (inefficient but
  // simple).
  
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
    seedMap.find(&state);
  if (it != seedMap.end()) {
    std::vector<SeedInfo> seeds = it->second;
    seedMap.erase(it);

    // Assume each seed only satisfies one condition (necessarily true
    // when conditions are mutually exclusive and their conjunction is
    // a tautology).
    for (std::vector<SeedInfo>::iterator siit = seeds.begin(), 
           siie = seeds.end(); siit != siie; ++siit) {
      unsigned i;
      for (i=0; i<N; ++i) {
        ref<ConstantExpr> res;
        bool success = 
          solver->getValue(state, siit->assignment.evaluate(conditions[i]), 
                           res);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;
        if (res->isTrue())
          break;
      }
      
      // If we didn't find a satisfying condition randomly pick one
      // (the seed will be patched).
      if (i==N)
        i = theRNG.getInt32() % N;

      seedMap[result[i]].push_back(*siit);
    }

    if (OnlyReplaySeeds) {
      for (unsigned i=0; i<N; ++i) {
        if (!seedMap.count(result[i])) {
          terminateState(*result[i], true);
          result[i] = NULL;
        }
      } 
    }
  }

  for (unsigned i=0; i<N; ++i)
    if (result[i])
      addConstraint(*result[i], conditions[i]);
}

Executor::StatePair 
Executor::fork(ExecutionState &current, ref<Expr> condition, bool isInternal,
    int reason) {
  Solver::Validity res;
  ForkTag tag = getForkTag(current, reason);

  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
    seedMap.find(&current);
  bool isSeeding = it != seedMap.end();

  //C9HACK_DEBUG("Fork requested: " << (isInternal ? "internal" : "external"), current);

  if (!isSeeding && !isa<ConstantExpr>(condition) && 
      (MaxStaticForkPct!=1. || MaxStaticSolvePct != 1. ||
       MaxStaticCPForkPct!=1. || MaxStaticCPSolvePct != 1.) &&
      statsTracker->elapsed() > 60.) {
    StatisticManager &sm = *theStatisticManager;
    CallPathNode *cpn = current.stack().back().callPathNode;
    if ((MaxStaticForkPct<1. &&
         sm.getIndexedValue(stats::forks, sm.getIndex()) > 
         stats::forks*MaxStaticForkPct) ||
        (MaxStaticCPForkPct<1. &&
         cpn && (cpn->statistics.getValue(stats::forks) > 
                 stats::forks*MaxStaticCPForkPct)) ||
        (MaxStaticSolvePct<1 &&
         sm.getIndexedValue(stats::solverTime, sm.getIndex()) > 
         stats::solverTime*MaxStaticSolvePct) ||
        (MaxStaticCPForkPct<1. &&
         cpn && (cpn->statistics.getValue(stats::solverTime) > 
                 stats::solverTime*MaxStaticCPSolvePct))) {
      ref<ConstantExpr> value; 
      bool success = solver->getValue(current, condition, value);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      CLOUD9_INFO("NONDETERMINISM! New constraint added!");
      addConstraint(current, EqExpr::create(value, condition));
      condition = value;
    }      
  }

  double timeout = stpTimeout;
  if (isSeeding)
    timeout *= it->second.size();
  solver->setTimeout(timeout);
  bool success = solver->evaluate(current, condition, res);
  solver->setTimeout(0);
  if (!success) {
    current.setPC(current.prevPC());
    terminateStateEarly(current, "query timed out");
    return StatePair((klee::ExecutionState*)NULL, (klee::ExecutionState*)NULL);
  }

  if (!isSeeding) {
    if (replayPath && !isInternal) {
      assert(replayPosition<replayPath->size() &&
             "ran out of branches in replay path mode");
      bool branch = (*replayPath)[replayPosition++];
      
      if (res==Solver::True) {
        assert(branch && "hit invalid branch in replay path mode");
      } else if (res==Solver::False) {
        assert(!branch && "hit invalid branch in replay path mode");
      } else {
        // add constraints
        if(branch) {
          res = Solver::True;
          addConstraint(current, condition);
        } else  {
          res = Solver::False;
          addConstraint(current, Expr::createIsZero(condition));
        }
      }
    } else if (res==Solver::Unknown) {
      assert(!replayOut && "in replay mode, only one branch can be true.");
      
      if ((MaxMemoryInhibit && atMemoryLimit) || 
          current.forkDisabled ||
          inhibitForking || 
          (MaxForks!=~0u && stats::forks >= MaxForks)) {

	if (MaxMemoryInhibit && atMemoryLimit)
	  klee_warning_once(0, "skipping fork (memory cap exceeded)");
	else if (current.forkDisabled)
	  klee_warning_once(0, "skipping fork (fork disabled on current path)");
	else if (inhibitForking)
	  klee_warning_once(0, "skipping fork (fork disabled globally)");
	else 
	  klee_warning_once(0, "skipping fork (max-forks reached)");

        TimerStatIncrementer timer(stats::forkTime);
        if (theRNG.getBool()) {
          addConstraint(current, condition);
          res = Solver::True;        
        } else {
          addConstraint(current, Expr::createIsZero(condition));
          res = Solver::False;
        }
      }
    }
  }

  // Fix branch in only-replay-seed mode, if we don't have both true
  // and false seeds.
  if (isSeeding && 
      (current.forkDisabled || OnlyReplaySeeds) && 
      res == Solver::Unknown) {
    bool trueSeed=false, falseSeed=false;
    // Is seed extension still ok here?
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(), 
           siie = it->second.end(); siit != siie; ++siit) {
      ref<ConstantExpr> res;
      bool success = 
        solver->getValue(current, siit->assignment.evaluate(condition), res);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      if (res->isTrue()) {
        trueSeed = true;
      } else {
        falseSeed = true;
      }
      if (trueSeed && falseSeed)
        break;
    }
    if (!(trueSeed && falseSeed)) {
      assert(trueSeed || falseSeed);
      
      res = trueSeed ? Solver::True : Solver::False;
      addConstraint(current, trueSeed ? condition : Expr::createIsZero(condition));
    }
  }


  // XXX - even if the constraint is provable one way or the other we
  // can probably benefit by adding this constraint and allowing it to
  // reduce the other constraints. For example, if we do a binary
  // search on a particular value, and then see a comparison against
  // the value it has been fixed at, we should take this as a nice
  // hint to just use the single constraint instead of all the binary
  // search ones. If that makes sense.
  if (res==Solver::True) {
    if (!isInternal) {
      if (pathWriter) {
        current.pathOS << "1";
      }
    }

    //fireStateBranched(NULL, &current, 0, tag);

    return StatePair(&current, (klee::ExecutionState*)NULL);
  } else if (res==Solver::False) {
    if (!isInternal) {
      if (pathWriter) {
        current.pathOS << "0";
      }
    }

    //fireStateBranched(NULL, &current, 1, tag);

    return StatePair((klee::ExecutionState*)NULL, &current);
  } else {
    TimerStatIncrementer timer(stats::forkTime);
    ExecutionState *falseState, *trueState = &current;

    ++stats::forks;
    stats::forksMult += current.multiplicity;

    falseState = trueState->branch();
    addedStates.insert(falseState);

    if (RandomizeFork && theRNG.getBool())
      std::swap(trueState, falseState);

    if (it != seedMap.end()) {
      std::vector<SeedInfo> seeds = it->second;
      it->second.clear();
      std::vector<SeedInfo> &trueSeeds = seedMap[trueState];
      std::vector<SeedInfo> &falseSeeds = seedMap[falseState];
      for (std::vector<SeedInfo>::iterator siit = seeds.begin(), 
             siie = seeds.end(); siit != siie; ++siit) {
        ref<ConstantExpr> res;
        bool success = 
          solver->getValue(current, siit->assignment.evaluate(condition), res);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;
        if (res->isTrue()) {
          trueSeeds.push_back(*siit);
        } else {
          falseSeeds.push_back(*siit);
        }
      }
      
      bool swapInfo = false;
      if (trueSeeds.empty()) {
        if (&current == trueState) swapInfo = true;
        seedMap.erase(trueState);
      }
      if (falseSeeds.empty()) {
        if (&current == falseState) swapInfo = true;
        seedMap.erase(falseState);
      }
      if (swapInfo) {
        std::swap(trueState->coveredNew, falseState->coveredNew);
        std::swap(trueState->coveredLines, falseState->coveredLines);
      }
    }

    current.ptreeNode->data = 0;
    std::pair<PTree::Node*, PTree::Node*> res =
      processTree->split(current.ptreeNode, falseState, trueState, condition, tag);
    falseState->ptreeNode = res.first;
    trueState->ptreeNode = res.second;

    if (&current == falseState)
    	fireStateBranched(trueState, falseState, 1, tag);
    else
    	fireStateBranched(falseState, trueState, 0, tag);

    if(DumpPTreeOnChange)
      dumpProcessTree();

    if (!isInternal) {
      if (pathWriter) {
        falseState->pathOS = pathWriter->open(current.pathOS);
        trueState->pathOS << "1";
        falseState->pathOS << "0";
      }      
      if (symPathWriter) {
        falseState->symPathOS = symPathWriter->open(current.symPathOS);
        trueState->symPathOS << "1";
        falseState->symPathOS << "0";
      }
    }

    addConstraint(*trueState, condition);
    addConstraint(*falseState, Expr::createIsZero(condition));

    // Kinda gross, do we even really still want this option?
    if (MaxDepth && MaxDepth<=trueState->depth) {
      terminateStateEarly(*trueState, "max-depth exceeded");
      terminateStateEarly(*falseState, "max-depth exceeded");
      return StatePair((klee::ExecutionState*)NULL, (klee::ExecutionState*)NULL);
    }

    return StatePair(trueState, falseState);
  }
}

Executor::StatePair
Executor::fork(ExecutionState &current, int reason) {
  ExecutionState *lastState = &current;
  ForkTag tag = getForkTag(current, reason);

  ExecutionState *newState = lastState->branch();

  addedStates.insert(newState);

  lastState->ptreeNode->data = 0;
  std::pair<PTree::Node*,PTree::Node*> res =
   processTree->split(lastState->ptreeNode, newState, lastState, 0, tag);
  newState->ptreeNode = res.first;
  lastState->ptreeNode = res.second;

  fireStateBranched(newState, lastState, 0, tag);
  return StatePair(newState, lastState);
}

ForkTag Executor::getForkTag(ExecutionState &current, int reason) {
  ForkTag tag((ForkClass)reason);

  if (current.crtThreadIt == current.threads.end())
    return tag;

  tag.functionName = current.stack().back().kf->function->getNameStr();
  tag.instrID = current.prevPC()->info->id;

  if (tag.forkClass == KLEE_FORK_FAULTINJ) {
    tag.fiVulnerable = false;
    // Check to see whether we are in a vulnerable call

    for (ExecutionState::stack_ty::iterator it = current.stack().begin();
        it != current.stack().end(); it++) {
      if (!it->caller)
        continue;

      KCallInstruction *callInst = dyn_cast<KCallInstruction>((KInstruction*)it->caller);
      assert(callInst);

      if (callInst->vulnerable) {
        tag.fiVulnerable = true;
        break;
      }
    }
  }

  return tag;
}

void Executor::addDuplicates(ExecutionState *main, ExecutionState *other) {
  assert(!other->isDuplicate);
  if (other->duplicates.empty()) {
    ExecutionState *dup = other->branch(true);
    dup->isDuplicate = true;
    dup->ptreeNode = processTree->duplicate(other->ptreeNode, dup);
    dup->ptreeNode->active = false;
    main->duplicates.insert(dup);
  } else {
    main->duplicates.insert(other->duplicates.begin(), other->duplicates.end());
  }
}

ExecutionState* Executor::merge(ExecutionState &current, ExecutionState &other) {
    WallTimer timer;

    ExecutionState *merged = current.merge(other, KeepMergedDuplicates);
    if (merged) {
        if (KeepMergedDuplicates) {
            addedStates.insert(merged);

            current.ptreeNode->data = NULL;
            other.ptreeNode->data = NULL;
            merged->ptreeNode = processTree->mergeCopy(
                    current.ptreeNode, other.ptreeNode, merged);

            addDuplicates(merged, &current);
            addDuplicates(merged, &other);

        } else {
            other.ptreeNode->data = NULL;
            processTree->merge(current.ptreeNode, other.ptreeNode);
        }
        if(DumpPTreeOnChange)
          dumpProcessTree();
        //terminateState(other);
        //updateStates(0);

        stats::mergesSuccess += 1;
        stats::mergeSuccessTime += timer.check();
        return merged;
    }

    stats::mergesFail += 1;
    stats::mergeFailTime += timer.check();
    return NULL;
}

void Executor::addConstraint(ExecutionState &state, ref<Expr> condition) {
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(condition)) {
    assert(CE->isTrue() && "attempt to add invalid constraint");
    return;
  }

  // Check to see if this constraint violates seeds.
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
    seedMap.find(&state);
  if (it != seedMap.end()) {
    bool warn = false;
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(), 
           siie = it->second.end(); siit != siie; ++siit) {
      bool res;
      bool success = 
        solver->mustBeFalse(state, siit->assignment.evaluate(condition), res);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      if (res) {
        siit->patchSeed(state, condition, solver);
        warn = true;
      }
    }
    if (warn)
      klee_warning("seeds patched for violating constraint"); 
  }

  state.addConstraint(condition);
  if (ivcEnabled)
    doImpliedValueConcretization(state, condition, 
                                 ConstantExpr::alloc(1, Expr::Bool));
}

ref<klee::ConstantExpr> Executor::evalConstant(Constant *c) {
  if (llvm::ConstantExpr *ce = dyn_cast<llvm::ConstantExpr>(c)) {
    return evalConstantExpr(ce);
  } else {
    if (const ConstantInt *ci = dyn_cast<ConstantInt>(c)) {
      return ConstantExpr::alloc(ci->getValue());
    } else if (const ConstantFP *cf = dyn_cast<ConstantFP>(c)) {
      return ConstantExpr::alloc(cf->getValueAPF().bitcastToAPInt());
    } else if (const GlobalValue *gv = dyn_cast<GlobalValue>(c)) {
      return globalAddresses.find(gv)->second;
    } else if (isa<ConstantPointerNull>(c)) {
      return Expr::createPointer(0);
    } else if (isa<UndefValue>(c) || isa<ConstantAggregateZero>(c)) {
      return ConstantExpr::create(0, getWidthForLLVMType(c->getType()));
    }else if (const ConstantStruct *cs = dyn_cast<ConstantStruct>(c)) { 
      if(cs->getNumOperands() == 0)
	      return Expr::createPointer(0);
      ref<klee::ConstantExpr> result = evalConstant(cs->getOperand(0));
      for (unsigned k=1, e=cs->getNumOperands(); k != e; ++k){
	      ref<klee::ConstantExpr> next = evalConstant(cs->getOperand(k));
	      result = next->Concat(result);
      }
      return result; 
    } else {
      // Constant{Array,Struct,Vector}
      assert(0 && "invalid argument to evalConstant()");
    }
  }
}

const Cell& Executor::evalV(int vnumber, ExecutionState &state) const {
  assert(vnumber != -1 &&
         "Invalid operand to eval(), not a value or constant!");

  // Determine if this is a constant or not.
  if (vnumber < 0) {
    unsigned index = -vnumber - 2;
    return kmodule->constantTable[index];
  } else {
    unsigned index = vnumber;
    StackFrame &sf = state.stack().back();
    return sf.locals[index];
  }
}

const Cell& Executor::eval(KInstruction *ki, unsigned index,
                           ExecutionState &state) const {
  assert(index < ki->inst->getNumOperands());
  int vnumber = ki->operands[index];

  assert(vnumber != -1 &&
         "Invalid operand to eval(), not a value or constant!");

  // Determine if this is a constant or not.
  if (vnumber < 0) {
    unsigned index = -vnumber - 2;
    return kmodule->constantTable[index];
  } else {
    unsigned index = vnumber;
    StackFrame &sf = state.stack().back();
    return sf.locals[index];
  }
}

void Executor::bindLocal(KInstruction *target, ExecutionState &state, 
                         ref<Expr> value) {
  verifyQceMap(state);
  updateQceLocalsValue(state, target->dest, value, target);
  getDestCell(state, target).value = value;
  verifyQceMap(state);
}

void Executor::bindArgument(KFunction *kf, unsigned index, 
                            ExecutionState &state, ref<Expr> value) {
  getArgumentCell(state, kf, index).value = value;
}


void Executor::bindArgumentToPthreadCreate(KFunction *kf, unsigned index, 
					   StackFrame &sf, ref<Expr> value) {
  getArgumentCell(sf, kf, index).value = value;
}

ref<Expr> Executor::toUnique(const ExecutionState &state, 
                             ref<Expr> &e) {
  ref<Expr> result = e;

  if (!isa<ConstantExpr>(e)) {
    ref<ConstantExpr> value;
    bool isTrue = false;

    solver->setTimeout(stpTimeout);      
    if (solver->getValue(state, e, value) &&
        solver->mustBeTrue(state, EqExpr::create(e, value), isTrue) &&
        isTrue)
      result = value;
    solver->setTimeout(0);
  }
  
  return result;
}


/* Concretize the given expression, and return a possible constant value. 
   'reason' is just a documentation string stating the reason for concretization. */
ref<klee::ConstantExpr> 
Executor::toConstant(ExecutionState &state, 
                     ref<Expr> e,
                     const char *reason) {
  e = state.constraints().simplifyExpr(e);
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(e))
    return CE;

  ref<ConstantExpr> value;
  bool success = solver->getValue(state, e, value);
  assert(success && "FIXME: Unhandled solver failure");
  (void) success;
    
  std::ostringstream os;
  os << "silently concretizing (reason: " << reason << ") expression " << e 
     << " to value " << value 
     << " (" << (*(state.pc())).info->file << ":" << (*(state.pc())).info->line << ")";
      
  if (AllExternalWarnings)
    klee_warning(reason, os.str().c_str());
  else
    klee_warning_once(reason, "%s", os.str().c_str());

  addConstraint(state, EqExpr::create(e, value));
    
  return value;
}

void Executor::executeGetValue(ExecutionState &state,
                               ref<Expr> e,
                               KInstruction *target) {
  //CLOUD9_DEBUG("Getting a concrete value for expression " << e);
  e = state.constraints().simplifyExpr(e);
  //CLOUD9_DEBUG("After simplification: " << e);
  std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
    seedMap.find(&state);
  if (it==seedMap.end() || isa<ConstantExpr>(e)) {
    ref<ConstantExpr> value;

    if (KeepMergedDuplicates && state.isDuplicate
            && !getValuePreferences.empty()) {
      ExecutionState tmp(state);
      std::vector< ref<Expr> >::const_iterator pi =
          getValuePreferences.begin(), pie = getValuePreferences.end();
      for (; pi != pie; ++pi) {
        bool mustBeTrue;
        bool success = solver->mustBeTrue(tmp, Expr::createIsZero(*pi),
                                          mustBeTrue);
        assert(success);
        if (!mustBeTrue) {
            tmp.addConstraint(*pi);
        } else {
            continue;
        }
      }

      bool success = solver->getValue(tmp, e, value);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      //CLOUD9_DEBUG("Concrete value: " << value);
      bindLocal(target, state, value);

    } else {
      bool success = solver->getValue(state, e, value);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      //CLOUD9_DEBUG("Concrete value: " << value);
      bindLocal(target, state, value);

      if (KeepMergedDuplicates && !state.isDuplicate) {
          getValuePreferences.push_back(EqExpr::create(e, value));
      }
    }
  } else {
    std::set< ref<Expr> > values;
    for (std::vector<SeedInfo>::iterator siit = it->second.begin(), 
           siie = it->second.end(); siit != siie; ++siit) {
      ref<ConstantExpr> value;
      bool success = 
        solver->getValue(state, siit->assignment.evaluate(e), value);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      values.insert(value);
    }
    
    std::vector< ref<Expr> > conditions;
    for (std::set< ref<Expr> >::iterator vit = values.begin(), 
           vie = values.end(); vit != vie; ++vit)
      conditions.push_back(EqExpr::create(e, *vit));

    std::vector<ExecutionState*> branches;
    branch(state, conditions, branches, KLEE_FORK_INTERNAL);
    
    std::vector<ExecutionState*>::iterator bit = branches.begin();
    for (std::set< ref<Expr> >::iterator vit = values.begin(), 
           vie = values.end(); vit != vie; ++vit) {
      ExecutionState *es = *bit;
      if (es)
        bindLocal(target, *es, *vit);
      ++bit;
    }
  }
}

void Executor::stepInstruction(ExecutionState &state, bool trackInstr) {
  if (DebugPrintInstructions) {
    printFileLine(state, state.pc());
    std::cerr << std::setw(10) << stats::instructions << " ";
    llvm::errs() << *(state.pc()->inst);
  }

  if (statsTracker)
    statsTracker->stepInstruction(state);

  if (trackInstr) {
      ++stats::instructions;

      uint64_t instructionsMultOld = stats::instructionsMult.getValue();
      stats::instructionsMult += state.multiplicity;
      if (stats::instructionsMult.getValue() < instructionsMultOld) // overflow
        stats::instructionsMultHigh+=1;
  }

  state.setPrevPC(state.pc());
  state.setPC(state.pc().next());

  if (stats::instructions==StopAfterNInstructions)
    haltExecution = true;
}

void Executor::executeCall(ExecutionState &state, 
                           KInstruction *ki,
                           Function *f,
                           std::vector< ref<Expr> > &arguments) {
  fireControlFlowEvent(&state, ::cloud9::worker::CALL);

  if (f && DebugCallHistory) {
    unsigned depth = state.stack().size();

    CLOUD9_DEBUG("Call: " << (std::string(" ") * depth) << f->getNameStr());
  }

  Instruction *i = NULL;
  if (ki)
      i = ki->inst;

  if (ki && f && f->isDeclaration()) {
    switch(f->getIntrinsicID()) {
    case Intrinsic::not_intrinsic:
      // state may be destroyed by this call, cannot touch
      callExternalFunction(state, ki, f, arguments);
      break;
        
      // va_arg is handled by caller and intrinsic lowering, see comment for
      // ExecutionState::varargs
    case Intrinsic::vastart:  {
      StackFrame &sf = state.stack().back();
      assert(sf.varargs && 
             "vastart called in function with no vararg object");

      // FIXME: This is really specific to the architecture, not the pointer
      // size. This happens to work fir x86-32 and x86-64, however.
      Expr::Width WordSize = Context::get().getPointerWidth();
      if (WordSize == Expr::Int32) {
        executeMemoryOperation(state, true, arguments[0], 
                               sf.varargs->getBaseExpr(), ki);
      } else {
        assert(WordSize == Expr::Int64 && "Unknown word size!");

        // X86-64 has quite complicated calling convention. However,
        // instead of implementing it, we can do a simple hack: just
        // make a function believe that all varargs are on stack.
        executeMemoryOperation(state, true, arguments[0],
                               ConstantExpr::create(48, 32), ki); // gp_offset
        executeMemoryOperation(state, true,
                               AddExpr::create(arguments[0], 
                                               ConstantExpr::create(4, 64)),
                               ConstantExpr::create(304, 32), ki); // fp_offset
        executeMemoryOperation(state, true,
                               AddExpr::create(arguments[0], 
                                               ConstantExpr::create(8, 64)),
                               sf.varargs->getBaseExpr(), ki); // overflow_arg_area
        executeMemoryOperation(state, true,
                               AddExpr::create(arguments[0], 
                                               ConstantExpr::create(16, 64)),
                               ConstantExpr::create(0, 64), ki); // reg_save_area
      }
      break;
    }
    case Intrinsic::vaend:
      // va_end is a noop for the interpreter.
      //
      // FIXME: We should validate that the target didn't do something bad
      // with vaeend, however (like call it twice).
      break;
        
    case Intrinsic::vacopy:
      // va_copy should have been lowered.
      //
      // FIXME: It would be nice to check for errors in the usage of this as
      // well.
    default:
      klee_error("unknown intrinsic: %s", f->getName().data());
    }

    if (InvokeInst *ii = dyn_cast<InvokeInst>(i))
      transferToBasicBlock(ii->getNormalDest(), i->getParent(), state);
  } else {
    // FIXME: I'm not really happy about this reliance on prevPC but it is ok, I
    // guess. This just done to avoid having to pass KInstIterator everywhere
    // instead of the actual instruction, since we can't make a KInstIterator
    // from just an instruction (unlike LLVM).
    KFunction *kf = kmodule->functionMap[f];
    state.pushFrame(state.prevPC(), kf);
    state.setPC(kf->instructions);

    updateQceMapOnFramePush(state);
        
    if (statsTracker)
      statsTracker->framePushed(state, &state.stack()[state.stack().size()-2]); //XXX TODO fix this ugly stuff
 
     // TODO: support "byval" parameter attribute
     // TODO: support zeroext, signext, sret attributes
        
    unsigned callingArgs = arguments.size();
    unsigned funcArgs = f->arg_size();
    if (!f->isVarArg()) {
      if (callingArgs > funcArgs) {
        klee_warning_once(f, "calling %s with extra arguments.", 
                          f->getName().data());
      } else if (callingArgs < funcArgs) {
        terminateStateOnError(state, "calling function with too few arguments", 
                              "user.err");
        return;
      }
    } else {
      if (callingArgs < funcArgs) {
        terminateStateOnError(state, "calling function with too few arguments", 
                              "user.err");
        return;
      }
            
      StackFrame &sf = state.stack().back();
      unsigned size = 0;
      for (unsigned i = funcArgs; i < callingArgs; i++) {
        // FIXME: This is really specific to the architecture, not the pointer
        // size. This happens to work fir x86-32 and x86-64, however.
        Expr::Width WordSize = Context::get().getPointerWidth();
        if (WordSize == Expr::Int32) {
          size += Expr::getMinBytesForWidth(arguments[i]->getWidth());
        } else {
          size += llvm::RoundUpToAlignment(arguments[i]->getWidth(), 
                                           WordSize) / 8;
        }
      }

      MemoryObject *mo = sf.varargs = memory->allocate(&state, size, true, false,
                                                       state.prevPC()->inst);
      if (!mo) {
        terminateStateOnExecError(state, "out of memory (varargs)");
        return;
      }
      ObjectState *os = bindObjectInState(state, mo, true);
      unsigned offset = 0;
      for (unsigned i = funcArgs; i < callingArgs; i++) {
        // FIXME: This is really specific to the architecture, not the pointer
        // size. This happens to work fir x86-32 and x86-64, however.
        Expr::Width WordSize = Context::get().getPointerWidth();
        if (WordSize == Expr::Int32) {
          os->write(offset, arguments[i]);
          offset += Expr::getMinBytesForWidth(arguments[i]->getWidth());
        } else {
          assert(WordSize == Expr::Int64 && "Unknown word size!");
          os->write(offset, arguments[i]);
          offset += llvm::RoundUpToAlignment(arguments[i]->getWidth(), 
                                             WordSize) / 8;
        }
      }
    }

    unsigned numFormals = f->arg_size();
    for (unsigned i=0; i<numFormals; ++i) 
      bindArgument(kf, i, state, arguments[i]);
  }
}

void Executor::transferToBasicBlock(BasicBlock *dst, BasicBlock *src, 
                                    ExecutionState &state) {
  // Note that in general phi nodes can reuse phi values from the same
  // block but the incoming value is the eval() result *before* the
  // execution of any phi nodes. this is pathological and doesn't
  // really seem to occur, but just in case we run the PhiCleanerPass
  // which makes sure this cannot happen and so it is safe to just
  // eval things in order. The PhiCleanerPass also makes sure that all
  // incoming blocks have the same order for each PHINode so we only
  // have to compute the index once.
  //
  // With that done we simply set an index in the state so that PHI
  // instructions know which argument to eval, set the pc, and continue.
  
  // XXX this lookup has to go ?
  KFunction *kf = state.stack().back().kf;
  unsigned entry = kf->basicBlockEntry[dst];
  state.setPC(&kf->instructions[entry]);
  if (state.pc()->inst->getOpcode() == Instruction::PHI) {
    PHINode *first = static_cast<PHINode*>(state.pc()->inst);
    state.crtThread().incomingBBIndex = first->getBasicBlockIndex(src);
  }
}

void Executor::printFileLine(ExecutionState &state, KInstruction *ki) {
  const InstructionInfo &ii = *ki->info;
  if (ii.file != "") 
    std::cerr << "     " << ii.file << ":" << ii.line << ":";
  else
    std::cerr << "     [no debug info]:";
}


Function* Executor::getCalledFunction(CallSite &cs, ExecutionState &state) {
  Function *f = cs.getCalledFunction();
  
  if (f) {
    std::string alias = state.getFnAlias(f->getName());
    if (alias != "") {
      llvm::Module* currModule = kmodule->module;
      Function* old_f = f;
      f = currModule->getFunction(alias);
      if (!f) {
	llvm::errs() << "Function " << alias << "(), alias for " 
                     << old_f->getName() << " not found!\n";
	assert(f && "function alias not found");
      }
    }
  }
  
  return f;
}

static bool isDebugIntrinsic(const Function *f, KModule *KM) {
#if (LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 7)
  // Fast path, getIntrinsicID is slow.
  if (f == KM->dbgStopPointFn)
    return true;

  switch (f->getIntrinsicID()) {
  case Intrinsic::dbg_stoppoint:
  case Intrinsic::dbg_region_start:
  case Intrinsic::dbg_region_end:
  case Intrinsic::dbg_func_start:
  case Intrinsic::dbg_declare:
    return true;

  default:
    return false;
  }
#else
  return false;
#endif
}

static inline const llvm::fltSemantics * fpWidthToSemantics(unsigned width) {
  switch(width) {
  case Expr::Int32:
    return &llvm::APFloat::IEEEsingle;
  case Expr::Int64:
    return &llvm::APFloat::IEEEdouble;
  case Expr::Fl80:
    return &llvm::APFloat::x87DoubleExtended;
  default:
    return 0;
  }
}

void Executor::executeInstruction(ExecutionState &state, KInstruction *ki) {
  Instruction *i = ki->inst;

#if 0
  if (const MDNode *md = i->getMetadata("ul")) {
    // A special case of blacklist node
    assert(cast<ConstantInt>(md->getOperand(0))->getZExtValue() == 0);
    assert(md->getOperand(1) == i);
    if (getWidthForLLVMType(i->getType()) <= 64) {
      state.updateValUseFrequency(i, ki->dest,
                cast<ConstantInt>(md->getOperand(2))->getZExtValue(),
                cast<ConstantInt>(md->getOperand(3))->getZExtValue());
    } else {
      // XXX
    }
  }
#endif

  switch (i->getOpcode()) {
    // Control flow
  case Instruction::Ret: {
    ReturnInst *ri = cast<ReturnInst>(i);
    KInstIterator kcaller = state.stack().back().caller;
    Instruction *caller = kcaller ? kcaller->inst : 0;
    bool isVoidReturn = (ri->getNumOperands() == 0);
    ref<Expr> result = ConstantExpr::alloc(0, Expr::Bool);

    fireControlFlowEvent(&state, ::cloud9::worker::RETURN);
    
    if (!isVoidReturn) {
      result = eval(ki, 0, state).value;
    }
    
    if (state.stack().size() <= 1) {
      assert(!caller && "caller set on initial stack frame");
      
      if (state.threads.size() == 1) {
        //main exit
        terminateStateOnExit(state);
      } else if (state.crtProcess().threads.size() == 1){
        // Invoke exit()
        Function *f = kmodule->module->getFunction("exit");
        std::vector<ref<Expr> > arguments;
        arguments.push_back(result);

        executeCall(state, NULL, f, arguments);
      } else {
        // Invoke pthread_exit()
        Function *f = kmodule->module->getFunction("pthread_exit");
        std::vector<ref<Expr> > arguments;
        arguments.push_back(result);

        executeCall(state, NULL, f, arguments);
      }
    } else {
      updateQceMapOnFramePop(state);
      state.popFrame();

      if (statsTracker)
        statsTracker->framePopped(state);

      if (InvokeInst *ii = dyn_cast<InvokeInst>(caller)) {
        transferToBasicBlock(ii->getNormalDest(), caller->getParent(), state);
      } else {
        state.setPC(kcaller);
        state.setPC(state.pc().next());
      }

      if (!isVoidReturn) {
        const Type *t = caller->getType();
        if (t != Type::getVoidTy(getGlobalContext())) {
          // may need to do coercion due to bitcasts
          Expr::Width from = result->getWidth();
          Expr::Width to = getWidthForLLVMType(t);
            
          if (from != to) {
            CallSite cs = (isa<InvokeInst>(caller) ? CallSite(cast<InvokeInst>(caller)) : 
                           CallSite(cast<CallInst>(caller)));

            // XXX need to check other param attrs ?
            if (cs.paramHasAttr(0, llvm::Attribute::SExt)) {
              result = SExtExpr::create(result, to);
            } else {
              result = ZExtExpr::create(result, to);
            }
          }

          bindLocal(kcaller, state, result);
        }
      } else {
        // We check that the return value has no users instead of
        // checking the type, since C defaults to returning int for
        // undeclared functions.
        if (!caller->use_empty()) {
          terminateStateOnExecError(state, "return void when caller expected a result");
        }
      }
    }      
    break;
  }
  case Instruction::Unwind: {
    for (;;) {
      KInstruction *kcaller = state.stack().back().caller;
      updateQceMapOnFramePop(state);
      state.popFrame();

      if (statsTracker)
        statsTracker->framePopped(state);

      if (state.stack().empty()) {
        terminateStateOnExecError(state, "unwind from initial stack frame");
        break;
      } else {
        Instruction *caller = kcaller->inst;
        if (InvokeInst *ii = dyn_cast<InvokeInst>(caller)) {
          transferToBasicBlock(ii->getUnwindDest(), caller->getParent(), state);
          break;
        }
      }
    }
    break;
  }
  case Instruction::Br: {
    BranchInst *bi = cast<BranchInst>(i);
    int reason = KLEE_FORK_DEFAULT;

    if (state.crtSpecialFork == i) {
      reason = state.crtForkReason;
      state.crtSpecialFork = NULL;
    } else {
      assert(!state.crtForkReason && "another branching instruction between a klee_branch and its corresponding 'if'");
    }

    if (bi->isUnconditional()) {
      transferToBasicBlock(bi->getSuccessor(0), bi->getParent(), state);
    } else {
      // FIXME: Find a way that we don't have this hidden dependency.
      assert(bi->getCondition() == bi->getOperand(0) &&
             "Wrong operand index!");
      ref<Expr> cond = eval(ki, 0, state).value;
      //C9HACK_DEBUG("Fork requested: " << (false ? "internal" : "external"), state);
      Executor::StatePair branches = fork(state, cond, false, reason);

      if (branches.first) {
    	  fireControlFlowEvent(branches.first, ::cloud9::worker::BRANCH_TRUE);
      }

      if (branches.second) {
    	  fireControlFlowEvent(branches.second, ::cloud9::worker::BRANCH_FALSE);
      }

      // NOTE: There is a hidden dependency here, markBranchVisited
      // requires that we still be in the context of the branch
      // instruction (it reuses its statistic id). Should be cleaned
      // up with convenient instruction specific data.
      if (statsTracker && state.stack().back().kf->trackCoverage)
        statsTracker->markBranchVisited(branches.first, branches.second);

      if (branches.first)
        transferToBasicBlock(bi->getSuccessor(0), bi->getParent(), *branches.first);
      if (branches.second)
        transferToBasicBlock(bi->getSuccessor(1), bi->getParent(), *branches.second);
    }
    break;
  }
  case Instruction::Switch: {
    SwitchInst *si = cast<SwitchInst>(i);
    ref<Expr> cond = eval(ki, 0, state).value;
    unsigned cases = si->getNumCases();
    BasicBlock *bb = si->getParent();

    cond = toUnique(state, cond);
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(cond)) {
      // Somewhat gross to create these all the time, but fine till we
      // switch to an internal rep.
      const llvm::IntegerType *Ty = 
        cast<IntegerType>(si->getCondition()->getType());
      ConstantInt *ci = ConstantInt::get(Ty, CE->getZExtValue());
      unsigned index = si->findCaseValue(ci);
      transferToBasicBlock(si->getSuccessor(index), si->getParent(), state);
    } else {
      std::vector<std::pair<BasicBlock*, ref<Expr> > > targets;

      ref<Expr> isDefault = ConstantExpr::alloc(1, Expr::Bool);

      for (unsigned i = 1; i < cases; ++i) {
        ref<Expr> value = evalConstant(si->getCaseValue(i));
        ref<Expr> match = EqExpr::create(cond, value);
        isDefault = AndExpr::create(isDefault, Expr::createIsZero(match));
        bool result;
        bool success = solver->mayBeTrue(state, match, result);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;

        if (result) {
          unsigned k = 0;
          for (k = 0; k < targets.size(); k++) {
            if (targets[k].first == si->getSuccessor(i)) {
              targets[k].second = OrExpr::create(match, targets[k].second);
              break;
            }
          }

          if (k == targets.size()) {
            targets.push_back(std::make_pair(si->getSuccessor(i), match));
          }
        }
      }

      bool res;
      bool success = solver->mayBeTrue(state, isDefault, res);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      if (res) {
        unsigned k = 0;
        for (k = 0; k < targets.size(); k++) {
          if (targets[k].first == si->getSuccessor(0)) {
            targets[k].second = OrExpr::create(isDefault, targets[k].second);
            break;
          }
        }

        if (k == targets.size()) {
          targets.push_back(std::make_pair(si->getSuccessor(0), isDefault));
        }
      }
      
      std::vector< ref<Expr> > conditions;
      for (std::vector<std::pair<BasicBlock*, ref<Expr> > >::iterator it =
             targets.begin(), ie = targets.end();
           it != ie; ++it)
        conditions.push_back(it->second);
      
      std::vector<ExecutionState*> branches;
      branch(state, conditions, branches, KLEE_FORK_DEFAULT);
        
      std::vector<ExecutionState*>::iterator bit = branches.begin();
      for (std::vector<std::pair<BasicBlock*, ref<Expr> > >::iterator it =
             targets.begin(), ie = targets.end();
           it != ie; ++it) {
        ExecutionState *es = *bit;
        if (es)
          transferToBasicBlock(it->first, bb, *es);
        ++bit;
      }
    }
    break;
 }
  case Instruction::Unreachable:
    // Note that this is not necessarily an internal bug, llvm will
    // generate unreachable instructions in cases where it knows the
    // program will crash. So it is effectively a SEGV or internal
    // error.
    terminateStateOnExecError(state, "reached \"unreachable\" instruction");
    break;

  case Instruction::Invoke:
  case Instruction::Call: {
    updateQceMapBeforeCall(state);

    CallSite cs(i);

    unsigned numArgs = cs.arg_size();
    Function *f = getCalledFunction(cs, state);

    // Skip debug intrinsics, we can't evaluate their metadata arguments.
    if (f && isDebugIntrinsic(f, kmodule))
      break;

    // evaluate arguments
    std::vector< ref<Expr> > arguments;
    arguments.reserve(numArgs);

    for (unsigned j=0; j<numArgs; ++j)
      arguments.push_back(eval(ki, j+1, state).value);

    if (!f) {
      // special case the call with a bitcast case
      Value *fp = cs.getCalledValue();
      llvm::ConstantExpr *ce = dyn_cast<llvm::ConstantExpr>(fp);
        
      if (ce && ce->getOpcode()==Instruction::BitCast) {
        f = dyn_cast<Function>(ce->getOperand(0));
        assert(f && "XXX unrecognized constant expression in call");
        const FunctionType *fType = 
          dyn_cast<FunctionType>(cast<PointerType>(f->getType())->getElementType());
        const FunctionType *ceType =
          dyn_cast<FunctionType>(cast<PointerType>(ce->getType())->getElementType());
        assert(fType && ceType && "unable to get function type");

        // XXX check result coercion

        // XXX this really needs thought and validation
        unsigned i=0;
        for (std::vector< ref<Expr> >::iterator
               ai = arguments.begin(), ie = arguments.end();
             ai != ie; ++ai) {
          Expr::Width to, from = (*ai)->getWidth();
            
          if (i<fType->getNumParams()) {
            to = getWidthForLLVMType(fType->getParamType(i));

            if (from != to) {
              // XXX need to check other param attrs ?
              if (cs.paramHasAttr(i+1, llvm::Attribute::SExt)) {
                arguments[i] = SExtExpr::create(arguments[i], to);
              } else {
                arguments[i] = ZExtExpr::create(arguments[i], to);
              }
            }
          }
            
          i++;
        }
      } else if (isa<InlineAsm>(fp)) {
        terminateStateOnExecError(state, "inline assembly is unsupported");
        break;
      }
    }

    if (f) {
      executeCall(state, ki, f, arguments);
    } else {
      ref<Expr> v = eval(ki, 0, state).value;

      ExecutionState *free = &state;
      bool hasInvalid = false, first = true;

      /* XXX This is wasteful, no need to do a full evaluate since we
         have already got a value. But in the end the caches should
         handle it for us, albeit with some overhead. */
      do {
        ref<ConstantExpr> value;
        bool success = solver->getValue(*free, v, value);
        assert(success && "FIXME: Unhandled solver failure");
        (void) success;
        //C9HACK_DEBUG("Fork requested: " << (true ? "internal" : "external"), state);
        StatePair res = fork(*free, EqExpr::create(v, value), true, KLEE_FORK_INTERNAL);
        if (res.first) {
          uint64_t addr = value->getZExtValue();
          if (legalFunctions.count(addr)) {
            f = (Function*) addr;

            // Don't give warning on unique resolution
            if (res.second || !first)
              klee_warning_once((void*) (unsigned long) addr, 
                                "resolved symbolic function pointer to: %s",
                                f->getName().data());

            executeCall(*res.first, ki, f, arguments);
          } else {
            if (!hasInvalid) {
              terminateStateOnExecError(state, "invalid function pointer");
              hasInvalid = true;
            }
          }
        }

        first = false;
        free = res.second;
      } while (free);
    }
    break;
  }
  case Instruction::PHI: {
    ref<Expr> result = eval(ki, state.crtThread().incomingBBIndex * 2, state).value;
    bindLocal(ki, state, result);
    break;
  }

    // Special instructions
  case Instruction::Select: {
    SelectInst *SI = cast<SelectInst>(ki->inst);
    assert(SI->getCondition() == SI->getOperand(0) &&
           "Wrong operand index!");
    ref<Expr> cond = eval(ki, 0, state).value;
    ref<Expr> tExpr = eval(ki, 1, state).value;
    ref<Expr> fExpr = eval(ki, 2, state).value;
    ref<Expr> result = SelectExpr::create(cond, tExpr, fExpr);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::VAArg:
    terminateStateOnExecError(state, "unexpected VAArg instruction");
    break;

    // Arithmetic / logical

  case Instruction::Add: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    bindLocal(ki, state, AddExpr::create(left, right));
    break;
  }

  case Instruction::Sub: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    bindLocal(ki, state, SubExpr::create(left, right));
    break;
  }
 
  case Instruction::Mul: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    bindLocal(ki, state, MulExpr::create(left, right));
    break;
  }

  case Instruction::UDiv: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = UDivExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::SDiv: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = SDivExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::URem: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = URemExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }
 
  case Instruction::SRem: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = SRemExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::And: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = AndExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::Or: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = OrExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::Xor: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = XorExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::Shl: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = ShlExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::LShr: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = LShrExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::AShr: {
    ref<Expr> left = eval(ki, 0, state).value;
    ref<Expr> right = eval(ki, 1, state).value;
    ref<Expr> result = AShrExpr::create(left, right);
    bindLocal(ki, state, result);
    break;
  }

    // Compare

  case Instruction::ICmp: {
    CmpInst *ci = cast<CmpInst>(i);
    ICmpInst *ii = cast<ICmpInst>(ci);
 
    switch(ii->getPredicate()) {
    case ICmpInst::ICMP_EQ: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = EqExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_NE: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = NeExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_UGT: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = UgtExpr::create(left, right);
      bindLocal(ki, state,result);
      break;
    }

    case ICmpInst::ICMP_UGE: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = UgeExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_ULT: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = UltExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_ULE: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = UleExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_SGT: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = SgtExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_SGE: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = SgeExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_SLT: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = SltExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    case ICmpInst::ICMP_SLE: {
      ref<Expr> left = eval(ki, 0, state).value;
      ref<Expr> right = eval(ki, 1, state).value;
      ref<Expr> result = SleExpr::create(left, right);
      bindLocal(ki, state, result);
      break;
    }

    default:
      terminateStateOnExecError(state, "invalid ICmp predicate");
    }
    break;
  }
 
    // Memory instructions...
#if (LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 7)
  case Instruction::Malloc:
  case Instruction::Alloca: {
    AllocationInst *ai = cast<AllocationInst>(i);
#else
  case Instruction::Alloca: {
    AllocaInst *ai = cast<AllocaInst>(i);
#endif
    unsigned elementSize = 
      kmodule->targetData->getTypeStoreSize(ai->getAllocatedType());
    ref<Expr> size = Expr::createPointer(elementSize);
    if (ai->isArrayAllocation()) {
      ref<Expr> count = eval(ki, 0, state).value;
      count = Expr::createCoerceToPointerType(count);
      size = MulExpr::create(size, count);
    }
    bool isLocal = i->getOpcode()==Instruction::Alloca;
    executeAlloc(state, size, isLocal, ki);
    break;
  }
#if (LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 7)
  case Instruction::Free: {
    executeFree(state, eval(ki, 0, state).value);
    break;
  }
#endif

  case Instruction::Load: {
    ref<Expr> base = eval(ki, 0, state).value;
    if (SimplifySymIndices && !isa<ConstantExpr>(base)) {
      ref<Expr> newBase = state.constraints().simplifyExpr(base);
      //if (!isa<ConstantExpr>(base))
      //  base = toUnique(state, base);
      if (base != newBase) {
        int vnumber = ki->operands[0];
        if (vnumber >= 0) {
          verifyQceMap(state);
          updateQceLocalsValue(state, vnumber, newBase, NULL);
          state.stack().back().locals[vnumber].value = newBase;
          verifyQceMap(state);
        }
        base = newBase;
      }
    }
    executeMemoryOperation(state, false, base, 0, ki);
    break;
  }
  case Instruction::Store: {
    ref<Expr> base = eval(ki, 1, state).value;
    ref<Expr> value = eval(ki, 0, state).value;
    if (SimplifySymIndices && !isa<ConstantExpr>(base)) {
      ref<Expr> newBase = state.constraints().simplifyExpr(base);
      //if (!isa<ConstantExpr>(base))
      //  base = toUnique(state, base);
      if (base != newBase) {
        int vnumber = ki->operands[1];
        if (vnumber >= 0) {
          verifyQceMap(state);
          updateQceLocalsValue(state, vnumber, newBase, NULL);
          state.stack().back().locals[vnumber].value = newBase;
          verifyQceMap(state);
        }
        base = newBase;
      }
    }
    executeMemoryOperation(state, true, base, value, ki);
    break;
  }

  case Instruction::GetElementPtr: {
    KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(ki);
    ref<Expr> base = eval(ki, 0, state).value;

    for (std::vector< std::pair<unsigned, uint64_t> >::iterator 
           it = kgepi->indices.begin(), ie = kgepi->indices.end(); 
         it != ie; ++it) {
      uint64_t elementSize = it->second;
      ref<Expr> index = eval(ki, it->first, state).value;
      base = AddExpr::create(base,
                             MulExpr::create(Expr::createCoerceToPointerType(index),
                                             Expr::createPointer(elementSize)));
    }
    if (kgepi->offset)
      base = AddExpr::create(base,
                             Expr::createPointer(kgepi->offset));
    bindLocal(ki, state, base);
    break;
  }

    // Conversion
  case Instruction::Trunc: {
    CastInst *ci = cast<CastInst>(i);
    ref<Expr> result = ExtractExpr::create(eval(ki, 0, state).value,
                                           0,
                                           getWidthForLLVMType(ci->getType()));
    bindLocal(ki, state, result);
    break;
  }
  case Instruction::ZExt: {
    CastInst *ci = cast<CastInst>(i);
    ref<Expr> result = ZExtExpr::create(eval(ki, 0, state).value,
                                        getWidthForLLVMType(ci->getType()));
    bindLocal(ki, state, result);
    break;
  }
  case Instruction::SExt: {
    CastInst *ci = cast<CastInst>(i);
    ref<Expr> result = SExtExpr::create(eval(ki, 0, state).value,
                                        getWidthForLLVMType(ci->getType()));
    bindLocal(ki, state, result);
    break;
  }

  case Instruction::IntToPtr: {
    CastInst *ci = cast<CastInst>(i);
    Expr::Width pType = getWidthForLLVMType(ci->getType());
    ref<Expr> arg = eval(ki, 0, state).value;
    bindLocal(ki, state, ZExtExpr::create(arg, pType));
    break;
  } 
  case Instruction::PtrToInt: {
    CastInst *ci = cast<CastInst>(i);
    Expr::Width iType = getWidthForLLVMType(ci->getType());
    ref<Expr> arg = eval(ki, 0, state).value;
    bindLocal(ki, state, ZExtExpr::create(arg, iType));
    break;
  }

  case Instruction::BitCast: {
    ref<Expr> result = eval(ki, 0, state).value;
    bindLocal(ki, state, result);
    break;
  }

    // Floating point instructions

  case Instruction::FAdd: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FAdd operation");

    llvm::APFloat Res(left->getAPValue());
    Res.add(APFloat(right->getAPValue()), APFloat::rmNearestTiesToEven);
    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }

  case Instruction::FSub: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FSub operation");

    llvm::APFloat Res(left->getAPValue());
    Res.subtract(APFloat(right->getAPValue()), APFloat::rmNearestTiesToEven);
    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }
 
  case Instruction::FMul: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FMul operation");

    llvm::APFloat Res(left->getAPValue());
    Res.multiply(APFloat(right->getAPValue()), APFloat::rmNearestTiesToEven);
    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }

  case Instruction::FDiv: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FDiv operation");

    llvm::APFloat Res(left->getAPValue());
    Res.divide(APFloat(right->getAPValue()), APFloat::rmNearestTiesToEven);
    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }

  case Instruction::FRem: {
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FRem operation");

    llvm::APFloat Res(left->getAPValue());
    Res.mod(APFloat(right->getAPValue()), APFloat::rmNearestTiesToEven);
    bindLocal(ki, state, ConstantExpr::alloc(Res.bitcastToAPInt()));
    break;
  }

  case Instruction::FPTrunc: {
    FPTruncInst *fi = cast<FPTruncInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || resultType > arg->getWidth())
      return terminateStateOnExecError(state, "Unsupported FPTrunc operation");

    llvm::APFloat Res(arg->getAPValue());
    bool losesInfo = false;
    Res.convert(*fpWidthToSemantics(resultType),
                llvm::APFloat::rmNearestTiesToEven,
                &losesInfo);
    bindLocal(ki, state, ConstantExpr::alloc(Res));
    break;
  }

  case Instruction::FPExt: {
    FPExtInst *fi = cast<FPExtInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || arg->getWidth() > resultType)
      return terminateStateOnExecError(state, "Unsupported FPExt operation");

    llvm::APFloat Res(arg->getAPValue());
    bool losesInfo = false;
    Res.convert(*fpWidthToSemantics(resultType),
                llvm::APFloat::rmNearestTiesToEven,
                &losesInfo);
    bindLocal(ki, state, ConstantExpr::alloc(Res));
    break;
  }

  case Instruction::FPToUI: {
    FPToUIInst *fi = cast<FPToUIInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || resultType > 64)
      return terminateStateOnExecError(state, "Unsupported FPToUI operation");

    llvm::APFloat Arg(arg->getAPValue());
    uint64_t value = 0;
    bool isExact = true;
    Arg.convertToInteger(&value, resultType, false,
                         llvm::APFloat::rmTowardZero, &isExact);
    bindLocal(ki, state, ConstantExpr::alloc(value, resultType));
    break;
  }

  case Instruction::FPToSI: {
    FPToSIInst *fi = cast<FPToSIInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    if (!fpWidthToSemantics(arg->getWidth()) || resultType > 64)
      return terminateStateOnExecError(state, "Unsupported FPToSI operation");

    llvm::APFloat Arg(arg->getAPValue());
    uint64_t value = 0;
    bool isExact = true;
    Arg.convertToInteger(&value, resultType, false,
                         llvm::APFloat::rmTowardZero, &isExact);
    bindLocal(ki, state, ConstantExpr::alloc(value, resultType));
    break;
  }

  case Instruction::UIToFP: {
    UIToFPInst *fi = cast<UIToFPInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    const llvm::fltSemantics *semantics = fpWidthToSemantics(resultType);
    if (!semantics)
      return terminateStateOnExecError(state, "Unsupported UIToFP operation");
    llvm::APFloat f(*semantics, 0);
    f.convertFromAPInt(arg->getAPValue(), false,
                       llvm::APFloat::rmNearestTiesToEven);

    bindLocal(ki, state, ConstantExpr::alloc(f));
    break;
  }

  case Instruction::SIToFP: {
    SIToFPInst *fi = cast<SIToFPInst>(i);
    Expr::Width resultType = getWidthForLLVMType(fi->getType());
    ref<ConstantExpr> arg = toConstant(state, eval(ki, 0, state).value,
                                       "floating point");
    const llvm::fltSemantics *semantics = fpWidthToSemantics(resultType);
    if (!semantics)
      return terminateStateOnExecError(state, "Unsupported SIToFP operation");
    llvm::APFloat f(*semantics, 0);
    f.convertFromAPInt(arg->getAPValue(), true,
                       llvm::APFloat::rmNearestTiesToEven);

    bindLocal(ki, state, ConstantExpr::alloc(f));
    break;
  }

  case Instruction::FCmp: {
    FCmpInst *fi = cast<FCmpInst>(i);
    ref<ConstantExpr> left = toConstant(state, eval(ki, 0, state).value,
                                        "floating point");
    ref<ConstantExpr> right = toConstant(state, eval(ki, 1, state).value,
                                         "floating point");
    if (!fpWidthToSemantics(left->getWidth()) ||
        !fpWidthToSemantics(right->getWidth()))
      return terminateStateOnExecError(state, "Unsupported FCmp operation");

    APFloat LHS(left->getAPValue());
    APFloat RHS(right->getAPValue());
    APFloat::cmpResult CmpRes = LHS.compare(RHS);

    bool Result = false;
    switch( fi->getPredicate() ) {
      // Predicates which only care about whether or not the operands are NaNs.
    case FCmpInst::FCMP_ORD:
      Result = CmpRes != APFloat::cmpUnordered;
      break;

    case FCmpInst::FCMP_UNO:
      Result = CmpRes == APFloat::cmpUnordered;
      break;

      // Ordered comparisons return false if either operand is NaN.  Unordered
      // comparisons return true if either operand is NaN.
    case FCmpInst::FCMP_UEQ:
      if (CmpRes == APFloat::cmpUnordered) {
        Result = true;
        break;
      }
    case FCmpInst::FCMP_OEQ:
      Result = CmpRes == APFloat::cmpEqual;
      break;

    case FCmpInst::FCMP_UGT:
      if (CmpRes == APFloat::cmpUnordered) {
        Result = true;
        break;
      }
    case FCmpInst::FCMP_OGT:
      Result = CmpRes == APFloat::cmpGreaterThan;
      break;

    case FCmpInst::FCMP_UGE:
      if (CmpRes == APFloat::cmpUnordered) {
        Result = true;
        break;
      }
    case FCmpInst::FCMP_OGE:
      Result = CmpRes == APFloat::cmpGreaterThan || CmpRes == APFloat::cmpEqual;
      break;

    case FCmpInst::FCMP_ULT:
      if (CmpRes == APFloat::cmpUnordered) {
        Result = true;
        break;
      }
    case FCmpInst::FCMP_OLT:
      Result = CmpRes == APFloat::cmpLessThan;
      break;

    case FCmpInst::FCMP_ULE:
      if (CmpRes == APFloat::cmpUnordered) {
        Result = true;
        break;
      }
    case FCmpInst::FCMP_OLE:
      Result = CmpRes == APFloat::cmpLessThan || CmpRes == APFloat::cmpEqual;
      break;

    case FCmpInst::FCMP_UNE:
      Result = CmpRes == APFloat::cmpUnordered || CmpRes != APFloat::cmpEqual;
      break;
    case FCmpInst::FCMP_ONE:
      Result = CmpRes != APFloat::cmpUnordered && CmpRes != APFloat::cmpEqual;
      break;

    default:
      assert(0 && "Invalid FCMP predicate!");
    case FCmpInst::FCMP_FALSE:
      Result = false;
      break;
    case FCmpInst::FCMP_TRUE:
      Result = true;
      break;
    }

    bindLocal(ki, state, ConstantExpr::alloc(Result, Expr::Bool));
    break;
  }
  case Instruction::InsertValue: {
    KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(ki);

    ref<Expr> agg = eval(ki, 0, state).value;
    ref<Expr> val = eval(ki, 1, state).value;

    ref<Expr> l = NULL, r = NULL;
    unsigned lOffset = kgepi->offset*8, rOffset = kgepi->offset*8 + val->getWidth();

    if (lOffset > 0)
      l = ExtractExpr::create(agg, 0, lOffset);
    if (rOffset < agg->getWidth())
      r = ExtractExpr::create(agg, rOffset, agg->getWidth() - rOffset);

    ref<Expr> result;
    if (!l.isNull() && !r.isNull())
      result = ConcatExpr::create(r, ConcatExpr::create(val, l));
    else if (!l.isNull())
      result = ConcatExpr::create(val, l);
    else if (!r.isNull())
      result = ConcatExpr::create(r, val);
    else
      result = val;

    bindLocal(ki, state, result);
    break;
  }
  case Instruction::ExtractValue: {
    KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(ki);

    ref<Expr> agg = eval(ki, 0, state).value;

    ref<Expr> result = ExtractExpr::create(agg, kgepi->offset*8, getWidthForLLVMType(i->getType()));

    bindLocal(ki, state, result);
    break;
  }
 
    // Other instructions...
    // Unhandled
  case Instruction::ExtractElement:
  case Instruction::InsertElement:
  case Instruction::ShuffleVector:
    terminateStateOnError(state, "XXX vector instructions unhandled",
                          "xxx.err");
    break;
 
  default:
    terminateStateOnExecError(state, "illegal instruction");
    break;
  }
}

bool _qceListComparator(const QCEMap::value_type &a,
                       const QCEMap::value_type &b) {
  return a.second.qce > b.second.qce;
}

bool _qceHotValueComparator(const HotValue &a, const HotValue &b) {
  if (a.getValue()->hasName() && b.getValue()->hasName())
    return strcmp(a.getValue()->getNameStr().c_str(),
                  b.getValue()->getNameStr().c_str()) < 0;
  else if (a.getValue()->hasName())
    return true;
  else if (b.getValue()->hasName())
    return false;
  else
    return a < b;
}

void Executor::dumpQceMap(ExecutionState &state) {
  dbgs() << "Total QCE: " << state.stack().back().qceTotal << "\n";
  dbgs() << "qceMap map:\n";

  StackFrame &sf = state.stack().back();
  std::vector<QCEMap::value_type> qceList(sf.qceMap.begin(), sf.qceMap.end());
  std::sort(qceList.begin(), qceList.end(), _qceListComparator);

  foreach (QCEMap::value_type &p, qceList) {
    dbgs() << "  " << (p.second.inVhAdd ? "+" : "-")
           << " (" << p.second.qce << ") "
           << " vn=" << p.second.vnumber << " ";
    p.first.dump();
  }

  dbgs() << "qceMemoryTrackMap:\n";

  std::set<HotValue, bool (*)(const HotValue&, const HotValue&)>
      qceMemoryTrackSet(_qceHotValueComparator);
  foreach (QCEMemoryTrackMap::value_type &p,
           state.crtThread().qceMemoryTrackMap) {
    foreach (const HotValue &hv, p.second)
      qceMemoryTrackSet.insert(hv);
  }

  foreach (const HotValue &hotValue, qceMemoryTrackSet) {
    dbgs() << "  ";
    hotValue.dump();
  }

  dbgs() << "qceLocalsTrackMap:\n    ";
  for (unsigned i = 0, e = sf.kf->numRegisters; i != e; ++i) {
    if (sf.qceLocalsTrackMap.get(i)) {
      dbgs() << i << " ";
    }
  }
  dbgs() << "\n";
}

#ifdef VERIFY_QCE_MAPS
void Executor::verifyQceMap(ExecutionState &state) {
  unsigned stackSize = state.stack().size();
  StackFrame &sf = state.stack().back();

  DenseSet<HotValue> activeHotValues1;
  DenseSet<HotValue> activeHotValues2;

  BitArray qceLocals(sf.qceLocalsTrackMap, sf.kf->numRegisters);

  foreach (QCEMap::value_type &p, sf.qceMap) {
    if (p.second.inVhAdd) {
      if (p.first.isPtr()) {
        activeHotValues1.insert(p.first);
      } else {
        assert(p.second.stackFrame < stackSize);
        if (p.second.stackFrame == stackSize - 1) {
          assert(p.second.vnumber >= 0 &&
                 p.second.vnumber < int(sf.kf->numRegisters));
          if (!qceLocals.get(p.second.vnumber)) {
            dumpQceMap(state);
            assert(qceLocals.get(p.second.vnumber));
          }
          qceLocals.unset(p.second.vnumber);
        }
      }
    }
  }

  SimpleIncHash hash;
  foreach (QCEMemoryTrackMap::value_type &p,
           state.crtThread().qceMemoryTrackMap) {
    assert(!p.second.empty());

    foreach (const HotValue &hv, p.second)
      activeHotValues2.insert(hv);

    const MemoryObject *mo = p.first.first;
    const ObjectState *os = state.addressSpace().findObject(mo);
    assert(os != NULL);

    unsigned value = os->read8c(p.first.second);
    hash.addValueAt(APInt(32, value), mo, p.first.second);
  }

  foreach (const HotValue &hv, activeHotValues1) {
    DenseSet<HotValue>::iterator it = activeHotValues2.find(hv);
    if (it == activeHotValues2.end()) {
      dumpQceMap(state);
      assert(it != activeHotValues2.end());
    }
    activeHotValues2.erase(it);
  }

  if (activeHotValues2.size() != 0) {
    dumpQceMap(state);
    assert(activeHotValues2.size() == 0);
  }

  if (hash != state.crtThread().qceMemoryTrackHash) {
    dumpQceMap(state);
    assert(hash == state.crtThread().qceMemoryTrackHash);
  }

  /* We verify only top-level locals */
  SimpleIncHash lHash;
  for (unsigned i = 0; i < sf.kf->numRegisters; ++i) {
    if (qceLocals.get(i)) {
      dumpQceMap(state);
      assert(!qceLocals.get(i));
    }

    if (sf.qceLocalsTrackMap.get(i)) {
      ref<Expr> &value = sf.locals[i].value;
      if (!value.isNull() && isa<ConstantExpr>(value)) {
        lHash.addValueAt(cast<ConstantExpr>(value)->getAPValue(), i);
      } else {
        lHash.addValueAt(APInt(64, QCE_LOCALS_MAGIC_VALUE), i);
      }
    }
  }

  if (lHash != sf.qceLocalsTrackHash) {
    dumpQceMap(state);
    assert(lHash == sf.qceLocalsTrackHash);
  }

}
#endif

bool Executor::modifyQceMemoryTrackMap(ExecutionState &state,
                                       const HotValue &hotValue,
                                       int vnumber, bool inVhAdd,
                                       const char* reason,
                                       KInstruction *ki) {
  // Try to get address
  const Cell& cell = evalV(vnumber, state);
  if (cell.value.isNull()) {
    return false; // Not allocated yet
  }

  ref<ConstantExpr> address = dyn_cast<ConstantExpr>(cell.value);

  if (address.isNull()) {
    klee_warning("!!! XXX, qce tracked address is symbolic ?\n");
    return false; // XXX: tracked address is symbolic ?
  }

  address = address->Add(ConstantExpr::create(hotValue.getOffset(),
                                              address->getWidth()));

  // Resolve and check address
  ObjectPair op;
  bool ok = state.addressSpace().resolveOne(address, op);
  if (!ok) {
    klee_warning("!!! XXX: can not resolve qce track item address!\n");
    return false; // XXX!
  }

  const MemoryObject* mo = op.first;

  //Expr::Width width = getWidthForLLVMType(
  //      cast<PointerType>(hotValue.getValue()->getType())->getElementType());
  //uint64_t size = Expr::getMinBytesForWidth(width);
  uint64_t size = hotValue.getSize();

  ref<Expr> chk = op.first->getBoundsCheckPointer(address, size);
  assert(chk->isTrue() && "Invalid qce track item?");

  uint64_t offset = address->getZExtValue() - op.first->address;

  if (DebugQceMaps) {
    std::string str;
    raw_string_ostream ostr(str);

    if (inVhAdd)
      ostr << "Adding new ";
    else
      ostr << "Removing ";

    ostr << "qce memory track item: ";
    hotValue.print(ostr);

    if (reason)
      ostr << " " << reason;

    if (ki) {
      ostr << "\n     at instruction: ";
      Instruction *tmp = ki->inst->clone();
      if (ki->inst->hasName())
        tmp->setName(ki->inst->getName());
      tmp->setMetadata("qce", NULL);
      tmp->print(ostr);
      delete tmp;
      ostr << " (at " << ki->inst->getParent()->getParent()->getName() << ")\n";
      ostr << "     at " << ki->info->file << ":" << ki->info->line
           << " (assembly line " << ki->info->assemblyLine << ")";
    }
    fprintf(stderr, "\n%s\n", ostr.str().c_str());
  }

  QCEMemoryTrackMap &qceMemoryTrackMap = state.crtThread().qceMemoryTrackMap;
  SimpleIncHash &qceMemoryTrackHash = state.crtThread().qceMemoryTrackHash;

  if (inVhAdd) {
    for (; size; --size, ++offset) {
      std::pair<QCEMemoryTrackMap::iterator, bool> res =
       qceMemoryTrackMap.insert(
            std::make_pair(QCEMemoryTrackIndex(mo, offset),
                           QCEMemoryTrackSet()));
      res.first->second.insert(hotValue);
      if (res.second) {
        unsigned value = op.second->read8c(offset);
        qceMemoryTrackHash.addValueAt(APInt(32, value), mo, offset);
      }
    }
  } else {
    for (; size; --size, ++offset) {
      QCEMemoryTrackMap::iterator it = qceMemoryTrackMap.find(
            QCEMemoryTrackIndex(mo, offset));
      if (it == qceMemoryTrackMap.end()) {
        dumpQceMap(state);
        assert(false && "*** XXX: qce memory track item not found");
        continue;
      }

      bool erased = it->second.erase(hotValue);
      if (!erased) {
        dumpQceMap(state);
        assert(false && "*** XXX: qce memory track item not found");
        continue;
      }

      if (it->second.empty()) {
        qceMemoryTrackMap.erase(it);

        unsigned value = op.second->read8c(offset);
        qceMemoryTrackHash.removeValueAt(APInt(32, value), mo, offset);
      }
    }
  }

  return true;
}

/** This function is executed before any CallInst. It updates qceMaps to the
    state that it would have after the called function returns, except that
    it does not change inVhAdd values. QC estimations inside the called function
    are based on the estimations computed here: QC equals QC inside the
    function, plus QC in the caller just after the function returns */
void Executor::updateQceMapBeforeCall(ExecutionState &state) {
  verifyQceMap(state);

  KInstruction *ki = state.pc();
  if (KQCEInfo *info = ki->qceInfo) {
    /* Update query count estimation information */

    StackFrame &sf = state.stack().back();
    sf.qceTotal = sf.qceTotalBase + info->total;

#warning Here we should walk ALL qceMap items!!!

    foreach (KQCEInfoItem &item, info->vars) {
      // Update QCE estimation
      QCEMap::iterator qceMapIt = sf.qceMap.insert(
          std::make_pair(item.hotValue,
                QCEFrameInfo(state.stack().size()-1, item.vnumber))).first;

      QCEFrameInfo &frame = qceMapIt->second;
      frame.qce = frame.qceBase + item.qce;
    }
  }

  verifyQceMap(state);
}

/** This function is called just after a new frame is pushed to the stack. It
    updates qceTotalBase and qceBase values (setting them to QCE estimation in
    the caller just after the function return) to speedup future computations */
void Executor::updateQceMapOnFramePush(ExecutionState &state) {
  verifyQceMap(state);

#warning XXX: first we need to update qceMap in the caller to the state after the call!
  StackFrame &sf = state.stack().back();
  sf.qceTotalBase = sf.qceTotal;
  foreach (QCEMap::value_type &p, sf.qceMap) {
    p.second.qceBase = p.second.qce;
  }

  verifyQceMap(state);
}

/** This function is called just before a frame is popped from the stack. It
    propagates inVhAdd information to the caller, and removes qce track items
    that are local to this function (i.e., not present in the caller). */
void Executor::updateQceMapOnFramePop(ExecutionState &state) {
  verifyQceMap(state);

  unsigned stackSize = state.stack().size();
  StackFrame &sf = state.stack().back();
  StackFrame *tSf = stackSize >= 2 ? &state.stack()[stackSize - 2] : NULL;

  foreach (QCEMap::value_type &p, sf.qceMap) {
    QCEMap::iterator t = tSf ? tSf->qceMap.find(p.first) : QCEMap::iterator();
    if (tSf && t != tSf->qceMap.end()) {
      // Propagate current status to the parent
      t->second.inVhAdd = p.second.inVhAdd;
    } else {
      // Remove the track item
      assert(p.second.stackFrame == stackSize-1);
      if (p.second.inVhAdd) {
        bool ok;
        if (p.first.isPtr()) {
          ok = modifyQceMemoryTrackMap(state, p.first, p.second.vnumber, false,
                                       " on frame pop");
        } else {
          ok = modifyQceLocalsTrackMap(state, p.first, sf, p.second.vnumber,
                                       false, " on frame pop");
        }
        if (ok) {
          p.second.inVhAdd = false;
        } else {
          klee_warning("*** XXX: can not remove qce memory track item on frame pop\n");
          assert(false);
        }
      }
    }
  }

  verifyQceMap(state);

  if (stackSize == 1) {
    // This was the last frame. Do the final check.
    assert(state.crtThread().qceMemoryTrackHash == SimpleIncHash());
  }
}

void Executor::updateQceMapOnFree(ExecutionState &state, const MemoryObject *mo,
                                  KInstruction*) {
  verifyQceMap(state);

  bool changed = false;
  DenseSet<HotValue> removedValues;

  QCEMemoryTrackMap &qceMemoryTrackMap = state.crtThread().qceMemoryTrackMap;
  SimpleIncHash &qceMemoryTrackHash = state.crtThread().qceMemoryTrackHash;

  const ObjectState *os = state.addressSpace().findObject(mo);
  assert(os && "qce memory track item was freed before disabling it");

  for (QCEMemoryTrackMap::iterator bi = qceMemoryTrackMap.begin(),
                                   be = qceMemoryTrackMap.end(); bi != be;) {
    if (bi->first.first != mo) {
      ++bi;
      continue;
    }

    foreach (const HotValue &hotValue, bi->second) {
      QCEMap::iterator qceMapIt = state.stack().back().qceMap.find(hotValue);
      assert(qceMapIt != state.stack().back().qceMap.end());

      // Remove blacklist item
      if (DebugQceMaps) {
        if (removedValues.count(hotValue) == 0) {
          assert(qceMapIt->second.inVhAdd);

          std::string str;
          raw_string_ostream ostr(str);
          ostr << "Removing qce memory track item: ";
          hotValue.print(ostr);
          ostr << " on free";
          fprintf(stderr, "%s\n", ostr.str().c_str());
          removedValues.insert(hotValue);
        }
      }

      qceMapIt->second.inVhAdd = false;
      changed = true;
    }

    // Remove value from the hash
    unsigned value = os->read8c(bi->first.second);
    qceMemoryTrackHash.removeValueAt(APInt(32, value), mo, bi->first.second);

    qceMemoryTrackMap.erase(bi++);
  }

  if (changed) {
    verifyQceMap(state);
  }
}

bool Executor::modifyQceLocalsTrackMap(ExecutionState &state,
                                       const HotValue &hotValue,
                                       StackFrame &sf, int vnumber,
                                       bool inVhAdd,
                                       const char* reason,
                                       KInstruction *ki) {
  assert(vnumber >= 0 && vnumber < int(sf.kf->numRegisters));

  if (DebugQceMaps) {
    std::string str;
    raw_string_ostream ostr(str);

    if (inVhAdd)
      ostr << "Adding new ";
    else
      ostr << "Removing ";

    ostr << "qce locals track item: ";
    hotValue.print(ostr);

    if (reason)
      ostr << " " << reason;

    if (ki) {
      ostr << "\n     at instruction: ";
      Instruction *tmp = ki->inst->clone();
      if (ki->inst->hasName())
        tmp->setName(ki->inst->getName());
      tmp->setMetadata("qce", NULL);
      tmp->print(ostr);
      delete tmp;
      ostr << " (at " << ki->inst->getParent()->getParent()->getName() << ")\n";
      ostr << "     at " << ki->info->file << ":" << ki->info->line
           << " (assembly line " << ki->info->assemblyLine << ")";
    }
    fprintf(stderr, "\n%s\n", ostr.str().c_str());
  }

  if (inVhAdd) {
    assert(!sf.qceLocalsTrackMap.get(vnumber));

    sf.qceLocalsTrackMap.set(vnumber);

    ref<Expr> &value = sf.locals[vnumber].value;
    if (!value.isNull() && isa<ConstantExpr>(value)) {
      sf.qceLocalsTrackHash.addValueAt(
            cast<ConstantExpr>(value)->getAPValue(), vnumber);
    } else {
      sf.qceLocalsTrackHash.addValueAt(
            APInt(64, QCE_LOCALS_MAGIC_VALUE), vnumber);
    }
  } else {
    assert(sf.qceLocalsTrackMap.get(vnumber));

    ref<Expr> &value = sf.locals[vnumber].value;
    if (!value.isNull() && isa<ConstantExpr>(value)) {
      sf.qceLocalsTrackHash.removeValueAt(
            cast<ConstantExpr>(value)->getAPValue(), vnumber);
    } else {
      sf.qceLocalsTrackHash.removeValueAt(
            APInt(64, QCE_LOCALS_MAGIC_VALUE), vnumber);
    }

    sf.qceLocalsTrackMap.unset(vnumber);
  }

  return true;
}

/** This function is called after each instruction execution to update QCE
    information to the current state, adding/removing qce track items as
    necessary */
void Executor::updateQceMapOnExec(ExecutionState &state) {
  KInstruction *ki = state.pc();
  if (KQCEInfo *info = ki->qceInfo) {
    verifyQceMap(state);

    bool changed = false;
    /* Update query count estimation information */

    unsigned stackSize = state.stack().size();
    StackFrame &sf = state.stack().back();
    sf.qceTotal = sf.qceTotalBase + info->total;
    float threshold = sf.qceTotal * QceThreshold;

#warning Here we should walk ALL qceMap items!!!

    foreach (KQCEInfoItem &item, info->vars) {
      // Update QCE estimation
      std::pair<QCEMap::iterator, bool> res = sf.qceMap.insert(
          std::make_pair(item.hotValue,
                    QCEFrameInfo(state.stack().size()-1, item.vnumber)));

      QCEMap::iterator qceMapIt = res.first;
      QCEFrameInfo &frame = qceMapIt->second;

      if (res.second
          && isa<Argument>(item.hotValue.getValue())
          && ki->inst ==
               ki->inst->getParent()->getParent()->getEntryBlock().begin()
          && sf.caller) {

        KInstruction *kCS = sf.caller;
        assert(isa<CallInst>(kCS->inst) || isa<InvokeInst>(kCS->inst));

        HotValueArgMap &argMap =
            static_cast<KCallInstruction*>(kCS)->hotValueArgMap;
        HotValueArgMap::iterator it = argMap.find(item.hotValue);
        if (it != argMap.end()) {
          assert(stackSize > 1);
          StackFrame &tSf = state.stack()[stackSize-2];
          foreach (const HotValue& hv, it->second) {
            QCEMap::iterator qIt = tSf.qceMap.find(hv);
            if (qIt != tSf.qceMap.end() && qIt->second.qce > frame.qceBase)
              frame.qceBase = qIt->second.qce;
          }
        }
      }

      /*
      assert(
        ki->inst == ki->inst->getParent()->getParent()->getEntryBlock().begin()
        || frame.qce < 0.5 || frame.qce + 0.5 > frame.qceBase + item.qce);
        */

      frame.qce = frame.qceBase + item.qce;

      // Now check whether to add the item to Vh
      bool inVhAdd = frame.qce > threshold &&
                     frame.qce > QceAbsThreshold;
      if (inVhAdd != frame.inVhAdd) {
        bool ok;
        if (item.hotValue.isPtr()) {
          ok = modifyQceMemoryTrackMap(state, item.hotValue,
                                       item.vnumber, inVhAdd, NULL, ki);
        } else {
          assert(frame.stackFrame < stackSize);
          ok = modifyQceLocalsTrackMap(state, item.hotValue,
                                       state.stack()[frame.stackFrame],
                                       item.vnumber, inVhAdd, NULL, ki);
        }
        if (ok) {
          frame.inVhAdd = inVhAdd;
          changed = true;
        } else {
          assert(!frame.inVhAdd); // XXX?
        }
      }

      // Remove items with zero QCE
      if (frame.qce < 0.5 && !frame.inVhAdd) {
        if (stackSize > 1) {
          QCEMap &pMap = state.stack()[stackSize-2].qceMap;
          QCEMap::iterator pIt = pMap.find(item.hotValue);
          if (pIt == pMap.end() || !pIt->second.inVhAdd) {
            sf.qceMap.erase(qceMapIt);
          }
        } else {
          sf.qceMap.erase(qceMapIt);
        }
      }
    }

    if (changed)
      verifyQceMap(state);
  }
}

void Executor::updateQceMemoryValue(ExecutionState &state,
                                    const MemoryObject *mo, ObjectState *os,
                                    ref<Expr> offset, ref<Expr> newValue,
                                    KInstruction *ki) {
  //if (os->numBlacklistRefs == 0)
  //  return; // This ObjectState is not involved in any blacklist item

  QCEMemoryTrackMap &qceMemoryTrackMap = state.crtThread().qceMemoryTrackMap;
  SimpleIncHash &qceMemoryTrackHash = state.crtThread().qceMemoryTrackHash;

  //bool notify = false;

  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(offset)) {
    if (newValue->getWidth() == 1)
      newValue = ZExtExpr::create(newValue, Expr::Int8);

    unsigned oc = CE->getZExtValue();
    unsigned size = newValue->getWidth()/8;

    for (unsigned i = 0; i < size; ++i, ++oc) {
      // Do not update bytes that are not in blacklist
      if (qceMemoryTrackMap.find(std::make_pair(mo, oc)) ==
                qceMemoryTrackMap.end())
        continue;

      // Remove the old value
      unsigned prevValueC = os->read8c(oc);
      qceMemoryTrackHash.removeValueAt(APInt(32, prevValueC), mo, oc);

      // Add the new value
      unsigned newValueC = unsigned(-1);
      ref<Expr> V = ExtractExpr::create(newValue, 8*i, Expr::Int8);
      if (ConstantExpr *CV = dyn_cast<ConstantExpr>(V))
        newValueC = CV->getZExtValue() & 0xFF;

      qceMemoryTrackHash.addValueAt(APInt(32, newValueC), mo, oc);

      //if (prevValueC != unsigned(-1) && !nextValueC == unsigned(-1))
      //  notify = true;
    }
  } else {
    // A write with symbolic address makes all bytes in array symbolic
    for (unsigned oc = 0; oc < os->size; ++oc) {
      // Do not update bytes that are not in blacklist
      if (qceMemoryTrackMap.find(std::make_pair(mo, oc)) ==
                qceMemoryTrackMap.end())
        continue;

      // Remove the old value
      unsigned prevValueC = os->read8c(oc);
      qceMemoryTrackHash.removeValueAt(APInt(32, prevValueC), mo, oc);

      // Add the new value (symbolic)
      qceMemoryTrackHash.addValueAt(APInt(32, unsigned(-1)), mo, oc);

      //if (prevValueC != unsigned(-1))
      //  notify = true;
    }
  }

  /*
  if (notify && DebugLogMergeBlacklistVals) {
    std::cerr << "*** Wrote symbolic value to blacklisted memory: ";
    mo->allocSite->dump();
    std::cerr << "  offset: "; offset->dump();
    if (ki)
      std::cerr << "  at:\n  "; ki->dump();
    std::cerr << "  New value: "; newValue->dump();
  }
  */
}

void Executor::updateQceLocalsValue(ExecutionState &state,
                                    int vnumber, ref<Expr> &newValue,
                                    KInstruction *ki) {
  if (vnumber < 0)
    return;

  StackFrame &sf = state.stack().back();
  assert(unsigned(vnumber) < sf.kf->numRegisters);

  if (!sf.qceLocalsTrackMap.get(vnumber))
    return;

  //bool prevIsConcrete = false;
  //bool nextIsConcrete = false;

  ref<Expr> &value = sf.locals[vnumber].value;
  if (!value.isNull() && isa<ConstantExpr>(value)) {
    sf.qceLocalsTrackHash.removeValueAt(
          cast<ConstantExpr>(value)->getAPValue(), vnumber);
  } else {
    sf.qceLocalsTrackHash.removeValueAt(
          APInt(64, QCE_LOCALS_MAGIC_VALUE), vnumber);
  }

  if (!newValue.isNull() && isa<ConstantExpr>(newValue)) {
    sf.qceLocalsTrackHash.addValueAt(
          cast<ConstantExpr>(newValue)->getAPValue(), vnumber);
  } else {
    sf.qceLocalsTrackHash.addValueAt(
          APInt(64, QCE_LOCALS_MAGIC_VALUE), vnumber);
  }

  /*
  if (prevIsConcrete && !nextIsConcrete && DebugLogMergeBlacklistVals) {
    std::cerr << "*** Wrote symbolic value to blacklisted local:\n  ";
    if (target)
      target->dump();
    std::cerr << " New value: "; newValue->dump();
  }
  */
}

void Executor::updateStates(ExecutionState *current) {
  if (searcher) {
    WallTimer searcherTimer;
    searcher->update(current, addedStates, removedStates);
    stats::searcherTime += searcherTimer.check();
  }
  
  states.insert(addedStates.begin(), addedStates.end());
  addedStates.clear();
  
  bool processTreeChanged = false;
  for (std::set<ExecutionState*>::iterator
         it = removedStates.begin(), ie = removedStates.end();
       it != ie; ++it) {
    ExecutionState *es = *it;
    std::set<ExecutionState*>::iterator it2 = states.find(es);
    assert(it2!=states.end());
    states.erase(it2);
    std::map<ExecutionState*, std::vector<SeedInfo> >::iterator it3 = 
      seedMap.find(es);
    if (it3 != seedMap.end())
      seedMap.erase(it3);
    if(es->ptreeNode->state != PTreeNode::MERGED) {
      es->ptreeNode->data = NULL;
      processTree->terminate(es->ptreeNode);
      processTreeChanged = true;
    }
    delete es;
  }
  if(processTreeChanged && DumpPTreeOnChange)
    dumpProcessTree();
  removedStates.clear();
}

template <typename TypeIt>
void Executor::computeOffsets(KGEPInstruction *kgepi, TypeIt ib, TypeIt ie) {
  ref<ConstantExpr> constantOffset =
    ConstantExpr::alloc(0, Context::get().getPointerWidth());
  uint64_t index = 1;
  for (TypeIt ii = ib; ii != ie; ++ii) {
    if (const StructType *st = dyn_cast<StructType>(*ii)) {
      const StructLayout *sl = kmodule->targetData->getStructLayout(st);
      const ConstantInt *ci = cast<ConstantInt>(ii.getOperand());
      uint64_t addend = sl->getElementOffset((unsigned) ci->getZExtValue());
      constantOffset = constantOffset->Add(ConstantExpr::alloc(addend,
                                                               Context::get().getPointerWidth()));
    } else {
      const SequentialType *set = cast<SequentialType>(*ii);
      uint64_t elementSize = 
        kmodule->targetData->getTypeStoreSize(set->getElementType());
      Value *operand = ii.getOperand();
      if (Constant *c = dyn_cast<Constant>(operand)) {
        ref<ConstantExpr> index = 
          evalConstant(c)->ZExt(Context::get().getPointerWidth());
        ref<ConstantExpr> addend = 
          index->Mul(ConstantExpr::alloc(elementSize,
                                         Context::get().getPointerWidth()));
        constantOffset = constantOffset->Add(addend);
      } else {
        kgepi->indices.push_back(std::make_pair(index, elementSize));
      }
    }
    index++;
  }
  kgepi->offset = constantOffset->getZExtValue();
}

void Executor::bindInstructionConstants(KInstruction *KI) {
  KGEPInstruction *kgepi = static_cast<KGEPInstruction*>(KI);

  if (GetElementPtrInst *gepi = dyn_cast<GetElementPtrInst>(KI->inst)) {
    computeOffsets(kgepi, gep_type_begin(gepi), gep_type_end(gepi));
  } else if (InsertValueInst *ivi = dyn_cast<InsertValueInst>(KI->inst)) {
    computeOffsets(kgepi, iv_type_begin(ivi), iv_type_end(ivi));
    assert(kgepi->indices.empty() && "InsertValue constant offset expected");
  } else if (ExtractValueInst *evi = dyn_cast<ExtractValueInst>(KI->inst)) {
    computeOffsets(kgepi, ev_type_begin(evi), ev_type_end(evi));
    assert(kgepi->indices.empty() && "ExtractValue constant offset expected");
  }
}

void Executor::bindModuleConstants() {
  for (std::vector<KFunction*>::iterator it = kmodule->functions.begin(), 
         ie = kmodule->functions.end(); it != ie; ++it) {
    KFunction *kf = *it;
    for (unsigned i=0; i<kf->numInstructions; ++i)
      bindInstructionConstants(kf->instructions[i]);
  }

  kmodule->constantTable = new Cell[kmodule->constants.size()];
  for (unsigned i=0; i<kmodule->constants.size(); ++i) {
    Cell &c = kmodule->constantTable[i];
    c.value = evalConstant(kmodule->constants[i]);
  }
}

void Executor::stepInState(ExecutionState *state) {
  assert(addedStates.count(state) == 0);
  assert(state->duplicates.empty() || state->multiplicity > 1);

  std::set<ExecutionState*> duplicates;
  duplicates.swap(state->duplicates);
  state->multiplicityExact = std::max(duplicates.size(), 1ul);

  getValuePreferences.clear();
  KInstruction *ki = state->pc();

#if 0
  if (ki->inst == &ki->inst->getParent()->front()) {
    if (ki->inst->getParent() ==
        &ki->inst->getParent()->getParent()->getEntryBlock()) {
      std::cerr << &state << " Entering function "
                << ki->inst->getParent()->getParent()->getNameStr()
                << std::endl;
    }
    std::cerr << &state << " Executing BB "
              << ki->inst->getParent()->getNameStr()
              << std::endl;
  }
  if (isa<ReturnInst>(ki->inst)) {
    std::cerr << &state << " Returning from function "
              << ki->inst->getParent()->getParent()->getNameStr()
              << std::endl;
  }
  if (isa<UnwindInst>(ki->inst)) {
    std::cerr << &state << " Unwinding from function "
              << ki->inst->getParent()->getParent()->getNameStr()
              << std::endl;
  }
#endif
#if 0
  if (ki->info->assemblyLine == 9150) {
    std::cerr << "9150" << std::endl;
  }
#endif

	stepInstruction(*state, true);

	//CLOUD9_DEBUG("Executing instruction: " << ki->info->assemblyLine);

  uint64_t executionTime = 0, duplicatesExecutionTime = 0;
  resetTimers();

  if (UseQueryPCLog) {
    foreach(Solver* s, loggingSolvers)
      setPCLoggingSolverStateID(s, state);
  }

  assert(addedStates.size() == 0);

  {
    WallTimer timer;
    state->lastResolveResult = 0;
    executeInstruction(*state, ki);
    executionTime = timer.check();

    stats::executionTime += executionTime;
    stats::instructionsMultExact += duplicates.size();
    if (KeepMergedDuplicates && duplicates.empty()) {
      duplicatesExecutionTime += executionTime;
      stats::duplicatesExecutionTime += executionTime;
      stats::forksMultExact += addedStates.size();
      ++stats::instructionsMultExact;
    }
  }

  if (removedStates.count(state) == 0)
    updateQceMapOnExec(*state);

  foreach (ExecutionState* addedState, addedStates)
    updateQceMapOnExec(*addedState);

  if (UseQueryPCLog) {
    foreach(Solver* s, loggingSolvers)
      setPCLoggingSolverStateID(s, 0);
  }
  state->stateTime++; // Each instruction takes one unit of time

  processTimers(state, MaxInstructionTime);

  if (KeepMergedDuplicates && !duplicates.empty()) {
    //klee_warning(">>> Running duplicated states.\n");
    bool stateIsTerminated = removedStates.count(state) != 0;

    std::set<ExecutionState*> savedAddedStates;
    savedAddedStates.swap(addedStates);

    std::set<ExecutionState*> savedRemovedStates;
    savedRemovedStates.swap(removedStates);

    std::set<ExecutionState*> nextStates(savedAddedStates);
    if (!stateIsTerminated)
      nextStates.insert(state);

    uint64_t forks = stats::forks.getValue();
    uint64_t forksMult = stats::forksMult.getValue();

    foreach (ExecutionState* state, nextStates)
      state->multiplicityExact = 0;

    foreach (ExecutionState* duplicate, duplicates) {
      // Execute the same instruction in a duplicate state
      assert(duplicate->isDuplicate);
      //assert(/*stateIsTerminated || */duplicate->pc() == state->prevPC());
      // XXX
      if (duplicate->pc() != state->prevPC()) {
        klee_warning("*** Duplicate diverged");
        delete duplicate;
        continue;
      }
      ki = duplicate->pc();
      duplicate->setPrevPC(duplicate->pc());
      duplicate->setPC(duplicate->pc().next());
      if (UseQueryPCLog) {
        foreach(Solver* s, loggingSolvers)
          setPCLoggingSolverStateID(s, duplicate);
      }
      {
        WallTimer timer;
        duplicate->lastResolveResult = 0;
        executeInstruction(*duplicate, ki);
        uint64_t time = timer.check();
        duplicatesExecutionTime += time;
        stats::duplicatesExecutionTime += time;
      }
      if (UseQueryPCLog) {
        foreach(Solver* s, loggingSolvers)
          setPCLoggingSolverStateID(s, 0);
      }
      duplicate->stateTime++;

      //assert(stats::forks.getValue() == forks + addedStates.size());
      //assert(stats::forksMult.getValue() == forksMult + addedStates.size());

      stats::forks += forks - stats::forks.getValue();
      stats::forksMult += forksMult - stats::forksMult.getValue();

      assert(stats::forks.getValue() == forks);
      assert(stats::forksMult.getValue() == forksMult);

      stats::forksMultExact += addedStates.size();

      // Sort all next states into duplicates of the main state
      if (removedStates.count(duplicate) == 0)
        addedStates.insert(duplicate);

      foreach (ExecutionState* addedState, addedStates)
        updateQceMapOnExec(*addedState);

      foreach (ExecutionState* addedState, addedStates) {
        assert(removedStates.count(addedState) == 0);
        bool found = false;

        if (!nextStates.empty()) {
          foreach (ExecutionState* nextMain, nextStates) {
            bool match = false;
            if (nextStates.size() > 1 &&
                nextMain->ptreeNode->parent->forkTag.forkClass == KLEE_FORK_RESOLVE) {
              match = nextMain->lastResolveResult == addedState->lastResolveResult;
            } else {
              match = nextMain->pc() == addedState->pc();
            }

            if (match) {
              //assert(!found);
              if (found) {
                klee_warning("*** Cannot match duplicate (more than one candidate)! Paths computation are no longer exact.");
              }
              found = true;
              nextMain->duplicates.insert(addedState);
              nextMain->multiplicityExact++;
            }
          }
          //assert(found);
          if (!found) {
            klee_warning("*** Cannot match duplicate (no candidates)! Paths computation are no longer exact.");
            // These will be fixed later, when states will diverge
            foreach (ExecutionState* nextMain, nextStates) {
              nextMain->duplicates.insert(addedState);
              nextMain->multiplicityExact++;
            }
          }
        } else {
          // delete addedState;
        }
          // It might happen that the main state were terminated (e.g., in case of timeout)
      }

      addedStates.clear();

      foreach (ExecutionState* removedState, removedStates) {
        assert(removedState == duplicate || duplicates.count(removedState) == 0);
      }
      //  delete removedState;
      //}
      removedStates.clear();
    }

    /*
    foreach (ExecutionState* nextMain, nextStates) {
      assert(!nextMain->duplicates.empty()
             && nextMain->multiplicityExact == nextMain->duplicates.size());
    }
    */

    addedStates.swap(savedAddedStates);
    removedStates.swap(savedRemovedStates);

    //klee_warning("<<< Finished running duplicated states.\n");
  }

  if (KeepMergedDuplicates && DebugMergeSlowdown &&
      executionTime > 50 && executionTime > 5*duplicatesExecutionTime) {
    klee_warning("Merged state is slow: %g instead of %g for individual states",
                 executionTime / 1000000., duplicatesExecutionTime / 1000000.);
    KInstruction *ki = (KInstruction*) state->prevPC();
    std::cerr << "  " << duplicates.size() << " duplicares, "
        << addedStates.size() << " added states" << std::endl;
    std::cerr << "  At " << ki->info->file << ":"
        << ki->info->line << std::endl;
    uint64_t size = state->stack().size();
    for (unsigned i = 1; i <= std::min(10ul, size); ++i) {
      if (state->stack()[size-i].caller) {
        std::cerr << "    " << state->stack()[size-i].caller->info->file
            << ":" << state->stack()[size-i].caller->info->line << std::endl;
      }
    }
    if (size > 10)
      std::cerr << "    ..." << std::endl;
    ki->dump();
    /*
    std::cerr << "  Instruction:" << std::endl << "    ";
    ki->inst->dump();
    std::cerr << "    at " << ki->info->file << ":" << ki->info->line
              << " (assembly line " << ki->info->assemblyLine << ")\n";
              */
    if ((isa<BranchInst>(ki->inst) && cast<BranchInst>(ki->inst)->isConditional())
            || isa<SwitchInst>(ki->inst)) {
      std::cerr << "  Branch condition in merged state:"<< std::endl << "    ";
      eval(ki, 0, *state).value->dump();
      std::cerr << "  Branch conditions in duplicates:" << std::endl;
      foreach (ExecutionState *d, duplicates) {
        std::cerr << "    ";
        eval(ki, 0, *d).value->dump();
      }
    } else if (isa<LoadInst>(ki->inst)) {
      std::cerr << "  Load address in merged state:"<< std::endl << "    ";
      eval(ki, 0, *state).value->dump();
      std::cerr << "  Load address in duplicates:" << std::endl;
      foreach (ExecutionState *d, duplicates) {
        std::cerr << "    ";
        eval(ki, 0, *d).value->dump();
      }
    } else if (isa<StoreInst>(ki->inst)) {
      std::cerr << "  Store address in merged state:"<< std::endl << "    ";
      eval(ki, 1, *state).value->dump();
      std::cerr << "  Store address in duplicates:" << std::endl;
      foreach (ExecutionState *d, duplicates) {
        std::cerr << "    ";
        eval(ki, 1, *d).value->dump();
      }
    }
    std::cerr << std::endl;
  }

	if (MaxMemory) {
		if ((stats::instructions & 0xFFFF) == 0) {
			// We need to avoid calling GetMallocUsage() often because it
			// is O(elts on freelist). This is really bad since we start
			// to pummel the freelist once we hit the memory cap.
			unsigned mbs = sys::Process::GetTotalMemoryUsage() >> 20;

			if (mbs > MaxMemory) {
				if (mbs > MaxMemory + 100) {
					// just guess at how many to kill
					unsigned numStates = states.size();
					unsigned toKill = std::max(1U, numStates - numStates
							* MaxMemory / mbs);

					if (MaxMemoryInhibit)
						klee_warning("killing %d states (over memory cap)",
								toKill);

					std::vector<ExecutionState*> arr(states.begin(),
							states.end());
					for (unsigned i = 0, N = arr.size(); N && i < toKill; ++i, --N) {
						unsigned idx = rand() % N;

						// Make two pulls to try and not hit a state that
						// covered new code.
						if (arr[idx]->coveredNew)
							idx = rand() % N;

						std::swap(arr[idx], arr[N - 1]);

						fireOutOfResources(arr[N-1]);
						terminateStateEarly(*arr[N - 1], "memory limit");
					}
				}
				atMemoryLimit = true;
			} else {
				atMemoryLimit = false;
			}
		}
	}

	fireControlFlowEvent(state, ::cloud9::worker::STEP);

    updateStates(state);

    getValuePreferences.clear();
}

void Executor::run(ExecutionState &initialState) {
  if (usingSeeds) {
    std::vector<SeedInfo> &v = seedMap[&initialState];
    
    for (std::vector<KTest*>::const_iterator it = usingSeeds->begin(), 
           ie = usingSeeds->end(); it != ie; ++it)
      v.push_back(SeedInfo(*it));

    int lastNumSeeds = usingSeeds->size()+10;
    double lastTime, startTime = lastTime = util::getWallTime();
    ExecutionState *lastState = 0;
    while (!seedMap.empty()) {
      if (haltExecution) goto dump;

      std::map<ExecutionState*, std::vector<SeedInfo> >::iterator it = 
        seedMap.upper_bound(lastState);
      if (it == seedMap.end())
        it = seedMap.begin();
      lastState = it->first;
      unsigned numSeeds = it->second.size();
      ExecutionState &state = *lastState;
      KInstruction *ki = state.pc();
      stepInstruction(state, true);

      executeInstruction(state, ki);
      processTimers(&state, MaxInstructionTime * numSeeds);
      updateStates(&state);

      if ((stats::instructions % 1000) == 0) {
        int numSeeds = 0, numStates = 0;
        for (std::map<ExecutionState*, std::vector<SeedInfo> >::iterator
               it = seedMap.begin(), ie = seedMap.end();
             it != ie; ++it) {
          numSeeds += it->second.size();
          numStates++;
        }
        double time = util::getWallTime();
        if (SeedTime>0. && time > startTime + SeedTime) {
          klee_warning("seed time expired, %d seeds remain over %d states",
                       numSeeds, numStates);
          break;
        } else if (numSeeds<=lastNumSeeds-10 ||
                   time >= lastTime+10) {
          lastTime = time;
          lastNumSeeds = numSeeds;          
          klee_message("%d seeds remaining over: %d states", 
                       numSeeds, numStates);
        }
      }
    }

    klee_message("seeding done (%d states remain)", (int) states.size());

    // XXX total hack, just because I like non uniform better but want
    // seed results to be equally weighted.
    for (std::set<ExecutionState*>::iterator
           it = states.begin(), ie = states.end();
         it != ie; ++it) {
      (*it)->weight = 1.;
    }

    if (OnlySeed)
      goto dump;
  }

  searcher = initSearcher(NULL);

  searcher->update(0, states, std::set<ExecutionState*>());

  // Tell searcher about initial current state
  searcher->update(&initialState, std::set<ExecutionState*>(),
                   std::set<ExecutionState*>());

  //while (!states.empty() && !haltExecution) {
  while (!searcher->empty() && !haltExecution) {
    assert(addedStates.empty() && removedStates.empty());

    WallTimer searcherTimer;
    ExecutionState &state = searcher->selectState();
    stats::searcherTime += searcherTimer.check();

    if (!addedStates.empty())
      updateStates(0);

    stepInState(&state);
  }

  delete searcher;
  searcher = 0;
  
 dump:
  if (DumpStatesOnHalt && !states.empty()) {
    std::cerr << "KLEE: halting execution, dumping remaining states\n";
    for (std::set<ExecutionState*>::iterator
           it = states.begin(), ie = states.end();
         it != ie; ++it) {
      ExecutionState &state = **it;
      stepInstruction(state, true); // keep stats rolling
      terminateStateEarly(state, "execution halting");
    }
    updateStates(0);
  }
}

std::string Executor::getAddressInfo(ExecutionState &state, 
                                     ref<Expr> address) const{
  std::ostringstream info;
  info << "\taddress: " << address << "\n";
  uint64_t example;
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(address)) {
    example = CE->getZExtValue();
  } else {
    ref<ConstantExpr> value;
    bool success = solver->getValue(state, address, value);
    assert(success && "FIXME: Unhandled solver failure");
    (void) success;
    example = value->getZExtValue();
    info << "\texample: " << example << "\n";
    std::pair< ref<Expr>, ref<Expr> > res = solver->getRange(state, address);
    info << "\trange: [" << res.first << ", " << res.second <<"]\n";
  }
  
  MemoryObject hack((unsigned) example);    
  MemoryMap::iterator lower = state.addressSpace().objects.upper_bound(&hack);
  info << "\tnext: ";
  if (lower==state.addressSpace().objects.end()) {
    info << "none\n";
  } else {
    const MemoryObject *mo = lower->first;
    std::string alloc_info;
    mo->getAllocInfo(alloc_info);
    info << "object at " << mo->address
         << " of size " << mo->size << "\n"
         << "\t\t" << alloc_info << "\n";
  }
  if (lower!=state.addressSpace().objects.begin()) {
    --lower;
    info << "\tprev: ";
    if (lower==state.addressSpace().objects.end()) {
      info << "none\n";
    } else {
      const MemoryObject *mo = lower->first;
      std::string alloc_info;
      mo->getAllocInfo(alloc_info);
      info << "object at " << mo->address 
           << " of size " << mo->size << "\n"
           << "\t\t" << alloc_info << "\n";
    }
  }

  return info.str();
}

bool Executor::terminateState(ExecutionState &state, bool silenced) {
	fireStateDestroy(&state, silenced);

	if (replayOut && replayPosition != replayOut->numObjects) {
		klee_warning_once(replayOut,
				"replay did not consume all objects in test input.");
	}

  if (state.ptreeNode->state != PTreeNode::MERGED && !state.isDuplicate) {
    interpreterHandler->incPathsExplored();
    ++stats::paths;
    stats::pathsMult += state.multiplicity;
    stats::pathsMultExact += state.multiplicityExact;

    if (OutputConstraints) {
      (*constraintsLog) << "# STATE[";
      (*constraintsLog) << "Instructions=" << stats::instructions
                        << ",WallTime=" << statsTracker->elapsed()
                        << ",ExecutionTime=" << stats::executionTime / 1000000.
                        << ",Paths=" << stats::paths
                        << ",PathsMult=" << stats::pathsMult
                        << ",PathsMultExact=" << stats::pathsMultExact
                        << ",StateMultiplicity=" << uint64_t(state.multiplicity)
                        << ",StateMultiplicityExact=" << state.multiplicityExact
                        << "]" << std::endl;
      ExprPPrinter::printConstraints(*constraintsLog, state.constraints());
      (*constraintsLog) << "# END_STATE" << std::endl << std::flush;
    }
  }

	std::set<ExecutionState*>::iterator it = addedStates.find(&state);
	if (it == addedStates.end()) {
      state.setPC(state.prevPC());

	  removedStates.insert(&state);
	} else {
		// never reached searcher, just delete immediately
		std::map<ExecutionState*, std::vector<SeedInfo> >::iterator it3 =
				seedMap.find(&state);
		if (it3 != seedMap.end())
			seedMap.erase(it3);
		addedStates.erase(it);

		if(state.ptreeNode->state != PTreeNode::MERGED) {
		  state.ptreeNode->data = NULL;
		  processTree->terminate(state.ptreeNode);
		  if(DumpPTreeOnChange)
		    dumpProcessTree();
		}

    delete &state;
	}

	return true;
}

void Executor::terminateStateEarly(ExecutionState &state, 
                                   const Twine &message) {
  if (!OnlyOutputStatesCoveringNew || state.coveredNew ||
      (AlwaysOutputSeeds && seedMap.count(&state))) {
    interpreterHandler->processTestCase(state, (message + "\n").str().c_str(),
                                        "early");
    terminateState(state, false);
  } else {
    terminateState(state, true);
  }
}

void Executor::terminateStateOnExit(ExecutionState &state) {
  if (!OnlyOutputStatesCoveringNew || state.coveredNew || 
      (AlwaysOutputSeeds && seedMap.count(&state))) {
    interpreterHandler->processTestCase(state, 0, 0);
    terminateState(state, false);
  } else {
    terminateState(state, true);
  }
}

void Executor::terminateStateOnError(ExecutionState &state,
                                     const llvm::Twine &messaget,
                                     const char *suffix,
                                     const llvm::Twine &info) {
  std::string message = messaget.str();
  static std::set< std::pair<Instruction*, std::string> > emittedErrors;

  assert(state.crtThreadIt != state.threads.end());

  const InstructionInfo &ii = *state.prevPC()->info;

  if (EmitAllErrors ||
      emittedErrors.insert(std::make_pair(state.prevPC()->inst, message)).second) {
    if (ii.file != "") {
      klee_message("ERROR: %s:%d: %s", ii.file.c_str(), ii.line, message.c_str());
    } else {
      klee_message("ERROR: %s", message.c_str());
    }
    if (!EmitAllErrors)
      klee_message("NOTE: now ignoring this error at this location");

    std::ostringstream msg;
    msg << "Error: " << message << "\n";
    if (ii.file != "") {
      msg << "File: " << ii.file << "\n";
      msg << "Line: " << ii.line << "\n";
    }
    msg << "Stack: \n";
    state.getStackTrace().dump(msg);

    std::string info_str = info.str();
    if (info_str != "")
      msg << "Info: \n" << info_str;
    interpreterHandler->processTestCase(state, msg.str().c_str(), suffix);
    terminateState(state, false);
  } else {
    terminateState(state, true);
  }
}

// XXX shoot me
static const char *okExternalsList[] = { "printf", 
                                         "fprintf", 
                                         "puts",
                                         "getpid" };
static std::set<std::string> okExternals(okExternalsList,
                                         okExternalsList + 
                                         (sizeof(okExternalsList)/sizeof(okExternalsList[0])));

void Executor::callExternalFunction(ExecutionState &state,
                                    KInstruction *target,
                                    Function *function,
                                    std::vector< ref<Expr> > &arguments) {
  // check if specialFunctionHandler wants it
  if (specialFunctionHandler->handle(state, function, target, arguments))
    return;
  
  callUnmodelledFunction(state, target, function, arguments);
}

void Executor::callUnmodelledFunction(ExecutionState &state,
                            KInstruction *target,
                            llvm::Function *function,
                            std::vector<ref<Expr> > &arguments) {

  if (NoExternals && !okExternals.count(function->getName())) {
    std::cerr << "KLEE:ERROR: Calling not-OK external function : " 
               << function->getNameStr() << "\n";
    terminateStateOnError(state, "externals disallowed", "user.err");
    return;
  }

  // normal external function handling path
  // allocate 128 bits for each argument (+return value) to support fp80's;
  // we could iterate through all the arguments first and determine the exact
  // size we need, but this is faster, and the memory usage isn't significant.
  uint64_t *args = (uint64_t*) alloca(2*sizeof(*args) * (arguments.size() + 1));
  memset(args, 0, 2 * sizeof(*args) * (arguments.size() + 1));
  unsigned wordIndex = 2;
  for (std::vector<ref<Expr> >::iterator ai = arguments.begin(), 
      ae = arguments.end(); ai!=ae; ++ai) {
    if (AllowExternalSymCalls) { // don't bother checking uniqueness
      ref<ConstantExpr> ce;
      bool success = solver->getValue(state, *ai, ce);
      assert(success && "FIXME: Unhandled solver failure");
      (void) success;
      ce->toMemory(&args[wordIndex]);
      wordIndex += (ce->getWidth()+63)/64;
    } else {
      ref<Expr> arg = toUnique(state, *ai);
      if (ConstantExpr *ce = dyn_cast<ConstantExpr>(arg)) {
        // XXX kick toMemory functions from here
        ce->toMemory(&args[wordIndex]);
        wordIndex += (ce->getWidth()+63)/64;
      } else {
        terminateStateOnExecError(state, 
                                  "external call with symbolic argument: " + 
                                  function->getName());
        return;
      }
    }
  }

  state.addressSpace().copyOutConcretes(&state.addressPool);

  if (!SuppressExternalWarnings) {
    std::ostringstream os;
    os << "calling external: " << function->getNameStr() << "(";
    for (unsigned i=0; i<arguments.size(); i++) {
      os << arguments[i];
      if (i != arguments.size()-1)
    os << ", ";
    }
    os << ")";
    
    if (AllExternalWarnings)
      klee_warning("%s", os.str().c_str());
    else
      klee_warning_once(function, "%s", os.str().c_str());
  }
  
  bool success = externalDispatcher->executeCall(function, target->inst, args);
  if (!success) {
    terminateStateOnError(state, "failed external call: " + function->getName(),
                          "external.err");
    return;
  }

  if (!state.addressSpace().copyInConcretes(&state.addressPool)) {
    terminateStateOnError(state, "external modified read-only object",
                          "external.err");
    return;
  }

  const Type *resultType = target->inst->getType();
  if (resultType != Type::getVoidTy(getGlobalContext())) {
    ref<Expr> e = ConstantExpr::fromMemory((void*) args, 
                                           getWidthForLLVMType(resultType));
    bindLocal(target, state, e);
  }
}

/***/

ref<Expr> Executor::replaceReadWithSymbolic(ExecutionState &state, 
                                            ref<Expr> e) {
  unsigned n = interpreterOpts.MakeConcreteSymbolic;
  if (!n || replayOut || replayPath)
    return e;

  // right now, we don't replace symbolics (is there any reason too?)
  if (!isa<ConstantExpr>(e))
    return e;

  if (n != 1 && random() %  n)
    return e;

  // create a new fresh location, assert it is equal to concrete value in e
  // and return it.
  
  static unsigned id;
  const Array *array = new Array("rrws_arr" + llvm::utostr(++id), 
                                 Expr::getMinBytesForWidth(e->getWidth()));
  ref<Expr> res = Expr::createTempRead(array, e->getWidth());
  ref<Expr> eq = NotOptimizedExpr::create(EqExpr::create(e, res));
  std::cerr << "Making symbolic: " << eq << "\n";
  state.addConstraint(eq);
  return res;
}

ObjectState *Executor::bindObjectInState(ExecutionState &state, 
                                         const MemoryObject *mo,
                                         bool isLocal,
                                         const Array *array) {
  ObjectState *os = array ? new ObjectState(mo, array) : new ObjectState(mo);
  state.addressSpace().bindObject(mo, os);

  // Its possible that multiple bindings of the same mo in the state
  // will put multiple copies on this list, but it doesn't really
  // matter because all we use this list for is to unbind the object
  // on function return.
  if (isLocal)
    state.stack().back().allocas.push_back(mo);

  return os;
}

void Executor::executeAlloc(ExecutionState &state,
                            ref<Expr> size,
                            bool isLocal,
                            KInstruction *target,
                            bool zeroMemory,
                            const ObjectState *reallocFrom) {
  size = toUnique(state, size);
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(size)) {
    MemoryObject *mo = memory->allocate(&state, CE->getZExtValue(), isLocal, false,
                                        state.prevPC()->inst);
    if (!mo) {
      bindLocal(target, state, 
                ConstantExpr::alloc(0, Context::get().getPointerWidth()));
    } else {
      ObjectState *os = bindObjectInState(state, mo, isLocal);
      if (zeroMemory) {
        os->initializeToZero();
      } else {
        os->initializeToRandom();
      }
      bindLocal(target, state, mo->getBaseExpr());
      
      if (reallocFrom) {
        unsigned count = std::min(reallocFrom->size, os->size);
        for (unsigned i=0; i<count; i++)
          os->write(i, reallocFrom->read8(i));
        updateQceMapOnFree(state, reallocFrom->getObject(), target);
        state.addressSpace().unbindObject(reallocFrom->getObject());
      }
    }
  } else {
    // XXX For now we just pick a size. Ideally we would support
    // symbolic sizes fully but even if we don't it would be better to
    // "smartly" pick a value, for example we could fork and pick the
    // min and max values and perhaps some intermediate (reasonable
    // value).
    // 
    // It would also be nice to recognize the case when size has
    // exactly two values and just fork (but we need to get rid of
    // return argument first). This shows up in pcre when llvm
    // collapses the size expression with a select.

    ref<ConstantExpr> example;
    bool success = solver->getValue(state, size, example);
    assert(success && "FIXME: Unhandled solver failure");
    (void) success;
    
    // Try and start with a small example.
    Expr::Width W = example->getWidth();
    while (example->Ugt(ConstantExpr::alloc(128, W))->isTrue()) {
      ref<ConstantExpr> tmp = example->LShr(ConstantExpr::alloc(1, W));
      bool res;
      bool success = solver->mayBeTrue(state, EqExpr::create(tmp, size), res);
      assert(success && "FIXME: Unhandled solver failure");      
      (void) success;
      if (!res)
        break;
      example = tmp;
    }

    StatePair fixedSize = fork(state, EqExpr::create(example, size), true, KLEE_FORK_INTERNAL);
    
    if (fixedSize.second) { 
      // Check for exactly two values
      ref<ConstantExpr> tmp;
      bool success = solver->getValue(*fixedSize.second, size, tmp);
      assert(success && "FIXME: Unhandled solver failure");      
      (void) success;
      bool res;
      success = solver->mustBeTrue(*fixedSize.second, 
                                   EqExpr::create(tmp, size),
                                   res);
      assert(success && "FIXME: Unhandled solver failure");      
      (void) success;
      if (res) {
        executeAlloc(*fixedSize.second, tmp, isLocal,
                     target, zeroMemory, reallocFrom);
      } else {
        // See if a *really* big value is possible. If so assume
        // malloc will fail for it, so lets fork and return 0.
        StatePair hugeSize = 
          fork(*fixedSize.second, 
               UltExpr::create(ConstantExpr::alloc(1<<31, W), size), 
               true, KLEE_FORK_INTERNAL);
        if (hugeSize.first) {
          klee_message("NOTE: found huge malloc, returing 0");
          bindLocal(target, *hugeSize.first, 
                    ConstantExpr::alloc(0, Context::get().getPointerWidth()));
        }
        
        if (hugeSize.second) {
          std::ostringstream info;
          ExprPPrinter::printOne(info, "  size expr", size);
          info << "  concretization : " << example << "\n";
          info << "  unbound example: " << tmp << "\n";
          terminateStateOnError(*hugeSize.second, 
                                "concretized symbolic size", 
                                "model.err", 
                                info.str());
        }
      }
    }

    if (fixedSize.first) // can be zero when fork fails
      executeAlloc(*fixedSize.first, example, isLocal, 
                   target, zeroMemory, reallocFrom);
  }
}

void Executor::executeFree(ExecutionState &state,
                           ref<Expr> address,
                           KInstruction *target) {
  StatePair zeroPointer = fork(state, Expr::createIsZero(address), true, KLEE_FORK_INTERNAL);
  if (zeroPointer.first) {
    if (target)
      bindLocal(target, *zeroPointer.first, Expr::createPointer(0));
  }
  if (zeroPointer.second) { // address != 0
    ExactResolutionList rl;
    resolveExact(*zeroPointer.second, address, rl, "free");
    
    for (Executor::ExactResolutionList::iterator it = rl.begin(), 
           ie = rl.end(); it != ie; ++it) {
      const MemoryObject *mo = it->first.first;
      if (mo->isLocal) {
        terminateStateOnError(*it->second, 
                              "free of alloca", 
                              "free.err",
                              getAddressInfo(*it->second, address));
      } else if (mo->isGlobal) {
        terminateStateOnError(*it->second, 
                              "free of global", 
                              "free.err",
                              getAddressInfo(*it->second, address));
      } else {
        updateQceMapOnFree(*it->second, mo, target);
        it->second->addressSpace().unbindObject(mo);
        if (target)
          bindLocal(target, *it->second, Expr::createPointer(0));
      }
    }
  }
}

void Executor::executeEvent(ExecutionState &state, unsigned int type,
    long int value) {
  fireEvent(&state, type, value);
}

void Executor::resolveExact(ExecutionState &state,
                            ref<Expr> p,
                            ExactResolutionList &results,
                            const std::string &name) {
  // XXX we may want to be capping this?
  ResolutionList rl;
  state.addressSpace().resolve(state, solver, p, rl);
  
  ExecutionState *unbound = &state;
  for (ResolutionList::iterator it = rl.begin(), ie = rl.end(); 
       it != ie; ++it) {
    ref<Expr> inBounds = EqExpr::create(p, it->first->getBaseExpr());
    
    StatePair branches = fork(*unbound, inBounds, true, KLEE_FORK_INTERNAL);
    
    if (branches.first)
      results.push_back(std::make_pair(*it, branches.first));

    unbound = branches.second;
    if (!unbound) // Fork failure
      break;
  }

  if (unbound) {
    terminateStateOnError(*unbound,
                          "memory error: invalid pointer: " + name,
                          "ptr.err",
                          getAddressInfo(*unbound, p));
  }
}

KFunction* Executor::resolveFunction(ref<Expr> address)
{
  for (std::vector<KFunction*>::iterator fi = kmodule->functions.begin();
	 fi != kmodule->functions.end(); fi++) {
    KFunction* f = (*fi);      
    ref<Expr> addr = Expr::createPointer((uint64_t) (void*) f->function);
    if(addr == address)
      return f;
  }
  return NULL;
}

//pthread handlers
void Executor::executeThreadCreate(ExecutionState &state, thread_id_t tid,
				     ref<Expr> start_function, ref<Expr> arg)
{
  CLOUD9_DEBUG("Creating thread...");
  KFunction *kf = resolveFunction(start_function);
  assert(kf && "cannot resolve thread start function");

  Thread &t = state.createThread(tid, kf);
 
  bindArgumentToPthreadCreate(kf, 0, t.stack.back(), arg);

  if (statsTracker)
    statsTracker->framePushed(&t.stack.back(), 0);
}

void Executor::executeThreadExit(ExecutionState &state) {
  //terminate this thread and schedule another one
  CLOUD9_DEBUG("Exiting thread...");

  if (state.threads.size() == 1) {
    klee_message("terminating state");
    terminateStateOnExit(state);
    return;
  }

  assert(state.threads.size() > 1);

  ExecutionState::threads_ty::iterator thrIt = state.crtThreadIt;
  thrIt->second.enabled = false;

  if (!schedule(state, false))
    return;

  state.terminateThread(thrIt);
}

void Executor::executeProcessExit(ExecutionState &state) {
  if (state.processes.size() == 1) {
    terminateStateOnExit(state);
    return;
  }

  CLOUD9_DEBUG("Terminating " << state.crtProcess().threads.size() << " threads of the current process...");

  ExecutionState::processes_ty::iterator procIt = state.crtProcessIt;

  // Disable all the threads of the current process
  for (std::set<thread_uid_t>::iterator it = procIt->second.threads.begin();
      it != procIt->second.threads.end(); it++) {
    ExecutionState::threads_ty::iterator thrIt = state.threads.find(*it);

    if (thrIt->second.enabled) {
      // Disable any enabled thread
      thrIt->second.enabled = false;
    } else {
      // If the thread is disabled, remove it from any waiting list
      wlist_id_t wlist = thrIt->second.waitingList;

      if (wlist > 0) {
        state.waitingLists[wlist].erase(thrIt->first);
        if (state.waitingLists[wlist].size() == 0)
          state.waitingLists.erase(wlist);

        thrIt->second.waitingList = 0;
      }
    }
  }

  if (!schedule(state, false))
    return;

  state.terminateProcess(procIt);
}

void Executor::executeProcessFork(ExecutionState &state, KInstruction *ki,
    process_id_t pid) {

  CLOUD9_DEBUG("Forking with pid " << pid);

  Thread &pThread = state.crtThread();

  Process &child = state.forkProcess(pid);

  Thread &cThread = state.threads.find(*child.threads.begin())->second;

  // Set return value in the child
  state.scheduleNext(state.threads.find(cThread.tuid));
  bindLocal(ki, state, ConstantExpr::create(0,
      getWidthForLLVMType(ki->inst->getType())));

  // Set return value in the parent
  state.scheduleNext(state.threads.find(pThread.tuid));
  bindLocal(ki, state, ConstantExpr::create(child.pid,
      getWidthForLLVMType(ki->inst->getType())));
}

void Executor::executeFork(ExecutionState &state, KInstruction *ki, int reason) {
  // Check to see if we really should fork
  if (reason == KLEE_FORK_DEFAULT || fireStateBranching(&state, getForkTag(state, reason))) {
    StatePair sp = fork(state, reason);

    // Return 0 in the original
    bindLocal(ki, *sp.first, ConstantExpr::create(0,
        getWidthForLLVMType(ki->inst->getType())));

    // Return 1 otherwise
    bindLocal(ki, *sp.second, ConstantExpr::create(1,
        getWidthForLLVMType(ki->inst->getType())));
  } else {
    bindLocal(ki, state, ConstantExpr::create(0,
        getWidthForLLVMType(ki->inst->getType())));
  }
}


bool Executor::schedule(ExecutionState &state, bool yield) {

  int enabledCount = 0;
  for(ExecutionState::threads_ty::iterator it = state.threads.begin();
      it != state.threads.end();  it++) {
    if(it->second.enabled) {
      enabledCount++;
    }
  }
  
  //CLOUD9_DEBUG("Scheduling " << state.threads.size() << " threads (" <<
  //    enabledCount << " enabled) in " << state.processes.size() << " processes ...");

  if (enabledCount == 0) {
    terminateStateOnError(state, " ******** hang (possible deadlock?)", "user.err");
    return false;
  }
  
  bool forkSchedule = false;
  bool incPreemptions = false;

  ExecutionState::threads_ty::iterator oldIt = state.crtThreadIt;

  if(!state.crtThread().enabled || yield) {
    ExecutionState::threads_ty::iterator it = state.nextThread(state.crtThreadIt);

    while (!it->second.enabled)
      it = state.nextThread(it);

    state.scheduleNext(it);

    if (ForkOnSchedule)
      forkSchedule = true;
  } else {
    if (state.preemptions < MaxPreemptions) {
      forkSchedule = true;
      incPreemptions = true;
    }
  }

  if (DebugCallHistory) {
    CLOUD9_DEBUG("Context Switch: --- TID: " << state.crtThread().tuid.first <<
        " PID: " << state.crtThread().tuid.second << " -----------------------");
    unsigned int depth = state.stack().size() - 1;
    CLOUD9_DEBUG("Call: " << (std::string(" ") * depth) << state.stack().back().kf->function->getNameStr());
  }

  if (forkSchedule) {
    ExecutionState::threads_ty::iterator finalIt = state.crtThreadIt;
    ExecutionState::threads_ty::iterator it = state.nextThread(finalIt);
    ExecutionState *lastState = &state;

    ForkClass forkClass = KLEE_FORK_SCHEDULE;

    while (it != finalIt) {
      // Choose only enabled states, and, in the case of yielding, do not
      // reschedule the same thread
      if (it->second.enabled && (!yield || it != oldIt)) {
        StatePair sp = fork(*lastState, forkClass);

        if (incPreemptions)
          sp.first->preemptions = state.preemptions + 1;

        sp.first->scheduleNext(sp.first->threads.find(it->second.tuid));

        lastState = sp.first;

        if (forkClass == KLEE_FORK_SCHEDULE) {
          forkClass = KLEE_FORK_MULTI;   // Avoid appearing like multiple schedules
        }
      }

      it = state.nextThread(it);
    }
  }

  return true;
}

void Executor::executeThreadNotifyOne(ExecutionState &state, wlist_id_t wlist) {
  // Copy the waiting list
  std::set<thread_uid_t> wl = state.waitingLists[wlist];

  if (!ForkOnSchedule || wl.size() <= 1) {
    if (wl.size() == 0)
      state.waitingLists.erase(wlist);
    else
      state.notifyOne(wlist, *wl.begin()); // Deterministically pick the first thread in the queue

    return;
  }

  ExecutionState *lastState = &state;

  for (std::set<thread_uid_t>::iterator it = wl.begin(); it != wl.end();) {
    thread_uid_t tuid = *it++;

    if (it != wl.end()) {
      StatePair sp = fork(*lastState, KLEE_FORK_SCHEDULE);

      sp.second->notifyOne(wlist, tuid);

      lastState = sp.first;
    } else {
      lastState->notifyOne(wlist, tuid);
    }
  }
}


void Executor::executeMemoryOperation(ExecutionState &state,
                                      bool isWrite,
                                      ref<Expr> address,
                                      ref<Expr> value /* undef if read */,
                                      KInstruction *target /* undef if write */) {
  Expr::Width type = (isWrite ? value->getWidth() : 
                     getWidthForLLVMType(target->inst->getType()));
  unsigned bytes = Expr::getMinBytesForWidth(type);

  if (SimplifySymIndices) {
    if (!isa<ConstantExpr>(address))
      address = state.constraints().simplifyExpr(address);
    if (isWrite && !isa<ConstantExpr>(value))
      value = state.constraints().simplifyExpr(value);
  }

  // fast path: single in-bounds resolution
  ObjectPair op;
  bool success;
  solver->setTimeout(stpTimeout);
  if (!state.addressSpace().resolveOne(state, solver, address, op, success)) {
    address = toConstant(state, address, "resolveOne failure");
    success = state.addressSpace().resolveOne(cast<ConstantExpr>(address), op);
  }
  solver->setTimeout(0);

  if (success) {
    const MemoryObject *mo = op.first;
    state.lastResolveResult = mo;

    if (MaxSymArraySize && mo->size>=MaxSymArraySize) {
      address = toConstant(state, address, "max-sym-array-size");
    }
    
    ref<Expr> offset = mo->getOffsetExpr(address);

    bool inBounds;
    solver->setTimeout(stpTimeout);
    bool success = solver->mustBeTrue(state, 
                                      mo->getBoundsCheckOffset(offset, bytes),
                                      inBounds);
    solver->setTimeout(0);
    if (!success) {
      state.setPC(state.prevPC());
      terminateStateEarly(state, "query timed out");
      return;
    }

    if (inBounds) {
      const ObjectState *os = op.second;
      if (isWrite) {
        if (os->readOnly) {
          terminateStateOnError(state,
                                "memory error: object read only",
                                "readonly.err");
        } else {
          ObjectState *wos = state.addressSpace().getWriteable(mo, os);
          /*
          if (!isa<ConstantExpr>(offset)) {
            state.addressSpace().removeMergeBlacklistItemHash(mo, wos);
          } else {
            state.addressSpace().removeMergeBlacklistItemHash(mo, wos,
                                                         offset, value->getWidth()/8);
          }
          */
          verifyQceMap(state);
          updateQceMemoryValue(state, mo, wos, offset, value, target);
          wos->write(offset, value);
          verifyQceMap(state);
          /*
          state.addressSpace().addMergeBlacklistItemHash(mo, wos,
                                                         offset, value->getWidth()/8);
          */

        }
      } else {
        ref<Expr> result = os->read(offset, type);

        if (interpreterOpts.MakeConcreteSymbolic)
          result = replaceReadWithSymbolic(state, result);
        
        bindLocal(target, state, result);
      }

      return;
    }
  } 

  // we are on an error path (no resolution, multiple resolution, one
  // resolution with out of bounds)
  
  ResolutionList rl;  
  solver->setTimeout(stpTimeout);
  bool incomplete = state.addressSpace().resolve(state, solver, address, rl,
                                               0, stpTimeout);
  solver->setTimeout(0);
  
  // XXX there is some query wasteage here. who cares?
  ExecutionState *unbound = &state;
  
  for (ResolutionList::iterator i = rl.begin(), ie = rl.end(); i != ie; ++i) {
    const MemoryObject *mo = i->first;
    const ObjectState *os = i->second;
    ref<Expr> inBounds = mo->getBoundsCheckPointer(address, bytes);
    
    StatePair branches = fork(*unbound, inBounds, true, KLEE_FORK_RESOLVE);
    ExecutionState *bound = branches.first;

    // bound can be 0 on failure or overlapped 
    if (bound) {
      bound->lastResolveResult = mo;
      if (isWrite) {
        if (os->readOnly) {
          terminateStateOnError(*bound,
                                "memory error: object read only",
                                "readonly.err");
        } else {
          ObjectState *wos = bound->addressSpace().getWriteable(mo, os);
          /*
          if (!isa<ConstantExpr>(mo->getOffsetExpr(address))) {
            state.addressSpace().removeMergeBlacklistItemHash(mo, wos);
          } else {
            state.addressSpace().removeMergeBlacklistItemHash(mo, wos,
                                                         mo->getOffsetExpr(address),
                                                         value->getWidth()/8);
          }
          */
          ref<Expr> offset = mo->getOffsetExpr(address);
          verifyQceMap(state);
          updateQceMemoryValue(state, mo, wos, offset, value, target);
          wos->write(mo->getOffsetExpr(address), value);
          verifyQceMap(state);
          /*
          state.addressSpace().addMergeBlacklistItemHash(mo, wos,
                                                         mo->getOffsetExpr(address),
                                                         value->getWidth()/8);
          */
        }
      } else {
        ref<Expr> result = os->read(mo->getOffsetExpr(address), type);
        bindLocal(target, *bound, result);
      }
    }

    unbound = branches.second;
    if (!unbound)
      break;
  }
  
  // XXX should we distinguish out of bounds and overlapped cases?
  if (unbound) {
    if (incomplete) {
      terminateStateEarly(*unbound, "query timed out (resolve)");
    } else {
      terminateStateOnError(*unbound,
                            "memory error: out of bound pointer",
                            "ptr.err",
                            getAddressInfo(*unbound, address));
    }
  }
}

void Executor::executeMakeSymbolic(ExecutionState &state, 
                                   const MemoryObject *mo, bool shared) {
  // Create a new object state for the memory object (instead of a copy).
  if (!replayOut) {
    if (OutputConstraints)
      assert(states.size() == 1 && "Can't add new symbolics after fork!\n");

    static unsigned id = 0;
    std::string name = "arr" + llvm::utostr(++id) + "_";
    for (unsigned i = 0; i < mo->name.size(); ++i)
      name += (isalnum(mo->name[i]) ? mo->name[i] : '_');

    const Array *array = new Array(name, mo->size);
    ObjectState *os = bindObjectInState(state, mo, false, array);
    os->isShared = shared;

    state.addSymbolic(mo, array);
    
    std::map< ExecutionState*, std::vector<SeedInfo> >::iterator it = 
      seedMap.find(&state);
    if (it!=seedMap.end()) { // In seed mode we need to add this as a
                             // binding.
      for (std::vector<SeedInfo>::iterator siit = it->second.begin(), 
             siie = it->second.end(); siit != siie; ++siit) {
        SeedInfo &si = *siit;
        KTestObject *obj = si.getNextInput(mo, NamedSeedMatching);

        if (!obj) {
          if (ZeroSeedExtension) {
            std::vector<unsigned char> &values = si.assignment.bindings[array];
            values = std::vector<unsigned char>(mo->size, '\0');
          } else if (!AllowSeedExtension) {
            terminateStateOnError(state, 
                                  "ran out of inputs during seeding",
                                  "user.err");
            break;
          }
        } else {
          if (obj->numBytes != mo->size &&
              ((!(AllowSeedExtension || ZeroSeedExtension)
                && obj->numBytes < mo->size) ||
               (!AllowSeedTruncation && obj->numBytes > mo->size))) {
	    std::stringstream msg;
	    msg << "replace size mismatch: "
		<< mo->name << "[" << mo->size << "]"
		<< " vs " << obj->name << "[" << obj->numBytes << "]"
		<< " in test\n";

            terminateStateOnError(state,
                                  msg.str(),
                                  "user.err");
            break;
          } else {
            std::vector<unsigned char> &values = si.assignment.bindings[array];
            values.insert(values.begin(), obj->bytes, 
                          obj->bytes + std::min(obj->numBytes, mo->size));
            if (ZeroSeedExtension) {
              for (unsigned i=obj->numBytes; i<mo->size; ++i)
                values.push_back('\0');
            }
          }
        }
      }
    }
  } else {
    ObjectState *os = bindObjectInState(state, mo, false);
    if (replayPosition >= replayOut->numObjects) {
      terminateStateOnError(state, "replay count mismatch", "user.err");
    } else {
      KTestObject *obj = &replayOut->objects[replayPosition++];
      if (obj->numBytes != mo->size) {
        terminateStateOnError(state, "replay size mismatch", "user.err");
      } else {
        for (unsigned i=0; i<mo->size; i++)
          os->write8(i, obj->bytes[i]);
      }
    }
  }
}

/***/

ExecutionState *Executor::createRootState(llvm::Function *f) {
	ExecutionState *state = new ExecutionState(this, kmodule->functionMap[f]);

	return state;
}

void Executor::initRootState(ExecutionState *state,
		int argc, char **argv, char **envp) {
	llvm::Function *f = state->stack().back().kf->function;

	std::vector<ref<Expr> > arguments;

	// force deterministic initialization of memory objects
	srand(1);
	srandom(1);

	MemoryObject *argvMO = 0;

	// In order to make uclibc happy and be closer to what the system is
	// doing we lay out the environments at the end of the argv array
	// (both are terminated by a null). There is also a final terminating
	// null that uclibc seems to expect, possibly the ELF header?

	int envc;
	for (envc = 0; envp[envc]; ++envc)
		;

	unsigned NumPtrBytes = Context::get().getPointerWidth() / 8;
	KFunction *kf = kmodule->functionMap[f];
	assert(kf);
	Function::arg_iterator ai = f->arg_begin(), ae = f->arg_end();
	if (ai != ae) {
		arguments.push_back(ConstantExpr::alloc(argc, Expr::Int32));

		if (++ai != ae) {
			argvMO = memory->allocate(state, (argc + 1 + envc + 1 + 1) * NumPtrBytes,
					false, true, f->begin()->begin());

			arguments.push_back(argvMO->getBaseExpr());

			if (++ai != ae) {
				uint64_t envp_start = argvMO->address + (argc + 1)
						* NumPtrBytes;
				arguments.push_back(Expr::createPointer(envp_start));

				if (++ai != ae)
					klee_error("invalid main function (expect 0-3 arguments)");
			}
		}
	}

	if (pathWriter)
		state->pathOS = pathWriter->open();
	if (symPathWriter)
		state->symPathOS = symPathWriter->open();

	if (statsTracker)
		statsTracker->framePushed(*state, 0);

	assert(arguments.size() == f->arg_size() && "wrong number of arguments");
	for (unsigned i = 0, e = f->arg_size(); i != e; ++i)
		bindArgument(kf, i, *state, arguments[i]);

	if (argvMO) {
		ObjectState *argvOS = bindObjectInState(*state, argvMO, false);

		for (int i = 0; i < argc + 1 + envc + 1 + 1; i++) {
			MemoryObject *arg;

			if (i == argc || i >= argc + 1 + envc) {
				arg = 0;
			} else {
				char *s = i < argc ? argv[i] : envp[i - (argc + 1)];
				int j, len = strlen(s);

				arg = memory->allocate(state, len + 1, false, true, state->pc()->inst);
				ObjectState *os = bindObjectInState(*state, arg, false);
				for (j = 0; j < len + 1; j++)
					os->write8(j, s[j]);
			}

			if (arg) {
				argvOS->write(i * NumPtrBytes, arg->getBaseExpr());
			} else {
				argvOS->write(i * NumPtrBytes, Expr::createPointer(0));
			}
		}
	}

	initializeGlobals(*state);

	processTree = new PTree(state);
	state->ptreeNode = processTree->root;

	bindModuleConstants();

	// Delay init till now so that ticks don't accrue during
	// optimization and such.
	initTimers();

	states.insert(state);
}

Searcher *Executor::initSearcher(Searcher *base) {
	return constructUserSearcher(*this, base);
}

void Executor::destroyStates() {
	if (DumpStatesOnHalt && !states.empty()) {
		std::cerr << "KLEE: halting execution, dumping remaining states\n";
		for (std::set<ExecutionState*>::iterator it = states.begin(), ie =
				states.end(); it != ie; ++it) {
			ExecutionState &state = **it;
			stepInstruction(state, true); // keep stats rolling
			terminateStateEarly(state, "execution halting");
		}
		updateStates(0);
	}

	delete processTree;
	processTree = 0;

	// hack to clear memory objects
	delete memory;
	memory = new MemoryManager();

	globalObjects.clear();
	globalAddresses.clear();

	if (statsTracker)
		statsTracker->done();

	if (theMMap) {
		munmap(theMMap, theMMapSize);
		theMMap = 0;
	}
}

void Executor::destroyState(ExecutionState *state) {
	terminateState(*state, true);
}

void Executor::runFunctionAsMain(Function *f, int argc, char **argv,
		char **envp) {

	ExecutionState *state = createRootState(f);
        initRootState(state, argc, argv, envp);
        updateQceMapOnExec(*state);

	run(*state);

	cloud9::instrum::theInstrManager.recordEvent(cloud9::instrum::TimeOut, "Timeout");

	destroyStates();

}

unsigned Executor::getPathStreamID(const ExecutionState &state) {
  assert(pathWriter);
  return state.pathOS.getID();
}

unsigned Executor::getSymbolicPathStreamID(const ExecutionState &state) {
  assert(symPathWriter);
  return state.symPathOS.getID();
}

void Executor::getConstraintLog(const ExecutionState &state,
                                std::string &res,
                                bool asCVC) {
  if (asCVC) {
    Query query(state.constraints(), ConstantExpr::alloc(0, Expr::Bool));
    char *log = solver->stpSolver->getConstraintLog(query);
    res = std::string(log);
    free(log);
  } else {
    std::ostringstream info;
    ExprPPrinter::printConstraints(info, state.constraints());
    res = info.str();    
  }
}

bool Executor::getSymbolicSolution(const ExecutionState &state,
                                   std::vector< 
                                   std::pair<std::string,
                                   std::vector<unsigned char> > >
                                   &res) {
  solver->setTimeout(stpTimeout);

  ExecutionState tmp(state);
  if (!NoPreferCex) {
    for (unsigned i = 0; i != state.symbolics.size(); ++i) {
      const MemoryObject *mo = state.symbolics[i].first;
      std::vector< ref<Expr> >::const_iterator pi =
        mo->cexPreferences.begin(), pie = mo->cexPreferences.end();
      for (; pi != pie; ++pi) {
        bool mustBeTrue;
        bool success = solver->mustBeTrue(tmp, Expr::createIsZero(*pi), 
                                          mustBeTrue);
        if (!success) break;
        if (!mustBeTrue) tmp.addConstraint(*pi);
      }
      if (pi!=pie) break;
    }
  }

  std::vector< std::vector<unsigned char> > values;
  std::vector<const Array*> objects;
  for (unsigned i = 0; i != state.symbolics.size(); ++i)
    objects.push_back(state.symbolics[i].second);
  bool success = solver->getInitialValues(tmp, objects, values);
  solver->setTimeout(0);
  if (!success) {
    klee_warning("unable to compute initial values (invalid constraints?)!");
    ExprPPrinter::printQuery(std::cerr,
                             state.constraints(),
                             ConstantExpr::alloc(0, Expr::Bool));
    return false;
  }
  
  for (unsigned i = 0; i != state.symbolics.size(); ++i)
    res.push_back(std::make_pair(state.symbolics[i].first->name, values[i]));
  return true;
}

void Executor::getCoveredLines(const ExecutionState &state,
                               std::map<const std::string*, std::set<unsigned> > &res) {
  res = state.coveredLines;
}

void Executor::doImpliedValueConcretization(ExecutionState &state,
                                            ref<Expr> e,
                                            ref<ConstantExpr> value) {
  abort(); // FIXME: Broken until we sort out how to do the write back.

  if (DebugCheckForImpliedValues)
    ImpliedValue::checkForImpliedValues(solver->solver, e, value);

  ImpliedValueList results;
  ImpliedValue::getImpliedValues(e, value, results);
  for (ImpliedValueList::iterator it = results.begin(), ie = results.end();
       it != ie; ++it) {
    ReadExpr *re = it->first.get();
    
    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(re->index)) {
      // FIXME: This is the sole remaining usage of the Array object
      // variable. Kill me.
      const MemoryObject *mo = 0; //re->updates.root->object;
      const ObjectState *os = state.addressSpace().findObject(mo);

      if (!os) {
        // object has been free'd, no need to concretize (although as
        // in other cases we would like to concretize the outstanding
        // reads, but we have no facility for that yet)
      } else {
        assert(!os->readOnly && 
               "not possible? read only object with static read?");
        ObjectState *wos = state.addressSpace().getWriteable(mo, os);
        wos->write(CE, it->second);
      }
    }
  }
}

Expr::Width Executor::getWidthForLLVMType(const llvm::Type *type) const {
  return kmodule->targetData->getTypeSizeInBits(type);
}

void Executor::dumpProcessTree()
{
  char name[32];
  sprintf(name, "ptree%08d.dot", (int) stats::instructions);
  std::ostream *os = interpreterHandler->openOutputFile(name);
  if (os) {
    processTree->dump(*os);
    delete os;
  }
}

///

Interpreter *Interpreter::create(const InterpreterOptions &opts,
                                 InterpreterHandler *ih) {
  return new Executor(opts, ih);
}

//}

