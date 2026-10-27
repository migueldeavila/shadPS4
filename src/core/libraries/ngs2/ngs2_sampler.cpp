// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>

#include "ngs2_error.h"
#include "ngs2_impl.h"
#include "ngs2_sampler.h"

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"

extern "C" {
#include <libatrac9.h>
}

namespace Libraries::Ngs2 {

namespace {

constexpr u32 ORBIS_NGS2_SAMPLER_VOICE_PARAM_SETUP = 0x10000000;
constexpr u32 ORBIS_NGS2_SAMPLER_VOICE_PARAM_WAVEFORM_BLOCKS = 0x10000001;
constexpr u32 ORBIS_NGS2_SAMPLER_VOICE_PARAM_WAVEFORM_ADDRESS = 0x10000002;
constexpr u32 ORBIS_NGS2_SAMPLER_VOICE_PARAM_WAVEFORM_FRAME_OFFSET = 0x10000003;
constexpr u32 ORBIS_NGS2_SAMPLER_VOICE_PARAM_EXIT_LOOP = 0x10000004;
constexpr u32 ORBIS_NGS2_SAMPLER_VOICE_PARAM_PITCH = 0x10000005;
constexpr u32 ORBIS_NGS2_SAMPLER_VOICE_PARAM_ENVELOPE = 0x10000006;
constexpr u32 ORBIS_NGS2_SAMPLER_VOICE_PARAM_DISTORTION = 0x10000007;
constexpr u32 ORBIS_NGS2_SAMPLER_VOICE_PARAM_USER_FX = 0x10000008;
constexpr u32 ORBIS_NGS2_SAMPLER_VOICE_PARAM_PEAK_METER = 0x10000009;
constexpr u32 ORBIS_NGS2_SAMPLER_VOICE_PARAM_FILTER = 0x1000000A;
constexpr u32 ORBIS_NGS2_SAMPLER_VOICE_PARAM_NUM_FILTERS = 0x1000000B;

constexpr u32 ORBIS_NGS2_WAVEFORM_TYPE_PCM_I16L = 0x12;
constexpr u32 ORBIS_NGS2_WAVEFORM_TYPE_PCM_F32L = 0x18;
constexpr u32 ORBIS_NGS2_WAVEFORM_TYPE_VAG = 0x1C;
constexpr u32 ORBIS_NGS2_WAVEFORM_TYPE_ATRAC9 = 0x40;

constexpr u32 VagFrameBytes = 16;
constexpr u32 VagFrameSamples = 28;
constexpr u32 VagFlagSilence = 7;

// HE-VAG prediction coefficients from vgmstream (src/coding/psv_decoder.c).
//
// Copyright (c) 2008-2025 Adam Gashlin, Fastelbja, Ronny Elfert, bnnm,
//                         Christopher Snowhill, NicknineTheEagle, bxaimc,
//                         Thealexbarney, CyberBotX, EdnessP, et al
//
// Portions Copyright (c) 2004-2008, Marko Kreen
// Portions Copyright 2001-2007  jagarl / Kazunori Ueno <jagarl@creator.club.ne.jp>
// Portions Copyright (c) 1998, Justin Frankel/Nullsoft Inc.
// Portions Copyright (C) 2006 Nullsoft, Inc.
// Portions Copyright (c) 2005-2007 Paul Hsieh
// Portions Copyright (C) 2000-2004 Leshade Entis, Entis-soft.
// Portions Public Domain originating with Sun Microsystems
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
constexpr std::array<std::array<float, 4>, 128> HevagCoefficients = {{
    {-0.0f, -0.0f, -0.0f, -0.0f},
    {0.9375f, -0.0f, -0.0f, -0.0f},
    {1.796875f, -0.8125f, -0.0f, -0.0f},
    {1.53125f, -0.859375f, -0.0f, -0.0f},
    {1.90625f, -0.9375f, -0.0f, -0.0f},
    {1.7982178f, -0.86169434f, -0.0f, -0.0f},
    {1.770874f, -0.89916992f, -0.0f, -0.0f},
    {1.6992188f, -0.91821289f, -0.0f, -0.0f},
    {1.6031494f, -0.9375f, -0.0f, -0.0f},
    {1.4682617f, -0.9375f, -0.0f, -0.0f},
    {1.3139648f, -0.9375f, -0.0f, -0.0f},
    {1.1424561f, -0.9375f, -0.0f, -0.0f},
    {0.95605469f, -0.9375f, -0.0f, -0.0f},
    {0.75695801f, -0.9375f, -0.0f, -0.0f},
    {0.54785156f, -0.9375f, -0.0f, -0.0f},
    {0.33166504f, -0.9375f, -0.0f, -0.0f},
    {0.11108398f, -0.9375f, -0.0f, -0.0f},
    {-0.11108398f, -0.9375f, -0.0f, -0.0f},
    {-0.33166504f, -0.9375f, -0.0f, -0.0f},
    {-0.54785156f, -0.9375f, -0.0f, -0.0f},
    {-0.75695801f, -0.9375f, -0.0f, -0.0f},
    {-0.95605469f, -0.9375f, -0.0f, -0.0f},
    {-1.1424561f, -0.9375f, -0.0f, -0.0f},
    {-1.3139648f, -0.9375f, -0.0f, -0.0f},
    {-1.4682617f, -0.9375f, -0.0f, -0.0f},
    {-1.6031494f, -0.9375f, -0.0f, -0.0f},
    {-1.6992188f, -0.91821289f, -0.0f, -0.0f},
    {-1.770874f, -0.89916992f, -0.0f, -0.0f},
    {-1.7982178f, -0.86169434f, -0.0f, -0.0f},
    {0.65625f, -1.125f, 0.40625f, -0.375f},
    {-0.78125f, -0.875f, -0.40625f, -0.28125f},
    {-1.28125f, -0.90625f, -0.4375f, -0.125f},
    {-0.020385742f, -0.33227539f, -0.060302734f, -0.066040039f},
    {-0.90698242f, -0.27111816f, -0.28051758f, 0.051757812f},
    {-0.97668457f, -0.38647461f, -0.34350586f, 0.03527832f},
    {0.73461914f, -0.57983398f, 0.32336426f, -0.15844727f},
    {0.46362305f, -0.84790039f, 0.47302246f, -0.1484375f},
    {-1.0054932f, -0.31689453f, -0.25280762f, 0.027709961f},
    {1.1229248f, 0.24194336f, -0.16870117f, -0.28271484f},
    {1.5894775f, -0.37158203f, -0.46289062f, 0.15466309f},
    {1.6005859f, -0.54772949f, -0.2746582f, 0.20324707f},
    {-0.20361328f, -0.45703125f, -0.78808594f, 0.10253906f},
    {0.95446777f, -0.52832031f, 0.25769043f, -0.061767578f},
    {1.168335f, -0.16308594f, -0.092407227f, 0.059448242f},
    {1.2246094f, -0.31274414f, 0.036621094f, 0.024291992f},
    {-0.57922363f, -0.50317383f, -0.66967773f, -0.18225098f},
    {-0.71972656f, 0.2902832f, -0.58435059f, -0.84802246f},
    {-0.14562988f, -1.112915f, -0.15100098f, -0.38012695f},
    {0.33972168f, -0.86767578f, -0.19226074f, -0.17663574f},
    {-0.89526367f, -0.25170898f, -0.27001953f, 0.054443359f},
    {0.7479248f, -0.3145752f, -0.038452148f, -0.0021972656f},
    {1.1544189f, -0.22680664f, 0.012451172f, 0.031494141f},
    {0.96142578f, -0.54724121f, 0.25952148f, -0.065673828f},
    {-0.87548828f, -0.21911621f, -0.25256348f, 0.058837891f},
    {-0.89819336f, -0.2565918f, -0.27258301f, 0.053710938f},
    {-1.1193848f, -0.42834473f, -0.32641602f, -0.047729492f},
    {-0.32202148f, -0.32312012f, -0.23547363f, -0.1998291f},
    {0.2286377f, 1.1209717f, 0.22705078f, -0.70141602f},
    {1.1247559f, 0.22692871f, -0.13720703f, -0.29626465f},
    {1.6118164f, -0.36767578f, -0.50524902f, 0.16723633f},
    {1.5181885f, -0.58496094f, -0.03125f, 0.075927734f},
    {-0.32385254f, -0.13964844f, -0.38842773f, -0.83959961f},
    {1.1390381f, -0.12792969f, -0.10107422f, 0.061889648f},
    {0.20043945f, -0.075683594f, -0.11547852f, -0.51623535f},
    {0.51831055f, -0.92590332f, -0.065063477f, -0.27575684f},
    {-1.097168f, -0.47497559f, -0.34265137f, 0.0053710938f},
    {-0.31274414f, -0.3338623f, -0.21118164f, -0.23181152f},
    {0.38842773f, -0.058959961f, -0.087158203f, -0.17346191f},
    {0.96887207f, -0.46923828f, 0.34436035f, -0.12438965f},
    {1.229126f, -0.31848145f, 0.038330078f, 0.023803711f},
    {1.0253906f, -0.40246582f, 0.18933105f, -0.018920898f},
    {-1.0411377f, -0.33874512f, -0.296875f, -0.041015625f},
    {1.1568604f, -0.22973633f, 0.013183594f, 0.03125f},
    {0.0091552734f, -0.27355957f, -0.036376953f, -0.84680176f},
    {-1.1160889f, -0.5078125f, -0.36169434f, 0.00061035156f},
    {-0.88745117f, -0.23901367f, -0.26318359f, 0.056152344f},
    {-0.33447266f, 0.45715332f, 0.72460938f, -0.13293457f},
    {1.0977783f, 0.23779297f, -0.083374023f, -0.33007812f},
    {1.5992432f, -0.34606934f, -0.47045898f, 0.12878418f},
    {1.164917f, -0.23937988f, 0.015869141f, 0.030517578f},
    {0.64355469f, -0.52124023f, 0.38134766f, -0.38537598f},
    {-0.93945312f, -0.41296387f, -0.3548584f, -0.055664062f},
    {0.89221191f, 0.3079834f, 0.052978516f, -0.30041504f},
    {1.2542725f, -0.34997559f, 0.047729492f, 0.020996094f},
    {1.3354492f, -0.45422363f, 0.081176758f, 0.01184082f},
    {0.0029296875f, -0.037841797f, -0.15405273f, 0.0390625f},
    {-0.99145508f, -0.29431152f, -0.28210449f, -0.033081055f},
    {-1.0389404f, -0.37438965f, -0.28527832f, 0.019897461f},
    {0.039794922f, -0.46948242f, 0.051147461f, -0.1138916f},
    {1.0858154f, 0.26782227f, -0.066040039f, -0.3515625f},
    {1.4737549f, -0.22900391f, -0.24621582f, -0.073364258f},
    {1.0655518f, -0.41784668f, 0.2043457f, -0.020629883f},
    {1.5808105f, -0.46960449f, -0.36706543f, 0.23754883f},
    {1.2253418f, -0.3137207f, 0.036865234f, 0.024169922f},
    {1.1456299f, -0.33654785f, 0.12304688f, 0.0050048828f},
    {-0.57617188f, -0.61108398f, -0.34814453f, -0.14172363f},
    {0.96057129f, -0.52807617f, 0.26062012f, -0.061157227f},
    {0.29907227f, -1.0494385f, 0.15856934f, -0.33935547f},
    {1.2441406f, -0.33728027f, 0.043945312f, 0.022094727f},
    {1.3809814f, -0.51428223f, 0.10168457f, 0.0064697266f},
    {1.239502f, -0.33154297f, 0.042114258f, 0.022583008f},
    {1.1765137f, -0.17297363f, -0.08996582f, 0.058837891f},
    {0.47045898f, -0.5559082f, 0.3470459f, -0.41467285f},
    {0.81774902f, -0.6907959f, 0.27453613f, -0.13110352f},
    {1.3527832f, -0.47705078f, 0.088867188f, 0.009765625f},
    {-0.12524414f, -1.1975098f, -0.098266602f, -0.42260742f},
    {1.269043f, -0.45727539f, 0.16687012f, -0.01171875f},
    {1.2557373f, 0.12060547f, -0.23376465f, -0.17541504f},
    {0.9708252f, 0.47338867f, -0.093261719f, -0.39831543f},
    {1.5489502f, -0.4119873f, -0.40942383f, 0.25378418f},
    {0.81066895f, 0.38647461f, 0.028198242f, -0.25500488f},
    {-0.28662109f, -0.89770508f, -0.23730469f, -0.50317383f},
    {1.1340332f, -0.49304199f, 0.23010254f, -0.030029297f},
    {0.56555176f, -0.78161621f, 0.21337891f, -0.19763184f},
    {1.3729248f, -0.50354004f, 0.097900391f, 0.0074462891f},
    {1.1971436f, -0.27880859f, 0.026733398f, 0.027099609f},
    {1.1884766f, -0.1875f, -0.086181641f, 0.057739258f},
    {1.0302734f, -0.41943359f, 0.19067383f, -0.021484375f},
    {1.1361084f, -0.12463379f, -0.10192871f, 0.062133789f},
    {0.20727539f, -1.1016846f, 0.083984375f, -0.37072754f},
    {1.2468262f, -0.34069824f, 0.044921875f, 0.021850586f},
    {1.0241699f, 0.39648438f, -0.092529297f, -0.36486816f},
    {0.87902832f, 0.40478516f, 0.0056152344f, -0.3190918f},
    {-0.010742188f, -0.95324707f, -0.065673828f, -0.5579834f},
    {0.75598145f, -0.63342285f, 0.33691406f, -0.15197754f},
    {1.5045166f, -0.1574707f, -0.40087891f, 0.030883789f},
    {1.5947266f, -0.49743652f, -0.34472656f, 0.22912598f},
    {0.65100098f, 0.36608887f, 0.094604492f, -0.13818359f},
}};

constexpr u32 MaxEnvelopePoints = 16;

struct Block {
    const u8* data;
    u32 dataSize;
    u32 numRepeats;
    u32 numSkipSamples;
    u32 numSamples;
    uintptr_t userData;
};

class SamplerVoice final : public Voice {
public:
    using Voice::Voice;

