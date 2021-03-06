#include "watchmode.h"

#include "characterselectscreen.h"
#include "titlescreen.h"
#include "fightscreen.h"
#include "gamelogic.h"
#include "versusscreen.h"
#include "fightscreen.h"
#include "playerdefinition.h"
#include "fightui.h"
#include "fightresultdisplay.h"

static void fightFinishedCB();

static void versusScreenFinishedCB() {
	setGameModeWatch();
	startFightScreen(fightFinishedCB);
}

static void characterSelectFinishedCB() {
	setVersusScreenFinishedCB(versusScreenFinishedCB);
	setNewScreen(getVersusScreen());
}

static void fightFinishedCB() {
	setCharacterSelectScreenModeName("Watch Mode");
	setCharacterSelectOnePlayerSelectAll();
	setCharacterSelectStageActive();
	setCharacterSelectFinishedCB(characterSelectFinishedCB);
	setNewScreen(getCharacterSelectScreen());
}

void startWatchMode()
{
	fightFinishedCB();
}
