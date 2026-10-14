#pragma once

// Miyoo Mini / OnionOS integration: save states, OnionOS's Game Switcher and the
// save at power-off. See miyoo_state.cpp for how it works.
namespace miyoo_state
{
	// At the start of WinMain, before SDL_Init.
	void Init();

	// Right before pb::init(): from here on the table's memory goes to the
	// state's own heap (everything a save state holds).
	void HeapOn();

	// After the table is set up: true if the game goes on from a saved state
	// (opened by the Game Switcher, or after a power-off in the game).
	bool ResumePending();

	// Loads that state; false if it could not be loaded (the game then starts
	// as usual). Call only when ResumePending() was true.
	bool Resume();

	// Once per main loop pass, between two frames: the MENU key (Game
	// Switcher, back to the menu, previous game), power-off.
	void Poll();

	// The game is closing normally.
	void Shutdown();

	// While one of these is alive, new memory comes from the normal heap
	// (settings, High Scores, buttons: things a save state must not hold).
	struct NormalHeap
	{
		NormalHeap();
		~NormalHeap();
		NormalHeap(const NormalHeap&) = delete;
		NormalHeap& operator=(const NormalHeap&) = delete;
	private:
		bool prev;
	};
}
