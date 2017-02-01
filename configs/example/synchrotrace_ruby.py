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
# Instantiate SynchroTrace with Ruby
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
from common import MemConfig
from ruby import Ruby

# Get relevant paths
config_path = os.path.dirname(os.path.abspath(__file__))
config_root = os.path.dirname(config_path)

# Add gem5 options
parser = optparse.OptionParser()
Options.addCommonOptions(parser)

# SynchroTrace specific options
# Mandatory to set the path to the traces directory
#parser.add_option("--num-cpus", type="int", default=4,
#                  help="Number of cpus")
parser.add_option("--num-threads", type="int",
                  help="Number of threads")
parser.add_option("--event-dir", type="string", default="",
                  help="Path to the directory that contains the event traces")
parser.add_option("--output-dir", type="string", default="",
                  help="Path to the directory where to dump the output")
parser.add_option("--master-freq", type="int", default=1,
                  help="Frequency at which to wake up master event")
parser.add_option("--cpi-iops", type="float", default=1,
                  help="CPI for integer ops")
parser.add_option("--cpi-flops", type="float", default=1,
                  help="CPI for floating point ops")
parser.add_option("--buffers-per-data-vc", type="int", default=4,
                  help="Buffer Depth per Virtual Channel")
#parser.add_option("--vcs-per-vnet", type="int", default=4,
#                  help="Number of Virtual Channels per Network")
parser.add_option("--bandwidth-factor", type="int", default=16,
                  help="Number of Virtual Channels per Network")
parser.add_option("--l1-latency", action="store", type="int", default="3",
                  help="Latency of a L1 Hit")
parser.add_option("--pc-skip", action="store_true", default=False,
                  help="Don't enforce producer->consumer dependencies")
parser.add_option("--memsys-clock", action="store", type="string",
                  default='1.6GHz',
                  help="Clock for Memory System")

# Add the ruby specific and protocol specific options
Ruby.define_options(parser)
# execfile(os.path.join(config_root, "common", "Options.py"))
(options, args) = parser.parse_args()

if args:
     print "Error: script doesn't take any positional arguments"
     sys.exit(1)

# Create the SynchroTrace replay mechanism
tester = SynchroTrace(num_cpus = options.num_cpus,
      num_threads = options.num_threads,
      event_dir = options.event_dir,
      output_dir = options.output_dir,
      master_wakeup_freq = options.master_freq,
      cpi_iops = options.cpi_iops,
      cpi_flops = options.cpi_flops,
      ruby = options.ruby,
      block_size_bytes = options.cacheline_size,
      mem_size_bytes = toMemorySize(options.mem_size),
      pc_skip=options.pc_skip)

# Create the system
system = System(cache_line_size = options.cacheline_size,
                cpu = tester,
                mem_ranges = AddrRange(options.mem_size))

# Create voltage and clock domains
system.voltage_domain = VoltageDomain(voltage = '1V')

system.clk_domain = SrcClockDomain(clock = options.sys_clock,
                                   voltage_domain = system.voltage_domain)

memsys_clk_domain = SrcClockDomain(clock = options.memsys_clock,
                                   voltage_domain = system.voltage_domain)

# Create the ruby instance
Ruby.create_system(options, False, system)

# Since ruby runs at an independent frequency, create a seperate clock
system.ruby.clk_domain = SrcClockDomain(clock = options.ruby_clock,
                                        voltage_domain = system.voltage_domain)

# Tie the SynchroTrace ports to the ruby cpu ports
assert(options.num_cpus == len(system.ruby._cpu_ports))
for ruby_port in system.ruby._cpu_ports:
    system.cpu.cpu_port = ruby_port.slave

for mem_ctrl in system.mem_ctrls:
    mem_ctrl.clk_domain = memsys_clk_domain

# Use SynchroTrace options for garnet parameters
system.ruby.network.vcs_per_vnet = options.vcs_per_vnet
system.ruby.network.buffers_per_data_vc = options.buffers_per_data_vc
system.ruby.network.ni_flit_size = options.bandwidth_factor
for link in system.ruby.network.int_links:
    link.bandwidth_factor = options.bandwidth_factor
for link in system.ruby.network.ext_links:
    link.bandwidth_factor = options.bandwidth_factor

# Setup simulation
root = Root( full_system = False, system = system )
root.system.mem_mode = 'timing'

# instantiate configuration
m5.instantiate()

# simulate until program terminates
exit_event = m5.simulate(options.abs_max_tick)

print 'Exiting @ tick', m5.curTick(), 'because', exit_event.getCause()
