/*
 * ReconstructionJob.h
 *
 *  Created on: Mar 10, 2011
 *      Author: stefan
 */

#ifndef RECONSTRUCTIONJOB_H_
#define RECONSTRUCTIONJOB_H_

#include "cloud9/worker/TreeNodeInfo.h"
#include "cloud9/ExecutionPath.h"

#include <list>
#include <map>

namespace cloud9 {

namespace worker {

struct ReconstructionTask {
  bool isMerge;
  unsigned long offset;
  WorkerTree::NodePin node1; // Skeleton pin
  unsigned node1index;
  WorkerTree::NodePin node2; // Skeleton pin
  unsigned node2index;

  ReconstructionTask() : isMerge(false), offset(0),
      node1(WORKER_LAYER_SKELETON), node1index(0),
      node2(WORKER_LAYER_SKELETON), node2index(0) { }

  ReconstructionTask(WorkerTree *tree, bool _isMerge, unsigned long _offset, WorkerTree::Node *_node1,
      WorkerTree::Node *_node2) : isMerge(_isMerge), offset(_offset),
      node1(WORKER_LAYER_SKELETON), node1index(0),
      node2(WORKER_LAYER_SKELETON), node2index(0) {

    node1 = tree->getNode(WORKER_LAYER_SKELETON, _node1)->pin(WORKER_LAYER_SKELETON);
    if (isMerge)
      node2 = tree->getNode(WORKER_LAYER_SKELETON, _node2)->pin(WORKER_LAYER_SKELETON);
  }

  ReconstructionTask(bool _isMerge, unsigned long _offset, unsigned _node1index,
      unsigned _node2index) : isMerge(_isMerge), offset(_offset),
      node1(WORKER_LAYER_SKELETON), node1index(_node1index),
      node2(WORKER_LAYER_SKELETON), node2index(_node2index) { }
};

class JobReconstruction {
public:
  JobReconstruction() { };
  virtual ~JobReconstruction() { };

  std::list<ReconstructionTask> tasks;

  void appendNodes(std::set<WorkerTree::Node*> &nodes) {
    for (std::list<ReconstructionTask>::iterator it = tasks.begin();
        it != tasks.end(); it++) {
      nodes.insert(it->node1.get());
      if (it->isMerge)
        nodes.insert(it->node2.get());
    }
  }

  void encode(std::map<WorkerTree::Node*, unsigned> &encodeMap) {
    for (std::list<ReconstructionTask>::iterator it = tasks.begin();
        it != tasks.end(); it++) {
      it->node1index = encodeMap[it->node1.get()];
      if (it->isMerge)
        it->node2index = encodeMap[it->node2.get()];
    }
  }

  void decode(WorkerTree *tree, std::map<unsigned, WorkerTree::Node*> &decodeMap) {
    for (std::list<ReconstructionTask>::iterator it = tasks.begin();
        it != tasks.end(); it++) {
      it->node1 = tree->getNode(WORKER_LAYER_SKELETON, decodeMap[it->node1index])->pin(WORKER_LAYER_SKELETON);
      if (it->isMerge)
        it->node2 = tree->getNode(WORKER_LAYER_SKELETON, decodeMap[it->node2index])->pin(WORKER_LAYER_SKELETON);
    }
  }

  static void getDefaultReconstruction(ExecutionPathSetPin paths,
      std::map<unsigned, JobReconstruction*> &reconstructions) {

    for (unsigned i = 0; i < paths->count(); i++) {
      JobReconstruction *job = new JobReconstruction();
      job->tasks.push_back(ReconstructionTask(false, 0, i, 0));

      reconstructions[i] = job;
    }
  }
};

}

}

#endif /* RECONSTRUCTIONJOB_H_ */
