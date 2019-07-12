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
    cxtSwitchCycles(p->cxt_switch_cycles),
    pthCycles(p->pth_cycles),
    schedSliceCycles(p->sched_slice_cycles),
    pcSkip(p->pc_skip),
    // memory configuration
    useRuby(p->ruby),
    blockSizeBytes(p->block_size_bytes),
    blockSizeBits(floorLog2(p->block_size_bytes)),
    memorySizeBytes(p->mem_size_bytes),
    // general per-thread/core state
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
    // MDL20190622 Some members should be set in the init routine
    // because the initialization order of all SimObjects is not guaranteed.
    // Afaict it is dependent upon the order of SimObject creation in the
    // python configuration script. Variables that depend on other SimObjects
    // being initialized should be set in the init routine. Variables that
    // depend only on input parameters should be set here in the constructor.
    //
    // In contrast, some members MUST be set in the constructor
    // because they are needed BEFORE `init` gets to run.
    // Namely, the ports need to be generated, as gem5 uses
    // them when connecting up SimObjects (before `init`ing).

    fatal_if(p->num_cpus > std::numeric_limits<CoreID>::max(),
             "cannot support %d cpus", p->num_cpus);
    fatal_if(!isPowerOf2(numCpus),
             "number of cpus expected to be power of 2, but got %d",
             numCpus);

    // Initialize the ports to the rest of the system
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

    // Initialize the events that will simulate time for each core
    // via sleeps/wakeups (scheduling a wakeup after N cycles).
    for (int i = 0; i < numContexts; i++)
        coreEvents.emplace_back(*this, i);

    // Currently map threads in a simple round robin fashion on cores.
    // The first thread context on a core (front) is the active thread.
    for (ThreadID i = 0; i < numThreads; i++)
        threadContexts.emplace_back(
            i, eventDir, blockSizeBytes, memorySizeBytes);
    for (auto& tcxt : threadContexts)
        coreToThreadMap.at(threadIdToCoreId(tcxt.threadId)).emplace_back(tcxt);

    // Initialize statistics
    workerThreadCount = 0;
    hourCounter = std::time(NULL);
    roiFlag = false;

    // Set master (first) thread as active.
    // Schedule first tick of the initial core.
    // (the other cores begin 'inactive', and
    //  expect the master thread to start them)
    threadContexts[0].status = ThreadStatus::ACTIVE;
    schedule(coreEvents[0], clockPeriod());

    // Schedule the monitor event to start and regularly
    // check the status of the replay system.
    schedule(synchroTraceMonitorEvent, 1);

    // Schedule the logging event, if enabled
    if (DTRACE(STIntervalPrint))
        schedule(synchroTraceDebugLogEvent, clockPeriod());
}

Port&
SynchroTraceReplayer::getPort(const std::string& if_name, PortID idx)
{
    // 'cpu_port' in SynchroTrace.py

    if (if_name != "cpu_port")
        return MemObject::getPort(if_name, idx);
    else if (idx > -1 && idx < static_cast<ssize_t>(ports.size()))
        return ports[idx];
    else
        fatal("synchrotrace: out-of-range getPort(%d)", idx);
}

void
SynchroTraceReplayer::wakeupMonitor()
{
    // Terminates the simulation if all the threads are done
    if (std::all_of(threadContexts.cbegin(), threadContexts.cend(),
                    [](const ThreadContext& tcxt)
                    { return tcxt.completed(); }))
        exitSimLoop("SynchroTrace completed");

    // Prints thread status every hour
    if (DTRACE(STIntervalPrintByHour) &&
        std::difftime(std::time(NULL), hourCounter) >= 60)
    {
        hourCounter = std::time(NULL);
        DPRINTFN("%s", std::ctime(&hourCounter));
        for (const auto& cxt : threadContexts)
            DPRINTFN("Thread<%d>:Event<%d>\n", cxt.threadId, cxt.currEventId);
    }

    // Reschedule self
    schedule(synchroTraceMonitorEvent,
             curTick() + clockPeriod() * wakeupFreqForMonitor);
}

