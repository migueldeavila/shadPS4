// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <vector>

#include "ngs2.h"

namespace Libraries::Ngs2 {

class Ngs2Reverb;

struct OrbisNgs2ReverbRackOption {
    OrbisNgs2RackOption rackOption;
    u32 maxChannels;
    u32 reverbSize;
};

struct OrbisNgs2ReverbI3DL2Param {
    float wet;
    float dry;
    s32 room;
    s32 roomHF;
    u32 reflectionPattern;
    float decayTime;
    float decayHFRatio;
    s32 reflections;
    float reflectionsDelay;
    s32 reverb;
    float reverbDelay;
    float diffusion;
    float density;
    float HFReference;
    u32 reserve[8];
};

struct OrbisNgs2ReverbVoiceSetupParam {
    OrbisNgs2VoiceParamHeader header;

    u32 numInputChannels;
    u32 numOutputChannels;
    u32 flags;
    u32 reserved;
};

struct OrbisNgs2ReverbVoiceI3DL2Param {
    OrbisNgs2VoiceParamHeader header;

    OrbisNgs2ReverbI3DL2Param i3dl2;
};

struct OrbisNgs2ReverbVoiceState {
    OrbisNgs2VoiceState voiceState;
};

struct OrbisNgs2ReverbRackInfo {
    OrbisNgs2RackInfo rackInfo;
    u32 maxChannels;
    u32 reverbSize;
};

class ReverbProcessor {
public:
    void SetParam(const OrbisNgs2ReverbI3DL2Param& param, u32 sampleRate);
    void Process(const std::vector<float>* input, u32 numInputChannels, std::vector<float>* output,
                 u32 numOutputChannels, u32 numSamples);

private:
    struct Filter {
        std::vector<float> buffer;
        u32 position = 0;
        float store = 0.0f;
    };

    float ProcessComb(Filter& comb, float input);
    float ProcessAllpass(Filter& allpass, float input);

    std::array<std::array<Filter, 8>, 2> combs;
    std::array<std::array<Filter, 4>, 2> allpasses;
    Filter preDelay;
    float feedback = 0.0f;
    float damping = 0.0f;
    float wetGain = 0.0f;
    float dryGain = 0.0f;
    u32 sampleRate = 0;
};

} // namespace Libraries::Ngs2
