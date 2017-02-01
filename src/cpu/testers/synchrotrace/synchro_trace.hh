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

#ifndef __CPU_TESTERS_SYNCHROTRACE_SYNCHROTRACE_HH
#define __CPU_TESTERS_SYNCHROTRACE_SYNCHROTRACE_HH

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "base/intmath.hh"
#include "base/misc.hh"
#include "cpu/testers/synchrotrace/st_event.hh"
#include "cpu/testers/synchrotrace/st_parser.hh"
#include "debug/ROI.hh"
#include "debug/STDebug.hh"
#include "debug/STEventPrint.hh"
#include "debug/STIntervalPrint.hh"
#include "debug/STMutexLogger.hh"
#include "mem/mem_object.hh"
#include "mem/packet.hh"
#include "mem/ruby/common/Address.hh"
#include "mem/ruby/common/DataBlock.hh"
#include "mem/ruby/common/SubBlock.hh"
#include "mem/ruby/system/RubyPort.hh"
#include "mem/ruby/system/RubySystem.hh"
#include "params/SynchroTrace.hh"
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
 *  3) Based on the compute ops for that sub event, a trigger time is
 *  scheduled to wakeup the core sometime in the future. The timing for the
 *  compute ops are configurable via the command line options 'cpi_iops' and
 *  'cpi_flops'.
 *  4) When the sub event's trigger time has been reached, a read or write
 *  request is sent out to the memory system and the core essentially blocks.
 *  5) Eventually the memory request returns as a "hitCallback" and the next
 *  sub event or event is scheduled.
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

class SynchroTrace : public MemObject
{
  public:

    class CpuPort : public MasterPort
    {
      private:

        SynchroTrace *tester;

      public:

        CpuPort(const std::string &_name, SynchroTrace *_tester,
                PortID _id)
            : MasterPort(_name, _tester, _id), tester(_tester)
        {}

      protected:

        /**
         * Receive memory request response from the memory system to the
         * SynchroTrace CPU port
         */
        virtual bool recvTimingResp(PacketPtr pkt);

        // TODO - Implement retry mechanism for classic memory model
        virtual void recvRetry() {
            panic("%s does not expect a retry\n", name());
        }
        void recvTimingSnoopReq(PacketPtr pkt)
        {}
        virtual void recvReqRetry() {
            panic("%s does not expect a retry\n", name());
        }
    };

    typedef SynchroTraceParams Params;
    SynchroTrace(const Params *p);
    ~SynchroTrace();

    /** Used to get a reference to the master port. */
    virtual BaseMasterPort &getMasterPort(const std::string &if_name,
                                          PortID idx = InvalidPortID);

    /** Used to get a reference to the CPU port. */
    MasterPort *getCpuPort(int idx);

    virtual void init();

    /** Wake up the back-end simulation thread. */
    void wakeup();

    /** Wake up the core. */
    void wakeup(int proc_id);

  protected:
    class SynchroTraceStartEvent : public Event
    {
      private:

        SynchroTrace *tester;

      public:

        SynchroTraceStartEvent(SynchroTrace *_tester)
            : Event(CPU_Tick_Pri), tester(_tester)
        {}
        /** Waking up back-end simulation thread. */
        void process() { tester->wakeup(); }
        virtual const char *description() const {
            return "SynchroTrace tick";
        }
    };

    class SynchroTraceCoreEvent : public Event
    {
      private:

        SynchroTrace *tester;
        int procID;

      public:

        SynchroTraceCoreEvent(SynchroTrace *_tester, int _procID)
            : Event(CPU_Tick_Pri), tester(_tester), procID(_procID)
        {}
        /** Waking up cores. */
        void process() { tester->wakeup(procID); }
        virtual const char *description() const {
            return "Core event tick";
        }
    };

    /** Waking up back-end simulation thread. */
    SynchroTraceStartEvent synchroTraceStartEvent;

    /** Waking up cores. */
    std::vector<SynchroTraceCoreEvent *> coreEvents;

    MasterID masterID;

  private:

    enum LocalMemAccessType {
        READ,
        WRITE,
    };


    // Python params passed to SynchroTrace object

    /** Vector of ports for CPU to memory system */
    std::vector<MasterPort*> ports;

    /** Number of CPUs set from the cmd line */
    int numCpus;

    /** Number of threads set from the cmd line */
    int numThreads;

    /** Directory of Sigil Traces and Pthread metadata file */
    std::string eventDir;

    /** Directory of output files */
    std::string outDir;

    /** Back-end simulator thread frequency */
    int wakeupFreq;

    /** Option to use Ruby */
    bool useRuby;

    /** Block Size */
    int m_block_size_bytes;

    /** Parser Object */
    STParser *parser;

    const uint16_t printCount;

    /**
     * Counter used for 'STIntervalPrint' to print event progression after
     * N core wakeups
     */
    int printThreadEventCounters;

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

    /** Abstract cpi estimation for integer ops */
    float CPI_IOPS;

    /** Abstract cpi estimation for floating point ops */
    float CPI_FLOPS;

    /** Flag to skip Producer -> Consumer dependencies */
    bool pcSkip;

