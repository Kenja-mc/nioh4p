# RE: Steam API call sites in `nioh.exe` (friends / P2P / matchmaking)

Offline static analysis of the **runtime image dump** `~/.cache/nioh3p/dumps/nioh3p_image_merged.bin` (50,779,928 bytes, `file offset == RVA`, image base `0x140000000`, so `VA = 0x140000000 + RVA`).
All RVAs below are file/RVA values; `VA = 0x140000000 + RVA`.

The on-disk `nioh.exe` was used **only** for the PE section table (it is intact on disk) and for cross-checking; the dump was used for every byte that matters. Its first `0x300` bytes (DOS/NT headers) are zeroed, so the header parse comes from the on-disk section table; the import directory *data* comes from the dump.

Disassembly recipe used throughout (windowed only):

```
nice -n 10 objdump -D -b binary -m i386:x86-64 --adjust-vma=0x140000000 \
  "$HOME/.cache/nioh3p/dumps/nioh3p_image_merged.bin" \
  --start-address=0x1405edcd0 --stop-address=0x1405edd60
```

`analysis/symbols.txt` is a **name pool with no addresses** (4,082 names, 0 RVAs) and `analysis/session_symbols.txt` likewise; address->name mapping therefore comes from `analysis/classmap.txt` (RTTI vtables: class -> vtable VA -> method VAs), with a prologue scan (an `int3` run followed by a function prologue) as fallback. Unnamed functions are reported as `sub_XXXXXX`.

---

## 1. Imports from `steam_api64.dll` (parsed from the dump's import directory)

Import directory RVA `0x1761EE0` (size `0x1A4`) holds 20 `IMAGE_IMPORT_DESCRIPTOR`s. Descriptor #0 is:

```
OFT/ILT = 0x1762D00   Name = 0x1762E74 "steam_api64.dll"   FirstThunk/IAT = 0x119BC78
```

### 1.1 The complete import list (11 entries)

| # | import name | IAT slot VA | RIP-relative ref sites in `.text` |
|---|---|---|---|
| 0 | `SteamInternal_FindOrCreateGameServerInterface` | `0x14119BC78` | 2 |
| 1 | `SteamInternal_FindOrCreateUserInterface` | `0x14119BC80` | 13 |
| 2 | `SteamAPI_Shutdown` | `0x14119BC88` | 2 |
| 3 | `SteamAPI_Init` | `0x14119BC90` | 2 |
| 4 | `SteamAPI_RunCallbacks` | `0x14119BC98` | 4 |
| 5 | `SteamAPI_UnregisterCallback` | `0x14119BCA0` | 128 |
| 6 | `SteamAPI_RegisterCallback` | `0x14119BCA8` | 48 |
| 7 | `SteamAPI_UnregisterCallResult` | `0x14119BCB0` | 107 |
| 8 | `SteamAPI_RegisterCallResult` | `0x14119BCB8` | 33 |
| 9 | `SteamAPI_GetHSteamUser` | `0x14119BCC0` | 10 |
| 10 | `SteamInternal_ContextInit` | `0x14119BCC8` | 306 |

The steam import slots occupy the contiguous IAT window `0x14119BC78 .. 0x14119BCCF` (11 x 8 bytes = 0x58).
The full IAT layout (descriptor IAT base .. last slot) confirms this window is exclusive:
`ole32.dll` `0x14119BC30..0x14119BC6F` (8 slots) sits immediately before it, `steam_api64.dll` is the last descriptor, and the
IAT data directory (`RVA 0x119B000`, size `0xCD8`) ends at `0x14119BCD8`, so exactly one 8-byte slot (`0x14119BCD0`) is unused.
No other DLL shares the steam window, so every reference counted below belongs to `steam_api64.dll`.

Scan method for the site column: one linear pass over `.text` (RVA `0x1000..0x119A498`) collecting every RIP-relative reference (`ff 15`/`ff 25` call/jmp, `48 8b 05/0d/15/1d`, `4c 8b ..`, `48 89 ..`) whose computed target lands in the IAT window; the table above counts *references*, which for `call` sites is the call instruction address (the return address is `site+6`).

### 1.2 Per-import call-site tables (RVA + enclosing function)

#### `SteamInternal_FindOrCreateGameServerInterface` — slot `0x14119BC78`, 2 site(s)

| site RVA | enclosing function |
|---|---|
| `0x5EDDC5` | `sub_5EDD90` |
| `0x116DD51` | `sub_116DD30` |

#### `SteamInternal_FindOrCreateUserInterface` — slot `0x14119BC80`, 13 site(s)

| site RVA | enclosing function |
|---|---|
| `0x5EDBA8` | `sub_5EDB90` |
| `0x5EDBF8` | `sub_5EDBE0` |
| `0x5EDC42` | `sub_5EDC30` |
| `0x5EDC98` | `sub_5EDC80` |
| `0x5EDCE8` | `sub_5EDCD0` |
| `0x5EDD38` | `sub_5EDD20` |
| `0x5EDDA2` | `sub_5EDD90` |
| `0x5EDDB6` | `sub_5EDD90` |
| `0x619568` | `sub_619510` |
| `0x6195B8` | `sub_6195A0` |
| `0xE6B578` | `sub_E6B560` |
| `0xE7BED8` | `sub_E7BEC0` |
| `0xFD1B1D` | `SaveIcon@Menu::vt[10]` |

#### `SteamAPI_Shutdown` — slot `0x14119BC88`, 2 site(s)

| site RVA | enclosing function |
|---|---|
| `0xE7BEB1` | `sub_E7BEB0` |
| `0xFD1B4D` | `SaveIcon@Menu::vt[10]` |

#### `SteamAPI_Init` — slot `0x14119BC90`, 2 site(s)

| site RVA | enclosing function |
|---|---|
| `0xE7BE14` | `sub_E7BE10` |
| `0xFD1B47` | `SaveIcon@Menu::vt[10]` |

#### `SteamAPI_RunCallbacks` — slot `0x14119BC98`, 4 site(s)

| site RVA | enclosing function |
|---|---|
| `0x5EECB0` | `CSessionP2pSteam::vt[2]` |
| `0x616EBA` | `sub_616EB0` |
| `0xE7BEA1` | `sub_E7BEA0` |
| `0xFD1B41` | `SaveIcon@Menu::vt[10]` |

#### `SteamAPI_UnregisterCallback` — slot `0x14119BCA0`, 128 site(s)

| site RVA | enclosing function |
|---|---|
| `0x5EDEE3` | `sub_5EDE90` |
| `0x5EDF65` | `CSessionP2p::vt[0]` |
| `0x5EDF81` | `CSessionP2p::vt[0]` |
| `0x5EE9B0` | `CSessionP2pSteam::vt[0]` |
| `0x5EE9CD` | `CSessionP2pSteam::vt[0]` |
| `0x5EEB31` | `sub_5EEA30` |
| `0x5EEB51` | `sub_5EEA30` |
| `0x5EEB8A` | `sub_5EEB60` |
| `0x5EEBA7` | `sub_5EEB60` |
| `0x5F0671` | `?$CCallback@VCSessionP2pSteam@@UGameLobbyJoinRequested_t@@$0A@::vt[0]` |
| `0x5F06B1` | `?$CCallback@VCSessionCreateHandle@@ULobbyDataUpdate_t@@$0A@::vt[0]` |
| `0x5F06DF` | `?$CCallbackImpl@$0BA@::vt[3]` |
| `0x5F072F` | `?$CCallbackImpl@$0BI@::vt[3]` |
| `0x5F087F` | `?$CCallback@VCSessionP2pSteam@@UGameLobbyJoinRequested_t@@$0A@::vt[3]` |
| `0x5F08EF` | `?$CCallback@VCSessionP2pSteam@@ULobbyDataUpdate_t@@$0A@::vt[3]` |
| `0x5F0984` | `sub_5F0960` |
| `0x5F09E4` | `sub_5F09C0` |
| `0x5F0A21` | `sub_5F0A20` |
| `0x5F0A31` | `sub_5F0A30` |
| `0x5F1CB0` | `CSessionP2pSessionInfo::vt[0]` |
| `0x5F1CCA` | `CSessionP2pSessionInfo::vt[0]` |
| `0x5F1CE4` | `CSessionP2pSessionInfo::vt[0]` |
| `0x5F1CFE` | `CSessionP2pSessionInfo::vt[0]` |
| `0x5F1D61` | `CSessionP2pSessionInfo::vt[0]` |
| `0x5F1D81` | `CSessionP2pSessionInfo::vt[0]` |
| `0x5F1DA1` | `CSessionP2pSessionInfo::vt[0]` |
| `0x5F1DC1` | `CSessionP2pSessionInfo::vt[0]` |
| `0x5F1DFA` | `sub_5F1DD0` |
| `0x5F1E14` | `sub_5F1DD0` |
| `0x5F1E2E` | `sub_5F1DD0` |
| `0x5F1E48` | `sub_5F1DD0` |
| `0x5F4581` | `?$CCallback@VCSessionP2pSessionInfo@@UP2PSessionConnectFail_t@@$0A@::vt[0]` |
| `0x5F45C1` | `?$CCallback@VCSessionP2pSessionInfo@@UP2PSessionRequest_t@@$0A@::vt[0]` |
| `0x5F4601` | `?$CCallback@VCSessionP2pSessionInfo@@ULobbyChatUpdate_t@@$0A@::vt[0]` |
| `0x5F462F` | `?$CCallbackImpl@$08::vt[3]` |
| `0x5F467F` | `?$CCallbackImpl@$07::vt[3]` |
| `0x5F46CF` | `?$CCallbackImpl@$0CA@::vt[3]` |
| `0x5F497F` | `?$CCallback@VCSessionP2pSessionInfo@@ULobbyDataUpdate_t@@$0A@::vt[3]` |
| `0x5F49CF` | `?$CCallback@VCSessionP2pSessionInfo@@ULobbyChatUpdate_t@@$0A@::vt[3]` |
| `0x5F4A3F` | `?$CCallback@VCSessionP2pSessionInfo@@UP2PSessionRequest_t@@$0A@::vt[3]` |
| `0x5F4AAF` | `?$CCallback@VCSessionP2pSessionInfo@@UP2PSessionConnectFail_t@@$0A@::vt[3]` |
| `0x5F4B54` | `sub_5F4B30` |
| `0x5F4BB4` | `sub_5F4B90` |
| `0x5F4C14` | `sub_5F4BF0` |
| `0x5F4C74` | `sub_5F4C50` |
| `0x5F4CB1` | `sub_5F4CB0` |
| `0x5F4CC1` | `sub_5F4CC0` |
| `0x5F4CD1` | `sub_5F4CD0` |
| `0x5F4CE1` | `sub_5F4CE0` |
| `0x5FB471` | `CSessionP2pLockHandle::vt[0]` |
| `0x5FB491` | `CSessionP2pLockHandle::vt[0]` |
| `0x5FB4DA` | `sub_5FB4A0` |
| `0x5FB63E` | `CSessionP2pLockHandle::vt[2]` |
| `0x5FB6CB` | `CSessionP2pLockHandle::vt[2]` |
| `0x5FB8D9` | `CSessionP2pLockHandle::vt[2]` |
| `0x5FBA83` | `CSessionP2pLockHandle::vt[2]` |
| `0x5FBCDF` | `?$CCallbackManual@VCSessionP2pLockHandle@@ULobbyDataUpdate_t@@$0A@::vt[3]` |
| `0x5FBDF8` | `?$CCallResult@VCSessionP2pLockHandle@@ULobbyCreated_t@@::vt[1]` |
| `0x5FBE31` | `sub_5FBE30` |
| `0x5FBE6F` | `?$CCallback@VCSessionP2pLockHandle@@ULobbyDataUpdate_t@@$0A@::vt[3]` |
| `0x5FCFDF` | `?$CCallback@VCSessionP2pPacketHandler@@ULobbyChatMsg_t@@$0A@::vt[3]` |
| `0x5FD02B` | `?$CCallback@VCSessionP2pPacketHandler@@ULobbyChatMsg_t@@$0A@::vt[3]` |
| `0x5FD061` | `sub_5FD060` |
| `0x5FD8DD` | `CSessionP2pPreparationHandle::vt[0]` |
| `0x5FD921` | `CSessionP2pPreparationHandle::vt[0]` |
| `0x5FD941` | `CSessionP2pPreparationHandle::vt[0]` |
| `0x5FD977` | `sub_5FD950` |
| `0x5FDBB8` | `CSessionP2pPreparationHandle::vt[2]` |
| `0x5FDCAA` | `CSessionP2pPreparationHandle::vt[2]` |
| `0x5FDF01` | `sub_5FDEB0` |
| `0x5FE2A1` | `?$CCallback@VCSessionP2pPreparationHandle@@USteamRelayNetworkStatus_t@@$0A@::vt[0]` |
| `0x5FE2EF` | `?$CCallbackImpl@$0BBA@::vt[3]` |
| `0x5FE33F` | `?$CCallbackManual@VCSessionP2pPreparationHandle@@USteamRelayNetworkStatus_t@@$0A@::vt[3]` |
| `0x5FE3B8` | `?$CCallbackManual@VCSessionP2pPreparationHandle@@USteamRelayNetworkStatus_t@@$0A@::vt[3]` |
| `0x5FE3F1` | `sub_5FE3F0` |
| `0x5FE42F` | `?$CCallback@VCSessionP2pPreparationHandle@@USteamRelayNetworkStatus_t@@$0A@::vt[3]` |
| `0x5FF196` | `CSessionP2pModifySessionInfoHandle::vt[0]` |
| `0x5FF1E1` | `CSessionP2pModifySessionInfoHandle::vt[0]` |
| `0x5FF201` | `CSessionP2pModifySessionInfoHandle::vt[0]` |
| `0x5FF240` | `sub_5FF210` |
| `0x5FF37E` | `CSessionP2pModifySessionInfoHandle::vt[2]` |
| `0x5FF38E` | `CSessionP2pModifySessionInfoHandle::vt[2]` |
| `0x5FF4AE` | `sub_5FF480` |
| `0x5FF7F8` | `sub_5FF570` |
| `0x6001AF` | `?$CCallbackManual@VCSessionP2pModifySessionInfoHandle@@ULobbyDataUpdate_t@@$0A@::vt[3]` |
| `0x600228` | `?$CCallbackManual@VCSessionP2pModifySessionInfoHandle@@ULobbyDataUpdate_t@@$0A@::vt[3]` |
| `0x600261` | `sub_600260` |
| `0x60029F` | `?$CCallback@VCSessionP2pModifySessionInfoHandle@@ULobbyDataUpdate_t@@$0A@::vt[3]` |
| `0x604721` | `CSessionCreateHandle::vt[0]` |
| `0x604741` | `CSessionCreateHandle::vt[0]` |
| `0x60477B` | `sub_604750` |
| `0x6048B8` | `CSessionCreateHandle::vt[2]` |
| `0x6055A1` | `sub_604F20` |
| `0x605932` | `sub_6057B0` |
| `0x6059DF` | `?$CCallbackManual@VCSessionCreateHandle@@ULobbyDataUpdate_t@@$0A@::vt[3]` |
| `0x605B58` | `?$CCallResult@VCSessionCreateHandle@@ULobbyCreated_t@@::vt[1]` |
| `0x605B91` | `sub_605B90` |
| `0x605BCF` | `?$CCallback@VCSessionCreateHandle@@ULobbyDataUpdate_t@@$0A@::vt[3]` |
| `0xE74831` | `sub_E74730` |
| `0xE74851` | `sub_E74730` |
| `0xE7487D` | `sub_E74860` |
| `0xE74897` | `sub_E74860` |
| `0xE748B1` | `sub_E74860` |
| `0xE748D0` | `sub_E74860` |
| `0xE748FF` | `?$CCallback@VCSteamAchievements@Trophy@System@@UGameOverlayActivated_t@@$0A@::vt[3]` |
| `0xE7494F` | `?$CCallback@VCSteamAchievements@Trophy@System@@UUserAchievementStored_t@@$0A@::vt[3]` |
| `0xE7499F` | `?$CCallback@VCSteamAchievements@Trophy@System@@UUserStatsReceived_t@@$0A@::vt[3]` |
| `0xE749EF` | `?$CCallback@VCSteamAchievements@Trophy@System@@UUserStatsStored_t@@$0A@::vt[3]` |
| `0xE74A3F` | `?$CCallbackImpl@$00::vt[3]` |
| `0xE74A8F` | `?$CCallbackImpl@$0JI@::vt[3]` |
| `0xE7BF51` | `sub_E7BEF0` |
| `0xE7BF7F` | `?$CCallback@VCSteamFriend@SteamFriend@@UPersonaStateChange_t@@$0A@::vt[3]` |
| `0xE7C297` | `sub_E7C250` |
| `0xE7C2B0` | `sub_E7C250` |
| `0xE7C2EF` | `?$CCallback@VCSteamStats@@UUserStatsReceived_t@@$0A@::vt[3]` |
| `0xE7C33F` | `?$CCallback@VCSteamStats@@UUserStatsStored_t@@$0A@::vt[3]` |
| `0xFD1B3B` | `SaveIcon@Menu::vt[10]` |
| `0x1192DE5` | `sub_1192DA0` |
| `0x1192ED0` | `sub_1192EA0` |
| `0x119302F` | `sub_1192FF0` |
| `0x11932B0` | `sub_1193280` |
| `0x11999D2` | `sub_11999B0` |
| `0x11999F6` | `sub_11999B0` |
| `0x1199A1A` | `sub_11999B0` |
| `0x1199A43` | `sub_11999B0` |
| `0x1199ABF` | `sub_1199A50` |
| `0x1199B24` | `sub_1199AD0` |
| `0x1199B4D` | `sub_1199AD0` |

