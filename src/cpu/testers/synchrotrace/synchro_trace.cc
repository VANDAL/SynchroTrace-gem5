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
 *          Mike Lui
 */

#include "sim/sim_exit.hh"
#include "synchro_trace.hh"

SynchroTraceReplayer::SynchroTraceReplayer(const Params *p)
  : MemObject(p),
    // general configuration
    numCpus(p->num_cpus),
    numThreads(p->num_threads),
    numContexts(std::min(p->num_threads, p->num_cpus)),
    barrStatDump(p->barrier_stat_dump),
    eventDir(p->event_dir),
    outDir(p->output_dir),
    CPI_IOPS(p->cpi_iops),
    CPI_FLOPS(p->cpi_flops),
    cxtSwitchTicks(100),
    pthTicks(1),
    pcSkip(p->pc_skip),
    // memory configuration
    useRuby(p->ruby),
    blockSizeBytes(p->block_size_bytes),
    blockSizeBits(floorLog2(p->block_size_bytes)),
    memorySizeBytes(p->mem_size_bytes),
    // general per-thread/core state
    perThreadStatus(p->num_threads, ThreadStatus::INACTIVE),
    coreToThreadMap(p->num_cpus),
    // synchronization state
    pthMetadata(p->event_dir),
    perThreadBarrierBlocked(p->num_threads, false),
    perThreadLocksHeld(p->num_threads),
    condSignals(p->num_threads),
    // gem5 events to wakeup at specific cycles
    wakeupFreqForMonitor(p->monitor_wakeup_freq),
    wakeupFreqForDebugLog(50000*p->monitor_wakeup_freq),
    synchroTraceMonitorEvent(*this),
    synchroTraceDebugLogEvent(*this),
    masterID(p->system->getMasterId(this, name()))
{
    // MDL20190622 Some members MUST be set in the init routine
    // because the initialization order of all SimObjects is undefined
    // afaict. Variables that depend on other SimObjects being initialized
    // should be set in the init routine. Only variables that depend on
    // input parameters should be set here in the constructor.
    //
    // In contrast, some member MUST be set in the constructor
    // because they are needed BEFORE `init` gets to run.
    // Namely, the ports need to be generated, as gem5 uses
    // them when connected up SimObjects (before `init`ing).

    fatal_if(p->num_cpus > std::numeric_limits<CoreID>::max(),
             "cannot support %d cpus", p->num_cpus);
    fatal_if(!isPowerOf2(numCpus),
             "number of cpus expected to be power of 2, but got %d",
             numCpus);

    // Initialize the SynchroTrace to Memory ports
    for (CoreID i = 0; i < numCpus; i++)
        ports.emplace_back(csprintf("%s-port%d", name(), i), *this, i);
}

void
SynchroTraceReplayer::init()
{
    // Memory configuration from Ruby
    if (useRuby)
    {
        blockSizeBytes = RubySystem::getBlockSizeBytes();
        blockSizeBits = RubySystem::getBlockSizeBits();
    }
    fatal_if(!isPowerOf2(blockSizeBytes),
             "cache block size expected to be power of 2, but got: %d",
             blockSizeBytes);

    // Initialize gem5 event per core
    for (int i = 0; i < numContexts; i++)
        coreEvents.emplace_back(*this, i);

    // Currently map threads in a simple round robin fashion on cores.
    // The first thread ID on a core (front) is the currently active thread.
    for (int i = 0; i < numThreads; i++)
        coreToThreadMap.at(threadToCore(i)).push_back(i);

    // Initialize synchronization statistics
    workerThreadCount = 0;

    // Initialize stats
    hourCounter = std::time(NULL);
    roiFlag = false;

    // Create trace streams for each event thread
    for (ThreadID tid = 0; tid < numThreads; tid++)
        perThreadEventStreams.emplace_back(
            tid, eventDir, blockSizeBytes, memorySizeBytes);

    // Set master thread as active
    perThreadStatus[0] = ThreadStatus::ACTIVE;

    // Schedule first tick of master simulator thread
    schedule(synchroTraceMonitorEvent, 1);

    // Schedule first tick of the simulator cores
    for (int core = 0; core < numContexts; core++)
        schedule(coreEvents[core], clockPeriod());

    // Schedule the logging event, if enabled
    if (DTRACE(STIntervalPrint))
        schedule(synchroTraceDebugLogEvent, clockPeriod());
}

