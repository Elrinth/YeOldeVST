// A CLAP extension for a plugin's own program list, per instance: what it offers to switch between now.
//
// CLAP's preset discovery lists presets per plugin file, before any instance exists. A plugin whose
// programs depend on the instance cannot use it: YeOldeVST's are those of the VST2 plugin an instance
// hosts (Sylenth1's banks, another instance's TAL-NoiseMaker programs). This extension lets a host list
// and switch them, as VST2 hosts did with effGetProgramNameIndexed / effSetProgram.
//
// MorningDAW lists them in a plugin device's preset browser (Plugin presets) and selects them there.
// The same file is in both projects; keep them identical.
#pragma once

#include <clap/plugin.h>

#ifdef __cplusplus
extern "C" {
#endif

static const char CLAP_EXT_PROGRAM_LIST[] = "com.thefirstboss.program-list/1";

typedef struct clap_plugin_program_list {
  // How many programs the instance offers now (0: none). [main-thread]
  uint32_t(CLAP_ABI *count)(const clap_plugin_t *plugin);
  // The name of program `index` (UTF-8, null-terminated, cut to `capacity`). [main-thread]
  bool(CLAP_ABI *get_name)(const clap_plugin_t *plugin, uint32_t index, char *name, uint32_t capacity);
  // The program selected now, or -1 when it cannot say. [main-thread]
  int32_t(CLAP_ABI *current)(const clap_plugin_t *plugin);
  // Selects program `index`, as the plugin's own program picker would: its parameters and state follow
  // (the plugin tells the host through clap.params rescan and clap.state mark_dirty). [main-thread]
  bool(CLAP_ABI *select)(const clap_plugin_t *plugin, uint32_t index);
} clap_plugin_program_list_t;

typedef struct clap_host_program_list {
  // The list changed: another plugin inside, a bank loaded, a program renamed. The host asks again. [main-thread]
  void(CLAP_ABI *changed)(const clap_host_t *host);
} clap_host_program_list_t;

#ifdef __cplusplus
}
#endif
