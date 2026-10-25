// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "ngs2_custom.h"
#include "ngs2_error.h"
#include "ngs2_impl.h"
#include "ngs2_reverb.h"
#include "ngs2_submixer.h"

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"

namespace Libraries::Ngs2 {

namespace {

constexpr u32 ORBIS_NGS2_SUBMIXER_VOICE_PARAM_SETUP = 0x20000000;
constexpr u32 ORBIS_NGS2_SUBMIXER_VOICE_PARAM_ENVELOPE = 0x20000001;
constexpr u32 ORBIS_NGS2_SUBMIXER_VOICE_PARAM_COMPRESSOR = 0x20000002;
constexpr u32 ORBIS_NGS2_SUBMIXER_VOICE_PARAM_DISTORTION = 0x20000003;
constexpr u32 ORBIS_NGS2_SUBMIXER_VOICE_PARAM_USER_FX = 0x20000004;
constexpr u32 ORBIS_NGS2_SUBMIXER_VOICE_PARAM_PEAK_METER = 0x20000005;
constexpr u32 ORBIS_NGS2_SUBMIXER_VOICE_PARAM_FILTER = 0x20000006;
constexpr u32 ORBIS_NGS2_SUBMIXER_VOICE_PARAM_NUM_FILTERS = 0x20000007;

constexpr u32 ORBIS_NGS2_REVERB_VOICE_PARAM_SETUP = 0x20010000;
constexpr u32 ORBIS_NGS2_REVERB_VOICE_PARAM_I3DL2 = 0x20010001;

constexpr u32 ORBIS_NGS2_CUSTOM_SUBMIXER_VOICE_PARAM_SETUP = 0x40020000;
constexpr u32 ORBIS_NGS2_CUSTOM_VOICE_PARAM_MODULE_MASK = 0xFFFF0000;
constexpr u32 ORBIS_NGS2_CUSTOM_VOICE_PARAM_MODULE = 0x40000000;

constexpr u32 CustomModuleDelay = 0x1D;
constexpr u32 UserFxFlagReset = 0x1;
constexpr u32 MaxInputs = 4;

class SubmixerVoice final : public Voice {
public:
    using Voice::Voice;

    s32 ControlModule(const OrbisNgs2VoiceParamHeader* param) override {
        switch (param->id) {
        case ORBIS_NGS2_SUBMIXER_VOICE_PARAM_SETUP: {
            const auto* p = reinterpret_cast<const OrbisNgs2SubmixerVoiceSetupParam*>(param);
            if (p->numIoChannels == 0 || p->numIoChannels > MaxChannels) {
                return ORBIS_NGS2_ERROR_INVALID_NUM_CHANNELS;
            }
            Setup(p->numIoChannels, p->numIoChannels);
            return ORBIS_OK;
        }
        case ORBIS_NGS2_REVERB_VOICE_PARAM_SETUP: {
            const auto* p = reinterpret_cast<const OrbisNgs2ReverbVoiceSetupParam*>(param);
            if (p->numInputChannels == 0 || p->numInputChannels > MaxChannels ||
                p->numOutputChannels == 0 || p->numOutputChannels > MaxChannels) {
                return ORBIS_NGS2_ERROR_INVALID_NUM_CHANNELS;
            }
            Setup(p->numInputChannels, p->numOutputChannels);
            return ORBIS_OK;
        }
        case ORBIS_NGS2_CUSTOM_SUBMIXER_VOICE_PARAM_SETUP: {
            const auto* p = reinterpret_cast<const OrbisNgs2CustomSubmixerVoiceSetupParam*>(param);
            if (p->numInputChannels == 0 || p->numInputChannels > MaxChannels ||
                p->numOutputChannels == 0 || p->numOutputChannels > MaxChannels) {
                return ORBIS_NGS2_ERROR_INVALID_NUM_CHANNELS;
            }
            Setup(p->numInputChannels, p->numOutputChannels);
            return ORBIS_OK;
        }
        case ORBIS_NGS2_SUBMIXER_VOICE_PARAM_USER_FX: {
            const auto* p = reinterpret_cast<const OrbisNgs2SubmixerVoiceUserFxParam*>(param);
            userFxHandler = p->handler;
            userFxData = {p->userData0, p->userData1, p->userData2};
            userFxFlags = UserFxFlagReset;
            DiagLog("userfx {} handler {} data {:#x} {:#x} {:#x}", DiagVoiceName(this),
                    fmt::ptr(p->handler), p->userData0, p->userData1, p->userData2);
            return ORBIS_OK;
        }
        case ORBIS_NGS2_REVERB_VOICE_PARAM_I3DL2: {
            const auto* p = reinterpret_cast<const OrbisNgs2ReverbVoiceI3DL2Param*>(param);
            reverb.SetParam(p->i3dl2, rack->system->sampleRate);
            return ORBIS_OK;
        }
        case ORBIS_NGS2_SUBMIXER_VOICE_PARAM_ENVELOPE:
        case ORBIS_NGS2_SUBMIXER_VOICE_PARAM_COMPRESSOR:
        case ORBIS_NGS2_SUBMIXER_VOICE_PARAM_DISTORTION:
        case ORBIS_NGS2_SUBMIXER_VOICE_PARAM_PEAK_METER:
        case ORBIS_NGS2_SUBMIXER_VOICE_PARAM_FILTER:
        case ORBIS_NGS2_SUBMIXER_VOICE_PARAM_NUM_FILTERS:
            DiagParam(this, param, "ignored");
            return ORBIS_OK;
        default:
            if ((param->id & ORBIS_NGS2_CUSTOM_VOICE_PARAM_MODULE_MASK) ==
                ORBIS_NGS2_CUSTOM_VOICE_PARAM_MODULE) {
                return ControlCustomModule(param);
            }
            LOG_ERROR(Lib_Ngs2, "Unhandled submixer param {:#x}", param->id);
            DiagParam(this, param, "unhandled");
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ID;
        }
    }

