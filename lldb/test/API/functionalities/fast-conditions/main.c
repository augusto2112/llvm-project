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

int main(void) {
  long sum = 0;
  for (int k = 0; k < 100000; k++)
    sum += accumulate(k, 3);
  printf("sum=%ld\n", sum);
  return 0;
}
