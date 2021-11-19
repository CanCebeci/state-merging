//===-- Searcher.h ----------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_SEARCHER_H
#define KLEE_SEARCHER_H

#include <vector>
#include <set>
#include <map>
#include <queue>
#include <functional>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/DenseMap.h>

#include <inttypes.h>

// FIXME: Move out of header, use llvm streams.
#include <ostream>

// FIXME: We do not want to be exposing these? :(
#include "klee/Internal/Module/KInstIterator.h"
#include "klee/Threading.h"

namespace llvm {
  class BasicBlock;
  class Function;
  class Instruction;
}

namespace klee {
  template<class T> class DiscretePDF;
  class ExecutionState;
  class Executor;

  class Searcher {
  public:
    virtual ~Searcher();

    virtual ExecutionState &selectState() = 0;

    virtual void update(ExecutionState *current,
                        const std::set<ExecutionState*> &addedStates,
                        const std::set<ExecutionState*> &removedStates) = 0;

    virtual bool empty() = 0;

    // prints name of searcher as a klee_message()
    // TODO: could probably make prettier or more flexible
    virtual void printName(std::ostream &os) { 
      os << "<unnamed searcher>\n";
    }

    // pgbovine - to be called when a searcher gets activated and
    // deactivated, say, by a higher-level searcher; most searchers
    // don't need this functionality, so don't have to override.
    virtual void activate() {}
    virtual void deactivate() {}

    // utility functions

    void addState(ExecutionState *es, ExecutionState *current = 0) {
      std::set<ExecutionState*> tmp;
      tmp.insert(es);
      update(current, tmp, std::set<ExecutionState*>());
    }

    void removeState(ExecutionState *es, ExecutionState *current = 0) {
      std::set<ExecutionState*> tmp;
      tmp.insert(es);
      update(current, std::set<ExecutionState*>(), tmp);
    }
  };

  class DFSSearcher : public Searcher {
    std::vector<ExecutionState*> states;

  public:
    ExecutionState &selectState();
    void update(ExecutionState *current,
                const std::set<ExecutionState*> &addedStates,
                const std::set<ExecutionState*> &removedStates);
    bool empty() { return states.empty(); }
    void printName(std::ostream &os) {
      os << "DFSSearcher\n";
    }
  };

  class RandomSearcher : public Searcher {
    std::vector<ExecutionState*> states;

  public:
    ExecutionState &selectState();
    void update(ExecutionState *current,
                const std::set<ExecutionState*> &addedStates,
                const std::set<ExecutionState*> &removedStates);
    bool empty() { return states.empty(); }
    void printName(std::ostream &os) {
      os << "RandomSearcher\n";
    }
  };

  class WeightedRandomSearcher : public Searcher {
  public:
    enum WeightType {
      Depth,
      QueryCost,
      InstCount,
      CPInstCount,
      MinDistToUncovered,
      CoveringNew
    };

  private:
    Executor &executor;
    DiscretePDF<ExecutionState*> *states;
    WeightType type;
    bool updateWeights;
    
    double getWeight(ExecutionState*);

  public:
    WeightedRandomSearcher(Executor &executor, WeightType type);
    ~WeightedRandomSearcher();

    ExecutionState &selectState();
    void update(ExecutionState *current,
                const std::set<ExecutionState*> &addedStates,
                const std::set<ExecutionState*> &removedStates);
    bool empty();
    void printName(std::ostream &os) {
      os << "WeightedRandomSearcher::";
      switch(type) {
      case Depth              : os << "Depth\n"; return;
      case QueryCost          : os << "QueryCost\n"; return;
      case InstCount          : os << "InstCount\n"; return;
      case CPInstCount        : os << "CPInstCount\n"; return;
      case MinDistToUncovered : os << "MinDistToUncovered\n"; return;
      case CoveringNew        : os << "CoveringNew\n"; return;
      default                 : os << "<unknown type>\n"; return;
      }
    }
  };

  class RandomPathSearcher : public Searcher {
    Executor &executor;

  public:
    RandomPathSearcher(Executor &_executor);
    ~RandomPathSearcher();

    ExecutionState &selectState();
    void update(ExecutionState *current,
                const std::set<ExecutionState*> &addedStates,
                const std::set<ExecutionState*> &removedStates);
    bool empty();
    void printName(std::ostream &os) {
      os << "RandomPathSearcher\n";
    }
  };

  class MergingSearcher : public Searcher {
    Executor &executor;
    std::set<ExecutionState*> statesAtMerge;
    Searcher *baseSearcher;
    llvm::Function *mergeFunction;

