#pragma once

class NAMChainEditor final : public IControl
{
public:
  NAMChainEditor(const IRECT& bounds, NeuralAmpModeler& plugin)
  : IControl(bounds), mPlugin(plugin)
  {
    SetTooltip("Select a block to edit it. Drag to change its position.");
  }

  void OnGUIIdle() override
  {
    const auto effects = mPlugin.GetEffects();
    const auto* selected = FindSelected(effects);
    if (!selected && mPlugin.GetSelectedEffect() != (effects.empty() ? 0 : effects.front().id))
      mPlugin.SelectEffect(effects.empty() ? 0 : effects.front().id);

    std::string signature = std::to_string(mPlugin.GetSelectedEffect());
    for (const auto& effect : effects)
      signature += "|" + std::to_string(effect.id) + ":" + effect.path + ":" + std::to_string(effect.enabled)
                   + ":" + std::to_string(effect.sidechain) + ":" + std::to_string(effect.threshold);
    const auto controls = SelectedControls();
    const auto focus = std::make_tuple(mPlugin.GetSelectedEffect(), controls.get(), mPlugin.PrimaryHasModelSize());
    if (focus != mFocus)
    {
      mFocus = focus;
      mPage = 0;
      mPlugin.SelectEffect(mPlugin.GetSelectedEffect());
    }
    if (controls)
    {
      signature += controls->path;
      for (size_t index = 0; index < controls->specs.size(); ++index)
        signature += ":" + std::to_string(controls->Get(index));
    }
    if (signature != mSignature)
    {
      mSignature = std::move(signature);
      SetDirty(false);
    }
  }

