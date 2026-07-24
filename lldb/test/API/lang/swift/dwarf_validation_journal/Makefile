SWIFT_SOURCES := main.swift
# The DWARF-only shadow context needs full type DWARF. The harness adds this
# automatically only when LLDB_SWIFT_VALIDATE_DWARF is set; this test enables
# the level via `settings set`, so emit -gdwarf-types explicitly.
SWIFTFLAGS_EXTRAS := -gdwarf-types

include Makefile.rules
