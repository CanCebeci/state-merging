//===-- Ref.h ---------------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_REF_H
#define KLEE_REF_H

#include "llvm/Support/Casting.h"
using llvm::isa;
using llvm::cast;
using llvm::cast_or_null;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

#include <assert.h>
#include <iosfwd> // FIXME: Remove this!!!

#include <boost/interprocess/detail/atomic.hpp>

namespace klee {

template<class T>
class ref {
  T *ptr;

public:
  // default constructor: create a NULL reference
  ref() : ptr(0) { }
  ~ref () { dec (); }

private:
  void inc() {
    if (ptr)
      boost::interprocess::detail::atomic_inc32(&ptr->refCount);
      //++ptr->refCount;
  }
  
  void dec() {
    //if (ptr && --ptr->refCount == 0)
    //  delete ptr;
    if (ptr) {
      if (1 == boost::interprocess::detail::atomic_dec32(&ptr->refCount))
        delete ptr;
    }
  }  

public:
  template<class U> friend class ref;

  // constructor from pointer
  ref(T *p) : ptr(p) {
    inc();
  }
  
  // normal copy constructor
  ref(const ref<T> &r) : ptr(r.ptr) {
    inc();
  }
  
  // conversion constructor
  template<class U>
  ref (const ref<U> &r) {
    ptr = r.ptr;
    inc();
  }
  
  // pointer operations
  T *get () const {
    return ptr;
  }

  /* The copy assignment operator must also explicitly be defined,
   * despite a redundant template. */
  ref<T> &operator= (const ref<T> &r) {
    dec();
    ptr = r.ptr;
    inc();
    
    return *this;
  }
  
  template<class U> ref<T> &operator= (const ref<U> &r) {
    dec();
    ptr = r.ptr;
    inc();
    
    return *this;
  }

  T& operator*() const {
    return *ptr;
  }

  T* operator->() const {
    return ptr;
  }

  bool isNull() const { return ptr == 0; }

  // assumes non-null arguments
  int compare(const ref &rhs) const {
    assert(!isNull() && !rhs.isNull() && "Invalid call to compare()");
    return get()->compare(*rhs.get());
  }

  // assumes non-null arguments
  bool operator<(const ref &rhs) const { return compare(rhs)<0; }
  bool operator==(const ref &rhs) const { return compare(rhs)==0; }
  bool operator!=(const ref &rhs) const { return compare(rhs)!=0; }
};

template<class T>
inline std::ostream &operator<<(std::ostream &os, const ref<T> &e) {
  os << *e;
  return os;
}

} // end namespace klee

namespace llvm {
  // simplify_type implementation for ref<>, which allows dyn_cast from on a
  // ref<> to apply to the wrapper type. Conceptually the result of such a
  // dyn_cast should probably be a ref of the casted type, but that breaks the
  // idiom of initializing a variable to the result of a dyn_cast inside an if
  // condition, or we would have to implement operator(bool) for ref<> with
  // isNull semantics, which doesn't seem like a good idea.
template<typename T>
struct simplify_type<const ::klee::ref<T> > {
  typedef T* SimpleType;
  static SimpleType getSimplifiedValue(const ::klee::ref<T> &Ref) {
    return Ref.get();
  }
};

template<typename T> 
struct simplify_type< ::klee::ref<T> >
  : public simplify_type<const ::klee::ref<T> > {};
}

#endif /* KLEE_REF_H */