#### `SteamAPI_RegisterCallback` — slot `0x14119BCA8`, 48 site(s)

| site RVA | enclosing function |
|---|---|
| `0x0A5E69` | `sub_0A5E50` |
| `0x0A5EAD` | `sub_0A5E50` |
| `0x0A5EEF` | `sub_0A5E50` |
| `0x0A5F31` | `sub_0A5E50` |
| `0x0A5FF0` | `sub_0A5FE0` |
| `0x0A6029` | `sub_0A6010` |
| `0x0A606D` | `sub_0A6010` |
| `0x5EE7F0` | `sub_5EE720` |
| `0x5EE88B` | `sub_5EE720` |
| `0x5EE8CC` | `sub_5EE720` |
| `0x5EEAE8` | `sub_5EEA30` |
| `0x5F07B5` | `sub_5F0760` |
| `0x5F0835` | `sub_5F07E0` |
| `0x5F099D` | `sub_5F0960` |
| `0x5F09FD` | `sub_5F09C0` |
| `0x5F19FB` | `sub_5F1980` |
| `0x5F1A39` | `sub_5F1980` |
| `0x5F1A77` | `sub_5F1980` |
| `0x5F1AB8` | `sub_5F1980` |
| `0x5F4755` | `sub_5F4700` |
| `0x5F47D5` | `sub_5F4780` |
| `0x5F4855` | `sub_5F4800` |
| `0x5F48D5` | `sub_5F4880` |
| `0x5F4B6D` | `sub_5F4B30` |
| `0x5F4BCD` | `sub_5F4B90` |
| `0x5F4C2D` | `sub_5F4BF0` |
| `0x5F4C8D` | `sub_5F4C50` |
| `0x5FB6E9` | `CSessionP2pLockHandle::vt[2]` |
| `0x5FB8F7` | `CSessionP2pLockHandle::vt[2]` |
| `0x5FBE0E` | `?$CCallResult@VCSessionP2pLockHandle@@ULobbyCreated_t@@::vt[1]` |
| `0x5FC021` | `CSessionP2pPacket::vt[2]` |
| `0x5FCE30` | `sub_5FCDF0` |
| `0x5FD048` | `?$CCallback@VCSessionP2pPacketHandler@@ULobbyChatMsg_t@@$0A@::vt[3]` |
| `0x5FDBD6` | `CSessionP2pPreparationHandle::vt[2]` |
| `0x5FE3CE` | `?$CCallbackManual@VCSessionP2pPreparationHandle@@USteamRelayNetworkStatus_t@@$0A@::vt[3]` |
| `0x5FF3AC` | `CSessionP2pModifySessionInfoHandle::vt[2]` |
| `0x5FF4CC` | `sub_5FF480` |
| `0x60023E` | `?$CCallbackManual@VCSessionP2pModifySessionInfoHandle@@ULobbyDataUpdate_t@@$0A@::vt[3]` |
| `0x6055C2` | `sub_604F20` |
| `0x605B6E` | `?$CCallResult@VCSessionCreateHandle@@ULobbyCreated_t@@::vt[1]` |
| `0xE74764` | `sub_E74730` |
| `0xE74793` | `sub_E74730` |
| `0xE747C2` | `sub_E74730` |
| `0xE747F1` | `sub_E74730` |
| `0xE7BF22` | `sub_E7BEF0` |
| `0xE7C1D4` | `sub_E7C1A0` |
| `0xE7C203` | `sub_E7C1A0` |
| `0xFD1B35` | `SaveIcon@Menu::vt[10]` |

#### `SteamAPI_UnregisterCallResult` — slot `0x14119BCB0`, 107 site(s)

| site RVA | enclosing function |
|---|---|
| `0x5FB4F8` | `sub_5FB4A0` |
| `0x5FBD5C` | `sub_5FBD40` |
| `0x5FBEB2` | `sub_5FBEA0` |
| `0x6037EF` | `sub_6037C0` |
| `0x603905` | `CSessionJoinHandle::vt[2]` |
| `0x603F16` | `sub_603EC0` |
| `0x60446C` | `sub_604450` |
| `0x6044A1` | `sub_604450` |
| `0x604532` | `sub_604520` |
| `0x604799` | `sub_604750` |
| `0x604D0D` | `sub_604940` |
| `0x605A5C` | `sub_605A40` |
| `0x605A91` | `sub_605A40` |
| `0x605C12` | `sub_605C00` |
| `0x609CDF` | `sub_609CB0` |
| `0x60A1EC` | `sub_609EA0` |
| `0x60A8DC` | `sub_60A8C0` |
| `0x60A911` | `sub_60A8C0` |
| `0x60A9A2` | `sub_60A990` |
| `0x617681` | `sub_617630` |
| `0x6176A9` | `sub_617630` |
| `0x6176CE` | `sub_617630` |
| `0x6176F0` | `sub_617630` |
| `0x617744` | `sub_617630` |
| `0x617772` | `sub_617630` |
| `0x6177A0` | `sub_617630` |
| `0x6177C5` | `sub_617630` |
| `0x6177E7` | `sub_617630` |
| `0x617809` | `sub_617630` |
| `0x6178A5` | `sub_617870` |
| `0x6178CD` | `sub_617870` |
| `0x6178F2` | `sub_617870` |
| `0x617914` | `sub_617870` |
| `0x617975` | `sub_617940` |
| `0x6179A3` | `sub_617940` |
| `0x6179D1` | `sub_617940` |
| `0x6179F6` | `sub_617940` |
| `0x617A18` | `sub_617940` |
| `0x617A3A` | `sub_617940` |
| `0x617A95` | `sub_617A60` |
| `0x617ABD` | `sub_617A60` |
| `0x617AE2` | `sub_617A60` |
| `0x617B04` | `sub_617A60` |
| `0x61815C` | `sub_618140` |
| `0x6181CC` | `sub_6181B0` |
| `0x61823C` | `sub_618220` |
| `0x6182AC` | `sub_618290` |
| `0x618312` | `sub_618300` |
| `0x618342` | `sub_618330` |
| `0x618372` | `sub_618360` |
| `0x6183A2` | `sub_618390` |
| `0x618949` | `sub_6188B0` |
| `0x618A13` | `sub_6188B0` |
| `0x619775` | `sub_619740` |
| `0x6197A3` | `sub_619740` |
| `0x6197D1` | `sub_619740` |
| `0x6197F6` | `sub_619740` |
| `0x619818` | `sub_619740` |
| `0x61983A` | `sub_619740` |
| `0x619A5B` | `sub_619A30` |
| `0x619B24` | `sub_619AA0` |
| `0x619BE1` | `sub_619BB0` |
| `0x619E66` | `sub_619DA0` |
| `0x619EDF` | `sub_619DA0` |
| `0x61A10E` | `sub_61A060` |
| `0x61A22D` | `sub_61A180` |
| `0x61A39C` | `sub_61A380` |
| `0x61A3D9` | `sub_61A380` |
| `0x61A4AC` | `sub_61A490` |
| `0x61A4E1` | `sub_61A490` |
| `0x61A5AC` | `sub_61A590` |
| `0x61A5E1` | `sub_61A590` |
| `0x61A6AC` | `sub_61A690` |
| `0x61A6E1` | `sub_61A690` |
| `0x61A7AC` | `sub_61A790` |
| `0x61A7E1` | `sub_61A790` |
| `0x61A8AC` | `sub_61A890` |
| `0x61A8E1` | `sub_61A890` |
| `0x61A972` | `sub_61A960` |
| `0x61A9A2` | `sub_61A990` |
| `0x61A9D2` | `sub_61A9C0` |
| `0x61AA02` | `sub_61A9F0` |
| `0x61AA32` | `sub_61AA20` |
| `0x61AA62` | `sub_61AA50` |
| `0x61B12D` | `sub_61B0C0` |
| `0x61B436` | `sub_61B2D0` |
| `0x61B891` | `sub_61B870` |
| `0x61B921` | `sub_61B900` |
| `0x61B9B1` | `sub_61B990` |
| `0x61BA41` | `sub_61BA20` |
| `0xD6669D` | `sub_D664F0` |
| `0xD666C0` | `sub_D664F0` |
| `0xD666E3` | `sub_D664F0` |
| `0xD674CF` | `sub_D67350` |
| `0xD676AE` | `sub_D67570` |
| `0xD68C9F` | `sub_D68BF0` |
| `0xD68D59` | `sub_D68BF0` |
| `0xE758F6` | `sub_E758C0` |
| `0xE7C275` | `sub_E7C250` |
| `0xE7C83C` | `sub_E7C7B0` |
| `0xFD1B2F` | `SaveIcon@Menu::vt[10]` |
| `0x1192E0C` | `sub_1192DA0` |
| `0x1193253` | `sub_1193220` |
| `0x11932D7` | `sub_1193280` |
| `0x1193363` | `sub_1193330` |
| `0x1199A81` | `sub_1199A50` |
| `0x1199AF5` | `sub_1199AD0` |

