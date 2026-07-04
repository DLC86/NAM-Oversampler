from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
H_PATH = ROOT / 'NeuralAmpModeler' / 'NeuralAmpModeler.h'
CPP_PATH = ROOT / 'NeuralAmpModeler' / 'NeuralAmpModeler.cpp'
CONTROLS_PATH = ROOT / 'NeuralAmpModeler' / 'NeuralAmpModelerControls.h'


def read(path):
    with path.open('r', encoding='utf-8', newline='') as f:
        return f.read()


def write(path, text):
    with path.open('w', encoding='utf-8', newline='') as f:
        f.write(text)


def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f'Expected one {label}, found {count}')
    return text.replace(old, new, 1)


h = read(H_PATH)
cpp = read(CPP_PATH)
controls = read(CONTROLS_PATH)

h = replace_once(
    h,
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
}''',
    '''enum class ENAMPhaseMulticoreWaitMode : int
{
  Off = 0,
  Sleep = 1,
  Spin = 2,
  Hybrid = 3,
  SpinIdle = 4,
  SpinSerial = 5
};

static inline ENAMPhaseMulticoreWaitMode NAMPhaseMulticoreWaitModeFromInt(int value)
{
  return static_cast<ENAMPhaseMulticoreWaitMode>(std::clamp(value, 0, 5));
}''',
    'wait-mode enum',
)

h = replace_once(
    h,
    '''    mJobContext = nullptr;
    mJobInvoker = nullptr;
  }

private:
  void WorkerLoop(int workerJobIndex)''',
    '''    mJobContext = nullptr;
    mJobInvoker = nullptr;
  }

  template <typename Fn>
  void RunSingleWorker(Fn&& fn)
  {
    if (mWorkers.empty())
    {
      fn();
      return;
    }

    if (!mActive.load(std::memory_order_acquire))
      SetActive(true);

    using JobType = std::remove_reference_t<Fn>;
    JobType job = std::forward<Fn>(fn);

    mJobContext = &job;
    mJobInvoker = [](void* context, int) {
      (*static_cast<JobType*>(context))();
    };
    // Job index zero belongs to the audio thread, so publishing two jobs wakes
    // exactly worker 1 while the audio thread only waits at the barrier.
    mJobCount = 2;
    mCompletedWorkers.store(0, std::memory_order_relaxed);
    mGeneration.fetch_add(1, std::memory_order_release);
    mCV.notify_all();

    int spins = 0;
    while (mCompletedWorkers.load(std::memory_order_acquire) < 1)
    {
      NAMPhaseMulticoreRealtimePause();
      if (++spins >= 16384)
      {
        spins = 0;
#if defined(_WIN32)
        int observed = mCompletedWorkers.load(std::memory_order_acquire);
        if (observed < 1)
          WaitOnAddress(&mCompletedWorkers, &observed, sizeof(observed), 1);
#else
        std::this_thread::yield();
#endif
      }
    }

    mJobContext = nullptr;
    mJobInvoker = nullptr;
  }

