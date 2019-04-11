/*
 * Copyright (c) 2015-2016 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * Copyright (c) 2015, Drexel University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Karthik Sangaiah
 *          Ankit More
 *          Radhika Jagtap
 */

#include "sim/sim_exit.hh"
#include "synchro_trace.hh"

using namespace std;

SynchroTrace::SynchroTrace(const Params *p)
  : MemObject(p), synchroTraceStartEvent(this),
    masterID(p->system->getMasterId(this, name())),
    numCpus(p->num_cpus), numThreads(p->num_threads),
    startSyncRegion(p->start_sync_region),
    instSyncRegion(p->inst_sync_region),
    barrStatDump(p->barrier_stat_dump),
    eventDir(p->event_dir), outDir(p->output_dir),
    wakeupFreq(p->master_wakeup_freq),
    useRuby(p->ruby),
    m_block_size_bytes(p->block_size_bytes),
    printCount(50000),
    CPI_IOPS(p->cpi_iops),
    CPI_FLOPS(p->cpi_flops),
    pcSkip(p->pc_skip),
    memorySizeBytes(p->mem_size_bytes)
{
    // Initialize the SynchroTrace to Memory ports
    for (int i = 0; i < p->num_cpus; ++i) {
        ports.push_back(new CpuPort(csprintf("%s-port%d", name(), i),
                        this, i));
    }
    assert(ports.size() > 0);
}

SynchroTrace::~SynchroTrace()
{
    for (int i = 0; i < ports.size(); i++)
        delete ports[i];

    for (int i = 0; i < numContexts; i++)
        delete coreEvents[i];

    for (int i = 0; i < numThreads; i++)
        delete eventMap[i];
    delete[] eventMap;
}

void
SynchroTrace::init()
{
    assert(isPowerOf2(numCpus));

    // Initialize memory params
    if (useRuby) {
        blockSizeBytes = RubySystem::getBlockSizeBytes();
        blockSizeBits = RubySystem::getBlockSizeBits();
    } else {
        blockSizeBytes = m_block_size_bytes;
        blockSizeBits = floorLog2(blockSizeBytes);
    }

    if (STParser::maxRequestSize > blockSizeBytes)
        panic("Error in SynchroTrace!!: maxRequestSize "
               " is greater than block size (%d)!!",
               blockSizeBytes);

    // Initialize thread/cpu params
    if (numThreads < numCpus) {
        numContexts = numThreads;
    } else {
        numContexts = numCpus;
    }

    // Initialize centralized event map
    eventMap = new deque<STEvent *>*[numThreads];
    for (int i = 0; i < numThreads; i++)
        eventMap[i] = new deque<STEvent *>;

   // Initiate thread maps and set master thread as active
    threadMap.resize(numContexts);
    threadStartedMap.resize(numThreads);
    for (int i = 0; i < numThreads; i++)
        threadStartedMap[i] = false;
    threadStartedMap[0] = true;

    // Initiate conditional wait state vector
    cond_wait_states.resize(numContexts);
    for (int i = 0; i < numContexts; i++)
        cond_wait_states[i] = INIT;

    // Initiate conditional wait signals for each thread
    condSignals.resize(numThreads);

     // Initiate event list per core
    for (int i = 0; i < numContexts; i++)
        coreEvents.push_back(new SynchroTraceCoreEvent(this, i));

    // Initial scheduling of master simulator thread and cores
    schedule(synchroTraceStartEvent, 1);
    for (int i = 0; i < numContexts; i++)
        schedule(*(coreEvents[i]), clockPeriod());

    // Create parser for pthread metadata and Sigil trace events
    parser = new STParser(numThreads, eventMap, startSyncRegion,
                    instSyncRegion, eventDir, outDir, memorySizeBytes,
                    blockSizeBytes, blockSizeBits);

    // Parse Pthread Metadata
    addresstoIDMap = parser->getAddressToIDMap();
    barrierMap = parser->getBarrierMap();

    // Get Sigil trace file pointers
    inputFilePointer = parser->getInputFilePointer();

    // Get Sigil trace file synchronization region file pointers
    syncRegionFilePointers = parser->getSynchroRegionFilePointers();

    // Get start of synchronization region
    startSyncRegion = parser->getStartSyncRegion();

    // Initialize debug flags and mutex lock/barrier maps
    initStats();

    // Initialize flag event counters
    parser->initFlagEventCounters();

    // Map threads to cores
    initialThreadMapping();

    // Parse the first set of events
    parser->generateEventQueue();
}

void
SynchroTrace::initStats()
{
    // ThreadContMap to check if thread is finished with a barrier.
    threadContMap.resize(numThreads);

    // ThreadMutexMap to check if thread is holding a mutex lock.
    threadMutexMap.resize(numThreads);

    for (int i = 0; i < numThreads; i++) {
        threadContMap[i] = false;
        //threadMutexMap[i] = 0;
    }

    hourCounter = std::time(NULL);
    printThreadEventCounters = 0;
    roiFlag = false;
    workerThreadCount = 0;
}

