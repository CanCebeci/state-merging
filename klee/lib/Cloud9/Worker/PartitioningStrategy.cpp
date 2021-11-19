/*
 * PartitioningStrategy.cpp
 *
 *  Created on: Feb 18, 2011
 *      Author: stefan
 */

#include "cloud9/worker/PartitioningStrategy.h"
#include "cloud9/worker/ComplexStrategies.h"
#include "cloud9/worker/TreeObjects.h"
#include "cloud9/worker/WorkerCommon.h"
#include "cloud9/worker/JobManager.h"

#include "klee/Internal/ADT/RNG.h"
#include "klee/Searcher.h"
#include "klee/Executor.h"

using namespace klee;

namespace cloud9 {

namespace worker {

part_id_t PartitioningStrategy::hashState(SymbolicState* state) {
  WorkerTree::Node *node = state->getNode().get();
  node = node->getParent();
  if (!node)
    return 0;

  ForkTag tag = (**node).getForkTag();

  return (part_id_t)tag.instrID;
}

StatePartition PartitioningStrategy::createPartition() {
  return StatePartition(jobManager->createBaseStrategy());
}

void PartitioningStrategy::activateStateInPartition(SymbolicState *state,
    part_id_t partID, StatePartition &part) {

  if (part.activeStates.insert(state).second) {
    part.strategy->onStateActivated(state);
  }

  activeStates.insert(state);
  if (nonEmpty.insert(partID).second) {
    part.active = true; // Activate by default new partitions
  }
}

void PartitioningStrategy::deactivateStateInPartition(SymbolicState *state,
    part_id_t partID, StatePartition &part) {
  if (part.activeStates.erase(state)) {
    part.strategy->onStateDeactivated(state);
  }

  activeStates.erase(state);

  if (part.activeStates.empty()) {
    if (partID == *nextPartition) {
      nextPartition++;
    }
    nonEmpty.erase(partID);
  }
}

void PartitioningStrategy::onStateActivated(SymbolicState *state) {
  part_id_t key = hashState(state);
  states[state] = key;

  if (partitions.count(key) == 0) {
    partitions.insert(std::make_pair(key, createPartition()));
  }
  StatePartition &part = partitions.find(key)->second;

  part.states.insert(state);

  // Now decide whether to activate the state or not
  if (part.active) {
    activateStateInPartition(state, key, part);
  } else {
    if (part.activeStates.count(state->getParent()) > 0) {
      if (theRNG.getBool()) {
        // Remove the parent from the active states, and replace it with the child
        deactivateStateInPartition(state->getParent(), key, part);
        activateStateInPartition(state, key, part);
      }
    } else {
      activateStateInPartition(state, key, part);
    }
  }
}

void PartitioningStrategy::onStateUpdated(SymbolicState *state, WorkerTree::Node *oldNode) {
  part_id_t newKey = hashState(state);
  part_id_t oldKey = states[state];
  StatePartition &oldPart = partitions.find(oldKey)->second;
  if (newKey == oldKey) {
    if (oldPart.activeStates.count(state) > 0)
      oldPart.strategy->onStateUpdated(state, oldNode);
    return;
  }

  states[state] = newKey;
  deactivateStateInPartition(state, oldKey, oldPart);
  oldPart.states.erase(state);

  if (partitions.count(newKey) == 0) {
    partitions.insert(std::make_pair(newKey, createPartition()));
  }

  StatePartition &newPart = partitions.find(newKey)->second;
  newPart.states.insert(state);
  // By default, the state is active - it may be deactivated when the
  // child state is added
  activateStateInPartition(state, newKey, newPart);
}

void PartitioningStrategy::onStateDeactivated(SymbolicState *state) {
  part_id_t key = hashState(state);
  states.erase(state);

  StatePartition &part = partitions.find(key)->second;

  deactivateStateInPartition(state, key, part);
  part.states.erase(state);
}

void PartitioningStrategy::onStateStepped(SymbolicState *state) {
  // Just forward this to the relevant strategy
  part_id_t key = hashState(state);
  StatePartition &part = partitions.find(key)->second;
  part.strategy->onStateStepped(state);
}

SymbolicState* PartitioningStrategy::onNextStateSelection() {
  if (nextPartition == nonEmpty.end()) {
    nextPartition = nonEmpty.begin();
  }

  if (nextPartition != nonEmpty.end()) {
    StatePartition &part = partitions.find(*nextPartition)->second;
    SymbolicState *state = part.strategy->onNextStateSelection();
    assert(state != NULL);
    if (!part.activeStates.count(state)) {
      CLOUD9_DEBUG("Orphan state selected for partition " << *nextPartition);
    }
    nextPartition++;

    return state;
  }

  return NULL;
}

void PartitioningStrategy::getStatistics(part_stats_t &stats) {
  std::stringstream ss;

  for (part_id_set_t::iterator it = nonEmpty.begin(); it != nonEmpty.end(); it++) {
    StatePartition &part = partitions.find(*it)->second;

    stats.insert(std::make_pair(*it, std::make_pair(part.states.size(),
        part.activeStates.size())));

    ss << '[' << *it << ": " << part.activeStates.size() << '/' << part.states.size() << "] ";
  }

  CLOUD9_DEBUG("State Partition: " << ss.str());
}

void PartitioningStrategy::setActivation(std::set<part_id_t> &activation) {
  for (std::set<part_id_t>::iterator it = activation.begin();
      it != activation.end(); it++) {
    StatePartition &part = partitions.find(*it)->second;

    part.active = true;
  }
}

ExecutionPathSetPin PartitioningStrategy::selectStates(part_select_t &counts) {
  jobManager->lockJobs();

  std::vector<WorkerTree::Node*> stateRoots;

  for (part_select_t::iterator it = counts.begin();
      it != counts.end(); it++) {
    if (!it->second)
      continue;

    StatePartition &part = partitions.find(it->first)->second;
    std::set<SymbolicState*> inactiveStates;
    getInactiveSet(it->first, inactiveStates);

    unsigned int inactiveCount = it->second;
    if (inactiveStates.size() < it->second) {
      inactiveCount = inactiveStates.size();
    }

    // Grab the inactive states...
    if (inactiveCount > 0) {
      unsigned int counter = inactiveCount;
      for (std::set<SymbolicState*>::iterator it = inactiveStates.begin();
          it != inactiveStates.end(); it++) {
        if (*it == jobManager->getCurrentState())
          continue;
        stateRoots.push_back((*it)->getNode().get());
        counter--;
        if (!counter) break;
      }

      inactiveCount -= counter; // Add back states that couldn't be selected
    }

    // Now grab the active ones...
    if (it->second - inactiveCount > 0) {
      unsigned int counter = it->second - inactiveCount;
      for (std::set<SymbolicState*>::iterator it = part.activeStates.begin();
          it != part.activeStates.end(); it++) {
        if (*it == jobManager->getCurrentState())
          continue;
        stateRoots.push_back((*it)->getNode().get());
        counter--;
        if (!counter) break;
      }
    }
  }

  CLOUD9_DEBUG("Selected " << stateRoots.size() << " from partitioning strategy.");

  ExecutionPathSetPin paths = jobManager->getTree()->buildPathSet(stateRoots.begin(),
        stateRoots.end(), (std::map<WorkerTree::Node*,unsigned>*)NULL);

  jobManager->unlockJobs();

  return paths;
}

void PartitioningStrategy::getInactiveSet(part_id_t partID,
    std::set<SymbolicState*> &inactiveStates) {
  StatePartition &part = partitions.find(partID)->second;

  for (std::set<SymbolicState*>::iterator it = part.states.begin();
      it != part.states.end(); it++) {
    if (!part.activeStates.count(*it))
      inactiveStates.insert(*it);
  }
}

void PartitioningStrategy::dumpSymbolicTree(WorkerTree::Node *highlight) {
  jobManager->dumpSymbolicTree(NULL, PartitioningDecorator(this, highlight));
}

////////////////////////////////////////////////////////////////////////////////
// KLEE FORK CAP STRATEGY
////////////////////////////////////////////////////////////////////////////////

KleeForkCapStrategy::KleeForkCapStrategy(unsigned long _forkCap, unsigned long _hardForkCap,
    WorkerTree *_tree, SymbolicEngine *_engine)
      : KleeStrategy(_tree) {
  klee::Searcher *baseSearcher = new RandomSearcher();

  klee::Executor *executor = dyn_cast<klee::Executor>(_engine); // XXX I should be ashamed of this
  searcher = new ForkCapSearcher(*executor, baseSearcher, _forkCap, _hardForkCap);
}

KleeForkCapStrategy::~KleeForkCapStrategy() {
  delete searcher;
}

}

}
