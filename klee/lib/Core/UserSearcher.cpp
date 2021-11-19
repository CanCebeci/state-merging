//===-- UserSearcher.cpp --------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Common.h"

#include "UserSearcher.h"

#include "klee/Searcher.h"
#include "klee/Executor.h"

#include "llvm/Support/CommandLine.h"

using namespace llvm;
using namespace klee;

namespace {
  enum MergingType {
    None,
    Manual,
    Bump,
    Lazy,
    Static
  };

  cl::opt<bool>
  UseRandomSearch("use-random-search");

  cl::opt<bool>
  UseInterleavedRS("use-interleaved-RS");

  cl::opt<bool>
  UseInterleavedNURS("use-interleaved-NURS");

  cl::opt<bool>
  UseInterleavedMD2UNURS("use-interleaved-MD2U-NURS");

  cl::opt<bool>
  UseInterleavedInstCountNURS("use-interleaved-icnt-NURS");

  cl::opt<bool>
  UseInterleavedCPInstCountNURS("use-interleaved-cpicnt-NURS");

  cl::opt<bool>
  UseInterleavedQueryCostNURS("use-interleaved-query-cost-NURS");

  cl::opt<bool>
  UseInterleavedCovNewNURS("use-interleaved-covnew-NURS");

  cl::opt<bool>
  UseNonUniformRandomSearch("use-non-uniform-random-search");

  cl::opt<bool>
  UseRandomPathSearch("use-random-path");

  cl::opt<WeightedRandomSearcher::WeightType>
  WeightType("weight-type", cl::desc("Set the weight type for --use-non-uniform-random-search"),
             cl::values(clEnumValN(WeightedRandomSearcher::Depth, "none", "use (2^depth)"),
                        clEnumValN(WeightedRandomSearcher::InstCount, "icnt", "use current pc exec count"),
                        clEnumValN(WeightedRandomSearcher::CPInstCount, "cpicnt", "use current pc exec count"),
                        clEnumValN(WeightedRandomSearcher::QueryCost, "query-cost", "use query cost"),
                        clEnumValN(WeightedRandomSearcher::MinDistToUncovered, "md2u", "use min dist to uncovered"),
                        clEnumValN(WeightedRandomSearcher::CoveringNew, "covnew", "use min dist to uncovered + coveringNew flag"),
                        clEnumValEnd));
  
  cl::opt<MergingType>
  UseMerge("use-merge", cl::desc("Enable support for state merging"),
      cl::values(clEnumValN(None, "none", "disable merging"),
                 clEnumValN(Manual, "manual", "klee_merge() (experimental)"),
                 clEnumValN(Bump, "bump", "klee_merge() (extra experimental)"),
                 clEnumValN(Lazy, "lazy", "lazy merging (research)"),
                 clEnumValN(Static, "static", "static merging (plain ol')"),
                 clEnumValEnd));

  cl::opt<bool>
  UseIterativeDeepeningTimeSearch("use-iterative-deepening-time-search",
                                    cl::desc("(experimental)"));

  cl::opt<bool>
  UseBatchingSearch("use-batching-search",
           cl::desc("Use batching searcher (keep running selected state for N instructions/time, see --batch-instructions and --batch-time"));

  cl::opt<bool>
  UseCheckpointSearch("use-checkpoint-search",
      cl::desc("Use checkpoint searcher (continuously execute states between static checkpoints in the program)"));

  cl::opt<unsigned>
  BatchInstructions("batch-instructions",
                    cl::desc("Number of instructions to batch when using --use-batching-search"),
                    cl::init(10000));
  
  cl::opt<double>
  BatchTime("batch-time",
            cl::desc("Amount of time to batch when using --use-batching-search"),
            cl::init(5.0));

  cl::opt<unsigned>
  UseForkCap("use-fork-cap",
            cl::desc("Limit the maximum number of states forked at the same point that are considered at a time"),
            cl::init(0));

  cl::opt<unsigned>
  UseHardForkCap("use-hard-fork-cap",
            cl::desc("Hard limit on the maximum number of states forked at the same point"),
            cl::init(0));
}

