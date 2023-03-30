// RUN: %clang_cc1 -debug-info-kind=limited -emit-llvm -o - %s | FileCheck %s


@interface ObjCClass
- (void)foo __attribute__((debug_transparent));
@end

@implementation ObjCClass
- (void)foo {}
@end


// CHECK: DISubprogram(name: "-[ObjCClass foo]"{{.*}} DISPFlagIsDebugTransparent
