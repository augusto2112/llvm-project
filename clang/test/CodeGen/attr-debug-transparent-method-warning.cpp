// RUN: %clang_cc1 -debug-info-kind=limited -gcodeview -emit-llvm -o /dev/null %s > FileCheck


struct S {
[[clang::debug_transparent]] 
void foo(void) {}
};

// CHECK: warning: 'debug_transparent' attribute is ignored since it is only supported by DWARF
