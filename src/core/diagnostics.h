// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>
#include "common/types.h"

namespace Core::Diagnostics {

void StartWatchdog();

void NotifyFlip();

bool IsFrameTraceActive();

u32 NextFrameTraceIndex();

std::string DescribeAddress(u64 address);

bool SafeRead(u64 address, void* out, u64 size);

void ReportUnhandledException(void* exception_pointers, bool guest_thread, s32 guest_signal);

void RegisterGuestStack(u64 base, u64 size, std::string_view name);

void UnregisterGuestStack(u64 base);

void CheckProtectOverlapsGuestStack(u64 address, u64 size);

void CheckGuestMemoryChange(u64 address, u64 size, std::string_view operation);

struct GuestStackRange {
    u64 begin;
    u64 end;
};

std::vector<GuestStackRange> GuestStackRangesIn(u64 address, u64 size);

} // namespace Core::Diagnostics
