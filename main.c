#include <prism/framerateselectscreen.h>
#include <prism/pvr.h>
#include <prism/physics.h>
#include <prism/file.h>
#include <prism/drawing.h>
#include <prism/log.h>
#include <prism/wrapper.h>
#include <prism/system.h>
#include <prism/stagehandler.h>
#include <prism/logoscreen.h>
#include <prism/mugentexthandler.h>

#include "titlescreen.h"
#include "fightscreen.h"
#include "playerdefinition.h"


#ifdef DREAMCAST
KOS_INIT_FLAGS(INIT_DEFAULT);

extern uint8 romdisk[];
KOS_INIT_ROMDISK(romdisk);

#define DEVELOP
#endif


void exitGame() {
	shutdownPrismWrapper();

#ifdef DEVELOP
	abortSystem();
#else
	returnToMenu();
#endif
}

void setMainFileSystem() {
#ifdef DEVELOP
	setFileSystem("/pc");
#else
	setFileSystem("/cd");
#endif
}

int main(int argc, char** argv) {
	(void)argc;
	(void)argv;

	setGameName("DOLMEXICA INFINITE");
	setScreenSize(320, 240);
	
	initPrismWrapperWithDefaultFlags();
	setMainFileSystem();	
	setFont("$/rd/fonts/segoe.hdr", "$/rd/fonts/segoe.pkg");
	loadMugenTextHandler();
	loadMugenSystemFonts();

	logg("Check framerate");
	FramerateSelectReturnType framerateReturnType = selectFramerate();
	if (framerateReturnType == FRAMERATE_SCREEN_RETURN_ABORT) {
		exitGame();
	}

	
	setMemoryHandlerCompressionActive();
	setPlayerDefinitionPath(0, "assets/chars/kfm/kfm.def");
	setPlayerDefinitionPath(1, "assets/chars/kfm/kfm.def");
	setPlayerHuman(0);
	setPlayerArtificial(1);
	setScreenAfterWrapperLogoScreen(&DreamTitleScreen);
	startScreenHandling(getLogoScreenFromWrapper());

	exitGame();
	
	return 0;
}

