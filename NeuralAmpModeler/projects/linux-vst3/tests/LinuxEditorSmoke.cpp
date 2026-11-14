#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

#include <gtk/gtkx.h>
#include <dlfcn.h>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace
{
using namespace Steinberg;

class HostFrame final : public IPlugFrame, public Linux::IRunLoop
{
public:
  tresult PLUGIN_API queryInterface(const TUID id, void** object) override
  {
    QUERY_INTERFACE(id, object, FUnknown::iid, IPlugFrame)
    QUERY_INTERFACE(id, object, IPlugFrame::iid, IPlugFrame)
    QUERY_INTERFACE(id, object, Linux::IRunLoop::iid, Linux::IRunLoop)
    *object = nullptr;
    return kNoInterface;
  }

  uint32 PLUGIN_API addRef() override { return ++mReferences; }
  uint32 PLUGIN_API release() override { return --mReferences; }

  tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* size) override { return view->onSize(size); }
  tresult PLUGIN_API registerEventHandler(Linux::IEventHandler*, Linux::FileDescriptor) override { return kNotImplemented; }
  tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler*) override { return kNotImplemented; }

  tresult PLUGIN_API registerTimer(Linux::ITimerHandler* handler, Linux::TimerInterval) override
  {
    mTimer = handler;
    return kResultOk;
  }

  tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler* handler) override
  {
    if (mTimer != handler)
      return kResultFalse;
    mTimer = nullptr;
    return kResultOk;
  }

  void Tick()
  {
    if (mTimer)
      mTimer->onTimer();
    for (int dispatch = 0; dispatch < 32 && g_main_context_pending(nullptr); ++dispatch)
      g_main_context_iteration(nullptr, false);
  }

private:
  Linux::ITimerHandler* mTimer = nullptr;
  uint32 mReferences = 1;
};

template<typename T>
auto OwnVst(T* pointer)
{
  return std::unique_ptr<T, void(*)(T*)>(pointer, [](T* object) {
    if (object)
      object->release();
  });
}

void Require(bool condition, const char* message)
{
  if (!condition)
    throw std::runtime_error(message);
}

template<typename Match>
GtkWidget* FindWidget(GtkWidget* node, const Match& match)
{
  if (match(node))
    return node;
  if (!GTK_IS_CONTAINER(node))
    return nullptr;
  GtkWidget* found = nullptr;
  GList* children = gtk_container_get_children(GTK_CONTAINER(node));
  for (GList* child = children; child && !found; child = child->next)
    found = FindWidget(GTK_WIDGET(child->data), match);
  g_list_free(children);
  return found;
}

GtkWidget* FindInEditor(GtkWidget* socket, const char* caption)
{
  Require(gtk_socket_get_plug_window(GTK_SOCKET(socket)) != nullptr, "The VST3 editor did not embed an X11 window.");
  GtkWidget* found = nullptr;
  GList* topLevels = gtk_window_list_toplevels();
  for (GList* window = topLevels; window && !found; window = window->next)
  {
    found = FindWidget(GTK_WIDGET(window->data), [&](GtkWidget* widget) {
      if (!GTK_IS_BUTTON(widget) || !gtk_widget_get_visible(widget))
        return false;
      const char* label = gtk_button_get_label(GTK_BUTTON(widget));
      return label && std::strcmp(label, caption) == 0;
    });
  }
  g_list_free(topLevels);
  return found;
}

