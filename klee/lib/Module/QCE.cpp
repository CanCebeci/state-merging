#include "klee/Internal/Module/QCE.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Debug.h"
#include "llvm/Instruction.h"
#include "llvm/BasicBlock.h"
#include "llvm/Function.h"
#include "llvm/Metadata.h"
#include "llvm/Constants.h"

using namespace llvm;
using namespace klee;

MDNode *HotValue::toMDNode(LLVMContext &Ctx, APInt useCount) const {
  assert(isVal() ? offset == 0 && size == 0 : size != 0);
  Value *Args[5] = {
    ConstantInt::get(Ctx, useCount),
    ConstantInt::get(Ctx, APInt(1, kind)),
    getValue(),
    ConstantInt::get(Ctx, APInt(16, offset)),
    ConstantInt::get(Ctx, APInt(16, size))
  };
  return MDNode::get(Ctx, Args, 5);
}

std::pair<HotValue, APInt> HotValue::fromMDNode(const MDNode *MD) {
  return std::make_pair(
      HotValue(
          HotValueKind(cast<ConstantInt>(MD->getOperand(1))->getZExtValue()),
          MD->getOperand(2),
          cast<ConstantInt>(MD->getOperand(3))->getZExtValue(),
          cast<ConstantInt>(MD->getOperand(4))->getZExtValue()),
      cast<ConstantInt>(MD->getOperand(0))->getValue());
}

void HotValue::print(llvm::raw_ostream &ostr) const {
  Function *function = NULL;

  ostr << (isPtr() ? "*  " : "   ");
  if (Instruction *I = dyn_cast<Instruction>(getValue())) {
    Instruction *tmp = I->clone();
    if (I->hasName())
      tmp->setName(I->getName());
    tmp->setMetadata("qce", NULL);
    tmp->print(ostr);
    delete tmp;
    function = I->getParent()->getParent();
  } else if (Argument *A = dyn_cast<Argument>(getValue())) {
    A->print(ostr);
    function = A->getParent();
  } else {
    getValue()->print(ostr);
  }

  if (offset)
    ostr << " (offset:" << offset << ")";

  if (size)
    ostr << " (size:" << size << ")";

  if (function && function->hasName())
    ostr << " (at " << function->getName() << ")";
}

void HotValue::dump() const {
  print(dbgs()); dbgs() << "\n";
}

