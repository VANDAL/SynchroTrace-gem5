#SYNCHROTRACE

##What is it?

A fast computer-architecture simulation framework for design space exploration.  
[See here for details](http://vlsi.ece.drexel.edu/index.php?title=SynchroTrace)

Two tools form the SynchroTrace Simulation Framework:

1. [Sigil2 - Multi-Threaded Application Trace Capture Tool][Sigil2]
2. **Replay** - Event-Trace Replay Framework
  * This repo, from the [official SynchroTrace patch](http://reviews.gem5.org/r/3687)

##Build and Run

1. :exclamation: Check dependencies :exclamation:
  * please see gem5's dependency list: http://gem5.org/Dependencies
2. Acquire SynchroTraces
  * Generate [multi-threaded event traces][Sigil2] for your program binary ***OR***
  * Use included sample traces
3. Build SynchroTrace

   ```sh
   $ git clone https://github.com/VANDAL/SynchroTrace-gem5
   $ cd SynchroTrace-gem5
   $ scons build/X86_MESI_Two_Level/gem5.opt -j
   [hit enter to install git commit hook]
   ```
4. Run SynchroTrace
  * Use the provided SynchroTrace configuration script
    * arguments are passed in to configure the desired system
    * this gem5 script is required to hook up the various architecture models packaged with the gem5 framework
  * `<BASEDIR>/configs/example/synchrotrace_ruby.py` for *Ruby/Garnet Simulations*
  * `<BASEDIR>/configs/example/synchrotrace_classic_memory.py` for *Classic Memory System Simulations*

   Example script:

   ```sh
   # set some convenience vars
   EVENT_PATH=/path/to/traces
   OUTPUT_PATH=/path/to/output
   
   ./build/X86_MESI_Two_Level/gem5.opt                                                           \
     --debug-flags=STIntervalPrint                                                               \
     ./configs/example/synchrotrace_ruby.py                                                      \
       --ruby                                                                                    \
       --network=garnet2.0                                                                       \
       --topology=Mesh_XY                                                                        \
       --mesh-rows=8                                                                             \
       --event-dir=$EVENT_PATH                                                                   \
       --output-dir=$OUTPUT_PATH                                                                 \
       --num-cpus=8 --num-threads=8 --num-dirs=8 --num-l2caches=8                                \
       --l1d_size=64kB --l1d_assoc=2 --l1i_size=64kB --l1i_assoc=2 --l2_size=4096kB --l2_assoc=8 \
       --cpi-iops=1 --cpi-flops=1                                                                \
       --bandwidth-factor=4 --master-freq=1 --cacheline_size=64
   ```
   List of valid arguments:
   ```sh
   $ ./build/X86_MESI_Two_Level/gem5.opt ./configs/example/synchrotrace_ruby.py --help
   ```

   Simulated metrics are placed in **`m5out/stats.txt`**.

[Sigil2]: https://github.com/VANDAL/Sigil2
