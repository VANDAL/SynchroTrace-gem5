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

#include "st_parser.hh"

using namespace std;

void
STParser::processPthreadFile()
{

    std::string fname_str = csprintf("%s/sigil.pthread.out",
                                     eventDir.c_str());
    char *fname = strdup(fname_str.c_str());
    pthreadFilePointer.open(fname);
    if (!pthreadFilePointer.is_open()) {
        panic("Failed to open pthread file");
    }
    free(fname);
    while (pthreadFilePointer.good() && !pthreadFilePointer.eof()) {
        string pthread_file_line;
        if (getline(pthreadFilePointer, pthread_file_line)) {
            size_t hash_pos = pthread_file_line.rfind('#');
            size_t star_pos = pthread_file_line.rfind('*');

            if (hash_pos != string::npos)
                processAddressToID(pthread_file_line, hash_pos);
            else if (star_pos != string::npos)
                processBarrierEvent(pthread_file_line, star_pos);
        } else
            break;
    }
    pthreadFilePointer.close();
}

void
STParser::processAddressToID(string line, size_t hash_pos){
    string thread_addr_line = line.substr(hash_pos + 1);
    size_t comma_pos = thread_addr_line.find(',');
    string thread_address_string = thread_addr_line.substr(0, comma_pos);
    string thread_id_string = thread_addr_line.substr(comma_pos + 1);
    Addr thread_address = strtoul(thread_address_string.c_str(), NULL, 0);
    ThreadID thread_id = strtol(thread_id_string.c_str(), NULL, 0);
    assert(thread_id >= 0);

    // Keep Thread IDs in a map
    addressToIDMap[thread_address] = thread_id - 1;
}

void
STParser::processBarrierEvent(string line, size_t star_pos){
    string barrier_line = line.substr(star_pos + 1);
    size_t comma_pos = barrier_line.find(',');
    size_t old_pos = 0;
    string barrier_address_string = barrier_line.substr(0, comma_pos);
    string tidlist = barrier_line.substr(comma_pos + 1);
    ThreadID thread_id;
    comma_pos = tidlist.find(',');

    set<ThreadID> thread_ids;

    while (comma_pos != string::npos) {
        thread_id = strtol(tidlist.substr(old_pos, comma_pos).c_str(),
                                    NULL, 0) - 1;
        assert(thread_id >= 0);
        thread_ids.insert(thread_id);
        old_pos = comma_pos + 1;
        comma_pos = tidlist.find(',', comma_pos + 1);
    }
    thread_id =
        strtol(tidlist.substr(old_pos, string::npos).c_str(),NULL,0) - 1;
    thread_ids.insert(thread_id);


    Addr barrier_address = strtoul(barrier_address_string.c_str(), NULL, 0);
    barrierMap[barrier_address] = thread_ids;
}

void
STParser::initSigilFilePointers()
{
    inputFilePointer.resize(numThreads);
    outputFilePointer.resize(numThreads);

    for (int i = 0; i < numThreads; i++) {
        std::string fname_str = csprintf("%s/sigil.events.out-%d.gz",
                                         eventDir.c_str(), i + 1);
        char *fname = strdup(fname_str.c_str());

        inputFilePointer[i] = new gzifstream(fname);
        if (inputFilePointer[i]->fail()) {
            panic("Failed to open file: %s\n", fname);
        }

        fname_str = csprintf("%s/eventTimeOutput-%d.csv.gz",
                             outputDir.c_str(), i + 1);
        fname = strdup(fname_str.c_str());

        outputFilePointer[i] = new gzofstream(fname);
        if (outputFilePointer[i]->fail()) {
            panic("ERROR!: Not able to create event file %s. Aborting!!\n",
                  fname);
        }
        free(fname);
    }
}

void
STParser::generateEventQueue(){
    for (int i = 0; i < numThreads; i++) {
        readEventFile(i);
    }
}

void
STParser::replenishEvents(ThreadID thread_id) {
    if (eventMap[thread_id]->size() < minEventsSize)
        readEventFile(thread_id);
}

void
STParser::readEventFile(ThreadID thread_id)
{
    string this_event;

    for (FileBlockSize count = 0; count < readBlock; count++) {
        if (!getline(*(inputFilePointer[thread_id]), this_event)) {
            break;
        } else {
            size_t hash_pos = this_event.find('#');
            size_t caret_pos = this_event.find('^');

            if (hash_pos != string::npos) {
                processCommEvent(this_event, hash_pos);
            } else if (caret_pos != string::npos){
                processPthreadEvent(this_event, caret_pos);
            } else {
                processCompEvent(this_event);
            }
        }
    }
}

