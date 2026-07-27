//===-- ReflectionContext.cpp --------------------------------------------===//
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

#include "swift/RemoteInspection/ReflectionContext.h"
#include "Plugins/TypeSystem/Swift/TypeSystemSwiftTypeRef.h"
#include "ReflectionContextInterface.h"
#include "SwiftDWARFValidationJournal.h"
#include "SwiftLanguageRuntime.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"
#include "swift/Demangling/Demangle.h"
#include "swift/RemoteInspection/DescriptorFinder.h"
#include "swift/RemoteInspection/TypeLowering.h"
#include "swift/RemoteInspection/TypeRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

using namespace lldb;
using namespace lldb_private;

namespace {

/// The descriptor finder needs to be an instance variable of the
/// TypeRefBuilder, but we would still want to swap out the descriptor finder,
/// as they are tied to each type system typeref's symbol file. This class's
/// only purpose is to allow this swapping.
struct DescriptorFinderForwarder : public swift::reflection::DescriptorFinder {
  DescriptorFinderForwarder() = default;
  ~DescriptorFinderForwarder() override = default;

  std::unique_ptr<swift::reflection::BuiltinTypeDescriptorBase>
  getBuiltinTypeDescriptor(const swift::reflection::TypeRef *TR) override {
    if (!m_descriptor_finders.empty() && shouldConsultDescriptorFinder())
      return m_descriptor_finders.back()->getBuiltinTypeDescriptor(TR);
    return nullptr;
  }

  std::unique_ptr<swift::reflection::FieldDescriptorBase>
  getFieldDescriptor(const swift::reflection::TypeRef *TR) override {
    if (!m_descriptor_finders.empty() && shouldConsultDescriptorFinder())
      return m_descriptor_finders.back()->getFieldDescriptor(TR);
    return nullptr;
  }

  std::unique_ptr<swift::reflection::MultiPayloadEnumDescriptorBase>
  getMultiPayloadEnumDescriptor(const swift::reflection::TypeRef *TR) override {
    if (!m_descriptor_finders.empty() && shouldConsultDescriptorFinder())
      return m_descriptor_finders.back()->getMultiPayloadEnumDescriptor(TR);
    return nullptr;
  }

  void PushExternalDescriptorFinder(
      swift::reflection::DescriptorFinder *descriptor_finder) {
    m_descriptor_finders.push_back(descriptor_finder);
  }

  void PopExternalDescriptorFinder() {
    assert(!m_descriptor_finders.empty() && "m_descriptor_finders is empty!");
    m_descriptor_finders.pop_back();
  }

  void SetImageAdded(bool image_added) {
    m_image_added |= image_added;
  }

  /// Record that LLDB has encountered an Embedded Swift type and thus that the
  /// external descriptor finder should be enabled.
  void SetEmbedded() { m_embedded = true; }

  /// The fixed source-selection policy for this context.
  enum class Policy {
    Primary,        // Existing GetSwiftEnableFullDwarfDebugging()-driven logic.
    ReflectionOnly, // Never consult DWARF (pure reflection-metadata layout).
    DwarfOnly,      // Always consult DWARF (pure DWARF layout).
  };

  void SetPolicy(Policy policy) { m_policy = policy; }

private:
  bool shouldConsultDescriptorFinder() {
    switch (m_policy) {
    case Policy::ReflectionOnly:
      return false;
    case Policy::DwarfOnly:
      return true;
    case Policy::Primary:
      break;
    }
    switch (ModuleList::GetGlobalModuleListProperties()
                .GetSwiftEnableFullDwarfDebugging()) {
    case lldb_private::AutoBool::True:
      return true;
    case lldb_private::AutoBool::False:
      return false;
    case lldb_private::AutoBool::Auto:
      // Embedded Swift never has reflection metadata for its types, so the
      // external descriptor finder is the only source of type information.
      if (m_embedded)
        return true;
      // Otherwise, full DWARF debugging is auto-enabled if there is no
      // reflection metadata to read from.
      return !m_image_added;
    }
  }

