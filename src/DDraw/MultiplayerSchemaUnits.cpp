#include "MultiplayerSchemaUnits.h"
#include "tamem.h"
#include "iddrawsurface.h"
#include "hook/hook.h"
#include "tafunctions.h"
#include "StartPositions.h"
#include "BattleroomCommands.h"
#include "GameTickHook.h"

#include <set>
#include <string.h>
#include <string>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

static UnitStruct* DoSpawnUnit(PlayerStruct *targetPlayer, MissionUnitsStruct* missionUnit, int playerUnitNumberOverride = 0)
{
	TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;

	std::string unitName = missionUnit->Unitname;
	int unitInfoId = -1;
	for (int i = 0; i < taPtr->UNITINFOCount; ++i) {
		if (std::string(taPtr->UnitDef[i].UnitName) == unitName) {
			unitInfoId = i;
			break;
		}
	}

	{
		int missionUnitIndex = std::distance(taPtr->GameingState_Ptr->uniqueIdentifiers, missionUnit);
		const char* identity = missionUnit->Ident ? missionUnit->Ident : "<null>";
		const char* initialMission = missionUnit->InitialMission ? missionUnit->InitialMission : "<null>";
		IDDrawSurface::OutptFmtTxt("[DoSpawnUnit] unitNumber=%d unitInfoId=%d, player=%d unitName=%s identity=%s mission=%s",
			missionUnitIndex, unitInfoId, int(missionUnit->Player), missionUnit->Unitname, identity, initialMission);
	}

	if (unitInfoId < 0)
	{
		return NULL;
	}

	unsigned x = missionUnit->XPos;
	unsigned y = missionUnit->YPos;
	unsigned z = missionUnit->ZPos;
	unsigned idx = (x >> 20) + (z >> 20) * taPtr->FeatureMapSizeX;
	if (idx < taPtr->FeatureMapSizeX * taPtr->FeatureMapSizeY) {
		FeatureStruct* f = &taPtr->FeatureMap[idx];
		y = unsigned(f->height) << 16;
	}

	int unitNumber = playerUnitNumberOverride ? playerUnitNumberOverride + targetPlayer->UnitsIndex_Begin : 0;
	UnitStruct *newUnit = UNITS_CreateUnit(targetPlayer->PlayerAryIndex, unitInfoId, x, y, z, true, 1, unitNumber);

	return newUnit;
}

static void DoParseInitialMissionCommands(int iMissionUnit, MissionUnitsStruct* missionUnit, UnitStruct *spawnedUnit, UnitStruct *allSpawnedUnits[],
	const char* script = NULL)	// GG: a script to run instead of the entry's InitialMission
{
	TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;

	const char* mission = script ? script : missionUnit->InitialMission;
	if (mission && mission[0] != '\0')
	{
		struct
		{
			int iMissionUnit;
			UnitStruct** spawnedUnitsAry;
		}
		spawnedUnitsAry;
		spawnedUnitsAry.iMissionUnit = iMissionUnit;
		spawnedUnitsAry.spawnedUnitsAry = allSpawnedUnits;

		Campaign_ParseUnitInitialMissionCommands(spawnedUnit, mission, (void*)&spawnedUnitsAry);
	}
}

static bool BattleroomAddAi(const std::string controlPrefix, int numClicks)
{
	TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;
	int availableSlot = -1;
	for (int i = 0; i < 10; ++i)
	{
		if (taPtr->Players[i].My_PlayerType == Player_none)
		{
			availableSlot = i;
			break;
		}
	}

	if (availableSlot >= 0)
	{
		// Theres a bit that needs doing and the required functionality is baked into the battleroom callback, not an encapsulated function.
		// So we'll invoke it by faking a GUI button press ...

		std::string targetControlName = controlPrefix + std::to_string(availableSlot);
		_GUI0IDControl* playerGuiControl = taPtr->desktopGUI.TheActive_GUIMEM ? taPtr->desktopGUI.TheActive_GUIMEM->ControlsAry : NULL;
		int idxPlayerGuiControl = -1;
		if (playerGuiControl)
		{
			int totalGadgets = playerGuiControl->totalgadgets;
			for (int i = 1; i <= totalGadgets; ++i)
			{
				if (targetControlName == playerGuiControl[i].name)
				{
					idxPlayerGuiControl = i;
					break;
				}
			}
		}
		if (idxPlayerGuiControl < 0)
		{
			// GG: not the battleroom (or no such slot control); clicking -1 would do nothing useful
			return false;
		}

		for (int i = 0; i < numClicks; ++i)
		{
			// cycle from "open" to "blocked" to "AI" (numClicks=2)
			taPtr->desktopGUI.UIChange_f = idxPlayerGuiControl;
			taPtr->desktopGUI.GUIUpdated_b = idxPlayerGuiControl;
			battleroom_OnCommand(&taPtr->desktopGUI);
		}
	}

	return availableSlot >= 0;
}

// GG: the battleroom slot holding a local AI, or -1
static int FindLocalAiSlot()
{
	TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;
	for (int i = 0; i < 10; ++i)
	{
		if (taPtr->Players[i].My_PlayerType == Player_LocalAI ||
			(taPtr->Players[i].PlayerInfo && taPtr->Players[i].PlayerInfo->PlayerType == Player_LocalAI))
		{
			return i;
		}
	}
	return -1;
}

// GG director: the finished units of these types that belong to players at war with `self`
// (every human that is not watching), each with its type (UnitID), to tell later that its slot has
// gone to another unit (UnitInGameIndex is the slot's own number, so it can't).
// The type "*" stands for any unit that doesn't fly: ground units and buildings.
static std::vector<std::pair<UnitStruct*, short> > EnemyUnitsOfTypes(PlayerStruct* self, const std::vector<std::string>& types)
{
	std::vector<std::pair<UnitStruct*, short> > found;
	TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;
	if (types.empty())
	{
		return found;
	}
	for (int p = 0; p < 10; ++p)
	{
		PlayerStruct* player = &taPtr->Players[p];
		if (player == self || !player->PlayerActive || !player->Units || !player->PlayerInfo ||
			(player->My_PlayerType != Player_LocalHuman && player->My_PlayerType != Player_RemoteHuman) ||
			(player->PlayerInfo->PropertyMask & WATCH))
		{
			continue;
		}
		for (int u = 0; u < taPtr->MaxUnitNumberPerPlayer; ++u)
		{
			UnitStruct* unit = &player->Units[u];
			if (!unit->IsUnit || unit->Nanoframe != 0.0f || unsigned(unit->UnitID) >= taPtr->UNITINFOCount)
			{
				continue;
			}
			const UnitDefStruct& def = taPtr->UnitDef[unit->UnitID];
			for (const std::string& type : types)
			{
				if (type == "*" ? !(def.UnitTypeMask_0 & canfly) : _stricmp(def.UnitName, type.c_str()) == 0)
				{
					found.push_back(std::make_pair(unit, unit->UnitID));
					break;
				}
			}
		}
	}
	return found;
}

