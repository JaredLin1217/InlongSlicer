#include "SupportCacheBudget.hpp"
#include <atomic>

#ifdef _WIN32
#include <Windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#endif

namespace Slic3r {

namespace { std::atomic<size_t> batch_reserved{0}; }

SupportBatchBudget::SupportBatchBudget(size_t requested, SupportMemorySnapshot memory)
{
    size_t reserved = batch_reserved.load();
    do {
        // Every active tree draws from one allowance based on live availability.
        const size_t limit = support_cache_budget(memory.physical, memory.available).cache_limit;
        m_bytes = reserved >= limit ? 0 : std::min(requested, limit - reserved);
        if (m_bytes == 0)
            return;
    } while (!batch_reserved.compare_exchange_weak(reserved, reserved + m_bytes));
}

SupportBatchBudget::~SupportBatchBudget() { batch_reserved.fetch_sub(m_bytes); }

SupportMemorySnapshot support_memory_snapshot()
{
#ifdef _WIN32
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    return GlobalMemoryStatusEx(&memory) ? SupportMemorySnapshot{size_t(memory.ullTotalPhys), size_t(memory.ullAvailPhys)} : SupportMemorySnapshot{};
#elif defined(__APPLE__)
    uint64_t physical = 0;
    size_t physical_size = sizeof(physical);
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page_size = 0;
    const mach_port_t host = mach_host_self();
    const bool ok = sysctlbyname("hw.memsize", &physical, &physical_size, nullptr, 0) == 0 &&
                    host_page_size(host, &page_size) == KERN_SUCCESS &&
                    host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS;
    mach_port_deallocate(mach_task_self(), host);
    return ok ? SupportMemorySnapshot{size_t(physical), size_t(vm.free_count + vm.inactive_count) * size_t(page_size)} : SupportMemorySnapshot{};
#elif defined(__linux__)
    struct sysinfo memory{};
    return sysinfo(&memory) == 0 ? SupportMemorySnapshot{size_t(memory.totalram) * memory.mem_unit,
        (size_t(memory.freeram) + size_t(memory.bufferram)) * memory.mem_unit} : SupportMemorySnapshot{};
#else
    return {};
#endif
}

} // namespace Slic3r
