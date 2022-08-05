
#ifndef liblldb_TypeRefCacher_h_
#define liblldb_TypeRefCacher_h_

#include <iterator>
#include <unordered_map>

#include "lldb/Core/Module.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"

#include "swift/Reflection/ReflectionContext.h"


namespace lldb_private {

template <typename T>
class TernarySearchTree {

  struct Node {
    char c;
    uint32_t left;
    uint32_t middle;
    uint32_t right;
    T value;
    Node(char c, uint32_t left, uint32_t middle, uint32_t right, T value)
        : c(c), left(left), middle(middle), right(right), value(value) {}
    Node(char c, T value)
        : c(c), left(UINT32_MAX), middle(UINT32_MAX), right(UINT32_MAX),
          value(value) {}
  };

public:
  TernarySearchTree(T null_value) : storage(), null_value(null_value) {}

  TernarySearchTree(T null_value, const char *data_start, const char *data_end)
      : storage((const Node *)data_start, (const Node *)data_end),
        null_value(null_value) {}

  void insert(llvm::StringRef key, T value) {
    if (key.empty())
      return;

    if (storage.empty()) {
      storage.push_back({key[0], null_value});
    }

    insert(key, value, 0, 0);
  }


  llvm::Optional<T> get(llvm::StringRef key) {
    if (storage.empty())
      return null_value;
    return get(key, 0);
  }

  llvm::ArrayRef<uint8_t> data() {
    llvm::ArrayRef<uint8_t> data((uint8_t *)storage.data(),
                                 (uint8_t *)(storage.data() + storage.size()));
    return data;
  }

  template <typename U>
  TernarySearchTree<U> map(std::function<U(T)> fn) {

  }


private:
  uint32_t insert(llvm::StringRef key, T value, uint32_t parent_index,
                  uint32_t current_offset) {
    if (key.empty())
      return UINT32_MAX;

    char c = key[0];
    uint32_t current_index = parent_index + current_offset;
    if (current_offset == UINT32_MAX) {
      storage.push_back({c, null_value});
      current_index = storage.size() - 1;
      current_offset = current_index - parent_index;
    }

    if (c < storage[current_index].c)
      storage[current_index].left = insert(key, value, current_index, storage[current_index].left);
    else if (c > storage[current_index].c)
      storage[current_index].right = insert(key, value, current_index, storage[current_index].right);
    else if (key.size() > 1)
      storage[current_index].middle =
          insert(key.drop_front(), value, current_index, storage[current_index].middle);
    else storage[current_index].value = value;
    return current_offset;
  }

  T get(llvm::StringRef key, uint32_t current_index) {
    if (current_index == UINT32_MAX)
      return null_value;
    if (storage.size() <= current_index)
      return null_value;

    char c = key[0];
    auto &current = storage[current_index];
    if (c < current.c)
      return get(key, std::max(current.left, current.left + current_index));
    if (c > current.c)
      return get(key, std::max(current.right, current.right + current_index));
    if (key.size() > 1)
      return get(key.drop_front(), std::max(current.middle, current.middle + current_index));
    return current.value;
  }

  std::vector<Node> storage;
  T null_value;
};


struct TypeRefCacher {
public:
  TypeRefCacher() : m_map(null_pair), m_info_to_module() {}

  void registerModuleWithReflectionInfoID(lldb::ModuleSP module,
                                          uint64_t InfoID);

  void registerFieldDescriptors(uint64_t InfoID,
                                const swift::reflection::ReflectionInfo &Info,
                                const std::vector<std::string> &Names);

  llvm::Optional<std::pair<uint64_t, uint64_t>> 
    getFieldDescriptor(const std::string &Name);

private:
  void loadCacheForModule(lldb::ModuleSP module, uint64_t InfoID);

  const std::pair<uint64_t, uint64_t> null_pair =
      std::make_pair(UINT64_MAX, UINT64_MAX);
  TernarySearchTree<std::pair<uint64_t, uint64_t>> m_map;
  std::unordered_map<uint64_t, lldb::ModuleSP> m_info_to_module;
};
} // namespace lldb_private
#endif
