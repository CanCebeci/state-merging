//===-- KInstruction.cpp --------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "llvm/Instruction.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Debug.h"
#include <iostream>

using namespace llvm;
using namespace klee;

/***/

KInstruction::~KInstruction() {
  delete[] operands;
  if (qceInfo)
    delete qceInfo;
}

void KInstruction::print(llvm::raw_ostream &ostr) const {
  ostr << "Instruction:\n    ";
  inst->print(ostr); ostr << "\n";
  ostr << "    at " << info->file << ":" << info->line
       << " (assembly line " << info->assemblyLine << ")";
}

void KInstruction::dump() const {
  print(dbgs()); dbgs() << '\n';
}
