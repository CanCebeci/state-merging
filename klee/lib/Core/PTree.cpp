//===-- PTree.cpp ---------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "PTree.h"

#include <klee/Expr.h>
#include <klee/util/ExprPPrinter.h>
#include "klee/ExecutionState.h"

#include <vector>
#include <iostream>

using namespace klee;

  /* *** */

PTree::PTree(const data_type &_root) : root(new Node(0,_root)) {
}

PTree::~PTree() {}

std::pair<PTreeNode*, PTreeNode*>
PTree::split(Node *n, 
             const data_type &leftData, 
             const data_type &rightData,
             const ref<Expr>& condition,
             ForkTag forkTag) {

  assert(n && !n->left && !n->right);
  assert(n->state == PTreeNode::RUNNING);
  n->state = PTreeNode::SPLITTED;
  n->condition = condition;
  n->left = new Node(n, leftData);
  n->right = new Node(n, rightData);
  n->forkTag = forkTag;

  return std::make_pair(n->left, n->right);
}

void PTree::merge(Node *target, Node *other) {
  assert(target);
  assert(other && !other->left && !other->right);
  assert(other->state == PTreeNode::RUNNING);

  other->left = target;
  other->state = PTreeNode::MERGED;
  target->mergedParents.push_back(other);
}

PTreeNode* PTree::mergeCopy(Node *target, Node *other,
                            const data_type &mergedData) {
  assert(target && !target->left && !target->right);
  assert(other && !other->left && !other->right);

  assert(target->state == PTreeNode::RUNNING);
  assert(other->state == PTreeNode::RUNNING);

  PTreeNode *merged = new Node(0, mergedData);
  target->state = PTreeNode::MERGED;
  target->right = merged;
  other->state = PTreeNode::MERGED;
  other->left = merged;

  merged->mergedParents.push_back(target);
  merged->mergedParents.push_back(other);
  return merged;
}

PTreeNode* PTree::duplicate(Node *main, const data_type &duplicateData)
{
  PTreeNode *dup = new Node(main, duplicateData);
  if (!main->left) {
    //main->left = dup;
  } else if (!main->right) {
    //main->right = dup;
  } else {
    assert(0);
  }
  return dup;
}

void PTree::markInactive(Node *n) {
  for (; n; n = n->parent) {
    if (!n->active)
      return;

    if (n->left && n->left->active)
      return;
    if (n->right && n->right->active)
      return;

    n->active = false;
    for (llvm::SmallVectorImpl<PTreeNode*>::iterator it = n->mergedParents.begin(),
                  ie = n->mergedParents.end(); it != ie; ++it) {
      markInactive(*it);
    }
  }
}

void PTree::markActive(Node *n) {
  for (; n; n = n->parent) {
    if (n->active)
      return;

    n->active = true;
    for (llvm::SmallVectorImpl<PTreeNode*>::iterator it = n->mergedParents.begin(),
                  ie = n->mergedParents.end(); it != ie; ++it) {
      markInactive(*it);
    }
  }
}

void PTree::terminate(Node *n) {
  if(n->state == PTreeNode::MERGED)
    return;

  assert(!n->left && !n->right);

  n->state = PTreeNode::TERMINATED;
  markInactive(n);
}

void PTree::dump(std::ostream &os) {
  ExprPPrinter *pp = ExprPPrinter::create(os);
  pp->setNewline("\\l");
  os << "digraph G {\n";
  os << "\tsize=\"10,7.5\";\n";
  os << "\tratio=fill;\n";
  os << "\trotate=90;\n";
  os << "\tcenter = \"true\";\n";
  os << "\tnode [style=\"filled\",width=.1,height=.1,fontname=\"Terminus\"]\n";
  os << "\tedge [arrowsize=.3]\n";
  std::vector<PTree::Node*> stack;
  stack.push_back(root);
  while (!stack.empty()) {
    PTree::Node *n = stack.back();
    stack.pop_back();

    if (true || n->condition.isNull()) {
      os << "\tn" << n << " [label=\"\"";
    } else {
      os << "\tn" << n << " [label=\"";
      pp->print(n->condition);
      os << "\",shape=diamond";
    }
    if (n->state == PTreeNode::RUNNING)
      os << ",fillcolor=green";
    else if(n->state == PTreeNode::TERMINATED)
      os << ",fillcolor=red";
    os << "];\n";
    if (n->left) {
      os << "\tn" << n << " -> n" << n->left << ";\n";
      //if(n->state != PTreeNode::MERGED)
        stack.push_back(n->left);
    }
    if (n->right) {
      os << "\tn" << n << " -> n" << n->right << ";\n";
      if(n->state != PTreeNode::MERGED || n->right->parent == n)
        stack.push_back(n->right);
    }
  }
  os << "}\n";
  delete pp;
}

PTreeNode::PTreeNode(PTreeNode *_parent, 
                     ExecutionState *_data) 
  : parent(_parent),
    left(0),
    right(0),
    data(_data),
    condition(0),
    state(RUNNING),
    active(true),
    forkTag(KLEE_FORK_DEFAULT) {
}

PTreeNode::~PTreeNode() {
}

