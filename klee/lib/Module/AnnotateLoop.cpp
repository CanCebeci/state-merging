//===-- AnnotateLoop.cpp --------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Passes.h"
#include "klee/Config/config.h"

#include "llvm/Support/CFG.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/ADT/SmallSet.h"

#if !(LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 7)
#include "llvm/LLVMContext.h"
#endif

namespace llvm {
  void initializeAnnotateLoopPassPass(PassRegistry&);
}

using namespace llvm;
using namespace klee;

AnnotateLoopPass::AnnotateLoopPass() : LoopPass(ID),
      m_kleeLoopIterFunc(0), m_kleeLoopExitFunc(0), lastLoopID(0) {
  initializeAnnotateLoopPassPass(*PassRegistry::getPassRegistry());
}

void AnnotateLoopPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<LoopInfo>();
  AU.addRequiredID(LoopSimplifyID);
  AU.setPreservesCFG();
}

bool AnnotateLoopPass::runOnLoop(Loop *L, LPPassManager &LPM) {
  BasicBlock *H = L->getHeader(); assert(H);
  Module *M = H->getParent()->getParent();

  if (!m_kleeLoopIterFunc) {
    // FIXME: create the prototypes if needed
    m_kleeLoopIterFunc = M->getFunction("_klee_loop_iter");
    m_kleeLoopExitFunc = M->getFunction("_klee_loop_exit");
    assert(m_kleeLoopIterFunc && m_kleeLoopExitFunc &&
         "Loop tracking functions not found!");
  }

  Value *args =
        ConstantInt::get(Type::getInt32Ty(getGlobalContext()), ++lastLoopID);

  // Annotate loop header: it runs every iteration
  CallInst::Create(m_kleeLoopIterFunc, &args, (&args)+1, "",
                   H->getFirstNonPHI());

  // Find and annotate all loop exits
  SmallVector<BasicBlock*, 8> ExitBBs;
  L->getExitBlocks(ExitBBs);

  SmallSet<BasicBlock*, 8> UExitBBs;
  UExitBBs.insert(ExitBBs.begin(), ExitBBs.end());
  for (SmallSet<BasicBlock*, 8>::iterator EI = UExitBBs.begin(),
                                          EE = UExitBBs.end(); EI != EE; ++EI) {
    CallInst::Create(m_kleeLoopExitFunc, &args, (&args)+1, "",
                     (*EI)->getFirstNonPHI());
  }

  return true;
}

char AnnotateLoopPass::ID = 0;

INITIALIZE_PASS_BEGIN(AnnotateLoopPass, "annotate-loop-pass",
                      "Annotate loops for KLEE", false, false)
INITIALIZE_PASS_DEPENDENCY(LoopInfo)
INITIALIZE_PASS_DEPENDENCY(LoopSimplify)
INITIALIZE_PASS_END(AnnotateLoopPass, "annotate-loop-pass",
                      "Annotate loops for KLEE", false, false)
