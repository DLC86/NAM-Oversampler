namespace
{
nlohmann::json SerializeModelControls(const std::shared_ptr<ModelControls>& controls)
{
  if (!controls)
    return {};
  auto values = nlohmann::json::object();
  for (size_t index = 0; index < controls->specs.size(); ++index)
    values[controls->specs[index].name] = controls->Get(index);
  return {{"path", controls->path}, {"values", values}};
}

void RestoreModelControls(const std::shared_ptr<ModelControls>& controls, const nlohmann::json& state)
{
  if (!controls)
    return;
  const auto path = state.is_object() ? state.find("path") : state.end();
  const auto values = state.is_object() ? state.find("values") : state.end();
  const bool match = path != state.end() && *path == controls->path && values != state.end() && values->is_object();
  for (size_t index = 0; index < controls->specs.size(); ++index)
  {
    const auto& spec = controls->specs[index];
    const auto value = match ? values->find(spec.name) : state.end();
    controls->Set(index, match && value != values->end() && value->is_number() ? value->get<float>() : spec.nominal);
  }
}
}

std::shared_ptr<ModelControls> NeuralAmpModeler::_GetEffectModelControlsLocked(int id, int channel) const
{
  if (id == kMainNam)
    return mPrimaryModelControls[GetParam(kNAMLink)->Bool() ? 0 : std::clamp(channel, 0, 1)];
  const auto block = std::find_if(mEditedEffects.begin(), mEditedEffects.end(),
                                  [id](const EffectBlock& effect) { return effect.settings.id == id; });
  return block != mEditedEffects.end() ? block->controls : nullptr;
}

std::shared_ptr<const ModelControls> NeuralAmpModeler::GetEffectModelControls(int id, int channel) const
{
  std::lock_guard<std::mutex> lock(mEffectsMutex);
  return _GetEffectModelControlsLocked(id, channel);
}

void NeuralAmpModeler::SetEffectModelControl(int id, int channel, size_t index, float value)
{
  {
    std::lock_guard<std::mutex> lock(mEffectsMutex);
    const auto controls = _GetEffectModelControlsLocked(id, channel);
    if (!controls)
      return;
    controls->Set(index, value);
  }
  _MarkCurrentInternalPresetDirty();
}
const char* NeuralAmpModeler::EffectName(EffectType type)
{
  switch (type)
  {
    case EffectType::Gate: return "Gate";
    case EffectType::Nam: return "NAM";
    case EffectType::Cab: return "Cab";
  }
  return "";
}

std::vector<NeuralAmpModeler::EffectBlock> NeuralAmpModeler::_DefaultEffects()
{
  return {
    {{kMainNam, EffectType::Nam}, {}},
    {{kMainGate, EffectType::Gate, true, -60.0, kChainInput}, {}},
    {{kMainCab, EffectType::Cab}, {}}
  };
}

std::vector<NeuralAmpModeler::EffectSettings> NeuralAmpModeler::GetEffects() const
{
  std::lock_guard<std::mutex> lock(mEffectsMutex);
  std::vector<EffectSettings> effects;
  effects.reserve(mEditedEffects.size());
  for (const auto& block : mEditedEffects)
  {
    auto& settings = effects.emplace_back(block.settings);
    if (settings.id == kMainNam)
    {
      settings.enabled = GetParam(kNAMToggle)->Bool();
      settings.path = mNAMPath.Get();
    }
    else if (settings.id == kMainGate)
    {
      settings.enabled = GetParam(kNoiseGateActive)->Bool();
      settings.threshold = GetParam(kNoiseGateThreshold)->Value();
    }
    else if (settings.id == kMainCab)
    {
      settings.path = mIRPath.Get();
    }
  }
  return effects;
}