void
STParser::processCommEvent(string this_event, size_t hash_pos)
{
    string event_info;

    event_info = this_event.substr(0, hash_pos - 1);
    STEvent *new_event = new STEvent();
    new_event->subEventList = NULL;
    new_event->eventClass = STEvent::COMM;

    size_t cur_pos = -1;
    for (numEntries i = 0; i < commEntries; i++) {
        size_t next_pos = event_info.find(',', cur_pos + 1);
        string this_entry = event_info.substr(cur_pos + 1,
            next_pos - (cur_pos + 1));
        cur_pos = next_pos;

        switch(i) {
            case 0:
                new_event->eventID = strtoul(this_entry.c_str(), NULL, 0);
                break;
            case 1:
                new_event->evThreadID = atoi(this_entry.c_str()) - 1;
                break;
            default:
                panic("Comm event entry crossed max\n");
        }
    }

    string dependency_info = this_event.substr(hash_pos + 2);
    size_t cur_hash_pos = -2;

    // Parse dependency information of Communication Event
    do {
        size_t next_hash_pos = dependency_info.find('#', cur_hash_pos + 2);
        string dep_info = dependency_info.substr(cur_hash_pos + 2,
            (next_hash_pos - 1) - (cur_hash_pos + 2));
        cur_hash_pos = next_hash_pos;

        ThreadID thread_id = 0;
        unsigned long event_id = 0;
        unsigned long mem_start = 0;
        unsigned long mem_end = 0;
        size_t cur_space_pos = -1;
        for (numEntries i = 0; i < commSharedInfoEntries; i++) {
            size_t next_space_pos = dep_info.find(' ',
                                   cur_space_pos + 1);
            string this_entry = dep_info.substr(cur_space_pos + 1,
                               next_space_pos - (cur_space_pos + 1));
            cur_space_pos = next_space_pos;

            switch(i) {
                case 0:
                    thread_id = atoi(this_entry.c_str()) - 1;
                    assert(thread_id >= 0);
                    break;
                case 1:
                    event_id = strtoul(this_entry.c_str(), NULL, 0);
                    break;
                case 2:
                    mem_start = strtoul(this_entry.c_str(), NULL, 0);
                    break;
                case 3:
                    mem_end = strtoul(this_entry.c_str(), NULL, 0);
                    break;
                default:
                    panic("Shared comm event entry crossed max\n");
            }
        }

        // Convert program virtual address to modified virtual for the address
        // space and +1 to offset the count of the start_range
        requestSize num_bytes = (uint64_t)(mem_end - mem_start + 1);
        uint64_t line_start = (uint64_t)((mem_start/blockBytes)
                             % numCacheLines);
        uint64_t start_offset = (uint64_t)(mem_start % blockBytes);
        uint64_t cur_add = (line_start << blockBits) | start_offset;

        // Split up each of the memory requests for subEvents
        requestSize  prev_req_size = 0;
        do {
            requestSize this_req_size;

            if (num_bytes >= maxRequestSize)
                this_req_size = maxRequestSize;
            else
                this_req_size = num_bytes;
            cur_add = (cur_add + prev_req_size) % memoryBytes;

            int residual = this_req_size - (int)(blockBytes -
                           (int)(cur_add % blockBytes));

            if (residual > 0) {
                // split it between 2 lines
                MemAddrInfo *this_line = new MemAddrInfo(
                    thread_id, event_id, cur_add, this_req_size - residual);
                new_event->commPreRequisiteEvents.push_back(this_line);
                prev_req_size = (uint64_t)(this_req_size - residual);
            } else {
                MemAddrInfo *this_line = new MemAddrInfo(
                    thread_id, event_id, cur_add, this_req_size);
                new_event->commPreRequisiteEvents.push_back(this_line);
                prev_req_size = this_req_size;
            }
            num_bytes -= prev_req_size;
        } while (num_bytes > 0);
    } while (cur_hash_pos != string::npos);

    if (new_event->evThreadID > numThreads || new_event->evThreadID < 0) {
        panic("ThreadID bad! Check number of threads in configuration\n");
    }
    eventMap[new_event->evThreadID]->push_back(new_event);
}

void
STParser::processCompEvent(string this_event)
{
    size_t dollar_pos = this_event.find('$');
    size_t star_pos = this_event.find('*');

    STEvent *new_event = new STEvent();
    new_event->subEventList = NULL;
    new_event->eventClass = STEvent::COMP;

    string event_info;
    string write_event_info;
    string read_event_info;

    string remainder_str = this_event;

    // Reads
    if (star_pos != string::npos) {
        read_event_info = remainder_str.substr(star_pos + 2);
        remainder_str = remainder_str.substr(0, star_pos - 1);
    }
    // Writes
    if (dollar_pos != string::npos) {
        write_event_info = remainder_str.substr(dollar_pos + 2);
        remainder_str = remainder_str.substr(0, dollar_pos - 1);
    }

    event_info = remainder_str;
    processCompMainEvent(event_info, new_event);

    if (dollar_pos != string::npos)
        processCompWriteEvent(write_event_info, new_event);

    if (star_pos != string::npos)
        processCompReadEvent(read_event_info, new_event);

    if (new_event->evThreadID > numThreads || new_event->evThreadID < 0) {
        panic("ThreadID bad! Check number of threads in configuration\n");
    }
    eventMap[new_event->evThreadID]->push_back(new_event);
}

