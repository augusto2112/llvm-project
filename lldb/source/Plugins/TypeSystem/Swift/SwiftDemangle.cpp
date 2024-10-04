//===-- SwiftDemangle.h ---------------------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "SwiftDemangle.h"

namespace lldb_private {
namespace swift_demangle {

/// Given a node pointer to a TypeMetadata node, return the type node of the
/// type metadata.
swift::Demangle::NodePointer
getTypeNodeFromMetadataNode(swift::Demangle::NodePointer node) {
  if (node->getKind() != Node::Kind::Global || node->getNumChildren() != 1)
    return nullptr;
  NodePointer type_medata_node = node->getFirstChild();
  if (type_medata_node->getKind() != Node::Kind::TypeMetadata ||
      type_medata_node->getNumChildren() != 1)
    return nullptr;
  NodePointer type_node = type_medata_node->getFirstChild();
  if (type_node->getKind() != Node::Kind::Type)
    return nullptr;
  return type_node;
}
} // namespace swift_demangle
} // namespace lldb_private

