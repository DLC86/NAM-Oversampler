#include "NeuralAmpModeler.h"
#include "LinuxEditor.h"

#include <gtk/gtkx.h>
#undef Bool
#undef Status
#include <fstream>
#include <iterator>

namespace
{
using Type = NeuralAmpModeler::EffectType;
using Settings = NeuralAmpModeler::EffectSettings;
constexpr const char* kBlockDragType = "application/x-nam-on-steroids-effect";

void Pack(GtkWidget* box, GtkWidget* child, bool expand = false)
{
  gtk_box_pack_start(GTK_BOX(box), child, expand, expand, 0);
}

template<typename Callback>
void OnClick(GtkWidget* button, Callback callback)
{
  g_signal_connect_data(button, "clicked",
    G_CALLBACK(+[](GtkButton*, gpointer data) {
      auto action = *static_cast<Callback*>(data);
      action();
    }),
    new Callback(std::move(callback)),
    +[](gpointer data, GClosure*) { delete static_cast<Callback*>(data); }, GConnectFlags(0));
}

GtkWidget* Button(GtkWidget* box, const char* title, std::function<void()> callback)
{
  auto* button = gtk_button_new_with_label(title);
  Pack(box, button);
  OnClick(button, std::move(callback));
  return button;
}

GtkWidget* Label(GtkWidget* box, const char* text)
{
  auto* label = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  Pack(box, label);
  return label;
}

void Clear(GtkWidget* container)
{
  GList* children = gtk_container_get_children(GTK_CONTAINER(container));
  for (GList* child = children; child; child = child->next)
    gtk_widget_destroy(GTK_WIDGET(child->data));
  g_list_free(children);
}

template<typename Callback>
void Choice(GtkWidget* box, const std::vector<std::string>& names, int selected, Callback callback)
{
  auto* combo = gtk_combo_box_text_new();
  for (const auto& name : names)
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), name.c_str());
  gtk_combo_box_set_active(GTK_COMBO_BOX(combo), selected);
  Pack(box, combo);
  g_signal_connect_data(combo, "changed",
    G_CALLBACK(+[](GtkComboBox* widget, gpointer data) {
      const int index = gtk_combo_box_get_active(widget);
      if (index >= 0)
        (*static_cast<Callback*>(data))(index);
    }), new Callback(std::move(callback)),
    +[](gpointer data, GClosure*) { delete static_cast<Callback*>(data); }, GConnectFlags(0));
}

template<typename Callback>
void Range(GtkWidget* box, const char* name, double lo, double hi, double step, double value, Callback callback)
{
  Label(box, name);
  auto* slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, lo, std::max(hi, lo + step), step);
  gtk_range_set_value(GTK_RANGE(slider), value);
  gtk_scale_set_value_pos(GTK_SCALE(slider), GTK_POS_RIGHT);
  Pack(box, slider);
  g_signal_connect_data(slider, "value-changed",
    G_CALLBACK(+[](GtkRange* widget, gpointer data) {
      (*static_cast<Callback*>(data))(gtk_range_get_value(widget));
    }), new Callback(std::move(callback)),
    +[](gpointer data, GClosure*) { delete static_cast<Callback*>(data); }, GConnectFlags(0));
}