  private:
    llvm::Instruction *getMergePoint(ExecutionState &es);

  public:
    MergingSearcher(Executor &executor, Searcher *baseSearcher);
    ~MergingSearcher();

    ExecutionState &selectState();
    void update(ExecutionState *current,
                const std::set<ExecutionState*> &addedStates,
                const std::set<ExecutionState*> &removedStates);
    bool empty() { return baseSearcher->empty() && statesAtMerge.empty(); }
    void printName(std::ostream &os) {
      os << "MergingSearcher\n";
    }
  };

  class BumpMergingSearcher : public Searcher {
    Executor &executor;
    std::map<llvm::Instruction*, ExecutionState*> statesAtMerge;
    Searcher *baseSearcher;
    llvm::Function *mergeFunction;

  private:
    llvm::Instruction *getMergePoint(ExecutionState &es);

  public:
    BumpMergingSearcher(Executor &executor, Searcher *baseSearcher);
    ~BumpMergingSearcher();

    ExecutionState &selectState();
    void update(ExecutionState *current,
                const std::set<ExecutionState*> &addedStates,
                const std::set<ExecutionState*> &removedStates);
    bool empty() { return baseSearcher->empty() && statesAtMerge.empty(); }
    void printName(std::ostream &os) {
      os << "BumpMergingSearcher\n";
    }
  };

  struct TopoIndexLess : public std::binary_function<TopoIndex, TopoIndex, bool> {
    bool operator() (const TopoIndex &a, const TopoIndex &b);
  };

  class StaticMergingSearcher: public Searcher {
  private:
    typedef std::set<ExecutionState*> SmallStatesSet;
    typedef std::set<ExecutionState*> StatesSet;

    typedef std::map<TopoIndex, SmallStatesSet, TopoIndexLess> TopoBucketsMap;
    typedef StatesSet FreeStatesSet;
    typedef std::map<uint64_t, SmallStatesSet > MergeIndexGroupsMap;

    TopoBucketsMap topoBuckets;
    FreeStatesSet freeStates;
    SmallStatesSet terminatedStates;

    Executor &executor;
    llvm::Function *rendezVousFunction;

    bool isAtRendezVous(ExecutionState *state);
  public:
    StaticMergingSearcher(Executor &executor);
    ~StaticMergingSearcher();

    ExecutionState &selectState();
    void update(ExecutionState *current,
		const std::set<ExecutionState*> &addedStates,
		const std::set<ExecutionState*> &removedStates);
    bool empty() { return topoBuckets.empty() && freeStates.empty(); }

    void printName(std::ostream &os) {
      os << "StaticMergingSearcher\n";
    }
  };

  class LazyMergingSearcher : public Searcher {
    Executor &executor;
    Searcher *baseSearcher;

    // TODO: use unordered multimap instead
    typedef llvm::SmallPtrSet<ExecutionState*, 8> StatesSet;
    typedef llvm::DenseMap<uint64_t, StatesSet*> StatesTrace;

    //typedef llvm::DenseSet<uint64_t> StateIndexes;
    struct StateIndexes;
    typedef llvm::DenseMap<ExecutionState*, StateIndexes*> StatesIndexesMap;

    StatesTrace statesTrace;
    StatesSet statesToForward;

    StatesIndexesMap statesIndexesMap;
    unsigned stateIndexesSize;

    void addItemToTraces(ExecutionState* state, uint64_t mergeIndex);
    void removeStateFromTraces(ExecutionState* state);
    void mergeStateTraces(ExecutionState* merged, ExecutionState* other);

    bool canFastForwardState(const ExecutionState* state) const;

    void verifyMaps();

  public:
    LazyMergingSearcher(Executor &executor, Searcher *baseSearcher);
    ~LazyMergingSearcher();

    ExecutionState &selectState();
    void update(ExecutionState *current,
                const std::set<ExecutionState*> &addedStates,
                const std::set<ExecutionState*> &removedStates);
    bool empty() { return baseSearcher->empty(); }
    void printName(std::ostream &os) {
      os << "LazyMergingSearcher\n";
    }
  };

  class BatchingSearcher : public Searcher {
    Searcher *baseSearcher;
    double timeBudget;
    unsigned instructionBudget;

    ExecutionState *lastState;
    double lastStartTime;
    unsigned lastStartInstructions;

  public:
    BatchingSearcher(Searcher *baseSearcher, 
                     double _timeBudget,
                     unsigned _instructionBudget);
    ~BatchingSearcher();