    ~SamplerVoice() override {
        ReleaseDecoder();
    }

    s32 ControlModule(const OrbisNgs2VoiceParamHeader* param) override {
        switch (param->id) {
        case ORBIS_NGS2_SAMPLER_VOICE_PARAM_SETUP:
            return Setup(reinterpret_cast<const OrbisNgs2SamplerVoiceSetupParam*>(param));
        case ORBIS_NGS2_SAMPLER_VOICE_PARAM_WAVEFORM_BLOCKS:
            return AddBlocks(
                reinterpret_cast<const OrbisNgs2SamplerVoiceWaveformBlocksParam*>(param));
        case ORBIS_NGS2_SAMPLER_VOICE_PARAM_PITCH:
            pitchRatio = reinterpret_cast<const OrbisNgs2SamplerVoicePitchParam*>(param)->ratio;
            return ORBIS_OK;
        case ORBIS_NGS2_SAMPLER_VOICE_PARAM_ENVELOPE:
            return SetEnvelope(reinterpret_cast<const OrbisNgs2SamplerVoiceEnvelopeParam*>(param));
        case ORBIS_NGS2_SAMPLER_VOICE_PARAM_EXIT_LOOP:
            for (auto& block : blocks) {
                block.numRepeats = 0;
            }
            return ORBIS_OK;
        case ORBIS_NGS2_SAMPLER_VOICE_PARAM_WAVEFORM_ADDRESS:
        case ORBIS_NGS2_SAMPLER_VOICE_PARAM_WAVEFORM_FRAME_OFFSET:
        case ORBIS_NGS2_SAMPLER_VOICE_PARAM_DISTORTION:
        case ORBIS_NGS2_SAMPLER_VOICE_PARAM_USER_FX:
        case ORBIS_NGS2_SAMPLER_VOICE_PARAM_PEAK_METER:
        case ORBIS_NGS2_SAMPLER_VOICE_PARAM_FILTER:
        case ORBIS_NGS2_SAMPLER_VOICE_PARAM_NUM_FILTERS:
            DiagParam(this, param, "ignored");
            return ORBIS_OK;
        default:
            LOG_ERROR(Lib_Ngs2, "Unhandled sampler param {:#x}", param->id);
            DiagParam(this, param, "unhandled");
            return ORBIS_NGS2_ERROR_INVALID_VOICE_CONTROL_ID;
        }
    }