class LinuxEditor
{
public:
  explicit LinuxEditor(NeuralAmpModeler& plugin) : mPlugin(plugin)
  {
    mRoot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_size_request(mRoot, 760, 480);
    gtk_widget_set_name(mRoot, "nam-steroids");
    gtk_container_set_border_width(GTK_CONTAINER(mRoot), 16);
    auto* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    Pack(mRoot, header);
    auto* title = gtk_label_new("NAM ON STEROIDS");
    gtk_widget_set_name(title, "nam-title");
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    Pack(header, title, true);
    Button(header, "Open session", [this] { Session(false); });
    Button(header, "Save session", [this] { Session(true); });
    Button(header, "Settings", [this] { SettingsWindow(); });

    auto* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    Pack(mRoot, bar);
    Label(bar, "EFFECT CHAIN");
    for (const auto type : {Type::Gate, Type::Nam, Type::Cab})
    {
      const std::string name = std::string("+ ") + mPlugin.EffectName(type);
      Button(bar, name.c_str(), [this, type] {
        const int block = mPlugin.AddEffect(type);
        if (block)
          mPlugin.SelectEffect(block);
        Refresh();
      });
    }
    mChain = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    Pack(mRoot, mChain);

    auto* scroller = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    Pack(mRoot, scroller, true);
    mInspector = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
    gtk_container_set_border_width(GTK_CONTAINER(mInspector), 10);
    gtk_container_add(GTK_CONTAINER(scroller), mInspector);

    auto* footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    Pack(mRoot, footer);
    for (const auto param : {kInputLevel, kOutputLevel, kOversamplingFactor})
    {
      auto* column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
      Pack(footer, column, true);
      Param(column, param);
    }
    mStatus = Label(mRoot, "");
    gtk_label_set_line_wrap(GTK_LABEL(mStatus), true);
    InstallStyle();
    Refresh();

    gtk_widget_show_all(mRoot);
    mTimer = g_timeout_add(120, +[](gpointer editor) -> gboolean {
      static_cast<LinuxEditor*>(editor)->RefreshIfChanged();
      return G_SOURCE_CONTINUE;
    }, this);
  }

  ~LinuxEditor() { g_source_remove(mTimer); }
  GtkWidget* Root() const { return mRoot; }

private:
  void InstallStyle()
  {
    auto* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider,
      "#nam-steroids { background-color: #11151f; color: #ecf0f7; font-family: Roboto, sans-serif; }"
      "#nam-title { color: #ecf0f7; font-size: 21px; font-weight: bold; }"
      "#nam-steroids button { border-radius: 6px; }"
      "#nam-steroids button.suggested-action { background: #295c68; color: white; }"
      "#nam-steroids button.nam-bypassed { opacity: 0.58; }", -1, nullptr);
    gtk_style_context_add_provider_for_screen(gtk_widget_get_screen(mRoot), GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
  }

  GtkWindow* Window() const { return GTK_WINDOW(gtk_widget_get_toplevel(mRoot)); }

  void SetStatus(const std::string& text)
  {
    gtk_label_set_text(GTK_LABEL(mStatus), text.c_str());
  }

  std::string ChainSignature() const
  {
    std::string signature = std::to_string(mPlugin.GetSelectedEffect());
    for (const auto& block : mPlugin.GetEffects())
      signature += "|" + std::to_string(block.id) + ":" + std::to_string(block.enabled) + ":" + block.path;
    signature += "|" + std::to_string(mPlugin.GetParam(kChannelMode)->Int());
    signature += "|" + std::to_string(mPlugin.GetParam(kNAMLink)->Bool());
    signature += "|" + std::to_string(mPlugin.GetParam(kIRLink)->Bool());
    return signature;
  }

  void RefreshIfChanged()
  {
    mPlugin.OnIdle();
    if (mSignature != ChainSignature())
      Refresh();
  }

  void SendParam(int index, double value)
  {
    if (mPlugin.GetComponentHandler())
    {
      mPlugin.BeginInformHostOfParamChangeFromUI(index);
      mPlugin.SendParameterValueFromUI(index, mPlugin.GetParam(index)->ToNormalized(value));
      mPlugin.EndInformHostOfParamChangeFromUI(index);
    }
    else
    {
      mPlugin.GetParam(index)->Set(value);
      mPlugin.OnParamChange(index);
    }
  }

  void Param(GtkWidget* box, int index)
  {
    const auto* param = mPlugin.GetParam(index);
    if (param->Type() == iplug::IParam::kTypeBool || param->Type() == iplug::IParam::kTypeEnum)
    {
      Label(box, param->GetName());
      std::vector<std::string> names;
      const int first = static_cast<int>(param->GetMin());
      const int last = static_cast<int>(param->GetMax());
      for (int value = first; value <= last; ++value)
      {
        WDL_String shown;
        param->GetDisplay(value, false, shown);
        names.emplace_back(shown.Get());
      }
      Choice(box, names, param->Int() - first, [this, index, first](int selected) {
        SendParam(index, selected + first);
      });
      return;
    }
    const double lo = param->GetMin();
    const double hi = param->GetMax();
    const double step = param->GetStep() > 0.0 ? param->GetStep() : (hi - lo) / 200.0;
    if (hi > lo)
      Range(box, param->GetName(), lo, hi, step, param->Value(), [this, index](double v) { SendParam(index, v); });
  }