void
SynchroTraceReplayer::wakeupMonitor()
{
    // Terminates the simulation if all the threads are done
    if (std::all_of(perThreadStatus.cbegin(), perThreadStatus.cend(),
                    [](ThreadStatus status) {
                        return status == ThreadStatus::COMPLETED;
                    }))
        exitSimLoop("SynchroTrace completed");

    // Prints thread status every hour
    if (DTRACE(STIntervalPrintByHour) &&
        std::difftime(std::time(NULL), hourCounter) >= 60)
    {
        hourCounter = std::time(NULL);
        DPRINTFN("%s", std::ctime(&hourCounter));
        for (int i = 0; i < numThreads; i++)
            DPRINTFN("Thread %d: Event %d\n", i, perThreadCurrentEventId[i]);
    }

    // Reschedule self
    schedule(synchroTraceMonitorEvent,
             curTick() + clockPeriod() * wakeupFreqForMonitor);
}

void
SynchroTraceReplayer::wakeupDebugLog()
{
    for (int i = 0; i < numThreads; i++)
        DPRINTFN("Thread %d is on Event %d\n", i, perThreadCurrentEventId[i]);

    for (int i = 0; i < numCpus; i++)
        if (i < numThreads)
            DPRINTFN("Thread %d is on top Core %d\n",
                     coreToThreadMap[i][0], i);
        else
            DPRINTFN("No Thread is on top Core %d\n", i);

    for (int i = 0; i < numThreads; i++)
        DPRINTFN("Thread %d is %s\n", i, toString(perThreadStatus[i]));

    // Reschedule self
    schedule(synchroTraceDebugLogEvent,
             curTick() + clockPeriod() * wakeupFreqForDebugLog);
}


/******************************************************************************
 * Event Processing
 */

void
SynchroTraceReplayer::wakeupCore(CoreID coreId)
{
    // Each time a core wakes up:
    // - Finds the current running thread (returns immediately if no threads
    //   available)
    // - Gets the next event and processes it:
    //   - COMPUTE: Simulate compute time by sleeping for a certain amount of
    //              ticks.
    //   - MEMORY: Send a detailed memory access timing request through the
    //             memory system, and wakeup to continue processing events on
    //             the timing response.
    //   - COMM: Check if the producer thread already advanced past the source
    //           event, otherwise keep waiting until it has.
    //           A deadlock avoidance heuristic is use, by simply continuing if
    //           the current thread is holding a lock.
    //   - THREAD: Modify synchronization state as necessary based on the event
    //
    //  Each event handler is responsible for (optionally)
    //   - consuming the event, or not if it could not be satisfied
    //   - rescheduling the core (and implicitly its current thread)
    //     to process the next event
    //   - swapping any blocked threads
    //
    // MDL20190618 could refactor into a cleaner state machine.

    const ThreadID currThreadId = {coreToThreadMap[coreId].front()};
    panic_if(perThreadStatus[currThreadId] == ThreadStatus::INACTIVE ||
             perThreadStatus[currThreadId] == ThreadStatus::COMPLETED,
             "Trying to run Thread %d with status %s",
             currThreadId, toString(perThreadStatus[currThreadId]));

    StEventStream& evStream = perThreadEventStreams[currThreadId];
    switch (evStream.peek().tag)
    {
    /** Replay events */
    case StEvent::Tag::COMPUTE:
        replayCompute(evStream, coreId, currThreadId);
        break;
    case StEvent::Tag::MEMORY:
        replayMemory(evStream, coreId, currThreadId);
        break;
    case StEvent::Tag::MEMORY_COMM:
        replayComm(evStream, coreId, currThreadId);
        break;
    case StEvent::Tag::THREAD_API:
        replayThreadAPI(evStream, coreId, currThreadId);
        break;

    /** Meta events */
    case StEvent::Tag::EVENT_MARKER:
        // a simple metadata marker; reschedule the next actual event
        DPRINTF(STEventPrint, "Started %d, %d\n",
                currThreadId, evStream.peek().eventId);
        schedule(coreEvents[coreId], curTick());
        evStream.pop();
        break;
    case StEvent::Tag::INSN_MARKER:
        // a simple metadata marker; reschedule the next actual event
        // TODO(now) track stats
        schedule(coreEvents[coreId], curTick());
        evStream.pop();
        break;
    case StEvent::Tag::END_OF_EVENTS:
        perThreadStatus[currThreadId] = ThreadStatus::COMPLETED;
        evStream.pop();
        break;
    default:
        panic("Unexpected event encountered!");
    }
}