void
STParser::processCompMainEvent(string this_event, STEvent *new_event)
{
    int element_count = count(this_event.begin(), this_event.end(), ',');

    if (element_count < (compEntries - 1)) {
        panic("Incorrect element count in computation event."
              " Number of elements: %d\n", element_count);
    }
    size_t cur_pos = -1;
    for (numEntries i = 0; i < compEntries; i++) {
        size_t next_pos = this_event.find(',', cur_pos + 1);
        string this_entry = this_event.substr(cur_pos + 1,
                                            next_pos - (cur_pos + 1));
        cur_pos = next_pos;

        switch(i) {
            case 0:
                new_event->eventID = strtoul(this_entry.c_str(), NULL, 0);
                break;
            case 1:
                new_event->evThreadID = atoi(this_entry.c_str()) - 1;
                break;
            case 2:
                new_event->compIOPS = strtoul(this_entry.c_str(), NULL, 0);
                break;
            case 3:
                new_event->compFLOPS = strtoul(this_entry.c_str(), NULL, 0);
                break;
            case 4:
                new_event->compMemReads = strtoul(this_entry.c_str(),
                                                    NULL, 0);
                break;
            case 5:
                new_event->compMemWrites = strtoul(this_entry.c_str(),
                                                    NULL, 0);
                break;
            default:
                panic("Comp entry crossed expected max entry count\n");
                break;
        }
    }
}

void
STParser::processCompWriteEvent(string dependency_info, STEvent *new_event)
{
    size_t curr_pos = -2;
    do {
        size_t next_pos = dependency_info.find('$', curr_pos + 2);
        string dep_info = dependency_info.substr(curr_pos + 2,
                                    (next_pos - 1) - (curr_pos + 2));
        curr_pos = next_pos;

        unsigned long mem_start = 0;
        unsigned long mem_end = 0;
        size_t cur_space_pos = -1;
        for (numEntries i = 0; i < compWriteEntries; i++) {
            size_t next_space_pos = dep_info.find(' ',
                cur_space_pos + 1);
            string this_entry = dep_info.substr(cur_space_pos + 1,
                next_space_pos - cur_space_pos + 1);
            cur_space_pos = next_space_pos;

            switch(i) {
                case 0:
                    mem_start = strtoul(this_entry.c_str(), NULL, 0);
                    break;
                case 1:
                    mem_end = strtoul(this_entry.c_str(), NULL, 0);
                    break;
                default:
                    panic("Comp write entry crossed expected max\n");
                    break;
            }
        }

        vector<MemAddrInfo*> this_comp_write_event;

        // Convert program virtual address to modified virtual for the address
        // space and +1 to offset the count of the start_range
        requestSize num_bytes = (uint64_t)(mem_end - mem_start + 1);

        uint64_t line_start = (uint64_t)((mem_start/blockBytes)
                              % numCacheLines);
        uint64_t start_offset = (uint64_t)(mem_start
                               % blockBytes);
        uint64_t cur_add = (line_start << blockBits) | start_offset;

        // Split up each of the memory requests for subEvents
        requestSize prev_req_size = 0;
        do {
            requestSize this_req_size;

            this_req_size = (num_bytes >= maxRequestSize ? maxRequestSize :
                                num_bytes);
            cur_add = (cur_add + prev_req_size) % memoryBytes;

            int residual = this_req_size - (int)(blockBytes -
                           (int)(cur_add % blockBytes));

            if (residual > 0) {
                // Split it between 2 lines
                MemAddrInfo *this_line = new MemAddrInfo(
                    0, 0, cur_add, this_req_size - residual);
                new_event->compWriteEvents.push_back(this_line);
                prev_req_size = (uint64_t)(this_req_size - residual);
            } else {
                MemAddrInfo *this_line = new MemAddrInfo(
                    0, 0, cur_add, this_req_size);
                new_event->compWriteEvents.push_back(this_line);
                prev_req_size = this_req_size;
            }
            num_bytes -= prev_req_size;
        } while (num_bytes > 0);
    } while (curr_pos != string::npos);
}

