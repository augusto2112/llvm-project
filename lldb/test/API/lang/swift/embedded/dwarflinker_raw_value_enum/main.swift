// Embedded Swift emits no reflection metadata, so DWARF is the only source of
// type information. dsymutil's classic DWARFLinker used to drop every
// DW_TAG_enumerator of a raw-value enum whose DW_TAG_enumeration_type became
// live only through the parent walk of one of its member accessors, leaving an
// enumeration type that still carried DW_AT_byte_size but had no cases at all.
//
// rawOf() forces the enum's raw-value accessors to be emitted as concrete
// definitions that point back at their declarations inside the enum via
// DW_AT_specification, which is what makes the parent walk reach the enum
// first. show() then keeps a value of the enum type live at the breakpoint so
// the missing enumerators are observable from the debugger.

enum Event: Int {
    case idle = 0
    case start = 1
    case fault = 100
}

@inline(never)
func rawOf(_ n: Int) -> Int {
    if let e = Event(rawValue: n) { return e.rawValue }
    return -1
}

@inline(never)
func show(_ ev: Event) {
    let s = StaticString("break here")
    print(s) // break here
    print(ev.rawValue)
}

func f() {
    print(rawOf(1))
    show(.fault)
}

f()