bool klee::userSearcherRequiresMD2U() {
  return (WeightType==WeightedRandomSearcher::MinDistToUncovered ||
          WeightType==WeightedRandomSearcher::CoveringNew ||
          UseInterleavedMD2UNURS ||
          UseInterleavedCovNewNURS || 
          UseInterleavedInstCountNURS || 
          UseInterleavedCPInstCountNURS || 
          UseInterleavedQueryCostNURS);
}

// FIXME: Remove.
bool klee::userSearcherRequiresBranchSequences() {
  return false;
}

bool klee::userSearcherRequiresMergeAnalysis() {
  return UseMerge && (UseMerge == Lazy);
}

Searcher *klee::constructUserSearcher(Executor &executor, Searcher *original) {
  Searcher *searcher = original;

  if (!searcher) {
	  if (UseRandomPathSearch) {
		searcher = new RandomPathSearcher(executor);
	  } else if (UseNonUniformRandomSearch) {
		searcher = new WeightedRandomSearcher(executor, WeightType);
	  } else if (UseRandomSearch) {
		searcher = new RandomSearcher();
	  } else {
		searcher = new DFSSearcher();
	  }
  }

  if (UseInterleavedNURS || UseInterleavedMD2UNURS || UseInterleavedRS ||
      UseInterleavedCovNewNURS || UseInterleavedInstCountNURS ||
      UseInterleavedCPInstCountNURS || UseInterleavedQueryCostNURS) {
    std::vector<Searcher *> s;
    s.push_back(searcher);
    
    if (UseInterleavedNURS)
      s.push_back(new WeightedRandomSearcher(executor, 
                                             WeightedRandomSearcher::Depth));
    if (UseInterleavedMD2UNURS)
      s.push_back(new WeightedRandomSearcher(executor, 
                                             WeightedRandomSearcher::MinDistToUncovered));

    if (UseInterleavedCovNewNURS)
      s.push_back(new WeightedRandomSearcher(executor, 
                                             WeightedRandomSearcher::CoveringNew));
   
    if (UseInterleavedInstCountNURS)
      s.push_back(new WeightedRandomSearcher(executor, 
                                             WeightedRandomSearcher::InstCount));
    
    if (UseInterleavedCPInstCountNURS)
      s.push_back(new WeightedRandomSearcher(executor, 
                                             WeightedRandomSearcher::CPInstCount));
    
    if (UseInterleavedQueryCostNURS)
      s.push_back(new WeightedRandomSearcher(executor, 
                                             WeightedRandomSearcher::QueryCost));

    if (UseInterleavedRS) 
      s.push_back(new RandomSearcher());

    searcher = new InterleavedSearcher(s);
  }

  if (UseBatchingSearch) {
    searcher = new BatchingSearcher(searcher, BatchTime, BatchInstructions);
  }

  if (UseMerge && (UseMerge != None)) {
    switch (UseMerge) {
    case Manual:
      searcher = new MergingSearcher(executor, searcher);
      break;
    case Bump:
      searcher = new BumpMergingSearcher(executor, searcher);
      break;
    case Lazy:
      searcher = new LazyMergingSearcher(executor, searcher);
      break;
    case Static:
      searcher = new StaticMergingSearcher(executor);
      break;
    default:
      // No merging
      break;
    }
  }
  
  if (UseCheckpointSearch) {
    searcher = new CheckpointSearcher(searcher);
  }

  if (UseIterativeDeepeningTimeSearch) {
    searcher = new IterativeDeepeningTimeSearcher(searcher);
  }

  /* XXX: think about the ordering of ForkCap and LazyMerge */
  if (UseForkCap != 0 || UseHardForkCap != 0) {
    searcher = new ForkCapSearcher(executor, searcher,
                                   UseForkCap, UseHardForkCap);
  }

  std::ostream &os = executor.getHandler().getInfoStream();

  os << "BEGIN searcher description\n";
  searcher->printName(os);
  os << "END searcher description\n";

  return searcher;
}