    void GetState(void* outState, size_t stateSize) override {
        OrbisNgs2SamplerVoiceState state{};
        state.voiceState.stateFlags = stateFlags;
        state.envelopeHeight = envelopeHeight;
        state.peakHeight = peakHeight;
        state.numDecodedSamples = numDecodedSamples;
        state.decodedDataSize = decodedDataSize;
        state.userData = currentUserData;
        state.waveformData = currentData;
        std::memset(outState, 0, stateSize);
        std::memcpy(outState, &state, std::min(stateSize, sizeof(state)));
    }

    void Render(u32 numSamples) override {
        const u32 numChannels = format.numChannels;
        for (u32 ch = 0; ch < numChannels; ch++) {
            std::fill_n(output[ch].begin(), numSamples, 0.0f);
        }
        if (!(stateFlags & ORBIS_NGS2_VOICE_STATE_FLAG_PLAYING)) {
            return;
        }
        const double step = static_cast<double>(pitchRatio) * format.sampleRate /
                            static_cast<double>(rack->system->sampleRate);
        float peak = 0.0f;
        u32 written = 0;
        for (; written < numSamples; written++) {
            const size_t position = static_cast<size_t>(readPosition);
            while (position + 1 >= pending[0].size()) {
                if (!DecodeFrame()) {
                    break;
                }
            }
            if (position >= pending[0].size()) {
                break;
            }
            const float frac = static_cast<float>(readPosition - static_cast<double>(position));
            const float gain = NextEnvelopeLevel();
            for (u32 ch = 0; ch < numChannels; ch++) {
                const float a = pending[ch][position];
                const float b = position + 1 < pending[ch].size() ? pending[ch][position + 1] : a;
                const float sample = (a + (b - a) * frac) * gain;
                output[ch][written] = sample;
                peak = std::max(peak, std::abs(sample));
            }
            readPosition += step;
        }
        peakHeight = peak;
        const size_t consumed = static_cast<size_t>(readPosition);
        if (consumed > 0) {
            for (u32 ch = 0; ch < numChannels; ch++) {
                pending[ch].erase(pending[ch].begin(),
                                  pending[ch].begin() + std::min(consumed, pending[ch].size()));
            }
            readPosition -= static_cast<double>(consumed);
        }
        if (written < numSamples || envelopeFinished) {
            LOG_DEBUG(Lib_Ngs2, "sampler voice {} finished: decoded {} samples, {} bytes", index,
                      numDecodedSamples, decodedDataSize);
            if (DiagEnabled()) {
                DiagEnd(envelopeFinished ? "release-done" : "data-end", DiagDetails());
            }
            stateFlags = 0;
        }
    }