void
SynchroTraceReplayer::wakeupDebugLog()
{
    for (const auto& cxt : threadContexts)
        DPRINTFN("Thread<%d>:Event<%d>:Status<%s>\n",
                 cxt.threadId,
                 cxt.currEventId,
                 toString(cxt.status));

    for (int i = 0; i < numCpus; i++)
        if (i < numThreads)
            DPRINTFN("Core<%d>:Thread<%d>\n",
                     i,
                     coreToThreadMap[i].front().get().threadId);
        else
            DPRINTFN("Core<%d>:EMPTY\n", i);

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
    // - Gets the next event for the active thread and processes it:
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

    ThreadContext& tcxt = coreToThreadMap[coreId].front();

    panic_if(!tcxt.running(),
             "Trying to run Thread %d with status %s",
             tcxt.threadId, toString(tcxt.status));

    switch (tcxt.evStream.peek().tag)
    {
    case StEvent::Tag::COMPUTE:
        replayCompute(tcxt, coreId);
        break;
    case StEvent::Tag::MEMORY:
        replayMemory(tcxt, coreId);
        break;
    case StEvent::Tag::MEMORY_COMM:
        replayComm(tcxt, coreId);
        break;
    case StEvent::Tag::THREAD_API:
        replayThreadAPI(tcxt, coreId);
        break;
    case StEvent::Tag::TRACE_EVENT_MARKER:
        processEventMarker(tcxt, coreId);
        break;
    case StEvent::Tag::INSN_MARKER:
        processInsnMarker(tcxt, coreId);
        break;
    case StEvent::Tag::END_OF_EVENTS:
        processEndMarker(tcxt, coreId);
        break;
    default:
        panic("unexpected event encountered: Thread<%d>:Event<%d>:Tag<%d>",
              tcxt.threadId,
              tcxt.currEventId,
              static_cast<std::underlying_type<StEvent::Tag>::type>
              (tcxt.evStream.peek().tag));
    }
}


/******************************************************************************
 * Replay
 */

void
SynchroTraceReplayer::replayCompute(ThreadContext& tcxt, CoreID coreId)
{
    assert(tcxt.evStream.peek().tag == StEvent::Tag::COMPUTE);

    // simulate time for the iops/flops
    const ComputeOps& ops = tcxt.evStream.peek().computeOps;
    schedule(coreEvents[coreId],
             curTick() + clockPeriod() +
             (clockPeriod() * (Cycles(CPI_IOPS * ops.iops) +
                               Cycles(CPI_FLOPS * ops.flops))));
    tcxt.evStream.pop();
}

void
SynchroTraceReplayer::replayMemory(ThreadContext& tcxt, CoreID coreId)
{
    assert(tcxt.evStream.peek().tag == StEvent::Tag::MEMORY);

    // Send the load/store
    const StEvent& ev = tcxt.evStream.peek();
    msgReqSend(coreId,
               ev.memoryReq.addr,
               ev.memoryReq.bytesRequested,
               ev.memoryReq.type);
    tcxt.evStream.pop();
    // Do not reschedule wakeup; will wakeup again on the timing response.
}

void
SynchroTraceReplayer::replayComm(ThreadContext& tcxt, CoreID coreId)
{
    assert(tcxt.evStream.peek().tag == StEvent::Tag::MEMORY_COMM);

    // Check if communication dependencies have been met.
    //
    // When consumer threads have a mutex lock, do not maintain the
    // dependency as this could cause a deadlock. Could be user-level
    // synchronization, and we do not want to maintain false
    // dependencies.

    const StEvent& ev = tcxt.evStream.peek();
    if (pcSkip ||
        (isCommDependencyBlocked(ev.memoryReqComm) ||
         !perThreadLocksHeld[tcxt.threadId].empty()))
    {
        // If dependencies have been met, trigger the read reqeust.
        msgReqSend(coreId,
                   ev.memoryReqComm.addr,
                   ev.memoryReqComm.bytesRequested,
                   ReqType::REQ_READ);
        tcxt.evStream.pop();
        // Do not reschedule wakeup; will wakeup again on the timing response.
    }
    else
    {
        tcxt.status = ThreadStatus::BLOCKED_COMM;

        // If there are no threads to swap then just keep checking until this
        // dependency is satisfied. This thread will wakeup again on the same
        // event.
        if (!tryCxtSwapAndSchedule(coreId))
            schedule(coreEvents[coreId],
                     curTick() + clockPeriod() * Cycles(1));
    }
}

