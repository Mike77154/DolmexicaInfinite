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

static void fightFinishedCB() {
	MugenDefScript script; 
	loadMugenDefScript(&script, "assets/data/select.def");

	setCharacterRandom(&script, 0);
	setCharacterRandom(&script, 1);
	setStageRandom(&script);

	setVersusScreenFinishedCB(versusScreenFinishedCB);
	setNewScreen(getVersusScreen());
}

void startRandomWatchMode()
{
	fightFinishedCB();
}
