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

#ifndef THREADING_H_
#define THREADING_H_

#include "klee/Expr.h"
#include "klee/Internal/Module/KInstIterator.h"
#include "klee/Internal/Module/QCE.h"
#include "klee/util/BitArray.h"
#include "klee/util/SimpleIncHash.h"
#include "../../lib/Core/AddressSpace.h"
#include "cloud9/Logger.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"

#include <map>

namespace llvm {
  class Value;
  class Instruction;
}

namespace klee {

class KFunction;
class KInstruction;
class ExecutionState;
class Process;
class CallPathNode;
class MemoryObject;
struct Cell;

typedef uint64_t thread_id_t;
typedef uint64_t process_id_t;
typedef uint64_t wlist_id_t;

typedef std::pair<thread_id_t, process_id_t> thread_uid_t;

struct LoopExecIndex {
  uint64_t loopID;
  uint64_t index;
};

struct QCEFrameInfo {
  unsigned stackFrame;
  int      vnumber;
  bool     inVhAdd;

  float qce;
  float qceBase;

  QCEFrameInfo(int _stackFrame = NULL, int _vnumber = 0)
    : stackFrame(_stackFrame), vnumber(_vnumber), inVhAdd(false),
      qce(0), qceBase(0) {}
};

typedef llvm::DenseMap<HotValue, QCEFrameInfo> QCEMap;

#define QCE_LOCALS_MAGIC_VALUE 0xde1fa442ff3e32abL

struct TopoFrame {
  uint64_t bbID;
  uint64_t count;

  TopoFrame(uint64_t _bbID, uint64_t _count) : bbID(_bbID), count(_count) { }
};
typedef std::vector<TopoFrame> TopoIndex;

struct StackFrame {
  KInstIterator caller;
  KFunction *kf;
  CallPathNode *callPathNode;

  std::vector<const MemoryObject*> allocas;
  Cell *locals;

  /// Minimum distance to an uncovered instruction once the function
  /// returns. This is not a good place for this but is used to
  /// quickly compute the context sensitive minimum distance to an
  /// uncovered instruction. This value is updated by the StatsTracker
  /// periodically.
  unsigned minDistToUncoveredOnReturn;

  // For vararg functions: arguments not passed via parameter are
  // stored (packed tightly) in a local (alloca) memory object. This
  // is setup to match the way the front-end generates vaarg code (it
  // does not pass vaarg through as expected). VACopy is lowered inside
  // of intrinsic lowering.
  MemoryObject *varargs;

  /// A stack of execution indexes. An item at index 0 corresponds to the
  /// non-loop function code, each next item corresponds to one loop level.
  /// This is updated by special function handlers for loop instrumentation.
  std::vector<LoopExecIndex> execIndexStack;

  bool isUserMain;

  float qceTotal;
  float qceTotalBase;
  QCEMap qceMap;

  BitArray      qceLocalsTrackMap;
  SimpleIncHash qceLocalsTrackHash;

  StackFrame(KInstIterator caller, uint64_t _callerExecIndex,
             KFunction *kf, StackFrame *parentFrame);
  StackFrame(const StackFrame &s);

  StackFrame& operator=(const StackFrame &sf);
  ~StackFrame();
};

typedef std::pair<const MemoryObject*, uint64_t> QCEMemoryTrackIndex;
typedef llvm::DenseSet<HotValue> QCEMemoryTrackSet;
typedef llvm::DenseMap<QCEMemoryTrackIndex, QCEMemoryTrackSet>
                  QCEMemoryTrackMap;

class Thread {
  friend class Executor;
  friend class ExecutionState;
  friend class Process;
  friend class SpecialFunctionHandler;
private:

  KInstIterator pc, prevPC;
  unsigned incomingBBIndex;

  std::vector<StackFrame> stack;

  // QCE bookkeeping
  QCEMemoryTrackMap qceMemoryTrackMap;
  SimpleIncHash qceMemoryTrackHash;

  bool enabled;
  wlist_id_t waitingList;

  thread_uid_t tuid;

  uint64_t execIndex;
  uint64_t mergeIndex;

  TopoIndex topoIndex;

public:
  Thread(thread_id_t tid, process_id_t pid, KFunction *start_function);

  thread_id_t getTid() const { return tuid.first; }
  process_id_t getPid() const { return tuid.second; }
};

}

#endif /* THREADING_H_ */
