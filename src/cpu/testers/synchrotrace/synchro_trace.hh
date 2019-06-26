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

#ifndef __CPU_TESTERS_SYNCHROTRACE_SYNCHROTRACE_HH
#define __CPU_TESTERS_SYNCHROTRACE_SYNCHROTRACE_HH

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <queue>
#include <string>
#include <vector>

#include "base/intmath.hh"
#include "cpu/testers/synchrotrace/st_event.hh"
#include "cpu/testers/synchrotrace/st_parser.hh"
#include "debug/ROI.hh"
#include "debug/STDebug.hh"
#include "debug/STEventPrint.hh"
#include "debug/STIntervalPrint.hh"
#include "debug/STIntervalPrintByHour.hh"
#include "debug/STMutexLogger.hh"
#include "mem/mem_object.hh"
#include "mem/packet.hh"
#include "mem/ruby/common/Address.hh"
#include "mem/ruby/common/DataBlock.hh"
#include "mem/ruby/common/SubBlock.hh"
#include "mem/ruby/system/RubyPort.hh"
#include "mem/ruby/system/RubySystem.hh"
#include "params/SynchroTraceReplayer.hh"
#include "sim/stat_control.hh"
#include "sim/system.hh"

/* SynchroTrace's trace replay is a 1-CPI timing model that interfaces with
 * the multi-threaded traces from Sigil to inject traffic into the detailed
 * memory model.
 *
 * The capture tool, Sigil, leverages the Valgrind dynamic binary
 * instrumentation tool. The processed instructions from the native
 * multi-threaded applications are abstracted into (3) events:
 * Computation (Work performed local to a thread), Communication (Read/Write
 * dependencies between threads), and Synchronization (embedded pthread calls
 * for each thread). These events form a trace for each individual thread,
 * so that these threads may progress in parallel when replaying the traces.
 *
 * SynchroTrace's trace replay is comprised of one tester module of type
 * MemObject and contains the contexts of each individual cores. Each core's
 * events are scheduled into the global event queue as
 * 'coreEvents[processor]'. The replay module connects each core context to
 * each of the ports of the private L1s of the memory model. In addition to
 * the core contexts, a back-end simulation thread runs periodically to
 * replenish the events data structure for each thread and check if the
 * simulation has been completed.
 *
 * Each line of the traces are parsed (by STParser) into events, which are
 * contained in 'eventMap'. 'eventMap' is a list of pointers to each thread's
 * deque of events. As the events of the traces are potentially comprised of
 * multiple memory requests, events are broken up into sub events for each
 * request. These sub events hold a divided portion of the compute ops, as
 * well as one request.
 *
 * The normal execution flow of the replay module is as follows:
 *  1) Processor wakes up
 *  2) Sub events are generated for the top event of the thread
 *  3) Based on the compute ops for that subevent, a trigger time is
 *  scheduled to wakeup the core sometime in the future. The timing for the
 *  compute ops are configurable via the command line options 'cpi_iops' and
 *  'cpi_flops'.
 *  4) When the subevent's trigger time has been reached, a read or write
 *  request is sent out to the memory system and the core essentially blocks.
 *  5) Eventually the memory request returns as a "hitCallback" and the next
 *  subevent or event is scheduled.
 *
 * In the case of synchronization events:
 *  1) Once the master thread obtains a "create", it issues a wakeup to the
 *  slave thread. The master thread will also wait on a "join".
 *  2) Threads can attempt to obtain or release mutex locks and are held in
 *  barrier fences to preserve the native synchronization behavior.
 *
 * In the case of communication events, the replay module will enforce
 * producer->consumer dependencies by forcing the consumer thread to wait on
 * the producer thread reaching the dependent event. After the producer
 * thread has reached the dependent event, the consumer thread will then send
 * its memory read request to the memory system.
 *
 * The SynchroTrace replay module handles the scheduling of multiple
 * threads per core. After each event is completed, the replay module checks
 * if there is an available thread on that core to schedule and swaps
 * accordingly.
 *
 */

class SynchroTraceReplayer : public MemObject
{
  public:

    using CoreID = PortID;

    class CpuPort : public MasterPort
    {
      private:

        SynchroTraceReplayer& m_tester;

      public:

        CpuPort(const std::string &name,
                SynchroTraceReplayer& tester,
                PortID id)
          : MasterPort(name, &tester, id), m_tester(tester)
        {}

        virtual ~CpuPort()
        {}

      protected:

        /**
         * Receive memory request response from the memory system to the
         * SynchroTrace CPU port
         */
        virtual bool recvTimingResp(PacketPtr pkt) override
        {
            m_tester.msgRespRecv(id, pkt);

            // Only need timing of the memory request.
            // No need for the actual data.
            delete pkt;
            return true;
        }

        virtual void recvTimingSnoopReq(PacketPtr pkt) override
        {}

        virtual void recvReqRetry() override
        {
            panic("%s does not expect a retry\n", name());
        }
    };

