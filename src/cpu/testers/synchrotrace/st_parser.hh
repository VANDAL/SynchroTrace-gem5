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
 * Authors: Karthik Sangaiah & Ankit More
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
 * The STParser reads the trace files from disk and parses them line by
 * line to create an STEvent per line item. The line fields are parsed by
 * delimiter chars like comma, asterix and hash.
 */
class STParser
{
  public:
    // Request size type
    typedef uint8_t requestSize;

    // The maximum size in bytes of a request being issued to memory
    static const requestSize maxRequestSize = 8;

  private:

    // Block size type read from file
    typedef uint16_t FileBlockSize;

    // Typedef for specifying number of entries
    typedef uint8_t numEntries;

    // Size of the block to be read from the file in one go
    const FileBlockSize readBlock = 1000;

    // Threshold for filling up event queue
    const size_t minEventsSize = 100;

    // Number of comp event entries
    numEntries compEntries = 6;

    // Number of inter thread communication event entries
    numEntries commEntries = 2;

    // Number of shared data read entries
    numEntries commSharedInfoEntries = 4;

    // Number of local write event entries
    numEntries compWriteEntries = 2;

    // Number of local read event entries
    numEntries compReadEntries = 2;

    // Number of pthread event  entries
    numEntries pthreadEntries = 3;

    // Pthread tag
    const std::string pthreadTag = "pth_ty";

    // Initial book keeping

    /** Parses Sigil Pthread file for Pthread meta-data */
    void processPthreadFile();

    /** Creates pthread address to thread ID map */
    void processAddressToID(std::string line, size_t hash_pos);

    /** Creates set of barriers used in application */
    void processBarrierEvent(std::string line, size_t star_pos);

    /** Initializes sigil trace and output file pointers */
    void initSigilFilePointers();

    /** Read event from Sigil event file and determine event type */
    void readEventFile(ThreadID thread_id);

    /**
     * Parse communication event and add to event map. Communication events
     * represent RAW dependency between threads. Parse dependencies and
     * corresponding memory reads. Format modified virtual memory addresses
     * into requests for the memory system.
     */
    void processCommEvent(std::string this_event, size_t hash_pos);

    /**
     * Parse computation events and add to event map. Computation events
     * represent abstract form of many integer/floating point operations
     * between loads and stores. Default: 1 LD/ST per computation event. For
     * compression: variable number of LD/STs per computation event. Format
     * modified virtual memory addresses into requests for the memory system.
     */
    void processCompEvent(std::string this_event);

    /** Parse beginning of computation event */
    void processCompMainEvent(std::string this_event, STEvent *new_event);

    /** Parse write protion of computation event */
    void processCompWriteEvent(std::string dependency_info,
                               STEvent *new_event);

    /** Parse read portion of computation event */
    void processCompReadEvent(std::string dependency_info, STEvent *new_event);

    /** Parse synchronization event and add to event map. */
    void processPthreadEvent(std::string this_event, size_t caret_pos);

    /** Process flag events */
    void processFlagEvent(ThreadID thread_id, std::string this_event);

   // Pthread Meta-data

    /** Map converting each slave thread's pthread address to thread ID */
    std::map<Addr, ThreadID>  addressToIDMap;

    /** Holds barriers used in application */
    std::map<uint64_t, std::set<ThreadID>> barrierMap;

    /** Number of threads set from the cmd line */
    int numThreads;

    /** Start of synchronization region of interest */
    int startSyncRegion;

    /** Synchronization region of interest to instrument */
    int instSyncRegion;

    /** Tracker for which region is being simulated by each thread*/
    std::vector<int> regionCounter;

    /** Indicator for which region is instrumented */
    int instRegion;

    // File Directories
    /** Directory of Sigil Traces and Pthread metadata file */
    std::string eventDir;

    /** Directory of output files */
    std::string outputDir;

    /** Block size in bytes */
    int blockBytes;

    /** Block size in bits */
    int blockBits;

    /** Main Memory size in bytes */
    uint64_t memoryBytes;

    /** Main Memory size in terms of number of cache lines */
    uint64_t numCacheLines;

    // Flags

    /** Instruction Counter */
    std::vector<uint64_t> numInsts;

    /** Pthread meta-data file pointer */
    std::ifstream pthreadFilePointer;

    /** Sigil trace file pointers */
    std::vector<gzifstream *> inputFilePointer;

    /** Sigil trace region of interest file pointers */
    std::vector<std::vector <gzifstream *> > syncRegionFilePointers;

  public:
    /** Return map of Pthread addresses/Thread ID */
    std::map<Addr, ThreadID> getAddressToIDMap() {
        return addressToIDMap;
    };

    /** Return map of barriers */
    std::map<uint64_t, std::set<ThreadID>> getBarrierMap() {
        return barrierMap;
    };

    /** Return sigil trace file pointers */
    std::vector<gzifstream *> getInputFilePointer() {
        return inputFilePointer;
    };

    /** Return sigil synchronization region file pointers */
    std::vector<std::vector<gzifstream *> > getSynchroRegionFilePointers() {
        return syncRegionFilePointers;
    };

    /** Return start of synchronization region */
    int getStartSyncRegion() {
        return startSyncRegion;
    };


    /**
     * Central event map: list of pointers to event deques of each thread.
     * Each deque reads in 1000 events each time the deque size falls to
     * 100 events.
     */
    std::deque<STEvent *> **eventMap;

    /** Parse initial events for eventMap */
    void generateEventQueue();

    /**
     * Add more events into the eventMap when eventMap size for a particular
     * thread falls under 100 events.
     */
    void replenishEvents(ThreadID thread_id);

    /** Initialize flag event counters */
    void initFlagEventCounters();

    /** Default Parser Constructor */
    STParser(int num_threads, std::deque<STEvent *> **event_map,
             int start_sync_region, int inst_sync_region,
             std::string event_dir, std::string output_dir,
             uint64_t mem_size_bytes, int block_size_bytes,
             int block_size_bits) {
        numThreads = num_threads;
        eventMap = event_map;
        eventDir = event_dir;
        outputDir = output_dir;
        memoryBytes = mem_size_bytes;
        blockBytes = block_size_bytes;
        blockBits = block_size_bits;
        startSyncRegion = start_sync_region;
        instSyncRegion = inst_sync_region;
        processPthreadFile();
        initSigilFilePointers();
        numCacheLines = mem_size_bytes / block_size_bytes;
    }

    ~STParser() {
    }
};
#endif // __CPU_TESTERS_SYNCHROTRACE_STPARSER_HH
