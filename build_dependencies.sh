# use apt mirror
sed -i 's/archive/old-releases/' /etc/apt/sources.list

apt-get update && \
apt-get install -y wget gcc g++ build-essential subversion libz-dev flex bison libboost-all-dev


echo "check_certificate = off" >> ~/.wgetrc

# get gcc frontend binaries (2.6)
echo "export PATH=/llvm-gcc-4.2-2.6-x86_64-linux/bin:$PATH" >> ~/.bashrc
. ~/.bashrc

# get llvm 2.6
cd llvm-2.6
./configure --enable-optimized --enable-assertions
make -j96
echo "export PATH=/llvm-2.6/Release/bin:$PATH" >> ~/.bashrc
. ~/.bashrc
cd ..

# get gcc frontend binaries (2.9)
echo "export PATH=/llvm-gcc-4.2-2.9-x86_64-linux/bin:$PATH" >> ~/.bashrc
. ~/.bashrc

# get llvm 2.9
cd llvm-2.9
REQUIRES_RTTI=1 ./configure --enable-optimized --enable-assertions
REQUIRES_RTTI=1 make -j96
echo "export PATH=/llvm-2.9/Release+Asserts/bin:$PATH" >> ~/.bashrc
. ~/.bashrc


# these are required to build klee-uclibc
ln -s /usr/include/asm-generic /usr/include/asm
ln -s /llvm-2.6/Release /llvm-2.6/Release+Asserts