static double SquaredDistance(const UnitStruct* a, const UnitStruct* b)
{
	double dx = double(a->XPos) - double(b->XPos);
	double dz = double(a->ZPos) - double(b->ZPos);
	return dx * dx + dz * dz;
}

static UnitStruct* NearestOf(const std::vector<std::pair<UnitStruct*, short> >& candidates, UnitStruct* from)
{
	UnitStruct* best = NULL;
	double bestDistance = 0.0;
	for (const auto& found : candidates)
	{
		double distance = SquaredDistance(found.first, from);
		if (!best || distance < bestDistance)
		{
			best = found.first;
			bestDistance = distance;
		}
	}
	return best;
}

// GG: whether another TA instance is in the game, to send to. Not in a skirmish, say.
static bool AnyRemotePlayer()
{
	TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;
	for (int i = 0; i < 10; ++i)
	{
		if (taPtr->Players[i].PlayerActive && taPtr->Players[i].My_PlayerType == Player_RemoteHuman)
		{
			return true;
		}
	}
	return false;
}

// GG: a chat line for every other TA instance in the game, shown as it is (no sender's name), the
// way ChallengeResponse.cpp sends its reports. SendText only shows a line locally.
static void BroadcastChatLine(const char* text)
{
	if (!AnyRemotePlayer())
	{
		return;
	}
	TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;
	char buffer[65] = { 0 };
	buffer[0] = 0x05;	// chat
	strncpy(buffer + 1, text, sizeof(buffer) - 2);
	unsigned fromDpid = taPtr->Players[taPtr->LocalHumanPlayer_PlayerID].DirectPlayID;
	HAPI_BroadcastMessage(fromDpid, buffer, sizeof(buffer));
}

// GG: takes the feature over a cell away as reclaimed, here and on every other TA instance: what
// TA does when a unit finishes reclaiming one (0x4237d0), less the credit. The others run the same
// FEATURES_Reclaimed from their 0x0F handler, and the packet is TA's own: 0F, FF (reclaimed), cell
// x and cell z as 16-bit words.
static void ReclaimFeatureEverywhere(int cellX, int cellZ, bool anyRemote)
{
	FEATURES_Reclaimed(cellX, cellZ, 1);
	if (anyRemote)
	{
		TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;
		char packet[6] = { 0x0f, char(0xff), char(cellX & 0xff), char(cellX >> 8), char(cellZ & 0xff), char(cellZ >> 8) };
		unsigned fromDpid = taPtr->Players[taPtr->LocalHumanPlayer_PlayerID].DirectPlayID;
		HAPI_BroadcastMessage(fromDpid, packet, sizeof(packet));
	}
}

// GG: a line for the whole team: shown here and sent on to every other TA instance.
static void TellTeam(const char* text)
{
	SendText(text, 0);
	BroadcastChatLine(text);
}

// GG: roughly where on the map a unit is, by thirds: "north-west" to "south-east", or "middle".
static const char* MapZone(const UnitStruct* unit)
{
	static const char* const zones[3][3] = {
		{ "north-west", "north", "north-east" },
		{ "west", "middle", "east" },
		{ "south-west", "south", "south-east" },
	};
	TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;
	int width = taPtr->FeatureMapSizeX * 16;
	int height = taPtr->FeatureMapSizeY * 16;
	int column = width > 0 ? 3 * int(unit->XPos) / width : 1;
	int row = height > 0 ? 3 * int(unit->ZPos) / height : 1;
	return zones[row < 0 ? 0 : row > 2 ? 2 : row][column < 0 ? 0 : column > 2 ? 2 : column];
}

static unsigned int InitMissionUnitSpawnQueueAddr = 0x49759f;
static unsigned int InitMissionUnitSpawnQueueProc(PInlineX86StackBuffer X86StrackBuffer)
{
	MultiplayerSchemaUnits::GetInstance()->initMissionUnitSpawnQueue();
	return 0;
}

static unsigned int BattleroomStartButtonHookAddr = 0x44872a;
static unsigned int BattleroomStartButtonHookProc(PInlineX86StackBuffer X86StrackBuffer)
{
	static bool userNotified = false;

	if (!MultiplayerSchemaUnits::GetInstance()->isUserSpawnEnabled()) {
		return 0;
	}
	if (!MultiplayerSchemaUnits::GetInstance()->mapHasSpawnUnits())
	{
		return 0;
	}

	// GG: a mission never starts without its AI, so check on every Start, not just the first
	bool missionLocked = MultiplayerSchemaUnits::GetInstance()->isMissionLocked();
	if (userNotified && !missionLocked)
	{
		return 0;
	}

	bool aiAdded = false;
	bool aiMissing = false;
	if (MultiplayerSchemaUnits::GetInstance()->mapHasNeutralSpawnUnits())
	{
		TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;
		bool alreadyHasAi = false;
		for (int i = 0; i < 10; ++i)
		{
			if (taPtr->Players[i].PlayerInfo->PlayerType == Player_LocalAI)
			{
				alreadyHasAi = true;
				break;
			}
		}
		if (!alreadyHasAi)
		{
			aiAdded = BattleroomAddAi("PLAYER", 2);
			aiMissing = !aiAdded;
		}
	}

	if (missionLocked)
	{
		if (aiAdded)
		{
			SendText("Mission: an AI has been added to play the enemy. Press Start again", 0);
		}
		else if (aiMissing)
		{
			SendText("Mission: the enemy AI needs a free slot. Open one, then press Start", 0);
		}
		if (aiAdded || aiMissing)
		{
			X86StrackBuffer->rtnAddr_Pvoid = (LPVOID)0x448a62;		// discard the START command
			return X86STRACKBUFFERCHANGE;
		}
		return 0;
	}

	if (aiAdded)
	{
		SendText("An AI has been added to accept neutral units for this map", 0);
		SendText("Remove the AI now if you don't want it", 0);
		SendText("Use +spawnoff to disable extra unit spawn in general", 0);
		userNotified = true;
		X86StrackBuffer->rtnAddr_Pvoid = (LPVOID)0x448a62;		// discard the START command
		return X86STRACKBUFFERCHANGE;
	}

	return 0;
}

