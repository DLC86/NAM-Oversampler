from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2] if '.github' in str(Path(__file__)) else Path.cwd()
H_PATH = ROOT / 'NeuralAmpModeler' / 'NeuralAmpModeler.h'
CPP_PATH = ROOT / 'NeuralAmpModeler' / 'NeuralAmpModeler.cpp'
CONTROLS_PATH = ROOT / 'NeuralAmpModeler' / 'NeuralAmpModelerControls.h'


def read(path: Path) -> str:
    with path.open('r', encoding='utf-8', newline='') as f:
        return f.read()


def write(path: Path, text: str) -> None:
    with path.open('w', encoding='utf-8', newline='') as f:
        f.write(text)


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f'Expected exactly one {label}, found {count}')
    return text.replace(old, new, 1)


def sub_once(text: str, pattern: str, replacement: str, label: str) -> str:
    updated, count = re.subn(pattern, lambda _: replacement, text, count=1, flags=re.MULTILINE | re.DOTALL)
    if count != 1:
        raise RuntimeError(f'Expected exactly one {label}, found {count}')
    return updated


h = read(H_PATH)
cpp = read(CPP_PATH)
controls = read(CONTROLS_PATH)

# Rename internal state variables while preserving the existing public parameter index/name.
for text_name in ('h', 'cpp'):
    text = h if text_name == 'h' else cpp
    text = text.replace('mPhaseMulticoreEnabledParam', 'mPhaseMulticoreWaitModeParam')
    text = text.replace('mPendingPhaseMulticoreEnabled', 'mPendingPhaseMulticoreWaitMode')
    text = text.replace('mPhaseMulticoreEnabled', 'mPhaseMulticoreWaitMode')
    if text_name == 'h':
        h = text
    else:
        cpp = text

h = replace_once(
    h,
    '''struct NAMPhaseMulticoreRuntimeConfig
{
  std::atomic<bool> enabled {true};
  // 0 = Smart Auto. Positive values are exact requested total threads including the audio thread.
  std::atomic<int> requestedThreads {0};
  std::atomic<int> reserveThreads {4};
};''',
    '''enum class ENAMPhaseMulticoreWaitMode : int
{
  Off = 0,
  Sleep = 1,
  Spin = 2,
  Hybrid = 3
};

static inline ENAMPhaseMulticoreWaitMode NAMPhaseMulticoreWaitModeFromInt(int value)
{
  return static_cast<ENAMPhaseMulticoreWaitMode>(std::clamp(value, 0, 3));
}

struct NAMPhaseMulticoreRuntimeConfig
{
  std::atomic<int> waitMode {static_cast<int>(ENAMPhaseMulticoreWaitMode::Sleep)};
  // 0 = Smart Auto. Positive values are exact requested total threads including the audio thread.
  std::atomic<int> requestedThreads {0};
  std::atomic<int> reserveThreads {4};
};''',
    'phase multicore runtime config',
)

h = replace_once(
    h,
    '''static inline void NAMSetPhaseMulticoreRuntimeSettings(bool enabled, int requestedThreads, int reserveThreads = 4)
{
  NAMPhaseMulticoreConfig().enabled.store(enabled);
  NAMPhaseMulticoreConfig().requestedThreads.store(NAMPhaseMulticoreClampInt(requestedThreads, 0, 64));
  NAMPhaseMulticoreConfig().reserveThreads.store(NAMPhaseMulticoreClampInt(reserveThreads, 1, 64));
}

static inline bool NAMPhaseMulticoreRuntimeEnabled()
{
  return NAMPhaseMulticoreConfig().enabled.load();
}''',
    '''static inline void NAMSetPhaseMulticoreRuntimeSettings(int waitMode, int requestedThreads, int reserveThreads = 4)
{
  NAMPhaseMulticoreConfig().waitMode.store(static_cast<int>(NAMPhaseMulticoreWaitModeFromInt(waitMode)));
  NAMPhaseMulticoreConfig().requestedThreads.store(NAMPhaseMulticoreClampInt(requestedThreads, 0, 64));
  NAMPhaseMulticoreConfig().reserveThreads.store(NAMPhaseMulticoreClampInt(reserveThreads, 1, 64));
}

static inline ENAMPhaseMulticoreWaitMode NAMPhaseMulticoreRuntimeWaitMode()
{
  return NAMPhaseMulticoreWaitModeFromInt(NAMPhaseMulticoreConfig().waitMode.load());
}

static inline bool NAMPhaseMulticoreRuntimeEnabled()
{
  return NAMPhaseMulticoreRuntimeWaitMode() != ENAMPhaseMulticoreWaitMode::Off;
}''',
    'phase multicore runtime accessors',
)

