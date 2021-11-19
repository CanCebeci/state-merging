//===-- SpecialFunctionHandler.cpp ----------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Common.h"

#include "Memory.h"
#include "SpecialFunctionHandler.h"
#include "TimingSolver.h"

#include "klee/ExecutionState.h"
#include "klee/util/ExprPPrinter.h"

#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "klee/Internal/Module/KModule.h"

#include "klee/Executor.h"
#include "MemoryManager.h"

#include "llvm/Module.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Type.h"
#include "llvm/DerivedTypes.h"
#include "llvm/InstrTypes.h"
#if (LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR >= 7)
#include "llvm/LLVMContext.h"
#endif

#include <errno.h>
#include <stdarg.h>
#include <sys/syscall.h>

using namespace llvm;
using namespace klee;

/// \todo Almost all of the demands in this file should be replaced
/// with terminateState calls.

///

struct HandlerInfo {
  const char *name;
  SpecialFunctionHandler::Handler handler;
  bool doesNotReturn; /// Intrinsic terminates the process
  bool hasReturnValue; /// Intrinsic has a return value
  bool doNotOverride; /// Intrinsic should not be used if already defined
};

// FIXME: We are more or less committed to requiring an intrinsic
// library these days. We can move some of this stuff there,
// especially things like realloc which have complicated semantics
// w.r.t. forking. Among other things this makes delayed query
// dispatch easier to implement.
HandlerInfo handlerInfo[] = {
#define add(name, handler, ret) { name, \
                                  &SpecialFunctionHandler::handler, \
                                  false, ret, false }
#define addDNR(name, handler) { name, \
                                &SpecialFunctionHandler::handler, \
                                true, false, false }
  addDNR("__assert_rtn", handleAssertFail),
  addDNR("__assert_fail", handleAssertFail),
  addDNR("_assert", handleAssert),
  addDNR("abort", handleAbort),
  addDNR("klee_abort", handleAbort),
  addDNR("klee_silent_exit", handleSilentExit),  
  addDNR("klee_report_error", handleReportError),
  addDNR("klee_thread_terminate", handleThreadTerminate),
  addDNR("klee_process_terminate", handleProcessTerminate),

  add("calloc", handleCalloc, true),
  add("free", handleFree, false),
  add("klee_assume", handleAssume, false),
  add("klee_event", handleEvent, false),
  add("klee_check_memory_access", handleCheckMemoryAccess, false),
  add("klee_get_valuef", handleGetValue, true),
  add("klee_get_valued", handleGetValue, true),
  add("klee_get_valuel", handleGetValue, true),
  add("klee_get_valuell", handleGetValue, true),
  add("klee_get_value_i32", handleGetValue, true),
  add("klee_get_value_i64", handleGetValue, true),
  add("klee_define_fixed_object", handleDefineFixedObject, false),
  add("klee_get_obj_size", handleGetObjSize, true),
  add("klee_get_errno", handleGetErrno, true),
  add("klee_is_symbolic", handleIsSymbolic, true),
  add("klee_make_symbolic", handleMakeSymbolic, false),
  add("klee_mark_global", handleMarkGlobal, false),
  add("klee_merge", handleMerge, false),
  add("klee_prefer_cex", handlePreferCex, false),
  add("klee_print_expr", handlePrintExpr, false),
  add("klee_print_range", handlePrintRange, false),
  add("klee_set_forking", handleSetForking, false),
  add("klee_stack_trace", handleStackTrace, false),
  add("klee_make_shared", handleMakeShared, false),
  add("klee_get_context", handleGetContext, false),
  add("klee_get_wlist", handleGetWList, true),
  add("klee_thread_preempt", handleThreadPreempt, false),
  add("klee_thread_sleep", handleThreadSleep, false),
  add("klee_thread_notify", handleThreadNotify, false),
  add("klee_warning", handleWarning, false),
  add("klee_warning_once", handleWarningOnce, false),
  add("klee_alias_function", handleAliasFunction, false),

  add("klee_thread_create", handleThreadCreate, false),
  add("klee_process_fork", handleProcessFork, true),

  add("klee_branch", handleBranch, true),
  add("klee_fork", handleFork, true),

  add("klee_debug", handleDebug, false),

  add("klee_get_time", handleGetTime, true),
  add("klee_set_time", handleSetTime, false),

  add("klee_merge_disable", handleMergeDisable, false),
  add("klee_merge_blacklist", handleMergeBlacklist, false),
  add("klee_merge_blacklist_clear", handleMergeBlacklistClear, false),

  add("_klee_use_freq", handleUseFreq, false),
  add("_klee_rendez_vous", handleRendezVous, false),

  add("malloc", handleMalloc, true),
  add("realloc", handleRealloc, true),
  add("valloc", handleValloc, true),

  // operator delete[](void*)
  add("_ZdaPv", handleDeleteArray, false),
  // operator delete(void*)
  add("_ZdlPv", handleDelete, false),

  // operator new[](unsigned int)
  add("_Znaj", handleNewArray, true),
  // operator new(unsigned int)
  add("_Znwj", handleNew, true),
  add("_ZnamRKSt9nothrow_t", handleNew, true),

  // FIXME-64: This is wrong for 64-bit long...

  // operator new[](unsigned long)
  add("_Znam", handleNewArray, true),
  // operator new(unsigned long)
  add("_Znwm", handleNew, true),
  add("_ZnwmRKSt9nothrow_t", handleNew, true),

  add("syscall", handleSyscall, true),

  // loop instrumentation
  add("_klee_loop_iter", handleLoopIter, false),
  add("_klee_loop_exit", handleLoopExit, false),

#undef addDNR
#undef add  
};

SpecialFunctionHandler::SpecialFunctionHandler(Executor &_executor) 
  : executor(_executor) {}


void SpecialFunctionHandler::prepare() {
  unsigned N = sizeof(handlerInfo)/sizeof(handlerInfo[0]);

  for (unsigned i=0; i<N; ++i) {
    HandlerInfo &hi = handlerInfo[i];
    Function *f = executor.kmodule->module->getFunction(hi.name);
    
    // No need to create if the function doesn't exist, since it cannot
    // be called in that case.
  
    if (f && (!hi.doNotOverride || f->isDeclaration())) {
      // Make sure NoReturn attribute is set, for optimization and
      // coverage counting.
      if (hi.doesNotReturn)
        f->addFnAttr(Attribute::NoReturn);

      // Change to a declaration since we handle internally (simplifies
      // module and allows deleting dead code).
      if (!f->isDeclaration())
        f->deleteBody();
    }
  }
}