int NeuralAmpModeler::AddEffect(EffectType type)
{
  int id = 0;
  {
    std::lock_guard<std::mutex> lock(mEffectsMutex);
    if (mEditedEffects.size() >= kMaxEffectBlocks)
      return 0;

    switch (type)
    {
      case EffectType::Gate: id = kMainGate; break;
      case EffectType::Nam: id = kMainNam; break;
      case EffectType::Cab: id = kMainCab; break;
    }
    while (std::any_of(mEditedEffects.begin(), mEditedEffects.end(),
                       [id](const EffectBlock& block) { return block.settings.id == id; }))
      id = std::max(id + 1, kMainCab + 1);

    EffectSettings settings {id, type};
    auto& block = mEditedEffects.emplace_back(EffectBlock {settings, {}, {}});
    if (id > kMainCab && type == EffectType::Gate)
      block.audio = std::make_shared<EffectAudio>();
    _PublishEffectsLocked();
  }
  _MarkCurrentInternalPresetDirty();
  return id;
}

void NeuralAmpModeler::RemoveEffect(int id)
{
  {
    std::lock_guard<std::mutex> lock(mEffectsMutex);
    const auto it = std::find_if(mEditedEffects.begin(), mEditedEffects.end(),
                                 [id](const EffectBlock& block) { return block.settings.id == id; });
    if (it == mEditedEffects.end())
      return;
    mEditedEffects.erase(it);
    _PublishEffectsLocked();
  }
  _MarkCurrentInternalPresetDirty();
}

void NeuralAmpModeler::MoveEffect(int id, int index)
{
  {
    std::lock_guard<std::mutex> lock(mEffectsMutex);
    const auto it = std::find_if(mEditedEffects.begin(), mEditedEffects.end(),
                                 [id](const EffectBlock& block) { return block.settings.id == id; });
    if (it == mEditedEffects.end())
      return;
    const auto source = static_cast<int>(std::distance(mEditedEffects.begin(), it));
    index = std::clamp(index, 0, static_cast<int>(mEditedEffects.size()) - 1);
    if (source == index)
      return;
    if (source < index)
      std::rotate(it, it + 1, mEditedEffects.begin() + index + 1);
    else
      std::rotate(mEditedEffects.begin() + index, it, it + 1);
    _PublishEffectsLocked();
  }
  _MarkCurrentInternalPresetDirty();
}

void NeuralAmpModeler::_EditEffect(int id, const std::function<void(EffectSettings&)>& edit)
{
  {
    std::lock_guard<std::mutex> lock(mEffectsMutex);
    const auto it = std::find_if(mEditedEffects.begin(), mEditedEffects.end(),
                                 [id](const EffectBlock& block) { return block.settings.id == id; });
    if (it == mEditedEffects.end())
      return;
    edit(it->settings);
    _PublishEffectsLocked();
  }
  _MarkCurrentInternalPresetDirty();
}

void NeuralAmpModeler::SetEffectEnabled(int id, bool enabled)
{
  int param = -1;
  switch (id)
  {
    case kMainNam: param = kNAMToggle; break;
    case kMainGate: param = kNoiseGateActive; break;
  }
  if (param < 0)
  {
    _EditEffect(id, [enabled](EffectSettings& effect) { effect.enabled = enabled; });
    return;
  }

  BeginInformHostOfParamChangeFromUI(param);
  SendParameterValueFromUI(param, enabled ? 1.0 : 0.0);
  EndInformHostOfParamChangeFromUI(param);
#if PLUG_HAS_UI
  SendParameterValueFromDelegate(param, enabled ? 1.0 : 0.0, true);
#endif
}

void NeuralAmpModeler::SetEffectThreshold(int id, double threshold)
{
  _EditEffect(id, [threshold](EffectSettings& effect) {
    effect.threshold = std::clamp(threshold, -100.0, 0.0);
  });
}

void NeuralAmpModeler::SetEffectSidechain(int id, int source)
{
  _EditEffect(id, [source](EffectSettings& effect) { effect.sidechain = source; });
}

