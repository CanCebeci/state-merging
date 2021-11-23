# use apt mirror
sed -i 's/archive/old-releases/' /etc/apt/sources.list

apt-get update && \
apt-get install -y wget gcc g++ build-essential subversion libz-dev flex bison libboost-all-dev

# get gcc frontend binaries (2.9)
echo "export PATH=/llvm-gcc4.2-2.9-x86_64-linux/bin:$PATH" >> ~/.bashrc
. ~/.bashrc

# get llvm 2.9
cd llvm-2.9
REQUIRES_RTTI=1 ./configure --enable-assertions
echo "========== LLVM make maken ==========0"
REQUIRES_RTTI=1 make -n
REQUIRES_RTTI=1 make -j96
echo "export PATH=/llvm-2.9/Release+Asserts/bin:$PATH" >> ~/.bashrc
. ~/.bashrc

# this is required to build klee-uclibc
ln -s /usr/include/asm-generic /usr/include/asm
