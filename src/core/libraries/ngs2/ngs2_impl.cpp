// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <unordered_set>
#include <fmt/format.h>

#include "ngs2.h"
#include "ngs2_error.h"
#include "ngs2_impl.h"
#include "ngs2_submixer.h"

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/libraries/error_codes.h"

namespace Libraries::Ngs2 {

namespace {

constexpr u32 MaxInputs = 4;

std::recursive_mutex g_mutex;
std::unordered_set<System*> g_systems;
std::unordered_set<Rack*> g_racks;
std::unordered_set<Voice*> g_voices;

std::unique_ptr<Voice> CreateVoice(Rack* rack, u32 index) {
    switch (rack->rackId) {
    case ORBIS_NGS2_RACK_ID_SAMPLER:
        return CreateSamplerVoice(rack, index);
    case ORBIS_NGS2_RACK_ID_MASTERING:
        return CreateMasteringVoice(rack, index);
    default:
        return CreateSubmixerVoice(rack, index);
    }
}

} // namespace

std::recursive_mutex& GetMutex() {
    return g_mutex;
}

System* GetSystem(OrbisNgs2Handle handle) {
    auto* system = reinterpret_cast<System*>(handle);
    return g_systems.contains(system) ? system : nullptr;
}

Rack* GetRack(OrbisNgs2Handle handle) {
    auto* rack = reinterpret_cast<Rack*>(handle);
    return g_racks.contains(rack) ? rack : nullptr;
}

Voice* GetVoice(OrbisNgs2Handle handle) {
    auto* voice = reinterpret_cast<Voice*>(handle);
    return g_voices.contains(voice) ? voice : nullptr;
}

namespace {

struct DiagWriter {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::string> lines;
    std::vector<s16> capture;
    std::deque<std::pair<std::filesystem::path, std::vector<u8>>> dumps;
    std::FILE* file = nullptr;
    std::FILE* captureFile = nullptr;
    std::thread thread;

