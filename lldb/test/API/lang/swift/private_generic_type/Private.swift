import Public

private struct InvisibleStruct {
  public init() {}
  public var name = "The invisible man."
}


public func privateDoIt()  {
  let x = Wrapper(InvisibleStruct())
  x.foo(u: 3)
}
