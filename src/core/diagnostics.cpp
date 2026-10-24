// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/diagnostics.h"
#include "core/linker.h"
#include "core/memory.h"

#ifdef _WIN32
#include <dbghelp.h>
#include <tlhelp32.h>
#include <windows.h>
#endif

namespace Core::Diagnostics {

namespace {

std::atomic<u64> g_flip_count{0};
std::jthread g_watchdog;
std::atomic<bool> g_frame_trace_pending{false};
std::atomic<bool> g_frame_trace_active{false};
std::atomic<u32> g_frame_trace_index{0};
std::array<std::atomic<u64>, static_cast<size_t>(Counter::Count)> g_counters{};
struct FaultSite {
    u64 count;
    u64 last_fault;
};
std::atomic<bool> g_fault_sites_active{false};
std::mutex g_fault_sites_mutex;
std::unordered_map<u64, FaultSite> g_fault_sites;

struct GuestStack {
    u64 size;
    std::string name;
};
std::shared_mutex g_guest_stacks_mutex;
std::map<u64, GuestStack> g_guest_stacks;

#ifdef _WIN32

constexpr u32 MaxBacktraceFrames = 24;
constexpr u64 StackScanBytes = 0x2000;
constexpr u32 MaxMinidumps = 6;

bool ReadU64(u64 address, u64& out) {
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), &out,
                             sizeof(out), &read) &&
           read == sizeof(out);
}

const Module* FindGuestModule(u64 address) {
    return Common::Singleton<Core::Linker>::Instance()->FindByAddress(address);
}

bool IsExecutable(u64 address) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info))) {
        return false;
    }
    return info.State == MEM_COMMIT &&
           (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                            PAGE_EXECUTE_WRITECOPY)) != 0;
}

std::string HostModuleName(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length == 0) {
        return "?";
    }
    return std::filesystem::path(std::wstring(path, length)).filename().string();
}

std::string ThreadName(HANDLE thread) {
    PWSTR description = nullptr;
    std::string name;
    if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
        name = std::filesystem::path(description).string();
        LocalFree(description);
    }
    return name;
}

std::string ScanStackForGuestFrames(u64 rsp) {
    std::string out;
    u32 found = 0;
    for (u64 offset = 0; offset < StackScanBytes && found < MaxBacktraceFrames; offset += 8) {
        u64 value = 0;
        if (!ReadU64(rsp + offset, value)) {
            break;
        }
        if (FindGuestModule(value) && IsExecutable(value)) {
            out += fmt::format("    scan[rsp+{:#x}] {}\n", offset, DescribeAddress(value));
            ++found;
        }
    }
    return out;
}

std::string FormatGuestBacktrace(u64 rip, u64 rsp, u64 rbp) {
    std::string out = fmt::format("    rip {}\n", DescribeAddress(rip));

    u64 frame = rbp;
    for (u32 i = 0; i < MaxBacktraceFrames; ++i) {
        u64 next = 0;
        u64 ret = 0;
        if (!ReadU64(frame, next) || !ReadU64(frame + 8, ret) || !FindGuestModule(ret)) {
            break;
        }
        out += fmt::format("    fp#{:<2} {}\n", i, DescribeAddress(ret));
        if (next <= frame || next - frame > 1_MB) {
            break;
        }
        frame = next;
    }
    return out + ScanStackForGuestFrames(rsp);
}

void WriteMinidump(std::string_view tag, EXCEPTION_POINTERS* exception_pointers) {
    static std::atomic<u32> dump_count{0};
    const u32 index = dump_count.fetch_add(1);
    if (index >= MaxMinidumps) {
        return;
    }
    using PFN_MiniDumpWriteDump =
        BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                      PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    static const auto write_dump = reinterpret_cast<PFN_MiniDumpWriteDump>(
        GetProcAddress(LoadLibraryW(L"dbghelp.dll"), "MiniDumpWriteDump"));
    if (!write_dump) {
        return;
    }
    const auto path = Common::FS::GetUserPath(Common::FS::PathType::LogDir) /
                      fmt::format("{}_{}_{}.dmp", tag, GetCurrentProcessId(), index);
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), exception_pointers, FALSE};
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithDataSegs |
                                                 MiniDumpWithIndirectlyReferencedMemory |
                                                 MiniDumpWithUnloadedModules);
    write_dump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
               exception_pointers ? &info : nullptr, nullptr, nullptr);
    CloseHandle(file);
    LOG_ERROR(Debug, "Wrote minidump {}", path.string());
}

