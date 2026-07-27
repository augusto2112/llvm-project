// A 33-byte payload: four Int64s (alignment 8) plus a trailing UInt8. Its
// DW_AT_byte_size (33) is deliberately not a power of two and much larger than
// its real alignment (8).
struct Wide {
  var a: Int64 = 1
  var b: Int64 = 2
  var c: Int64 = 3
  var d: Int64 = 4
  var e: UInt8 = 5
}

// A multi-payload enum: emitted as a DW_TAG_structure_type whose first child is
// a DW_TAG_variant_part, with DW_AT_byte_size 34 and *no* DW_AT_alignment. Its
// authoritative layout is alignment 8 (the max of its payload alignments) and
// stride alignUp(34, 8) = 40.
enum WideEnum {
  case wide(Wide)
  case pair(Int64, Int64)
  case none
}

// Two enums back to back: the offset of `second`, and the whole struct's size,
// are a direct readout of the enum's stride. Correct: 40 + 34 = 74.
struct EnumPair {
  var first: WideEnum
  var second: WideEnum
}

// A one-byte prefix followed by an enum: the offset of `payload` is a readout
// of the enum's alignment. Correct: the payload lands at alignUp(1, 8) = 8, so
// the struct's size is 8 + 34 = 42. A fabricated alignment of 34 rounded 1 up
// to 2 instead (the mask-based round-up is only valid for powers of two).
struct PrefixedEnum {
  var tag: Int8
  var payload: WideEnum
}

@inline(never)
func blackHole(_ x: Int64) {}

func f() {
  let w = WideEnum.wide(Wide())
  let pairs = EnumPair(first: .pair(7, 8), second: .pair(9, 10))
  let prefixed = PrefixedEnum(tag: 3, payload: .pair(11, 12))

  if case .pair(let x, _) = pairs.second { blackHole(x) }
  if case .pair(let y, _) = prefixed.payload { blackHole(y) }
  if case .wide(let z) = w { blackHole(z.a) }

  let s = StaticString("break here")
  print(s) // break here
}

f()
