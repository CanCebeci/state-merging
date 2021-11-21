FROM ubuntu:10.04

COPY build_dependencies.sh / 
COPY build.sh /
COPY stp /stp
COPY klee /klee
COPY klee-uclibc /klee-uclibc

# copy downloads (instead of using wget in container -- SLL issues)
COPY downloads/llvm-gcc4.2-2.9-x86_64-linux /llvm-gcc4.2-2.9-x86_64-linux
COPY downloads/llvm-2.9 /llvm-2.9

RUN chmod u+x build_dependencies.sh && ./build_dependencies.sh
RUN chmod u+x build.sh && ./build.sh

COPY test-klee /test-klee

CMD ["/bin/bash"]
