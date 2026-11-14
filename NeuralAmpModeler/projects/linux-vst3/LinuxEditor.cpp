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


struct RotaryBinding
{
  double min;
  double max;
  double step;
  double value;
  double nominal;
  double previousY = 0.0;
  bool active = false;
  GtkWidget* number = nullptr;
  std::function<void(double)> update;

  void Set(GtkWidget* drawing, double requested)
  {
    value = std::clamp(requested, min, max);
    char display[52];
    g_snprintf(display, sizeof(display), "%.2f", value);
    gtk_label_set_text(GTK_LABEL(number), display);
    gtk_widget_queue_draw(drawing);
    update(value);
  }
};

void Rotary(GtkWidget* parent, const char* title, double minimum, double maximum, double step,
            double value, double nominal, std::function<void(double)> update)
{
  auto* cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
  Pack(parent, cell, true);
  auto* name = gtk_label_new(title);
  gtk_widget_set_name(name, "nam-rotary-label");
  Pack(cell, name);
  auto* drawing = gtk_drawing_area_new();
  gtk_widget_set_size_request(drawing, 68, 64);
  gtk_widget_set_halign(drawing, GTK_ALIGN_CENTER);
  Pack(cell, drawing);
  char text[52];
  g_snprintf(text, sizeof(text), "%.2f", value);
  auto* number = gtk_label_new(text);
  gtk_widget_set_name(number, "nam-rotary-value");
  Pack(cell, number);

  auto* knob = new RotaryBinding {minimum, maximum, step, value, nominal, 0, false, number, std::move(update)};
  g_object_set_data_full(G_OBJECT(drawing), "nam-rotary", knob,
    +[](gpointer data) { delete static_cast<RotaryBinding*>(data); });
  gtk_widget_add_events(drawing, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK);
  g_signal_connect(drawing, "draw", G_CALLBACK(+[](GtkWidget* widget, cairo_t* cr, gpointer data) -> gboolean {
    const auto& state = *static_cast<RotaryBinding*>(data);
    const double radius = std::min(gtk_widget_get_allocated_width(widget), gtk_widget_get_allocated_height(widget)) / 2.0 - 4;
    const double x = gtk_widget_get_allocated_width(widget) / 2.0;
    const double y = gtk_widget_get_allocated_height(widget) / 2.0;
    constexpr double begin = -3.926990817;
    constexpr double end = .7853981634;
    const double ratio = std::clamp((state.value - state.min) / (state.max - state.min), 0.0, 1.0);
    const double angle = begin + (end - begin) * ratio;
    cairo_set_line_width(cr, radius * .16);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgb(cr, .176, .176, .176);
    cairo_arc(cr, x, y, radius, begin, end);
    cairo_stroke(cr);
    if (ratio > .001)
    {
      cairo_set_source_rgb(cr, .749, 0, 0);
      cairo_arc(cr, x, y, radius, begin, angle);
      cairo_stroke(cr);
    }
    const auto face = radius * .72;
    auto* gradient = cairo_pattern_create_linear(x, y - face, x, y + face);
    cairo_pattern_add_color_stop_rgb(gradient, 0, .141, .141, .141);
    cairo_pattern_add_color_stop_rgb(gradient, 1, .110, .110, .110);
    cairo_set_source(cr, gradient);
    cairo_arc(cr, x, y, face, 0, 6.283185307);
    cairo_fill(cr);
    cairo_pattern_destroy(gradient);
    cairo_set_source_rgb(cr, .749, 0, 0);
    cairo_set_line_width(cr, radius * .09);
    cairo_move_to(cr, x + face * .35 * std::cos(angle), y + face * .35 * std::sin(angle));
    cairo_line_to(cr, x + face * .85 * std::cos(angle), y + face * .85 * std::sin(angle));
    cairo_stroke(cr);
    return TRUE;
  }), knob);
  g_signal_connect(drawing, "button-press-event", G_CALLBACK(+[](GtkWidget* widget, GdkEventButton* event, gpointer data) -> gboolean {
    if (event->button != 1)
      return FALSE;
    auto& state = *static_cast<RotaryBinding*>(data);
    if (event->type == GDK_2BUTTON_PRESS)
      state.Set(widget, state.nominal);
    state.previousY = event->y;
    state.active = true;
    return TRUE;
  }), knob);
  g_signal_connect(drawing, "button-release-event", G_CALLBACK(+[](GtkWidget*, GdkEventButton*, gpointer data) -> gboolean {
    static_cast<RotaryBinding*>(data)->active = false;
    return TRUE;
  }), knob);
  g_signal_connect(drawing, "motion-notify-event", G_CALLBACK(+[](GtkWidget* widget, GdkEventMotion* event, gpointer data) -> gboolean {
    auto& state = *static_cast<RotaryBinding*>(data);
    if (state.active)
    {
      const auto scale = (event->state & GDK_SHIFT_MASK) ? .0015 : .0075;
      state.Set(widget, state.value + (state.previousY - event->y) * (state.max - state.min) * scale);
      state.previousY = event->y;
    }
    return TRUE;
  }), knob);
  g_signal_connect(drawing, "scroll-event", G_CALLBACK(+[](GtkWidget* widget, GdkEventScroll* event, gpointer data) -> gboolean {
    auto& state = *static_cast<RotaryBinding*>(data);
    const auto direction = event->direction == GDK_SCROLL_UP ? 1 : -1;
    state.Set(widget, state.value + direction * (event->state & GDK_SHIFT_MASK ? 1 : 5) * state.step);
    return TRUE;
  }), knob);
}

