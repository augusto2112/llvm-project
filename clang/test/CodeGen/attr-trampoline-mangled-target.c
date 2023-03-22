// RUN: %clang_cc1 -debug-info-kind=limited -emit-llvm -o - %s | FileCheck %s

void bar(void) {}

__attribute__((trampoline_mangled_target("bar")))
void foo(void) {
  bar();
}

// CHECK: DISubprogram(name: "foo"{{.*}} targetFuncName: "bar"