struct ThreadSample {
    DWORD tid;
    HANDLE handle;
    u64 rip;
    u64 rsp;
    u64 rbp;
    bool sampled;
};

void ReportAllThreads(std::string_view reason) {
    std::vector<ThreadSample> samples;
    samples.reserve(512);

    const DWORD pid = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 entry{.dwSize = sizeof(THREADENTRY32)};
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) {
            continue;
        }
        const HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                             THREAD_QUERY_LIMITED_INFORMATION,
                                         FALSE, entry.th32ThreadID);
        if (handle) {
            samples.push_back({entry.th32ThreadID, handle, 0, 0, 0, false});
        }
    }
    CloseHandle(snapshot);

    for (auto& sample : samples) {
        if (SuspendThread(sample.handle) == static_cast<DWORD>(-1)) {
            continue;
        }
        alignas(16) CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        sample.sampled = GetThreadContext(sample.handle, &context);
        ResumeThread(sample.handle);
        sample.rip = context.Rip;
        sample.rsp = context.Rsp;
        sample.rbp = context.Rbp;
    }

    std::string report = fmt::format("{}: {} threads\n", reason, samples.size());
    for (const auto& sample : samples) {
        if (!sample.sampled) {
            continue;
        }
        report += fmt::format("  tid {:>6} '{}' rip {}\n", sample.tid, ThreadName(sample.handle),
                              DescribeAddress(sample.rip));
        if (FindGuestModule(sample.rip)) {
            report += FormatGuestBacktrace(sample.rip, sample.rsp, sample.rbp);
        } else {
            report += ScanStackForGuestFrames(sample.rsp);
        }
    }
    for (const auto& sample : samples) {
        CloseHandle(sample.handle);
    }
    LOG_ERROR(Debug, "{}", report);
    Common::Log::Flush();
}

HANDLE OpenThreadByName(std::string_view name, DWORD& tid) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    HANDLE found = nullptr;
    THREADENTRY32 entry{.dwSize = sizeof(THREADENTRY32)};
    for (BOOL ok = Thread32First(snapshot, &entry); ok && !found;
         ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != GetCurrentProcessId()) {
            continue;
        }
        const HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                             THREAD_QUERY_LIMITED_INFORMATION,
                                         FALSE, entry.th32ThreadID);
        if (!handle) {
            continue;
        }
        if (ThreadName(handle) == name) {
            found = handle;
            tid = entry.th32ThreadID;
        } else {
            CloseHandle(handle);
        }
    }
    CloseHandle(snapshot);
    return found;
}