void
SynchroTraceReplayer::replayCompute(
    StEventStream& evStream, CoreID coreId, ThreadID threadId)
{
    assert(evStream.peek().tag == StEvent::Tag::COMPUTE);

    // simulate time for the iops/flops
    const ComputeOps& ops = evStream.peek().computeOps;
    schedule(coreEvents[coreId],
             curTick() + clockPeriod() +
             (clockPeriod() * (Cycles(CPI_IOPS * ops.iops) +
                               Cycles(CPI_FLOPS * ops.flops))));
    evStream.pop();
}

void
SynchroTraceReplayer::replayMemory(
    StEventStream& evStream, CoreID coreId, ThreadID threadId)
{
    assert(evStream.peek().tag == StEvent::Tag::MEMORY);

    // Send LD/ST for computation events
    const StEvent& ev = evStream.peek();
    msgReqSend(coreId,
               threadId,
               ev.eventId,
               ev.memoryReq.addr,
               ev.memoryReq.bytesRequested,
               ev.memoryReq.type);
    evStream.pop();
    // Do not reschedule wakeup; will wakeup again on the timing response.
}

void
SynchroTraceReplayer::replayComm(
    StEventStream& evStream, CoreID coreId, ThreadID threadId)
{
    assert(evStream.peek().tag == StEvent::Tag::MEMORY_COMM);

    // Check if communication dependencies have been met.
    //
    // When consumer threads have a mutex lock, do not maintain the
    // dependency as this could cause a deadlock. Could be user-level
    // synchronization, and we do not want to maintain false
    // dependencies.

    const StEvent& ev = evStream.peek();
    if (pcSkip ||
        (isCommDependencyBlocked(ev.memoryReqComm) ||
         !perThreadLocksHeld[threadId].empty()))
    {
        // If dependencies have been met, trigger the read reqeust.
        msgReqSend(coreId,
                   ev.threadId,
                   ev.eventId,
                   ev.memoryReqComm.addr,
                   ev.memoryReqComm.bytesRequested,
                   ReqType::REQ_READ);
        evStream.pop();
        // Do not reschedule wakeup; will wakeup again on the timing response.
    }
    else
    {
        perThreadStatus[threadId] = ThreadStatus::BLOCKED_COMM;
        tryCxtSwapAndSchedule(coreId);
    }
}

