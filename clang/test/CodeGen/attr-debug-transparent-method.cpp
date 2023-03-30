// RUN: %clang_cc1 -debug-info-kind=limited -emit-llvm -o - %s | FileCheck %s

void bar(void) {}

struct A {
[[clang::debug_transparent()]]
void foo(void) {
  bar();
}
};

int main() {
  A().foo();
}

// CHECK: DISubprogram(name: "foo"{{.*}} DISPFlagIsDebugTransparent