void ProfileThread(std::string_view name, std::chrono::seconds duration) {
    constexpr u32 MaxFrames = 12;
    constexpr size_t TopN = 40;
    DWORD tid = 0;
    const HANDLE thread = OpenThreadByName(name, tid);
    if (!thread) {
        LOG_ERROR(Debug, "Profiler: no thread named '{}'", name);
        return;
    }
    LOG_INFO(Debug, "Profiler: sampling '{}' (tid {}) for {}s", name, tid, duration.count());
    {
        std::scoped_lock lock{g_fault_sites_mutex};
        g_fault_sites.clear();
    }
    g_fault_sites_active = true;

    std::unordered_map<u64, u32> exclusive;
    std::unordered_map<u64, u32> inclusive;
    u32 samples = 0;
    u32 guest_samples = 0;
    const auto end = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < end) {
        std::array<u64, MaxFrames> frames{};
        u32 depth = 0;
        u64 leaf_rip = 0;
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            alignas(16) CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &ctx)) {
                leaf_rip = ctx.Rip;
                for (; depth < MaxFrames && ctx.Rip; ++depth) {
                    DWORD64 image_base = 0;
                    auto* entry = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
                    if (!entry) {
                        frames[depth] = ctx.Rip;
                        if (depth != 0 || !ReadU64(ctx.Rsp, ctx.Rip)) {
                            ++depth;
                            break;
                        }
                        ctx.Rsp += 8;
                        continue;
                    }
                    frames[depth] = image_base + entry->BeginAddress;
                    void* handler_data = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, entry, &ctx,
                                     &handler_data, &establisher, nullptr);
                }
            }
            ResumeThread(thread);
        }
        if (depth > 0) {
            ++samples;
            guest_samples += FindGuestModule(leaf_rip) != nullptr;
            ++exclusive[frames[0]];
            for (u32 i = 0; i < depth; ++i) {
                if (std::find(frames.begin(), frames.begin() + i, frames[i]) ==
                    frames.begin() + i) {
                    ++inclusive[frames[i]];
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CloseHandle(thread);
    g_fault_sites_active = false;
    std::vector<std::pair<u64, FaultSite>> sites;
    {
        std::scoped_lock lock{g_fault_sites_mutex};
        sites.assign(g_fault_sites.begin(), g_fault_sites.end());
    }
    std::ranges::sort(sites,
                      [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
    std::string sites_report;
    for (size_t i = 0; i < std::min<size_t>(25, sites.size()); ++i) {
        sites_report += fmt::format("  {:>7} {} (last fault {:#x})\n", sites[i].second.count,
                                    DescribeAddress(sites[i].first), sites[i].second.last_fault);
    }
    LOG_INFO(Debug, "Read-fault sites during profile ({} distinct):\n{}", sites.size(),
             sites_report);

    const auto top = [&](const std::unordered_map<u64, u32>& counts) {
        std::vector<std::pair<u64, u32>> sorted(counts.begin(), counts.end());
        std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second > b.second; });
        std::string out;
        for (size_t i = 0; i < std::min(TopN, sorted.size()); ++i) {
            out += fmt::format("  {:5.1f}% {:>6} {}\n", 100.0 * sorted[i].second / samples,
                               sorted[i].second, DescribeAddress(sorted[i].first));
        }
        return out;
    };
    if (samples == 0) {
        return;
    }
    LOG_INFO(Debug,
             "Profiler '{}': {} samples, {:.1f}% in guest code\nExclusive (leaf function):\n{}"
             "Inclusive (on stack):\n{}",
             name, samples, 100.0 * guest_samples / samples, top(exclusive), top(inclusive));
    Common::Log::Flush();
}

void WatchdogBody(std::stop_token token) {
    Common::SetCurrentThreadName("shadPS4:Watchdog");
    using Clock = std::chrono::steady_clock;

    const char* env = std::getenv("SHADPS4_HANG_SECONDS");
    const auto hang_threshold = std::chrono::seconds(env ? std::max(1, std::atoi(env)) : 10);
    const auto trigger_path = Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "dump_now";
    const auto trace_path = Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "trace_frame";
    const auto profile_path = Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "profile_now";
    constexpr auto FpsInterval = std::chrono::seconds(5);
    constexpr u32 MaxHangReports = 3;

    u64 last_flips = g_flip_count.load();
    auto last_change = Clock::now();
    u64 interval_start_flips = last_flips;
    auto interval_start = Clock::now();
    bool hang_reported = false;
    std::array<u64, static_cast<size_t>(Counter::Count)> last_counters{};
    u32 hang_reports = 0;

    while (!token.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto now = Clock::now();
        const u64 flips = g_flip_count.load();

        if (now - interval_start >= FpsInterval) {
            const double seconds = std::chrono::duration<double>(now - interval_start).count();
            size_t guest_stacks = 0;
            {
                std::shared_lock lock{g_guest_stacks_mutex};
                guest_stacks = g_guest_stacks.size();
            }
            LOG_INFO(Debug, "Watchdog: {:.1f} flips/s ({} total), {} guest stacks registered",
                     static_cast<double>(flips - interval_start_flips) / seconds, flips,
                     guest_stacks);
            std::array<u64, static_cast<size_t>(Counter::Count)> now_counters{};
            for (size_t i = 0; i < now_counters.size(); ++i) {
                now_counters[i] = g_counters[i].load(std::memory_order_relaxed);
            }
            const auto delta = [&](Counter c) {
                const auto i = static_cast<size_t>(c);
                return static_cast<double>(now_counters[i] - last_counters[i]) / seconds;
            };
            LOG_INFO(Debug,
                     "Perf/s: read faults gpu-thread {:.0f} other {:.0f}, write faults {:.0f}, "
                     "downloads {:.0f} ({:.2f} MB, wait {:.1f} ms), pipeline compiles {:.1f}, "
                     "gpu-thread idle {:.0f} ms, fence waits {:.0f} ms, written bindings {:.0f} "
                     "({:.1f} MB marked GPU-modified)",
                     delta(Counter::ReadFaultGpuThread), delta(Counter::ReadFaultOtherThread),
                     delta(Counter::WriteFault), delta(Counter::Downloads),
                     delta(Counter::DownloadBytes) / 1e6, delta(Counter::DownloadWaitUs) / 1e3,
                     delta(Counter::PipelineCompiles), delta(Counter::GpuThreadIdleUs) / 1e3,
                     delta(Counter::FenceWaitUs) / 1e3, delta(Counter::GpuWrittenBindings),
                     delta(Counter::GpuWrittenBytes) / 1e6);
            last_counters = now_counters;
            interval_start = now;
            interval_start_flips = flips;
        }

        if (flips != last_flips) {
            if (hang_reported) {
                LOG_WARNING(
                    Debug, "Watchdog: flips resumed after {}s",
                    std::chrono::duration_cast<std::chrono::seconds>(now - last_change).count());
                hang_reported = false;
            }
            last_flips = flips;
            last_change = now;
        } else if (flips != 0 && !hang_reported && hang_reports < MaxHangReports &&
                   now - last_change >= hang_threshold) {
            hang_reported = true;
            ++hang_reports;
            ReportAllThreads(fmt::format("Watchdog: no flip for {}s", hang_threshold.count()));
            WriteMinidump("hang", nullptr);
        }

        std::error_code ec;
        if (std::filesystem::exists(trigger_path, ec)) {
            std::filesystem::remove(trigger_path, ec);
            ReportAllThreads("Watchdog: dump requested");
            WriteMinidump("manual", nullptr);
        }
        if (std::filesystem::exists(trace_path, ec)) {
            std::filesystem::remove(trace_path, ec);
            g_frame_trace_pending = true;
            LOG_INFO(Debug, "Frame trace armed for the next frame");
        }
        if (std::filesystem::exists(profile_path, ec)) {
            std::string name;
            {
                std::ifstream file{profile_path};
                std::getline(file, name);
            }
            std::filesystem::remove(profile_path, ec);
            while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) {
                name.pop_back();
            }
            ProfileThread(name.empty() ? "shadPS4:GpuCommandProcessor" : name,
                          std::chrono::seconds(5));
        }
    }
}

#endif

} // Anonymous namespace

