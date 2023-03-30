// RUN: %clang_cc1 -debug-info-kind=limited -emit-llvm -o - %s | FileCheck %s

void bar(void) {}

__attribute__((debug_transparent))
void foo(void) {
  bar();
}

// CHECK: DISubprogram(name: "foo"{{.*}} DISPFlagIsDebugTransparent
