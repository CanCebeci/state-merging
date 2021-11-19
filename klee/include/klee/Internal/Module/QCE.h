#ifndef KLEE_QCE_H
#define KLEE_QCE_H

#include "llvm/Value.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/APInt.h"

#define QCE_BWIDTH 64

namespace llvm {
  class raw_ostream;
  class MDNode;
  class LLVMContext;
}

namespace klee {

enum HotValueKind { HVVal, HVPtr };

class HotValue {
  llvm::Value *value;
  unsigned offset;
  short    size;
  short    kind;

public:
  typedef HotValueKind Kind;

  HotValue(): value(NULL), offset(0), size(0), kind(0) {}
  HotValue(Kind _kind, llvm::Value *_value,
           unsigned _offset = 0, unsigned _size = 0):
      value(_value), offset(_offset), size(_size), kind(_kind) {}

  Kind getKind() const { return Kind(kind); }
  llvm::Value *getValue() const { return value; }
  unsigned getOffset() const { return offset; }
  unsigned getSize() const { return size; }

  bool isVal() const { return getKind() == HVVal; }
  bool isPtr() const { return getKind() == HVPtr; }

  void print(llvm::raw_ostream& ostr) const;
  void dump() const;

  bool operator==(const HotValue &b) const {
    return value == b.value && offset == b.offset &&
           size == b.size && kind == b.kind;
  }

  llvm::MDNode *toMDNode(llvm::LLVMContext &Ctx, llvm::APInt useCount) const;
  static std::pair<HotValue, llvm::APInt> fromMDNode(const llvm::MDNode *MD);

  // Helper for type traits. Hope compiler will optimize this...
  uint64_t getExtra() const {
    return uint64_t(offset) | (uint64_t(size) << 32) | (uint64_t(kind) << 48);
  }

  bool operator<(const HotValue &b) const {
    return (value < b.value) ||
           (!(value < b.value) && getExtra() < b.getExtra());
  }
};

} // namespace klee

namespace llvm {
  /*
  template<> struct FoldingSetTrait<klee::HotValue> {
    static inline void Profile(const klee::HotValue &hv, FoldingSetNodeID& ID) {
      ID.AddPointer(hv.getValue());
      ID.AddInteger(hv.getExtra());
    }
  };
  */

  template<> struct DenseMapInfo<klee::HotValue> {
    static inline klee::HotValue getEmptyKey() {
      return klee::HotValue(klee::HVVal,
                            DenseMapInfo<Value*>::getEmptyKey());
    }
    static inline klee::HotValue getTombstoneKey() {
      return klee::HotValue(klee::HVPtr,
                            DenseMapInfo<Value*>::getTombstoneKey());
    }
    static unsigned getHashValue(const klee::HotValue &v) {
      uint64_t key =
        (uint64_t)DenseMapInfo<Value*>::getHashValue(v.getValue()) << 32
        | (v.getExtra() * 37ULL);
      key += ~(key << 32);
      key ^= (key >> 22);
      key += ~(key << 13);
      key ^= (key >> 8);
      key += (key << 3);
      key ^= (key >> 15);
      key += ~(key << 27);
      key ^= (key >> 31);
      return (unsigned)key;
    }
    static bool isEqual(const klee::HotValue &l, const klee::HotValue &r) {
      return l.getValue() == r.getValue() && l.getExtra() == r.getExtra();
    }
  };

  // HotValue can be treated like a POD type.
  template <typename T> struct isPodLike;
  template <> struct isPodLike<klee::HotValue> {
    static const bool value = true;
  };
} // namespace llvm

#if 0
namespace klee {

enum HotValueKind { HVVal, HVPtr };
typedef llvm::PointerIntPair<llvm::Value*, 1, HotValueKind> HotValueBaseTy;

class HotValue: public HotValueBaseTy {
public:
  HotValue(): HotValueBaseTy(NULL, HVVal) {}
  HotValue(HotValueKind K, llvm::Value *V): HotValueBaseTy(V, K) {}

  llvm::Value *getValue() const { return getPointer(); }
  HotValueKind getKind() const { return getInt(); }

  bool isVal() const { return getInt() == HVVal; }
  bool isPtr() const { return getInt() == HVPtr; }

  // An easy way to support DenseMap
  HotValue(const HotValueBaseTy& P): HotValueBaseTy(P) {}
  operator HotValueBaseTy () {
    return HotValueBaseTy::getFromOpaqueValue(getOpaqueValue());
  }

  void print(llvm::raw_ostream& ostr) const;
  void dump() const;
};

} // namespace klee

namespace llvm {
  template<> struct FoldingSetTrait<klee::HotValue> {
    static inline void Profile(const klee::HotValue hv, FoldingSetNodeID& ID) {
      ID.AddPointer(hv.getOpaqueValue());
    }
  };
  template<> struct PointerLikeTypeTraits<klee::HotValue>:
    public PointerLikeTypeTraits<klee::HotValueBaseTy> {};
  template<> struct DenseMapInfo<klee::HotValue>:
    public DenseMapInfo<klee::HotValueBaseTy> {};
} // namespace llvm
#endif

#endif // KLEE_QCE_H
