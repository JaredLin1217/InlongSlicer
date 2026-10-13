#include <catch2/catch_all.hpp>
#include "libslic3r/Support/SupportCacheBudget.hpp"

using Slic3r::support_cache_budget;

namespace {
constexpr size_t gib = size_t(1) << 30;
}

TEST_CASE("Support caches leave working memory available across hardware capacities", "[SupportCacheBudget]")
{
    const size_t capacity = GENERATE(2, 4, 8, 16, 32, 64, 128) * gib;
    const size_t available = capacity / 2;
    const auto budget = support_cache_budget(capacity, available);
    REQUIRE(budget.cache_limit <= capacity / 4);
    REQUIRE(budget.cache_limit <= available / 3);
    REQUIRE(budget.cache_limit + budget.reserve + budget.headroom <= available);
    if (capacity <= 4 * gib)
        REQUIRE(budget.cache_limit == 0);
    else
        REQUIRE(budget.cache_limit > 0);
}

TEST_CASE("Support caches stop committing memory under pressure or an unknown memory reading", "[SupportCacheBudget]")
{
    const size_t capacity = GENERATE(2, 4, 8, 16, 32, 64, 128) * gib;
    const auto idle = support_cache_budget(capacity, capacity);
    REQUIRE(support_cache_budget(capacity, idle.reserve + idle.headroom).cache_limit == 0);
    REQUIRE(support_cache_budget(capacity, 0).cache_limit == 0);
    REQUIRE(support_cache_budget(0, capacity).cache_limit == 0);
}

TEST_CASE("Support caches adapt their allowance to current availability", "[SupportCacheBudget]")
{
    const size_t capacity = GENERATE(8, 16, 32, 64, 128) * gib;
    const auto busy = support_cache_budget(capacity, capacity / 2);
    const auto idle = support_cache_budget(capacity, capacity);
    REQUIRE(idle.cache_limit > busy.cache_limit);
    const auto invalid_available = support_cache_budget(capacity, capacity * 2);
    REQUIRE(invalid_available.cache_limit == idle.cache_limit);
}

TEST_CASE("Concurrent support batches share and release one memory allowance", "[SupportCacheBudget]")
{
    const size_t capacity = GENERATE(8, 16, 32, 64, 128) * gib;
    const Slic3r::SupportMemorySnapshot memory{capacity, capacity / 2};
    const size_t limit = support_cache_budget(memory.physical, memory.available).cache_limit;
    {
        Slic3r::SupportBatchBudget first(limit * 3 / 4, memory);
        Slic3r::SupportBatchBudget second(limit, memory);
        REQUIRE(first.bytes() + second.bytes() == limit);
        Slic3r::SupportBatchBudget blocked(limit, memory);
        REQUIRE(blocked.bytes() == 0);
    }
    Slic3r::SupportBatchBudget reclaimed(limit, memory);
    REQUIRE(reclaimed.bytes() == limit);
}