void
SynchroTraceReplayer::replayThreadAPI(
    StEventStream& evStream, CoreID coreId, ThreadID threadId)
{
    // This is the most complex replay function as it handles the most state
    // transitions within the system, and does several sanity checks to make
    // sure threads have been replayed in a sensible way.
    //
    // N.B. Rather than create a complex state machine architecture, we instead
    // simply give responsibility for popping events, rescheduling,
    // and swapping threads to each event case.
    // This is to make future modification more accessible.

    assert(evStream.peek().tag == StEvent::Tag::THREAD_API);
    const StEvent& ev = evStream.peek();
    const Addr pthAddr = ev.threadApi.pthAddr;

    switch (ev.threadApi.eventType)
    {
    // Lock/unlock
    case ThreadApi::EventType::MUTEX_LOCK:
    {
        auto p = mutexLocks.insert(pthAddr);
        if (p.second)
        {
            perThreadStatus[threadId] = ThreadStatus::ACTIVE;
            perThreadLocksHeld[threadId].push_back(pthAddr);
            DPRINTF(STMutexLogger,
                    "Thread %d locked mutex <0x%X>",
                    threadId,
                    pthAddr);
            evStream.pop();
            schedule(coreEvents[coreId],
                     curTick() + clockPeriod() * Cycles(pthTicks));
        }
        else
        {
            perThreadStatus[threadId] = ThreadStatus::BLOCKED_MUTEX;
            DPRINTF(STMutexLogger,
                    "Thread %d blocked trying to lock mutex <0x%X>",
                    threadId,
                    pthAddr);
            tryCxtSwapAndSchedule(coreId);
        }
    }
        break;
    case ThreadApi::EventType::MUTEX_UNLOCK:
    {
        std::vector<Addr>& locksHeld = perThreadLocksHeld[threadId];
        auto s_it = mutexLocks.find(pthAddr);
        auto v_it = std::find(locksHeld.begin(), locksHeld.end(), pthAddr);

        fatal_if(s_it == mutexLocks.end(),
                 "Thread %d tried to unlock mutex <0x%X> before having lock!",
                 threadId,
                 pthAddr);
        fatal_if(v_it == locksHeld.end(),
                 "Thread %d tried to unlock mutex <0x%X> "
                 "but a different thread holds the lock!",
                 threadId,
                 pthAddr);

        mutexLocks.erase(s_it);
        locksHeld.erase(v_it);
        evStream.pop();
        schedule(coreEvents[coreId],
                 curTick() + clockPeriod() * Cycles(pthTicks));
    }
        break;

    // Thread Spawn/Join
    case ThreadApi::EventType::THREAD_CREATE:
    {
        panic_if(perThreadStatus[threadId] != ThreadStatus::ACTIVE,
                 "Thread %d is creating other threads but isn't event ACTIVE!",
                 threadId);

        if (!roiFlag)
        {
            roiFlag = true;
            DPRINTF(ROI, "Reached parallel region");
        }

        workerThreadCount++;

        const ThreadID serfThreadId =
            {pthMetadata.addressToIdMap().at(pthAddr)};

        fatal_if(perThreadStatus[serfThreadId] != ThreadStatus::INACTIVE,
                 "Tried to create Thread %d, but it already exists!",
                 serfThreadId);
        perThreadStatus[serfThreadId] = ThreadStatus::ACTIVE;

        // wake up core (only core 0 is initially working)
        const CoreID serfCoreId = {threadToCore(serfThreadId)};
        schedule(coreEvents[serfCoreId], curTick() + clockPeriod());

        DPRINTF(STDebug, "Thread %d created", serfThreadId);
        evStream.pop();
        schedule(coreEvents[coreId],
                 curTick() + clockPeriod() * Cycles(pthTicks));
    }
        break;
    case ThreadApi::EventType::THREAD_JOIN:
    {
        const ThreadID serfThreadId =
            {pthMetadata.addressToIdMap().at(pthAddr)};

        const ThreadStatus serfStatus =
            {perThreadStatus[serfThreadId]};

        switch (serfStatus)
        {
        case ThreadStatus::COMPLETED:
            // reset to active, in case this thread was previously blocked
            perThreadStatus[threadId] = ThreadStatus::ACTIVE;
            workerThreadCount--;
            DPRINTF(STDebug, "Thread %d joined", serfThreadId);
            if (DTRACE(ROI) && !workerThreadCount)
                DPRINTFN("Last thread joined!");
            evStream.pop();
            schedule(coreEvents[coreId],
                     curTick() + clockPeriod() * Cycles(pthTicks));
            break;
        case ThreadStatus::ACTIVE:
        case ThreadStatus::BLOCKED_COMM:
        case ThreadStatus::BLOCKED_MUTEX:
        case ThreadStatus::BLOCKED_BARRIER:
        case ThreadStatus::BLOCKED_COND:
        case ThreadStatus::BLOCKED_JOIN:
            perThreadStatus[threadId] = ThreadStatus::BLOCKED_JOIN;
            DPRINTF(STDebug,
                    "Thread %d on Core %d blocked trying to join Thread %d"
                    " with status: %s",
                    threadId,
                    coreId,
                    serfThreadId,
                    toString(perThreadStatus[serfThreadId]));
            tryCxtSwapAndSchedule(coreId);
            break;
        case ThreadStatus::INACTIVE:
            fatal("Tried joining Thread %d that was not created yet!",
                  serfThreadId);
            break;
        default:
            panic("Unexpected thread status detected in join!");
            break;
        }
    }
        break;

    // Barriers
    case ThreadApi::EventType::BARRIER_WAIT:
    {
        auto p = threadBarrierMap[pthAddr].insert(threadId);
        fatal_if(p.second, "Thread %d already waiting in barrier <0x%X>",
                 threadId,
                 pthAddr);

        // Check if this is the last thread to enter the barrier,
        // in which case, unblock all the threads.
        if (threadBarrierMap[pthAddr] == pthMetadata.barrierMap().at(pthAddr))
        {
            for (auto tid : pthMetadata.barrierMap().at(pthAddr))
            {
                // We always expect the thread to have more events after the
                // barrier.
                // That is, we never expected the barrier to be the last event.
                //
                // Expect this thread to be in the list of threads being
                // reactivated
                perThreadStatus[tid] = ThreadStatus::ACTIVE;
            }
            // clear the barrier
            fatal_if(threadBarrierMap.erase(pthAddr) != 1,
                     "Tried to clear barrier that doesn't exist! <0x%X>",
                     pthAddr);
            evStream.pop();
            schedule(coreEvents[coreId],
                     curTick() + clockPeriod() * Cycles(pthTicks));
        }
        else
        {
            perThreadStatus[threadId] = ThreadStatus::BLOCKED_BARRIER;
            tryCxtSwapAndSchedule(coreId);
        }
    }
        break;

    // Condition variables
    //
    // HEURISTIC: We treat signals and broadcasts the same because of
    // deadlocking problems.
    // When a condition signal/broadcast is encountered, we record it for all
    // threads. If a thread later encounters a wait on a condition that has
    // already been signaled/broadcast, we immediately continue and remove that
    // signal from a central container.
    //
    // Put another way, we ensure no signals are "missed".
    // The trade off is that condition waits will not be delayed as long as
    // they should be (or at all). An alternative is to store a counter for
    // each condition variable and treat it as a semaphore.
    //
    // In general, condition variables are difficult to effectively replay
    // because we don't capture the predicate that they would otherwise be
    // waiting on.
    //
    // TODO(someday) reimplement correctly
    case ThreadApi::EventType::COND_WAIT:
    {
        // unlock the mutex required for the wait
        const Addr mtx = {ev.threadApi.mutexLockAddr};
        const size_t erased = mutexLocks.erase(mtx);
        fatal_if(erased != 1,
                 "Thread %d tried to wait before holding mutex: <0x%X>",
                 threadId,
                 mtx);

        auto it = condSignals[threadId].find(pthAddr);
        if (it != condSignals[threadId].end() && it->second > 0)
        {
            // decrement signal and reactivate thread
            it->second--;
            perThreadStatus[threadId] = ThreadStatus::ACTIVE;
            evStream.pop();
            schedule(coreEvents[coreId],
                     curTick() + clockPeriod() * Cycles(pthTicks));
        }
        else
        {
            perThreadStatus[threadId] = ThreadStatus::BLOCKED_COND;
            tryCxtSwapAndSchedule(coreId);
        }
    }
        break;
    case ThreadApi::EventType::COND_SG:
    case ThreadApi::EventType::COND_BR:
    {
        panic_if(perThreadStatus[threadId] != ThreadStatus::ACTIVE,
                 "Thread %d is signaling/broadcasting, "
                 "but isn't event ACTIVE!",
                 threadId);
        // post condition signal to all threads
        for (ThreadID tid = 0; tid < numThreads; tid++)
        {
            // If the condition doesn't exist yet for the thread,
            // it will be inserted and value-initialized, so the
            // mapped_type (Addr) will default to `0`.
            condSignals[tid][pthAddr]++;
        }
        evStream.pop();
        schedule(coreEvents[coreId],
                 curTick() + clockPeriod() * Cycles(pthTicks));
    }
        break;

    // Spin locks
    // Threads should always be ACTIVE when acquiring/releasing spin locks.
    // However, an event is not necessarily completed.
    case ThreadApi::EventType::SPIN_LOCK:
    {
        panic_if(perThreadStatus[threadId] != ThreadStatus::ACTIVE,
                 "Thread %d is spinlocking, but isn't event ACTIVE!",
                 threadId);
        auto p = spinLocks.insert(pthAddr);
        if (p.second)
            evStream.pop();

        // Reschedule regardless of whether the lock was acquired.
        // If the lock wasn't acquired, we spin and try again the next cycle.
        schedule(coreEvents[coreId],
                 curTick() + clockPeriod() * Cycles(pthTicks));
    }
        break;
    case ThreadApi::EventType::SPIN_UNLOCK:
    {
        panic_if(perThreadStatus[threadId] != ThreadStatus::ACTIVE,
                 "Thread %d is spinunlocking, but isn't event ACTIVE!",
                 threadId);
        auto it = spinLocks.find(pthAddr);
        fatal_if(it == spinLocks.end(),
                 "Thread %d is unlocking spinlock <0x%X>, "
                 "but it isn't even locked!",
                 threadId,
                 pthAddr);
        // TODO(someday) check that THIS thread holds the lock, and not some
        // other thread
        spinLocks.erase(it);
        evStream.pop();
        schedule(coreEvents[coreId],
                 curTick() + clockPeriod() * Cycles(pthTicks));
    }
        break;

    // unimplemented
    using EventIntegerType = std::underlying_type<ThreadApi::EventType>::type;
    case ThreadApi::EventType::SEM_INIT:
    case ThreadApi::EventType::SEM_WAIT:
    case ThreadApi::EventType::SEM_POST:
    case ThreadApi::EventType::SEM_GETV:
    case ThreadApi::EventType::SEM_DEST:
        fatal("Unsupported Semaphore Event Type encountered: %d!",
              static_cast<EventIntegerType>(ev.threadApi.eventType));
        break;

    default:
        panic("Unexpected Thread Event Type encountered: %d!",
              static_cast<EventIntegerType>(ev.threadApi.eventType));
        break;
    }
}