    u32 NumInputChannels() const override {
        return 0;
    }

    u32 NumOutputChannels() const override {
        return format.numChannels;
    }

protected:
    s32 HandleEvent(u32 eventId) override {
        if (eventId == ORBIS_NGS2_VOICE_EVENT_PLAY && format.numChannels == 0) {
            DiagLog("event {} {} on voice without setup", DiagVoiceName(this), eventId);
            return ORBIS_NGS2_ERROR_UNINIT_VOICE;
        }
        switch (eventId) {
        case ORBIS_NGS2_VOICE_EVENT_PLAY:
            if (DiagEnabled()) {
                DiagEnd("replay", DiagDetails());
            }
            Rewind();
            stateFlags = ORBIS_NGS2_VOICE_STATE_FLAG_INUSE | ORBIS_NGS2_VOICE_STATE_FLAG_PLAYING;
            if (DiagEnabled()) {
                DiagStart();
                DiagLog("start-info {} {}", DiagVoiceName(this), DiagDetails());
            }
            return ORBIS_OK;
        case ORBIS_NGS2_VOICE_EVENT_STOP:
            if (numReleasePoints > 0 && (stateFlags & ORBIS_NGS2_VOICE_STATE_FLAG_PLAYING)) {
                StartRelease();
                return ORBIS_OK;
            }
            if (DiagEnabled()) {
                DiagEnd("stop", DiagDetails());
            }
            stateFlags = 0;
            return ORBIS_OK;
        case ORBIS_NGS2_VOICE_EVENT_STOP_IMMEDIATE:
        case ORBIS_NGS2_VOICE_EVENT_KILL:
            if (DiagEnabled()) {
                DiagEnd(eventId == ORBIS_NGS2_VOICE_EVENT_KILL ? "kill" : "stop-immediate",
                        DiagDetails());
            }
            return Voice::HandleEvent(eventId);
        default:
            return Voice::HandleEvent(eventId);
        }
    }

private:
    std::string DiagDetails() const {
        return fmt::format("type {:#x} ch {} rate {} pitch {:.3f} env {}+{} h {:.3f}{} decoded {} "
                           "bytes {} queued {}{}",
                           format.waveformType, format.numChannels, format.sampleRate, pitchRatio,
                           numForwardPoints, numReleasePoints, envelopeHeight,
                           releasing ? " releasing" : "", numDecodedSamples, decodedDataSize,
                           blocks.size(), lastError.empty() ? "" : " error " + lastError);
    }

