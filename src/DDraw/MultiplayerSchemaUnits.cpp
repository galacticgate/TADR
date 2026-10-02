#include "MultiplayerSchemaUnits.h"
#include "tamem.h"
#include "iddrawsurface.h"
#include "hook/hook.h"
#include "tafunctions.h"
#include "StartPositions.h"
#include "BattleroomCommands.h"
#include "GameTickHook.h"

#include <set>
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

static void DoParseInitialMissionCommands(int iMissionUnit, MissionUnitsStruct* missionUnit, UnitStruct *spawnedUnit, UnitStruct *allSpawnedUnits[])
{
	TAdynmemStruct* taPtr = *(TAdynmemStruct**)0x00511de8;

	if (missionUnit->InitialMission && missionUnit->InitialMission[0] != '\0')
	{
		struct
		{
			int iMissionUnit;
			UnitStruct** spawnedUnitsAry;
		}
		spawnedUnitsAry;
		spawnedUnitsAry.iMissionUnit = iMissionUnit;
		spawnedUnitsAry.spawnedUnitsAry = allSpawnedUnits;

		Campaign_ParseUnitInitialMissionCommands(spawnedUnit, missionUnit->InitialMission, (void*)&spawnedUnitsAry);
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
	m_lastAiAddTicks(0)
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
	m_spawnedUnits.assign(taPtr->GameingState_Ptr->uniqueIdentifierCount, NULL);
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
			}
			m_spawnedUnits[iMissionUnit] = newUnit;
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
				// T1 (GG): in a mission, a delayed unit runs its InitialMission like one spawned at
				// the start. Other games keep upstream's behaviour (delayed units get no orders).
				if (newUnit && isMissionLocked())
				{
					if (m_spawnedUnits.size() < taPtr->GameingState_Ptr->uniqueIdentifierCount)
					{
						m_spawnedUnits.resize(taPtr->GameingState_Ptr->uniqueIdentifierCount, NULL);
					}
					m_spawnedUnits[iMissionUnit] = newUnit;
					DoParseInitialMissionCommands(iMissionUnit, missionUnit, newUnit, m_spawnedUnits.data());
				}
			}
		}

		iMissionUnit = peekNextMissionUnit(gameTimeSecs);
	}
}
