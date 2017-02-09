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
 *
 * Defines an event in the queue for synchronization and event-based
 * dependency tracking
 */

#ifndef __CPU_TESTERS_SYNCHROTRACE_STEVENT_HH__
#define __CPU_TESTERS_SYNCHROTRACE_STEVENT_HH__

#include <cstdlib>

#include "sim/system.hh"

/**
 * This struct contains the memory access information needed to create a
 * request.
 */
struct MemAddrInfo {

    /** Thread ID corresponding to the trace used to generate this request */
    ThreadID reqThreadID;

    /** Event ID for the sub event that this request is linked to */
    unsigned long reqEventID;

    /** Physical address */
    Addr addr;

    /** Size of the request in bytes */
    unsigned int numBytes;

    /** Constructors */
    MemAddrInfo() { }

    MemAddrInfo(ThreadID thread_id, unsigned long event_id,
                Addr addr, unsigned int num_bytes)
      : reqThreadID(thread_id),
        reqEventID(event_id),
        addr(addr),
        numBytes(num_bytes)
    { }

    MemAddrInfo(Addr addr, unsigned int num_bytes)
      : reqThreadID(-1),
        reqEventID(-1),
        addr(addr),
        numBytes(num_bytes)
    { }
};

/**
 * This struct contains information needed to calculate the timing, e.g.
 * number of integer or floating point operations, and a pointer to the
 * MemAddrInfo.
 */
struct SubEvent {

    /**
     * Memory request type for a sub event
     *
     * REQ_READ     A read type request
     * REQ_WRITE    A write type request
     */
    enum EventReqType {
        REQ_READ,
        REQ_WRITE
    };

    /** The number of integer ops that this sub event is broken down into */
    uint64_t numIOPS;

    /**
     * The number of floating point ops that this sub event is broken down
     * into.
     */
    uint64_t numFLOPS;

    /** Whether the request is read or write */
    EventReqType msgType;

    /** The time at which to send the request */
    Tick triggerTime;

    /** Set to true when request is sent */
    bool msgTriggered;

    /** Whether the event has a request associated with it */
    bool containsMsg;

    /** Pointer to the struct holding request related information */
    MemAddrInfo *thisMsg;

    /** Constructors */
    SubEvent() { }

    SubEvent(unsigned long i_ops, unsigned long f_ops,
             EventReqType req_type, bool req_sent, bool contains_req,
             MemAddrInfo *req_info_ptr)
      : numIOPS(i_ops),
        numFLOPS(f_ops),
        msgType(req_type),
        msgTriggered(req_sent),
        containsMsg(contains_req),
        thisMsg(req_info_ptr)
    { }

    SubEvent(unsigned long i_ops, unsigned long f_ops,
             bool req_sent, bool contains_req)
      : numIOPS(i_ops),
        numFLOPS(f_ops),
        msgTriggered(req_sent),
        containsMsg(contains_req)
    { }
};

/**
 * STEvent encapsulates a record in the trace as an event during trace replay
 */
class STEvent
{
  public:

    /**
     * Events have a broad classification which is captured in EventClass
     *
     * EVENT_CLASS_NONE Initialisation value
     *
     * COMP             Computation event, that is a local read, local write
     *                  or an integer or floating point operation
     *
     * COMM             An event for communication between threads
     *
     * THREAD_API       Calls to thread library such as thread creation, join,
     *                  barriers and locks
     */
    enum EventClass {
        EVENT_CLASS_NONE,
        COMP,
        COMM,
        THREAD_API
    };

