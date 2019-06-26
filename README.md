# SYNCHROTRACE

## What is it?

A fast computer-architecture simulation framework for design space exploration.  
[See our publication for details](https://sites.tufts.edu/tcal/files/2018/09/2018_SynchroTrace_TACO.pdf)

The SynchroTrace Simulation Framework has two parts:

* prism-based *Trace Capture* of multi-threaded applications
  * https://github.com/VANDAL/prism
* gem5-based *Trace Replay*
  * this repo (based on the [official SynchroTrace patch](http://reviews.gem5.org/r/3687))


## Build REPLAY

:exclamation: Check dependencies :exclamation:
* please see gem5's dependency list: http://gem5.org/Dependencies

   ```sh
   git clone https://github.com/VANDAL/SynchroTrace-gem5
   cd SynchroTrace-gem5
   scons build/ARM/gem5.opt -j
   ```

## Acquire TRACES

* Generate [multi-threaded event traces][Prism] for your program binary
* **REQUIRES v2** trace format
  * can convert v1 format to v2 format via: https://github.com/VANDAL/stgen_1to2

Example:

   ```sh
   git clone https://github.com/VANDAL/prism
   cd prism
   mkdir build && cd build
   cmake .. -DCMAKE_BUILD_TYPE=Release
   make -j
   bin/prism --backend=stgen -ltextv2 --executable=./a.out
   ```


## Run REPLAY

* Uses the gem5 simulation system
  * http://learning.gem5.org/book/index.html
  * SynchroTrace is modeled as a CPU tester (a mock-CPU)
* Use the example SynchroTrace configuration scripts
  * `configs/example/synchrotrace_ruby.py` for *Ruby/Garnet Simulations*
  * `configs/example/synchrotrace_classic_memory.py` for *Classic Memory System Simulations*
  * arguments to the example script will configure the system
  * the python scripts are required by gem5 to hook up the various architecture components

Example wrapper:

   ```sh
   # set some convenience vars
   EVENT_PATH=/path/to/traces
   OUTPUT_PATH=/path/to/output
   
   build/ARM/gem5.opt                                                                          \
     --debug-flags=STIntervalPrint                                                               \
     configs/example/synchrotrace_ruby.py                                                      \
       --ruby                                                                                    \
       --network=garnet2.0                                                                       \
       --topology=Mesh_XY                                                                        \
       --mesh-rows=8                                                                             \
       --event-dir=$EVENT_PATH                                                                   \
       --output-dir=$OUTPUT_PATH                                                                 \
       --num-cpus=8 --num-threads=8 --num-dirs=8 --num-l2caches=8                                \
       --l1d_size=64kB --l1d_assoc=2 --l1i_size=64kB --l1i_assoc=2 --l2_size=4096kB --l2_assoc=8 \
       --cpi-iops=1 --cpi-flops=1                                                                \
       --bandwidth-factor=4 --monitor-freq=100 --cacheline_size=64
   ```

   List of valid arguments:

   ```sh
   build/ARM/gem5.opt ./configs/example/synchrotrace_ruby.py --help
   ```

## Simulation Results:

   Simulated metrics are placed in **`m5out/stats.txt`**.

## NOTE TO PREVIOUS USERS

Synchronization region-of-interest options have not been reimplemented as of the latest release.
Checkout tag [st_v1.0.0](https://github.com/VANDAL/SynchroTrace-gem5/releases/tag/st_v1.0.0) if
required. File an issue if it's required again for the latest release.

[Prism]: https://github.com/VANDAL/prism
