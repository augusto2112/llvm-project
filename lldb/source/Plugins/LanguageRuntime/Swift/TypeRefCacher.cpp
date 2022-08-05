#include "TypeRefCacher.h"

#include "lldb/Core/DataFileCache.h"
#include "lldb/Utility/DataEncoder.h"

#include "llvm/Support/Compression.h"

using namespace lldb;
using namespace lldb_private;
using namespace swift::reflection;
using namespace swift::remote;

void TypeRefCacher::registerModuleWithReflectionInfoID(ModuleSP module,
                                                       uint64_t InfoID) {
  m_info_to_module[InfoID] = module;
  loadCacheForModule(module, InfoID);
}

void TypeRefCacher::loadCacheForModule(ModuleSP module, uint64_t InfoID) {
  auto *index_cache = Module::GetIndexCache();
  if (!index_cache)
    return;;

  auto UUID = module->GetUUID().GetAsString();
  auto mem_buffer_up = index_cache->GetCachedData(UUID);;
  // Nothing cached.
  if (!mem_buffer_up)
    return;

  TernarySearchTree<uint32_t> tree(UINT32_MAX, mem_buffer_up->getBufferStart(), mem_buffer_up->getBufferEnd());

}

void TypeRefCacher::registerFieldDescriptors(
    uint64_t InfoID, const ReflectionInfo &Info,
    const std::vector<std::string> &Names) {
  /* assert(Info.Field.size() == Names.size() && */
  /*        "Expected same number of field descriptors and mangled names!"); */
  auto *index_cache = Module::GetIndexCache();
  if (!index_cache)
    return;

  TernarySearchTree<uint32_t> tree(UINT32_MAX);

  for (auto tuple : llvm::zip(Info.Field, Names)) {
    auto &field_descriptor = std::get<0>(tuple);
    auto &mangled_name = std::get<1>(tuple);
    tree.insert(mangled_name, field_descriptor.getAddressData() -
                                  Info.Field.startAddress().getAddressData());
  }

  /* const auto byte_order = endian::InlHostByteOrder(); */
  /* DataEncoder encoder(byte_order, 8); */
  /* encoder.AppendU64(Names.size()); */
  /* for (auto tuple : llvm::zip(Info.Field, Names)) { */
  /*   auto &field_descriptor = std::get<0>(tuple); */
  /*   auto &mangled_name = std::get<1>(tuple); */
  /*   encoder.AppendCString(mangled_name.data()); */
  /*   encoder.AppendU64(field_descriptor.getAddressData() - */
  /*                     Info.Field.startAddress().getAddressData()); */
  /* } */
  auto module = m_info_to_module[InfoID];
  auto UUID = module->GetUUID().GetAsString();
  index_cache->SetCachedData(UUID, tree.data());
}

llvm::Optional<std::pair<uint64_t, uint64_t>> 
TypeRefCacher::getFieldDescriptor(const std::string &Name) {
  auto pair = m_map.get(Name);
  if (pair == null_pair)
    return {};
  return pair;
}
