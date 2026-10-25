// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <vector>
#include "common/types.h"

namespace Libraries::Ngs2 {

struct OrbisNgs2RackOption;
struct OrbisNgs2VoiceParamHeader;
struct OrbisNgs2RenderBufferInfo;

static const int ORBIS_NGS2_SYSTEM_NAME_LENGTH = 16;
static const int ORBIS_NGS2_RACK_NAME_LENGTH = 16;

typedef uintptr_t OrbisNgs2Handle;

struct OrbisNgs2ContextBufferInfo {
    void* hostBuffer;
    size_t hostBufferSize;
    uintptr_t reserved[5];
    uintptr_t userData;
};

struct OrbisNgs2SystemOption {
    size_t size;
    char name[ORBIS_NGS2_SYSTEM_NAME_LENGTH];

    u32 flags;
    u32 maxGrainSamples;
    u32 numGrainSamples;
    u32 sampleRate;
    u32 aReserved[6];
};

using OrbisNgs2BufferAllocHandler =
    s32 PS4_SYSV_ABI (*)(OrbisNgs2ContextBufferInfo* io_buffer_info);
using OrbisNgs2BufferFreeHandler = s32 PS4_SYSV_ABI (*)(OrbisNgs2ContextBufferInfo* io_buffer_info);

struct OrbisNgs2SystemInfo {
    char name[ORBIS_NGS2_SYSTEM_NAME_LENGTH]; // 0

    OrbisNgs2Handle systemHandle;          // 16
    OrbisNgs2ContextBufferInfo bufferInfo; // 24

    u32 uid;             // 88
    u32 minGrainSamples; // 92
    u32 maxGrainSamples; // 96

    u32 stateFlags;        // 100
    u32 rackCount;         // 104
    float lastRenderRatio; // 108
    s64 lastRenderTick;    // 112
    s64 renderCount;       // 120
    u32 sampleRate;        // 128
    u32 numGrainSamples;   // 132
};

struct OrbisNgs2RackInfo {
    char name[ORBIS_NGS2_RACK_NAME_LENGTH]; // 0

    OrbisNgs2Handle rackHandle;            // 16
    OrbisNgs2ContextBufferInfo bufferInfo; // 24

    OrbisNgs2Handle ownerSystemHandle; // 88

    u32 type;            // 96
    u32 rackId;          // 100
    u32 uid;             // 104
    u32 minGrainSamples; // 108
    u32 maxGrainSamples; // 112
    u32 maxVoices;       // 116
    u32 maxChannelWorks; // 120
    u32 maxInputs;       // 124
    u32 maxMatrices;     // 128
    u32 maxPorts;        // 132

    u32 stateFlags;             // 136
    float lastProcessRatio;     // 140
    u64 lastProcessTick;        // 144
    u64 renderCount;            // 152
    u32 activeVoiceCount;       // 160
    u32 activeChannelWorkCount; // 164
};

constexpr u32 ORBIS_NGS2_RACK_ID_SAMPLER = 0x1000;
constexpr u32 ORBIS_NGS2_RACK_ID_SUBMIXER = 0x2000;
constexpr u32 ORBIS_NGS2_RACK_ID_REVERB = 0x2001;
constexpr u32 ORBIS_NGS2_RACK_ID_MASTERING = 0x3000;
constexpr u32 ORBIS_NGS2_RACK_ID_CUSTOM_SAMPLER = 0x4001;
constexpr u32 ORBIS_NGS2_RACK_ID_CUSTOM_SUBMIXER = 0x4002;
constexpr u32 ORBIS_NGS2_RACK_ID_CUSTOM_MASTERING = 0x4003;

constexpr u32 ORBIS_NGS2_VOICE_PARAM_MATRIX_LEVELS = 1;
constexpr u32 ORBIS_NGS2_VOICE_PARAM_PORT_VOLUME = 2;
constexpr u32 ORBIS_NGS2_VOICE_PARAM_PORT_MATRIX = 3;
constexpr u32 ORBIS_NGS2_VOICE_PARAM_PORT_DELAY = 4;
constexpr u32 ORBIS_NGS2_VOICE_PARAM_PATCH = 5;
constexpr u32 ORBIS_NGS2_VOICE_PARAM_EVENT = 6;
constexpr u32 ORBIS_NGS2_VOICE_PARAM_CALLBACK = 7;

constexpr u32 ORBIS_NGS2_VOICE_EVENT_RESET = 0;
constexpr u32 ORBIS_NGS2_VOICE_EVENT_PAUSE = 1;
constexpr u32 ORBIS_NGS2_VOICE_EVENT_STOP = 2;
constexpr u32 ORBIS_NGS2_VOICE_EVENT_KILL = 3;
constexpr u32 ORBIS_NGS2_VOICE_EVENT_RESUME_A = 4;
constexpr u32 ORBIS_NGS2_VOICE_EVENT_RESUME_B = 5;

constexpr u32 ORBIS_NGS2_VOICE_STATE_FLAG_INUSE = 0x1;
constexpr u32 ORBIS_NGS2_VOICE_STATE_FLAG_PLAYING = 0x2;
constexpr u32 ORBIS_NGS2_VOICE_STATE_FLAG_PAUSED = 0x4;
constexpr u32 ORBIS_NGS2_VOICE_STATE_FLAG_STOPPED = 0x8;

constexpr int MaxChannels = 8;

class Rack;
class System;

class Voice {
public:
    Voice(Rack* rack, u32 index);
    virtual ~Voice() = default;