  llvm::SmallVector<swift::reflection::DescriptorFinder *, 1>
      m_descriptor_finders;
  bool m_image_added = false;
  bool m_embedded = false;
  Policy m_policy = Policy::Primary;
};

/// The DWARF differential is active iff the master validation switch is on and
/// the DWARF comparison level parses to a non-empty flag set.
static bool IsDwarfValidationActive() {
  auto &props = ModuleList::GetGlobalModuleListProperties();
  if (!props.GetSwiftValidateTypeSystem())
    return false;
  return swift::reflection::parseTypeInfoComparison(
             props.GetSwiftValidateTypeSystemDWARF()) !=
         swift::reflection::TypeInfoComparison::None;
}

/// The active comparison flags (only meaningful when IsDwarfValidationActive()).
static swift::reflection::TypeInfoComparison DwarfValidationFlags() {
  return swift::reflection::parseTypeInfoComparison(
      ModuleList::GetGlobalModuleListProperties()
          .GetSwiftValidateTypeSystemDWARF());
}

/// The report-mode journal directory; empty means the default assert-on-
/// divergence behavior.
static std::string DwarfValidationJournalDir() {
  return ModuleList::GetGlobalModuleListProperties()
      .GetSwiftValidateTypeSystemDWARFJournal()
      .str();
}

/// Whether \p mangled names an Embedded Swift type, for which the
/// reflection-vs-DWARF differential is not a meaningful comparison.
///
/// Embedded Swift emits no reflection metadata at all: the frontend sets
/// ReflectionMetadataMode::None for Feature::Embedded, and an embedded binary
/// has no __swift5_* sections. Any reflection answer for such a type therefore
/// comes from some other image -- in practice the host's non-embedded
/// libswiftCore, which LLDB loads into the process for expression evaluation
/// and which reflection-name matching finds because it is mangling-flavor
/// blind. Comparing that against the program's own DWARF diffs two different
/// ABIs (extra inhabitants, for one, differ because SwiftTargetInfo skips the
/// Darwin LeastValidPointerValue under Feature::Embedded), so a mismatch says
/// nothing about the debug info under test. Skip it rather than report it.
static bool IsEmbeddedSwiftName(llvm::StringRef mangled) {
  return SwiftLanguageRuntime::GetManglingFlavor(mangled) ==
         swift::Mangle::ManglingFlavor::Embedded;
}

/// Best-effort, succinct description of the FIRST way two TypeInfos differ, for
/// a human-readable divergence summary. Mirrors the dimensions
/// TypeInfo::Equals checks; falls back to a generic note for nested/field-level
/// differences (the full dumps carry those details).
static std::string
firstTypeInfoDifference(const swift::reflection::TypeInfo &a,
                        const swift::reflection::TypeInfo &b,
                        swift::reflection::TypeInfoComparison flags) {
  using namespace swift::reflection;
  auto n = [](uint64_t x) { return std::to_string(x); };

  if (a.getKind() != b.getKind())
    return "kind: " + n((unsigned)a.getKind()) + " != " +
           n((unsigned)b.getKind());
  if (contains(flags, TypeInfoComparison::Size) && a.getSize() != b.getSize())
    return "size: " + n(a.getSize()) + " != " + n(b.getSize());
  if (contains(flags, TypeInfoComparison::Alignment) &&
      a.getAlignment() != b.getAlignment())
    return "alignment: " + n(a.getAlignment()) + " != " + n(b.getAlignment());
  if (contains(flags, TypeInfoComparison::Stride) &&
      a.getStride() != b.getStride())
    return "stride: " + n(a.getStride()) + " != " + n(b.getStride());
  if (contains(flags, TypeInfoComparison::NumExtraInhabitants) &&
      a.getNumExtraInhabitants() != b.getNumExtraInhabitants())
    return "num_extra_inhabitants: " + n(a.getNumExtraInhabitants()) + " != " +
           n(b.getNumExtraInhabitants());
  if (contains(flags, TypeInfoComparison::Borrowability) &&
      a.getBorrowability() != b.getBorrowability())
    return "borrowability: " + n((unsigned)a.getBorrowability()) + " != " +
           n((unsigned)b.getBorrowability());
  if (contains(flags, TypeInfoComparison::AddressableForDependencies) &&
      a.isAddressableForDependencies() != b.isAddressableForDependencies())
    return std::string("addressable_for_dependencies: ") +
           (a.isAddressableForDependencies() ? "true" : "false") + " != " +
           (b.isAddressableForDependencies() ? "true" : "false");

  if (auto *ra = llvm::dyn_cast<RecordTypeInfo>(&a)) {
    auto *rb = llvm::cast<RecordTypeInfo>(&b);
    if (ra->getRecordKind() != rb->getRecordKind())
      return "record kind: " + n((unsigned)ra->getRecordKind()) + " != " +
             n((unsigned)rb->getRecordKind());
    if (ra->getNumFields() != rb->getNumFields())
      return "field count: " + n(ra->getNumFields()) + " != " +
             n(rb->getNumFields());
    return "(field-level difference — see dumps below)";
  }
  if (auto *ea = llvm::dyn_cast<EnumTypeInfo>(&a)) {
    auto *eb = llvm::cast<EnumTypeInfo>(&b);
    if (ea->getEnumKind() != eb->getEnumKind())
      return "enum kind: " + n((unsigned)ea->getEnumKind()) + " != " +
             n((unsigned)eb->getEnumKind());
    if (ea->getCases().size() != eb->getCases().size())
      return "case count: " + n(ea->getCases().size()) + " != " +
             n(eb->getCases().size());
    return "(case-level difference — see dumps below)";
  }
  if (auto *fa = llvm::dyn_cast<ReferenceTypeInfo>(&a)) {
    auto *fb = llvm::cast<ReferenceTypeInfo>(&b);
    if (fa->getReferenceKind() != fb->getReferenceKind())
      return "reference kind: " + n((unsigned)fa->getReferenceKind()) + " != " +
             n((unsigned)fb->getReferenceKind());
    if (fa->getReferenceCounting() != fb->getReferenceCounting())
      return "reference counting: " +
             n((unsigned)fa->getReferenceCounting()) + " != " +
             n((unsigned)fb->getReferenceCounting());
  }
  return "(structural difference — see dumps below)";
}

/// Report a reflection-vs-DWARF divergence. `refl`/`dwarf` are the two shadow
/// results; errors are consumed here. Default (no journal directory): print the
/// stderr summary and assert on divergence, log asymmetry. Report mode (journal
/// directory set): append a structured JSON record per divergence/asymmetry and
/// keep going, so one suite run captures every divergence. Works for TypeInfo
/// and any subclass (comparison and dump dispatch virtually).
template <typename ExpectedTI>
static void ReportShadowComparison(llvm::StringRef name, ExpectedTI &refl,
                                   ExpectedTI &dwarf,
                                   swift::reflection::TypeInfoComparison flags,
                                   llvm::StringRef producer,
                                   uint64_t provider_id) {
  Log *log = GetLog(LLDBLog::Types);
  bool have_refl = static_cast<bool>(refl);
  bool have_dwarf = static_cast<bool>(dwarf);

  if (have_refl && have_dwarf) {
    if (!refl->Equals(*dwarf, flags)) {
      std::string journal = DwarfValidationJournalDir();
      std::string level = ModuleList::GetGlobalModuleListProperties()
                              .GetSwiftValidateTypeSystemDWARF()
                              .str();
      if (!journal.empty()) {
        // Report mode: record and continue.
        swift_dwarf_journal::JournalRecordInputs in;
        in.type_mangled = name;
        in.producer = producer;
        in.level = level;
        in.provider_id = provider_id;
        in.refl = &*refl;
        in.dwarf = &*dwarf;
        in.flags = flags;
        swift_dwarf_journal::writeJournalRecord(
            journal,
            llvm::json::Value(swift_dwarf_journal::buildJournalRecord(in)));
      } else {
        std::stringstream r, d;
        refl->dump(r);
        dwarf->dump(d);
        // Print a succinct, always-visible summary to stderr just before the
        // assert, so a divergence is diagnosable without the "lldb types" log
        // channel enabled: what was queried, the level, the first differing
        // dimension, and both TypeInfo dumps for detail.
        std::string demangled = swift::Demangle::demangleSymbolAsString(name);
        std::string summary;
        llvm::raw_string_ostream os(summary);
        os << "\n=== Swift reflection-vs-DWARF TypeInfo divergence ===\n"
           << "  type:  " << name << "\n";
        if (!demangled.empty() && demangled != name.str())
          os << "  as:    " << demangled << "\n";
        os << "  level: " << level << "\n"
           << "  diff:  " << firstTypeInfoDifference(*refl, *dwarf, flags)
           << "\n"
           << "  --- reflection-only ---\n"
           << r.str() << "  --- DWARF-only ---\n"
           << d.str()
           << "=====================================================\n";
        llvm::errs() << os.str();
        LLDB_LOG(log,
                 "reflection-vs-DWARF TypeInfo divergence for {0}:\n"
                 "reflection-only:\n{1}\nDWARF-only:\n{2}",
                 name, r.str(), d.str());
        assert(false && "reflection-vs-DWARF TypeInfo divergence");
      }
    }
  } else if (have_refl != have_dwarf) {
    // Exactly one side produced a result: a DWARF-completeness signal, not a
    // layout bug. Record it in report mode; otherwise log without asserting.
    std::string journal = DwarfValidationJournalDir();
    std::string level = ModuleList::GetGlobalModuleListProperties()
                            .GetSwiftValidateTypeSystemDWARF()
                            .str();
    if (!journal.empty()) {
      swift_dwarf_journal::JournalRecordInputs in;
      in.type_mangled = name;
      in.producer = producer;
      in.level = level;
      in.provider_id = provider_id;
      in.refl = have_refl ? &*refl : nullptr;
      in.dwarf = have_dwarf ? &*dwarf : nullptr;
      in.flags = flags;
      swift_dwarf_journal::writeJournalRecord(
          journal,
          llvm::json::Value(swift_dwarf_journal::buildJournalRecord(in)));
    } else {
      LLDB_LOG(log,
               "reflection-vs-DWARF TypeInfo asymmetry for {0}: "
               "reflection-only={1} DWARF-only={2}",
               name, have_refl, have_dwarf);
    }
  }
  if (!refl)
    llvm::consumeError(refl.takeError());
  if (!dwarf)
    llvm::consumeError(dwarf.takeError());
}

/// An implementation of the generic ReflectionContextInterface that
/// is templatized on target pointer width and specialized to either
/// 32-bit or 64-bit pointers, with and without ObjC interoperability.
template <typename ReflectionContext, bool ObjCEnabled, unsigned PointerSize>
class TargetReflectionContext : public ReflectionContextInterface {
  DescriptorFinderForwarder m_forwader;
  ReflectionContext m_reflection_ctx;
  swift::reflection::TypeConverter &m_type_converter;