h = replace_once(
    h,
    '''  explicit NAMPhaseMulticorePool(int totalThreads)
  {
    const int workerCount = std::max(0, totalThreads - 1);
    mWorkers.reserve(static_cast<size_t>(workerCount));''',
    '''  explicit NAMPhaseMulticorePool(int totalThreads, ENAMPhaseMulticoreWaitMode waitMode)
  : mWaitMode(static_cast<int>(waitMode))
  {
    const int workerCount = std::max(0, totalThreads - 1);
    mHybridWarmWorkerCount = NAMPhaseMulticoreEnvInt(
      "NAM_PHASE_MULTICORE_HYBRID_WARM_WORKERS", std::min(3, workerCount), 0, workerCount);
    mHybridColdSpinLimit = NAMPhaseMulticoreEnvInt(
      "NAM_PHASE_MULTICORE_HYBRID_COLD_SPINS", 4096, 0, 1048576);
    mSpinSafetyLimit = NAMPhaseMulticoreEnvInt(
      "NAM_PHASE_MULTICORE_SPIN_SAFETY_LIMIT", 262144, 1024, 4194304);
    mWorkers.reserve(static_cast<size_t>(workerCount));''',
    'phase pool constructor',
)

h = replace_once(
    h,
    '''  void SetActive(bool active)
  {
    const bool previous = mActive.exchange(active, std::memory_order_acq_rel);
    if (previous != active)
      mCV.notify_all();
  }

  void SetAudioWorkgroup(void* workgroup)''',
    '''  void SetActive(bool active)
  {
    const bool previous = mActive.exchange(active, std::memory_order_acq_rel);
    if (previous != active)
      mCV.notify_all();
  }

  void SetWaitMode(ENAMPhaseMulticoreWaitMode waitMode)
  {
    const int normalized = static_cast<int>(waitMode);
    const int previous = mWaitMode.exchange(normalized, std::memory_order_acq_rel);
    if (previous != normalized)
      mCV.notify_all();
  }

  void SetAudioWorkgroup(void* workgroup)''',
    'phase pool wait-mode setter',
)