    void Run() {
        std::vector<s16> samples;
        std::deque<std::string> pending;
        std::deque<std::pair<std::filesystem::path, std::vector<u8>>> pendingDumps;
        while (true) {
            {
                std::unique_lock lock{mutex};
                cv.wait(lock,
                        [this] { return !lines.empty() || !capture.empty() || !dumps.empty(); });
                pending.swap(lines);
                samples.swap(capture);
                pendingDumps.swap(dumps);
            }
            for (const auto& line : pending) {
                std::fwrite(line.data(), 1, line.size(), file);
            }
            std::fflush(file);
            if (captureFile && !samples.empty()) {
                std::fwrite(samples.data(), sizeof(s16), samples.size(), captureFile);
                std::fflush(captureFile);
            }
            for (const auto& [path, bytes] : pendingDumps) {
                if (std::FILE* dump = std::fopen(path.string().c_str(), "wb")) {
                    std::fwrite(bytes.data(), 1, bytes.size(), dump);
                    std::fclose(dump);
                }
            }
            pending.clear();
            samples.clear();
            pendingDumps.clear();
        }
    }
};

struct DiagState {
    bool initialized = false;
    bool enabled = false;
    DiagWriter* writer = nullptr;
    std::array<s16, 2 * 1024> stereo{};
    u64 captureFrames = 0;
    std::filesystem::path logDir;
    std::unordered_set<uintptr_t> dumpedWaveforms;
    size_t dumpedBytes = 0;
    std::chrono::steady_clock::time_point start;
    std::map<std::pair<u32, u32>, u32> paramCounts;
    u32 starts = 0;
    u32 ends = 0;
    u32 silentEnds = 0;
    std::array<std::array<float, 2>, 5> rackPeaks{};
    std::array<float, 4> bufferPeaks{};
    std::array<double, 4> bufferSquares{};
    u64 bufferSamples = 0;
    u32 renders = 0;
};

DiagState g_diag;
uintptr_t g_diagCaller = 0;
std::map<uintptr_t, u32> g_diagZeroMatrixCallers;

constexpr u64 DiagMaxCaptureSeconds = 20 * 60;
constexpr u32 DiagReportSeconds = 5;
constexpr u32 DiagMaxParamLogs = 10;
constexpr size_t DiagMaxRouteLength = 800;
constexpr u32 DiagMaxRouteDepth = 6;

void AppendRoute(std::string& out, const Voice* voice, u32 depth) {
    for (u32 i = 0; i < voice->ports.size(); i++) {
        const auto& port = voice->ports[i];
        if (!port.dest || out.size() > DiagMaxRouteLength) {
            continue;
        }
        float gain = port.volume;
        if (port.matrixId >= 0 && voice->matrixNumLevels[port.matrixId] > 0) {
            const auto& matrix = voice->matrices[port.matrixId];
            const u32 numLevels = voice->matrixNumLevels[port.matrixId];
            float maxLevel = 0.0f;
            for (u32 j = 0; j < numLevels; j++) {
                maxLevel = std::max(maxLevel, std::abs(matrix[j]));
            }
            gain *= maxLevel;
        }
        out += fmt::format(" p{}(g{:.3f}{})->{}{}", i, gain,
                           port.destInputId ? fmt::format(",in{}", port.destInputId) : "",
                           DiagVoiceName(port.dest), port.dest->IsActive() ? "" : "[INACTIVE]");
        if (port.dest->OutputId() >= 0) {
            out += fmt::format("=>out{}", port.dest->OutputId());
        } else if (depth < DiagMaxRouteDepth) {
            out += " {";
            AppendRoute(out, port.dest, depth + 1);
            out += " }";
        }
    }
}

} // namespace

bool DiagEnabled() {
    return g_diag.enabled;
}

void DiagInit() {
    if (g_diag.initialized) {
        return;
    }
    g_diag.initialized = true;
    const auto& logDir = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
    std::error_code ec;
    if (!std::filesystem::exists(logDir / "ngs2_diag", ec)) {
        return;
    }
    std::FILE* file = std::fopen((logDir / "ngs2_diag.txt").string().c_str(), "wb");
    if (!file) {
        LOG_ERROR(Lib_Ngs2, "Unable to open NGS2 diagnostics file");
        return;
    }
    g_diag.logDir = logDir;
    g_diag.writer = new DiagWriter();
    g_diag.writer->file = file;
    g_diag.writer->captureFile = std::fopen((logDir / "ngs2_capture.raw").string().c_str(), "wb");
    g_diag.writer->thread = std::thread([writer = g_diag.writer] { writer->Run(); });
    g_diag.writer->thread.detach();
    g_diag.start = std::chrono::steady_clock::now();
    g_diag.enabled = true;
    LOG_INFO(Lib_Ngs2, "NGS2 diagnostics enabled");
    DiagWrite("ngs2 diagnostics; capture = ngs2_capture.raw, s16le stereo 48000 Hz, "
              "downmix of every 8-channel render buffer");
}

void DiagWrite(std::string_view line) {
    if (!g_diag.writer) {
        return;
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - g_diag.start).count();
    std::string text = fmt::format("[{:9.3f}] {}\n", seconds, line);
    {
        std::scoped_lock lock{g_diag.writer->mutex};
        g_diag.writer->lines.push_back(std::move(text));
    }
    g_diag.writer->cv.notify_one();
}

std::string DiagVoiceName(const Voice* voice) {
    return fmt::format("{:04x}:{}@{:06x}", voice->rack->rackId, voice->index,
                       reinterpret_cast<uintptr_t>(voice) & 0xFFFFFF);
}

std::string DiagRoute(const Voice* voice) {
    std::string route;
    AppendRoute(route, voice, 0);
    return route.empty() ? " (no patch)" : route;
}

void DiagParam(const Voice* voice, const OrbisNgs2VoiceParamHeader* param, std::string_view what) {
    if (!g_diag.enabled) {
        return;
    }
    const u32 count = ++g_diag.paramCounts[{voice->rack->rackId, param->id}];
    if (count > DiagMaxParamLogs && count % 1000 != 0) {
        return;
    }
    const auto* words = reinterpret_cast<const u32*>(param);
    std::string raw;
    for (size_t i = 2; i < std::min<size_t>(param->size, 0x80) / 4; i++) {
        raw += fmt::format(" {:x}", words[i]);
    }
    DiagWrite(fmt::format("param {} {} id {:#x} size {} count {}:{}", what, DiagVoiceName(voice),
                          param->id, param->size, count, raw));
}

void DiagDumpWaveform(const Voice* voice, const void* data, u32 size, std::string_view info) {
    constexpr size_t MaxDumpedBytes = 64ULL << 20;
    const auto address = reinterpret_cast<uintptr_t>(data);
    if (!g_diag.enabled || !data || size == 0 || g_diag.dumpedBytes + size > MaxDumpedBytes ||
        !g_diag.dumpedWaveforms.insert(address).second) {
        return;
    }
    g_diag.dumpedBytes += size;
    const std::string name = fmt::format("ngs2_wave_{:x}.bin", address);
    const auto* bytes = static_cast<const u8*>(data);
    {
        std::scoped_lock lock{g_diag.writer->mutex};
        g_diag.writer->dumps.emplace_back(g_diag.logDir / name,
                                          std::vector<u8>(bytes, bytes + size));
    }
    g_diag.writer->cv.notify_one();
    DiagWrite(fmt::format("wavedump {} {} size {:#x} {}", DiagVoiceName(voice), name, size, info));
}

void DiagSetCaller(uintptr_t caller) {
    g_diagCaller = caller;
}

void DiagMatrix(Voice* voice, u32 matrixId) {
    const auto& matrix = voice->matrices[matrixId];
    float maxLevel = 0.0f;
    for (u32 i = 0; i < voice->matrixNumLevels[matrixId]; i++) {
        maxLevel = std::max(maxLevel, std::abs(matrix[i]));
    }
    if (voice->diag.playing) {
        voice->diag.matrixUpdates++;
        voice->diag.maxMatrixLevel = std::max(voice->diag.maxMatrixLevel, maxLevel);
    }
    if (maxLevel == 0.0f && voice->rack->rackId == ORBIS_NGS2_RACK_ID_SAMPLER &&
        g_diagZeroMatrixCallers[g_diagCaller]++ < 3) {
        DiagWrite(fmt::format("zero-matrix {} levels {} caller {:#x}", DiagVoiceName(voice),
                              voice->matrixNumLevels[matrixId], g_diagCaller));
    }
}

void Voice::DiagStart() {
    if (!g_diag.enabled) {
        return;
    }
    const u32 playId = diag.playId + 1;
    diag = {};
    diag.playId = playId;
    diag.playing = true;
    g_diag.starts++;
    DiagWrite(fmt::format("start {} #{} route:{}", DiagVoiceName(this), playId, DiagRoute(this)));
}

void Voice::DiagEnd(std::string_view reason, std::string_view details) {
    if (!g_diag.enabled || !diag.playing) {
        return;
    }
    diag.playing = false;
    g_diag.ends++;
    const bool silent = diag.audibleGrains == 0;
    if (silent) {
        g_diag.silentEnds++;
    }
    DiagWrite(
        fmt::format("end {} #{} reason {} grains {} audible {} peak {:.5f} maxSendGain {:.4f} "
                    "matrixUpdates {} maxMatrix {:.4f}{} {} route:{}",
                    DiagVoiceName(this), diag.playId, reason, diag.grains, diag.audibleGrains,
                    diag.peakOut, diag.maxSendGain, diag.matrixUpdates, diag.maxMatrixLevel,
                    silent ? " SILENT" : "", details, DiagRoute(this)));
}

void DiagRender(System* system, const OrbisNgs2RenderBufferInfo* bufferInfo, u32 numBufferInfo,
                u32 numSamples, u32 numActiveVoices) {
    constexpr u32 PcmF32L = 0x18;
    auto& stereo = g_diag.stereo;
    std::fill_n(stereo.begin(), numSamples * 2, s16{0});
    bool hasCapture = false;
    for (u32 i = 0; i < numBufferInfo; i++) {
        const auto& info = bufferInfo[i];
        if (info.waveformType != PcmF32L || !info.buffer || info.numChannels == 0) {
            continue;
        }
        const auto* samples = static_cast<const float*>(info.buffer);
        const u32 frames = std::min<u32>(
            numSamples, static_cast<u32>(info.bufferSize / (sizeof(float) * info.numChannels)));
        if (i < g_diag.bufferPeaks.size()) {
            for (u32 j = 0; j < frames * info.numChannels; j++) {
                g_diag.bufferPeaks[i] = std::max(g_diag.bufferPeaks[i], std::abs(samples[j]));
                g_diag.bufferSquares[i] += static_cast<double>(samples[j]) * samples[j];
            }
        }
        if (info.numChannels != MaxChannels || frames > stereo.size() / 2) {
            continue;
        }
        hasCapture = true;
        for (u32 j = 0; j < frames; j++) {
            const float* frame = samples + j * MaxChannels;
            const float left = frame[0] + 0.7f * (frame[2] + frame[4] + frame[6]);
            const float right = frame[1] + 0.7f * (frame[2] + frame[5] + frame[7]);
            stereo[j * 2] =
                static_cast<s16>(std::clamp(stereo[j * 2] + left * 32767.0f, -32768.0f, 32767.0f));
            stereo[j * 2 + 1] = static_cast<s16>(
                std::clamp(stereo[j * 2 + 1] + right * 32767.0f, -32768.0f, 32767.0f));
        }
    }
    g_diag.bufferSamples += numSamples;
    if (hasCapture && g_diag.captureFrames < DiagMaxCaptureSeconds * system->sampleRate) {
        {
            std::scoped_lock lock{g_diag.writer->mutex};
            g_diag.writer->capture.insert(g_diag.writer->capture.end(), stereo.begin(),
                                          stereo.begin() + numSamples * 2);
        }
        g_diag.writer->cv.notify_one();
        g_diag.captureFrames += numSamples;
    }

    if (++g_diag.renders < DiagReportSeconds * system->sampleRate / numSamples) {
        return;
    }
    u32 samplerInUse = 0;
    u32 samplerPlaying = 0;
    for (const Rack* rack : system->racks) {
        if (rack->rackId != ORBIS_NGS2_RACK_ID_SAMPLER) {
            continue;
        }
        for (const auto& voice : rack->voices) {
            samplerInUse += (voice->stateFlags & ORBIS_NGS2_VOICE_STATE_FLAG_INUSE) ? 1 : 0;
            samplerPlaying += voice->diag.playing ? 1 : 0;
        }
    }
    std::string report = fmt::format(
        "summary active {} samplerInUse {} samplerPlaying {} starts {} ends {} silentEnds {} |",
        numActiveVoices, samplerInUse, samplerPlaying, g_diag.starts, g_diag.ends,
        g_diag.silentEnds);
    for (u32 i = 1; i < g_diag.rackPeaks.size(); i++) {
        report += fmt::format(" rack{}xxx in {:.4f} out {:.4f}", i, g_diag.rackPeaks[i][0],
                              g_diag.rackPeaks[i][1]);
    }
    report += " |";
    for (u32 i = 0; i < std::min<u32>(numBufferInfo, g_diag.bufferPeaks.size()); i++) {
        const double count =
            static_cast<double>(g_diag.bufferSamples) * std::max(bufferInfo[i].numChannels, 1u);
        report += fmt::format(" buf{} rms {:.5f} peak {:.5f}", i,
                              std::sqrt(g_diag.bufferSquares[i] / count), g_diag.bufferPeaks[i]);
    }
    DiagWrite(report);
    g_diag.starts = g_diag.ends = g_diag.silentEnds = 0;
    g_diag.rackPeaks = {};
    g_diag.bufferPeaks = {};
    g_diag.bufferSquares = {};
    g_diag.bufferSamples = 0;
    g_diag.renders = 0;
}

void DiagVoiceGrain(Voice* voice, u32 numSamples) {
    float inPeak = 0.0f;
    for (u32 ch = 0; ch < voice->NumInputChannels(); ch++) {
        for (u32 i = 0; i < numSamples; i++) {
            inPeak = std::max(inPeak, std::abs(voice->input[ch][i]));
        }
    }
    float outPeak = 0.0f;
    for (u32 ch = 0; ch < voice->NumOutputChannels(); ch++) {
        for (u32 i = 0; i < numSamples; i++) {
            outPeak = std::max(outPeak, std::abs(voice->output[ch][i]));
        }
    }
    auto& peaks = g_diag.rackPeaks[std::min<u32>(voice->rack->rackId >> 12, 4)];
    peaks[0] = std::max(peaks[0], inPeak);
    peaks[1] = std::max(peaks[1], outPeak);
    if (voice->diag.playing) {
        voice->diag.grains++;
        voice->diag.audibleGrains += outPeak > 1e-5f ? 1 : 0;
        voice->diag.peakOut = std::max(voice->diag.peakOut, outPeak);
    }
}

Voice::Voice(Rack* rack, u32 index) : rack(rack), index(index) {
    ports.resize(rack->maxPorts);
    matrices.resize(rack->maxMatrices);
    matrixNumLevels.resize(rack->maxMatrices);
    input.resize(rack->maxInputs * MaxChannels);
    for (auto& buffer : input) {
        buffer.resize(rack->maxGrainSamples);
    }
    for (auto& buffer : output) {
        buffer.resize(rack->maxGrainSamples);
    }
}

void Voice::ResetPorts() {
    for (auto& port : ports) {
        port = {};
    }
}

void Voice::Unpatch(const Voice* target) {
    for (auto& port : ports) {
        if (port.dest == target) {
            port.dest = nullptr;
        }
    }
}

void Voice::GetState(void* outState, size_t stateSize) {
    std::memset(outState, 0, stateSize);
    if (stateSize >= sizeof(u32)) {
        std::memcpy(outState, &stateFlags, sizeof(u32));
    }
}

s32 Voice::HandleEvent(u32 eventId) {
    switch (eventId) {
    case ORBIS_NGS2_VOICE_EVENT_PLAY:
        return ORBIS_OK;
    case ORBIS_NGS2_VOICE_EVENT_PAUSE:
        stateFlags |= ORBIS_NGS2_VOICE_STATE_FLAG_PAUSED;
        return ORBIS_OK;
    case ORBIS_NGS2_VOICE_EVENT_STOP:
    case ORBIS_NGS2_VOICE_EVENT_STOP_IMMEDIATE:
    case ORBIS_NGS2_VOICE_EVENT_KILL:
        stateFlags = 0;
        return ORBIS_OK;
    case ORBIS_NGS2_VOICE_EVENT_RESUME:
        stateFlags &= ~ORBIS_NGS2_VOICE_STATE_FLAG_PAUSED;
        return ORBIS_OK;
    default:
        DiagLog("event {} invalid id {}", DiagVoiceName(this), eventId);
        return ORBIS_NGS2_ERROR_INVALID_EVENT_TYPE;
    }
}

s32 Voice::Control(const OrbisNgs2VoiceParamHeader* param) {
    switch (param->id) {
    case ORBIS_NGS2_VOICE_PARAM_MATRIX_LEVELS: {
        const auto* p = reinterpret_cast<const OrbisNgs2VoiceMatrixLevelsParam*>(param);
        if (p->matrixId >= matrices.size()) {
            return ORBIS_NGS2_ERROR_INVALID_MATRIX_INDEX;
        }
        if (p->numLevels > MaxChannels * MaxChannels) {
            return ORBIS_NGS2_ERROR_INVALID_NUM_MATRIX_LEVELS;
        }
        auto& matrix = matrices[p->matrixId];
        matrix.fill(0.0f);
        if (p->aLevel) {
            std::copy_n(p->aLevel, p->numLevels, matrix.begin());
        }
        matrixNumLevels[p->matrixId] = p->numLevels;
        if (DiagEnabled()) {
            DiagMatrix(this, p->matrixId);
        }
        return ORBIS_OK;
    }
    case ORBIS_NGS2_VOICE_PARAM_PORT_VOLUME: {
        const auto* p = reinterpret_cast<const OrbisNgs2VoicePortVolumeParam*>(param);
        if (p->port >= ports.size()) {
            return ORBIS_NGS2_ERROR_INVALID_PORT_INDEX;
        }
        if (DiagEnabled() && rack->rackId != ORBIS_NGS2_RACK_ID_SAMPLER &&
            ports[p->port].volume != p->level) {
            DiagLog("portvol {} port {} {:.4f} -> {:.4f}", DiagVoiceName(this), p->port,
                    ports[p->port].volume, p->level);
        }
        ports[p->port].volume = p->level;
        return ORBIS_OK;
    }
    case ORBIS_NGS2_VOICE_PARAM_PORT_MATRIX: {
        const auto* p = reinterpret_cast<const OrbisNgs2VoicePortMatrixParam*>(param);
        if (p->port >= ports.size()) {
            return ORBIS_NGS2_ERROR_INVALID_PORT_INDEX;
        }
        if (p->matrixId >= static_cast<s32>(matrices.size())) {
            return ORBIS_NGS2_ERROR_INVALID_MATRIX_INDEX;
        }
        ports[p->port].matrixId = p->matrixId;
        return ORBIS_OK;
    }
    case ORBIS_NGS2_VOICE_PARAM_PORT_DELAY: {
        const auto* p = reinterpret_cast<const OrbisNgs2VoicePortDelayParam*>(param);
        if (p->port >= ports.size()) {
            return ORBIS_NGS2_ERROR_INVALID_PORT_INDEX;
        }
        ports[p->port].numDelaySamples = p->numSamples;
        return ORBIS_OK;
    }
    case ORBIS_NGS2_VOICE_PARAM_PATCH: {
        const auto* p = reinterpret_cast<const OrbisNgs2VoicePatchParam*>(param);
        if (p->port >= ports.size()) {
            return ORBIS_NGS2_ERROR_INVALID_PORT_INDEX;
        }
        if (!p->destHandle) {
            ports[p->port].dest = nullptr;
            return ORBIS_OK;
        }
        Voice* dest = GetVoice(p->destHandle);
        if (Rack* destRack = GetRack(p->destHandle); destRack && !destRack->voices.empty()) {
            dest = destRack->voices[0].get();
        }
        if (!dest || dest == this) {
            LOG_ERROR(Lib_Ngs2, "Invalid patch destination {:#x} (rack: {})", p->destHandle,
                      GetRack(p->destHandle) != nullptr);
            DiagLog("patch {} port {} invalid destination {:#x}", DiagVoiceName(this), p->port,
                    p->destHandle);
            return ORBIS_NGS2_ERROR_INVALID_PATCH;
        }
        ports[p->port].dest = dest;
        ports[p->port].destInputId = p->destInputId;
        if (rack->rackId != ORBIS_NGS2_RACK_ID_SAMPLER) {
            DiagLog("patch {} port {} -> {} input {}", DiagVoiceName(this), p->port,
                    DiagVoiceName(dest), p->destInputId);
        }
        return ORBIS_OK;
    }
    case ORBIS_NGS2_VOICE_PARAM_EVENT: {
        const u32 eventId = reinterpret_cast<const OrbisNgs2VoiceEventParam*>(param)->eventId;
        DiagLog("event {} {} flags {:#x}", DiagVoiceName(this), eventId, stateFlags);
        return HandleEvent(eventId);
    }
    case ORBIS_NGS2_VOICE_PARAM_CALLBACK:
        DiagParam(this, param, "ignored");
        return ORBIS_OK;
    default:
        return ControlModule(param);
    }
}

void Voice::MixToPorts(u32 numSamples) {
    const u32 numOut = NumOutputChannels();
    for (const auto& port : ports) {
        if (!port.dest || port.destInputId >= port.dest->rack->maxInputs || port.volume == 0.0f ||
            !port.dest->IsActive()) {
            continue;
        }
        Voice* dest = port.dest;
        if (diag.playing) {
            float maxLevel = 1.0f;
            if (port.matrixId >= 0 && matrixNumLevels[port.matrixId] > 0) {
                maxLevel = 0.0f;
                for (u32 i = 0; i < matrixNumLevels[port.matrixId]; i++) {
                    maxLevel = std::max(maxLevel, std::abs(matrices[port.matrixId][i]));
                }
            }
            diag.maxSendGain = std::max(diag.maxSendGain, maxLevel * std::abs(port.volume));
        }
        std::vector<float>* destInput = &dest->input[port.destInputId * MaxChannels];
        const u32 numDest = dest->NumInputChannels();
        if (numDest == 0) {
            continue;
        }
        if (port.matrixId >= 0 && matrixNumLevels[port.matrixId] > 0) {
            const auto& matrix = matrices[port.matrixId];
            const u32 numLevels = matrixNumLevels[port.matrixId];
            for (u32 src = 0; src < numOut; src++) {
                for (u32 dst = 0; dst < numDest; dst++) {
                    const u32 level_index = dst * numOut + src;
                    if (level_index >= numLevels) {
                        continue;
                    }
                    const float level = matrix[level_index] * port.volume;
                    if (level == 0.0f) {
                        continue;
                    }
                    const float* in = output[src].data();
                    float* out = destInput[dst].data();
                    for (u32 i = 0; i < numSamples; i++) {
                        out[i] += in[i] * level;
                    }
                }
            }
        } else {
            for (u32 dst = 0; dst < numDest; dst++) {
                const u32 src = numOut == 1 ? 0 : dst;
                if (src >= numOut) {
                    continue;
                }
                const float* in = output[src].data();
                float* out = destInput[dst].data();
                for (u32 i = 0; i < numSamples; i++) {
                    out[i] += in[i] * port.volume;
                }
            }
        }
    }
}

void Voice::Process(u32 numSamples) {
    Render(numSamples);
    if (DiagEnabled()) {
        DiagVoiceGrain(this, numSamples);
    }
    MixToPorts(numSamples);
}

Rack::Rack(System* system, u32 rackId, const OrbisNgs2RackOption* option)
    : system(system), rackId(rackId), maxGrainSamples(system->maxGrainSamples) {
    if (option) {
        maxVoices = std::max(option->maxVoices, 1u);
        maxPorts = std::max(option->maxPorts, 1u);
        maxMatrices = std::max(option->maxMatrices, 1u);
        if (option->maxGrainSamples) {
            maxGrainSamples = std::max(option->maxGrainSamples, system->maxGrainSamples);
        }
        if (rackId == ORBIS_NGS2_RACK_ID_SUBMIXER &&
            option->size >= sizeof(OrbisNgs2SubmixerRackOption)) {
            const auto* submixerOption =
                reinterpret_cast<const OrbisNgs2SubmixerRackOption*>(option);
            maxInputs = std::clamp(submixerOption->maxInputs, 1u, MaxInputs);
        }
    }
    voices.reserve(maxVoices);
    for (u32 i = 0; i < maxVoices; i++) {
        voices.push_back(CreateVoice(this, i));
        g_voices.insert(voices.back().get());
    }
}

System::System(const OrbisNgs2SystemOption* option) {
    if (option) {
        sampleRate = option->sampleRate;
        numGrainSamples = option->numGrainSamples;
        maxGrainSamples = option->maxGrainSamples;
    }
}

void System::UpdateLevelStats(const OrbisNgs2RenderBufferInfo* bufferInfo, u32 numBufferInfo,
                              u32 numActiveVoices) {
    constexpr u32 ReportInterval = 375;
    constexpr u32 PcmF32L = 0x18;
    for (u32 i = 0; i < std::min<u32>(numBufferInfo, levelStats.size()); i++) {
        if (bufferInfo[i].waveformType != PcmF32L || !bufferInfo[i].buffer) {
            continue;
        }
        const auto* samples = static_cast<const float*>(bufferInfo[i].buffer);
        const size_t count = bufferInfo[i].bufferSize / sizeof(float);
        auto& stats = levelStats[i];
        for (size_t j = 0; j < count; j++) {
            stats.peak = std::max(stats.peak, std::abs(samples[j]));
            stats.sumSquares += static_cast<double>(samples[j]) * samples[j];
        }
        stats.count += count;
    }
    if (++levelStatsRenders < ReportInterval) {
        return;
    }
    std::string report;
    for (u32 i = 0; i < std::min<u32>(numBufferInfo, levelStats.size()); i++) {
        auto& stats = levelStats[i];
        const double rms = stats.count ? std::sqrt(stats.sumSquares / stats.count) : 0.0;
        report += fmt::format(" [{}] rms {:.5f} peak {:.5f}", i, rms, stats.peak);
        stats = {};
    }
    LOG_DEBUG(Lib_Ngs2, "render {} voices {}:{}", renderCount, numActiveVoices, report);
    levelStatsRenders = 0;
}

void System::Render(const OrbisNgs2RenderBufferInfo* bufferInfo, u32 numBufferInfo) {
    for (u32 i = 0; i < numBufferInfo; i++) {
        if (bufferInfo[i].buffer && bufferInfo[i].bufferSize) {
            std::memset(bufferInfo[i].buffer, 0, bufferInfo[i].bufferSize);
        }
    }
    currentBuffers = bufferInfo;
    currentNumBuffers = numBufferInfo;
    const u32 numSamples = numGrainSamples;

    std::vector<Voice*> order;
    visitEpoch++;
    const auto visit = [&](auto& self, Voice* voice) -> void {
        if (voice->visitMark == visitEpoch || !voice->IsActive()) {
            return;
        }
        voice->visitMark = visitEpoch;
        for (const auto& port : voice->ports) {
            if (port.dest) {
                self(self, port.dest);
            }
        }
        order.push_back(voice);
    };
    for (Rack* rack : racks) {
        for (auto& voice : rack->voices) {
            visit(visit, voice.get());
        }
    }
    std::reverse(order.begin(), order.end());

    for (Voice* voice : order) {
        for (auto& buffer : voice->input) {
            std::fill_n(buffer.begin(), numSamples, 0.0f);
        }
    }
    for (Voice* voice : order) {
        voice->Process(numSamples);
    }
    UpdateLevelStats(bufferInfo, numBufferInfo, static_cast<u32>(order.size()));
    if (DiagEnabled()) {
        DiagRender(this, bufferInfo, numBufferInfo, numSamples, static_cast<u32>(order.size()));
    }
    currentBuffers = nullptr;
    currentNumBuffers = 0;
    renderCount++;
}

s32 ValidateSystemOption(const OrbisNgs2SystemOption* option) {
    if (!option) {
        return ORBIS_OK;
    }
    if (option->size != sizeof(OrbisNgs2SystemOption)) {
        LOG_ERROR(Lib_Ngs2, "Invalid system option size ({})", option->size);
        return ORBIS_NGS2_ERROR_INVALID_OPTION_SIZE;
    }
    const u32 maxGrainSamples = option->maxGrainSamples;
    const u32 numGrainSamples = option->numGrainSamples;
    const u32 sampleRate = option->sampleRate;
    if (maxGrainSamples < 64 || maxGrainSamples > 1024 || (maxGrainSamples & 63) != 0) {
        LOG_ERROR(Lib_Ngs2, "Invalid system option (maxGrainSamples={},x64)", maxGrainSamples);
        return ORBIS_NGS2_ERROR_INVALID_MAX_GRAIN_SAMPLES;
    }
    if (numGrainSamples < 64 || numGrainSamples > maxGrainSamples || (numGrainSamples & 63) != 0) {
        LOG_ERROR(Lib_Ngs2, "Invalid system option (numGrainSamples={},x64)", numGrainSamples);
        return ORBIS_NGS2_ERROR_INVALID_NUM_GRAIN_SAMPLES;
    }
    if (sampleRate != 11025 && sampleRate != 12000 && sampleRate != 22050 && sampleRate != 24000 &&
        sampleRate != 44100 && sampleRate != 48000 && sampleRate != 88200 && sampleRate != 96000 &&
        sampleRate != 176400 && sampleRate != 192000) {
        LOG_ERROR(Lib_Ngs2, "Invalid system option(sampleRate={}:44.1/48kHz series)", sampleRate);
        return ORBIS_NGS2_ERROR_INVALID_SAMPLE_RATE;
    }
    return ORBIS_OK;
}

System* CreateSystem(const OrbisNgs2SystemOption* option) {
    DiagInit();
    auto* system = new System(option);
    g_systems.insert(system);
    return system;
}

void DestroySystem(System* system) {
    while (!system->racks.empty()) {
        DestroyRack(system->racks.back());
    }
    g_systems.erase(system);
    delete system;
}

s32 ValidateRackId(u32 rackId) {
    switch (rackId) {
    case ORBIS_NGS2_RACK_ID_SAMPLER:
    case ORBIS_NGS2_RACK_ID_SUBMIXER:
    case ORBIS_NGS2_RACK_ID_REVERB:
    case ORBIS_NGS2_RACK_ID_MASTERING:
    case ORBIS_NGS2_RACK_ID_CUSTOM_SUBMIXER:
        return ORBIS_OK;
    default:
        LOG_ERROR(Lib_Ngs2, "Unsupported rack id {:#x}", rackId);
        DiagLog("unsupported rack id {:#x}", rackId);
        return ORBIS_NGS2_ERROR_INVALID_RACK_ID;
    }
}

Rack* CreateRack(System* system, u32 rackId, const OrbisNgs2RackOption* option) {
    auto* rack = new Rack(system, rackId, option);
    g_racks.insert(rack);
    system->racks.push_back(rack);
    return rack;
}

void DestroyRack(Rack* rack) {
    for (Rack* other : rack->system->racks) {
        for (auto& voice : other->voices) {
            for (auto& target : rack->voices) {
                voice->Unpatch(target.get());
            }
        }
    }
    for (auto& voice : rack->voices) {
        g_voices.erase(voice.get());
    }
    std::erase(rack->system->racks, rack);
    g_racks.erase(rack);
    delete rack;
}

} // namespace Libraries::Ngs2
