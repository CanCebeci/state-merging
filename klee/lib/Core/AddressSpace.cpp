//===-- AddressSpace.cpp --------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "AddressSpace.h"
#include "CoreStats.h"
#include "Memory.h"
#include "TimingSolver.h"

#include "../Core/Common.h"

#include "klee/Expr.h"
#include "klee/TimerStatIncrementer.h"
#include "klee/AddressPool.h"

#include <sys/mman.h>

using namespace klee;

///

void AddressSpace::bindObject(const MemoryObject *mo, ObjectState *os) {
  assert(os->copyOnWriteOwner==0 && "object already has owner");
  os->copyOnWriteOwner = cowKey;

  const MemoryMap::value_type *prev = objects.lookup(mo);
  if (prev)
    hash -= (uintptr_t) prev->first;

  objects = objects.replace(std::make_pair(mo, os));
  hash += (uintptr_t) mo;
}

void AddressSpace::bindSharedObject(const MemoryObject *mo, ObjectState *os) {
  assert(os->isShared);
  assert(os->copyOnWriteOwner > 0);

  objects = objects.insert(std::make_pair(mo, os));
}

void AddressSpace::unbindObject(const MemoryObject *mo) {
  const MemoryMap::value_type *prev = objects.lookup(mo);
  if (prev) {
    hash -= (uintptr_t) prev->first;
    /*
    removeMergeBlacklistItemHash(mo, prev->second);
    for (MergeBlacklist::iterator it = mergeBlacklist.begin(),
            ie = mergeBlacklist.end(); it != ie;) {
      if (it->first == mo) {
        mergeBlacklistHash -= uintptr_t(it->second);
        mergeBlacklistHash -= uintptr_t(mo);
        mergeBlacklist.erase(it++);
      } else {
        ++it;
      }
    }*/
#warning Remove from mergeBlacklistMap
  }
  objects = objects.remove(mo);
}

const ObjectState *AddressSpace::findObject(const MemoryObject *mo) const {
  const MemoryMap::value_type *res = objects.lookup(mo);
  
  return res ? res->second : 0;
}

ObjectState *AddressSpace::getWriteable(const MemoryObject *mo,
                                        const ObjectState *os) {
  assert(!os->readOnly);

  if (cowKey != os->copyOnWriteOwner) {
    if (os->isShared) {
      ObjectState *n = new ObjectState(*os);
      n->copyOnWriteOwner = cowKey;

      for (cow_domain_t::iterator it = cowDomain->begin(); it != cowDomain->end();
          it++) {
        AddressSpace *as = *it;
        if (as->findObject(mo) != NULL) {
          as->objects = as->objects.replace(std::make_pair(mo, n));
        }
      }

      return n;
    } else {
      ObjectState *n = new ObjectState(*os);
      n->copyOnWriteOwner = cowKey;

      objects = objects.replace(std::make_pair(mo, n));
      return n;
    }
  } else {
    return const_cast<ObjectState*>(os);
  }
}

/// 

bool AddressSpace::resolveOne(const ref<ConstantExpr> &addr, 
                              ObjectPair &result) {
  uint64_t address = addr->getZExtValue();
  MemoryObject hack(address);

  if (const MemoryMap::value_type *res = objects.lookup_previous(&hack)) {
    const MemoryObject *mo = res->first;
    if ((mo->size==0 && address==mo->address) ||
        (address - mo->address < mo->size)) {
      result = *res;
      return true;
    }
  }

  return false;
}

bool AddressSpace::resolveOne(ExecutionState &state,
                              TimingSolver *solver,
                              ref<Expr> address,
                              ObjectPair &result,
                              bool &success) {
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(address)) {
    success = resolveOne(CE, result);
    return true;
  } else {
    if (SelectExpr *se = dyn_cast<SelectExpr>(address)) {
      if (se->isConstantCases()) {
        std::vector<uint64_t> cases;
        se->getConstantCases(&cases);
        assert(!cases.empty());

        MemoryObject hack(cases[0]);
        const MemoryMap::value_type *res = objects.lookup_previous(&hack);
        if (res) {
          std::vector<uint64_t>::iterator it = cases.begin(), ie = cases.end();
          for (; it != ie; ++it) {
            if (*it < res->first->address ||
                *it - res->first->address >= res->first->size)
              break;
          }
          if (it == ie) {
            // All cases are within one memory object
            result = *res;
            success = true;
            return true;
          }
        }
      }
    }

    TimerStatIncrementer timer(stats::resolveTime);

    // try cheap search, will succeed for any inbounds pointer

    ref<ConstantExpr> cex;
    if (!solver->getValue(state, address, cex))
      return false;
    uint64_t example = cex->getZExtValue();
    MemoryObject hack(example);
    const MemoryMap::value_type *res = objects.lookup_previous(&hack);
    
    if (res) {
      const MemoryObject *mo = res->first;
      if (example - mo->address < mo->size) {
        result = *res;
        success = true;
        return true;
      }
    }

    // didn't work, now we have to search
       
    MemoryMap::iterator oi = objects.upper_bound(&hack);
    MemoryMap::iterator begin = objects.begin();
    MemoryMap::iterator end = objects.end();
      
    MemoryMap::iterator start = oi;
    while (oi!=begin) {
      --oi;
      const MemoryObject *mo = oi->first;
        
      bool mayBeTrue;
      if (!solver->mayBeTrue(state, 
                             mo->getBoundsCheckPointer(address), mayBeTrue))
        return false;
      if (mayBeTrue) {
        result = *oi;
        success = true;
        return true;
      } else {
        bool mustBeTrue;
        if (!solver->mustBeTrue(state, 
                                UgeExpr::create(address, mo->getBaseExpr()),
                                mustBeTrue))
          return false;
        if (mustBeTrue)
          break;
      }
    }

    // search forwards
    for (oi=start; oi!=end; ++oi) {
      const MemoryObject *mo = oi->first;

      bool mustBeTrue;
      if (!solver->mustBeTrue(state, 
                              UltExpr::create(address, mo->getBaseExpr()),
                              mustBeTrue))
        return false;
      if (mustBeTrue) {
        break;
      } else {
        bool mayBeTrue;

        if (!solver->mayBeTrue(state, 
                               mo->getBoundsCheckPointer(address),
                               mayBeTrue))
          return false;
        if (mayBeTrue) {
          result = *oi;
          success = true;
          return true;
        }
      }
    }

    success = false;
    return true;
  }
}

