#include "NeuralAmpModeler.h"
#include "LinuxEditor.h"

#include <alsa/asoundlib.h>
#include <cmath>
#include <iostream>
#include <numbers>

namespace
{
constexpr unsigned kSampleRate = 48000;
constexpr int kFrames = 128;

void SetParam(NeuralAmpModeler& plugin, int index, double value)
{
  plugin.GetParam(index)->Set(value);
  plugin.OnParamChange(index);
}

void Configure(NeuralAmpModeler& plugin, bool offline = false)
{
  Steinberg::Vst::ProcessSetup setup {};
  setup.sampleRate = kSampleRate;
  setup.maxSamplesPerBlock = kFrames;
  setup.symbolicSampleSize = Steinberg::Vst::kSample64;
  setup.processMode = offline ? Steinberg::Vst::kOffline : Steinberg::Vst::kRealtime;
  if (plugin.setupProcessing(setup) != Steinberg::kResultOk)
    throw std::runtime_error("Could not initialize the audio engine.");
}

class LinuxAudio
{
public:
  explicit LinuxAudio(NeuralAmpModeler& plugin) : mPlugin(plugin) {}
  ~LinuxAudio() { Stop(); }

  bool IsRunning() const { return mThread.joinable(); }

  std::string Start()
  {
    for (int index = 0; index < 2; ++index)
    {
      const auto direction = index == 0 ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK;
      auto code = snd_pcm_open(&mDevices[index], "default", direction, 0);
      if (code >= 0)
        code = snd_pcm_set_params(mDevices[index], SND_PCM_FORMAT_FLOAT_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                  2, kSampleRate, 1, 20000);
      if (code < 0)
      {
        const std::string description = index == 0 ? "Audio input: " : "Audio output: ";
        Stop();
        return description + snd_strerror(code);
      }
      snd_pcm_nonblock(mDevices[index], 1);
    }
    mStop.store(false);
    mThread = std::thread([this] { Process(); });
    return {};
  }

  void Stop()
  {
    mStop.store(true);
    if (mThread.joinable())
      mThread.join();
    for (auto& device : mDevices)
    {
      if (!device)
        continue;
      snd_pcm_drop(device);
      snd_pcm_close(device);
      device = nullptr;
    }
  }

private:
  bool Retry(snd_pcm_t* pcm, snd_pcm_sframes_t status)
  {
    if (status == -EAGAIN || status == 0)
      return snd_pcm_wait(pcm, 25) >= 0;
    return snd_pcm_recover(pcm, static_cast<int>(status), 1) >= 0;
  }

  void Process()
  {
    std::array<float, kFrames * 2> samples {};
    std::array<double, kFrames> inputLeft {}, inputRight {}, outputLeft {}, outputRight {};
    double* inputs[] = {inputLeft.data(), inputRight.data()};
    double* outputs[] = {outputLeft.data(), outputRight.data()};
    while (!mStop.load())
    {
      const auto frames = snd_pcm_readi(mDevices[0], samples.data(), kFrames);
      if (frames <= 0)
      {
        if (!Retry(mDevices[0], frames))
          return;
        continue;
      }
      for (int frame = 0; frame < frames; ++frame)
      {
        inputLeft[frame] = samples[frame * 2];
        inputRight[frame] = samples[frame * 2 + 1];
      }
      mPlugin.ProcessBlock(inputs, outputs, static_cast<int>(frames));
      for (int frame = 0; frame < frames; ++frame)
      {
        samples[frame * 2] = static_cast<float>(std::clamp(outputLeft[frame], -1.0, 1.0));
        samples[frame * 2 + 1] = static_cast<float>(std::clamp(outputRight[frame], -1.0, 1.0));
      }

      snd_pcm_sframes_t written = 0;
      while (written < frames && !mStop.load())
      {
        const auto count = snd_pcm_writei(mDevices[1], samples.data() + 2 * written, frames - written);
        if (count > 0)
          written += count;
        else if (!Retry(mDevices[1], count))
          return;
      }
    }
  }

