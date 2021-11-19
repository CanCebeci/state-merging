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

/*
 * Implementation invariants, between two consecutive job executions, when the
 * job lock is set:
 * - Every time in the tree, there is a full frontier of symbolic states.
 *
 * - A job can be either on the frontier, or ahead of it (case in which
 * replaying needs to be done). XXX: Perform a more clever way of replay.
 *
 * - An exported job will leave the frontier intact.
 *
 * - A job import cannot happen in such a way that a job lies within the limits
 * of the frontier.
 *
 */

#include "cloud9/worker/JobManager.h"
#include "cloud9/worker/TreeObjects.h"
#include "cloud9/worker/WorkerCommon.h"
#include "cloud9/worker/KleeCommon.h"
#include "cloud9/worker/CoreStrategies.h"
#include "cloud9/worker/ComplexStrategies.h"
#include "cloud9/worker/OracleStrategy.h"
#include "cloud9/worker/FIStrategy.h"
#include "cloud9/worker/PartitioningStrategy.h"
#include "cloud9/worker/LazyMergingStrategy.h"
#include "cloud9/Logger.h"
#include "cloud9/Common.h"
#include "cloud9/ExecutionTree.h"
#include "cloud9/instrum/InstrumentationManager.h"
#include "cloud9/instrum/LocalFileWriter.h"
#include "cloud9/instrum/Timing.h"

#include "llvm/Function.h"
#include "llvm/Module.h"
#include "llvm/Instructions.h"
#if (LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 9)
#include "llvm/System/TimeValue.h"
#else
#include "llvm/Support/TimeValue.h"
#endif
#if (LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 9)
#include "llvm/System/Path.h"
#else
#include "llvm/Support/Path.h"
#endif
#include "llvm/Support/CommandLine.h"
#if !(LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 7)
#include "llvm/Support/raw_os_ostream.h"
#endif

#include "klee/Interpreter.h"
#include "klee/Statistics.h"
#include "klee/ExecutionState.h"
#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "klee/Internal/Module/KModule.h"
#include "klee/Internal/System/Time.h"
#include "klee/Internal/ADT/RNG.h"
#include "klee/util/ExprPPrinter.h"
#include "klee/KleeHandler.h"
#include "klee/Init.h"
#include "klee/Constants.h"

#include "../../Core/Common.h"

#include <boost/io/ios_state.hpp>
#include <boost/crc.hpp>
#include <boost/bind.hpp>
#include <stack>
#include <map>
#include <set>
#include <fstream>
#include <iostream>
#include <iomanip>

using llvm::sys::TimeValue;
using cloud9::instrum::Timer;
using namespace llvm;

namespace klee {
namespace stats {
extern Statistic locallyCoveredInstructions;
extern Statistic globallyCoveredInstructions;
extern Statistic locallyUncoveredInstructions;
extern Statistic globallyUncoveredInstructions;
}
}

namespace {
/* Job Selection Settings *****************************************************/

cl::opt<bool> StratRandom("c9-job-random", cl::desc("Use random job selection"));
cl::opt<bool> StratRandomPath("c9-job-random-path" , cl::desc("Use random path job selection"));
cl::opt<bool> StratCovOpt("c9-job-cov-opt", cl::desc("Use coverage optimized job selection"));
cl::opt<bool> StratOracle("c9-job-oracle", cl::desc("Use the almighty oracle"));
cl::opt<bool> StratFaultInj("c9-job-fault-inj", cl::desc("Use fault injection"));

/* Other Settings *************************************************************/

cl::opt<unsigned> MakeConcreteSymbolic("make-concrete-symbolic", cl::desc(
  "Rate at which to make concrete reads symbolic (0=off)"), cl::init(0));

cl::opt<bool> OptimizeModule("optimize", cl::desc("Optimize before execution"));

cl::opt<bool> CheckDivZero("check-div-zero", cl::desc(
  "Inject checks for division-by-zero"), cl::init(true));

cl::list<unsigned int> CodeBreakpoints("c9-code-bp", cl::desc(
  "Breakpoints in the LLVM assembly file"));

cl::opt<bool> BreakOnReplayBroken("c9-bp-on-replaybr", cl::desc(
  "Break on last valid position if a broken replay occurrs."));

cl::opt<bool>
  DumpStateTraces("c9-dump-traces",
  cl::desc("Dump state traces when a breakpoint or any other relevant event happens during execution"),
  cl::init(false));

cl::opt<bool>
  DumpInstrTraces("c9-dump-instr",
  cl::desc("Dump instruction traces for each finished state. Designed to be used for concrete executions."),
  cl::init(false));

cl::opt<bool>
  DebugJobReconstruction("debug-job-reconstruction",
      cl::desc("Dump job reconstruction debug information"),
      cl::init(false));

cl::opt<std::string>
  OraclePath("c9-oracle-path", cl::desc("The file used by the oracle strategy to get the path."));

cl::opt<double>
  JobQuanta("c9-job-quanta", cl::desc("The maximum quantum of time for a job"),
  cl::init(1.0));

cl::opt<bool>
  InjectFaults("c9-fault-inj", cl::desc("Fork at fault injection points"),
      cl::init(false));

cl::opt<bool>
  ZombieNodes("zombie-nodes", cl::desc("Preserve the structure of the paths that finished."),
      cl::init(false));

cl::opt<unsigned> FlowSizeLimit("flow-size-limit", cl::init(10));

}

using namespace klee;

