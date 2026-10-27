// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/diagnostics.h"
#include "input/controller.h"
#include "input/scripted_input.h"
#include "video_core/renderdoc.h"

namespace Input::Scripted {

namespace {

using Libraries::Pad::OrbisPadButtonDataOffset;
using namespace std::chrono_literals;

std::jthread g_thread;

struct Buttons {
    OrbisPadButtonDataOffset mask{};
    bool l2{};
    bool r2{};
};

std::optional<OrbisPadButtonDataOffset> ParseButton(const std::string& name) {
    static constexpr std::pair<const char*, OrbisPadButtonDataOffset> Names[] = {
        {"cross", OrbisPadButtonDataOffset::Cross},
        {"circle", OrbisPadButtonDataOffset::Circle},
        {"square", OrbisPadButtonDataOffset::Square},
        {"triangle", OrbisPadButtonDataOffset::Triangle},
        {"up", OrbisPadButtonDataOffset::Up},
        {"down", OrbisPadButtonDataOffset::Down},
        {"left", OrbisPadButtonDataOffset::Left},
        {"right", OrbisPadButtonDataOffset::Right},
        {"l1", OrbisPadButtonDataOffset::L1},
        {"r1", OrbisPadButtonDataOffset::R1},
        {"l2", OrbisPadButtonDataOffset::L2},
        {"r2", OrbisPadButtonDataOffset::R2},
        {"l3", OrbisPadButtonDataOffset::L3},
        {"r3", OrbisPadButtonDataOffset::R3},
        {"options", OrbisPadButtonDataOffset::Options},
        {"touchpad", OrbisPadButtonDataOffset::TouchPad},
    };
    for (const auto& [n, button] : Names) {
        if (name == n) {
            return button;
        }
    }
    return std::nullopt;
}

std::optional<Buttons> ParseButtons(const std::string& list) {
    Buttons buttons;
    std::stringstream ss{list};
    std::string name;
    while (std::getline(ss, name, '+')) {
        const auto button = ParseButton(name);
        if (!button) {
            return std::nullopt;
        }
        // The triggers are axes; setting the axis also sets the button bit (State::OnAxis).
        if (*button == OrbisPadButtonDataOffset::L2) {
            buttons.l2 = true;
        } else if (*button == OrbisPadButtonDataOffset::R2) {
            buttons.r2 = true;
        } else {
            buttons.mask |= *button;
        }
    }
    return buttons;
}

std::optional<Input::Axis> ParseAxis(const std::string& name) {
    if (name == "lx") {
        return Input::Axis::LeftX;
    }
    if (name == "ly") {
        return Input::Axis::LeftY;
    }
    if (name == "rx") {
        return Input::Axis::RightX;
    }
    if (name == "ry") {
        return Input::Axis::RightY;
    }
    if (name == "l2") {
        return Input::Axis::TriggerLeft;
    }
    if (name == "r2") {
        return Input::Axis::TriggerRight;
    }
    return std::nullopt;
}

void SetButtons(GameController* pad, const Buttons& buttons, bool pressed) {
    for (u32 bit = 1; bit != 0; bit <<= 1) {
        const auto button = static_cast<OrbisPadButtonDataOffset>(bit);
        if (True(buttons.mask & button)) {
            pad->Button(button, pressed);
        }
    }
    if (buttons.l2) {
        pad->Axis(Input::Axis::TriggerLeft, pressed ? 255 : 0, false);
    }
    if (buttons.r2) {
        pad->Axis(Input::Axis::TriggerRight, pressed ? 255 : 0, false);
    }
}

void CentreAxes(GameController* pad) {
    for (const auto axis : {Input::Axis::LeftX, Input::Axis::LeftY, Input::Axis::RightX,
                            Input::Axis::RightY}) {
        pad->Axis(axis, 128, false);
    }
    pad->Axis(Input::Axis::TriggerLeft, 0, false);
    pad->Axis(Input::Axis::TriggerRight, 0, false);
}

void WaitFlips(std::stop_token token, u64 frames) {
    const u64 target = Core::Diagnostics::FlipCount() + frames;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!token.stop_requested() && Core::Diagnostics::FlipCount() < target &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
}

void Sleep(std::stop_token token, u64 ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!token.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(
            deadline - std::chrono::steady_clock::now(), 20ms));
    }
}