static unsigned int SkirmishSpawnPlayerCommanderHookAddr = 0x496fe1;
static unsigned int SkirmishSpawnPlayerCommanderHookProc(PInlineX86StackBuffer X86StrackBuffer)
{
	TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;
	if (taPtr->GameingState_Ptr->uniqueIdentifierCount == 0u) {
		return 0;
	}
	if (DataShare->PlayingDemo) {
		return 0;
	}

	int* targetPlayerIndex = (int*)(X86StrackBuffer->Esp + 0x9c + 0x04);
	int* startPosMapPlayerId = (int*)(X86StrackBuffer->Esp + 0x9c + 0x08);
	PlayerStruct* targetPlayer = &taPtr->Players[*targetPlayerIndex];

	if (MultiplayerSchemaUnits::GetInstance()->mapHasNeutralSpawnUnits() &&
		taPtr->skirmishInfo->location[0] == 0u) // random position
	{
		int* startPosArray = (int*)(X86StrackBuffer->Esp + 0x9c + 0x40);	// beware, parent scope only valid when called from "random positions" context

		int mapPositionLastHuman = -1;
		int mapPositionLastAi = -1;
		int idxLastHuman = -1;
		int idxLastAi = -1;
		for (int i = 0; i < 10; ++i)
		{
			int mapPosition = startPosArray[i];
			if (taPtr->Players[i].My_PlayerType == Player_LocalHuman && mapPosition > mapPositionLastHuman)
			{
				mapPositionLastHuman = mapPosition;
				idxLastHuman = i;
			}
			else if (taPtr->Players[i].My_PlayerType == Player_LocalAI && mapPosition > mapPositionLastAi)
			{
				mapPositionLastAi = mapPosition;
				idxLastAi = i;
			}
		}

		if (mapPositionLastAi < mapPositionLastHuman)
		{
			*startPosMapPlayerId = *startPosMapPlayerId == mapPositionLastHuman
				? mapPositionLastAi
				: mapPositionLastHuman;
			std::swap(startPosArray[idxLastHuman], startPosArray[idxLastAi]);
		}
	}

	if (MultiplayerSchemaUnits::GetInstance()->spawnInitialUnits(targetPlayer, *startPosMapPlayerId))
	{
		X86StrackBuffer->rtnAddr_Pvoid = (LPVOID)0x497026;
		return X86STRACKBUFFERCHANGE;
	}
	else
	{
		return 0;
	}
}

static unsigned int MultiplayerSpawnPlayerCommanderHookAddr = 0x497794;
static unsigned int MultiplayerSpawnPlayerCommanderHookProc(PInlineX86StackBuffer X86StrackBuffer)
{
	TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;
	if (taPtr->GameingState_Ptr->uniqueIdentifierCount == 0u) {
		return 0;
	}
	if (DataShare->PlayingDemo) {
		return 0;
	}
	if (!MultiplayerSchemaUnits::GetInstance()->isUserSpawnEnabled()) {
		return 0;
	}

	PlayerStruct* targetPlayer = (PlayerStruct*)(X86StrackBuffer->Esi);

	if (MultiplayerSchemaUnits::GetInstance()->spawnInitialUnits(targetPlayer, targetPlayer->mapStartPos))
	{
		X86StrackBuffer->rtnAddr_Pvoid = (LPVOID)0x4977c0;
		return X86STRACKBUFFERCHANGE;
	}
	else
	{
		return 0;
	}
}

static void BattleroomCommand_SpawnOff(const std::vector<std::string>&)
{
	if (MultiplayerSchemaUnits::GetInstance()->isMissionLocked())
	{
		SendText("Mission: unit spawn stays on", 0);
		return;
	}
	MultiplayerSchemaUnits::GetInstance()->setUserSpawnEnabled(false);
	SendText("Unit spawn is disabled ...", 0);
}

static void BattleroomCommand_SpawnOn(const std::vector<std::string>&)
{
	MultiplayerSchemaUnits::GetInstance()->setUserSpawnEnabled(true);
	SendText("Unit spawn is enabled ...", 0);
}

static void SpawnLaterUnits(int gameTime)
{
	MultiplayerSchemaUnits::GetInstance()->spawnLaterUnits(gameTime);
}

std::unique_ptr<MultiplayerSchemaUnits> MultiplayerSchemaUnits::m_instance;
MultiplayerSchemaUnits* MultiplayerSchemaUnits::GetInstance()
{
	if (!m_instance)
	{
		m_instance.reset(new MultiplayerSchemaUnits());
	}
	return m_instance.get();
}

MultiplayerSchemaUnits::MultiplayerSchemaUnits():
	m_spawnEnabled(true),
	m_spawnQueueIterator(m_spawnQueue.end()),
	m_neutralPlayer(NULL),
	m_missionLock(-1),
	m_lastAiAddTicks(0),
	m_ggMap(false),
	m_anchorReleased(false),
	m_lastDirectorTick(0),
	m_lastStragglers(-1)
{
	std::fill(m_startPositionsByPlayer, m_startPositionsByPlayer + 10, -1);
	std::fill(m_playersByStartPosition, m_playersByStartPosition + 10, -1);
	m_hooks.push_back(std::make_shared<InlineSingleHook>(SkirmishSpawnPlayerCommanderHookAddr, 5, INLINE_5BYTESLAGGERJMP, SkirmishSpawnPlayerCommanderHookProc));
	m_hooks.push_back(std::make_shared<InlineSingleHook>(MultiplayerSpawnPlayerCommanderHookAddr, 5, INLINE_5BYTESLAGGERJMP, MultiplayerSpawnPlayerCommanderHookProc));
	m_hooks.push_back(std::make_shared<InlineSingleHook>(BattleroomStartButtonHookAddr, 5, INLINE_5BYTESLAGGERJMP, BattleroomStartButtonHookProc));
	m_hooks.push_back(std::make_shared<InlineSingleHook>(InitMissionUnitSpawnQueueAddr, 5, INLINE_5BYTESLAGGERJMP, InitMissionUnitSpawnQueueProc));

	GameTickHook::GetInstance()->addCallback(&SpawnLaterUnits);

	BattleroomCommands::GetInstance()->RegisterCommand("+spawnoff", &BattleroomCommand_SpawnOff);
	BattleroomCommands::GetInstance()->RegisterCommand("+spawnon", &BattleroomCommand_SpawnOn);
}

MultiplayerSchemaUnits::~MultiplayerSchemaUnits()
{
}

bool MultiplayerSchemaUnits::isUserSpawnEnabled()
{
	return m_spawnEnabled;
}