std::string NeuralAmpModeler::LoadEffectFile(int id, const std::string& path, int channel)
{
  if (id == kMainNam)
  {
    const WDL_String file(path.c_str());
    return channel == 0 ? _StageModel(file) : _StageModelRight(file);
  }
  if (id == kMainCab)
  {
    const WDL_String file(path.c_str());
    const auto code = channel == 0 ? _StageIR(file) : _StageIRRight(file);
    return code == dsp::wav::LoadReturnCode::SUCCESS ? std::string() : dsp::wav::GetMsgForLoadReturnCode(code);
  }

  EffectSettings settings;
  {
    std::lock_guard<std::mutex> lock(mEffectsMutex);
    const auto it = std::find_if(mEditedEffects.begin(), mEditedEffects.end(),
                                 [id](const EffectBlock& block) { return block.settings.id == id; });
    if (it == mEditedEffects.end() || id <= kMainCab || it->settings.type == EffectType::Gate)
      return "This block cannot load a file here.";
    settings = it->settings;
  }

  settings.path = path;
  std::shared_ptr<EffectAudio> audio;
  std::shared_ptr<ModelControls> controls;
  try
  {
    audio = _BuildEffectAudio(settings, controls);
  }
  catch (const std::exception& error)
  {
    return error.what();
  }

  {
    std::lock_guard<std::mutex> lock(mEffectsMutex);
    const auto it = std::find_if(mEditedEffects.begin(), mEditedEffects.end(),
                                 [id](const EffectBlock& block) { return block.settings.id == id; });
    if (it == mEditedEffects.end() || it->settings.type != settings.type)
      return "This block has been removed.";
    it->settings.path = path;
    it->audio = std::move(audio);
    it->controls = std::move(controls);
    _PublishEffectsLocked();
  }
  _MarkCurrentInternalPresetDirty();
  return {};
}

void NeuralAmpModeler::_RefreshEffectRoutingLocked()
{
  for (auto it = mEditedEffects.begin(); it != mEditedEffects.end(); ++it)
  {
    auto& settings = it->settings;
    if (settings.sidechain > kChainInput &&
        std::none_of(mEditedEffects.begin(), it, [&](const EffectBlock& block) {
          return block.settings.id == settings.sidechain;
        }))
      settings.sidechain = kEffectInput;
  }
}

void NeuralAmpModeler::_PublishEffectsLocked()
{
  _RefreshEffectRoutingLocked();
  mStagedEffects = mEditedEffects;
  mEffectsPending = true;
}

void NeuralAmpModeler::_ApplyEffectStaging()
{
  bool changed = false;
  {
    std::unique_lock<std::mutex> lock(mEffectsMutex, std::try_to_lock);
    if (lock && mEffectsPending)
    {
      // Leave the retired graph in staging so it is reclaimed off the audio thread.
      mActiveEffects.swap(mStagedEffects);
      mEffectsPending = false;
      changed = true;
    }
  }
  if (changed)
    _UpdateLatency();
}

std::shared_ptr<NeuralAmpModeler::EffectAudio> NeuralAmpModeler::_BuildEffectAudio(
  const EffectSettings& settings, std::shared_ptr<ModelControls>& controls)
{
  auto audio = std::make_shared<EffectAudio>();
  if (settings.path.empty())
    return audio;

  const auto rate = GetSampleRate() > 0.0 ? GetSampleRate() : 48000.0;
  const auto frames = std::max(1, GetBlockSize());
  if (settings.type == EffectType::Nam)
  {
    const WDL_String path(settings.path.c_str());
    const int phase = _GetAntiAliasFilterPhaseIndex();
    for (auto& model : audio->nam)
    {
      model = _CreateModel(path, nullptr, &controls);
      model->SetOversamplingFactor(_GetActiveOversamplingFactor());
      model->SetAntiAliasFilterPhase(phase == 0 ? dsp::EAntiAliasFilterPhase::MinimumPhaseCascadedFIR
                                  : phase == 1 ? dsp::EAntiAliasFilterPhase::LinearCascadedFIRShort
                                               : dsp::EAntiAliasFilterPhase::LinearCascadedFIRLong);
    }
    for (auto& output : audio->output)
      output.resize(frames);
  }
  else if (settings.type == EffectType::Cab)
  {
    auto path = std::filesystem::u8path(settings.path);
    audio->cab[0] = std::make_unique<dsp::ImpulseResponse>(path.string().c_str(), rate);
    const auto result = audio->cab[0]->GetWavState();
    if (result != dsp::wav::LoadReturnCode::SUCCESS)
      throw std::runtime_error(dsp::wav::GetMsgForLoadReturnCode(result));
    audio->cab[1] = std::make_unique<dsp::ImpulseResponse>(audio->cab[0]->GetData(), rate);
  }
  return audio;
}

