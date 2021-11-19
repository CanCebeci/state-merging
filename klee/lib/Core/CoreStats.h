//===-- CoreStats.h ---------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_CORESTATS_H
#define KLEE_CORESTATS_H

#include "klee/Statistic.h"

namespace klee {
namespace stats {

  extern Statistic allocations;
  extern Statistic resolveTime;
  extern Statistic instructions;
  extern Statistic instructionsMult;
  extern Statistic instructionsMultHigh;
  extern Statistic instructionsMultExact;
  extern Statistic instructionTime;
  extern Statistic instructionRealTime;
  extern Statistic locallyCoveredInstructions;
  extern Statistic globallyCoveredInstructions;
  extern Statistic locallyUncoveredInstructions;
  extern Statistic globallyUncoveredInstructions;
  extern Statistic trueBranches;
  extern Statistic falseBranches;
  extern Statistic forkTime;
  extern Statistic mergeSuccessTime;
  extern Statistic mergeFailTime;
  extern Statistic solverTime;

  /// The number of process forks.
  extern Statistic forks;
  extern Statistic forksMult;
  extern Statistic forksMultExact;

  extern Statistic paths;
  extern Statistic pathsMult;
  extern Statistic pathsMultExact;

  /// The number of started fast forwards.
  extern Statistic fastForwardsStart;

  /// The number of failed fast forwards.
  extern Statistic fastForwardsFail;

  /// The number of successful merges.
  extern Statistic mergesSuccess;

  /// The number of failed merge attempts.
  extern Statistic mergesFail;

  /// Number of states, this is a "fake" statistic used by istats, it
  /// isn't normally up-to-date.
  extern Statistic states;

  /// Instruction level statistic for tracking number of reachable
  /// uncovered instructions.
  extern Statistic reachableGloballyUncovered;

  /// Instruction level statistic tracking the minimum intraprocedural
  /// distance to an uncovered instruction; this is only periodically
  /// updated.
  extern Statistic minDistToGloballyUncovered;

  /// Instruction level statistic tracking the minimum intraprocedural
  /// distance to a function return.
  extern Statistic minDistToReturn;

  /// Execution time (excluding initialization, statistics, the searcher, etc.)
  extern Statistic executionTime;
  extern Statistic duplicatesExecutionTime;

  extern Statistic searcherTime;
}
}

#endif
