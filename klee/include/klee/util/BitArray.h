//===-- BitArray.h ----------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_UTIL_BITARRAY_H
#define KLEE_UTIL_BITARRAY_H

#include <assert.h>

namespace klee {

  // XXX would be nice not to have
  // two allocations here for allocated
  // BitArrays
class BitArray {
private:
  uint32_t *bits;
  unsigned length;

protected:
  static uint32_t lengthForSize(unsigned size) { return (size+31)/32; }

public:
  BitArray(unsigned size, bool value = false)
      : bits(new uint32_t[lengthForSize(size)]), length(lengthForSize(size)) {
    memset(bits, value?0xFF:0, sizeof(*bits)*length);
  }

  BitArray(const BitArray &b)
      : bits(new uint32_t[b.length]), length(b.length) {
    memcpy(bits, b.bits, sizeof(*bits) * b.length);
  }

  BitArray(const BitArray &b, unsigned size)
      : bits(new uint32_t[b.length]), length(b.length) {
    memcpy(bits, b.bits, sizeof(*bits)*length);
    assert(length == lengthForSize(size));
  }

  const BitArray& operator=(const BitArray &b) {
    if (this != &b) {
      delete[] bits; bits = new uint32_t[b.length]; length = b.length;
      memcpy(bits, b.bits, sizeof(*bits) * length);
    }
    return *this;
  }

  ~BitArray() { delete[] bits; }

  bool get(unsigned idx) const { return (bool) ((bits[idx/32]>>(idx&0x1F))&1); }
  void set(unsigned idx) { bits[idx/32] |= 1<<(idx&0x1F); }
  void unset(unsigned idx) { bits[idx/32] &= ~(1<<(idx&0x1F)); }
  void set(unsigned idx, bool value) { if (value) set(idx); else unset(idx); }
};

} // End klee namespace

#endif