  void Draw(IGraphics& g) override
  {
    const auto accent = mPlugin.GetThemeColor();
    g.FillRect(IColor(255, 15, 18, 24), mRECT);
    g.FillRect(IColor(255, 22, 26, 35), mRECT.GetFromTop(64));
    Text(g, "NAM ON STEROIDS", {24, 11, 290, 53}, 23, kText, EAlign::Near);
    Text(g, "EFFECT CHAIN", {24, 70, 180, 97}, 12, kMuted, EAlign::Near);
    Text(g, "IN", {17, 107, 45, 149}, 11, kMuted);
    Text(g, "OUT", {810, 107, 846, 149}, 11, kMuted);
    g.DrawLine(kEdge, 45, 127, 809, 127);
    const auto effects = mPlugin.GetEffects();
    if (effects.size() > 6)
      g.DrawLine(kEdge, 45, 172, 809, 172);

    Button(g, AddRect(), "+ Add block", accent);
    for (size_t index = 0; index < effects.size(); ++index)
    {
      const auto& effect = effects[index];
      const auto rect = CardRect(static_cast<int>(index));
      const bool selected = effect.id == mPlugin.GetSelectedEffect();
      const IColor color = effect.type == Type::Gate ? IColor(255, 101, 208, 181)
                         : effect.type == Type::Nam ? accent : IColor(255, 170, 157, 241);
      g.FillRoundRect(selected ? IColor(255, 47, 51, 64) : kSurface, rect, 7);
      g.DrawRoundRect(selected ? color : kEdge, rect, 7, nullptr, selected ? 1.6f : 1);
      g.FillCircle(effect.enabled ? color : kMuted, rect.L + 13, rect.MH(), 3);
      Text(g, NeuralAmpModeler::EffectName(effect.type), rect.GetReducedFromLeft(25), 15,
           effect.enabled ? kText : kMuted, EAlign::Near);
      Text(g, "::", rect.GetFromRight(22), 15, kMuted);
    }
    if (mDragId > 0 && mDropIndex >= 0)
      g.DrawRoundRect(accent, CardRect(mDropIndex).GetPadded(2), 8, nullptr, 2.0f);

    g.FillRoundRect(kSurface, {116, 211, 744, 454}, 12);
    g.FillRoundRect(kSurface, {15, 211, 109, 454}, 12);
    g.FillRoundRect(kSurface, {751, 211, 845, 454}, 12);
    Text(g, "INPUT", {15, 223, 109, 244}, 11, kMuted);
    Text(g, "OUTPUT", {751, 223, 845, 244}, 11, kMuted);

    const auto* effect = FindSelected(effects);
    if (!effect)
    {
      Text(g, "Add a block to build your signal chain", {143, 254, 717, 397}, 15, kMuted);
      return;
    }

    Text(g, NeuralAmpModeler::EffectName(effect->type), {137, 222, 405, 257}, 22, kText, EAlign::Near);
    Button(g, EnabledRect(), effect->enabled ? "Active" : "Bypassed", effect->enabled ? accent : kMuted);
    Button(g, RemoveRect(), "Remove", kMuted);
    if (effect->type == Type::Gate)
    {
      if (effect->id != 2)
        Slider(g, ExtraThresholdRect(), "Threshold", (std::to_string(static_cast<int>(effect->threshold)) + " dB").c_str(),
               (effect->threshold + 100.0) / 100.0, accent);
      Text(g, "SIDECHAIN", SidechainRect().GetVShifted(-30), 11, kMuted, EAlign::Near);
      Button(g, SidechainRect(), SidechainLabel(effects, effect->sidechain).c_str(), kText);
    }
    if (effect->id > 3 && effect->type != Type::Gate)
    {
      const std::string fileName = effect->path.empty() ? "Load file..." : WDL_get_filepart(effect->path.c_str());
      Button(g, FileRect(), fileName.c_str(), kText);
    }
    if (const auto controls = SelectedControls())
    {
      if (effect->id == 1 && CanChooseChannel())
        Button(g, ChannelRect(), mChannel == 0 ? "Left controls" : "Right controls", accent);
      if (controls->specs.size() > kControlsPerPage)
      {
        Button(g, PreviousPage(), "<", mPage > 0 ? accent : kMuted);
        Button(g, NextPage(), ">", (mPage + 1) * kControlsPerPage < controls->specs.size() ? accent : kMuted);
      }
      for (size_t local = 0; local < kControlsPerPage; ++local)
      {
        const size_t index = mPage * kControlsPerPage + local;
        if (index >= controls->specs.size())
          break;
        const auto& spec = controls->specs[index];
        const auto value = controls->Get(index);
        char number[24];
        std::snprintf(number, sizeof(number), "%.3g", value);
        const auto label = spec.choices.empty() ? number : spec.choices.at(static_cast<size_t>(value)).c_str();
        const double range = spec.maximum - spec.minimum;
        Slider(g, ParameterRect(local), spec.name.c_str(), label,
               range > 0.0 ? (value - spec.minimum) / range : 0.0, accent);
      }
    }
    else if (effect->id == 1 && CanChooseChannel())
      Button(g, ChannelRect(), mChannel == 0 ? "Left controls" : "Right controls", accent);
  }