void
SynchroTraceReplayer::replayThreadAPI(ThreadContext& tcxt, CoreID coreId)
{
    // This is the most complex replay function as it handles the most state
    // transitions within the system, and does several sanity checks to make
    // sure threads have been replayed in a sensible way.
    // Each case is responsible for popping events, rescheduling,
    // and swapping threads appropriately.
    //
    // If a thread is blocked and can't swap in a different active thread,
    // then it generally will simulate a kernel scheduler rescheduling the
    // thread via a gem5 event reschedule of the core.

    assert(tcxt.evStream.peek().tag == StEvent::Tag::THREAD_API);
    const StEvent& ev = tcxt.evStream.peek();
    const Addr pthAddr = ev.threadApi.pthAddr;

    switch (ev.threadApi.eventType)
    {
    // Lock/unlock
    case ThreadApi::EventType::MUTEX_LOCK:
    {
        auto p = mutexLocks.insert(pthAddr);
        if (p.second)
        {
            tcxt.status = ThreadStatus::ACTIVE;
            perThreadLocksHeld[tcxt.threadId].push_back(pthAddr);
            DPRINTF(STMutexLogger,
                    "Thread %d locked mutex <0x%X>",
                    tcxt.threadId,
                    pthAddr);
            tcxt.evStream.pop();
            schedule(coreEvents[coreId],
                     curTick() + clockPeriod() * Cycles(pthCycles));
        }
        else
        {
            tcxt.status = ThreadStatus::BLOCKED_MUTEX;
            DPRINTF(STMutexLogger,
                    "Thread %d blocked trying to lock mutex <0x%X>",
                    tcxt.threadId,
                    pthAddr);
            if (!tryCxtSwapAndSchedule(coreId))
                schedule(coreEvents[coreId],
                         curTick() + clockPeriod() * Cycles(schedSliceCycles));
        }
    }
        break;
    case ThreadApi::EventType::MUTEX_UNLOCK:
    {
        std::vector<Addr>& locksHeld = perThreadLocksHeld[tcxt.threadId];
        auto s_it = mutexLocks.find(pthAddr);
        auto v_it = std::find(locksHeld.begin(), locksHeld.end(), pthAddr);

        fatal_if(s_it == mutexLocks.end(),
                 "Thread %d tried to unlock mutex <0x%X> before having lock!",
                 tcxt.threadId,
                 pthAddr);
        fatal_if(v_it == locksHeld.end(),
                 "Thread %d tried to unlock mutex <0x%X> "
                 "but a different thread holds the lock!",
                 tcxt.threadId,
                 pthAddr);

        mutexLocks.erase(s_it);
        locksHeld.erase(v_it);
        tcxt.evStream.pop();
        schedule(coreEvents[coreId],
                 curTick() + clockPeriod() * Cycles(pthCycles));
    }
        break;

    // Thread Spawn/Join
    case ThreadApi::EventType::THREAD_CREATE:
    {
        panic_if(tcxt.status != ThreadStatus::ACTIVE,
                 "Thread %d is creating other threads but isn't event ACTIVE!",
                 tcxt.threadId);

        if (!roiFlag)
        {
            roiFlag = true;
            DPRINTF(ROI, "Reached parallel region");
        }

        workerThreadCount++;

        fatal_if(pthMetadata.addressToIdMap().find(pthAddr) ==
                 pthMetadata.addressToIdMap().cend(),
                 "could not find pthread thread id in "
                 "sigil.pthread.out file: %d\n"
                 "Are the traces and pthread file in sync?", pthAddr);

        const ThreadID workerThreadId =
            {pthMetadata.addressToIdMap().at(pthAddr)};
        fatal_if(workerThreadId >= threadContexts.size(),
                 "Tried to create Thread %d, "
                 "but simulation is configured with %d threads!",
                 workerThreadId,
                 numThreads);

        ThreadContext& workertcxt = threadContexts[workerThreadId];
        fatal_if(workertcxt.status != ThreadStatus::INACTIVE,
                 "Tried to create Thread %d, but it already exists!",
                 workerThreadId);

        workertcxt.status = ThreadStatus::ACTIVE;
        const CoreID workerCoreId = {threadIdToCoreId(workerThreadId)};

        // The new thread may be mapped to an already active core,
        // e.g. the currently executing core. Check that we don't
        // schedule a wakeup on the same core twice. Creating a
        // thread mapped to this core will NOT immediately cause a
        // context switch.
        // (scheduling the same event multiple times before it fires
        //  is not allowed)
        schedule(coreEvents[coreId],
                 curTick() + clockPeriod() * Cycles(pthCycles));
        if (!coreEvents[workerCoreId].scheduled())
            // wake up core (only core 0 is initially working)
            schedule(coreEvents[workerCoreId], curTick() + clockPeriod());

        DPRINTF(STDebug, "Thread %d created", workerThreadId);
        tcxt.evStream.pop();
    }
        break;
    case ThreadApi::EventType::THREAD_JOIN:
    {
        fatal_if(pthMetadata.addressToIdMap().find(pthAddr) ==
                 pthMetadata.addressToIdMap().cend(),
                 "no matching pthread value to join: %x", pthAddr);

        const ThreadContext& workertcxt =
            threadContexts[pthMetadata.addressToIdMap().at(pthAddr)];

        if (workertcxt.completed())
        {
            // reset to active, in case this thread was previously blocked
            tcxt.status = ThreadStatus::ACTIVE;
            workerThreadCount--;
            DPRINTF(STDebug, "Thread %d joined", workertcxt.threadId);
            if (DTRACE(ROI) && !workerThreadCount)
                DPRINTFN("Last thread joined!");
            tcxt.evStream.pop();
            schedule(coreEvents[coreId],
                     curTick() + clockPeriod() * Cycles(pthCycles));
        }
        else if (workertcxt.running())
        {
            tcxt.status = ThreadStatus::BLOCKED_JOIN;
            DPRINTF(STDebug,
                    "Thread %d on Core %d blocked trying to join Thread %d"
                    " with status: %s",
                    tcxt.threadId,
                    coreId,
                    workertcxt.threadId,
                    toString(workertcxt.status));
            if (!tryCxtSwapAndSchedule(coreId))
                schedule(coreEvents[coreId],
                         curTick() + clockPeriod() * Cycles(schedSliceCycles));
        }
        else
        {
            fatal("Tried joining Thread %d that was not created yet!",
                  workertcxt.threadId);
        }
    }
        break;

    // Barriers
    case ThreadApi::EventType::BARRIER_WAIT:
    {
        auto p = threadBarrierMap[pthAddr].insert(tcxt.threadId);
        fatal_if(p.second, "Thread %d already waiting in barrier <0x%X>",
                 tcxt.threadId,
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
                threadContexts[tid].status = ThreadStatus::ACTIVE;
            }
            // clear the barrier
            fatal_if(threadBarrierMap.erase(pthAddr) != 1,
                     "Tried to clear barrier that doesn't exist! <0x%X>",
                     pthAddr);
            tcxt.evStream.pop();
            schedule(coreEvents[coreId],
                     curTick() + clockPeriod() * Cycles(pthCycles));
        }
        else
        {
            tcxt.status = ThreadStatus::BLOCKED_BARRIER;
            if (!tryCxtSwapAndSchedule(coreId))
                schedule(coreEvents[coreId],
                         curTick() + clockPeriod() * Cycles(schedSliceCycles));
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
                 tcxt.threadId,
                 mtx);

        auto it = condSignals[tcxt.threadId].find(pthAddr);
        if (it != condSignals[tcxt.threadId].end() && it->second > 0)
        {
            // decrement signal and reactivate thread
            it->second--;
            tcxt.status = ThreadStatus::ACTIVE;
            tcxt.evStream.pop();
            schedule(coreEvents[coreId],
                     curTick() + clockPeriod() * Cycles(pthCycles));
        }
        else
        {
            tcxt.status = ThreadStatus::BLOCKED_COND;
            if (!tryCxtSwapAndSchedule(coreId))
                schedule(coreEvents[coreId],
                         curTick() + clockPeriod() * Cycles(schedSliceCycles));
        }
    }
        break;
    case ThreadApi::EventType::COND_SG:
    case ThreadApi::EventType::COND_BR:
    {
        panic_if(tcxt.status != ThreadStatus::ACTIVE,
                 "Thread %d is signaling/broadcasting, "
                 "but isn't event ACTIVE!",
                 tcxt.threadId);
        // post condition signal to all threads
        for (ThreadID tid = 0; tid < numThreads; tid++)
        {
            // If the condition doesn't exist yet for the thread,
            // it will be inserted and value-initialized, so the
            // mapped_type (Addr) will default to `0`.
            condSignals[tid][pthAddr]++;
        }
        tcxt.evStream.pop();
        schedule(coreEvents[coreId],
                 curTick() + clockPeriod() * Cycles(pthCycles));
    }
        break;

    // Spin locks
    // Threads should always be ACTIVE when acquiring/releasing spin locks.
    // However, an event is not necessarily completed.
    case ThreadApi::EventType::SPIN_LOCK:
    {
        panic_if(tcxt.status != ThreadStatus::ACTIVE,
                 "Thread %d is spinlocking, but isn't event ACTIVE!",
                 tcxt.threadId);
        auto p = spinLocks.insert(pthAddr);
        if (p.second)
            tcxt.evStream.pop();

        // Reschedule regardless of whether the lock was acquired.
        // If the lock wasn't acquired, we spin and try again the next cycle.
        schedule(coreEvents[coreId],
                 curTick() + clockPeriod() * Cycles(pthCycles));
    }
        break;
    case ThreadApi::EventType::SPIN_UNLOCK:
    {
        panic_if(tcxt.status != ThreadStatus::ACTIVE,
                 "Thread %d is spinunlocking, but isn't event ACTIVE!",
                 tcxt.threadId);
        auto it = spinLocks.find(pthAddr);
        fatal_if(it == spinLocks.end(),
                 "Thread %d is unlocking spinlock <0x%X>, "
                 "but it isn't even locked!",
                 tcxt.threadId,
                 pthAddr);
        // TODO(someday) check that THIS thread holds the lock, and not some
        // other thread
        spinLocks.erase(it);
        tcxt.evStream.pop();
        schedule(coreEvents[coreId],
                 curTick() + clockPeriod() * Cycles(pthCycles));
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
 * Meta events
 */
void
SynchroTraceReplayer::processEventMarker(ThreadContext& tcxt, CoreID coreId)
{
    // a simple metadata marker; reschedule the next actual event
    DPRINTF(STEventPrint, "Started %d, %d\n",
            tcxt.threadId,
            tcxt.evStream.peek().traceEventMarker.eventId);
    assert(tcxt.currEventId ==
           tcxt.evStream.peek().traceEventMarker.eventId);
    tcxt.currEventId++;  // increment after, starts at 0
    schedule(coreEvents[coreId], curTick());
    tcxt.evStream.pop();
}

void
SynchroTraceReplayer::processInsnMarker(ThreadContext& tcxt, CoreID coreId)
{
    // a simple metadata marker; reschedule the next actual event
    // TODO(soonish) track stats
    schedule(coreEvents[coreId], curTick());
    tcxt.evStream.pop();
}

void
SynchroTraceReplayer::processEndMarker(ThreadContext& tcxt, CoreID coreId)
{
        tcxt.status = ThreadStatus::COMPLETED;
        tcxt.evStream.pop();

        // it's okay if there's not another thread to schedule
        (void)tryCxtSwapAndSchedule(coreId);
}


/******************************************************************************
 * Helper functions
 */

void SynchroTraceReplayer::msgReqSend(CoreID coreId,
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
    // N.B. the address is most likely a (modified-to-be-valid) virtual memory
    // address. SynchroTrace doesn't model a TLB, for simulation speed-up, at
    // the cost of some accuracy.
    RequestPtr req = std::make_shared<Request>(
        addr, bytes, Request::Flags{}, masterID);
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
        DPRINTF(STDebug,
                "Tick<%d>: Message Triggered:"
                " Core<%d>:Thread<%d>:Event<%d>:Addr<0x%x>\n",
                curTick(),
                coreId,
                coreToThreadMap[coreId].front().get().threadId,
                coreToThreadMap[coreId].front().get().currEventId,
                addr);
    } else {
        // The packet may not have been issued because another component
        // along the way became overwhelmed and had to drop the packet.
        warn("%d: Packet did not issue from CoreID: %d, ThreadID: %d",
             curTick(),
             coreId,
             coreToThreadMap[coreId].front().get().threadId);

        // If the packet did not issue, delete it and create a new one upon
        // reissue. Cannot reuse it because it's created with the current
        // system state.
        // Note: No need to delete the data, the packet destructor
        // will delete it
        delete pkt;

        // Because there will be no response packet to wakeup the core,
        // reschedule the core to try again next cycle.
        schedule(coreEvents[coreId], curTick() + clockPeriod());
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
SynchroTraceReplayer::tryCxtSwapAndSchedule(CoreID coreId)
{
    auto& threadsOnCore = coreToThreadMap[coreId];
    assert(threadsOnCore.size() > 0);

    auto it = std::find_if(std::next(threadsOnCore.begin()),
                           threadsOnCore.end(),
                           [](const ThreadContext &tcxt)
                           { return tcxt.running(); });

    // if no threads were found that could be swapped
    if (it == threadsOnCore.end())
        return false;

    // else we found a thread to swap.
    // Rotate threads round-robin and schedule the context swap.
    std::rotate(threadsOnCore.begin(), it, threadsOnCore.end());
    schedule(coreEvents[coreId],
             curTick() + clockPeriod() * Cycles(cxtSwitchCycles));
    return true;
}

SynchroTraceReplayer*
SynchroTraceReplayerParams::create()
{
    return new SynchroTraceReplayer(this);
}