void NeuralAmpModeler::_ResetEffects(double sampleRate, int maxBlockSize)
{
  for (auto& tap : mEffectTaps)
    for (auto& channel : tap)
      channel.resize(maxBlockSize);

  std::lock_guard<std::mutex> lock(mEffectsMutex);
  const auto reset = [&](std::vector<EffectBlock>& effects) {
    for (auto& block : effects)
    {
      if (!block.audio)
        continue;
      for (auto& model : block.audio->nam)
        if (model)
          model->Reset(sampleRate, maxBlockSize);
      for (auto& cab : block.audio->cab)
        if (cab)
          cab = std::make_unique<dsp::ImpulseResponse>(cab->GetData(), sampleRate);
    }
  };
  reset(mActiveEffects);
  if (mEffectsPending)
  {
    for (auto& block : mStagedEffects)
    {
      if (!block.audio || std::any_of(mActiveEffects.begin(), mActiveEffects.end(),
                                        [&](const EffectBlock& active) { return active.audio == block.audio; }))
        continue;
      for (auto& model : block.audio->nam)
        if (model)
          model->Reset(sampleRate, maxBlockSize);
      for (auto& cab : block.audio->cab)
        if (cab)
          cab = std::make_unique<dsp::ImpulseResponse>(cab->GetData(), sampleRate);
    }
  }
}

void NeuralAmpModeler::_ApplyExtraModelQuality(int oversamplingFactor, dsp::EAntiAliasFilterPhase phase)
{
  for (auto& block : mActiveEffects)
  {
    if (!block.audio)
      continue;
    for (auto& model : block.audio->nam)
    {
      if (model)
      {
        model->SetOversamplingFactor(oversamplingFactor);
        model->SetAntiAliasFilterPhase(phase);
      }
    }
  }
}

std::string NeuralAmpModeler::_SerializeEffects() const
{
  std::lock_guard<std::mutex> lock(mEffectsMutex);
  auto state = nlohmann::json::array();
  for (const auto& block : mEditedEffects)
  {
    const auto& effect = block.settings;
    state.push_back({{"id", effect.id}, {"type", static_cast<int>(effect.type)}, {"enabled", effect.enabled},
                     {"threshold", effect.threshold}, {"sidechain", effect.sidechain}, {"path", effect.path},
                     {"controls", SerializeModelControls(block.controls)}});
  }
  auto primary = nlohmann::json::array();
  for (const auto& controls : mPrimaryModelControls)
    primary.push_back(SerializeModelControls(controls));
  return nlohmann::json {{"blocks", state}, {"primary", primary}}.dump();
}

void NeuralAmpModeler::_RestoreEffects(const std::string& state)
{
  _RestoreEffects(nlohmann::json::parse(state, nullptr, false));
}