    /** Block size in bytes */
    int blockSizeBytes;

    /** Block size in bits*/
    int blockSizeBits;

    /** Memory Size in bytes */
    uint64_t memorySizeBytes;

    /**
     * Central event map: list of pointers to event deques of each thread.
     * Each deque reads in 1000 events each time the deque size falls to
     * 100 events.
     */
    std::deque<STEvent *> **eventMap;

    /** Vector of cores' threads */
    std::vector<std::vector<ThreadID>> threadMap;

    /** Vector of threads' statuses, i.e. whether they are active */
    std::vector<bool> threadStartedMap;

    /** Holds which threads currently possess a mutex lock */
    std::vector<bool> threadMutexMap;

    /** Holds if thread can proceed past a barrier */
    std::vector<bool> threadContMap;

    /** Holds mutex locks in use */
    std::set<Addr> mutexLocks;

    /** Holds spin locks in use */
    std::set<Addr> spinLocks;

    /** Map converting each slave thread's pthread address to thread ID */
    std::map<Addr, ThreadID>  addresstoIDMap;

    /** Holds barriers used in application */
    std::map<Addr, std::set<ThreadID>> barrierMap;

    /** Holds which threads are waiting for a barrier */
    std::map<Addr, std::set<ThreadID>> threadWaitMap;

    /** Sigil trace file pointer */
    std::vector<gzifstream *> inputFilePointer;

    /** Output file pointers */
    std::vector<gzofstream *> outputFilePointer;

    /**
     * Number of threads (if less than number of cores). Otherwise,
     * number of cores.
     */
    int numContexts;

    /** Private copy constructor */
    SynchroTrace(const SynchroTrace &obj);

    /** Private assignment operator */
    SynchroTrace& operator=(const SynchroTrace &obj);

    /** Initialize debug flags and mutex lock/barrier maps */
    void initStats();

    /**
     * Map threads to cores. Currently implemented as a round robin mapping
     * of threads to cores.
     */
    void initialThreadMapping();

    /**
     * Check of master thread has completed its events and issue an exit
     * call back.
     */
    void checkCompletion();

    /**
     * Debug functions to view progress, prints event id for every thread
     *  at 50k wakeups.
     */
    void printThreadEvents();

    /**
     * Print Event id for specific thread before/after event is
     * loaded/completed.
     *
     * @param thread_id thread ID of event
     * @param is_end set to true if this event has completed
     */
    void printEvent(ThreadID thread_id, bool is_end);

    /**
     * Break up events into sub events comprised of a division of the
     * compute ops and one memory request. The sub events are placed in
     * a sub event list for each event. Creating sub events only occurs
     * prior to an events actual processing as to reduce the total amount
     * of memory usage in simulation (compared to creating a sub event for
     * every event in the eventMap).
     */
    void createSubEvents(int proc_id, bool event_id_passed = false,
                         ThreadID event_thread_id = 0);

    /** Handle synchronization progress for each of the threads. */
    void progressPthreadEvent(STEvent *this_event, int proc_id);

    /**
     * Handles progression of each thread depending on event type and
     * status.
     */
    void progressEvents(int proc_id);

    /**
     * Swapping of threads within a core is allowed if:
     * The head event in the currently allocated thread has finished an event
     * and one of the other threads is ready.
     *
     * The swapThreads() function is called every time an event finishes.
     */
    void swapThreads(int proc_id);

    /**
     * Function is called on core wakeups to prevent cores from stalling
     * when scheduling multiple threads per core.
     */
    void swapStalledThreads(int proc_id);

    /**
     * Handles moving thread from top of thread queue on a core to the
     * back.
     */
    void moveThreadToHead(int proc_id, ThreadID thread_id);

    /** Handles selecting a read or a write when generating sub events. */
     LocalMemAccessType memTypeToInsert(unsigned long loc_reads,
                                        unsigned long loc_writes,
                                        unsigned long max_loc_reads,
                                        unsigned long max_loc_writes,
                                        LocalMemAccessType type);

    /** Send a blocking message request to memory system. */
    void triggerMsg(int proc_id, ThreadID thread_id,
                    SubEvent *this_sub_event);

    /** Memory request returned! Queue up next event. */
    void hitCallback(NodeID proc, PacketPtr pkt);

    /**
     * For a communication event, check to see if the producer has reached
     * the dependent event. This function also handles the case of a system
     * call. System calls are viewed as producer->consumer interactions with
     * the 'producer' system call having a ThreadID of 30000. For obvious
     * reasons, there are no 'dependencies' to enforce in the case of a system
     * call.
     */
    bool checkCommDependency(MemAddrInfo *comm_event, ThreadID thread_id);

    /**
     * There can be mutiple Producer->Consumer dependencies within an event.
     * This function calls checkCommDependency(...) for all
     * producer->consumer dependencies.
     */
    bool checkAllCommDependencies(STEvent *this_event);


    /** Check if all necessary threads are in barrier. */
    bool checkBarriers(STEvent *this_event);

};
#endif // __CPU_TESTERS_SYNCHROTRACE_SYNCHROTRACE_HH