    s32 Setup(const OrbisNgs2SamplerVoiceSetupParam* param) {
        const auto& fmt = param->format;
        if (fmt.numChannels == 0 || fmt.numChannels > MaxChannels) {
            return ORBIS_NGS2_ERROR_INVALID_NUM_CHANNELS;
        }
        if (fmt.sampleRate == 0) {
            return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_SAMPLE_RATE;
        }
        ReleaseDecoder();
        format = {};
        stateFlags = 0;
        if (fmt.waveformType == ORBIS_NGS2_WAVEFORM_TYPE_ATRAC9) {
            for (u32 i = 0; i < ATRAC9_CONFIG_DATA_SIZE; i++) {
                config[i] =
                    static_cast<u8>(fmt.configData >> (8 * (ATRAC9_CONFIG_DATA_SIZE - 1 - i)));
            }
            decoder = Atrac9GetHandle();
            if (Atrac9InitDecoder(decoder, config.data()) != 0) {
                LOG_ERROR(Lib_Ngs2, "Invalid ATRAC9 config {:#x}", fmt.configData);
                ReleaseDecoder();
                return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_CONFIG;
            }
            Atrac9GetCodecInfo(decoder, &codecInfo);
            if (static_cast<u32>(codecInfo.channels) != fmt.numChannels) {
                LOG_ERROR(Lib_Ngs2, "ATRAC9 channel mismatch {} != {}", codecInfo.channels,
                          fmt.numChannels);
                ReleaseDecoder();
                return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_CONFIG;
            }
            frameBuffer.resize(static_cast<size_t>(codecInfo.frameSamples) * codecInfo.channels);
        } else if (fmt.waveformType == ORBIS_NGS2_WAVEFORM_TYPE_VAG) {
            frameBuffer.resize(static_cast<size_t>(VagFrameSamples) * fmt.numChannels);
        } else if (fmt.waveformType != ORBIS_NGS2_WAVEFORM_TYPE_PCM_I16L &&
                   fmt.waveformType != ORBIS_NGS2_WAVEFORM_TYPE_PCM_F32L) {
            LOG_ERROR(Lib_Ngs2, "Unsupported waveform type {:#x} channels {} rate {} config {:#x}",
                      fmt.waveformType, fmt.numChannels, fmt.sampleRate, fmt.configData);
            DiagLog("setup {} UNSUPPORTED type {:#x} channels {} rate {} config {:#x}",
                    DiagVoiceName(this), fmt.waveformType, fmt.numChannels, fmt.sampleRate,
                    fmt.configData);
            return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_TYPE;
        }
        LOG_DEBUG(Lib_Ngs2, "sampler voice {} setup: type {:#x} channels {} rate {} config {:#x}",
                  index, fmt.waveformType, fmt.numChannels, fmt.sampleRate, fmt.configData);
        if (DiagEnabled()) {
            DiagEnd("re-setup", DiagDetails());
            DiagLog("setup {} type {:#x} channels {} rate {} config {:#x} flags {:#x}{}",
                    DiagVoiceName(this), fmt.waveformType, fmt.numChannels, fmt.sampleRate,
                    fmt.configData, param->flags,
                    blocks.empty() ? "" : fmt::format(" (discarding {} blocks)", blocks.size()));
        }
        format = fmt;
        ResetPlayback();
        stateFlags = ORBIS_NGS2_VOICE_STATE_FLAG_INUSE;
        return ORBIS_OK;
    }

