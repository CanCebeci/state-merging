#ifndef KLEE_SIMPLEINCHASH_H
#define KLEE_SIMPLEINCHASH_H

#include "llvm/ADT/APInt.h"

#define SIMPLE_INC_HASH_BITS 64
#define SIMPLE_INC_HASH_USE_APINT_HASH

class SimpleIncHash: public llvm::APInt {
public:
  SimpleIncHash(): llvm::APInt(SIMPLE_INC_HASH_BITS,
                               llvm::APInt(64, 0).getHashValue()) {}

  void addValueAt(APInt value, unsigned offset) {
#ifdef SIMPLE_INC_HASH_USE_APINT_HASH
    offset &= 63;
    uint64_t valueHash = value.getHashValue();
    valueHash = (valueHash << offset) | (valueHash >> (64 - offset));
    *this += APInt(SIMPLE_INC_HASH_BITS, valueHash);
#else
    value.rotl(offset & (SIMPLE_INC_HASH_BITS-1));
    uint8_t *bytes = value.getRawData();
    for (unsigned i = 0, e = value.getNumWords()*8; i != e; ++i)
      (*this) += APInt(SIMPLE_INC_HASH_BITS, bytes[i]);
#endif
  }

  void removeValueAt(APInt value, unsigned offset) {
#ifdef SIMPLE_INC_HASH_USE_APINT_HASH
    offset &= 63;
    uint64_t valueHash = value.getHashValue();
    valueHash = (valueHash << offset) | (valueHash >> (64 - offset));
    *this -= APInt(SIMPLE_INC_HASH_BITS, valueHash);
#else
    value.rotl(offset & (SIMPLE_INC_HASH_BITS-1));
    uint8_t *bytes = value.getRawData();
    for (unsigned i = 0, e = value.getNumWords()*8; i != e; ++i)
      (*this) -= APInt(SIMPLE_INC_HASH_BITS, bytes[i]);
#endif
  }

  void addValueAt(APInt value, const void *obj, unsigned offset) {
    addValueAt(value, uintptr_t(obj) + offset);
  }

  void removeValueAt(APInt value, const void *obj, unsigned offset) {
    removeValueAt(value, uintptr_t(obj) + offset);
  }
};

#endif // SIMPLEINCHASH_H
