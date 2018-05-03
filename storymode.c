#include "trainingmode.h"

#include <prism/log.h>

#include "characterselectscreen.h"
#include "titlescreen.h"
#include "fightscreen.h"
#include "gamelogic.h"
#include "versusscreen.h"
#include "fightscreen.h"
#include "playerdefinition.h"
#include "fightui.h"
#include "dolmexicastoryscreen.h"
#include "storymode.h"
#include "stage.h"

static struct {
	char mStoryPath[1024];
	int mCurrentState;

	int mNextStateAfterWin;
} gData;

static MugenDefScriptGroup* getMugenDefStoryScriptGroupByIndex(MugenDefScript* tScript, int tIndex) {
	MugenDefScriptGroup* current = tScript->mFirstGroup;
	if (!current || !current->mNext) return NULL;
	current = current->mNext;

	while (tIndex--) {
		if (!current->mNext) return NULL;
		current = current->mNext;
	}

	return current;
}

static void loadStoryboardGroup(MugenDefScriptGroup* tGroup) {
	char path[1024];
	char folder[1024];

	getPathToFile(folder, gData.mStoryPath);
	char* file = getAllocatedMugenDefStringVariableAsGroup(tGroup, "file");

	sprintf(path, "assets/%s%s", folder, file); // TODO: remove assets
	if (!isFile(path)) {
		logWarningFormat("Unable to open storyboard %s. Returning to title.", path);
		setNewScreen(&DreamTitleScreen);
		return;
	}

	setDolmexicaStoryScreenFile(path);
	setNewScreen(&DolmexicaStoryScreen);
}

static void fightFinishedCB() {
	storyModeOverCB(gData.mNextStateAfterWin); // TODO: lose + dynamic
}

static void loadFightGroup(MugenDefScriptGroup* tGroup) {
	char path[1024];
	char folder[1024];
	char* file;

	getPathToFile(folder, gData.mStoryPath);

	file = getAllocatedMugenDefStringVariableAsGroup(tGroup, "player1");
	sprintf(path, "assets/chars/%s/%s.def", file, file); // TODO: remove assets
	setPlayerDefinitionPath(0, path);

	file = getAllocatedMugenDefStringVariableAsGroup(tGroup, "player2");
	sprintf(path, "assets/chars/%s/%s.def", file, file); // TODO: remove assets
	setPlayerDefinitionPath(1, path);

	int palette1 = getMugenDefIntegerOrDefaultAsGroup(tGroup, "palette1", 1);
	setPlayerPreferredPalette(0, palette1);
	int palette2 = getMugenDefIntegerOrDefaultAsGroup(tGroup, "palette2", 2);
	setPlayerPreferredPalette(1, palette2);

	file = getAllocatedMugenDefStringVariableAsGroup(tGroup, "stage");
	sprintf(path, "assets/%s", file); // TODO: remove assets
	setDreamStageMugenDefinition(path);

	gData.mNextStateAfterWin = getMugenDefIntegerOrDefaultAsGroup(tGroup, "win", gData.mCurrentState + 1);

	setTimerFinite();
	setPlayersToRealFightMode();
	setPlayerHuman(0);
	setPlayerArtificial(1); // TODO: implement lose
	setFightScreenFinishedCB(fightFinishedCB);
	startFightScreen();
}

static void loadTitleGroup() {
	setNewScreen(&DreamTitleScreen);
}

static void loadStoryModeScreen() {
	char path[1024];
	sprintf(path, "assets/%s", gData.mStoryPath);
	MugenDefScript script = loadMugenDefScript(path);

	MugenDefScriptGroup* group = getMugenDefStoryScriptGroupByIndex(&script, gData.mCurrentState);
	if (!group || !isMugenDefStringVariableAsGroup(group, "type")) {
		logWarningFormat("Unable to read story state %d (from %s). Returning to title.", gData.mCurrentState, gData.mStoryPath);
		setNewScreen(&DreamTitleScreen);
		return;
	}

	char* type = getAllocatedMugenDefStringVariableAsGroup(group, "type");
	if (!strcmp("storyboard", type)) {
		loadStoryboardGroup(group);
	} else if (!strcmp("fight", type)) {
		loadFightGroup(group);
	} else if (!strcmp("title", type)) {
		loadTitleGroup();
	}
	else {
		logWarningFormat("Unable to read story state %d type %s (from %s). Returning to title.", gData.mCurrentState, type, gData.mStoryPath);
		setNewScreen(&DreamTitleScreen);
	}
}


static Screen StoryModeScreen = {
	.mLoad = loadStoryModeScreen,
};

static void loadStoryHeader(char* tScriptPath) {
	strcpy(gData.mStoryPath, tScriptPath);

	char path[1024];
	sprintf(path, "assets/%s", gData.mStoryPath);
	MugenDefScript storyScript = loadMugenDefScript(path);

	gData.mCurrentState = getMugenDefIntegerOrDefault(&storyScript, "Info", "startstate", 0);
}

void startStoryMode()
{
	MugenDefScript selectScript = loadMugenDefScript("assets/data/select.def");
	if (!string_map_contains(&selectScript.mGroups, "Stories")) {
		setNewScreen(&DreamTitleScreen);
		return;
	}
	
	MugenDefScriptGroup* stories = string_map_get(&selectScript.mGroups, "Stories");
	if (!list_size(&stories->mOrderedElementList)) {
		setNewScreen(&DreamTitleScreen);
		return;
	}

	MugenDefScriptGroupElement* story = list_front(&stories->mOrderedElementList);
	if (story->mType != MUGEN_DEF_SCRIPT_GROUP_STRING_ELEMENT) {
		setNewScreen(&DreamTitleScreen);
		return;
	}

	MugenDefScriptStringElement* stringElement = story->mData;
	loadStoryHeader(stringElement->mString);
	setNewScreen(&StoryModeScreen);
}

void storyModeOverCB(int tStep)
{
	gData.mCurrentState = tStep;
	setNewScreen(&StoryModeScreen);
}