  // Validation-only shadow contexts, present only when the DWARF differential
  // is active. Same template instantiation as the primary; their own shadow
  // pointers are null so they compute without recursing.
  std::unique_ptr<TargetReflectionContext> m_reflection_only;
  std::unique_ptr<TargetReflectionContext> m_dwarf_only;

public:
  TargetReflectionContext(
      std::shared_ptr<swift::reflection::MemoryReader> reader,
      SwiftMetadataCache *swift_metadata_cache,
      DescriptorFinderForwarder::Policy policy =
          DescriptorFinderForwarder::Policy::Primary)
      : m_reflection_ctx(reader, swift_metadata_cache, &m_forwader),
        m_type_converter(m_reflection_ctx.getBuilder().getTypeConverter()) {
    m_forwader.SetPolicy(policy);
    m_type_converter.enableErrorCache();
    if (policy == DescriptorFinderForwarder::Policy::Primary &&
        IsDwarfValidationActive()) {
      m_reflection_only = std::make_unique<TargetReflectionContext>(
          reader, swift_metadata_cache,
          DescriptorFinderForwarder::Policy::ReflectionOnly);
      m_dwarf_only = std::make_unique<TargetReflectionContext>(
          reader, swift_metadata_cache,
          DescriptorFinderForwarder::Policy::DwarfOnly);
    }
  }

