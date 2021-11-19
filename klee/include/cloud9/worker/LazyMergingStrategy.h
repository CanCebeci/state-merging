/*
 * LazyMergingStrategy.h
 *
 *  Created on: Mar 8, 2011
 *      Author: stefan
 */

#ifndef LAZYMERGINGSTRATEGY_H_
#define LAZYMERGINGSTRATEGY_H_

#include "cloud9/worker/CoreStrategies.h"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/DenseMap.h>

#include <set>
#include <map>

namespace cloud9 {

namespace worker {

class JobManager;

class LazyMergingStrategy: public StateSelectionStrategy {
private:
  JobManager *jobManager;
  StateSelectionStrategy *strategy;

  typedef llvm::SmallPtrSet<SymbolicState*, 8> StatesSet;
  typedef llvm::DenseMap<uint32_t, StatesSet*> StatesTrace;

  StatesTrace statesTrace;
  StatesSet statesToForward;

  bool canFastForwardState(const SymbolicState* state) const;
public:
  LazyMergingStrategy(JobManager *_jobManager, StateSelectionStrategy *_strategy)
    : jobManager(_jobManager), strategy(_strategy) {

  }

  virtual ~LazyMergingStrategy() { }

  virtual void onStateActivated(SymbolicState *state);
  virtual void onStateUpdated(SymbolicState *state, WorkerTree::Node *oldNode);
  virtual void onStateDeactivated(SymbolicState *state);
  virtual void onStateStepped(SymbolicState *state);

  virtual SymbolicState* onNextStateSelection();
  virtual SymbolicState* onNextStateSelectionEx(bool &canBatch, uint32_t &batchDest);
};

class MergingDecorator: public WorkerNodeDecorator {
  WorkerTree::Node *dest, *src;
public:
  MergingDecorator(WorkerTree::Node *_dest, WorkerTree::Node *_src) :
    WorkerNodeDecorator(NULL), dest(_dest), src(_src) {

  }

  void operator() (WorkerTree::Node *node, deco_t &deco, edge_deco_t &inEdges) {
    WorkerNodeDecorator::operator() (node, deco, inEdges);

    if (node == dest) {
      deco["fillcolor"] = "red";
    }
    if (node == src) {
      deco["fillcolor"] = "green";
    }
  }
};

}

}

#endif /* LAZYMERGINGSTRATEGY_H_ */