bool
SynchroTrace::CpuPort::recvTimingResp(PacketPtr pkt)
{
    tester->hitCallback(id, pkt);

    // Only need timing of the memory request.
    // No need for the actual data.
    delete pkt;
    return true;
}

void
SynchroTrace::wakeup()
{
    // Replenish each thread's events if depleted
    for (ThreadID thread_id = 0; thread_id < numThreads; thread_id++)
        parser->replenishEvents(thread_id);

    // Terminates the simulation after checking if all the events are done
    checkCompletion();

    // Prints thread status every hour
    printThreadEventsPerHour();

    // Schedule to keep this back-end simulation thread running
    schedule(synchroTraceStartEvent, curTick() + clockPeriod() * wakeupFreq);
}

void
SynchroTrace::wakeup(int proc_id)
{
    // For every 50k wakeup counts, print all the thread's EventID#
    printThreadEvents();

    // Create subevents for the thread at the head of the core list
    // it is not necessary that this will happen always.
    // If the subevents have been created, simply skip
    createSubEvents(proc_id);

    // Main progression of Event Queue
    progressEvents(proc_id);

    // Swap threads in cores if allowed
    swapStalledThreads(proc_id);
}

void
SynchroTrace::printThreadEventsPerHour()
{
    if (std::difftime(std::time(NULL), hourCounter) >= 60) {
        hourCounter = std::time(NULL);
        DPRINTF(STIntervalPrintByHour, "%s", std::ctime(&hourCounter));
        for (int i = 0; i < numThreads; i++) {
            if (!eventMap[i]->empty()) {
                DPRINTF(STIntervalPrintByHour, "Thread %d: Event %d\n",
                        i, eventMap[i]->front()->eventID);
            }
        }
    }
}

void
SynchroTrace::printThreadEvents()
{
    if (printThreadEventCounters == printCount) {
        for (int i = 0; i < numThreads; i++) {
            if (!eventMap[i]->empty()) {
                DPRINTF(STIntervalPrint, "Thread %d is on Event %d\n",
                        i, eventMap[i]->front()->eventID);
            }
        }
        for (int i = 0; i < numCpus; i++) {
            if (i < numThreads) {
                DPRINTF(STIntervalPrint, "Thread %d is on top Core %d\n",
                        threadMap[i][0], i);
            } else {
                DPRINTF(STIntervalPrint, "No Thread is on top Core %d\n", i);
            }
        }
        for (int i = 0; i < numThreads; i++) {
            if (!eventMap[i]->empty()) {

                std::string thread_started =
                        (threadStartedMap[i] == 1) ? "ON" : "OFF";
                DPRINTF(STIntervalPrint, "Thread %d is %s\n",
                        i, thread_started);
            }
        }
        printThreadEventCounters = 0;
    } else {
        printThreadEventCounters++;
    }
}

void
SynchroTrace::printEvent(ThreadID thread_id, bool is_end)
{
    if (!eventMap[thread_id]->empty()) {
        if (!is_end) {
            DPRINTF(STEventPrint, "Starting %d, %d\n",
                    thread_id, eventMap[thread_id]->front()->eventID);
        } else {
            DPRINTF(STEventPrint, "Finished %d, %d\n",
                    thread_id, eventMap[thread_id]->front()->eventID);
        }
    }
}

void
SynchroTrace::checkCompletion()
{
    bool terminate = true;

    if (startSyncRegion == 0) {
        // Termination condition: terminate when thread 0 is completed.
        // Occurs when entire benchmark is simulated
        if (!eventMap[0]->empty())
            terminate = false;

    } else {
        // Termination condition: terminate when all threads are completed.
        // Occurs when synchronization region is simulated
        for (int TID=0; TID < numThreads; TID++) {
            if (!eventMap[TID]->empty()) {
                terminate = false;
                break;
            }
        }
    }

    if (terminate) {
        if (startSyncRegion == 0) {
            for (int i = 0; i < numThreads; i++) {
                if (inputFilePointer[i] != NULL) {
                    inputFilePointer[i]->close();
                    delete inputFilePointer[i];
                }
            }
        }
        exitSimLoop("SynchroTrace completed");
    }
}

