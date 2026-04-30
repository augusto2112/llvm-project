@_marker protocol Foo {}

struct Conformer: Foo {
    let bar: Int
}

func f() {
    let optMarker: (any Foo)? = Conformer(bar: 3)

    print("break here")
    print(optMarker as Any)
}

f()
