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

int main(void) {
  long sum = 0;
  for (int k = 0; k < 100000; k++)
    sum += accumulate(k, 3);
  for (int k = 0; k < 100000; k++)
    sum += file_local(k, 3);
  printf("sum=%ld\n", sum);
  return 0;
}