  NeuralAmpModeler& mPlugin;
  std::array<snd_pcm_t*, 2> mDevices {};
  std::atomic<bool> mStop {false};
  std::thread mThread;
};

std::vector<double> Render(NeuralAmpModeler& plugin)
{
  std::array<double, kFrames> left {}, right {}, outLeft {}, outRight {};
  double* input[] = {left.data(), right.data()};
  double* output[] = {outLeft.data(), outRight.data()};
  for (int chunk = 0; chunk < 42; ++chunk)
  {
    for (int frame = 0; frame < kFrames; ++frame)
      left[frame] = 0.15 * std::sin(2.0 * std::numbers::pi * (chunk * kFrames + frame) / 53.0);
    plugin.ProcessBlock(input, output, kFrames);
  }
  return {outLeft.begin(), outLeft.end()};
}

double Difference(const std::vector<double>& left, const std::vector<double>& right)
{
  double error = 0.0;
  for (size_t frame = 0; frame < left.size(); ++frame)
  {
    if (!std::isfinite(left[frame]) || !std::isfinite(right[frame]))
      throw std::runtime_error("Non-finite inference output.");
    error += std::abs(left[frame] - right[frame]);
  }
  return error;
}

int SelfTest(const char* model)
{
  using Type = NeuralAmpModeler::EffectType;
  NeuralAmpModeler plugin(iplug::InstanceInfo {});
  Configure(plugin, true);
  SetParam(plugin, kOutputMode, 0);
  SetParam(plugin, kOfflineOversamplingFactor, 0);
  const auto result = plugin.LoadEffectFile(1, model);
  if (!result.empty())
    throw std::runtime_error(result);
  const auto controls = plugin.GetEffectModelControls(1);
  if (!controls || controls->specs.empty())
    throw std::runtime_error("Parametric controls were not discovered.");

  const auto& spec = controls->specs.front();
  plugin.SetEffectModelControl(1, 0, 0, spec.minimum);
  const auto low = Render(plugin);
  plugin.SetEffectModelControl(1, 0, 0, spec.maximum);
  const auto high = Render(plugin);
  if (Difference(low, high) < 1e-4)
    throw std::runtime_error("Model control did not affect inference.");

  SetParam(plugin, kOfflineAntiAliasFilterPhase, 2);
  SetParam(plugin, kOfflineOversamplingFactor, 2);
  const auto fourTimes = Render(plugin);
  if (plugin.GetLatency() <= 0)
    throw std::runtime_error("The oversampled PNAM path did not report linear-phase latency.");
  if (Difference(high, fourTimes) < 1e-5)
    throw std::runtime_error("Offline oversampling did not change the signal.");

  const int nam = plugin.AddEffect(Type::Nam);
  const int gate = plugin.AddEffect(Type::Gate);
  const auto secondModel = plugin.LoadEffectFile(nam, model);
  if (!secondModel.empty())
    throw std::runtime_error(secondModel);
  plugin.SetEffectSidechain(gate, nam);
  const auto chained = Render(plugin);
  if (Difference(fourTimes, chained) < 1e-5)
    throw std::runtime_error("Additional blocks did not change the signal.");

  iplug::IByteChunk state;
  if (!plugin.SerializeState(state))
    throw std::runtime_error("Could not serialize signal chain.");
  NeuralAmpModeler reopened(iplug::InstanceInfo {});
  Configure(reopened);
  if (reopened.UnserializeState(state, 0) != state.Size())
    throw std::runtime_error("Could not restore the complete 3.0.0 state chunk.");
  const auto effects = reopened.GetEffects();
  const auto restoredControls = reopened.GetEffectModelControls(1);
  if (effects.size() != 5 || effects.back().id != gate)
    throw std::runtime_error("The saved signal chain did not restore.");
  if (effects.back().sidechain != nam || !restoredControls || restoredControls->Get(0) != spec.maximum)
    throw std::runtime_error("Sidechain routing or model controls did not restore.");
  reopened.MoveEffect(gate, 0);
  if (reopened.GetEffects().front().sidechain != NeuralAmpModeler::kEffectInput)
    throw std::runtime_error("A moved block retained a downstream sidechain.");
  std::cout << "PNAM controls, oversampling, repeated blocks, routing and state passed\n";
  return 0;
}
}

int main(int argc, char** argv)
{
  if (argc == 3 && std::strcmp(argv[1], "--self-test") == 0)
  {
    try { return SelfTest(argv[2]); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
  }
  if (argc == 2 && std::strcmp(argv[1], "--version") == 0)
  {
    std::cout << "NAM On Steroids " << PLUG_VERSION_STR << '\n';
    return 0;
  }
  if (!gtk_init_check(&argc, &argv))
  {
    std::cerr << "NAM On Steroids requires a graphical session.\n";
    return 1;
  }

  NeuralAmpModeler plugin(iplug::InstanceInfo {});
  Configure(plugin);
  LinuxAudio audio(plugin);
  auto* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_widget_set_name(window, "nam-window");
  gtk_window_set_title(GTK_WINDOW(window), "NAM On Steroids");
  gtk_window_set_default_size(GTK_WINDOW(window), PLUG_WIDTH, PLUG_HEIGHT);
  auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(window), root);
  gtk_box_pack_start(GTK_BOX(root), MakeLinuxEditor(plugin), true, true, 0);

  auto* audioBar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
  gtk_widget_set_name(audioBar, "nam-audio");
  gtk_container_set_border_width(GTK_CONTAINER(audioBar), 8);
  gtk_box_pack_start(GTK_BOX(root), audioBar, false, false, 0);
  auto* start = gtk_button_new_with_label("Start audio");
  auto* status = gtk_label_new("ALSA default input and output");
  gtk_box_pack_start(GTK_BOX(audioBar), start, false, false, 0);
  gtk_widget_set_name(status, "nam-rotary-value");
  gtk_label_set_xalign(GTK_LABEL(status), 0);
  gtk_box_pack_start(GTK_BOX(audioBar), status, true, true, 0);
  struct Binding { LinuxAudio* audio; GtkWidget* status; } binding {&audio, status};
  g_signal_connect(start, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer ptr) {
    auto& api = *static_cast<Binding*>(ptr);
    if (api.audio->IsRunning())
    {
      api.audio->Stop();
      gtk_button_set_label(button, "Start audio");
      gtk_label_set_text(GTK_LABEL(api.status), "Audio stopped");
      return;
    }
    const auto error = api.audio->Start();
    if (error.empty())
      gtk_button_set_label(button, "Stop audio");
    gtk_label_set_text(GTK_LABEL(api.status), error.empty() ? "Audio running at 48 kHz" : error.c_str());
  }), &binding);
  g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
  gtk_widget_show_all(window);
  gtk_main();
  return 0;
}
