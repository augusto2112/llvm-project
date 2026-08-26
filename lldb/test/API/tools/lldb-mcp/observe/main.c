#include <dlfcn.h>
#include <pthread.h>
#include <setjmp.h>
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

/* Called with a null pointer at its first calls and a real one at the rest, so
   that a capture reaching through it is unavailable at some hits and available
   at others. That is the case a capture must not be given up on: the value is
   readable, just not yet.

   How many of the calls are null is load-bearing. A capture that has failed at
   every hit so far is given UnresolvableCaptureAttempts of them before it is
   turned off, which is exactly what the attempts are for, so a fixture whose
   null run is that long is testing the boundary rather than the behaviour --
   and lands on the wrong side of it. Fewer nulls than attempts. */
int sometimes_null(struct Outer *o) { return o ? o->c : 0; }

/* The worked example in trace_program's own description names these two
   functions and reads exactly these members, and TestObserve runs that example
   as it is written. A rename here is a rename there, which is the point: an
   example a test does not execute drifts away from the code it describes, and
   the description is the one string a caller copies before it has read anything
   else. */
struct Token {
  int kind;
  const char *text;
};

int classify_token(struct Token *tok, int depth) { return tok->kind + depth; }

/* Returns a different value at every call, so that a return observation in
   on_change mode has a change to emit at each one. */
int parse_expr(int depth) { return depth * depth + 1; }

/* Recursion gives a backtrace runs of identical frames to collapse. */
int recurse(int n) { return n <= 0 ? 0 : recurse(n - 1) + 1; }

/* Called from a capture expression rather than by the program, which is how an
   object that knows how to describe itself is read. Void, so the capture has no
   value and what it printed is the whole of its answer. Both streams, because
   attributing output to the expression that produced it is only useful if the
   two can be told apart -- and stdout is the buffered one, which is what makes
   the flush load-bearing rather than tidy. */
void describe(int n) {
  printf("described %d\n", n);
  fprintf(stderr, "warned %d\n", n);
}

/* The same thing on one stream, which is what a real `dump()` does. Writing to
   two streams at once leaves nothing ordering one against the other, so such a
   hit cannot have its printed text as its value; this one can, and that is the
   case worth having a subject for. The trailing newline is deliberate: it is
   near-universal in dump output and it is what the value must not carry. */
void describe_one_stream(int n) { fprintf(stderr, "node %d\n", n); }

/* Observed while a capture calls `describe`, so the attribution has more than
   one hit to keep straight. */
int step_printing(int n) { return n + 1; }

/* Never returns, so that a wall-clock ceiling is the only way a run ends. */
void spin(void) {
  for (volatile long i = 0;; ++i) {
  }
}

/* Hit steadily for several seconds, so that a stall ceiling has a run that is
   plainly making progress while almost nothing is being emitted. The sleep is
   what makes the run outlast that ceiling however fast the machine is. */
int tick(int n) { return n + 1; }

void churn(void) {
  int i;
  for (i = 0; i < 3000; ++i) {
    tick(i);
    usleep(1000);
  }
}

static jmp_buf escape;

/* Leaves without returning at one of its calls, so that a return observation
   has a frame that never reaches its return address. */
int abandons(int n) {
  if (n == 1)
    longjmp(escape, 1);
  return n + 7;
}

int unwind_loop(void) {
  volatile int sum = 0;
  int i;
  for (i = 0; i < 3; ++i)
    if (setjmp(escape) == 0)
      sum += abandons(i);
  return sum;
}

/* Called from more than one thread, so that an observation has an interleaving
   to report rather than one thread's sequence. */
int shared_step(int n) { return n * 2; }

/* Spread over several lines, because the work a tracepoint does in the program
   stands where a statement stands: a one-line function's only line is its
   declaration, which holds none, and a braceless loop body has room for one
   statement rather than several. Everything a compiled-in condition is tested
   against is here.

   The parameter is what the conditions read, so that a condition which is never
   true and one which is true exactly once can both be written against the same
   function. */
int accumulate(int seed, int rounds) {
  int total = seed;
  int i;
  for (i = 0; i < rounds; ++i) {
    total += i;
  }
  return total;
}

/* The only caller that passes two rounds, so that restricting a condition on
   accumulate to hits reached from here has something to exclude. */
int accumulate_via(int seed) { return accumulate(seed, 2); }

static void *worker(void *arg) {
  int base = *(int *)arg;
  int i;
  for (i = 0; i < 5; ++i)
    shared_step(base + i);
  return NULL;
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

  /* A run whose tracepoint is hit while a capture calls into the program. The
     program's own line comes first and is left buffered, so a test can see both
     that it is reported and that a capture's window did not swallow it. */
  if (strcmp(mode, "printing") == 0) {
    describe(-1);
    for (i = 0; i < 3; ++i)
      total += step_printing(i);
    return 0;
  }

  /* The modes below are the whole of the run they belong to: each one exists to
     give one observation something specific to see, and the loops that follow
     would only add hits nobody asked about. */
  if (strcmp(mode, "churn") == 0) {
    churn();
    return 0;
  }

  if (strcmp(mode, "unwind") == 0) {
    printf("unwound=%d\n", unwind_loop());
    fflush(stdout);
    return 0;
  }

  if (strcmp(mode, "threads") == 0) {
    pthread_t first, second;
    int first_base = 0;
    int second_base = 100;
    if (pthread_create(&first, NULL, worker, &first_base) != 0)
      return 1;
    if (pthread_create(&second, NULL, worker, &second_base) != 0)
      return 1;
    pthread_join(first, NULL);
    pthread_join(second, NULL);
    return 0;
  }

  /* Loaded while the program is running, so that a name observed inside it
     cannot resolve before the launch. The path arrives as an argument because
     the library sits beside the binary rather than anywhere a loader searches.
   */
  if (strcmp(mode, "plugin") == 0) {
    void *handle = argc > 2 ? dlopen(argv[2], RTLD_NOW) : NULL;
    int (*step)(int) = NULL;
    if (handle)
      step = (int (*)(int))dlsym(handle, "plugin_step");
    printf("plugin=%d\n", step != NULL);
    fflush(stdout);
    if (!step)
      return 1;
    for (i = 0; i < 4; ++i)
      total += step(i);
    return 0;
  }

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
  for (i = 0; i < 6; ++i)
    total += sometimes_null(i < 2 ? NULL : &o);
  total += recurse(4);

  /* Twenty calls of three rounds and five of two, so that a condition on the
     rounds separates the two callers. */
  for (i = 0; i < 20; ++i)
    total += accumulate(i, 3);
  for (i = 0; i < 5; ++i)
    total += accumulate_via(i);

  /* The description's example observes these two, so the run it describes has to
     reach them on the path a plan naming no mode takes. */
  {
    struct Token tokens[3] = {{1, "let"}, {2, "x"}, {4, "="}};
    for (i = 0; i < 3; ++i)
      total += classify_token(&tokens[i], i);
    for (i = 0; i < 4; ++i)
      total += parse_expr(i);
  }

  printf("total=%d\n", total);

  /* The crash is behind an argument so that one binary serves both the crash
     triage case and the tracing cases, which need the program to exit. */
  if (strcmp(mode, "crash") == 0)
    crash_now();

  return 0;
}
