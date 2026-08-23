add_lldb_library(lldbPluginProtocolServerMCP PLUGIN
  EventArtifact.cpp
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
