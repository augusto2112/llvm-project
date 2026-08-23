#include <string.h>

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

void crash_now(void) {
  volatile int *null_pointer = (volatile int *)0;
  *null_pointer = 1; /* the null dereference */
}

int main(int argc, char **argv) {
  int total = 0;
  int i;

  for (i = 0; i < 100; ++i)
    total += record_bucket(i / 25);

  for (i = 0; i < 100; ++i)
    total += record_value(i == 42 ? 99 : 7);

  total += compute_value(total);

  /* The crash is behind an argument so that one binary serves both the crash
     triage case and the tracing cases, which need the program to exit. */
  if (argc > 1 && strcmp(argv[1], "crash") == 0)
    crash_now();

  return total < 0 ? 1 : 0;
}
