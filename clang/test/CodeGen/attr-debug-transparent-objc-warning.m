// RUN: %clang_cc1 %s -debug-info-kind=limited -gcodeview -emit-llvm -verify -Wno-objc-root-class -o /dev/null

@interface ObjCClass
- (void)foo __attribute__((debug_transparent)); // expected-warning {{'debug_transparent' attribute is ignored since it is only supported by DWARF}}
@end

@implementation ObjCClass
- (void)foo {}
@end

