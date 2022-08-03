#include "TypeRefCacher.h"


using namespace lldb_private;
using namespace swift::reflection;
using namespace swift::remote;

void TypeRefCacher::registerFieldDescriptors(
    uint64_t InfoID, const ReflectionInfo &Info,
    const std::vector<std::string> &Names) {
  for (auto tuple : llvm::zip(Info.Field, Names)) {
    auto &field_descriptor = std::get<0>(tuple);
    auto &mangled_name = std::get<1>(tuple);
    m_map[mangled_name] = {InfoID,
                         field_descriptor.getAddressData() -
                             Info.Field.startAddress().getAddressData()};
  }
}

llvm::Optional<std::pair<uint64_t, uint64_t>> 
TypeRefCacher::getFieldDescriptor(const std::string &Name) {
  auto it = m_map.find(Name);
  if (it != m_map.end())
    return it->second;
  return {};
}
