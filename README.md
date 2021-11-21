# state-merging
An attempt at replicating the results from the PLDI'12 submission

To test: 

```
cd test-klee
llvm-gcc -I /klee/include -emit-llvm -c -g -O0 get_sign.c
/klee-build/Release+Asserts/bin/klee get_sign.o
```
