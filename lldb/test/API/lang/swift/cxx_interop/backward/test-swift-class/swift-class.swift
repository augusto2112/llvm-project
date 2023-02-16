// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Class -clang-header-expose-decls=all-public -emit-clang-header-path %t/class.h
// RUN: %FileCheck %s < %t/class.h

// RUN: %check-interop-cxx-header-in-clang(%t/class.h)

// RUN: %target-swift-frontend %s -typecheck -module-name Class -enable-library-evolution -clang-header-expose-decls=all-public -emit-clang-header-path %t/class-evo.h
// RUN: %FileCheck %s < %t/class-evo.h

// RUN: %check-interop-cxx-header-in-clang(%t/class-evo.h)

public class ClassWithIntField {
  var field: Int64
  var str = "Hello this is a big string how are things!!!"
  var arr = ["Hldsfkja", "dsafasdf", "dsafadsf"]

  init() {
    field = 42
    print("init ClassWithIntField")
  }
  deinit {
    print("destroy ClassWithIntField")
  }
}

public class Subclass: ClassWithIntField {
  var blaaa = "dlfkjdlfk"
}

public struct StructWithIntField {
  var field: Int64
  var str = "Hello this is a big string how are things"

  init() {
    field = 0xdeadbeef
    print("init ClassWithIntField")
  }
}




public func returnClassWithIntField() -> ClassWithIntField {
  let s = Subclass()
  return s
}

public func returnStructWithIntField() -> StructWithIntField {
  return StructWithIntField()
}