  std::optional<uint32_t> AddImage(
      llvm::function_ref<std::pair<swift::remote::RemoteRef<void>, uint64_t>(
          swift::ReflectionSectionKind)>
          find_section,
      llvm::SmallVector<llvm::StringRef, 1> likely_module_names) override {
    auto id = m_reflection_ctx.addImage(find_section, likely_module_names);
    m_forwader.SetImageAdded(id.has_value());
    if (m_reflection_only)
      m_reflection_only->AddImage(find_section, likely_module_names);
    return id;
  }

  std::optional<uint32_t>
  AddImage(swift::remote::RemoteAddress image_start,
           llvm::SmallVector<llvm::StringRef, 1> likely_module_names) override {
    auto id = m_reflection_ctx.addImage(image_start, likely_module_names);
    m_forwader.SetImageAdded(id.has_value());
    if (m_reflection_only)
      m_reflection_only->AddImage(image_start, likely_module_names);
    return id;
  }

  llvm::Expected<const swift::reflection::TypeRef &>
  GetTypeRef(StringRef mangled_type_name) override {
    swift::Demangle::Demangler dem;
    swift::Demangle::NodePointer node = dem.demangleSymbol(mangled_type_name);
    return GetTypeRef(dem, node);
  }

  /// Sets the descriptor finder, and on scope exit clears it out.
  auto PushDescriptorFinderAndPopOnExit(
      swift::reflection::DescriptorFinder *descriptor_finder) {
    m_forwader.PushExternalDescriptorFinder(descriptor_finder);
    return llvm::make_scope_exit(
        [&]() { m_forwader.PopExternalDescriptorFinder(); });
  }

  llvm::Expected<const swift::reflection::TypeRef &>
  GetTypeRef(swift::Demangle::Demangler &dem,
             swift::Demangle::NodePointer node) override {
    auto type_ref_or_err =
        swift::Demangle::decodeMangledType(m_reflection_ctx.getBuilder(), node);
    if (type_ref_or_err.isError())
      return llvm::createStringError(
          type_ref_or_err.getError()->copyErrorString());
    auto *tr = type_ref_or_err.getType();
    if (!tr)
      return llvm::createStringError(
          "decoder returned nullptr typeref but no error");
    return *tr;
  }

  llvm::Expected<const swift::reflection::RecordTypeInfo &>
  GetClassInstanceTypeInfo(
      const swift::reflection::TypeRef &type_ref,
      swift::remote::TypeInfoProvider *provider,
      swift::reflection::DescriptorFinder *descriptor_finder,
      swift::Mangle::ManglingFlavor flavor) override {
    auto on_exit = PushDescriptorFinderAndPopOnExit(descriptor_finder);
    auto start =
        m_reflection_ctx.computeUnalignedFieldStartOffset(&type_ref, provider);
    if (!start) {
      std::stringstream ss;
      type_ref.dump(ss);
      return llvm::createStringError(
          "Could not compute start field offset for typeref: " + ss.str());
    }

    auto *rti =
        m_type_converter.getClassInstanceTypeInfo(&type_ref, *start, provider);
    if (!rti)
      return llvm::createStringError(m_type_converter.takeLastError());
    if (m_reflection_only && m_dwarf_only)
      CompareShadowsForClassInstance(type_ref, provider, descriptor_finder,
                                     flavor);
    return *rti;
  }