private:
  void WorkerLoop(int workerJobIndex)''',
    'single-worker diagnostic dispatch',
)

h = replace_once(
    h,
    '''        const bool warmWorker =
          waitMode == ENAMPhaseMulticoreWaitMode::Spin
          || (waitMode == ENAMPhaseMulticoreWaitMode::Hybrid
              && workerJobIndex <= mHybridWarmWorkerCount);''',
    '''        const bool fullSpinMode =
          waitMode == ENAMPhaseMulticoreWaitMode::Spin
          || waitMode == ENAMPhaseMulticoreWaitMode::SpinIdle
          || waitMode == ENAMPhaseMulticoreWaitMode::SpinSerial;
        const bool warmWorker =
          fullSpinMode
          || (waitMode == ENAMPhaseMulticoreWaitMode::Hybrid
              && workerJobIndex <= mHybridWarmWorkerCount);''',
    'worker warm-mode selection',
)

old_stereo = '''    pool->ParallelFor(jobCount, [this,
                                 &right,
                                 leftResampledInput,
                                 rightResampledInput,
                                 resampledFrames,
                                 jobCount,
                                 leftNeedsDeinterleave,
                                 rightNeedsDeinterleave](int jobIndex) {
      ProcessPhaseLaneBufferedUnlocked(
        leftResampledInput, resampledFrames, jobIndex, jobCount, leftNeedsDeinterleave);
      right.ProcessPhaseLaneBufferedUnlocked(
        rightResampledInput, resampledFrames, jobIndex, jobCount, rightNeedsDeinterleave);
    });

    ReinterleavePhaseOutputsUnlocked(leftResampledOutput, resampledFrames);'''
new_stereo = '''    const auto waitMode = NAMPhaseMulticoreWaitModeFromInt(mPhaseMulticoreWaitMode);
    auto processLane = [this,
                        &right,
                        leftResampledInput,
                        rightResampledInput,
                        resampledFrames,
                        jobCount,
                        leftNeedsDeinterleave,
                        rightNeedsDeinterleave](int jobIndex) {
      ProcessPhaseLaneBufferedUnlocked(
        leftResampledInput, resampledFrames, jobIndex, jobCount, leftNeedsDeinterleave);
      right.ProcessPhaseLaneBufferedUnlocked(
        rightResampledInput, resampledFrames, jobIndex, jobCount, rightNeedsDeinterleave);
    };

    if (waitMode == ENAMPhaseMulticoreWaitMode::SpinIdle)
    {
      // Keep the full spin pool alive, but run every phase lane sequentially on
      // the host audio thread. No generation is published to the workers.
      for (int jobIndex = 0; jobIndex < jobCount; ++jobIndex)
        processLane(jobIndex);
    }
    else if (waitMode == ENAMPhaseMulticoreWaitMode::SpinSerial)
    {
      // Exercise the pool wake/barrier path, but let exactly one worker process
      // all phase lanes so there is no parallel NAM computation.
      pool->RunSingleWorker([&processLane, jobCount] {
        for (int jobIndex = 0; jobIndex < jobCount; ++jobIndex)
          processLane(jobIndex);
      });
    }
    else
    {
      pool->ParallelFor(jobCount, processLane);
    }

    ReinterleavePhaseOutputsUnlocked(leftResampledOutput, resampledFrames);'''
h = replace_once(h, old_stereo, new_stereo, 'stereo diagnostic dispatch')

old_mono = '''    pool->ParallelFor(jobCount, [this, resampledInput, resampledFrames, jobCount, needsDeinterleave](int jobIndex) {
      ProcessPhaseLaneBufferedUnlocked(
        resampledInput, resampledFrames, jobIndex, jobCount, needsDeinterleave);
    });

    ReinterleavePhaseOutputsUnlocked(resampledOutput, resampledFrames);'''
new_mono = '''    const auto waitMode = NAMPhaseMulticoreWaitModeFromInt(mPhaseMulticoreWaitMode);
    auto processLane = [this, resampledInput, resampledFrames, jobCount, needsDeinterleave](int jobIndex) {
      ProcessPhaseLaneBufferedUnlocked(
        resampledInput, resampledFrames, jobIndex, jobCount, needsDeinterleave);
    };

    if (waitMode == ENAMPhaseMulticoreWaitMode::SpinIdle)
    {
      // Workers remain in the Spin wait policy while the audio thread executes
      // every phase lane serially. This isolates idle worker EMI from parallel work.
      for (int jobIndex = 0; jobIndex < jobCount; ++jobIndex)
        processLane(jobIndex);
    }
    else if (waitMode == ENAMPhaseMulticoreWaitMode::SpinSerial)
    {
      // Publish one pool generation and process every phase lane on worker 1.
      // The audio thread participates only in the completion barrier.
      pool->RunSingleWorker([&processLane, jobCount] {
        for (int jobIndex = 0; jobIndex < jobCount; ++jobIndex)
          processLane(jobIndex);
      });
    }
    else
    {
      pool->ParallelFor(jobCount, processLane);
    }

    ReinterleavePhaseOutputsUnlocked(resampledOutput, resampledFrames);'''
h = replace_once(h, old_mono, new_mono, 'mono diagnostic dispatch')

cpp = replace_once(
    cpp,
    '''               {"OFF", "SLEEP", "SPIN", "HYBRID"});''',
    '''               {"OFF", "SLEEP", "SPIN", "HYBRID", "IDLE", "SERIAL"});''',
    'parameter mode labels',
)

controls = replace_once(
    controls,
    '''  : NAMOversamplingRadioButtonControl(bounds, paramIdx, {"OFF", "SLEEP", "SPIN", "HYBRID"}, style,
                                      buttonSize, direction) {};''',
    '''  : NAMOversamplingRadioButtonControl(bounds, paramIdx,
                                      {"OFF", "SLEEP", "SPIN", "HYBRID", "IDLE", "SERIAL"}, style,
                                      buttonSize, direction) {};''',
    'UI mode labels',
)

controls = replace_once(
    controls,
    '''    const auto radioButtonStyle =
      mRadioButtonStyle.WithValueText(mRadioButtonStyle.valueText.WithSize(mRadioButtonStyle.valueText.mSize - 1.0f));''',
    '''    const auto radioButtonStyle =
      mRadioButtonStyle.WithValueText(mRadioButtonStyle.valueText.WithSize(mRadioButtonStyle.valueText.mSize - 1.0f));
    // Six diagnostic states share one row. Use a smaller label and button only
    // for this control so the options remain inside the oversampling page.
    const auto multicoreButtonStyle =
      radioButtonStyle.WithValueText(radioButtonStyle.valueText.WithSize(radioButtonStyle.valueText.mSize - 1.5f));
    const float multicoreButtonSize = 8.0f;''',
    'compact multicore style',
)

controls = replace_once(
    controls,
    '''      AddNamedChildControl(new PhaseMulticoreControl(realtimeMulticoreArea, kPhaseMulticoreEnabled, radioButtonStyle,
                                                     buttonSize, EDirection::Horizontal),''',
    '''      AddNamedChildControl(new PhaseMulticoreControl(realtimeMulticoreArea, kPhaseMulticoreEnabled,
                                                     multicoreButtonStyle, multicoreButtonSize,
                                                     EDirection::Horizontal),''',
    'compact multicore control construction',
)

controls = replace_once(
    controls,
    '''      "Phase worker wait mode. Sleep parks all workers; Spin polls aggressively; Hybrid keeps three workers warm.");''',
    '''      "Phase worker diagnostic mode. Idle keeps Spin workers alive but runs all phases on the audio thread; "
      "Serial runs all phases on one worker through the pool barrier.");''',
    'diagnostic tooltip',
)

checks = {
    'enum SpinIdle': 'SpinIdle = 4' in h,
    'enum SpinSerial': 'SpinSerial = 5' in h,
    'single worker dispatch': 'void RunSingleWorker' in h,
    'mono idle branch': 'waitMode == ENAMPhaseMulticoreWaitMode::SpinIdle' in h,
    'serial branch': 'waitMode == ENAMPhaseMulticoreWaitMode::SpinSerial' in h,
    'six parameter labels': '{"OFF", "SLEEP", "SPIN", "HYBRID", "IDLE", "SERIAL"}' in cpp,
    'six UI labels': '{"OFF", "SLEEP", "SPIN", "HYBRID", "IDLE", "SERIAL"}' in controls,
    'compact UI': 'multicoreButtonSize = 8.0f' in controls,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise RuntimeError('Validation failed: ' + ', '.join(failed))

write(H_PATH, h)
write(CPP_PATH, cpp)
write(CONTROLS_PATH, controls)
print('Added IDLE and SERIAL diagnostic modes with compact UI layout.')
