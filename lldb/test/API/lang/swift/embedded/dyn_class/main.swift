class A {
    let i = 42
}

class B: A {
    let j = 4329
}

class C: B {
    let k = 98
}

func f() {
    let b: A = B()
    let c: A = C()
    StaticString("break here")
}

f()

