#include "ParametricModel.h"
#include "../NeuralAmpModelerCore/NAM/slimmable.h"

// Keep the time-scaling/phase-parallel stock engine and the PNAM engine in
// separate namespaces. A stock model is always handled by the original engine.
#define nam pnamcore
#include "../ParametricCore/NAM/get_dsp.h"
#include "../ParametricCore/NAM/wavenet/hyper_a2.h"
#undef nam

#include <algorithm>
#include <array>
#include <cmath>

ModelControls::ModelControls(std::vector<ModelControl> spec, std::string file)
: specs(std::move(spec)), path(std::move(file)), mValues(std::make_unique<std::atomic<float>[]>(specs.size()))
{
  for (size_t index = 0; index < specs.size(); ++index)
    Set(index, specs[index].nominal);
}

void ModelControls::Set(size_t index, float value)
{
  if (index >= specs.size() || !std::isfinite(value))
    return;
  const auto& spec = specs[index];
  if (!spec.choices.empty())
    value = std::round(value);
  mValues[index].store(std::clamp(value, spec.minimum, spec.maximum), std::memory_order_relaxed);
}

namespace
{
class ParametricDSP : public nam::DSP
{
public:
  ParametricDSP(std::unique_ptr<pnamcore::DSP> main, std::unique_ptr<pnamcore::DSP> lite,
                std::shared_ptr<ModelControls> controls)
  : nam::DSP(1, 1, main->GetExpectedSampleRate()), mModels {std::move(main), std::move(lite)},
    mControls(std::move(controls)), mValues(mControls->specs.size())
  {
    auto& source = *mModels[0];
    for (size_t index = 0; index < mModels.size(); ++index)
    {
      if (mModels[index])
        mControlTargets[index] = &dynamic_cast<pnamcore::IParametricControl&>(*mModels[index]);
    }
    if (source.HasLoudness())
      SetLoudness(source.GetLoudness());
    if (source.HasInputLevel())
      SetInputLevel(source.GetInputLevel());
    if (source.HasOutputLevel())
      SetOutputLevel(source.GetOutputLevel());
  }

  void process(NAM_SAMPLE** input, NAM_SAMPLE** output, int frames) override
  {
    ApplyControls(mSelected);
    mModels[mSelected]->process(input, output, frames);
  }

  void Reset(double sampleRate, int maxBlockSize) override
  {
    nam::DSP::Reset(sampleRate, maxBlockSize);
    for (size_t index = 0; index < mModels.size(); ++index)
    {
      if (!mModels[index])
        continue;
      ApplyControls(index);
      mModels[index]->Reset(sampleRate, maxBlockSize);
    }
  }

  void SelectSize(double size)
  {
    const size_t selected = size < 0.5 ? 1 : 0;
    if (selected != mSelected)
    {
      mSelected = selected;
      ApplyControls(mSelected);
      if (GetMaxBufferSize() > 0)
        mModels[mSelected]->Reset(mExternalSampleRate, GetMaxBufferSize());
    }
  }

private:
  void ApplyControls(size_t index)
  {
    auto& target = *mControlTargets[index];
    const auto current = target.GetParams();
    bool changed = false;
    for (size_t i = 0; i < mValues.size(); ++i)
    {
      mValues[i] = mControls->Get(i);
      changed |= current[i] != mValues[i];
    }
    if (changed)
      target.SetParams(mValues);
  }

  std::array<std::unique_ptr<pnamcore::DSP>, 2> mModels;
  std::array<pnamcore::IParametricControl*, 2> mControlTargets {};
  std::shared_ptr<ModelControls> mControls;
  std::vector<float> mValues;
  size_t mSelected = 0;
};

class HyperA2DSP final : public ParametricDSP, public nam::SlimmableModel
{
public:
  using ParametricDSP::ParametricDSP;
  void SetSlimmableSize(double size) override { SelectSize(size); }
};
}

std::unique_ptr<nam::DSP> LoadParametricModel(const nlohmann::json& file, const std::string& path,
                                            std::shared_ptr<ModelControls>& controls, nam::dspData* metadata)
{
  const auto& model = file.contains("embedded_parametric") ? file.at("embedded_parametric") : file;
  if (!model.is_object() || !model.contains("architecture") || !model.at("architecture").is_string())
    throw std::runtime_error("Invalid NAM architecture.");
  const std::string architecture = model.at("architecture").get<std::string>();
  if (architecture != "HyperWaveNet" && architecture != "HyperA2" &&
      architecture != "ConcatWaveNet" && architecture != "ConcatLSTM")
    return {};

  const bool hasSizes = architecture == "HyperA2";
  auto main = pnamcore::get_dsp(hasSizes ? pnamcore::wavenet::SelectHyperA2Branch(model, false) : model,
                                pnamcore::DspLoadOptions {false});
  if (!main || main->NumInputChannels() != 1 || main->NumOutputChannels() != 1)
    throw std::runtime_error("Parametric models must have one audio input and one output.");

  const auto& target = dynamic_cast<pnamcore::IParametricControl&>(*main);
  std::vector<ModelControl> specs;
  for (const auto& spec : target.GetParamSpecs())
    specs.push_back({spec.name, spec.min, spec.max, spec.defaultValue, spec.enum_names});
  if (!controls || controls->path != path || controls->specs != specs)
    controls = std::make_shared<ModelControls>(std::move(specs), path);

  std::unique_ptr<nam::DSP> dsp;
  if (hasSizes)
  {
    auto lite = pnamcore::get_dsp(pnamcore::wavenet::SelectHyperA2Branch(model, true),
                                 pnamcore::DspLoadOptions {false});
    dsp = std::make_unique<HyperA2DSP>(std::move(main), std::move(lite), controls);
  }
  else
    dsp = std::make_unique<ParametricDSP>(std::move(main), nullptr, controls);

  if (metadata)
  {
    metadata->metadata = file.value("metadata", model.value("metadata", nlohmann::json::object()));
    metadata->config = model.at("config");
  }
  return dsp;
}
