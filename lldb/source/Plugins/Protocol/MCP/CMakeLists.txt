add_lldb_library(lldbPluginProtocolServerMCP PLUGIN
  Aggregate.cpp
  EventArtifact.cpp
  ObservationEngine.cpp
  ObservationPlan.cpp
  ProtocolServerMCP.cpp
  Resource.cpp
  SerializeValue.cpp
  Tool.cpp
  ValueObjectNode.cpp

  LINK_COMPONENTS
    Support

  LINK_LIBS
    lldbHost
    lldbProtocolMCP
    lldbTarget
    lldbUtility
    lldbValueObject
)