class LinuxEditor
{
public:
  explicit LinuxEditor(NeuralAmpModeler& plugin) : mPlugin(plugin)
  {
    mRoot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_size_request(mRoot, 790, 520);
    gtk_widget_set_name(mRoot, "nam-steroids");
    gtk_container_set_border_width(GTK_CONTAINER(mRoot), 14);

    auto* top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_name(top, "nam-top");
    gtk_container_set_border_width(GTK_CONTAINER(top), 7);
    Pack(mRoot, top);
    for (int index : {static_cast<int>(kInputLevel), -1, static_cast<int>(kOutputLevel)})
    {
      if (index >= 0)
      {
        const auto* param = mPlugin.GetParam(index);
        auto* column = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request(column, 105, -1);
        Pack(top, column);
        Rotary(column, index == kInputLevel ? "Input" : "Output", param->GetMin(), param->GetMax(), .1,
               param->Value(), param->GetDefault(), [this, index](double value) { SendParam(index, value); });
      }
      else
      {
        auto* labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
        gtk_widget_set_valign(labels, GTK_ALIGN_CENTER);
        Pack(top, labels, true);
        auto* title = gtk_label_new("NAM On Steroids");
        gtk_widget_set_name(title, "nam-title");
        Pack(labels, title);
        auto* version = gtk_label_new("v" PLUG_VERSION_STR);
        gtk_widget_set_name(version, "nam-rotary-value");
        Pack(labels, version);
      }
    }

    auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    Pack(mRoot, actions);
    Button(actions, "<", [this] { mPlugin.SelectAdjacentInternalPreset(-1); Refresh(); });
    Button(actions, "Presets", [this] { PresetWindow(); });
    Button(actions, ">", [this] { mPlugin.SelectAdjacentInternalPreset(1); Refresh(); });
    Button(actions, "Open session", [this] { Session(false); });
    Button(actions, "Save session", [this] { Session(true); });
    Button(actions, "Oversampling", [this] { QualityWindow(); });
    Button(actions, "Mixer", [this] { MixerWindow(); });
    Button(actions, "Tuner", [this] { TunerWindow(); });
    Button(actions, "Settings", [this] { SettingsWindow(); });

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

    mFiles = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    gtk_widget_set_name(mFiles, "nam-files");
    gtk_container_set_border_width(GTK_CONTAINER(mFiles), 8);
    Pack(mRoot, mFiles);
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
      "#nam-steroids { background-color: #0f0f0f; color: #ededed; font-family: Roboto, sans-serif; }"
      "#nam-top, #nam-files { background-color: #1b1b1b; border: 1px solid #292929; }"
      "#nam-title { color: #bf0000; font-size: 32px; font-weight: bold; }"
      "#nam-rotary-label, #nam-rotary-value { color: #a1a1a1; font-size: 13px; }"
      "#nam-steroids button { background-image: none; background-color: #1b1b1b; color: #ededed; "
      "border: 1px solid #292929; border-radius: 18px; padding: 4px 11px; box-shadow: none; text-shadow: none; }"
      "#nam-steroids button:hover { border-color: #bf0000; }"
      "#nam-steroids button.suggested-action { background: #bf0000; color: #fff; }"
      "#nam-steroids button.nam-bypassed { opacity: .57; }"
      "#nam-steroids combobox button { color: #ededed; border-radius: 14px; }", -1, nullptr);
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