    s32 AddBlocks(const OrbisNgs2SamplerVoiceWaveformBlocksParam* param) {
        if (param->numBlocks && !param->aBlock) {
            return ORBIS_NGS2_ERROR_INVALID_WAVEFORM_BLOCK_ADDRESS;
        }
        const auto* base = static_cast<const u8*>(param->data);
        for (u32 i = 0; i < param->numBlocks; i++) {
            const auto& block = param->aBlock[i];
            LOG_DEBUG(Lib_Ngs2,
                      "sampler voice {} block: data {} offset {:#x} size {:#x} repeats {} skip {} "
                      "samples {}",
                      index, param->data, block.dataOffset, block.dataSize, block.numRepeats,
                      block.numSkipSamples, block.numSamples);
            blocks.push_back({base + block.dataOffset, block.dataSize, block.numRepeats,
                              block.numSkipSamples, block.numSamples, block.userData});
            if (DiagEnabled() && format.waveformType == ORBIS_NGS2_WAVEFORM_TYPE_VAG) {
                DiagDumpWaveform(this, base + block.dataOffset, block.dataSize,
                                 fmt::format("type {:#x} ch {} samples {} skip {} repeats {}",
                                             format.waveformType, format.numChannels,
                                             block.numSamples, block.numSkipSamples,
                                             block.numRepeats));
            }
        }
        if (DiagEnabled()) {
            const auto* first = param->numBlocks ? &param->aBlock[0] : nullptr;
            DiagLog("blocks {} n {} flags {:#x} data {} first(off {:#x} size {:#x} rep {} skip {} "
                    "samples {}) queued {} playing {}",
                    DiagVoiceName(this), param->numBlocks, param->flags, param->data,
                    first ? first->dataOffset : 0, first ? first->dataSize : 0,
                    first ? first->numRepeats : 0, first ? first->numSkipSamples : 0,
                    first ? first->numSamples : 0, blocks.size(), diag.playing);
        }
        return ORBIS_OK;
    }

    s32 SetEnvelope(const OrbisNgs2SamplerVoiceEnvelopeParam* param) {
        const u32 numPoints = param->numForwardPoints + param->numReleasePoints;
        if (numPoints > MaxEnvelopePoints) {
            return ORBIS_NGS2_ERROR_INVALID_NUM_ENVELOPE_POINTS;
        }
        if (numPoints && !param->aPoint) {
            return ORBIS_NGS2_ERROR_INVALID_ENVELOPE_POINT_ADDRESS;
        }
        std::copy_n(param->aPoint, numPoints, envelope.begin());
        numForwardPoints = param->numForwardPoints;
        numReleasePoints = param->numReleasePoints;
        envelopeIndex = 0;
        envelopePosition = 0;
        envelopeStart = numPoints ? 0.0f : 1.0f;
        envelopeHeight = envelopeStart;
        envelopeFinished = false;
        releasing = false;
        return ORBIS_OK;
    }

    void StartRelease() {
        releasing = true;
        envelopeIndex = numForwardPoints;
        envelopePosition = 0;
        envelopeStart = envelopeHeight;
    }

    float NextEnvelopeLevel() {
        const u32 end = releasing ? numForwardPoints + numReleasePoints : numForwardPoints;
        if (envelopeIndex >= end) {
            envelopeFinished = releasing;
            return envelopeHeight;
        }
        const auto& point = envelope[envelopeIndex];
        if (point.duration == 0) {
            envelopeHeight = point.height;
        } else {
            const float t = static_cast<float>(envelopePosition) / point.duration;
            envelopeHeight = envelopeStart + (point.height - envelopeStart) * t;
        }
        if (++envelopePosition >= point.duration) {
            envelopeHeight = point.height;
            envelopeStart = point.height;
            envelopePosition = 0;
            envelopeIndex++;
        }
        return envelopeHeight;
    }