old_wait_block = '''      unsigned generation = mGeneration.load(std::memory_order_acquire);
      if (generation == seenGeneration)
      {
        if (mStop.load(std::memory_order_acquire))
          return;

#if defined(__APPLE__)
        // Avoid a kernel wake-up on every tiny audio block. The timed sleep is
        // only a fallback for stopped or inactive playback. Apple Silicon can
        // spin longer because the AudioWorkgroup/P-core path is very effective;
        // Intel Macs get a shorter spin to reduce condition_variable wake
        // latency without burning an entire hyper-thread while transport is idle.
        const int spinLimit = NAMPhaseMulticoreIsAppleSilicon() ? 262144 : 32768;
        if (idleSpins++ < spinLimit)
        {
          NAMPhaseMulticoreRealtimePause();
          continue;
        }

        std::unique_lock<std::mutex> lock(mMutex);
        mCV.wait_for(lock, std::chrono::microseconds(500), [this, seenGeneration] {
          return mStop.load(std::memory_order_acquire)
                 || !mActive.load(std::memory_order_acquire)
                 || mGeneration.load(std::memory_order_acquire) != seenGeneration;
        });
        idleSpins = 0;
#else
        std::unique_lock<std::mutex> lock(mMutex);
        mCV.wait(lock, [this, seenGeneration] {
          return mStop.load(std::memory_order_acquire)
                 || !mActive.load(std::memory_order_acquire)
                 || mGeneration.load(std::memory_order_acquire) != seenGeneration;
        });
#endif
        continue;
      }
'''
new_wait_block = '''      unsigned generation = mGeneration.load(std::memory_order_acquire);
      if (generation == seenGeneration)
      {
        if (mStop.load(std::memory_order_acquire))
          return;

        const auto waitMode = NAMPhaseMulticoreWaitModeFromInt(mWaitMode.load(std::memory_order_acquire));
        const bool warmWorker =
          waitMode == ENAMPhaseMulticoreWaitMode::Spin
          || (waitMode == ENAMPhaseMulticoreWaitMode::Hybrid
              && workerJobIndex <= mHybridWarmWorkerCount);

        int spinLimit = 0;
        if (warmWorker)
          spinLimit = mSpinSafetyLimit;
        else if (waitMode == ENAMPhaseMulticoreWaitMode::Hybrid)
          spinLimit = mHybridColdSpinLimit;

        if (idleSpins++ < spinLimit)
        {
          NAMPhaseMulticoreRealtimePause();
          continue;
        }

        auto wakePredicate = [this, seenGeneration, waitMode] {
          return mStop.load(std::memory_order_acquire)
                 || !mActive.load(std::memory_order_acquire)
                 || mGeneration.load(std::memory_order_acquire) != seenGeneration
                 || NAMPhaseMulticoreWaitModeFromInt(mWaitMode.load(std::memory_order_acquire)) != waitMode;
        };

        std::unique_lock<std::mutex> lock(mMutex);
        if (warmWorker)
        {
          // Diagnostic Spin and the warm subset of Hybrid poll aggressively
          // between live blocks, with a short periodic park so stopped transport
          // cannot leave high-priority workers burning a core indefinitely.
          mCV.wait_for(lock, std::chrono::microseconds(500), wakePredicate);
        }
        else
        {
          // Sleep parks immediately. Hybrid cold workers perform the short spin
          // above and then park until the next generation is published.
          mCV.wait(lock, wakePredicate);
        }
        idleSpins = 0;
        continue;
      }
'''
h = replace_once(h, old_wait_block, new_wait_block, 'worker wait policy block')

h = replace_once(
    h,
    '''  std::atomic<int> mCompletedWorkers {0};
  std::atomic<unsigned> mGeneration {0};
  std::atomic<bool> mActive {true};
  std::atomic<bool> mStop {false};''',
    '''  std::atomic<int> mCompletedWorkers {0};
  std::atomic<unsigned> mGeneration {0};
  std::atomic<int> mWaitMode {static_cast<int>(ENAMPhaseMulticoreWaitMode::Sleep)};
  std::atomic<bool> mActive {true};
  std::atomic<bool> mStop {false};
  int mHybridWarmWorkerCount = 0;
  int mHybridColdSpinLimit = 4096;
  int mSpinSafetyLimit = 262144;''',
    'phase pool members',
)

h = replace_once(
    h,
    '''  void SetPhaseMulticoreSettings(bool enabled, int requestedThreads)
  {
    mPendingPhaseMulticoreThreads.store(
      NAMPhaseMulticoreClampInt(requestedThreads, 0, 64), std::memory_order_relaxed);
    mPendingPhaseMulticoreWaitMode.store(enabled ? 1 : 0, std::memory_order_release);
  }''',
    '''  void SetPhaseMulticoreSettings(int waitMode, int requestedThreads)
  {
    mPendingPhaseMulticoreThreads.store(
      NAMPhaseMulticoreClampInt(requestedThreads, 0, 64), std::memory_order_relaxed);
    mPendingPhaseMulticoreWaitMode.store(
      static_cast<int>(NAMPhaseMulticoreWaitModeFromInt(waitMode)), std::memory_order_release);
  }''',
    'ResamplingNAM multicore settings setter',
)

