//===-- CoreStats.cpp -----------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "CoreStats.h"

using namespace klee;

Statistic stats::allocations("Allocations", "Alloc");
Statistic stats::locallyCoveredInstructions("LocallyCoveredInstructions", "LIcov");
Statistic stats::globallyCoveredInstructions("GloballyCoveredInstructions", "GIcov");
Statistic stats::falseBranches("FalseBranches", "Bf");
Statistic stats::forkTime("ForkTime", "Ftime", true);
Statistic stats::forks("Forks", "Forks");
Statistic stats::forksMult("ForksMult", "ForksMult");
Statistic stats::forksMultExact("ForksMultExact", "ForksMultExact");
Statistic stats::mergeSuccessTime("MergeSuccessTime", "MStime", true);
Statistic stats::mergeFailTime("MergeFailTime", "MFtime", true);
Statistic stats::mergesSuccess("MergesSuccess", "MergesS");
Statistic stats::mergesFail("MergesFail", "MergesF");
Statistic stats::fastForwardsStart("FastForwardStart", "FFForwardT");
Statistic stats::fastForwardsFail("FastForwardFail", "FForwardF");
Statistic stats::instructionRealTime("InstructionRealTimes", "Ireal", true);
Statistic stats::instructionTime("InstructionTimes", "Itime", true);
Statistic stats::instructions("Instructions", "I");
Statistic stats::instructionsMult("InstructionsMult", "InstructionsMult");
Statistic stats::instructionsMultHigh("InstructionsMultHigh", "InstructionsMultHigh");
Statistic stats::instructionsMultExact("InstructionsMultExact", "InstructionsMultExact");
Statistic stats::minDistToReturn("MinDistToReturn", "Rdist");
Statistic stats::minDistToGloballyUncovered("MinDistToGloballyUncovered", "UCdist");
Statistic stats::reachableGloballyUncovered("ReachableGloballyUncovered", "IuncovReach");
Statistic stats::resolveTime("ResolveTime", "Rtime", true);
Statistic stats::solverTime("SolverTime", "Stime", true);
Statistic stats::states("States", "States");
Statistic stats::trueBranches("TrueBranches", "Bt");
Statistic stats::locallyUncoveredInstructions("LocallyUncoveredInstructions", "LIuncov");
Statistic stats::globallyUncoveredInstructions("GloballyUncoveredInstructions", "GIuncov");

Statistic stats::executionTime("ExecutionTime", "ETime", true);
Statistic stats::duplicatesExecutionTime("ExecutionDuplicatesTime", "EDTime", true);

Statistic stats::paths("Paths", "Paths");
Statistic stats::pathsMult("PathsMult", "PathsMult");
Statistic stats::pathsMultExact("PathsMultExact", "PathsMultExact");

Statistic stats::searcherTime("SearcherTime", "SearcherTime", true);