#### `SteamAPI_RegisterCallResult` — slot `0x14119BCB8`, 33 site(s)

| site RVA | enclosing function |
|---|---|
| `0x60392A` | `CSessionJoinHandle::vt[2]` |
| `0x603F3B` | `sub_603EC0` |
| `0x6044C5` | `sub_604450` |
| `0x604D32` | `sub_604940` |
| `0x605AB5` | `sub_605A40` |
| `0x60A211` | `sub_609EA0` |
| `0x60A935` | `sub_60A8C0` |
| `0x61896E` | `sub_6188B0` |
| `0x618A38` | `sub_6188B0` |
| `0x619A80` | `sub_619A30` |
| `0x619B48` | `sub_619AA0` |
| `0x619C06` | `sub_619BB0` |
| `0x619EFE` | `sub_619DA0` |
| `0x61A12E` | `sub_61A060` |
| `0x61A25C` | `sub_61A180` |
| `0x61A3F6` | `sub_61A380` |
| `0x61A505` | `sub_61A490` |
| `0x61A605` | `sub_61A590` |
| `0x61A705` | `sub_61A690` |
| `0x61A805` | `sub_61A790` |
| `0x61A905` | `sub_61A890` |
| `0x61B152` | `sub_61B0C0` |
| `0x61B45A` | `sub_61B2D0` |
| `0x61B8B5` | `sub_61B870` |
| `0x61B945` | `sub_61B900` |
| `0x61B9D5` | `sub_61B990` |
| `0x61BA65` | `sub_61BA20` |
| `0xD674EE` | `sub_D67350` |
| `0xD676CD` | `sub_D67570` |
| `0xD68CBE` | `sub_D68BF0` |
| `0xD68D79` | `sub_D68BF0` |
| `0xE7C85C` | `sub_E7C7B0` |
| `0xFD1B29` | `SaveIcon@Menu::vt[10]` |

#### `SteamAPI_GetHSteamUser` — slot `0x14119BCC0`, 10 site(s)

| site RVA | enclosing function |
|---|---|
| `0x5EDB99` | `sub_5EDB90` |
| `0x5EDBE9` | `sub_5EDBE0` |
| `0x5EDC89` | `sub_5EDC80` |
| `0x5EDCD9` | `sub_5EDCD0` |
| `0x5EDD29` | `sub_5EDD20` |
| `0x619559` | `sub_619510` |
| `0x6195A9` | `sub_6195A0` |
| `0xE6B569` | `sub_E6B560` |
| `0xE7BEC9` | `sub_E7BEC0` |
| `0xFD1B23` | `SaveIcon@Menu::vt[10]` |

#### `SteamInternal_ContextInit` — slot `0x14119BCC8`, 306 site(s)

| site RVA | enclosing function |
|---|---|
| `0x5E9384` | `sub_5E92C0` |
| `0x5E9DB1` | `sub_5E9D20` |
| `0x5E9E06` | `sub_5E9D20` |
| `0x5E9F22` | `sub_5E9EB0` |
| `0x5E9FE2` | `sub_5E9EB0` |
| `0x5EB5EC` | `sub_5EB400` |
| `0x5ECFE1` | `sub_5ECC50` |
| `0x5ECFFE` | `sub_5ECC50` |
| `0x5ED057` | `sub_5ECC50` |
| `0x5EDBCB` | `sub_5EDBC0` |
| `0x5EDC1B` | `sub_5EDC10` |
| `0x5EDC6B` | `sub_5EDC60` |
| `0x5EDCBB` | `sub_5EDCB0` |
| `0x5EDD0B` | `sub_5EDD00` |
| `0x5EDD5B` | `sub_5EDD50` |
| `0x5EDD7B` | `sub_5EDD70` |
| `0x5EDDEB` | `sub_5EDDE0` |
| `0x5EDE0B` | `sub_5EDE00` |
| `0x5EE31C` | `sub_5EE200` |
| `0x5EE350` | `sub_5EE200` |
| `0x5EE36F` | `sub_5EE200` |
| `0x5EECE6` | `CSessionP2pSteam::vt[2]` |
| `0x5EF122` | `CSessionP2pSteam::vt[2]` |
| `0x5EF7FA` | `sub_5EF7E0` |
| `0x5EF8F8` | `sub_5EF820` |
| `0x5F00CE` | `sub_5F00A0` |
| `0x5F0113` | `sub_5F00A0` |
| `0x5F0146` | `sub_5F00A0` |
| `0x5F01F9` | `sub_5F0180` |
| `0x5F0216` | `sub_5F0180` |
| `0x5F0277` | `sub_5F0180` |
| `0x5F040C` | `sub_5F0310` |
| `0x5F212E` | `sub_5F2010` |
| `0x5F21BB` | `sub_5F2010` |
| `0x5F22E7` | `sub_5F2010` |
| `0x5F2346` | `sub_5F2010` |
| `0x5F2463` | `sub_5F2010` |
| `0x5F2484` | `sub_5F2010` |
| `0x5F2898` | `sub_5F2640` |
| `0x5F28B9` | `sub_5F2640` |
| `0x5F2EC0` | `sub_5F2EB0` |
| `0x5F40CF` | `sub_5F4050` |
| `0x5F4126` | `sub_5F4050` |
| `0x5F4262` | `sub_5F41F0` |
| `0x5F4322` | `sub_5F41F0` |
| `0x5F4517` | `sub_5F4500` |
| `0x5F78F1` | `sub_5F7850` |
| `0x5F7954` | `sub_5F7920` |
| `0x5F8511` | `sub_5F8390` |
| `0x5FA453` | `CSessionP2pGrantOwnerHandle::vt[2]` |
| `0x5FA475` | `CSessionP2pGrantOwnerHandle::vt[2]` |
| `0x5FA5E9` | `CSessionP2pGrantOwnerHandle::vt[2]` |
| `0x5FA5F9` | `CSessionP2pGrantOwnerHandle::vt[2]` |
| `0x5FA62B` | `CSessionP2pGrantOwnerHandle::vt[2]` |
| `0x5FA65D` | `CSessionP2pGrantOwnerHandle::vt[2]` |
| `0x5FA689` | `CSessionP2pGrantOwnerHandle::vt[2]` |
| `0x5FA6BD` | `CSessionP2pGrantOwnerHandle::vt[2]` |
| `0x5FA6E6` | `CSessionP2pGrantOwnerHandle::vt[2]` |
| `0x5FA98D` | `sub_5FA920` |
| `0x5FA99D` | `sub_5FA920` |
| `0x5FA9CF` | `sub_5FA920` |
| `0x5FAA01` | `sub_5FA920` |
| `0x5FAA2D` | `sub_5FA920` |
| `0x5FAA60` | `sub_5FA920` |
| `0x5FAA89` | `sub_5FA920` |
| `0x5FAE7C` | `sub_5FADE0` |
| `0x5FB687` | `CSessionP2pLockHandle::vt[2]` |
| `0x5FB890` | `CSessionP2pLockHandle::vt[2]` |
| `0x5FC1A3` | `sub_5FC0D0` |
| `0x5FDA1C` | `CSessionP2pPreparationHandle::vt[2]` |
| `0x5FDB69` | `CSessionP2pPreparationHandle::vt[2]` |
| `0x5FDB97` | `CSessionP2pPreparationHandle::vt[2]` |
| `0x5FDCB9` | `CSessionP2pPreparationHandle::vt[2]` |
| `0x5FDEC0` | `sub_5FDEB0` |
| `0x5FF668` | `sub_5FF570` |
| `0x5FF6FE` | `sub_5FF570` |
| `0x5FFD41` | `sub_5FF970` |
| `0x5FFE30` | `sub_5FF970` |
| `0x6012EF` | `sub_601130` |
| `0x6017D8` | `CRawSocketSteam::vt[4]` |
| `0x6018D7` | `CRawSocketSteam::vt[5]` |
| `0x601926` | `CRawSocketSteam::vt[5]` |
| `0x6038E3` | `CSessionJoinHandle::vt[2]` |
| `0x603A98` | `CSessionJoinHandle::vt[2]` |
| `0x603AF8` | `CSessionJoinHandle::vt[2]` |
| `0x603CC4` | `CSessionJoinHandle::vt[2]` |
| `0x603D58` | `CSessionJoinHandle::vt[2]` |
| `0x603EF4` | `sub_603EC0` |
| `0x6041F4` | `sub_604180` |
| `0x604252` | `sub_604180` |
| `0x604BB1` | `sub_604940` |
| `0x604BD4` | `sub_604940` |
| `0x604CDD` | `sub_604940` |
| `0x604FD0` | `sub_604F20` |
| `0x605017` | `sub_604F20` |
| `0x60503F` | `sub_604F20` |
| `0x60507A` | `sub_604F20` |
| `0x60508A` | `sub_604F20` |
| `0x6050BC` | `sub_604F20` |
| `0x6050ED` | `sub_604F20` |
| `0x605194` | `sub_604F20` |
| `0x6052B7` | `sub_604F20` |
| `0x6053B2` | `sub_604F20` |
| `0x605500` | `sub_604F20` |
| `0x60552D` | `sub_604F20` |
| `0x60580E` | `sub_6057B0` |
| `0x6058B9` | `sub_6057B0` |
| `0x6058FC` | `sub_6057B0` |
| `0x606ED1` | `sub_606EC0` |
| `0x606F01` | `sub_606EF0` |
| `0x606F6D` | `sub_606F20` |
| `0x606FE4` | `sub_606F20` |
| `0x60700F` | `sub_606F20` |
| `0x6070CC` | `sub_607080` |
| `0x607678` | `sub_607650` |
| `0x6076E7` | `sub_607650` |
| `0x607712` | `sub_607650` |
| `0x60774F` | `sub_607650` |
| `0x60800A` | `sub_607FF0` |
| `0x60804A` | `sub_608030` |
| `0x608162` | `sub_608070` |
| `0x608269` | `sub_608070` |
| `0x60829C` | `sub_608070` |
| `0x608D8D` | `CPacketReject::vt[2]` |
| `0x609FFE` | `sub_609EA0` |
| `0x60A0A7` | `sub_609EA0` |
| `0x60A142` | `sub_609EA0` |
| `0x60A199` | `sub_609EA0` |
| `0x60A1B1` | `sub_609EA0` |
| `0x60A1CD` | `sub_609EA0` |
| `0x60A4B7` | `sub_60A3B0` |
| `0x60A4DD` | `sub_60A3B0` |
| `0x60A505` | `sub_60A3B0` |
| `0x60A527` | `sub_60A3B0` |
| `0x60A5DE` | `sub_60A3B0` |
| `0x60A66C` | `sub_60A3B0` |
| `0x60CD49` | `sub_60CCC0` |
| `0x60CD96` | `sub_60CCC0` |
| `0x60CDB9` | `sub_60CCC0` |
| `0x616EC7` | `sub_616EB0` |
| `0x616F46` | `sub_616EB0` |
| `0x61756E` | `sub_617230` |
| `0x617B34` | `sub_617B20` |
| `0x617B4B` | `sub_617B20` |
| `0x617E8B` | `sub_617D60` |
| `0x617EC8` | `sub_617EB0` |
| `0x617F97` | `sub_617EB0` |
| `0x617FFB` | `sub_617FF0` |
| `0x618036` | `sub_618020` |
| `0x618070` | `sub_618060` |
| `0x6180CB` | `sub_6180C0` |
| `0x618537` | `sub_618490` |
| `0x6185FC` | `sub_618490` |
| `0x618706` | `sub_618660` |
| `0x618915` | `sub_6188B0` |
| `0x6189AA` | `sub_6188B0` |
| `0x61904D` | `sub_619010` |
| `0x6190E8` | `sub_6190A0` |
| `0x619242` | `sub_6191C0` |
| `0x619291` | `sub_6191C0` |
| `0x6192C7` | `sub_6191C0` |
| `0x619302` | `sub_6191C0` |
| `0x61958B` | `sub_619580` |
| `0x6195DB` | `sub_6195D0` |
| `0x619720` | `sub_6195F0` |
| `0x619AF6` | `sub_619AA0` |
| `0x619CC7` | `sub_619CB0` |
| `0x619D3F` | `sub_619D20` |
| `0x619DEC` | `sub_619DA0` |
| `0x619E75` | `sub_619DA0` |
| `0x619FB6` | `sub_619DA0` |
| `0x61A09A` | `sub_61A060` |
| `0x61A1BE` | `sub_61A180` |
| `0x61AC1C` | `sub_61ABD0` |
| `0x61ACCF` | `sub_61ABD0` |
| `0x61B0EF` | `sub_61B0C0` |
| `0x61B1F4` | `sub_61B0C0` |
| `0x61B280` | `sub_61B250` |
| `0x61B357` | `sub_61B2D0` |
| `0x61B408` | `sub_61B2D0` |
| `0x61B4DC` | `sub_61B4C0` |
| `0x61B555` | `sub_61B540` |
| `0x61B595` | `sub_61B580` |
| `0x61B5D5` | `sub_61B5C0` |
| `0xD58DFE` | `sub_D58DD0` |
| `0xD5C932` | `sub_D5C7D0` |
| `0xD5CBDC` | `sub_D5CBC0` |
| `0xD5CC9A` | `sub_D5CC40` |
| `0xD5CDA6` | `sub_D5CC40` |
| `0xD5D162` | `sub_D5CFF0` |
| `0xD5E58C` | `sub_D5E570` |
| `0xD67480` | `sub_D67350` |
| `0xD67657` | `sub_D67570` |
| `0xD68067` | `sub_D68040` |
| `0xD68C48` | `sub_D68BF0` |
| `0xD68CCE` | `sub_D68BF0` |
| `0xD68D17` | `sub_D68BF0` |
| `0xD707AC` | `sub_D70790` |
| `0xD70831` | `sub_D70800` |
| `0xD709E7` | `sub_D709D0` |
| `0xD70A58` | `sub_D70A30` |
| `0xD71FDC` | `sub_D71D80` |
| `0xD73FAE` | `sub_D73ED0` |
| `0xE68AD0` | `sub_E68AB0` |
| `0xE68AFD` | `sub_E68AB0` |
| `0xE693E2` | `sub_E693C0` |
| `0xE693F5` | `sub_E693C0` |
| `0xE69417` | `sub_E693C0` |
| `0xE6943A` | `sub_E693C0` |
| `0xE6A9E3` | `sub_E6A960` |
| `0xE6A9F6` | `sub_E6A960` |
| `0xE6ACE5` | `sub_E6ABE0` |
| `0xE6ACF7` | `sub_E6ABE0` |
| `0xE6B482` | `sub_E6B460` |
| `0xE6B495` | `sub_E6B460` |
| `0xE6B4B7` | `sub_E6B460` |
| `0xE6B4DA` | `sub_E6B460` |
| `0xE6B523` | `sub_E6B460` |
| `0xE6B541` | `sub_E6B460` |
| `0xE74B00` | `sub_E74AF0` |
| `0xE74B13` | `sub_E74AF0` |
| `0xE74CBA` | `sub_E74C20` |
| `0xE74DC0` | `sub_E74DB0` |
| `0xE74DD3` | `sub_E74DB0` |
| `0xE74DE6` | `sub_E74DB0` |
| `0xE74E07` | `sub_E74DB0` |
| `0xE74EE2` | `sub_E74E90` |
| `0xE74EFB` | `sub_E74E90` |
| `0xE74F40` | `sub_E74E90` |
| `0xE74F59` | `sub_E74E90` |
| `0xE74FAB` | `sub_E74FA0` |
| `0xE74FC2` | `sub_E74FA0` |
| `0xE74FE1` | `sub_E74FA0` |
| `0xE74FF4` | `sub_E74FA0` |
| `0xE75007` | `sub_E74FA0` |
| `0xE75028` | `sub_E74FA0` |
| `0xE7508F` | `sub_E75050` |
| `0xE750A8` | `sub_E75050` |
| `0xE76065` | `sub_E75F70` |
| `0xE76DC3` | `?$CCallResult@VCEGSEOS@EGS@@UEncryptedAppTicketResponse_t@@::vt[2]` |
| `0xE76DF4` | `?$CCallResult@VCEGSEOS@EGS@@UEncryptedAppTicketResponse_t@@::vt[2]` |
| `0xE76E79` | `sub_E76E50` |
| `0xE775CA` | `sub_E77510` |
| `0xE7BB7B` | `sub_E7BB70` |
| `0xE7BB8E` | `sub_E7BB70` |
| `0xE7BBD4` | `sub_E7BBB0` |
| `0xE7BBEB` | `sub_E7BBB0` |
| `0xE7BC02` | `sub_E7BBB0` |
| `0xE7BC2F` | `sub_E7BBB0` |
| `0xE7BC6B` | `sub_E7BBB0` |
| `0xE7BC7E` | `sub_E7BBB0` |
| `0xE7BC9A` | `sub_E7BBB0` |
| `0xE7BCDB` | `sub_E7BBB0` |
| `0xE7BCF5` | `sub_E7BBB0` |
| `0xE7BD37` | `sub_E7BBB0` |
| `0xE7BD7B` | `sub_E7BBB0` |
| `0xE7BDAB` | `sub_E7BDA0` |
| `0xE7BDDB` | `sub_E7BDD0` |
| `0xE7BDFB` | `sub_E7BDF0` |
| `0xE7BE3F` | `sub_E7BE30` |
| `0xE7BE52` | `sub_E7BE30` |
| `0xE7BE6E` | `sub_E7BE30` |
| `0xE7C087` | `sub_E7C060` |
| `0xE7C3A3` | `sub_E7C380` |
| `0xE7C41F` | `sub_E7C3F0` |
| `0xE7C4AB` | `sub_E7C490` |
| `0xE7C51B` | `sub_E7C510` |
| `0xE7C52E` | `sub_E7C510` |
| `0xE7C541` | `sub_E7C510` |
| `0xE7C615` | `sub_E7C570` |
| `0xE7C634` | `sub_E7C570` |
| `0xE7C7CB` | `sub_E7C7B0` |
| `0xE7C7E2` | `sub_E7C7B0` |
| `0xE7C7F9` | `sub_E7C7B0` |
| `0xE7C813` | `sub_E7C7B0` |
| `0xE7C8B0` | `sub_E7C8A0` |
| `0xE7C8C3` | `sub_E7C8A0` |
| `0xE7C8D6` | `sub_E7C8A0` |
| `0xE7C8FB` | `sub_E7C8A0` |
| `0xE7C9EC` | `sub_E7C9A0` |
| `0xE7CA13` | `sub_E7C9A0` |
| `0xE7CA32` | `sub_E7C9A0` |
| `0xE7CA52` | `sub_E7C9A0` |
| `0xE7CA7D` | `sub_E7C9A0` |
| `0xE7CAE3` | `sub_E7CAC0` |
| `0xE7CB64` | `sub_E7CB30` |
| `0xE7CC23` | `sub_E7CC10` |
| `0xE7CC42` | `sub_E7CC10` |
| `0xE7CC55` | `sub_E7CC10` |
| `0xE7CC68` | `sub_E7CC10` |
| `0xE7CC93` | `sub_E7CC10` |
| `0xE7CD4B` | `sub_E7CD40` |
| `0xE7CD5E` | `sub_E7CD40` |
| `0xE7CD71` | `sub_E7CD40` |
| `0xE7CD9C` | `sub_E7CD40` |
| `0xE7CE1D` | `sub_E7CDC0` |
| `0xE7CE44` | `sub_E7CDC0` |
| `0xE7CE63` | `sub_E7CDC0` |
| `0xE7CE83` | `sub_E7CDC0` |
| `0xE7CEAE` | `sub_E7CDC0` |
| `0xE7CF3C` | `sub_E7CEF0` |
| `0xE7CF63` | `sub_E7CEF0` |
| `0xE7CF82` | `sub_E7CEF0` |
| `0xE7CFA2` | `sub_E7CEF0` |
| `0xE7CFCD` | `sub_E7CEF0` |
| `0xFD1B17` | `SaveIcon@Menu::vt[10]` |

