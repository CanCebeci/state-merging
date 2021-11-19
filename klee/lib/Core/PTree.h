//===-- PTree.h -------------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef __UTIL_PTREE_H__
#define __UTIL_PTREE_H__

#include <klee/Expr.h>
#include <klee/ForkTag.h>

#include "llvm/ADT/SmallVector.h"

#include <utility>
#include <cassert>
#include <iostream>

namespace klee {
  class ExecutionState;

  /* XXX: with merging enabled this is no longer a tree */
  class PTree {
  public:
    typedef class PTreeNode Node;

  private:
    typedef ExecutionState* data_type;

  public:
    Node *root;

    PTree(const data_type &_root);
    ~PTree();
    
    std::pair<Node*,Node*> split(Node *n,
                                 const data_type &leftData,
                                 const data_type &rightData,
                                 const ref<Expr>& condition,
                                 ForkTag forkTag);

    void merge(Node *target, Node *other);
    Node* mergeCopy(Node *target, Node *other,
                    const data_type &mergedData);
    Node* duplicate(Node *main, const data_type &duplicateData);

    void terminate(Node *n);

    void markInactive(Node *n);
    void markActive(Node *n);

    void dump(std::ostream &os);
  };

  class PTreeNode {
    friend class PTree;
  public:
    PTreeNode *parent, *left, *right;
    llvm::SmallVector<PTreeNode*, 2> mergedParents;
    ExecutionState *data;
    ref<Expr> condition;
    enum { RUNNING, SPLITTED, MERGED, TERMINATED } state;
    bool active; ///< at least one node in a subtree is running

    ForkTag forkTag;
  private:
    PTreeNode(PTreeNode *_parent, ExecutionState *_data);
    ~PTreeNode();
  };
}

#endif