void SpecialFunctionHandler::bind() {
  unsigned N = sizeof(handlerInfo)/sizeof(handlerInfo[0]);

  for (unsigned i=0; i<N; ++i) {
    HandlerInfo &hi = handlerInfo[i];
    Function *f = executor.kmodule->module->getFunction(hi.name);
    
    if (f && (!hi.doNotOverride || f->isDeclaration()))
      handlers[f] = std::make_pair(hi.handler, hi.hasReturnValue);
  }
}


bool SpecialFunctionHandler::handle(ExecutionState &state, 
                                    Function *f,
                                    KInstruction *target,
                                    std::vector< ref<Expr> > &arguments) {
  handlers_ty::iterator it = handlers.find(f);
  if (it != handlers.end()) {    
    Handler h = it->second.first;
    bool hasReturnValue = it->second.second;
     // FIXME: Check this... add test?
    if (!hasReturnValue && !target->inst->use_empty()) {
      executor.terminateStateOnExecError(state, 
                                         "expected return value from void special function");
    } else {
      (this->*h)(state, target, arguments);
    }
    return true;
  } else {
    return false;
  }
}

void SpecialFunctionHandler::processMemoryLocation(ExecutionState &state,
    ref<Expr> address, ref<Expr> size,
    const std::string &name, resolutions_ty &resList) {
  Executor::ExactResolutionList rl;
  executor.resolveExact(state, address, rl, name);

  for (Executor::ExactResolutionList::iterator it = rl.begin(),
         ie = rl.end(); it != ie; ++it) {
    const MemoryObject *mo = it->first.first;
    const ObjectState *os = it->first.second;
    ExecutionState *s = it->second;

    // FIXME: Type coercion should be done consistently somewhere.
    bool res;
    bool success =
      executor.solver->mustBeTrue(*s,
                                  EqExpr::create(ZExtExpr::create(size,
                                                                  Context::get().getPointerWidth()),
                                                 mo->getSizeExpr()),
                                  res);
    assert(success && "FIXME: Unhandled solver failure");

    if (res) {
      resList.push_back(std::make_pair(std::make_pair(mo, os), s));
    } else {
      executor.terminateStateOnError(*s,
                                     "wrong size given to memory operation",
                                     "user.err");
    }
  }
}

bool SpecialFunctionHandler::writeConcreteValue(KInstruction *target,
        ExecutionState &state,
        ref<Expr> address, uint64_t value, Expr::Width width) {
  ObjectPair op;

  if (!state.addressSpace().resolveOne(cast<ConstantExpr>(address), op)) {
    executor.terminateStateOnError(state, "invalid pointer for writing concrete value into", "user.err");
    return false;
  }

  ObjectState *os = state.addressSpace().getWriteable(op.first, op.second);

  ref<Expr> offset = op.first->getOffsetExpr(address);
  ref<ConstantExpr> valueExpr = ConstantExpr::create(value, width);
  executor.verifyQceMap(state);
  executor.updateQceMemoryValue(state, op.first, os, offset, valueExpr, target);
  os->write(offset, valueExpr);
  //os->write(op.first->getOffsetExpr(address), ConstantExpr::create(value, width));
  executor.verifyQceMap(state);

  return true;
}

/****/

// reads a concrete string from memory
std::string 
SpecialFunctionHandler::readStringAtAddress(ExecutionState &state, 
                                            ref<Expr> addressExpr) {
  ObjectPair op;
  addressExpr = executor.toUnique(state, addressExpr);
  if (!isa<ConstantExpr>(addressExpr))
    return std::string("<KLEE<symaddr>>");

  ref<ConstantExpr> address = cast<ConstantExpr>(addressExpr);
  if (!state.addressSpace().resolveOne(address, op))
    return std::string("<KLEE<multires>>");

  const MemoryObject *mo = op.first;
  const ObjectState *os = op.second;

  char *buf = new char[mo->size];
  unsigned ioffset = 0;

  ref<Expr> offset_expr = SubExpr::create(address, op.first->getBaseExpr());
  if (isa<ConstantExpr>(offset_expr)) {
	  ref<ConstantExpr> value = cast<ConstantExpr>(offset_expr.get());
  	  ioffset = value.get()->getZExtValue();
  } else
	  return std::string("<KLEE<invalstring>>");

  assert(ioffset < mo->size);

  unsigned i;
  for (i = 0; i < mo->size - ioffset - 1; i++) {
    ref<Expr> cur = os->read8(i + ioffset);
    cur = executor.toUnique(state, cur);
    if (!isa<ConstantExpr>(cur)) //XXX: Should actually concretize the value...
           return std::string("<KLEE<symchar>>");
    buf[i] = cast<ConstantExpr>(cur)->getZExtValue(8);
    if(buf[i] == 0)
      break;
  }
  buf[i] = 0;
  
  std::string result(buf);
  delete[] buf;
  return result;
}

/****/

void SpecialFunctionHandler::handleAbort(ExecutionState &state,
                           KInstruction *target,
                           std::vector<ref<Expr> > &arguments) {
  assert(arguments.size()==0 && "invalid number of arguments to abort");


  executor.terminateStateOnError(state, "abort failure", "abort.err");
}

void SpecialFunctionHandler::handleSilentExit(ExecutionState &state,
                                              KInstruction *target,
                                              std::vector<ref<Expr> > &arguments) {
  assert(arguments.size()==1 && "invalid number of arguments to exit");

  executor.terminateState(state, true);
}

void SpecialFunctionHandler::handleAliasFunction(ExecutionState &state,
						 KInstruction *target,
						 std::vector<ref<Expr> > &arguments) {
  assert(arguments.size()==2 && 
         "invalid number of arguments to klee_alias_function");
  std::string old_fn = readStringAtAddress(state, arguments[0]);
  std::string new_fn = readStringAtAddress(state, arguments[1]);
  //std::cerr << "Replacing " << old_fn << "() with " << new_fn << "()\n";
  if (old_fn == new_fn)
    state.removeFnAlias(old_fn);
  else state.addFnAlias(old_fn, new_fn);
}