  void OnMouseDown(float x, float y, const IMouseMod& mod) override
  {
    const auto effects = mPlugin.GetEffects();
    for (int index = 0; index < static_cast<int>(effects.size()); ++index)
    {
      if (!CardRect(index).Contains(x, y))
        continue;
      mPlugin.SelectEffect(effects[index].id);
      mDragId = effects[index].id;
      mDropIndex = -1;
      mDragX = x;
      mDragY = y;
      mChannel = 0;
      mPage = 0;
      if (mod.R)
      {
        mDragId = 0;
        mMenu.Clear();
        mMenu.AddItem("Move left");
        mMenu.AddItem("Move right");
        mMenu.AddItem("Remove block");
        mMenuAction = MenuAction::Block;
        mMenuIndex = index;
        mMenuEffect = effects[index].id;
        GetUI()->CreatePopupMenu(*this, mMenu, CardRect(index));
      }
      SetDirty(false);
      return;
    }

    if (AddRect().Contains(x, y) && effects.size() < NeuralAmpModeler::kMaxEffectBlocks)
    {
      mMenu.Clear();
      mMenu.AddItem("Gate");
      mMenu.AddItem("NAM");
      mMenu.AddItem("Cab");
      mMenuAction = MenuAction::Add;
      GetUI()->CreatePopupMenu(*this, mMenu, AddRect());
      return;
    }
    const auto* effect = FindSelected(effects);
    if (!effect)
      return;
    if (EnabledRect().Contains(x, y))
      mPlugin.SetEffectEnabled(effect->id, !effect->enabled);
    else if (RemoveRect().Contains(x, y))
      mPlugin.RemoveEffect(effect->id);
    else if (effect->type == Type::Gate && SidechainRect().Contains(x, y))
      OpenSidechainMenu(effects, effect->id);
    else if (effect->type == Type::Gate && effect->id != 2 && ExtraThresholdRect().Contains(x, y))
    {
      mDragThreshold = true;
      mPlugin.SetEffectThreshold(effect->id, -100.0 + 100.0 * SliderPosition(ExtraThresholdRect(), x));
    }
    else if (effect->id > 3 && effect->type != Type::Gate && FileRect().Contains(x, y))
      OpenFile(*effect);
    else if (effect->id == 1 && CanChooseChannel() && ChannelRect().Contains(x, y))
    {
      mChannel = 1 - mChannel;
      mPage = 0;
    }
    else if (const auto controls = SelectedControls())
    {
      if (PreviousPage().Contains(x, y) && mPage > 0)
        --mPage;
      else if (NextPage().Contains(x, y) && (mPage + 1) * kControlsPerPage < controls->specs.size())
        ++mPage;
      else
      {
        for (size_t local = 0; local < kControlsPerPage; ++local)
        {
          const size_t index = mPage * kControlsPerPage + local;
          const auto rect = ParameterRect(local);
          if (index >= controls->specs.size() || !rect.Contains(x, y))
            continue;
          const auto& spec = controls->specs[index];
          if (spec.choices.empty())
          {
            mDragParameter = static_cast<int>(local);
            SetParameter(*controls, local, x);
          }
          else
          {
            mMenu.Clear();
            for (const auto& choice : spec.choices)
              mMenu.AddItem(choice.c_str());
            mMenuAction = MenuAction::Choice;
            mMenuIndex = static_cast<int>(index);
            mMenuEffect = effect->id;
            GetUI()->CreatePopupMenu(*this, mMenu, rect);
          }
          break;
        }
      }
    }
    SetDirty(false);
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const IMouseMod& mod) override
  {
    if (mDragId > 0 && (std::abs(x - mDragX) > 3.f || std::abs(y - mDragY) > 3.f))
    {
      const auto effects = mPlugin.GetEffects();
      const int row = y > 151.f ? 1 : 0;
      const int column = std::clamp(static_cast<int>((x - 52.f) / 125.f), 0, 5);
      mDropIndex = std::clamp(row * 6 + column, 0, static_cast<int>(effects.size()) - 1);
    }
    if (mDragThreshold)
      mPlugin.SetEffectThreshold(mPlugin.GetSelectedEffect(), -100.0 + 100.0 * SliderPosition(ExtraThresholdRect(), x));
    if (mDragParameter >= 0)
    {
      if (const auto controls = SelectedControls())
        SetParameter(*controls, static_cast<size_t>(mDragParameter), x);
    }
    SetDirty(false);
  }

  void OnMouseUp(float x, float y, const IMouseMod& mod) override
  {
    if (mDragId > 0 && mDropIndex >= 0)
      mPlugin.MoveEffect(mDragId, mDropIndex);
    mDragId = 0;
    mDropIndex = -1;
    mDragThreshold = false;
    mDragParameter = -1;
    SetDirty(false);
  }