---

## 2. Structural finding: the flat `SteamAPI_ISteam*` API is **not imported**

Explicit absence: the binary imports **no** `SteamAPI_ISteamFriends_*`, `SteamAPI_ISteamNetworking*`, `SteamAPI_ISteamMatchmaking_*`, `SteamAPI_ISteamUser_*`, `SteamAPI_ISteamUtils_*`, `SteamAPI_ISteamUserStats_*`, `SteamAPI_ISteamController_*` or `SteamAPI_ISteamApps_*` symbols. The 11 imports in section 1 are the entire steam surface.

Instead the game obtains **interface pointers** and calls methods through the interface vtable:

* `SteamInternal_FindOrCreateUserInterface(HSteamUser, "SteamXxxxNNN")` — 13 sites, all of which are the *init thunks* of the cached-interface contexts (below).
* `SteamInternal_ContextInit(&ctx)` — 306 sites. This is the SDK's lazily-initialised interface cache. Each context is a 24-byte record `{initFn, hSteamUser, cached}` whose `initFn` calls `GetHSteamUser` + `FindOrCreateUserInterface` with one fixed version string.

### 2.1 Interface context map

| context global | interface version string | init thunk (RVA) | standalone accessor (RVA) | `ContextInit` sites |
|---|---|---|---|---|
| `0x001417A47E0` | `SteamController008` | `0xE6B560` | — | 15 |
| `0x001417A6838` | `STEAMAPPS_INTERFACE_VERSION008` | `0xE7BEC0` | — | 0 |
| `0x001417FBC58` | `SteamMatchMaking009` | `0x5EDC80` | `0x5EDCB0` | 73 |
| `0x001417FBC70` | `SteamNetworking006` | `0x5EDCD0` | `0x5EDD00` | 5 |
| `0x001417FBC88` | `SteamNetworkingSockets009` | `0x5EDD20` | `0x5EDD50` | 3 |
| `0x001417FBCA0` | `SteamFriends017` | `0x5EDBE0` | `0x5EDC10` | 27 |
| `0x001417FBCB8` | `SteamUser021` | `0x5EDB90` | `0x5EDBC0` | 44 |
| `0x001417FBCD0` | `SteamNetworkingUtils003` | `0x5EDD90` | `0x5EDDE0` | 14 |
| `0x001417FBCE8` | `SteamUtils010` | `0x5EDC30` | `0x5EDC60` | 13 |
| `0x001417FBD00` | `STEAMUSERSTATS_INTERFACE_VERSION012` | `0x6195A0` | `0x6195D0` | 0 |
| `0x001417FBD18` | `STEAMREMOTESTORAGE_INTERFACE_VERSION014` | `0x619550` | `0x619580` | 0 |

Interface version strings present in the image (only these 8 `Steam*0NN` names exist):
`SteamController008`, `SteamFriends017`, `SteamUser021`, `SteamMatchMaking009`, `SteamUtils010`, `SteamNetworking006`, `SteamNetworkingUtils003`, `SteamNetworkingSockets009`, plus `STEAMAPPS_INTERFACE_VERSION008`, `STEAMUSERSTATS_INTERFACE_VERSION012`, `STEAMREMOTESTORAGE_INTERFACE_VERSION014`.

So the game targets the **2017-era SDK** interface set: `SteamMatchMaking009` (vtable index 14 = `JoinLobby`, index 28 = `RequestLobbyData`), `SteamFriends017`, `SteamUser021`, `SteamNetworking006`.

### 2.2 How vtable offsets were resolved (authoritative, not from memory)

`steam_api64.dll` is present on disk next to the game (`~/.local/share/Steam/steamapps/common/Nioh/steam_api64.dll`, 265,504 bytes, **not** encrypted). Its 877 `SteamAPI_ISteam*` exports are 16-byte stubs of the form `mov rax,[rcx]; jmp *OFF(%rax)`; disassembling each export gives the vtable byte-offset of every method, i.e. `index = OFF/8`. Example:

```
SteamAPI_ISteamMatchmaking_JoinLobby            -> 0x70  (index 14)
SteamAPI_ISteamMatchmaking_GetLobbyData         -> 0x98  (index 19)
SteamAPI_ISteamMatchmaking_SetLobbyData         -> 0xa0  (index 20)
SteamAPI_ISteamMatchmaking_RequestLobbyData     -> 0xe0  (index 28)
SteamAPI_ISteamMatchmaking_RequestLobbyList     -> 0x20  (index  4)
SteamAPI_ISteamMatchmaking_GetLobbyByIndex      -> 0x60  (index 12)
SteamAPI_ISteamFriends_GetFriendGamePlayed      -> 0x40  (index  8)
SteamAPI_ISteamFriends_GetFriendRichPresence    -> 0x168 (index 45)
```

---

## 3. Ground-truth cross-check (step 3 of the ticket)

The live mod's backtrace names `nioh.exe+0x5edc9e` and `nioh.exe+0x5ef800` as the places where the matchmaking interface is fetched. Result:

* **`0x5EDC9E` — confirmed, exactly as the ticket describes.** It is the instruction *after* the `call` at `0x5EDC98` that references `SteamInternal_FindOrCreateUserInterface` (slot `0x14119BC80`), and the `rdx` argument is `lea 0xf98d50(%rip),%rdx  # 0x1415869E8` = the string **`SteamMatchMaking009`**. This is the init thunk at `0x5EDC80`.
* **`0x5EF800` — confirmed as a matchmaking-interface fetch, but via the cached path, not via a direct import call.** `0x5EF800` is the instruction after the `call` at `0x5EF7FA` to `SteamInternal_ContextInit` (slot `0x14119BCC8`) with `rcx = 0x1417FBC58` — the `SteamMatchMaking009` context (its init thunk is `0x5EDC80`, which requests `SteamMatchMaking009`). The very next instructions dereference the cached pointer and tail-call vtable `+0xE0` (= `RequestLobbyData`).

No scanner change was needed: the scanner finds both, because both are real references to steam_api64 import slots (`0x14119BC80` and `0x14119BCC8`). The only correction to the ticket's wording is that `0x5EF800` is a `ContextInit` site (the SDK's cached-interface initialiser) rather than a `FindOrCreateUserInterface` call site; both fetch the matchmaking interface.

Anchors verified against the ticket's own evidence: the mod's `call *0xbadfe1(%rip) # 0x14119bcc0` at `VA 0x1405edcd0` is `SteamAPI_GetHSteamUser`; the disassembly at that VA is `push rbx; sub rsp,0x20; mov rbx,rcx; call *...GetHSteamUser` — i.e. RVAs and the IAT region line up.

---

## 4. Interface-method call sites (the real Steam API usage)

