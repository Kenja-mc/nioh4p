/*
 * The two matchmaking thunks of the fake steam_api64.dll, byte-for-byte the
 * instructions the shipped DLL uses (read out of the real binary):
 *
 *   SteamAPI_ISteamMatchmaking_CreateLobby:         mov rax,[rcx]
 *                                                   jmp qword ptr [rax+0x68]
 *   SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit: mov rax,[rcx]
 *                                                   jmp qword ptr [rax+0xf8]
 *
 * 0x68/8 = slot 13, 0xf8/8 = slot 31 of the object's vtable. Written as raw
 * bytes so the assembler cannot drop the redundant REX.W byte MSVC emits - the
 * proxy has to recognise exactly this shape.
 */
	.text
	.globl	SteamAPI_ISteamMatchmaking_CreateLobby
SteamAPI_ISteamMatchmaking_CreateLobby:
	.byte	0x48, 0x8b, 0x01		/* mov rax,[rcx]  */
	.byte	0x48, 0xff, 0x60, 0x68		/* jmp [rax+0x68] */

	.globl	SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit
SteamAPI_ISteamMatchmaking_SetLobbyMemberLimit:
	.byte	0x48, 0x8b, 0x01		/* mov rax,[rcx]  */
	.byte	0x48, 0xff, 0xa0, 0xf8, 0x00, 0x00, 0x00	/* jmp [rax+0xf8] */

	.globl	SteamAPI_ISteamMatchmaking_GetNumLobbyMembers
SteamAPI_ISteamMatchmaking_GetNumLobbyMembers:
	.byte	0x48, 0x8b, 0x01		/* mov rax,[rcx]  */
	.byte	0x48, 0xff, 0xa0, 0x88, 0x00, 0x00, 0x00	/* jmp [rax+0x88] -> slot 17 */

	.globl	SteamAPI_ISteamMatchmaking_GetLobbyOwner
SteamAPI_ISteamMatchmaking_GetLobbyOwner:
	.byte	0x48, 0x83, 0xec, 0x28			/* sub rsp,0x28 */
	.byte	0x48, 0x8b, 0x01			/* mov rax,[rcx] */
	.byte	0x4c, 0x8b, 0xc2			/* mov r8,rdx */
	.byte	0x48, 0x8d, 0x54, 0x24, 0x30		/* lea rdx,[rsp+0x30] */
	.byte	0xff, 0x90, 0x18, 0x01, 0x00, 0x00	/* call [rax+0x118]  -> slot 35 */
	.byte	0x48, 0x8b, 0x00			/* mov rax,[rax] */
	.byte	0x48, 0x83, 0xc4, 0x28			/* add rsp,0x28 */
	.byte	0xc3					/* ret */

	.globl	SteamAPI_ISteamMatchmaking_InviteUserToLobby
SteamAPI_ISteamMatchmaking_InviteUserToLobby:
	.byte	0x48, 0x8b, 0x01			/* mov rax,[rcx] */
	.byte	0x48, 0xff, 0xa0, 0x80, 0x00, 0x00, 0x00	/* jmp [rax+0x80]  -> slot 16 */

	.globl	SteamAPI_ISteamMatchmaking_SetLobbyMemberData
SteamAPI_ISteamMatchmaking_SetLobbyMemberData:
	.byte	0x48, 0x8b, 0x01			/* mov rax,[rcx] */
	.byte	0x48, 0xff, 0xa0, 0xc8, 0x00, 0x00, 0x00	/* jmp [rax+0xc8]  -> slot 25 */

	.globl	SteamAPI_ISteamFriends_GetFriendRichPresence
SteamAPI_ISteamFriends_GetFriendRichPresence:
	.byte	0x48, 0x8b, 0x01			/* mov rax,[rcx] */
	.byte	0x48, 0xff, 0xa0, 0x68, 0x01, 0x00, 0x00	/* jmp [rax+0x168] -> slot 45 */

	.globl	SteamAPI_ISteamMatchmaking_RequestLobbyList
SteamAPI_ISteamMatchmaking_RequestLobbyList:
	.byte	0x48, 0x8b, 0x01			/* mov rax,[rcx] */
	.byte	0x48, 0xff, 0x60, 0x20			/* jmp [rax+0x20]  -> slot 4 */

	.globl	SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter
SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter:
	.byte	0x48, 0x8b, 0x01			/* mov rax,[rcx] */
	.byte	0x48, 0xff, 0x60, 0x28			/* jmp [rax+0x28]  -> slot 5 */

	.globl	SteamAPI_ISteamMatchmaking_GetLobbyByIndex
SteamAPI_ISteamMatchmaking_GetLobbyByIndex:
	.byte	0x48, 0x83, 0xec, 0x28			/* sub rsp,0x28 */
	.byte	0x48, 0x8b, 0x01			/* mov rax,[rcx] */
	.byte	0x44, 0x8b, 0xc2			/* mov r8d,edx */
	.byte	0x48, 0x8d, 0x54, 0x24, 0x30		/* lea rdx,[rsp+0x30] */
	.byte	0xff, 0x50, 0x60			/* call [rax+0x60] -> slot 12 */
	.byte	0x48, 0x8b, 0x00			/* mov rax,[rax] */
	.byte	0x48, 0x83, 0xc4, 0x28			/* add rsp,0x28 */
	.byte	0xc3

	.globl	SteamAPI_ISteamMatchmaking_SetLobbyData
SteamAPI_ISteamMatchmaking_SetLobbyData:
	.byte	0x48, 0x8b, 0x01			/* mov rax,[rcx] */
	.byte	0x48, 0xff, 0xa0, 0xa0, 0x00, 0x00, 0x00	/* jmp [rax+0xa0]  -> slot 20 */

	.globl	SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable
SteamAPI_ISteamMatchmaking_AddRequestLobbyListFilterSlotsAvailable:
	.byte	0x48, 0x8b, 0x01			/* mov rax,[rcx] */
	.byte	0x48, 0xff, 0x60, 0x40			/* jmp [rax+0x40]  -> slot 8 */

	.globl	SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex
SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex:
	.byte	0x48, 0x83, 0xec, 0x28
	.byte	0x48, 0x8b, 0x01
	.byte	0x45, 0x8b, 0xc8			/* mov r9d,r8d */
	.byte	0x4c, 0x8b, 0xc2			/* mov r8,rdx */
	.byte	0x48, 0x8d, 0x54, 0x24, 0x30
	.byte	0xff, 0x90, 0x90, 0x00, 0x00, 0x00	/* call [rax+0x90] -> slot 18 */
	.byte	0x48, 0x8b, 0x00
	.byte	0x48, 0x83, 0xc4, 0x28
	.byte	0xc3

	.globl	SteamAPI_ISteamFriends_GetFriendByIndex
SteamAPI_ISteamFriends_GetFriendByIndex:
	.byte	0x48, 0x83, 0xec, 0x28
	.byte	0x48, 0x8b, 0x01
	.byte	0x45, 0x8b, 0xc8			/* mov r9d,r8d */
	.byte	0x44, 0x8b, 0xc2			/* mov r8d,edx */
	.byte	0x48, 0x8d, 0x54, 0x24, 0x30
	.byte	0xff, 0x50, 0x20			/* call [rax+0x20] -> slot 4 */
	.byte	0x48, 0x8b, 0x00
	.byte	0x48, 0x83, 0xc4, 0x28
	.byte	0xc3
