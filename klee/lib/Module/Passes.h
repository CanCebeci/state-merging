//===-- Passes.h ------------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_PASSES_H
#define KLEE_PASSES_H

#include "klee/Config/config.h"

#include "llvm/Constants.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#include "llvm/Pass.h"
#include "llvm/CallGraphSCCPass.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/CodeGen/IntrinsicLowering.h"

namespace llvm {
  class Function;
  class Instruction;
  class Module;
  class TargetData;
  class TargetLowering;
  class Type;
  class Region;
}

namespace klee {

  /// RaiseAsmPass - This pass raises some common occurences of inline
  /// asm which are used by glibc into normal LLVM IR.
class RaiseAsmPass : public llvm::ModulePass {
  static char ID;

#if (LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR >= 9)
  const llvm::TargetLowering *TLI;
#endif

  llvm::Function *getIntrinsic(llvm::Module &M,
                               unsigned IID,
                               const llvm::Type **Tys,
                               unsigned NumTys);
  llvm::Function *getIntrinsic(llvm::Module &M,
                               unsigned IID, 
                               const llvm::Type *Ty0) {
    return getIntrinsic(M, IID, &Ty0, 1);
  }

  bool runOnInstruction(llvm::Module &M, llvm::Instruction *I);

public:
#if (LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 8)
  RaiseAsmPass() : llvm::ModulePass((intptr_t) &ID) {}
#else
  RaiseAsmPass() : llvm::ModulePass(ID) {}
#endif
  
  virtual bool runOnModule(llvm::Module &M);
};

  // This is a module pass because it can add and delete module
  // variables (via intrinsic lowering).
class IntrinsicCleanerPass : public llvm::ModulePass {
  static char ID;
  const llvm::TargetData &TargetData;
  llvm::IntrinsicLowering *IL;
  bool LowerIntrinsics;

  bool runOnBasicBlock(llvm::BasicBlock &b);
public:
  IntrinsicCleanerPass(const llvm::TargetData &TD,
                       bool LI=true)
#if (LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 8)
    : llvm::ModulePass((intptr_t) &ID),
#else
    : llvm::ModulePass(ID),
#endif
      TargetData(TD),
      IL(new llvm::IntrinsicLowering(TD)),
      LowerIntrinsics(LI) {}
  ~IntrinsicCleanerPass() { delete IL; } 
  
  virtual bool runOnModule(llvm::Module &M);
};
  
class LowerSSEPass : public llvm::ModulePass {
  static char ID;

  bool runOnBasicBlock(llvm::BasicBlock &b);
public:
  LowerSSEPass()
#if (LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 8)
    : llvm::ModulePass((intptr_t) &ID) {}
#else
    : llvm::ModulePass(ID) {}
#endif
  
  virtual bool runOnModule(llvm::Module &M);
};
  
class SIMDInstrumentationPass : public llvm::ModulePass {
  static char ID;

  bool runOnBasicBlock(llvm::BasicBlock &b);
public:
  SIMDInstrumentationPass()
#if (LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 8)
    : llvm::ModulePass((intptr_t) &ID) {}
#else
    : llvm::ModulePass(ID) {}
#endif
  
  virtual bool runOnModule(llvm::Module &M);
};

  // performs two transformations which make interpretation
  // easier and faster.
  //
  // 1) Ensure that all the PHI nodes in a basic block have
  //    the incoming block list in the same order. Thus the
  //    incoming block index only needs to be computed once
  //    for each transfer.
  // 
  // 2) Ensure that no PHI node result is used as an argument to
  //    a subsequent PHI node in the same basic block. This allows
  //    the transfer to execute the instructions in order instead
  //    of in two passes.
class PhiCleanerPass : public llvm::FunctionPass {
  static char ID;

public:
#if (LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 8)
  PhiCleanerPass() : llvm::FunctionPass((intptr_t) &ID) {}
#else
  PhiCleanerPass() : llvm::FunctionPass(ID) {}
#endif
  
  virtual bool runOnFunction(llvm::Function &f);
};
  
class DivCheckPass : public llvm::ModulePass {
  static char ID;
public:
#if (LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 8)
  DivCheckPass(): ModulePass((intptr_t) &ID) {}
#else
  DivCheckPass(): ModulePass(ID) {}
#endif
  virtual bool runOnModule(llvm::Module &M);
};

/// LowerSwitchPass - Replace all SwitchInst instructions with chained branch
/// instructions.  Note that this cannot be a BasicBlock pass because it
/// modifies the CFG!
class LowerSwitchPass : public llvm::FunctionPass {
public:
  static char ID; // Pass identification, replacement for typeid
#if (LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 8)
  LowerSwitchPass() : FunctionPass((intptr_t) &ID) {} 
#else
  LowerSwitchPass() : FunctionPass(ID) {} 
#endif
  
  virtual bool runOnFunction(llvm::Function &F);
  
  struct SwitchCase {
    llvm ::Constant *value;
    llvm::BasicBlock *block;
    
    SwitchCase() : value(0), block(0) { }
    SwitchCase(llvm::Constant *v, llvm::BasicBlock *b) :
      value(v), block(b) { }
  };
  
  typedef std::vector<SwitchCase>           CaseVector;
  typedef std::vector<SwitchCase>::iterator CaseItr;
  
private:
  void processSwitchInst(llvm::SwitchInst *SI);
  void switchConvert(CaseItr begin,
                     CaseItr end,
                     llvm::Value *value,
                     llvm::BasicBlock *origBlock,
                     llvm::BasicBlock *defaultBlock);
};

/// AnnotateLoopPass - Annotate all loop headers and exits with a call
/// to a special KLEE functions. This pass does not changes the CFG.
///
/// NOTE: LLVM loop analysis ignores loops that have multiple entry
///       points. Such loops will remain uninstrumented.
class AnnotateLoopPass : public llvm::LoopPass {
  llvm::Function *m_kleeLoopIterFunc;
  llvm::Function *m_kleeLoopExitFunc;

  int lastLoopID;
public:
  static char ID;
  AnnotateLoopPass();

  virtual void getAnalysisUsage(llvm::AnalysisUsage &) const;
  virtual bool runOnLoop(llvm::Loop *L, llvm::LPPassManager &LPM);
};

class QCEAnalyzerPass : public llvm::FunctionPass {
  llvm::TargetData *m_targetData;

public:
  static char ID;
  QCEAnalyzerPass(llvm::TargetData *TD = 0);

  virtual void getAnalysisUsage(llvm::AnalysisUsage &Info) const;
  bool runOnFunction(llvm::Function &F);
};

class CheckpointAnalyzer: public llvm::BasicBlockPass {
public:
  CheckpointAnalyzer();

  virtual void getAnalysisUsage(llvm::AnalysisUsage &Info) const;
  virtual bool runOnBasicBlock(llvm::BasicBlock &BB);

  virtual ~CheckpointAnalyzer() { };

  bool isCheckpoint(llvm::Instruction &Instr);

  static char ID;
};

class RendezVousPointPass: public llvm::FunctionPass {
private:
  llvm::Function *m_kleeRendezVousFunc;

  unsigned bbID;

  void traverseBB(llvm::BasicBlock *bb,
      std::map<llvm::BasicBlock*, unsigned> &status);
public:
  static char ID;

  RendezVousPointPass();

  virtual bool doInitialization(llvm::Module &M);
  virtual bool runOnFunction(llvm::Function &F);
};

}

#endif
