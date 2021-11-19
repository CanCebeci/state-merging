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

#ifndef INSTRUMENTATIONMANAGER_H_
#define INSTRUMENTATIONMANAGER_H_

#if (LLVM_VERSION_MAJOR == 2 && LLVM_VERSION_MINOR < 9)
#include "llvm/System/Process.h"
#else
#include "llvm/Support/Process.h"
#endif

#include "cloud9/instrum/Timing.h"

#include <set>
#include <vector>
#include <utility>
#include <cassert>
#include <sstream>
#include <map>


namespace cloud9 {

namespace instrum {


using namespace std;

class InstrumentationWriter;

enum Statistics {
  TotalProcInstructions = 0,

  TotalProcJobs = 1,
  TotalReplayedJobs = 14,
  TotalExportedJobs = 8,
  TotalImportedJobs = 9,
  TotalDroppedJobs = 10,

  TotalForkedStates = 15,
  TotalFinishedStates = 16,

  TotalTreePaths = 17,

  CurrentJobCount = 11,
  CurrentActiveStateCount = 18,
  CurrentStateCount = 19,

  TotalReplayInstructions = 20,
  TotalWastedInstructions = 21,


  MAX_STATISTICS = 22
};

enum EventClass {
  TestCase = 0,
  ErrorCase = 1,
  JobExecutionState = 2,
  TimeOut = 3,
  InstructionBatch = 4,
  ReplayBatch = 5,
  SMTSolve = 6,
  SATSolve = 7,
  ConstraintSolve = 8
};

enum EventAttribute {
  Default = 0,
  WallTime = 1,
  ThreadTime = 2,
  StateDepth = 3,
  StateMultiplicity = 4,
  SolvingResult = 5
};

class IOServices;

class InstrumentationManager {
public:
	typedef double TimeStamp;
	typedef long int stat_value_t;
	typedef vector<stat_value_t> statistics_t;

	typedef map<int, string> event_attributes_t;
	typedef pair<TimeStamp, int> event_id_t;

	typedef map<int, event_attributes_t> pending_events_t;
	typedef vector<pair<event_id_t, event_attributes_t> > events_t;

	typedef map<string, pair<unsigned, unsigned> > coverage_t;
private:
	typedef set<InstrumentationWriter*> writer_set_t;

	Timer absoluteCounter;

	statistics_t stats;

	pending_events_t pendingEvents;
	events_t events;

	writer_set_t writers;

	coverage_t coverage;
	bool covUpdated;

	IOServices *ioServices;

	void instrumThreadControl();

	void writeStatistics();
	void writeEvents();
	void writeCoverage();

	InstrumentationManager();

public:
	virtual ~InstrumentationManager();

	void registerWriter(InstrumentationWriter *writer) {
		assert(writer != NULL);

		writers.insert(writer);
	}

	void start();
	void stop();

	void recordEventAttributeStr(EventClass id, EventAttribute attr, string value);

	template <class T>
	void recordEventAttribute(EventClass id, EventAttribute attr, T value) {
	  stringstream ss;
	  ss << value;
	  ss.flush();

	  recordEventAttributeStr(id, attr, ss.str());
	}

	void clearEventAttribute(EventClass id, EventAttribute attr);

	void recordEventTiming(EventClass id, const Timer &t) {
	  recordEventAttribute(id, WallTime, t.getRealTime());
	  recordEventAttribute(id, ThreadTime, t.getThreadTime());
	}

	void recordEvent(EventClass id, bool reset = true);

	void recordEvent(EventClass id, Timer &t) {
	  recordEventTiming(id, t);
	  recordEvent(id);
	}

	void recordEvent(EventClass id, string value) {
	  recordEventAttributeStr(id, Default, value);
	  recordEvent(id);
	}

	void setStatistic(Statistics id, stat_value_t value) {
		stats[id] = value;
	}

	void topStatistic(Statistics id, stat_value_t value) {
		if (value > stats[id])
			stats[id] = value;
	}

	void incStatistic(Statistics id, stat_value_t value = 1) {
		stats[id] += value;
	}

	void decStatistic(Statistics id, stat_value_t value = 1) {
		stats[id] -= value;
	}

	void updateCoverage(string tag, std::pair<unsigned, unsigned> value);

	static InstrumentationManager &getManager() {
		static InstrumentationManager manager;

		return manager;
	}

	static std::string stampToString(TimeStamp stamp);
	static std::ostream &writeStamp(std::ostream &s, const TimeStamp &stamp);
};

extern InstrumentationManager &theInstrManager;

}

}

#endif /* INSTRUMENTATIONMANAGER_H_ */