void SpecialFunctionHandler::handleAssert(ExecutionState &state,
                                          KInstruction *target,
                                          std::vector<ref<Expr> > &arguments) {
  assert(arguments.size()==3 && "invalid number of arguments to _assert");  
  

  executor.terminateStateOnError(state,
                                 "ASSERTION FAIL: " + readStringAtAddress(state, arguments[0]),
                                 "assert.err");
}

void SpecialFunctionHandler::handleAssertFail(ExecutionState &state,
                                              KInstruction *target,
                                              std::vector<ref<Expr> > &arguments) {
  assert(arguments.size()==4 && "invalid number of arguments to __assert_fail");
  

  executor.terminateStateOnError(state,
                                 "ASSERTION FAIL: " + readStringAtAddress(state, arguments[0]),
                                 "assert.err");
}

void SpecialFunctionHandler::handleReportError(ExecutionState &state,
                                               KInstruction *target,
                                               std::vector<ref<Expr> > &arguments) {
  assert(arguments.size()==4 && "invalid number of arguments to klee_report_error");
  
  // arguments[0], arguments[1] are file, line
  
  executor.terminateStateOnError(state,
                                 readStringAtAddress(state, arguments[2]),
                                 readStringAtAddress(state, arguments[3]).c_str());
}

void SpecialFunctionHandler::handleMerge(ExecutionState &state,
                           KInstruction *target,
                           std::vector<ref<Expr> > &arguments) {
  // nop
}

void SpecialFunctionHandler::handleNew(ExecutionState &state,
                         KInstruction *target,
                         std::vector<ref<Expr> > &arguments) {
  // XXX should type check args
  assert(arguments.size()>=1 && "invalid number of arguments to new");

  executor.executeAlloc(state, arguments[0], false, target);
}

void SpecialFunctionHandler::handleDelete(ExecutionState &state,
                            KInstruction *target,
                            std::vector<ref<Expr> > &arguments) {
  // FIXME: Should check proper pairing with allocation type (malloc/free,
  // new/delete, new[]/delete[]).

  // XXX should type check args
  assert(arguments.size()==1 && "invalid number of arguments to delete");
  executor.executeFree(state, arguments[0]);
}

void SpecialFunctionHandler::handleNewArray(ExecutionState &state,
                              KInstruction *target,
                              std::vector<ref<Expr> > &arguments) {
  // XXX should type check args
  assert(arguments.size()==1 && "invalid number of arguments to new[]");
  executor.executeAlloc(state, arguments[0], false, target);
}

void SpecialFunctionHandler::handleDeleteArray(ExecutionState &state,
                                 KInstruction *target,
                                 std::vector<ref<Expr> > &arguments) {
  // XXX should type check args
  assert(arguments.size()==1 && "invalid number of arguments to delete[]");
  executor.executeFree(state, arguments[0]);
}

void SpecialFunctionHandler::handleMalloc(ExecutionState &state,
                                  KInstruction *target,
                                  std::vector<ref<Expr> > &arguments) {
  // XXX should type check args
  assert(arguments.size()==1 && "invalid number of arguments to malloc");
  executor.executeAlloc(state, arguments[0], false, target);
}


void SpecialFunctionHandler::handleValloc(ExecutionState &state, 
					  KInstruction *target, 
					  std::vector<ref<Expr> > &arguments) {
  
  // XXX ignoring for now the "multiple of page size " requirement 
  //- executing the regular alloc
  // XXX should type check args
  assert(arguments.size() == 1 && "invalid number of arguments to valloc");
  executor.executeAlloc(state, arguments[0], false, target);
}

void SpecialFunctionHandler::handleAssume(ExecutionState &state,
                            KInstruction *target,
                            std::vector<ref<Expr> > &arguments) {
  assert(arguments.size()==1 && "invalid number of arguments to klee_assume");
  
  ref<Expr> e = arguments[0];
  
  if (e->getWidth() != Expr::Bool)
    e = NeExpr::create(e, ConstantExpr::create(0, e->getWidth()));
  
  bool res;
  bool success = executor.solver->mustBeFalse(state, e, res);
  assert(success && "FIXME: Unhandled solver failure");
  if (res) {
    executor.terminateStateOnError(state, 
                                   "invalid klee_assume call (provably false)",
                                   "user.err");
  } else {
    executor.addConstraint(state, e);
  }
}

void SpecialFunctionHandler::handleIsSymbolic(ExecutionState &state,
                                KInstruction *target,
                                std::vector<ref<Expr> > &arguments) {
  assert(arguments.size()==1 && "invalid number of arguments to klee_is_symbolic");

  //arguments[0]->print(std::cerr);

  executor.bindLocal(target, state, 
                     ConstantExpr::create(!isa<ConstantExpr>(arguments[0]),
                                          Expr::Int32));
}

void SpecialFunctionHandler::handlePreferCex(ExecutionState &state,
                                             KInstruction *target,
                                             std::vector<ref<Expr> > &arguments) {
  assert(arguments.size()==2 &&
         "invalid number of arguments to klee_prefex_cex");

  ref<Expr> cond = arguments[1];
  if (cond->getWidth() != Expr::Bool)
    cond = NeExpr::create(cond, ConstantExpr::alloc(0, cond->getWidth()));

  Executor::ExactResolutionList rl;
  executor.resolveExact(state, arguments[0], rl, "prefex_cex");
  
  assert(rl.size() == 1 &&
         "prefer_cex target must resolve to precisely one object");

  rl[0].first.first->cexPreferences.push_back(cond);
}

void SpecialFunctionHandler::handlePrintExpr(ExecutionState &state,
                                  KInstruction *target,
                                  std::vector<ref<Expr> > &arguments) {
  assert(arguments.size()==2 &&
         "invalid number of arguments to klee_print_expr");

  std::string msg_str = readStringAtAddress(state, arguments[0]);
  std::cerr << msg_str << ":" << arguments[1] << std::endl;

  for (ConstraintManager::constraint_iterator it = state.constraints().begin();
      it != state.constraints().end(); it++) {
    std::cerr << *it << std::endl;
  }
}

