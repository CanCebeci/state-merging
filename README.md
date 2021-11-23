# state-merging
An attempt at replicating the results from the PLDI'12 submission

Changes to Klee worth noting:
* QCE is now implemented as a FunctionPass (instead of a CallGraphSCCPass). The latter did not play well with Dominator Tree Construction (see [this](https://marc.info/?l=llvm-dev&m=123440460016423&w=2))
* Klee and LLVM are now compliled with -O0. This can affect the benchamrk results.
* Rendez-vous points (?!) are disabled

To test get_sign.c: 

```
cd test-klee
llvm-gcc -I /klee/include -emit-llvm -c -g -O0 get_sign.c
/klee-build/Release+Asserts/bin/klee get_sign.o
```
