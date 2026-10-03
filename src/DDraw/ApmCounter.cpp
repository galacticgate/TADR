#include "ApmCounter.h"

#include "tamem.h"
#include "iddrawsurface.h"
#include "tafunctions.h"

#include <cstdio>
#include <cstring>

namespace
{
	const char* const BUILD = "2025.12.13.2-apm";

	// Leaving the game screen this long (or a new game starting) ends the game. Shorter gaps are
	// treated as the same game, in case an in-game menu leaves the game screen for a moment.
	const DWORD GAME_END_GRACE_MS = 15000;

	struct Parts
	{
		unsigned keys = 0;       // key presses that are neither modifiers nor auto-repeats
		unsigned modifiers = 0;  // Shift, Ctrl, Alt or Windows pressed
		unsigned repeats = 0;    // auto-repeats while a key is held down
		unsigned clicks = 0;     // mouse button presses
		unsigned dblclicks = 0;  // the second press of a double-click, which Windows sends as ...BUTTONDBLCLK
		unsigned wheel = 0;      // wheel notches, either direction

		bool Any() const { return keys || modifiers || repeats || clicks || dblclicks || wheel; }

		void Add(const Parts& o)
		{
			keys += o.keys;
			modifiers += o.modifiers;
			repeats += o.repeats;
			clicks += o.clicks;
			dblclicks += o.dblclicks;
			wheel += o.wheel;
		}
	};

	HANDLE logFile = INVALID_HANDLE_VALUE;
	bool logOpened = false;

	Parts minute;
	SYSTEMTIME minuteTime = {};
	bool minuteOpen = false;
	bool minuteInGame = false;

	Parts game;
	bool inGame = false;          // a game is being counted, even while briefly off the game screen
	bool onGameScreen = false;
	SYSTEMTIME gameStartTime = {};
	DWORD gameStartMs = 0;
	DWORD gameLastMs = 0;         // last moment seen on the game screen
	int gameStartTick = 0;
	int gameLastTick = 0;
	bool gameWatcher = false;
	bool gameDemo = false;

	int repostedClicks = 0;
	int wheelDelta = 0;

	void OpenLog()
	{
		logOpened = true;
		logFile = CreateFileA("gg-apm-log.txt", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (logFile == INVALID_HANDLE_VALUE)
		{
			return;
		}
		SYSTEMTIME now;
		GetLocalTime(&now);
		char line[128];
		_snprintf_s(line, sizeof(line), _TRUNCATE, "SESSION %04d-%02d-%02d %02d:%02d:%02d build=%s\r\n",
			now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, BUILD);
		DWORD written;
		WriteFile(logFile, line, (DWORD)strlen(line), &written, NULL);
	}

	void WriteLine(const char* text)
	{
		if (!logOpened)
		{
			OpenLog();
		}
		if (logFile == INVALID_HANDLE_VALUE)
		{
			return;
		}
		DWORD written;
		WriteFile(logFile, text, (DWORD)strlen(text), &written, NULL);
		WriteFile(logFile, "\r\n", 2, &written, NULL);
	}

	void FormatParts(char* out, size_t size, const Parts& p)
	{
		_snprintf_s(out, size, _TRUNCATE, "keys=%u mod=%u rep=%u clicks=%u dbl=%u wheel=%u",
			p.keys, p.modifiers, p.repeats, p.clicks, p.dblclicks, p.wheel);
	}

	bool SameMinute(const SYSTEMTIME& a, const SYSTEMTIME& b)
	{
		return a.wMinute == b.wMinute && a.wHour == b.wHour && a.wDay == b.wDay
			&& a.wMonth == b.wMonth && a.wYear == b.wYear;
	}

	void FlushMinute()
	{
		if (minuteOpen && minute.Any())
		{
			char parts[160];
			FormatParts(parts, sizeof(parts), minute);
			char line[256];
			_snprintf_s(line, sizeof(line), _TRUNCATE, "MIN %04d-%02d-%02d %02d:%02d %s ingame=%d",
				minuteTime.wYear, minuteTime.wMonth, minuteTime.wDay, minuteTime.wHour, minuteTime.wMinute,
				parts, minuteInGame ? 1 : 0);
			WriteLine(line);
		}
		minute = Parts();
		minuteOpen = false;
		minuteInGame = false;
	}

	void EndGame()
	{
		double realSeconds = (DWORD)(gameLastMs - gameStartMs) / 1000.0;
		double gameSeconds = (gameLastTick - gameStartTick) / 30.0;
		unsigned actions = game.keys + game.modifiers + game.clicks + game.dblclicks;
		double apm = realSeconds > 0 ? actions * 60.0 / realSeconds : 0;

		char parts[160];
		FormatParts(parts, sizeof(parts), game);
		char line[384];
		_snprintf_s(line, sizeof(line), _TRUNCATE,
			"GAME_END start=%02d:%02d:%02d real_s=%.1f game_s=%.1f %s apm_keys_mod_clicks_dbl=%.1f watcher=%d demo=%d",
			gameStartTime.wHour, gameStartTime.wMinute, gameStartTime.wSecond, realSeconds, gameSeconds,
			parts, apm, gameWatcher ? 1 : 0, gameDemo ? 1 : 0);
		WriteLine(line);

		inGame = false;
		game = Parts();
	}

	void StartGame(TAdynmemStruct* ta, DWORD nowMs)
	{
		inGame = true;
		game = Parts();
		GetLocalTime(&gameStartTime);
		gameStartMs = nowMs;
		gameLastMs = nowMs;
		gameStartTick = ta ? ta->GameTime : 0;
		gameLastTick = gameStartTick;

		PlayerStruct* me = ta ? &ta->Players[ta->LocalHumanPlayer_PlayerID] : NULL;
		gameWatcher = me && me->PlayerInfo && (me->PlayerInfo->PropertyMask & WATCH) != 0;
		gameDemo = DataShare->PlayingDemo != 0;

		char line[96];
		_snprintf_s(line, sizeof(line), _TRUNCATE, "GAME_START %02d:%02d:%02d tick=%d",
			gameStartTime.wHour, gameStartTime.wMinute, gameStartTime.wSecond, gameStartTick);
		WriteLine(line);
	}

	// DataShare->TAProgress is fresh: _WinProc calls UpdateTAProcess before counting.
	void TrackGame()
	{
		TAdynmemStruct* ta = *(TAdynmemStruct**)0x00511de8;
		DWORD nowMs = GetTickCount();
		onGameScreen = DataShare->TAProgress == TAInGame;

		if (onGameScreen)
		{
			int tick = ta ? ta->GameTime : 0;
			if (inGame && tick < gameLastTick)
			{
				EndGame();  // the clock went back: a new game
			}
			if (!inGame)
			{
				StartGame(ta, nowMs);
			}
			gameLastMs = nowMs;
			gameLastTick = tick;
		}
		else if (inGame && (DataShare->TAProgress == TAExiting || (DWORD)(nowMs - gameLastMs) > GAME_END_GRACE_MS))
		{
			EndGame();
		}
	}

	bool IsModifier(WPARAM vk)
	{
		switch (vk)
		{
		case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
		case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
		case VK_MENU: case VK_LMENU: case VK_RMENU:
		case VK_LWIN: case VK_RWIN:
			return true;
		}
		return false;
	}
}

namespace ApmCounter
{
	void OnMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		TrackGame();

