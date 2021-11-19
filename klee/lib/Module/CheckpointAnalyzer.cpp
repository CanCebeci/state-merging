/*
 * CheckpointAnalyzer.cpp
 *
 *  Created on: Jul 9, 2011
 *      Author: stefan
 */

#include "Passes.h"

#include "llvm/Pass.h"
#include "llvm/BasicBlock.h"

using namespace llvm;

namespace klee {

// TODO: Be more clever about the points to choose for checkpointing,
// based on loop analysis

CheckpointAnalyzer::CheckpointAnalyzer() : BasicBlockPass(ID) {

}

void CheckpointAnalyzer::getAnalysisUsage(AnalysisUsage &Info) const {
  Info.setPreservesAll();
}

bool CheckpointAnalyzer::runOnBasicBlock(BasicBlock &BB) {
  return false;
}

bool isCheckpoint(llvm::Instruction &Instr) {
  return true;
}

char CheckpointAnalyzer::ID = 0;

} /* namespace klee */
