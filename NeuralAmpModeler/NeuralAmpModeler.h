
#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <avrt.h>
  #pragma comment(lib, "Avrt.lib")
#endif

#pragma once

#include "../AudioDSPTools/dsp/ImpulseResponse.h"
#include "../AudioDSPTools/dsp/NoiseGate.h"
#include "../AudioDSPTools/dsp/dsp.h"
#include "../AudioDSPTools/dsp/wav.h"
#include "../AudioDSPTools/dsp/ResamplingContainer/ResamplingContainer.h"
#include "../NeuralAmpModelerCore/NAM/dsp.h"
#include "../NeuralAmpModelerCore/NAM/get_dsp.h"
#include "../NeuralAmpModelerCore/NAM/slimmable.h"

#if defined(NAM_ENABLE_A2_FAST)
  #include "../NeuralAmpModelerCore/NAM/wavenet/a2_fast.h"
#endif

#include "ToneStack.h"

#include "IPlug_include_in_plug_hdr.h"
#include "ISender.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>
#include <thread>
#include <string>
#include <stdexcept>
#include <functional>
#include <filesystem>
#include <cstdlib>
#include <condition_variable>


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
  kPhaseMulticoreEnabled,
  kPhaseMulticoreThreadCount,
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
  kCtrlTagPhaseMulticoreEnabled,
  kCtrlTagPhaseMulticoreThreadCount,
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


static inline bool NAMPhaseMulticoreEnvEnabled(const char* name)
{
  const char* v = std::getenv(name);
  return v != nullptr && v[0] != '\0' && v[0] != '0';
}

static inline int NAMPhaseMulticoreEnvInt(const char* name, int fallback, int minValue, int maxValue)
{
  const char* v = std::getenv(name);
  if (v == nullptr || v[0] == '\0')
    return fallback;

  try
  {
    const int parsed = std::stoi(v);
    return std::max(minValue, std::min(maxValue, parsed));
  }
  catch (...)
  {
    return fallback;
  }
}


static inline void NAMConfigurePhaseWorkerThread(int workerJobIndex)
{
  (void)workerJobIndex;

#if defined(_WIN32)
  // Experimental GatewayOS-style scheduling hint. This does not "fake" the DAW CPU
  // meter; it gives persistent phase workers a Pro Audio MMCSS class so the audio
  // thread waits less often for worker wake-up/scheduling.
  //
  // Enabled by default. Set NAM_PHASE_MULTICORE_MMCSS_DISABLE=1 to disable.
  if (!NAMPhaseMulticoreEnvEnabled("NAM_PHASE_MULTICORE_MMCSS_DISABLE"))
  {
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsA("Pro Audio", &taskIndex);
    if (mmcss == nullptr)
      mmcss = AvSetMmThreadCharacteristicsA("Audio", &taskIndex);

    if (mmcss != nullptr)
      AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  }
#endif
}


struct NAMPhaseMulticoreRuntimeConfig
{
  std::atomic<bool> enabled {true};
  // 0 = Smart Auto. Positive values are exact requested total threads including the audio thread.
  std::atomic<int> requestedThreads {0};
  std::atomic<int> reserveThreads {4};
};

static inline NAMPhaseMulticoreRuntimeConfig& NAMPhaseMulticoreConfig()
{
  static NAMPhaseMulticoreRuntimeConfig config;
  return config;
}

static inline int NAMPhaseMulticoreClampInt(int v, int lo, int hi)
{
  return std::max(lo, std::min(hi, v));
}

static inline int NAMPhaseMulticoreHardwareThreads()
{
  const unsigned hw = std::thread::hardware_concurrency();
  return hw > 0 ? static_cast<int>(hw) : 8;
}

static inline bool NAMPhaseMulticoreIsAppleSilicon()
{
#if defined(__APPLE__) && (defined(__arm64__) || defined(__aarch64__))
  return true;
#else
  return false;
#endif
}