  llvm::Expected<const swift::reflection::TypeInfo &>
  GetTypeInfo(CompilerType type, swift::remote::TypeInfoProvider *provider,
              swift::reflection::DescriptorFinder *descriptor_finder) override {
    if (SwiftLanguageRuntime::GetManglingFlavor(
            type.GetMangledTypeName().GetStringRef()) ==
        swift::Mangle::ManglingFlavor::Embedded)
      m_forwader.SetEmbedded();

    auto type_ref_or_err = GetCanonicalTypeRef(type);
    if (!type_ref_or_err)
      return type_ref_or_err.takeError();
    auto result = GetTypeInfoFromTypeRef(*type_ref_or_err, provider,
                                         descriptor_finder);
    if (m_reflection_only && m_dwarf_only)
      CompareShadowsForType(type, provider, descriptor_finder);
    return result;
  }

  llvm::Expected<const swift::reflection::TypeInfo &> GetTypeInfoFromInstance(
      lldb::addr_t instance, swift::remote::TypeInfoProvider *provider,
      swift::reflection::DescriptorFinder *descriptor_finder) override {
    auto on_exit = PushDescriptorFinderAndPopOnExit(descriptor_finder);
    auto *ti = m_reflection_ctx.getInstanceTypeInfo(
        swift::remote::RemoteAddress(
            instance, swift::remote::RemoteAddress::DefaultAddressSpace),
        provider);
    if (!ti)
      return llvm::createStringError("could not get instance type info");
    return *ti;
  }

  swift::reflection::MemoryReader &GetReader() override {
    return m_reflection_ctx.getReader();
  }

  const swift::reflection::TypeRef *LookupSuperclass(
      const swift::reflection::TypeRef &tr,
      swift::reflection::DescriptorFinder *descriptor_finder) override {
    auto on_exit = PushDescriptorFinderAndPopOnExit(descriptor_finder);
    return m_reflection_ctx.getBuilder().lookupSuperclass(&tr);
  }

  bool
  ForEachSuperClassType(swift::remote::TypeInfoProvider *tip,
                        swift::reflection::DescriptorFinder *descriptor_finder,
                        const swift::reflection::TypeRef *tr,
                        swift::Mangle::ManglingFlavor flavor,
                        std::function<bool(SuperClassType)> fn) override {
    // Guard against faulty self-referential metadata.
    unsigned limit = 256;
    while (tr && --limit) {
      if (fn({[=]() -> const swift::reflection::RecordTypeInfo * {
                auto ti_or_err =
                    GetRecordTypeInfo(*tr, tip, descriptor_finder, flavor);
                if (!ti_or_err) {
                  LLDB_LOG_ERRORV(GetLog(LLDBLog::Types), ti_or_err.takeError(),
                                  "ForEachSuperClassType: {0}");
                  return nullptr;
                }
                return &*ti_or_err;
              },
              [=]() -> const swift::reflection::TypeRef * { return tr; }}))
        return true;

      tr = LookupSuperclass(*tr, descriptor_finder);
    }
    return false;
  }

  bool
  ForEachSuperClassType(swift::remote::TypeInfoProvider *tip,
                        swift::reflection::DescriptorFinder *descriptor_finder,
                        lldb::addr_t pointer,
                        std::function<bool(SuperClassType)> fn) override {
    auto on_exit = PushDescriptorFinderAndPopOnExit(descriptor_finder);
    // Guard against faulty self-referential metadata.
    unsigned limit = 256;
    auto md_ptr =
        m_reflection_ctx.readMetadataFromInstance(swift::remote::RemoteAddress(
            pointer, swift::remote::RemoteAddress::DefaultAddressSpace));
    if (!md_ptr)
      return false;

    // Class object.
    while (md_ptr && *md_ptr && --limit) {
      // Reading metadata is potentially expensive since (in a remote
      // debugging scenario it may even incur network traffic) so we
      // just return closures that the caller can use to query details
      // if they need them.'
      auto metadata = *md_ptr;
      if (fn({[=]() -> const swift::reflection::RecordTypeInfo * {
                auto *ti = m_reflection_ctx.getMetadataTypeInfo(metadata, tip);
                return llvm::dyn_cast_or_null<
                    swift::reflection::RecordTypeInfo>(ti);
              },
              [=]() -> const swift::reflection::TypeRef * {
                return m_reflection_ctx.readTypeFromMetadata(metadata);
              }}))
        return true;

      // Continue with the base class.
      md_ptr = m_reflection_ctx.readSuperClassFromClassMetadata(metadata);
    }
    return false;
  }

  std::optional<int32_t> ProjectEnumValue(
      swift::remote::RemoteAddress enum_addr,
      const swift::reflection::TypeRef *enum_type_ref,
      swift::remote::TypeInfoProvider *provider,
      swift::reflection::DescriptorFinder *descriptor_finder) override {
    auto on_exit = PushDescriptorFinderAndPopOnExit(descriptor_finder);
    int32_t case_idx;
    if (m_reflection_ctx.projectEnumValue(enum_addr, enum_type_ref, &case_idx,
                                          provider))
      return case_idx;
    return {};
  }

