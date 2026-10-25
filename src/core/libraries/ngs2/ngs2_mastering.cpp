// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "ngs2_error.h"
#include "ngs2_impl.h"
#include "ngs2_mastering.h"

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"

namespace Libraries::Ngs2 {

namespace {

constexpr u32 ORBIS_NGS2_MASTERING_VOICE_PARAM_SETUP = 0x30000000;
constexpr u32 ORBIS_NGS2_MASTERING_VOICE_PARAM_MATRIX = 0x30000001;
constexpr u32 ORBIS_NGS2_MASTERING_VOICE_PARAM_LFE = 0x30000002;
constexpr u32 ORBIS_NGS2_MASTERING_VOICE_PARAM_LIMITER = 0x30000003;
constexpr u32 ORBIS_NGS2_MASTERING_VOICE_PARAM_GAIN = 0x30000004;
constexpr u32 ORBIS_NGS2_MASTERING_VOICE_PARAM_OUTPUT = 0x30000005;
constexpr u32 ORBIS_NGS2_MASTERING_VOICE_PARAM_PEAK_METER = 0x30000006;

constexpr u32 ORBIS_NGS2_WAVEFORM_TYPE_PCM_I16L = 0x12;
constexpr u32 ORBIS_NGS2_WAVEFORM_TYPE_PCM_F32L = 0x18;

constexpr u32 LfeChannel = 3;

class MasteringVoice final : public Voice {
public:
    using Voice::Voice;

    s32 ControlModule(const OrbisNgs2VoiceParamHeader* param) override {
        switch (param->id) {
        case ORBIS_NGS2_MASTERING_VOICE_PARAM_SETUP: {
            const auto* p = reinterpret_cast<const OrbisNgs2MasteringVoiceSetupParam*>(param);
            if (p->numInputChannels == 0 || p->numInputChannels > MaxChannels) {
                return ORBIS_NGS2_ERROR_INVALID_NUM_CHANNELS;
            }
            numInputChannels = p->numInputChannels;
            stateFlags = ORBIS_NGS2_VOICE_STATE_FLAG_INUSE;
            return ORBIS_OK;
        }
        case ORBIS_NGS2_MASTERING_VOICE_PARAM_GAIN: {
            const auto* p = reinterpret_cast<const OrbisNgs2MasteringVoiceGainParam*>(param);
            fbwLevel = p->fbwLevel;
            lfeLevel = p->lfeLevel;
            return ORBIS_OK;
        }
        case ORBIS_NGS2_MASTERING_VOICE_PARAM_LIMITER: {
            const auto* p = reinterpret_cast<const OrbisNgs2MasteringVoiceLimiterParam*>(param);
            limiterEnabled = p->enableFlag != 0;
            limiterThreshold = p->threshold;
            return ORBIS_OK;
        }
        case ORBIS_NGS2_MASTERING_VOICE_PARAM_OUTPUT: {
            const auto* p = reinterpret_cast<const OrbisNgs2MasteringVoiceOutputParam*>(param);
            outputId = p->outputId;
            return ORBIS_OK;
        }
        case ORBIS_NGS2_MASTERING_VOICE_PARAM_MATRIX:
        case ORBIS_NGS2_MASTERING_VOICE_PARAM_LFE:
        case ORBIS_NGS2_MASTERING_VOICE_PARAM_PEAK_METER:
            return ORBIS_OK;
        default:
            LOG_ERROR(Lib_Ngs2, "Unhandled mastering param {:#x}", param->id);
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ID;
        }
    }

    void Render(u32 numSamples) override {
        const System* system = rack->system;
        if (outputId >= system->currentNumBuffers) {
            return;
        }
        const OrbisNgs2RenderBufferInfo& info = system->currentBuffers[outputId];
        if (!info.buffer || info.numChannels == 0) {
            return;
        }
        const u32 numOut = std::min<u32>(info.numChannels, MaxChannels);
        const size_t sampleSize =
            info.waveformType == ORBIS_NGS2_WAVEFORM_TYPE_PCM_I16L ? sizeof(s16) : sizeof(float);
        const u32 frames =
            std::min<u32>(numSamples, static_cast<u32>(info.bufferSize / (sampleSize * numOut)));
        const float limit = limiterEnabled && limiterThreshold > 0.0f ? limiterThreshold : 1.0f;

        for (u32 ch = 0; ch < numOut; ch++) {
            const float level = ch == LfeChannel && numOut > LfeChannel ? lfeLevel : fbwLevel;
            const float* in = input[std::min(ch, numInputChannels - 1)].data();
            for (u32 i = 0; i < frames; i++) {
                const float sample = std::clamp(in[i] * level, -limit, limit);
                const size_t index = static_cast<size_t>(i) * info.numChannels + ch;
                if (info.waveformType == ORBIS_NGS2_WAVEFORM_TYPE_PCM_F32L) {
                    static_cast<float*>(info.buffer)[index] += sample;
                } else if (info.waveformType == ORBIS_NGS2_WAVEFORM_TYPE_PCM_I16L) {
                    auto& out = static_cast<s16*>(info.buffer)[index];
                    out =
                        static_cast<s16>(std::clamp(out + sample * 32767.0f, -32768.0f, 32767.0f));
                }
            }
        }
    }

    u32 NumInputChannels() const override {
        return numInputChannels;
    }

    u32 NumOutputChannels() const override {
        return 0;
    }

private:
    u32 numInputChannels = 0;
    u32 outputId = 0;
    float fbwLevel = 1.0f;
    float lfeLevel = 1.0f;
    bool limiterEnabled = false;
    float limiterThreshold = 1.0f;
};

} // namespace

std::unique_ptr<Voice> CreateMasteringVoice(Rack* rack, u32 index) {
    return std::make_unique<MasteringVoice>(rack, index);
}

} // namespace Libraries::Ngs2