void SpecialFunctionHandler::handleSetForking(ExecutionState &state,
                                              KInstruction *target,
                                              std::vector<ref<Expr> > &arguments) {
  assert(arguments.size()==1 &&
         "invalid number of arguments to klee_set_forking");
  ref<Expr> value = executor.toUnique(state, arguments[0]);
  
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(value)) {
    state.forkDisabled = CE->isZero();
  } else {
    executor.terminateStateOnError(state, 
                                   "klee_set_forking requires a constant arg",
                                   "user.err");
  }
}

void SpecialFunctionHandler::handleStackTrace(ExecutionState &state,
                                              KInstruction *target,
                                              std::vector<ref<Expr> > &arguments) {
  state.getStackTrace().dump(std::cout);
}

void SpecialFunctionHandler::handleWarning(ExecutionState &state,
                                           KInstruction *target,
                                           std::vector<ref<Expr> > &arguments) {
  assert(arguments.size()==1 && "invalid number of arguments to klee_warning");

  std::string msg_str = readStringAtAddress(state, arguments[0]);
  klee_warning("%s: %s", state.stack().back().kf->function->getName().data(),
               msg_str.c_str());
}

void SpecialFunctionHandler::handleDebug(ExecutionState &state,
                                           KInstruction *target,
                                           std::vector<ref<Expr> > &arguments) {
  assert(arguments.size() >= 1 && "invalid number of arguments to klee_debug");

  std::string formatStr = readStringAtAddress(state, arguments[0]);

  // XXX Ugly hack, need to use libffi here... Ashamed of myself

  if (arguments.size() == 2 && arguments[1]->getWidth() == sizeof(long)*8) {
    // Special case for displaying strings

    std::string paramStr = readStringAtAddress(state, arguments[1]);

    fprintf(stderr, formatStr.c_str(), paramStr.c_str());
    return;
  }

  std::vector<int> args;

  for (unsigned int i = 1; i < arguments.size(); i++) {
    if (!isa<ConstantExpr>(arguments[i])) {
      fprintf(stderr, "%s: %s\n", formatStr.c_str(), "<nonconst args>");
      return;
    }

    ref<ConstantExpr> arg = cast<ConstantExpr>(arguments[i]);

    if (arg->getWidth() != sizeof(int)*8) {
      fprintf(stderr, "%s: %s\n", formatStr.c_str(), "<non-32-bit args>");
      return;
    }

    args.push_back((int)arg->getZExtValue());
  }

  switch (args.size()) {
  case 0:
    fprintf(stderr, "%s", formatStr.c_str());
    break;
  case 1:
    fprintf(stderr, formatStr.c_str(), args[0]);
    break;
  case 2:
    fprintf(stderr, formatStr.c_str(), args[0], args[1]);
    break;
  case 3:
    fprintf(stderr, formatStr.c_str(), args[0], args[1], args[2]);
    break;
  default:
    executor.terminateStateOnError(state, "klee_debug allows up to 3 arguments", "user.err");
    return;
  }
}

void SpecialFunctionHandler::handleWarningOnce(ExecutionState &state,
                                               KInstruction *target,
                                               std::vector<ref<Expr> > &arguments) {
  assert(arguments.size()==1 &&
         "invalid number of arguments to klee_warning_once");

  std::string msg_str = readStringAtAddress(state, arguments[0]);
  klee_warning_once(0, "%s: %s", state.stack().back().kf->function->getName().data(),
                    msg_str.c_str());
}

void SpecialFunctionHandler::handlePrintRange(ExecutionState &state,
                                  KInstruction *target,
                                  std::vector<ref<Expr> > &arguments) {
  assert(arguments.size()==2 &&
         "invalid number of arguments to klee_print_range");

  std::string msg_str = readStringAtAddress(state, arguments[0]);
  std::cerr << msg_str << ":" << arguments[1];
  if (!isa<ConstantExpr>(arguments[1])) {
    // FIXME: Pull into a unique value method?
    ref<ConstantExpr> value;
    bool success = executor.solver->getValue(state, arguments[1], value);
    assert(success && "FIXME: Unhandled solver failure");
    bool res;
    success = executor.solver->mustBeTrue(state, 
                                          EqExpr::create(arguments[1], value), 
                                          res);
    assert(success && "FIXME: Unhandled solver failure");
    if (res) {
      std::cerr << " == " << value;
    } else { 
      std::cerr << " ~= " << value;
      std::pair< ref<Expr>, ref<Expr> > res =
        executor.solver->getRange(state, arguments[1]);
      std::cerr << " (in [" << res.first << ", " << res.second <<"])";
    }
  }
  std::cerr << "\n";
}

void SpecialFunctionHandler::handleGetObjSize(ExecutionState &state,
                                  KInstruction *target,
                                  std::vector<ref<Expr> > &arguments) {
  // XXX should type check args
  assert(arguments.size()==1 &&
         "invalid number of arguments to klee_get_obj_size");
  Executor::ExactResolutionList rl;
  executor.resolveExact(state, arguments[0], rl, "klee_get_obj_size");
  for (Executor::ExactResolutionList::iterator it = rl.begin(), 
         ie = rl.end(); it != ie; ++it) {
    executor.bindLocal(target, *it->second, 
                       ConstantExpr::create(it->first.first->size, Expr::Int32));
  }
}

void SpecialFunctionHandler::handleGetErrno(ExecutionState &state,
                                            KInstruction *target,
                                            std::vector<ref<Expr> > &arguments) {
  // XXX should type check args
  assert(arguments.size()==0 &&
         "invalid number of arguments to klee_get_obj_size");
  executor.bindLocal(target, state,
                     ConstantExpr::create(errno, Expr::Int32));
}

void SpecialFunctionHandler::handleCalloc(ExecutionState &state,
                            KInstruction *target,
                            std::vector<ref<Expr> > &arguments) {
  // XXX should type check args
  assert(arguments.size()==2 &&
         "invalid number of arguments to calloc");

  ref<Expr> size = MulExpr::create(arguments[0],
                                   arguments[1]);
  executor.executeAlloc(state, size, false, target, true);
}