void
SynchroTrace::swapThreads(int proc_id)
{
    int num_threads = threadMap[proc_id].size();
    int curr_thread_id = threadMap[proc_id][0];

    // Check list of threads on the core to see if the thread is available
    for (int i = 1; i < num_threads; i++) {
        // Next thread on core
        ThreadID top_thread_id = threadMap[proc_id][i];

        // Thread is unavailable or completed its events, try next thread
        if (!threadStartedMap[top_thread_id] ||
            eventMap[top_thread_id]->empty())
            continue;

        // Pull up next threads events
        STEvent *topEvent = eventMap[top_thread_id]->front();

        // Threads with Computation events and Pthread events
        // can be swapped in always.
        //
        // Threads with Communication events are checked to see
        // if all producer -> consumer dependencies have been met.
        //
        // Corner case: Consumer thread has a mutex while waiting
        // for a dependency. We don't enforce this communication
        // edge.
        if (topEvent->eventClass == STEvent::COMP) {
            moveThreadToHead(proc_id, i);
            break;
        } else if (topEvent->eventClass == STEvent::THREAD_API &&
                topEvent->eventType != STEvent::THREAD_JOIN) {
            moveThreadToHead(proc_id, i);
            break;
        } else if ((topEvent->eventClass == STEvent::THREAD_API) &&
                (topEvent->eventType == STEvent::THREAD_JOIN) &&
                (eventMap[addresstoIDMap[topEvent->pthAddr]]->empty())) {
            moveThreadToHead(proc_id, i);
            break;
        // Communication Event
        } else if (((topEvent->eventClass == STEvent::COMM) &&
                (checkAllCommDependencies(topEvent)) &&
//                (threadMutexMap[curr_thread_id] == 0)) ||
                (threadMutexMap[curr_thread_id].empty())) ||
                pcSkip) {
                moveThreadToHead(proc_id, i);
                break;
        }
    }
}

void
SynchroTrace::swapStalledThreads(int proc_id)
{
    ThreadID event_thread_id = threadMap[proc_id].front();
    // If current thread is completed or is unavailable,
    // check to swap in next threads
    if (eventMap[event_thread_id]->empty() ||
        !threadStartedMap[event_thread_id]) {
            swapThreads(proc_id);

        // Scheduling next thread's core to wake up next cycle.
        if (!(coreEvents[proc_id]->scheduled())) {
             schedule(*(coreEvents[proc_id]), curTick() + clockPeriod());
        }
        return;
    }

    // Swap if current thread is waiting on obtaining a mutex lock
    STEvent *this_event = eventMap[event_thread_id]->front();
    if ((this_event->eventType == STEvent::THREAD_JOIN) ||
        ((this_event->eventType == STEvent::MUTEX_LOCK) &&
//        (threadMutexMap[this_event->evThreadID] == 0))) {
        (threadMutexMap[this_event->evThreadID].empty()))) {
        swapThreads(proc_id);
        if (!(coreEvents[proc_id]->scheduled())) {
            schedule(*(coreEvents[proc_id]), curTick());
        }
    }
}

void
SynchroTrace::moveThreadToHead(int proc_id, ThreadID thread_id)
{
    for (ThreadID i = 0; i < thread_id; i++) {
        // Move the thread at the head to the end
        threadMap[proc_id].push_back(threadMap[proc_id][0]);
        threadMap[proc_id].erase(threadMap[proc_id].begin());
    }
}

bool
SynchroTrace::checkAllCommDependencies(STEvent *this_event)
{
    assert(this_event->eventClass == STEvent::COMM);
    bool check = true;
    // Check all of the producer->consumer dependencies within the event
    for (unsigned long i = 0;
        i < this_event->commPreRequisiteEvents.size(); i++) {
        check &= checkCommDependency(this_event->commPreRequisiteEvents[i],
                                     this_event->evThreadID);
    }
    return check;
}

bool
SynchroTrace::checkCommDependency(MemAddrInfo *comm_event, ThreadID thread_id)
{
    // This check is for OS-related traffic.
    // We indicate a communication event with the OS producer thread
    // as having a ThreadID of 30000
    if (comm_event->reqThreadID == 30000) {
       return true;
    }

    // If the producer thread's eventID is greater than the dependent event
    // then the dependency is satisfied
    if (!eventMap[comm_event->reqThreadID]->empty()) {
        if (eventMap[comm_event->reqThreadID]->front()->eventID >
            comm_event->reqEventID)
            return true;
        else
            return false;
    } else {
        return true;
    }
}

void
SynchroTrace::initialThreadMapping()
{
    // Currently map threads in a simple round robin fashion on cores
    for (int i = 0; i < numThreads; i++) {
        threadMap[i % numContexts].push_back(i);
    }
}

SynchroTrace*
SynchroTraceParams::create()
{
    return new SynchroTrace(this);
}

