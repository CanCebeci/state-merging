cd klee-uclibc
./configure --with-llvm=/llvm-2.6
make -j8
cd ..

cd stp
./scripts/configure --with-prefix=$(pwd) --with-cloud9=$(pwd)/../klee
make
cp src/c_interface/c_interface.h include/stp
cd ..


ln -s /llvm-2.6/include/llvm/System/Process.h /llvm-2.6/include/llvm/Support/Process.h
mkdir klee-build
cd klee-build
../klee/configure --with-llvmsrc=</path/to/llvm-2.6>
      --with-llvmobj=</path/to/llvm-2.6-build> \
      --with-uclibc=../klee-uclibc --enable-posix-runtime --with-stp=../stp
make -j8
cd ..
