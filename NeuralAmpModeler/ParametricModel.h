#pragma once

#include "../NeuralAmpModelerCore/NAM/get_dsp.h"
#include <atomic>
#include <memory>

struct ModelControl
{
  std::string name;
  float minimum;
  float maximum;
  float nominal;
  std::vector<std::string> choices;
  bool operator==(const ModelControl&) const = default;
};

class ModelControls
{
public:
  ModelControls(std::vector<ModelControl> spec, std::string file);
  float Get(size_t index) const { return mValues[index].load(std::memory_order_relaxed); }
  void Set(size_t index, float value);

  const std::vector<ModelControl> specs;
  const std::string path;

private:
  std::unique_ptr<std::atomic<float>[]> mValues;
};

std::unique_ptr<nam::DSP> LoadParametricModel(const nlohmann::json& file, const std::string& path,
                                            std::shared_ptr<ModelControls>& controls, nam::dspData* metadata);
