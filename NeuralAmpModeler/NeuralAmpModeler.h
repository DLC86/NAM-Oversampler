#pragma once

#include "../AudioDSPTools/dsp/ImpulseResponse.h"
#include "../AudioDSPTools/dsp/NoiseGate.h"
#include "../AudioDSPTools/dsp/dsp.h"
#include "../AudioDSPTools/dsp/wav.h"
#include "../AudioDSPTools/dsp/ResamplingContainer/ResamplingContainer.h"
#include "../NeuralAmpModelerCore/NAM/dsp.h"
#include "../NeuralAmpModelerCore/NAM/slimmable.h"

#include "ToneStack.h"

#include "IPlug_include_in_plug_hdr.h"
#include "ISender.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>


const int kNumPresets = 1;
constexpr size_t kNumChannelsMono = 1;
constexpr size_t kNumChannelsStereo = 2;

class NAMSender : public iplug::IPeakAvgSender<>
{
public:
  NAMSender()
  : iplug::IPeakAvgSender<>(-90.0, true, 5.0f, 1.0f, 300.0f, 500.0f)
  {
  }
};

enum EParams
{
  // These need to be the first ones because I use their indices to place
  // their rects in the GUI.
  kInputLevel = 0,
  kNoiseGateThreshold,
  kToneBass,
  kToneMid,
  kToneTreble,
  kOutputLevel,
  // The rest is fine though.
  kNoiseGateActive,
  kEQActive,
  kIRToggle,
  // Input calibration
  kCalibrateInput,
  kInputCalibrationLevel,
  kOutputMode,
  kSlim,
  kOversamplingFactor,
  kAntiAliasFilterPhase,
  kOfflineOversamplingFactor,
  kEQPostNAM,
  kChannelMode,
  kOfflineAntiAliasFilterPhase,
  kNumParams
};

const int numKnobs = 6;

enum ECtrlTags
{
  kCtrlTagModelFileBrowser = 0,
  kCtrlTagIRFileBrowser,
  kCtrlTagInputMeter,
  kCtrlTagOutputMeter,
  kCtrlTagSettingsBox,
  kCtrlTagOutputMode,
  kCtrlTagCalibrateInput,
  kCtrlTagInputCalibrationLevel,
  kCtrlTagSlimmableIcon,
  kCtrlTagSlimOverlayBackdrop,
  kCtrlTagSlimKnob,
  kCtrlTagOversamplingBox,
  kCtrlTagOversampling,
  kCtrlTagAntiAliasFilterPhase,
  kCtrlTagOfflineOversampling,
  kCtrlTagOfflineAntiAliasFilterPhase,
  kCtrlTagEQPostNAM,
  kCtrlTagChannelMode,
  kCtrlTagOversamplingIndicator,
  kNumCtrlTags
};

enum EMsgTags
{
  // These tags are used from UI -> DSP
  kMsgTagClearModel = 0,
  kMsgTagClearIR,
  kMsgTagHighlightColor,
  // The following tags are from DSP -> UI
  kMsgTagLoadFailed,
  kMsgTagLoadedModel,
  kMsgTagLoadedIR,
  kNumMsgTags
};

// Get the sample rate of a NAM model.
// Sometimes, the model doesn't know its own sample rate; this wrapper guesses 48k based on the way that most
// people have used NAM in the past.
double GetNAMSampleRate(const std::unique_ptr<nam::DSP>& model)
{
  // Some models are from when we didn't have sample rate in the model.
  // For those, this wraps with the assumption that they're 48k models, which is probably true.
  const double assumedSampleRate = 48000.0;
  const double reportedEncapsulatedSampleRate = model->GetExpectedSampleRate();
  const double encapsulatedSampleRate =
    reportedEncapsulatedSampleRate <= 0.0 ? assumedSampleRate : reportedEncapsulatedSampleRate;
  return encapsulatedSampleRate;
};

class ResamplingNAM : public nam::DSP
{
public:
  // Resampling wrapper around the NAM models
  ResamplingNAM(std::unique_ptr<nam::DSP> encapsulated, const double expected_sample_rate)
  : nam::DSP(encapsulated->NumInputChannels(), encapsulated->NumOutputChannels(), expected_sample_rate)
  , mEncapsulated(std::move(encapsulated))
  {
    // Get the other information from the encapsulated NAM so that we can tell the outside world about what we're
    // holding.
    if (mEncapsulated->HasLoudness())
    {
      SetLoudness(mEncapsulated->GetLoudness());
    }
    if (mEncapsulated->HasInputLevel())
    {
      SetInputLevel(mEncapsulated->GetInputLevel());
    }
    if (mEncapsulated->HasOutputLevel())
    {
      SetOutputLevel(mEncapsulated->GetOutputLevel());
    }

    // And be ready
    int maxBlockSize = 2048; // Conservative
    Reset(expected_sample_rate, maxBlockSize);
  };

