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

#include "cloud9/instrum/InstrumentationManager.h"
#include "cloud9/instrum/InstrumentationWriter.h"
#include "llvm/Support/CommandLine.h"
#include "cloud9/Logger.h"

#include <iomanip>
#include <boost/io/ios_state.hpp>
#include <boost/thread.hpp>
#include <boost/asio.hpp>

using namespace llvm;

namespace {
cl::opt<int> InstrUpdateRate("c9-instr-update-rate",
		cl::desc("Rate (in seconds) of updating instrumentation info"),
		cl::init(2));
}

namespace cloud9 {

namespace instrum {

class IOServices {
public:
  boost::timed_mutex terminateMutex;
  boost::mutex eventsMutex;
  boost::mutex coverageMutex;
  boost::thread instrumThread;
public:
  IOServices() { }
};

InstrumentationManager &theInstrManager = InstrumentationManager::getManager();

void InstrumentationManager::instrumThreadControl() {

	CLOUD9_INFO("Instrumentation started");

	for (;;) {
		boost::system::error_code code;
        bool terminated = ioServices->terminateMutex.timed_lock(boost::posix_time::seconds(InstrUpdateRate));

        if (terminated) {
			CLOUD9_INFO("Instrumentation interrupted. Stopping.");
			writeStatistics();
			writeEvents();
            writeCoverage();
            ioServices->terminateMutex.unlock();
			break;
		}

		writeStatistics();
		writeEvents();
		writeCoverage();
	}
}


InstrumentationManager::InstrumentationManager() :
        stats(MAX_STATISTICS), covUpdated(false) {

  absoluteCounter.start();
  ioServices = new IOServices();
}

InstrumentationManager::~InstrumentationManager() {
	stop();

	for (writer_set_t::iterator it = writers.begin(); it != writers.end(); it++) {
		InstrumentationWriter *writer = *it;

		delete writer;
	}

	delete ioServices;
}

void InstrumentationManager::start() {
    ioServices->terminateMutex.lock();
    ioServices->instrumThread = boost::thread(&InstrumentationManager::instrumThreadControl, this);
}

void InstrumentationManager::stop() {
	if (ioServices->instrumThread.joinable()) {
        ioServices->terminateMutex.unlock();

		ioServices->instrumThread.join();
	}
}

void InstrumentationManager::writeStatistics() {
        absoluteCounter.split();
	TimeStamp stamp = absoluteCounter.getRealTime();

	for (writer_set_t::iterator it = writers.begin(); it != writers.end(); it++) {
		InstrumentationWriter *writer = *it;

		writer->writeStatistics(stamp, stats);
	}
}

void InstrumentationManager::writeEvents() {
	boost::unique_lock<boost::mutex> lock(ioServices->eventsMutex);
	events_t eventsCopy = events;
	events.clear();
	lock.unlock();

	for (writer_set_t::iterator it = writers.begin(); it != writers.end(); it++) {
		InstrumentationWriter *writer = *it;

		writer->writeEvents(eventsCopy);
	}
}

void InstrumentationManager::writeCoverage() {
  boost::unique_lock<boost::mutex> lock(ioServices->coverageMutex);
  if (!covUpdated) {
    lock.unlock();
    return;
  }

  coverage_t coverageCopy = coverage;
  covUpdated = false;
  lock.unlock();

  absoluteCounter.split();
  TimeStamp stamp = absoluteCounter.getRealTime();

  for (writer_set_t::iterator it = writers.begin(); it != writers.end(); it++) {
    InstrumentationWriter *writer = *it;

    writer->writeCoverage(stamp, coverageCopy);
  }
}

void InstrumentationManager::recordEventAttributeStr(EventClass id, EventAttribute attr, string value) {
  boost::lock_guard<boost::mutex> lock(ioServices->eventsMutex);
  pendingEvents[id][attr] = value;
}

void InstrumentationManager::clearEventAttribute(EventClass id, EventAttribute attr) {
  boost::lock_guard<boost::mutex> lock(ioServices->eventsMutex);
  pendingEvents[id].erase(attr);
}

void InstrumentationManager::recordEvent(EventClass id, bool reset) {
  boost::lock_guard<boost::mutex> lock(ioServices->eventsMutex);
  absoluteCounter.split();

  events.push_back(make_pair(make_pair(absoluteCounter.getRealTime(), id), pendingEvents[id]));

  if (reset)
    pendingEvents.erase(id);
}

void InstrumentationManager::updateCoverage(string tag, std::pair<unsigned, unsigned> value) {
  boost::lock_guard<boost::mutex> lock(ioServices->coverageMutex);

  coverage[tag] = value;
  covUpdated = true;
}

std::string InstrumentationManager::stampToString(TimeStamp stamp) {
	std::ostringstream ss;
	writeStamp(ss, stamp);
	ss.flush();

	return ss.str();
}

std::ostream &InstrumentationManager::writeStamp(std::ostream &s,
    const TimeStamp &stamp) {
  boost::io::ios_all_saver saver(s);

  s << fixed << setprecision(9) << stamp;

  return s;
}

}

}