bool AddressSpace::resolve(ExecutionState &state,
                           TimingSolver *solver, 
                           ref<Expr> p, 
                           ResolutionList &rl, 
                           unsigned maxResolutions,
                           double timeout) {
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(p)) {
    ObjectPair res;
    if (resolveOne(CE, res))
      rl.push_back(res);
    return false;
  } else {
    TimerStatIncrementer timer(stats::resolveTime);
    uint64_t timeout_us = (uint64_t) (timeout*1000000.);

    // XXX in general this isn't exactly what we want... for
    // a multiple resolution case (or for example, a \in {b,c,0})
    // we want to find the first object, find a cex assuming
    // not the first, find a cex assuming not the second...
    // etc.
    
    // XXX how do we smartly amortize the cost of checking to
    // see if we need to keep searching up/down, in bad cases?
    // maybe we don't care?
    
    // XXX we really just need a smart place to start (although
    // if its a known solution then the code below is guaranteed
    // to hit the fast path with exactly 2 queries). we could also
    // just get this by inspection of the expr.
    
    ref<ConstantExpr> cex;
    if (!solver->getValue(state, p, cex))
      return true;
    uint64_t example = cex->getZExtValue();
    MemoryObject hack(example);
    
    MemoryMap::iterator oi = objects.upper_bound(&hack);
    MemoryMap::iterator begin = objects.begin();
    MemoryMap::iterator end = objects.end();
      
    MemoryMap::iterator start = oi;
      
    // XXX in the common case we can save one query if we ask
    // mustBeTrue before mayBeTrue for the first result. easy
    // to add I just want to have a nice symbolic test case first.
      
    // search backwards, start with one minus because this
    // is the object that p *should* be within, which means we
    // get write off the end with 4 queries (XXX can be better,
    // no?)
    while (oi!=begin) {
      --oi;
      const MemoryObject *mo = oi->first;
      if (timeout_us && timeout_us < timer.check())
        return true;

      // XXX I think there is some query wasteage here?
      ref<Expr> inBounds = mo->getBoundsCheckPointer(p);
      bool mayBeTrue;
      if (!solver->mayBeTrue(state, inBounds, mayBeTrue))
        return true;
      if (mayBeTrue) {
        rl.push_back(*oi);
        
        // fast path check
        unsigned size = rl.size();
        if (size==1) {
          bool mustBeTrue;
          if (!solver->mustBeTrue(state, inBounds, mustBeTrue))
            return true;
          if (mustBeTrue)
            return false;
        } else if (size==maxResolutions) {
          return true;
        }
      }
        
      bool mustBeTrue;
      if (!solver->mustBeTrue(state, 
                              UgeExpr::create(p, mo->getBaseExpr()),
                              mustBeTrue))
        return true;
      if (mustBeTrue)
        break;
    }
    // search forwards
    for (oi=start; oi!=end; ++oi) {
      const MemoryObject *mo = oi->first;
      if (timeout_us && timeout_us < timer.check())
        return true;

      bool mustBeTrue;
      if (!solver->mustBeTrue(state, 
                              UltExpr::create(p, mo->getBaseExpr()),
                              mustBeTrue))
        return true;
      if (mustBeTrue)
        break;
      
      // XXX I think there is some query wasteage here?
      ref<Expr> inBounds = mo->getBoundsCheckPointer(p);
      bool mayBeTrue;
      if (!solver->mayBeTrue(state, inBounds, mayBeTrue))
        return true;
      if (mayBeTrue) {
        rl.push_back(*oi);
        
        // fast path check
        unsigned size = rl.size();
        if (size==1) {
          bool mustBeTrue;
          if (!solver->mustBeTrue(state, inBounds, mustBeTrue))
            return true;
          if (mustBeTrue)
            return false;
        } else if (size==maxResolutions) {
          return true;
        }
      }
    }
  }

  return false;
}

