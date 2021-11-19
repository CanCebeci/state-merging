#include "Passes.h"

#include <llvm/Module.h>
#include <llvm/Support/CFG.h>

#include <stack>

using namespace llvm;
using namespace klee;

namespace llvm {
  void initializeRendezVousPointPassPass(PassRegistry&);
}

namespace klee {

char RendezVousPointPass::ID = 0;

RendezVousPointPass::RendezVousPointPass() : FunctionPass(ID),
    m_kleeRendezVousFunc(NULL), bbID(0) {
  initializeRendezVousPointPassPass(*PassRegistry::getPassRegistry());
}

bool RendezVousPointPass::doInitialization(Module &M) {
  bool changed = false;
  LLVMContext &Ctx = M.getContext();

  m_kleeRendezVousFunc = M.getFunction("_klee_rendez_vous");
  if (!m_kleeRendezVousFunc) {
    m_kleeRendezVousFunc = Function::Create(
        FunctionType::get(Type::getVoidTy(Ctx),
            std::vector<const Type*>(1, Type::getInt32Ty(Ctx)), false),
        Function::ExternalLinkage,
        "_klee_rendez_vous",
        &M);
    changed = true;
  }

  return changed;
}

void RendezVousPointPass::traverseBB(BasicBlock *bb,
    std::map<BasicBlock*,unsigned> &status) {

  status[bb] = 1;

  for (succ_iterator it = succ_begin(bb), ie = succ_end(bb);
      it != ie; it++) {
    BasicBlock *succBB = *it;

    // XXX: Need to detect only the join points (?)

    switch(status[succBB]) {
    case 0: // unvisited
      traverseBB(succBB, status);
      break;
    case 1: // back edge
      break;
    case 2: // forward or across
      break;
    default:
      assert(0 && "broken DFS");
      break;
    }
  }

  status[bb] = 2;

  // Annotate the BB with its own ID
  Value *args = ConstantInt::get(Type::getInt32Ty(bb->getContext()), ++bbID);

  CallInst::Create(m_kleeRendezVousFunc, &args, (&args)+1, Twine(),
      bb->getFirstNonPHI());
}

bool RendezVousPointPass::runOnFunction(Function &F) {
  std::map<BasicBlock*, unsigned> visitStatus; // 0 - unvisited; 1 - pending; 2 - done

  traverseBB(&F.getEntryBlock(), visitStatus);

  return true;
}

}

INITIALIZE_PASS_BEGIN(RendezVousPointPass, "rendez-vous-point",
		      "Analyze static merging points.", false, false)
INITIALIZE_PASS_END(RendezVousPointPass, "rendez-vous-point",
		      "Analyze static merging points.", false, false)