void MultiplayerSchemaUnits::setUserSpawnEnabled(bool enabled)
{
	m_spawnEnabled = enabled;
}

bool MultiplayerSchemaUnits::isMissionLocked()
{
	if (m_missionLock < 0)
	{
		// gpgnet4ta rewrites TAForever.ini, next to the game executable, before every launch
		char exePath[MAX_PATH] = { 0 };
		DWORD n = GetModuleFileNameA(NULL, exePath, MAX_PATH);
		std::string iniPath(exePath, n);
		iniPath = iniPath.substr(0, iniPath.find_last_of("\\/") + 1) + "TAForever.ini";
		m_missionLock = GetPrivateProfileIntA("totala", "ggmissionlock", 0, iniPath.c_str()) ? 1 : 0;
		IDDrawSurface::OutptFmtTxt("[MultiplayerSchemaUnits::isMissionLocked] %d from %s", m_missionLock, iniPath.c_str());
	}
	return m_missionLock > 0;
}

void MultiplayerSchemaUnits::onBattleroomHostProc(_GUIInfo* gui)
{
	// T4 only. This runs inside TDraw's hook at 0x447b9c, i.e. inside battleroom_OnCommand, and only
	// when there's a GUI event. Never fake clicks from here: they re-enter battleroom_OnCommand and
	// so this same hook, whose saved registers are per thread, and TA crashes on return (0x49fd66,
	// 2026-10-02). Adding the AI is onFrame's job.
	TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;
	if (!gui || !taPtr->GameingState_Ptr || !isMissionLocked() || !mapHasNeutralSpawnUnits())
	{
		return;
	}

	// A click on the AI's name cycles it out of its slot. TA reads the click (UIChange_f) after this
	// point, in its per-slot loop, so clearing it here swallows the click.
	int aiSlot = FindLocalAiSlot();
	_GUI0IDControl* controls = gui->TheActive_GUIMEM ? gui->TheActive_GUIMEM->ControlsAry : NULL;
	int clicked = gui->UIChange_f;
	if (aiSlot >= 0 && controls && clicked > 0 && clicked <= controls->totalgadgets &&
		std::string("PLAYER") + std::to_string(aiSlot) == controls[clicked].name)
	{
		gui->UIChange_f = -1;
		gui->GUIUpdated_b = 0;
		IDDrawSurface::OutptFmtTxt("[MultiplayerSchemaUnits::onBattleroomHostProc] ignored a click on the mission AI's slot %d", aiSlot);
	}
}

// GG: is the active GUI the multiplayer battleroom? It alone has both a PLAYER0 and a LOGO0 control.
static bool IsBattleroomActive()
{
	TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;
	_GUI0IDControl* controls = taPtr->desktopGUI.TheActive_GUIMEM ? taPtr->desktopGUI.TheActive_GUIMEM->ControlsAry : NULL;
	if (!controls)
	{
		return false;
	}
	bool player0 = false;
	bool logo0 = false;
	for (int i = 1; i <= controls->totalgadgets; ++i)
	{
		player0 = player0 || std::string("PLAYER0") == controls[i].name;
		logo0 = logo0 || std::string("LOGO0") == controls[i].name;
	}
	return player0 && logo0;
}

void MultiplayerSchemaUnits::onFrame()
{
	// T3: add the mission's AI as soon as the host is in the battleroom, so the first Start isn't
	// swallowed. Called once a frame on the GUI thread (IDDrawSurface::Unlock), outside any hook,
	// the same way upstream fakes these clicks from its Start hook rather than from the proc's own.
	if (!isMissionLocked() || DataShare->PlayingDemo)
	{
		return;
	}
	TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;
	if (!taPtr->GameingState_Ptr || !mapHasNeutralSpawnUnits() || FindLocalAiSlot() >= 0)
	{
		return;
	}
	PlayerInfoStruct* localInfo = taPtr->Players[taPtr->LocalHumanPlayer_PlayerID].PlayerInfo;
	if (!localInfo || !(localInfo->SharedBits & IsHost))
	{
		return;
	}
	// The new AI may not show in Players[] at once; don't add a second one meanwhile.
	DWORD now = GetTickCount();
	if (m_lastAiAddTicks != 0 && now - m_lastAiAddTicks < 3000)
	{
		return;
	}
	if (!IsBattleroomActive())
	{
		return;
	}
	m_lastAiAddTicks = now;

	GUIInfo* desktop = &taPtr->desktopGUI;
	int savedChange = desktop->UIChange_f;
	int savedUpdated = desktop->GUIUpdated_b;
	bool added = BattleroomAddAi("PLAYER", 2);
	desktop->UIChange_f = savedChange;
	desktop->GUIUpdated_b = savedUpdated;
	IDDrawSurface::OutptFmtTxt("[MultiplayerSchemaUnits::onFrame] mission AI added=%d", int(added));
}

bool MultiplayerSchemaUnits::mapHasSpawnUnits()
{
	TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;
	return taPtr->GameingState_Ptr->uniqueIdentifierCount > 0;
}

bool MultiplayerSchemaUnits::mapHasNeutralSpawnUnits()
{
	TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;
	for (int iMissionUnit = 0; iMissionUnit < taPtr->GameingState_Ptr->uniqueIdentifierCount; ++iMissionUnit)
	{
		MissionUnitsStruct* missionUnit = &taPtr->GameingState_Ptr->uniqueIdentifiers[iMissionUnit];
		if (missionUnit->Player == 11) {
			return true;
		}
	}
	return false;
}