// Runs one command line; returns false if it could not be parsed.
bool RunCommand(std::stop_token token, GameController* pad, const std::string& line) {
    std::stringstream ss{line};
    std::string op;
    ss >> op;
    if (op.empty()) {
        return true;
    }
    std::vector<std::string> args;
    for (std::string arg; ss >> arg;) {
        args.push_back(arg);
    }
    const auto number = [&](size_t i, u64 fallback) -> std::optional<u64> {
        if (i >= args.size()) {
            return fallback;
        }
        try {
            return std::stoull(args[i]);
        } catch (...) {
            return std::nullopt;
        }
    };

    if (op == "press" || op == "hold" || op == "release") {
        const auto buttons = args.empty() ? std::nullopt : ParseButtons(args[0]);
        if (!buttons) {
            return false;
        }
        if (op == "release") {
            SetButtons(pad, *buttons, false);
            return true;
        }
        SetButtons(pad, *buttons, true);
        if (op == "press") {
            const auto frames = number(1, 3);
            if (!frames) {
                return false;
            }
            const auto start = std::chrono::steady_clock::now();
            WaitFlips(token, *frames);
            const auto held = std::chrono::steady_clock::now() - start;
            if (held < 100ms) {
                std::this_thread::sleep_for(100ms - held);
            }
            SetButtons(pad, *buttons, false);
        }
        return true;
    }
    if (op == "axis") {
        const auto axis = args.empty() ? std::nullopt : ParseAxis(args[0]);
        const auto value = number(1, 128);
        if (!axis || args.size() < 2 || !value || *value > 255) {
            return false;
        }
        pad->Axis(*axis, static_cast<int>(*value), false);
        return true;
    }
    if (op == "stick") {
        if (args.size() != 4 || (args[0] != "l" && args[0] != "r")) {
            return false;
        }
        const auto x = number(1, 128);
        const auto y = number(2, 128);
        const auto ms = number(3, 0);
        if (!x || !y || !ms || *x > 255 || *y > 255) {
            return false;
        }
        const bool left = args[0] == "l";
        const auto ax = left ? Input::Axis::LeftX : Input::Axis::RightX;
        const auto ay = left ? Input::Axis::LeftY : Input::Axis::RightY;
        pad->Axis(ax, static_cast<int>(*x), false);
        pad->Axis(ay, static_cast<int>(*y), false);
        Sleep(token, *ms);
        pad->Axis(ax, 128, false);
        pad->Axis(ay, 128, false);
        return true;
    }
    if (op == "wait") {
        const auto ms = number(0, 0);
        if (!ms) {
            return false;
        }
        Sleep(token, *ms);
        return true;
    }
    if (op == "waitf") {
        const auto frames = number(0, 1);
        if (!frames) {
            return false;
        }
        WaitFlips(token, *frames);
        return true;
    }
    if (op == "shot") {
        VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
        return true;
    }
    if (op == "reset") {
        SetButtons(pad, {.mask = static_cast<OrbisPadButtonDataOffset>(0x7FFFFFFF)}, false);
        CentreAxes(pad);
        return true;
    }
    return false;
}

void ScriptedInputBody(std::stop_token token) {
    Common::SetCurrentThreadName("shadPS4:ScriptedInput");
    const auto log_dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
    const auto script_path = log_dir / "input";
    const auto done_path = log_dir / "input_done";
    auto* pad = (*Common::Singleton<GameControllers>::Instance())[0];

    while (!token.stop_requested()) {
        std::this_thread::sleep_for(50ms);
        std::error_code ec;
        if (!std::filesystem::exists(script_path, ec)) {
            continue;
        }
        std::vector<std::string> lines;
        {
            std::ifstream file{script_path};
            for (std::string line; std::getline(file, line);) {
                if (const auto hash = line.find('#'); hash != std::string::npos) {
                    line.erase(hash);
                }
                std::transform(line.begin(), line.end(), line.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                lines.push_back(std::move(line));
            }
        }
        std::filesystem::remove(script_path, ec);

        const u64 start_flips = Core::Diagnostics::FlipCount();
        u32 errors = 0;
        LOG_INFO(Input, "Scripted input: running {} lines at flip {}", lines.size(), start_flips);
        for (const auto& line : lines) {
            if (token.stop_requested()) {
                break;
            }
            if (!RunCommand(token, pad, line)) {
                ++errors;
                LOG_WARNING(Input, "Scripted input: bad command '{}'", line);
            }
        }
        const u64 end_flips = Core::Diagnostics::FlipCount();
        auto tmp_path = done_path;
        tmp_path += ".tmp";
        std::ofstream{tmp_path} << start_flips << ' ' << end_flips << ' ' << errors << '\n';
        std::filesystem::rename(tmp_path, done_path, ec);
    }
}

} // Anonymous namespace

void StartScriptedInput() {
    g_thread = std::jthread(ScriptedInputBody);
}

} // namespace Input::Scripted