  void OnPopupMenuSelection(IPopupMenu* menu, int valueIndex) override
  {
    if (!menu || menu->GetChosenItemIdx() < 0)
      return;
    const int chosen = menu->GetChosenItemIdx();
    switch (mMenuAction)
    {
      case MenuAction::Add:
        if (const int id = mPlugin.AddEffect(static_cast<Type>(chosen)))
          mPlugin.SelectEffect(id);
        break;
      case MenuAction::Sidechain:
        if (static_cast<size_t>(chosen) < mSources.size())
          mPlugin.SetEffectSidechain(mMenuEffect, mSources[chosen]);
        break;
      case MenuAction::Choice:
        mPlugin.SetEffectModelControl(mMenuEffect, mChannel, mMenuIndex, static_cast<float>(chosen));
        break;
      case MenuAction::Block:
        if (chosen == 2)
          mPlugin.RemoveEffect(mMenuEffect);
        else
          mPlugin.MoveEffect(mMenuEffect, mMenuIndex + (chosen == 0 ? -1 : 1));
        break;
    }
    SetDirty(false);
  }

private:
  using Type = NeuralAmpModeler::EffectType;
  using Settings = NeuralAmpModeler::EffectSettings;
  enum class MenuAction { Add, Sidechain, Choice, Block };
  static constexpr size_t kControlsPerPage = 6;
  const IColor kSurface {255, 25, 29, 39};
  const IColor kEdge {255, 63, 70, 85};
  const IColor kText {255, 236, 240, 247};
  const IColor kMuted {255, 160, 170, 187};
  NeuralAmpModeler& mPlugin;
  IPopupMenu mMenu;
  MenuAction mMenuAction = MenuAction::Add;
  std::vector<int> mSources;
  int mMenuEffect = 0;
  int mMenuIndex = 0;
  int mDragId = 0;
  int mDropIndex = -1;
  float mDragX = 0;
  float mDragY = 0;
  int mDragParameter = -1;
  bool mDragThreshold = false;
  int mChannel = 0;
  size_t mPage = 0;
  std::string mSignature;
  std::tuple<int, const ModelControls*, bool> mFocus {};

  const Settings* FindSelected(const std::vector<Settings>& effects) const
  {
    const auto it = std::find_if(effects.begin(), effects.end(),
      [&](const Settings& effect) { return effect.id == mPlugin.GetSelectedEffect(); });
    return it == effects.end() ? nullptr : &*it;
  }

  bool CanChooseChannel() const
  {
    return mPlugin.GetParam(kChannelMode)->Int() == 1 && !mPlugin.GetParam(kNAMLink)->Bool() &&
           (mPlugin.GetEffectModelControls(1, 0) || mPlugin.GetEffectModelControls(1, 1));
  }

  std::shared_ptr<const ModelControls> SelectedControls() const
  {
    return mPlugin.GetEffectModelControls(mPlugin.GetSelectedEffect(), CanChooseChannel() ? mChannel : 0);
  }

  static IRECT CardRect(int index)
  {
    const float x = 54.f + static_cast<float>(index % 6) * 125.f;
    const float y = 107.f + static_cast<float>(index / 6) * 45.f;
    return {x, y, x + 116.f, y + 39.f};
  }
  static IRECT AddRect() { return {697, 70, 831, 98}; }
  static IRECT EnabledRect() { return {526, 225, 627, 253}; }
  static IRECT RemoveRect() { return {633, 225, 723, 253}; }
  static IRECT ChannelRect() { return {139, 304, 264, 332}; }
  static IRECT SidechainRect() { return {305, 339, 700, 375}; }
  static IRECT ExtraThresholdRect() { return {139, 335, 286, 391}; }
  static IRECT FileRect() { return {139, 265, 721, 301}; }
  static IRECT PreviousPage() { return {652, 303, 684, 330}; }
  static IRECT NextPage() { return {690, 303, 722, 330}; }