void StartWatchdog() {
#ifdef _WIN32
    g_watchdog = std::jthread(WatchdogBody);
#endif
}

bool SafeRead(u64 address, void* out, u64 size) {
#ifdef _WIN32
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), out, size,
                             &read) &&
           read == size;
#else
    return false;
#endif
}

void NotifyFlip() {
    const u64 flip = g_flip_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g_frame_trace_active.exchange(false)) {
        LOG_INFO(Debug, "[trace] end of frame at flip {} ({} commands)", flip,
                 g_frame_trace_index.load());
        Common::Log::Flush();
    } else if (g_frame_trace_pending.exchange(false)) {
        g_frame_trace_index = 0;
        g_frame_trace_active = true;
        LOG_INFO(Debug, "[trace] begin frame after flip {}", flip);
    }
}

void AddCounter(Counter counter, u64 value) {
    g_counters[static_cast<size_t>(counter)].fetch_add(value, std::memory_order_relaxed);
}

void RecordReadFaultSite(u64 code_address, u64 fault_address) {
    if (!g_fault_sites_active.load(std::memory_order_relaxed)) {
        return;
    }
    std::scoped_lock lock{g_fault_sites_mutex};
    auto& site = g_fault_sites[code_address];
    ++site.count;
    site.last_fault = fault_address;
}

bool IsFrameTraceActive() {
    return g_frame_trace_active.load(std::memory_order_relaxed);
}

u32 NextFrameTraceIndex() {
    constexpr u32 MaxTracedCommands = 2000;
    const u32 index = g_frame_trace_index.fetch_add(1, std::memory_order_relaxed);
    if (index + 1 == MaxTracedCommands && g_frame_trace_active.exchange(false)) {
        LOG_INFO(Debug, "[trace] stopped after {} commands without a flip", MaxTracedCommands);
        Common::Log::Flush();
    }
    return index;
}

std::string DescribeAddress(u64 address) {
#ifdef _WIN32
    if (const auto* module = FindGuestModule(address)) {
        return fmt::format("{:#x} {}+{:#x}", address, module->name,
                           address - module->GetBaseAddress());
    }
    HMODULE host_module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &host_module)) {
        return fmt::format("{:#x} {}+{:#x}", address, HostModuleName(host_module),
                           address - reinterpret_cast<u64>(host_module));
    }
#endif
    return fmt::format("{:#x}", address);
}

