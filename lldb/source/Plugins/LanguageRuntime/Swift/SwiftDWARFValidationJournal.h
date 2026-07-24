//===-- SwiftDWARFValidationJournal.h ---------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2026 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Report-mode journal for the reflection-vs-DWARF differential validation.
// These are pure, header-only helpers that turn two independently-computed
// swift::reflection::TypeInfos into a structured JSON divergence record and
// append it to a per-process journal file. They are intentionally free of lldb
// Target/Process/ModuleList dependencies so they can be unit-tested directly.
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_LANGUAGERUNTIME_SWIFT_SWIFTDWARFVALIDATIONJOURNAL_H
#define LLDB_SOURCE_PLUGINS_LANGUAGERUNTIME_SWIFT_SWIFTDWARFVALIDATIONJOURNAL_H

#include "swift/Demangling/Demangler.h"
#include "swift/RemoteInspection/TypeLowering.h"
#include "swift/RemoteInspection/TypeRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/JSON.h"

#include <string>
#include <vector>

namespace lldb_private {
namespace swift_dwarf_journal {

/// One differing dimension between two TypeInfos. `refl`/`dwarf` are the
/// stringified values from each shadow context. For a deep (nested) difference
/// the sentinel dimension "nested" is used with both values "(differ)".
struct TIDifference {
  std::string dimension;
  std::string refl;
  std::string dwarf;
};

/// The dump kind string for a TypeInfo. Mirrors PrintTypeInfo::print in
/// swift/stdlib/public/RemoteInspection/TypeLowering.cpp so journal records and
/// text dumps use the same vocabulary.
inline std::string typeInfoKindString(const swift::reflection::TypeInfo &ti) {
  using namespace swift::reflection;
  switch (ti.getKind()) {
  case TypeInfoKind::Invalid:
    return "invalid";
  case TypeInfoKind::Builtin:
    return "builtin";
  case TypeInfoKind::Reference:
    return "reference";
  case TypeInfoKind::Array:
    return "array";
  case TypeInfoKind::Borrow:
    return "borrow";
  case TypeInfoKind::Record:
    switch (llvm::cast<RecordTypeInfo>(ti).getRecordKind()) {
    case RecordKind::Invalid:
      return "invalid";
    case RecordKind::Tuple:
      return "tuple";
    case RecordKind::Struct:
      return "struct";
    case RecordKind::ThickFunction:
      return "thick_function";
    case RecordKind::OpaqueExistential:
      return "opaque_existential";
    case RecordKind::ClassExistential:
      return "class_existential";
    case RecordKind::ExistentialMetatype:
      return "existential_metatype";
    case RecordKind::ErrorExistential:
      return "error_existential";
    case RecordKind::ClassInstance:
      return "class_instance";
    case RecordKind::ClosureContext:
      return "closure_context";
    }
    return "record";
  case TypeInfoKind::Enum:
    switch (llvm::cast<EnumTypeInfo>(ti).getEnumKind()) {
    case EnumKind::NoPayloadEnum:
      return "no_payload_enum";
    case EnumKind::SinglePayloadEnum:
      return "single_payload_enum";
    case EnumKind::MultiPayloadEnum:
      return "multi_payload_enum";
    }
    return "enum";
  }
  return "unknown";
}

/// Reference kind string (strong/weak/unowned/...), reusing the same X-macro
/// the dumper uses.
inline std::string referenceKindString(swift::reflection::ReferenceKind k) {
  using namespace swift::reflection;
  switch (k) {
  case ReferenceKind::Strong:
    return "strong";
#define REF_STORAGE(Name, name, ...)                                           \
  case ReferenceKind::Name:                                                    \
    return #name;
#include "swift/AST/ReferenceStorage.def"
  }
  return "unknown";
}

/// Reference-counting string (native/unknown).
inline std::string
referenceCountingString(swift::reflection::ReferenceCounting c) {
  using namespace swift::reflection;
  switch (c) {
  case ReferenceCounting::Native:
    return "native";
  case ReferenceCounting::Unknown:
    return "unknown";
  }
  return "unknown";
}

/// Serialize one level of a record's fields / an enum's cases. Each entry gets
/// name, offset, value (enum case index/value), the nested TypeInfo's kind
/// string, and the field/case TypeRef's mangled name (null when absent). Deeper
/// nesting is not recursed — the capped text dump covers full detail.
inline llvm::json::Array
serializeFields(const std::vector<swift::reflection::FieldInfo> &fields) {
  llvm::json::Array arr;
  for (const auto &f : fields) {
    llvm::json::Object fo;
    fo["name"] = f.Name;
    fo["offset"] = (int64_t)f.Offset;
    fo["value"] = (int64_t)f.Value;
    fo["kind"] = typeInfoKindString(f.TI);
    if (f.TR) {
      swift::Demangle::Demangler dem;
      if (auto mangled = f.TR->mangle(dem))
        fo["type_mangled"] = *mangled;
      else
        fo["type_mangled"] = nullptr;
    } else {
      fo["type_mangled"] = nullptr;
    }
    arr.push_back(std::move(fo));
  }
  return arr;
}

/// Serialize a TypeInfo to a structured JSON object, reusing the dump kind
/// vocabulary. Scalars are always included; reference/builtin/record/enum add
/// their kind-specific fields.
inline llvm::json::Object
serializeTypeInfo(const swift::reflection::TypeInfo &ti) {
  using namespace swift::reflection;
  llvm::json::Object o;
  o["kind"] = typeInfoKindString(ti);
  o["size"] = (int64_t)ti.getSize();
  o["alignment"] = (int64_t)ti.getAlignment();
  o["stride"] = (int64_t)ti.getStride();
  o["num_extra_inhabitants"] = (int64_t)ti.getNumExtraInhabitants();
  o["bitwise_takable"] = ti.isBitwiseTakable();
  o["addressable_for_dependencies"] = ti.isAddressableForDependencies();

  if (auto *ref = llvm::dyn_cast<ReferenceTypeInfo>(&ti)) {
    o["reference_kind"] = referenceKindString(ref->getReferenceKind());
    o["refcounting"] = referenceCountingString(ref->getReferenceCounting());
  } else if (auto *bti = llvm::dyn_cast<BuiltinTypeInfo>(&ti)) {
    o["builtin_mangled_name"] = bti->getMangledTypeName();
  } else if (auto *rec = llvm::dyn_cast<RecordTypeInfo>(&ti)) {
    o["fields"] = serializeFields(rec->getFields());
  } else if (auto *en = llvm::dyn_cast<EnumTypeInfo>(&ti)) {
    o["fields"] = serializeFields(en->getCases());
  }
  return o;
}

/// Every way two TypeInfos differ, honoring `flags` exactly as TypeInfo::Equals
/// does. Scalar dimensions (size/alignment/stride/num_extra_inhabitants/
/// borrowability/addressable) are flag-gated; structural invariants (kind,
/// record/enum sub-kind, reference kind/counting, field/case count) are always
/// reported. Deep field/name/offset/typeref differences are not enumerated here
/// (the capped text dumps carry that detail); if Equals still disagrees after
/// the checks above, a single sentinel "nested" difference is recorded so a real
/// divergence is never emitted with an empty list.
inline std::vector<TIDifference>
allTypeInfoDifferences(const swift::reflection::TypeInfo &a,
                       const swift::reflection::TypeInfo &b,
                       swift::reflection::TypeInfoComparison flags) {
  using namespace swift::reflection;
  std::vector<TIDifference> diffs;
  auto n = [](uint64_t x) { return std::to_string(x); };

  if (a.getKind() != b.getKind()) {
    diffs.push_back(
        {"kind", n((unsigned)a.getKind()), n((unsigned)b.getKind())});
    return diffs; // Different kinds: nothing else is comparable.
  }
  if (contains(flags, TypeInfoComparison::Size) && a.getSize() != b.getSize())
    diffs.push_back({"size", n(a.getSize()), n(b.getSize())});
  if (contains(flags, TypeInfoComparison::Alignment) &&
      a.getAlignment() != b.getAlignment())
    diffs.push_back({"alignment", n(a.getAlignment()), n(b.getAlignment())});
  if (contains(flags, TypeInfoComparison::Stride) &&
      a.getStride() != b.getStride())
    diffs.push_back({"stride", n(a.getStride()), n(b.getStride())});
  if (contains(flags, TypeInfoComparison::NumExtraInhabitants) &&
      a.getNumExtraInhabitants() != b.getNumExtraInhabitants())
    diffs.push_back({"num_extra_inhabitants", n(a.getNumExtraInhabitants()),
                     n(b.getNumExtraInhabitants())});
  if (contains(flags, TypeInfoComparison::Borrowability) &&
      a.getBorrowability() != b.getBorrowability())
    diffs.push_back({"borrowability", n((unsigned)a.getBorrowability()),
                     n((unsigned)b.getBorrowability())});
  if (contains(flags, TypeInfoComparison::AddressableForDependencies) &&
      a.isAddressableForDependencies() != b.isAddressableForDependencies())
    diffs.push_back({"addressable_for_dependencies",
                     a.isAddressableForDependencies() ? "true" : "false",
                     b.isAddressableForDependencies() ? "true" : "false"});

  if (auto *ra = llvm::dyn_cast<RecordTypeInfo>(&a)) {
    auto *rb = llvm::cast<RecordTypeInfo>(&b);
    if (ra->getRecordKind() != rb->getRecordKind())
      diffs.push_back({"record_kind", n((unsigned)ra->getRecordKind()),
                       n((unsigned)rb->getRecordKind())});
    if (ra->getNumFields() != rb->getNumFields())
      diffs.push_back(
          {"field_count", n(ra->getNumFields()), n(rb->getNumFields())});
  } else if (auto *ea = llvm::dyn_cast<EnumTypeInfo>(&a)) {
    auto *eb = llvm::cast<EnumTypeInfo>(&b);
    if (ea->getEnumKind() != eb->getEnumKind())
      diffs.push_back({"enum_kind", n((unsigned)ea->getEnumKind()),
                       n((unsigned)eb->getEnumKind())});
    if (ea->getCases().size() != eb->getCases().size())
      diffs.push_back({"case_count", n(ea->getCases().size()),
                       n(eb->getCases().size())});
  } else if (auto *fa = llvm::dyn_cast<ReferenceTypeInfo>(&a)) {
    auto *fb = llvm::cast<ReferenceTypeInfo>(&b);
    if (fa->getReferenceKind() != fb->getReferenceKind())
      diffs.push_back({"reference_kind",
                       referenceKindString(fa->getReferenceKind()),
                       referenceKindString(fb->getReferenceKind())});
    if (fa->getReferenceCounting() != fb->getReferenceCounting())
      diffs.push_back({"refcounting",
                       referenceCountingString(fa->getReferenceCounting()),
                       referenceCountingString(fb->getReferenceCounting())});
  }

  if (diffs.empty() && !a.Equals(b, flags))
    diffs.push_back({"nested", "(differ)", "(differ)"});

  return diffs;
}

/// A coarse, deterministic cluster fingerprint: the comma-joined differing
/// dimensions, then a leaf/category token (a builtin's mangled name, or a
/// reference's "<kind>.<counting>", else the reflection TypeInfo's kind
/// string), then the producer. Heuristic by design — downstream clustering may
/// split or merge on it.
inline std::string computeRootCauseKey(const std::vector<TIDifference> &diffs,
                                       const swift::reflection::TypeInfo *refl,
                                       llvm::StringRef producer) {
  std::string dims;
  for (size_t i = 0; i < diffs.size(); ++i) {
    if (i)
      dims += ",";
    dims += diffs[i].dimension;
  }
  std::string category = "unknown";
  if (refl) {
    category = typeInfoKindString(*refl);
    if (auto *bti = llvm::dyn_cast<swift::reflection::BuiltinTypeInfo>(refl)) {
      if (!bti->getMangledTypeName().empty())
        category = bti->getMangledTypeName();
    } else if (auto *ref =
                   llvm::dyn_cast<swift::reflection::ReferenceTypeInfo>(refl)) {
      category = referenceKindString(ref->getReferenceKind()) + "." +
                 referenceCountingString(ref->getReferenceCounting());
    }
  }
  return dims + "|" + category + "|" + producer.str();
}

} // namespace swift_dwarf_journal
} // namespace lldb_private

#endif // LLDB_SOURCE_PLUGINS_LANGUAGERUNTIME_SWIFT_SWIFTDWARFVALIDATIONJOURNAL_H
