#include "pi/app/memory_monitor.h"

#include <charconv>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace pi
{

namespace
{

bool parse_kib_value(std::string_view line, std::string_view key, uint64_t* bytes)
{
    if (line.substr(0, key.size()) != key) return false;

    const size_t value_start = line.find_first_not_of(" \t", key.size());
    if (value_start == std::string_view::npos) return false;
    const size_t value_end = line.find_first_of(" \t", value_start);
    const std::string_view value = line.substr(value_start, value_end - value_start);

    uint64_t kib = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), kib);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        kib > std::numeric_limits<uint64_t>::max() / 1024)
    {
        return false;
    }

    *bytes = kib * 1024;
    return true;
}

}  // namespace

ProcessMemoryUsage parse_proc_status_memory(std::string_view status)
{
    ProcessMemoryUsage usage;
    size_t start = 0;
    while (start < status.size())
    {
        const size_t end = status.find('\n', start);
        const std::string_view line = status.substr(start, end - start);
        uint64_t bytes = 0;
        if (!usage.hasResident && parse_kib_value(line, "VmRSS:", &bytes))
        {
            usage.residentBytes = bytes;
            usage.hasResident = true;
        }
        else if (!usage.hasVirtual && parse_kib_value(line, "VmSize:", &bytes))
        {
            usage.virtualBytes = bytes;
            usage.hasVirtual = true;
        }

        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return usage;
}

ProcessMemoryUsage sample_process_memory()
{
#if defined(__linux__)
    std::ifstream file("/proc/self/status");
    if (!file) return {};
    std::ostringstream content;
    content << file.rdbuf();
    return parse_proc_status_memory(content.str());
#elif defined(__APPLE__)
    // macOS 使用 task_vm_info 的 phys_footprint（与「活动监视器」的内存口径一致，
    // 包含压缩页；resident_size 只计未压缩的物理页）。
    // virtual_size 含系统级稀疏映射（Apple Silicon 的 jumbo region 可达数百 GiB），
    // 数值巨大但不代表实际占用，仅供对比。
    task_vm_info_data_t vm_info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm_info),
                  &count) != KERN_SUCCESS)
    {
        return {};
    }
    ProcessMemoryUsage usage;
    usage.residentBytes = static_cast<uint64_t>(vm_info.phys_footprint);
    usage.virtualBytes = static_cast<uint64_t>(vm_info.virtual_size);
    usage.hasResident = true;
    usage.hasVirtual = true;
    return usage;
#else
    return {};
#endif
}

std::string format_memory_bytes(uint64_t bytes)
{
    static constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(kUnits) / sizeof(kUnits[0]))
    {
        value /= 1024.0;
        ++unit;
    }

    std::ostringstream out;
    if (unit == 0)
    {
        out << bytes;
    }
    else
    {
        out << std::fixed << std::setprecision(value >= 100.0 ? 0 : 1) << value;
    }
    out << ' ' << kUnits[unit];
    return out.str();
}

std::string format_process_memory(const ProcessMemoryUsage& usage)
{
    if (!usage.hasResident && !usage.hasVirtual) return "unavailable";

    std::string result;
    if (usage.hasResident) result += "rss=" + format_memory_bytes(usage.residentBytes);
    if (usage.hasVirtual)
    {
        if (!result.empty()) result += " ";
        result += "vms=" + format_memory_bytes(usage.virtualBytes);
    }
    return result;
}

}  // namespace pi
