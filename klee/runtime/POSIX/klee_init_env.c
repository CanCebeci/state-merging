//===-- klee_init_env.c ---------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/klee.h"
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif
#include "fd.h"
#include "multiprocess.h"
#include "misc.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/syscall.h>
#include <unistd.h>

static void __emit_error(const char *msg) {
  klee_report_error(__FILE__, __LINE__, msg, "user.err");
}

/* Helper function that converts a string to an integer, and
   terminates the program with an error message is the string is not a
   proper number */   
static long int __str_to_int(char *s, const char *error_msg) {
  long int res;
  char *endptr;

  if (*s == '\0')
    __emit_error(error_msg);

  res = strtol(s, &endptr, 0);

  if (*endptr != '\0')
    __emit_error(error_msg);

  return res;
}

static int __isprint(const char c) {
  /* Assume ASCII */
  return ((32 <= c) & (c <= 126));
}

static int __streq(const char *a, const char *b) {
  while (*a == *b) {
    if (!*a)
      return 1;
    a++;
    b++;
  }
  return 0;
}

static char *__get_sym_str(int numChars, char *name) {
  int i;
  char *s = malloc(numChars+1);
  klee_mark_global(s);
  klee_make_symbolic(s, numChars+1, name);

  for (i=0; i<numChars; i++)
    klee_prefer_cex(s, __isprint(s[i]));
  
  s[numChars] = '\0';
  return s;
}

/*
static void __add_arg(int *argc, char **argv, char *arg, int argcMax) {
  if (*argc==argcMax) {
    __emit_error("too many arguments for klee_init_env");
  } else {
    argv[*argc] = arg;
    (*argc)++;
  }
}
*/

void klee_init_env(int argc, char **argv) {
  unsigned sym_files = 0, sym_file_len = 0;
  char unsafe_flag = 0;
  int k=0;

  while (k < argc) {
    if (__streq(argv[k], "--sym-files") || __streq(argv[k], "-sym-files")) {
      const char* msg = "--sym-files expects two integer arguments <no-sym-files> <sym-file-len>";

      if (k+2 >= argc)
        __emit_error(msg);

      k++;
      sym_files = __str_to_int(argv[k++], msg);
      sym_file_len = __str_to_int(argv[k++], msg);

    }
    else if (__streq(argv[k], "--unsafe") || __streq(argv[k], "-unsafe")) {
      unsafe_flag = 1;
      k++;
    }
    else {
      k++;
    }
  }

  klee_init_processes();
  klee_init_fds(sym_files, sym_file_len, unsafe_flag);
  klee_init_mmap();
}

void klee_process_args(int* argcPtr, char*** argvPtr) {
  const char *sym_arg_msg = "--sym-arg expects an integer argument <max-len>";
  const char *sym_args_msg =
      "--sym-args expects three integer arguments <min-argvs> <max-argvs> <max-len>";

  int argc = *argcPtr;
  char** argv = *argvPtr;

  // Get maximum number of arguments
  int k;
  int new_argc_max = 0;
  for (k = 0; k < argc; ) {
    if (__streq(argv[k], "--sym-arg") || __streq(argv[k], "-sym-arg")) {

      if (k+1 >= argc)
        __emit_error(sym_arg_msg);

      ++k;
      ++k;
      ++new_argc_max;

    }
    else if (__streq(argv[k], "--sym-args") || __streq(argv[k], "-sym-args")) {
      if (k+3 >= argc)
        __emit_error(sym_args_msg);

      ++k;
      ++k;
      unsigned max_argvs = __str_to_int(argv[k++], sym_args_msg);
      ++k;

      new_argc_max += max_argvs;
    }
    else if (__streq(argv[k], "--sym-files") || __streq(argv[k], "-sym-files")) {
      ++k;
    }
    else if (__streq(argv[k], "--unsafe") || __streq(argv[k], "-unsafe")) {
      ++k;
    }
    else {
      ++k;
      ++new_argc_max;
    }
  }

  if (new_argc_max > 1024)
    __emit_error("too many arguments");


  char** new_argv = (char**) malloc((new_argc_max+1) * sizeof(char*));
  klee_mark_global(new_argv);
  int new_argc = 0;

  char sym_arg_name[5] = "arg";
  unsigned sym_arg_num = 0;
  sym_arg_name[4] = '\0';

  for (k = 0; k < argc; ) {
    if (__streq(argv[k], "--sym-arg") || __streq(argv[k], "-sym-arg")) {
      ++k;
      unsigned max_len = __str_to_int(argv[k++], sym_arg_msg);
      sym_arg_name[3] = '0' + sym_arg_num++;

      new_argv[new_argc] = __get_sym_str(max_len, sym_arg_name);
      ++new_argc;
    }
    else if (__streq(argv[k], "--sym-args") || __streq(argv[k], "-sym-args")) {
      ++k;
      unsigned min_argvs = __str_to_int(argv[k++], sym_args_msg);
      unsigned max_argvs = __str_to_int(argv[k++], sym_args_msg);
      unsigned max_len = __str_to_int(argv[k++], sym_args_msg);

      // Allocate a memory object to store the actual number of arg as
      // a concrete value that could be used in a merge black list
      volatile unsigned* n_args_i = (unsigned*) malloc(sizeof(unsigned));
      *n_args_i = 0;
      klee_merge_blacklist(n_args_i, sizeof(*n_args_i), 1);

      // Create a symbolic value that determined n_args
      unsigned n_args;
      klee_make_symbolic(&n_args, sizeof(n_args), "n_args");
      klee_assume(n_args >= min_argvs);
      klee_assume(n_args <= max_argvs);

      for (; *n_args_i < n_args; ++*n_args_i) { // This will fork
        sym_arg_name[3] = '0' + sym_arg_num++;
        new_argv[new_argc] = __get_sym_str(max_len, sym_arg_name);
        new_argc++; // This will always be concrete
      }

      //n_args = klee_range(min_argvs, max_argvs+1, "n_args");
    }
    else if (__streq(argv[k], "--sym-files") || __streq(argv[k], "-sym-files")) {
      k++;
    }
    else if (__streq(argv[k], "--unsafe") || __streq(argv[k], "-unsafe")) {
      k++;
    }
    else {
      /* simply copy arguments */
      new_argv[new_argc] = argv[k++];
      ++new_argc;
    }
  }

  new_argv[new_argc] = 0;

  if (klee_is_symbolic(new_argc))
    __emit_error("new_argc is symbolic???");
  for (k = 0; k < new_argc; ++k)
    if (klee_is_symbolic((uintptr_t) new_argv[k]))
      __emit_error("new_argv[k] is symbolic???");

  *argcPtr = new_argc;
  *argvPtr = new_argv;

  klee_event(__KLEE_EVENT_BREAKPOINT, __KLEE_BREAK_TRACE);
}