void ReportUnhandledException(void* exception_pointers, bool guest_thread, s32 guest_signal) {
#ifdef _WIN32
    auto* pExp = static_cast<EXCEPTION_POINTERS*>(exception_pointers);
    const auto* record = pExp->ExceptionRecord;
    const auto* ctx = pExp->ContextRecord;
    const bool is_av = record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION;
    LOG_ERROR(Debug,
              "Exception {:#x} not handled by emulator, dispatching signal {} to guest (guest "
              "thread {})\n"
              "  at {}\n"
              "  fault addr {:#x} ({}), access {}\n"
              "  rax {:#018x} rbx {:#018x} rcx {:#018x} rdx {:#018x}\n"
              "  rsi {:#018x} rdi {:#018x} rbp {:#018x} rsp {:#018x}\n"
              "  r8  {:#018x} r9  {:#018x} r10 {:#018x} r11 {:#018x}\n"
              "  r12 {:#018x} r13 {:#018x} r14 {:#018x} r15 {:#018x}\n"
              "  backtrace:\n{}",
              record->ExceptionCode, guest_signal, guest_thread,
              DescribeAddress(reinterpret_cast<u64>(record->ExceptionAddress)),
              is_av ? record->ExceptionInformation[1] : 0,
              is_av ? DescribeAddress(record->ExceptionInformation[1]) : "-",
              is_av ? record->ExceptionInformation[0] : 0, ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx,
              ctx->Rsi, ctx->Rdi, ctx->Rbp, ctx->Rsp, ctx->R8, ctx->R9, ctx->R10, ctx->R11,
              ctx->R12, ctx->R13, ctx->R14, ctx->R15,
              FormatGuestBacktrace(ctx->Rip, ctx->Rsp, ctx->Rbp));
    Common::Log::Flush();
    WriteMinidump("exception", pExp);
    Common::Log::Flush();
#endif
}

void RegisterGuestStack(u64 base, u64 size, std::string_view name) {
    {
        std::unique_lock lock{g_guest_stacks_mutex};
        g_guest_stacks[base] = {size, std::string(name)};
    }
    Core::Memory::Instance()->InvalidateMemory(base, size);
}

void UnregisterGuestStack(u64 base) {
    std::unique_lock lock{g_guest_stacks_mutex};
    g_guest_stacks.erase(base);
}

static bool ShouldLogOccurrence(std::atomic<u64>& counter, u64& n) {
    n = counter.fetch_add(1) + 1;
    return n <= 30 || std::has_single_bit(n);
}

static std::optional<std::string> FindOverlappingGuestStack(u64 address, u64 size) {
    std::shared_lock lock{g_guest_stacks_mutex};
    auto it = g_guest_stacks.upper_bound(address + size - 1);
    if (it == g_guest_stacks.begin()) {
        return std::nullopt;
    }
    --it;
    const u64 stack_end = it->first + it->second.size;
    if (stack_end <= address) {
        return std::nullopt;
    }
    return fmt::format("'{}' [{:#x}, {:#x})", it->second.name, it->first, stack_end);
}

void CheckProtectOverlapsGuestStack(u64 address, u64 size) {
    static std::atomic<u64> count{0};
    if (const auto stack = FindOverlappingGuestStack(address, size)) {
        if (u64 n; ShouldLogOccurrence(count, n)) {
            LOG_WARNING(Debug,
                        "Left guest stack {} writable while write-protecting [{:#x}, {:#x}) (#{})",
                        *stack, address, address + size, n);
        }
    }
}

std::vector<GuestStackRange> GuestStackRangesIn(u64 address, u64 size) {
    std::vector<GuestStackRange> ranges;
    std::shared_lock lock{g_guest_stacks_mutex};
    auto it = g_guest_stacks.upper_bound(address);
    if (it != g_guest_stacks.begin()) {
        --it;
    }
    for (; it != g_guest_stacks.end() && it->first < address + size; ++it) {
        const u64 stack_end = it->first + it->second.size;
        if (stack_end > address) {
            ranges.push_back({it->first, stack_end});
        }
    }
    return ranges;
}

void CheckGuestMemoryChange(u64 address, u64 size, std::string_view operation) {
    static std::atomic<u64> stack_count{0};
    static std::atomic<u64> own_sp_count{0};
    const u64 sp = reinterpret_cast<u64>(&address);
    if (sp >= address && sp < address + size) {
        if (u64 n; ShouldLogOccurrence(own_sp_count, n)) {
            LOG_ERROR(Debug,
                      "{} [{:#x}, {:#x}) contains the calling thread's stack pointer {:#x} (#{})",
                      operation, address, address + size, sp, n);
        }
    }
    if (const auto stack = FindOverlappingGuestStack(address, size)) {
        if (u64 n; ShouldLogOccurrence(stack_count, n)) {
            LOG_ERROR(Debug, "{} [{:#x}, {:#x}) overlaps registered guest stack {} (#{})",
                      operation, address, address + size, *stack, n);
        }
    }
}

} // namespace Core::Diagnostics
