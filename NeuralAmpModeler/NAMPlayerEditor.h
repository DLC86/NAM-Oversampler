#pragma once

// Palette and rotary design adapted from Phillip Self's MIT-licensed PNAM Player.
namespace PlayerPalette
{
inline const IColor surface(255, 15, 15, 15);
inline const IColor bar(255, 27, 27, 27);
inline const IColor border(255, 41, 41, 41);
inline const IColor track(255, 45, 45, 45);
inline const IColor text(255, 237, 237, 237);
inline const IColor muted(255, 150, 150, 150);
inline const IColor accent(255, 191, 0, 0);
}

class NAMPlayerMeter final : public IControl
{
public:
  explicit NAMPlayerMeter(IRECT bounds) : IControl(bounds) { SetIgnoreMouse(true); }

  void OnMsgFromDelegate(int tag, int size, const void* data) override
  {
    if (tag == ISender<>::kUpdateMessage && size == sizeof(mData))
    {
      std::memcpy(&mData, data, sizeof(mData));
      SetDirty(false);
    }
  }

  void Draw(IGraphics& g) override
  {
    const int channels = std::clamp(mData.nChans, 1, 2);
    for (int channel = 0; channel < channels; ++channel)
    {
      const auto r = mRECT.SubRectVertical(channels, channel).GetPadded(-1.f);
      const float db = 20.f * std::log10(std::max(1.e-6f, mData.vals[channel].first));
      g.FillRoundRect(PlayerPalette::track, r, 1.f);
      const float level = std::clamp((db + 60.f) / 60.f, 0.f, 1.f);
      if (level > 0)
        g.FillRoundRect(db >= 0 ? IColor(255, 255, 94, 82) : PlayerPalette::accent, r.GetFromLeft(r.W() * level), 1.f);
    }
  }

private:
  ISenderData<2, std::pair<float, float>> mData;
};

class NAMPlayerEditor final : public IControl
{
public:
  NAMPlayerEditor(IRECT bounds, NeuralAmpModeler& plugin) : IControl(bounds), mPlugin(plugin) {}

  void OnGUIIdle() override { SetDirty(false); }

  void Draw(IGraphics& g) override
  {
    mButtons.clear();
    mRotaries.clear();
    mCards.clear();
    g.FillRect(PlayerPalette::surface, mRECT);
    Header(g);
    if (mView == View::Chain)
      Chain(g);
    else
      Utility(g);
    if (!mStatus.empty())
      Text(g, mStatus, {25, 479, 835, 502}, 12, PlayerPalette::muted);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    for (size_t index = 0; index < mCards.size(); ++index)
    {
      if (!mCards[index].bounds.Contains(x, y))
        continue;
      const int id = mCards[index].id;
      mPlugin.SelectEffect(id);
      mChannel = mControlsPage = 0;
      if (mod.R)
        Choose({"Move left", "Move right", "Remove block"}, mCards[index].bounds, [this, id, index](int selected) {
          if (selected == 2)
            mPlugin.RemoveEffect(id);
          else
            mPlugin.MoveEffect(id, static_cast<int>(index) + (selected == 0 ? -1 : 1));
        });
      else
      {
        mMoving = id;
        mPress = {x, y};
      }
      return;
    }
    if (const auto* knob = FindRotary(x, y))
    {
      if (mod.R && knob->host >= 0 && mPlugin.IsMidiAssignableParam(knob->host))
        Midi(knob->host, knob->bounds);
      else
      {
        mDrag = *knob;
        if (mDrag->host >= 0)
          mPlugin.BeginInformHostOfParamChangeFromUI(mDrag->host);
      }
      return;
    }
    for (auto it = mButtons.rbegin(); it != mButtons.rend(); ++it)
    {
      if (it->bounds.Contains(x, y))
      {
        auto action = it->action;
        action();
        return;
      }
    }
  }

  void OnMouseDrag(float x, float y, float, float dy, const IMouseMod& mod) override
  {
    if (mDrag)
    {
      mDrag->value = std::clamp(mDrag->value - dy * (mod.S ? .0015 : .0075), 0.0, 1.0);
      mDrag->set(mDrag->value);
    }
    else if (mMoving > 0 && std::hypot(x - mPress.first, y - mPress.second) > 4)
    {
      float nearest = std::numeric_limits<float>::max();
      for (size_t index = 0; index < mCards.size(); ++index)
      {
        const auto& bounds = mCards[index].bounds;
        const float distance = std::hypot(x - bounds.MW(), y - bounds.MH());
        if (distance < nearest)
        {
          nearest = distance;
          mDrop = static_cast<int>(index);
        }
      }
    }
    SetDirty(false);
  }

  void OnMouseUp(float, float, const IMouseMod&) override
  {
    if (mDrag && mDrag->host >= 0)
      mPlugin.EndInformHostOfParamChangeFromUI(mDrag->host);
    mDrag.reset();
    if (mMoving > 0 && mDrop >= 0)
      mPlugin.MoveEffect(mMoving, mDrop);
    mMoving = 0;
    mDrop = -1;
  }

  void OnMouseDblClick(float x, float y, const IMouseMod&) override
  {
    if (const auto* knob = FindRotary(x, y))
      Adjust(*knob, knob->nominal);
  }