void
STParser::processCompReadEvent(string dependency_info, STEvent *new_event)
{
    size_t cur_star_pos = -2;
    do {
        size_t nextStar_Pos = dependency_info.find('*', cur_star_pos + 2);
        string dep_info = dependency_info.substr(cur_star_pos + 2,
                                    (nextStar_Pos - 1) - (cur_star_pos + 2));
        cur_star_pos = nextStar_Pos;

        unsigned long mem_start = 0;
        unsigned long mem_end = 0;
        size_t cur_space_pos = -1;
        for (numEntries i = 0; i < compReadEntries; i++) {
            size_t next_space_pos = dep_info.find(' ', cur_space_pos + 1);
            string this_entry = dep_info.substr(cur_space_pos + 1,
                                                next_space_pos -
                                                cur_space_pos + 1);
            cur_space_pos = next_space_pos;

            switch(i) {
                case 0:
                    mem_start = strtoul(this_entry.c_str(), NULL, 0);
                    break;
                case 1:
                    mem_end = strtoul(this_entry.c_str(), NULL, 0);
                    break;
                default:
                    panic("Comp read entry crossed expected max\n");
                    break;
            }
        }

        vector<MemAddrInfo*> this_comp_readEvent;
        // Convert program virtual address to modified virtual for the
        // address space
        requestSize num_bytes = (uint64_t)(mem_end -
                            mem_start + 1);
                            // +1 to offset the count of the start_range
        uint64_t line_start = (uint64_t)((mem_start/blockBytes)
                             % numCacheLines);
        uint64_t start_offset = (uint64_t)(mem_start
                               % blockBytes);
        uint64_t cur_add = (line_start << blockBits) | start_offset;

        // Split up each of the memory requests for subEvents
        requestSize prev_req_size = 0;
        do {
            requestSize this_req_size;

            if (num_bytes >= maxRequestSize)
                this_req_size = maxRequestSize;
            else
                this_req_size = num_bytes;
            cur_add = (cur_add + prev_req_size) % memoryBytes;

            int residual = this_req_size - (int)(blockBytes -
                           (int)(cur_add % blockBytes));

            if (residual > 0) {
                // Split it between 2 lines
                MemAddrInfo *this_line = new MemAddrInfo(
                    0, 0, cur_add, this_req_size - residual);
                new_event->compReadEvents.push_back(this_line);
                prev_req_size = (uint64_t)(this_req_size - residual);
            } else {
                MemAddrInfo *this_line = new MemAddrInfo(
                    0, 0, cur_add, this_req_size);
                new_event->compReadEvents.push_back(this_line);
                prev_req_size = this_req_size;
            }
            num_bytes -= prev_req_size;
        } while (num_bytes > 0);
    } while (cur_star_pos != string::npos);
}

void
STParser::processPthreadEvent(string this_event, size_t caret_pos)
{
    string event_info = this_event.substr(0, caret_pos);
    STEvent *new_event = new STEvent();
    new_event->subEventList = NULL;
    new_event->eventClass = STEvent::THREAD_API;

    string tmp_str;
    size_t cur_pos = -1, next_pos = -1;

    for (numEntries i = 0; i < pthreadEntries; i++) {
      next_pos = event_info.find(',', cur_pos + 1);
      tmp_str = event_info.substr(cur_pos + 1, next_pos - (cur_pos + 1));
      cur_pos = next_pos;

      switch (i) {
        case 0:
            new_event->eventID = strtoul(tmp_str.c_str(), NULL, 0);
            break;
        case 1:
            new_event->evThreadID = strtoul(tmp_str.c_str(), NULL, 0) - 1;
            break;
        case 2:
            assert(next_pos == string::npos);
            next_pos = tmp_str.find(':');
            assert(tmp_str.substr(0, next_pos) == pthreadTag);
            new_event->eventType = (STEvent::EventType)
              strtol(tmp_str.substr(next_pos + 1, string::npos).c_str(),
                     NULL, 0);
            break;
        default:
            panic("phtread event entry crossed expected max\n");
      }
    }

    // Capture mutex lock address for Conditional Wait/Signal in addition to
    // pthread address, otherwise capture pthread address.

    size_t amp_pos = this_event.rfind('&');
    if (amp_pos != string::npos) { // Conditional wait found
        new_event->mutexLockAddr =
            strtoul(this_event.substr(amp_pos + 1, string::npos).c_str(),
                                      NULL, 0);
        new_event->pthAddr =
            strtoul(this_event.substr(caret_pos + 1, amp_pos - 1).c_str(),
                                      NULL, 0);
    } else {
        new_event->pthAddr =
            strtoul(this_event.substr(caret_pos + 1, string::npos).c_str(),
                NULL, 0);
    }
    if (new_event->evThreadID > numThreads || new_event->evThreadID < 0)
        panic("ThreadID bad! Check number of threads in configuration\n");
    eventMap[new_event->evThreadID]->push_back(new_event);
}