static inline int NAMPhaseMulticoreSmartAutoThreadCount()
{
  const int hw = NAMPhaseMulticoreHardwareThreads();

  // Smart Auto: leave headroom for the DAW, UI, audio driver, and OS.
  //
  // Default reserve stays 4 threads, matching the Windows tuning.
  // Override for testing:
  //   NAM_PHASE_MULTICORE_RESERVE_THREADS=6
  int reserve = NAMPhaseMulticoreConfig().reserveThreads.load();
  reserve = NAMPhaseMulticoreEnvInt("NAM_PHASE_MULTICORE_RESERVE_THREADS",
                                    reserve,
                                    1,
                                    std::max(1, hw - 1));

  int total = hw - reserve;

  // Avoid tiny auto pools on small CPUs, but never overcommit.
  if (hw >= 8)
    total = std::max(4, total);
  else
    total = std::max(2, total);

  total = NAMPhaseMulticoreClampInt(total, 1, 64);

  if (NAMPhaseMulticoreIsAppleSilicon())
  {
    // Apple Silicon has performance + efficiency cores. std::thread::hardware_concurrency()
    // may include efficiency cores, which are not ideal for realtime audio worker load.
    //
    // Therefore Auto is intentionally conservative on Apple Silicon.
    //
    // Manual OS Threads still bypasses this cap.
    //
    // Override for testing:
    //   NAM_PHASE_MULTICORE_APPLE_SILICON_AUTO_CAP=12
    const int appleAutoCap =
      NAMPhaseMulticoreEnvInt("NAM_PHASE_MULTICORE_APPLE_SILICON_AUTO_CAP", 8, 1, 64);

    total = std::min(total, appleAutoCap);

    // Keep at least two logical threads free when possible.
    if (hw > 2)
      total = std::min(total, hw - 2);

    total = NAMPhaseMulticoreClampInt(total, 1, 64);
  }

  return total;
}

static inline void NAMSetPhaseMulticoreRuntimeSettings(bool enabled, int requestedThreads, int reserveThreads = 4)
{
  NAMPhaseMulticoreConfig().enabled.store(enabled);
  NAMPhaseMulticoreConfig().requestedThreads.store(NAMPhaseMulticoreClampInt(requestedThreads, 0, 64));
  NAMPhaseMulticoreConfig().reserveThreads.store(NAMPhaseMulticoreClampInt(reserveThreads, 1, 64));
}

static inline bool NAMPhaseMulticoreRuntimeEnabled()
{
  return NAMPhaseMulticoreConfig().enabled.load();
}

static inline int NAMPhaseMulticoreConfiguredThreadCount()
{
  const int requested = NAMPhaseMulticoreConfig().requestedThreads.load();
  return requested > 0 ? NAMPhaseMulticoreClampInt(requested, 1, 64) : NAMPhaseMulticoreSmartAutoThreadCount();
}

static inline int NAMPhaseMulticoreMaxPoolThreadCount()
{
  // The pool is intentionally created once at a safe maximum. The active job
  // count is selected dynamically per block from the plugin parameter.
  return NAMPhaseMulticoreClampInt(NAMPhaseMulticoreHardwareThreads(), 1, 64);
}

class NAMPhaseMulticorePool
{
public:
  explicit NAMPhaseMulticorePool(int totalThreads)
  {
    const int workerCount = std::max(0, totalThreads - 1);
    mWorkers.reserve(static_cast<size_t>(workerCount));

    // Worker job indices start at 1. The audio thread always runs job 0.
    for (int i = 0; i < workerCount; i++)
    {
      const int workerJobIndex = i + 1;
      mWorkers.emplace_back([this, workerJobIndex] { WorkerLoop(workerJobIndex); });
    }
  }

  ~NAMPhaseMulticorePool()
  {
    {
      std::lock_guard<std::mutex> lock(mMutex);
      mStop = true;
      ++mGeneration;
    }
    mCV.notify_all();

    for (auto& t : mWorkers)
    {
      if (t.joinable())
        t.join();
    }
  }

  int ThreadCount() const { return static_cast<int>(mWorkers.size()) + 1; }

