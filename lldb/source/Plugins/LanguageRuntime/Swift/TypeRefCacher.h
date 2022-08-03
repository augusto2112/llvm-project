
#ifndef liblldb_TypeRefCacher_h_
#define liblldb_TypeRefCacher_h_

#include <unordered_map>

#include "lldb/Core/Module.h"

#include "llvm/ADT/STLExtras.h"

#include "swift/Reflection/ReflectionContext.h"


namespace lldb_private {
struct TypeRefCacher {
public:
  void registerFieldDescriptors(uint64_t InfoID,
                                const swift::reflection::ReflectionInfo &Info,
                                const std::vector<std::string> &Names);

  llvm::Optional<std::pair<uint64_t, uint64_t>> 
    getFieldDescriptor(const std::string &Name);

private:
  std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> m_map;
};
} // namespace lldb_private
#endif
