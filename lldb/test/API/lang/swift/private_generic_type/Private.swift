import Public

private struct InvisibleStruct {
  public init() {}
  public var name = "The invisible man."
}


func freeStanding<T>(t: Wrapper<T>) {
  t.foo()
}

public func privateDoIt()  {
  let x = Wrapper(InvisibleStruct())
  freeStanding(t: x)
}