    bool DecodeFrame() {
        while (!blocks.empty()) {
            Block& block = blocks.front();
            currentUserData = block.userData;
            const bool samplesDone = block.numSamples && blockSamples >= block.numSamples;
            if (blockOffset >= block.dataSize || samplesDone) {
                if (block.numRepeats > 0) {
                    if (block.numRepeats != 0xFFFFFFFF) {
                        block.numRepeats--;
                    }
                    blockOffset = 0;
                    superframeRemaining = 0;
                    blockSamples = 0;
                    blockSkipped = 0;
                    continue;
                }
                blocks.pop_front();
                blockOffset = 0;
                superframeRemaining = 0;
                blockSamples = 0;
                blockSkipped = 0;
                continue;
            }
            const u8* data = block.data + blockOffset;
            currentData = data;
            u32 frameSamples = 0;
            u32 bytesUsed = 0;
            if (!DecodeData(data, block.dataSize - blockOffset, frameSamples, bytesUsed)) {
                blocks.pop_front();
                blockOffset = 0;
                superframeRemaining = 0;
                blockSamples = 0;
                blockSkipped = 0;
                continue;
            }
            blockOffset += bytesUsed;
            decodedDataSize += bytesUsed;
            u32 first = 0;
            if (blockSkipped < block.numSkipSamples) {
                first = std::min(frameSamples, block.numSkipSamples - blockSkipped);
                blockSkipped += first;
            }
            u32 count = frameSamples - first;
            if (block.numSamples) {
                count = std::min(count, block.numSamples - blockSamples);
            }
            blockSamples += count;
            numDecodedSamples += count;
            const u32 numChannels = format.numChannels;
            for (u32 ch = 0; ch < numChannels; ch++) {
                for (u32 i = 0; i < count; i++) {
                    pending[ch].push_back(frameBuffer[(first + i) * numChannels + ch]);
                }
            }
            if (count > 0) {
                return true;
            }
        }
        return false;
    }

    void DecodeVagFrame(const u8* frame, std::array<float, 4>& history, u32 channel,
                        u32 numChannels) {
        const u32 shift = frame[0] & 0xF;
        const u32 flags = frame[1] & 0xF;
        u32 predictor = (frame[1] & 0xF0) | (frame[0] >> 4);
        if (predictor >= HevagCoefficients.size()) {
            predictor = 0;
        }
        const auto& coef = HevagCoefficients[predictor];
        for (u32 i = 0; i < VagFrameSamples; i++) {
            float sample = 0.0f;
            if (flags < VagFlagSilence) {
                const u8 byte = frame[2 + i / 2];
                const s32 code = static_cast<s16>(((i & 1) ? byte >> 4 : byte & 0xF) << 12);
                sample = static_cast<float>(code >> shift) + history[0] * coef[0] +
                         history[1] * coef[1] + history[2] * coef[2] + history[3] * coef[3];
            }
            history = {sample, history[0], history[1], history[2]};
            frameBuffer[i * numChannels + channel] =
                std::clamp(sample, -32768.0f, 32767.0f) / 32768.0f;
        }
    }

