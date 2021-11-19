Cloud9 Symbolic Execution Engine - State Merging Branch.

This archieve contains the state merging branch of the Cloud9 symbolic
execution engine that corresponds to a prototype implementation of the state
merging technique as presented in the "Efficient State Merging in Symbolic
Execution" paper at PLDI 2012.

This is an early development prototype intended for developers who want to
understand and improve the state merging techniques. The code is not ready
for any production use.

To build the code, download and compile llvm 2.6 and corresponding llvm-gcc,
then execute the following commands (replacing paths in <> by corresponding
paths on your system):

$ cd klee-uclibc
$ ./configure --with-llvm=</path/to/llvm-2.6-build>
$ make -j8
$ cd ..

$ cd stp
$ ./scripts/configure --with-prefix=$(pwd) --with-cloud9=$(pwd)/../klee
$ make
$ cp src/c_interface/c_interface.h include/stp
$ cd ..

$ mkdir klee-build
$ cd klee-build
$ ../klee/configure --with-llvmsrc=</path/to/llvm-2.6>
        --with-llvmobj=</path/to/llvm-2.6-build> \
        --with-uclibc=../klee-uclibc --enable-posix-runtime --with-stp=../stp
$ make -j8
$ cd ..