void SpecialFunctionHandler::handleRealloc(ExecutionState &state,
                            KInstruction *target,
                            std::vector<ref<Expr> > &arguments) {
  // XXX should type check args
  assert(arguments.size()==2 &&
         "invalid number of arguments to realloc");
  ref<Expr> address = arguments[0];
  ref<Expr> size = arguments[1];

  Executor::StatePair zeroSize = executor.fork(state, 
                                               Expr::createIsZero(size), 
                                               true, KLEE_FORK_INTERNAL);
  
  if (zeroSize.first) { // size == 0
    executor.executeFree(*zeroSize.first, address, target);   
  }
  if (zeroSize.second) { // size != 0
    Executor::StatePair zeroPointer = executor.fork(*zeroSize.second, 
                                                    Expr::createIsZero(address), 
                                                    true, KLEE_FORK_INTERNAL);
    
    if (zeroPointer.first) { // address == 0
      executor.executeAlloc(*zeroPointer.first, size, false, target);
    } 
    if (zeroPointer.second) { // address != 0
      Executor::ExactResolutionList rl;
      executor.resolveExact(*zeroPointer.second, address, rl, "realloc");
      
      for (Executor::ExactResolutionList::iterator it = rl.begin(), 
             ie = rl.end(); it != ie; ++it) {
        executor.executeAlloc(*it->second, size, false, target, false, 
                              it->first.second);
      }
    }
  }
}

void SpecialFunctionHandler::handleFree(ExecutionState &state,
                          KInstruction *target,
                          std::vector<ref<Expr> > &arguments) {
  // XXX should type check args
  assert(arguments.size()==1 &&
         "invalid number of arguments to free");
  executor.executeFree(state, arguments[0]);
}

void SpecialFunctionHandler::handleMakeShared(ExecutionState &state,
                          KInstruction *target,
                          std::vector<ref<Expr> > &arguments) {

  assert(arguments.size() == 2 &&
        "invalid number of arguments to klee_make_shared");

  resolutions_ty resList;

  processMemoryLocation(state, arguments[0], arguments[1], "make_shared", resList);

  for (resolutions_ty::iterator it = resList.begin(); it != resList.end();
      it++) {
    const MemoryObject *mo = it->first.first;
    const ObjectState *os = it->first.second;
    ExecutionState *s = it->second;

    if (mo->isLocal) {
      executor.terminateStateOnError(*s,
                                     "cannot share local object",
                                     "user.err");
      continue;
    }

    unsigned int bindCount = 0;
    for (ExecutionState::processes_ty::iterator pit = s->processes.begin();
        pit != s->processes.end(); pit++) {
      if (pit->second.addressSpace.findObject(mo) != NULL)
        bindCount++;
    }

    if (bindCount != 1) {
      executor.terminateStateOnError(*s, "cannot shared already forked object",
           "user.err");
      continue;
    }

    ObjectState *newOS = state.addressSpace().getWriteable(mo, os);
    newOS->isShared = true;

    // Now bind this object in the other address spaces
    for (ExecutionState::processes_ty::iterator pit = s->processes.begin();
        pit != s->processes.end(); pit++) {
      if (pit == s->crtProcessIt)
        continue; // Skip the current process

      pit->second.addressSpace.bindSharedObject(mo, newOS);
    }
  }
}

void SpecialFunctionHandler::handleGetContext(ExecutionState &state,
                          KInstruction *target,
                          std::vector<ref<Expr> > &arguments) {
  assert(arguments.size() == 2 &&
      "invalid number of arguments to klee_get_context");

  ref<Expr> tidAddr = executor.toUnique(state, arguments[0]);
  ref<Expr> pidAddr = executor.toUnique(state, arguments[1]);

  if (!isa<ConstantExpr>(tidAddr) || !isa<ConstantExpr>(pidAddr)) {
    executor.terminateStateOnError(state,
                                   "klee_get_context requires constant args",
                                   "user.err");
    return;
  }

  if (!tidAddr->isZero()) {
    if (!writeConcreteValue(target, state, tidAddr, state.crtThread().getTid(),
        executor.getWidthForLLVMType(Type::getInt64Ty(getGlobalContext()))))
      return;
  }

  if (!pidAddr->isZero()) {
    if (!writeConcreteValue(target, state, pidAddr, state.crtProcess().pid,
        executor.getWidthForLLVMType(Type::getInt32Ty(getGlobalContext()))))
      return;
  }
}

void SpecialFunctionHandler::handleGetTime(ExecutionState &state,
                    KInstruction *target,
                    std::vector<ref<Expr> > &arguments) {
  assert(arguments.empty() && "invalid number of arguments to klee_get_time");

  executor.bindLocal(target, state, ConstantExpr::create(state.stateTime,
      executor.getWidthForLLVMType(target->inst->getType())));
}

void SpecialFunctionHandler::handleSetTime(ExecutionState &state,
                    KInstruction *target,
                    std::vector<ref<Expr> > &arguments) {
  assert(arguments.size() == 1 && "invalid number of arguments to klee_set_time");

  if (!isa<ConstantExpr>(arguments[0])) {
    executor.terminateStateOnError(state, "klee_set_time requires a constant argument", "user.err");
    return;
  }

  state.stateTime = cast<ConstantExpr>(arguments[0])->getZExtValue();
}

void SpecialFunctionHandler::handleGetWList(ExecutionState &state,
                    KInstruction *target,
                    std::vector<ref<Expr> > &arguments) {
  assert(arguments.empty() && "invalid number of arguments to klee_get_wlist");

  wlist_id_t id = state.getWaitingList();

  executor.bindLocal(target, state, ConstantExpr::create(id,
      executor.getWidthForLLVMType(target->inst->getType())));
}

void SpecialFunctionHandler::handleThreadPreempt(ExecutionState &state,
                    KInstruction *target,
                    std::vector<ref<Expr> > &arguments) {
  assert(arguments.size() == 1 && "invalid number of arguments to klee_thread_preempt");

  if (!isa<ConstantExpr>(arguments[0])) {
    executor.terminateStateOnError(state, "klee_thread_preempt", "user.err");
  }

  executor.schedule(state, !arguments[0]->isZero());
}

void SpecialFunctionHandler::handleThreadSleep(ExecutionState &state,
                    KInstruction *target,
                    std::vector<ref<Expr> > &arguments) {

  assert(arguments.size() == 1 && "invalid number of arguments to klee_thread_sleep");

  ref<Expr> wlistExpr = executor.toUnique(state, arguments[0]);

  if (!isa<ConstantExpr>(wlistExpr)) {
    executor.terminateStateOnError(state, "klee_thread_sleep", "user.err");
    return;
  }

  state.sleepThread(cast<ConstantExpr>(wlistExpr)->getZExtValue());
  executor.schedule(state, false);
}