void
SynchroTrace::hitCallback(NodeID proc_id, PacketPtr pkt)
{
    assert(proc_id < numContexts);
    ThreadID event_thread_id = threadMap[proc_id].front();
    STEvent *this_event = eventMap[event_thread_id]->front();

    if (!(this_event->subEventList->front().msgTriggered)) {
        warn("%x: Message not triggered but received hitCallback:"
             " nodeID: %d Event: %d\n", curTick(), proc_id,
             this_event->eventID);
        return;
    }

    assert(this_event->subEventList->front().msgTriggered);

    // Sub event completed when memory request returns
    this_event->subEventList->pop_front();

    // If all sub events are completed, delete event.
    if (this_event->subEventList->empty()) {
        printEvent(event_thread_id, true); // Print event completion

        // Delete Event
        DPRINTF(STDebug, "Event %d completed for thread %d on core %d \n",
                eventMap[event_thread_id]->front()->eventID,
                event_thread_id, proc_id);

        STEvent *completed_event = eventMap[event_thread_id]->front();
        eventMap[event_thread_id]->pop_front();
        delete completed_event;

        // If multiple threads per core, check if we can swap threads
        if (threadMap[proc_id].size() > 1)
            swapThreads(proc_id);

        if (!eventMap[event_thread_id]->empty()) {
            // Print: Event is starting
            printEvent(event_thread_id, false);

            this_event = eventMap[event_thread_id]->front();
            parser->replenishEvents(this_event->evThreadID);
        }

        // Schedule core to handle new event
        schedule(*(coreEvents[proc_id]), curTick());
    } else {
        // Event not completed - Pull up the next subevent and schedule
        SubEvent *new_sub_event = &(this_event->subEventList->front());
        Tick comp_time = clockPeriod() *
                        (Cycles(CPI_IOPS * new_sub_event->numIOPS) +
                        Cycles(CPI_FLOPS * new_sub_event->numFLOPS));
        new_sub_event->triggerTime = comp_time + curTick() + clockPeriod();
        schedule(*(coreEvents[proc_id]), new_sub_event->triggerTime);
    }
}

void
SynchroTrace::triggerMsg(int proc_id, ThreadID thread_id,
                         SubEvent *this_sub_event)
{
    // Package memory request
    assert (!eventMap[thread_id]->empty());

    Addr addr;
    addr = this_sub_event->thisMsg->addr;

    Request::Flags flags;

    RequestPtr req = std::make_shared<Request>(
        addr, this_sub_event->thisMsg->numBytes, flags, masterID);
    req->setContext(proc_id);

    Packet::Command cmd;

    if (this_sub_event->msgType == SubEvent::REQ_READ)
        cmd = MemCmd::ReadReq;
    else
        cmd = MemCmd::WriteReq;

    PacketPtr pkt = new Packet(req, cmd);
    uint8_t *dummy_data = new uint8_t[1];
    *dummy_data = 0;
    pkt->dataDynamic(dummy_data);

    DPRINTF(STDebug, "Trying to access Addr 0x%x\n", pkt->getAddr());

    // Send memory request
    if (ports[proc_id]->sendTimingReq(pkt)) {
        this_sub_event->msgTriggered = true;
        DPRINTF(STDebug, "%d: Message Triggered:"
                  " Core %d; Thread %d; Event %d; Subevents size %d;"
                  " Addr 0x%x\n",
                  curTick(), proc_id, thread_id,
                  eventMap[thread_id]->front()->eventID,
                  eventMap[thread_id]->front()->subEventList->size(), addr);
    } else {
        warn("%d: Packet did not issue from ProcID: %d, ThreadID: %d",
             curTick(), proc_id, thread_id);
        // If the packet did not issue, must delete!
        // Note: No need to delete the data, the packet destructor
        // will delete it
        delete pkt;
    }
}

