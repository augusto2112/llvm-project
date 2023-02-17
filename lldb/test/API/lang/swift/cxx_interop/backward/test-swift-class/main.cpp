
#include <assert.h>
#include <cstdio>
#include <cstddef>
#include <../../../swift/lib/PrintAsClang/_SwiftCxxInteroperability.h>
#include "swift-class.h"

extern "C" size_t swift_retainCount(void * _Nonnull obj);

size_t getRetainCount(const a::ClassWithIntField & swiftClass) {
  void *p = swift::_impl::_impl_RefCountedClass::getOpaquePointer(swiftClass);
  return swift_retainCount(p);
}

int main() {
  using namespace a;
  auto x = returnClassWithIntField();
  auto y = returnStructWithIntField();
  return 0; // Set breakpoint here.
}