void NeuralAmpModeler::_RestoreEffects(const nlohmann::json& state)
{
  auto effects = _DefaultEffects();
  const auto& blocks = state.is_object() && state.contains("blocks") ? state.at("blocks") : state;
  if (blocks.is_array())
  {
    effects.clear();
    for (const auto& entry : blocks)
    {
      if (effects.size() == kMaxEffectBlocks)
        break;
      try
      {
        EffectSettings effect;
        effect.id = entry.at("id").get<int>();
        const int type = entry.at("type").get<int>();
        if (effect.id < 1 || effect.id > kMaxEffectBlocks + kMainCab || type < 0 || type > 2 ||
            (effect.id == kMainGate && type != static_cast<int>(EffectType::Gate)) ||
            (effect.id == kMainNam && type != static_cast<int>(EffectType::Nam)) ||
            (effect.id == kMainCab && type != static_cast<int>(EffectType::Cab)) ||
            std::any_of(effects.begin(), effects.end(), [&](const EffectBlock& existing) {
              return existing.settings.id == effect.id;
            }))
          continue;

        effect.type = static_cast<EffectType>(type);
        effect.enabled = entry.value("enabled", true);
        effect.threshold = std::clamp(entry.value("threshold", -60.0), -100.0, 0.0);
        effect.sidechain = entry.value("sidechain", kEffectInput);
        effect.path = entry.value("path", "");
        auto& block = effects.emplace_back(EffectBlock {effect, {}});
        if (effect.id > kMainCab)
        {
          block.audio = _BuildEffectAudio(effect, block.controls);
          RestoreModelControls(block.controls, entry.value("controls", nlohmann::json()));
        }
      }
      catch (const std::exception& error)
      {
        // A moved file leaves its slot and path available for relinking.
        std::cerr << "Could not restore effect: " << error.what() << '\n';
      }
    }
  }

  std::lock_guard<std::mutex> lock(mEffectsMutex);
  mEditedEffects = std::move(effects);
  const auto primary = state.is_object() ? state.value("primary", nlohmann::json()) : nlohmann::json();
  for (size_t channel = 0; channel < mPrimaryModelControls.size(); ++channel)
    if (channel == 0 || mPrimaryModelControls[channel] != mPrimaryModelControls[0])
      RestoreModelControls(mPrimaryModelControls[channel], primary.is_array() && channel < primary.size() ?
                           primary[channel] : nlohmann::json());
  _PublishEffectsLocked();
}

sample** NeuralAmpModeler::_ProcessGate(EffectAudio& dsp, double threshold, sample** detector,
                                        sample** input, size_t channels, int frames)
{
  dsp.trigger.SetSampleRate(GetSampleRate());
  dsp.trigger.SetParams(dsp::noise_gate::TriggerParams(0.01, threshold, 0.1, 0.005, 0.01, 0.05));
  dsp.trigger.Process(detector, channels, frames);
  return dsp.gain.Process(input, channels, frames);
}

sample** NeuralAmpModeler::_ProcessExtraEffect(const EffectBlock& block, sample** input, size_t channels, int frames)
{
  auto& audio = *block.audio;
  if (audio.nam[0])
  {
    for (size_t channel = 0; channel < channels; ++channel)
    {
      if (audio.output[channel].size() < static_cast<size_t>(frames))
        audio.output[channel].resize(frames);
      audio.pointers[channel] = audio.output[channel].data();
      audio.nam[channel]->SetAudioWorkgroup(mAudioWorkgroup.load(std::memory_order_acquire));
    }
    if (channels == kNumChannelsStereo)
      audio.nam[0]->process_stereo(*audio.nam[1], input, audio.pointers.data(), input + 1, audio.pointers.data() + 1, frames);
    else
      audio.nam[0]->process(input, audio.pointers.data(), frames);
    return audio.pointers.data();
  }
  if (audio.cab[0])
  {
    for (size_t channel = 0; channel < channels; ++channel)
      audio.pointers[channel] = audio.cab[channel]->Process(input + channel, kNumChannelsMono, frames)[0];
    return audio.pointers.data();
  }
  return input;
}

void NeuralAmpModeler::_SaveEffectTap(size_t tap, sample** input, size_t channels, int frames)
{
  for (size_t channel = 0; channel < channels; ++channel)
  {
    auto& buffer = mEffectTaps[tap][channel];
    if (buffer.size() < static_cast<size_t>(frames))
      buffer.resize(frames);
    std::copy_n(input[channel], frames, buffer.data());
    mEffectTapPointers[tap][channel] = buffer.data();
  }
}



void NeuralAmpModeler::SelectEffect(int id)
{
  mSelectedEffect = id;
#if PLUG_HAS_UI
  _UpdateLinkAndBrowserAvailability();
#endif
}