void
SynchroTrace::progressEvents(int proc_id)
{
    ThreadID event_thread_id = threadMap[proc_id].front();

    // Skip if thread hasn't been activated
    if (!threadStartedMap[event_thread_id])
        return;

    // Skip when all events completed
    if (eventMap[event_thread_id]->empty())
        return;

    STEvent *this_event = eventMap[event_thread_id]->front();
    SubEvent *top_sub_event = &(this_event->subEventList->front());

    if (top_sub_event->triggerTime > curTick() ||
        top_sub_event->msgTriggered) {
        // Schedule sub event
        if (!(coreEvents[proc_id]->scheduled()))
            schedule(*(coreEvents[proc_id]), top_sub_event->triggerTime);
        return;
    }


    // The first subevent contains no messages indicating it is completed and
    // we can pop it from the list.
    if (!top_sub_event->containsMsg) {

        if (this_event->eventClass != STEvent::THREAD_API) {
            this_event->subEventList->pop_front();
        } else
            progressPthreadEvent(this_event, proc_id);

        // Events with no remaining subevents are completed
        // and we can pop it from the events list
        if (this_event->subEventList->empty()) {
            // Print: Event was just completed
            printEvent(event_thread_id, true);

            // Delete event
            DPRINTF(STDebug, "Event %d completed for thread %d on core %d \n",
                    eventMap[event_thread_id]->front()->eventID,
                    event_thread_id, proc_id);

            STEvent *completed_event = eventMap[event_thread_id]->front();
            eventMap[event_thread_id]->pop_front();
            delete completed_event;

            // If multiple threads per core, check if we can swap threads
            if (threadMap[proc_id].size() > 1)
                swapThreads(proc_id);

            if (!eventMap[event_thread_id]->empty()) {
                // Print: Event is starting
                printEvent(event_thread_id, false);

                this_event = eventMap[event_thread_id]->front();
                parser->replenishEvents(this_event->evThreadID);
            }

            // Schedule core to handle new event
            if (!(coreEvents[proc_id]->scheduled())) {
                schedule(*(coreEvents[proc_id]), curTick());
            }

            DPRINTF(STDebug, "Core %d scheduled for %d\n", proc_id, curTick());

        } else {
            // Pull up the next subevent and schedule
            SubEvent *new_sub_event = &(this_event->subEventList->front());
            Tick comp_time = clockPeriod() *
                            (Cycles(CPI_IOPS * new_sub_event->numIOPS)
                            + Cycles(CPI_FLOPS * new_sub_event->numFLOPS));
            new_sub_event->triggerTime = comp_time + curTick() + clockPeriod();
            schedule(*(coreEvents[proc_id]), new_sub_event->triggerTime);
            DPRINTF(STDebug, "Core %d scheduled for %d\n",
                    proc_id, new_sub_event->triggerTime);
        }
    } else {
        // The first sub-event contains an unprocessed message so we
        // attempt to create and trigger it here.
        if (this_event->eventClass == STEvent::COMM) {
            // Check if communication dependencies have been met. If yes,
            // trigger the msg. If not, reschedule wakeup for next cycle. When
            // consumer threads have a mutex lock, do not maintain the
            // dependency as this could cause a deadlock. Could be user-level
            // synchronization, and we do not want to maintain false
            // dependencies.
            if ((checkCommDependency(top_sub_event->thisMsg, event_thread_id)
//                || threadMutexMap[event_thread_id] != 0) || (pcSkip)) {
                || !threadMutexMap[event_thread_id].empty()) || (pcSkip)) {
                triggerMsg(proc_id, event_thread_id, top_sub_event);
            } else {
                // Check if dependency met next cycle
                top_sub_event->triggerTime = clockPeriod() * (curCycle() +
                                           Cycles(1));
                schedule(*(coreEvents[proc_id]), top_sub_event->triggerTime);
            }
        } else {
            // Send LD/ST for computation events
            triggerMsg(proc_id, event_thread_id, top_sub_event);
        }
    }
}

