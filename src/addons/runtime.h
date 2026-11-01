#ifndef NIOH4P_ADDON_RUNTIME_H
#define NIOH4P_ADDON_RUNTIME_H
#include "../common/nioh4p_addon.h"
#include "../common/freestanding.h"

#define N4P_MANAGER_RVA 0x18715e0u
#define N4P_ROOT_RVA 0x1790a70u
#define N4P_SAVE_RVA 0x189f408u
#define N4P_SELECTED_RVA 0x191e290u
#define N4P_PARTY_RVA 0x18a0490u
#define N4P_PARTY_STRIDE 0x18u
#define N4P_PARTY_COUNT 4u

int n4p_config_int(const Nioh4pCoreApi *core, HMODULE module,
                   const char *key, const char *default_value,
                   int minimum, int maximum, int *value);
uint8_t *n4p_manager(const Nioh4pCoreApi *core);
unsigned n4p_party_count(const Nioh4pCoreApi *core);
#endif
