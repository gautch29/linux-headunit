// Author: gautch29
// Notice date: 2026-11-23
// Provided as is, without warranty. See NOTICE.md.

#include <headunit/theme/script_runtime.hpp>
#include <map>
#include <stdexcept>

namespace headunit::theme::script {
ResourceReader cache_resources(ResourceReader source, std::size_t max_bytes,
                               std::size_t max_entries) {
  if (!source || max_bytes > 64 * 1024 * 1024 || max_entries > 4096)
    throw std::invalid_argument("invalid asset resource cache policy");
  struct Cache {
    ResourceReader source;
    std::map<std::string, std::string, std::less<>> entries;
    std::size_t used = 0;
  };
  auto cache = std::make_shared<Cache>();
  cache->source = std::move(source);
  return [cache, max_bytes, max_entries](std::string_view path) {
    if (path.size() > 1024) throw std::invalid_argument("asset resource path exceeds limit");
    if (const auto found = cache->entries.find(path); found != cache->entries.end())
      return found->second;
    auto bytes = cache->source(path);
    if (cache->entries.size() < max_entries && bytes.size() <= max_bytes - cache->used &&
        path.size() <= max_bytes - cache->used - bytes.size()) {
      cache->used += path.size() + bytes.size();
      cache->entries.emplace(path, bytes);
    }
    return bytes;
  };
}
}  // namespace headunit::theme::script