  void SettingsWindow()
  {
    auto* dialog = gtk_dialog_new_with_buttons("NAM On Steroids settings", Window(),
      GTK_DIALOG_DESTROY_WITH_PARENT, "_Close", GTK_RESPONSE_CLOSE, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 520, 560);
    auto* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_container_add(GTK_CONTAINER(scroll), box);
    Pack(gtk_dialog_get_content_area(GTK_DIALOG(dialog)), scroll, true);
    for (int index = 0; index < kNumParams; ++index)
      Param(box, index);
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    Refresh();
  }

  void Session(bool save)
  {
    auto* dialog = gtk_file_chooser_dialog_new(save ? "Save session" : "Open session", Window(),
      save ? GTK_FILE_CHOOSER_ACTION_SAVE : GTK_FILE_CHOOSER_ACTION_OPEN,
      "_Cancel", GTK_RESPONSE_CANCEL, save ? "_Save" : "_Open", GTK_RESPONSE_ACCEPT, nullptr);
    auto* chooser = GTK_FILE_CHOOSER(dialog);
    if (save)
    {
      gtk_file_chooser_set_do_overwrite_confirmation(chooser, true);
      gtk_file_chooser_set_current_name(chooser, "Signal Chain.nos");
    }
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
    {
      char* path = gtk_file_chooser_get_filename(chooser);
      iplug::IByteChunk state;
      if (save)
      {
        std::ofstream output(path, std::ios::binary);
        if (mPlugin.SerializeState(state))
          output.write(reinterpret_cast<const char*>(state.GetData()), state.Size());
        SetStatus(output ? "Saved session" : "Unable to save session");
      }
      else
      {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        const auto bytes = input ? input.tellg() : std::streampos(-1);
        if (bytes > 0 && bytes < 32 * 1024 * 1024)
        {
          std::vector<char> data(static_cast<size_t>(bytes));
          input.seekg(0);
          if (input.read(data.data(), static_cast<std::streamsize>(data.size())))
          {
            state.PutBytes(data.data(), static_cast<int>(data.size()));
            mPlugin.UnserializeState(state, 0);
            SetStatus("Loaded session");
          }
        }
        else
          SetStatus("Unable to open session");
      }
      g_free(path);
    }
    gtk_widget_destroy(dialog);
    Refresh();
  }

