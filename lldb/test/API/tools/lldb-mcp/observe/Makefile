C_SOURCES := main.c
USE_LIBDL := 1
ENABLE_THREADS := YES

a.out: lib_plugin

include Makefile.rules

# Built as a library of its own rather than linked into the program, because a
# name that resolves before the launch does not exercise resolving one that only
# arrives partway through the run.
lib_plugin:
	"$(MAKE)" -f $(MAKEFILE_RULES) \
		DYLIB_ONLY=YES DYLIB_C_SOURCES=plugin.c DYLIB_NAME=observe_plugin
