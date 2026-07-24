class Foo {
  var x: Int = 42
  var y: Double = 3.5
}

struct Bar {
  var a: Int
  var b: Foo
}

func use(_ foo: Foo, _ bar: Bar) {
  print(foo.x, bar.a) // break here
}

let foo = Foo()
let bar = Bar(a: 1, b: foo)
use(foo, bar)