  void MixerWindow()
  {
    auto* dialog = gtk_dialog_new_with_buttons("Mixer and filters", Window(), GTK_DIALOG_DESTROY_WITH_PARENT,
      "_Close", GTK_RESPONSE_CLOSE, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 540, 570);
    auto* body = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    auto* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    auto* fields = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(fields), 14);
    gtk_container_add(GTK_CONTAINER(scroll), fields);
    Pack(body, scroll, true);
    auto* knobs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    Pack(fields, knobs);
    for (int index : {kLevelL, kPanL, kTimeAlign, kPanR, kLevelR})
    {
      const auto* param = mPlugin.GetParam(index);
      Rotary(knobs, param->GetName(), param->GetMin(), param->GetMax(), param->GetStep(), param->Value(),
             param->GetDefault(), [this, index](double value) { SendParam(index, value); });
    }
    auto* align = Button(fields, "Auto align", [this] { mPlugin.ToggleAutoAlignment(); });
    gtk_widget_set_tooltip_text(align, "Start or cancel stereo alignment");
    for (int index : {kPanLink, kLevelLink, kPhaseInvertL, kPhaseInvertR, kLowCutFrequency, kLowCutSlope,
                      kLowCutPostNAM, kHighCutFrequency, kHighCutSlope, kHighCutPostNAM, kDCBlockerActive})
      Param(fields, index);
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
  }

  void TunerWindow()
  {
    auto* dialog = gtk_dialog_new_with_buttons("Tuner", Window(), GTK_DIALOG_DESTROY_WITH_PARENT,
      "_Close", GTK_RESPONSE_CLOSE, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 370, 230);
    auto* box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(box), 15);
    auto* label = gtk_label_new("Play a note");
    gtk_widget_set_name(label, "nam-title");
    Pack(box, label, true);
    Param(box, kTunerMute);
    mPlugin.SetTunerActive(true);
    struct Update { NeuralAmpModeler& plugin; GtkWidget* label; } update {mPlugin, label};
    const guint timer = g_timeout_add(55, +[](gpointer ptr) -> gboolean {
      const auto& data = *static_cast<Update*>(ptr);
      const auto pitch = data.plugin.GetTunerResult();
      if (pitch.valid && pitch.frequency > 0)
      {
        const int key = std::clamp(static_cast<int>(std::lround(69 + 12 * std::log2(pitch.frequency / 440.f))), 0, 127);
        constexpr const char* notes[] {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
        char text[64];
        g_snprintf(text, sizeof(text), "%s%d  %+.0f cents", notes[key % 12], key / 12 - 1, pitch.cents);
        gtk_label_set_text(GTK_LABEL(data.label), text);
      }
      else
        gtk_label_set_text(GTK_LABEL(data.label), "Play a note");
      return G_SOURCE_CONTINUE;
    }, &update);
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    g_source_remove(timer);
    mPlugin.SetTunerActive(false);
    gtk_widget_destroy(dialog);
  }

  void QualityWindow()
  {
    auto* dialog = gtk_dialog_new_with_buttons("Oversampling", Window(), GTK_DIALOG_DESTROY_WITH_PARENT,
      "_Close", GTK_RESPONSE_CLOSE, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 360, 375);
    auto* fields = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(fields), 14);
    for (int index : {kOversamplingFactor, kAntiAliasFilterPhase, kOfflineOversamplingFactor,
                      kOfflineAntiAliasFilterPhase, kPhaseMulticoreEnabled, kPhaseMulticoreThreadCount})
      Param(fields, index);
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
  }

  void PresetWindow()
  {
    auto* dialog = gtk_dialog_new_with_buttons("Presets", Window(), GTK_DIALOG_DESTROY_WITH_PARENT,
      "_Cancel", GTK_RESPONSE_CANCEL, "_Save to slot", GTK_RESPONSE_APPLY, "_Load", GTK_RESPONSE_ACCEPT, nullptr);
    auto* box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(box), 14);
    auto* name = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(name), "Preset name");
    gtk_entry_set_text(GTK_ENTRY(name), mPlugin.GetCurrentInternalPresetName());
    Pack(box, name);
    auto* slots = gtk_combo_box_text_new();
    for (int slot = 0; slot < NeuralAmpModeler::GetInternalPresetCount(); ++slot)
    {
      const auto title = std::to_string(slot + 1) + "  " + mPlugin.GetInternalPresetName(slot);
      gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(slots), title.c_str());
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(slots), std::max(0, mPlugin.GetCurrentInternalPresetIndex()));
    Pack(box, slots);
    gtk_widget_show_all(dialog);
    const int action = gtk_dialog_run(GTK_DIALOG(dialog));
    const int slot = gtk_combo_box_get_active(GTK_COMBO_BOX(slots));
    if (slot >= 0 && action == GTK_RESPONSE_ACCEPT)
      mPlugin.SelectInternalPreset(slot);
    else if (slot >= 0 && action == GTK_RESPONSE_APPLY)
    {
      mPlugin.SaveCurrentInternalPresetToSlot(slot);
      mPlugin.RenameCurrentInternalPreset(gtk_entry_get_text(GTK_ENTRY(name)));
      mPlugin.SaveCurrentInternalPreset();
    }
    gtk_widget_destroy(dialog);
    Refresh();
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
    gtk_file_filter_set_name(filter, nam ? "NAM and PNAM models" : "WAVE impulse responses");
    gtk_file_filter_add_pattern(filter, nam ? "*.[nN][aA][mM]" : "*.[wW][aA][vV]");
    if (nam)
      gtk_file_filter_add_pattern(filter, "*.[pP][nN][aA][mM]");
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
    Clear(mFiles);
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
        const bool nam = block->type == Type::Nam;
        const bool stereo = mPlugin.GetParam(kChannelMode)->Int() == 1;
        const bool split = (selected == NeuralAmpModeler::kMainNam && !mPlugin.GetParam(kNAMLink)->Bool()) ||
                           (selected == NeuralAmpModeler::kMainCab && !mPlugin.GetParam(kIRLink)->Bool());
        const auto path = mPlugin.GetEffectFile(selected, 0);
        Button(mFiles, nam ? "Select model" : "Select IR", [this, effect = *block] { Load(effect, 0); });
        auto* loaded = Label(mFiles, path.empty() ? (nam ? "No model loaded" : "No IR loaded") :
                                      std::filesystem::u8path(path).filename().string().c_str());
        gtk_label_set_ellipsize(GTK_LABEL(loaded), PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_set_hexpand(loaded, true);
        if (!path.empty())
          Button(mFiles, "Clear", [this, selected] { mPlugin.ClearEffectFile(selected); Refresh(); });
        if (stereo && split)
          Button(mFiles, "Select right channel", [this, effect = *block] { Load(effect, 1); });
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
    gtk_widget_show_all(mFiles);
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
    GtkWidget* row = nullptr;
    for (size_t index = 0; index < controls->specs.size(); ++index)
    {
      if (index % 6 == 0)
      {
        row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 15);
        gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
        Pack(mInspector, row);
      }
      const auto& spec = controls->specs[index];
      const auto set = [this, id, channel, index](double value) {
        mPlugin.SetEffectModelControl(id, channel, index, static_cast<float>(value));
      };
      if (!spec.choices.empty())
      {
        auto* cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        Pack(row, cell);
        Label(cell, spec.name.c_str());
        Choice(cell, spec.choices, static_cast<int>(controls->Get(index)), set);
      }
      else if (spec.maximum > spec.minimum)
        Rotary(row, spec.name.c_str(), spec.minimum, spec.maximum, (spec.maximum - spec.minimum) / 300.0,
               controls->Get(index), spec.nominal, set);
    }
    return true;
  }

  NeuralAmpModeler& mPlugin;
  GtkWidget* mRoot = nullptr;
  GtkWidget* mChain = nullptr;
  GtkWidget* mInspector = nullptr;
  GtkWidget* mFiles = nullptr;
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