/******************************************************************************
 * Helper functions
 */

void SynchroTraceReplayer::msgReqSend(CoreID coreId,
                                      ThreadID threadId,
                                      StEventID eventId,
                                      Addr addr,
                                      uint32_t bytes,
                                      ReqType type)
{
    // Packetize a simple memory request.

    // No special flags are required for the request because
    // - we only care about the timing of the underlying Packet within the
    //   memory system,
    // - and because we only thinly model a simple 1-CPI CPU.
    //
    // N.B. the address is most likely a (modified-to-be-valid)
    // virtual memory address.
    // SynchroTrace doesn't model a TLB for simulation speed-up at the cost of
    // some accuracy.
    RequestPtr req = std::make_shared<Request>(
        addr, bytes, Request::Flags{}, masterID);

    // MDL20190615 what is contextId used for?
    req->setContext(coreId);

    // Create the request packet that will be sent through the memory system.
    PacketPtr pkt = new Packet(req, type == ReqType::REQ_READ ?
                               MemCmd::ReadReq : MemCmd::WriteReq);

    // We don't care about the actual data since we're only interested in the
    // timing.
    pkt->allocate();

    DPRINTF(STDebug, "Requesting access to Addr 0x%x\n", pkt->getAddr());

    // Send memory request
    if (ports[coreId].sendTimingReq(pkt)) {
        DPRINTF(STDebug, "%d: Message Triggered:"
                " Core %d; Thread %d; Event %d; Addr 0x%x\n",
                curTick(), coreId, threadId, eventId, addr);
    } else {
        warn("%d: Packet did not issue from CoreID: %d, ThreadID: %d",
             curTick(), coreId, threadId);
        // If the packet did not issue, delete it and create a new one upon
        // reissue. Cannot reuse it because it's created with the current
        // system state.
        // Note: No need to delete the data, the packet destructor
        // will delete it
        delete pkt;

        fatal("Unexpected Timing Request failed for "
              "Core: %d; Thread: %d; Event: %zu\n", coreId, threadId, eventId);
    }
}