  llvm::Expected<std::pair<const swift::reflection::TypeRef *,
                           swift::reflection::RemoteAddress>>
  ProjectExistentialAndUnwrapClass(
      swift::reflection::RemoteAddress existential_address,
      CompilerType existential_type,
      swift::reflection::DescriptorFinder *descriptor_finder) override {
    auto type_ref_or_err = GetCanonicalTypeRef(existential_type);
    if (!type_ref_or_err)
      return type_ref_or_err.takeError();
    auto &existential_tr = *type_ref_or_err;
    auto on_exit = PushDescriptorFinderAndPopOnExit(descriptor_finder);
    auto result = m_reflection_ctx.projectExistentialAndUnwrapClass(
        existential_address, existential_tr);
    if (!result)
      return llvm::createStringError(
          "failed to project existential and unwrap class");
    return *result;
  }

  llvm::Expected<const swift::reflection::TypeRef &>
  LookupTypeWitness(const std::string &MangledTypeName,
                    const std::string &Member, StringRef Protocol) override {
    if (auto *tr = m_type_converter.getBuilder().lookupTypeWitness(
            MangledTypeName, Member, Protocol))
      return *tr;
    return llvm::createStringError("could not lookup type witness");
  }
  swift::reflection::ConformanceCollectionResult GetAllConformances() override {
    swift::reflection::TypeRefBuilder &b = m_type_converter.getBuilder();
    if (ObjCEnabled)
      return b.collectAllConformances<swift::WithObjCInterop, PointerSize>();
    return b.collectAllConformances<swift::NoObjCInterop, PointerSize>();
  }

  llvm::Expected<const swift::reflection::TypeRef &>
  ReadTypeFromMetadata(lldb::addr_t metadata_address,
                       swift::reflection::DescriptorFinder *descriptor_finder,
                       bool skip_artificial_subclasses) override {
    auto on_exit = PushDescriptorFinderAndPopOnExit(descriptor_finder);
    if (auto *tr = m_reflection_ctx.readTypeFromMetadata(
            swift::remote::RemoteAddress(
                metadata_address,
                swift::remote::RemoteAddress::DefaultAddressSpace),
            skip_artificial_subclasses))
      return *tr;
    return llvm::createStringError("could not read type from metadata");
  }

  llvm::Expected<const swift::reflection::TypeRef &>
  ReadTypeFromInstance(lldb::addr_t instance_address,
                       swift::reflection::DescriptorFinder *descriptor_finder,
                       bool skip_artificial_subclasses) override {
    auto on_exit = PushDescriptorFinderAndPopOnExit(descriptor_finder);
    auto metadata_address =
        m_reflection_ctx.readMetadataFromInstance(swift::remote::RemoteAddress(
            instance_address,
            swift::remote::RemoteAddress::DefaultAddressSpace));
    if (!metadata_address)
      return llvm::createStringError(
          llvm::formatv("could not read heap metadata for object at {0:x}",
                        instance_address));

    if (auto *tr = m_reflection_ctx.readTypeFromMetadata(
            *metadata_address, skip_artificial_subclasses))
      return *tr;
    return llvm::createStringError("could not read type from metadata");
  }

  std::optional<swift::remote::RemoteAbsolutePointer>
  ReadPointer(lldb::addr_t instance_address) override {
    auto ptr = m_reflection_ctx.readPointer(swift::remote::RemoteAddress(
        instance_address, swift::remote::RemoteAddress::DefaultAddressSpace));
    return ptr;
  }

  std::optional<swift::remote::RemoteExistential>
  ReadMetadataAndValueOpaqueExistential(
      lldb::addr_t existential_address) override {
    return m_reflection_ctx.readMetadataAndValueOpaqueExistential(
        swift::remote::RemoteAddress(
            existential_address,
            swift::remote::RemoteAddress::DefaultAddressSpace));
  }

  std::optional<bool> IsValueInlinedInExistentialContainer(
      swift::remote::RemoteAddress existential_address) override {
    return m_reflection_ctx.isValueInlinedInExistentialContainer(
        existential_address);
  }

  llvm::Expected<const swift::reflection::TypeRef &> ApplySubstitutions(
      const swift::reflection::TypeRef &type_ref,
      swift::reflection::GenericArgumentMap substitutions,
      swift::reflection::DescriptorFinder *descriptor_finder) override {
    auto on_exit = PushDescriptorFinderAndPopOnExit(descriptor_finder);
    if (auto *tr = type_ref.subst(m_reflection_ctx.getBuilder(), substitutions))
      return *tr;
    return llvm::createStringError("failed to apply substitutions");
  }

  swift::remote::RemoteAbsolutePointer
  StripSignedPointer(swift::remote::RemoteAbsolutePointer pointer) override {
    return m_reflection_ctx.stripSignedPointer(pointer);
  }

