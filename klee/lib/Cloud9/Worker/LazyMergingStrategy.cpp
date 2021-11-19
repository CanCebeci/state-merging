/*
 * LazyMergingStrategy.cpp
 *
 *  Created on: Mar 8, 2011
 *      Author: stefan
 */

#include "cloud9/worker/LazyMergingStrategy.h"
#include "cloud9/worker/TreeObjects.h"
#include "cloud9/worker/JobManager.h"

#include "llvm/Support/CommandLine.h"

using namespace klee;
using namespace llvm;

namespace {
  cl::opt<unsigned>
  MaxStateMultiplicity("max-state-multiplicity",
            cl::desc("Maximum number of states merged into one"),
            cl::init(0));
}

namespace cloud9 {

namespace worker {

bool LazyMergingStrategy::canFastForwardState(const SymbolicState* state) const {
  if (MaxStateMultiplicity && (**state).multiplicity >= MaxStateMultiplicity)
      return false;

  StatesTrace::const_iterator it = statesTrace.find((**state).getMergeIndex());

  if (it == statesTrace.end())
    return false;

  // This loop could have at most two iterations
  for (StatesSet::const_iterator it1 = it->second->begin(),
                      ie1 = it->second->end(); it1 != ie1; ++it1) {
    if (*it1 != state)
      return true;
  }

  return false;
}

void LazyMergingStrategy::onStateActivated(SymbolicState *state) {
  // TODO: we could add every newly created state to fast-forward track,
  // that would be more aggressive. This can be done be removing the following
  // 'if' condition. Worth trying and evaluating.
  if (!statesToForward.empty()) {
    // States created during fast-forward are also candidates for fast-forward
    // We would like to check it as soon as possible
    if (canFastForwardState(state)) {
      statesToForward.insert(state);
      //stats::fastForwardsStart += 1;
    }
  }
  strategy->onStateActivated(state);
}

void LazyMergingStrategy::onStateUpdated(SymbolicState *state, WorkerTree::Node *oldNode) {
  strategy->onStateUpdated(state, oldNode);
}

void LazyMergingStrategy::onStateDeactivated(SymbolicState *state) {
  statesToForward.erase(state);

  // Terminated states are useless for merge, remove them from traces
  for (StatesTrace::iterator it1 = statesTrace.begin(),
                             ie1 = statesTrace.end(); it1 != ie1;) {
    it1->second->erase(state);
    if (it1->second->empty()) {
      delete it1->second;
      statesTrace.erase(it1++);
    } else {
      ++it1;
    }
  }
  strategy->onStateDeactivated(state);
}

void LazyMergingStrategy::onStateStepped(SymbolicState *state) {
  // At this point, the pc of current state corresponds to the instruction
  // that is not yet executed. It will be executed when the state is selected.
  if (!MaxStateMultiplicity || (**state).multiplicity < MaxStateMultiplicity) {
    uint32_t mergeIndex = (**state).getMergeIndex();
    StatesTrace::iterator it = statesTrace.find(mergeIndex);
    if (it == statesTrace.end()) {
        it = statesTrace.insert(std::make_pair(mergeIndex, new StatesSet)).first;
    }
    it->second->insert(state);

    // XXX for some reason the following causes a slowdown
    /*
    if (statesToForward.empty() && it->second->size() > 1) {
        // We are not currently fast-forwarding, but the current
        // state can now be fast-forwarded
        statesToForward.insert(current);
        stats::fastForwardsStart += 1;
    }
    */
  }

  strategy->onStateStepped(state);
}

SymbolicState* LazyMergingStrategy::onNextStateSelectionEx(bool &canBatch, uint32_t &batchDest) {
  SymbolicState *state = NULL, *merged = NULL;
  canBatch = false;

  while (!statesToForward.empty()) {
   // TODO: do not fast-forward state if there are other
   // states that could be merged with state first (i.e., select
   // smartly what state to fast-forward first).

   uint32_t mergeIndex = 0;
   unsigned candidates = 0;
   StatesTrace::iterator traceIt;

   /* Find the state that has maximum number of potential targets to merge */
   for (StatesSet::iterator it = statesToForward.begin(),
                       ie = statesToForward.end(); it != ie;) {
     uint32_t _mergeIndex = (**(*it)).getMergeIndex();
     StatesTrace::iterator _traceIt = statesTrace.find(_mergeIndex);
     unsigned _candidates = (_traceIt == statesTrace.end() ? 0
                    : _traceIt->second->size() - _traceIt->second->count(*it));

     if (_candidates == 0) {
         // State can no longer be fast-forwarded, perhaps it branched
         // in a different direction that it's merge target

         // XXX: StatesSet does not support erase by iterator!
         // Moreover, erasing seems to invalidate iterators.
         statesToForward.erase(*it);
         //stats::fastForwardsFail += 1;

         // Restart iteration from the begining since our iterators
         // are invalidated by calling erase
         it = statesToForward.begin();

     } else {
         if (_candidates > candidates) {
           state = *it;
           mergeIndex = _mergeIndex;
           candidates = _candidates;
           traceIt = _traceIt;
         }
         ++it;
     }
   }

   if (state == NULL) {
       // All states were removed from statesToForward
       break;
   }

   // Check wether we can already merge
   for (StatesSet::iterator it = traceIt->second->begin(),
                            ie = traceIt->second->end(); it != ie; ++it) {
     SymbolicState *state1 = *it;
     assert(!MaxStateMultiplicity || (**state1).multiplicity < MaxStateMultiplicity);

     if (state1 != state && (**state1).getMergeIndex() == mergeIndex) {
       // State is at the same execution index as state1, let's try merging
       if (jobManager->mergeStates(state1, state)) {
         // We've merged !
         // Any of the merged states could be followed for fast forwards.
         // Make traces that was pointing to state to point to state1
         for (StatesTrace::iterator it1 = statesTrace.begin(),
                                    ie1 = statesTrace.end(); it1 != ie1; ++it1) {
           it1->second->erase(state);
           if (MaxStateMultiplicity && (**state1).multiplicity >= MaxStateMultiplicity) {
             it1->second->erase(state1);
           }
         }

         statesToForward.erase(state);

         state = NULL;
         merged = state1;
         break;
       }
     }
   }

   if (state) {
     canBatch = true;
     batchDest = mergeIndex;
     // The state was not merged right now. Let us fast-forward it.
     return state;
   }
  }

  assert(state == NULL);

  // At this point we might have terminated states, but the base searcher is
  // unaware about it. We can not call it since it may crash. Instead, we
  // simply return the last merged state.
  if (merged) {
   return merged;
  }

  // Nothing to fast-forward
  // Get state from base searcher
  state = strategy->onNextStateSelection();

  if (state == NULL)
    return NULL;

  if (canFastForwardState(state)) {
   statesToForward.insert(state);
   //stats::fastForwardsStart += 1;
   return onNextStateSelectionEx(canBatch, batchDest); // recursive
  }

  return state;
}

SymbolicState* LazyMergingStrategy::onNextStateSelection() {
  bool canBatch = false;
  uint32_t batchDest = 0;

  return onNextStateSelectionEx(canBatch, batchDest);
}

}

}
