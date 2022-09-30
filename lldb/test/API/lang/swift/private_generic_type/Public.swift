
public struct Wrapper<T> {
  let t: T
  public init(_ t: T) {
    self.t = t
  }
  public func foo<U>(u: U) {
    print(self) // break here
  }
}