  void Load(const Settings& block, int channel)
  {
    const bool nam = block.type == Type::Nam;
    auto* dialog = gtk_file_chooser_dialog_new(nam ? "Load NAM model" : "Load cabinet IR", Window(),
      GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, nullptr);
    auto* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, nam ? "NAM models" : "WAVE impulse responses");
    gtk_file_filter_add_pattern(filter, nam ? "*.[nN][aA][mM]" : "*.[wW][aA][vV]");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
    {
      char* file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
      SetStatus(mPlugin.LoadEffectFile(block.id, file, channel));
      g_free(file);
    }
    gtk_widget_destroy(dialog);
    Refresh();
  }

  void EnableDrag(GtkWidget* widget, int id, int index)
  {
    GtkTargetEntry target {const_cast<gchar*>(kBlockDragType), GTK_TARGET_SAME_APP, 0};
    gtk_drag_source_set(widget, GDK_BUTTON1_MASK, &target, 1, GDK_ACTION_MOVE);
    gtk_drag_dest_set(widget, GTK_DEST_DEFAULT_ALL, &target, 1, GDK_ACTION_MOVE);
    g_object_set_data(G_OBJECT(widget), "nam-block", GINT_TO_POINTER(id));
    g_object_set_data(G_OBJECT(widget), "nam-index", GINT_TO_POINTER(index));
    g_signal_connect(widget, "drag-data-get", G_CALLBACK(+[](
      GtkWidget* source, GdkDragContext*, GtkSelectionData* data, guint, guint, gpointer) {
        const int block = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(source), "nam-block"));
        gtk_selection_data_set(data, gdk_atom_intern_static_string(kBlockDragType), 8,
                               reinterpret_cast<const guchar*>(&block), sizeof(block));
      }), nullptr);
    g_signal_connect(widget, "drag-data-received", G_CALLBACK(+[](
      GtkWidget* dest, GdkDragContext* context, gint, gint, GtkSelectionData* data, guint, guint time, gpointer ptr) {
        auto& editor = *static_cast<LinuxEditor*>(ptr);
        const bool valid = gtk_selection_data_get_length(data) == sizeof(int);
        if (valid)
        {
          int moved = 0;
          std::memcpy(&moved, gtk_selection_data_get_data(data), sizeof(moved));
          editor.mPlugin.MoveEffect(moved, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dest), "nam-index")));
          editor.mPlugin.SelectEffect(moved);
        }
        gtk_drag_finish(context, valid, false, time);
      }), this);
  }

  void Refresh()
  {
    const auto effects = mPlugin.GetEffects();
    int selected = mPlugin.GetSelectedEffect();
    const auto find = [&](int id) {
      return std::find_if(effects.begin(), effects.end(), [id](const Settings& block) { return block.id == id; });
    };
    if (find(selected) == effects.end())
    {
      selected = effects.empty() ? 0 : effects.front().id;
      mPlugin.SelectEffect(selected);
    }

    Clear(mChain);
    Clear(mInspector);
    GtkWidget* track = nullptr;
    for (int index = 0; index < static_cast<int>(effects.size()); ++index)
    {
      if (index % 6 == 0)
      {
        track = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        Pack(mChain, track);
      }
      const auto& block = effects[index];
      auto* button = Button(track, mPlugin.EffectName(block.type), [this, id = block.id] {
        mPlugin.SelectEffect(id);
        Refresh();
      });
      gtk_widget_set_size_request(button, 104, 32);
      const auto filename = std::filesystem::u8path(block.path).filename().string();
      gtk_widget_set_tooltip_text(button, filename.empty() ? "Drag to reorder" : filename.c_str());
      auto* style = gtk_widget_get_style_context(button);
      if (block.id == selected)
        gtk_style_context_add_class(style, "suggested-action");
      if (!block.enabled)
        gtk_style_context_add_class(style, "nam-bypassed");
      EnableDrag(button, block.id, index);
    }

    if (const auto block = find(selected); block != effects.end())
    {
      const int position = static_cast<int>(std::distance(effects.begin(), block));
      auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
      Pack(mInspector, row);
      Label(row, mPlugin.EffectName(block->type));
      Button(row, block->enabled ? "Bypass" : "Enable", [this, effect = *block] {
        mPlugin.SetEffectEnabled(effect.id, !effect.enabled);
        Refresh();
      });
      Button(row, "Remove", [this, selected] { mPlugin.RemoveEffect(selected); Refresh(); });
      Button(row, "Move left", [this, selected, position] { mPlugin.MoveEffect(selected, position - 1); Refresh(); });
      Button(row, "Move right", [this, selected, position] { mPlugin.MoveEffect(selected, position + 1); Refresh(); });
      if (block->type == Type::Gate)
      {
        if (selected == NeuralAmpModeler::kMainGate)
          Param(mInspector, kNoiseGateThreshold);
        else
          Range(mInspector, "Threshold (dB)", -100.0, 0.0, 0.1, block->threshold, [this, selected](double value) {
            mPlugin.SetEffectThreshold(selected, value);
          });
        Label(mInspector, "Sidechain");
        std::vector<int> sources {NeuralAmpModeler::kEffectInput, NeuralAmpModeler::kChainInput};
        std::vector<std::string> titles {"Block input", "Chain input"};
        for (auto it = effects.begin(); it != block; ++it)
        {
          sources.push_back(it->id);
          titles.emplace_back(std::to_string(titles.size() - 1) + "  " + mPlugin.EffectName(it->type) + " output");
        }
        const int source = static_cast<int>(std::distance(sources.begin(), std::find(sources.begin(), sources.end(), block->sidechain)));
        Choice(mInspector, titles, source, [this, selected, sources](int index) {
          mPlugin.SetEffectSidechain(selected, sources[index]);
        });
      }
      else
      {
        const std::string label = block->path.empty() ? "Load file..." : std::filesystem::u8path(block->path).filename().string();
        Button(mInspector, label.c_str(), [this, effect = *block] { Load(effect, 0); });
        const bool stereo = mPlugin.GetParam(kChannelMode)->Int() == 1;
        const bool split = (selected == NeuralAmpModeler::kMainNam && !mPlugin.GetParam(kNAMLink)->Bool()) ||
                           (selected == NeuralAmpModeler::kMainCab && !mPlugin.GetParam(kIRLink)->Bool());
        if (stereo && split)
          Button(mInspector, "Load right channel...", [this, effect = *block] { Load(effect, 1); });
        if (block->type == Type::Nam)
        {
          const bool left = ModelControlsFor(selected, 0, stereo && split ? "Left model" : nullptr);
          const bool right = stereo && split && ModelControlsFor(selected, 1, "Right model");
          if (!left && !right && selected == NeuralAmpModeler::kMainNam)
            for (const auto index : {kEQActive, kToneStackType, kEQPostNAM, kToneBass, kToneMid, kToneTreble})
              Param(mInspector, index);
          if (!left && !right && selected > NeuralAmpModeler::kMainCab && !block->path.empty())
            ExtraTone(*block);
          if (selected == NeuralAmpModeler::kMainNam && mPlugin.PrimaryHasModelSize())
            Param(mInspector, kSlim);
          else if (block->hasModelSize)
            Range(mInspector, "Model size", 0.0, 1.0, 0.01, block->modelSize, [this, selected](double size) {
              mPlugin.SetExtraEffectParam(selected, kSlim, size);
            });
        }
      }
    }
    mSignature = ChainSignature();
    gtk_widget_show_all(mChain);
    gtk_widget_show_all(mInspector);
  }

  void ExtraTone(const Settings& block)
  {
    const int id = block.id;
    Label(mInspector, "Tone stack");
    Choice(mInspector, {"Off", "On"}, block.tone.active ? 1 : 0, [this, id](int index) {
      mPlugin.SetExtraEffectParam(id, kEQActive, index);
    });
    std::vector<std::string> types;
    for (int index = 0; index < dsp::tone_stack::kNumToneStackTypes; ++index)
      types.emplace_back(dsp::tone_stack::GetToneStackTypeName(dsp::tone_stack::ToneStackTypeFromInt(index)));
    Choice(mInspector, types, block.tone.type, [this, id](int index) {
      mPlugin.SetExtraEffectParam(id, kToneStackType, index);
    });
    Choice(mInspector, {"Pre NAM", "Post NAM"}, block.tone.post ? 1 : 0, [this, id](int index) {
      mPlugin.SetExtraEffectParam(id, kEQPostNAM, index);
    });
    static constexpr std::array<const char*, 3> names {"Bass", "Middle", "Treble"};
    for (size_t index = 0; index < names.size(); ++index)
      Range(mInspector, names[index], 0.0, 10.0, 0.1, block.tone.values[index], [this, id, index](double value) {
        mPlugin.SetExtraEffectParam(id, kToneBass + static_cast<int>(index), value);
      });
  }

  bool ModelControlsFor(int id, int channel, const char* heading)
  {
    const auto controls = mPlugin.GetEffectModelControls(id, channel);
    if (!controls)
      return false;
    if (heading)
      Label(mInspector, heading);
    for (size_t index = 0; index < controls->specs.size(); ++index)
    {
      const auto& spec = controls->specs[index];
      const auto set = [this, id, channel, index](double value) {
        mPlugin.SetEffectModelControl(id, channel, index, static_cast<float>(value));
      };
      if (!spec.choices.empty())
      {
        Label(mInspector, spec.name.c_str());
        Choice(mInspector, spec.choices, static_cast<int>(controls->Get(index)), set);
      }
      else if (spec.maximum > spec.minimum)
        Range(mInspector, spec.name.c_str(), spec.minimum, spec.maximum,
              (spec.maximum - spec.minimum) / 300.0, controls->Get(index), set);
    }
    return true;
  }

  NeuralAmpModeler& mPlugin;
  GtkWidget* mRoot = nullptr;
  GtkWidget* mChain = nullptr;
  GtkWidget* mInspector = nullptr;
  GtkWidget* mStatus = nullptr;
  std::string mSignature;
  guint mTimer = 0;
};

