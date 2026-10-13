#ifndef slic3r_SupportCacheBudget_hpp_
#define slic3r_SupportCacheBudget_hpp_

#include <algorithm>
#include <cstddef>

namespace Slic3r {

struct SupportCacheBudget {
    size_t reserve{0};
    size_t headroom{0};
    size_t cache_limit{0};
};

struct SupportMemorySnapshot {
    size_t physical{0};
    size_t available{0};
};

SupportMemorySnapshot support_memory_snapshot();

class SupportBatchBudget {
public:
    SupportBatchBudget(size_t requested, SupportMemorySnapshot memory);
    ~SupportBatchBudget();
    SupportBatchBudget(const SupportBatchBudget &) = delete;
    SupportBatchBudget &operator=(const SupportBatchBudget &) = delete;
    size_t bytes() const { return m_bytes; }
private:
    size_t m_bytes{0};
};

// This limits additional reusable geometry, not the model's working storage.
// Use live availability after allocating the model and share cache_limit across
// concurrent objects. A failed memory query disables optional caching.
inline SupportCacheBudget support_cache_budget(size_t physical, size_t available)
{
    constexpr size_t mib = size_t(1) << 20;
    constexpr size_t gib = size_t(1) << 30;
    SupportCacheBudget result;
    if (physical == 0)
        return result;
    available = std::min(available, physical);
    result.reserve = std::max(512 * mib, physical / 5);
    result.headroom = std::max(256 * mib, physical / 32);
    const size_t protected_bytes = result.reserve + result.headroom;
    if (physical <= 4 * gib || available <= protected_bytes)
        return result;
    result.cache_limit = std::min({physical / 4, available / 3, available - protected_bytes});
    if (result.cache_limit < 64 * mib)
        result.cache_limit = 0;
    return result;
}

} // namespace Slic3r

#endif