Each row is a call/jmp through an interface vtable, found by taking every interface *acquisition* point (306 inline `ContextInit` sites + 15 calls to the standalone accessors of section 2.1), tracking the register that receives the interface pointer (register-name normalised, caller-saved registers clobbered across calls), then reading the `call/jmp *OFF(reg)` whose base holds the loaded vtable. 232 sites survived.

Legend: `idx` is the vtable index, `off` the byte offset, `kind` `call`/`jmp` (a `jmp` is a tail-call thunk).

### 4.1 `Apps` — 7 sites

| idx | off | method | kind | site RVA | enclosing function |
|---|---|---|---|---|---|
| 6 | `0x030` | `BIsSubscribedApp` | call | `0xE7BC8C` | `sub_E7BBB0` |
| 6 | `0x030` | `BIsSubscribedApp` | call | `0xE7BE60` | `sub_E7BE30` |
| 7 | `0x038` | `BIsDlcInstalled` | call | `0xE7BCA8` | `sub_E7BBB0` |
| 7 | `0x038` | `BIsDlcInstalled` | call | `0xE7BE7C` | `sub_E7BE30` |
| 10 | `0x050` | `GetDLCCount` | call | `0xE7BC0E` | `sub_E7BBB0` |
| 10 | `0x050` | `GetDLCCount` | call | `0xE7BD01` | `sub_E7BBB0` |
| 11 | `0x058` | `BGetDLCDataByIndex` | call | `0xE7BC59` | `sub_E7BBB0` |

### 4.2 `Controller` — 10 sites

| idx | off | method | kind | site RVA | enclosing function |
|---|---|---|---|---|---|
| 0 | `0x000` | `Init` | call | `0xE6AA02` | `sub_E6A960` |
| 1 | `0x008` | `Shutdown` | call | `0xE6AD03` | `sub_E6ABE0` |
| 3 | `0x018` | `GetConnectedControllers` | call | `0xE68B0E` | `sub_E68AB0` |
| 3 | `0x018` | `GetConnectedControllers` | call | `0xE69409` | `sub_E693C0` |
| 3 | `0x018` | `GetConnectedControllers` | call | `0xE6944B` | `sub_E693C0` |
| 3 | `0x018` | `GetConnectedControllers` | call | `0xE6B4A9` | `sub_E6B460` |
| 3 | `0x018` | `GetConnectedControllers` | call | `0xE6B4EB` | `sub_E6B460` |
| 27 | `0x0D8` | `GetControllerForGamepadIndex` | call | `0xE69425` | `sub_E693C0` |
| 27 | `0x0D8` | `GetControllerForGamepadIndex` | call | `0xE6B4C5` | `sub_E6B460` |
| 27 | `0x0D8` | `GetControllerForGamepadIndex` | call | `0xE6B531` | `sub_E6B460` |

### 4.3 `Friends` — 27 sites

| idx | off | method | kind | site RVA | enclosing function |
|---|---|---|---|---|---|
| 0 | `0x000` | `GetPersonaName` | call | `0x5E9390` | `sub_5E92C0` |
| 0 | `0x000` | `GetPersonaName` | call | `0x5F4523` | `sub_5F4500` |
| 0 | `0x000` | `GetPersonaName` | call | `0x5FA60B` | `CSessionP2pGrantOwnerHandle::vt[2]` |
| 0 | `0x000` | `GetPersonaName` | call | `0x5FA9AF` | `sub_5FA920` |
| 0 | `0x000` | `GetPersonaName` | call | `0x604BE0` | `sub_604940` |
| 0 | `0x000` | `GetPersonaName` | call | `0x60509C` | `sub_604F20` |
| 0 | `0x000` | `GetPersonaName` | call | `0x60CDC5` | `sub_60CCC0` |
| 3 | `0x018` | `GetFriendCount` | jmp | `0x617EA0` | `sub_617D60` |
| 3 | `0x018` | `GetFriendCount` | jmp | `0xD5CBF6` | `sub_D5CBC0` |
| 3 | `0x018` | `GetFriendCount` | call | `0xD71FFA` | `sub_D71D80` |
| 4 | `0x020` | `GetFriendByIndex` | call | `0x617FB1` | `sub_617EB0` |
| 4 | `0x020` | `GetFriendByIndex` | call | `0xD72025` | `sub_D71D80` |
| 7 | `0x038` | `GetFriendPersonaName` | call | `0xE7BD4B` | `sub_E7BBB0` |
| 17 | `0x088` | `HasFriend` | call | `0xD5C966` | `sub_D5C7D0` |
| 17 | `0x088` | `HasFriend` | call | `0xD5D196` | `sub_D5CFF0` |
| 17 | `0x088` | `HasFriend` | call | `0xD5E5B8` | `sub_D5E570` |
| 32 | `0x100` | `SetPlayedWith` | jmp | `0x618084` | `sub_618060` |
| 32 | `0x100` | `SetPlayedWith` | call | `0xD73FCF` | `sub_D73ED0` |
| 33 | `0x108` | `ActivateGameOverlayInviteDialog` | call | `0x5ED00E` | `sub_5ECC50` |
| 33 | `0x108` | `ActivateGameOverlayInviteDialog` | call | `0x5F0226` | `sub_5F0180` |
| 33 | `0x108` | `ActivateGameOverlayInviteDialog` | call | `0x6180E5` | `sub_6180C0` |
| 37 | `0x128` | `RequestUserInformation` | call | `0xE7C0BA` | `sub_E7C060` |
| 43 | `0x158` | `SetRichPresence` | call | `0xD70A73` | `sub_D70A30` |
| 44 | `0x160` | `ClearRichPresence` | call | `0xD707BD` | `sub_D70790` |
| 45 | `0x168` | `GetFriendRichPresence` | call | `0xD70862` | `sub_D70800` |
| 50 | `0x190` | `GetCoplayFriendCount` | jmp | `0x61800B` | `sub_617FF0` |
| 51 | `0x198` | `GetCoplayFriend` | call | `0x618048` | `sub_618020` |

### 4.4 `Matchmaking` — 71 sites

| idx | off | method | kind | site RVA | enclosing function |
|---|---|---|---|---|---|
| 4 | `0x020` | `RequestLobbyList` | call | `0x60A1D9` | `sub_609EA0` |
| 5 | `0x028` | `AddRequestLobbyListStringFilter` | call | `0x60A0BE` | `sub_609EA0` |
| 5 | `0x028` | `AddRequestLobbyListStringFilter` | call | `0x60A15F` | `sub_609EA0` |
| 6 | `0x030` | `AddRequestLobbyListNumericalFilter` | call | `0x60A016` | `sub_609EA0` |
| 9 | `0x048` | `AddRequestLobbyListDistanceFilter` | call | `0x60A1A7` | `sub_609EA0` |
| 10 | `0x050` | `AddRequestLobbyListResultCountFilter` | call | `0x60A1C3` | `sub_609EA0` |
| 12 | `0x060` | `GetLobbyByIndex` | call | `0x60A4CB` | `sub_60A3B0` |
| 13 | `0x068` | `CreateLobby` | call | `0x604CFA` | `sub_604940` |
| 14 | `0x070` | `JoinLobby` | call | `0x6038F2` | `CSessionJoinHandle::vt[2]` |
| 14 | `0x070` | `JoinLobby` | call | `0x603F03` | `sub_603EC0` |
| 15 | `0x078` | `LeaveLobby` | call | `0x6012FF` | `sub_601130` |
| 15 | `0x078` | `LeaveLobby` | call | `0x603AA8` | `CSessionJoinHandle::vt[2]` |
| 15 | `0x078` | `LeaveLobby` | call | `0x603B08` | `CSessionJoinHandle::vt[2]` |
| 15 | `0x078` | `LeaveLobby` | call | `0x604204` | `sub_604180` |
| 15 | `0x078` | `LeaveLobby` | call | `0x604262` | `sub_604180` |
| 16 | `0x080` | `InviteUserToLobby` | call | `0x5ED072` | `sub_5ECC50` |
| 16 | `0x080` | `InviteUserToLobby` | call | `0x5F0292` | `sub_5F0180` |
| 17 | `0x088` | `GetNumLobbyMembers` | jmp | `0x5E9FF6` | `sub_5E9EB0` |
| 17 | `0x088` | `GetNumLobbyMembers` | jmp | `0x5F4336` | `sub_5F41F0` |
| 19 | `0x098` | `GetLobbyData` | call | `0x5E9DC7` | `sub_5E9D20` |
| 19 | `0x098` | `GetLobbyData` | call | `0x5E9E1C` | `sub_5E9D20` |
| 19 | `0x098` | `GetLobbyData` | call | `0x5F0422` | `sub_5F0310` |
| 19 | `0x098` | `GetLobbyData` | call | `0x5F2142` | `sub_5F2010` |
| 19 | `0x098` | `GetLobbyData` | call | `0x5F21CF` | `sub_5F2010` |
| 19 | `0x098` | `GetLobbyData` | call | `0x5F22FE` | `sub_5F2010` |
| 19 | `0x098` | `GetLobbyData` | call | `0x5F40E5` | `sub_5F4050` |
| 19 | `0x098` | `GetLobbyData` | call | `0x5F413C` | `sub_5F4050` |
| 19 | `0x098` | `GetLobbyData` | call | `0x5FF67D` | `sub_5FF570` |
| 19 | `0x098` | `GetLobbyData` | call | `0x5FF713` | `sub_5FF570` |
| 19 | `0x098` | `GetLobbyData` | call | `0x603D6E` | `CSessionJoinHandle::vt[2]` |
| 19 | `0x098` | `GetLobbyData` | call | `0x605824` | `sub_6057B0` |
| 19 | `0x098` | `GetLobbyData` | call | `0x60590F` | `sub_6057B0` |
| 19 | `0x098` | `GetLobbyData` | call | `0x60A4F5` | `sub_60A3B0` |
| 19 | `0x098` | `GetLobbyData` | call | `0x60A5F6` | `sub_60A3B0` |
| 19 | `0x098` | `GetLobbyData` | call | `0x60A684` | `sub_60A3B0` |
| 20 | `0x0A0` | `SetLobbyData` | call | `0x5FA61E` | `CSessionP2pGrantOwnerHandle::vt[2]` |
| 20 | `0x0A0` | `SetLobbyData` | call | `0x5FA67C` | `CSessionP2pGrantOwnerHandle::vt[2]` |
| 20 | `0x0A0` | `SetLobbyData` | call | `0x5FA705` | `CSessionP2pGrantOwnerHandle::vt[2]` |
| 20 | `0x0A0` | `SetLobbyData` | call | `0x5FA9C2` | `sub_5FA920` |
| 20 | `0x0A0` | `SetLobbyData` | call | `0x5FAA20` | `sub_5FA920` |
| 20 | `0x0A0` | `SetLobbyData` | call | `0x5FAAA8` | `sub_5FA920` |
| 20 | `0x0A0` | `SetLobbyData` | call | `0x5FB6B7` | `CSessionP2pLockHandle::vt[2]` |
| 20 | `0x0A0` | `SetLobbyData` | call | `0x5FB8C0` | `CSessionP2pLockHandle::vt[2]` |
| 20 | `0x0A0` | `SetLobbyData` | call | `0x5FFD5A` | `sub_5FF970` |
| 20 | `0x0A0` | `SetLobbyData` | call | `0x5FFE4C` | `sub_5FF970` |
| 20 | `0x0A0` | `SetLobbyData` | call | `0x60505D` | `sub_604F20` |
| 20 | `0x0A0` | `SetLobbyData` | call | `0x6050AF` | `sub_604F20` |
| 20 | `0x0A0` | `SetLobbyData` | call | `0x60510B` | `sub_604F20` |
| 20 | `0x0A0` | `SetLobbyData` | call | `0x6051AE` | `sub_604F20` |
| 20 | `0x0A0` | `SetLobbyData` | call | `0x6052D5` | `sub_604F20` |
| 20 | `0x0A0` | `SetLobbyData` | call | `0x6053CC` | `sub_604F20` |
| 20 | `0x0A0` | `SetLobbyData` | call | `0x605520` | `sub_604F20` |
| 20 | `0x0A0` | `SetLobbyData` | call | `0x60554B` | `sub_604F20` |
| 26 | `0x0D0` | `SendLobbyChatMsg` | call | `0x5F852C` | `sub_5F8390` |
| 26 | `0x0D0` | `SendLobbyChatMsg` | call | `0x608DA8` | `CPacketReject::vt[2]` |
| 27 | `0x0D8` | `GetLobbyChatEntry` | call | `0x5FC1D7` | `sub_5FC0D0` |
| 28 | `0x0E0` | `RequestLobbyData` | call | `0x5EE343` | `sub_5EE200` |
| 28 | `0x0E0` | `RequestLobbyData` | call | `0x5EF157` | `CSessionP2pSteam::vt[2]` |
| 28 | `0x0E0` | `RequestLobbyData` | jmp | `0x5EF80E` | `sub_5EF7E0` |
| 28 | `0x0E0` | `RequestLobbyData` | call | `0x616F55` | `sub_616EB0` |
| 32 | `0x100` | `GetLobbyMemberLimit` | jmp | `0x5E9F36` | `sub_5E9EB0` |
| 32 | `0x100` | `GetLobbyMemberLimit` | jmp | `0x5F4276` | `sub_5F41F0` |
| 32 | `0x100` | `GetLobbyMemberLimit` | call | `0x603CD4` | `CSessionJoinHandle::vt[2]` |
| 35 | `0x118` | `GetLobbyOwner` | call | `0x5EF90C` | `sub_5EF820` |
| 35 | `0x118` | `GetLobbyOwner` | call | `0x5F235B` | `sub_5F2010` |
| 35 | `0x118` | `GetLobbyOwner` | call | `0x5F2477` | `sub_5F2010` |
| 35 | `0x118` | `GetLobbyOwner` | call | `0x5F28AC` | `sub_5F2640` |
| 35 | `0x118` | `GetLobbyOwner` | call | `0x5FA468` | `CSessionP2pGrantOwnerHandle::vt[2]` |
| 35 | `0x118` | `GetLobbyOwner` | call | `0x60CD5E` | `sub_60CCC0` |
| 36 | `0x120` | `SetLobbyOwner` | call | `0x5EB600` | `sub_5EB400` |
| 36 | `0x120` | `SetLobbyOwner` | call | `0x5FAE90` | `sub_5FADE0` |