  llvm::Expected<AsyncTaskInfo>
  asyncTaskInfo(lldb::addr_t AsyncTaskPtr, unsigned ChildTaskLimit,
                unsigned AsyncBacktraceLimit) override {
    auto [error, task_info] = m_reflection_ctx.asyncTaskInfo(
        swift::remote::RemoteAddress(
            AsyncTaskPtr, swift::remote::RemoteAddress::DefaultAddressSpace),
        ChildTaskLimit, AsyncBacktraceLimit);
    if (error)
      return llvm::createStringError(*error);

    AsyncTaskInfo result;
    result.taskAddr = AsyncTaskPtr;
    result.isChildTask = task_info.IsChildTask;
    result.isFuture = task_info.IsFuture;
    result.isGroupChildTask = task_info.IsGroupChildTask;
    result.isAsyncLetTask = task_info.IsAsyncLetTask;
    result.isCancelled = task_info.IsCancelled;
    result.isStatusRecordLocked = task_info.IsStatusRecordLocked;
    result.isEscalated = task_info.IsEscalated;
    result.hasIsRunning = task_info.HasIsRunning;
    result.isRunning = task_info.IsRunning;
    result.isEnqueued = task_info.IsEnqueued;
    result.isComplete = task_info.IsComplete;
    result.isSuspended = task_info.IsSuspended;
    result.id = task_info.Id;
    result.kind = task_info.Kind;
    result.enqueuePriority = task_info.EnqueuePriority;
    result.resumeAsyncContext = task_info.ResumeAsyncContext;
    result.runJob = task_info.RunJob;
    result.parentTask = task_info.ParentTask;
    for (auto child : task_info.ChildTasks)
      result.childTasks.push_back(child);
    for (auto waiting : task_info.WaitingTasks)
      result.waitingTasks.push_back(waiting);
    for (auto pc : task_info.AsyncBacktraceFrames)
      result.asyncBacktracePcs.push_back(pc);
    return result;
  }

private:
  /// Return a description of the layout of the record (classes, structs and
  /// tuples) type given its typeref.
  llvm::Expected<const swift::reflection::RecordTypeInfo &>
  GetRecordTypeInfo(const swift::reflection::TypeRef &type_ref,
                    swift::remote::TypeInfoProvider *tip,
                    swift::reflection::DescriptorFinder *descriptor_finder,
                    swift::Mangle::ManglingFlavor flavor) {
    auto type_info_or_err =
        GetTypeInfoFromTypeRef(type_ref, tip, descriptor_finder);
    if (!type_info_or_err)
      return type_info_or_err.takeError();
    auto *type_info = &*type_info_or_err;
    if (auto record_type_info =
            llvm::dyn_cast_or_null<swift::reflection::RecordTypeInfo>(
                type_info))
      return *record_type_info;
    if (llvm::isa_and_nonnull<swift::reflection::ReferenceTypeInfo>(type_info))
      return GetClassInstanceTypeInfo(type_ref, tip, descriptor_finder, flavor);
    std::stringstream ss;
    type_ref.dump(ss);
    return llvm::createStringError(
        "Could not get record type info for typeref: " + ss.str());
  }

  llvm::Expected<const swift::reflection::TypeInfo &> GetTypeInfoFromTypeRef(
      const swift::reflection::TypeRef &type_ref,
      swift::remote::TypeInfoProvider *provider,
      swift::reflection::DescriptorFinder *descriptor_finder) {
    auto on_exit = PushDescriptorFinderAndPopOnExit(descriptor_finder);

    Log *log(GetLog(LLDBLog::Types));
    if (log && log->GetVerbose()) {
      std::stringstream ss;
      type_ref.dump(ss);
      LLDB_LOG(log,
               "[TargetReflectionContext[{0:x}]::getTypeInfo] Getting type "
               "info for typeref {1}",
               provider ? provider->getId() : 0, ss.str());
    }

    auto type_info_or_err = m_reflection_ctx.getTypeInfo(type_ref, provider);
    if (!type_info_or_err)
      return llvm::joinErrors(
          llvm::createStringError(
              "Could not find reflection metadata for type"),
          type_info_or_err.takeError());

    if (log && log->GetVerbose()) {
      std::stringstream ss;
      type_info_or_err->dump(ss);
      LLDB_LOG(log,
               "[TargetReflectionContext::getTypeInfo] Found type info {0}",
               ss.str());
    }
    return *type_info_or_err;
  }

  /// Compute a class-instance RecordTypeInfo on a shadow by re-deriving the
  /// TypeRef in that shadow's own builder from the mangled name.
  llvm::Expected<const swift::reflection::RecordTypeInfo &>
  ClassInstanceOnShadow(TargetReflectionContext &shadow, llvm::StringRef mangled,
                        swift::remote::TypeInfoProvider *provider,
                        swift::reflection::DescriptorFinder *descriptor_finder,
                        swift::Mangle::ManglingFlavor flavor) {
    auto tr_or_err = shadow.GetTypeRef(mangled);
    if (!tr_or_err)
      return tr_or_err.takeError();
    return shadow.GetClassInstanceTypeInfo(*tr_or_err, provider,
                                           descriptor_finder, flavor);
  }