    // XXX: DO NOT CHANGE THE ORDER OF THESE STATUSES
    enum class ThreadStatus {
        INACTIVE,
        ACTIVE,
        BLOCKED_COMM,
        BLOCKED_MUTEX,
        BLOCKED_BARRIER,
        BLOCKED_COND,
        BLOCKED_JOIN,
        COMPLETED,
        NUM_STATUSES
    };

    const char* toString(ThreadStatus status) const
    {
        switch (status) {
        case ThreadStatus::INACTIVE:
            return "INACTIVE";
        case ThreadStatus::ACTIVE:
            return "ACTIVE";
        case ThreadStatus::BLOCKED_COMM:
            return "BLOCKED_COMM";
        case ThreadStatus::BLOCKED_MUTEX:
            return "BLOCKED_MUTEX";
        case ThreadStatus::BLOCKED_BARRIER:
            return "BLOCKED_BARRIER";
        case ThreadStatus::BLOCKED_COND:
            return "BLOCKED_COND";
        case ThreadStatus::BLOCKED_JOIN:
            return "BLOCKED_JOIN";
        case ThreadStatus::COMPLETED:
            return "COMPLETED";
        default:
            panic("Unexpected Thread Status");
        }
    }

    using Params = SynchroTraceReplayerParams;
    SynchroTraceReplayer(const Params *p);
    SynchroTraceReplayer(const SynchroTraceReplayer &obj) = delete;
    SynchroTraceReplayer& operator=(const SynchroTraceReplayer &obj) = delete;

    virtual Port &getPort(const std::string &if_name,
                          PortID idx=InvalidPortID) override;

    virtual void init() override;

    /** Wake up the back-end simulation thread. */
    void wakeupMonitor();

    /** Debug logging. */
    void wakeupDebugLog();

    /** Wake up the core. */
    void wakeupCore(CoreID coreId);

  protected:
    class SynchroTraceMonitorEvent : public Event
    {
      private:
        SynchroTraceReplayer& m_tester;

      public:
        SynchroTraceMonitorEvent(SynchroTraceReplayer& tester)
          : Event(CPU_Tick_Pri), m_tester(tester)
        {}

        /** Waking up back-end simulation thread. */
        void process() { m_tester.wakeupMonitor(); }

        virtual const char *description() const {
            return "SynchroTrace tick";
        }
    };

    class SynchroTraceDebugLogEvent : public Event
    {
      private:
        SynchroTraceReplayer& m_tester;

      public:
        SynchroTraceDebugLogEvent(SynchroTraceReplayer& tester)
          : Event(Stat_Event_Pri), m_tester(tester)
        {}

        /** Waking up back-end simulation thread. */
        void process() { m_tester.wakeupDebugLog(); }

        virtual const char *description() const {
            return "SynchroTrace Debug tick";
        }
    };

    class SynchroTraceCoreEvent : public Event
    {
      private:
        SynchroTraceReplayer& m_tester;
        CoreID m_coreId;

      public:
        SynchroTraceCoreEvent(SynchroTraceReplayer& tester, CoreID coreId)
          : Event{CPU_Tick_Pri}, m_tester{tester}, m_coreId{coreId}
        {}
        /** Waking up cores. */
        void process() { m_tester.wakeupCore(m_coreId); }
        virtual const char *description() const {
            return "Core event tick";
        }
    };

    struct ThreadContext
    {
        ThreadID threadId;
        StEventID currEventId;
        StEventStream evStream;
        ThreadStatus status;

        ThreadContext(ThreadID threadId,
                      const std::string& eventDir,
                      uint32_t blockSizeBytes,
                      uint64_t memSizeBytes)
          : threadId{threadId},
            currEventId{0},
            evStream{static_cast<ThreadID>(threadId+1),
                     eventDir,
                     blockSizeBytes,
                     memSizeBytes},
            status{ThreadStatus::INACTIVE}
        {}
    };

  private:
    /**
     * Replay each event type.
     *
     * Some event replayers return a boolean to indicate whether the event was
     * completed or not (e.g. if the event is blocked).
     * Otherwise, the event unconditionally completes.
     */
    void replayCompute(ThreadContext& tcxt, CoreID coreId);
    void replayMemory(ThreadContext& tcxt, CoreID coreId);
    void replayComm(ThreadContext& tcxt, CoreID coreId);
    void replayThreadAPI(ThreadContext& tcxt, CoreID coreId);

    /** Send a blocking message request to memory system. */
    void msgReqSend(CoreID coreId, Addr addr, uint32_t bytes, ReqType type);

    /** Memory request returned. Schedule to wake up and process next event. */
    void msgRespRecv(PortID coreId, PacketPtr pkt);

