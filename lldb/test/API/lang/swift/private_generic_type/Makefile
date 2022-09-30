SWIFT_SOURCES := main.swift

all:  libPublic.dylib libPrivate.dylib a.out

include Makefile.rules
LD_EXTRAS = -lPublic -lPrivate -L$(BUILDDIR)
SWIFTFLAGS_EXTRAS = -I$(BUILDDIR)

libPrivate.dylib: Private.swift
	$(MAKE) MAKE_DSYM=NO CC=$(CC) SWIFTC=$(SWIFTC) \
		ARCH=$(ARCH) DSYMUTIL=$(DSYMUTIL) \
		BASENAME=Private DEBUG_INFO_FLAG=-gnone \
		LD_EXTRAS="-lPublic -L$(BUILDDIR)" \
		SWIFTFLAGS_EXTRAS="-I$(BUILDDIR)" \
		VPATH=$(SRCDIR) -I $(SRCDIR) -f $(SRCDIR)/dylib.mk all

libPublic.dylib: Public.swift
	$(MAKE) MAKE_DSYM=YES CC=$(CC) SWIFTC=$(SWIFTC) \
		ARCH=$(ARCH) DSYMUTIL=$(DSYMUTIL) \
		BASENAME=Public \
		SWIFTFLAGS_EXTRAS="-I$(BUILDDIR)" \
		VPATH=$(SRCDIR) -I $(SRCDIR) -f $(SRCDIR)/dylib.mk all

clean::
	$(MAKE) BASENAME=Private VPATH=$(SRCDIR) -I $(SRCDIR) -f $(SRCDIR)/dylib.mk clean
	$(MAKE) BASENAME=Public VPATH=$(SRCDIR) -I $(SRCDIR) -f $(SRCDIR)/dylib.mk clean