void MultiplayerSchemaUnits::initMissionUnitSpawnQueue(void)
{
	TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;

	m_spawnQueue.clear();
	unsigned count = taPtr->GameingState_Ptr->uniqueIdentifierCount;
	m_spawnedUnits.assign(count, NULL);
	m_spawnedType.assign(count, 0);
	m_gone.assign(count, 0);
	m_wreckTypes.clear();
	m_wreckQueue.clear();
	m_orderedTarget.assign(count, NULL);
	m_orderedTargetType.assign(count, 0);
	m_orderedAt.assign(count, 0);
	m_pending.clear();
	m_unlessWatch.clear();
	m_anchorReleased = false;
	m_lastDirectorTick = 0;
	m_lastStragglers = -1;

	// GG: the director's rules, from the text after '@' in each entry's Ident.
	m_rules.assign(count, GGRule());
	m_ggMap = false;
	for (unsigned i = 0; i < count; ++i)
	{
		MissionUnitsStruct* missionUnit = &taPtr->GameingState_Ptr->uniqueIdentifiers[i];
		if (_stricmp(missionUnit->Unitname, "GGMSG") == 0 || _stricmp(missionUnit->Unitname, "GGCLEAR") == 0)
		{
			m_ggMap = true;
		}
		std::string ident = missionUnit->Ident ? missionUnit->Ident : "";
		size_t at = ident.find('@');
		GGRule& rule = m_rules[i];
		rule.name = ident.substr(0, at);
		if (at == std::string::npos)
		{
			continue;
		}
		rule.any = m_ggMap = true;
		auto split = [](const std::string& text, char by) {
			std::vector<std::string> parts;
			size_t start = 0;
			while (start <= text.size())
			{
				size_t end = text.find(by, start);
				if (end == std::string::npos) end = text.size();
				if (end > start) parts.push_back(text.substr(start, end - start));
				start = end + 1;
			}
			return parts;
		};
		for (const std::string& term : split(ident.substr(at + 1), '|'))
		{
			size_t eq = term.find('=');
			std::string key = term.substr(0, eq);
			std::string value = eq == std::string::npos ? "" : term.substr(eq + 1);
			if (key == "unless") rule.unless = split(value, ',');
			else if (key == "hunt")
			{
				rule.hunt = split(value, ',');
				for (const std::string& type : rule.hunt)
				{
					rule.huntAny = rule.huntAny || type == "*";
				}
			}
			else if (key == "after") rule.after = value;
			else if (key == "escort") rule.escort = value;
			else if (key == "from") rule.from = atoi(value.c_str());
			else if (key == "anchor") rule.anchor = true;
			else if (key == "tell") rule.tell = atoi(value.c_str());
		}
		IDDrawSurface::OutptFmtTxt("[MultiplayerSchemaUnits] rule %s: %s", rule.name.c_str(), ident.c_str() + at + 1);
	}

	for (int iMissionUnit = 0; iMissionUnit < taPtr->GameingState_Ptr->uniqueIdentifierCount; ++iMissionUnit)
	{
		MissionUnitsStruct* missionUnit = &taPtr->GameingState_Ptr->uniqueIdentifiers[iMissionUnit];
		if (missionUnit->Unitname[0] != '\0' && missionUnit->creationCountdown > 0)
		{
			m_spawnQueue.push_back(iMissionUnit);
		}
	}

	std::sort(m_spawnQueue.begin(), m_spawnQueue.end(), [](int iLhs, auto& iRhs) {
		TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;
		MissionUnitsStruct* missionUnitLhs = &taPtr->GameingState_Ptr->uniqueIdentifiers[iLhs];
		MissionUnitsStruct* missionUnitRhs = &taPtr->GameingState_Ptr->uniqueIdentifiers[iRhs];
		return missionUnitLhs->creationCountdown < missionUnitRhs->creationCountdown;
	});

	m_spawnQueueIterator = m_spawnQueue.begin();
}

int MultiplayerSchemaUnits::peekNextMissionUnit(int gameTime)
{
	TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;

	int result = -1;
	if (m_spawnQueueIterator != m_spawnQueue.end() &&
		unsigned(*m_spawnQueueIterator) < taPtr->GameingState_Ptr->uniqueIdentifierCount &&
		unsigned(taPtr->GameingState_Ptr->uniqueIdentifiers[*m_spawnQueueIterator].creationCountdown) <= unsigned(gameTime))
	{
		result = *m_spawnQueueIterator;
	}
	return result;
}

void MultiplayerSchemaUnits::popMissionUnit()
{
	if (m_spawnQueueIterator != m_spawnQueue.end())
	{
		++m_spawnQueueIterator;
	}
}

bool MultiplayerSchemaUnits::spawnInitialUnits(PlayerStruct* targetPlayer, int targetPlayerPosition)
{
	TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;

	bool anyNeutralUnits = mapHasNeutralSpawnUnits();
	m_startPositionsByPlayer[targetPlayer->PlayerAryIndex] = targetPlayerPosition;
	m_playersByStartPosition[targetPlayerPosition] = targetPlayer->PlayerAryIndex;

	int countActivePlayers = 0;
	for (int i = 0; i < 10; ++i) {
		if (taPtr->Players[i].PlayerActive && taPtr->Players[i].My_PlayerType != Player_none &&
			!(taPtr->Players[i].PlayerInfo->PropertyMask & WATCH)) {
			++countActivePlayers;
		}
	}

	PlayerStruct* neutralPlayer = NULL;
	if (anyNeutralUnits &&
		targetPlayer->PlayerActive &&
		1 + targetPlayerPosition == countActivePlayers &&
		GetInferredPlayerType(targetPlayer) == Player_LocalAI &&
		!(targetPlayer->PlayerInfo->PropertyMask & WATCH))
	{
		neutralPlayer = targetPlayer;
		m_neutralPlayer = targetPlayer;
	}

	std::set<std::string> allCommanderUnitNames;
	for (int i = 0; i < 5; ++i) {
		allCommanderUnitNames.insert(taPtr->RaceSideDataAry[i].commanderUnitName);
	}

	bool anySpawnedUnits = false;
	std::vector<UnitStruct*> newUnits(taPtr->GameingState_Ptr->uniqueIdentifierCount);
	for (int iMissionUnit = 0; iMissionUnit < taPtr->GameingState_Ptr->uniqueIdentifierCount; ++iMissionUnit)
	{
		UnitStruct* newUnit = NULL;
		MissionUnitsStruct* missionUnit = &taPtr->GameingState_Ptr->uniqueIdentifiers[iMissionUnit];
		if (missionUnit->creationCountdown <= 0)
		{
			std::string unitName = missionUnit->Unitname;
			bool isCommanderUnit = allCommanderUnitNames.count(unitName);

			// Ghost-Commander bugfix only works for commanders.  90 + iMissionUnit is a workaround for other types of units
			int unitNumber = isCommanderUnit ? 0 : 90 + iMissionUnit;
			if (targetPlayer == neutralPlayer && missionUnit->Player == 11)
			{
				newUnit = DoSpawnUnit(targetPlayer, missionUnit, unitNumber);
				anySpawnedUnits = true;
			}
			else if (targetPlayer != neutralPlayer &&
				1 + targetPlayerPosition == missionUnit->Player &&
				InferredPlayerTypeIsLocal(targetPlayer) &&
				!(targetPlayer->PlayerInfo->PropertyMask & WATCH))
			{
				newUnit = DoSpawnUnit(targetPlayer, missionUnit, unitNumber);
				anySpawnedUnits = true;
			}
		}
		newUnits[iMissionUnit] = newUnit;
		if (newUnit)
		{
			// GG: remembered so a delayed unit's orders can name units spawned at the start
			if (m_spawnedUnits.size() < newUnits.size())
			{
				m_spawnedUnits.resize(newUnits.size(), NULL);
				m_spawnedType.resize(newUnits.size(), 0);
				m_gone.resize(newUnits.size(), 0);
			}
			m_spawnedUnits[iMissionUnit] = newUnit;
			m_spawnedType[iMissionUnit] = newUnit->UnitID;
		}
	}

	for (int iMissionUnit = 0; iMissionUnit < taPtr->GameingState_Ptr->uniqueIdentifierCount; ++iMissionUnit)
	{
		if (newUnits[iMissionUnit])
		{
			MissionUnitsStruct* missionUnit = &taPtr->GameingState_Ptr->uniqueIdentifiers[iMissionUnit];
			DoParseInitialMissionCommands(iMissionUnit, missionUnit, newUnits[iMissionUnit], newUnits.data());
		}
	}

	return anySpawnedUnits;
}

