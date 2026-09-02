// A class hierarchy whose instances force LLDB down the class-instance
// TypeRef path: a subclass held in a superclass-typed variable, and one held
// in a class-constrained existential, so dynamic type resolution has to run.

protocol Shape: AnyObject {}

class Base: Shape {
    let baseField = 42
}

class Derived: Base {
    let derivedField = 100
}

class Leaf: Derived {
    let leafField = 7
}

func f() {
    let asBase: Base = Derived()
    let asExistential: Shape = Derived()
    let deep: Base = Leaf()
    print("break here")
}

f()