void SpecialFunctionHandler::handleThreadNotify(ExecutionState &state,
                    KInstruction *target,
                    std::vector<ref<Expr> > &arguments) {
  assert(arguments.size() == 2 && "invalid number of arguments to klee_thread_notify");

  ref<Expr> wlist = executor.toUnique(state, arguments[0]);
  ref<Expr> all = executor.toUnique(state, arguments[1]);

  if (!isa<ConstantExpr>(wlist) || !isa<ConstantExpr>(all)) {
    executor.terminateStateOnError(state, "klee_thread_notify", "user.err");
    return;
  }

  if (all->isZero()) {
    executor.executeThreadNotifyOne(state, cast<ConstantExpr>(wlist)->getZExtValue());
  } else {
    // It's simple enough such that it can be handled by the state class itself
    state.notifyAll(cast<ConstantExpr>(wlist)->getZExtValue());
  }
}

void SpecialFunctionHandler::handleThreadCreate(ExecutionState &state,
                    KInstruction *target,
                    std::vector<ref<Expr> > &arguments) {
  assert(arguments.size() == 3 && "invalid number of arguments to klee_thread_create");

  ref<Expr> tid = executor.toUnique(state, arguments[0]);

  if (!isa<ConstantExpr>(tid)) {
    executor.terminateStateOnError(state, "klee_thread_create", "user.err");
    return;
  }

  executor.executeThreadCreate(state, cast<ConstantExpr>(tid)->getZExtValue(),
      arguments[1], arguments[2]);
}

void SpecialFunctionHandler::handleThreadTerminate(ExecutionState &state,
                    KInstruction *target,
                    std::vector<ref<Expr> > &arguments) {
  assert(arguments.empty() && "invalid number of arguments to klee_thread_terminate");

  executor.executeThreadExit(state);
}

void SpecialFunctionHandler::handleBranch(ExecutionState &state,
                    KInstruction *target,
                    std::vector<ref<Expr> > &arguments) {
  assert(arguments.size() == 2 && "invalid number of arguments to klee_branch");

  if (!isa<ConstantExpr>(arguments[1])) {
    executor.terminateStateOnError(state, "symbolic reason in klee_branch", "user.err");
    return;
  }

  // We must check that klee_branch is correctly used - the use case of the
  // return value must be a comparison instruction
  Instruction *inst = target->inst;

  if (!inst->hasOneUse()) {
    executor.terminateStateOnError(state, "klee_branch must be used once", "user.err");
    return;
  }

  User *user = *inst->use_begin();

  if (!isa<CmpInst>(user) || inst->getParent() != cast<Instruction>(user)->getParent()) {
    executor.terminateStateOnError(state, "klee_branch must be used together with a comparison", "user.err");
    return;
  }

  // We just bind the result to the first argument, and mark the reason

  state.crtForkReason = cast<ConstantExpr>(arguments[1])->getZExtValue();
  state.crtSpecialFork = cast<Instruction>(user);

  executor.bindLocal(target, state, arguments[0]);
}

void SpecialFunctionHandler::handleFork(ExecutionState &state,
                    KInstruction *target,
                    std::vector<ref<Expr> > &arguments) {
  assert(arguments.size() == 1 && "invalid number of arguments to klee_fork");

  if (!isa<ConstantExpr>(arguments[0])) {
    executor.terminateStateOnError(state, "symbolic reason in klee_fork", "user.err");
    return;
  }

  int reason = cast<ConstantExpr>(arguments[0])->getZExtValue();

  executor.executeFork(state, target, reason);
}

void SpecialFunctionHandler::handleProcessFork(ExecutionState &state,
                    KInstruction *target,
                    std::vector<ref<Expr> > &arguments) {
  assert(arguments.size() == 1 && "invalid number of arguments to klee_process_fork");

  ref<Expr> pid = executor.toUnique(state, arguments[0]);

  if (!isa<ConstantExpr>(pid)) {
    executor.terminateStateOnError(state, "klee_process_fork", "user.err");
    return;
  }

  executor.executeProcessFork(state, target,
      cast<ConstantExpr>(pid)->getZExtValue());
}

void SpecialFunctionHandler::handleProcessTerminate(ExecutionState &state,
                    KInstruction *target,
                    std::vector<ref<Expr> > &arguments) {
  assert(arguments.empty() && "invalid number of arguments to klee_process_terminate");

  executor.executeProcessExit(state);
}



void SpecialFunctionHandler::handleCheckMemoryAccess(ExecutionState &state,
                                                     KInstruction *target,
                                                     std::vector<ref<Expr> > 
                                                       &arguments) {
  assert(arguments.size()==2 &&
         "invalid number of arguments to klee_check_memory_access");

  ref<Expr> address = executor.toUnique(state, arguments[0]);
  ref<Expr> size = executor.toUnique(state, arguments[1]);
  if (!isa<ConstantExpr>(address) || !isa<ConstantExpr>(size)) {
    executor.terminateStateOnError(state, 
                                   "check_memory_access requires constant args",
                                   "user.err");
  } else {
    ObjectPair op;

    if (!state.addressSpace().resolveOne(cast<ConstantExpr>(address), op)) {
      executor.terminateStateOnError(state,
                                     "check_memory_access: memory error",
                                     "ptr.err",
                                     executor.getAddressInfo(state, address));
    } else {
      ref<Expr> chk = 
        op.first->getBoundsCheckPointer(address, 
                                        cast<ConstantExpr>(size)->getZExtValue());
      if (!chk->isTrue()) {
        executor.terminateStateOnError(state,
                                       "check_memory_access: memory error",
                                       "ptr.err",
                                       executor.getAddressInfo(state, address));
      }
    }
  }
}

void SpecialFunctionHandler::handleGetValue(ExecutionState &state,
                                            KInstruction *target,
                                            std::vector<ref<Expr> > &arguments) {
  assert(arguments.size()==1 &&
         "invalid number of arguments to klee_get_value");

  executor.executeGetValue(state, arguments[0], target);
}

