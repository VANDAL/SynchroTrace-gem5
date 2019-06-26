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
 * Copyright (c) 2019, Drexel University
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
 * Authors: Karthik Sangaiah,
 *          Ankit More,
 *          Mike Lui
 *
 * Parses Sigil event trace files and Sigil Pthread file
 */

#ifndef __CPU_TESTERS_SYNCHROTRACE_STPARSER_HH
#define __CPU_TESTERS_SYNCHROTRACE_STPARSER_HH

#include <zfstream.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "st_event.hh"

/**
 * Parses individual events from a line in an event trace.
 */
class StTraceParser
{
    /** Parse tokens */
    static constexpr char COMP_EVENT_TOKEN = '@';
    static constexpr char COMM_EVENT_TOKEN = '#';
    static constexpr char THREADAPI_EVENT_TOKEN = '^';
    static constexpr char MARKER_EVENT_TOKEN = '!';
    static constexpr char WRITE_ADDR_DELIM = '$';
    static constexpr char READ_ADDR_DELIM = '*';

    /** System config for parsing event data */
    const uint32_t m_bytesPerCacheBlock;
    const uint64_t m_bytesInMainMemoryTotal;
    const uint64_t m_maskForCacheOffset;
    static constexpr uint8_t maxBytesPerRequest = 8;

    /** Cached state, optimization */
    std::mt19937_64 rng;
    std::vector<MemoryRequest> tempWriteAddrs;
    std::vector<MemoryRequest> tempReadAddrs;

  public:
    StTraceParser(
        uint32_t bytesPerCacheBlock, uint64_t bytesInMainMemoryTotal);

    /**
     * Parses a textual synchrotrace event and adds any events generated to the
     * supplied buffer. The number of events generated is unknown until the
     * line is fully parsed.
     *
     * The buffer can theoretically be any type with push_back/emplace_back
     * semantics.
     *
     * The parser takes a reference to the event id so it knows to increment it
     * if it is a real event (comp, comm, thread), and not a meta-event like a
     * marker.
     */
    void parseTo(std::vector<StEvent>& buffer,
                 std::string& line,
                 ThreadID threadId,
                 StEventID& eventId);

  private:
    void parseCompEventTo(std::vector<StEvent>& buffer,
                          std::string& line,
                          ThreadID threadId,
                          StEventID eventId);
    void parseCommEventTo(std::vector<StEvent>& buffer,
                          std::string& line,
                          ThreadID threadId,
                          StEventID eventId);
    void parseThreadEventTo(std::vector<StEvent>& buffer,
                            std::string& line,
                            ThreadID threadId,
                            StEventID eventId);
    void parseMarkerEventTo(std::vector<StEvent>& buffer,
                            std::string& line,
                            ThreadID threadId,
                            StEventID eventId);

    template<typename F>
    void foreach_MemAddrChunk(uintptr_t startAddr, uintptr_t endAddr, F f);
};


/**
 * Outputs a stream of events which can be queried one-at-a-time.
 * Parses a gzipped trace file to events for replay.
 * The decompressed trace file is expected to be a raw text file.
 */
class StEventStream
{
    /** Trace attributes */
    ThreadID threadId;
    StEventID lastEventIdParsed;
    uint64_t lastLineParsed;

    /** File streams */
    std::string filename;
    std::string line;
    gzifstream traceFile;

    /** The actual parser and underlying buffer */
    StTraceParser parser;
    std::vector<StEvent> buffer;
    std::vector<StEvent>::const_iterator buffer_current;
    std::vector<StEvent>::const_iterator buffer_end;
    size_t eventsPerFill;

  public:
    StEventStream(ThreadID threadId,
                  const std::string& eventDir,
                  uint32_t bytesPerCacheBlock,
                  uint64_t bytesInMainMemoryTotal,
                  size_t initialBufferSize=1024);

    /**
     * Returns the next event in the stream.
     * If the stream is finished, will continue returning `end` events.
     */
    const StEvent& peek();

    /**
     * Pops the next event off stream.
     */
    void pop();

    /**
     * Get the current last line number parsed
     */
    uint64_t getLineNo() const { return lastLineParsed; }
    uint64_t getEventNo() const { return lastEventIdParsed; }

  private:
    /**
     * Refills the buffer with more events from the trace file.
     */
    void refill();
};


/**
 * Parses the pthread metadata file, which contains information on thread
 * creation and barriers. Used for proper synchronization during replay.
 */
class StTracePthreadMetadata
{
    /** Parse tokens */
    static constexpr char ADDR_TO_TID_TOKEN = '#';
    static constexpr char BARRIER_TOKEN = '*';

    /** Map converting each slave thread's pthread address to thread ID */
    std::unordered_map<Addr, ThreadID> m_addressToIdMap;

    /** Holds barriers used in application */
    std::unordered_map<uint64_t, std::set<ThreadID>> m_barrierMap;

  public:
    StTracePthreadMetadata(const std::string& eventDir);

    const decltype(m_addressToIdMap)& addressToIdMap() const
    {
        return m_addressToIdMap;
    }

    const decltype(m_barrierMap)& barrierMap() const
    {
        return m_barrierMap;
    }

  private:
    /** Parses Sigil Pthread file for Pthread meta-data */
    void parsePthreadFile(std::ifstream& pthFile);

    /** Creates pthread address to thread ID map */
    void parseAddressToID(std::string& line);

    /** Creates set of barriers used in application */
    void parseBarrierEvent(std::string& line);
};

#endif // __CPU_TESTERS_SYNCHROTRACE_STPARSER_HH
