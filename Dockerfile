FROM ubuntu:10.04

COPY build_dependencies.sh / 
COPY stp /stp
COPY klee /klee

# copy downloads (instead of using wget in container -- SLL issues)
COPY downloads/llvm-gcc-4.2-2.6-x86_64-linux /llvm-gcc-4.2-2.6-x86_64-linux
COPY downloads/llvm-2.6 /llvm-2.6


RUN chmod u+x build_dependencies.sh && ./build_dependencies.sh
COPY klee-uclibc /klee-uclibc

CMD ["/bin/bash"]
