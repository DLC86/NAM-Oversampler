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


cpp_path = "NeuralAmpModeler/NeuralAmpModeler.cpp"
cpp = read(cpp_path)

cpp = replace_once(
    cpp,
    """  mApplyingInternalPreset.store(true, std::memory_order_release);
  for (int i = 0; i < kNumParams; ++i)
  {
    if (!_IsInternalPresetParam(i))
      continue;
    GetParam(i)->Set(preset.paramValues[i]);
    OnParamChange(i);
  }""",
    """  mApplyingInternalPreset.store(true, std::memory_order_release);
  static constexpr double kPresetParamEpsilon = 1.0e-9;
  for (int i = 0; i < kNumParams; ++i)
  {
    if (!_IsInternalPresetParam(i))
      continue;

    const double targetValue = preset.paramValues[i];
    if (std::abs(GetParam(i)->Value() - targetValue) <= kPresetParamEpsilon)
      continue;

    GetParam(i)->Set(targetValue);
    OnParamChange(i);
  }""",
    "preset parameter loop",
)

cpp = replace_once(
    cpp,
    """  if (!preset.toneStackComponentState.empty())
  {
    try
    {
      nlohmann::json config = nlohmann::json::object();
      config["ToneStack Components"] = nlohmann::json::parse(preset.toneStackComponentState);
      _UnserializeApplyToneStackComponentState(config);
      OnParamChange(kToneStackType);
    }
    catch (...)
    {
    }
  }
  else
  {
    _ResetToneStackToDefaults();
    OnParamChange(kToneStackType);
  }""",
    """  if (!preset.toneStackComponentState.empty())
  {
    if (preset.toneStackComponentState != _SerializeToneStackComponentState())
    {
      try
      {
        nlohmann::json config = nlohmann::json::object();
        config["ToneStack Components"] = nlohmann::json::parse(preset.toneStackComponentState);
        _UnserializeApplyToneStackComponentState(config);
        OnParamChange(kToneStackType);
      }
      catch (...)
      {
      }
    }
  }
  else
  {
    _ResetToneStackToDefaults();
    OnParamChange(kToneStackType);
  }""",
    "tone-stack preset block",
)

cpp = replace_once(
    cpp,
    """    if (preset.namPath.empty())
    {
      OnMessage(kMsgTagClearModel, kCtrlTagModelFileBrowser, 0, nullptr);
#if PLUG_HAS_UI
      SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadedModel, 0, "");
#endif
    }
    else if (preset.namPath != mNAMPath.Get())
    {
      WDL_String path(preset.namPath.c_str());
      _StageModel(path);
    }

    if (preset.irPath.empty())
    {
      OnMessage(kMsgTagClearIR, kCtrlTagIRFileBrowser, 0, nullptr);
#if PLUG_HAS_UI
      SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadedIR, 0, "");
#endif
    }
    else if (preset.irPath != mIRPath.Get())
    {
      WDL_String path(preset.irPath.c_str());
      _StageIR(path);
    }""",
    """    if (preset.namPath.empty())
    {
      if (CStringHasContents(mNAMPath.Get()) || mModel != nullptr || mStagedModel != nullptr)
      {
        OnMessage(kMsgTagClearModel, kCtrlTagModelFileBrowser, 0, nullptr);
#if PLUG_HAS_UI
        SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadedModel, 0, "");
#endif
      }
    }
    else if (!_InternalPresetPathsEqual(preset.namPath, mNAMPath.Get()))
    {
      WDL_String path(preset.namPath.c_str());
      _StageModel(path);
    }

    if (preset.irPath.empty())
    {
      if (CStringHasContents(mIRPath.Get()) || mIR != nullptr || mStagedIR != nullptr)
      {
        OnMessage(kMsgTagClearIR, kCtrlTagIRFileBrowser, 0, nullptr);
#if PLUG_HAS_UI
        SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadedIR, 0, "");
#endif
      }
    }
    else if (!_InternalPresetPathsEqual(preset.irPath, mIRPath.Get()))
    {
      WDL_String path(preset.irPath.c_str());
      _StageIR(path);
    }""",
    "direct preset file block",
)

cpp = replace_once(
    cpp,
    """      if (preset.namPath.empty())
      {
        OnMessage(kMsgTagClearModel, kCtrlTagModelFileBrowser, 0, nullptr);
        SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadedModel, 0, "");
      }
      else if (preset.namPath != mNAMPath.Get())
      {
        WDL_String path(preset.namPath.c_str());
        _StageModel(path);
      }

      if (preset.irPath.empty())
      {
        OnMessage(kMsgTagClearIR, kCtrlTagIRFileBrowser, 0, nullptr);
        SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadedIR, 0, "");
      }
      else if (preset.irPath != mIRPath.Get())
      {
        WDL_String path(preset.irPath.c_str());
        _StageIR(path);
      }""",
    """      if (preset.namPath.empty())
      {
        if (CStringHasContents(mNAMPath.Get()) || mModel != nullptr || mStagedModel != nullptr)
        {
          OnMessage(kMsgTagClearModel, kCtrlTagModelFileBrowser, 0, nullptr);
          SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadedModel, 0, "");
        }
      }
      else if (!_InternalPresetPathsEqual(preset.namPath, mNAMPath.Get()))
      {
        WDL_String path(preset.namPath.c_str());
        _StageModel(path);
      }

      if (preset.irPath.empty())
      {
        if (CStringHasContents(mIRPath.Get()) || mIR != nullptr || mStagedIR != nullptr)
        {
          OnMessage(kMsgTagClearIR, kCtrlTagIRFileBrowser, 0, nullptr);
          SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadedIR, 0, "");
        }
      }
      else if (!_InternalPresetPathsEqual(preset.irPath, mIRPath.Get()))
      {
        WDL_String path(preset.irPath.c_str());
        _StageIR(path);
      }""",
    "deferred preset file block",
)

write(cpp_path, cpp)


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
    "Apple active wait predicate",
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
    // Preserve the original active low-latency spin policy. Only inactive
    // phase pools are parked indefinitely, without joining on the audio thread.
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
