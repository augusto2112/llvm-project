#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Takes four distinct values over a hundred calls, so that an on_change
   observation has long runs of identical hits to collapse. */
int record_bucket(int bucket) { return bucket + 1; }

/* Takes the same value at every call but one, so that an aggregate over a
   hundred hits has exactly one rare value to name. Its body differs from
   record_bucket's so the two cannot end up sharing an address. */
int record_value(int value) { return value * 2; }

/* Only ever named misspelled, so resolving that name has a near miss to
   suggest. */
int compute_value(int seed) { return seed - 1; }

/* Reached from two different callers, so that restricting an observation to one
   of them has something to exclude. */
int leaf(int n) { return n + 1; } /* the leaf body */

int gate(int n) { return leaf(n) * 2; }

int ungated(int n) { return leaf(n) * 3; }

/* Returns a different value at one call, so a return observation has a rare
   returned value to name. */
int returns_value(int n) { return n == 3 ? -1 : n * 10; }

/* Nested aggregates begin at the same address as their outermost struct, which
   is what an address-only identity mistakes for a cycle. */
struct Inner {
  int a;
  int b;
};

struct Outer {
  struct Inner in;
  int c;
  const char *name;
};

int nested(struct Outer *o) { return o->in.a + o->in.b + o->c; }

/* Recursion gives a backtrace runs of identical frames to collapse. */
int recurse(int n) { return n <= 0 ? 0 : recurse(n - 1) + 1; }

/* Never returns, so that a wall-clock ceiling is the only way a run ends. */
void spin(void) {
  for (volatile long i = 0;; ++i) {
  }
}

void crash_now(void) {
  volatile int *null_pointer = (volatile int *)0;
  *null_pointer = 1; /* the null dereference */
}

int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "trace";
  struct Outer o = {{1, 2}, 3, "outer"};
  char line[64] = "nostdin";
  int total = 0;
  int i;

  /* Echoing the process inputs is what lets a test see that each one arrived,
     since a plan cannot read them back any other way. Reading standard input is
     part of that echo and only happens in the mode that asks for it: with no
     input redirected, a read would wait for a terminal that is not there. */
  if (strcmp(mode, "probe") == 0) {
    printf("mode=%s\n", mode);
    printf("env=%s\n", getenv("OBSERVE_ENV") ? getenv("OBSERVE_ENV") : "unset");
    printf("cwd_marker=%d\n", access("cwd_marker", F_OK) == 0);
    if (fgets(line, sizeof line, stdin))
      line[strcspn(line, "\n")] = '\0';
    printf("stdin=%s\n", line);
    fflush(stdout);
  }

  /* A run that never ends, so that only the ceiling stops it. The spin comes
     before the loops so that a no-progress ceiling has no events to see. */
  if (strcmp(mode, "spin") == 0)
    spin();

  for (i = 0; i < 100; ++i)
    total += record_bucket(i / 25);

  for (i = 0; i < 100; ++i)
    total += record_value(i == 42 ? 99 : 7);

  total += compute_value(total);

  for (i = 0; i < 3; ++i)
    total += gate(i);
  for (i = 0; i < 2; ++i)
    total += ungated(i);
  for (i = 0; i < 5; ++i)
    total += returns_value(i);
  total += nested(&o);
  total += recurse(4);

  printf("total=%d\n", total);

  /* The crash is behind an argument so that one binary serves both the crash
     triage case and the tracing cases, which need the program to exit. */
  if (strcmp(mode, "crash") == 0)
    crash_now();

  return 0;
}