  void OnMouseWheel(float x, float y, const IMouseMod& mod, float delta) override
  {
    if (const auto* knob = FindRotary(x, y))
      Adjust(*knob, knob->value + delta * (mod.S ? .002 : .015));
  }

  void OnPopupMenuSelection(IPopupMenu* menu, int) override
  {
    auto action = std::move(mChoose);
    const int index = menu ? menu->GetChosenItemIdx() : -1;
    if (action && index >= 0)
      action(index);
  }

  void OnTextEntryCompletion(const char* text, int) override
  {
    auto action = std::move(mEnter);
    if (action && text)
      action(text);
  }

  void OnDrop(const char* path) override
  {
    const auto extension = Extension(std::filesystem::u8path(path));
    if (extension != ".nam" && extension != ".pnam" && extension != ".wav")
      return;
    const auto type = extension == ".wav" ? Type::Cab : Type::Nam;
    const auto effects = mPlugin.GetEffects();
    int id = 0;
    for (const auto& effect : effects)
      if (effect.type == type && (id == 0 || effect.id == mPlugin.GetSelectedEffect()))
        id = effect.id;
    if (!id)
      id = mPlugin.AddEffect(type);
    if (id)
    {
      Open(View::Chain);
      mPlugin.SelectEffect(id);
      Load(id, path, 0);
    }
  }

private:
  using Type = NeuralAmpModeler::EffectType;
  using Effect = NeuralAmpModeler::EffectSettings;
  using Setter = std::function<void(double)>;
  enum class View { Chain, Mixer, Filters, Quality, Tuner, Settings, Midi, Circuit, About };
  struct Hit { IRECT bounds; std::function<void()> action; };
  struct Card { IRECT bounds; int id; };
  struct Knob { IRECT bounds; double value, nominal; Setter set; int host; };

  NeuralAmpModeler& mPlugin;
  View mView = View::Chain;
  std::vector<Hit> mButtons;
  std::vector<Knob> mRotaries;
  std::vector<Card> mCards;
  std::optional<Knob> mDrag;
  IPopupMenu mMenu;
  std::function<void(int)> mChoose;
  std::function<void(const char*)> mEnter;
  std::pair<float, float> mPress;
  int mMoving = 0;
  int mDrop = -1;
  int mChannel = 0;
  int mControlsPage = 0;
  std::string mStatus;

  static constexpr int mainNam = NeuralAmpModeler::kMainNam;
  static constexpr int mainGate = NeuralAmpModeler::kMainGate;
  static constexpr int mainCab = NeuralAmpModeler::kMainCab;

  IColor Accent() const { return mPlugin.GetThemeColor(); }

  static void Text(IGraphics& g, const std::string& value, IRECT r, float size, IColor color, EAlign align = EAlign::Center)
  {
    g.DrawText(IText(size, color, "Roboto-Regular").WithAlign(align), value.c_str(), r);
  }

  static std::string Value(double value, const char* unit = "")
  {
    char text[48];
    std::snprintf(text, sizeof(text), "%.2f%s", value, unit);
    return text;
  }

