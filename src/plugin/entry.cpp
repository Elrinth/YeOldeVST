// clap_entry: exposes the instrument and effect flavours. The VST3 build gets the same entry and
// clap-wrapper turns it into a VST3 factory.
#include "plugin/Gui.h"
#include "plugin/Plugin.h"

#include <clap/clap.h>

#include <cstring>
#include <iterator>

namespace {

constexpr yov::Flavour kFlavours[] = {yov::Flavour::Instrument, yov::Flavour::Effect,
                                      yov::Flavour::InstrumentMulti};

uint32_t factoryCount(const clap_plugin_factory_t*) { return (uint32_t)std::size(kFlavours); }

const clap_plugin_descriptor_t* factoryDescriptor(const clap_plugin_factory_t*, uint32_t index) {
  return index < std::size(kFlavours) ? yov::Plugin::descriptor(kFlavours[index]) : nullptr;
}

const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
  if (!clap_version_is_compatible(host->clap_version)) return nullptr;
  for (auto f : kFlavours)
    if (!std::strcmp(id, yov::Plugin::descriptor(f)->id)) return (new yov::Plugin(host, f))->clapPlugin();
  return nullptr;
}

const clap_plugin_factory_t kFactory = {factoryCount, factoryDescriptor, factoryCreate};

bool entryInit(const char*) { return true; }
void entryDeinit() { yov::Gui::unregisterClasses(); }
const void* entryGetFactory(const char* id) {
  return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}

}  // namespace

extern "C" {
#ifdef _WIN32
__declspec(dllexport)
#endif
const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entryInit, entryDeinit, entryGetFactory};
}