### 4.5 `Networking` — 4 sites

| idx | off | method | kind | site RVA | enclosing function |
|---|---|---|---|---|---|
| 0 | `0x000` | `SendP2PPacket` | call | `0x6017FB` | `CRawSocketSteam::vt[4]` |
| 1 | `0x008` | `IsP2PPacketAvailable` | call | `0x5F015A` | `sub_5F00A0` |
| 1 | `0x008` | `IsP2PPacketAvailable` | call | `0x6018EB` | `CRawSocketSteam::vt[5]` |
| 2 | `0x010` | `ReadP2PPacket` | call | `0x601952` | `CRawSocketSteam::vt[5]` |

### 4.6 `NetworkingUtils` — 11 sites

| idx | off | method | kind | site RVA | enclosing function |
|---|---|---|---|---|---|
| 1 | `0x008` | `GetRelayNetworkStatus` | call | `0x5FDB7A` | `CSessionP2pPreparationHandle::vt[2]` |
| 2 | `0x010` | `GetLocalPingLocation` | call | `0x5FA69A` | `CSessionP2pGrantOwnerHandle::vt[2]` |
| 2 | `0x010` | `GetLocalPingLocation` | call | `0x5FAA3E` | `sub_5FA920` |
| 2 | `0x010` | `GetLocalPingLocation` | call | `0x5FDCCA` | `CSessionP2pPreparationHandle::vt[2]` |
| 2 | `0x010` | `GetLocalPingLocation` | call | `0x604FE1` | `sub_604F20` |
| 4 | `0x020` | `EstimatePingTimeFromLocalHost` | call | `0x60A537` | `sub_60A3B0` |
| 5 | `0x028` | `ConvertPingLocationToString` | call | `0x5FA6DC` | `CSessionP2pGrantOwnerHandle::vt[2]` |
| 5 | `0x028` | `ConvertPingLocationToString` | call | `0x5FAA7F` | `sub_5FA920` |
| 5 | `0x028` | `ConvertPingLocationToString` | call | `0x605035` | `sub_604F20` |
| 6 | `0x030` | `ParsePingLocationString` | call | `0x60A518` | `sub_60A3B0` |
| 7 | `0x038` | `CheckPingDataUpToDate` | call | `0x5FDBAB` | `CSessionP2pPreparationHandle::vt[2]` |

### 4.7 `User` — 45 sites

| idx | off | method | kind | site RVA | enclosing function |
|---|---|---|---|---|---|
| 1 | `0x008` | `BLoggedOn` | call | `0x5EECF2` | `CSessionP2pSteam::vt[2]` |
| 1 | `0x008` | `BLoggedOn` | call | `0x5FDA28` | `CSessionP2pPreparationHandle::vt[2]` |
| 1 | `0x008` | `BLoggedOn` | call | `0x616ED3` | `sub_616EB0` |
| 1 | `0x008` | `BLoggedOn` | call | `0x617B57` | `sub_617B20` |
| 1 | `0x008` | `BLoggedOn` | call | `0xE74DF2` | `sub_E74DB0` |
| 1 | `0x008` | `BLoggedOn` | call | `0xE75013` | `sub_E74FA0` |
| 1 | `0x008` | `BLoggedOn` | jmp | `0xE7BB9E` | `sub_E7BB70` |
| 1 | `0x008` | `BLoggedOn` | call | `0xE7C54D` | `sub_E7C510` |
| 1 | `0x008` | `BLoggedOn` | call | `0xE7C805` | `sub_E7C7B0` |
| 1 | `0x008` | `BLoggedOn` | call | `0xE7C8E2` | `sub_E7C8A0` |
| 1 | `0x008` | `BLoggedOn` | call | `0xE7CC74` | `sub_E7CC10` |
| 1 | `0x008` | `BLoggedOn` | call | `0xE7CD7D` | `sub_E7CD40` |
| 2 | `0x010` | `GetSteamID` | call | `0x5F2494` | `sub_5F2010` |
| 2 | `0x010` | `GetSteamID` | call | `0x5F28C9` | `sub_5F2640` |
| 2 | `0x010` | `GetSteamID` | call | `0x5FA486` | `CSessionP2pGrantOwnerHandle::vt[2]` |
| 2 | `0x010` | `GetSteamID` | call | `0x5FA63C` | `CSessionP2pGrantOwnerHandle::vt[2]` |
| 2 | `0x010` | `GetSteamID` | call | `0x5FA9E0` | `sub_5FA920` |
| 2 | `0x010` | `GetSteamID` | call | `0x604BC2` | `sub_604940` |
| 2 | `0x010` | `GetSteamID` | call | `0x6050CD` | `sub_604F20` |
| 2 | `0x010` | `GetSteamID` | call | `0x60CDA7` | `sub_60CCC0` |
| 2 | `0x010` | `GetSteamID` | call | `0xD579E1` | `sub_D57850` |
| 2 | `0x010` | `GetSteamID` | call | `0xD5C88B` | `sub_D5C7D0` |
| 2 | `0x010` | `GetSteamID` | call | `0xD5D0BB` | `sub_D5CFF0` |
| 2 | `0x010` | `GetSteamID` | call | `0xD5D299` | `sub_D5D280` |
| 2 | `0x010` | `GetSteamID` | call | `0xD5D83B` | `sub_D5D760` |
| 2 | `0x010` | `GetSteamID` | call | `0xD6510E` | `sub_D64BF0` |
| 2 | `0x010` | `GetSteamID` | call | `0xD7146E` | `sub_D71450` |
| 2 | `0x010` | `GetSteamID` | call | `0xD7FDAD` | `sub_D7FC10` |
| 2 | `0x010` | `GetSteamID` | call | `0xD7FF5C` | `sub_D7FC10` |
| 2 | `0x010` | `GetSteamID` | call | `0xD809B7` | `sub_D7FC10` |
| 2 | `0x010` | `GetSteamID` | call | `0xE704EC` | `sub_E70460` |
| 2 | `0x010` | `GetSteamID` | call | `0xFAD36A` | `sub_FAD340` |
| 7 | `0x038` | `StartVoiceRecording` | call | `0x5F78FD` | `sub_5F7850` |
| 7 | `0x038` | `StartVoiceRecording` | jmp | `0x606EE1` | `sub_606EC0` |
| 7 | `0x038` | `StartVoiceRecording` | call | `0x607684` | `sub_607650` |
| 7 | `0x038` | `StartVoiceRecording` | call | `0x60771E` | `sub_607650` |
| 7 | `0x038` | `StartVoiceRecording` | jmp | `0x60801A` | `sub_607FF0` |
| 8 | `0x040` | `StopVoiceRecording` | call | `0x5F7960` | `sub_5F7920` |
| 8 | `0x040` | `StopVoiceRecording` | jmp | `0x606F11` | `sub_606EF0` |
| 8 | `0x040` | `StopVoiceRecording` | call | `0x6076F3` | `sub_607650` |
| 8 | `0x040` | `StopVoiceRecording` | call | `0x60775B` | `sub_607650` |
| 8 | `0x040` | `StopVoiceRecording` | jmp | `0x60805A` | `sub_608030` |
| 10 | `0x050` | `GetVoice` | call | `0x607049` | `sub_606F20` |
| 10 | `0x050` | `GetVoice` | call | `0x6082D8` | `sub_608070` |
| 11 | `0x058` | `DecompressVoice` | call | `0x6070FF` | `sub_607080` |

### 4.8 `UserStats` — 47 sites

| idx | off | method | kind | site RVA | enclosing function |
|---|---|---|---|---|---|
| 1 | `0x008` | `GetStatFloat` | call | `0xE7C628` | `sub_E7C570` |
| 1 | `0x008` | `GetStatFloat` | call | `0xE7CA26` | `sub_E7C9A0` |
| 1 | `0x008` | `GetStatFloat` | call | `0xE7CE57` | `sub_E7CDC0` |
| 1 | `0x008` | `GetStatFloat` | call | `0xE7CF76` | `sub_E7CEF0` |
| 2 | `0x010` | `GetStatInt32` | call | `0xE7C647` | `sub_E7C570` |
| 3 | `0x018` | `SetStatFloat` | call | `0xE7CA46` | `sub_E7C9A0` |
| 3 | `0x018` | `SetStatFloat` | call | `0xE7CE77` | `sub_E7CDC0` |
| 3 | `0x018` | `SetStatFloat` | call | `0xE7CF96` | `sub_E7CEF0` |
| 4 | `0x020` | `SetStatInt32` | call | `0xE7CA66` | `sub_E7C9A0` |
| 4 | `0x020` | `SetStatInt32` | call | `0xE7CE97` | `sub_E7CDC0` |
| 4 | `0x020` | `SetStatInt32` | call | `0xE7CFB6` | `sub_E7CEF0` |
| 5 | `0x028` | `UpdateAvgRateStat` | call | `0xE7CA09` | `sub_E7C9A0` |
| 5 | `0x028` | `UpdateAvgRateStat` | call | `0xE7CE3A` | `sub_E7CDC0` |
| 5 | `0x028` | `UpdateAvgRateStat` | call | `0xE7CF59` | `sub_E7CEF0` |
| 6 | `0x030` | `GetAchievement` | call | `0xE74CCD` | `sub_E74C20` |
| 7 | `0x038` | `SetAchievement` | call | `0xE74F4F` | `sub_E74E90` |
| 10 | `0x050` | `StoreStats` | jmp | `0xE74F10` | `sub_E74E90` |
| 10 | `0x050` | `StoreStats` | jmp | `0xE74F6A` | `sub_E74E90` |
| 10 | `0x050` | `StoreStats` | jmp | `0xE750BD` | `sub_E75050` |
| 10 | `0x050` | `StoreStats` | call | `0xE7CA89` | `sub_E7C9A0` |
| 10 | `0x050` | `StoreStats` | call | `0xE7CEBA` | `sub_E7CDC0` |
| 10 | `0x050` | `StoreStats` | call | `0xE7CFD9` | `sub_E7CEF0` |
| 22 | `0x0B0` | `FindOrCreateLeaderboard` | call | `0x619CDF` | `sub_619CB0` |
| 22 | `0x0B0` | `FindOrCreateLeaderboard` | call | `0xD70D93` | `sub_D70D60` |
| 22 | `0x0B0` | `FindOrCreateLeaderboard` | call | `0xD70DC3` | `sub_D70D60` |
| 23 | `0x0B8` | `FindLeaderboard` | call | `0xD6749D` | `sub_D67350` |
| 23 | `0x0B8` | `FindLeaderboard` | call | `0xD67674` | `sub_D67570` |
| 24 | `0x0C0` | `GetLeaderboardName` | call | `0x61B4EB` | `sub_61B4C0` |
| 25 | `0x0C8` | `GetLeaderboardEntryCount` | call | `0x619311` | `sub_6191C0` |
| 25 | `0x0C8` | `GetLeaderboardEntryCount` | jmp | `0x61B569` | `sub_61B540` |
| 26 | `0x0D0` | `GetLeaderboardSortMethod` | call | `0x6192A0` | `sub_6191C0` |
| 26 | `0x0D0` | `GetLeaderboardSortMethod` | jmp | `0x61B5A9` | `sub_61B580` |
| 27 | `0x0D8` | `GetLeaderboardDisplayType` | call | `0x6192D6` | `sub_6191C0` |
| 27 | `0x0D8` | `GetLeaderboardDisplayType` | jmp | `0x61B5E9` | `sub_61B5C0` |
| 28 | `0x0E0` | `DownloadLeaderboardEntries` | call | `0x61B20F` | `sub_61B0C0` |
| 29 | `0x0E8` | `DownloadLeaderboardEntriesForUsers` | call | `0x619062` | `sub_619010` |
| 29 | `0x0E8` | `DownloadLeaderboardEntriesForUsers` | call | `0x61B295` | `sub_61B250` |
| 30 | `0x0F0` | `GetDownloadedLeaderboardEntry` | call | `0x61B38A` | `sub_61B2D0` |
| 30 | `0x0F0` | `GetDownloadedLeaderboardEntry` | call | `0xD680A1` | `sub_D68040` |
| 31 | `0x0F8` | `UploadLeaderboardScore` | call | `0x618933` | `sub_6188B0` |
| 31 | `0x0F8` | `UploadLeaderboardScore` | call | `0x61B116` | `sub_61B0C0` |
| 32 | `0x100` | `AttachLeaderboardUGC` | call | `0x619B08` | `sub_619AA0` |
| 38 | `0x130` | `RequestGlobalStats` | call | `0xE7C821` | `sub_E7C7B0` |
| 40 | `0x140` | `GetGlobalStatInt64` | jmp | `0xE7C3CD` | `sub_E7C380` |
| 40 | `0x140` | `GetGlobalStatInt64` | jmp | `0xE7CB0D` | `sub_E7CAC0` |
| 42 | `0x150` | `GetGlobalStatHistoryInt64` | jmp | `0xE7C451` | `sub_E7C3F0` |
| 42 | `0x150` | `GetGlobalStatHistoryInt64` | jmp | `0xE7CB96` | `sub_E7CB30` |