void SpecialFunctionHandler::handleDefineFixedObject(ExecutionState &state,
                                                     KInstruction *target,
                                                     std::vector<ref<Expr> > &arguments) {
  assert(arguments.size()==2 &&
         "invalid number of arguments to klee_define_fixed_object");
  assert(isa<ConstantExpr>(arguments[0]) &&
         "expect constant address argument to klee_define_fixed_object");
  assert(isa<ConstantExpr>(arguments[1]) &&
         "expect constant size argument to klee_define_fixed_object");
  
  uint64_t address = cast<ConstantExpr>(arguments[0])->getZExtValue();
  uint64_t size = cast<ConstantExpr>(arguments[1])->getZExtValue();
  MemoryObject *mo = executor.memory->allocateFixed(address, size, state.prevPC()->inst, 0);
  executor.bindObjectInState(state, mo, false);
  mo->isUserSpecified = true; // XXX hack;
}

void SpecialFunctionHandler::handleMakeSymbolic(ExecutionState &state,
                                                KInstruction *target,
                                                std::vector<ref<Expr> > &arguments) {
  std::string name;

  // FIXME: For backwards compatibility, we should eventually enforce the
  // correct arguments.
  if (arguments.size() == 2) {
    name = "unnamed";
  } else {
    // FIXME: Should be a user.err, not an assert.
    assert(arguments.size()==3 &&
           "invalid number of arguments to klee_make_symbolic");  
    name = readStringAtAddress(state, arguments[2]);
  }

  resolutions_ty resList;

  processMemoryLocation(state, arguments[0], arguments[1], "make_symbolic", resList);

  for (resolutions_ty::iterator it = resList.begin(); it != resList.end();
      it++) {
    const MemoryObject *mo = it->first.first;
    const ObjectState *os = it->first.second;
    ExecutionState *s = it->second;

    mo->setName(name);

    if (os->readOnly) {
      executor.terminateStateOnError(*s,
                                     "cannot make readonly object symbolic",
                                     "user.err");
    } else {
      executor.updateQceMapOnFree(state, mo, target);
      executor.executeMakeSymbolic(*s, mo, os->isShared);
    }
  }
}

void SpecialFunctionHandler::handleEvent(ExecutionState &state,
                                                KInstruction *target,
                                                std::vector<ref<Expr> > &arguments) {
  assert(arguments.size() == 2 && "invalid number of arguments to klee_event");

  if (!isa<ConstantExpr>(arguments[0]) || !isa<ConstantExpr>(arguments[1])) {
    executor.terminateStateOnError(state, "klee_event requires a constant arg", "user.err");
    return;
  }

  ref<ConstantExpr> type = cast<ConstantExpr>(arguments[0]);
  ref<ConstantExpr> value = cast<ConstantExpr>(arguments[1]);

  executor.executeEvent(state, (unsigned int)type->getZExtValue(),
      (long int)value->getZExtValue());
}

void SpecialFunctionHandler::handleMarkGlobal(ExecutionState &state,
                                              KInstruction *target,
                                              std::vector<ref<Expr> > &arguments) {
  assert(arguments.size()==1 &&
         "invalid number of arguments to klee_mark_global");  

  Executor::ExactResolutionList rl;
  executor.resolveExact(state, arguments[0], rl, "mark_global");
  
  for (Executor::ExactResolutionList::iterator it = rl.begin(), 
         ie = rl.end(); it != ie; ++it) {
    const MemoryObject *mo = it->first.first;
    assert(!mo->isLocal);
    mo->isGlobal = true;
  }
}

void SpecialFunctionHandler::handleSyscall(ExecutionState &state,
                            KInstruction *target,
                            std::vector<ref<Expr> > &arguments) {
  assert(arguments.size() >= 1 && "invalid number of arguments to syscall");

  if (ConstantExpr *syscallNo = dyn_cast<ConstantExpr>(arguments[0])) {
    switch(syscallNo->getZExtValue()) {
    /* Signal syscalls */
    case SYS_rt_sigaction:
    case SYS_sigaltstack:
    case SYS_signalfd:
    case SYS_signalfd4:
    case SYS_rt_sigpending:
    case SYS_rt_sigprocmask:
    case SYS_rt_sigreturn:
    case SYS_rt_sigsuspend:
      CLOUD9_DEBUG("Blocked syscall " << syscallNo->getZExtValue());
      executor.bindLocal(target, state, ConstantExpr::create(0,
            executor.getWidthForLLVMType(target->inst->getType())));
      break;
    default:
      executor.callUnmodelledFunction(state, target,
          executor.kmodule->module->getFunction("syscall"),
          arguments);
      break;
    }
  } else {
    executor.terminateStateOnError(state, "syscall requires a concrete syscall number", "user.err");
  }
}

void SpecialFunctionHandler::handleLoopIter(ExecutionState &state,
                                              KInstruction *target,
                                              std::vector<ref<Expr> > &arguments) {
  assert(arguments.size()==1 &&
         "invalid number of arguments to _klee_loop_iter");

  assert(isa<ConstantExpr>(arguments[0]) &&
         "argument to _klee_loop_iter is not a constant");

  uint32_t loopID = (uint32_t)cast<ConstantExpr>(arguments[0])->getZExtValue();
  std::vector<LoopExecIndex> &indexStack = state.stack().back().execIndexStack;

  // This function is called from the loop header. This means that either
  // a new loop or another iteration is just started.
  if (loopID != indexStack.back().loopID) {
    // New loop started, push corresponding item to the loop index stack
    LoopExecIndex execIndex = { loopID, indexStack.back().index };
    indexStack.push_back(execIndex);
    state.crtThread().topoIndex.push_back(TopoFrame(uint64_t(-1), 0));
    //CLOUD9_DEBUG("Loop enter: " << state);
  } else {
    state.crtThread().topoIndex.back().count++;
    //CLOUD9_DEBUG("Loop iteration: " << state);
  }
  indexStack.back().index = hashUpdate(indexStack.back().index, loopID);
}

void SpecialFunctionHandler::handleLoopExit(ExecutionState &state,
                                              KInstruction *target,
                                              std::vector<ref<Expr> > &arguments) {
  assert(arguments.size()==1 &&
         "invalid number of arguments to _klee_loop_exit");

  assert(isa<ConstantExpr>(arguments[0]) &&
         "argument to _klee_loop_exit is not a constant");

  uint64_t loopID = cast<ConstantExpr>(arguments[0])->getZExtValue();
  std::vector<LoopExecIndex> &indexStack = state.stack().back().execIndexStack;

  // This function is called from the loop exit basic blocks. This does not
  // mean that it is called after the loop: the block can have other
  // incoming edges. To distinguish this we check current loop ID
  if (loopID == indexStack.back().loopID) {
    // Loop terminated, pop corresponding item form the loop index stack
    assert(indexStack.size() > 1 && "Unexpected loop end");
    indexStack.pop_back();
    state.crtThread().topoIndex.pop_back();
    //CLOUD9_DEBUG("Loop exit: " << state);
  }
}