  static IRECT ParameterRect(size_t local)
  {
    const float x = 139.f + static_cast<float>(local % 3) * 194.f;
    const float y = 338.f + static_cast<float>(local / 3) * 53.f;
    return {x, y, x + 190.f, y + 49.f};
  }

  static float SliderPosition(const IRECT& rect, float x)
  {
    return std::clamp((x - rect.L - 9.f) / (rect.W() - 18.f), 0.f, 1.f);
  }

  void SetParameter(const ModelControls& controls, size_t local, float x)
  {
    const auto index = mPage * kControlsPerPage + local;
    if (index >= controls.specs.size())
      return;
    const auto& spec = controls.specs[index];
    mPlugin.SetEffectModelControl(mPlugin.GetSelectedEffect(), mChannel, index,
      spec.minimum + SliderPosition(ParameterRect(local), x) * (spec.maximum - spec.minimum));
  }

  static void Text(IGraphics& g, const char* label, const IRECT& rect, float size, IColor color,
                   EAlign align = EAlign::Center)
  {
    g.DrawText(IText(size, color, "Roboto-Regular").WithAlign(align), label, rect);
  }

  void Button(IGraphics& g, const IRECT& rect, const char* text, IColor value) const
  {
    g.FillRoundRect(IColor(255, 37, 43, 55), rect, 6);
    g.DrawRoundRect(kEdge, rect, 6);
    Text(g, text, rect.GetPadded(-4), 13, value);
  }

  void Slider(IGraphics& g, const IRECT& rect, const char* name, const char* value,
              double normalized, IColor accent) const
  {
    g.FillRoundRect(IColor(255, 33, 39, 51), rect.GetPadded(-2), 6);
    Text(g, name, rect.GetReducedFromLeft(10).GetReducedFromRight(60).GetFromTop(29), 12, kText, EAlign::Near);
    Text(g, value, rect.GetReducedFromRight(8).GetFromRight(62).GetFromTop(29), 12, accent, EAlign::Far);
    const auto track = rect.GetPadded(-9).GetFromBottom(4);
    g.FillRoundRect(kEdge, track, 2);
    g.FillRoundRect(accent, track.GetFromLeft(track.W() * static_cast<float>(normalized)), 2);
  }

  std::string SidechainLabel(const std::vector<Settings>& effects, int source) const
  {
    if (source == NeuralAmpModeler::kChainInput)
      return "Chain input";
    for (size_t index = 0; index < effects.size(); ++index)
      if (effects[index].id == source)
        return std::to_string(index + 1) + "  " + NeuralAmpModeler::EffectName(effects[index].type) + " output";
    return "Block input";
  }

  void OpenSidechainMenu(const std::vector<Settings>& effects, int id)
  {
    mMenu.Clear();
    mSources = {NeuralAmpModeler::kEffectInput, NeuralAmpModeler::kChainInput};
    for (const auto& source : effects)
    {
      if (source.id == id)
        break;
      mSources.push_back(source.id);
    }
    for (const int source : mSources)
      mMenu.AddItem(SidechainLabel(effects, source).c_str());
    mMenuAction = MenuAction::Sidechain;
    mMenuEffect = id;
    GetUI()->CreatePopupMenu(*this, mMenu, SidechainRect());
  }

  void OpenFile(const Settings& effect)
  {
    WDL_String file;
    WDL_String directory;
    GetUI()->PromptForFile(file, directory, EFileAction::Open, effect.type == Type::Nam ? "nam" : "wav",
      [this, id = effect.id](const WDL_String& path, const WDL_String&) {
        if (!path.GetLength())
          return;
        const auto error = mPlugin.LoadEffectFile(id, path.Get());
        if (!error.empty())
          _ShowMessageBox(GetUI(), error.c_str(), "Could not load file", kMB_OK);
      });
  }
};
