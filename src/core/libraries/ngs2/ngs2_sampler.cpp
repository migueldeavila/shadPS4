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
constexpr std::array<std::array<float, 2>, 5> VagCoefficients = {{
    {0.0f, 0.0f},
    {60.0f / 64.0f, 0.0f},
    {115.0f / 64.0f, -52.0f / 64.0f},
    {98.0f / 64.0f, -55.0f / 64.0f},
    {122.0f / 64.0f, -60.0f / 64.0f},
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
                const u8* frame = data + ch * VagFrameBytes;
                const u32 predictor = std::min<u32>(frame[0] >> 4, VagCoefficients.size() - 1);
                const u32 shift = frame[0] & 0xF;
                const auto& coef = VagCoefficients[predictor];
                auto& history = vagHistory[ch];
                for (u32 i = 0; i < VagFrameSamples; i++) {
                    const u8 byte = frame[2 + i / 2];
                    const s32 nibble = static_cast<s16>(((i & 1) ? byte >> 4 : byte & 0xF) << 12);
                    const float sample = static_cast<float>(nibble >> shift) +
                                         history[0] * coef[0] + history[1] * coef[1];
                    history[1] = history[0];
                    history[0] = sample;
                    frameBuffer[i * numChannels + ch] =
                        std::clamp(sample, -32768.0f, 32767.0f) / 32768.0f;
                }
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
    std::array<std::array<float, 2>, MaxChannels> vagHistory{};
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