void
SynchroTraceReplayer::msgRespRecv(CoreID coreId, PacketPtr pkt)
{
    // Called upon timing response for this core from a memory request.

    assert(coreId < numContexts);

    // TODO(someday)
    // assert this is the expected timing response for the last request
    // this core/thread sent

    // Schedule core to handle next event, now
    schedule(coreEvents[coreId], curTick());
}

bool
SynchroTraceReplayer::isCommDependencyBlocked(
    const MemoryRequest_ThreadCommunication& comm)
{
    // If the producer thread's EventID is greater than the dependent event
    // then the dependency is satisfied
    return perThreadCurrentEventId[comm.sourceThreadId] > comm.sourceEventId;
}

void
SynchroTraceReplayer::tryCxtSwapAndSchedule(CoreID coreId)
{
    // TODO(soon)
    // This function assumes pthread tick timings, but it is also used if
    // a thread is blocked on a communication event. There should be separate
    // `swap` and `reschedule` timings passed in to allow the communication
    // event to be scheduled differently.
    // MDL20190619 How much different do we want to handle communication event
    // blocks?

    std::deque<ThreadID>& threadsOnCore = coreToThreadMap[coreId];
    assert(threadsOnCore.size() > 0);

    auto it = threadsOnCore.begin();
    while (++it != threadsOnCore.cend())
    {
        if (perThreadStatus[*it] != ThreadStatus::INACTIVE &&
            perThreadStatus[*it] != ThreadStatus::COMPLETED)
            break;
    }

    if (it != threadsOnCore.cend())
    {
        // Found a thread to swap.
        // Rotate threads round-robin.
        std::rotate(threadsOnCore.begin(), it, threadsOnCore.end());

        // Schedule the context swap.
        schedule(coreEvents[coreId],
                 curTick() + clockPeriod() * Cycles(cxtSwitchTicks));
    }
    else
    {
        // No thread available to run, reschedule the current thread to try
        // running again the next cycle.
        schedule(coreEvents[coreId], curTick() + clockPeriod() * Cycles(1));
    }
}

SynchroTraceReplayer*
SynchroTraceReplayerParams::create()
{
    return new SynchroTraceReplayer(this);
}
