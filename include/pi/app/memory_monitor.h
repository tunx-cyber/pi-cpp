#pragma once

#include <cstdint>

#include <string>
#include <string_view>

namespace pi
{

/** 当前 picpp 进程的驻留内存快照。不可用时 hasResident=false。 */
struct ProcessMemoryUsage
{
    uint64_t residentBytes = 0;
    bool hasResident = false;
};

/**
 * 采样当前进程实际使用的内存（只取驻留内存）。
 * Linux 使用 /proc/self/status 的 VmRSS；macOS 使用 task_vm_info 的 phys_footprint
 * （与「活动监视器」口径一致，含压缩页）。
 * 不支持的平台或系统调用失败时返回标记为不可用的快照，不抛出异常。
 */
ProcessMemoryUsage sample_process_memory();

/** 解析 Linux /proc/<pid>/status 内容，供采样实现和测试使用。 */
ProcessMemoryUsage parse_proc_status_memory(std::string_view status);

/** 将字节数格式化为稳定、适合终端展示的二进制单位。 */
std::string format_memory_bytes(uint64_t bytes);

/** 将内存快照格式化为驻留内存数值（如 "128 MiB"），不可用时返回 "unavailable"。 */
std::string format_process_memory(const ProcessMemoryUsage& usage);

}  // namespace pi