  template <typename Fn>
  void ParallelFor(int jobCount, Fn&& fn)
  {
    if (jobCount <= 1 || mWorkers.empty())
    {
      for (int j = 0; j < jobCount; j++)
        fn(j);
      return;
    }

    // This pool is intentionally not a general task queue. It is a fixed
    // phase-processing barrier: one coarse job per worker. This avoids the
    // previous mutex/queue contention on every tiny audio block.
    const int clampedJobCount = std::max(1, std::min(jobCount, ThreadCount()));
    const int workerJobs = std::max(0, clampedJobCount - 1);

    {
      std::lock_guard<std::mutex> lock(mMutex);
      mJob = std::forward<Fn>(fn);
      mJobCount = clampedJobCount;
      mRemainingWorkers = workerJobs;
      mDone = workerJobs == 0;
      ++mGeneration;
    }

    mCV.notify_all();

    // The audio thread participates and processes job 0.
    mJob(0);

    if (workerJobs > 0)
    {
      std::unique_lock<std::mutex> lock(mMutex);
      mDoneCV.wait(lock, [this] { return mDone; });
    }

    {
      std::lock_guard<std::mutex> lock(mMutex);
      mJob = nullptr;
    }
  }

private:
  void WorkerLoop(int workerJobIndex)
  {
    
    NAMConfigurePhaseWorkerThread(workerJobIndex);
int seenGeneration = 0;

    for (;;)
    {
      std::function<void(int)> job;
      bool shouldRun = false;

      {
        std::unique_lock<std::mutex> lock(mMutex);
        mCV.wait(lock, [this, &seenGeneration] { return mStop || mGeneration != seenGeneration; });

        if (mStop)
          return;

        seenGeneration = mGeneration;
        shouldRun = workerJobIndex < mJobCount && static_cast<bool>(mJob);
        if (shouldRun)
          job = mJob;
      }

      if (!shouldRun)
        continue;

      job(workerJobIndex);

      {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mRemainingWorkers > 0)
          --mRemainingWorkers;

        if (mRemainingWorkers == 0 && !mDone)
        {
          mDone = true;
          mDoneCV.notify_one();
        }
      }
    }
  }

  std::vector<std::thread> mWorkers;
  std::mutex mMutex;
  std::condition_variable mCV;
  std::condition_variable mDoneCV;
  std::function<void(int)> mJob;
  int mJobCount = 0;
  int mRemainingWorkers = 0;
  int mGeneration = 0;
  bool mDone = true;
  bool mStop = false;
};

static inline int NAMPhaseMulticoreThreadCount()
{
  return NAMPhaseMulticoreConfiguredThreadCount();
}

static inline NAMPhaseMulticorePool& NAMGetPhaseMulticorePool()
{
  static NAMPhaseMulticorePool pool(NAMPhaseMulticoreMaxPoolThreadCount());
  return pool;
}