namespace cloud9 {

namespace worker {

/*******************************************************************************
 * HELPER FUNCTIONS FOR THE JOB MANAGER
 ******************************************************************************/

StateSelectionStrategy *JobManager::createCoverageOptimizedStrat(StateSelectionStrategy *base) {
  std::vector<StateSelectionStrategy*> strategies;

  strategies.push_back(new WeightedRandomStrategy(
         WeightedRandomStrategy::CoveringNew,
         tree,
         symbEngine));
  strategies.push_back(base);
  return new TimeMultiplexedStrategy(strategies);
}

bool JobManager::isJob(WorkerTree::Node *node) {
  return (**node).getJob() != NULL;
}

bool JobManager::isExportableJob(WorkerTree::Node *node) {
  ExecutionJob *job = (**node).getJob();
  if (!job)
    return false;

  if (job == currentJob)
    return false;

  return true;
}

bool JobManager::isValidJob(WorkerTree::Node *node) {
  assert(node->layerExists(WORKER_LAYER_JOBS));
  while (node != NULL) {
    if (node->layerExists(WORKER_LAYER_STATES)) {
      if ((**node).getSymbolicState() != NULL)
        return true;
      else
        return false;
    } else {
      node = node->getParent();
    }
  }

  return false;
}

void JobManager::serializeInstructionTrace(std::ostream &s,
    WorkerTree::Node *node) {
  std::vector<int> path;

  tree->buildPath(WORKER_LAYER_STATES, node, tree->getRoot(), path);

  const WorkerTree::Node *crtNode = tree->getRoot();
  unsigned int count = 0;

  bool enabled = false;

  for (unsigned int i = 0; i <= path.size(); i++) {
    const ExecutionTrace &trace = (**crtNode).getTrace();

    for (ExecutionTrace::const_iterator it = trace.getEntries().begin();
        it != trace.getEntries().end(); it++) {

      if (InstructionTraceEntry *instEntry = dyn_cast<InstructionTraceEntry>(*it)) {
        if (enabled) {
          unsigned int opcode = instEntry->getInstruction()->inst->getOpcode();
          s.write((char*)&opcode, sizeof(unsigned int));
        }
        count++;
      } else if (BreakpointEntry *brkEntry = dyn_cast<BreakpointEntry>(*it)) {
        if (!enabled && brkEntry->getID() == KLEE_BRK_START_TRACING) {
          CLOUD9_DEBUG("Starting to serialize. Skipped " << count << " instructions.");
          enabled = true;
        }
      }
    }

    if (i < path.size()) {
      crtNode = crtNode->getChild(WORKER_LAYER_STATES, path[i]);
      assert(crtNode);
    }
  }

  CLOUD9_DEBUG("Serialized " << count << " instructions.");
}

void JobManager::parseInstructionTrace(std::istream &s,
    std::vector<unsigned int> &dest) {

  dest.clear();

  while (!s.eof()) {
    unsigned int instID;

    s.read((char*)&instID, sizeof(unsigned int));

    if (!s.fail()) {
      dest.push_back(instID);
    }
  }

  CLOUD9_DEBUG("Parsed " << dest.size() << " instructions.");
}

void JobManager::serializeExecutionTrace(std::ostream &os,
    WorkerTree::Node *node) { // XXX very slow - read the .ll file and use it instead
  std::vector<int> path;

  tree->buildPath(WORKER_LAYER_STATES, node, tree->getRoot(), path);

  WorkerTree::Node *crtNode = tree->getRoot();
  const llvm::BasicBlock *crtBasicBlock = NULL;
  const llvm::Function *crtFunction = NULL;

  llvm::raw_os_ostream raw_os(os);

  for (unsigned int i = 0; i <= path.size(); i++) {
    const ExecutionTrace &trace = (**crtNode).getTrace();
    // Output each instruction in the node
    for (ExecutionTrace::const_iterator it = trace.getEntries().begin(); it
        != trace.getEntries().end(); it++) {
      if (InstructionTraceEntry *instEntry = dyn_cast<InstructionTraceEntry>(*it)) {
        klee::KInstruction *ki = instEntry->getInstruction();
        bool newBB = false;
        bool newFn = false;

        if (ki->inst->getParent() != crtBasicBlock) {
          crtBasicBlock = ki->inst->getParent();
          newBB = true;
        }

        if (crtBasicBlock != NULL && crtBasicBlock->getParent() != crtFunction) {
          crtFunction = crtBasicBlock->getParent();
          newFn = true;
        }

        if (newFn) {
          os << std::endl;
          os << "   Function '"
              << ((crtFunction != NULL) ? crtFunction->getNameStr() : "")
              << "':" << std::endl;
        }

        if (newBB) {
          os << "----------- "
              << ((crtBasicBlock != NULL) ? crtBasicBlock->getNameStr() : "")
              << " ----" << std::endl;
        }

        boost::io::ios_all_saver saver(os);
        os << std::setw(9) << ki->info->assemblyLine << ": ";
        saver.restore();
        ki->inst->print(raw_os, NULL);
        os << std::endl;
      } else if (DebugLogEntry *logEntry = dyn_cast<DebugLogEntry>(*it)) {
        os << logEntry->getMessage() << std::endl;
      }
    }

    if (i < path.size()) {
      crtNode = crtNode->getChild(WORKER_LAYER_STATES, path[i]);
    }
  }
}

void JobManager::serializeExecutionTrace(std::ostream &os, SymbolicState *state) {
  WorkerTree::Node *node = state->getNode().get();
  serializeExecutionTrace(os, node);
}

void JobManager::processTestCase(SymbolicState *state) {
  std::vector<EventEntry*> eventEntries;

  // First, collect the event entries
  std::vector<int> path;

  tree->buildPath(WORKER_LAYER_STATES, state->getNode().get(), tree->getRoot(), path);

  const WorkerTree::Node *crtNode = tree->getRoot();

  for (unsigned int i = 0; i <= path.size(); i++) {
    const ExecutionTrace &trace = (**crtNode).getTrace();

    for (ExecutionTrace::const_iterator it = trace.getEntries().begin();
        it != trace.getEntries().end(); it++) {

      if (EventEntry *eventEntry = dyn_cast<EventEntry>(*it)) {
        eventEntries.push_back(eventEntry);
      }
    }

    if (i < path.size()) {
      crtNode = crtNode->getChild(WORKER_LAYER_STATES, path[i]);
      assert(crtNode);
    }
  }

  if (eventEntries.size() == 0)
    return;

  std::ostream *f = kleeHandler->openTestFile("events");

  if (f) {
    for (std::vector<EventEntry*>::iterator it = eventEntries.begin();
        it != eventEntries.end(); it++) {
      EventEntry *event = *it;
      *f << "Event: " << event->getType() << " Value: " << event->getValue() << std::endl;
      event->getStackTrace().dump(*f);
      *f << std::endl;
    }
    delete f;
  }
}

/*******************************************************************************
 * JOB MANAGER METHODS
 ******************************************************************************/

/* Initialization Methods *****************************************************/

JobManager::JobManager(llvm::Module *module, std::string mainFnName, int argc,
    char **argv, char **envp) :
  terminationRequest(false), jobCount(0), currentJob(NULL), currentState(NULL),
  replaying(false), batching(true), traceCounter(0) {

  tree = new WorkerTree();

  llvm::Function *mainFn = module->getFunction(mainFnName);

  collectTraces = DumpStateTraces || DumpInstrTraces;

  initialize(module, mainFn, argc, argv, envp);
}

void JobManager::initialize(llvm::Module *module, llvm::Function *_mainFn,
    int argc, char **argv, char **envp) {
  mainFn = _mainFn;
  assert(mainFn);

  klee::Interpreter::InterpreterOptions iOpts;
  iOpts.MakeConcreteSymbolic = MakeConcreteSymbolic;

  llvm::sys::Path libraryPath(getKleeLibraryPath());

  klee::Interpreter::ModuleOptions mOpts(libraryPath.c_str(),
  /*Optimize=*/OptimizeModule,
  /*CheckDivZero=*/CheckDivZero);

  kleeHandler = new klee::KleeHandler(argc, argv);
  interpreter = klee::Interpreter::create(iOpts, kleeHandler);
  kleeHandler->setInterpreter(interpreter);

  symbEngine = dyn_cast<SymbolicEngine> (interpreter);
  interpreter->setModule(module, mOpts);

  kleeModule = symbEngine->getModule();

  klee::externalsAndGlobalsCheck(kleeModule->module);

  symbEngine->registerStateEventHandler(this);

  theStatisticManager->trackChanges(stats::locallyCoveredInstructions);

  initStrategy();
  initStatistics();
  initBreakpoints();

  initRootState(mainFn, argc, argv, envp);
}

void JobManager::initRootState(llvm::Function *f, int argc, char **argv,
    char **envp) {
  klee::ExecutionState *kState = symbEngine->createRootState(f);
  SymbolicState *state = new SymbolicState(kState, NULL);

  state->rebindToNode(tree->getRoot());

  symbEngine->initRootState(kState, argc, argv, envp);
}

StateSelectionStrategy *JobManager::createBaseStrategy() {
  // Step 1: Compose the basic strategy
  StateSelectionStrategy *stateStrat = NULL;

  if (StratRandomPath) {
    stateStrat = new RandomPathStrategy(tree);
  } else {
    stateStrat = new RandomStrategy();
  }

  // Step 2: Check to see if want the coverage-optimized strategy
  if (StratCovOpt) {
    stateStrat = createCoverageOptimizedStrat(stateStrat);
  }

  return stateStrat;
}

void JobManager::initStrategy() {
  if (StratOracle)
    batching = false;

  if (StratOracle) {
    selStrategy = createOracleStrategy();
    CLOUD9_INFO("Using the oracle");
    return;
  }

  StateSelectionStrategy *stateStrat = NULL;

  stateStrat = createBaseStrategy();
  CLOUD9_INFO("Using the regular strategy stack");

  selStrategy = new RandomJobFromStateStrategy(tree, stateStrat, this);
}

OracleStrategy *JobManager::createOracleStrategy() {
  std::vector<unsigned int> goalPath;

  std::ifstream ifs(OraclePath.c_str());

  assert(!ifs.fail());

  parseInstructionTrace(ifs, goalPath);

  OracleStrategy *result = new OracleStrategy(tree, goalPath, this);

  return result;
}

void JobManager::initStatistics() {
  // Configure the root as a statistics node
  WorkerTree::NodePin rootPin = tree->getRoot()->pin(WORKER_LAYER_STATISTICS);

  stats.insert(rootPin);
  statChanged = true;
  refineStats = false;
}

void JobManager::initBreakpoints() {
  // Register code breakpoints
  for (unsigned int i = 0; i < CodeBreakpoints.size(); i++) {
    setCodeBreakpoint(CodeBreakpoints[i]);
  }
}

/* Finalization Methods *******************************************************/

JobManager::~JobManager() {
  if (symbEngine != NULL) {
    delete symbEngine;
  }

  cloud9::instrum::theInstrManager.stop();
}

void JobManager::finalize() {
  dumpSymbolicTree(NULL, WorkerNodeDecorator(NULL));
  symbEngine->deregisterStateEventHandler(this);
  symbEngine->destroyStates();

  CLOUD9_INFO("Finalized job execution.");
}

/* Misc. Methods **************************************************************/

unsigned JobManager::getModuleCRC() const {
  std::string moduleContents;
  llvm::raw_string_ostream os(moduleContents);

  kleeModule->module->print(os, NULL);

  os.flush();

  boost::crc_ccitt_type crc;
  crc.process_bytes(moduleContents.c_str(), moduleContents.size());

  return crc.checksum();
}

/* Job Manipulation Methods ***************************************************/

void JobManager::processJobs(bool standAlone, unsigned int timeOut) {
  if (timeOut > 0) {
    CLOUD9_INFO("Processing jobs with a timeout of " << timeOut << " seconds.");
  }

  if (standAlone) {
    // We need to import the root job
    std::vector<long> replayInstrs;
    std::map<unsigned, JobReconstruction*> reconstructions;
    ExecutionPathSetPin pathSet = ExecutionPathSet::getRootSet();
    JobReconstruction::getDefaultReconstruction(pathSet, reconstructions);

    importJobs(ExecutionPathSet::getRootSet(), reconstructions);
  }

  processLoop(true, !standAlone, timeOut);
}

void JobManager::replayJobs(ExecutionPathSetPin paths, unsigned int timeOut) {
  // First, we need to import the jobs in the manager
  std::map<unsigned, JobReconstruction*> reconstructions;
  JobReconstruction::getDefaultReconstruction(paths, reconstructions);
  importJobs(paths, reconstructions);

  // Then we execute them, but only them (non blocking, don't allow growth),
  // until the queue is exhausted
  processLoop(false, false, timeOut);
}

void JobManager::processLoop(bool allowGrowth, bool blocking,
    unsigned int timeOut) {
  ExecutionJob *job = NULL;

  boost::unique_lock<boost::mutex> lock(jobsMutex);

  TimeValue deadline = TimeValue::now() + TimeValue(timeOut, 0);

  while (!terminationRequest) {
    TimeValue now = TimeValue::now();

    bool canBatch = false;
    uint32_t batchDest = 0;

    if (timeOut > 0 && now > deadline) {
      CLOUD9_INFO("Timeout reached. Suspending execution.");
      break;
    }

    if (blocking) {
      if (timeOut > 0) {
        TimeValue remaining = deadline - now;

        job = selectNextJob(lock, remaining.seconds(), canBatch, batchDest);
      } else {
        job = selectNextJob(lock, 0, canBatch, batchDest);
      }
    } else {
      job = selectNextJob(canBatch, batchDest);
    }

    if (blocking && timeOut == 0 && !terminationRequest) {
      assert(job != NULL);
    } else {
      if (job == NULL)
        break;
    }

    executeJobsBatch(lock, job, allowGrowth, canBatch, batchDest);

    if (refineStats) {
      refineStatistics();
      refineStats = false;
    }
  }

  if (terminationRequest)
    CLOUD9_INFO("Termination was requested.");
}

ExecutionJob* JobManager::selectNextJob(boost::unique_lock<boost::mutex> &lock,
    unsigned int timeOut, bool &canBatch, uint32_t &batchDest) {
  ExecutionJob *job = selectNextJob(canBatch, batchDest);
  assert(job != NULL || jobCount == 0);

  while (job == NULL && !terminationRequest) {
    CLOUD9_INFO("No jobs in the queue, waiting for...");
    cloud9::instrum::theInstrManager.recordEvent(
        cloud9::instrum::JobExecutionState, "idle");

    bool result = true;
    if (timeOut > 0)
      result = jobsAvailabe.timed_wait(lock,
          boost::posix_time::seconds(timeOut));
    else
      jobsAvailabe.wait(lock);

    if (!result) {
      CLOUD9_INFO("Timeout while waiting for new jobs. Aborting.");
      return NULL;
    } else
      CLOUD9_INFO("More jobs available. Resuming exploration...");

    job = selectNextJob(canBatch, batchDest);

    if (job != NULL)
      cloud9::instrum::theInstrManager.recordEvent(
          cloud9::instrum::JobExecutionState, "working");
  }

  return job;
}


ExecutionJob* JobManager::selectNextJob(bool &canBatch, uint32_t &batchDest) {
  ExecutionJob *job = selStrategy->onNextJobSelectionEx(canBatch, batchDest);

  if (!StratOracle)
    assert(job != NULL || jobCount == 0);

  return job;
}

void JobManager::submitJob(ExecutionJob* job, bool activateStates) {
  WorkerTree::Node *node = job->getNode().get();
  assert((**node).symState || job->reconstruct);

  fireAddJob(job);

  if (activateStates) {
    // Check for the state on the supporting branch
    while (node) {
      SymbolicState *state = (**node).getSymbolicState();

      if (state) {
        if (!state->_active) {
          cloud9::instrum::theInstrManager.decStatistic(
              cloud9::instrum::TotalWastedInstructions,
              state->_instrSinceFork);
        }
        fireActivateState(state);
        break;
      }

      node = node->getParent();
    }
  }

  jobCount++;

}

void JobManager::finalizeJob(ExecutionJob *job, bool deactivateStates, bool invalid) {
  WorkerTree::Node *node = job->getNode().get();

  if (deactivateStates) {
    while (node) {
      if (node->getCount(WORKER_LAYER_JOBS) > 1)
        break; // We reached a junction

      SymbolicState *state = (**node).getSymbolicState();

      if (state) {
        fireDeactivateState(state);
        cloud9::instrum::theInstrManager.incStatistic(
            cloud9::instrum::TotalWastedInstructions,
            state->_instrSinceFork);
        break;
      }

      node = node->getParent();
    }
  }

  if (currentJob == job) {
    currentJob = NULL;
  }

  fireRemovingJob(job);
  delete job;

  jobCount--;
  if (invalid) {
    cloud9::instrum::theInstrManager.incStatistic(cloud9::instrum::TotalDroppedJobs);
  }
}

void JobManager::selectJobs(WorkerTree::Node *root,
    std::vector<ExecutionJob*> &jobSet, int maxCount) {

  std::vector<WorkerTree::Node*> nodes;

  tree->getLeaves(WORKER_LAYER_JOBS, root, boost::bind(
      &JobManager::isExportableJob, this, _1), maxCount, nodes);

  for (std::vector<WorkerTree::Node*>::iterator it = nodes.begin(); it
      != nodes.end(); it++) {
    WorkerTree::Node* node = *it;
    jobSet.push_back((**node).getJob());
  }
}

unsigned int JobManager::countJobs(WorkerTree::Node *root) {
  return tree->countLeaves(WORKER_LAYER_JOBS, root, &isJob);
}

void JobManager::importJobs(ExecutionPathSetPin paths,
    std::map<unsigned,JobReconstruction*> &reconstruct) {
  boost::unique_lock<boost::mutex> lock(jobsMutex);

  std::vector<WorkerTree::Node*> nodes;
  std::map<unsigned, WorkerTree::Node*> decodeMap;
  std::vector<ExecutionJob*> jobs;

  tree->getNodes(WORKER_LAYER_SKELETON, paths, nodes, &decodeMap);

  CLOUD9_DEBUG("Importing " << reconstruct.size() << " jobs");

  unsigned droppedCount = 0;

  for (std::map<unsigned,JobReconstruction*>::iterator it = reconstruct.begin();
      it != reconstruct.end(); it++) {
    WorkerTree::Node *crtNode = decodeMap[it->first];

    assert(crtNode->getCount(WORKER_LAYER_JOBS) == 0
        && "Job duplication detected");
    assert((!crtNode->layerExists(WORKER_LAYER_STATES) || crtNode->getCount(WORKER_LAYER_STATES) == 0)
        && "Job before the state frontier");

    it->second->decode(tree, decodeMap);

    // The exploration job object gets a pin on the node, thus
    // ensuring it will be released automatically after it's no
    // longer needed
    ExecutionJob *job = new ExecutionJob(tree->getNode(WORKER_LAYER_JOBS, crtNode));
    job->reconstruct = it->second;

    if (!isValidJob(crtNode)) {
      droppedCount++;

      delete job;
    } else {
      jobs.push_back(job);
    }
  }

  if (droppedCount > 0) {
    CLOUD9_DEBUG("BUG! " << droppedCount << " jobs dropped before being imported.");
  }

  submitJobs(jobs.begin(), jobs.end(), true);

  cloud9::instrum::theInstrManager.incStatistic(
      cloud9::instrum::TotalImportedJobs, jobs.size());
  cloud9::instrum::theInstrManager.incStatistic(
      cloud9::instrum::TotalTreePaths, jobs.size());
}

ExecutionPathSetPin JobManager::exportJobs(ExecutionPathSetPin seeds,
    std::vector<int> &counts,
    std::map<unsigned,JobReconstruction*> &reconstruct) {

  boost::unique_lock<boost::mutex> lock(jobsMutex);

  std::vector<WorkerTree::Node*> roots;
  std::vector<ExecutionJob*> jobs;
  std::map<WorkerTree::Node*, JobReconstruction*> nodeReconMap;
  std::map<WorkerTree::Node*, unsigned> encodeMap;
  std::set<WorkerTree::Node*> nodeSet;

  // First, collect the set of jobs we'll be working with
  tree->getNodes(WORKER_LAYER_JOBS, seeds, roots, (std::map<unsigned,WorkerTree::Node*>*)NULL);

  for (unsigned int i = 0; i < seeds->count(); i++) {
    selectJobs(roots[i], jobs, (counts.size() > 0) ? counts[i] : 0);
  }

  for (std::vector<ExecutionJob*>::iterator it = jobs.begin(); it != jobs.end(); it++) {
    ExecutionJob *job = *it;
    WorkerTree::Node *node = job->getNode().get();
    JobReconstruction *reconJob = getJobReconstruction(job);

    nodeReconMap[node] = reconJob;
    reconJob->appendNodes(nodeSet);
  }

  // Do this before de-registering the jobs, in order to keep the nodes pinned
  ExecutionPathSetPin paths = tree->buildPathSet(nodeSet.begin(),
      nodeSet.end(), &encodeMap);

  for (std::map<WorkerTree::Node*, JobReconstruction*>::iterator it = nodeReconMap.begin();
      it != nodeReconMap.end(); it++) {
    reconstruct[encodeMap[it->first]] = it->second;
    it->second->encode(encodeMap);
  }

  CLOUD9_DEBUG("Exporting " << jobs.size() << " jobs");

  // De-register the jobs with the worker
  for (std::vector<ExecutionJob*>::iterator it = jobs.begin(); it != jobs.end(); it++) {
    // Cancel each job
    ExecutionJob *job = *it;
    assert(currentJob != job);

    finalizeJob(job, true, false);
  }

  cloud9::instrum::theInstrManager.incStatistic(
      cloud9::instrum::TotalExportedJobs, paths->count());
  cloud9::instrum::theInstrManager.decStatistic(
      cloud9::instrum::TotalTreePaths, paths->count());

  return paths;
}

/* Strategy Handler Triggers *************************************************/

void JobManager::fireActivateState(SymbolicState *state) {
  if (!state->_active) {
    state->_active = true;

    selStrategy->onStateActivated(state);
    cloud9::instrum::theInstrManager.incStatistic(
        cloud9::instrum::CurrentActiveStateCount);
  }
}

void JobManager::fireDeactivateState(SymbolicState *state) {
  if (state->_active) {
    state->_active = false;

    selStrategy->onStateDeactivated(state);
    cloud9::instrum::theInstrManager.decStatistic(
        cloud9::instrum::CurrentActiveStateCount);
  }
}

void JobManager::fireUpdateState(SymbolicState *state, WorkerTree::Node *oldNode) {
  if (state->_active) {
    selStrategy->onStateUpdated(state, oldNode);
  }
}

void JobManager::fireStepState(SymbolicState *state) {
  if (state->_active) {
    selStrategy->onStateStepped(state);
  }
}

void JobManager::fireAddJob(ExecutionJob *job) {
  selStrategy->onJobAdded(job);
  cloud9::instrum::theInstrManager.incStatistic(
      cloud9::instrum::CurrentJobCount);
}

void JobManager::fireRemovingJob(ExecutionJob *job) {
  selStrategy->onRemovingJob(job);
  cloud9::instrum::theInstrManager.decStatistic(
      cloud9::instrum::CurrentJobCount);
}

/* Job Execution Methods ******************************************************/

void JobManager::executeJobsBatch(boost::unique_lock<boost::mutex> &lock,
      ExecutionJob *origJob, bool spawnNew, bool canBatch, uint32_t batchDest) {
  Timer timer;
  timer.start();

  WorkerTree::NodePin nodePin = origJob->getNode();

  if (origJob->reconstruct) {
    // Replay job
    executeJob(lock, origJob, canBatch, batchDest);
    return;
  }

  double startTime = klee::util::getUserTime();
  double currentTime = startTime;

  do {
    executeJob(lock, (**nodePin).getJob(), canBatch, batchDest);

    if ((**nodePin).getJob() == NULL) {
      WorkerTree::Node *newNode = tree->selectRandomLeaf(WORKER_LAYER_JOBS, nodePin.get(), theRNG);

      if ((**newNode).getJob() == NULL)
        break;

      nodePin = (**newNode).getJob()->getNode();
    }

    currentTime = klee::util::getUserTime();

    if (!batching) {
      SymbolicState *state = (**nodePin).getSymbolicState();
      if (state && !(**state).pc()->isBBHead)
        continue;
    }
  } while (batching && currentTime - startTime < JobQuanta);

  timer.stop();

  cloud9::instrum::theInstrManager.recordEvent(
      cloud9::instrum::InstructionBatch, timer);

  //CLOUD9_DEBUG("Batched " << count << " jobs");
}

void JobManager::executeJob(boost::unique_lock<boost::mutex> &lock,
    ExecutionJob *job, bool canBatch, uint32_t batchDest) {
  WorkerTree::NodePin nodePin = job->getNode(); // Keep the node around until we finish with it

  currentJob = job;

  if (job->reconstruct) {
    JobReconstruction *reconstruct = job->reconstruct;
    job->reconstruct = NULL;
    cloud9::instrum::theInstrManager.recordEvent(
        cloud9::instrum::JobExecutionState, "startReplay");

    runJobReconstruction(lock, reconstruct);

    // Release all the associated pins
    delete reconstruct;

    cloud9::instrum::theInstrManager.recordEvent(
        cloud9::instrum::JobExecutionState, "endReplay");

    cloud9::instrum::theInstrManager.incStatistic(
        cloud9::instrum::TotalReplayedJobs);
  }

  if (currentJob) {
    if (!(**job->getNode().get()).getSymbolicState()) {
      CLOUD9_DEBUG("BUG! Could not replay the state to the job position.");
      if (job->getNode()->layerExists(WORKER_LAYER_STATES)) {
        dumpSymbolicTree(NULL, WorkerNodeDecorator(nodePin.get()));
      }
      finalizeJob(job, false, true);
    } else {
      stepInNode(lock, nodePin.get(), 1/*canBatch ? -1 : 1*/, batchDest);
    }
  }

  currentJob = NULL;
}

void JobManager::cleanInvalidJobs(WorkerTree::Node *rootNode) {
  std::vector<WorkerTree::Node*> jobNodes;

  if (!rootNode->layerExists(WORKER_LAYER_JOBS))
    return;

  tree->getLeaves(WORKER_LAYER_JOBS, rootNode, jobNodes);

  CLOUD9_DEBUG("BUG! Cleaning " << jobNodes.size() << " broken jobs.");

  for (std::vector<WorkerTree::Node*>::iterator it = jobNodes.begin();
      it != jobNodes.end(); it++) {
    WorkerTree::Node *node = *it;
    assert(!node->layerExists(WORKER_LAYER_STATES));

    ExecutionJob *job = (**node).job;
    if (job != NULL)
      finalizeJob(job, false, true);
  }

}

void JobManager::getReconstructionTasks(WorkerTree::Node *node,
    unsigned long offset, JobReconstruction *job,
    std::set<std::pair<WorkerTree::Node*, unsigned long> > &reconstructed) {
  // XXX Refactor this into something nicer
  WorkerTree::Node *crtNode = node;
  std::vector<WorkerTree::Node*> path;

  while (crtNode != NULL) {
    path.push_back(crtNode);
    crtNode = crtNode->getParent();
  }

  for (std::vector<WorkerTree::Node*>::reverse_iterator it = path.rbegin();
      it != path.rend(); it++) {
    crtNode = *it;

    if ((**crtNode).getMergePoints().size() > 0) {
      // We'll have first to reconstruct the merged states
      WorkerNodeInfo::merge_points_t &mp = (**crtNode).getMergePoints();

      for (WorkerNodeInfo::merge_points_t::iterator it = mp.begin();
          it != mp.end(); it++) {
        std::pair<WorkerTree::Node*, unsigned long> dest, src;
        dest = std::make_pair(crtNode, it->first.first);
        src = std::make_pair(it->second.get(), it->first.second);

        if (reconstructed.count(src) == 0) {
          getReconstructionTasks(src.first, src.second, job, reconstructed);
        }

        if (reconstructed.count(dest) == 0) {
          job->tasks.push_back(ReconstructionTask(tree, false, dest.second, dest.first, NULL));
          if (DebugJobReconstruction)
            CLOUD9_DEBUG("Creating reconstruction job for replay at " << *dest.first << ":" << dest.second);
          reconstructed.insert(dest);

          job->tasks.push_back(ReconstructionTask(tree, true, 0, dest.first, src.first));
          if (DebugJobReconstruction)
            CLOUD9_DEBUG("Creating reconstruction job for merging between " << *dest.first << " and " << *src.first);
        }
      }
    }
  }

  job->tasks.push_back(ReconstructionTask(tree, false, offset, node, NULL));
  reconstructed.insert(std::make_pair(node, offset));
  if (DebugJobReconstruction)
    CLOUD9_DEBUG("Creating reconstruction job for replay at " << *node << ":" << offset);
}

JobReconstruction *JobManager::getJobReconstruction(ExecutionJob *job) {
  if (job->reconstruct) // The job is not yet reconstructed
    return new JobReconstruction(*job->reconstruct);

  WorkerTree::Node *node = job->getNode().get();

  if (DebugJobReconstruction)
    CLOUD9_DEBUG("Getting job reconstruction for " << *node << ":" << (**node).getSymbolicState()->_instrSinceFork);

  assert((**node).getSymbolicState() != NULL);

  JobReconstruction *reconstruct = new JobReconstruction();

  std::set<std::pair<WorkerTree::Node*, unsigned long> > reconstructed;

  getReconstructionTasks(node, (**node).getSymbolicState()->_instrSinceFork,
      reconstruct, reconstructed);

  return reconstruct;
}

void JobManager::runJobReconstruction(boost::unique_lock<boost::mutex> &lock,
    JobReconstruction* job) {
  if (DebugJobReconstruction)
    CLOUD9_DEBUG("Running job reconstruction " << job);

  for (std::list<ReconstructionTask>::iterator it = job->tasks.begin();
      it != job->tasks.end(); it++) {
    if (it->isMerge) {
      if (DebugJobReconstruction)
        CLOUD9_DEBUG("Reconstructing a merge between " << (*it->node1.get()) << " and " << (*it->node2.get()));
      // Attempt state merging
      SymbolicState *dest = (**it->node1).getSymbolicState();
      SymbolicState *src = (**it->node2).getSymbolicState();
      if (dest != NULL && src != NULL) {
        if (!mergeStates(dest, src)) {
          if (DebugJobReconstruction)
            CLOUD9_DEBUG("BUG! Cannot merge states during reconstruction. Losing the source state.");
        }
      }
    } else {
      // Try to replay to this point
      if (DebugJobReconstruction)
        CLOUD9_DEBUG("Reconstructing a state at " << (*it->node1.get()) << ":" << it->offset);
      WorkerTree::Node *brokenNode = NULL;
      replayPath(lock, it->node1.get(), it->offset, brokenNode);
      if (brokenNode != NULL) {
        if (DebugJobReconstruction)
          CLOUD9_DEBUG("BUG! Broken path replay during reconstruction");
        cleanInvalidJobs(brokenNode);
      }
    }
  }
}

void JobManager::requestStateDestroy(SymbolicState *state) {
  pendingDeletions.insert(state);
}

void JobManager::processPendingDeletions() {
  if (pendingDeletions.empty())
    return;

  std::set<SymbolicState*> workingSet;
  workingSet.swap(pendingDeletions);

  for (std::set<SymbolicState*>::iterator it = workingSet.begin();
      it != workingSet.end(); it++) {
    symbEngine->destroyState(&(**(*it)));
  }
}

long JobManager::stepInNode(boost::unique_lock<boost::mutex> &lock,
    WorkerTree::Node *node, long count, uint32_t batchDest) {
  assert((**node).symState != NULL);

  // Keep the node alive until we finish with it
  WorkerTree::NodePin nodePin = node->pin(WORKER_LAYER_STATES);

  long totalExec = 0;

  while ((**node).symState != NULL) {

    SymbolicState *state = (**node).symState;
    currentState = state;

    if (!codeBreaks.empty()) {
      if (codeBreaks.find((**state).pc()->info->assemblyLine)
          != codeBreaks.end()) {
        // We hit a breakpoint
        fireBreakpointHit(node);
      }
    }

    //if (replaying)
    //  fprintf(stderr, "%d ", state->getKleeState()->pc()->info->assemblyLine);

    if (state->collectProgress) {
      state->_instrProgress.push_back((**state).pc());
    }

    // Execute the instruction
    state->_instrSinceFork++;
    lock.unlock();

    processPendingDeletions();
    if (currentState) {
      symbEngine->stepInState(&(**state));
    }

    lock.lock();

    if (currentState) {
      totalExec++;
      cloud9::instrum::theInstrManager.incStatistic(
          cloud9::instrum::TotalProcInstructions);
      if (replaying)
        cloud9::instrum::theInstrManager.incStatistic(
            cloud9::instrum::TotalReplayInstructions);
    }

    if (count == 1) {
      break;
    } else if (count > 1) {
      count--;
    } else {
      if (currentState && (**currentState).getMergeIndex() == batchDest) {
        CLOUD9_DEBUG("Found batching destination!");
        break;
      }
    }
  }

  currentState = NULL;

  return totalExec;
}

void JobManager::replayPath(boost::unique_lock<boost::mutex> &lock,
    WorkerTree::Node *pathEnd, unsigned long offset,
    WorkerTree::Node *&brokenEnd) {
  Timer timer;
  timer.start();

  std::vector<int> path;

  WorkerTree::Node *crtNode = pathEnd;
  brokenEnd = NULL;

  //CLOUD9_DEBUG("Replaying path: " << *crtNode);

  // Find the state to pick from
  while (crtNode != NULL && (**crtNode).symState == NULL) {
    assert(crtNode->layerExists(WORKER_LAYER_SKELETON));
    path.push_back(crtNode->getIndex());

    crtNode = crtNode->getParent();
  }

  if (crtNode == NULL) {
    if (DebugJobReconstruction)
      CLOUD9_DEBUG("Could not find a starting state to replay from.");
    return;
  }

  std::reverse(path.begin(), path.end());

  //CLOUD9_DEBUG("Started path replay at position: " << *crtNode);

  replaying = true;

  // Perform the replay work
  for (unsigned int i = 0; i < path.size(); i++) {
    if (!crtNode->layerExists(WORKER_LAYER_STATES)) {
      if (crtNode->getParent() && crtNode->getParent()->layerExists(WORKER_LAYER_STATES) &&
          crtNode->getParent()->getChild(WORKER_LAYER_STATES, 1-crtNode->getIndex())) {
        CLOUD9_DEBUG("Replay broken because of different branch taken");

        WorkerTree::Node *node = crtNode->getParent()->getChild(WORKER_LAYER_STATES, 1-crtNode->getIndex());
        if ((**node).getSymbolicState()) {
          (**(**node).getSymbolicState()).getStackTrace().dump(std::cout);
        }
      }
      // We have a broken replay
      break;
    }

    if ((**crtNode).symState != NULL) {
      stepInNode(lock, crtNode, -1, 0);
    }

    if (crtNode->layerExists(WORKER_LAYER_JOBS)) {
      if (crtNode->getChild(WORKER_LAYER_JOBS, 1-path[i]) &&
          !crtNode->getChild(WORKER_LAYER_STATES, 1-path[i])) {
        // Broken replays...
        cleanInvalidJobs(crtNode->getChild(WORKER_LAYER_JOBS, 1-path[i]));
      }
    }

    crtNode = crtNode->getChild(WORKER_LAYER_SKELETON, path[i]);
    assert(crtNode != NULL);
  }

  // Now advance the state to the desired offset
  if ((**crtNode).symState != NULL) {
    long count = offset - (**crtNode).symState->_instrSinceFork;
    if (count > 0) {
      long result = stepInNode(lock, crtNode, count, 0);
      if (result != count) {
        CLOUD9_DEBUG("BUG! Could not set the state at the desired offset");
      }
    }
  }

  replaying = false;

  if (!crtNode->layerExists(WORKER_LAYER_STATES)) {
    assert((**crtNode).symState == NULL);
    CLOUD9_ERROR("Replay broken, NULL state at the end of the path.");

    brokenEnd = crtNode;

    if (BreakOnReplayBroken) {
      fireBreakpointHit(crtNode->getParent());
    }
  }

  timer.stop();
  cloud9::instrum::theInstrManager.recordEvent(cloud9::instrum::ReplayBatch, timer);
}

bool JobManager::mergeStates(SymbolicState* dest, SymbolicState *src) {
  // Perform the actual merging
  klee::ExecutionState *mState = symbEngine->merge(**dest, **src);

  if (!mState) {
    CLOUD9_DEBUG("States could not be merged...");
    return false;
  }

  // Record the event...

  WorkerTree::Node *destNode = dest->getNode().get();
  // Pin the skeleton layer on source
  WorkerTree::NodePin srcNodePin = tree->getNode(WORKER_LAYER_SKELETON,
      src->getNode().get())->pin(WORKER_LAYER_SKELETON);

  (**destNode).getMergePoints().push_back(std::make_pair(std::make_pair(dest->_instrSinceFork,
      src->_instrSinceFork), srcNodePin));
  // Update the destination state container
  if (&(**dest) != mState) {
    (**dest).setCloud9State(NULL);
    dest->replaceKleeState(mState);
    mState->setCloud9State(dest);
  }
  // Now terminate the source state. Issue this from the executor - this will
  // propagate all way through the entire infrastructure
  requestStateDestroy(src);

  CLOUD9_DEBUG("State merged");

  return true;
}

/* Symbolic Engine Callbacks **************************************************/

bool JobManager::onStateBranching(klee::ExecutionState *state, klee::ForkTag forkTag) {
  switch (forkTag.forkClass) {
  case KLEE_FORK_FAULTINJ:
    return StratFaultInj;
  default:
    return false;
  }
}

void JobManager::onStateBranched(klee::ExecutionState *kState,
    klee::ExecutionState *parent, int index, klee::ForkTag forkTag) {
  boost::unique_lock<boost::mutex> lock(jobsMutex);

  assert(parent);
  assert(kState);

  //if (kState)
  //	CLOUD9_DEBUG("State branched: " << parent->getCloud9State()->getNode());

  WorkerTree::NodePin pNode = parent->getCloud9State()->getNode();

  updateTreeOnBranch(kState, parent, index, forkTag);

  SymbolicState *state = kState->getCloud9State();

  if (parent->getCloud9State()->collectProgress) {
    state->collectProgress = true;
    state->_instrProgress = parent->getCloud9State()->_instrProgress; // XXX This is totally inefficient
    state->_instrPos = parent->getCloud9State()->_instrPos;
  }

  //CLOUD9_DEBUG("State forked at level " << state->getNode()->getLevel());

  SymbolicState *pState = parent->getCloud9State();

  if (pState->getNode()->layerExists(WORKER_LAYER_JOBS) || !replaying) {
    fireUpdateState(pState, pNode.get());
  } else {
    fireUpdateState(pState, pNode.get());
    fireDeactivateState(pState);
  }

  if (state->getNode()->layerExists(WORKER_LAYER_JOBS) || !replaying) {
    fireActivateState(state);
  }

  // Reset the number of instructions since forking
  state->_instrSinceFork = 0;
  pState->_instrSinceFork = 0;

}

void JobManager::onStateDestroy(klee::ExecutionState *kState, bool silenced) {
  boost::unique_lock<boost::mutex> lock(jobsMutex);

  assert(kState);

  if (replaying && DebugJobReconstruction) {
    CLOUD9_DEBUG("State destroyed during replay");
    kState->getStackTrace().dump(std::cout);
  }

  //CLOUD9_DEBUG("State destroyed");

  SymbolicState *state = kState->getCloud9State();

  pendingDeletions.erase(state);
  if (currentState == state) {
    currentState = NULL;
  }

  if (!silenced) {
    processTestCase(state);
  }

  if (DumpInstrTraces) {
    dumpInstructionTrace(state->getNode().get());
  }

  fireDeactivateState(state);

  updateTreeOnDestroy(kState);
}

void JobManager::onOutOfResources(klee::ExecutionState *destroyedState) {
  // TODO: Implement a job migration mechanism
  CLOUD9_INFO("Executor ran out of resources. Dropping state.");
}

void JobManager::onEvent(klee::ExecutionState *kState,
          unsigned int type, long int value) {
  WorkerTree::Node *node = kState->getCloud9State()->getNode().get();

  switch (type) {
  case KLEE_EVENT_BREAKPOINT:
    if (collectTraces) {
      (**node).trace.appendEntry(new BreakpointEntry(value));
    }

    if (StratOracle) {
      if (value == KLEE_BRK_START_TRACING) {
        kState->getCloud9State()->collectProgress = true; // Enable progress collection in the manager
      }
    }

    break;
  default:
    (**node).trace.appendEntry(new EventEntry(kState->getStackTrace(), type, value));
    break;
  }

}

void JobManager::onControlFlowEvent(klee::ExecutionState *kState,
    ControlFlowEvent event) {
  boost::unique_lock<boost::mutex> lock(jobsMutex);

  SymbolicState *state = kState->getCloud9State();
  if (!state) {
    return;
  }

  WorkerTree::Node *node = state->getNode().get();

  switch(event) {
  case STEP:
    // XXX Hack - do this only for basic block starts
    if (kState->pc()->isBBHead)
      fireStepState(state);
    break;
  default:
    break;
  }

  // Add the instruction to the node trace
  if (collectTraces) {
    switch (event) {
    case STEP:
      (**node).trace.appendEntry(new InstructionTraceEntry(kState->pc()));
      break;
    case BRANCH_FALSE:
    case BRANCH_TRUE:
      //(**node).trace.appendEntry(new ConstraintLogEntry(state));
      (**node).trace.appendEntry(new ControlFlowEntry(true, false, false));
      break;
    case CALL:
      break;
    case RETURN:
      break;
    }

  }
}

void JobManager::onDebugInfo(klee::ExecutionState *kState,
    const std::string &message) {
  WorkerTree::Node *node = kState->getCloud9State()->getNode().get();

  if (collectTraces) {
    (**node).trace.appendEntry(new DebugLogEntry(message));
  }
}

void JobManager::fireBreakpointHit(WorkerTree::Node *node) {
  SymbolicState *state = (**node).symState;

  CLOUD9_INFO("Breakpoint hit!");
  CLOUD9_DEBUG("State at position: " << *node);

  if (state) {
    CLOUD9_DEBUG("State stack trace: " << *state);
    klee::ExprPPrinter::printConstraints(std::cerr, (**state).constraints());
    dumpStateTrace(node);
  }

  // Also signal a breakpoint, for stopping GDB
  cloud9::breakSignal();
}

void JobManager::updateTreeOnBranch(klee::ExecutionState *kState,
    klee::ExecutionState *parent, int index, klee::ForkTag forkTag) {
  WorkerTree::NodePin pNodePin = parent->getCloud9State()->getNode();
  (**pNodePin).forkTag = forkTag;

  WorkerTree::Node *newNode, *oldNode;

  // Obtain the new node pointers
  oldNode = tree->getNode(WORKER_LAYER_STATES, pNodePin.get(), 1 - index);
  parent->getCloud9State()->rebindToNode(oldNode);

  newNode = tree->getNode(WORKER_LAYER_STATES, pNodePin.get(), index);
  SymbolicState *state = new SymbolicState(kState, parent->getCloud9State());
  state->rebindToNode(newNode);

  if (!replaying) {
    ExecutionJob *job = (**pNodePin).getJob();
    if (job != NULL) {
      oldNode = tree->getNode(WORKER_LAYER_JOBS, oldNode);
      job->rebindToNode(oldNode);

      newNode = tree->getNode(WORKER_LAYER_JOBS, newNode);
      ExecutionJob *newJob = new ExecutionJob(newNode);

      submitJob(newJob, false);

      cloud9::instrum::theInstrManager.incStatistic(
                  cloud9::instrum::TotalTreePaths);

    } else {
      CLOUD9_DEBUG("Job-less state terminated. Probably it was exported while the state was executing.");
    }
  }
}

void JobManager::updateTreeOnDestroy(klee::ExecutionState *kState) {
  SymbolicState *state = kState->getCloud9State();

  WorkerTree::Node *node = state->getNode().get();

  if (ZombieNodes) {
    // Pin the state on the skeleton layer
    WorkerTree::NodePin zombiePin =
        tree->getNode(WORKER_LAYER_SKELETON, node)->pin(WORKER_LAYER_SKELETON);
    zombieNodes.insert(zombiePin);
  }

  if (node->layerExists(WORKER_LAYER_JOBS)) {
    std::vector<WorkerTree::Node*> jobNodes;
    tree->getLeaves(WORKER_LAYER_JOBS, node, jobNodes);

    for (std::vector<WorkerTree::Node*>::iterator it = jobNodes.begin(); it != jobNodes.end(); it++) {
      ExecutionJob *job = (**(*it)).getJob();
      if (job != NULL) {
        finalizeJob(job, false, false);

        cloud9::instrum::theInstrManager.incStatistic(
            cloud9::instrum::TotalProcJobs);
      }
    }
  }

  state->rebindToNode(NULL);

  kState->setCloud9State(NULL);
  delete state;
}

/* Statistics Management ******************************************************/

void JobManager::refineStatistics() {
  std::set<WorkerTree::NodePin> newStats;

  for (std::set<WorkerTree::NodePin>::iterator it = stats.begin(); it
      != stats.end(); it++) {
    const WorkerTree::NodePin &nodePin = *it;

    if (!nodePin->layerExists(WORKER_LAYER_JOBS)) {
      // Statistic node invalidated
      continue;
    }

    if (nodePin->getCount(WORKER_LAYER_JOBS) > 0) {
      // Move along the path

      WorkerTree::Node *left = nodePin->getChild(WORKER_LAYER_JOBS, 0);
      WorkerTree::Node *right = nodePin->getChild(WORKER_LAYER_JOBS, 1);

      if (left) {
        WorkerTree::NodePin leftPin = tree->getNode(WORKER_LAYER_STATISTICS,
            left)->pin(WORKER_LAYER_STATISTICS);
        newStats.insert(leftPin);
      }

      if (right) {
        WorkerTree::NodePin rightPin = tree->getNode(WORKER_LAYER_STATISTICS,
            right)->pin(WORKER_LAYER_STATISTICS);
        newStats.insert(rightPin);
      }
    } else {
      newStats.insert(nodePin);
    }
  }

  stats = newStats;
  statChanged = true;
}

void JobManager::cleanupStatistics() {

  for (std::set<WorkerTree::NodePin>::iterator it = stats.begin(); it
      != stats.end();) {
    std::set<WorkerTree::NodePin>::iterator oldIt = it++;
    WorkerTree::NodePin nodePin = *oldIt;

    if (!nodePin->layerExists(WORKER_LAYER_JOBS)) {
      stats.erase(oldIt);
      statChanged = true;
    }
  }

  if (stats.empty()) {
    // Add back the root state in the statistics
    WorkerTree::NodePin rootPin = tree->getRoot()->pin(WORKER_LAYER_STATISTICS);
    stats.insert(rootPin);
    statChanged = true;
  }
}

#if 0
void JobManager::getStatisticsData(std::vector<int> &data,
    ExecutionPathSetPin &paths, bool onlyChanged) {
  boost::unique_lock<boost::mutex> lock(jobsMutex);

  cleanupStatistics();

  if (statChanged || !onlyChanged) {
    std::vector<WorkerTree::Node*> newStats;
    for (std::set<WorkerTree::NodePin>::iterator it = stats.begin(); it
        != stats.end(); it++) {
      newStats.push_back((*it).get());
    }

    paths = tree->buildPathSet(newStats.begin(), newStats.end());
    statChanged = false;

    //CLOUD9_DEBUG("Sent node set: " << getASCIINodeSet(newStats.begin(), newStats.end()));
  }

  data.clear();

  for (std::set<WorkerTree::NodePin>::iterator it = stats.begin(); it
      != stats.end(); it++) {
    const WorkerTree::NodePin &crtNodePin = *it;
    unsigned int jobCount = countJobs(crtNodePin.get());
    data.push_back(jobCount);
  }

  //CLOUD9_DEBUG("Sent data set: " << getASCIIDataSet(data.begin(), data.end()));
}
#else
void JobManager::getStatisticsData(std::vector<int> &data,
    ExecutionPathSetPin &paths, bool onlyChanged) {
  boost::unique_lock<boost::mutex> lock(jobsMutex);

  paths = ExecutionPathSet::getRootSet();

  data.clear();

  data.push_back(jobCount);
}
#endif

/* Coverage Management ********************************************************/

void JobManager::getUpdatedLocalCoverage(cov_update_t &data) {
  theStatisticManager->collectChanges(stats::locallyCoveredInstructions, data);
  theStatisticManager->resetChanges(stats::locallyCoveredInstructions);
}

void JobManager::setUpdatedGlobalCoverage(const cov_update_t &data) {
  for (cov_update_t::const_iterator it = data.begin(); it != data.end(); it++) {
    assert(it->second != 0 && "code uncovered after update");

    uint32_t index = it->first;

    if (!theStatisticManager->getIndexedValue(
        stats::globallyCoveredInstructions, index)) {
      theStatisticManager->incrementIndexedValue(
          stats::globallyCoveredInstructions, index, 1);
      theStatisticManager->incrementIndexedValue(
          stats::globallyUncoveredInstructions, index, (uint64_t) -1);

      assert(!theStatisticManager->getIndexedValue(stats::globallyUncoveredInstructions, index));
    }
  }
}

uint32_t JobManager::getCoverageIDCount() const {
  return symbEngine->getModule()->infos->getMaxID();
}

/* Debugging Support **********************************************************/

void JobManager::setPathBreakpoint(ExecutionPathPin path) {
  assert(0 && "Not yet implemented"); // TODO: Implement this as soon as the
  // tree data structures are refactored
}

void JobManager::setCodeBreakpoint(int assemblyLine) {
  CLOUD9_INFO("Code breakpoint at assembly line " << assemblyLine);
  codeBreaks.insert(assemblyLine);
}

void JobManager::dumpStateTrace(WorkerTree::Node *node) {
  if (!collectTraces) {
    // We have nothing collected, why bother?
    return;
  }

  // Get a file to dump the path into
  char fileName[256];
  snprintf(fileName, 256, "pathDump%05d.txt", traceCounter);
  traceCounter++;

  CLOUD9_INFO("Dumping state trace in file '" << fileName << "'");

  std::ostream *os = kleeHandler->openOutputFile(fileName);
  assert(os != NULL);

  (*os) << (*node) << std::endl;

  SymbolicState *state = (**node).symState;
  assert(state != NULL);

  serializeExecutionTrace(*os, state);

  delete os;
}

void JobManager::dumpInstructionTrace(WorkerTree::Node *node) {
  if (!collectTraces)
    return;

  char fileName[256];
  snprintf(fileName, 256, "instrDump%05d.txt", traceCounter);
  traceCounter++;

  CLOUD9_INFO("Dumping instruction trace in file " << fileName);

  std::ostream *os = kleeHandler->openOutputFile(fileName);
  assert(os != NULL);

  serializeInstructionTrace(*os, node);

  delete os;
}

}
}