### 4.9 `Utils` — 10 sites

| idx | off | method | kind | site RVA | enclosing function |
|---|---|---|---|---|---|
| 3 | `0x018` | `GetServerRealTime` | call | `0xE7BD8C` | `sub_E7BBB0` |
| 4 | `0x020` | `GetIPCountry` | call | `0xD58E0F` | `sub_D58DD0` |
| 9 | `0x048` | `GetAppID` | call | `0xD5CDBC` | `sub_D5CC40` |
| 9 | `0x048` | `GetAppID` | call | `0xE74B1F` | `sub_E74AF0` |
| 9 | `0x048` | `GetAppID` | call | `0xE74FCE` | `sub_E74FA0` |
| 9 | `0x048` | `GetAppID` | call | `0xE7C4B7` | `sub_E7C490` |
| 9 | `0x048` | `GetAppID` | call | `0xE7CC2F` | `sub_E7CC10` |
| 17 | `0x088` | `IsOverlayEnabled` | call | `0x5ECFED` | `sub_5ECC50` |
| 17 | `0x088` | `IsOverlayEnabled` | call | `0x5F0205` | `sub_5F0180` |
| 23 | `0x0B8` | `GetSteamUILanguage` | jmp | `0xE7BDC0` | `sub_E7BDA0` |

---

## 5. Answers to the four questions

### 5a. `GetFriendGamePlayed` / `GetFriendRichPresence` / `GetFriendCount` / `GetFriendByIndex`

**`GetFriendGamePlayed` is NOT referenced anywhere.** `ISteamFriends017` index 8 (`+0x40`) has **zero** call sites: the game never asks Steam "is this friend currently playing Nioh". This is the single most important absence in this report — the co-op partner discovery does not filter by "friend owns/is playing this app".

Also unreferenced in `SteamFriends017`: `GetFriendPersonaNameHistory` (9), `GetFriendSteamLevel` (10), `GetFriendsGroup*` (12-16), `GetClanCount`/`GetClanTag`/`DownloadClanActivityCounts` (18/21/23), `GetFriendCountFromSource`/`GetFriendFromSourceByIndex`/`IsUserInSource` (24/25/26), `SetInGameVoiceSpeaking` (27), `GetFriendCoplayTime`/`GetFriendCoplayGame` (52/53), `JoinClanChatRoom`/`LeaveClanChatRoom` (54/55), the clan-chat block (56-65), `InviteUserToGame` (49), `GetFollowerCount`/`IsFollowing`/`EnumerateFollowingList` (67-69), `IsClanPublic`/`IsClanOfficialGameGroup` (70/71).

`GetFriendRichPresence` — 1 site(s):

| site RVA | kind | enclosing function |
|---|---|---|
| `0xD70862` | call | `sub_D70800` |

`GetFriendCount` — 3 site(s):

| site RVA | kind | enclosing function |
|---|---|---|
| `0x617EA0` | jmp | `sub_617D60` |
| `0xD5CBF6` | jmp | `sub_D5CBC0` |
| `0xD71FFA` | call | `sub_D71D80` |

`GetFriendByIndex` — 2 site(s):

| site RVA | kind | enclosing function |
|---|---|---|
| `0x617FB1` | call | `sub_617EB0` |
| `0xD72025` | call | `sub_D71D80` |

Reading of the friend sites: the `jmp`/`call` at `0x617EA0`/`0x617FB1` sit in the pair `sub_617D60` (a `GetFriendCount(k_EFriendFlagImmediate=4)` thunk) and `sub_617EB0`, which loops `GetFriendByIndex` and reports `0x80210003` when the immediate-friend list is empty — a friend-list enumerator used by the `ktol::online_service::eos` task layer. `sub_D71D80` enumerates with friend flags `0x600` and `sub_D5C7D0`/`sub_D5CFF0`/`sub_D5E570` call `HasFriend`; all of these live in the `0xD5xxxx`/`0xD7xxxx` platform-abstraction region, **not** in the matchmaking session code.

The rich-presence block (`SetRichPresence` `0xD70A73`, `ClearRichPresence` `0xD707BD`, `GetFriendRichPresence` `0xD70862`) is in `sub_D70A30` / `sub_D70790` / `sub_D70800` — a small "presence" wrapper: each first calls `0xD66C60`/`0xD68F10` (a guard) and then the `SteamFriends017` method with a key taken from a pointer table (`0x141790430`).

### 5b. Who calls `JoinLobby`, and who calls *that*

`JoinLobby` — 2 site(s):

| site RVA | kind | enclosing function |
|---|---|---|
| `0x6038F2` | call | `CSessionJoinHandle::vt[2]` |
| `0x603F03` | call | `sub_603EC0` |

`JoinLobby` (`ISteamMatchmaking009` index 14, `+0x70`) has exactly **two** call sites:

* `0x6038F2` inside `CSessionJoinHandle::vt[2]` (`0x603860`).
* `0x603F03` inside `sub_603EC0` (a second, near-identical join routine — same shape, different handle owner).

Both are preceded by the same logic (see section 6.1): fetch the singleton, read the target lobby id from `[svc+0x20]`, get the matchmaking interface, `JoinLobby(id)`, then register a `LobbyEnter_t` `CallResult` with the handler at `0x604310`.

**Second scanner pass over call targets** (linear `e8`/`e9 rel32` scan of `.text`): `0x603860` has **0 direct callers**, `0x603EC0` has **0 direct callers**, `0x6036D0` (`CSessionJoin::vt[1]`) has **0**, `0x5F6780` (`CSessionP2pOpen::vt[2]`) has **0**. The join path is therefore reached **virtually**; the concrete chain is:

```
CSessionP2pOpen::vt[2]  (0x5F6780)   writes CSessionJoin vtable 0x141587888 @ 0x5F69E8
   -> CSessionJoin::vt[1] (0x6036D0) writes CSessionJoinHandle vtable 0x1415878D0 @ 0x60372C
      -> CSessionJoinHandle::vt[2] (0x603860)  ->  JoinLobby()  @ 0x6038F2
```

`0x5F6780` also constructs a `CSessionCreate` (vtable `0x141587928` @ `0x5F69D2`) — i.e. the same "open session" operation covers both create and join. `CSessionP2pOpen`'s singleton (`0x1417F96C0`) is used by `sub_5F7070` (the code that asks the session service to run the open operation: `call *0x18(%rax)` with the operation object in `r8`).

### 5c. `RequestLobbyData` / `InviteUserToLobby` / `GetLobbyData` / `SetLobbyData` / `GetLobbyOwner`

`RequestLobbyData` — 4 site(s):

| site RVA | kind | enclosing function |
|---|---|---|
| `0x5EE343` | call | `sub_5EE200` |
| `0x5EF157` | call | `CSessionP2pSteam::vt[2]` |
| `0x5EF80E` | jmp | `sub_5EF7E0` |
| `0x616F55` | call | `sub_616EB0` |

`InviteUserToLobby` — 2 site(s):

| site RVA | kind | enclosing function |
|---|---|---|
| `0x5ED072` | call | `sub_5ECC50` |
| `0x5F0292` | call | `sub_5F0180` |

`GetLobbyData` — 16 site(s):

| site RVA | kind | enclosing function |
|---|---|---|
| `0x5E9DC7` | call | `sub_5E9D20` |
| `0x5E9E1C` | call | `sub_5E9D20` |
| `0x5F0422` | call | `sub_5F0310` |
| `0x5F2142` | call | `sub_5F2010` |
| `0x5F21CF` | call | `sub_5F2010` |
| `0x5F22FE` | call | `sub_5F2010` |
| `0x5F40E5` | call | `sub_5F4050` |
| `0x5F413C` | call | `sub_5F4050` |
| `0x5FF67D` | call | `sub_5FF570` |
| `0x5FF713` | call | `sub_5FF570` |
| `0x603D6E` | call | `CSessionJoinHandle::vt[2]` |
| `0x605824` | call | `sub_6057B0` |
| `0x60590F` | call | `sub_6057B0` |
| `0x60A4F5` | call | `sub_60A3B0` |
| `0x60A5F6` | call | `sub_60A3B0` |
| `0x60A684` | call | `sub_60A3B0` |

`SetLobbyData` — 18 site(s):

| site RVA | kind | enclosing function |
|---|---|---|
| `0x5FA61E` | call | `CSessionP2pGrantOwnerHandle::vt[2]` |
| `0x5FA67C` | call | `CSessionP2pGrantOwnerHandle::vt[2]` |
| `0x5FA705` | call | `CSessionP2pGrantOwnerHandle::vt[2]` |
| `0x5FA9C2` | call | `sub_5FA920` |
| `0x5FAA20` | call | `sub_5FA920` |
| `0x5FAAA8` | call | `sub_5FA920` |
| `0x5FB6B7` | call | `CSessionP2pLockHandle::vt[2]` |
| `0x5FB8C0` | call | `CSessionP2pLockHandle::vt[2]` |
| `0x5FFD5A` | call | `sub_5FF970` |
| `0x5FFE4C` | call | `sub_5FF970` |
| `0x60505D` | call | `sub_604F20` |
| `0x6050AF` | call | `sub_604F20` |
| `0x60510B` | call | `sub_604F20` |
| `0x6051AE` | call | `sub_604F20` |
| `0x6052D5` | call | `sub_604F20` |
| `0x6053CC` | call | `sub_604F20` |
| `0x605520` | call | `sub_604F20` |
| `0x60554B` | call | `sub_604F20` |

`GetLobbyOwner` — 6 site(s):

| site RVA | kind | enclosing function |
|---|---|---|
| `0x5EF90C` | call | `sub_5EF820` |
| `0x5F235B` | call | `sub_5F2010` |
| `0x5F2477` | call | `sub_5F2010` |
| `0x5F28AC` | call | `sub_5F2640` |
| `0x5FA468` | call | `CSessionP2pGrantOwnerHandle::vt[2]` |
| `0x60CD5E` | call | `sub_60CCC0` |

**Do these sites share functions with the `ktol_*` string handlers? Yes, extensively.** A RIP-relative scan for the 23 `ktol_*` string literals (table `0x141563DF0`-`0x1415643A8`, `0x141586A50`, `0x141586CC0`-`0x141587870`) found 69 references in 29 functions. Intersecting with the lobby-data call sites:

| function | `ktol_*` keys loaded | lobby-data Steam calls in the same function |
|---|---|---|
| `sub_5E9D20` | `ktol_owner_name` @`0x5E9DBD`, `ktol_owner_id` @`0x5E9E12` | `GetLobbyData` @`0x5E9DC7`, `0x5E9E1C` |
| `CSessionP2pGrantOwnerHandle::vt[2]` `0x5FA3E0` | `ktol_owner_name` @`0x5FA610`, `ktol_owner_id` @`0x5FA671`, `ktol_owner_ping_location` @`0x5FA6FA` | `SetLobbyData` x3 (`0x5FA61E`,`0x5FA67C`,`0x5FA705`), `GetLobbyOwner` @`0x5FA468`, `GetSteamID` @`0x5FA486`/`0x5FA63C` |
| `CSessionP2pLockHandle::vt[2]` `0x5FB560` | `ktol_session_lock`/`ktol_session_unlock`/`ktol_lock_status` @`0x5FB693..0x5FB6AC` and `0x5FB89C..0x5FB8B5` | `SetLobbyData` @`0x5FB6B7`, `0x5FB8C0` |
| `sub_604F20` | `ktol_owner_ping_location`, `ktol_owner_name`, `ktol_owner_id`, `ktol_invite_data`, `ktol_common_key`, `ktol_session_unlock`, `ktol_lock_status` (7 refs) | `SetLobbyData` x8, `GetLobbyData` x2, `GetSteamID`, `GetLocalPingLocation`, `ConvertPingLocationToString` |
| `sub_6057B0` | `ktol_owner_name` @`0x605817` | `GetLobbyData` @`0x605824`, `0x60590F` |
| `sub_5F4050` | `ktol_owner_name` @`0x5F40DB`, `ktol_owner_id` @`0x5F4132` | `GetLobbyData` @`0x5F40E5`, `0x5F413C` |
| `sub_5F2010` | `ktol_owner_id` @`0x5F22F3` | `GetLobbyData` x3, `GetLobbyOwner` x2, `GetSteamID` x2 |
| `sub_5F0310` | `ktol_invite_data` @`0x5F0418` | `GetLobbyData` @`0x5F0422` |
| `CSessionJoinHandle::vt[2]` `0x603860` | `ktol_common_key` @`0x603D64` | `JoinLobby` @`0x6038F2`, `LeaveLobby` x4, `GetLobbyData` @`0x603D6E`, `GetLobbyMemberLimit` @`0x603CD4` |
| `sub_609EA0` | `ktol_session_unlock` @`0x60A151`, `ktol_lock_status` @`0x60A158` | `RequestLobbyList` @`0x60A1D9` + all `AddRequestLobbyList*` |
| `sub_60A3B0` | `ktol_owner_ping_location` @`0x60A4E9` | `GetLobbyByIndex` @`0x60A4CB`, `GetLobbyData` x3 |

