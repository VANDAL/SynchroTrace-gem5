# Copyright (c) 2015-2016 ARM Limited
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# Copyright (c) 2015, Drexel University
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
# 1. Redistributions of source code must retain the above copyright notice,
# this list of conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution.
#
# 3. Neither the name of the copyright holder nor the names of its
# contributors may be used to endorse or promote products derived from this
# software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
# IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
# THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
# LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
# NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
# SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
# Authors: Karthik Sangaiah
#          Ankit More
#          Radhika Jagtap
#
# Instantiate SynchroTrace with classic memory
#

import m5
from m5.objects import *
from m5.defines import buildEnv
from m5.util import addToPath
from m5.util.convert import toMemorySize
from math import floor, ceil
import os, optparse, sys
addToPath('../')

from common import Options
from common import Simulation
from common import MemConfig
from Caches import *

def config_caches(options, system):
    # Set L1 and L2 as default cache classes
    dcache_class, l2_cache_class = L1_DCache, L2Cache

    # Create a resonable L3 Cache class for 4 to 8 cores
    class L3Cache(L2Cache):
        size = '8192kB'
        assoc = 16
        tag_latency = 20
        data_latency = 20
        response_latency = 20
        mshrs = 20
        tgts_per_mshr = 12
        write_buffers = 8

    l3_cache_class = L3Cache

    # Number of clusters
    if options.num_cpus % options.cpus_per_cluster:
        fatal("num_cpus %s is not exactly divisible by cpus_per_cluster %s" %
                (options.num_cpus, options.cpus_per_cluster))
    num_clusters = int(options.num_cpus / options.cpus_per_cluster)

    # First create a list, instantiate caches and later connect them
    l1_caches = []
    l2_caches = []
    l2_crossbar = []

    # Instantiate L3 cache and crossbar if user sets the option
    # It is shared across all clusters so connect the downstream port to the
    # memory bus
    if options.l3cache:
        l3_cache = l3_cache_class()
        l3_crossbar = L2XBar(width = 64)
        l3_cache.cpu_side = l3_crossbar.master
        l3_cache.mem_side = system.membus.slave

    # Setup an L2 per cluster shared by the cores in the cluster
    for i in xrange(num_clusters):
        l2_crossbar.append(L2XBar(width = 64,
                           snoop_filter = SnoopFilter(max_capacity = '16MB')))
        l2_crossbar[i].clk_domain = cluster_clk_domain
        l2_caches.append(L2Cache(size = '1024kB'))
        l2_caches[i].clk_domain = cluster_clk_domain
        l2_caches[i].cpu_side = l2_crossbar[i].master
        if not options.l3cache:
            l2_caches[i].mem_side = system.membus.slave
        else:
            # If there is an L3 in the system connect to it
            l2_caches[i].mem_side = l3_crossbar.slave

    for cpu_id in xrange(options.num_cpus):
        cluster_id = int(cpu_id / options.cpus_per_cluster)
        l1_caches.append(L1_DCache(size = '32kB'))
        l1_caches[cpu_id].clk_domain = cluster_clk_domain
        l1_caches[cpu_id].mem_side = l2_crossbar[cluster_id].slave
        l1_caches[cpu_id].cpu_side = system.tester.cpu_port[cpu_id]

    # Attach all instantiated list of caches as a child to the system
    system.toL2bus = l2_crossbar
    system.l2caches = l2_caches
    system.l1caches = l1_caches
    if options.l3cache:
        system.l3cache = l3_cache
        system.toL3bus = l3_crossbar

# Get relevant paths
config_path = os.path.dirname(os.path.abspath(__file__))
config_root = os.path.dirname(config_path)

# Add gem5 options
parser = optparse.OptionParser()
Options.addCommonOptions(parser)

# Add SynchroTrace specific options
# Mandatory to set the path to the traces directory
parser.add_option("--event-dir", type = "string", default = "",
                  help = "path to the directory containing event traces")
parser.add_option("--output-dir", type = "string", default = "",
                  help = "path to the directory where to dump the output")

# To set the number of threads (must equal the number of threads traced)
# In the intuitive case, the --num-cpus equals the number of threads
parser.add_option("--num-threads", type = "int", help = "Number of threads")

# Number of cpus per cluster that share L2 cache
parser.add_option("--cpus-per-cluster", type = "int", default = 4,
                  help = "Number of CPUs per Cluster")

# Other synchrotrace behaviour options that have defaults
parser.add_option("--master-freq", type = "int", default = 1000,
                  help = "Frequency at which to wake up master event")
parser.add_option("--cpi-iops", type = "float", default = 1,
                  help = "CPI for integer ops")
parser.add_option("--cpi-flops", type = "float", default = 1,
                  help = "CPI for floating point ops")
parser.add_option("--pc-skip", action = "store_true", default = False,
                  help = "Don't enforce producer->consumer dependencies")

# Memory-system configuration options
parser.add_option("--l3cache", action = "store_true", default = False)
parser.add_option("--membus-width", action = "store", type = "int",
                  default = "64", help = "Width of System XBar")

# Clocks and maxtick
parser.add_option("--cluster-clock", action = "store", type = "string",
                  default = '1.7GHz',
                  help = "Clock for blocks running in the cluster")
parser.add_option("--memsys-clock", action = "store", type = "string",
                  default = '1.6GHz', help = "Clock for Memory System")

# execfile(os.path.join(config_root, "common", "Options.py"))
(options, args) = parser.parse_args()

if args:
    print "Error: script doesn't take any positional arguments"
    sys.exit(1)

# Create the system
system = System(cache_line_size = options.cacheline_size,
                membus = SystemXBar(width = options.membus_width),
                mem_ranges = AddrRange(options.mem_size))
MemConfig.config_mem(options, system)

# Create voltage and clock domains
system.voltage_domain = VoltageDomain(voltage = '1V')

system.clk_domain = SrcClockDomain(clock = options.sys_clock,
                                   voltage_domain = system.voltage_domain)

cluster_clk_domain = SrcClockDomain(clock = options.cluster_clock,
                                    voltage_domain = system.voltage_domain)

memsys_clk_domain = SrcClockDomain(clock = options.memsys_clock,
                                   voltage_domain = system.voltage_domain)

# Create the SynchroTrace Replay Mechanism
system.tester = SynchroTrace(num_cpus = options.num_cpus,
                            num_threads = options.num_threads,
                            event_dir = options.event_dir,
                            output_dir = options.output_dir,
                            master_wakeup_freq = options.master_freq,
                            cpi_iops = options.cpi_iops,
                            cpi_flops = options.cpi_flops,
                            ruby = options.ruby,
                            block_size_bytes = options.cacheline_size,
                            mem_size_bytes = toMemorySize(options.mem_size),
                            pc_skip = options.pc_skip)

# Set memory and cluster clock domains
system.membus.clk_domain = memsys_clk_domain

for mem_ctrl in system.mem_ctrls:
    mem_ctrl.clk_domain = memsys_clk_domain

system.tester.clk_domain = cluster_clk_domain

# Create the cache hierarchy and busses
config_caches(options, system)

# The system port is never used in the tester so merely connect it
# to avoid problems
system.system_port = system.membus.slave

# Setup simulation
root = Root(full_system = False, system = system)
root.system.mem_mode = 'timing'

# Instantiate configuration
m5.instantiate()

# Simulate until program terminates
exit_event = m5.simulate(m5.MaxTick)

print 'Exiting @ tick', m5.curTick(), 'because', exit_event.getCause()