    /**
     * For a communication event, check to see if the producer has reached
     * the dependent event. This function also handles the case of a system
     * call. System calls are viewed as producer->consumer interactions with
     * the 'producer' system call having a ThreadID of 30000. For obvious
     * reasons, there are no 'dependencies' to enforce in the case of a system
     * call.
     */
    bool isCommDependencyBlocked(
        const MemoryRequest_ThreadCommunication& comm) const
    {
        // If the producer thread's EventID is greater than the dependent event
        // then the dependency is satisfied
        return (threadContexts[comm.sourceThreadId].currEventId >
                comm.sourceEventId);
    }

    /**
     * Called when a thread detects it is blocked and cannot proceed to consume
     * the event. All other threads scheduled on the core are checked
     * (round-robin) to see if they are runnable, and if so, are rotated to the
     * front of the thread queue and scheduled. A runnable thread may be
     * blocked, in which case, upon being rescheduled, it is expected to just
     * recheck if it can unblock itself.
     *
     * Returns `true` if another thread was able to be scheduled and `false`
     * otherwise. It is up to the caller to decide whether or not to reschedule
     * itself.
     */
    bool tryCxtSwapAndSchedule(CoreID coreId);

    /** Simple helper. Threads are statically mapped to cores round-robin */
    CoreID threadIdToCoreId(ThreadID threadId) const
    {
        return threadId % numCpus;
    }

    /**************************************************************************
     * General configuration
     */

    /** Number of CPUs set from the cmd line */
    CoreID numCpus;

    /** Number of threads set from the cmd line */
    ThreadID numThreads;

    /**
     * Number of threads (if less than number of cores). Otherwise,
     * number of cores.
     */
    int numContexts;

    /** Option to dump stats following each barrier */
    bool barrStatDump;

    /** Directory of Sigil Traces and Pthread metadata file */
    std::string eventDir;

    /** Directory of output files */
    std::string outDir;

    /**
     * Counter used for 'STIntervalPrintByHour' to print event progression per
     * hour
     */
    std::time_t hourCounter;

    /**
     * Flag used for 'ROI' to print out entering and leaving the parallel
     * region
     */
    bool roiFlag;

    /**
     * Counter used for 'ROI' to print out entering and leaving the parallel
     * region
     */
    int workerThreadCount;

    /**************************************************************************
     * Timing configuration
     */

    /** Abstract cpi estimation for integer ops */
    const float CPI_IOPS;

    /** Abstract cpi estimation for floating point ops */
    const float CPI_FLOPS;

    /** Cycles for context switch on a core */
    const uint32_t cxtSwitchCycles;

    /** Cycles for pthread event */
    const uint32_t pthCycles;

    /** Cycles to simulate the time slice the scheduler gives to a thread */
    const uint32_t schedSliceCycles;

    /** Flag to skip Producer -> Consumer dependencies */
    const bool pcSkip;

    /**************************************************************************
     * Memory configuration
     */

    /**
     * Option to use Ruby
     * If Ruby is used, then it's used for memory configuration.
     */
    const bool useRuby;

    /** Block size in bytes */
    uint64_t blockSizeBytes;

    /** Block size in bits*/
    uint64_t blockSizeBits;

    /** Memory Size in bytes */
    uint64_t memorySizeBytes;

    /**************************************************************************
     * Per-core data.
     */

    /** Vector of ports for CPU to memory system */
    std::vector<CpuPort> ports;

    /**
     * Mapping of cores' threads.
     * A single core will have multiple threads assigned to it
     * if (threads > cores)
     *
     * The front of each core's thread queue is the current running thread.
     */
    std::vector<ThreadContext> threadContexts;
    std::vector<std::deque<std::reference_wrapper<ThreadContext>>>
        coreToThreadMap;

    /**************************************************************************
     * Synchronization state
     */

    StTracePthreadMetadata pthMetadata;

    /** stats: holds if thread can proceed past a barrier */
    std::vector<bool> perThreadBarrierBlocked;

    /** Holds which threads currently possess a mutex lock */
    std::vector<std::vector<Addr>> perThreadLocksHeld;

    /** Holds mutex locks in use */
    std::set<Addr> mutexLocks;

    /** Holds spin locks in use */
    std::set<Addr> spinLocks;

    /** Holds condition variables signaled by broadcasts and signals */
    std::vector<std::map<Addr, int>> condSignals;

    /** Holds which threads are waiting for a barrier */
    std::map<Addr, std::set<ThreadID>> threadBarrierMap;

  protected:

    /** Wakeup frequencies */
    const uint64_t wakeupFreqForMonitor;
    const uint64_t wakeupFreqForDebugLog;

    /** Waking up back-end simulation thread. */
    SynchroTraceMonitorEvent synchroTraceMonitorEvent;

    /** Waking up logger. */
    SynchroTraceDebugLogEvent synchroTraceDebugLogEvent;

    /** Waking up cores. */
    std::vector<SynchroTraceCoreEvent> coreEvents;

    MasterID masterID;
};
#endif // __CPU_TESTERS_SYNCHROTRACE_SYNCHROTRACE_HH