void
SynchroTrace::progressPthreadEvent(STEvent *this_event, int proc_id)
{
    assert(this_event);
    bool consumed_event = false;
    set<Addr>::iterator mutex_ittr, spin_ittr;

    ThreadID slave_thread_id;
    int slave_proc_id;
    //int wait_count;
    //bool all_thread_wait;
    uint64_t mutexLockAddr;
    std::map<Addr, int>::iterator cond_signal_ittr;

    switch (this_event->eventType) {
      case STEvent::MUTEX_LOCK:
        if (mutexLocks.find(this_event->pthAddr) == mutexLocks.end()) {
          mutexLocks.insert(this_event->pthAddr);
//          threadMutexMap[this_event->evThreadID] = this_event->pthAddr;
          threadMutexMap[this_event->evThreadID].push_back(this_event-> \
                          pthAddr);
          // Thread is now holding mutex lock.

          DPRINTF(STMutexLogger,"Thread %d locked mutex %d\n",
                  this_event->evThreadID, this_event->pthAddr);
          consumed_event = true;
        }
        break;

      case STEvent::MUTEX_UNLOCK:
        mutex_ittr = mutexLocks.find(this_event->pthAddr);
        //assert(mutex_ittr != mutexLocks.end());
        if (mutex_ittr != mutexLocks.end())
            mutexLocks.erase(mutex_ittr);
        //threadMutexMap[this_event->evThreadID] = 0;
        threadMutexMap[this_event->evThreadID].pop_back();
        // Thread returned mutex lock.

        DPRINTF(STMutexLogger,"Thread %d unlocked mutex %d\n",
                this_event->evThreadID, this_event->pthAddr);
        consumed_event = true;
        break;

      case STEvent::THREAD_CREATE:
        if (!roiFlag) {
            DPRINTF(ROI,"Reached parallel region.\n");
            roiFlag = true;
        }

        workerThreadCount++;
        slave_thread_id = addresstoIDMap[this_event->pthAddr];
        threadStartedMap[slave_thread_id] = true;
        // Activated Slave Thread
        consumed_event = true;

        // Wake up slave threads' cores
        // TODO - '%numCpus' is used to obtain the slave thread's proc_id
        // from the thread_id. This is only relevant for the default
        // round-robin scheduling.
        slave_proc_id = slave_thread_id % numCpus;
        if (!(coreEvents[slave_proc_id]->scheduled())) {
            schedule(*(coreEvents[slave_proc_id]), curTick() + clockPeriod());
        }
        DPRINTF(STDebug, "Thread %d created \n",
                addresstoIDMap[this_event->pthAddr]);
        break;

      case STEvent::THREAD_JOIN:
        //assert(threadStartedMap[addresstoIDMap[this_event->pthAddr]]);
        if (eventMap[addresstoIDMap[this_event->pthAddr]]->empty())
            consumed_event = true;
        workerThreadCount--;
        if (workerThreadCount == 0)
            DPRINTF(ROI,"Last Thread Joined.\n");
        break;

      case STEvent::BARRIER_WAIT:
        if (!threadContMap[this_event->evThreadID]) {
            // Put thread in waitmap
            if (threadWaitMap[this_event->pthAddr].find(this_event->
                                                        evThreadID)
                == threadWaitMap[this_event->pthAddr].end()) {
                threadWaitMap[this_event->pthAddr].insert(this_event->
                                                          evThreadID);
                threadStartedMap[this_event->evThreadID] = false;
            }

            if (checkBarriers(this_event)) {
                if ((startSyncRegion != 0) || barrStatDump) {
                   // Dump stats every barrier if synchronization region
                   Stats::schedStatEvent(true, true, curTick(), 0);
                }

                set<ThreadID>::iterator barr_ittr =
                    barrierMap[this_event->pthAddr].begin();
                for (; barr_ittr != barrierMap[this_event->pthAddr].end();
                    barr_ittr++) {
                    threadStartedMap[*barr_ittr] = true;
                    threadContMap[*barr_ittr] = true;
                }
                threadWaitMap[this_event->pthAddr].clear();

                // Release current thread
                threadContMap[this_event->evThreadID] = false;
                consumed_event = true;
            }
        } else {
            // Release waiting threads
            threadContMap[this_event->evThreadID] = false;
            consumed_event = true;
        }

      break;

      case STEvent::COND_WAIT:
        // Unlock mutex lock of condition wait call
        mutexLockAddr = this_event->mutexLockAddr;
        mutex_ittr = mutexLocks.find(mutexLockAddr);
        if (mutex_ittr != mutexLocks.end())
            mutexLocks.erase(mutex_ittr);

        // If cond var is 0, signal has not been posted => Wait
        // If cond var is > 0, decrement condSignal and progress the event
        cond_signal_ittr =
            condSignals[this_event->evThreadID].find(this_event->pthAddr);

        if (cond_signal_ittr != condSignals[this_event->evThreadID].end()) {
            if (cond_signal_ittr->second != 0) {
                // Signal posted by broadcast/signal. Decrement signal.
                cond_signal_ittr->second--;
                consumed_event = true;
            }
        }
        break;

/*      // Mutex lock is released prior to waiting, acquired after conditional
       // wait release
       switch (cond_wait_states[this_event->evThreadID]) {
          case INIT: // Push thread onto conditional wait queue
            cond_wait_queue.push(this_event->evThreadID);
            cond_wait_states[this_event->evThreadID] = QUEUED;

            // Unlock mutex for conditional wait
            mutex_ittr = mutexLocks.find(this_event->mutexLockAddr);
            //assert(mutex_ittr != mutexLocks.end());
            // TODO - Fix the broadcast first call mutex unlock
            if (mutex_ittr != mutexLocks.end())
                mutexLocks.erase(mutex_ittr);
            // Temporarily allow Thread to keep mutex address to acquire
            break;
          case QUEUED: // Thread already on wait queue
            break;
          case SIGNALED: // Thread has been signaled, removed from wait queue
            // TODO - Adding temp code to attempt to Acquire Mutex Lock
            //
            if (mutexLocks.find(this_event->mutexLockAddr) ==
                                mutexLocks.end()) {
                mutexLocks.insert(this_event->mutexLockAddr);
                // Thread is now holding mutex lock.

                DPRINTF(STMutexLogger,"Thread %d locked mutex %d\n",
                      this_event->evThreadID, this_event->mutexLockAddr);
                cond_wait_states[this_event->evThreadID] = INIT;
                consumed_event = true;
            }
            break;
       }
         break;
*/

      case STEvent::COND_SG:
        // TODO - Currently treated as a broadcast to resolve asynchronous
        // deadlocking across threads dependent on the condition variable

        // Post condition signal to all threads
        // for all threads:
        for (int tid = 0; tid < numThreads; tid++) {
            cond_signal_ittr =
                    condSignals[tid].find(this_event->pthAddr);

            if (cond_signal_ittr != condSignals[tid].end()) {
                // Add an additional signal to the condition variable
                cond_signal_ittr->second++;
            } else {
                // Add the condition variable and set the first signal
                condSignals[tid].insert({this_event->pthAddr, 1});
            }
        }
        consumed_event = true;
        break;

        // Pop one thread from the conditional wait queue
/*        if (!cond_wait_queue.empty()) {
            cond_wait_states[cond_wait_queue.front()] = SIGNALED;
            cond_wait_queue.pop();
        }
        consumed_event = true;
        break;
*/
      case STEvent::COND_BR:
        //// Unlock mutex lock of most recent mutex lock call
        //mutexLockAddr = threadMutexMap[this_event->evThreadID].back();
        //mutex_ittr = mutexLocks.find(mutexLockAddr);
        //assert(mutex_ittr != mutexLocks.end());
        //mutexLocks.erase(mutex_ittr);

        // Post condition signal to all threads
        // for all threads:
        for (int tid = 0; tid < numThreads; tid++) {
            cond_signal_ittr =
                    condSignals[tid].find(this_event->pthAddr);

            if (cond_signal_ittr != condSignals[tid].end()) {
                // Add an additional signal to the condition variable
                cond_signal_ittr->second++;
            } else {
                // Add the condition variable and set the first signal
                condSignals[tid].insert({this_event->pthAddr, 1});
            }
        }
        consumed_event = true;
        break;

/*        // TODO - Statement that incorporates unlocking mutex on first call
        // of broadcast
        //
        // Unlock mutex lock if calling thread holds it
//        mutexLockAddr = threadMutexMap[this_event->evThreadID];
        mutexLockAddr = threadMutexMap[this_event->evThreadID].back();
        mutex_ittr = mutexLocks.find(mutexLockAddr);
        //assert(mutex_ittr != mutexLocks.end());
        if (mutex_ittr != mutexLocks.end())
            mutexLocks.erase(mutex_ittr);

        // Check if all threads have reached the wait
        wait_count = 0;
        all_thread_wait = false;
        for (int i = 0; i < numContexts; i++) {
            if (cond_wait_states[i] == QUEUED)
                    wait_count += 1;
        }
        all_thread_wait = (wait_count == (numContexts - 1)) ? true : false;

        if (all_thread_wait) {
            // Obtain mutex lock again
            if (mutexLocks.find(mutexLockAddr) == mutexLocks.end()) {
                mutexLocks.insert(mutexLockAddr);
                consumed_event = true;
                // Pop all the threads from the conditional wait queue
                while (!cond_wait_queue.empty()) {
                    cond_wait_states[cond_wait_queue.front()] = SIGNALED;
                    cond_wait_queue.pop();
                }
            }
        }
        break;
*/

      case STEvent::SPIN_LOCK:
        if (spinLocks.find(this_event->pthAddr) == spinLocks.end()) {
            spinLocks.insert(this_event->pthAddr);
            consumed_event = true;
        }
        break;

      case STEvent::SPIN_UNLOCK:
        spin_ittr = spinLocks.find(this_event->pthAddr);
        assert(spin_ittr != spinLocks.end());
        spinLocks.erase(spin_ittr);
        consumed_event = true;
        break;

      case STEvent::SEM_INIT:
      case STEvent::SEM_WAIT:
      case STEvent::SEM_POST:
      case STEvent::SEM_GETV:
      case STEvent::SEM_DEST:
      default:
        panic("Invalid pthread event enum value %i.\n", this_event->eventType);
    }

    if (consumed_event) {
        // Pthread 'dummy' sub event completed
        this_event->subEventList->pop_front();
    }
}