void SpecialFunctionHandler::handleMergeDisable(ExecutionState &state,
                                                KInstruction *target,
                                                std::vector<ref<Expr> > &arguments)
{
  assert(arguments.size()==1 &&
         "invalid number of arguments klee_merge_disable");

  assert(isa<ConstantExpr>(arguments[0]) &&
         "argument to klee_merge_disable is not a constant");

  uint64_t disabled = cast<ConstantExpr>(arguments[0])->getZExtValue();
  if (disabled)
    ++state.addressSpace().mergeDisabledCount;
  else
    --state.addressSpace().mergeDisabledCount;
}

void SpecialFunctionHandler::handleMergeBlacklist(ExecutionState &state,
                                                  KInstruction *target,
                                                  std::vector<ref<Expr> > &arguments)
{
#if 0
  assert(arguments.size()==3 &&
         "invalid number of arguments klee_merge_blacklist");

  assert(isa<ConstantExpr>(arguments[0]) &&
         isa<ConstantExpr>(arguments[1]) &&
         isa<ConstantExpr>(arguments[2]) &&
         "arguments to klee_merge_blacklist are not a constant");

  ref<ConstantExpr> address = cast<ConstantExpr>(arguments[0]);
  uint64_t size = cast<ConstantExpr>(arguments[1])->getZExtValue();

  uint64_t activate = cast<ConstantExpr>(arguments[2])->getZExtValue();
  state.updateMemoryUseFrequency(target->inst, address, size,
                                 activate ? INT_MAX : 0, INT_MAX);
#endif

#if 0
  ObjectPair op;
  bool ok = state.addressSpace().resolveOne(address, op);
  assert(ok && "arguments to klee_merge_set_active are invalid");

  ref<Expr> chk = op.first->getBoundsCheckPointer(address, size);
  assert(chk->isTrue() && "arguments to klee_merge_set_active are invalid");

  uint64_t offset = address->getZExtValue() - op.first->address;
  uint64_t activate = cast<ConstantExpr>(arguments[2])->getZExtValue();
  if (activate) {
    for (unsigned i = 0; i < size; ++i)
      state.addressSpace().addMergeBlacklistItem(op.first, offset+i);
  } else {
    for (unsigned i = 0; i < size; ++i)
      state.addressSpace().removeMergeBlacklistItem(op.first, offset+i);
  }
#endif
}

void SpecialFunctionHandler::handleMergeBlacklistClear(ExecutionState &state,
                                                  KInstruction *target,
                                                  std::vector<ref<Expr> > &arguments)
{
#if 0
  assert(arguments.size()==0 &&
         "invalid number of arguments klee_merge_blacklist");
  state.addressSpace().clearMergeBlacklist();
#endif
}

void SpecialFunctionHandler::handleUseFreq(ExecutionState &state,
                                           KInstruction *target,
                                           std::vector<ref<Expr> > &arguments)
{
#if 0
  /*
  assert(arguments.size()==0 &&
         "invalid number of arguments klee_use_freq");
         */

  /*
  assert(isa<ConstantExpr>(arguments[0]) &&
         isa<ConstantExpr>(arguments[1]) &&
         isa<ConstantExpr>(arguments[2]) &&
         isa<ConstantExpr>(arguments[3]) &&
         "arguments to klee_use_freq are not a constant");
         */

  const MDNode *md = target->inst->getMetadata("uf");
  assert(md && md->getNumOperands() == 4);
  KUseFreqInstruction *ku = static_cast<KUseFreqInstruction*>(target);

  if (ku->isPointer) {
    Value *ptr = md->getOperand(1);
    assert(isa<Constant>(ptr) || isa<AllocaInst>(ptr) || isa<Argument>(ptr) ||
           (isa<CallInst>(ptr) &&
              cast<CallInst>(ptr)->getCalledFunction()->getName() == "malloc"));

    ref<Expr> addrExpr = executor.evalV(ku->valueIdx, state).value;
    if (ConstantExpr *addrCExpr = dyn_cast<ConstantExpr>(addrExpr)) {
      Expr::Width width = executor.getWidthForLLVMType(
            cast<PointerType>(md->getOperand(1)->getType())->getElementType());

      uint64_t size = Expr::getMinBytesForWidth(width);
      state.updateMemoryUseFrequency(target->inst, addrCExpr, size,
                                     ku->numUses, ku->totalNumUses);
    } else {
      // XXX?
    }
  } else {
    Value *val = md->getOperand(1);
    assert(isa<Instruction>(val) || isa<Argument>(val));

    if (executor.getWidthForLLVMType(val->getType()) <= 64) {
      state.updateValUseFrequency(target->inst, ku->valueIdx,
                                ku->numUses, ku->totalNumUses);
    } else {
      // XXX
    }
  }

  /*
  ref<ConstantExpr> address = cast<ConstantExpr>(arguments[0]);
  uint64_t width = cast<ConstantExpr>(arguments[1])->getZExtValue();
  uint64_t size = Expr::getMinBytesForWidth(width);

  uint64_t useFreq = cast<ConstantExpr>(arguments[2])->getZExtValue();
  uint64_t totalUseFreq = cast<ConstantExpr>(arguments[3])->getZExtValue();

  state.updateUseFrequency(target->inst, address, size, useFreq, totalUseFreq);
  */
#endif
}

void SpecialFunctionHandler::handleRendezVous(ExecutionState &state,
                                           KInstruction *target,
                                           std::vector<ref<Expr> > &arguments) {
  assert(arguments.size() == 1
      && "invalid number of arguments to _klee_rendez_vous");

  assert(isa<ConstantExpr>(arguments[0])
      && "argument to _klee_rendez_vous is not a constant");

  uint64_t bbID = cast<ConstantExpr>(arguments[0])->getZExtValue();

  state.crtThread().topoIndex.back().bbID = bbID;
  //CLOUD9_DEBUG("Rendez-vous hit: " << state);
}