class LinuxView final : public Steinberg::CPluginView, public Steinberg::Linux::ITimerHandler
{
public:
  explicit LinuxView(NeuralAmpModeler& plugin) : mPlugin(plugin)
  {
    rect = {0, 0, PLUG_WIDTH, PLUG_HEIGHT};
    mPlugin.addRef();
  }

  ~LinuxView() override
  {
    if (mPlug)
      gtk_widget_destroy(mPlug);
    mPlugin.release();
  }

  Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override
  {
    return type && std::strcmp(type, Steinberg::kPlatformTypeX11EmbedWindowID) == 0
             ? Steinberg::kResultTrue : Steinberg::kResultFalse;
  }

  Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type) override
  {
    if (!parent || isPlatformTypeSupported(type) != Steinberg::kResultTrue)
      return Steinberg::kResultFalse;
    if (!gtk_init_check(nullptr, nullptr) || !GDK_IS_X11_DISPLAY(gdk_display_get_default()))
      return Steinberg::kResultFalse;
    mLoop = Steinberg::FUnknownPtr<Steinberg::Linux::IRunLoop>(plugFrame);
    if (!mLoop || mLoop->registerTimer(this, 16) != Steinberg::kResultOk)
      return Steinberg::kResultFalse;
    mPlug = gtk_plug_new(static_cast<Window>(reinterpret_cast<uintptr_t>(parent)));
    g_signal_connect(mPlug, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer view) {
      static_cast<LinuxView*>(view)->mPlug = nullptr;
    }), this);
    gtk_container_add(GTK_CONTAINER(mPlug), MakeLinuxEditor(mPlugin));
    gtk_widget_show_all(mPlug);
    return CPluginView::attached(parent, type);
  }

  Steinberg::tresult PLUGIN_API removed() override
  {
    if (mLoop)
    {
      mLoop->unregisterTimer(this);
      mLoop = nullptr;
    }
    if (mPlug)
    {
      gtk_widget_destroy(mPlug);
      mPlug = nullptr;
    }
    return CPluginView::removed();
  }

  void PLUGIN_API onTimer() override
  {
    for (int dispatch = 0; dispatch < 32 && g_main_context_pending(nullptr); ++dispatch)
      g_main_context_iteration(nullptr, false);
  }

  Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** object) override
  {
    QUERY_INTERFACE(iid, object, Steinberg::Linux::ITimerHandler::iid, Steinberg::Linux::ITimerHandler)
    return CPluginView::queryInterface(iid, object);
  }
  Steinberg::uint32 PLUGIN_API addRef() override { return CPluginView::addRef(); }
  Steinberg::uint32 PLUGIN_API release() override { return CPluginView::release(); }

private:
  NeuralAmpModeler& mPlugin;
  GtkWidget* mPlug = nullptr;
  Steinberg::FUnknownPtr<Steinberg::Linux::IRunLoop> mLoop;
};
}

GtkWidget* MakeLinuxEditor(NeuralAmpModeler& plugin)
{
  auto* editor = new LinuxEditor(plugin);
  auto* widget = editor->Root();
  g_object_set_data_full(G_OBJECT(widget), "nam-linux-editor", editor,
                         +[](gpointer data) { delete static_cast<LinuxEditor*>(data); });
  return widget;
}

Steinberg::IPlugView* PLUGIN_API NeuralAmpModeler::createView(const char* name)
{
  return name && std::strcmp(name, Steinberg::Vst::ViewType::kEditor) == 0 ? new LinuxView(*this) : nullptr;
}