void MultiplayerSchemaUnits::spawnLaterUnits(int gameTime)
{
	TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;

	int gameTimeSecs = gameTime / 30;
	int iMissionUnit = peekNextMissionUnit(gameTimeSecs);
	while (unsigned(iMissionUnit) < taPtr->GameingState_Ptr->uniqueIdentifierCount)
	{
		popMissionUnit();
		MissionUnitsStruct* missionUnit = &taPtr->GameingState_Ptr->uniqueIdentifiers[iMissionUnit];
		if (missionUnit->Unitname[0] != '\0')
		{
			// Units, GGMSG messages and GGCLEAR wreck clears alike: due now, unless a gate holds them.
			const GGRule* rule = unsigned(iMissionUnit) < m_rules.size() ? &m_rules[iMissionUnit] : NULL;
			if (rule && !rule->unless.empty())
			{
				// Go now if the enemy has none of these; otherwise once one it has now dies.
				std::vector<std::pair<UnitStruct*, short> > watch = EnemyUnitsOfTypes(m_neutralPlayer, rule->unless);
				IDDrawSurface::OutptFmtTxt("[MultiplayerSchemaUnits] %s due at %ds: enemy has %d of its unless-types",
					rule->name.c_str(), gameTimeSecs, int(watch.size()));
				if (watch.empty())
				{
					fireEntry(iMissionUnit, gameTimeSecs);
				}
				else
				{
					m_unlessWatch[iMissionUnit] = watch;
					m_pending.push_back(iMissionUnit);
				}
			}
			else if (rule && !rule->after.empty())
			{
				m_pending.push_back(iMissionUnit);
			}
			else
			{
				fireEntry(iMissionUnit, gameTimeSecs);
			}
		}

		iMissionUnit = peekNextMissionUnit(gameTimeSecs);
	}

	runDirector(gameTime);
}

void MultiplayerSchemaUnits::fireEntry(int iMissionUnit, int gameTimeSecs)
{
	TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;
	MissionUnitsStruct* missionUnit = &taPtr->GameingState_Ptr->uniqueIdentifiers[iMissionUnit];
	bool message = _stricmp(missionUnit->Unitname, "GGMSG") == 0;
	bool clear = _stricmp(missionUnit->Unitname, "GGCLEAR") == 0;
	if (!message && !clear)
	{
		spawnLateEntry(iMissionUnit, gameTimeSecs);
		return;
	}

	// GG mission message (GGMSG) or wreck clear (GGCLEAR): a schema entry with no real unit, due at
	// its CreationCountdown, and at its gate if it has one. Acted on once, by whoever owns its
	// Player (the AI's host), from the game tick, never from inside the battleroom proc. Stock
	// TDraw finds no unit by either name and skips the entry, so such maps stay playable.
	int idxPosition = missionUnit->Player - 1;
	int idxPlayer = unsigned(idxPosition) < 10 ? m_playersByStartPosition[idxPosition] : -1;
	PlayerStruct* owner = idxPosition == 10 ? m_neutralPlayer
		: unsigned(idxPlayer) < 10 ? &taPtr->Players[idxPlayer] : NULL;
	if (!owner || !InferredPlayerTypeIsLocal(owner))
	{
		return;
	}
	if (clear)
	{
		queueWreckClear(gameTimeSecs);
		return;
	}
	// A message's text is its Ident up to any '@', so a message can wait on a gate like a unit.
	std::string text = unsigned(iMissionUnit) < m_rules.size() ? m_rules[iMissionUnit].name : std::string();
	if (!text.empty())
	{
		IDDrawSurface::OutptFmtTxt("[MultiplayerSchemaUnits] message at %ds: %s", gameTimeSecs, text.c_str());
		TellTeam(text.c_str());
	}
}

void MultiplayerSchemaUnits::spawnLateEntry(int iMissionUnit, int gameTimeSecs)
{
	TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;
	MissionUnitsStruct* missionUnit = &taPtr->GameingState_Ptr->uniqueIdentifiers[iMissionUnit];

	int idxPosition = missionUnit->Player - 1;
	int idxPlayer = unsigned(idxPosition) < 10 ? m_playersByStartPosition[idxPosition] : -1;

	PlayerStruct* targetPlayer = NULL;
	if (idxPosition == 10) {
		targetPlayer = m_neutralPlayer;
	}
	else if (unsigned(idxPlayer) < 10) {
		targetPlayer = &taPtr->Players[idxPlayer];
	}

	if (missionUnit->Player < 11 && targetPlayer == m_neutralPlayer) {
		targetPlayer = NULL;
	}

	if (targetPlayer && InferredPlayerTypeIsLocal(targetPlayer) && !(targetPlayer->PlayerInfo->PropertyMask & WATCH))
	{
		UnitStruct* newUnit = DoSpawnUnit(targetPlayer, missionUnit, 0);
		// T1 (GG): in a mission, or on a map with GG rules, a delayed unit runs its InitialMission
		// like one spawned at the start. Other games keep upstream's behaviour (no orders).
		if (isMissionLocked() || m_ggMap)
		{
			unsigned count = taPtr->GameingState_Ptr->uniqueIdentifierCount;
			if (m_spawnedUnits.size() < count)
			{
				m_spawnedUnits.resize(count, NULL);
				m_spawnedType.resize(count, 0);
				m_gone.resize(count, 0);
			}
			if (!newUnit)
			{
				// It will never come, so a gate waiting on it (after=W03*) mustn't wait forever.
				m_gone[iMissionUnit] = 1;
				IDDrawSurface::OutptFmtTxt("[MultiplayerSchemaUnits] %s could not be made at %ds",
					unsigned(iMissionUnit) < m_rules.size() ? m_rules[iMissionUnit].name.c_str() : "?", gameTimeSecs);
				return;
			}
			// TA hands a dead unit's slot to the next unit it creates, so an earlier entry whose
			// unit lived in this slot is dead for good, not alive again (the zombie test's anchor
			// waited on eight such names for one Zeus, 2026-10-02).
			for (unsigned j = 0; j < m_spawnedUnits.size(); ++j)
			{
				if (m_spawnedUnits[j] == newUnit)
				{
					m_gone[j] = 1;
				}
			}
			m_spawnedUnits[iMissionUnit] = newUnit;
			m_spawnedType[iMissionUnit] = newUnit->UnitID;
			m_gone[iMissionUnit] = 0;
			// Its script may name earlier units: only the ones still alive, never a slot's new tenant.
			std::vector<UnitStruct*> named(m_spawnedUnits);
			for (unsigned j = 0; j < named.size(); ++j)
			{
				if (j != unsigned(iMissionUnit) && !isAlive(int(j)))
				{
					named[j] = NULL;
				}
			}
			DoParseInitialMissionCommands(iMissionUnit, missionUnit, newUnit, named.data());
			IDDrawSurface::OutptFmtTxt("[MultiplayerSchemaUnits] spawned %s at %ds",
				unsigned(iMissionUnit) < m_rules.size() ? m_rules[iMissionUnit].name.c_str() : "?", gameTimeSecs);
		}
	}
}

