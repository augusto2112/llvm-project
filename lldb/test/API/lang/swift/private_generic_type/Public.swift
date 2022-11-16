
public class Wrapper<T> {
  let t: T
  public init(_ t: T) {
    self.t = t
  }
  public func foo() {
    print(self) // break here
  }
}