  /// Differential check for GetTypeInfo(CompilerType). Each shadow re-derives
  /// its own TypeRef through the normal pipeline.
  void CompareShadowsForType(CompilerType type,
                             swift::remote::TypeInfoProvider *provider,
                             swift::reflection::DescriptorFinder *df) {
    llvm::StringRef name = type.GetMangledTypeName().GetStringRef();
    if (IsEmbeddedSwiftName(name))
      return;
    auto flags = DwarfValidationFlags();
    auto refl = m_reflection_only->GetTypeInfo(type, provider, df);
    auto dwarf = m_dwarf_only->GetTypeInfo(type, provider, df);
    ReportShadowComparison(name, refl, dwarf, flags, "GetTypeInfo",
                           provider ? (uint64_t)(uintptr_t)provider->getId()
                                    : 0);
  }

  /// Differential check for GetClassInstanceTypeInfo(TypeRef).
  void CompareShadowsForClassInstance(
      const swift::reflection::TypeRef &type_ref,
      swift::remote::TypeInfoProvider *provider,
      swift::reflection::DescriptorFinder *df,
      swift::Mangle::ManglingFlavor flavor) {
    swift::Demangle::Demangler dem;
    // Re-mangle with the flavor the caller's type actually came from. A
    // TypeRef stores nominal names without a mangling prefix, so mangling
    // defaults to $s and would silently rename every Embedded Swift type to
    // one that does not exist in the program.
    auto mangled = type_ref.mangle(dem, flavor);
    if (!mangled)
      return;
    if (IsEmbeddedSwiftName(*mangled))
      return;
    auto flags = DwarfValidationFlags();
    auto refl =
        ClassInstanceOnShadow(*m_reflection_only, *mangled, provider, df, flavor);
    auto dwarf =
        ClassInstanceOnShadow(*m_dwarf_only, *mangled, provider, df, flavor);
    ReportShadowComparison(*mangled, refl, dwarf, flags,
                           "GetClassInstanceTypeInfo",
                           provider ? (uint64_t)(uintptr_t)provider->getId()
                                    : 0);
  }
};
} // namespace

namespace lldb_private {
std::unique_ptr<ReflectionContextInterface>
ReflectionContextInterface::CreateReflectionContext(
    uint8_t ptr_size, std::shared_ptr<swift::remote::MemoryReader> reader,
    bool ObjCInterop, SwiftMetadataCache *swift_metadata_cache) {
  using ReflectionContext32ObjCInterop = TargetReflectionContext<
      swift::reflection::ReflectionContext<
          swift::External<swift::WithObjCInterop<swift::RuntimeTarget<4>>>>,
      true, 4>;
  using ReflectionContext32NoObjCInterop = TargetReflectionContext<
      swift::reflection::ReflectionContext<
          swift::External<swift::NoObjCInterop<swift::RuntimeTarget<4>>>>,
      false, 4>;
  using ReflectionContext64ObjCInterop = TargetReflectionContext<
      swift::reflection::ReflectionContext<
          swift::External<swift::WithObjCInterop<swift::RuntimeTarget<8>>>>,
      true, 8>;
  using ReflectionContext64NoObjCInterop = TargetReflectionContext<
      swift::reflection::ReflectionContext<
          swift::External<swift::NoObjCInterop<swift::RuntimeTarget<8>>>>,
      false, 8>;
  if (ptr_size == 4) {
    if (ObjCInterop)
      return std::make_unique<ReflectionContext32ObjCInterop>(
          reader, swift_metadata_cache);
    return std::make_unique<ReflectionContext32NoObjCInterop>(
        reader, swift_metadata_cache);
  }
  if (ptr_size == 8) {
    if (ObjCInterop)
      return std::make_unique<ReflectionContext64ObjCInterop>(
          reader, swift_metadata_cache);
    return std::make_unique<ReflectionContext64NoObjCInterop>(
        reader, swift_metadata_cache);
  }
  return {};
}

llvm::Expected<const swift::reflection::TypeRef &>
ReflectionContextInterface::GetCanonicalTypeRef(CompilerType type) {
  auto tss = type.GetTypeSystem().dyn_cast_or_null<TypeSystemSwift>();
  if (!tss)
    return llvm::createStringError("not a Swift type");
  auto tr_ts = tss->GetTypeSystemSwiftTypeRef();
  if (!tr_ts)
    return llvm::createStringError("no TypeSystemSwiftTypeRef");

  ConstString mangled_name = type.GetMangledTypeName();
  swift::Demangle::Demangler dem;
  swift::Demangle::NodePointer node =
      tr_ts->GetCanonicalDemangleTree(dem, mangled_name);
  if (!node)
    return llvm::createStringError("could not canonically demangle type");

  auto flavor =
      SwiftLanguageRuntime::GetManglingFlavor(mangled_name.GetStringRef());
  ExecutionContext exe_ctx;
  if (auto *expr_ts =
          llvm::dyn_cast<TypeSystemSwiftTypeRefForExpressions>(tr_ts.get()))
    exe_ctx =
        expr_ts->GetExecutionContextForType(type.GetOpaqueQualType()).Lock(false);
  auto node_or_err = tr_ts->RemoveMarkerProtocols(dem, node, flavor, exe_ctx);
  if (!node_or_err)
    return node_or_err.takeError();
  return GetTypeRef(dem, *node_or_err);
}

} // namespace lldb_private