bool MultiplayerSchemaUnits::isAlive(int iMissionUnit)
{
	if (unsigned(iMissionUnit) >= m_spawnedUnits.size() || !m_spawnedUnits[iMissionUnit] || m_gone[iMissionUnit])
	{
		return false;
	}
	UnitStruct* unit = m_spawnedUnits[iMissionUnit];
	if (unit->IsUnit && unit->UnitID == m_spawnedType[iMissionUnit])
	{
		return true;
	}
	m_gone[iMissionUnit] = 1;	// dead for good: whatever takes its slot next is another unit
	return false;
}

bool MultiplayerSchemaUnits::isDone(int iMissionUnit)
{
	if (unsigned(iMissionUnit) >= m_spawnedUnits.size())
	{
		return false;
	}
	return m_gone[iMissionUnit] || (m_spawnedUnits[iMissionUnit] && !isAlive(iMissionUnit));
}

// after=NAME: that entry's unit has spawned and died. after=PREFIX*: every entry whose name starts
// with PREFIX has (a wave: after=W03*), and there is at least one. An entry still to come keeps the
// gate shut; one whose unit couldn't be made counts as dead.
bool MultiplayerSchemaUnits::afterGateOpen(const std::string& after)
{
	if (after.size() > 1 && after[after.size() - 1] == '*')
	{
		std::string prefix = after.substr(0, after.size() - 1);
		bool any = false;
		for (unsigned j = 0; j < m_rules.size(); ++j)
		{
			if (_strnicmp(m_rules[j].name.c_str(), prefix.c_str(), prefix.size()) != 0)
			{
				continue;
			}
			if (!isDone(int(j)))
			{
				return false;
			}
			any = true;
		}
		return any;
	}
	int other = findRule(after);
	return other >= 0 && isDone(other);
}

// GG: a GGCLEAR: every wreck and heap on the map goes, on every machine, as if reclaimed. Wreck
// types are each unit type's corpse (UnitDef +0x1BC) and what it turns into when destroyed
// (FeatureDef +0xF4, the heap), as TA's corpse drop (0x486360) chooses them, so trees, rocks,
// vents, metal patches and Dragon's Teeth stay. Queued here; clearQueuedWrecks takes a slice a pass.
void MultiplayerSchemaUnits::queueWreckClear(int gameTimeSecs)
{
	TAdynmemStruct* ta = *(TAdynmemStruct**)0x00511de8;
	if (m_wreckTypes.empty() && ta->NumFeatureDefs > 0)
	{
		m_wreckTypes.assign(ta->NumFeatureDefs, 0);
		for (unsigned u = 0; u < ta->UNITINFOCount; ++u)
		{
			// The corpse and its heap only: whatever a heap leaves in turn may be a plain decal that
			// maps use as decoration too.
			unsigned feature = ta->UnitDef[u].corpse;
			for (int step = 0; step < 2 && feature < m_wreckTypes.size(); ++step)
			{
				m_wreckTypes[feature] = 1;
				feature = *(unsigned short*)ta->FeatureDef[feature].unknownField_F4;	// its heap
			}
		}
	}
	int queued = 0;
	for (int z = 0; z < ta->FeatureMapSizeY; ++z)
	{
		for (int x = 0; x < ta->FeatureMapSizeX; ++x)
		{
			// A feature sits on its anchor cell; the rest of its footprint holds 0xfffe.
			unsigned feature = ta->FeatureMap[z * ta->FeatureMapSizeX + x].FeatureDefIndex;
			if (feature < m_wreckTypes.size() && m_wreckTypes[feature])
			{
				m_wreckQueue.push_back(std::make_pair(x, z));
				++queued;
			}
		}
	}
	IDDrawSurface::OutptFmtTxt("[MultiplayerSchemaUnits] wreck clear at %ds: %d wrecks", gameTimeSecs, queued);
}

// GG: up to 16 queued wrecks a director pass (32 a second), one 6-byte packet each as TA's own
// reclaim sends, rather than a burst of hundreds in one tick. A cell is checked again first: a
// player may have reclaimed it meanwhile.
void MultiplayerSchemaUnits::clearQueuedWrecks()
{
	if (m_wreckQueue.empty())
	{
		return;
	}
	TAdynmemStruct* ta = *(TAdynmemStruct**)0x00511de8;
	bool anyRemote = AnyRemotePlayer();
	for (int n = 0; n < 16 && !m_wreckQueue.empty(); ++n)
	{
		std::pair<int, int> cell = m_wreckQueue.back();
		m_wreckQueue.pop_back();
		unsigned feature = ta->FeatureMap[cell.second * ta->FeatureMapSizeX + cell.first].FeatureDefIndex;
		if (feature < m_wreckTypes.size() && m_wreckTypes[feature])
		{
			ReclaimFeatureEverywhere(cell.first, cell.second, anyRemote);
		}
	}
}

int MultiplayerSchemaUnits::findRule(const std::string& name)
{
	for (unsigned i = 0; i < m_rules.size(); ++i)
	{
		if (_stricmp(m_rules[i].name.c_str(), name.c_str()) == 0)
		{
			return int(i);
		}
	}
	return -1;
}