  ~ResamplingNAM() = default;

  void prewarm() override
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    mEncapsulated->prewarm();
  };

  void process(NAM_SAMPLE** input, NAM_SAMPLE** output, const int num_frames) override
  {
    std::lock_guard<std::mutex> lock(mStateMutex);

    if (num_frames > mMaxExternalBlockSize)
      ResetUnlocked(mExternalSampleRate, num_frames);

    if (!IsResamplingActive())
    {
      mEncapsulated->process(input, output, num_frames);
    }
    else
    {
      mResamplingContainer->ProcessBlock(
        input, output, num_frames,
        [this](NAM_SAMPLE** resampledInput, NAM_SAMPLE** resampledOutput, int resampledFrames) {
          mEncapsulated->process(resampledInput, resampledOutput, resampledFrames);
        });
    }
  };

  int GetLatency() const
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    return IsResamplingActive() ? mResamplingContainer->GetLatency() : 0;
  };

  void SetOversamplingFactor(int factor)
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    mRequestedOversamplingFactor = factor < 1 ? 1 : factor;
    if (mEncapsulated)
      ResetUnlocked(mExternalSampleRate, mMaxExternalBlockSize);
  };

  void SetAntiAliasFilterPhase(dsp::EAntiAliasFilterPhase filterPhase)
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    mAntiAliasFilterPhase = filterPhase;
    if (mEncapsulated)
      ResetUnlocked(mExternalSampleRate, mMaxExternalBlockSize);
  };

  void Reset(const double sampleRate, const int maxBlockSize) override
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    ResetUnlocked(sampleRate, maxBlockSize);
  };

  // So that we can let the world know if we're resampling (useful for debugging)
  double GetEncapsulatedSampleRate() const { return GetNAMSampleRate(mEncapsulated); };

  nam::SlimmableModel* GetSlimmableModel() { return dynamic_cast<nam::SlimmableModel*>(mEncapsulated.get()); }
  const nam::SlimmableModel* GetSlimmableModel() const
  {
    return dynamic_cast<const nam::SlimmableModel*>(mEncapsulated.get());
  }

private:
  void ResetUnlocked(const double sampleRate, const int maxBlockSize)
  {
    mExpectedSampleRate = sampleRate;
    mExternalSampleRate = sampleRate;
    mMaxExternalBlockSize = maxBlockSize;

    const double renderingSampleRate = GetRenderingSampleRate(sampleRate);
    const double encapsulatedSampleRate = GetEncapsulatedSampleRate();
    const bool resamplingActive = std::abs(renderingSampleRate - sampleRate) > 1.0e-6;
    const auto maxEncapsulatedBlockSize =
      static_cast<int>(std::ceil(maxBlockSize * renderingSampleRate / sampleRate)) + 1;
    const int timeScale =
      static_cast<int>(std::max(1.0, std::round(renderingSampleRate / encapsulatedSampleRate)));

    mEncapsulated->SetTimeScale(timeScale);

    if (resamplingActive)
    {
      if (mResamplingContainer == nullptr || std::abs(mRenderingSampleRate - renderingSampleRate) > 1.0e-6
          || std::abs(mResamplingBandwidthSampleRate - encapsulatedSampleRate) > 1.0e-6)
      {
        mResamplingContainer = std::make_unique<dsp::ResamplingContainer<NAM_SAMPLE, 1, 32>>(
          renderingSampleRate, mAntiAliasFilterPhase, encapsulatedSampleRate);
        mRenderingSampleRate = renderingSampleRate;
        mResamplingBandwidthSampleRate = encapsulatedSampleRate;
      }
      mResamplingContainer->SetAntiAliasFilterPhase(mAntiAliasFilterPhase);
      mResamplingContainer->Reset(sampleRate, maxBlockSize);
      mEncapsulated->ResetAndPrewarm(renderingSampleRate, maxEncapsulatedBlockSize);
    }
    else
    {
      mResamplingContainer = nullptr;
      mRenderingSampleRate = sampleRate;
      mEncapsulated->ResetAndPrewarm(sampleRate, maxBlockSize);
    }
  };
  double GetRenderingSampleRate(double externalSampleRate) const
  {
    const double encapsulatedSampleRate = GetEncapsulatedSampleRate();
    if (mRequestedOversamplingFactor <= 1)
      return encapsulatedSampleRate;

    const double requestedRenderingSampleRate = externalSampleRate * static_cast<double>(mRequestedOversamplingFactor);
    const double timeScale = std::max(1.0, std::round(requestedRenderingSampleRate / encapsulatedSampleRate));
    return encapsulatedSampleRate * timeScale;
  }

  bool IsResamplingActive() const { return mResamplingContainer != nullptr; };

  // The encapsulated NAM
  std::unique_ptr<nam::DSP> mEncapsulated;
  mutable std::mutex mStateMutex;

  // Stateful real-time resampler for model sample-rate matching and user oversampling.
  std::unique_ptr<dsp::ResamplingContainer<NAM_SAMPLE, 1, 32>> mResamplingContainer;
  double mRenderingSampleRate = 0.0;
  double mResamplingBandwidthSampleRate = 0.0;

  // Used to check that we don't get too large a block to process.
  int mMaxExternalBlockSize = 0;

  // The requested oversampling factor (can override model's natural resampling)
  int mRequestedOversamplingFactor = 1;
  dsp::EAntiAliasFilterPhase mAntiAliasFilterPhase = dsp::EAntiAliasFilterPhase::MinimumPhaseIIR;
  double mExternalSampleRate = 48000.0;
};

