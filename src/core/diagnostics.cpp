// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <vector>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/diagnostics.h"
#include "core/linker.h"

#ifdef _WIN32
#include <dbghelp.h>
#include <tlhelp32.h>
#include <windows.h>
#endif

namespace Core::Diagnostics {

namespace {

std::atomic<u64> g_flip_count{0};
std::jthread g_watchdog;

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

void WatchdogBody(std::stop_token token) {
    Common::SetCurrentThreadName("shadPS4:Watchdog");
    using Clock = std::chrono::steady_clock;

    const char* env = std::getenv("SHADPS4_HANG_SECONDS");
    const auto hang_threshold = std::chrono::seconds(env ? std::max(1, std::atoi(env)) : 10);
    const auto trigger_path = Common::FS::GetUserPath(Common::FS::PathType::LogDir) / "dump_now";
    constexpr auto FpsInterval = std::chrono::seconds(5);
    constexpr u32 MaxHangReports = 3;

    u64 last_flips = g_flip_count.load();
    auto last_change = Clock::now();
    u64 interval_start_flips = last_flips;
    auto interval_start = Clock::now();
    bool hang_reported = false;
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
    }
}

#endif

} // Anonymous namespace

void StartWatchdog() {
#ifdef _WIN32
    g_watchdog = std::jthread(WatchdogBody);
#endif
}

void NotifyFlip() {
    g_flip_count.fetch_add(1, std::memory_order_relaxed);
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
    std::unique_lock lock{g_guest_stacks_mutex};
    g_guest_stacks[base] = {size, std::string(name)};
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