  static std::string Extension(const std::filesystem::path& file)
  {
    auto extension = file.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return std::tolower(c); });
    return extension;
  }

  void Button(IGraphics& g, IRECT r, const std::string& text, std::function<void()> action, bool on = false)
  {
    g.FillRoundRect(on ? Accent() : PlayerPalette::bar, r, r.H() / 2);
    g.DrawRoundRect(on ? Accent() : PlayerPalette::border, r, r.H() / 2);
    Text(g, text, r.GetPadded(-5), 13, PlayerPalette::text);
    mButtons.push_back({r, std::move(action)});
  }

  void Choose(const std::vector<std::string>& choices, IRECT r, std::function<void(int)> select)
  {
    mMenu.Clear();
    for (const auto& choice : choices)
      mMenu.AddItem(choice.c_str());
    mChoose = std::move(select);
    GetUI()->CreatePopupMenu(*this, mMenu, r);
  }

  void Entry(IRECT r, const std::string& contents, std::function<void(const char*)> action)
  {
    mEnter = std::move(action);
    GetUI()->CreateTextEntry(*this, IText(14, PlayerPalette::text, "Roboto-Regular"), r, contents.c_str());
  }

  void Open(View page)
  {
    mView = page;
    mStatus.clear();
    mPlugin.SetTunerActive(page == View::Tuner);
  }

  void Navigate(IGraphics& g, IRECT r, const char* title, View page, bool related = false)
  {
    Button(g, r, title, [this, page] { Open(page); }, mView == page || related);
  }

  const Knob* FindRotary(float x, float y) const
  {
    auto found = std::find_if(mRotaries.begin(), mRotaries.end(),
      [x, y](const Knob& knob) { return knob.bounds.Contains(x, y); });
    return found != mRotaries.end() ? &*found : nullptr;
  }

  void Adjust(const Knob& knob, double value)
  {
    if (knob.host >= 0)
      mPlugin.BeginInformHostOfParamChangeFromUI(knob.host);
    knob.set(std::clamp(value, 0.0, 1.0));
    if (knob.host >= 0)
      mPlugin.EndInformHostOfParamChangeFromUI(knob.host);
  }

  void Send(int index, double normalized)
  {
    mPlugin.SendParameterValueFromUI(index, normalized);
    mPlugin.SendParameterValueFromDelegate(index, normalized, true);
  }

  void Set(int index, double value)
  {
    Adjust({{}, 0, 0, [this, index](double v) { Send(index, v); }, index}, mPlugin.GetParam(index)->ToNormalized(value));
  }

  void Rotary(IGraphics& g, IRECT bounds, const std::string& name, const std::string& display,
              double ratio, double nominal, Setter setter, int host = -1)
  {
    using namespace PlayerPalette;
    const float radius = std::min(31.f, (bounds.H() - 34.f) / 2);
    const float x = bounds.MW(), y = bounds.MH();
    const float angle = static_cast<float>(ratio) * 270.f - 135.f;
    const float rad = angle * .01745329252f;
    const float thickness = radius * .16f;
    g.DrawArc(track, x, y, radius - 2, -135, 135, nullptr, thickness);
    if (ratio > .001)
      g.DrawArc(Accent(), x, y, radius - 2, -135, angle, nullptr, thickness);
    const float face = radius - thickness * 1.6f - 2;
    const IRECT disk {x - face, y - face, x + face, y + face};
    g.PathClear();
    g.PathEllipse(disk);
    g.PathFill(IPattern::CreateLinearGradient(disk, EDirection::Vertical,
      {IColorStop(IColor(255, 36, 36, 36), 0.f), IColorStop(IColor(255, 28, 28, 28), 1.f)}));
    const float dx = std::sin(rad) * face, dy = -std::cos(rad) * face;
    g.DrawLine(Accent(), x + dx * .35f, y + dy * .35f, x + dx * .85f, y + dy * .85f, nullptr, thickness * .55f);
    Text(g, name, bounds.GetFromTop(17), 13, muted);
    Text(g, display, bounds.GetFromBottom(17), 13, text);
    mRotaries.push_back({bounds, ratio, nominal, std::move(setter), host});
  }

  void KnobParam(IGraphics& g, IRECT bounds, int index, const char* name = nullptr)
  {
    const auto* param = mPlugin.GetParam(index);
    WDL_String display;
    param->GetDisplayWithLabel(display);
    Rotary(g, bounds, name ? name : param->GetName(), display.Get(), param->GetNormalized(),
           param->GetDefault(true), [this, index](double v) { Send(index, v); }, index);
  }

  void ChoiceParam(IGraphics& g, IRECT bounds, int index, const char* name = nullptr)
  {
    const auto* param = mPlugin.GetParam(index);
    WDL_String shown;
    param->GetDisplay(shown);
    const bool toggle = param->Type() == IParam::kTypeBool;
    Button(g, bounds, std::string(name ? name : param->GetName()) + "  /  " + shown.Get(),
      [this, bounds, index, toggle] {
        const auto* p = mPlugin.GetParam(index);
        if (toggle)
        {
          Set(index, !p->Bool());
          return;
        }
        const int first = static_cast<int>(p->GetMin());
        std::vector<std::string> choices;
        for (int value = first; value <= static_cast<int>(p->GetMax()); ++value)
        {
          WDL_String item;
          p->GetDisplay(value, false, item);
          choices.emplace_back(item.Get());
        }
        Choose(choices, bounds, [this, index, first](int value) { Set(index, value + first); });
      }, toggle && param->Bool());
  }

  static IRECT Cell(int index, int count, float y)
  {
    const float width = std::min(125.f, 794.f / std::max(1, count));
    const float x = (860 - width * count) / 2 + width * index;
    return {x + 4, y, x + width - 4, y + 100};
  }

  void Header(IGraphics& g)
  {
    using namespace PlayerPalette;
    g.FillRect(bar, {0, 0, 860, 110});
    g.DrawLine(border, 0, 109, 860, 109);
    Text(g, "NAM On Steroids", {202, 14, 658, 64}, 33, Accent());
    Text(g, std::string("v") + PLUG_VERSION_STR, {320, 65, 540, 91}, 14, muted);
    KnobParam(g, {18, 3, 106, 94}, kInputLevel, "Input");
    KnobParam(g, {754, 3, 842, 94}, kOutputLevel, "Output");
    Button(g, {116, 52, 187, 78}, "Boost", [this] { Set(kInputBoost, !mPlugin.GetParam(kInputBoost)->Bool()); },
           mPlugin.GetParam(kInputBoost)->Bool());
    Button(g, {671, 52, 744, 78}, mPlugin.GetParam(kChannelMode)->Int() ? "Stereo" : "Mono",
           [this] { Set(kChannelMode, !mPlugin.GetParam(kChannelMode)->Int()); });
    Button(g, {20, 121, 50, 149}, "<", [this] { mPlugin.SelectAdjacentInternalPreset(-1); });
    Button(g, {55, 121, 233, 149},
           std::string(mPlugin.GetCurrentInternalPresetName()) + (mPlugin.IsCurrentInternalPresetDirty() ? " *" : ""),
           [this] { Presets(false); });
    Button(g, {238, 121, 268, 149}, ">", [this] { mPlugin.SelectAdjacentInternalPreset(1); });
    const IRECT save {275, 121, 345, 149};
    Button(g, save, "Save", [this, save] {
      Choose({"Save preset", "Save to slot...", "Rename..."}, save, [this](int action) {
        if (action == 0)
          mPlugin.SaveCurrentInternalPreset();
        else if (action == 1)
          Presets(true);
        else
          Entry({55, 120, 233, 150}, mPlugin.GetCurrentInternalPresetName(),
                [this](const char* name) { mPlugin.RenameCurrentInternalPreset(name); });
      });
    });
    Navigate(g, {363, 121, 435, 149}, "Chain", View::Chain, mView == View::Circuit);
    Navigate(g, {441, 121, 514, 149}, "Mixer", View::Mixer, mView == View::Filters);
    Navigate(g, {520, 121, 642, 149}, "Oversampling", View::Quality);
    Navigate(g, {648, 121, 722, 149}, "Tuner", View::Tuner);
    Navigate(g, {728, 121, 840, 149}, "Settings", View::Settings, mView == View::Midi || mView == View::About);
    g.DrawLine(border, 20, 159, 840, 159);
  }

  void Presets(bool save)
  {
    std::vector<std::string> names;
    for (int slot = 0; slot < NeuralAmpModeler::GetInternalPresetCount(); ++slot)
      names.emplace_back(std::to_string(slot + 1) + "  " + mPlugin.GetInternalPresetName(slot));
    Choose(names, {55, 121, 233, 149}, [this, save](int slot) {
      if (save)
        mPlugin.SaveCurrentInternalPresetToSlot(slot);
      else
        mPlugin.SelectInternalPreset(slot);
    });
  }

  void Chain(IGraphics& g)
  {
    using namespace PlayerPalette;
    const auto effects = mPlugin.GetEffects();
    Text(g, "Signal chain", {25, 166, 210, 195}, 15, muted, EAlign::Near);
    const IRECT add {713, 170, 840, 196};
    if (effects.size() < NeuralAmpModeler::kMaxEffectBlocks)
      Button(g, add, "+ Add block", [this, add] {
        Choose({"Gate", "NAM", "Cab"}, add, [this](int type) {
          const int id = mPlugin.AddEffect(static_cast<Type>(type));
          if (id)
            mPlugin.SelectEffect(id);
          mControlsPage = mChannel = 0;
        });
      });
    auto selected = std::find_if(effects.begin(), effects.end(), [this](const Effect& effect) {
      return effect.id == mPlugin.GetSelectedEffect();
    });
    if (selected == effects.end() && !effects.empty())
    {
      selected = effects.begin();
      mPlugin.SelectEffect(selected->id);
      mControlsPage = mChannel = 0;
    }

    for (size_t index = 0; index < effects.size(); ++index)
    {
      const auto& effect = effects[index];
      const size_t columns = std::min<size_t>(6, effects.size() - index / 6 * 6);
      const float x = (860 - static_cast<float>(columns) * 130) / 2 + static_cast<float>(index % 6) * 130;
      const float y = 203 + static_cast<float>(index / 6) * 46;
      const IRECT r {x + 3, y, x + 122, y + 39};
      const bool active = selected != effects.end() && selected->id == effect.id;
      g.DrawLine(track, r.L - 10, r.MH(), r.L, r.MH(), nullptr, 2);
      g.FillRoundRect(bar, r, r.H() / 2);
      g.DrawRoundRect(active ? Accent() : border, r, r.H() / 2, nullptr, active ? 1.6f : 1.f);
      g.FillCircle(effect.enabled ? Accent() : track, r.L + 17, r.MH(), 4);
      Text(g, mPlugin.EffectName(effect.type), r.GetReducedFromLeft(14), 15, effect.enabled ? text : muted);
      mCards.push_back({r, effect.id});
    }
    if (mDrop >= 0 && mDrop < static_cast<int>(mCards.size()))
      g.DrawRoundRect(Accent(), mCards[mDrop].bounds.GetPadded(3), 22.f, nullptr, 2.f);
    const float top = effects.size() > 6 ? 301.f : 255.f;
    g.DrawLine(border, 20, top, 840, top);
    if (selected != effects.end())
      Inspector(g, effects, *selected, top);
    else
      Text(g, "Add a block to build your signal chain", {25, top + 24, 835, 460}, 16, muted);
  }

  void Inspector(IGraphics& g, const std::vector<Effect>& effects, const Effect& effect, float top)
  {
    const IRECT row {20, top + 11, 840, top + 40};
    const int id = effect.id;
    const int channel = Channel(effect);
    const auto controls = mPlugin.GetEffectModelControls(id, channel);
    const bool stereoModel = Split(effect) && mPlugin.GetEffectModelControls(id, 1 - channel);
    std::string title = mPlugin.EffectName(effect.type);
    if (controls)
      title += "  /  Model parameters";
    else if (effect.type == Type::Nam && !stereoModel)
      title += "  /  Tone stack";
    Text(g, title, row.GetFromLeft(249), 16, PlayerPalette::muted, EAlign::Near);
    Button(g, {665, row.T, 752, row.B}, effect.enabled ? "Active" : "Bypassed", [this, effect] {
      mPlugin.SetEffectEnabled(effect.id, !effect.enabled);
    }, effect.enabled);
    Button(g, {758, row.T, 840, row.B}, "Remove", [this, id] { mPlugin.RemoveEffect(id); });
    const float y = top + 65;

    if (effect.type == Type::Gate)
    {
      const IRECT threshold {259, y, 365, y + 100};
      if (id == mainGate)
        KnobParam(g, threshold, kNoiseGateThreshold, "Threshold");
      else
        Rotary(g, threshold, "Threshold", Value(effect.threshold, " dB"), (effect.threshold + 100) / 100, .2,
               [this, id](double v) { mPlugin.SetEffectThreshold(id, v * 100 - 100); });
      Sidechain(g, effects, effect, {387, y + 34, 605, y + 66});
      return;
    }
    if (controls)
      Model(g, *controls, id, channel, row, y);
    else if (effect.type == Type::Nam && !stereoModel)
      Tone(g, effect, row, y);
    else if (id == mainCab)
    {
      if (mPlugin.GetParam(kChannelMode)->Int())
        ChoiceParam(g, {268, y + 34, 425, y + 65}, kIRLink, "Link channels");
      ChoiceParam(g, {434, y + 34, 592, y + 65}, channel ? kIRToggleRight : kIRToggle, "Cabinet IR");
    }
    Files(g, effect, channel);
  }

  void Sidechain(IGraphics& g, const std::vector<Effect>& effects, const Effect& gate, IRECT r)
  {
    Text(g, "Sidechain", r.GetVShifted(-25).GetFromTop(18), 13, PlayerPalette::muted);
    std::vector<int> sources {NeuralAmpModeler::kEffectInput, NeuralAmpModeler::kChainInput};
    std::vector<std::string> labels {"Block input", "Chain input"};
    for (const auto& effect : effects)
    {
      if (effect.id == gate.id)
        break;
      labels.push_back(std::to_string(labels.size() - 1) + "  " + mPlugin.EffectName(effect.type) + " output");
      sources.push_back(effect.id);
    }
    const auto found = std::find(sources.begin(), sources.end(), gate.sidechain);
    const auto index = found == sources.end() ? 0 : found - sources.begin();
    Button(g, r, labels[index], [this, r, labels, sources, id = gate.id] {
      Choose(labels, r, [this, id, sources](int choice) { mPlugin.SetEffectSidechain(id, sources[choice]); });
    });
  }

  void Model(IGraphics& g, const ModelControls& controls, int id, int channel, IRECT row, float y)
  {
    const int pages = std::max(1, (static_cast<int>(controls.specs.size()) + 5) / 6);
    mControlsPage = std::clamp(mControlsPage, 0, pages - 1);
    if (pages > 1)
    {
      Button(g, {549, row.T, 577, row.B}, "<", [this] { mControlsPage = std::max(0, mControlsPage - 1); });
      Text(g, std::to_string(mControlsPage + 1) + "/" + std::to_string(pages), {579, row.T, 621, row.B},
           13, PlayerPalette::muted);
      Button(g, {622, row.T, 651, row.B}, ">", [this, pages] { mControlsPage = std::min(pages - 1, mControlsPage + 1); });
    }
    const int first = mControlsPage * 6;
    const int count = std::min(6, static_cast<int>(controls.specs.size()) - first);
    for (int i = 0; i < count; ++i)
    {
      const int index = first + i;
      const auto& info = controls.specs[index];
      const float value = controls.Get(index);
      const auto cell = Cell(i, count, y);
      if (info.choices.empty())
      {
        const float span = std::max(.0001f, info.maximum - info.minimum);
        Rotary(g, cell, info.name, Value(value), (value - info.minimum) / span, (info.nominal - info.minimum) / span,
          [this, id, channel, index, lo = info.minimum, span](double v) {
            mPlugin.SetEffectModelControl(id, channel, index, lo + static_cast<float>(v) * span);
          });
      }
      else
      {
        Text(g, info.name, cell.GetFromTop(17), 13, PlayerPalette::muted);
        const auto rect = cell.GetCentredInside(cell.W(), 31);
        const int selected = static_cast<int>(value);
        auto callback = [this, id, channel, index](int choice) {
          mPlugin.SetEffectModelControl(id, channel, index, static_cast<float>(choice));
        };
        if (info.choices.size() == 2 && info.choices[0].size() + info.choices[1].size() <= 14)
        {
          for (int choice = 0; choice < 2; ++choice)
            Button(g, rect.SubRectHorizontal(2, choice).GetPadded(-1), info.choices[choice],
                   [callback, choice] { callback(choice); }, choice == selected);
        }
        else
          Button(g, rect, info.choices.at(selected), [this, rect, options = info.choices, callback] {
            Choose(options, rect, callback);
          });
      }
    }
  }

  void Tone(IGraphics& g, const Effect& effect, IRECT row, float y)
  {
    if (effect.id == mainNam)
    {
      ChoiceParam(g, {262, row.T, 378, row.B}, kEQActive, "Tone stack");
      ChoiceParam(g, {386, row.T, 509, row.B}, kEQPostNAM, "Post NAM");
      Navigate(g, {516, row.T, 650, row.B}, "Circuit...", View::Circuit);
      for (int index = 0; index < 3; ++index)
        KnobParam(g, Cell(index, 3, y), kToneBass + index);
      return;
    }
    Button(g, {261, row.T, 357, row.B}, "Tone stack", [this, effect] {
      mPlugin.SetExtraEffectParam(effect.id, kEQActive, !effect.tone.active);
    }, effect.tone.active);
    const IRECT type {362, row.T, 529, row.B};
    Button(g, type, dsp::tone_stack::GetToneStackTypeName(dsp::tone_stack::ToneStackTypeFromInt(effect.tone.type)),
      [this, type, id = effect.id] {
        std::vector<std::string> names;
        for (int i = 0; i < dsp::tone_stack::kNumToneStackTypes; ++i)
          names.emplace_back(dsp::tone_stack::GetToneStackTypeName(dsp::tone_stack::ToneStackTypeFromInt(i)));
        Choose(names, type, [this, id](int choice) { mPlugin.SetExtraEffectParam(id, kToneStackType, choice); });
      });
    Button(g, {534, row.T, 651, row.B}, effect.tone.post ? "Post NAM" : "Pre NAM", [this, effect] {
      mPlugin.SetExtraEffectParam(effect.id, kEQPostNAM, !effect.tone.post);
    });
    constexpr const char* names[] {"Bass", "Middle", "Treble"};
    for (int index = 0; index < 3; ++index)
      Rotary(g, Cell(index, 3, y), names[index], Value(effect.tone.values[index]), effect.tone.values[index] / 10, .5,
        [this, id = effect.id, index](double v) { mPlugin.SetExtraEffectParam(id, kToneBass + index, v * 10); });
  }

  bool Split(const Effect& effect) const
  {
    return mPlugin.GetParam(kChannelMode)->Int() &&
      ((effect.id == mainNam && !mPlugin.GetParam(kNAMLink)->Bool()) ||
       (effect.id == mainCab && !mPlugin.GetParam(kIRLink)->Bool()));
  }

  int Channel(const Effect& effect) const { return Split(effect) ? mChannel : 0; }

  void Files(IGraphics& g, const Effect& effect, int channel)
  {
    using namespace PlayerPalette;
    const bool nam = effect.type == Type::Nam;
    const auto path = mPlugin.GetEffectFile(effect.id, channel);
    const auto split = path.find_last_of("/\\");
    const auto name = path.substr(split != std::string::npos ? split + 1 : 0);
    Footer(g);
    Button(g, {20, 523, 144, 552}, nam ? "Select model" : "Select IR", [this, effect, channel] {
      WDL_String file, directory;
      GetUI()->PromptForFile(file, directory, EFileAction::Open, effect.type == Type::Nam ? "nam pnam" : "wav",
        [this, id = effect.id, channel](const WDL_String& selected, const WDL_String&) {
          if (selected.GetLength())
            Load(id, selected.Get(), channel);
        });
    });
    g.FillCircle(path.empty() ? track : Accent(), 161, 537, 4);
    Text(g, path.empty() ? (nam ? "No model loaded" : "No IR loaded") : name,
      {174, 519, 482, 554}, 15, path.empty() ? muted : text, EAlign::Near);
    if (!path.empty())
    {
      Button(g, {484, 523, 512, 552}, "<", [this, effect, channel, path] { StepFile(effect, channel, path, -1); });
      Button(g, {516, 523, 544, 552}, ">", [this, effect, channel, path] { StepFile(effect, channel, path, 1); });
      Button(g, {548, 523, 576, 552}, "×", [this, id = effect.id, channel] { mPlugin.ClearEffectFile(id, channel); });
    }
    if (Split(effect))
      Button(g, {583, 523, 657, 552}, channel ? "Right" : "Left", [this] { mChannel = 1 - mChannel; mControlsPage = 0; });
    if (effect.id == mainNam && mPlugin.GetParam(kChannelMode)->Int())
      ChoiceParam(g, {663, 523, 758, 552}, kNAMLink, "Link");
    if (effect.id == mainNam && mPlugin.PrimaryHasModelSize())
      Navigate(g, {765, 523, 840, 552}, "Size", View::Quality);
    else if (effect.hasModelSize)
      Button(g, {765, 523, 840, 552}, effect.modelSize < .5 ? "Lite" : "Full", [this, effect] {
        mPlugin.SetExtraEffectParam(effect.id, kSlim, effect.modelSize < .5 ? 1.0 : 0.0);
      });
  }

  void Load(int id, const std::string& path, int channel)
  {
    mStatus = mPlugin.LoadEffectFile(id, path, channel);
    mControlsPage = 0;
  }

  void StepFile(const Effect& effect, int channel, const std::string& path, int direction)
  {
    const auto current = std::filesystem::u8path(path);
    std::vector<std::filesystem::path> entries;
    std::error_code error;
    for (std::filesystem::directory_iterator it(current.parent_path(), error), end; !error && it != end; it.increment(error))
    {
      const auto extension = Extension(it->path());
      if ((effect.type == Type::Nam ? extension == ".nam" || extension == ".pnam" : extension == ".wav") &&
          it->is_regular_file(error))
        entries.push_back(it->path());
    }
    std::sort(entries.begin(), entries.end());
    const auto entry = std::find(entries.begin(), entries.end(), current);
    if (entry != entries.end() && entries.size() > 1)
    {
      const auto count = static_cast<std::ptrdiff_t>(entries.size());
      const auto next = entries[(entry - entries.begin() + count + direction) % count].u8string();
      Load(effect.id, std::string(next.begin(), next.end()), channel);
    }
  }

  void Footer(IGraphics& g)
  {
    g.FillRect(PlayerPalette::bar, {0, 509, 860, 565});
    g.DrawLine(PlayerPalette::border, 0, 509, 860, 509);
  }

  void Utility(IGraphics& g)
  {
    using namespace PlayerPalette;
    Footer(g);
    Navigate(g, {20, 523, 126, 552}, "Back to chain", View::Chain);
    switch (mView)
    {
      case View::Mixer:
      case View::Filters: MixAndFilters(g); break;
      case View::Tuner: Tuning(g); break;
      case View::Quality: Quality(g); break;
      case View::Circuit: Circuit(g); break;
      case View::Settings:
      case View::Midi:
      case View::About: SettingsPage(g); break;
      case View::Chain: break;
    }
  }

  void MixAndFilters(IGraphics& g)
  {
    Navigate(g, {318, 181, 423, 211}, "Stereo mixer", View::Mixer);
    Navigate(g, {431, 181, 542, 211}, "Cut filters", View::Filters);
    if (mView == View::Filters)
    {
      KnobParam(g, Cell(0, 2, 246), kLowCutFrequency, "Low cut");
      KnobParam(g, Cell(1, 2, 246), kHighCutFrequency, "High cut");
      ChoiceParam(g, {60, 367, 225, 399}, kLowCutSlope, "Low slope");
      ChoiceParam(g, {231, 367, 422, 399}, kLowCutPostNAM, "Low cut post NAM");
      ChoiceParam(g, {440, 367, 630, 399}, kHighCutPostNAM, "High cut post NAM");
      ChoiceParam(g, {636, 367, 800, 399}, kHighCutSlope, "High slope");
      ChoiceParam(g, {327, 415, 533, 449}, kDCBlockerActive, "DC filter");
      return;
    }
    constexpr int ids[] {kLevelL, kPanL, kTimeAlign, kPanR, kLevelR};
    constexpr const char* names[] {"Level L", "Pan L", "Time alignment", "Pan R", "Level R"};
    for (int index = 0; index < 5; ++index)
      KnobParam(g, Cell(index, 5, 244), ids[index], names[index]);
    ChoiceParam(g, {40, 367, 177, 399}, kPhaseInvertL, "Invert L");
    ChoiceParam(g, {183, 367, 315, 399}, kPanLink, "Link pan");
    const bool idle = mPlugin.GetAutoAlignState() == NeuralAmpModeler::EAutoAlignState::Idle;
    Button(g, {324, 367, 536, 399}, idle ? "Auto align" : "Cancel alignment", [this] { mPlugin.ToggleAutoAlignment(); }, !idle);
    ChoiceParam(g, {544, 367, 675, 399}, kLevelLink, "Link levels");
    ChoiceParam(g, {682, 367, 820, 399}, kPhaseInvertR, "Invert R");
  }

  void Quality(IGraphics& g)
  {
    Text(g, "Real-time", {68, 195, 400, 236}, 20, PlayerPalette::text);
    Text(g, "Offline render", {460, 195, 792, 236}, 20, PlayerPalette::text);
    ChoiceParam(g, {68, 252, 400, 284}, kOversamplingFactor, "Oversampling");
    ChoiceParam(g, {68, 298, 400, 330}, kAntiAliasFilterPhase, "Filter phase");
    ChoiceParam(g, {460, 252, 792, 284}, kOfflineOversamplingFactor, "Oversampling");
    ChoiceParam(g, {460, 298, 792, 330}, kOfflineAntiAliasFilterPhase, "Filter phase");
    ChoiceParam(g, {68, 349, 230, 382}, kPhaseMulticoreEnabled, "Multicore");
    ChoiceParam(g, {237, 349, 400, 382}, kPhaseMulticoreThreadCount, "Threads");
    if (mPlugin.PrimaryHasModelSize())
      KnobParam(g, {567, 345, 685, 445}, kSlim, "Model size");
  }

  void Tuning(IGraphics& g)
  {
    using namespace PlayerPalette;
    const auto reading = mPlugin.GetTunerResult();
    std::string note = "—";
    if (reading.valid && reading.frequency > 0)
    {
      const int midi = std::clamp(static_cast<int>(std::lround(69 + 12 * std::log2(reading.frequency / 440.0))), 0, 127);
      constexpr const char* notes[] {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
      note = std::string(notes[midi % 12]) + std::to_string(midi / 12 - 1);
    }
    Text(g, note, {328, 204, 532, 309}, 72, Accent());
    Text(g, reading.valid ? Value(reading.frequency, " Hz") : "Play a note", {255, 308, 605, 343}, 16, muted);
    g.DrawLine(track, 190, 375, 670, 375, nullptr, 6);
    g.DrawLine(muted, 430, 364, 430, 386);
    if (reading.valid)
      g.FillCircle(Accent(), 430 + 240 * std::clamp(reading.cents / 50.f, -1.f, 1.f), 375, 7);
    ChoiceParam(g, {339, 415, 521, 449}, kTunerMute, "Mute output");
  }

  void SettingsPage(IGraphics& g)
  {
    Navigate(g, {259, 181, 374, 211}, "Calibration", View::Settings);
    Navigate(g, {381, 181, 488, 211}, "MIDI", View::Midi);
    Navigate(g, {494, 181, 601, 211}, "About", View::About);
    if (mView == View::About)
    {
      Text(g, "NAM On Steroids", {40, 248, 820, 314}, 30, Accent());
      Text(g, "The Tone Scientist  /  Neural Amp Modeler by Steven Atkinson", {40, 325, 820, 356},
           15, PlayerPalette::muted);
      Button(g, {332, 387, 528, 420}, "Project and licenses", [this] {
        GetUI()->OpenURL("https://github.com/DLC86/NAM-Oversampler");
      });
      return;
    }
    if (mView == View::Midi)
    {
      ChoiceParam(g, {270, 241, 590, 275}, kMidiChannel, "MIDI channel");
      const IRECT bindings {220, 297, 640, 331};
      Button(g, bindings, "MIDI mappings...", [this, bindings] {
        std::vector<int> ids;
        std::vector<std::string> names;
        for (int index = 0; index < kNumParams; ++index)
        {
          if (!mPlugin.IsMidiAssignableParam(index))
            continue;
          const int cc = mPlugin.GetMidiCCForParam(index);
          ids.push_back(index);
          names.push_back(std::string(mPlugin.GetParam(index)->GetName()) + (cc < 0 ? "" : "  /  CC " + std::to_string(cc)));
        }
        Choose(names, bindings, [this, ids, bindings](int choice) { Midi(ids[choice], bindings); });
      });
      const IRECT prev {183, 357, 422, 391}, next {438, 357, 677, 391};
      Button(g, prev, "Previous preset CC", [this, prev] { Midi(NeuralAmpModeler::kMidiActionPreviousPreset, prev); });
      Button(g, next, "Next preset CC", [this, next] { Midi(NeuralAmpModeler::kMidiActionNextPreset, next); });
      return;
    }
    ChoiceParam(g, {66, 253, 317, 287}, kCalibrateInput, "Input calibration");
    KnobParam(g, {369, 225, 491, 325}, kInputCalibrationLevel, "Interface level");
    ChoiceParam(g, {543, 253, 794, 287}, kOutputMode, "Output mode");
    ChoiceParam(g, {162, 359, 419, 395}, kFollowTrackColor, "Follow track color");
    Button(g, {441, 359, 698, 395}, "Highlight color", [this] {
      auto current = Accent();
      GetUI()->PromptForColor(current, "Highlight color", [this](IColor color) {
        char value[10];
        std::snprintf(value, sizeof(value), "#%02X%02X%02X", color.R, color.G, color.B);
        mPlugin.OnMessage(kMsgTagHighlightColor, 0, static_cast<int>(std::strlen(value)), value);
      });
    });
  }

  void Midi(int index, IRECT bounds)
  {
    Choose({"Learn MIDI CC", "Enter CC number...", "Clear assignment"}, bounds, [this, index, bounds](int choice) {
      if (choice == 0)
      {
        mPlugin.StartMidiLearnForParam(index);
        mStatus = "Move a MIDI control to assign it";
      }
      else if (choice == 1)
        Entry(bounds, std::to_string(std::max(0, mPlugin.GetMidiCCForParam(index))), [this, index](const char* text) {
          char* end = nullptr;
          const long value = std::strtol(text, &end, 10);
          if (end != text && !*end && value >= 0 && value <= 127)
            mPlugin.AssignMidiCCToParam(index, static_cast<int>(value));
        });
      else
        mPlugin.ClearMidiCCForParam(index);
    });
  }

  void Circuit(IGraphics& g)
  {
    using namespace dsp::tone_stack;
    ChoiceParam(g, {206, 180, 654, 214}, kToneStackType, "Circuit");
    const int type = mPlugin.GetParam(kToneStackType)->Int();
    int slot = 0;
    for (int index = 0; index < kNumToneStackComponents; ++index)
    {
      const auto component = ToneStackComponentFromInt(index);
      if (!ToneStackTypeHasComponent(ToneStackTypeFromInt(type), component))
        continue;
      const float x = 50.f + (slot % 3) * 255.f, y = 237.f + (slot / 3) * 47.f;
      const IRECT bounds {x, y, x + 247, y + 32};
      char text[32];
      std::snprintf(text, sizeof(text), "%.6g", mPlugin.GetToneStackComponentValue(type, index));
      Button(g, bounds, std::string(GetToneStackComponentName(component)) + "  /  " + text + " " +
        GetToneStackComponentUnit(component), [this, type, index, bounds, initial = std::string(text)] {
          Entry(bounds, initial, [this, type, index](const char* value) {
            char* end = nullptr;
            const double number = std::strtod(value, &end);
            if (end != value && !*end && std::isfinite(number))
              mPlugin.SetToneStackComponentValue(type, index, number);
          });
      });
      ++slot;
    }
    Button(g, {676, 523, 840, 552}, "Reset components", [this, type] { mPlugin.ResetToneStackComponentValues(type); });
  }
};