class NeuralAmpModeler final : public iplug::Plugin
{
public:
  NeuralAmpModeler(const iplug::InstanceInfo& info);
  ~NeuralAmpModeler();

  void ProcessBlock(iplug::sample** inputs, iplug::sample** outputs, int nFrames) override;
  void OnReset() override;
  void OnIdle() override;

  bool SerializeState(iplug::IByteChunk& chunk) const override;
  int UnserializeState(const iplug::IByteChunk& chunk, int startPos) override;
  void OnUIOpen() override;
  bool OnHostRequestingSupportedViewConfiguration(int width, int height) override { return true; }

  void OnParamChange(int paramIdx) override;
  void OnParamChangeUI(int paramIdx, iplug::EParamSource source) override;
  bool OnMessage(int msgTag, int ctrlTag, int dataSize, const void* pData) override;

private:
  // Allocates mInputPointers and mOutputPointers
  void _AllocateIOPointers(const size_t nChans);
  // Moves DSP modules from staging area to the main area.
  // Also deletes DSP modules that are flagged for removal.
  // Exists so that we don't try to use a DSP module that's only
  // partially-instantiated.
  void _ApplyDSPStaging();
  // Deallocates mInputPointers and mOutputPointers
  void _DeallocateIOPointers();
  // Fallback that just copies inputs to outputs if mDSP doesn't hold a model.
  void _FallbackDSP(iplug::sample** inputs, iplug::sample** outputs, const size_t numChannels, const size_t numFrames);
  // Sizes based on mInputArray
  size_t _GetBufferNumChannels() const;
  size_t _GetBufferNumFrames() const;
  void _InitToneStack();
  bool _IsStereoRequested() const;
  bool _CanProcessStereo(const size_t nChansIn, const size_t nChansOut) const;
  void _SetStereoProcessingFromParam();
  std::unique_ptr<ResamplingNAM> _CreateModel(const WDL_String& modelPath);
  // Loads a NAM model and stores it to mStagedNAM
  // Returns an empty string on success, or an error message on failure.
  std::string _StageModel(const WDL_String& dspFile);
  // Loads an IR and stores it to mStagedIR.
  // Return status code so that error messages can be relayed if
  // it wasn't successful.
  dsp::wav::LoadReturnCode _StageIR(const WDL_String& irPath);

  bool _HaveModel() const { return this->mModel != nullptr; };
  // Prepare the input & output buffers
  void _PrepareBuffers(const size_t numChannels, const size_t numFrames);
  // Manage pointers
  void _PrepareIOPointers(const size_t nChans);
  // Copy the input buffer to the object, collapsing to the plugin's internal channel layout.
  // :param nChansIn: In from external
  // :param nChansOut: Out to the internal of the DSP routine
  void _ProcessInput(iplug::sample** inputs, const size_t nFrames, const size_t nChansIn, const size_t nChansOut);
  void _ApplyInputGain(iplug::sample** inputs, const size_t nFrames, const size_t nChans);
  // Copy the output to the output buffer, applying output level.
  // :param nChansIn: In from internal
  // :param nChansOut: Out to external
  void _ProcessOutput(iplug::sample** inputs, iplug::sample** outputs, const size_t nFrames, const size_t nChansIn,
                      const size_t nChansOut);
  // Resetting for models and IRs, called by OnReset
  void _ResetModelAndIR(const double sampleRate, const int maxBlockSize);