    s32 Control(const OrbisNgs2VoiceParamHeader* param);
    void ResetPorts();
    void Process(u32 numSamples);
    void Unpatch(const Voice* target);

    virtual s32 ControlModule(const OrbisNgs2VoiceParamHeader* param) = 0;
    virtual void GetState(void* outState, size_t stateSize);
    virtual void Render(u32 numSamples) = 0;
    virtual u32 NumInputChannels() const = 0;
    virtual u32 NumOutputChannels() const = 0;
    bool IsActive() const {
        return (stateFlags & ORBIS_NGS2_VOICE_STATE_FLAG_INUSE) &&
               !(stateFlags & ORBIS_NGS2_VOICE_STATE_FLAG_PAUSED);
    }

    Rack* rack;
    u32 index;
    u32 stateFlags = 0;

    struct Port {
        s32 matrixId = -1;
        float volume = 1.0f;
        u32 numDelaySamples = 0;
        Voice* dest = nullptr;
        u32 destInputId = 0;
    };
    std::vector<Port> ports;
    std::vector<std::array<float, MaxChannels * MaxChannels>> matrices;
    std::vector<u32> matrixNumLevels;

    std::vector<std::vector<float>> input;
    std::array<std::vector<float>, MaxChannels> output;
    u32 visitMark = 0;

protected:
    virtual s32 HandleEvent(u32 eventId);
    void MixToPorts(u32 numSamples);
};

class Rack {
public:
    Rack(System* system, u32 rackId, const OrbisNgs2RackOption* option);

    System* system;
    u32 rackId;
    u32 maxVoices = 1;
    u32 maxPorts = 1;
    u32 maxMatrices = 1;
    u32 maxGrainSamples = 512;
    u32 maxInputs = 1;
    OrbisNgs2ContextBufferInfo bufferInfo{};
    OrbisNgs2BufferFreeHandler hostFree = nullptr;
    std::vector<std::unique_ptr<Voice>> voices;
};

class System {
public:
    explicit System(const OrbisNgs2SystemOption* option);

    void Render(const OrbisNgs2RenderBufferInfo* bufferInfo, u32 numBufferInfo);

    u32 sampleRate = 48000;
    u32 numGrainSamples = 256;
    u32 maxGrainSamples = 512;
    OrbisNgs2ContextBufferInfo bufferInfo{};
    OrbisNgs2BufferFreeHandler hostFree = nullptr;
    std::vector<Rack*> racks;
    u64 renderCount = 0;
    u32 visitEpoch = 0;

    const OrbisNgs2RenderBufferInfo* currentBuffers = nullptr;
    u32 currentNumBuffers = 0;

private:
    void UpdateLevelStats(const OrbisNgs2RenderBufferInfo* bufferInfo, u32 numBufferInfo,
                          u32 numActiveVoices);

    struct LevelStats {
        float peak = 0.0f;
        double sumSquares = 0.0;
        size_t count = 0;
    };
    std::array<LevelStats, 4> levelStats{};
    u32 levelStatsRenders = 0;
};

std::recursive_mutex& GetMutex();

System* GetSystem(OrbisNgs2Handle handle);
Rack* GetRack(OrbisNgs2Handle handle);
Voice* GetVoice(OrbisNgs2Handle handle);

s32 ValidateSystemOption(const OrbisNgs2SystemOption* option);
System* CreateSystem(const OrbisNgs2SystemOption* option);
void DestroySystem(System* system);
s32 ValidateRackId(u32 rackId);
Rack* CreateRack(System* system, u32 rackId, const OrbisNgs2RackOption* option);
void DestroyRack(Rack* rack);

std::unique_ptr<Voice> CreateSamplerVoice(Rack* rack, u32 index);
std::unique_ptr<Voice> CreateSubmixerVoice(Rack* rack, u32 index);
std::unique_ptr<Voice> CreateMasteringVoice(Rack* rack, u32 index);

} // namespace Libraries::Ngs2
