#pragma once

// [MemCensus][rawengine] 临时内存定位探针（仅 iOS 真机生效，其它平台空实现）。定位完整体移除本文件与所有调用。
// 与 PhotoEditor util/mem_census.h 同口径：footprint 及按账本拆分（internal 匿名 / compressed / gfx / neural），
// 用于确认 RAW 整解各阶段的内存峰值、以及 rte_decode 返回前是否已释放（RawImageSource 是否析构）。
// 走 stdout（与 img_log 默认回调同一路径，确认能进 App 日志）。

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE
#include <cstdio>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <os/proc.h>

inline void rteMemCensus(const char* tag, int w = 0, int h = 0)
{
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return;
    const struct mstats s = mstats();
    const double mb = 1024.0 * 1024.0;
    const bool has_ledgers = count >= TASK_VM_INFO_REV6_COUNT;
    std::printf("[MemCensus][rawengine] tag=%s size=%dx%d footprintMB=%.1f availMB=%.1f residentMB=%.1f internalMB=%.1f "
                "compressedMB=%.1f gfxMB=%.1f neuralMB=%.1f heapInUseMB=%.1f heapFreeMB=%.1f\n",
                tag, w, h, info.phys_footprint / mb, os_proc_available_memory() / mb, info.resident_size / mb,
                info.internal / mb, info.compressed / mb,
                has_ledgers ? info.ledger_tag_graphics_footprint / mb : -1.0,
                has_ledgers ? info.ledger_tag_neural_footprint / mb : -1.0,
                s.bytes_used / mb, (s.bytes_total - s.bytes_used) / mb);
    std::fflush(stdout);
}
#else
inline void rteMemCensus(const char*, int = 0, int = 0) {}
#endif
