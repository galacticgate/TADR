#pragma once

#include <cinttypes>
#include <memory>
#include <random>
#include <vector>
#include <windows.h>

class SingleHook;
struct MissionUnitsStruct;
struct PlayerStruct;
struct UnitStruct;
struct _GUIInfo;

// spawns units for the first AI player as defined in the OTA file's schema
class MultiplayerSchemaUnits
{
public:
	// Create and Get the instance
	static MultiplayerSchemaUnits* GetInstance();
	~MultiplayerSchemaUnits();

	bool mapHasSpawnUnits();
	bool mapHasNeutralSpawnUnits();

	bool isUserSpawnEnabled();
	void setUserSpawnEnabled(bool enabled);

	void initMissionUnitSpawnQueue(void);
	int peekNextMissionUnit(int gameTime); // index into taPtr->GameingState_Ptr->uniqueIdentifiers, or -1
	void popMissionUnit();

	bool spawnInitialUnits(PlayerStruct* targetPlayer, int targetPlayerPosition);
	void spawnLaterUnits(int gameTime);

	// GG placement missions: gpgnet4ta writes ggmissionlock=1 into TAForever.ini for a mission
	// game. On a map with neutral (Player=11) units the lock keeps the mission's AI in the
	// battleroom, refuses +spawnoff, and gives delayed units their InitialMission too.
	bool isMissionLocked();
	void onBattleroomHostProc(_GUIInfo* gui);	// the host's battleroom events, from sharedialog's hook
	void onFrame();								// once a frame on the GUI thread, from IDDrawSurface::Unlock

private:
	MultiplayerSchemaUnits();

	static std::unique_ptr<MultiplayerSchemaUnits> m_instance;

	std::vector<std::shared_ptr<SingleHook> > m_hooks;
	bool m_spawnEnabled;
	std::vector<int> m_spawnQueue;	// indices into mission units sorted by CreationCountdown
	std::vector<int>::iterator m_spawnQueueIterator;
	int m_startPositionsByPlayer[10];
	int m_playersByStartPosition[10];
	PlayerStruct* m_neutralPlayer;
	std::vector<UnitStruct*> m_spawnedUnits;	// by mission unit index, for named targets in late units' orders
	int m_missionLock;							// -1 until TAForever.ini is read
	DWORD m_lastAiAddTicks;
};
