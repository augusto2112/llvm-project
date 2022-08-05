#include "TypeRefCacher.h"

#include "llvm/Support/Compression.h"
#include "lldb/Core/DataFileCache.h"
#include "lldb/Utility/DataEncoder.h"


using namespace lldb;
using namespace lldb_private;
using namespace swift::reflection;
using namespace swift::remote;


struct CachedPair {
  char *mangled_name;
  uint64_t offset;
};

void TypeRefCacher::registerModuleWithReflectionInfoID(ModuleSP module,
                                                       uint64_t InfoID) {
  m_info_to_module[InfoID] = module;
  loadCacheForModule(module, InfoID);
}

void TypeRefCacher::loadCacheForModule(ModuleSP module, uint64_t InfoID) {
  auto *index_cache = Module::GetIndexCache();
  if (!index_cache)
    return;

  auto UUID = module->GetUUID().GetAsString();
  auto mem_buffer_up = index_cache->GetCachedData(UUID);;
  // Nothing cached.
  if (!mem_buffer_up)
    return;
  DataExtractor data(mem_buffer_up->getBufferStart(),
                     mem_buffer_up->getBufferSize(),
                     module->GetObjectFile()->GetByteOrder(),
                     module->GetObjectFile()->GetAddressByteSize());
  lldb::offset_t read_offset = 0;
  auto num_entries = data.GetU64(&read_offset);
  for (size_t i = 0; i < num_entries; i++) {
    const auto *mangled_name = data.GetCStr(&read_offset);
    if (!mangled_name)
      return;
    uint64_t offset = 0; 
    if (!data.GetU64(&read_offset, &offset, 1))
      return;
    m_map[mangled_name] = {InfoID, offset}; ;

  }
}

void TypeRefCacher::registerFieldDescriptors(
    uint64_t InfoID, const ReflectionInfo &Info,
    const std::vector<std::string> &Names) {
  /* assert(Info.Field.size() == Names.size() && */
  /*        "Expected same number of field descriptors and mangled names!"); */
  auto *index_cache = Module::GetIndexCache();
  if (!index_cache)
    return;
  const auto byte_order = endian::InlHostByteOrder();
  DataEncoder encoder(byte_order, 8);
  encoder.AppendU64(Names.size());
  for (auto tuple : llvm::zip(Info.Field, Names)) {
    auto &field_descriptor = std::get<0>(tuple);
    auto &mangled_name = std::get<1>(tuple);
    encoder.AppendCString(mangled_name.data());
    encoder.AppendU64(field_descriptor.getAddressData() -
                      Info.Field.startAddress().getAddressData());
  }
  auto module = m_info_to_module[InfoID];
  auto UUID = module->GetUUID().GetAsString();
  llvm::SmallString<1> Compressed;

  llvm::StringRef data((const char *)encoder.GetData().data(), encoder.GetData().size());
  auto error = llvm::zlib::compress(data, Compressed);
  if (error) {
    assert(false);
    llvm::consumeError(std::move(error));
  }
  llvm::ArrayRef<uint8_t> compressed((uint8_t *)Compressed.data(), Compressed.size());
  index_cache->SetCachedData(UUID, compressed); 
}

llvm::Optional<std::pair<uint64_t, uint64_t>> 
TypeRefCacher::getFieldDescriptor(const std::string &Name) const {
  auto it = m_map.find(Name);
  if (it != m_map.end())
    return it->second;
  return {};
}