class ResamplingNAM : public nam::DSP
{
public:
  // Resampling wrapper around the NAM models
  ResamplingNAM(std::unique_ptr<nam::DSP> encapsulated, const double expected_sample_rate,
                const std::filesystem::path& modelPath = std::filesystem::path())
  : nam::DSP(encapsulated->NumInputChannels(), encapsulated->NumOutputChannels(), expected_sample_rate)
  , mEncapsulated(std::move(encapsulated))
  , mModelPath(modelPath)
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
      return;
    }

    mResamplingContainer->ProcessBlock(
      input, output, num_frames,
      [this](NAM_SAMPLE** resampledInput, NAM_SAMPLE** resampledOutput, int resampledFrames) {
        if (mPhaseMulticoreActive)
          ProcessPhaseMulticoreUnlocked(resampledInput, resampledOutput, resampledFrames);
        else
          mEncapsulated->process(resampledInput, resampledOutput, resampledFrames);
      });
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

  void SetPhaseMulticoreSettings(bool enabled, int requestedThreads)
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    mPhaseMulticoreEnabled = enabled;
    mPhaseMulticoreRequestedThreads = NAMPhaseMulticoreClampInt(requestedThreads, 0, 64);
    NAMSetPhaseMulticoreRuntimeSettings(mPhaseMulticoreEnabled, mPhaseMulticoreRequestedThreads, 4);
    if (mEncapsulated)
      ResetUnlocked(mExternalSampleRate, mMaxExternalBlockSize);
  }

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

  void SetSlimmableSize(double value)
  {
    std::lock_guard<std::mutex> lock(mStateMutex);
    mSlimmableSize = value;
    ApplySlimmableSizeUnlocked();
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

    const bool phaseMulticoreRequested = ShouldUsePhaseMulticoreUnlocked(timeScale, resamplingActive);

    // A2Fast backend policy:
    // - For A2Fast models, OS Multi-Core uses the single-model frame-OpenMP backend.
    //   That avoids 32 tiny phase-model process() calls, which the profiler showed
    //   to be the real bottleneck.
    // - For non-A2Fast models, keep the existing phase-multicore fallback.
#if defined(NAM_ENABLE_A2_FAST)
    const bool isA2FastModel =
      phaseMulticoreRequested && mEncapsulated
      && nam::wavenet::a2_fast::is_a2_fast_dsp(mEncapsulated.get());
#else
    const bool isA2FastModel = false;
#endif

    const bool useA2FrameOMPBackend = isA2FastModel;
mPhaseMulticoreActive = phaseMulticoreRequested && !useA2FrameOMPBackend;
    mPhaseCount = mPhaseMulticoreActive ? timeScale : 1;

#if defined(NAM_ENABLE_A2_FAST)
    nam::wavenet::a2_fast::SetFrameOMPRuntimeConfig(
      useA2FrameOMPBackend,
      useA2FrameOMPBackend ? NAMPhaseMulticoreThreadCount() : 0,
      1024,
      128);
#endif

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

      if (mPhaseMulticoreActive)
      {
        const int maxPhaseBlockSize = (maxEncapsulatedBlockSize + mPhaseCount - 1) / mPhaseCount + 1;
        mEncapsulated->SetTimeScale(1);
        ApplySlimmableSizeUnlocked();
        mEncapsulated->ResetAndPrewarm(encapsulatedSampleRate, maxPhaseBlockSize);
        RebuildPhaseModelsUnlocked(encapsulatedSampleRate, maxPhaseBlockSize);
        ResizePhaseBuffersUnlocked(maxPhaseBlockSize);
      }
      else
      {
        ClearPhaseModelsUnlocked();
        if (timeScale != mAppliedModelTimeScale)
    {
      mEncapsulated->SetTimeScale(timeScale);
      mAppliedModelTimeScale = timeScale;
    }
        ApplySlimmableSizeUnlocked();
        mEncapsulated->ResetAndPrewarm(renderingSampleRate, maxEncapsulatedBlockSize);
      }
    }
    else
    {
      ClearPhaseModelsUnlocked();
      mResamplingContainer = nullptr;
      mRenderingSampleRate = sampleRate;
      mEncapsulated->SetTimeScale(1);
      ApplySlimmableSizeUnlocked();
      mEncapsulated->ResetAndPrewarm(sampleRate, maxBlockSize);
    }
  };
  bool ShouldUsePhaseMulticoreUnlocked(int timeScale, bool resamplingActive) const
  {
    return resamplingActive && mRequestedOversamplingFactor > 1 && timeScale > 1 && !mModelPath.empty()
           && NAMPhaseMulticoreRuntimeEnabled();
  }

  std::unique_ptr<nam::DSP> CreatePhaseModelCloneUnlocked(double encapsulatedSampleRate, int maxPhaseBlockSize)
  {
    // Prefer a state-only clone: shared immutable weights/config, independent
    // delay/history/state. This removes the most expensive part of phase
    // multicore for backends that implement CloneForPhase().
    std::unique_ptr<nam::DSP> clone = mEncapsulated ? mEncapsulated->CloneForPhase() : nullptr;

    // Fallback for unsupported model types: preserve the existing behavior.
    if (!clone)
      clone = nam::get_dsp(mModelPath);

    if (clone->NumInputChannels() != 1 || clone->NumOutputChannels() != 1)
      throw std::runtime_error("Phase multicore clone requires a mono NAM model.");

    clone->SetTimeScale(1);
    if (nam::SlimmableModel* slimmable = dynamic_cast<nam::SlimmableModel*>(clone.get()))
      slimmable->SetSlimmableSize(mSlimmableSize);
    clone->ResetAndPrewarm(encapsulatedSampleRate, maxPhaseBlockSize);
    return clone;
  }

  void ApplySlimmableSizeUnlocked()
  {
    if (nam::SlimmableModel* slimmable = dynamic_cast<nam::SlimmableModel*>(mEncapsulated.get()))
      slimmable->SetSlimmableSize(mSlimmableSize);

    for (auto& model : mPhaseModels)
    {
      if (model)
      {
        if (nam::SlimmableModel* slimmable = dynamic_cast<nam::SlimmableModel*>(model.get()))
          slimmable->SetSlimmableSize(mSlimmableSize);
      }
    }
  }

  void RebuildPhaseModelsUnlocked(double encapsulatedSampleRate, int maxPhaseBlockSize)
  {
    const int requiredClones = std::max(0, mPhaseCount - 1);

    if (static_cast<int>(mPhaseModels.size()) != requiredClones)
      mPhaseModels.clear();

    while (static_cast<int>(mPhaseModels.size()) < requiredClones)
      mPhaseModels.push_back(CreatePhaseModelCloneUnlocked(encapsulatedSampleRate, maxPhaseBlockSize));

    for (auto& model : mPhaseModels)
    {
      model->SetTimeScale(1);
      if (nam::SlimmableModel* slimmable = dynamic_cast<nam::SlimmableModel*>(model.get()))
        slimmable->SetSlimmableSize(mSlimmableSize);
      model->ResetAndPrewarm(encapsulatedSampleRate, maxPhaseBlockSize);
    }
  }

  void ResizePhaseBuffersUnlocked(int maxPhaseBlockSize)
  {
    mPhaseInputBuffers.resize(static_cast<size_t>(mPhaseCount));
    mPhaseOutputBuffers.resize(static_cast<size_t>(mPhaseCount));

    for (int phase = 0; phase < mPhaseCount; phase++)
    {
      mPhaseInputBuffers[static_cast<size_t>(phase)].assign(static_cast<size_t>(maxPhaseBlockSize), NAM_SAMPLE(0.0));
      mPhaseOutputBuffers[static_cast<size_t>(phase)].assign(static_cast<size_t>(maxPhaseBlockSize), NAM_SAMPLE(0.0));
    }
  }

  void ClearPhaseModelsUnlocked()
  {
    mPhaseMulticoreActive = false;
    mPhaseCount = 1;
    mPhaseModels.clear();
    mPhaseInputBuffers.clear();
    mPhaseOutputBuffers.clear();
  }

  nam::DSP* GetPhaseModelUnlocked(int phase)
  {
    return phase == 0 ? mEncapsulated.get() : mPhaseModels[static_cast<size_t>(phase - 1)].get();
  }









  void ProcessPhaseMulticoreUnlocked(NAM_SAMPLE** resampledInput, NAM_SAMPLE** resampledOutput, int resampledFrames)
  {
    if (!mPhaseMulticoreActive || mPhaseCount <= 1)
    {
      mEncapsulated->process(resampledInput, resampledOutput, resampledFrames);
      return;
    }

    const int phaseCount = mPhaseCount;
    const int requestedThreads = NAMPhaseMulticoreThreadCount();

    // Important: do NOT schedule one job per phase. At 32x this creates 32 tiny
    // jobs per audio block. Gateway-style phase processing should schedule one
    // coarse job per worker, and each worker processes several phases sequentially.
    const int availableThreads = NAMGetPhaseMulticorePool().ThreadCount();
    const int jobCount = std::max(1, std::min(std::min(requestedThreads, availableThreads), phaseCount));
    const int phasesPerJob = (phaseCount + jobCount - 1) / jobCount;

    NAMGetPhaseMulticorePool().ParallelFor(jobCount, [this, resampledInput, resampledOutput, resampledFrames,
                                                       phaseCount, phasesPerJob](int jobIndex) {
      const int phaseBegin = jobIndex * phasesPerJob;
      const int phaseEnd = std::min(phaseCount, phaseBegin + phasesPerJob);

      for (int phase = phaseBegin; phase < phaseEnd; phase++)
      {
        const int phaseFrames =
          phase < resampledFrames ? ((resampledFrames - phase + phaseCount - 1) / phaseCount) : 0;
        if (phaseFrames <= 0)
          continue;

        nam::DSP* phaseModel = GetPhaseModelUnlocked(phase);

        if (phaseModel && phaseModel->SupportsStridedProcess())
        {
          // Phase-aware backends can read/write the interleaved oversampled
          // buffer directly. This removes the extra deinterleave/interleave
          // copies from the phase-parallel wrapper.
          phaseModel->process_strided(
            resampledInput[0] + phase, phaseCount, resampledOutput[0] + phase, phaseCount, phaseFrames);
          continue;
        }

        auto& phaseInput = mPhaseInputBuffers[static_cast<size_t>(phase)];
        auto& phaseOutput = mPhaseOutputBuffers[static_cast<size_t>(phase)];

        for (int i = 0; i < phaseFrames; i++)
          phaseInput[static_cast<size_t>(i)] = resampledInput[0][phase + i * phaseCount];

        NAM_SAMPLE* phaseInputPtrs[1] = {phaseInput.data()};
        NAM_SAMPLE* phaseOutputPtrs[1] = {phaseOutput.data()};
        phaseModel->process(phaseInputPtrs, phaseOutputPtrs, phaseFrames);

        for (int i = 0; i < phaseFrames; i++)
          resampledOutput[0][phase + i * phaseCount] = phaseOutput[static_cast<size_t>(i)];
      }
    });
  }

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

  // Experimental Gateway-style phase-parallel oversampling.
  // With time-scaled dilations, each oversampled phase is independent and can be processed by a separate model copy.
  std::filesystem::path mModelPath;
  bool mPhaseMulticoreActive = false;
  int mPhaseCount = 1;
  double mSlimmableSize = 1.0;
  std::vector<std::unique_ptr<nam::DSP>> mPhaseModels;
  std::vector<std::vector<NAM_SAMPLE>> mPhaseInputBuffers;
  std::vector<std::vector<NAM_SAMPLE>> mPhaseOutputBuffers;
  mutable std::mutex mStateMutex;

  // Stateful real-time resampler for model sample-rate matching and user oversampling.
  std::unique_ptr<dsp::ResamplingContainer<NAM_SAMPLE, 1, 32>> mResamplingContainer;
  double mRenderingSampleRate = 0.0;
  double mResamplingBandwidthSampleRate = 0.0;

  // Used to check that we don't get too large a block to process.
  int mMaxExternalBlockSize = 0;

  // The requested oversampling factor (can override model's natural resampling)
  int mRequestedOversamplingFactor = 1;
  int mAppliedModelTimeScale = -1;
  dsp::EAntiAliasFilterPhase mAntiAliasFilterPhase = dsp::EAntiAliasFilterPhase::MinimumPhaseCascadedFIR;
  bool mPhaseMulticoreEnabled = true;
  int mPhaseMulticoreRequestedThreads = 0; // 0 = Smart Auto
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
  int _GetPhaseMulticoreThreadCountFromParam() const;
  void _SetPhaseMulticoreSettingsFromParams();
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
  std::atomic<int> mOfflineAntiAliasFilterPhaseIndex = 2;
  std::atomic<bool> mPhaseMulticoreEnabledParam = true;
  std::atomic<int> mPhaseMulticoreRequestedThreadsParam = 0; // 0 = Smart Auto
  // Tracks the last known offline-rendering state so that DSP settings/latency are refreshed on transitions.
  bool mOfflineRenderLatencyArmed = false;

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