    /**
     * The type within the THREAD_API class of events
     *
     * INVALID_EVENT    Initialization value
     *
     * MUTEX_LOCK       Mutex lock event simulating lock acquire
     *
     * MUTEX_UNLOCK     Mutex unlock event simulating lock release
     *
     * THREAD_CREATE    New thread creation event
     *
     * THREAD_JOIN      Thread join
     *
     * BARRIER_WAIT     Synchronisation barrier
     *
     * COND_WAIT        Pthread condition wait
     *
     * COND_SG          Pthread condition signal
     *
     * SPIN_LOCK        Pthread spin lock
     *
     * SPIN_UNLOCK      Pthread spin unlock
     *
     * SEM_INIT         Initialise a semaphore
     *
     * SEM_WAIT         Block on a semaphore count
     *
     * SEM_POST         Increment a semaphore
     *
     */
    enum EventType {
        INVALID_EVENT = 0,
        MUTEX_LOCK = 1,
        MUTEX_UNLOCK = 2,
        THREAD_CREATE = 3,
        THREAD_JOIN = 4,
        BARRIER_WAIT = 5,
        COND_WAIT = 6,
        COND_SG = 7,
        COND_BR = 8,
        SPIN_LOCK = 9,
        SPIN_UNLOCK = 10,
        SEM_INIT = 11,
        SEM_WAIT = 12,
        SEM_POST = 13,
        SEM_GETV = 14,
        SEM_DEST = 15
    };

    /** Typedef for the memory address information vector */
    typedef std::vector<MemAddrInfo *> vectorMemAddr;

    /**
     * The class of event to specify if the event captures local compute,
     * inter-thread communication or thread API calls
     */
    EventClass eventClass;

    /** To specify the type of thread API call, for e.g. mutex lock */
    EventType eventType;

    /** Unique ID of the event */
    unsigned long eventID;

    /** Thread ID corresponding to the event */
    ThreadID evThreadID;

    /**
     * Addresses which are written by some other thread and read by this thread
     */
    vectorMemAddr commPreRequisiteEvents;

    /** Addresses that this event writes which are private to its thread */
    vectorMemAddr compWriteEvents;

    /** Addresses that this event reads which are provate to its thread */
    vectorMemAddr compReadEvents;

    /** Number of integer operations */
    unsigned long compIOPS;

    /** Number of floating point operations */
    unsigned long compFLOPS;

    /** Number of reads to private memory ie local to event's thread */
    unsigned long compMemReads;

    /** Number of writes to private memory, ie local to event's thread */
    unsigned long compMemWrites;

    /** Set to true when sub events are created to handle each operation */
    bool subEventsCreated;

    /**
     * Address of the critical variable used in Pthread calls, for e.g. the
     * mutex lock address, barrier variable address, conditional variable
     * address, or address of the input variable that holds the thread
     * information when creating a new thread
     */
    Addr pthAddr;

    /**
     * Mutex lock address used in conjunction with conditional variable
     * address set in pthAddr
     */
    Addr mutexLockAddr;

    /**
     * A queue of sub events which hold operations that are bundled up in
     * STEvent
     */
    std::deque<SubEvent> *subEventList;

    /** Constructor */
    STEvent()
      : eventClass(EVENT_CLASS_NONE),
        eventType(INVALID_EVENT),
        eventID(-1),
        evThreadID(-1),
        compIOPS(-1),
        compFLOPS(-1),
        compMemReads(-1),
        compMemWrites(-1),
        subEventsCreated(false)
    { }

    /** Desctructor */
    ~STEvent() {
        if (subEventList)
            delete subEventList;

        for (auto& event : commPreRequisiteEvents)
            delete event;
        for (auto& event : compWriteEvents)
            delete event;
        for (auto& event : compReadEvents)
            delete event;
    }
};

// Output overloading
inline std::ostream &operator <<(std::ostream &os,
                                 const STEvent &event)
{
    os << "Class:" << event.eventClass;
    os << " EventID:" << event.eventID;
    os << " ThreadID:" << event.evThreadID;

    if (event.eventClass == STEvent::COMP){
        os << " compIOPS:" << event.compIOPS;
        os << " compFLOPS:" << event.compFLOPS;
        os << " compMemReads:" << event.compMemReads;
        os << " compMemWrites:" << event.compMemWrites;
    }

    os << " Sub-events creation status:" << event.subEventsCreated;

    return os;
}

inline std::ostream &operator <<(std::ostream &os,
                                 const MemAddrInfo &mem_addr_info)
{
    os << " reqThreadID:" << mem_addr_info.reqThreadID;
    os << " reqEventID:" << mem_addr_info.reqEventID;
    os << " addr:" << mem_addr_info.addr;
    os << " numBytes:" << mem_addr_info.numBytes;

    return os;
}
#endif