bool
SynchroTrace::checkBarriers(STEvent *this_event)
{
    // Check if this thread is the last one for the barrier by
    // comparing the threads waiting on the barrier against the
    // map of threads required in the barrier.
    set<ThreadID> thread_wait_map_set;
    set<ThreadID> barrier_map_set;
    set<ThreadID> difference_set;

    thread_wait_map_set = threadWaitMap[this_event->pthAddr];
    barrier_map_set = barrierMap[this_event->pthAddr];
    set_difference(barrier_map_set.begin(), barrier_map_set.end(),
                   thread_wait_map_set.begin(), thread_wait_map_set.end(),
                   inserter(difference_set,difference_set.begin()));
    if (difference_set.empty()){
        return true;
    } else {
        return false;
    }
}

void
SynchroTrace::createSubEvents(int proc_id, bool event_id_passed,
                              ThreadID event_thread_id)
{
    if (!event_id_passed)
        event_thread_id = threadMap[proc_id].front();

    // Skip when thread is inactive
    if (!threadStartedMap[event_thread_id])
        return;

    // Skip when no events
    if (eventMap[event_thread_id]->empty())
        return;

    STEvent *this_event = eventMap[event_thread_id]->front();
    if (!this_event->subEventsCreated) {
        // For Pthread events, create 'dummy' sub event.
        if (this_event->eventClass == STEvent::THREAD_API) {
            this_event->subEventList = new deque<SubEvent>;
            SubEvent sub_event(0, 0, false, false);
            this_event->subEventList->push_back(sub_event);
        } else if (this_event->eventClass == STEvent::COMM) {
            // For communication events, create read-based sub events
            // for each dependency.
            this_event->subEventList = new deque<SubEvent>;

            for (unsigned long j = 0;
                 j < this_event->commPreRequisiteEvents.size(); j++) {
                SubEvent sub_event(0, 0, SubEvent::REQ_READ, false, true,
                                   this_event->commPreRequisiteEvents[j]);
                this_event->subEventList->push_back(sub_event);
            }
        } else {
            // Computation Event
            unsigned long max_loc_reads;
            unsigned long max_loc_writes;

            if (this_event->compMemReads >=
                this_event->compReadEvents.size()) {
                max_loc_reads = this_event->compMemReads;
            } else
                max_loc_reads = this_event->compReadEvents.size();

            if (this_event->compMemWrites >=
                this_event->compWriteEvents.size()) {
                max_loc_writes = this_event->compMemWrites;
            } else
                max_loc_writes = this_event->compWriteEvents.size();

            unsigned long total_mem_ops = max_loc_reads + max_loc_writes;
            // Create a single sub event if there are no memory ops.
            if (total_mem_ops == 0) {
                this_event->subEventList = new deque<SubEvent>;
                SubEvent sub_event(this_event->compIOPS,
                                   this_event->compFLOPS, false, false);
                this_event->subEventList->push_back(sub_event);
            } else {
                // Split up compute ops across the number of requests, i.e.
                // the number of sub events.
                unsigned long IOPS_div = this_event->compIOPS /
                                         total_mem_ops;
                unsigned int IOPS_rem = this_event->compIOPS %
                                         total_mem_ops;
                unsigned long FLOPS_div = this_event->compFLOPS /
                                          total_mem_ops;
                unsigned int FLOPS_rem = this_event->compFLOPS %
                                         total_mem_ops;

                unsigned long mem_reads_inserted = 0;
                unsigned long mem_writes_inserted = 0;

                // Mark off memory accesses and distribute the IOPS
                // and FLOPS evenly
                this_event->subEventList = new deque<SubEvent>;

                // Write first then read
                LocalMemAccessType init_type = WRITE;
                for (unsigned long j = 0; j < total_mem_ops; j++) {
                    switch(memTypeToInsert(mem_reads_inserted,
                           mem_writes_inserted, max_loc_reads,
                           max_loc_writes, init_type)) {
                        case READ:
                        {
                            SubEvent sub_event(
                                IOPS_div, FLOPS_div,
                                SubEvent::REQ_READ, false, true,
                                this_event->
                                compReadEvents[mem_reads_inserted %
                                this_event->compReadEvents.size()]);
                            this_event->subEventList->push_back(sub_event);
                            mem_reads_inserted++;
                            break;
                        }
                        case WRITE:
                        {
                            SubEvent sub_event(
                                IOPS_div, FLOPS_div,
                                SubEvent::REQ_WRITE, false, true, this_event->
                                compWriteEvents[mem_writes_inserted %
                                this_event->compWriteEvents.size()]);
                            this_event->subEventList->push_back(sub_event);
                            mem_writes_inserted++;
                            break;
                        }
                        default:
                            panic("Invalid memory request type.\n");
                    }

                    // Flip read/write
                    init_type = (init_type == WRITE) ? READ : WRITE;
                }

                // Distribute the residuals randomly
                for (unsigned int j = 0; j < IOPS_rem; j++) {
                    (*(this_event->subEventList))
                        [rand() % total_mem_ops].numIOPS++;
                }

                for (unsigned int j = 0; j < FLOPS_rem; j++) {
                    (*(this_event->subEventList))
                        [rand() % total_mem_ops].numFLOPS++;
                }
            }
        }
        // Schedule the sub event
        SubEvent *top_sub_event = &(this_event->subEventList->front());
        Tick comp_time = clockPeriod() * (Cycles(CPI_IOPS *
                        top_sub_event->numIOPS) + Cycles(CPI_FLOPS *
                        top_sub_event->numFLOPS));
        top_sub_event->triggerTime = comp_time + curTick() + clockPeriod();
        this_event->subEventsCreated = true;
    }
}

SynchroTrace::LocalMemAccessType
SynchroTrace::memTypeToInsert(unsigned long loc_reads,
                              unsigned long loc_writes,
                              unsigned long max_loc_reads,
                              unsigned long max_loc_writes,
                              LocalMemAccessType type)
{
    // Either the number of local reads or writes must not have reached the max
    // allowed.
    assert(loc_reads < max_loc_reads || loc_writes < max_loc_writes);
    // Assert that the type is valid.
    assert(type == READ || type == WRITE);

    if (type == READ && loc_reads < max_loc_reads) return READ;
    if (loc_writes < max_loc_writes)
        return WRITE;
    else
        return READ;
}