h = replace_once(
    h,
    '''    const int pendingEnabled = mPendingPhaseMulticoreWaitMode.exchange(-1, std::memory_order_acquire);
    if (pendingEnabled < 0)
      return;

    mPhaseMulticoreWaitMode = pendingEnabled != 0;
    mPhaseMulticoreRequestedThreads =
      NAMPhaseMulticoreClampInt(mPendingPhaseMulticoreThreads.load(std::memory_order_relaxed), 0, 64);
    NAMSetPhaseMulticoreRuntimeSettings(mPhaseMulticoreWaitMode, mPhaseMulticoreRequestedThreads, 4);''',
    '''    const int pendingWaitMode = mPendingPhaseMulticoreWaitMode.exchange(-1, std::memory_order_acquire);
    if (pendingWaitMode < 0)
      return;

    mPhaseMulticoreWaitMode = static_cast<int>(NAMPhaseMulticoreWaitModeFromInt(pendingWaitMode));
    mPhaseMulticoreRequestedThreads =
      NAMPhaseMulticoreClampInt(mPendingPhaseMulticoreThreads.load(std::memory_order_relaxed), 0, 64);
    NAMSetPhaseMulticoreRuntimeSettings(mPhaseMulticoreWaitMode, mPhaseMulticoreRequestedThreads, 4);''',
    'pending multicore wait-mode application',
)

h = replace_once(
    h,
    '''           && mPhaseMulticoreWaitMode && mEncapsulated && mEncapsulated->SupportsStridedProcess();''',
    '''           && NAMPhaseMulticoreWaitModeFromInt(mPhaseMulticoreWaitMode) != ENAMPhaseMulticoreWaitMode::Off
           && mEncapsulated && mEncapsulated->SupportsStridedProcess();''',
    'phase multicore enable condition',
)

h = replace_once(
    h,
    '''    if (!mPhasePool || mPhasePoolThreadCount != clampedThreads)
    {
      mPhasePool = std::make_shared<NAMPhaseMulticorePool>(clampedThreads);
      mPhasePoolThreadCount = clampedThreads;
    }

    return mPhasePool;''',
    '''    const auto waitMode = NAMPhaseMulticoreWaitModeFromInt(mPhaseMulticoreWaitMode);
    if (!mPhasePool || mPhasePoolThreadCount != clampedThreads)
    {
      mPhasePool = std::make_shared<NAMPhaseMulticorePool>(clampedThreads, waitMode);
      mPhasePoolThreadCount = clampedThreads;
    }
    else
    {
      mPhasePool->SetWaitMode(waitMode);
    }

    return mPhasePool;''',
    'phase pool creation',
)

h = replace_once(
    h,
    '''    return mPhaseMulticoreActive && right.mPhaseMulticoreActive && mPhaseCount > 1
           && mPhaseCount == right.mPhaseCount
           && PhaseMulticoreThreadCountUnlocked() == right.PhaseMulticoreThreadCountUnlocked()''',
    '''    return mPhaseMulticoreActive && right.mPhaseMulticoreActive && mPhaseCount > 1
           && mPhaseCount == right.mPhaseCount
           && mPhaseMulticoreWaitMode == right.mPhaseMulticoreWaitMode
           && PhaseMulticoreThreadCountUnlocked() == right.PhaseMulticoreThreadCountUnlocked()''',
    'stereo wait-mode compatibility',
)

h = replace_once(
    h,
    '''  bool mPhaseMulticoreWaitMode = true;
  int mPhaseMulticoreRequestedThreads = 0; // 0 = Smart Auto''',
    '''  int mPhaseMulticoreWaitMode = static_cast<int>(ENAMPhaseMulticoreWaitMode::Sleep);
  int mPhaseMulticoreRequestedThreads = 0; // 0 = Smart Auto''',
    'ResamplingNAM wait-mode member',
)

h = replace_once(
    h,
    '''  void _ApplyImmediatePhaseMulticoreSettings(bool enabled, int requestedThreads);''',
    '''  void _ApplyImmediatePhaseMulticoreSettings(int waitMode, int requestedThreads);''',
    'plugin immediate multicore declaration',
)

h = replace_once(
    h,
    '''  std::atomic<bool> mPhaseMulticoreWaitModeParam = true;
  std::atomic<int> mPhaseMulticoreRequestedThreadsParam = 0; // 0 = Smart Auto''',
    '''  std::atomic<int> mPhaseMulticoreWaitModeParam {
    static_cast<int>(ENAMPhaseMulticoreWaitMode::Sleep)};
  std::atomic<int> mPhaseMulticoreRequestedThreadsParam = 0; // 0 = Smart Auto''',
    'plugin wait-mode parameter cache',
)

