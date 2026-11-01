#ifndef NIOH4P_CORE_ADDONS_H
#define NIOH4P_CORE_ADDONS_H
#include "../common/nioh4p_addon.h"

unsigned nioh4p_load_addons(const Nioh4pCoreApi *api, const char *game_directory,
                           Nioh4pRules *local_rules,
                           int (*install_hook)(const Nioh4pHook *hook));
void nioh4p_addons_rules_changed(const Nioh4pRules *rules);
DWORD WINAPI nioh4p_addons_worker(LPVOID unused);
#endif