void ExercisePlugin(const char* library)
{
  Require(gtk_init_check(nullptr, nullptr), "Could not open the X11 test display.");
  void* module = dlopen(library, RTLD_NOW | RTLD_LOCAL);
  if (!module)
    throw std::runtime_error(dlerror());
  std::unique_ptr<void, int(*)(void*)> libraryOwner(module, dlclose);
  using ModuleEntry = bool(*)(void*);
  using ModuleExit = bool(*)();
  using GetFactory = IPluginFactory* (*)();
  auto entry = reinterpret_cast<ModuleEntry>(dlsym(module, "ModuleEntry"));
  auto exit = reinterpret_cast<ModuleExit>(dlsym(module, "ModuleExit"));
  auto getFactory = reinterpret_cast<GetFactory>(dlsym(module, "GetPluginFactory"));
  Require(entry && exit && getFactory, "The installed VST3 lacks its module entry points.");
  Require(entry(module), "The installed VST3 failed module initialization.");
  {
    auto factory = OwnVst(getFactory());
    Require(static_cast<bool>(factory), "The installed VST3 returned no factory.");
    PClassInfo info {};
    Require(factory->getClassInfo(0, &info) == kResultOk, "The VST3 has no component class.");
    Vst::IComponent* instance = nullptr;
    Require(factory->createInstance(info.cid, Vst::IComponent::iid, reinterpret_cast<void**>(&instance)) == kResultOk && instance,
            "The VST3 component could not be created.");
    auto component = OwnVst(instance);
    Require(component->initialize(nullptr) == kResultOk, "The VST3 failed to initialize.");
    Vst::IEditController* interface = nullptr;
    Require(component->queryInterface(Vst::IEditController::iid, reinterpret_cast<void**>(&interface)) == kResultOk && interface,
            "The VST3 did not expose its editor controller.");
    auto controller = OwnVst(interface);
    auto view = OwnVst(controller->createView(Vst::ViewType::kEditor));
    Require(static_cast<bool>(view), "The VST3 did not expose a graphical editor.");
    Require(view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) == kResultTrue,
            "The VST3 editor rejected X11 embedding.");

    HostFrame host;
    auto* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    auto* socket = gtk_socket_new();
    gtk_container_add(GTK_CONTAINER(window), socket);
    gtk_window_set_default_size(GTK_WINDOW(window), 860, 510);
    gtk_widget_show_all(window);
    const auto xid = gtk_socket_get_id(GTK_SOCKET(socket));
    view->setFrame(&host);
    Require(view->attached(reinterpret_cast<void*>(static_cast<uintptr_t>(xid)), kPlatformTypeX11EmbedWindowID) == kResultOk,
            "The VST3 editor could not attach to an X11 host.");
    for (int attempt = 0; attempt < 20; ++attempt)
    {
      host.Tick();
      g_usleep(10000);
    }
    auto* addGate = FindInEditor(socket, "+ Gate");
    Require(addGate && FindInEditor(socket, "+ NAM") && FindInEditor(socket, "+ Cab"),
            "The VST3 effect chain controls were missing from the embedded editor.");
    if (const char* preview = std::getenv("NAM_LINUX_EDITOR_PREVIEW"))
    {
      auto* capture = gdk_pixbuf_get_from_window(gtk_widget_get_window(window), 0, 0,
        gtk_widget_get_allocated_width(window), gtk_widget_get_allocated_height(window));
      if (capture)
      {
        gdk_pixbuf_save(capture, preview, "png", nullptr, nullptr);
        g_object_unref(capture);
      }
    }
    gtk_button_clicked(GTK_BUTTON(addGate));
    host.Tick();
    Require(FindInEditor(socket, "Remove") && FindInEditor(socket, "Move left"),
            "The embedded effect inspector did not respond after adding a block.");

    Require(view->removed() == kResultOk, "The VST3 editor did not close cleanly.");
    view->setFrame(nullptr);
    view.reset();
    gtk_widget_destroy(window);
    component->terminate();
  }
  Require(exit(), "The installed VST3 failed module shutdown.");
  std::cout << "Packaged VST3 editor embedded and responded to controls over X11\n";
}
}

int main(int argc, char** argv)
{
  try
  {
    Require(argc == 2, "Provide the VST3 shared-library path.");
    ExercisePlugin(argv[1]);
    return 0;
  }
  catch (const std::exception& error)
  {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
