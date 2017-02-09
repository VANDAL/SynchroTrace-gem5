#SYNCHROTRACE

There are two tools which together form the prototype SynchroTrace simulation flow built into gem5.
	1) Sigil2 (https://github.com/mikelui/Sigil2/) - Multi-Threaded Application Trace Capture Tool
	2) Replay - Event-Trace Replay Framework

This code base includes (2) Replay from the patch (http://reviews.gem5.org/r/3687/), built into a forked version of gem5.
	
The logical steps to using this simulation environment for design space exploration or CMP simulation is as follows:
	1)
	  a) Generate Multi-threaded Event Traces for the program binary you are testing (See Sigil2 documentation for further information):
		-Use the Sigil wrapping script with necessary options on your binary
	   OR
	  b) Use previously generated or sample traces
	2) Compile SynchroTrace (For a list of dependencies, please look at gem5's dependency list: http://gem5.org/Dependencies)
	3) Run SynchroTrace with necessary options on the generated traces

#####Installing SynchroTrace and Running 

1) Check Necessary Dependencies
   
2) Build SynchroTrace

  a) If not done so, clone the SynchroTrace repo from GitHub:

```sh
$ git clone https://github.com/dpac-vlsi/SynchroTrace-gem5.git
```
  b) Go to the base gem5 directory

  c) Run the following command (Note that the number of jobs refers to the number of cores available for compilation):
     
```sh
     scons build/X86_MESI_Two_Level/gem5.opt --jobs=6
```
At this point, the gem5 executable should be built with integrated Trace Replay in the location specified in the command above.

gem5 is usually run with a configuration script that hooks up the various architecture models packaged with the gem5 framework.
We have written a SynchroTrace configuration script to which arguments can be passed to configure the desired system.

This script can be found at:

 <BASEDIR>/configs/example/synchrotrace_ruby.py for Ruby/Garnet Simulations
 <BASEDIR>/configs/example/synchrotrace_classic_memory.py for Classic Memory System Simulations

3) Run SynchroTrace

An example command to run SynchroTrace:

```sh
./build/X86_MESI_Two_Level/gem5.opt --debug-flags=STIntervalPrint ./configs/example/synchrotrace_ruby.py --ruby --network=garnet2.0 --topology=Mesh_XY --mesh-rows=8 --event-dir=$eventDir --output-dir=. --num-cpus=8 --num-threads=8 --num-dirs=8 --num-l2caches=8 --l1d_size=64kB --l1d_assoc=2 --l1i_size=64kB --l1i_assoc=2 --l2_size=4096kB --l2_assoc=8 --cpi-iops=1 --cpi-flops=1 --bandwidth-factor=4 --master-freq=1 --cacheline_size=64"
```

where the $eventDir points to the directory of the traces and $outputDir points to the desired output directory path.

For a list of valid arguments, run the following from the main SynchroTrace folder:
```sh
$ ./build/X86_MESI_CMP_directory/gem5.opt ./configs/example/synchrotrace_ruby.py --help
```

Once the run is completed, simulated metrics can be found in m5out/stats.txt
