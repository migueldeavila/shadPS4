// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <numbers>

#include "ngs2_custom.h"
#include "ngs2_error.h"
#include "ngs2_impl.h"

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"

namespace Libraries::Ngs2 {

namespace {

constexpr float MaxDelaySeconds = 2.0f;

} // namespace

void DelayProcessor::SetParam(const OrbisNgs2CustomVoiceDelayParam& param, u32 sampleRate) {
    dryLevel = param.dryLevel;
    wetLevel = param.wetLevel;
    inputLevel = param.inputLevel;
    feedbackLevel = std::clamp(param.feedbackLevel, 0.0f, 0.99f);
    numTaps = std::min<u32>(param.numTaps, ORBIS_NGS2_CUSTOM_DELAY_MAX_TAPS);

    const u32 maxDelay = static_cast<u32>(MaxDelaySeconds * sampleRate);
    u32 longest = 1;
    for (u32 i = 0; i < numTaps; i++) {
        const float delay = std::max(param.aTap[i].delayTime, 0.0f) * sampleRate / 1000.0f;
        tapDelays[i] = std::clamp(static_cast<u32>(delay), 1u, maxDelay);
        tapLevels[i] = param.aTap[i].tapLevel;
        longest = std::max(longest, tapDelays[i]);
    }

    const float nyquist = sampleRate / 2.0f;
    lowpassCoefficient =
        param.lowpassFc > 0.0f && param.lowpassFc < nyquist
            ? 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> * param.lowpassFc / sampleRate)
            : 1.0f;

    const size_t lineSize = static_cast<size_t>(longest) + 1;
    for (auto& line : lines) {
        if (line.size() < lineSize) {
            line.resize(lineSize, 0.0f);
        }
    }
    writePosition %= static_cast<u32>(lines[0].size());
}

void DelayProcessor::Process(std::vector<float>* channels, u32 numChannels, u32 numSamples) {
    const bool hasWet = numTaps > 0 && (wetLevel != 0.0f || feedbackLevel != 0.0f);
    if (!hasWet || lines[0].empty()) {
        for (u32 ch = 0; ch < numChannels; ch++) {
            float* data = channels[ch].data();
            for (u32 i = 0; i < numSamples; i++) {
                data[i] *= dryLevel;
            }
        }
        return;
    }
    const u32 lineSize = static_cast<u32>(lines[0].size());
    u32 position = writePosition;
    for (u32 ch = 0; ch < numChannels; ch++) {
        float* data = channels[ch].data();
        float* line = lines[ch].data();
        float& lowpass = lowpassState[ch];
        position = writePosition;
        for (u32 i = 0; i < numSamples; i++) {
            float wet = 0.0f;
            for (u32 tap = 0; tap < numTaps; tap++) {
                wet += tapLevels[tap] * line[(position + lineSize - tapDelays[tap]) % lineSize];
            }
            lowpass += (wet - lowpass) * lowpassCoefficient;
            const float in = data[i];
            line[position] = in * inputLevel + lowpass * feedbackLevel;
            data[i] = in * dryLevel + lowpass * wetLevel;
            position = (position + 1) % lineSize;
        }
    }
    writePosition = position;
}

} // namespace Libraries::Ngs2