    bool DecodeData(const u8* data, u32 size, u32& outSamples, u32& outBytes) {
        const u32 numChannels = format.numChannels;
        switch (format.waveformType) {
        case ORBIS_NGS2_WAVEFORM_TYPE_ATRAC9: {
            if (superframeRemaining == 0) {
                superframeRemaining = static_cast<u32>(codecInfo.superframeSize);
                superframeFrame = 0;
            }
            if (size < superframeRemaining) {
                return false;
            }
            int bytesUsed = 0;
            const int result = Atrac9DecodeF32(decoder, data, static_cast<int>(superframeRemaining),
                                               frameBuffer.data(), &bytesUsed, 0);
            if (result != 0) {
                lastError =
                    fmt::format("atrac9 {:#x} at {:#x}", static_cast<u32>(result), blockOffset);
                LOG_ERROR(Lib_Ngs2, "ATRAC9 decode failed: {:#x} voice {} offset {:#x} size {:#x}",
                          static_cast<u32>(result), index, blockOffset, size);
                return false;
            }
            u32 used = std::min(static_cast<u32>(bytesUsed), superframeRemaining);
            if (++superframeFrame >= static_cast<u32>(codecInfo.framesInSuperframe)) {
                used = superframeRemaining;
            }
            superframeRemaining -= used;
            outSamples = static_cast<u32>(codecInfo.frameSamples);
            outBytes = used;
            return true;
        }
        case ORBIS_NGS2_WAVEFORM_TYPE_VAG: {
            if (size < VagFrameBytes * numChannels) {
                return false;
            }
            for (u32 ch = 0; ch < numChannels; ch++) {
                DecodeVagFrame(data + ch * VagFrameBytes, vagHistory[ch], ch, numChannels);
            }
            outSamples = VagFrameSamples;
            outBytes = VagFrameBytes * numChannels;
            return true;
        }
        case ORBIS_NGS2_WAVEFORM_TYPE_PCM_I16L:
        case ORBIS_NGS2_WAVEFORM_TYPE_PCM_F32L: {
            const bool isFloat = format.waveformType == ORBIS_NGS2_WAVEFORM_TYPE_PCM_F32L;
            const u32 frameBytes = numChannels * (isFloat ? sizeof(float) : sizeof(s16));
            const u32 frames = std::min<u32>(PcmChunkFrames, size / frameBytes);
            if (frames == 0) {
                return false;
            }
            frameBuffer.resize(static_cast<size_t>(frames) * numChannels);
            for (u32 i = 0; i < frames * numChannels; i++) {
                if (isFloat) {
                    std::memcpy(&frameBuffer[i], data + i * sizeof(float), sizeof(float));
                } else {
                    s16 sample;
                    std::memcpy(&sample, data + i * sizeof(s16), sizeof(s16));
                    frameBuffer[i] = sample / 32768.0f;
                }
            }
            outSamples = frames;
            outBytes = frames * frameBytes;
            return true;
        }
        default:
            return false;
        }
    }

    void Rewind() {
        for (auto& channel : pending) {
            channel.clear();
        }
        readPosition = 0.0;
        blockOffset = 0;
        superframeRemaining = 0;
        blockSamples = 0;
        blockSkipped = 0;
        numDecodedSamples = 0;
        decodedDataSize = 0;
        peakHeight = 0.0f;
        envelopeIndex = 0;
        envelopePosition = 0;
        envelopeStart = numForwardPoints + numReleasePoints ? 0.0f : 1.0f;
        envelopeHeight = envelopeStart;
        envelopeFinished = false;
        releasing = false;
        if (decoder) {
            Atrac9InitDecoder(decoder, config.data());
        }
        vagHistory = {};
    }

    void ResetPlayback() {
        lastError.clear();
        blocks.clear();
        for (auto& channel : pending) {
            channel.clear();
        }
        readPosition = 0.0;
        blockOffset = 0;
        superframeRemaining = 0;
        blockSamples = 0;
        blockSkipped = 0;
        numDecodedSamples = 0;
        decodedDataSize = 0;
        currentUserData = 0;
        currentData = nullptr;
        pitchRatio = 1.0f;
        peakHeight = 0.0f;
        numForwardPoints = 0;
        numReleasePoints = 0;
        envelopeIndex = 0;
        envelopePosition = 0;
        envelopeStart = 1.0f;
        envelopeHeight = 1.0f;
        envelopeFinished = false;
        releasing = false;
    }

    void ReleaseDecoder() {
        if (decoder) {
            Atrac9ReleaseHandle(decoder);
            decoder = nullptr;
        }
    }

    static constexpr u32 PcmChunkFrames = 256;

    OrbisNgs2WaveformFormat format{};
    void* decoder = nullptr;
    u32 superframeRemaining = 0;
    u32 superframeFrame = 0;
    std::array<u8, ATRAC9_CONFIG_DATA_SIZE> config{};
    std::array<std::array<float, 4>, MaxChannels> vagHistory{};
    Atrac9CodecInfo codecInfo{};
    std::vector<float> frameBuffer;
    std::deque<Block> blocks;
    std::array<std::vector<float>, MaxChannels> pending;
    double readPosition = 0.0;
    u32 blockOffset = 0;
    u32 blockSamples = 0;
    u32 blockSkipped = 0;
    u64 numDecodedSamples = 0;
    u64 decodedDataSize = 0;
    uintptr_t currentUserData = 0;
    const void* currentData = nullptr;
    float pitchRatio = 1.0f;
    float peakHeight = 0.0f;

    std::array<OrbisNgs2EnvelopePoint, MaxEnvelopePoints> envelope{};
    u32 numForwardPoints = 0;
    u32 numReleasePoints = 0;
    u32 envelopeIndex = 0;
    u32 envelopePosition = 0;
    float envelopeStart = 1.0f;
    float envelopeHeight = 1.0f;
    bool envelopeFinished = false;
    bool releasing = false;
    std::string lastError;
};

} // namespace

std::unique_ptr<Voice> CreateSamplerVoice(Rack* rack, u32 index) {
    return std::make_unique<SamplerVoice>(rack, index);
}

} // namespace Libraries::Ngs2