So the `ktol_*` vocabulary is written and read **exactly at** the `SetLobbyData`/`GetLobbyData` sites: the keys are the lobby-data keys, and the functions above are the ktol session-state machine. (No `SetLobbyData`/`GetLobbyData` site exists *outside* a function that also touches a `ktol_*` key, with the single exception of `sub_5F0310`'s sibling `CSessionP2pSteam::vt[2]`-driven `RequestLobbyData` sites.)

### 5d. Lobby-list APIs (`RequestLobbyList` / `GetLobbyByIndex` / `AddRequestLobbyList*`)

**They ARE referenced — this is not the "no" case.** But every single one of them lives in just two functions:

`RequestLobbyList` — 1 site(s):

| site RVA | kind | enclosing function |
|---|---|---|
| `0x60A1D9` | call | `sub_609EA0` |

`GetLobbyByIndex` — 1 site(s):

| site RVA | kind | enclosing function |
|---|---|---|
| `0x60A4CB` | call | `sub_60A3B0` |

`AddRequestLobbyListStringFilter` — 2 site(s):

| site RVA | kind | enclosing function |
|---|---|---|
| `0x60A0BE` | call | `sub_609EA0` |
| `0x60A15F` | call | `sub_609EA0` |

`AddRequestLobbyListNumericalFilter` — 1 site(s):

| site RVA | kind | enclosing function |
|---|---|---|
| `0x60A016` | call | `sub_609EA0` |

`AddRequestLobbyListDistanceFilter` — 1 site(s):

| site RVA | kind | enclosing function |
|---|---|---|
| `0x60A1A7` | call | `sub_609EA0` |

`AddRequestLobbyListResultCountFilter` — 1 site(s):

| site RVA | kind | enclosing function |
|---|---|---|
| `0x60A1C3` | call | `sub_609EA0` |

Zero references: `AddRequestLobbyListNearValueFilter` (7), `AddRequestLobbyListFilterSlotsAvailable` (8), `AddRequestLobbyListCompatibleMembersFilter` (11), `GetLobbyDataCount` (21), `GetLobbyDataByIndex` (22), `DeleteLobbyData` (23), `GetLobbyMemberData` (24), `SetLobbyMemberData` (25), `SetLobbyGameServer`/`GetLobbyGameServer` (29/30), `SetLobbyMemberLimit` (31), `SetLobbyType` (33), `SetLobbyJoinable` (34), `SetLinkedLobby` (37), `GetFavoriteGame`/`AddFavoriteGame`/`RemoveFavoriteGame` (1/2/3).

Conclusion: **the session search is lobby-list-driven, not friend-list-driven** — it issues `RequestLobbyList()` after adding filters (`AddRequestLobbyListStringFilter("ktol_lock_status", "ktol_session_unlock")`, a distance filter and a result-count filter) and then consumes the `LobbyMatchList_t` results with `GetLobbyByIndex` + `GetLobbyData` in the callback. The friend-list calls (`GetFriendCount`, `GetFriendByIndex`, `HasFriend`, `GetFriendRichPresence`) exist but serve the separate platform/presence layer described in 5a — they are not part of the matchmaking search. `GetFriendGamePlayed` is never used, so there is no "friend is playing this game" gate either.

---

## 6. Where the friend-session search and the join attempt live

### 6.1 Search path (lobby-list driven)

```
sub_5FEBD0 (called from 0x5E99FC)            ; asks the session service to run a search operation
  service->vt[3]( &CSessionP2pSearch_singleton(0x1417F9938), out )   @0x5FED8F
CSessionP2pSearch::vt[2]  (0x5FE8E0)         ; constructs a CSessionSearch (vtable 0x141587570 @ 0x5FE952)
  -> CSessionSearch::vt[1] (0x609BC0)        ; constructs a CSessionSearchHandle (vtable 0x141587D20 @ 0x609C1C)
     -> CSessionSearchHandle::vt[2] (0x609D50)
        -> sub_609EA0 (only caller: 0x609E1C) ; the lobby-list search worker
           AddRequestLobbyListStringFilter("ktol_lock_status","ktol_session_unlock") @0x60A15F
           AddRequestLobbyListDistanceFilter @0x60A1A7   AddRequestLobbyListResultCountFilter @0x60A1C3
           RequestLobbyList() @0x60A1D9   -> RegisterCallResult(handle, cb, 0x14060A3B0) @0x60A211
        -> sub_60A3B0 = the LobbyMatchList_t handler (registered at 0x60A211)
           GetLobbyByIndex(i) @0x60A4CB ; GetLobbyData(lobby,"ktol_owner_ping_location") @0x60A4F5/0x60A5F6/0x60A684
           ParsePingLocationString @0x60A518 ; EstimatePingTimeFromLocalHost @0x60A537
```

### 6.2 Join path

```
sub_5F7070 (uses CSessionP2pOpen singleton 0x1417F96C0 @0x5F755E)
  service->vt[3]( &CSessionP2pOpen_singleton, out ) @0x5F756D
CSessionP2pOpen::vt[2]  (0x5F6780)           ; constructs CSessionCreate @0x5F69D2 AND CSessionJoin @0x5F69E8
  -> CSessionJoin::vt[1] (0x6036D0)          ; constructs CSessionJoinHandle (vtable 0x1415878D0 @ 0x60372C)
     -> CSessionJoinHandle::vt[2] (0x603860)
        JoinLobby( [svc+0x20] ) @0x6038F2 ; RegisterCallResult(cb 0x140604310) @0x60392A
     -> sub_603EC0 : second JoinLobby @0x603F03 ; RegisterCallResult(cb 0x140604310) @0x603F3B
```

`SteamAPI_RunCallbacks` is pumped from `CSessionP2pSteam::vt[2]` (`0x5EECB0`), so the lobby callbacks only fire while the ktol session layer is ticking. Callback registration is heavy: 48 `RegisterCallback`, 128 `UnregisterCallback`, 33 `RegisterCallResult`, 107 `UnregisterCallResult` sites.

---

## 7. Disassembly excerpts (<= 6 lines each)

### 7.1 `JoinLobby` — `CSessionJoinHandle::vt[2]`, site `0x6038F2`

```
1406038d3: e8 e8 c6 fe ff        call   0x1405effc0          ; get session singleton
1406038d8: 48 8b 58 20           mov    0x20(%rax),%rbx      ; rbx = target lobby CSteamID
1406038dc: 48 8d 0d 75 83 1f 01  lea    0x1417fbc58,%rcx     ; matchmaking context
1406038e3: ff 15 df 83 b9 00     call   *SteamInternal_ContextInit
1406038e9: 48 8b 08              mov    (%rax),%rcx          ; rcx = ISteamMatchmaking*
1406038ec: 48 8b 01              mov    (%rcx),%rax          ; rax = vtable
1406038ef: 48 8b d3              mov    %rbx,%rdx            ; arg = lobby id
1406038f2: ff 50 70              call   *0x70(%rax)          ; JoinLobby(CSteamID)  <- index 14
```

Immediately before the call there is no comparison at all: the lobby id is taken verbatim from the session object (`[singleton+0x20]`) and passed in. What *is* tested right after is the returned `SteamAPICall_t`: `test %rsi,%rsi` decides whether a `LobbyEnter_t` `CallResult` (handler `0x140604310`) is registered, and the pre-existing call-result slot at `+0x30` is unregistered first (`call *SteamAPI_UnregisterCallResult` at `0x603905`).

### 7.2 `RequestLobbyList` — `sub_609EA0`, site `0x60A1D9`

```
14060a13b: lea    0x1417fbc58,%rcx        ; matchmaking context
14060a142: call   *SteamInternal_ContextInit
14060a148: mov    (%rax),%rcx ; mov (%rcx),%rax
14060a151: lea    0x141587260,%r8        ; "ktol_session_unlock"
14060a158: lea    0x141587248,%rdx       ; "ktol_lock_status"
14060a15f: call   *0x28(%rax)            ; AddRequestLobbyListStringFilter(key,value,0)  <- index 5
14060a1bd: mov    0x598(%rbx),%edx ; call *0x50(%rax)   ; AddRequestLobbyListResultCountFilter  <- index 10
14060a1d9: call   *0x20(%rax)            ; RequestLobbyList()  -> SteamAPICall_t  <- index 4
```

The immediate logic is: (1) if the search object's byte `+0x590` is clear, add the string filter `ktol_lock_status == ktol_session_unlock` (i.e. "only unlocked lobbies"); (2) map the enum at `+0x594` (`0..3`) into the `AddRequestLobbyListDistanceFilter` argument (`0..3`); (3) pass `[+0x598]` as the result-count limit; (4) `RequestLobbyList()`; (5) unregister the previous call-result slot, store the new handle at `+0x30`, store the handler `0x14060A3B0` at `+0x40` and `RegisterCallResult`; (6) arm a 15000 ms (`0x3A98`) timeout at `+0x4E0`.

### 7.3 `RequestLobbyData` — `sub_5EF7E0`, site `0x5EF80E` (the tail call reached from the backtrace frame `0x5EF800`)

```
1405ef7e6: mov    (%rdx),%rax          ; CSteamID from the caller's argument
1405ef7ec: mov    %rax,0x260(%rcx)     ; stash it on the session object
1405ef7f3: lea    0x1417fbc58,%rcx     ; matchmaking context
1405ef7fa: call   *SteamInternal_ContextInit     ; <- backtrace frame 0x5EF800
1405ef803: mov    (%rax),%rcx ; mov (%rcx),%rax
1405ef80e: jmp    *0xe0(%rax)          ; RequestLobbyData(CSteamID)  <- index 28
```

---

## 8. Absences (explicit)

* `GetFriendGamePlayed` — **0 references** (Friends idx 8 / `+0x40`).
* No `SteamAPI_ISteamFriends_*` / `SteamAPI_ISteamNetworking*` / `SteamAPI_ISteamMatchmaking_*` / `SteamAPI_ISteamUser_*` imports exist at all.
* `SteamAPI_ISteamMatchmaking_GetLobbyDataByIndex` (22), `DeleteLobbyData` (23), `GetLobbyMemberData` (24), `SetLobbyMemberData` (25), `GetLobbyGameServer`/`SetLobbyGameServer` (29/30), `SetLobbyMemberLimit` (31), `SetLobbyType` (33), `SetLobbyJoinable` (34), `SetLinkedLobby` (37), all three favourite-game methods (1/2/3), `AddRequestLobbyListNearValueFilter` (7), `AddRequestLobbyListFilterSlotsAvailable` (8), `AddRequestLobbyListCompatibleMembersFilter` (11) — 0 references.
* `SteamAPI_ISteamNetworking006` is only used for the legacy P2P path: `SendP2PPacket` (idx 0) 1 site @`0x6017FB`, `IsP2PPacketAvailable` (1) 2 sites, `ReadP2PPacket` (2) 1 site @`0x601952` — all in `CRawSocketSteam` (`0x601650`/`0x601870`). `AcceptP2PSessionWithUser` (3), `CloseP2PSessionWithUser` (4), `CloseP2PChannelWithUser` (5), `GetP2PSessionState` (6), `AllowP2PPacketRelay` (7), all socket methods (8-21) — **0 references**.
* `SteamNetworkingSockets009` is fetched (`ContextInit` x3, all inside the `0x5EDD50`/`0x5EDD70` accessor thunks) but **no vtable method of it is ever called** — 0 call sites. Same for `SteamNetworkingUtils003`'s socket-callback methods; only the ping-location/relay-status methods are used (`GetRelayNetworkStatus`, `GetLocalPingLocation`, `ConvertPingLocationToString`, `ParsePingLocationString`, `EstimatePingTimeFromLocalHost`, `CheckPingDataUpToDate`).
* `SteamAPI_Init` / `SteamAPI_Shutdown` / `SteamAPI_GetHSteamUser` are each called from the two small wrappers `0xE7BE10`/`0xE7BEB0`/`0xE7BEC0` (Steam bootstrap) and from the monolithic function at `0xFD1B00` (`SaveIcon@Menu::vt[10]`, i.e. an unnamed large `Menu` method that inlines its own Steam bootstrap).

---

## 9. Caveats

* `analysis/symbols.txt` / `analysis/session_symbols.txt` contain **names only, no RVAs**, so function naming here comes from `analysis/classmap.txt` (11,414 vtable-method addresses) plus a prologue scan (56,729 starts). Functions without an RTTI symbol are shown as `sub_XXXXXX`; each cited `sub_` start was verified to be preceded by `int3` padding.
* Register-state tracking is a static approximation: a register that is overwritten by an unmodelled instruction is dropped, which biases toward *missing* sites rather than inventing them. Two early false positives (`Matchmaking` index 1/`GetFavoriteGame` at `0x5F8591` and index 2 at `0x6050CD`) were removed after register-name normalisation (`ecx` -> `rcx`) and caller-saved clobber handling; they are reported here for transparency.
* Sites reached only through an interface pointer cached in a heap object (not freshly fetched via `ContextInit`) would be missed. The 232 sites found cover every `ContextInit`/accessor acquisition in the image, so any miss would be a second-order cache.