cpp = replace_once(
    cpp,
    '''  GetParam(kPhaseMulticoreEnabled)->InitBool("OS Multi-Core", true);''',
    '''  GetParam(kPhaseMulticoreEnabled)
    ->InitEnum("OS Multi-Core", static_cast<int>(ENAMPhaseMulticoreWaitMode::Sleep),
               {"OFF", "SLEEP", "SPIN", "HYBRID"});''',
    'OS multicore parameter initialization',
)

old_plugin_settings = '''void NeuralAmpModeler::_SetPhaseMulticoreSettingsFromParams()
{
  const bool enabled = GetParam(kPhaseMulticoreEnabled)->Bool();
  const int requestedThreads = _GetPhaseMulticoreThreadCountFromParam();

  mPhaseMulticoreWaitModeParam = enabled;
  mPhaseMulticoreRequestedThreadsParam = requestedThreads;

  // Staged models are not audible yet, so keep them ready without waiting for
  // the realtime transition.
  if (mStagedModel != nullptr)
    mStagedModel->SetPhaseMulticoreSettings(enabled, requestedThreads);
  if (mStagedModelRight != nullptr)
    mStagedModelRight->SetPhaseMulticoreSettings(enabled, requestedThreads);

  if (mModel == nullptr || GetRenderingOffline())
  {
    _ApplyImmediatePhaseMulticoreSettings(enabled, requestedThreads);
    return;
  }

  // Do not change the global runtime policy or queue a model reset until the
  // audible signal has reached the bottom of the fade-out.
  mPendingPhaseMulticoreThreads.store(requestedThreads, std::memory_order_relaxed);
  mPendingPhaseMulticoreWaitMode.store(enabled ? 1 : 0, std::memory_order_release);
  _StartRealtimeDSPTransition();
}

void NeuralAmpModeler::_ApplyImmediatePhaseMulticoreSettings(bool enabled, int requestedThreads)
{
  NAMSetPhaseMulticoreRuntimeSettings(enabled, requestedThreads, 4);

  if (mModel != nullptr)
    mModel->SetPhaseMulticoreSettings(enabled, requestedThreads);
  if (mModelRight != nullptr)
    mModelRight->SetPhaseMulticoreSettings(enabled, requestedThreads);

  _UpdateLatency();
}'''
new_plugin_settings = '''void NeuralAmpModeler::_SetPhaseMulticoreSettingsFromParams()
{
  const int waitMode = static_cast<int>(
    NAMPhaseMulticoreWaitModeFromInt(GetParam(kPhaseMulticoreEnabled)->Int()));
  const int requestedThreads = _GetPhaseMulticoreThreadCountFromParam();

  mPhaseMulticoreWaitModeParam = waitMode;
  mPhaseMulticoreRequestedThreadsParam = requestedThreads;

  // Staged models are not audible yet, so keep them ready without waiting for
  // the realtime transition.
  if (mStagedModel != nullptr)
    mStagedModel->SetPhaseMulticoreSettings(waitMode, requestedThreads);
  if (mStagedModelRight != nullptr)
    mStagedModelRight->SetPhaseMulticoreSettings(waitMode, requestedThreads);

  if (mModel == nullptr || GetRenderingOffline())
  {
    _ApplyImmediatePhaseMulticoreSettings(waitMode, requestedThreads);
    return;
  }

  // Do not change the global runtime policy or queue a model reset until the
  // audible signal has reached the bottom of the fade-out.
  mPendingPhaseMulticoreThreads.store(requestedThreads, std::memory_order_relaxed);
  mPendingPhaseMulticoreWaitMode.store(waitMode, std::memory_order_release);
  _StartRealtimeDSPTransition();
}

void NeuralAmpModeler::_ApplyImmediatePhaseMulticoreSettings(int waitMode, int requestedThreads)
{
  waitMode = static_cast<int>(NAMPhaseMulticoreWaitModeFromInt(waitMode));
  NAMSetPhaseMulticoreRuntimeSettings(waitMode, requestedThreads, 4);

  if (mModel != nullptr)
    mModel->SetPhaseMulticoreSettings(waitMode, requestedThreads);
  if (mModelRight != nullptr)
    mModelRight->SetPhaseMulticoreSettings(waitMode, requestedThreads);

  _UpdateLatency();
}'''
cpp = replace_once(cpp, old_plugin_settings, new_plugin_settings, 'plugin multicore settings functions')

