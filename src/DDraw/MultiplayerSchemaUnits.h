#pragma once

#include <cinttypes>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include <windows.h>

class SingleHook;
struct MissionUnitsStruct;
struct PlayerStruct;
struct UnitStruct;
struct _GUIInfo;

// GG mission director rules, written in a schema entry's Ident after an '@':
//   NAME@unless=ARMFIG,CORVENG   spawn at CreationCountdown if the enemy has none of these types;
//                                otherwise once the first of the ones it had then dies
//   NAME@after=OTHER             spawn once the unit spawned from entry OTHER is dead
//   NAME@from=100|hunt=A,B|escort=PREFIX
//                                from that game second: attack the nearest enemy A or B; with none,
//                                guard the newest live unit whose entry name starts with PREFIX
//   NAME@anchor                  keeps its owner in the game; self-destructs once every other
//                                mission unit has spawned and died, so the game can end
// Stock TDraw ignores all of it: to TA the Ident is only a label.
struct GGRule
{
	std::string name;
	bool any = false;
	int from = -1;
	std::vector<std::string> unless;
	std::vector<std::string> hunt;
	std::string after;
	std::string escort;
	bool anchor = false;
};

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
	void runDirector(int gameTime);				// the '@' rules, from the game tick

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

	void spawnLateEntry(int iMissionUnit, int gameTimeSecs);
	bool isAlive(int iMissionUnit);
	int findRule(const std::string& name);
	std::vector<GGRule> m_rules;					// by mission unit index
	std::vector<short> m_spawnedIndex;				// UnitInGameIndex at spawn, to tell a reused slot
	std::vector<int> m_pending;						// late entries waiting for their gate
	std::map<int, std::vector<std::pair<UnitStruct*, short> > > m_unlessWatch;
	std::vector<UnitStruct*> m_orderedTarget;		// the director's last target, by mission unit
	std::vector<int> m_orderedAt;					// and when it gave that order (game ticks)
	bool m_ggMap;									// the map has GGMSG entries or '@' rules
	bool m_anchorReleased;
	int m_lastDirectorTick;
};
