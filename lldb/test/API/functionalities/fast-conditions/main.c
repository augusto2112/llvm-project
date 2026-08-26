#include <stdio.h>

int accumulate(int seed, int rounds) {
  int total = seed;
  // A float in scope at the return, so that capturing it is capturing a value
  // whose bits a conversion would not have preserved.
  double scaled = seed * -1.5e-30;
  for (int i = 0; i < rounds; i++) {
    total += i;
  }
  if (scaled > 1.0)
    total = -1;
  return total;
}

// File-local, and declared across two lines with its return type on the first,
// which is what the debug info's declaration line points past. Both of those are
// ordinary in C and neither says anything about what the body does, so a
// condition compiles into this one exactly as it does into the function above.
static int
file_local(int seed, int rounds)
{
  int local_total = seed * 2;
  for (int i = 0; i < rounds; i++) {
    local_total += i;
  }
  return local_total;
}

// Names itself, which is what makes a second compile of it interesting: the copy
// carries the original's name, so a copy already in the program is a second
// definition of the name this body calls.
int countdown(int n) {
  if (n <= 0) {
    return 0;
  }
  int rest = countdown(n - 1);
  return rest + n;
}

int main(int argc, char **argv) {
  long sum = 0;
  for (int k = 0; k < 100000; k++)
    sum += accumulate(k, 3);
  for (int k = 0; k < 100000; k++)
    sum += file_local(k, 3);
  for (int k = 0; k < 200; k++)
    sum += countdown(k % 8);
  printf("sum=%ld\n", sum);
  // Somewhere a test can read once the debugger has let go of this process,
  // which is how a run that finished is told from one a trap killed.
  if (argc > 1) {
    FILE *done = fopen(argv[1], "w");
    if (done) {
      fprintf(done, "sum=%ld\n", sum);
      fclose(done);
    }
  }
  return 0;
}
