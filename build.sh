cd klee-uclibc
./configure --with-llvm=/llvm-2.9
make -j96
cd ..

cd stp
./clean-install.sh
mkdir include
mkdir include/stp
cp src/c_interface/c_interface.h include/stp
cd ..


#ln -s /llvm-2.6/include/llvm/System/Process.h /llvm-2.6/include/llvm/Support/Process.h
apt-get install -y libncurses5-dev libncursesw5-dev dejagnu flex bison protobuf-compiler libprotobuf-dev libboost-thread-dev libboost-system-dev binutils-gold binutils-source

mkdir klee-build
cd klee-build
REQUIRES_RTTI=1 ../klee/configure --with-llvmsrc=/llvm-2.9 \
      --with-llvmobj=/llvm-2.9 \
      --with-uclibc=../klee-uclibc --enable-posix-runtime --with-stp=../stp
REQUIRES_RTTI=1 make -n
REQUIRES_RTTI=1 make
cd ..