void MultiplayerSchemaUnits::runDirector(int gameTime)
{
	// The '@' rules. Only on the machine that owns the mission's units (the AI's host), twice a
	// second, from the game tick (never from inside the battleroom proc).
	if (!m_ggMap || !m_neutralPlayer || !InferredPlayerTypeIsLocal(m_neutralPlayer) || gameTime - m_lastDirectorTick < 15)
	{
		return;
	}
	m_lastDirectorTick = gameTime;
	int gameTimeSecs = gameTime / 30;
	TAdynmemStruct* ta = *(TAdynmemStruct**)0x00511de8;

	// Gated entries (units, messages, wreck clears) whose condition has come true.
	for (auto it = m_pending.begin(); it != m_pending.end();)
	{
		int i = *it;
		const GGRule& rule = m_rules[i];
		bool open = false;
		if (!rule.unless.empty())
		{
			for (const auto& watched : m_unlessWatch[i])
			{
				if (!watched.first->IsUnit || watched.first->UnitID != watched.second)
				{
					open = true;
				}
			}
		}
		else
		{
			open = afterGateOpen(rule.after);
		}
		if (open)
		{
			IDDrawSurface::OutptFmtTxt("[MultiplayerSchemaUnits] %s's gate opened at %ds", rule.name.c_str(), gameTimeSecs);
			it = m_pending.erase(it);
			fireEntry(i, gameTimeSecs);
		}
		else
		{
			++it;
		}
	}
	clearQueuedWrecks();

	// Hunters and escorts. The enemy's units are listed once a pass for each hunt list, not once a
	// hunter: a wave of a hundred hunters would otherwise read every unit slot a hundred times.
	std::map<std::string, std::vector<std::pair<UnitStruct*, short> > > enemiesByHunt;
	for (unsigned i = 0; i < m_rules.size(); ++i)
	{
		const GGRule& rule = m_rules[i];
		if ((rule.hunt.empty() && rule.escort.empty()) || gameTimeSecs < rule.from || !isAlive(int(i)))
		{
			continue;
		}
		UnitStruct* unit = m_spawnedUnits[i];
		UnitStruct* target = NULL;
		if (!rule.hunt.empty())
		{
			std::string key;
			for (const std::string& type : rule.hunt)
			{
				key += type + ",";
			}
			auto enemies = enemiesByHunt.find(key);
			if (enemies == enemiesByHunt.end())
			{
				enemies = enemiesByHunt.insert(std::make_pair(key, EnemyUnitsOfTypes(m_neutralPlayer, rule.hunt))).first;
			}
			target = NearestOf(enemies->second, unit);

			// A hunter of anything keeps its target while it lives, unless something twice as close
			// turns up, so a wave doesn't switch targets twice a second as everything moves.
			UnitStruct* current = m_orderedTarget[i];
			if (rule.huntAny && target && current && current != target &&
				current->IsUnit && current->UnitID == m_orderedTargetType[i] &&
				4.0 * SquaredDistance(target, unit) > SquaredDistance(current, unit))
			{
				target = current;
			}
		}
		ordertype::ORDERTYPE order = ordertype::ATTACK;
		if (!target && !rule.escort.empty())
		{
			// Guard the newest live unit spawned from an entry named PREFIX...
			for (unsigned j = 0; j < m_rules.size(); ++j)
			{
				if (j != i && isAlive(int(j)) && _strnicmp(m_rules[j].name.c_str(), rule.escort.c_str(), rule.escort.size()) == 0)
				{
					target = m_spawnedUnits[j];
				}
			}
			order = ordertype::DEFEND;
		}
		if (!target)
		{
			continue;
		}
		bool onIt = unit->UnitOrders && unit->UnitOrders->AttackTargat == target;
		if (target != m_orderedTarget[i] || (!onIt && gameTime - m_orderedAt[i] >= 150))
		{
			SendOrder(unit, target->XPos, target->YPos, target->ZPos, target, order, -1, false);
			m_orderedTarget[i] = target;
			m_orderedTargetType[i] = target->UnitID;
			m_orderedAt[i] = gameTime;
			IDDrawSurface::OutptFmtTxt("[MultiplayerSchemaUnits] %s: %s %s at %ds", rule.name.c_str(),
				order == ordertype::ATTACK ? "attack" : "guard", ta->UnitDef[target->UnitID].UnitName, gameTimeSecs);
		}
	}

	// The anchor goes once nothing is left to come and every other mission unit is dead.
	if (!m_anchorReleased && m_pending.empty() && m_spawnQueueIterator == m_spawnQueue.end())
	{
		int anchor = -1;
		std::vector<int> alive;
		for (unsigned i = 0; i < m_rules.size(); ++i)
		{
			if (m_rules[i].anchor)
			{
				anchor = int(i);
			}
			else if (isAlive(int(i)))
			{
				alive.push_back(int(i));
			}
		}
		if (anchor >= 0 && alive.empty() && isAlive(anchor))
		{
			IDDrawSurface::OutptFmtTxt("[MultiplayerSchemaUnits] every mission unit is spent at %ds; the anchor self-destructs", gameTimeSecs);
			m_anchorReleased = true;
			if (m_rules[anchor].tell > 0)
			{
				TellTeam("Every enemy is dead. Well done!");
			}
			DoParseInitialMissionCommands(anchor, &ta->GameingState_Ptr->uniqueIdentifiers[anchor],
				m_spawnedUnits[anchor], m_spawnedUnits.data(), "d");
		}
		else if (anchor >= 0 && !alive.empty() && int(alive.size()) <= m_rules[anchor].tell &&
			int(alive.size()) != m_lastStragglers)
		{
			// tell=N: the last few can be slow units still crossing the map from a far edge, and
			// the game waits for them, so say how many are left and roughly where.
			m_lastStragglers = int(alive.size());
			std::string text = std::to_string(alive.size()) + (alive.size() == 1 ? " enemy left: " : " enemies left: ");
			for (size_t k = 0; k < alive.size(); ++k)
			{
				UnitStruct* unit = m_spawnedUnits[alive[k]];
				const UnitDefStruct& def = ta->UnitDef[unit->UnitID];
				std::string next = text + (k ? ", " : "") + std::string(def.Name, strnlen(def.Name, sizeof(def.Name))) +
					" (" + MapZone(unit) + ")";
				if (next.size() > 63)	// a chat line's length
				{
					break;
				}
				text = next;
			}
			IDDrawSurface::OutptFmtTxt("[MultiplayerSchemaUnits] at %ds: %s", gameTimeSecs, text.c_str());
			TellTeam(text.c_str());
		}
	}
}
