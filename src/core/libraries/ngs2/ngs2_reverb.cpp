// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>

#include "ngs2_error.h"
#include "ngs2_impl.h"
#include "ngs2_reverb.h"

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"

namespace Libraries::Ngs2 {

namespace {

constexpr u32 ReferenceSampleRate = 44100;
constexpr u32 StereoSpread = 23;
constexpr std::array<u32, 8> CombLengths = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
constexpr std::array<u32, 4> AllpassLengths = {556, 441, 341, 225};
constexpr float AllpassFeedback = 0.5f;
constexpr float InputGain = 0.015f;
constexpr float WetScale = 3.0f;
constexpr u32 LfeChannel = 3;

float MillibelToGain(s32 millibel) {
    return std::pow(10.0f, std::clamp(millibel, -10000, 2000) / 2000.0f);
}

} // namespace

void ReverbProcessor::SetParam(const OrbisNgs2ReverbI3DL2Param& param, u32 rate) {
    if (rate != sampleRate) {
        sampleRate = rate;
        const float scale = static_cast<float>(rate) / ReferenceSampleRate;
        for (u32 side = 0; side < combs.size(); side++) {
            const u32 spread = side * StereoSpread;
            for (u32 i = 0; i < CombLengths.size(); i++) {
                combs[side][i] = {};
                combs[side][i].buffer.resize(
                    static_cast<size_t>((CombLengths[i] + spread) * scale));
            }
            for (u32 i = 0; i < AllpassLengths.size(); i++) {
                allpasses[side][i] = {};
                allpasses[side][i].buffer.resize(
                    static_cast<size_t>((AllpassLengths[i] + spread) * scale));
            }
        }
    }

    const float decayTime = std::clamp(param.decayTime, 0.1f, 20.0f);
    float averageLength = 0.0f;
    for (const auto& comb : combs[0]) {
        averageLength += static_cast<float>(comb.buffer.size());
    }
    averageLength /= combs[0].size();
    feedback = std::pow(10.0f, -3.0f * averageLength / (decayTime * rate));
    damping = std::clamp(1.0f - param.decayHFRatio, 0.0f, 0.9f) * 0.5f;
    wetGain = param.wet * MillibelToGain(param.room) * MillibelToGain(param.reverb) * WetScale;
    dryGain = param.dry;

    const float delaySeconds = std::clamp(param.reflectionsDelay + param.reverbDelay, 0.0f, 0.5f);
    const size_t delaySamples = static_cast<size_t>(delaySeconds * rate) + 1;
    if (preDelay.buffer.size() != delaySamples) {
        preDelay = {};
        preDelay.buffer.resize(delaySamples);
    }
}

float ReverbProcessor::ProcessComb(Filter& comb, float in) {
    const float out = comb.buffer[comb.position];
    comb.store = out * (1.0f - damping) + comb.store * damping;
    comb.buffer[comb.position] = in + comb.store * feedback;
    comb.position = (comb.position + 1) % static_cast<u32>(comb.buffer.size());
    return out;
}

float ReverbProcessor::ProcessAllpass(Filter& allpass, float in) {
    const float delayed = allpass.buffer[allpass.position];
    allpass.buffer[allpass.position] = in + delayed * AllpassFeedback;
    allpass.position = (allpass.position + 1) % static_cast<u32>(allpass.buffer.size());
    return delayed - in;
}

void ReverbProcessor::Process(const std::vector<float>* input, u32 numInputChannels,
                              std::vector<float>* output, u32 numOutputChannels, u32 numSamples) {
    for (u32 ch = 0; ch < numOutputChannels; ch++) {
        float* out = output[ch].data();
        if (ch < numInputChannels) {
            const float* in = input[ch].data();
            for (u32 i = 0; i < numSamples; i++) {
                out[i] = in[i] * dryGain;
            }
        } else {
            std::fill_n(out, numSamples, 0.0f);
        }
    }
    if (sampleRate == 0 || wetGain == 0.0f || numOutputChannels == 0) {
        return;
    }

    const u32 numSides = std::min<u32>(numOutputChannels, 2);
    for (u32 i = 0; i < numSamples; i++) {
        float mono = 0.0f;
        for (u32 ch = 0; ch < numInputChannels; ch++) {
            if (ch != LfeChannel) {
                mono += input[ch][i];
            }
        }
        const float delayed = preDelay.buffer[preDelay.position];
        preDelay.buffer[preDelay.position] = mono * InputGain;
        preDelay.position = (preDelay.position + 1) % static_cast<u32>(preDelay.buffer.size());

        for (u32 side = 0; side < numSides; side++) {
            float sum = 0.0f;
            for (auto& comb : combs[side]) {
                sum += ProcessComb(comb, delayed);
            }
            for (auto& allpass : allpasses[side]) {
                sum = ProcessAllpass(allpass, sum);
            }
            output[side][i] += sum * wetGain;
        }
    }
}

} // namespace Libraries::Ngs2
