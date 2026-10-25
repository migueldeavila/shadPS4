// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_set>
#include <fmt/format.h>

#include "ngs2.h"
#include "ngs2_error.h"
#include "ngs2_impl.h"
#include "ngs2_submixer.h"

#include "common/logging/log.h"
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
    case ORBIS_NGS2_VOICE_EVENT_RESET:
        return ORBIS_OK;
    case ORBIS_NGS2_VOICE_EVENT_PAUSE:
        stateFlags |= ORBIS_NGS2_VOICE_STATE_FLAG_PAUSED;
        return ORBIS_OK;
    case ORBIS_NGS2_VOICE_EVENT_STOP:
    case ORBIS_NGS2_VOICE_EVENT_KILL:
        stateFlags = 0;
        return ORBIS_OK;
    case ORBIS_NGS2_VOICE_EVENT_RESUME_A:
    case ORBIS_NGS2_VOICE_EVENT_RESUME_B:
        stateFlags &= ~ORBIS_NGS2_VOICE_STATE_FLAG_PAUSED;
        return ORBIS_OK;
    default:
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
        return ORBIS_OK;
    }
    case ORBIS_NGS2_VOICE_PARAM_PORT_VOLUME: {
        const auto* p = reinterpret_cast<const OrbisNgs2VoicePortVolumeParam*>(param);
        if (p->port >= ports.size()) {
            return ORBIS_NGS2_ERROR_INVALID_PORT_INDEX;
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
            return ORBIS_NGS2_ERROR_INVALID_PATCH;
        }
        ports[p->port].dest = dest;
        ports[p->port].destInputId = p->destInputId;
        return ORBIS_OK;
    }
    case ORBIS_NGS2_VOICE_PARAM_EVENT:
        return HandleEvent(reinterpret_cast<const OrbisNgs2VoiceEventParam*>(param)->eventId);
    case ORBIS_NGS2_VOICE_PARAM_CALLBACK:
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
