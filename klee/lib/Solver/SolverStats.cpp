//===-- SolverStats.cpp ---------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "SolverStats.h"

using namespace klee;

Statistic stats::cexCacheTime("CexCacheTime", "CCtime", true);
Statistic stats::queries("Queries", "Q");
Statistic stats::queriesInvalid("QueriesInvalid", "Qiv");
Statistic stats::queriesValid("QueriesValid", "Qv");
Statistic stats::queryCacheHits("QueryCacheHits", "QChits") ;
Statistic stats::queryCacheMisses("QueryCacheMisses", "QCmisses");
Statistic stats::queryConstructTime("QueryConstructTime", "QBtime", true);
Statistic stats::queryConstructs("QueriesConstructs", "QB");
Statistic stats::queryCounterexamples("QueriesCEX", "Qcex");
Statistic stats::queryTime("QueryTime", "Qtime", true);
