#pragma once

#include <windows.h>

// GG: counts the local player's key presses and mouse clicks in TA, as an APM tracker does, and
// appends them per wall-clock minute to gg-apm-log.txt in the game folder, for calibrating
// against Desktop APM. Each kind of input is kept apart so the APM definition can be settled
// after calibration. See APM_PLAN.md in the GG client repository.
namespace ApmCounter
{
	// Call first for every message TA's window gets, before any TDraw feature can swallow it.
	void OnMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

	// TDraw is about to post back a left click it swallowed earlier; that click was counted
	// when it really happened.
	void SkipRepostedClick();

	// TA is closing: write whatever is still pending.
	void Flush();
}