    void Render(u32 numSamples) override {
        if (rack->rackId == ORBIS_NGS2_RACK_ID_REVERB) {
            reverb.Process(input.data(), numInputChannels, output.data(), numOutputChannels,
                           numSamples);
        } else {
            for (u32 ch = 0; ch < numOutputChannels; ch++) {
                float* out = output[ch].data();
                if (ch < numInputChannels) {
                    std::copy_n(input[ch].begin(), numSamples, out);
                } else {
                    std::fill_n(out, numSamples, 0.0f);
                }
            }
            if (hasDelay) {
                delay.Process(output.data(), numOutputChannels, numSamples);
            }
        }
        if (userFxHandler) {
            std::array<float*, MaxChannels * MaxInputs> channels{};
            for (u32 ch = 0; ch < MaxChannels; ch++) {
                channels[ch] = output[ch].data();
            }
            for (u32 i = MaxChannels; i < channels.size(); i++) {
                if (i < input.size()) {
                    channels[i] = input[i].data();
                } else {
                    silence.resize(rack->maxGrainSamples);
                    channels[i] = silence.data();
                }
            }
            OrbisNgs2UserFxProcessContext context{};
            context.aChannelData = channels.data();
            context.userData0 = userFxData[0];
            context.userData1 = userFxData[1];
            context.userData2 = userFxData[2];
            context.numChannels = numOutputChannels;
            context.numGrainSamples = numSamples;
            context.sampleRate = rack->system->sampleRate;
            context.flags = userFxFlags;
            userFxFlags = 0;
            userFxHandler(&context);
        }
    }

    u32 NumInputChannels() const override {
        return numInputChannels;
    }

    u32 NumOutputChannels() const override {
        return numOutputChannels;
    }

private:
    s32 ControlCustomModule(const OrbisNgs2VoiceParamHeader* param) {
        const u32 moduleId = (param->id >> 8) & 0xFF;
        if (moduleId == CustomModuleDelay &&
            param->size >= sizeof(OrbisNgs2CustomVoiceDelayParam)) {
            delay.SetParam(*reinterpret_cast<const OrbisNgs2CustomVoiceDelayParam*>(param),
                           rack->system->sampleRate);
            hasDelay = true;
            return ORBIS_OK;
        }
        DiagParam(this, param, "ignored");
        return ORBIS_OK;
    }

    void Setup(u32 numInputs, u32 numOutputs) {
        numInputChannels = numInputs;
        numOutputChannels = numOutputs;
        DiagLog("setup {} in {} out {} maxInputs {}", DiagVoiceName(this), numInputs, numOutputs,
                rack->maxInputs);
        stateFlags = ORBIS_NGS2_VOICE_STATE_FLAG_INUSE;
        userFxFlags = UserFxFlagReset;
    }

    u32 numInputChannels = 0;
    u32 numOutputChannels = 0;
    ReverbProcessor reverb;
    DelayProcessor delay;
    bool hasDelay = false;
    OrbisNgs2UserFxProcessHandler userFxHandler = nullptr;
    std::array<uintptr_t, 3> userFxData{};
    u32 userFxFlags = 0;
    std::vector<float> silence;
};

} // namespace

std::unique_ptr<Voice> CreateSubmixerVoice(Rack* rack, u32 index) {
    return std::make_unique<SubmixerVoice>(rack, index);
}

} // namespace Libraries::Ngs2