    ExecutionState &selectState();
    void update(ExecutionState *current,
                const std::set<ExecutionState*> &addedStates,
                const std::set<ExecutionState*> &removedStates);
    bool empty() { return baseSearcher->empty(); }
    void printName(std::ostream &os) {
      os << "<BatchingSearcher> timeBudget: " << timeBudget
         << ", instructionBudget: " << instructionBudget
         << ", baseSearcher:\n";
      baseSearcher->printName(os);
      os << "</BatchingSearcher>\n";
    }
  };

  class CheckpointSearcher: public Searcher {
    typedef llvm::SmallPtrSet<ExecutionState*, 4> StatesSet;
  private:
    Searcher *baseSearcher;

    ExecutionState *activeState;
    StatesSet addedUnchecked;
    StatesSet addedChecked;

    unsigned long aggregateCount;
    unsigned long totalUpdatesRecv;
    unsigned long totalUpdatesSent;
    bool isCheckpoint(ExecutionState *state);

    llvm::DenseMap<llvm::BasicBlock*, llvm::Instruction*> m_firstInstMap;

  public:
    CheckpointSearcher(Searcher *baseSearcher);
    virtual ~CheckpointSearcher();

    ExecutionState &selectState();
    void update(ExecutionState *current,
        const std::set<ExecutionState*> &addedStates,
        const std::set<ExecutionState*> &removedStates);

    bool empty();

    void printName(std::ostream &os) {
      os << "<CheckpointSearcher>\n";
      baseSearcher->printName(os);
      os << "</CheckpointSearcher>\n";
    }
  };

  class IterativeDeepeningTimeSearcher : public Searcher {
    Searcher *baseSearcher;
    double time, startTime;
    std::set<ExecutionState*> pausedStates;

  public:
    IterativeDeepeningTimeSearcher(Searcher *baseSearcher);
    ~IterativeDeepeningTimeSearcher();

    ExecutionState &selectState();
    void update(ExecutionState *current,
                const std::set<ExecutionState*> &addedStates,
                const std::set<ExecutionState*> &removedStates);
    bool empty() { return baseSearcher->empty() && pausedStates.empty(); }
    void printName(std::ostream &os) {
      os << "IterativeDeepeningTimeSearcher\n";
    }
  };

  class InterleavedSearcher : public Searcher {
    typedef std::vector<Searcher*> searchers_ty;

    searchers_ty searchers;
    unsigned index;

  public:
    explicit InterleavedSearcher(const searchers_ty &_searchers);
    ~InterleavedSearcher();

    ExecutionState &selectState();
    void update(ExecutionState *current,
                const std::set<ExecutionState*> &addedStates,
                const std::set<ExecutionState*> &removedStates);
    bool empty() { return searchers[0]->empty(); }
    void printName(std::ostream &os) {
      os << "<InterleavedSearcher> containing "
         << searchers.size() << " searchers:\n";
      for (searchers_ty::iterator it = searchers.begin(), ie = searchers.end();
           it != ie; ++it)
        (*it)->printName(os);
      os << "</InterleavedSearcher>\n";
    }
  };

  class ForkCapSearcher : public Searcher {
    Executor &executor;
    Searcher *baseSearcher;

#if 0
    typedef llvm::SmallPtrSet<ExecutionState*, 8> StatesSet;
#else
    typedef std::set<ExecutionState*> StatesSet;
#endif

    struct StatesAtFork {
      unsigned long totalForks;
      StatesSet active;
      StatesSet paused;

      StatesAtFork(): totalForks(0) {}
    };

#if 0
    typedef llvm::DenseMap<KInstruction*, StatesAtFork*> ForkMap;
    typedef llvm::DenseMap<ExecutionState*, StatesAtFork*> StatesMap;
#else
    typedef std::map<KInstruction*, StatesAtFork*> ForkMap;
    typedef std::map<ExecutionState*, StatesAtFork*> StatesMap;
#endif

    ForkMap forkMap;
    StatesMap statesMap;
    StatesSet disabledStates;

    unsigned long forkCap;
    unsigned long hardForkCap;

  public:
    ForkCapSearcher(Executor &executor,
                    Searcher *baseSearcher,
                    unsigned long forkCap,
                    unsigned long hardForkCap);
    ~ForkCapSearcher();

    ExecutionState &selectState();
    void update(ExecutionState *current,
                const std::set<ExecutionState*> &addedStates,
                const std::set<ExecutionState*> &removedStates);
    bool empty() { return baseSearcher->empty(); }
    void printName(std::ostream &os) {
      os << "FrokCapSearcher\n";
    }
  };

}

#endif
