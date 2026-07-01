from pathlib import Path
import re


def read(path: str) -> str:
    with Path(path).open("r", encoding="utf-8", newline="") as f:
        return f.read()


def write(path: str, text: str) -> None:
    with Path(path).open("w", encoding="utf-8", newline="") as f:
        f.write(text)


def replace_once(text: str, old: str, new: str, label: str) -> str:
    pattern = re.compile(r"\r?\n".join(re.escape(line) for line in old.split("\n")))
    matches = list(pattern.finditer(text))
    if len(matches) != 1:
        raise RuntimeError(f"Expected exactly one {label} block, found {len(matches)}")
    match = matches[0]
    newline = "\r\n" if "\r\n" in match.group(0) else "\n"
    replacement = new.replace("\n", newline)
    return text[:match.start()] + replacement + text[match.end():]


# Restore the normal Apple Silicon A2 residual SIMD selection. The temporary
# forced fallback did not change the measured CPU load.
config_path = "NeuralAmpModeler/config/NeuralAmpModeler-mac.xcconfig"
config = read(config_path)
config = replace_once(
    config,
    """A2_RESIDUAL_SIMD_DEF =
A2_RESIDUAL_SIMD_DEF[arch=arm64] = NAM_A2_RESIDUAL_SIMD=0
EXTRA_ALL_DEFS = OBJC_PREFIX=vNAMOnSteroids SWELL_APP_PREFIX=Swell_vNAMOnSteroids IGRAPHICS_NANOVG IGRAPHICS_METAL GRAYED_ALPHA=0.5f NAM_ENABLE_A2_FAST $(A2_RESIDUAL_SIMD_DEF)""",
    """EXTRA_ALL_DEFS = OBJC_PREFIX=vNAMOnSteroids SWELL_APP_PREFIX=Swell_vNAMOnSteroids IGRAPHICS_NANOVG IGRAPHICS_METAL GRAYED_ALPHA=0.5f NAM_ENABLE_A2_FAST""",
    "temporary ARM64 residual SIMD override",
)
write(config_path, config)


# Retain the original low-latency active worker policy, but park an existing
# pool indefinitely when oversampling/phase multicore is inactive.
h_path = "NeuralAmpModeler/NeuralAmpModeler.h"
h = read(h_path)

h = replace_once(
    h,
    """  int ThreadCount() const { return static_cast<int>(mWorkers.size()) + 1; }

  void SetAudioWorkgroup(void* workgroup)""",
    """  int ThreadCount() const { return static_cast<int>(mWorkers.size()) + 1; }

  void SetActive(bool active)
  {
    const bool previous = mActive.exchange(active, std::memory_order_acq_rel);
    if (previous != active)
      mCV.notify_all();
  }

  void SetAudioWorkgroup(void* workgroup)""",
    "pool active setter",
)

h = replace_once(
    h,
    """    if (jobCount <= 1 || mWorkers.empty())
    {
      for (int j = 0; j < jobCount; j++)
        fn(j);
      return;
    }

    // This pool is intentionally not a general task queue.""",
    """    if (jobCount <= 1 || mWorkers.empty())
    {
      for (int j = 0; j < jobCount; j++)
        fn(j);
      return;
    }

    if (!mActive.load(std::memory_order_acquire))
      SetActive(true);

    // This pool is intentionally not a general task queue.""",
    "pool reactivation",
)

h = replace_once(
    h,
    """#if defined(__APPLE__) && NAM_HAS_AUDIO_WORKGROUP
      os_workgroup_t audioWorkgroup = nullptr;
#endif

      unsigned generation = mGeneration.load(std::memory_order_acquire);""",
    """#if defined(__APPLE__) && NAM_HAS_AUDIO_WORKGROUP
      os_workgroup_t audioWorkgroup = nullptr;
#endif

      if (!mActive.load(std::memory_order_acquire))
      {
        std::unique_lock<std::mutex> lock(mMutex);
        mCV.wait(lock, [this] {
          return mStop.load(std::memory_order_acquire) || mActive.load(std::memory_order_acquire);
        });
        idleSpins = 0;
        if (mStop.load(std::memory_order_acquire))
          return;
        continue;
      }

      unsigned generation = mGeneration.load(std::memory_order_acquire);""",
    "inactive pool wait",
)

h = replace_once(
    h,
    """        mCV.wait_for(lock, std::chrono::microseconds(500), [this, seenGeneration] {
          return mStop.load(std::memory_order_acquire)
                 || mGeneration.load(std::memory_order_acquire) != seenGeneration;
        });""",
    """        mCV.wait_for(lock, std::chrono::microseconds(500), [this, seenGeneration] {
          return mStop.load(std::memory_order_acquire)
                 || !mActive.load(std::memory_order_acquire)
                 || mGeneration.load(std::memory_order_acquire) != seenGeneration;
        });""",
    "Apple wait predicate",
)

h = replace_once(
    h,
    """        mCV.wait(lock, [this, &seenGeneration] {
          return mStop.load(std::memory_order_acquire)
                 || mGeneration.load(std::memory_order_acquire) != seenGeneration;
        });""",
    """        mCV.wait(lock, [this, seenGeneration] {
          return mStop.load(std::memory_order_acquire)
                 || !mActive.load(std::memory_order_acquire)
                 || mGeneration.load(std::memory_order_acquire) != seenGeneration;
        });""",
    "non-Apple wait predicate",
)

h = replace_once(
    h,
    """  std::atomic<int> mCompletedWorkers {0};
  std::atomic<unsigned> mGeneration {0};
  std::atomic<bool> mStop {false};""",
    """  std::atomic<int> mCompletedWorkers {0};
  std::atomic<unsigned> mGeneration {0};
  std::atomic<bool> mActive {true};
  std::atomic<bool> mStop {false};""",
    "pool active member",
)

h = replace_once(
    h,
    """  void ClearPhaseModelsUnlocked()
  {
    mPhaseMulticoreActive = false;
    mPhaseCount = 1;
    mPhaseModels.clear();
    mPhaseInputBuffers.clear();
    mPhaseOutputBuffers.clear();
  }""",
    """  void ParkPhasePoolUnlocked()
  {
    if (mPhasePool)
      mPhasePool->SetActive(false);
  }

  void ClearPhaseModelsUnlocked()
  {
    // Keep the pool object so no realtime thread performs joins, but block all
    // workers indefinitely while oversampling or phase multicore is inactive.
    ParkPhasePoolUnlocked();
    mPhaseMulticoreActive = false;
    mPhaseCount = 1;
    mPhaseModels.clear();
    mPhaseInputBuffers.clear();
    mPhaseOutputBuffers.clear();
  }""",
    "phase pool parking",
)

write(h_path, h)