		SYSTEMTIME now;
		GetLocalTime(&now);
		if (minuteOpen && !SameMinute(now, minuteTime))
		{
			FlushMinute();
		}
		if (!minuteOpen)
		{
			minuteOpen = true;
			minuteTime = now;
		}

		if (msg == WM_ACTIVATEAPP)
		{
			char line[64];
			_snprintf_s(line, sizeof(line), _TRUNCATE, "FOCUS %s %02d:%02d:%02d", wParam ? "on" : "off",
				now.wHour, now.wMinute, now.wSecond);
			WriteLine(line);
			return;
		}

		Parts p;
		switch (msg)
		{
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
			if (lParam & (1 << 30))
				p.repeats = 1;
			else if (IsModifier(wParam))
				p.modifiers = 1;
			else
				p.keys = 1;
			break;

		case WM_LBUTTONDOWN:
			if (repostedClicks > 0)
			{
				--repostedClicks;
				return;
			}
			p.clicks = 1;
			break;

		case WM_RBUTTONDOWN:
		case WM_MBUTTONDOWN:
		case 0x020B: // WM_XBUTTONDOWN
			p.clicks = 1;
			break;

		case WM_LBUTTONDBLCLK:
		case WM_RBUTTONDBLCLK:
		case WM_MBUTTONDBLCLK:
		case 0x020D: // WM_XBUTTONDBLCLK
			p.dblclicks = 1;
			break;

		case 0x020A: // WM_MOUSEWHEEL
		case 0x020E: // WM_MOUSEHWHEEL
		{
			int delta = (short)HIWORD(wParam);
			wheelDelta += delta < 0 ? -delta : delta;
			p.wheel = wheelDelta / 120;
			wheelDelta %= 120;
			break;
		}

		default:
			return;
		}

		// Desktop APM credits actions to the application in the foreground.
		if (!p.Any() || GetForegroundWindow() != hwnd)
		{
			return;
		}

		minute.Add(p);
		if (onGameScreen)
		{
			game.Add(p);
			minuteInGame = true;
		}
	}

	void SkipRepostedClick()
	{
		++repostedClicks;
	}

	void Flush()
	{
		FlushMinute();
		if (inGame)
		{
			EndGame();
		}
		if (logFile != INVALID_HANDLE_VALUE)
		{
			CloseHandle(logFile);
			logFile = INVALID_HANDLE_VALUE;
		}
	}
}