cpp = replace_once(
    cpp,
    '''    const int pendingMulticoreEnabled = mPendingPhaseMulticoreWaitMode.exchange(-1, std::memory_order_acquire);
    if (pendingMulticoreEnabled >= 0)
    {
      const int pendingThreads = mPendingPhaseMulticoreThreads.load(std::memory_order_relaxed);
      _ApplyImmediatePhaseMulticoreSettings(pendingMulticoreEnabled != 0, pendingThreads);
    }''',
    '''    const int pendingMulticoreWaitMode = mPendingPhaseMulticoreWaitMode.exchange(-1, std::memory_order_acquire);
    if (pendingMulticoreWaitMode >= 0)
    {
      const int pendingThreads = mPendingPhaseMulticoreThreads.load(std::memory_order_relaxed);
      _ApplyImmediatePhaseMulticoreSettings(pendingMulticoreWaitMode, pendingThreads);
    }''',
    'realtime transition multicore application',
)

controls = replace_once(
    controls,
    '''  : NAMOversamplingRadioButtonControl(bounds, paramIdx, {"OFF", "ON"}, style, buttonSize, direction) {};''',
    '''  : NAMOversamplingRadioButtonControl(bounds, paramIdx, {"OFF", "SLEEP", "SPIN", "HYBRID"}, style,
                                      buttonSize, direction) {};''',
    'multicore mode control options',
)

controls = replace_once(
    controls,
    '''    const auto realtimeMulticoreArea =
      realtimeMulticoreRow.GetFromRight(rowsArea.W() - rowLabelWidth).GetFromLeft((rowsArea.W() - rowLabelWidth) / 3.0f);''',
    '''    const auto realtimeMulticoreArea =
      realtimeMulticoreRow.GetFromRight(rowsArea.W() - rowLabelWidth);''',
    'multicore mode control area',
)

controls = replace_once(
    controls,
    '''    phaseMulticoreControl->SetTooltip("Enable phase-parallel oversampling multicore");''',
    '''    phaseMulticoreControl->SetTooltip(
      "Phase worker wait mode. Sleep parks all workers; Spin polls aggressively; Hybrid keeps three workers warm.");''',
    'multicore mode tooltip',
)

checks = {
    'header wait-mode enum': 'enum class ENAMPhaseMulticoreWaitMode' in h,
    'header hybrid workers': 'mHybridWarmWorkerCount' in h,
    'header sleep mode': 'ENAMPhaseMulticoreWaitMode::Sleep' in h,
    'header spin mode': 'ENAMPhaseMulticoreWaitMode::Spin' in h,
    'header hybrid mode': 'ENAMPhaseMulticoreWaitMode::Hybrid' in h,
    'parameter four states': '{"OFF", "SLEEP", "SPIN", "HYBRID"}' in cpp,
    'control four states': '{"OFF", "SLEEP", "SPIN", "HYBRID"}' in controls,
    'old boolean initializer removed': 'InitBool("OS Multi-Core"' not in cpp,
    'old pool constructor removed': 'NAMPhaseMulticorePool(int totalThreads)' not in h,
    'old plugin bool cache removed': 'std::atomic<bool> mPhaseMulticoreWaitModeParam' not in h,
}
failed = [name for name, passed in checks.items() if not passed]
if failed:
    raise RuntimeError('Static validation failed: ' + ', '.join(failed))

write(H_PATH, h)
write(CPP_PATH, cpp)
write(CONTROLS_PATH, controls)
print('Applied phase worker wait diagnostic modes to:')
print(f'  {H_PATH.relative_to(ROOT)}')
print(f'  {CPP_PATH.relative_to(ROOT)}')
print(f'  {CONTROLS_PATH.relative_to(ROOT)}')
