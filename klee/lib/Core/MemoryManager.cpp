//===-- MemoryManager.cpp -------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Common.h"

#include "CoreStats.h"
#include "Memory.h"
#include "MemoryManager.h"

#include "klee/ExecutionState.h"
#include "klee/Expr.h"
#include "klee/Solver.h"

#include "llvm/Support/CommandLine.h"

using namespace klee;

/***/

MemoryManager::~MemoryManager() { 
  while (!objects.empty()) {
    MemoryObject *mo = objects.back();
    objects.pop_back();
    delete mo;
  }
}

MemoryObject *MemoryManager::allocate(ExecutionState *state, uint64_t size, bool isLocal,
                                      bool isGlobal,
                                      const llvm::Value *allocSite) {
  if (size>10*1024*1024) {
    klee_warning_once(0, "failing large alloc: %u bytes", (unsigned) size);
    return 0;
  }
  uint64_t address = (uint64_t)state->addressPool.allocate(size);
  if (!address)
    return 0;
  
  ++stats::allocations;
  MemoryObject *res = new MemoryObject(address, size, isLocal, isGlobal, false,
                                       allocSite);
  objects.push_back(res);
  return res;
}

MemoryObject *MemoryManager::allocateFixed(uint64_t address, uint64_t size,
                                           const llvm::Value *allocSite,
                                           const char* name) {
#ifndef NDEBUG
  for (objects_ty::iterator it = objects.begin(), ie = objects.end();
       it != ie; ++it) {
    MemoryObject *mo = *it;
    assert(!(address+size > mo->address && address < mo->address+mo->size) &&
           "allocated an overlapping object");
  }
#endif

  ++stats::allocations;
  MemoryObject *res = new MemoryObject(address, size, false, true, true,
                                       allocSite);
  if (name)
    res->setName(name);
  objects.push_back(res);
  return res;
}

void MemoryManager::deallocate(const MemoryObject *mo) {
  assert(0);
}
