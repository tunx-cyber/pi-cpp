#include "pi/app/memory_monitor.h"

#include <gtest/gtest.h>

namespace pi
{
namespace
{

TEST(MemoryMonitorTest, ParsesLinuxProcessStatus)
{
    const auto usage = parse_proc_status_memory(
        "Name:\tpicpp\nVmSize:\t  24576 kB\nVmRSS:\t   4096 kB\nThreads:\t4\n");

    EXPECT_TRUE(usage.hasResident);
    EXPECT_TRUE(usage.hasVirtual);
    EXPECT_EQ(usage.residentBytes, 4096U * 1024U);
    EXPECT_EQ(usage.virtualBytes, 24576U * 1024U);
}

TEST(MemoryMonitorTest, IgnoresInvalidAndOverflowingStatusValues)
{
    const auto usage = parse_proc_status_memory(
        "VmRSS:\tinvalid kB\nVmSize:\t18446744073709551615 kB\n");

    EXPECT_FALSE(usage.hasResident);
    EXPECT_FALSE(usage.hasVirtual);
}

TEST(MemoryMonitorTest, FormatsMemoryForTerminal)
{
    EXPECT_EQ(format_memory_bytes(0), "0 B");
    EXPECT_EQ(format_memory_bytes(1536), "1.5 KiB");
    EXPECT_EQ(format_memory_bytes(128ULL * 1024ULL * 1024ULL), "128 MiB");
    EXPECT_EQ(format_process_memory({2ULL * 1024ULL * 1024ULL, 0, true, false}), "rss=2.0 MiB");
}

TEST(MemoryMonitorTest, SamplesTheRunningProcessWhenSupported)
{
    const auto usage = sample_process_memory();
#if defined(__linux__) || defined(__APPLE__)
    EXPECT_TRUE(usage.hasResident);
    EXPECT_GT(usage.residentBytes, 0U);
#else
    EXPECT_FALSE(usage.hasResident);
#endif
}

}  // namespace
}  // namespace pi
