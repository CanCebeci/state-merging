/*
 * Cloud9 Parallel Symbolic Execution Engine
 *
 * Copyright (c) 2011, Dependable Systems Laboratory, EPFL
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Dependable Systems Laboratory, EPFL nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE DEPENDABLE SYSTEMS LABORATORY, EPFL BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * All contributors are listed in CLOUD9-AUTHORS file.
 *
*/

#include "klee/Threading.h"
#include "klee/ExecutionState.h"
#include "klee/Internal/Module/KModule.h"
#include "klee/Internal/Module/Cell.h"
#include "klee/Expr.h"

#include "llvm/Function.h"

namespace klee {

/* StackFrame Methods */

StackFrame::StackFrame(KInstIterator _caller, uint64_t _callerExecIndex, KFunction *_kf,
                       StackFrame *parentFrame)
  : caller(_caller), kf(_kf), callPathNode(0),
    minDistToUncoveredOnReturn(0), varargs(0),
    execIndexStack(1),
    qceTotal(parentFrame ? parentFrame->qceTotal : 0),
    qceTotalBase(parentFrame ? parentFrame->qceTotalBase : 0),
    qceMap(parentFrame ? parentFrame->qceMap : QCEMap()),
    qceLocalsTrackMap(_kf->numRegisters, false) {

  execIndexStack[0].loopID = uint64_t(-1);
  execIndexStack[0].index = hashUpdate(_callerExecIndex, (uintptr_t) _kf);

  locals = new Cell[kf->numRegisters];

  isUserMain = _kf->function->getName() == "__user_main";
}

StackFrame::StackFrame(const StackFrame &s)
  : caller(s.caller),
    kf(s.kf),
    callPathNode(s.callPathNode),
    allocas(s.allocas),
    minDistToUncoveredOnReturn(s.minDistToUncoveredOnReturn),
    varargs(s.varargs),
    execIndexStack(s.execIndexStack),
    isUserMain(s.isUserMain),
    qceTotal(s.qceTotal),
    qceTotalBase(s.qceTotalBase),
    qceMap(s.qceMap),
    qceLocalsTrackMap(s.qceLocalsTrackMap, s.kf->numRegisters),
    qceLocalsTrackHash(s.qceLocalsTrackHash) {

  locals = new Cell[s.kf->numRegisters];
  for (unsigned i=0; i<s.kf->numRegisters; i++)
    locals[i] = s.locals[i];
}

StackFrame& StackFrame::operator=(const StackFrame &s) {
  if (this != &s) {
    caller = s.caller;
    kf = s.kf;
    callPathNode = s.callPathNode;
    allocas = s.allocas;
    minDistToUncoveredOnReturn = s.minDistToUncoveredOnReturn;
    varargs = s.varargs;
    execIndexStack = s.execIndexStack;
    isUserMain = s.isUserMain;
    qceTotal = s.qceTotal;
    qceTotalBase = s.qceTotalBase;
    qceMap = s.qceMap;
    qceLocalsTrackMap = BitArray(s.qceLocalsTrackMap, s.kf->numRegisters);
    qceLocalsTrackHash = s.qceLocalsTrackHash;

    if (locals)
      delete []locals;

    locals = new Cell[s.kf->numRegisters];
    for (unsigned i=0; i<s.kf->numRegisters; i++)
        locals[i] = s.locals[i];
  }

  return *this;
}

StackFrame::~StackFrame() {
  delete[] locals;
}

/* Thread class methods */

Thread::Thread(thread_id_t tid, process_id_t pid, KFunction * kf) :
  enabled(true), waitingList(0), execIndex(hashInit()), mergeIndex(0) {

  execIndex = hashUpdate(execIndex, tid);
  execIndex = hashUpdate(execIndex, pid);
  mergeIndex = execIndex;

  tuid = std::make_pair(tid, pid);

  if (kf) {
    stack.push_back(StackFrame(0, execIndex, kf, NULL));
    topoIndex.push_back(TopoFrame(uint64_t(-1), 0));

    pc = kf->instructions;
    prevPC = pc;
  } else {
    pc = NULL;
    prevPC = NULL;
  }
}

}
