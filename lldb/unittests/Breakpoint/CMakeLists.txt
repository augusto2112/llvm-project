add_lldb_unittest(LLDBBreakpointTests
  BreakpointIDTest.cpp
  BreakpointSiteTest.cpp
  WatchpointAlgorithmsTests.cpp

  LINK_COMPONENTS
    Support
  LINK_LIBS
    lldbBreakpoint
    lldbCore
  )