  void _SetInputGain();
  void _SetOutputGain();
  void _ApplySlimParamToLoadedNAMs();
  int _GetActiveOversamplingFactor() const;
  int _GetAntiAliasFilterPhaseIndex() const;
  void _ApplyActiveDSPSettings(bool allowSmoothRealtimeTransition);
  void _ApplyImmediateDSPSettings(int oversamplingFactor, int filterPhaseIndex);
  void _PrepareRealtimeDSPTransition(const double sampleRate);
  void _ApplyRealtimeDSPTransitionGain(iplug::sample** outputs, const size_t nFrames, const size_t nChans);

  // See: Unserialization.cpp
  void _UnserializeApplyConfig(nlohmann::json& config);
  // 0.7.9 and later
  int _UnserializeStateWithKnownVersion(const iplug::IByteChunk& chunk, int startPos);
  // Hopefully 0.7.3-0.7.8, but no gurantees
  int _UnserializeStateWithUnknownVersion(const iplug::IByteChunk& chunk, int startPos);

  // Update all controls that depend on a model
  void _UpdateControlsFromModel();

  // Make sure that the latency is reported correctly.
  void _UpdateLatency();

  // Update level meters
  // Called within ProcessBlock().
  // Assume _ProcessInput() and _ProcessOutput() were run immediately before.
  void _UpdateMeters(iplug::sample** inputPointer, iplug::sample** outputPointer, const size_t nFrames,
                     const size_t nChansIn, const size_t nChansOut);

  // Member data

  // Input arrays to NAM
  std::vector<std::vector<iplug::sample>> mInputArray;
  // Output from NAM
  std::vector<std::vector<iplug::sample>> mOutputArray;
  // Pointer versions
  iplug::sample** mInputPointers = nullptr;
  iplug::sample** mOutputPointers = nullptr;

  // Input and output gain
  double mInputGain = 1.0;
  double mOutputGain = 1.0;

  // Noise gates
  dsp::noise_gate::Trigger mNoiseGateTrigger;
  dsp::noise_gate::Gain mNoiseGateGain;
  // The model actually being used:
  std::unique_ptr<ResamplingNAM> mModel;
  std::unique_ptr<ResamplingNAM> mModelRight;
  // And the IR
  std::unique_ptr<dsp::ImpulseResponse> mIR;
  std::unique_ptr<dsp::ImpulseResponse> mIRRight;
  // Manages switching what DSP is being used.
  std::unique_ptr<ResamplingNAM> mStagedModel;
  std::unique_ptr<ResamplingNAM> mStagedModelRight;
  std::unique_ptr<dsp::ImpulseResponse> mStagedIR;
  std::unique_ptr<dsp::ImpulseResponse> mStagedIRRight;
  std::mutex mDSPStagingMutex;
  // Flags to take away the modules at a safe time.
  std::atomic<bool> mShouldRemoveModel = false;
  std::atomic<bool> mShouldRemoveIR = false;

  std::atomic<bool> mNewModelLoadedInDSP = false;
  std::atomic<bool> mModelCleared = false;

  // Tone stack modules
  std::unique_ptr<dsp::tone_stack::AbstractToneStack> mToneStack;

  // Post-IR filters
  recursive_linear_filter::HighPass mHighPass;
  //  recursive_linear_filter::LowPass mLowPass;

  // Oversampling factor (1, 2, 4, 8, 16, 32)
  std::atomic<int> mOversamplingFactor = 1;
  std::atomic<int> mOfflineOversamplingFactor = 1;
  int mAppliedOversamplingFactor = 1;
  int mAppliedAntiAliasFilterPhase = 0;
  std::atomic<int> mAntiAliasFilterPhaseIndex = 0;
  std::atomic<int> mOfflineAntiAliasFilterPhaseIndex = 0;
  std::atomic<bool> mEQPostNAM = true;
  std::atomic<bool> mStereoProcessing = false;
  std::atomic<int> mPendingOversamplingFactor = 0;
  std::atomic<int> mPendingAntiAliasFilterPhase = -1;
  bool mRealtimeDSPTransitionFadingOut = false;
  bool mRealtimeDSPTransitionFadingIn = false;
  int mRealtimeDSPTransitionSamplesRemaining = 0;
  int mRealtimeDSPTransitionLength = 480;

  // Path to model's config.json or model.nam
  WDL_String mNAMPath;
  // Path to IR (.wav file)
  WDL_String mIRPath;

  WDL_String mHighLightColor{"#5085e8"};

  std::unordered_map<std::string, double> mNAMParams = {{"Input", 0.0}, {"Output", 0.0}};

  iplug::sample* mStereoIRPointers[kNumChannelsStereo] = {nullptr, nullptr};

  NAMSender mInputSender, mOutputSender;
};