// These two are pretty big hack so we can sort of pass memory back
// and forth to externals. They work by abusing the concrete cache
// store inside of the object states, which allows them to
// transparently avoid screwing up symbolics (if the byte is symbolic
// then its concrete cache byte isn't being used) but is just a hack.

void AddressSpace::copyOutConcretes(AddressPool *pool) {
  for (MemoryMap::iterator it = objects.begin(), ie = objects.end(); 
       it != ie; ++it) {
    const MemoryObject *mo = it->first;

    if (!mo->isUserSpecified) {
      ObjectState *os = it->second;
      uint8_t *address = (uint8_t*) (unsigned long) mo->address;

      if (!os->readOnly)
        memcpy(address, os->concreteStore, mo->size);
    }
  }
}

bool AddressSpace::copyInConcretes(AddressPool *pool) {
  for (MemoryMap::iterator it = objects.begin(), ie = objects.end(); 
       it != ie; ++it) {
    const MemoryObject *mo = it->first;

    if (!mo->isUserSpecified) {
      const ObjectState *os = it->second;
      uint8_t *address = (uint8_t*) (unsigned long) mo->address;

      if (memcmp(address, os->concreteStore, mo->size)!=0) {
        if (os->readOnly) {
          return false;
        } else {
          ObjectState *wos = getWriteable(mo, os);
          memcpy(wos->concreteStore, address, mo->size);
        }
      }
    }
  }

  return true;
}

void AddressSpace::_testAddressSpace() {
	uint64_t prevAddr = 0;

	for (MemoryMap::iterator it = objects.begin(); it != objects.end(); ++it) {
		uint64_t crtAddr = it->first->address;

		assert(crtAddr >= prevAddr);

		prevAddr = crtAddr;
	}
}

/***/

bool MemoryObjectLT::operator()(const MemoryObject *a, const MemoryObject *b) const {
  return a->address < b->address;
}

#if 0
void AddressSpace::addMergeBlacklistItem(const MemoryObject *mo, unsigned offset) {
  bool inserted = mergeBlacklist.insert(std::make_pair(mo, offset)).second;
  if (inserted) {
    mergeBlacklistHash += uintptr_t(mo);
    mergeBlacklistHash += uintptr_t(offset);
    addMergeBlacklistItemHash(mo, const_cast<ObjectState*>(findObject(mo)),
                              ConstantExpr::alloc(offset, 32), 1);
  }
}

void AddressSpace::removeMergeBlacklistItem(const MemoryObject *mo, unsigned offset) {
  MergeBlacklist::iterator it = mergeBlacklist.find(std::make_pair(mo, offset));
  if (it != mergeBlacklist.end()) {
    removeMergeBlacklistItemHash(mo, const_cast<ObjectState*>(findObject(mo)),
                                 ConstantExpr::alloc(offset, 32), 1);
    mergeBlacklistHash -= uintptr_t(offset);
    mergeBlacklistHash -= uintptr_t(mo);
    mergeBlacklist.erase(it);
  }
}

void AddressSpace::clearMergeBlacklist() {
  mergeBlacklist.clear();
  mergeBlacklistHash = hashInit();
}

void AddressSpace::removeMergeBlacklistItemHash(const MemoryObject *mo, ObjectState *os) {
  foreach (const MergeBlacklist::value_type &v, mergeBlacklist) {
    if (v.first == mo) {
      if (os->isByteConcrete(v.second))
        mergeBlacklistHash -= uintptr_t(cast<ConstantExpr>(os->read8(v.second))->getZExtValue());
    }
  }
}

void AddressSpace::removeMergeBlacklistItemHash(const MemoryObject *mo, ObjectState *os,
                              ref<Expr> offset, unsigned size) {
  if (!isa<ConstantExpr>(offset))
    return;

  unsigned oc = cast<ConstantExpr>(offset)->getZExtValue();
  for (unsigned i = 0; i < size; ++i) {
    if (os->isByteConcrete(oc+i))
      mergeBlacklistHash -= uintptr_t(cast<ConstantExpr>(os->read8(oc+i))->getZExtValue());
  }
}

void AddressSpace::addMergeBlacklistItemHash(const MemoryObject *mo, ObjectState *os,
                              ref<Expr> offset, unsigned size) {
  if (!isa<ConstantExpr>(offset))
    return;

  unsigned oc = cast<ConstantExpr>(offset)->getZExtValue();
  for (unsigned i = 0; i < size; ++i) {
    if (os->isByteConcrete(oc+i))
      mergeBlacklistHash += uintptr_t(cast<ConstantExpr>(os->read8(oc+i))->getZExtValue());
  }
}
#endif
