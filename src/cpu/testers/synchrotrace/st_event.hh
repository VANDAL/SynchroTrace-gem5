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
 *
 * Defines an event in the queue for synchronization and event-based
 * dependency tracking
 */

#ifndef __CPU_TESTERS_SYNCHROTRACE_STEVENT_HH__
#define __CPU_TESTERS_SYNCHROTRACE_STEVENT_HH__

#include <cstdlib>
#include <random>

#include "sim/system.hh"

using StEventID = std::size_t;
constexpr StEventID InvalidEventID = std::numeric_limits<StEventID>::max();

/** Read/Write */
enum class ReqType: uint8_t {
    REQ_READ,
    REQ_WRITE
};

/**
 * A memory request for the SynchroTrace...TODO documentation
 */
struct MemoryRequest {
    /** Physical address */
    Addr addr;

    /** Size of the request starting at the address */
    uint32_t bytesRequested;

    ReqType type;
};


struct MemoryRequest_ThreadCommunication {
    /** Physical address */
    Addr addr;

    /** Size of the request starting at the address */
    uint32_t bytesRequested;

    /** Event ID for the sub event that this request is linked to */
    StEventID sourceEventId;

    /** Thread ID corresponding to the trace used to generate this request */
    ThreadID sourceThreadId;
};


struct ComputeOps {
    uint32_t iops;
    uint32_t flops;
};

struct ThreadApi {
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
    enum class EventType {
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
        SEM_DEST = 15,
        NUM_TYPES,
    };

    EventType eventType;
};

struct End {
};

struct InsnMarker {
    /** number of instructions this marker represents */
    uint64_t insns;
};


struct StEvent {
    /**
     * Tag for event.
     * Events have a broad classification.
     */
    enum class Tag
    {
        UNDEFINED,     // Initial value.

        /** Replay events */
        COMPUTE,       // Computation event, a combination of iops and flops.

        MEMORY,        // A memory request, either read or write.

        MEMORY_COMM,   // An inter-thread communication memory request,
                       // implicitly read

        THREAD_API,    // Calls to thread library such as thread creation,
                       // join, barriers, and locks

        /** Meta events */
        INSN_MARKER,   // Marks a specific number of machine instructions
                       // passed in the trace.

        EVENT_MARKER,  // New event in trace.

        END_OF_EVENTS  // The last event in the event stream.
    };

    /**
     * Constants for tagged dispatch constructors.
     */
    using ComputeTagType = std::integral_constant<Tag, Tag::COMPUTE>;
    static constexpr auto ComputeTag = ComputeTagType{};

    using MemoryTagType = std::integral_constant<Tag, Tag::MEMORY>;
    static constexpr auto MemoryTag = MemoryTagType{};

    using MemoryCommTagType = std::integral_constant<Tag, Tag::MEMORY_COMM>;
    static constexpr auto MemoryCommTag = MemoryCommTagType{};

    using ThreadApiTagType = std::integral_constant<Tag, Tag::THREAD_API>;
    static constexpr auto ThreadApiTag = ThreadApiTagType{};

    using InsnMarkerTagType = std::integral_constant<Tag, Tag::INSN_MARKER>;
    static constexpr auto InsnMarkerTag = InsnMarkerTagType{};

    using EndTagType = std::integral_constant<Tag, Tag::END_OF_EVENTS>;
    static constexpr auto EndTag = EndTagType{};


    /**
     * The actual event.
     * An event stream is a sequence of any of the union'd types.
     *
     * impl: these are expected to be set only during construction,
     * hence they are const.
     */
    const Tag tag = Tag::UNDEFINED;
    union
    {
        ComputeOps                        computeOps;
        MemoryRequest                     memoryReq;
        MemoryRequest_ThreadCommunication memoryReqComm;
        ThreadApi                         threadApi;
        InsnMarker                        insnMarker;
        End                               end;
    };

    /**
     * The original event id in the trace file.
     * Multiple replay events can be generated
     * from the same event in the trace.
     */
    const StEventID eventId = InvalidEventID;
    const ThreadID threadId = InvalidThreadID;

    /**
     * Use tagged dispatched constructors to create the variant.
     * Allow direct construction instead of static builder methods that may
     * cause additional copies/moves
     */
    StEvent(StEventID eventId,
            ThreadID threadId,
            ComputeTagType,
            uint32_t iops,
            uint32_t flops) noexcept
      : tag{Tag::COMPUTE},
        computeOps{iops, flops},
        eventId{eventId},
        threadId{threadId}
    {}

    StEvent(StEventID eventId, ThreadID threadId,
            MemoryTagType,
            const MemoryRequest& memReq) noexcept
      : tag{Tag::MEMORY},
        memoryReq{memReq},
        eventId{eventId},
        threadId{threadId}
    {}

    StEvent(StEventID eventId,
            ThreadID threadId,
            MemoryCommTagType,
            Addr addr,
            uint32_t bytes,
            StEventID sourceEventId,
            ThreadID sourceThreadId) noexcept
      : tag{Tag::MEMORY},
        memoryReqComm{addr, bytes, sourceEventId, sourceThreadId},
        eventId{eventId},
        threadId{threadId}
    {}

    StEvent(StEventID eventId,
            ThreadID threadId,
            ThreadApiTagType,
            ThreadApi::EventType type,
            Addr pthAddr,
            Addr mutexLockAddr) noexcept
      : tag{Tag::THREAD_API},
        threadApi{pthAddr, mutexLockAddr, type},
        eventId{eventId},
        threadId{threadId}
    {}

    StEvent(StEventID eventId,
            ThreadID threadId,
            InsnMarkerTagType,
            uint64_t insns) noexcept
      : tag{Tag::INSN_MARKER},
        insnMarker{insns},
        eventId{eventId},
        threadId{threadId}
    {}

    StEvent(StEventID eventId,
            ThreadID threadId,
            EndTagType) noexcept
      : tag{Tag::END_OF_EVENTS},
        end{},
        eventId{eventId},
        threadId{threadId}
    {}
};

#endif
