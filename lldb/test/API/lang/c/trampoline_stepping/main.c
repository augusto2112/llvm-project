void foo(void) {}

__attribute__((__trampoline__("foo")))
void bar(void) {
  foo();
}

__attribute__((__trampoline__("bar")))
void baz(void) {
  bar();
}

__attribute__((__trampoline__("bar")))
void doesnt_call_trampoline(void) {
  int a = 2;
  int b = 3;
  int c = a + b;
}


void direct_trampoline_call(void) {
  bar(); // Break here for direct 
  bar();
}

void chained_trampoline_call(void) {
  baz(); // Break here for chained
  baz();
}

void unused_target(void) {
  doesnt_call_trampoline(); // Break here for unused
}

int main(void) {
  direct_trampoline_call();
  chained_trampoline_call();
  unused_target();
  return 0;
}

