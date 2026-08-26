#include <stdio.h>

int accumulate(int seed, int rounds) {
  int total = seed;
  for (int i = 0; i < rounds; i++) {
    total += i;
  }
  return total;
}

int main(void) {
  long sum = 0;
  for (int k = 0; k < 100000; k++)
    sum += accumulate(k, 3);
  printf("sum=%ld\n", sum);
  return 0;
}
