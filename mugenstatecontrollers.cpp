#include "mugenstatecontrollers.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <string>

#define LOGGER_WARNINGS_DISABLED

#include <prism/memoryhandler.h>
#include <prism/log.h>
#include <prism/system.h>
#include <prism/clipboardhandler.h>
#include <prism/input.h>
#include <prism/math.h>
#include <prism/mugenanimationhandler.h>
#include <prism/screeneffect.h>

#include "playerdefinition.h"
#include "playerhitdata.h"
#include "mugenexplod.h"
#include "stage.h"
#include "mugenstagehandler.h"
#include "config.h"
#include "pausecontrollers.h"
#include "mugenassignmentevaluator.h"
#include "fightui.h"
#include "gamelogic.h"
#include "projectile.h"
#include "dolmexicastoryscreen.h"
#include "titlescreen.h"
#include "intro.h"
#include "mugensound.h"

#define GAME_MAKE_ANIM_UNDER_Z 31
#define GAME_MAKE_ANIM_OVER_Z 51

using namespace std;

typedef void(*StateControllerParseFunction)(DreamMugenStateController*, MugenDefScriptGroup*);
typedef int(*StateControllerHandleFunction)(DreamMugenStateController*, DreamPlayer*); // return 1 iff state changed
typedef void(*StateControllerUnloadFunction)(DreamMugenStateController*);

static struct {
	map<string, StateControllerParseFunction> mStateControllerParsers; 
	map<int, StateControllerHandleFunction> mStateControllerHandlers;
	map<int, StateControllerUnloadFunction> mStateControllerUnloaders; 
	MemoryStack* mMemoryStack;
} gMugenStateControllerVariableHandler;


typedef struct {

	char mName[100];
	DreamMugenAssignment** mTriggerRoot;
} TriggerParseCaller;

static void checkSingleElementForTrigger(void* tCaller, void* tData) {
	TriggerParseCaller* caller = (TriggerParseCaller*)tCaller;
	MugenDefScriptGroupElement* e = (MugenDefScriptGroupElement*)tData;

	if (strcmp(e->mName.data(), caller->mName)) return;

	char* text = getAllocatedMugenDefStringVariableAsElement(e);

	DreamMugenAssignment* trigger = parseDreamMugenAssignmentFromString(text);
	freeMemory(text);

	*caller->mTriggerRoot = makeDreamAndMugenAssignment(*caller->mTriggerRoot, trigger);
}

static int parseTriggerAndReturnIfFound(const char* tName, DreamMugenAssignment** tRoot, MugenDefScriptGroup* tGroup) {
	if (!stl_string_map_contains_array(tGroup->mElements, tName)) return 0;

	DreamMugenAssignment* triggerRoot = NULL;
	TriggerParseCaller caller;
	strcpy(caller.mName, tName);
	caller.mTriggerRoot = &triggerRoot;
	list_map(&tGroup->mOrderedElementList, checkSingleElementForTrigger, &caller);

	*tRoot = makeDreamOrMugenAssignment(*tRoot, triggerRoot);

	return 1;
}

static int parseNumberedTriggerAndReturnIfFound(int i, DreamMugenAssignment** tRoot, MugenDefScriptGroup* tGroup) {
	char name[100];
	sprintf(name, "trigger%d", i);

	return parseTriggerAndReturnIfFound(name, tRoot, tGroup);
}

static void parseStateControllerTriggers(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	DreamMugenAssignment* allRoot = NULL;
	parseTriggerAndReturnIfFound("triggerall", &allRoot, tGroup);

	DreamMugenAssignment* root = NULL;
	int i = 1;
	while (parseNumberedTriggerAndReturnIfFound(i, &root, tGroup)) i++;

	root = makeDreamAndMugenAssignment(allRoot, root);
	if (!root) {
		root = makeDreamTrueMugenAssignment();
	}
	tController->mTrigger.mAssignment = root;
}

static void* allocMemoryOnMemoryStackOrMemory(uint32_t tSize) {
	if (gMugenStateControllerVariableHandler.mMemoryStack && canFitOnMemoryStack(gMugenStateControllerVariableHandler.mMemoryStack, tSize)) return allocMemoryOnMemoryStack(gMugenStateControllerVariableHandler.mMemoryStack, tSize);
	else return allocMemory(tSize);
}

typedef struct {
	DreamMugenAssignment* x;
	DreamMugenAssignment* y;

	uint8_t mIsSettingX;
	uint8_t mIsSettingY;
} Set2DPhysicsController;


static void parse2DPhysicsController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup, DreamMugenStateControllerType tType) {
	Set2DPhysicsController* e = (Set2DPhysicsController*)allocMemoryOnMemoryStackOrMemory(sizeof(Set2DPhysicsController));
	
	e->mIsSettingX = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("x", tGroup, &e->x);
	e->mIsSettingY = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("y", tGroup, &e->y);

	tController->mType = tType;
	tController->mData = e;
}

static void unload2DPhysicsController(DreamMugenStateController* tController) {
	Set2DPhysicsController* e = (Set2DPhysicsController*)tController->mData;
	if (e->mIsSettingX) {
		destroyDreamMugenAssignment(e->x);
	}
	if (e->mIsSettingY) {
		destroyDreamMugenAssignment(e->y);
	}
	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mState;
	DreamMugenAssignment* mControl;
	DreamMugenAssignment* mAnimation;

	uint8_t mIsChangingControl;
	uint8_t mIsChangingAnimation;
} ChangeStateController;


static void parseChangeStateController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup, DreamMugenStateControllerType tType) {
	ChangeStateController* e = (ChangeStateController*)allocMemoryOnMemoryStackOrMemory(sizeof(ChangeStateController));
	fetchDreamAssignmentFromGroupAndReturnWhetherItExists("value", tGroup, &e->mState);

	e->mIsChangingControl = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("ctrl", tGroup, &e->mControl);
	e->mIsChangingAnimation = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("anim", tGroup, &e->mAnimation);

	tController->mType = tType;
	tController->mData = e;
}

static void unloadChangeStateController(DreamMugenStateController* tController) {
	ChangeStateController* e = (ChangeStateController*)tController->mData;
	destroyDreamMugenAssignment(e->mState);

	if (e->mIsChangingControl) {
		destroyDreamMugenAssignment(e->mControl);
	} 

	if (e->mIsChangingAnimation) {
		destroyDreamMugenAssignment(e->mAnimation);
	}

	freeMemory(e);
}

static void fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString(const char* tName, MugenDefScriptGroup* tGroup, DreamMugenAssignment** tDst, const char* tDefault = NULL) {
	if (!fetchDreamAssignmentFromGroupAndReturnWhetherItExists(tName, tGroup, tDst)) {
		if (tDefault) {
			*tDst = makeDreamStringMugenAssignment(tDefault);
		}
		else {
			*tDst = NULL;
		}
	}
}

typedef struct {
	DreamMugenAssignment* mState;
	DreamMugenAssignment* mControl;
	DreamMugenAssignment* mID;

	uint8_t mIsChangingControl;
} TargetChangeStateController;

static void parseTargetChangeStateController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	TargetChangeStateController* e = (TargetChangeStateController*)allocMemoryOnMemoryStackOrMemory(sizeof(TargetChangeStateController));
	fetchDreamAssignmentFromGroupAndReturnWhetherItExists("value", tGroup, &e->mState);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);

	e->mIsChangingControl = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("ctrl", tGroup, &e->mControl);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_SET_TARGET_STATE;
	tController->mData = e;
}

static void unloadTargetChangeStateController(DreamMugenStateController* tController) {
	TargetChangeStateController* e = (TargetChangeStateController*)tController->mData;
	destroyDreamMugenAssignment(e->mState);
	destroyDreamMugenAssignment(e->mID);

	if (e->mIsChangingControl) {
		destroyDreamMugenAssignment(e->mControl);
	}

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mAttribute;
	DreamMugenAssignment* mHitFlag;

	DreamMugenAssignment* mGuardFlag;
	DreamMugenAssignment* mAffectTeam;
	DreamMugenAssignment* mAnimationType;
	DreamMugenAssignment* mAirAnimationType;
	DreamMugenAssignment* mFallAnimationType;

	DreamMugenAssignment* mPriority;
	DreamMugenAssignment* mDamage;
	DreamMugenAssignment* mPauseTime;
	DreamMugenAssignment* mGuardPauseTime;
	DreamMugenAssignment* mSparkNumber;
	DreamMugenAssignment* mGuardSparkNumber;

	DreamMugenAssignment* mSparkXY;
	DreamMugenAssignment* mHitSound;
	DreamMugenAssignment* mGuardSound;
	DreamMugenAssignment* mGroundType;
	DreamMugenAssignment* mAirType;

	DreamMugenAssignment* mGroundSlideTime;
	DreamMugenAssignment* mGuardSlideTime;
	DreamMugenAssignment* mGroundHitTime;
	DreamMugenAssignment* mGuardHitTime;
	DreamMugenAssignment* mAirHitTime;

	DreamMugenAssignment* mGuardControlTime;
	DreamMugenAssignment* mGuardDistance;
	DreamMugenAssignment* mYAccel;
	DreamMugenAssignment* mGroundVelocity;
	DreamMugenAssignment* mGuardVelocity;
	DreamMugenAssignment* mAirVelocity;
	DreamMugenAssignment* mAirGuardVelocity;

	DreamMugenAssignment* mGroundCornerPushVelocityOffset;
	DreamMugenAssignment* mAirCornerPushVelocityOffset;
	DreamMugenAssignment* mDownCornerPushVelocityOffset;
	DreamMugenAssignment* mGuardCornerPushVelocityOffset;
	DreamMugenAssignment* mAirGuardCornerPushVelocityOffset;

	DreamMugenAssignment* mAirGuardControlTime;
	DreamMugenAssignment* mAirJuggle;
	DreamMugenAssignment* mMinimumDistance;
	DreamMugenAssignment* mMaximumDistance;
	DreamMugenAssignment* mSnap;

	DreamMugenAssignment* mPlayerSpritePriority1;
	DreamMugenAssignment* mPlayerSpritePriority2;

	DreamMugenAssignment* mPlayer1ChangeFaceDirection;
	DreamMugenAssignment* mPlayer1ChangeFaceDirectionRelativeToPlayer2;
	DreamMugenAssignment* mPlayer2ChangeFaceDirectionRelativeToPlayer1;

	DreamMugenAssignment* mPlayer1StateNumber;
	DreamMugenAssignment* mPlayer2StateNumber;
	DreamMugenAssignment* mPlayer2CapableOfGettingPlayer1State;
	DreamMugenAssignment* mForceStanding;

	DreamMugenAssignment* mFall;
	DreamMugenAssignment* mFallXVelocity;
	DreamMugenAssignment* mFallYVelocity;
	DreamMugenAssignment* mFallCanBeRecovered;
	DreamMugenAssignment* mFallRecoveryTime;
	DreamMugenAssignment* mFallDamage;

	DreamMugenAssignment* mAirFall;
	DreamMugenAssignment* mForceNoFall;
	DreamMugenAssignment* mDownVelocity;
	DreamMugenAssignment* mDownHitTime;
	DreamMugenAssignment* mDownBounce;

	DreamMugenAssignment* mHitID;
	DreamMugenAssignment* mChainID;
	DreamMugenAssignment* mNoChainID;
	DreamMugenAssignment* mHitOnce;

	DreamMugenAssignment* mKill;
	DreamMugenAssignment* mGuardKill;
	DreamMugenAssignment* mFallKill;
	DreamMugenAssignment* mNumberOfHits;

	DreamMugenAssignment* mGetPower;
	DreamMugenAssignment* mGivePower;

	DreamMugenAssignment* mPaletteEffectTime;
	DreamMugenAssignment* mPaletteEffectMultiplication;
	DreamMugenAssignment* mPaletteEffectAddition;

	DreamMugenAssignment* mEnvironmentShakeTime;
	DreamMugenAssignment* mEnvironmentShakeFrequency;
	DreamMugenAssignment* mEnvironmentShakeAmplitude;
	DreamMugenAssignment* mEnvironmentShakePhase;

	DreamMugenAssignment* mFallEnvironmentShakeTime;
	DreamMugenAssignment* mFallEnvironmentShakeFrequency;
	DreamMugenAssignment* mFallEnvironmentShakeAmplitude;
	DreamMugenAssignment* mFallEnvironmentShakePhase;
} HitDefinitionController;

static void readHitDefinitionFromGroup(HitDefinitionController* e, MugenDefScriptGroup* tGroup) {
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("attr", tGroup, &e->mAttribute, "s , na");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("hitflag", tGroup, &e->mHitFlag, "maf");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("guardflag", tGroup, &e->mGuardFlag);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("affectteam", tGroup, &e->mAffectTeam, "e");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("animtype", tGroup, &e->mAnimationType, "light");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("air.animtype", tGroup, &e->mAirAnimationType);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("fall.animtype", tGroup, &e->mFallAnimationType);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("priority", tGroup, &e->mPriority);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("damage", tGroup, &e->mDamage, "0 , 0");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("pausetime", tGroup, &e->mPauseTime, "0 , 0");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("guard.pausetime", tGroup, &e->mGuardPauseTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("sparkno", tGroup, &e->mSparkNumber);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("guard.sparkno", tGroup, &e->mGuardSparkNumber);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("sparkxy", tGroup, &e->mSparkXY, "0 , 0");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("hitsound", tGroup, &e->mHitSound);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("guardsound", tGroup, &e->mGuardSound);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("ground.type", tGroup, &e->mGroundType, "high");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("air.type", tGroup, &e->mAirType);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("ground.slidetime", tGroup, &e->mGroundSlideTime, "0");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("guard.slidetime", tGroup, &e->mGuardSlideTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("ground.hittime", tGroup, &e->mGroundHitTime, "0");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("guard.hittime", tGroup, &e->mGuardHitTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("air.hittime", tGroup, &e->mAirHitTime, "20");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("guard.ctrltime", tGroup, &e->mGuardControlTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("guard.dist", tGroup, &e->mGuardDistance);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("yaccel", tGroup, &e->mYAccel);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("ground.velocity", tGroup, &e->mGroundVelocity, "0 , 0");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("guard.velocity", tGroup, &e->mGuardVelocity);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("air.velocity", tGroup, &e->mAirVelocity, "0 , 0");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("airguard.velocity", tGroup, &e->mAirGuardVelocity);

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("ground.cornerpush.veloff", tGroup, &e->mGroundCornerPushVelocityOffset);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("air.cornerpush.veloff", tGroup, &e->mAirCornerPushVelocityOffset);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("down.cornerpush.veloff", tGroup, &e->mDownCornerPushVelocityOffset);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("guard.cornerpush.veloff", tGroup, &e->mGuardCornerPushVelocityOffset);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("airguard.cornerpush.veloff", tGroup, &e->mAirGuardCornerPushVelocityOffset);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("airguard.ctrltime", tGroup, &e->mAirGuardControlTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("air.juggle", tGroup, &e->mAirJuggle, "0");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("mindist", tGroup, &e->mMinimumDistance);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("maxdist", tGroup, &e->mMaximumDistance);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("snap", tGroup, &e->mSnap);

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("p1sprpriority", tGroup, &e->mPlayerSpritePriority1, "1");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("p2sprpriority", tGroup, &e->mPlayerSpritePriority2, "0");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("p1facing", tGroup, &e->mPlayer1ChangeFaceDirection);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("p1getp2facing", tGroup, &e->mPlayer1ChangeFaceDirectionRelativeToPlayer2, "0");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("p2facing", tGroup, &e->mPlayer2ChangeFaceDirectionRelativeToPlayer1, "0");

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("p1stateno", tGroup, &e->mPlayer1StateNumber, "-1");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("p2stateno", tGroup, &e->mPlayer2StateNumber, "-1");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("p2getp1state", tGroup, &e->mPlayer2CapableOfGettingPlayer1State, "1");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("forcestand", tGroup, &e->mForceStanding);

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("fall", tGroup, &e->mFall, "0");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("fall.xvelocity", tGroup, &e->mFallXVelocity, "0");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("fall.yvelocity", tGroup, &e->mFallYVelocity);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("fall.recover", tGroup, &e->mFallCanBeRecovered, "1");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("fall.recovertime", tGroup, &e->mFallRecoveryTime, "4");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("fall.damage", tGroup, &e->mFallDamage, "0");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("air.fall", tGroup, &e->mAirFall);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("forcenofall", tGroup, &e->mForceNoFall, "0");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("down.velocity", tGroup, &e->mDownVelocity);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("down.hittime", tGroup, &e->mDownHitTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("down.bounce", tGroup, &e->mDownBounce, "0");

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mHitID, "0");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("chainID", tGroup, &e->mChainID, "-1");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("nochainID", tGroup, &e->mNoChainID);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("hitonce", tGroup, &e->mHitOnce);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("kill", tGroup, &e->mKill, "1");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("guard.kill", tGroup, &e->mGuardKill, "1");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("fall.kill", tGroup, &e->mFallKill, "1");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("numhits", tGroup, &e->mNumberOfHits, "1");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("getpower", tGroup, &e->mGetPower);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("givepower", tGroup, &e->mGivePower);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("palfx.time", tGroup, &e->mPaletteEffectTime, "0");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("palfx.mul", tGroup, &e->mPaletteEffectMultiplication);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("palfx.add", tGroup, &e->mPaletteEffectAddition);

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("envshake.time", tGroup, &e->mEnvironmentShakeTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("envshake.freq", tGroup, &e->mEnvironmentShakeFrequency);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("envshake.ampl", tGroup, &e->mEnvironmentShakeAmplitude);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("envshake.phase", tGroup, &e->mEnvironmentShakePhase);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("fall.envshake.time", tGroup, &e->mFallEnvironmentShakeTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("fall.envshake.freq", tGroup, &e->mFallEnvironmentShakeFrequency);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("fall.envshake.ampl", tGroup, &e->mFallEnvironmentShakeAmplitude);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("fall.envshake.phase", tGroup, &e->mFallEnvironmentShakePhase);

}

static void parseHitDefinitionController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	HitDefinitionController* e = (HitDefinitionController*)allocMemoryOnMemoryStackOrMemory(sizeof(HitDefinitionController));
	readHitDefinitionFromGroup(e, tGroup);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_HIT_DEFINITION;
	tController->mData = e;
}

static void unloadHitDefinitionData(HitDefinitionController* e) {
	destroyDreamMugenAssignment(e->mAttribute);
	destroyDreamMugenAssignment(e->mHitFlag);

	destroyDreamMugenAssignment(e->mGuardFlag);
	destroyDreamMugenAssignment(e->mAffectTeam);
	destroyDreamMugenAssignment(e->mAnimationType);
	destroyDreamMugenAssignment(e->mAirAnimationType);
	destroyDreamMugenAssignment(e->mFallAnimationType);


	destroyDreamMugenAssignment(e->mPriority);
	destroyDreamMugenAssignment(e->mDamage);
	destroyDreamMugenAssignment(e->mPauseTime);
	destroyDreamMugenAssignment(e->mGuardPauseTime);
	destroyDreamMugenAssignment(e->mSparkNumber);
	destroyDreamMugenAssignment(e->mGuardSparkNumber);


	destroyDreamMugenAssignment(e->mSparkXY);
	destroyDreamMugenAssignment(e->mHitSound);
	destroyDreamMugenAssignment(e->mGuardSound);
	destroyDreamMugenAssignment(e->mGroundType);
	destroyDreamMugenAssignment(e->mAirType);

	destroyDreamMugenAssignment(e->mGroundSlideTime);
	destroyDreamMugenAssignment(e->mGuardSlideTime);
	destroyDreamMugenAssignment(e->mGroundHitTime);
	destroyDreamMugenAssignment(e->mGuardHitTime);
	destroyDreamMugenAssignment(e->mAirHitTime);

	destroyDreamMugenAssignment(e->mGuardControlTime);
	destroyDreamMugenAssignment(e->mGuardDistance);
	destroyDreamMugenAssignment(e->mYAccel);
	destroyDreamMugenAssignment(e->mGroundVelocity);
	destroyDreamMugenAssignment(e->mGuardVelocity);
	destroyDreamMugenAssignment(e->mAirVelocity);
	destroyDreamMugenAssignment(e->mAirGuardVelocity);

	destroyDreamMugenAssignment(e->mGroundCornerPushVelocityOffset);
	destroyDreamMugenAssignment(e->mAirCornerPushVelocityOffset);
	destroyDreamMugenAssignment(e->mDownCornerPushVelocityOffset);
	destroyDreamMugenAssignment(e->mGuardCornerPushVelocityOffset);
	destroyDreamMugenAssignment(e->mAirGuardCornerPushVelocityOffset);

	destroyDreamMugenAssignment(e->mAirGuardControlTime);
	destroyDreamMugenAssignment(e->mAirJuggle);
	destroyDreamMugenAssignment(e->mMinimumDistance);
	destroyDreamMugenAssignment(e->mMaximumDistance);
	destroyDreamMugenAssignment(e->mSnap);

	destroyDreamMugenAssignment(e->mPlayerSpritePriority1);
	destroyDreamMugenAssignment(e->mPlayerSpritePriority2);

	destroyDreamMugenAssignment(e->mPlayer1ChangeFaceDirection);
	destroyDreamMugenAssignment(e->mPlayer1ChangeFaceDirectionRelativeToPlayer2);
	destroyDreamMugenAssignment(e->mPlayer2ChangeFaceDirectionRelativeToPlayer1);

	destroyDreamMugenAssignment(e->mPlayer1StateNumber);
	destroyDreamMugenAssignment(e->mPlayer2StateNumber);
	destroyDreamMugenAssignment(e->mPlayer2CapableOfGettingPlayer1State);
	destroyDreamMugenAssignment(e->mForceStanding);

	destroyDreamMugenAssignment(e->mFall);
	destroyDreamMugenAssignment(e->mFallXVelocity);
	destroyDreamMugenAssignment(e->mFallYVelocity);
	destroyDreamMugenAssignment(e->mFallCanBeRecovered);
	destroyDreamMugenAssignment(e->mFallRecoveryTime);
	destroyDreamMugenAssignment(e->mFallDamage);

	destroyDreamMugenAssignment(e->mAirFall);
	destroyDreamMugenAssignment(e->mForceNoFall);
	destroyDreamMugenAssignment(e->mDownVelocity);
	destroyDreamMugenAssignment(e->mDownHitTime);
	destroyDreamMugenAssignment(e->mDownBounce);

	destroyDreamMugenAssignment(e->mHitID);
	destroyDreamMugenAssignment(e->mChainID);
	destroyDreamMugenAssignment(e->mNoChainID);
	destroyDreamMugenAssignment(e->mHitOnce);

	destroyDreamMugenAssignment(e->mKill);
	destroyDreamMugenAssignment(e->mGuardKill);
	destroyDreamMugenAssignment(e->mFallKill);
	destroyDreamMugenAssignment(e->mNumberOfHits);

	destroyDreamMugenAssignment(e->mGetPower);
	destroyDreamMugenAssignment(e->mGivePower);

	destroyDreamMugenAssignment(e->mPaletteEffectTime);
	destroyDreamMugenAssignment(e->mPaletteEffectMultiplication);
	destroyDreamMugenAssignment(e->mPaletteEffectAddition);

	destroyDreamMugenAssignment(e->mEnvironmentShakeTime);
	destroyDreamMugenAssignment(e->mEnvironmentShakeFrequency);
	destroyDreamMugenAssignment(e->mEnvironmentShakeAmplitude);
	destroyDreamMugenAssignment(e->mEnvironmentShakePhase);

	destroyDreamMugenAssignment(e->mFallEnvironmentShakeTime);
	destroyDreamMugenAssignment(e->mFallEnvironmentShakeFrequency);
	destroyDreamMugenAssignment(e->mFallEnvironmentShakeAmplitude);
	destroyDreamMugenAssignment(e->mFallEnvironmentShakePhase);
}

static void unloadHitDefinitionController(DreamMugenStateController* tController) {
	HitDefinitionController* e = (HitDefinitionController*)tController->mData;

	unloadHitDefinitionData(e);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mValue;

	DreamMugenAssignment* mVolumeScale;
	DreamMugenAssignment* mChannel;
	DreamMugenAssignment* mLowPriority;
	DreamMugenAssignment* mFrequencyMultiplier;
	DreamMugenAssignment* mLoop;
	DreamMugenAssignment* mPanning;
	DreamMugenAssignment* mAbsolutePanning;

} PlaySoundController;


static void parsePlaySoundController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	PlaySoundController* e = (PlaySoundController*)allocMemoryOnMemoryStackOrMemory(sizeof(PlaySoundController));

	fetchDreamAssignmentFromGroupAndReturnWhetherItExists("value", tGroup, &e->mValue);

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("volumescale", tGroup, &e->mVolumeScale);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("channel", tGroup, &e->mChannel);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("lowpriority", tGroup, &e->mLowPriority);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("freqmul", tGroup, &e->mFrequencyMultiplier);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("loop", tGroup, &e->mLoop);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("pan", tGroup, &e->mPanning);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("abspan", tGroup, &e->mAbsolutePanning);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_PLAY_SOUND;
	tController->mData = e;
}

static void unloadPlaySoundController(DreamMugenStateController* tController) {
	PlaySoundController* e = (PlaySoundController*)tController->mData;

	destroyDreamMugenAssignment(e->mValue);

	destroyDreamMugenAssignment(e->mVolumeScale);
	destroyDreamMugenAssignment(e->mChannel);
	destroyDreamMugenAssignment(e->mLowPriority);
	destroyDreamMugenAssignment(e->mFrequencyMultiplier);
	destroyDreamMugenAssignment(e->mLoop);
	destroyDreamMugenAssignment(e->mPanning);
	destroyDreamMugenAssignment(e->mAbsolutePanning);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mValue;

	DreamMugenAssignment* mEdge;
	DreamMugenAssignment* mPlayer;

	uint8_t mHasValue;
} WidthController;


static void parseWidthController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	WidthController* e = (WidthController*)allocMemoryOnMemoryStackOrMemory(sizeof(WidthController));
	
	e->mHasValue = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("value", tGroup, &e->mValue);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("edge", tGroup, &e->mEdge);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("player", tGroup, &e->mPlayer);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_WIDTH;
	tController->mData = e;
}

static void unloadWidthController(DreamMugenStateController* tController) {
	WidthController* e = (WidthController*)tController->mData;

	if (e->mHasValue) {
		destroyDreamMugenAssignment(e->mValue);
	}

	destroyDreamMugenAssignment(e->mEdge);
	destroyDreamMugenAssignment(e->mPlayer);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* tNewAnimation;

	DreamMugenAssignment* tStep;
} ChangeAnimationController;


static void parseChangeAnimationController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup, DreamMugenStateControllerType tType) {
	ChangeAnimationController* e = (ChangeAnimationController*)allocMemoryOnMemoryStackOrMemory(sizeof(ChangeAnimationController));

	fetchDreamAssignmentFromGroupAndReturnWhetherItExists("value", tGroup, &e->tNewAnimation);


	if (!fetchDreamAssignmentFromGroupAndReturnWhetherItExists("elem", tGroup, &e->tStep)) {
		e->tStep = makeDreamNumberMugenAssignment(0);
	}

	tController->mType = tType;
	tController->mData = e;
}

static void unloadChangeAnimationController(DreamMugenStateController* tController) {
	ChangeAnimationController* e = (ChangeAnimationController*)tController->mData;

	destroyDreamMugenAssignment(e->tNewAnimation);
	destroyDreamMugenAssignment(e->tStep);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* tValue;
} ControlSettingController;


static void parseControlSettingController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	ControlSettingController* e = (ControlSettingController*)allocMemoryOnMemoryStackOrMemory(sizeof(ControlSettingController));

	fetchDreamAssignmentFromGroupAndReturnWhetherItExists("value", tGroup, &e->tValue);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_SET_CONTROL;
	tController->mData = e;
}

static void unloadControlSettingController(DreamMugenStateController* tController) {
	ControlSettingController* e = (ControlSettingController*)tController->mData;

	destroyDreamMugenAssignment(e->tValue);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* tValue;
} SpritePriorityController;


static void parseSpritePriorityController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	SpritePriorityController* e = (SpritePriorityController*)allocMemoryOnMemoryStackOrMemory(sizeof(SpritePriorityController));

	fetchDreamAssignmentFromGroupAndReturnWhetherItExists("value", tGroup, &e->tValue);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_SPRITE_PRIORITY;
	tController->mData = e;
}

static void unloadSpritePriorityController(DreamMugenStateController* tController) {
	SpritePriorityController* e = (SpritePriorityController*)tController->mData;

	destroyDreamMugenAssignment(e->tValue);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mFlag;
	DreamMugenAssignment* mFlag2;
	DreamMugenAssignment* mFlag3;

	uint8_t mHasFlag2;
	uint8_t mHasFlag3;
} SpecialAssertController;


static void parseSpecialAssertController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	SpecialAssertController* e = (SpecialAssertController*)allocMemoryOnMemoryStackOrMemory(sizeof(SpecialAssertController));

	fetchDreamAssignmentFromGroupAndReturnWhetherItExists("flag", tGroup, &e->mFlag);
	e->mHasFlag2 = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("flag2", tGroup, &e->mFlag2);
	e->mHasFlag3 = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("flag3", tGroup, &e->mFlag3);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_ASSERT_SPECIAL;
	tController->mData = e;
}

static void unloadSpecialAssertController(DreamMugenStateController* tController) {
	SpecialAssertController* e = (SpecialAssertController*)tController->mData;

	destroyDreamMugenAssignment(e->mFlag);
	if (e->mHasFlag2) {
		destroyDreamMugenAssignment(e->mFlag2);
	}
	if (e->mHasFlag3) {
		destroyDreamMugenAssignment(e->mFlag3);
	}

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mPositionOffset;
	DreamMugenAssignment* mPositionOffset2;
	DreamMugenAssignment* mSpacing;

	uint8_t mHasSecondDustCloud;
} MakeDustController;


static void parseMakeDustController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	MakeDustController* e = (MakeDustController*)allocMemoryOnMemoryStackOrMemory(sizeof(MakeDustController));

	if (!fetchDreamAssignmentFromGroupAndReturnWhetherItExists("pos", tGroup, &e->mPositionOffset)) {
		e->mPositionOffset = makeDream2DVectorMugenAssignment(makePosition(0, 0, 0));
	}

	e->mHasSecondDustCloud = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("pos2", tGroup, &e->mPositionOffset2);

	if (!fetchDreamAssignmentFromGroupAndReturnWhetherItExists("spacing", tGroup, &e->mSpacing)) {
		e->mSpacing = makeDreamNumberMugenAssignment(1);
	}

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_MAKE_DUST;
	tController->mData = e;
}

static void unloadMakeDustController(DreamMugenStateController* tController) {
	MakeDustController* e = (MakeDustController*)tController->mData;

	destroyDreamMugenAssignment(e->mPositionOffset);
	if (e->mHasSecondDustCloud) {
		destroyDreamMugenAssignment(e->mPositionOffset2);
	}
	destroyDreamMugenAssignment(e->mSpacing);

	freeMemory(e);
}

typedef struct {
	uint8_t mDummy;
} NullController;


static void parseNullController(DreamMugenStateController* tController, DreamMugenStateControllerType tType) {
	NullController* e = (NullController*)allocMemoryOnMemoryStackOrMemory(sizeof(NullController));
	e->mDummy = 0;

	tController->mType = tType;
	tController->mData = e;
}

static void unloadNullController(DreamMugenStateController* tController) {
	freeMemory(tController->mData);
}

enum VarSetType : uint8_t {
	VAR_SET_TYPE_SYSTEM,
	VAR_SET_TYPE_SYSTEM_FLOAT,
	VAR_SET_TYPE_INTEGER,
	VAR_SET_TYPE_FLOAT
};

typedef struct {
	DreamMugenAssignment* mID;
	DreamMugenAssignment* mAssignment;

	uint8_t mType;
} VarSetControllerEntry;

typedef struct {
	Vector mVarSets;
} VarSetController;

typedef struct {
	VarSetController* mController;
	MugenDefScriptGroup* mGroup;
} VarSetControllerCaller;

static void parseSingleVarSetControllerEntry(VarSetControllerCaller* tCaller, const string& tName, MugenDefScriptGroupElement& /*tElement*/) {
	VarSetControllerCaller* caller = (VarSetControllerCaller*)tCaller;

	int isEntry = tName.find('(') != string::npos;
	if (!isEntry) return;

	VarSetControllerEntry* e = (VarSetControllerEntry*)allocMemoryOnMemoryStackOrMemory(sizeof(VarSetControllerEntry));

	char name[100];
	strcpy(name, tName.data());
	char* start = strchr(name, '(');
	assert(start != NULL);
	*start = '\0';

	char value[100];
	strcpy(value, start + 1);
	char* end = strchr(value, ')');
	assert(end != NULL);
	*end = '\0';


	if (!strcmp("var", name)) {
		e->mType = VAR_SET_TYPE_INTEGER;
	}
	else if (!strcmp("sysvar", name)) {
		e->mType = VAR_SET_TYPE_SYSTEM;
	}
	else if (!strcmp("sysfvar", name)) {
		e->mType = VAR_SET_TYPE_SYSTEM_FLOAT;
	}
	else if (!strcmp("fvar", name)) {
		e->mType = VAR_SET_TYPE_FLOAT;
	}
	else {
		logWarningFormat("Unrecognized variable setting name %s. Default to var.", name);
		e->mType = VAR_SET_TYPE_INTEGER;
	}

	e->mID = makeDreamNumberMugenAssignment(atoi(value));

	fetchDreamAssignmentFromGroupAndReturnWhetherItExists(tName.data(), caller->mGroup, &e->mAssignment);

	vector_push_back_owned(&caller->mController->mVarSets, e);
}

static void loadSingleOriginalVarSetController(Vector* tDst, MugenDefScriptGroup* tGroup, MugenDefScriptGroupElement* tIDElement, VarSetType tType) {
	VarSetControllerEntry* e = (VarSetControllerEntry*)allocMemoryOnMemoryStackOrMemory(sizeof(VarSetControllerEntry));
	e->mType = tType;
	fetchDreamAssignmentFromGroupAsElement(tIDElement, &e->mID);
	fetchDreamAssignmentFromGroupAndReturnWhetherItExists("value", tGroup, &e->mAssignment);

	vector_push_back_owned(tDst, e);
}

static void unloadSingleVarSetEntry(void* tCaller, void* tData) {
	(void)tCaller;
	VarSetControllerEntry* e = (VarSetControllerEntry*)tData;
	destroyDreamMugenAssignment(e->mAssignment);
}

static void parseVarSetController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup, DreamMugenStateControllerType tType) {
	int isIntegerVersion = stl_string_map_contains_array(tGroup->mElements, "v");
	int isFloatVersion = stl_string_map_contains_array(tGroup->mElements, "fv");

	VarSetController* e = (VarSetController*)allocMemoryOnMemoryStackOrMemory(sizeof(VarSetController));
	e->mVarSets = new_vector();

	if (isIntegerVersion) {
		loadSingleOriginalVarSetController(&e->mVarSets, tGroup, &tGroup->mElements["v"], VAR_SET_TYPE_INTEGER);
	}
	else if (isFloatVersion) {
		loadSingleOriginalVarSetController(&e->mVarSets, tGroup, &tGroup->mElements["fv"], VAR_SET_TYPE_FLOAT);
	}
	else {
		VarSetControllerCaller caller;
		caller.mController = e;
		caller.mGroup = tGroup;

		stl_string_map_map(tGroup->mElements, parseSingleVarSetControllerEntry, &caller);
	}

	if (vector_size(&e->mVarSets) != 1) {
		logWarning("Unable to parse VarSetController. Missing elements. Defaulting to Null controller.");
		vector_map(&e->mVarSets, unloadSingleVarSetEntry, NULL);
		delete_vector(&e->mVarSets);
		//freeMemory(e); // TOOD: free _maybe_?
		parseNullController(tController, MUGEN_STATE_CONTROLLER_TYPE_NULL);
		return;
	}

	tController->mType = tType;
	tController->mData = e;
}



static void unloadVarSetController(DreamMugenStateController* tController) {
	VarSetController* e = (VarSetController*)tController->mData;
	vector_map(&e->mVarSets, unloadSingleVarSetEntry, NULL);
	delete_vector(&e->mVarSets);
	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mValue;
	DreamMugenAssignment* mFirst;
	DreamMugenAssignment* mLast;

	uint8_t mType;
} VarRangeSetController;

static void parseVarRangeSetController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	VarRangeSetController* e = (VarRangeSetController*)allocMemoryOnMemoryStackOrMemory(sizeof(VarRangeSetController));

	int isIntegerVersion = stl_string_map_contains_array(tGroup->mElements, "value");

	if (isIntegerVersion) {
		e->mType = VAR_SET_TYPE_INTEGER;
		fetchDreamAssignmentFromGroupAndReturnWhetherItExists("value", tGroup, &e->mValue);
	}
	else {
		e->mType = VAR_SET_TYPE_FLOAT;
		fetchDreamAssignmentFromGroupAndReturnWhetherItExists("fvalue", tGroup, &e->mValue);
	}

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("first", tGroup, &e->mFirst);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("last", tGroup, &e->mLast);
 
	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_SET_VARIABLE_RANGE;
	tController->mData = e;
}

static void unloadVarRangeSetController(DreamMugenStateController* tController) {
	VarRangeSetController* e = (VarRangeSetController*)tController->mData;
	destroyDreamMugenAssignment(e->mValue);
	destroyDreamMugenAssignment(e->mFirst);
	destroyDreamMugenAssignment(e->mLast);
	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mStateType;
	DreamMugenAssignment* mMoveType;
	DreamMugenAssignment* mPhysics;

	uint8_t mHasStateType;
	uint8_t mHasMoveType;
	uint8_t mHasPhysics;
} StateTypeSetController;


static void parseStateTypeSetController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	StateTypeSetController* e = (StateTypeSetController*)allocMemoryOnMemoryStackOrMemory(sizeof(StateTypeSetController));

	e->mHasStateType = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("statetype", tGroup, &e->mStateType);
	e->mHasMoveType = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("movetype", tGroup, &e->mMoveType);
	e->mHasPhysics = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("physics", tGroup, &e->mPhysics);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_SET_STATE_TYPE;
	tController->mData = e;
}

static void unloadStateTypeSetController(DreamMugenStateController* tController) {
	StateTypeSetController* e = (StateTypeSetController*)tController->mData;

	if (e->mHasStateType) {
		destroyDreamMugenAssignment(e->mStateType);
	}
	if (e->mHasMoveType) {
		destroyDreamMugenAssignment(e->mMoveType);
	}
	if (e->mHasPhysics) {
		destroyDreamMugenAssignment(e->mPhysics);
	}

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mWaveform;
	DreamMugenAssignment* mTime;
	DreamMugenAssignment* mFrequency;
	DreamMugenAssignment* mAmplitude;
	DreamMugenAssignment* mSelf;

} ForceFeedbackController;

static void parseForceFeedbackController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	ForceFeedbackController*e = (ForceFeedbackController*)allocMemoryOnMemoryStackOrMemory(sizeof(ForceFeedbackController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("waveform", tGroup, &e->mWaveform, "sine");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("time", tGroup, &e->mTime, "sine");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("freq", tGroup, &e->mFrequency, "sine");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("ampl", tGroup, &e->mAmplitude, "sine");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("self", tGroup, &e->mSelf, "sine");

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_FORCE_FEEDBACK;
	tController->mData = e;
}

static void unloadForceFeedbackController(DreamMugenStateController* tController) {
	ForceFeedbackController*e = (ForceFeedbackController*)tController->mData;

	destroyDreamMugenAssignment(e->mWaveform);
	destroyDreamMugenAssignment(e->mTime);
	destroyDreamMugenAssignment(e->mFrequency);
	destroyDreamMugenAssignment(e->mAmplitude);
	destroyDreamMugenAssignment(e->mSelf);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mValue;
} DefenseMultiplierController;

static void parseDefenseMultiplierController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	DefenseMultiplierController* e = (DefenseMultiplierController*)allocMemoryOnMemoryStackOrMemory(sizeof(DefenseMultiplierController));

	fetchDreamAssignmentFromGroupAndReturnWhetherItExists("value", tGroup, &e->mValue);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_SET_DEFENSE_MULTIPLIER;
	tController->mData = e;
}

static void unloadDefenseMultiplierController(DreamMugenStateController* tController) {
	DefenseMultiplierController* e = (DefenseMultiplierController*)tController->mData;
	destroyDreamMugenAssignment(e->mValue);
	freeMemory(e);
}


typedef struct {
	DreamMugenAssignment* mAnim;

	DreamMugenAssignment* mID;
	DreamMugenAssignment* mPosition;
	DreamMugenAssignment* mPositionType;
	DreamMugenAssignment* mHorizontalFacing;
	DreamMugenAssignment* mVerticalFacing;
	DreamMugenAssignment* mBindTime;
	DreamMugenAssignment* mVelocity;
	DreamMugenAssignment* mAcceleration;
	DreamMugenAssignment* mRandomOffset;
	DreamMugenAssignment* mRemoveTime;
	DreamMugenAssignment* mSuperMove;
	DreamMugenAssignment* mSuperMoveTime;
	DreamMugenAssignment* mPauseMoveTime;
	DreamMugenAssignment* mScale;
	DreamMugenAssignment* mSpritePriority;
	DreamMugenAssignment* mOnTop;
	DreamMugenAssignment* mShadow;
	DreamMugenAssignment* mOwnPalette;
	DreamMugenAssignment* mIsRemovedOnGetHit;
	DreamMugenAssignment* mIgnoreHitPause;
	DreamMugenAssignment* mTransparencyType;

	uint8_t mHasTransparencyType;
} ExplodController;

static void parseExplodController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	ExplodController* e = (ExplodController*)allocMemoryOnMemoryStackOrMemory(sizeof(ExplodController));

	fetchDreamAssignmentFromGroupAndReturnWhetherItExists("anim", tGroup, &e->mAnim);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("pos", tGroup, &e->mPosition);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("postype", tGroup, &e->mPositionType, "p1");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("facing", tGroup, &e->mHorizontalFacing);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("vfacing", tGroup, &e->mVerticalFacing);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("bindtime", tGroup, &e->mBindTime);
	if (stl_string_map_contains_array(tGroup->mElements, "vel")) {
		fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("vel", tGroup, &e->mVelocity);
	}
	else {
		fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("velocity", tGroup, &e->mVelocity);
	}
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("accel", tGroup, &e->mAcceleration);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("random", tGroup, &e->mRandomOffset);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("removetime", tGroup, &e->mRemoveTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("supermove", tGroup, &e->mSuperMove);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("supermovetime", tGroup, &e->mSuperMoveTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("pausemovetime", tGroup, &e->mPauseMoveTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("scale", tGroup, &e->mScale);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("sprpriority", tGroup, &e->mSpritePriority);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("ontop", tGroup, &e->mOnTop);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("shadow", tGroup, &e->mShadow);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("ownpal", tGroup, &e->mOwnPalette);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("removeongethit", tGroup, &e->mIsRemovedOnGetHit);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("ignorehitpause", tGroup, &e->mIgnoreHitPause);
	e->mHasTransparencyType = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("trans", tGroup, &e->mTransparencyType);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_EXPLOD;
	tController->mData = e;
}

static void unloadExplodController(DreamMugenStateController* tController) {
	ExplodController* e = (ExplodController*)tController->mData;

	destroyDreamMugenAssignment(e->mAnim);

	destroyDreamMugenAssignment(e->mID);
	destroyDreamMugenAssignment(e->mPosition);
	destroyDreamMugenAssignment(e->mPositionType);
	destroyDreamMugenAssignment(e->mHorizontalFacing);
	destroyDreamMugenAssignment(e->mVerticalFacing);
	destroyDreamMugenAssignment(e->mBindTime);
	destroyDreamMugenAssignment(e->mVelocity);
	destroyDreamMugenAssignment(e->mAcceleration);
	destroyDreamMugenAssignment(e->mRandomOffset);
	destroyDreamMugenAssignment(e->mRemoveTime);
	destroyDreamMugenAssignment(e->mSuperMove);
	destroyDreamMugenAssignment(e->mSuperMoveTime);
	destroyDreamMugenAssignment(e->mPauseMoveTime);
	destroyDreamMugenAssignment(e->mScale);
	destroyDreamMugenAssignment(e->mSpritePriority);
	destroyDreamMugenAssignment(e->mOnTop);
	destroyDreamMugenAssignment(e->mShadow);
	destroyDreamMugenAssignment(e->mOwnPalette);
	destroyDreamMugenAssignment(e->mIsRemovedOnGetHit);
	destroyDreamMugenAssignment(e->mIgnoreHitPause);

	if (e->mHasTransparencyType) {
		destroyDreamMugenAssignment(e->mTransparencyType);
	}

	freeMemory(e);
}

static void parseModifyExplodController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	ExplodController* e = (ExplodController*)allocMemoryOnMemoryStackOrMemory(sizeof(ExplodController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("anim", tGroup, &e->mAnim);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("pos", tGroup, &e->mPosition);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("postype", tGroup, &e->mPositionType, "p1");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("facing", tGroup, &e->mHorizontalFacing);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("vfacing", tGroup, &e->mVerticalFacing);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("bindtime", tGroup, &e->mBindTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("vel", tGroup, &e->mVelocity);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("accel", tGroup, &e->mAcceleration);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("random", tGroup, &e->mRandomOffset);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("removetime", tGroup, &e->mRemoveTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("supermove", tGroup, &e->mSuperMove);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("supermovetime", tGroup, &e->mSuperMoveTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("pausemovetime", tGroup, &e->mPauseMoveTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("scale", tGroup, &e->mScale);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("sprpriority", tGroup, &e->mSpritePriority);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("ontop", tGroup, &e->mOnTop);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("shadow", tGroup, &e->mShadow);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("ownpal", tGroup, &e->mOwnPalette);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("removeongethit", tGroup, &e->mIsRemovedOnGetHit);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("ignorehitpause", tGroup, &e->mIgnoreHitPause);
	e->mHasTransparencyType = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("trans", tGroup, &e->mTransparencyType);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_MODIFY_EXPLOD;
	tController->mData = e;
}

static void unloadModifyExplodController(DreamMugenStateController* tController) {
	unloadExplodController(tController);
}

typedef struct {
	DreamMugenAssignment* mValue;
} PositionFreezeController;

static void parsePositionFreezeController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	PositionFreezeController* e = (PositionFreezeController*)allocMemoryOnMemoryStackOrMemory(sizeof(PositionFreezeController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("value", tGroup, &e->mValue, "1");

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_FREEZE_POSITION;
	tController->mData = e;
}

static void unloadPositionFreezeController(DreamMugenStateController* tController) {
	PositionFreezeController* e = (PositionFreezeController*)tController->mData;
	destroyDreamMugenAssignment(e->mValue);
	freeMemory(e);
}

typedef struct {
	MugenStringVector mValue;
	MugenStringVector mValue2;
	DreamMugenAssignment* mTime;

	uint8_t mHasValue;
	uint8_t mHasValue2;
} NotHitByController;

static void readMugenDefStringVector(MugenStringVector* tDst, MugenDefScriptGroup* tGroup, const char* tName, uint8_t* oHasValue) {
	if (stl_string_map_contains_array(tGroup->mElements, tName)) {
		MugenDefScriptGroupElement* elem = &tGroup->mElements[tName];
		*oHasValue = 1;
		if (isMugenDefStringVectorVariableAsElement(elem)) {
			*tDst = copyMugenDefStringVectorVariableAsElement(elem);
		}
		else {
			char* text = getAllocatedMugenDefStringVariableAsElement(elem);
			tDst->mSize = 1;
			tDst->mElement = (char**)allocMemoryOnMemoryStackOrMemory(sizeof(char*));
			tDst->mElement[0] = (char*)allocMemoryOnMemoryStackOrMemory(strlen(text) + 10);
			strcpy(tDst->mElement[0], text);
			freeMemory(text);
		}
	}
	else {
		*oHasValue = 0;
	}

}

static void parseNotHitByController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup, DreamMugenStateControllerType tType) {
	NotHitByController* e = (NotHitByController*)allocMemoryOnMemoryStackOrMemory(sizeof(NotHitByController));

	readMugenDefStringVector(&e->mValue, tGroup, "value", &e->mHasValue);
	readMugenDefStringVector(&e->mValue2, tGroup, "value2", &e->mHasValue2);
	assert(e->mHasValue || e->mHasValue2);

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("time", tGroup, &e->mTime, "1");
	
	tController->mType = tType;
	tController->mData = e;
}

static void unloadMugenDefStringVector(MugenStringVector tVector) {
	int i;
	for (i = 0; i < tVector.mSize; i++) {
		freeMemory(tVector.mElement[i]);
	}

	freeMemory(tVector.mElement);
}

static void unloadNotHitByController(DreamMugenStateController* tController) {
	NotHitByController* e = (NotHitByController*)tController->mData;

	if (e->mHasValue) {
		unloadMugenDefStringVector(e->mValue);
	}
	if (e->mHasValue2) {
		unloadMugenDefStringVector(e->mValue2);
	}
	destroyDreamMugenAssignment(e->mTime);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mValue;
	DreamMugenAssignment* mXVelocity;
	DreamMugenAssignment* mYVelocity;

	uint8_t mHasXVelocity;
	uint8_t mHasYVelocity;
} HitFallSetController;

static void parseHitFallSetController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	HitFallSetController* e = (HitFallSetController*)allocMemoryOnMemoryStackOrMemory(sizeof(HitFallSetController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("value", tGroup, &e->mValue, "-1");
	e->mHasXVelocity = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("xvel", tGroup, &e->mXVelocity);
	e->mHasYVelocity = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("yvel", tGroup, &e->mYVelocity);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_SET_HIT_FALL;
	tController->mData = e;
}

static void unloadHitFallSetController(DreamMugenStateController* tController) {
	HitFallSetController* e = (HitFallSetController*)tController->mData;

	destroyDreamMugenAssignment(e->mValue);
	if (e->mHasXVelocity) {
		destroyDreamMugenAssignment(e->mXVelocity);
	}
	if (e->mHasYVelocity) {
		destroyDreamMugenAssignment(e->mYVelocity);
	}

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mValue;
} SingleRequiredValueController;

static void parseSingleRequiredValueController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup, DreamMugenStateControllerType tType) {
	SingleRequiredValueController* e = (SingleRequiredValueController*)allocMemoryOnMemoryStackOrMemory(sizeof(SingleRequiredValueController));

	fetchDreamAssignmentFromGroupAndReturnWhetherItExists("value", tGroup, &e->mValue);

	tController->mType = tType;
	tController->mData = e;
}

static void unloadSingleRequiredValueController(DreamMugenStateController* tController) {
	SingleRequiredValueController* e = (SingleRequiredValueController*)tController->mData;
	destroyDreamMugenAssignment(e->mValue);
	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mTime;

	DreamMugenAssignment* mFrequency;
	DreamMugenAssignment* mAmplitude;
	DreamMugenAssignment* mPhaseOffset;
} EnvironmentShakeController;

static void parseEnvironmentShakeController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	EnvironmentShakeController* e = (EnvironmentShakeController*)allocMemoryOnMemoryStackOrMemory(sizeof(EnvironmentShakeController));

	fetchDreamAssignmentFromGroupAndReturnWhetherItExists("time", tGroup, &e->mTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("freq", tGroup, &e->mFrequency);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("ampl", tGroup, &e->mAmplitude);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("phase", tGroup, &e->mPhaseOffset);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_ENVIRONMENT_SHAKE;
	tController->mData = e;
}

static void unloadEnvironmentShakeController(DreamMugenStateController* tController) {
	EnvironmentShakeController* e = (EnvironmentShakeController*)tController->mData;

	destroyDreamMugenAssignment(e->mTime);
	destroyDreamMugenAssignment(e->mFrequency);
	destroyDreamMugenAssignment(e->mAmplitude);
	destroyDreamMugenAssignment(e->mPhaseOffset);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mTime;
	DreamMugenAssignment* mBufferTimeForCommandsDuringPauseEnd;
	DreamMugenAssignment* mMoveTime;
	DreamMugenAssignment* mDoesPauseBackground;

	DreamMugenAssignment* mAnim;
	DreamMugenAssignment* mSound;
	DreamMugenAssignment* mPosition;
	DreamMugenAssignment* mIsDarkening;
	DreamMugenAssignment* mPlayer2DefenseMultiplier;
	DreamMugenAssignment* mPowerToAdd;
	DreamMugenAssignment* mSetPlayerUnhittable;

} SuperPauseController;

static void parseSuperPauseController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	SuperPauseController* e = (SuperPauseController*)allocMemoryOnMemoryStackOrMemory(sizeof(SuperPauseController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("time", tGroup, &e->mTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("endcmdbuftime", tGroup, &e->mBufferTimeForCommandsDuringPauseEnd);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("movetime", tGroup, &e->mMoveTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("pausebg", tGroup, &e->mDoesPauseBackground);

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("anim", tGroup, &e->mAnim);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("sound", tGroup, &e->mSound);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("pos", tGroup, &e->mPosition);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("darken", tGroup, &e->mIsDarkening);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("p2defmul", tGroup, &e->mPlayer2DefenseMultiplier);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("poweradd", tGroup, &e->mPowerToAdd);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("unhittable", tGroup, &e->mSetPlayerUnhittable);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_SUPER_PAUSE;
	tController->mData = e;
}

static void unloadSuperPauseController(DreamMugenStateController* tController) {
	SuperPauseController* e = (SuperPauseController*)tController->mData;

	destroyDreamMugenAssignment(e->mTime);
	destroyDreamMugenAssignment(e->mBufferTimeForCommandsDuringPauseEnd);
	destroyDreamMugenAssignment(e->mMoveTime);
	destroyDreamMugenAssignment(e->mDoesPauseBackground);

	destroyDreamMugenAssignment(e->mAnim);
	destroyDreamMugenAssignment(e->mSound);
	destroyDreamMugenAssignment(e->mPosition);
	destroyDreamMugenAssignment(e->mIsDarkening);
	destroyDreamMugenAssignment(e->mPlayer2DefenseMultiplier);
	destroyDreamMugenAssignment(e->mPowerToAdd);
	destroyDreamMugenAssignment(e->mSetPlayerUnhittable);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mType;
	DreamMugenAssignment* mName;
	DreamMugenAssignment* mID;
	DreamMugenAssignment* mPosition;
	DreamMugenAssignment* mPositionType;

	DreamMugenAssignment* mFacing;
	DreamMugenAssignment* mStateNumber;
	DreamMugenAssignment* mCanControl;

	DreamMugenAssignment* mHasOwnPalette;
	DreamMugenAssignment* mSuperMoveTime;
	DreamMugenAssignment* mPauseMoveTime;

	DreamMugenAssignment* mSizeScaleX;
	DreamMugenAssignment* mSizeScaleY;
	DreamMugenAssignment* mSizeGroundBack;
	DreamMugenAssignment* mSizeGroundFront;
	DreamMugenAssignment* mSizeAirBack;
	DreamMugenAssignment* mSizeAirFront;

	DreamMugenAssignment* mSizeHeight;
	DreamMugenAssignment* mSizeProjectilesDoScale;
	DreamMugenAssignment* mSizeHeadPosition;
	DreamMugenAssignment* mSizeMiddlePosition;
	DreamMugenAssignment* mSizeShadowOffset;

} HelperController;

static void parseHelperController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	HelperController* e = (HelperController*)allocMemoryOnMemoryStackOrMemory(sizeof(HelperController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("helpertype", tGroup, &e->mType, "normal");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("name", tGroup, &e->mType, "normal");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("pos", tGroup, &e->mPosition);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("postype", tGroup, &e->mPositionType, "p1");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("facing", tGroup, &e->mFacing);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("stateno", tGroup, &e->mStateNumber);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("keyctrl", tGroup, &e->mCanControl);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("ownpal", tGroup, &e->mHasOwnPalette);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("supermovetime", tGroup, &e->mSuperMoveTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("pausemovetime", tGroup, &e->mPauseMoveTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("size.xscale", tGroup, &e->mSizeScaleX);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("size.yscale", tGroup, &e->mSizeScaleY);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("size.ground.back", tGroup, &e->mSizeGroundBack);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("size.ground.front", tGroup, &e->mSizeGroundFront);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("size.air.back", tGroup, &e->mSizeAirBack);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("size.air.front", tGroup, &e->mSizeAirFront);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("size.height", tGroup, &e->mSizeHeight);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("size.proj.doscale", tGroup, &e->mSizeProjectilesDoScale);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("size.head.pos", tGroup, &e->mSizeHeadPosition);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("size.mid.pos", tGroup, &e->mSizeMiddlePosition);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("size.shadowoffset", tGroup, &e->mSizeShadowOffset);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_HELPER;
	tController->mData = e;
}

static void unloadHelperController(DreamMugenStateController* tController) {
	HelperController* e = (HelperController*)tController->mData;

	destroyDreamMugenAssignment(e->mType);
	freeMemory(&e->mName);
	destroyDreamMugenAssignment(e->mID);
	destroyDreamMugenAssignment(e->mPosition);
	destroyDreamMugenAssignment(e->mPositionType);

	destroyDreamMugenAssignment(e->mFacing);
	destroyDreamMugenAssignment(e->mStateNumber);
	destroyDreamMugenAssignment(e->mCanControl);

	destroyDreamMugenAssignment(e->mHasOwnPalette);
	destroyDreamMugenAssignment(e->mSuperMoveTime);
	destroyDreamMugenAssignment(e->mPauseMoveTime);

	destroyDreamMugenAssignment(e->mSizeScaleX);
	destroyDreamMugenAssignment(e->mSizeScaleY);
	destroyDreamMugenAssignment(e->mSizeGroundBack);
	destroyDreamMugenAssignment(e->mSizeGroundFront);
	destroyDreamMugenAssignment(e->mSizeAirBack);
	destroyDreamMugenAssignment(e->mSizeAirFront);

	destroyDreamMugenAssignment(e->mSizeHeight);
	destroyDreamMugenAssignment(e->mSizeProjectilesDoScale);
	destroyDreamMugenAssignment(e->mSizeHeadPosition);
	destroyDreamMugenAssignment(e->mSizeMiddlePosition);
	destroyDreamMugenAssignment(e->mSizeShadowOffset);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mValue;
	DreamMugenAssignment* mCanKill;
	DreamMugenAssignment* mIsAbsolute;

} LifeAddController;

static void parseLifeAddController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	LifeAddController* e = (LifeAddController*)allocMemoryOnMemoryStackOrMemory(sizeof(LifeAddController));

	fetchDreamAssignmentFromGroupAndReturnWhetherItExists("value", tGroup, &e->mValue);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("kill", tGroup, &e->mCanKill);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("absolute", tGroup, &e->mIsAbsolute);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_ADD_LIFE;
	tController->mData = e;
}

static void unloadLifeAddController(DreamMugenStateController* tController) {
	LifeAddController* e = (LifeAddController*)tController->mData;

	destroyDreamMugenAssignment(e->mValue);
	destroyDreamMugenAssignment(e->mCanKill);
	destroyDreamMugenAssignment(e->mIsAbsolute);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mValue;
	DreamMugenAssignment* mID;
	DreamMugenAssignment* mCanKill;
	DreamMugenAssignment* mIsAbsolute;

} TargetLifeAddController;

static void parseTargetLifeAddController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	TargetLifeAddController* e = (TargetLifeAddController*)allocMemoryOnMemoryStackOrMemory(sizeof(TargetLifeAddController));

	fetchDreamAssignmentFromGroupAndReturnWhetherItExists("value", tGroup, &e->mValue);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("kill", tGroup, &e->mCanKill);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("absolute", tGroup, &e->mIsAbsolute);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_ADD_TARGET_LIFE;
	tController->mData = e;
}

static void unloadTargetLifeAddController(DreamMugenStateController* tController) {
	TargetLifeAddController* e = (TargetLifeAddController*)tController->mData;

	destroyDreamMugenAssignment(e->mValue);
	destroyDreamMugenAssignment(e->mID);
	destroyDreamMugenAssignment(e->mCanKill);
	destroyDreamMugenAssignment(e->mIsAbsolute);

	freeMemory(e);
}


typedef struct {
	DreamMugenAssignment* mID;

	uint8_t mHasID;
} RemoveExplodController;

static void parseRemoveExplodController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	RemoveExplodController* e = (RemoveExplodController*)allocMemoryOnMemoryStackOrMemory(sizeof(RemoveExplodController));

	e->mHasID = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("id", tGroup, &e->mID);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_REMOVE_EXPLOD;
	tController->mData = e;
}

static void unloadRemoveExplodController(DreamMugenStateController* tController) {
	RemoveExplodController* e = (RemoveExplodController*)tController->mData;

	if (e->mHasID) {
		destroyDreamMugenAssignment(e->mID);
	}

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mValue;
	DreamMugenAssignment* mScale;

	uint8_t mHasValue;
	uint8_t mHasScale;
} AngleDrawController;

static void parseAngleDrawController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	AngleDrawController* e = (AngleDrawController*)allocMemoryOnMemoryStackOrMemory(sizeof(AngleDrawController));

	e->mHasValue = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("value", tGroup, &e->mValue);
	e->mHasScale = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("scale", tGroup, &e->mScale);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_DRAW_ANGLE;
	tController->mData = e;
}

static void unloadAngleDrawController(DreamMugenStateController* tController) {
	AngleDrawController* e = (AngleDrawController*)tController->mData;

	if (e->mHasValue) {
		destroyDreamMugenAssignment(e->mValue);
	}
	if (e->mHasScale) {
		destroyDreamMugenAssignment(e->mScale);
	}

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mTime;
	DreamMugenAssignment* mFacing;
	DreamMugenAssignment* mID;
	DreamMugenAssignment* mPosition;

} BindController;

static void parseBindController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup, DreamMugenStateControllerType tType) {
	BindController* e = (BindController*)allocMemoryOnMemoryStackOrMemory(sizeof(BindController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("time", tGroup, &e->mTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("facing", tGroup, &e->mFacing);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("pos", tGroup, &e->mPosition);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);

	tController->mType = tType;
	tController->mData = e;
}

static void unloadBindController(DreamMugenStateController* tController) {
	BindController* e = (BindController*)tController->mData;

	destroyDreamMugenAssignment(e->mTime);
	destroyDreamMugenAssignment(e->mFacing);
	destroyDreamMugenAssignment(e->mID);
	destroyDreamMugenAssignment(e->mPosition);
	
	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mValue;
	DreamMugenAssignment* mMoveCameraFlags;

} ScreenBoundController;

static void parseScreenBoundController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	ScreenBoundController* e = (ScreenBoundController*)allocMemoryOnMemoryStackOrMemory(sizeof(ScreenBoundController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("value", tGroup, &e->mValue);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("movecamera", tGroup, &e->mMoveCameraFlags);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_SCREEN_BOUND;
	tController->mData = e;
}

static void unloadScreenBoundController(DreamMugenStateController* tController) {
	ScreenBoundController* e = (ScreenBoundController*)tController->mData;

	destroyDreamMugenAssignment(e->mValue);
	destroyDreamMugenAssignment(e->mMoveCameraFlags);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mValue;
	DreamMugenAssignment* mID;

} SetTargetFacingController;

static void parseSetTargetFacingController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	SetTargetFacingController* e = (SetTargetFacingController*)allocMemoryOnMemoryStackOrMemory(sizeof(SetTargetFacingController));

	fetchDreamAssignmentFromGroupAndReturnWhetherItExists("value", tGroup, &e->mValue);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_SET_TARGET_FACING;
	tController->mData = e;
}

static void unloadSetTargetFacingController(DreamMugenStateController* tController) {
	SetTargetFacingController* e = (SetTargetFacingController*)tController->mData;

	destroyDreamMugenAssignment(e->mValue);
	destroyDreamMugenAssignment(e->mID);

	freeMemory(e);
}

typedef struct {
	MugenStringVector mAttributes;
} ReversalDefinitionController;

static void parseReversalDefinitionController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	ReversalDefinitionController* e = (ReversalDefinitionController*)allocMemoryOnMemoryStackOrMemory(sizeof(ReversalDefinitionController));

	uint8_t hasString;
	readMugenDefStringVector(&e->mAttributes, tGroup, "reversal.attr", &hasString);
	assert(hasString);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_REVERSAL_DEFINITION;
	tController->mData = e;
}

static void unloadReversalDefinitionController(DreamMugenStateController* tController) {
	ReversalDefinitionController* e = (ReversalDefinitionController*)tController->mData;

	unloadMugenDefStringVector(e->mAttributes);

	freeMemory(e);
}

typedef struct {
	HitDefinitionController mHitDef;

	DreamMugenAssignment* mID;
	DreamMugenAssignment* mAnimation;
	DreamMugenAssignment* mHitAnimation;
	DreamMugenAssignment* mRemoveAnimation;
	DreamMugenAssignment* mCancelAnimation;

	DreamMugenAssignment* mScale;
	DreamMugenAssignment* mIsRemovingProjectileAfterHit;
	DreamMugenAssignment* mRemoveTime;
	DreamMugenAssignment* mVelocity;
	DreamMugenAssignment* mRemoveVelocity;
	DreamMugenAssignment* mAcceleration;
	DreamMugenAssignment* mVelocityMultipliers;
	DreamMugenAssignment* mHitAmountBeforeVanishing;

	DreamMugenAssignment* mMissTime;
	DreamMugenAssignment* mPriority;
	DreamMugenAssignment* mSpriteSpriority;

	DreamMugenAssignment* mEdgeBound;
	DreamMugenAssignment* mStageBound;
	DreamMugenAssignment* mHeightBoundValues;
	DreamMugenAssignment* mOffset;
	DreamMugenAssignment* mPositionType;

	DreamMugenAssignment* mShadow;
	DreamMugenAssignment* mSuperMoveTime;
	DreamMugenAssignment* mPauseMoveTime;
	DreamMugenAssignment* mHasOwnPalette;

	DreamMugenAssignment* mRemapPalette;
	DreamMugenAssignment* mAfterImageTime;
	DreamMugenAssignment* mAfterImageLength;
	DreamMugenAssignment* mAfterImage;
} ProjectileController;

static void parseProjectileController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	ProjectileController* e = (ProjectileController*)allocMemoryOnMemoryStackOrMemory(sizeof(ProjectileController));
	readHitDefinitionFromGroup(&e->mHitDef, tGroup);

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("projid", tGroup, &e->mID);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("projanim", tGroup, &e->mAnimation);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("projhitanim", tGroup, &e->mHitAnimation);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("projremanim", tGroup, &e->mRemoveAnimation);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("projcancelanim", tGroup, &e->mCancelAnimation);

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("projscale", tGroup, &e->mScale);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("projremove", tGroup, &e->mIsRemovingProjectileAfterHit);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("projremovetime", tGroup, &e->mRemoveTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("velocity", tGroup, &e->mVelocity);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("remvelocity", tGroup, &e->mRemoveVelocity);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("accel", tGroup, &e->mAcceleration);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("velmul", tGroup, &e->mVelocityMultipliers);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("projhits", tGroup, &e->mHitAmountBeforeVanishing);

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("projmisstime", tGroup, &e->mMissTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("projpriority", tGroup, &e->mPriority);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("projsprpriority", tGroup, &e->mSpriteSpriority);

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("projedgebound", tGroup, &e->mEdgeBound);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("projstagebound", tGroup, &e->mStageBound);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("projheightbound", tGroup, &e->mHeightBoundValues);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("offset", tGroup, &e->mOffset);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("postype", tGroup, &e->mPositionType);

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("projshadow", tGroup, &e->mShadow);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("supermovetime", tGroup, &e->mSuperMoveTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("pausemovetime", tGroup, &e->mPauseMoveTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("ownpal", tGroup, &e->mHasOwnPalette);

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("remappal", tGroup, &e->mRemapPalette);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("afterimage.time", tGroup, &e->mAfterImageTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("afterimage.length", tGroup, &e->mAfterImageLength);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("afterimage", tGroup, &e->mAfterImage);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_PROJECTILE;
	tController->mData = e;
}

static void unloadProjectileController(DreamMugenStateController* tController) {
	ProjectileController* e = (ProjectileController*)tController->mData;
	unloadHitDefinitionData(&e->mHitDef);

	destroyDreamMugenAssignment(e->mID);
	destroyDreamMugenAssignment(e->mAnimation);
	destroyDreamMugenAssignment(e->mHitAnimation);
	destroyDreamMugenAssignment(e->mRemoveAnimation);
	destroyDreamMugenAssignment(e->mCancelAnimation);

	destroyDreamMugenAssignment(e->mScale);
	destroyDreamMugenAssignment(e->mIsRemovingProjectileAfterHit);
	destroyDreamMugenAssignment(e->mRemoveTime);
	destroyDreamMugenAssignment(e->mVelocity);
	destroyDreamMugenAssignment(e->mRemoveVelocity);
	destroyDreamMugenAssignment(e->mAcceleration);
	destroyDreamMugenAssignment(e->mVelocityMultipliers);
	destroyDreamMugenAssignment(e->mHitAmountBeforeVanishing);

	destroyDreamMugenAssignment(e->mMissTime);
	destroyDreamMugenAssignment(e->mPriority);
	destroyDreamMugenAssignment(e->mSpriteSpriority);

	destroyDreamMugenAssignment(e->mEdgeBound);
	destroyDreamMugenAssignment(e->mStageBound);
	destroyDreamMugenAssignment(e->mHeightBoundValues);
	destroyDreamMugenAssignment(e->mOffset);
	destroyDreamMugenAssignment(e->mPositionType);

	destroyDreamMugenAssignment(e->mShadow);
	destroyDreamMugenAssignment(e->mSuperMoveTime);
	destroyDreamMugenAssignment(e->mPauseMoveTime);
	destroyDreamMugenAssignment(e->mHasOwnPalette);

	destroyDreamMugenAssignment(e->mRemapPalette);
	destroyDreamMugenAssignment(e->mAfterImageTime);
	destroyDreamMugenAssignment(e->mAfterImageLength);
	destroyDreamMugenAssignment(e->mAfterImage);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mTime;
	DreamMugenAssignment* mLength;

	DreamMugenAssignment* mPalColor;
	DreamMugenAssignment* mPalInvertAll;
	DreamMugenAssignment* mPalBright;
	DreamMugenAssignment* mPalContrast;
	DreamMugenAssignment* mPalPostBright;
	DreamMugenAssignment* mPalAdd;
	DreamMugenAssignment* mPalMul;
	DreamMugenAssignment* mTimeGap;
	DreamMugenAssignment* mFrameGap;
	DreamMugenAssignment* mTrans;

} AfterImageController;

static void parseAfterImageController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	AfterImageController* e = (AfterImageController*)allocMemoryOnMemoryStackOrMemory(sizeof(AfterImageController));
	
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("time", tGroup, &e->mTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("length", tGroup, &e->mLength);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("palcolor", tGroup, &e->mPalColor);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("palinvertall", tGroup, &e->mPalInvertAll);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("palbright", tGroup, &e->mPalBright);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("palcontrast", tGroup, &e->mPalContrast);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("palpostbright", tGroup, &e->mPalPostBright);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("paladd", tGroup, &e->mPalAdd);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("palmul", tGroup, &e->mPalMul);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("timegap", tGroup, &e->mTimeGap);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("framegap", tGroup, &e->mFrameGap);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("trans", tGroup, &e->mTrans);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_AFTER_IMAGE;
	tController->mData = e;
}

static void unloadAfterImageController(DreamMugenStateController* tController) {
	AfterImageController* e = (AfterImageController*)tController->mData;

	destroyDreamMugenAssignment(e->mTime);
	destroyDreamMugenAssignment(e->mLength);
	destroyDreamMugenAssignment(e->mPalColor);
	destroyDreamMugenAssignment(e->mPalInvertAll);
	destroyDreamMugenAssignment(e->mPalBright);
	destroyDreamMugenAssignment(e->mPalContrast);
	destroyDreamMugenAssignment(e->mPalPostBright);
	destroyDreamMugenAssignment(e->mPalAdd);
	destroyDreamMugenAssignment(e->mPalMul);
	destroyDreamMugenAssignment(e->mTimeGap);
	destroyDreamMugenAssignment(e->mFrameGap);
	destroyDreamMugenAssignment(e->mTrans);

	freeMemory(e);
}


typedef struct {
	DreamMugenAssignment* mTime;
} AfterImageTimeController;

static void parseAfterImageTimeController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	AfterImageTimeController* e = (AfterImageTimeController*)allocMemoryOnMemoryStackOrMemory(sizeof(AfterImageTimeController));

	if (isMugenDefNumberVariableAsGroup(tGroup, "time")) {
		fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("time", tGroup, &e->mTime);
	}
	else {
		fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("value", tGroup, &e->mTime);
	}

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_AFTER_IMAGE_TIME;
	tController->mData = e;
}

static void unloadAfterImageTimeController(DreamMugenStateController* tController) {
	AfterImageTimeController* e = (AfterImageTimeController*)tController->mData;
	destroyDreamMugenAssignment(e->mTime);
	freeMemory(e);
}


typedef struct {
	DreamMugenAssignment* mTime;
	DreamMugenAssignment* mAdd;
	DreamMugenAssignment* mMul;
	DreamMugenAssignment* mSinAdd;
	DreamMugenAssignment* mInvertAll;
	DreamMugenAssignment* mColor;

} PalFXController;

static void parsePalFXController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup, DreamMugenStateControllerType tType) {
	PalFXController* e = (PalFXController*)allocMemoryOnMemoryStackOrMemory(sizeof(PalFXController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("time", tGroup, &e->mTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("add", tGroup, &e->mAdd);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("mul", tGroup, &e->mMul);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("sinadd", tGroup, &e->mSinAdd);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("invertall", tGroup, &e->mInvertAll);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("color", tGroup, &e->mColor);

	tController->mType = tType;
	tController->mData = e;
}

static void unloadPalFXController(DreamMugenStateController* tController) {
	PalFXController* e = (PalFXController*)tController->mData;

	destroyDreamMugenAssignment(e->mTime);
	destroyDreamMugenAssignment(e->mAdd);
	destroyDreamMugenAssignment(e->mMul);
	destroyDreamMugenAssignment(e->mSinAdd);
	destroyDreamMugenAssignment(e->mInvertAll);
	destroyDreamMugenAssignment(e->mColor);

	freeMemory(e);
}


typedef struct {
	DreamMugenAssignment* mText;
	DreamMugenAssignment* mParams;
} ClipboardController;

static void parseClipboardController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup, DreamMugenStateControllerType tType) {
	ClipboardController* e = (ClipboardController*)allocMemoryOnMemoryStackOrMemory(sizeof(ClipboardController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("text", tGroup, &e->mText);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("params", tGroup, &e->mParams);

	tController->mType = tType;
	tController->mData = e;
}

static void unloadClipboardController(DreamMugenStateController* tController) {
	ClipboardController* e = (ClipboardController*)tController->mData;

	destroyDreamMugenAssignment(e->mText);
	destroyDreamMugenAssignment(e->mParams);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mRecursive;
	DreamMugenAssignment* mRemoveExplods;

} DestroySelfController;

static void parseDestroySelfController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	DestroySelfController* e = (DestroySelfController*)allocMemoryOnMemoryStackOrMemory(sizeof(DestroySelfController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("recursive", tGroup, &e->mRecursive);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("removeexplods", tGroup, &e->mRemoveExplods);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_DESTROY_SELF;
	tController->mData = e;
}

static void unloadDestroySelfController(DreamMugenStateController* tController) {
	DestroySelfController* e = (DestroySelfController*)tController->mData;

	destroyDreamMugenAssignment(e->mRecursive);
	destroyDreamMugenAssignment(e->mRemoveExplods);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mValue;
	DreamMugenAssignment* mTime;
	DreamMugenAssignment* mUnder;

} EnvironmentColorController;

static void parseEnvColorController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	EnvironmentColorController* e = (EnvironmentColorController*)allocMemoryOnMemoryStackOrMemory(sizeof(EnvironmentColorController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("value", tGroup, &e->mValue, "255 , 255 , 255");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("time", tGroup, &e->mTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("under", tGroup, &e->mUnder);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_ENVIRONMENT_COLOR;
	tController->mData = e;
	
}

static void unloadEnvColorController(DreamMugenStateController* tController) {
	EnvironmentColorController* e = (EnvironmentColorController*)tController->mData;

	destroyDreamMugenAssignment(e->mValue);
	destroyDreamMugenAssignment(e->mTime);
	destroyDreamMugenAssignment(e->mUnder);

	freeMemory(e);

}

typedef struct {
	DreamMugenAssignment* mID;
	DreamMugenAssignment* mTime;

} ExplodBindTimeController;

static void parseExplodBindTimeController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	ExplodBindTimeController* e = (ExplodBindTimeController*)allocMemoryOnMemoryStackOrMemory(sizeof(ExplodBindTimeController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);

	if (isMugenDefNumberVariableAsGroup(tGroup, "time")) {
		fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("time", tGroup, &e->mTime);
	}
	else {
		fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("value", tGroup, &e->mTime);
	}

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_EXPLOD_BIND_TIME;
	tController->mData = e;
}

static void unloadExplodBindTimeController(DreamMugenStateController* tController) {
	ExplodBindTimeController* e = (ExplodBindTimeController*)tController->mData;

	destroyDreamMugenAssignment(e->mID);
	destroyDreamMugenAssignment(e->mTime);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mValue;
	DreamMugenAssignment* mIsUnderPlayer;
	DreamMugenAssignment* mPosOffset;
	DreamMugenAssignment* mRandomOffset;

} GameMakeAnimController;

static void parseGameMakeAnimController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	GameMakeAnimController* e = (GameMakeAnimController*)allocMemoryOnMemoryStackOrMemory(sizeof(GameMakeAnimController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("value", tGroup, &e->mValue);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("under", tGroup, &e->mIsUnderPlayer);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("pos", tGroup, &e->mPosOffset);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("random", tGroup, &e->mRandomOffset);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_MAKE_GAME_ANIMATION;
	tController->mData = e;
}

static void unloadGameMakeAnimController(DreamMugenStateController* tController) {
	GameMakeAnimController* e = (GameMakeAnimController*)tController->mData;

	destroyDreamMugenAssignment(e->mValue);
	destroyDreamMugenAssignment(e->mIsUnderPlayer);
	destroyDreamMugenAssignment(e->mPosOffset);
	destroyDreamMugenAssignment(e->mRandomOffset);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mAttributeString;
	DreamMugenAssignment* mStateNo;

	DreamMugenAssignment* mSlot;
	DreamMugenAssignment* mTime;
	DreamMugenAssignment* mForceAir;
} HitOverrideController;

static void parseHitOverrideController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	HitOverrideController* e = (HitOverrideController*)allocMemoryOnMemoryStackOrMemory(sizeof(HitOverrideController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("attr", tGroup, &e->mAttributeString);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("stateno", tGroup, &e->mStateNo);

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("slot", tGroup, &e->mSlot);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("time", tGroup, &e->mTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("forceair", tGroup, &e->mForceAir);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_HIT_OVERRIDE;
	tController->mData = e;
}

static void unloadHitOverrideController(DreamMugenStateController* tController) {
	HitOverrideController* e = (HitOverrideController*)tController->mData;

	destroyDreamMugenAssignment(e->mAttributeString);
	destroyDreamMugenAssignment(e->mStateNo);

	destroyDreamMugenAssignment(e->mSlot);
	destroyDreamMugenAssignment(e->mTime);
	destroyDreamMugenAssignment(e->mForceAir);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mTime;
	DreamMugenAssignment* mBufferTimeForCommandsDuringPauseEnd;
	DreamMugenAssignment* mMoveTime;
	DreamMugenAssignment* mDoesPauseBackground;
} PauseController;

static void parsePauseController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	PauseController* e = (PauseController*)allocMemoryOnMemoryStackOrMemory(sizeof(PauseController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("time", tGroup, &e->mTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("endcmdbuftime", tGroup, &e->mBufferTimeForCommandsDuringPauseEnd);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("movetime", tGroup, &e->mMoveTime);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("pausebg", tGroup, &e->mDoesPauseBackground);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_PAUSE;
	tController->mData = e;
}

static void unloadPauseController(DreamMugenStateController* tController) {
	PauseController* e = (PauseController*)tController->mData;

	destroyDreamMugenAssignment(e->mTime);
	destroyDreamMugenAssignment(e->mBufferTimeForCommandsDuringPauseEnd);
	destroyDreamMugenAssignment(e->mMoveTime);
	destroyDreamMugenAssignment(e->mDoesPauseBackground);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mSource;
	DreamMugenAssignment* mDestination;
} RemapPaletteController;

static void parseRemapPaletteController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	RemapPaletteController* e = (RemapPaletteController*)allocMemoryOnMemoryStackOrMemory(sizeof(RemapPaletteController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("source", tGroup, &e->mSource);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("dest", tGroup, &e->mDestination);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_REMAP_PALETTE;
	tController->mData = e;
}

static void unloadRemapPaletteController(DreamMugenStateController* tController) {
	RemapPaletteController* e = (RemapPaletteController*)tController->mData;

	destroyDreamMugenAssignment(e->mSource);
	destroyDreamMugenAssignment(e->mDestination);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mChannel;
	DreamMugenAssignment* mPan;
} SoundPanController;

static void parseSoundPanController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	SoundPanController* e = (SoundPanController*)allocMemoryOnMemoryStackOrMemory(sizeof(SoundPanController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("channel", tGroup, &e->mChannel);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("pan", tGroup, &e->mPan);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_PAN_SOUND;
	tController->mData = e;
}

static void unloadSoundPanController(DreamMugenStateController* tController) {
	SoundPanController* e = (SoundPanController*)tController->mData;

	destroyDreamMugenAssignment(e->mChannel);
	destroyDreamMugenAssignment(e->mPan);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mChannel;
} SoundStopController;

static void parseStopSoundController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	SoundStopController* e = (SoundStopController*)allocMemoryOnMemoryStackOrMemory(sizeof(SoundStopController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("channel", tGroup, &e->mChannel);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_STOP_SOUND;
	tController->mData = e;
}

static void unloadStopSoundController(DreamMugenStateController* tController) {
	SoundStopController* e = (SoundStopController*)tController->mData;

	destroyDreamMugenAssignment(e->mChannel);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mExcludeID;
	DreamMugenAssignment* mKeepOne;

} TargetDropController;

static void parseTargetDropController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	TargetDropController* e = (TargetDropController*)allocMemoryOnMemoryStackOrMemory(sizeof(TargetDropController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("excludeid", tGroup, &e->mExcludeID);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("keepone", tGroup, &e->mKeepOne);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_DROP_TARGET;
	tController->mData = e;
}

static void unloadTargetDropController(DreamMugenStateController* tController) {
	TargetDropController* e = (TargetDropController*)tController->mData;

	destroyDreamMugenAssignment(e->mExcludeID);
	destroyDreamMugenAssignment(e->mKeepOne);

	freeMemory(e);
}


typedef struct {
	DreamMugenAssignment* mValue;
	DreamMugenAssignment* mID;
} TargetPowerAddController;

static void parseTargetPowerAddController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	TargetPowerAddController* e = (TargetPowerAddController*)allocMemoryOnMemoryStackOrMemory(sizeof(TargetPowerAddController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("value", tGroup, &e->mValue);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_ADD_TARGET_POWER;
	tController->mData = e;
}

static void unloadTargetPowerAddController(DreamMugenStateController* tController) {
	TargetPowerAddController* e = (TargetPowerAddController*)tController->mData;

	destroyDreamMugenAssignment(e->mValue);
	destroyDreamMugenAssignment(e->mID);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mID;
	DreamMugenAssignment* x;
	DreamMugenAssignment* y;

	uint8_t mIsSettingX;
	uint8_t mIsSettingY;
} Target2DPhysicsController;

static void parseTarget2DPhysicsController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup, DreamMugenStateControllerType tType) {
	Target2DPhysicsController* e = (Target2DPhysicsController*)allocMemoryOnMemoryStackOrMemory(sizeof(Target2DPhysicsController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);
	e->mIsSettingX = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("x", tGroup, &e->x);
	e->mIsSettingY = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("y", tGroup, &e->y);

	tController->mType = tType;
	tController->mData = e;
}

static void unloadTarget2DPhysicsController(DreamMugenStateController* tController) {
	Target2DPhysicsController* e = (Target2DPhysicsController*)tController->mData;

	destroyDreamMugenAssignment(e->mID);
	if (e->mIsSettingX) {
		destroyDreamMugenAssignment(e->x);
	}
	if (e->mIsSettingY) {
		destroyDreamMugenAssignment(e->y);
	}

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mTransparency;
	DreamMugenAssignment* mAlpha;
} TransparencyController;

static void parseTransparencyController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	TransparencyController* e = (TransparencyController*)allocMemoryOnMemoryStackOrMemory(sizeof(TransparencyController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("trans", tGroup, &e->mTransparency, "default");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("alpha", tGroup, &e->mAlpha);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_TRANSPARENCY;
	tController->mData = e;
}

static void unloadTransparencyController(DreamMugenStateController* tController) {
	TransparencyController* e = (TransparencyController*)tController->mData;

	destroyDreamMugenAssignment(e->mTransparency);
	destroyDreamMugenAssignment(e->mAlpha);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mValue;
	DreamMugenAssignment* mRange;
} VarRandomController;

static void parseVarRandomController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	VarRandomController* e = (VarRandomController*)allocMemoryOnMemoryStackOrMemory(sizeof(VarRandomController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("v", tGroup, &e->mValue);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("range", tGroup, &e->mRange, "0 , 1000");

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_SET_VARIABLE_RANDOM;
	tController->mData = e;
}

static void unloadVarRandomController(DreamMugenStateController* tController) {
	VarRandomController* e = (VarRandomController*)tController->mData;

	destroyDreamMugenAssignment(e->mValue);
	destroyDreamMugenAssignment(e->mRange);

	freeMemory(e);
}

typedef struct {
	DreamMugenAssignment* mValue;
} VictoryQuoteController;

static void parseVictoryQuoteController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	VictoryQuoteController* e = (VictoryQuoteController*)allocMemoryOnMemoryStackOrMemory(sizeof(VictoryQuoteController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("value", tGroup, &e->mValue);

	tController->mType = MUGEN_STATE_CONTROLLER_TYPE_VICTORY_QUOTE;
	tController->mData = e;
}

static void unloadVictoryQuoteController(DreamMugenStateController* tController) {
	VictoryQuoteController* e = (VictoryQuoteController*)tController->mData;
	destroyDreamMugenAssignment(e->mValue);
	freeMemory(e);
}

static void parseStateControllerType(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	assert(stl_string_map_contains_array(tGroup->mElements, "type"));
	MugenDefScriptGroupElement* e = &tGroup->mElements["type"];
	tController->mData = NULL;

	char* type = getAllocatedMugenDefStringVariableAsElement(e);
	turnStringLowercase(type);

	if(!stl_string_map_contains_array(gMugenStateControllerVariableHandler.mStateControllerParsers, type)) {
		logWarningFormat("Unable to determine state controller type %s. Defaulting to null.", type);
		freeMemory(type);
		type = (char*)allocMemory(10);
		strcpy(type, "null");
	}

	StateControllerParseFunction func = gMugenStateControllerVariableHandler.mStateControllerParsers[type];
	func(tController, tGroup);

	freeMemory(type);
}

static void parseStateControllerPersistence(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	tController->mPersistence = (int16_t)getMugenDefIntegerOrDefaultAsGroup(tGroup, "persistent", 1);
	tController->mAccessAmount = 0;
}

int gDebugStateControllerAmount;

DreamMugenStateController * parseDreamMugenStateControllerFromGroup(MugenDefScriptGroup* tGroup)
{
	DreamMugenStateController* ret = (DreamMugenStateController*)allocMemoryOnMemoryStackOrMemory(sizeof(DreamMugenStateController));
	gDebugStateControllerAmount++;
	parseStateControllerType(ret, tGroup);
	parseStateControllerTriggers(ret, tGroup);
	parseStateControllerPersistence(ret, tGroup);

	return ret;
}

static void unloadStateControllerType(DreamMugenStateController* tController) {
	if (!stl_map_contains(gMugenStateControllerVariableHandler.mStateControllerUnloaders, (int)tController->mType)) {
		logWarningFormat("Unable to determine state controller type %d. Defaulting to null.", tController->mType);
		tController->mType = MUGEN_STATE_CONTROLLER_TYPE_NULL;
	}

	StateControllerUnloadFunction func = gMugenStateControllerVariableHandler.mStateControllerUnloaders[tController->mType];
	func(tController);
}

void unloadDreamMugenStateController(DreamMugenStateController * tController)
{
	destroyDreamMugenAssignment(tController->mTrigger.mAssignment);
	unloadStateControllerType(tController);
}

static int handleVelocitySetting(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	Set2DPhysicsController* e = (Set2DPhysicsController*)tController->mData;

	if (e->mIsSettingX) {
		double x = evaluateDreamAssignmentAndReturnAsFloat(&e->x, tPlayer);
		setPlayerVelocityX(tPlayer, x, getPlayerCoordinateP(tPlayer));
	}

	if (e->mIsSettingY) {
		double y = evaluateDreamAssignmentAndReturnAsFloat(&e->y, tPlayer);
		setPlayerVelocityY(tPlayer, y, getPlayerCoordinateP(tPlayer));
	}

	return 0;
}

static int handleVelocityMultiplication(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	Set2DPhysicsController* e = (Set2DPhysicsController*)tController->mData;

	if (e->mIsSettingX) {
		double x = evaluateDreamAssignmentAndReturnAsFloat(&e->x, tPlayer);
		multiplyPlayerVelocityX(tPlayer, x, getPlayerCoordinateP(tPlayer));
	}

	if (e->mIsSettingY) {
		double y = evaluateDreamAssignmentAndReturnAsFloat(&e->y, tPlayer);
		multiplyPlayerVelocityY(tPlayer, y, getPlayerCoordinateP(tPlayer));
	}

	return 0;
}

static int handleVelocityAddition(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	Set2DPhysicsController* e = (Set2DPhysicsController*)tController->mData;

	if (e->mIsSettingX) {
		double x = evaluateDreamAssignmentAndReturnAsFloat(&e->x, tPlayer);
		addPlayerVelocityX(tPlayer, x, getPlayerCoordinateP(tPlayer));
	}

	if (e->mIsSettingY) {
		double y = evaluateDreamAssignmentAndReturnAsFloat(&e->y, tPlayer);
		addPlayerVelocityY(tPlayer, y, getPlayerCoordinateP(tPlayer));
	}

	return 0;
}

static int handlePositionSetting(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	Set2DPhysicsController* e = (Set2DPhysicsController*)tController->mData;

	if (e->mIsSettingX) {
		double x = evaluateDreamAssignmentAndReturnAsFloat(&e->x, tPlayer);
		setPlayerPositionBasedOnScreenCenterX(tPlayer, x, getPlayerCoordinateP(tPlayer));
	}

	if (e->mIsSettingY) {
		double y = evaluateDreamAssignmentAndReturnAsFloat(&e->y, tPlayer);
		setPlayerPositionY(tPlayer, y, getPlayerCoordinateP(tPlayer));
	}

	return 0;
}

static int handlePositionAdding(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	Set2DPhysicsController* e = (Set2DPhysicsController*)tController->mData;

	if (e->mIsSettingX) {
		double x = evaluateDreamAssignmentAndReturnAsFloat(&e->x, tPlayer);
		addPlayerPositionX(tPlayer, x, getPlayerCoordinateP(tPlayer));
	}

	if (e->mIsSettingY) {
		double y = evaluateDreamAssignmentAndReturnAsFloat(&e->y, tPlayer);
		addPlayerPositionY(tPlayer, y, getPlayerCoordinateP(tPlayer));
	}

	return 0;
}


static int handleStateChange(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	ChangeStateController* e = (ChangeStateController*)tController->mData;

	int state = evaluateDreamAssignmentAndReturnAsInteger(&e->mState, tPlayer);
	changePlayerStateBeforeImmediatelyEvaluatingIt(tPlayer, state);

	if (e->mIsChangingControl) {
		int control = evaluateDreamAssignmentAndReturnAsInteger(&e->mControl, tPlayer);
		setPlayerControl(tPlayer, control);
	}

	if (e->mIsChangingAnimation) {
		int animation = evaluateDreamAssignmentAndReturnAsInteger(&e->mAnimation, tPlayer);
		changePlayerAnimation(tPlayer, animation);
	}

	return 1;
}

static int handleSelfStateChange(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	ChangeStateController* e = (ChangeStateController*)tController->mData;

	int state = evaluateDreamAssignmentAndReturnAsInteger(&e->mState, tPlayer);

	if (e->mIsChangingControl) {
		int control = evaluateDreamAssignmentAndReturnAsInteger(&e->mControl, tPlayer);
		setPlayerControl(tPlayer, control);
	}

	if (e->mIsChangingAnimation) {
		int animation = evaluateDreamAssignmentAndReturnAsInteger(&e->mAnimation, tPlayer);
		changePlayerAnimation(tPlayer, animation);
	}

	changePlayerStateToSelfBeforeImmediatelyEvaluatingIt(tPlayer, state);

	return 1;
}

static void getSingleIntegerValueOrDefault(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, int* tDst, int tDefault) {
	if (!(*tAssignment)) *tDst = tDefault;
	else *tDst = evaluateDreamAssignmentAndReturnAsInteger(tAssignment, tPlayer);
}

static void getSingleFloatValueOrDefault(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, double* tDst, double tDefault) {
	if (!(*tAssignment)) *tDst = tDefault;
	else *tDst = evaluateDreamAssignmentAndReturnAsFloat(tAssignment, tPlayer);
}

static int handleTargetStateChange(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	TargetChangeStateController* e = (TargetChangeStateController*)tController->mData;

	int id;
	getSingleIntegerValueOrDefault(&e->mID, tPlayer, &id, -1);

	if (e->mIsChangingControl) {
		int control = evaluateDreamAssignmentAndReturnAsInteger(&e->mControl, tPlayer);
		setPlayerTargetControl(tPlayer, id, control);
	}

	int state = evaluateDreamAssignmentAndReturnAsInteger(&e->mState, tPlayer);
	changePlayerTargetState(tPlayer, id, state);

	return 0;
}

static void handleSoundEffectValue(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer) {
	string flag;
	evaluateDreamAssignmentAndReturnAsString(flag, tAssignment, tPlayer);

	MugenSounds* soundFile;
	int group;
	int item;

	char firstW[20], comma[10];
	int items = sscanf(flag.data(), "%s", firstW);
	if (items != 1) {
		logWarningFormat("Unable to parse flag: %s. Abort.");
		return;
	}

	if (!strcmp("isinotherfilef", firstW)) {
		soundFile = getDreamCommonSounds();
		items = sscanf(flag.data(), "%s %d %s %d", firstW, &group, comma, &item);
		if (items != 4) {
			logWarningFormat("Unable to parse flag: %s. Abort.");
			return;
		}
	} else if (!strcmp("isinotherfiles", firstW)) {
		soundFile = getPlayerSounds(tPlayer);
		items = sscanf(flag.data(), "%s %d %s %d", firstW, &group, comma, &item);
		if (items != 4) {
			logWarningFormat("Unable to parse flag: %s. Abort.");
			return;
		}
	}
	else {
		soundFile = getPlayerSounds(tPlayer);
		items = sscanf(flag.data(), "%d %s %d", &group, comma, &item);
		if (items != 3) {
			logWarningFormat("Unable to parse flag: %s. Abort.");
			return;
		}
	}


	tryPlayMugenSound(soundFile, group, item);
}

static int handlePlaySound(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	PlaySoundController* e = (PlaySoundController*)tController->mData;

	handleSoundEffectValue(&e->mValue, tPlayer); // TODO: other parameters (https://dev.azure.com/captdc/DogmaRnDA/_workitems/edit/303)

	return 0;
}

static void getHitDefinitionAttributeValuesFromString(const char* attr, DreamMugenStateType* tStateType, MugenAttackClass* tAttackClass, MugenAttackType* tAttackType) {
	char arg1[20], comma[10], arg2[20];
	sscanf(attr, "%s %s %s", arg1, comma, arg2);
	//assert(strcmp("", arg1));
	//assert(strlen(arg2) == 2);
	//assert(!strcmp(",", comma));

	turnStringLowercase(arg1);
	if (strchr(arg1, 's') != NULL) *tStateType = MUGEN_STATE_TYPE_STANDING;
	else if (strchr(arg1, 'c') != NULL) *tStateType = MUGEN_STATE_TYPE_CROUCHING;
	else if (strchr(arg1, 'a') != NULL) *tStateType = MUGEN_STATE_TYPE_AIR;
	else {
		logWarning("Unable to parse hitdef attr 1.");
		logWarningString(arg1);
		*tStateType = MUGEN_STATE_TYPE_UNCHANGED;
	}

	turnStringLowercase(arg2);
	if (arg2[0] == 'n') *tAttackClass = MUGEN_ATTACK_CLASS_NORMAL;
	else if (arg2[0] == 's') *tAttackClass = MUGEN_ATTACK_CLASS_SPECIAL;
	else if (arg2[0] == 'h') *tAttackClass = MUGEN_ATTACK_CLASS_HYPER;
	else {
		logWarning("Unable to parse hitdef attr 2.");
		logWarningString(arg2);
		*tAttackClass = MUGEN_ATTACK_CLASS_NORMAL;
	}

	if (arg2[1] == 'a') *tAttackType = MUGEN_ATTACK_TYPE_ATTACK;
	else if (arg2[1] == 't') *tAttackType = MUGEN_ATTACK_TYPE_THROW;
	else if (arg2[1] == 'p')  *tAttackType = MUGEN_ATTACK_TYPE_PROJECTILE;
	else {
		logWarning("Unable to parse hitdef attr 2.");
		logWarningString(arg2);
		*tAttackType = MUGEN_ATTACK_TYPE_ATTACK;
	}

}

static void handleHitDefinitionAttribute(HitDefinitionController* e, DreamPlayer* tPlayer) {
	string attr;
	evaluateDreamAssignmentAndReturnAsString(attr, &e->mAttribute, tPlayer);

	DreamMugenStateType stateType;
	MugenAttackClass attackClass;
	MugenAttackType attackType;

	getHitDefinitionAttributeValuesFromString(attr.data(), &stateType, &attackClass, &attackType);

	setHitDataType(tPlayer, stateType);
	setHitDataAttackClass(tPlayer, attackClass);
	setHitDataAttackType(tPlayer, attackType);
}

static void handleHitDefinitionSingleHitFlag(DreamMugenAssignment** tFlagAssignment, DreamPlayer* tPlayer, void(tSetFunc)(DreamPlayer* tPlayer, const char*)) {
	if (!(*tFlagAssignment)) {
		tSetFunc(tPlayer, "");
		return;
	}
	
	string flag;
	evaluateDreamAssignmentAndReturnAsString(flag, tFlagAssignment, tPlayer);
	tSetFunc(tPlayer, flag.data());
}

static void handleHitDefinitionAffectTeam(DreamMugenAssignment** tAffectAssignment, DreamPlayer* tPlayer) {
	string flag;
	evaluateDreamAssignmentAndReturnAsString(flag, tAffectAssignment, tPlayer);
	if (flag.size() != 1) {
		logWarningFormat("Unable to parse hitdef affectteam %s. Set to enemy.", flag);
		setHitDataAffectTeam(tPlayer, MUGEN_AFFECT_TEAM_ENEMY);
		return;
	}

	if (flag[0] == 'b') setHitDataAffectTeam(tPlayer, MUGEN_AFFECT_TEAM_BOTH);
	else if (flag[0] == 'e') setHitDataAffectTeam(tPlayer, MUGEN_AFFECT_TEAM_ENEMY);
	else if (flag[0] == 'f') setHitDataAffectTeam(tPlayer, MUGEN_AFFECT_TEAM_FRIENDLY);
	else {
		logWarningFormat("Unable to parse hitdef affectteam %s. Set to enemy.", flag);
		setHitDataAffectTeam(tPlayer, MUGEN_AFFECT_TEAM_ENEMY);
	}
}

static void handleHitDefinitionSingleAnimationType(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, void(tFunc)(DreamPlayer*, MugenHitAnimationType), MugenHitAnimationType tDefault) {
	if (!(*tAssignment)) {
		tFunc(tPlayer, tDefault);
		return;
	}
	
	string flag;
	evaluateDreamAssignmentAndReturnAsString(flag, tAssignment, tPlayer);

	if ("light" == flag) tFunc(tPlayer, MUGEN_HIT_ANIMATION_TYPE_LIGHT);
	else if (("medium" == flag) || ("med" == flag)) tFunc(tPlayer, MUGEN_HIT_ANIMATION_TYPE_MEDIUM);
	else if ("hard" == flag) tFunc(tPlayer, MUGEN_HIT_ANIMATION_TYPE_HARD);
	else if ("heavy" == flag) tFunc(tPlayer, MUGEN_HIT_ANIMATION_TYPE_HEAVY);
	else if ("back" == flag) tFunc(tPlayer, MUGEN_HIT_ANIMATION_TYPE_BACK);
	else if ("up" == flag) tFunc(tPlayer, MUGEN_HIT_ANIMATION_TYPE_UP);
	else if ("diagup" == flag) tFunc(tPlayer, MUGEN_HIT_ANIMATION_TYPE_DIAGONAL_UP);
	else {
		logWarningFormat("Unable to parse hitdef animation type %s. Setting to default", flag);
		tFunc(tPlayer, tDefault);
	}
}

static void handleHitDefinitionPriority(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer) {
	int prio;
	MugenHitPriorityType type;
	if (!(*tAssignment)) {
		prio = 4;
		type = MUGEN_HIT_PRIORITY_HIT;
	}
	else {
		string flag;
		evaluateDreamAssignmentAndReturnAsString(flag, tAssignment, tPlayer);

		char prioString[20], comma[10], typeString[20];
		int items = sscanf(flag.data(), "%s %s %s", prioString, comma, typeString);

		if (items < 1 || !strcmp("", prioString)) prio = 4;
		else prio = atoi(prioString);

		if (items < 3 || !strcmp("", typeString)) type = MUGEN_HIT_PRIORITY_HIT;
		else if (!strcmp("hit", typeString)) type = MUGEN_HIT_PRIORITY_HIT;
		else if (!strcmp("miss", typeString)) type = MUGEN_HIT_PRIORITY_MISS;
		else if (!strcmp("dodge", typeString)) type = MUGEN_HIT_PRIORITY_DODGE;
		else {
			logWarningFormat("Unable to parse hitdef priority type %s with string %s. Defaulting to hit.", flag, typeString);
			type = MUGEN_HIT_PRIORITY_HIT;
		}
	}
	setHitDataPriority(tPlayer, prio, type);
}

static void getTwoIntegerValuesWithDefaultValues(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, int* v1, int* v2, int tDefault1, int tDefault2) {
	if (!(*tAssignment)) {
		*v1 = tDefault1;
		*v2 = tDefault2;
		return;
	}
	
	string flag;
	evaluateDreamAssignmentAndReturnAsString(flag, tAssignment, tPlayer);

	char string1[20], comma[10], string2[20];
	int items = sscanf(flag.data(), "%s %s %s", string1, comma, string2);

	if (items < 1 || !strcmp("", string1)) *v1 = tDefault1;
	else *v1 = atoi(string1);
	if (items < 3 || !strcmp("", string2)) *v2 = tDefault2;
	else *v2 = atoi(string2);
}

static void getTwoFloatValuesWithDefaultValues(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, double* v1, double* v2, double tDefault1, double tDefault2) {
	if (!(*tAssignment)) {
		*v1 = tDefault1;
		*v2 = tDefault2;
		return;
	}
	
	string flag;
	evaluateDreamAssignmentAndReturnAsString(flag, tAssignment, tPlayer);

	char string1[20], comma[10], string2[20];
	int items = sscanf(flag.data(), "%s %s %s", string1, comma, string2);

	if (items < 1 || !strcmp("", string1)) *v1 = tDefault1;
	else *v1 = atof(string1);
	if (items < 3 || !strcmp("", string2)) *v2 = tDefault2;
	else *v2 = atof(string2);
}

static void handleHitDefinitionDamage(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer) {
	int damage, guardDamage;

	getTwoIntegerValuesWithDefaultValues(tAssignment, tPlayer, &damage, &guardDamage, 0, 0);
	setHitDataDamage(tPlayer, damage, guardDamage);
}

static void handleHitDefinitionSinglePauseTime(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, void(tFunc)(DreamPlayer*, int, int), int tDefault1, int tDefault2) {
	int p1PauseTime, p2PauseTime;

	getTwoIntegerValuesWithDefaultValues(tAssignment, tPlayer, &p1PauseTime, &p2PauseTime, tDefault1, tDefault2);
	tFunc(tPlayer, p1PauseTime, p2PauseTime);
}

static void handleHitDefinitionSparkNumberSingle(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, void(tFunc)(DreamPlayer*, int, int), int tDefaultIsInFile, int tDefaultNumber) {
	int isInPlayerFile;
	int number;
	if (!(*tAssignment)) {
		isInPlayerFile = tDefaultIsInFile;
		number = tDefaultNumber;
	}
	else {
		string flag;
		evaluateDreamAssignmentAndReturnAsString(flag, tAssignment, tPlayer);

		char firstW[200];
		int which = -1;

		int items = sscanf(flag.data(), "%s %d", firstW, &which);

		if (items < 1) {
			isInPlayerFile = tDefaultIsInFile;
			number = tDefaultNumber;
		}
		else if (!strcmp("isinotherfilef", firstW) || !strcmp("isinotherfiles", firstW)) {
			assert(items == 2);
			isInPlayerFile = 1;
			number = which;
		}
		else {
			assert(items == 1);
			isInPlayerFile = 0;
			number = atoi(flag.data());
		}
	}

	tFunc(tPlayer, isInPlayerFile, number);
}

static void handleHitDefinitionSparkXY(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer) {
	int x, y;

	getTwoIntegerValuesWithDefaultValues(tAssignment, tPlayer, &x, &y, 0, 0);
	setHitDataSparkXY(tPlayer, x, y);
}

static void handleHitDefinitionSingleSound(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, void(tFunc)(DreamPlayer*, int, int, int), int tDefaultGroup, int tDefaultItem) {
	int group;
	int item;
	int isInPlayerFile;
	if (!(*tAssignment)) {
		isInPlayerFile = 0;
		group = tDefaultGroup;
		item = tDefaultItem;
	}
	else {
		string flag;
		evaluateDreamAssignmentAndReturnAsString(flag, tAssignment, tPlayer);

		char firstW[200], comma[10];
		int items = sscanf(flag.data(), "%s", firstW);

		if (items < 1) {
			isInPlayerFile = 0;
			group = tDefaultGroup;
			item = tDefaultItem;
		}
		else if (!strcmp("isinotherfilef", firstW)) {
			isInPlayerFile = 0;
			int fullItems = sscanf(flag.data(), "%s %d %s %d", firstW, &group, comma, &item);
			assert(fullItems >= 2);

			if (fullItems < 3) {
				item = tDefaultItem;
			}
		}
		else if (!strcmp("isinotherfiles", firstW)) {
			isInPlayerFile = 1;
			int fullItems = sscanf(flag.data(), "%s %d %s %d", firstW, &group, comma, &item);
			if (fullItems < 2) {
				logWarningFormat("Unable to parse hit definition sound flag %s. Defaulting.", flag);
				group = tDefaultGroup;
				item = tDefaultItem;
			}
			else if (fullItems < 3) {
				item = tDefaultItem;
			}
		}
		else {
			isInPlayerFile = 0;
			int fullItems = sscanf(flag.data(), "%d %s %d", &group, comma, &item);
			if (fullItems < 1) {
				logWarningFormat("Unable to parse hit definition sound flag %s. Defaulting.", flag);
				group = tDefaultGroup;
				item = tDefaultItem;
			}
			else if (fullItems < 2) {
				item = tDefaultItem;
			}
		}
	}

	tFunc(tPlayer, isInPlayerFile, group, item);
}

static void handleHitDefinitionSingleAttackHeight(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, void(tFunc)(DreamPlayer*, MugenAttackHeight), MugenAttackHeight tDefault) {
	if (!(*tAssignment)) {
		tFunc(tPlayer, tDefault);
		return;
	}

	string flag;
	evaluateDreamAssignmentAndReturnAsString(flag, tAssignment, tPlayer);

	if ("high" == flag) tFunc(tPlayer, MUGEN_ATTACK_HEIGHT_HIGH);
	else if ("low" == flag) tFunc(tPlayer, MUGEN_ATTACK_HEIGHT_LOW);
	else if ("trip" == flag) tFunc(tPlayer, MUGEN_ATTACK_HEIGHT_TRIP);
	else if ("heavy" == flag) tFunc(tPlayer, MUGEN_ATTACK_HEIGHT_HEAVY);
	else if ("none" == flag) tFunc(tPlayer, MUGEN_ATTACK_HEIGHT_NONE);
	else {
		logWarningFormat("Unable to parse hitdef attack height type %s. Defaulting.", flag);
		tFunc(tPlayer, tDefault);
	}
}

static void handleExplodOneIntegerElement(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, int tID, void(tFunc)(int, int), int tDefault) {
	if (!(*tAssignment)) {
		tFunc(tID, tDefault);
		return;
	}
	
	tFunc(tID, evaluateDreamAssignmentAndReturnAsInteger(tAssignment, tPlayer));
}

static void handleExplodTwoIntegerElements(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, int tID, void(tFunc)(int, int, int), int tDefault1, int tDefault2) {
	int val1, val2;
	if (!(*tAssignment)) {
		val1 = tDefault1;
		val2 = tDefault2;
	}
	else {
		string flag;
		evaluateDreamAssignmentAndReturnAsString(flag, tAssignment, tPlayer);

		char string1[20], comma[10], string2[20];
		int items = sscanf(flag.data(), "%s %s %s", string1, comma, string2);

		if (items < 1 || !strcmp("", string1)) val1 = tDefault1;
		else val1 = atoi(string1);

		if (items < 3 || !strcmp("", string2)) val2 = tDefault2;
		else val2 = atoi(string2);
	}
	
	tFunc(tID, val1, val2);
}

static void handleExplodThreeIntegerElements(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, int tID, void(tFunc)(int, int, int, int), int tDefault1, int tDefault2, int tDefault3) {
	int vals[3];
	if (!(*tAssignment)) {
		vals[0] = tDefault1;
		vals[1] = tDefault2;
		vals[2] = tDefault3;
	}
	else {
		string flag;
		evaluateDreamAssignmentAndReturnAsString(flag, tAssignment, tPlayer);

		char string[3][20], comma[2][10];
		int items = sscanf(flag.data(), "%s %s %s %s %s", string[0], comma[0], string[1], comma[1], string[2]);

		int defaults[3];
		defaults[0] = tDefault1;
		defaults[1] = tDefault2;
		defaults[2] = tDefault3;

		int j;
		for (j = 0; j < 3; j++) {
			if (items < (1 + j * 2) || !strcmp("", string[j])) vals[j] = defaults[j];
			else vals[j] = atoi(string[j]);
		}
	}
	
	tFunc(tID, vals[0], vals[1], vals[2]);
}

static void handleExplodTwoFloatElements(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, int tID, void(tFunc)(int, double, double), double tDefault1, double tDefault2) {
	double val1, val2;
	if (!(*tAssignment)) {
		val1 = tDefault1;
		val2 = tDefault2;
	}
	else {
		string flag;
		evaluateDreamAssignmentAndReturnAsString(flag, tAssignment, tPlayer);

		char string1[20], comma[10], string2[20];
		int items = sscanf(flag.data(), "%s %s %s", string1, comma, string2);

		if (items < 1 || !strcmp("", string1)) val1 = tDefault1;
		else val1 = atof(string1);

		if (items < 3 || !strcmp("", string2)) val2 = tDefault2;
		else val2 = atof(string2);
	}

	tFunc(tID, val1, val2);
}

static void handleHitDefinitionOneIntegerElement(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, void(tFunc)(DreamPlayer*, int), int tDefault) {
	if (!(*tAssignment)) {
		tFunc(tPlayer, tDefault);
		return;
	}

	tFunc(tPlayer, evaluateDreamAssignmentAndReturnAsInteger(tAssignment, tPlayer));
}

static void handleHitDefinitionTwoIntegerElements(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, void(tFunc)(DreamPlayer*, int, int), int tDefault1, int tDefault2) {
	int val1, val2;
	if (!(*tAssignment)) {
		val1 = tDefault1;
		val2 = tDefault2;
	}
	else {
		string flag;
		evaluateDreamAssignmentAndReturnAsString(flag, tAssignment, tPlayer);

		char string1[20], comma[10], string2[20];
		int items = sscanf(flag.data(), "%s %s %s", string1, comma, string2);

		if (items < 1 || !strcmp("", string1)) val1 = tDefault1;
		else val1 = atoi(string1);

		if (items < 3 || !strcmp("", string2)) val2 = tDefault2;
		else val2 = atoi(string2);
	}
	tFunc(tPlayer, val1, val2);
}

static void handleHitDefinitionThreeIntegerElements(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, void(tFunc)(DreamPlayer*, int, int, int), int tDefault1, int tDefault2, int tDefault3) {
	int vals[3];
	if (!(*tAssignment)) {
		vals[0] = tDefault1;
		vals[1] = tDefault2;
		vals[2] = tDefault3;
	}
	else {
		string flag;
		evaluateDreamAssignmentAndReturnAsString(flag, tAssignment, tPlayer);

		char string[3][20], comma[2][10];
		int items = sscanf(flag.data(), "%s %s %s %s %s", string[0], comma[0], string[1], comma[1], string[2]);

		int defaults[3];
		defaults[0] = tDefault1;
		defaults[1] = tDefault2;
		defaults[2] = tDefault3;

		int j;
		for (j = 0; j < 3; j++) {
			if (items < (1 + j * 2) || !strcmp("", string[j])) vals[j] = defaults[j];
			else vals[j] = atoi(string[j]);
		}
	}
	tFunc(tPlayer, vals[0], vals[1], vals[2]);
}

static void handleHitDefinitionOneFloatElement(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, void(tFunc)(DreamPlayer*, double), double tDefault) {
	if (!(*tAssignment)) {
		tFunc(tPlayer, tDefault);
		return;
	}
	
	tFunc(tPlayer, evaluateDreamAssignmentAndReturnAsFloat(tAssignment, tPlayer));
}

static void handleHitDefinitionTwoFloatElements(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, void(tFunc)(DreamPlayer*, double, double), double tDefault1, double tDefault2) {
	double val1, val2;
	if (!(*tAssignment)) {
		val1 = tDefault1;
		val2 = tDefault2;
	}
	else {
		string flag;
		evaluateDreamAssignmentAndReturnAsString(flag, tAssignment, tPlayer);

		char string1[20], comma[10], string2[20];
		int items = sscanf(flag.data(), "%s %s %s", string1, comma, string2);

		if (items < 1 || !strcmp("", string1)) val1 = tDefault1;
		else val1 = atof(string1);

		if (items < 3 || !strcmp("", string2)) val2 = tDefault2;
		else val2 = atof(string2);
	}

	tFunc(tPlayer, val1, val2);
}

static void handleHitDefinitionTwoOptionalIntegerElements(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, void(tFunc)(DreamPlayer*, int, int), void(tFuncDisable)(DreamPlayer*)) {
	if (!(*tAssignment)) {
		tFuncDisable(tPlayer);
		return;
	}
	
	int x, y;
	getTwoIntegerValuesWithDefaultValues(tAssignment, tPlayer, &x, &y, 0, 0);

	tFunc(tPlayer, x, y);
}

static void handleHitDefinitionSinglePowerAddition(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, void(tFunc)(DreamPlayer*, int, int), double tDefaultFactor, int tDamage) {
	int val1, val2;
	if (!(*tAssignment)) {
		val1 = (int)(tDamage*tDefaultFactor);
		val2 = val1 / 2;
	}
	else {
		string flag;
		evaluateDreamAssignmentAndReturnAsString(flag, tAssignment, tPlayer);

		char string1[20], comma[10], string2[20];
		int items = sscanf(flag.data(), "%s %s %s", string1, comma, string2);

		if (items < 1 || !strcmp("", string1)) val1 = (int)(tDamage*tDefaultFactor);
		else val1 = atoi(string1);

		if (items < 3 || !strcmp("", string2)) val2 = val1 / 2;
		else val2 = atoi(string2);
	}

	tFunc(tPlayer, val1, val2);
}

// TODO: add different target for use in projectile code (https://dev.azure.com/captdc/DogmaRnDA/_workitems/edit/299)
static void handleHitDefinitionWithController(HitDefinitionController* e, DreamPlayer* tPlayer) {
	setHitDataVelocityX(tPlayer, 0);
	setHitDataVelocityY(tPlayer, 0);

	handleHitDefinitionAttribute(e, tPlayer);
	handleHitDefinitionSingleHitFlag(&e->mHitFlag, tPlayer, setHitDataHitFlag);
	handleHitDefinitionSingleHitFlag(&e->mGuardFlag, tPlayer, setHitDataGuardFlag);
	handleHitDefinitionAffectTeam(&e->mAffectTeam, tPlayer);
	handleHitDefinitionSingleAnimationType(&e->mAnimationType, tPlayer, setHitDataAnimationType, MUGEN_HIT_ANIMATION_TYPE_LIGHT);
	handleHitDefinitionSingleAnimationType(&e->mAirAnimationType, tPlayer, setHitDataAirAnimationType, getHitDataAnimationType(tPlayer));
	handleHitDefinitionSingleAnimationType(&e->mFallAnimationType, tPlayer, setHitDataFallAnimationType, getHitDataAirAnimationType(tPlayer) == MUGEN_HIT_ANIMATION_TYPE_UP ? MUGEN_HIT_ANIMATION_TYPE_UP : MUGEN_HIT_ANIMATION_TYPE_BACK);
	handleHitDefinitionPriority(&e->mPriority, tPlayer);
	handleHitDefinitionDamage(&e->mDamage, tPlayer);
	handleHitDefinitionSinglePauseTime(&e->mPauseTime, tPlayer, setHitDataPauseTime, 0, 0);
	handleHitDefinitionSinglePauseTime(&e->mGuardPauseTime, tPlayer, setHitDataGuardPauseTime, getHitDataPlayer1PauseTime(tPlayer), getHitDataPlayer2PauseTime(tPlayer));

	handleHitDefinitionSparkNumberSingle(&e->mSparkNumber, tPlayer, setHitDataSparkNumber, getDefaultPlayerSparkNumberIsInPlayerFile(tPlayer), getDefaultPlayerSparkNumber(tPlayer));
	handleHitDefinitionSparkNumberSingle(&e->mGuardSparkNumber, tPlayer, setHitDataGuardSparkNumber, getDefaultPlayerGuardSparkNumberIsInPlayerFile(tPlayer), getDefaultPlayerGuardSparkNumber(tPlayer));
	handleHitDefinitionSparkXY(&e->mSparkXY, tPlayer);
	handleHitDefinitionSingleSound(&e->mHitSound, tPlayer, setHitDataHitSound, 5, 0);
	handleHitDefinitionSingleSound(&e->mGuardSound, tPlayer, setHitDataGuardSound, 6, 0);

	handleHitDefinitionSingleAttackHeight(&e->mGroundType, tPlayer, setHitDataGroundType, MUGEN_ATTACK_HEIGHT_HIGH);
	handleHitDefinitionSingleAttackHeight(&e->mAirType, tPlayer, setHitDataAirType, getHitDataGroundType(tPlayer));

	handleHitDefinitionOneIntegerElement(&e->mGroundHitTime, tPlayer, setHitDataGroundHitTime, 0);
	handleHitDefinitionOneIntegerElement(&e->mGroundSlideTime, tPlayer, setHitDataGroundSlideTime, 0);
	handleHitDefinitionOneIntegerElement(&e->mGuardHitTime, tPlayer, setHitDataGuardHitTime, getHitDataGroundHitTime(tPlayer));
	handleHitDefinitionOneIntegerElement(&e->mGuardSlideTime, tPlayer, setHitDataGuardSlideTime, getHitDataGuardHitTime(tPlayer));
	handleHitDefinitionOneIntegerElement(&e->mAirHitTime, tPlayer, setHitDataAirHitTime, 20);
	handleHitDefinitionOneIntegerElement(&e->mGuardControlTime, tPlayer, setHitDataGuardControlTime, getHitDataGuardSlideTime(tPlayer));
	handleHitDefinitionOneIntegerElement(&e->mGuardDistance, tPlayer, setHitDataGuardDistance, getDefaultPlayerAttackDistance(tPlayer));
	handleHitDefinitionOneFloatElement(&e->mYAccel, tPlayer, setHitDataYAccel, transformDreamCoordinates(0.7, 480, getPlayerCoordinateP(tPlayer)));
	handleHitDefinitionTwoFloatElements(&e->mGroundVelocity, tPlayer, setHitDataGroundVelocity, 0, 0);
	handleHitDefinitionOneFloatElement(&e->mGuardVelocity, tPlayer, setHitDataGuardVelocity, getHitDataGroundVelocityX(tPlayer));
	handleHitDefinitionTwoFloatElements(&e->mAirVelocity, tPlayer, setHitDataAirVelocity, 0, 0);
	handleHitDefinitionTwoFloatElements(&e->mAirGuardVelocity, tPlayer, setHitDataAirGuardVelocity, getHitDataAirVelocityX(tPlayer) * 1.5, getHitDataAirVelocityY(tPlayer) / 2);

	handleHitDefinitionOneFloatElement(&e->mGroundCornerPushVelocityOffset, tPlayer, setGroundCornerPushVelocityOffset, getHitDataAttackType(tPlayer) == MUGEN_ATTACK_TYPE_ATTACK ? 0.0 : 1.3*getHitDataGuardVelocity(tPlayer));
	handleHitDefinitionOneFloatElement(&e->mAirCornerPushVelocityOffset, tPlayer, setAirCornerPushVelocityOffset, getGroundCornerPushVelocityOffset(tPlayer));
	handleHitDefinitionOneFloatElement(&e->mDownCornerPushVelocityOffset, tPlayer, setDownCornerPushVelocityOffset, getGroundCornerPushVelocityOffset(tPlayer));
	handleHitDefinitionOneFloatElement(&e->mGuardCornerPushVelocityOffset, tPlayer, setGuardCornerPushVelocityOffset, getGroundCornerPushVelocityOffset(tPlayer));
	handleHitDefinitionOneFloatElement(&e->mAirGuardCornerPushVelocityOffset, tPlayer, setAirGuardCornerPushVelocityOffset, getGuardCornerPushVelocityOffset(tPlayer));

	handleHitDefinitionOneIntegerElement(&e->mAirGuardControlTime, tPlayer, setHitDataAirGuardControlTime, getHitDataGuardControlTime(tPlayer));
	handleHitDefinitionOneIntegerElement(&e->mAirJuggle, tPlayer, setHitDataAirJuggle, 0);

	handleHitDefinitionTwoOptionalIntegerElements(&e->mMinimumDistance, tPlayer, setHitDataMinimumDistance, setHitDataMinimumDistanceInactive);
	handleHitDefinitionTwoOptionalIntegerElements(&e->mMaximumDistance, tPlayer, setHitDataMaximumDistance, setHitDataMaximumDistanceInactive);
	handleHitDefinitionTwoOptionalIntegerElements(&e->mSnap, tPlayer, setHitDataSnap, setHitDataSnapInactive);

	handleHitDefinitionOneIntegerElement(&e->mPlayerSpritePriority1, tPlayer, setHitDataPlayer1SpritePriority, 1);
	handleHitDefinitionOneIntegerElement(&e->mPlayerSpritePriority2, tPlayer, setHitDataPlayer2SpritePriority, 0);

	handleHitDefinitionOneIntegerElement(&e->mPlayer1ChangeFaceDirection, tPlayer, setHitDataPlayer1FaceDirection, 0);
	handleHitDefinitionOneIntegerElement(&e->mPlayer1ChangeFaceDirectionRelativeToPlayer2, tPlayer, setHitDataPlayer1ChangeFaceDirectionRelativeToPlayer2, 0);
	handleHitDefinitionOneIntegerElement(&e->mPlayer2ChangeFaceDirectionRelativeToPlayer1, tPlayer, setHitDataPlayer2ChangeFaceDirectionRelativeToPlayer1, 0);

	handleHitDefinitionOneIntegerElement(&e->mPlayer1StateNumber, tPlayer, setPlayer1StateNumber, -1);
	handleHitDefinitionOneIntegerElement(&e->mPlayer2StateNumber, tPlayer, setPlayer2StateNumber, -1);
	handleHitDefinitionOneIntegerElement(&e->mPlayer2CapableOfGettingPlayer1State, tPlayer, setHitDataPlayer2CapableOfGettingPlayer1State, 1);
	handleHitDefinitionOneIntegerElement(&e->mForceStanding, tPlayer, setHitDataForceStanding, getHitDataGroundVelocityY(tPlayer) != 0.0 ? 1 : 0);

	handleHitDefinitionOneIntegerElement(&e->mFall, tPlayer, setHitDataFall, 0);
	handleHitDefinitionOneFloatElement(&e->mFallXVelocity, tPlayer, setHitDataFallXVelocity, 0);
	handleHitDefinitionOneFloatElement(&e->mFallYVelocity, tPlayer, setHitDataFallYVelocity, transformDreamCoordinates(-9, 480, getPlayerCoordinateP(tPlayer))); 
	handleHitDefinitionOneIntegerElement(&e->mFallCanBeRecovered, tPlayer, setHitDataFallRecovery, 1);
	handleHitDefinitionOneIntegerElement(&e->mFallRecoveryTime, tPlayer, setHitDataFallRecoveryTime, 4);
	handleHitDefinitionOneIntegerElement(&e->mFallDamage, tPlayer, setHitDataFallDamage, 0);
	handleHitDefinitionOneIntegerElement(&e->mAirFall, tPlayer, setHitDataAirFall, getHitDataFall(tPlayer));
	handleHitDefinitionOneIntegerElement(&e->mForceNoFall, tPlayer, setHitDataForceNoFall, 0);

	handleHitDefinitionTwoFloatElements(&e->mDownVelocity, tPlayer, setHitDataDownVelocity, getHitDataAirVelocityX(tPlayer), getHitDataAirVelocityY(tPlayer));
	handleHitDefinitionOneIntegerElement(&e->mDownHitTime, tPlayer, setHitDataDownHitTime, 0);
	handleHitDefinitionOneIntegerElement(&e->mDownBounce, tPlayer, setHitDataDownBounce, 0);

	handleHitDefinitionOneIntegerElement(&e->mHitID, tPlayer, setHitDataHitID, 0);
	handleHitDefinitionOneIntegerElement(&e->mChainID, tPlayer, setHitDataChainID, -1);
	handleHitDefinitionTwoIntegerElements(&e->mNoChainID, tPlayer, setHitDataNoChainID, -1, -1);
	handleHitDefinitionOneIntegerElement(&e->mHitOnce, tPlayer, setHitDataHitOnce, 1);

	handleHitDefinitionOneIntegerElement(&e->mKill, tPlayer, setHitDataKill, 1);
	handleHitDefinitionOneIntegerElement(&e->mGuardKill, tPlayer, setHitDataGuardKill, 1);
	handleHitDefinitionOneIntegerElement(&e->mFallKill, tPlayer, setHitDataFallKill, 1);
	handleHitDefinitionOneIntegerElement(&e->mNumberOfHits, tPlayer, setHitDataNumberOfHits, 1);
	handleHitDefinitionSinglePowerAddition(&e->mGetPower, tPlayer, setHitDataGetPower, getDreamDefaultAttackDamageDoneToPowerMultiplier(), getHitDataDamage(tPlayer));
	handleHitDefinitionSinglePowerAddition(&e->mGivePower, tPlayer, setHitDataGivePower, getDreamDefaultAttackDamageReceivedToPowerMultiplier(), getHitDataDamage(tPlayer));

	handleHitDefinitionOneIntegerElement(&e->mPaletteEffectTime, tPlayer, setHitDataPaletteEffectTime, 0);
	handleHitDefinitionThreeIntegerElements(&e->mPaletteEffectMultiplication, tPlayer, setHitDataPaletteEffectMultiplication, 1, 1, 1);
	handleHitDefinitionThreeIntegerElements(&e->mPaletteEffectAddition, tPlayer, setHitDataPaletteEffectAddition, 0, 0, 0);

	handleHitDefinitionOneIntegerElement(&e->mEnvironmentShakeTime, tPlayer, setHitDataEnvironmentShakeTime, 0);
	handleHitDefinitionOneFloatElement(&e->mEnvironmentShakeFrequency, tPlayer, setHitDataEnvironmentShakeFrequency, 60.0);
	handleHitDefinitionOneIntegerElement(&e->mEnvironmentShakeAmplitude, tPlayer, setHitDataEnvironmentShakeAmplitude, (int)transformDreamCoordinates(-4.0, 240, getDreamStageCoordinateP()));
	handleHitDefinitionOneFloatElement(&e->mEnvironmentShakePhase, tPlayer, setHitDataEnvironmentShakePhase, (getHitDataEnvironmentShakeFrequency(tPlayer) >= 90.0) ? 90.0 : 0.0);

	handleHitDefinitionOneIntegerElement(&e->mFallEnvironmentShakeTime, tPlayer, setHitDataFallEnvironmentShakeTime, 0);
	handleHitDefinitionOneFloatElement(&e->mFallEnvironmentShakeFrequency, tPlayer, setHitDataFallEnvironmentShakeFrequency, 60.0);
	handleHitDefinitionOneIntegerElement(&e->mFallEnvironmentShakeAmplitude, tPlayer, setHitDataFallEnvironmentShakeAmplitude, (int)transformDreamCoordinates(-4.0, 240, getDreamStageCoordinateP()));
	handleHitDefinitionOneFloatElement(&e->mFallEnvironmentShakePhase, tPlayer, setHitDataFallEnvironmentShakePhase, getHitDataFallEnvironmentShakeFrequency(tPlayer) >= 90.0 ? 90.0 : 0);

	setHitDataIsFacingRight(tPlayer, getPlayerIsFacingRight(tPlayer));

	setHitDataActive(tPlayer);
}

static int handleHitDefinition(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	HitDefinitionController* e = (HitDefinitionController*)tController->mData;

	handleHitDefinitionWithController(e, tPlayer);

	return 0;
}

static int handleAnimationChange(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	ChangeAnimationController* e = (ChangeAnimationController*)tController->mData;

	int animation = evaluateDreamAssignmentAndReturnAsInteger(&e->tNewAnimation, tPlayer);
	int step = evaluateDreamAssignmentAndReturnAsInteger(&e->tStep, tPlayer);
	changePlayerAnimationWithStartStep(tPlayer, animation, step);

	return 0;
}

static int handleAnimationChange2(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	ChangeAnimationController* e = (ChangeAnimationController*)tController->mData;

	int animation = evaluateDreamAssignmentAndReturnAsInteger(&e->tNewAnimation, tPlayer);
	int step = evaluateDreamAssignmentAndReturnAsInteger(&e->tStep, tPlayer);
	changePlayerAnimationToPlayer2AnimationWithStartStep(tPlayer, animation, step);

	return 0;
}

static int handleControlSetting(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	ControlSettingController* e = (ControlSettingController*)tController->mData;

	int control = evaluateDreamAssignmentAndReturnAsInteger(&e->tValue, tPlayer);
	setPlayerControl(tPlayer, control);

	return 0;
}

static int handleWidth(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	WidthController* e = (WidthController*)tController->mData;

	Vector3DI stage;
	Vector3DI player;

	if (e->mHasValue) {
		stage = player = evaluateDreamAssignmentAndReturnAsVector3DI(&e->mValue, tPlayer);	
	}
	else {
		stage = player = makeVector3DI(0, 0, 0);
		getTwoIntegerValuesWithDefaultValues(&e->mEdge, tPlayer, &stage.x, &stage.y, 0, 0);
		getTwoIntegerValuesWithDefaultValues(&e->mPlayer, tPlayer, &player.x, &player.y, 0, 0);
	}

	setPlayerWidthOneFrame(tPlayer, stage, player);

	return 0;
}

static int handleSpritePriority(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	SpritePriorityController* e = (SpritePriorityController*)tController->mData;

	int value = evaluateDreamAssignmentAndReturnAsInteger(&e->tValue, tPlayer);
	setPlayerSpritePriority(tPlayer, value);

	return 0;
}

static void handleSingleSpecialAssert(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer) {
	string flag;
	evaluateDreamAssignmentAndReturnAsString(flag, tAssignment, tPlayer);

	if ("intro" == flag) {
		setPlayerIntroFlag(tPlayer);
	}
	else if ("invisible" == flag) {
		setPlayerInvisibleFlag(tPlayer);
	}
	else if ("roundnotover" == flag) {
		setDreamRoundNotOverFlag();
	}
	else if ("nobardisplay" == flag) {
		setDreamBarInvisibleForOneFrame();
	}
	else if ("nobg" == flag) {
		setDreamStageInvisibleForOneFrame();
	}
	else if ("nofg" == flag) {
		setDreamStageLayer1InvisibleForOneFrame();
	}
	else if ("nostandguard" == flag) {
		setPlayerNoStandGuardFlag(tPlayer);
	}
	else if ("nocrouchguard" == flag) {
		setPlayerNoCrouchGuardFlag(tPlayer);
	}
	else if ("noairguard" == flag) {
		setPlayerNoAirGuardFlag(tPlayer);
	}
	else if ("noautoturn" == flag) {
		setPlayerNoAutoTurnFlag(tPlayer);
	}
	else if ("nojugglecheck" == flag) {
		setPlayerNoJuggleCheckFlag(tPlayer);
	}
	else if ("nokosnd" == flag) {
		setPlayerNoKOSoundFlag(tPlayer);
	}
	else if ("nokoslow" == flag) {
		setPlayerNoKOSlowdownFlag(tPlayer);
	}
	else if ("noshadow" == flag) {
		setPlayerNoShadow(tPlayer);
	}
	else if ("globalnoshadow" == flag) {
		setAllPlayersNoShadow();
	}
	else if ("nomusic" == flag) {
		setNoMusicFlag();
	}
	else if ("nowalk" == flag) {
		setPlayerNoWalkFlag(tPlayer);
	}
	else if ("timerfreeze" == flag) {
		setTimerFreezeFlag();
	}
	else if ("unguardable" == flag) {
		setPlayerUnguardableFlag(tPlayer);
	}
	else {
		logWarningFormat("Unrecognized special assert flag %s. Ignoring.", flag);
	}
}

static int handleSpecialAssert(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	SpecialAssertController* e = (SpecialAssertController*)tController->mData;

	handleSingleSpecialAssert(&e->mFlag, tPlayer);

	if (e->mHasFlag2) {
		handleSingleSpecialAssert(&e->mFlag2, tPlayer);
	}
	if (e->mHasFlag3) {
		handleSingleSpecialAssert(&e->mFlag3, tPlayer);
	}

	return 0;
}

static int handleMakeDust(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	MakeDustController* e = (MakeDustController*)tController->mData;

	Vector3D pos = evaluateDreamAssignmentAndReturnAsVector3D(&e->mPositionOffset, tPlayer);
	int spacing = evaluateDreamAssignmentAndReturnAsInteger(&e->mSpacing, tPlayer);

	addPlayerDust(tPlayer, 0, pos, spacing);

	if (e->mHasSecondDustCloud) {
		pos = evaluateDreamAssignmentAndReturnAsVector3D(&e->mPositionOffset2, tPlayer);
		addPlayerDust(tPlayer, 1, pos, spacing);
	}

	return 0;
}

typedef struct {
	DreamPlayer* mPlayer;
	DreamPlayer* mTarget;

} VarSetHandlingCaller;

static void handleSettingSingleVariable(void* tCaller, void* tData) {
	VarSetHandlingCaller* caller = (VarSetHandlingCaller*)tCaller;
	VarSetControllerEntry* e = (VarSetControllerEntry*)tData;

	int id = evaluateDreamAssignmentAndReturnAsInteger(&e->mID, caller->mPlayer);

	if (e->mType == VAR_SET_TYPE_SYSTEM) {
		int val = evaluateDreamAssignmentAndReturnAsInteger(&e->mAssignment, caller->mPlayer);
		setPlayerSystemVariable(caller->mTarget, id, val);
	}
	else if (e->mType == VAR_SET_TYPE_INTEGER) {
		int val = evaluateDreamAssignmentAndReturnAsInteger(&e->mAssignment, caller->mPlayer);
		setPlayerVariable(caller->mTarget, id, val);
	}
	else if (e->mType == VAR_SET_TYPE_SYSTEM_FLOAT) {
		double val = evaluateDreamAssignmentAndReturnAsFloat(&e->mAssignment, caller->mPlayer);
		setPlayerSystemFloatVariable(caller->mTarget, id, val);
	}
	else if (e->mType == VAR_SET_TYPE_FLOAT) {
		double val = evaluateDreamAssignmentAndReturnAsFloat(&e->mAssignment, caller->mPlayer);
		setPlayerFloatVariable(caller->mTarget, id, val);
	}
	else {
		logWarningFormat("Unrecognized variable type %d. Ignoring.", e->mType);
	}
}

static int handleSettingVariable(DreamMugenStateController* tController, DreamPlayer* tPlayer, DreamPlayer* tTarget) {
	if (!tPlayer) return 0;

	VarSetController* e = (VarSetController*)tController->mData;
	VarSetHandlingCaller caller;
	caller.mPlayer = tPlayer;
	caller.mTarget = tTarget;

	vector_map(&e->mVarSets, handleSettingSingleVariable, &caller);

	return 0;
}

static void handleSettingSingleGlobalVariable(void* tCaller, void* tData) {
	VarSetHandlingCaller* caller = (VarSetHandlingCaller*)tCaller;
	VarSetControllerEntry* e = (VarSetControllerEntry*)tData;

	int id = evaluateDreamAssignmentAndReturnAsInteger(&e->mID, caller->mPlayer);

	if (e->mType == VAR_SET_TYPE_SYSTEM) {
		int val = evaluateDreamAssignmentAndReturnAsInteger(&e->mAssignment, caller->mPlayer);
		logWarning("Trying to set global system variable, defaulting to normal global variable.");
		setGlobalVariable(id, val);
	}
	else if (e->mType == VAR_SET_TYPE_INTEGER) {
		int val = evaluateDreamAssignmentAndReturnAsInteger(&e->mAssignment, caller->mPlayer);
		setGlobalVariable(id, val);
	}
	else if (e->mType == VAR_SET_TYPE_SYSTEM_FLOAT) {
		double val = evaluateDreamAssignmentAndReturnAsFloat(&e->mAssignment, caller->mPlayer);
		logWarning("Trying to set global system variable, defaulting to normal global variable.");
		setGlobalFloatVariable(id, val);
	}
	else if (e->mType == VAR_SET_TYPE_FLOAT) {
		double val = evaluateDreamAssignmentAndReturnAsFloat(&e->mAssignment, caller->mPlayer);
		setGlobalFloatVariable(id, val);
	}
	else {
		logWarningFormat("Unrecognized variable type %d. Ignoring.", e->mType);
	}
}

static int handleSettingGlobalVariable(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	if (!tPlayer) return 0;

	VarSetController* e = (VarSetController*)tController->mData;
	VarSetHandlingCaller caller;
	caller.mPlayer = tPlayer;
	caller.mTarget = NULL;

	vector_map(&e->mVarSets, handleSettingSingleGlobalVariable, &caller);

	return 0;
}

static void handleAddingSingleVariable(void* tCaller, void* tData) {
	VarSetHandlingCaller* caller = (VarSetHandlingCaller*)tCaller;
	VarSetControllerEntry* e = (VarSetControllerEntry*)tData;
	
	int id = evaluateDreamAssignmentAndReturnAsInteger(&e->mID, caller->mPlayer);

	if (e->mType == VAR_SET_TYPE_SYSTEM) {
		int val = evaluateDreamAssignmentAndReturnAsInteger(&e->mAssignment, caller->mPlayer);
		addPlayerSystemVariable(caller->mTarget, id, val);
	}
	else if (e->mType == VAR_SET_TYPE_INTEGER) {
		int val = evaluateDreamAssignmentAndReturnAsInteger(&e->mAssignment, caller->mPlayer);
		addPlayerVariable(caller->mTarget, id, val);
	}
	else if (e->mType == VAR_SET_TYPE_SYSTEM_FLOAT) {
		double val = evaluateDreamAssignmentAndReturnAsFloat(&e->mAssignment, caller->mPlayer);
		addPlayerSystemFloatVariable(caller->mTarget, id, val);
	}
	else if (e->mType == VAR_SET_TYPE_FLOAT) {
		double val = evaluateDreamAssignmentAndReturnAsFloat(&e->mAssignment, caller->mPlayer);
		addPlayerFloatVariable(caller->mTarget, id, val);
	}
	else {
		logWarningFormat("Unrecognized variable type %d. Ignoring.", e->mType);
	}
}

static int handleAddingVariable(DreamMugenStateController* tController, DreamPlayer* tPlayer, DreamPlayer* tTarget) {
	if (!tPlayer) return 0;
	
	VarSetController* e = (VarSetController*)tController->mData;
	VarSetHandlingCaller caller;
	caller.mPlayer = tPlayer;
	caller.mTarget = tTarget;

	vector_map(&e->mVarSets, handleAddingSingleVariable, &caller);

	return 0;
}

static void handleAddingSingleGlobalVariable(void* tCaller, void* tData) {
	VarSetHandlingCaller* caller = (VarSetHandlingCaller*)tCaller;
	VarSetControllerEntry* e = (VarSetControllerEntry*)tData;

	int id = evaluateDreamAssignmentAndReturnAsInteger(&e->mID, caller->mPlayer);

	if (e->mType == VAR_SET_TYPE_SYSTEM) {
		int val = evaluateDreamAssignmentAndReturnAsInteger(&e->mAssignment, caller->mPlayer);
		logWarning("Trying to set global system variable, defaulting to normal global variable.");
		addGlobalVariable(id, val);
	}
	else if (e->mType == VAR_SET_TYPE_INTEGER) {
		int val = evaluateDreamAssignmentAndReturnAsInteger(&e->mAssignment, caller->mPlayer);
		addGlobalVariable(id, val);
	}
	else if (e->mType == VAR_SET_TYPE_SYSTEM_FLOAT) {
		double val = evaluateDreamAssignmentAndReturnAsFloat(&e->mAssignment, caller->mPlayer);
		logWarning("Trying to set global system variable, defaulting to normal global variable.");
		addGlobalFloatVariable(id, val);
	}
	else if (e->mType == VAR_SET_TYPE_FLOAT) {
		double val = evaluateDreamAssignmentAndReturnAsFloat(&e->mAssignment, caller->mPlayer);
		addGlobalFloatVariable(id, val);
	}
	else {
		logWarningFormat("Unrecognized variable type %d. Ignoring.", e->mType);
	}
}

static int handleAddingGlobalVariable(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	if (!tPlayer) return 0;

	VarSetController* e = (VarSetController*)tController->mData;
	VarSetHandlingCaller caller;
	caller.mPlayer = tPlayer;
	caller.mTarget = NULL;

	vector_map(&e->mVarSets, handleAddingSingleGlobalVariable, &caller);

	return 0;
}

static int handleNull() { return 0; }

static DreamMugenStateType handleStateTypeAssignment(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer) {
	DreamMugenStateType ret;

	string text;
	evaluateDreamAssignmentAndReturnAsString(text, tAssignment, tPlayer);
	
	if ("a" == text) ret = MUGEN_STATE_TYPE_AIR;
	else if ("s" == text) ret = MUGEN_STATE_TYPE_STANDING;
	else if ("c" == text) ret = MUGEN_STATE_TYPE_CROUCHING;
	else {
		logWarningFormat("Unrecognized state type %s. Defaulting to not changing state.", text);
		ret = MUGEN_STATE_TYPE_UNCHANGED;
	}

	return ret;
}

static DreamMugenStatePhysics handleStatePhysicsAssignment(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer) {
	DreamMugenStatePhysics ret;

	string text;
	evaluateDreamAssignmentAndReturnAsString(text, tAssignment, tPlayer);
	if ("s" == text) ret = MUGEN_STATE_PHYSICS_STANDING;
	else if ("a" == text) ret = MUGEN_STATE_PHYSICS_AIR;
	else if ("c" == text) ret = MUGEN_STATE_PHYSICS_CROUCHING;
	else if ("n" == text) ret = MUGEN_STATE_PHYSICS_NONE;
	else {
		logWarning("Unrecognized state physics type");
		logWarningString(text);
		ret = MUGEN_STATE_PHYSICS_UNCHANGED;
	}

	return ret;
}

static DreamMugenStateMoveType handleStateMoveTypeAssignment(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer) {
	DreamMugenStateMoveType ret;

	string text;
	evaluateDreamAssignmentAndReturnAsString(text, tAssignment, tPlayer);
	if ("a" == text) ret = MUGEN_STATE_MOVE_TYPE_ATTACK;
	else if ("i" == text) ret = MUGEN_STATE_MOVE_TYPE_IDLE;
	else {
		logWarning("Unrecognized state move type");
		logWarningString(text);
		ret = MUGEN_STATE_MOVE_TYPE_UNCHANGED;
	}

	return ret;
}

static int handleStateTypeSet(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	StateTypeSetController* e = (StateTypeSetController*)tController->mData;

	if (e->mHasStateType) {
		DreamMugenStateType type = handleStateTypeAssignment(&e->mStateType, tPlayer);
		setPlayerStateType(tPlayer, type);
	}

	if (e->mHasMoveType) {
		DreamMugenStateMoveType moveType = handleStateMoveTypeAssignment(&e->mMoveType, tPlayer);
		setPlayerStateMoveType(tPlayer, moveType);
	}

	if (e->mHasPhysics) {
		DreamMugenStatePhysics physics = handleStatePhysicsAssignment(&e->mPhysics, tPlayer);
		setPlayerPhysics(tPlayer, physics);
	}

	return 0;
}

static int handleHitVelocitySetting(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	Set2DPhysicsController* e = (Set2DPhysicsController*)tController->mData;

	if (e->mIsSettingX) {
		double x = evaluateDreamAssignmentAndReturnAsFloat(&e->x, tPlayer);
		setActiveHitDataVelocityX(tPlayer, x);
	}

	if (e->mIsSettingY) {
		double y = evaluateDreamAssignmentAndReturnAsFloat(&e->y, tPlayer);
		setActiveHitDataVelocityY(tPlayer, y);
	}

	return 0;
}

static int handleDefenseMultiplier(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	DefenseMultiplierController* e = (DefenseMultiplierController*)tController->mData;

	double val = evaluateDreamAssignmentAndReturnAsFloat(&e->mValue, tPlayer);
	setPlayerDefenseMultiplier(tPlayer, val);

	return 0;
}

static int handleFallEnvironmentShake(DreamPlayer* tPlayer) {

	if (!isActiveHitDataActive(tPlayer)) return 0;

	int time = getActiveHitDataFallEnvironmentShakeTime(tPlayer);
	setActiveHitDataFallEnvironmentShakeTime(tPlayer, 0);
	double freq = getActiveHitDataFallEnvironmentShakeFrequency(tPlayer);
	int ampl = getActiveHitDataFallEnvironmentShakeAmplitude(tPlayer);
	double phase = getActiveHitDataFallEnvironmentShakePhase(tPlayer);

	setEnvironmentShake(time, freq, ampl, phase);

	return 0;
}

static void handleExplodAnimation(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, int tID) {
	string text;
	evaluateDreamAssignmentAndReturnAsString(text, tAssignment, tPlayer);

	char firstW[100];
	int anim;
	int items = sscanf(text.data(), "%s %d", firstW, &anim);

	int isInFightDefFile;
	if (items > 1 && !strcmp("isinotherfilef", firstW)) {
		isInFightDefFile = 1;
		if (items < 2) anim = -1;
	}
	else {
		isInFightDefFile = 0;
		anim = atoi(text.data());
	}

	setExplodAnimation(tID, isInFightDefFile, anim);
}

DreamExplodPositionType getPositionTypeFromAssignment(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer) {
	string text;
	evaluateDreamAssignmentAndReturnAsString(text, tAssignment, tPlayer);

	DreamExplodPositionType type;
	if ("p1" == text) {
		type = EXPLOD_POSITION_TYPE_RELATIVE_TO_P1;
	}
	else if ("p2" == text) {
		type = EXPLOD_POSITION_TYPE_RELATIVE_TO_P2;
	}
	else if ("front" == text) {
		type = EXPLOD_POSITION_TYPE_RELATIVE_TO_FRONT;
	}
	else if ("back" == text) {
		type = EXPLOD_POSITION_TYPE_RELATIVE_TO_BACK;
	}
	else if ("left" == text) {
		type = EXPLOD_POSITION_TYPE_RELATIVE_TO_LEFT;
	}
	else if ("right" == text) {
		type = EXPLOD_POSITION_TYPE_RELATIVE_TO_RIGHT;
	}
	else if ("none" == text) {
		type = EXPLOD_POSITION_TYPE_NONE;
	}
	else {
		logWarningFormat("Unable to determine position type %s. Defaulting to EXPLOD_POSITION_TYPE_RELATIVE_TO_P1.", text);
		type = EXPLOD_POSITION_TYPE_RELATIVE_TO_P1;
	}

	return type;
}

static void handleExplodPositionType(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, int tID) {

	DreamExplodPositionType type = getPositionTypeFromAssignment(tAssignment, tPlayer);
	setExplodPositionType(tID, type);
}

static void handleExplodTransparencyType(DreamMugenAssignment** tAssignment, int isUsed, DreamPlayer* tPlayer, int tID) {
	if (!isUsed) {
		setExplodTransparencyType(tID, 0, EXPLOD_TRANSPARENCY_TYPE_ALPHA);
		return;
	}

	string text;
	evaluateDreamAssignmentAndReturnAsString(text, tAssignment, tPlayer);

	DreamExplodTransparencyType type;
	if ("alpha" == text) {
		type = EXPLOD_TRANSPARENCY_TYPE_ALPHA;
	}
	else if ("addalpha" == text) {
		type = EXPLOD_TRANSPARENCY_TYPE_ADD_ALPHA;
	}
	else {
		logWarningFormat("Unable to determine explod transparency type %s. Default to alpha.", text);
		type = EXPLOD_TRANSPARENCY_TYPE_ALPHA;
	}

	setExplodTransparencyType(tID, 1, type);
}

static int handleExplod(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	ExplodController* e = (ExplodController*)tController->mData;

	int id = addExplod(tPlayer);
	handleExplodAnimation(&e->mAnim, tPlayer, id);
	handleExplodOneIntegerElement(&e->mID, tPlayer, id, setExplodID, -1);
	handleExplodTwoIntegerElements(&e->mPosition, tPlayer, id, setExplodPosition, 0, 0);
	handleExplodPositionType(&e->mPositionType, tPlayer, id);
	handleExplodOneIntegerElement(&e->mHorizontalFacing, tPlayer, id, setExplodHorizontalFacing, 1);
	handleExplodOneIntegerElement(&e->mVerticalFacing, tPlayer, id, setExplodVerticalFacing, 1);
	handleExplodOneIntegerElement(&e->mBindTime, tPlayer, id, setExplodBindTime, 0);
	handleExplodTwoFloatElements(&e->mVelocity, tPlayer, id, setExplodVelocity, 0, 0);
	handleExplodTwoFloatElements(&e->mAcceleration, tPlayer, id, setExplodAcceleration, 0, 0);
	handleExplodTwoIntegerElements(&e->mRandomOffset, tPlayer, id, setExplodRandomOffset, 0, 0);

	handleExplodOneIntegerElement(&e->mRemoveTime, tPlayer, id, setExplodRemoveTime, -2);
	handleExplodOneIntegerElement(&e->mSuperMove, tPlayer, id, setExplodSuperMove, 0);
	handleExplodOneIntegerElement(&e->mSuperMoveTime, tPlayer, id, setExplodSuperMoveTime, 0);
	handleExplodOneIntegerElement(&e->mPauseMoveTime, tPlayer, id, setExplodPauseMoveTime, 0);
	handleExplodTwoFloatElements(&e->mScale, tPlayer, id, setExplodScale, 1, 1);
	handleExplodOneIntegerElement(&e->mSpritePriority, tPlayer, id, setExplodSpritePriority, 0);
	handleExplodOneIntegerElement(&e->mOnTop, tPlayer, id, setExplodOnTop, 0);
	handleExplodThreeIntegerElements(&e->mShadow, tPlayer, id, setExplodShadow, 0, 0, 0);
	handleExplodOneIntegerElement(&e->mOwnPalette, tPlayer, id, setExplodOwnPalette, 0);
	handleExplodOneIntegerElement(&e->mIsRemovedOnGetHit, tPlayer, id, setExplodRemoveOnGetHit, 0);
	handleExplodOneIntegerElement(&e->mIgnoreHitPause, tPlayer, id, setExplodIgnoreHitPause, 1);
	handleExplodTransparencyType(&e->mTransparencyType, e->mHasTransparencyType, tPlayer, id);

	finalizeExplod(id);

	return 0;
}

static int modifyExplod(DreamMugenStateController* /*tController*/, DreamPlayer* /*tPlayer*/) {
	return 0; // TODO: readd modifyExplod (https://dev.azure.com/captdc/DogmaRnDA/_workitems/edit/430)
	/*
	ExplodController* e = (ExplodController*)tController->mData;
	int id;
	getSingleIntegerValueOrDefault(&e->mID, tPlayer, &id, -1);
	removeExplodsWithID(tPlayer, id);

	handleExplod(tController, tPlayer);

	return 0;
	*/
}

static int handleHitFallDamage(DreamPlayer* tPlayer) {
	if (!isActiveHitDataActive(tPlayer)) return 0;

	int fallDamage = getActiveHitDataFallDamage(tPlayer);
	addPlayerDamage(tPlayer, getPlayerOtherPlayer(tPlayer), fallDamage);

	return 0;
}

static int handlePositionFreeze(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	PositionFreezeController* e = (PositionFreezeController*)tController->mData;

	int val = evaluateDreamAssignment(&e->mValue, tPlayer);
	if (val) setPlayerPositionFrozen(tPlayer);

	return 0;
}

static int handleHitFallVelocity(DreamPlayer* tPlayer) {

	addPlayerVelocityX(tPlayer, getActiveHitDataFallXVelocity(tPlayer), getPlayerCoordinateP(tPlayer));
	addPlayerVelocityY(tPlayer, getActiveHitDataFallYVelocity(tPlayer), getPlayerCoordinateP(tPlayer));

	return 0;
}


static void handleSingleNotHitBy(int tSlot, int tHasValue, MugenStringVector tValue, int tTime, DreamPlayer* tPlayer, void(*tResetFunc)(DreamPlayer*, int)) {
	if (!tHasValue) return;
	tResetFunc(tPlayer, tSlot);

	char* flag1 = tValue.mElement[0];
	setPlayerNotHitByFlag1(tPlayer, tSlot, flag1);
	int i;
	for (i = 1; i < tValue.mSize; i++) {
		char* flag2 = tValue.mElement[i];
		addPlayerNotHitByFlag2(tPlayer, tSlot, flag2);
	}

	setPlayerNotHitByTime(tPlayer, tSlot, tTime);
}

static void handleReversalDefinitionEntry(MugenStringVector tValue, DreamPlayer* tPlayer) {
	resetHitDataReversalDef(tPlayer);
	
	char* flag1 = tValue.mElement[0];
	setHitDataReversalDefFlag1(tPlayer, flag1);
	int i;
	for (i = 1; i < tValue.mSize; i++) {
		char* flag2 = tValue.mElement[i];
		addHitDataReversalDefFlag2(tPlayer, flag2);
	}
}



static int handleNotHitBy(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	NotHitByController* e = (NotHitByController*)tController->mData;

	int time = evaluateDreamAssignmentAndReturnAsInteger(&e->mTime, tPlayer);

	handleSingleNotHitBy(0, e->mHasValue, e->mValue, time, tPlayer, resetPlayerNotHitBy);
	handleSingleNotHitBy(1, e->mHasValue2, e->mValue2, time, tPlayer, resetPlayerNotHitBy);

	return 0;
}

static int handleHitBy(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	NotHitByController* e = (NotHitByController*)tController->mData;

	int time = evaluateDreamAssignmentAndReturnAsInteger(&e->mTime, tPlayer);

	handleSingleNotHitBy(0, e->mHasValue, e->mValue, time, tPlayer, resetPlayerHitBy);
	handleSingleNotHitBy(1, e->mHasValue2, e->mValue2, time, tPlayer, resetPlayerHitBy);

	return 0;
}

static int handleHitFallSet(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	HitFallSetController* e = (HitFallSetController*)tController->mData;
	DreamPlayer* otherPlayer = getPlayerOtherPlayer(tPlayer);

	int val = evaluateDreamAssignmentAndReturnAsInteger(&e->mValue, tPlayer);
	if (val != -1) {
		setActiveHitDataFall(otherPlayer, val);
	}

	if (e->mHasXVelocity) {
		double x = evaluateDreamAssignmentAndReturnAsFloat(&e->mXVelocity, tPlayer);
		setActiveHitDataFallXVelocity(otherPlayer, x);
	}

	if (e->mHasYVelocity) {
		double y = evaluateDreamAssignmentAndReturnAsFloat(&e->mYVelocity, tPlayer);
		setActiveHitDataFallYVelocity(otherPlayer, y);
	}

	return 0;
}


static int handleAttackMultiplierSetting(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	SingleRequiredValueController* e = (SingleRequiredValueController*)tController->mData;

	double value = evaluateDreamAssignmentAndReturnAsFloat(&e->mValue, tPlayer);

	setPlayerAttackMultiplier(tPlayer, value);

	return 0;
}

static int handlePowerAddition(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	SingleRequiredValueController* e = (SingleRequiredValueController*)tController->mData;

	int value = evaluateDreamAssignmentAndReturnAsInteger(&e->mValue, tPlayer);
	addPlayerPower(tPlayer, value);

	return 0;
}

static int handlePowerSettingController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	SingleRequiredValueController* e = (SingleRequiredValueController*)tController->mData;

	int value = evaluateDreamAssignmentAndReturnAsInteger(&e->mValue, tPlayer);
	setPlayerPower(tPlayer, value);

	return 0;
}

static void handleSuperPauseAnimation(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer) {
	int isInPlayerFile;
	int id;
	if (!(*tAssignment)) {
		isInPlayerFile = 0;
		id = 30;
	}
	else {
		string flag;
		evaluateDreamAssignmentAndReturnAsString(flag, tAssignment, tPlayer);

		char firstW[200];
		int which = -1;

		int items = sscanf(flag.data(), "%s %d", firstW, &which);

		if (items < 1) {
			isInPlayerFile = 0;
			id = 30;
		}
		else if (!strcmp("isinotherfilef", firstW) || !strcmp("isinotherfiles", firstW)) {
			assert(items == 2);
			isInPlayerFile = 1;
			id = which;
		}
		else {
			assert(items == 1);
			isInPlayerFile = 0;
			id = atoi(flag.data());
		}
	}

	setDreamSuperPauseAnimation(tPlayer, isInPlayerFile, id);
}

static void handleSuperPauseSound(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer) {
	int isInPlayerFile;
	int group;
	int item;
	if (!(*tAssignment)) {
		isInPlayerFile = 0;
		group = -1;
		item = -1;
	}
	else {
		string flag;
		evaluateDreamAssignmentAndReturnAsString(flag, tAssignment, tPlayer);

		char firstW[20], comma[10];
		int items = sscanf(flag.data(), "%s", firstW);
		(void)items;
		assert(items == 1);

		if (!strcmp("isinotherfilef", firstW)) {
			isInPlayerFile = 0;
			items = sscanf(flag.data(), "%s %d %s %d", firstW, &group, comma, &item);
			if (items < 2) { 
				group = 0;
			}
			if (items < 4) {
				item = 0;
			}
		}
		else if (!strcmp("isinotherfiles", firstW)) {
			isInPlayerFile = 1;
			items = sscanf(flag.data(), "%s %d %s %d", firstW, &group, comma, &item);
			if (items < 2) { 
				group = 0;
			}
			if (items < 4) {
				item = 0;
			}
		}
		else {
			isInPlayerFile = 0;
			items = sscanf(flag.data(), "%d %s %d", &group, comma, &item);
			if (items != 3) {
				logWarningFormat("Unable to parse super pause flag %s. Ignoring.", flag);
				return;
			}
		}
	}

	setDreamSuperPauseSound(tPlayer, isInPlayerFile, group, item);
}

static int handleSuperPause(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	SuperPauseController* e = (SuperPauseController*)tController->mData;

	setDreamSuperPauseActive(tPlayer);

	handleHitDefinitionOneIntegerElement(&e->mTime, tPlayer, setDreamSuperPauseTime, 30);
	handleHitDefinitionOneIntegerElement(&e->mBufferTimeForCommandsDuringPauseEnd, tPlayer, setDreamSuperPauseBufferTimeForCommandsDuringPauseEnd, 0);
	handleHitDefinitionOneIntegerElement(&e->mMoveTime, tPlayer, setDreamSuperPauseMoveTime, 0);
	handleHitDefinitionOneIntegerElement(&e->mDoesPauseBackground, tPlayer, setDreamSuperPauseIsPausingBG, 1);

	handleSuperPauseAnimation(&e->mAnim, tPlayer);
	handleSuperPauseSound(&e->mSound, tPlayer);

	handleHitDefinitionTwoFloatElements(&e->mPosition, tPlayer, setDreamSuperPausePosition, 0, 0);
	handleHitDefinitionOneIntegerElement(&e->mIsDarkening, tPlayer, setDreamSuperPauseDarkening, 1);
	handleHitDefinitionOneFloatElement(&e->mPlayer2DefenseMultiplier, tPlayer, setDreamSuperPausePlayer2DefenseMultiplier, 0);
	handleHitDefinitionOneIntegerElement(&e->mPowerToAdd, tPlayer, setDreamSuperPausePowerToAdd, 0);
	handleHitDefinitionOneIntegerElement(&e->mSetPlayerUnhittable, tPlayer, setDreamSuperPausePlayerUnhittability, 1);

	return 0;
}

static int handlePauseController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	PauseController* e = (PauseController*)tController->mData;

	setDreamPauseActive(tPlayer);

	handleHitDefinitionOneIntegerElement(&e->mTime, tPlayer, setDreamPauseTime, 30);
	handleHitDefinitionOneIntegerElement(&e->mBufferTimeForCommandsDuringPauseEnd, tPlayer, setDreamPauseBufferTimeForCommandsDuringPauseEnd, 0);
	handleHitDefinitionOneIntegerElement(&e->mMoveTime, tPlayer, setDreamPauseMoveTime, 0);
	handleHitDefinitionOneIntegerElement(&e->mDoesPauseBackground, tPlayer, setDreamPauseIsPausingBG, 1);

	return 0;
}

static void handleHelperOneIntegerElement(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, DreamPlayer* tHelper, void(tFunc)(DreamPlayer*, int), int tDefault) {
	int val;
	getSingleIntegerValueOrDefault(tAssignment, tPlayer, &val, tDefault);
	tFunc(tHelper, val);
}

static void handleHelperTwoFloatElements(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, DreamPlayer* tHelper, void(tFunc)(DreamPlayer*, double, double), double tDefault1, double tDefault2) {
	double val1, val2;
	getTwoFloatValuesWithDefaultValues(tAssignment, tPlayer, &val1, &val2, tDefault1, tDefault2);
	tFunc(tHelper, val1, val2);
}

static void handleHelperTwoIntegerElements(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, DreamPlayer* tHelper, void(tFunc)(DreamPlayer*, int, int), int tDefault1, int tDefault2) {
	int val1, val2;
	getTwoIntegerValuesWithDefaultValues(tAssignment, tPlayer, &val1, &val2, tDefault1, tDefault2);
	tFunc(tHelper, val1, val2);
}

static void handleHelperFacing(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, DreamPlayer* tHelper) {
	int facing;
	getSingleIntegerValueOrDefault(tAssignment, tPlayer, &facing, 1);

	if (facing == 1) {
		setPlayerIsFacingRight(tHelper, getPlayerIsFacingRight(tPlayer));
	}
	else {
		setPlayerIsFacingRight(tHelper, !getPlayerIsFacingRight(tPlayer));
	}

}

static Position getFinalHelperPositionFromPositionType(DreamExplodPositionType tPositionType, Position mOffset, DreamPlayer* tPlayer) {
	Position p1 = getPlayerPosition(tPlayer, getPlayerCoordinateP(tPlayer));


	if (tPositionType == EXPLOD_POSITION_TYPE_RELATIVE_TO_P1) {
		DreamPlayer* target = tPlayer;
		int isReversed = !getPlayerIsFacingRight(target);
		if (isReversed) mOffset.x *= -1;
		return vecAdd(p1, mOffset);
	}
	else if (tPositionType == EXPLOD_POSITION_TYPE_RELATIVE_TO_P2) {
		DreamPlayer* target = getPlayerOtherPlayer(tPlayer);
		Position p2 = getPlayerPosition(target, getPlayerCoordinateP(tPlayer));
		int isReversed = !getPlayerIsFacingRight(target);
		if (isReversed) mOffset.x *= -1;
		return vecAdd(p2, mOffset);
	}
	else if (tPositionType == EXPLOD_POSITION_TYPE_RELATIVE_TO_FRONT) {
		DreamPlayer* target = tPlayer;
		Position p = makePosition(getPlayerScreenEdgeInFrontX(target), p1.y, 0);
		int isReversed = getPlayerIsFacingRight(target);
		if (isReversed) mOffset.x *= -1;
		return vecAdd(p, mOffset);
	}
	else if (tPositionType == EXPLOD_POSITION_TYPE_RELATIVE_TO_BACK) {
		DreamPlayer* target = tPlayer;
		Position p = makePosition(getPlayerScreenEdgeInBackX(target), p1.y, 0);
		int isReversed = !getPlayerIsFacingRight(target);
		if (isReversed) mOffset.x *= -1;
		return vecAdd(p, mOffset);
	}
	else if (tPositionType == EXPLOD_POSITION_TYPE_RELATIVE_TO_LEFT) {
		if (getPlayerIsFacingRight(tPlayer)) {
			Position p = makePosition(getDreamStageLeftOfScreenBasedOnPlayer(getPlayerCoordinateP(tPlayer)), p1.y, 0);
			return vecAdd(p, mOffset);
		}
		else {
			Position p = makePosition(getDreamStageRightOfScreenBasedOnPlayer(getPlayerCoordinateP(tPlayer)), p1.y, 0);
			p.x -= mOffset.x;
			p.y += mOffset.y;
			return p;
		}
	}
	else if (tPositionType == EXPLOD_POSITION_TYPE_RELATIVE_TO_RIGHT) {
		if (getPlayerIsFacingRight(tPlayer)) {
			Position p = makePosition(getDreamStageRightOfScreenBasedOnPlayer(getPlayerCoordinateP(tPlayer)), p1.y, 0);
			return vecAdd(p, mOffset);
		}
		else {
			Position p = makePosition(getDreamStageLeftOfScreenBasedOnPlayer(getPlayerCoordinateP(tPlayer)), p1.y, 0);
			p.x -= mOffset.x;
			p.y += mOffset.y;
			return p;
		}
	}
	else if (tPositionType == EXPLOD_POSITION_TYPE_NONE) {
		Position p = makePosition(getDreamGameWidth(getPlayerCoordinateP(tPlayer)) / 2, p1.y, 0);
		return vecAdd(p, mOffset);
	}
	else {
		logWarningFormat("Unrecognized position type %d. Defaulting to EXPLOD_POSITION_TYPE_RELATIVE_TO_P1.", tPositionType);
		DreamPlayer* target = tPlayer;
		int isReversed = !getPlayerIsFacingRight(target);
		if (isReversed) mOffset.x *= -1;
		return vecAdd(p1, mOffset);
	}

}

static void handleHelperScale(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer, DreamPlayer* tHelper, void(tFunc)(DreamPlayer*, double), double tParentScale) {
	double val;
	getSingleFloatValueOrDefault(tAssignment, tPlayer, &val, 1.0);
	tFunc(tHelper, val*tParentScale);
}

static int handleHelper(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	HelperController* e = (HelperController*)tController->mData;
	DreamPlayer* helper = clonePlayerAsHelper(tPlayer);

	handleHelperOneIntegerElement(&e->mID, tPlayer, helper, setPlayerID, 0);

	Vector3DI mOffset = makeVector3DI(0, 0, 0);
	getTwoIntegerValuesWithDefaultValues(&e->mPosition, tPlayer, &mOffset.x, &mOffset.y, 0, 0);
	DreamExplodPositionType positionType = getPositionTypeFromAssignment(&e->mPositionType, tPlayer);
	Position position = getFinalHelperPositionFromPositionType(positionType, makePosition(mOffset.x, mOffset.y, mOffset.z), tPlayer);

	handleHelperFacing(&e->mFacing, tPlayer, helper);


	handleHelperOneIntegerElement(&e->mCanControl, tPlayer, helper, setPlayerHelperControl, 0);
	// TODO: own palette (https://dev.azure.com/captdc/DogmaRnDA/_workitems/edit/297)
	// TODO: supermovetime (https://dev.azure.com/captdc/DogmaRnDA/_workitems/edit/297)
	// TODO: pausemovetime (https://dev.azure.com/captdc/DogmaRnDA/_workitems/edit/297)

	handleHelperScale(&e->mSizeScaleX, tPlayer, helper, setPlayerRelativeScaleX, getPlayerScaleX(tPlayer));
	handleHelperScale(&e->mSizeScaleY, tPlayer, helper, setPlayerRelativeScaleY, getPlayerScaleY(tPlayer));
	handleHelperOneIntegerElement(&e->mSizeGroundBack, tPlayer, helper, setPlayerGroundSizeBack, getPlayerGroundSizeBack(tPlayer));
	handleHelperOneIntegerElement(&e->mSizeGroundFront, tPlayer, helper, setPlayerGroundSizeFront, getPlayerGroundSizeFront(tPlayer));
	handleHelperOneIntegerElement(&e->mSizeAirBack, tPlayer, helper, setPlayerAirSizeBack, getPlayerAirSizeBack(tPlayer));
	handleHelperOneIntegerElement(&e->mSizeAirFront, tPlayer, helper, setPlayerAirSizeFront, getPlayerAirSizeFront(tPlayer));
	handleHelperOneIntegerElement(&e->mSizeHeight, tPlayer, helper, setPlayerHeight, getPlayerHeight(tPlayer));

	// TODO: scale projectiles (https://dev.azure.com/captdc/DogmaRnDA/_workitems/edit/297)

	handleHelperTwoFloatElements(&e->mSizeHeadPosition, tPlayer, helper, setPlayerHeadPosition, getPlayerHeadPositionX(tPlayer), getPlayerHeadPositionY(tPlayer));
	handleHelperTwoFloatElements(&e->mSizeMiddlePosition, tPlayer, helper, setPlayerMiddlePosition, getPlayerMiddlePositionX(tPlayer), getPlayerMiddlePositionY(tPlayer));
	handleHelperOneIntegerElement(&e->mSizeShadowOffset, tPlayer, helper, setPlayerShadowOffset, getPlayerShadowOffset(tPlayer));

	string type;
	evaluateDreamAssignmentAndReturnAsString(type, &e->mType, tPlayer);
	if ("player" == type) {
		setPlayerScreenBoundForever(helper, 1);
	}
	else {
		setPlayerScreenBoundForever(helper, 0);
	}

	setPlayerPosition(helper, position, getPlayerCoordinateP(helper));
	handleHelperOneIntegerElement(&e->mStateNumber, tPlayer, helper, changePlayerState, 0);



	return 0;
}

static int handleDestroySelf(DreamPlayer* tPlayer) {
	logFormat("%d %d destroying self\n", tPlayer->mRootID, tPlayer->mID);

	destroyPlayer(tPlayer);

	return 1;
}

static int handleAddingLife(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	LifeAddController* e = (LifeAddController*)tController->mData;

	int val, canKill, isAbsolute;
	getSingleIntegerValueOrDefault(&e->mValue, tPlayer, &val, 0);
	getSingleIntegerValueOrDefault(&e->mCanKill, tPlayer, &canKill, 1);
	getSingleIntegerValueOrDefault(&e->mIsAbsolute, tPlayer, &isAbsolute, 0);

	if (!isAbsolute) {
		const auto defenseMultiplier = (1.0 / getPlayerDefenseMultiplier(tPlayer));
		if (defenseMultiplier != 1.0) val = int(val * defenseMultiplier);
	}

	int playerLife = getPlayerLife(tPlayer);
	if (!canKill && playerLife + val <= 0) {
		val = -(playerLife - 1);
	}

	addPlayerDamage(tPlayer, tPlayer, -val);

	return 0;
}

static int handleAddingTargetLife(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	TargetLifeAddController* e = (TargetLifeAddController*)tController->mData;

	int val, canKill, isAbsolute, id;
	getSingleIntegerValueOrDefault(&e->mValue, tPlayer, &val, 0);
	getSingleIntegerValueOrDefault(&e->mID, tPlayer, &id, -1);
	getSingleIntegerValueOrDefault(&e->mCanKill, tPlayer, &canKill, 1);
	getSingleIntegerValueOrDefault(&e->mIsAbsolute, tPlayer, &isAbsolute, 0);

	addPlayerTargetLife(tPlayer, tPlayer, id, val, canKill, isAbsolute);

	return 0;
}

static int handleAddingTargetPower(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	TargetPowerAddController* e = (TargetPowerAddController*)tController->mData;

	int val, id;
	getSingleIntegerValueOrDefault(&e->mValue, tPlayer, &val, 0);
	getSingleIntegerValueOrDefault(&e->mID, tPlayer, &id, -1);

	addPlayerTargetPower(tPlayer, id, val);

	return 0;
}

static int handleTargetVelocityAddController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	Target2DPhysicsController* e = (Target2DPhysicsController*)tController->mData;

	int id;
	getSingleIntegerValueOrDefault(&e->mID, tPlayer, &id, -1);

	if (e->mIsSettingX) {
		double x = evaluateDreamAssignmentAndReturnAsFloat(&e->x, tPlayer);
		addPlayerTargetVelocityX(tPlayer, id, x, getPlayerCoordinateP(tPlayer));
	}

	if (e->mIsSettingY) {
		double y = evaluateDreamAssignmentAndReturnAsFloat(&e->y, tPlayer);
		addPlayerTargetVelocityY(tPlayer, id, y, getPlayerCoordinateP(tPlayer));
	}

	return 0;
}

static int handleTargetVelocitySetController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	Target2DPhysicsController* e = (Target2DPhysicsController*)tController->mData;

	int id;
	getSingleIntegerValueOrDefault(&e->mID, tPlayer, &id, -1);

	if (e->mIsSettingX) {
		double x = evaluateDreamAssignmentAndReturnAsFloat(&e->x, tPlayer);
		setPlayerTargetVelocityX(tPlayer, id, x, getPlayerCoordinateP(tPlayer));
	}

	if (e->mIsSettingY) {
		double y = evaluateDreamAssignmentAndReturnAsFloat(&e->y, tPlayer);
		setPlayerTargetVelocityY(tPlayer, id, y, getPlayerCoordinateP(tPlayer));
	}

	return 0;
}

static int handleAngleDrawController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	AngleDrawController* e = (AngleDrawController*)tController->mData;

	if (e->mHasScale) {
		Vector3D scale = evaluateDreamAssignmentAndReturnAsVector3D(&e->mScale, tPlayer);
		setPlayerTempScaleActive(tPlayer, scale);
	}

	if (e->mHasValue) {
		double val = evaluateDreamAssignmentAndReturnAsFloat(&e->mValue, tPlayer);
		setPlayerDrawAngleValue(tPlayer, val);
	}

	setPlayerDrawAngleActive(tPlayer);

	return 0;
}

static int handleAngleAddController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	SingleRequiredValueController* e = (SingleRequiredValueController*)tController->mData;

	double angle = evaluateDreamAssignmentAndReturnAsFloat(&e->mValue, tPlayer);
	addPlayerDrawAngle(tPlayer, angle);

	return 0;
}

static int handleAngleMulController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	SingleRequiredValueController* e = (SingleRequiredValueController*)tController->mData;

	double angle = evaluateDreamAssignmentAndReturnAsFloat(&e->mValue, tPlayer);
	multiplyPlayerDrawAngle(tPlayer, angle);

	return 0;
}

static int handleAngleSetController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	SingleRequiredValueController* e = (SingleRequiredValueController*)tController->mData;

	double angle = evaluateDreamAssignmentAndReturnAsFloat(&e->mValue, tPlayer);
	setPlayerDrawAngleValue(tPlayer, angle);

	return 0;
}

static int handleRemovingExplod(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	RemoveExplodController* e = (RemoveExplodController*)tController->mData;

	if (e->mHasID) {
		int id = evaluateDreamAssignmentAndReturnAsInteger(&e->mID, tPlayer);
		removeExplodsWithID(tPlayer, id);
	}
	else {
		removeAllExplodsForPlayer(tPlayer);
	}

	return 0;
}

static int handleBindToRootController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	BindController* e = (BindController*)tController->mData;

	int time, facing;
	Vector3D offset = makePosition(0, 0, 0);
	getSingleIntegerValueOrDefault(&e->mTime, tPlayer, &time, 1);
	getSingleIntegerValueOrDefault(&e->mFacing, tPlayer, &facing, 0);
	getTwoFloatValuesWithDefaultValues(&e->mPosition, tPlayer, &offset.x, &offset.y, 0, 0);

	bindPlayerToRoot(tPlayer, time, facing, offset);

	return 0;
}

static int handleBindToParentController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	BindController* e = (BindController*)tController->mData;

	int time, facing;
	Vector3D offset = makePosition(0, 0, 0);
	getSingleIntegerValueOrDefault(&e->mTime, tPlayer, &time, 1);
	getSingleIntegerValueOrDefault(&e->mFacing, tPlayer, &facing, 0);
	getTwoFloatValuesWithDefaultValues(&e->mPosition, tPlayer, &offset.x, &offset.y, 0, 0);

	bindPlayerToParent(tPlayer, time, facing, offset);

	return 0;
}

static DreamPlayerBindPositionType handleBindToTargetPositionType(DreamMugenAssignment** tPosition, DreamPlayer* tPlayer) {
	string text;
	evaluateDreamAssignmentAndReturnAsString(text, tPosition, tPlayer);

	char val1[20], val2[20], comma1[10], comma2[10], postype[30];
	int items = sscanf(text.data(), "%s %s %s %s %s", val1, comma1, val2, comma2, postype);

	if (items < 5) {
		return PLAYER_BIND_POSITION_TYPE_AXIS;
	}
	else {
		assert(!strcmp(comma1, ","));
		assert(!strcmp(comma2, ","));
		turnStringLowercase(postype);

		if (!strcmp("foot", postype)) {
			return PLAYER_BIND_POSITION_TYPE_AXIS;
		} else if (!strcmp("mid", postype)) {
			return PLAYER_BIND_POSITION_TYPE_MID;
		} else if (!strcmp("head", postype)) {
			return PLAYER_BIND_POSITION_TYPE_HEAD;
		}
		else {
			logWarningFormat("Unrecognized postype: %s. Defaulting to PLAYER_BIND_POSITION_TYPE_AXIS.", postype);
			return PLAYER_BIND_POSITION_TYPE_AXIS;
		}
	}

}

static int handleBindToTargetController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	BindController* e = (BindController*)tController->mData;

	int time, id;
	Vector3D offset = makePosition(0, 0, 0);
	getSingleIntegerValueOrDefault(&e->mTime, tPlayer, &time, 1);
	getSingleIntegerValueOrDefault(&e->mID, tPlayer, &id, -1);
	getTwoFloatValuesWithDefaultValues(&e->mPosition, tPlayer, &offset.x, &offset.y, 0, 0);
	DreamPlayerBindPositionType bindType = handleBindToTargetPositionType(&e->mPosition, tPlayer);

	bindPlayerToTarget(tPlayer, time, offset, bindType, id);

	return 0;
}

static int handleTurnController(DreamPlayer* tPlayer) {
	turnPlayerAround(tPlayer);

	return 0;
}

static int handlePushPlayerController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	SingleRequiredValueController* e = (SingleRequiredValueController*)tController->mData;

	int isPushDisabled = evaluateDreamAssignment(&e->mValue, tPlayer);

	setPlayerPushDisabledFlag(tPlayer, isPushDisabled);

	return 0;
}

static int handleSettingVariableRange(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	VarRangeSetController* e = (VarRangeSetController*)tController->mData;

	if (e->mType == VAR_SET_TYPE_INTEGER) {
		int value, first, last;
		getSingleIntegerValueOrDefault(&e->mValue, tPlayer, &value, 0);
		getSingleIntegerValueOrDefault(&e->mFirst, tPlayer, &first, 0);
		getSingleIntegerValueOrDefault(&e->mLast, tPlayer, &last, 59);
		int i;
		for (i = first; i <= last; i++) {
			setPlayerVariable(tPlayer, i, value);
		}
	}
	else if (e->mType == VAR_SET_TYPE_FLOAT) {
		double value;
		int first, last;
		getSingleFloatValueOrDefault(&e->mValue, tPlayer, &value, 0);
		getSingleIntegerValueOrDefault(&e->mFirst, tPlayer, &first, 0);
		getSingleIntegerValueOrDefault(&e->mLast, tPlayer, &last, 39);
		int i;
		for (i = first; i <= last; i++) {
			setPlayerFloatVariable(tPlayer, i, value);
		}
	}
	else {
		logWarningFormat("Unrecognized var type %d. Ignoring.", e->mType);
	}

	return 0;
}

static int handleScreenBound(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	ScreenBoundController* e = (ScreenBoundController*)tController->mData;

	int val;
	int moveCameraX, moveCameraY;

	getSingleIntegerValueOrDefault(&e->mValue, tPlayer, &val, 0);
	getTwoIntegerValuesWithDefaultValues(&e->mMoveCameraFlags, tPlayer, &moveCameraX, &moveCameraY, 0, 0);

	setPlayerScreenBoundForTick(tPlayer, val, moveCameraX, moveCameraY);

	return 0;
}

static int handleMoveHitReset(DreamPlayer* tPlayer) {
	setPlayerMoveHitReset(tPlayer);

	return 0;
}

static int handleGravity(DreamPlayer* tPlayer) {
	double accel = getPlayerVerticalAcceleration(tPlayer);
	addPlayerVelocityY(tPlayer, accel, getPlayerCoordinateP(tPlayer));

	return 0;
}

static int handleSettingAttackDistance(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	SingleRequiredValueController* e = (SingleRequiredValueController*)tController->mData;

	int value = evaluateDreamAssignmentAndReturnAsInteger(&e->mValue, tPlayer);
	setHitDataGuardDistance(tPlayer, value);

	return 0;
}

static int handleTargetBindController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	BindController* e = (BindController*)tController->mData;

	int time, id;
	Vector3D offset = makePosition(0, 0, 0);
	getSingleIntegerValueOrDefault(&e->mTime, tPlayer, &time, 1);
	getSingleIntegerValueOrDefault(&e->mID, tPlayer, &id, -1);
	getTwoFloatValuesWithDefaultValues(&e->mPosition, tPlayer, &offset.x, &offset.y, 0, 0);

	bindPlayerTargetToPlayer(tPlayer, time, offset, id);

	return 0;
}

static int handleSetTargetFacing(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	SetTargetFacingController* e = (SetTargetFacingController*)tController->mData;

	int value = evaluateDreamAssignmentAndReturnAsInteger(&e->mValue, tPlayer);
	int id;
	getSingleIntegerValueOrDefault(&e->mID, tPlayer, &id, -1);

	setPlayerTargetFacing(tPlayer, id, value);

	return 0;
}

static int handleReversalDefinition(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	ReversalDefinitionController* e = (ReversalDefinitionController*)tController->mData;

	handleReversalDefinitionEntry(e->mAttributes, tPlayer);

	return 0;
}

static Position getFinalProjectilePositionFromPositionType(DreamExplodPositionType tPositionType, Position mOffset, DreamPlayer* tPlayer) {
	Position p1 = getPlayerPosition(tPlayer, getPlayerCoordinateP(tPlayer));


	if (tPositionType == EXPLOD_POSITION_TYPE_RELATIVE_TO_P1) {
		DreamPlayer* target = tPlayer;
		int isReversed = !getPlayerIsFacingRight(target);
		if (isReversed) mOffset.x *= -1;
		return vecAdd(p1, mOffset);
	}
	else if (tPositionType == EXPLOD_POSITION_TYPE_RELATIVE_TO_P2) {
		DreamPlayer* target = getPlayerOtherPlayer(tPlayer);
		Position p2 = getPlayerPosition(target, getPlayerCoordinateP(tPlayer));
		int isReversed = !getPlayerIsFacingRight(target);
		if (isReversed) mOffset.x *= -1;
		return vecAdd(p2, mOffset);
	}
	else if (tPositionType == EXPLOD_POSITION_TYPE_RELATIVE_TO_FRONT) {
		DreamPlayer* target = tPlayer;
		Position p = makePosition(getPlayerScreenEdgeInFrontX(target), p1.y, 0);
		int isReversed = getPlayerIsFacingRight(target);
		if (isReversed) mOffset.x *= -1;
		return vecAdd(p, mOffset);
	}
	else if (tPositionType == EXPLOD_POSITION_TYPE_RELATIVE_TO_BACK) {
		DreamPlayer* target = tPlayer;
		Position p = makePosition(getPlayerScreenEdgeInBackX(target), p1.y, 0);
		int isReversed = !getPlayerIsFacingRight(target);
		if (isReversed) mOffset.x *= -1;
		return vecAdd(p, mOffset);
	}
	else if (tPositionType == EXPLOD_POSITION_TYPE_RELATIVE_TO_LEFT) {
		if (getPlayerIsFacingRight(tPlayer)) {
			Position p = makePosition(getDreamStageLeftOfScreenBasedOnPlayer(getPlayerCoordinateP(tPlayer)), p1.y, 0);
			return vecAdd(p, mOffset);
		}
		else {
			Position p = makePosition(getDreamStageRightOfScreenBasedOnPlayer(getPlayerCoordinateP(tPlayer)), p1.y, 0);
			p.x -= mOffset.x;
			p.y += mOffset.y;
			return p;
		}
	}
	else if (tPositionType == EXPLOD_POSITION_TYPE_RELATIVE_TO_RIGHT) {
		if (getPlayerIsFacingRight(tPlayer)) {
			Position p = makePosition(getDreamStageRightOfScreenBasedOnPlayer(getPlayerCoordinateP(tPlayer)), p1.y, 0);
			return vecAdd(p, mOffset);
		}
		else {
			Position p = makePosition(getDreamStageLeftOfScreenBasedOnPlayer(getPlayerCoordinateP(tPlayer)), p1.y, 0);
			p.x -= mOffset.x;
			p.y += mOffset.y;
			return p;
		}
	}
	else if (tPositionType == EXPLOD_POSITION_TYPE_NONE) {
		Position p = makePosition(getDreamGameWidth(getPlayerCoordinateP(tPlayer)) / 2, p1.y, 0);
		return vecAdd(p, mOffset);
	}
	else {
		logWarningFormat("Unrecognized position type %d. Defaulting to EXPLOD_POSITION_TYPE_RELATIVE_TO_P1.", tPositionType);
		DreamPlayer* target = tPlayer;
		int isReversed = !getPlayerIsFacingRight(target);
		if (isReversed) mOffset.x *= -1;
		return vecAdd(p1, mOffset);
	}

}

static int handleProjectile(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	ProjectileController* e = (ProjectileController*)tController->mData;

	DreamPlayer* p = createNewProjectileFromPlayer(tPlayer);

	handleHelperOneIntegerElement(&e->mID, tPlayer, p, setProjectileID, -1);
	handleHelperOneIntegerElement(&e->mAnimation, tPlayer, p, setProjectileAnimation, 0);
	handleHelperOneIntegerElement(&e->mHitAnimation, tPlayer, p, setProjectileHitAnimation, -1);
	handleHelperOneIntegerElement(&e->mRemoveAnimation, tPlayer, p, setProjectileRemoveAnimation, getProjectileHitAnimation(p));
	handleHelperOneIntegerElement(&e->mCancelAnimation, tPlayer, p, setProjectileCancelAnimation, getProjectileRemoveAnimation(p));
	handleHelperTwoFloatElements(&e->mScale, tPlayer, p, setProjectileScale, 1, 1);
	handleHelperOneIntegerElement(&e->mIsRemovingProjectileAfterHit, tPlayer, p, setProjectileRemoveAfterHit, 1);
	handleHelperOneIntegerElement(&e->mRemoveTime, tPlayer, p, setProjectileRemoveTime, -1);
	handleHelperTwoFloatElements(&e->mVelocity, tPlayer, p, setProjectileVelocity, 0, 0);
	handleHelperTwoFloatElements(&e->mRemoveVelocity, tPlayer, p, setProjectileRemoveVelocity, 0, 0);
	handleHelperTwoFloatElements(&e->mAcceleration, tPlayer, p, setProjectileAcceleration, 0, 0);
	handleHelperTwoFloatElements(&e->mVelocityMultipliers, tPlayer, p, setProjectileVelocityMultipliers, 1, 1);


	handleHelperOneIntegerElement(&e->mHitAmountBeforeVanishing, tPlayer, p, setProjectileHitAmountBeforeVanishing, 1);
	handleHelperOneIntegerElement(&e->mMissTime, tPlayer, p, setProjectilMisstime, 0);
	handleHelperOneIntegerElement(&e->mPriority, tPlayer, p, setProjectilePriority, 1);
	handleHelperOneIntegerElement(&e->mSpriteSpriority, tPlayer, p, setProjectileSpritePriority, 3);

	handleHelperOneIntegerElement(&e->mEdgeBound, tPlayer, p, setProjectileEdgeBound, (int)transformDreamCoordinates(40, 240, getPlayerCoordinateP(p)));
	handleHelperOneIntegerElement(&e->mStageBound, tPlayer, p, setProjectileStageBound, (int)transformDreamCoordinates(40, 240, getPlayerCoordinateP(p)));
	handleHelperTwoIntegerElements(&e->mHeightBoundValues, tPlayer, p, setProjectileHeightBoundValues, (int)transformDreamCoordinates(-240, 240, getPlayerCoordinateP(p)), (int)transformDreamCoordinates(1, 240, getPlayerCoordinateP(p)));

	Position offset;
	getTwoFloatValuesWithDefaultValues(&e->mOffset, p, &offset.x, &offset.y, 0, 0);
	offset.z = 0;
	int positionType;
	getSingleIntegerValueOrDefault(&e->mPositionType, p, &positionType, EXPLOD_POSITION_TYPE_RELATIVE_TO_P1);
	Position pos = getFinalProjectilePositionFromPositionType((DreamExplodPositionType)positionType, offset, tPlayer);
	setProjectilePosition(p, pos);

	handleHelperOneIntegerElement(&e->mShadow, tPlayer, p, setProjectileShadow, 0);
	handleHelperOneIntegerElement(&e->mSuperMoveTime, tPlayer, p, setProjectileSuperMoveTime, 0);
	handleHelperOneIntegerElement(&e->mPauseMoveTime, tPlayer, p, setProjectilePauseMoveTime, 0);

	handleHelperOneIntegerElement(&e->mHasOwnPalette, tPlayer, p, setProjectileHasOwnPalette, 0);
	handleHelperTwoIntegerElements(&e->mRemapPalette, tPlayer, p, setProjectileRemapPalette, -1, 0);
	handleHelperOneIntegerElement(&e->mAfterImageTime, tPlayer, p, setProjectileAfterImageTime, 0);
	handleHelperOneIntegerElement(&e->mAfterImageLength, tPlayer, p, setProjectileAfterImageLength, 0);
	handleHelperOneIntegerElement(&e->mAfterImage, tPlayer, p, setProjectileAfterImage, 0);

	handleHitDefinitionWithController(&e->mHitDef, p); // TODO: parse from original player (https://dev.azure.com/captdc/DogmaRnDA/_workitems/edit/299)

	return 0;
}

int handleDreamMugenStateControllerAndReturnWhetherStateChanged(DreamMugenStateController * tController, DreamPlayer* tPlayer)
{

	if (!stl_map_contains(gMugenStateControllerVariableHandler.mStateControllerHandlers, (int)tController->mType)) {
		logWarningFormat("Unrecognized state controller %d. Ignoring.", tController->mType);
		return 0;
	}

	StateControllerHandleFunction func = gMugenStateControllerVariableHandler.mStateControllerHandlers[tController->mType];
	return func(tController, tPlayer);
}


static int handleAfterImage(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	(void)tController;
	(void)tPlayer;
	// TODO (https://dev.azure.com/captdc/DogmaRnDA/_workitems/edit/207)

	return 0;
}

static int handleAfterImageTime(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	(void)tController;
	(void)tPlayer;
	// TODO (https://dev.azure.com/captdc/DogmaRnDA/_workitems/edit/207)

	return 0;
}

static int handleAppendToClipboardController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	ClipboardController* e = (ClipboardController*)tController->mData;
	string formatString, parameterString;
	if(e->mText) evaluateDreamAssignmentAndReturnAsString(formatString, &e->mText, tPlayer);
	if (e->mParams) evaluateDreamAssignmentAndReturnAsString(parameterString, &e->mParams, tPlayer);

	addClipboardLineFormatString(formatString.data(), parameterString.data());

	return 0;
}

static int handleAllPalFXController(DreamMugenStateController* /*tController*/, DreamPlayer* /*tPlayer*/) {
	// TODO: Palettes (https://dev.azure.com/captdc/DogmaRnDA/_workitems/edit/208)
	return 0;
}

static int handlePalFXController(DreamMugenStateController* /*tController*/, DreamPlayer* /*tPlayer*/) {
	// TODO: Palettes (https://dev.azure.com/captdc/DogmaRnDA/_workitems/edit/208)
	return 0;
}

static int handleBGPalFXController(DreamMugenStateController* /*tController*/, DreamPlayer* /*tPlayer*/) {
	// TODO: Palettes (https://dev.azure.com/captdc/DogmaRnDA/_workitems/edit/208)
	return 0;
}

static int handleClearClipboardController() {
	clearClipboard();
	return 0;
}

static int handleDisplayToClipboardController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	clearClipboard();
	handleAppendToClipboardController(tController, tPlayer);
	
	return 0;
}

static int handleEnvironmentColorController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	EnvironmentColorController* e = (EnvironmentColorController*)tController->mData;

	int time;
	int isUnderCharacters;
	Vector3DI colors = evaluateDreamAssignmentAndReturnAsVector3DI(&e->mValue, tPlayer);
	getSingleIntegerValueOrDefault(&e->mTime, tPlayer, &time, 1);
	getSingleIntegerValueOrDefault(&e->mUnder, tPlayer, &isUnderCharacters, 1);
	
	setEnvironmentColor(colors, time, isUnderCharacters);

	return 0;
}

static int handleEnvironmentShakeController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	EnvironmentShakeController* e = (EnvironmentShakeController*)tController->mData;

	int time, ampl;
	double freq, phase;

	getSingleIntegerValueOrDefault(&e->mTime, tPlayer, &time, 1);
	getSingleFloatValueOrDefault(&e->mFrequency, tPlayer, &freq, 60);
	getSingleIntegerValueOrDefault(&e->mAmplitude, tPlayer, &ampl, (int)transformDreamCoordinates(-4.0, 240, getDreamStageCoordinateP()));
	getSingleFloatValueOrDefault(&e->mPhaseOffset, tPlayer, &phase, freq >= 90.0 ? 90.0 : 0.0);

	setEnvironmentShake(time, freq, ampl, phase);

	return 0;
}

static int handleExplodBindTimeController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	ExplodBindTimeController* e = (ExplodBindTimeController*)tController->mData;
	
	int id, time;

	getSingleIntegerValueOrDefault(&e->mID, tPlayer, &id, -1);
	getSingleIntegerValueOrDefault(&e->mTime, tPlayer, &time, 1);

	setExplodBindTimeForID(tPlayer, id, time);

	return 0;
}

static int handleForceFeedbackController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	ForceFeedbackController* e = (ForceFeedbackController*)tController->mData;

	int time;
	getSingleIntegerValueOrDefault(&e->mTime, tPlayer, &time, 1);
	int freq1, freq2;
	getTwoIntegerValuesWithDefaultValues(&e->mFrequency, tPlayer, &freq1, &freq2, 128, 0);
	int ampl1, ampl2;
	getTwoIntegerValuesWithDefaultValues(&e->mAmplitude, tPlayer, &ampl1, &ampl2, 128, 0);
	int self;
	getSingleIntegerValueOrDefault(&e->mSelf, tPlayer, &self, 1);

	int i = self ? tPlayer->mRootID : getPlayerOtherPlayer(tPlayer)->mRootID;
	addControllerRumbleSingle(i, time, freq1, ampl1 / 255.0);

	return 0;
}

static int handleGameMakeAnimController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	GameMakeAnimController* e = (GameMakeAnimController*)tController->mData;
	int animationNumber;
	getSingleIntegerValueOrDefault(&e->mValue, tPlayer, &animationNumber, 0);

	int isUnderPlayer;
	getSingleIntegerValueOrDefault(&e->mIsUnderPlayer, tPlayer, &isUnderPlayer, 0);

	Position pos = makePosition(0, 0, 0);
	getTwoFloatValuesWithDefaultValues(&e->mPosOffset, tPlayer, &pos.x, &pos.y, 0, 0);

	int random;
	getSingleIntegerValueOrDefault(&e->mRandomOffset, tPlayer, &random, 0);

	pos = vecAdd(pos, makePosition(randfrom(-random / 2.0, random / 2.0), randfrom(-random / 2.0, random / 2.0), 0));
	pos = vecAdd(pos, getPlayerPosition(tPlayer, getPlayerCoordinateP(tPlayer)));
	pos = vecAdd(pos, getDreamStageCoordinateSystemOffset(getPlayerCoordinateP(tPlayer)));

	if (isUnderPlayer) pos.z = GAME_MAKE_ANIM_UNDER_Z;
	else pos.z = GAME_MAKE_ANIM_OVER_Z;

	auto element = addMugenAnimation(getDreamFightEffectAnimation(animationNumber), getDreamFightEffectSprites(), pos);
	setMugenAnimationCameraPositionReference(element, getDreamMugenStageHandlerCameraPositionReference());
	setMugenAnimationNoLoop(element);

	return 0;
}

static int handleHitAddController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	SingleRequiredValueController* e = (SingleRequiredValueController*)tController->mData;

	int value;
	getSingleIntegerValueOrDefault(&e->mValue, tPlayer, &value, 0);
	increasePlayerComboCounter(tPlayer, value);

	return 0;
}

static int handleHitOverrideController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	HitOverrideController* e = (HitOverrideController*)tController->mData;
	string attr;
	evaluateDreamAssignmentAndReturnAsString(attr, &e->mAttributeString, tPlayer);

	DreamMugenStateType stateType;
	MugenAttackClass attackClass;
	MugenAttackType attackType;
	getHitDefinitionAttributeValuesFromString(attr.data(), &stateType, &attackClass, &attackType);

	int stateno, slot, time, forceAir;
	getSingleIntegerValueOrDefault(&e->mStateNo, tPlayer, &stateno, 0);
	getSingleIntegerValueOrDefault(&e->mSlot, tPlayer, &slot, 0);
	getSingleIntegerValueOrDefault(&e->mTime, tPlayer, &time, 1);
	getSingleIntegerValueOrDefault(&e->mForceAir, tPlayer, &forceAir, 0);

	setPlayerHitOverride(tPlayer, stateType, attackClass, attackType, stateno, slot, time, forceAir);

	return 0;
}

static int handleSetLifeController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	SingleRequiredValueController* e = (SingleRequiredValueController*)tController->mData;

	int value;
	getSingleIntegerValueOrDefault(&e->mValue, tPlayer, &value, 0);

	setPlayerLife(tPlayer, tPlayer, value);

	return 0;
}

static int handleDrawOffsetController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	Set2DPhysicsController* e = (Set2DPhysicsController*)tController->mData;

	if (e->mIsSettingX) {
		double x = evaluateDreamAssignmentAndReturnAsFloat(&e->x, tPlayer);
		setPlayerDrawOffsetX(tPlayer, x, getPlayerCoordinateP(tPlayer));
	}

	if (e->mIsSettingY) {
		double y = evaluateDreamAssignmentAndReturnAsFloat(&e->y, tPlayer);
		setPlayerDrawOffsetY(tPlayer, y, getPlayerCoordinateP(tPlayer));
	}

	return 0;
}

static int handleRemapPaletteController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	(void)tController;
	(void)tPlayer;
	// TODO: palettes (https://dev.azure.com/captdc/DogmaRnDA/_workitems/edit/208)

	return 0;
}

static int handleSoundPanController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	(void)tController;
	(void)tPlayer;
	// TODO: more advanced sound stuff (https://dev.azure.com/captdc/DogmaRnDA/_workitems/edit/307)

	return 0;
}

static int handleStopSoundController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	(void)tController;
	(void)tPlayer;
	// TODO: more advanced sound stuff (https://dev.azure.com/captdc/DogmaRnDA/_workitems/edit/307)

	return 0;
}

static int handleTargetDropController(DreamMugenStateController* /*tController*/, DreamPlayer* /*tPlayer*/) {
	// only makes sense in team matches, which aren't in dolmexica
	// TODO: reevaluate after proper targeting (https://dev.azure.com/captdc/DogmaRnDA/_workitems/edit/376)
	return 0;
}

static int handleVictoryQuoteController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	(void)tController;
	(void)tPlayer;
	// TODO: victory quotes (https://dev.azure.com/captdc/DogmaRnDA/_workitems/edit/309)

	return 0;
}


static BlendType handleTransparencyType(DreamMugenAssignment** tType, DreamPlayer* tPlayer, int* tAlphaDefaultSrc, int* tAlphaDefaultDst) {
	string text;
	evaluateDreamAssignmentAndReturnAsString(text, tType, tPlayer);

	BlendType ret;
	if (("default" == text) || "none" == text) {
		ret = BLEND_TYPE_NORMAL;
		*tAlphaDefaultSrc = 256;
		*tAlphaDefaultDst = 256;
	}
	else if ("add" == text) {
		ret = BLEND_TYPE_ADDITION;
		*tAlphaDefaultSrc = 256;
		*tAlphaDefaultDst = 256;
	}
	else if ("addalpha" == text) {
		ret = BLEND_TYPE_ADDITION;
		*tAlphaDefaultSrc = 256;
		*tAlphaDefaultDst = 0;
	}
	else if ("add1" == text) {
		ret = BLEND_TYPE_ADDITION;
		*tAlphaDefaultSrc = 256;
		*tAlphaDefaultDst = 128;
	}
	else  if ("sub" == text) {
		ret = BLEND_TYPE_SUBTRACTION;
		*tAlphaDefaultSrc = 256;
		*tAlphaDefaultDst = 256;
	}
	else {
		logWarningFormat("Unrecognized transparency format: %s. Default to normal blending.", text);
		ret = BLEND_TYPE_NORMAL;
		*tAlphaDefaultSrc = 256;
		*tAlphaDefaultDst = 256;
	}

	return ret;
}

static int handleTransparencyController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	TransparencyController* e = (TransparencyController*)tController->mData;
	
	int alphaDefaultSrc, alphaDefaultDst;
	BlendType type = handleTransparencyType(&e->mTransparency, tPlayer, &alphaDefaultSrc, &alphaDefaultDst);

	int alphaSource, alphaDest;
	getTwoIntegerValuesWithDefaultValues(&e->mAlpha, tPlayer, &alphaSource, &alphaDest, alphaDefaultSrc, alphaDefaultDst);

	setPlayerOneFrameTransparency(tPlayer, type, alphaSource, alphaDest);

	return 0;
}

static int handleRandomVariableController(DreamMugenStateController* tController, DreamPlayer* tPlayer) {
	VarRandomController* e = (VarRandomController*)tController->mData;
	
	int index;
	getSingleIntegerValueOrDefault(&e->mValue, tPlayer, &index, 0);

	string rangeText;
	evaluateDreamAssignmentAndReturnAsString(rangeText, &e->mRange, tPlayer);
	char comma[10];
	int val1, val2;
	int items = sscanf(rangeText.data(), "%d %s %d", &val1, comma, &val2);

	int value;
	if (items == 3) {
		value = randfromInteger(val1, val2);
	}
	else {
		value = randfromInteger(0, val1);
	}

	setPlayerVariable(tPlayer, index, value);

	return 0;
}

int afterImageHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAfterImage(tController, tPlayer); }
int afterImageTimeHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAfterImageTime(tController, tPlayer); }
int allPalFXHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAllPalFXController(tController, tPlayer); }
int angleAddHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAngleAddController(tController, tPlayer); }
int angleDrawHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAngleDrawController(tController, tPlayer); }
int angleMulHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAngleMulController(tController, tPlayer); }
int angleSetHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAngleSetController(tController, tPlayer); }
int appendToClipboardHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAppendToClipboardController(tController, tPlayer); }
int assertSpecialHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleSpecialAssert(tController, tPlayer); }
int attackDistHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleSettingAttackDistance(tController, tPlayer); }
int attackMulSetHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAttackMultiplierSetting(tController, tPlayer); }
int bgPalFXHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleBGPalFXController(tController, tPlayer); }
int bindToParentHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleBindToParentController(tController, tPlayer); }
int bindToRootHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleBindToRootController(tController, tPlayer); }
int bindToTargetHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleBindToTargetController(tController, tPlayer);}
int changeAnimHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAnimationChange(tController, tPlayer); }
int changeAnim2HandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAnimationChange2(tController, tPlayer); }
int changeStateHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleStateChange(tController, tPlayer); }
int clearClipboardHandleFunction(DreamMugenStateController* /*tController*/, DreamPlayer* /*tPlayer*/) { return handleClearClipboardController(); }
int ctrlSetHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleControlSetting(tController, tPlayer); }
int defenceMulSetHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleDefenseMultiplier(tController, tPlayer); }
int destroySelfHandleFunction(DreamMugenStateController* /*tController*/, DreamPlayer* tPlayer) { return handleDestroySelf(tPlayer); }
int displayToClipboardHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleDisplayToClipboardController(tController, tPlayer); }
int envColorHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleEnvironmentColorController(tController, tPlayer); }
int envShakeHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleEnvironmentShakeController(tController, tPlayer); }
int explodHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleExplod(tController, tPlayer); }
int explodBindTimeHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleExplodBindTimeController(tController, tPlayer); }
int forceFeedbackHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleForceFeedbackController(tController, tPlayer); }
int fallEnvShakeHandleFunction(DreamMugenStateController* /*tController*/, DreamPlayer* tPlayer) { return handleFallEnvironmentShake(tPlayer); }
int gameMakeAnimHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleGameMakeAnimController(tController, tPlayer); }
int gravityHandleFunction(DreamMugenStateController* /*tController*/, DreamPlayer* tPlayer) { return handleGravity(tPlayer); }
int helperHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleHelper(tController, tPlayer); }
int hitAddHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleHitAddController(tController, tPlayer); }
int hitByHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleHitBy(tController, tPlayer); }
int hitDefHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleHitDefinition(tController, tPlayer); }
int hitFallDamageHandleFunction(DreamMugenStateController* /*tController*/, DreamPlayer* tPlayer) { return handleHitFallDamage(tPlayer); }
int hitFallSetHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleHitFallSet(tController, tPlayer); }
int hitFallVelHandleFunction(DreamMugenStateController* /*tController*/, DreamPlayer* tPlayer) { return handleHitFallVelocity(tPlayer); }
int hitOverrideHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleHitOverrideController(tController, tPlayer); }
int hitVelSetHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleHitVelocitySetting(tController, tPlayer); }
int lifeAddHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAddingLife(tController, tPlayer); }
int lifeSetHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleSetLifeController(tController, tPlayer); }
int makeDustHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleMakeDust(tController, tPlayer); }
int modifyExplodHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return modifyExplod(tController, tPlayer); }
int moveHitResetHandleFunction(DreamMugenStateController* /*tController*/, DreamPlayer* tPlayer) { return handleMoveHitReset(tPlayer); }
int notHitByHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleNotHitBy(tController, tPlayer); }
int nullHandleFunction(DreamMugenStateController* /*tController*/, DreamPlayer* /*tPlayer*/) { return handleNull(); }
int offsetHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleDrawOffsetController(tController, tPlayer); }
int palFXHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handlePalFXController(tController, tPlayer); }
int parentVarAddHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAddingVariable(tController, tPlayer, getPlayerParent(tPlayer)); }
int parentVarSetHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleSettingVariable(tController, tPlayer, getPlayerParent(tPlayer)); }
int pauseHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handlePauseController(tController, tPlayer); }
int playerPushHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handlePushPlayerController(tController, tPlayer); }
int playSndHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handlePlaySound(tController, tPlayer); }
int posAddHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handlePositionAdding(tController, tPlayer); }
int posFreezeHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handlePositionFreeze(tController, tPlayer); }
int posSetHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handlePositionSetting(tController, tPlayer); }
int powerAddHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handlePowerAddition(tController, tPlayer); }
int powerSetHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handlePowerSettingController(tController, tPlayer); }
int projectileHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleProjectile(tController, tPlayer); }
int remapPalHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleRemapPaletteController(tController, tPlayer); }
int removeExplodHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleRemovingExplod(tController, tPlayer); }
int reversalDefHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleReversalDefinition(tController, tPlayer); }
int screenBoundHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleScreenBound(tController, tPlayer); }
int selfStateHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleSelfStateChange(tController, tPlayer); }
int sprPriorityHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleSpritePriority(tController, tPlayer); }
int stateTypeSetHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleStateTypeSet(tController, tPlayer); }
int sndPanHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleSoundPanController(tController, tPlayer); }
int stopSndHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleStopSoundController(tController, tPlayer); }
int superPauseHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleSuperPause(tController, tPlayer); }
int targetBindHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleTargetBindController(tController, tPlayer); }
int targetDropHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleTargetDropController(tController, tPlayer); }
int targetFacingHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleSetTargetFacing(tController, tPlayer); }
int targetLifeAddHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAddingTargetLife(tController, tPlayer); }
int targetPowerAddHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAddingTargetPower(tController, tPlayer); }
int targetStateHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleTargetStateChange(tController, tPlayer); }
int targetVelAddHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleTargetVelocityAddController(tController, tPlayer); }
int targetVelSetHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleTargetVelocitySetController(tController, tPlayer); }
int transHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleTransparencyController(tController, tPlayer); }
int turnHandleFunction(DreamMugenStateController* /*tController*/, DreamPlayer* tPlayer) { return handleTurnController(tPlayer); }
int varAddHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAddingVariable(tController, tPlayer, tPlayer); }
int varRandomHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleRandomVariableController(tController, tPlayer); }
int varRangeSetHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleSettingVariableRange(tController, tPlayer); }
int varSetHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleSettingVariable(tController, tPlayer, tPlayer); }
int globalVarSetHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleSettingGlobalVariable(tController, tPlayer); }
int globalVarAddHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAddingGlobalVariable(tController, tPlayer); }
int velAddHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleVelocityAddition(tController, tPlayer); }
int velMulHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleVelocityMultiplication(tController, tPlayer); }
int velSetHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleVelocitySetting(tController, tPlayer); }
int victoryQuoteHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleVictoryQuoteController(tController, tPlayer); }
int widthHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleWidth(tController, tPlayer); }


static void setupStateControllerHandlers() {
	gMugenStateControllerVariableHandler.mStateControllerHandlers.clear();

	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_AFTER_IMAGE] = afterImageHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_AFTER_IMAGE_TIME] = afterImageTimeHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_PALETTE_EFFECT_ALL] = allPalFXHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_ADD_ANGLE] = angleAddHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_DRAW_ANGLE] = angleDrawHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_MUL_ANGLE] = angleMulHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_SET_ANGLE] = angleSetHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_APPEND_TO_CLIPBOARD] = appendToClipboardHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_ASSERT_SPECIAL] = assertSpecialHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_SET_ATTACK_DISTANCE] = attackDistHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_SET_ATTACK_MULTIPLIER] = attackMulSetHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_PALETTE_EFFECT_BACKGROUND] = bgPalFXHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_BIND_TO_PARENT] = bindToParentHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_BIND_TO_ROOT] = bindToRootHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_BIND_TO_TARGET] = bindToTargetHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_CHANGE_ANIMATION] = changeAnimHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_CHANGE_ANIMATION_2] = changeAnim2HandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_CHANGE_STATE] = changeStateHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_CLEAR_CLIPBOARD] = clearClipboardHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_SET_CONTROL] = ctrlSetHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_SET_DEFENSE_MULTIPLIER] = defenceMulSetHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_DESTROY_SELF] = destroySelfHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_DISPLAY_TO_CLIPBOARD] = displayToClipboardHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_ENVIRONMENT_COLOR] = envColorHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_ENVIRONMENT_SHAKE] = envShakeHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_EXPLOD] = explodHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_EXPLOD_BIND_TIME] = explodBindTimeHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_FORCE_FEEDBACK] = forceFeedbackHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_FALL_ENVIRONMENT_SHAKE] = fallEnvShakeHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_MAKE_GAME_ANIMATION] = gameMakeAnimHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_GRAVITY] = gravityHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_HELPER] = helperHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_ADD_HIT] = hitAddHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_HIT_BY] = hitByHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_HIT_DEFINITION] = hitDefHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_HIT_FALL_DAMAGE] = hitFallDamageHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_SET_HIT_FALL] = hitFallSetHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_HIT_FALL_VELOCITY] = hitFallVelHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_HIT_OVERRIDE] = hitOverrideHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_SET_HIT_VELOCITY] = hitVelSetHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_ADD_LIFE] = lifeAddHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_SET_LIFE] = lifeSetHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_MAKE_DUST] = makeDustHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_MODIFY_EXPLOD] = modifyExplodHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_RESET_MOVE_HIT] = moveHitResetHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_NOT_HIT_BY] = notHitByHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_NULL] = nullHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_SET_OFFSET] = offsetHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_PALETTE_EFFECT] = palFXHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_PARENT_ADD_VARIABLE] = parentVarAddHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_SET_PARENT_VARIABLE] = parentVarSetHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_PAUSE] = pauseHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_PLAYER_PUSH] = playerPushHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_PLAY_SOUND] = playSndHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_ADD_POSITION] = posAddHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_FREEZE_POSITION] = posFreezeHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_SET_POSITION] = posSetHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_ADD_POWER] = powerAddHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_SET_POWER] = powerSetHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_PROJECTILE] = projectileHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_REMAP_PALETTE] = remapPalHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_REMOVE_EXPLOD] = removeExplodHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_REVERSAL_DEFINITION] = reversalDefHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_SCREEN_BOUND] = screenBoundHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_SET_SELF_STATE] = selfStateHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_SPRITE_PRIORITY] = sprPriorityHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_SET_STATE_TYPE] = stateTypeSetHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_PAN_SOUND] = sndPanHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_STOP_SOUND] = stopSndHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_SUPER_PAUSE] = superPauseHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_BIND_TARGET] = targetBindHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_DROP_TARGET] = targetDropHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_SET_TARGET_FACING] = targetFacingHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_ADD_TARGET_LIFE] = targetLifeAddHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_ADD_TARGET_POWER] = targetPowerAddHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_SET_TARGET_STATE] = targetStateHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_TARGET_ADD_VELOCITY] = targetVelAddHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_TARGET_SET_VELOCITY] = targetVelSetHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_TRANSPARENCY] = transHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_TURN] = turnHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_ADD_VARIABLE] = varAddHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_SET_VARIABLE_RANDOM] = varRandomHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_SET_VARIABLE_RANGE] = varRangeSetHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_SET_VARIABLE] = varSetHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_GLOBAL_VAR_SET] = globalVarSetHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_GLOBAL_VAR_ADD] = globalVarAddHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_ADD_VELOCITY] = velAddHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_MULTIPLY_VELOCITY] = velMulHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_SET_VELOCITY] = velSetHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_VICTORY_QUOTE] = victoryQuoteHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STATE_CONTROLLER_TYPE_WIDTH] = widthHandleFunction;

}

void afterImageParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseAfterImageController(tController, tGroup);}
void afterImageTimeParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseAfterImageTimeController(tController, tGroup); }
void allPalFXParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parsePalFXController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_PALETTE_EFFECT_ALL); }
void angleAddParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseSingleRequiredValueController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_ADD_ANGLE); }
void angleDrawParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseAngleDrawController(tController, tGroup); }
void angleMulParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseSingleRequiredValueController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_MUL_ANGLE); }
void angleSetParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseSingleRequiredValueController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_SET_ANGLE); }
void appendToClipboardParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseClipboardController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_DISPLAY_TO_CLIPBOARD); }
void assertSpecialParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseSpecialAssertController(tController, tGroup); }
void attackDistParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseSingleRequiredValueController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_SET_ATTACK_DISTANCE); }
void attackMulSetParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseSingleRequiredValueController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_SET_ATTACK_MULTIPLIER); }
void bgPalFXParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parsePalFXController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_PALETTE_EFFECT_BACKGROUND); }
void bindToParentParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseBindController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_BIND_TO_PARENT); }
void bindToRootParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseBindController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_BIND_TO_ROOT); }
void bindToTargetParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseBindController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_BIND_TO_TARGET); }
void changeAnimParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseChangeAnimationController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_CHANGE_ANIMATION); }
void changeAnim2ParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseChangeAnimationController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_CHANGE_ANIMATION_2); }
void changeStateParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseChangeStateController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_CHANGE_STATE); }
void clearClipboardParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* /*tGroup*/) { parseNullController(tController, MUGEN_STATE_CONTROLLER_TYPE_CLEAR_CLIPBOARD); }
void ctrlSetParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseControlSettingController(tController, tGroup); }
void defenceMulSetParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseDefenseMultiplierController(tController, tGroup); }
void destroySelfParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseDestroySelfController(tController, tGroup); }
void displayToClipboardParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseClipboardController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_DISPLAY_TO_CLIPBOARD); }
void envColorParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseEnvColorController(tController, tGroup); }
void envShakeParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseEnvironmentShakeController(tController, tGroup); }
void explodParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseExplodController(tController, tGroup); }
void explodBindTimeParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseExplodBindTimeController(tController, tGroup); }
void forceFeedbackParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseForceFeedbackController(tController, tGroup); }
void fallEnvShakeParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* /*tGroup*/) { parseNullController(tController, MUGEN_STATE_CONTROLLER_TYPE_FALL_ENVIRONMENT_SHAKE); }
void gameMakeAnimParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseGameMakeAnimController(tController, tGroup); }
void gravityParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* /*tGroup*/) { parseNullController(tController, MUGEN_STATE_CONTROLLER_TYPE_GRAVITY); }
void helperParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseHelperController(tController, tGroup); }
void hitAddParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseSingleRequiredValueController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_ADD_HIT); }
void hitByParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseNotHitByController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_HIT_BY); }
void hitDefParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseHitDefinitionController(tController, tGroup); }
void hitFallDamageParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* /*tGroup*/) { parseNullController(tController, MUGEN_STATE_CONTROLLER_TYPE_HIT_FALL_DAMAGE); }
void hitFallSetParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseHitFallSetController(tController, tGroup); }
void hitFallVelParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* /*tGroup*/) { parseNullController(tController, MUGEN_STATE_CONTROLLER_TYPE_HIT_FALL_VELOCITY); }
void hitOverrideParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseHitOverrideController(tController, tGroup); }
void hitVelSetParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parse2DPhysicsController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_SET_HIT_VELOCITY); }
void lifeAddParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseLifeAddController(tController, tGroup); }
void lifeSetParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseSingleRequiredValueController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_SET_LIFE); }
void makeDustParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseMakeDustController(tController, tGroup); }
void modifyExplodParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseModifyExplodController(tController, tGroup); }
void moveHitResetParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* /*tGroup*/) { parseNullController(tController, MUGEN_STATE_CONTROLLER_TYPE_RESET_MOVE_HIT); }
void notHitByParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseNotHitByController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_NOT_HIT_BY); }
void nullParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* /*tGroup*/) { parseNullController(tController, MUGEN_STATE_CONTROLLER_TYPE_NULL); }
void offsetParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parse2DPhysicsController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_SET_OFFSET); }
void palFXParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parsePalFXController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_PALETTE_EFFECT); }
void parentVarAddParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseVarSetController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_PARENT_ADD_VARIABLE); }
void parentVarSetParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseVarSetController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_SET_PARENT_VARIABLE); }
void pauseParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parsePauseController(tController, tGroup); }
void playerPushParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseSingleRequiredValueController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_PLAYER_PUSH); }
void playSndParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parsePlaySoundController(tController, tGroup); }
void posAddParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parse2DPhysicsController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_ADD_POSITION); }
void posFreezeParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parsePositionFreezeController(tController, tGroup); }
void posSetParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parse2DPhysicsController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_SET_POSITION); }
void powerAddParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseSingleRequiredValueController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_ADD_POWER); }
void powerSetParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseSingleRequiredValueController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_SET_POWER); }
void projectileParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseProjectileController(tController, tGroup); }
void remapPalParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseRemapPaletteController(tController, tGroup); }
void removeExplodParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseRemoveExplodController(tController, tGroup); }
void reversalDefParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseReversalDefinitionController(tController, tGroup); }
void screenBoundParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseScreenBoundController(tController, tGroup); }
void selfStateParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseChangeStateController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_SET_SELF_STATE); }
void sprPriorityParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseSpritePriorityController(tController, tGroup); }
void stateTypeSetParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseStateTypeSetController(tController, tGroup); }
void sndPanParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseSoundPanController(tController, tGroup); }
void stopSndParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseStopSoundController(tController, tGroup); }
void superPauseParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseSuperPauseController(tController, tGroup); }
void targetBindParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseBindController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_BIND_TARGET); }
void targetDropParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseTargetDropController(tController, tGroup); }
void targetFacingParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseSetTargetFacingController(tController, tGroup); }
void targetLifeAddParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseTargetLifeAddController(tController, tGroup); }
void targetPowerAddParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseTargetPowerAddController(tController, tGroup); }
void targetStateParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseTargetChangeStateController(tController, tGroup); }
void targetVelAddParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseTarget2DPhysicsController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_TARGET_ADD_VELOCITY); }
void targetVelSetParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseTarget2DPhysicsController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_TARGET_SET_VELOCITY); }
void transParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseTransparencyController(tController, tGroup); }
void turnParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* /*tGroup*/) { parseNullController(tController, MUGEN_STATE_CONTROLLER_TYPE_TURN); }
void varAddParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseVarSetController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_ADD_VARIABLE); }
void varRandomParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseVarRandomController(tController, tGroup); }
void varRangeSetParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseVarRangeSetController(tController, tGroup); }
void varSetParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseVarSetController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_SET_VARIABLE); }
void globalVarSetParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseVarSetController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_GLOBAL_VAR_SET); }
void globalVarAddParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseVarSetController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_GLOBAL_VAR_ADD); }
void velAddParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parse2DPhysicsController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_ADD_VELOCITY); }
void velMulParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parse2DPhysicsController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_MULTIPLY_VELOCITY); }
void velSetParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parse2DPhysicsController(tController, tGroup, MUGEN_STATE_CONTROLLER_TYPE_SET_VELOCITY); }
void victoryQuoteParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseVictoryQuoteController(tController, tGroup); }
void widthParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseWidthController(tController, tGroup); }


static void setupStateControllerParsers() {
	gMugenStateControllerVariableHandler.mStateControllerParsers.clear();

	gMugenStateControllerVariableHandler.mStateControllerParsers["afterimage"] = afterImageParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["afterimagetime"] = afterImageTimeParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["allpalfx"] = allPalFXParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["angleadd"] = angleAddParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["angledraw"] = angleDrawParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["anglemul"] = angleMulParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["angleset"] = angleSetParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["appendtoclipboard"] = appendToClipboardParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["assertspecial"] = assertSpecialParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["attackdist"] = attackDistParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["attackmulset"] = attackMulSetParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["bgpalfx"] = bgPalFXParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["bindtoparent"] = bindToParentParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["bindtoroot"] = bindToRootParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["bindtotarget"] = bindToTargetParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["changeanim"] = changeAnimParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["changeanim2"] = changeAnim2ParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["changestate"] = changeStateParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["clearclipboard"] = clearClipboardParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["ctrlset"] = ctrlSetParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["defencemulset"] = defenceMulSetParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["destroyself"] = destroySelfParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["displaytoclipboard"] = displayToClipboardParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["envcolor"] = envColorParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["envshake"] = envShakeParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["explod"] = explodParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["explodbindtime"] = explodBindTimeParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["forcefeedback"] = forceFeedbackParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["fallenvshake"] = fallEnvShakeParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["gamemakeanim"] = gameMakeAnimParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["gravity"] = gravityParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["helper"] = helperParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["hitadd"] = hitAddParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["hitby"] = hitByParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["hitdef"] = hitDefParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["hitfalldamage"] = hitFallDamageParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["hitfallset"] = hitFallSetParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["hitfallvel"] = hitFallVelParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["hitoverride"] = hitOverrideParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["hitvelset"] = hitVelSetParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["lifeadd"] = lifeAddParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["lifeset"] = lifeSetParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["makedust"] = makeDustParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["modifyexplod"] = modifyExplodParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["movehitreset"] = moveHitResetParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["nothitby"] = notHitByParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["null"] = nullParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["offset"] = offsetParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["palfx"] = palFXParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["parentvaradd"] = parentVarAddParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["parentvarset"] = parentVarSetParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["pause"] = pauseParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["playerpush"] = playerPushParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["playsnd"] = playSndParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["posadd"] = posAddParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["posfreeze"] = posFreezeParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["posset"] = posSetParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["poweradd"] = powerAddParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["powerset"] = powerSetParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["projectile"] = projectileParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["remappal"] = remapPalParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["removeexplod"] = removeExplodParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["reversaldef"] = reversalDefParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["screenbound"] = screenBoundParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["selfstate"] = selfStateParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["sprpriority"] = sprPriorityParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["statetypeset"] = stateTypeSetParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["sndpan"] = sndPanParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["stopsnd"] = stopSndParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["superpause"] = superPauseParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["targetbind"] = targetBindParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["targetdrop"] = targetDropParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["targetfacing"] = targetFacingParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["targetlifeadd"] = targetLifeAddParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["targetpoweradd"] = targetPowerAddParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["targetstate"] = targetStateParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["targetveladd"] = targetVelAddParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["targetvelset"] = targetVelSetParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["trans"] = transParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["turn"] = turnParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["varadd"] = varAddParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["varrandom"] = varRandomParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["varrangeset"] = varRangeSetParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["varset"] = varSetParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["veladd"] = velAddParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["globalvarset"] = globalVarSetParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["globalvarset"] = globalVarAddParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["velmul"] = velMulParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["velset"] = velSetParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["victoryquote"] = victoryQuoteParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["width"] = widthParseFunction;
}

void afterImageUnloadFunction(DreamMugenStateController* tController) { unloadAfterImageController(tController); }
void afterImageTimeUnloadFunction(DreamMugenStateController* tController) { unloadAfterImageTimeController(tController); }
void allPalFXUnloadFunction(DreamMugenStateController* tController) { unloadPalFXController(tController); }
void angleAddUnloadFunction(DreamMugenStateController* tController) { unloadSingleRequiredValueController(tController); }
void angleDrawUnloadFunction(DreamMugenStateController* tController) { unloadAngleDrawController(tController); }
void angleMulUnloadFunction(DreamMugenStateController* tController) { unloadSingleRequiredValueController(tController); }
void angleSetUnloadFunction(DreamMugenStateController* tController) { unloadSingleRequiredValueController(tController); }
void appendToClipboardUnloadFunction(DreamMugenStateController* tController) { unloadClipboardController(tController); }
void assertSpecialUnloadFunction(DreamMugenStateController* tController) { unloadSpecialAssertController(tController); }
void attackDistUnloadFunction(DreamMugenStateController* tController) { unloadSingleRequiredValueController(tController); }
void attackMulSetUnloadFunction(DreamMugenStateController* tController) { unloadSingleRequiredValueController(tController); }
void bgPalFXUnloadFunction(DreamMugenStateController* tController) { unloadPalFXController(tController); }
void bindToParentUnloadFunction(DreamMugenStateController* tController) { unloadBindController(tController); }
void bindToRootUnloadFunction(DreamMugenStateController* tController) { unloadBindController(tController); }
void bindToTargetUnloadFunction(DreamMugenStateController* tController) { unloadBindController(tController); }
void changeAnimUnloadFunction(DreamMugenStateController* tController) { unloadChangeAnimationController(tController); }
void changeAnim2UnloadFunction(DreamMugenStateController* tController) { unloadChangeAnimationController(tController); }
void changeStateUnloadFunction(DreamMugenStateController* tController) { unloadChangeStateController(tController); }
void clearClipboardUnloadFunction(DreamMugenStateController* tController) { unloadNullController(tController); }
void ctrlSetUnloadFunction(DreamMugenStateController* tController) { unloadControlSettingController(tController); }
void defenceMulSetUnloadFunction(DreamMugenStateController* tController) { unloadDefenseMultiplierController(tController); }
void destroySelfUnloadFunction(DreamMugenStateController* tController) { unloadDestroySelfController(tController); }
void displayToClipboardUnloadFunction(DreamMugenStateController* tController) { unloadClipboardController(tController); }
void envColorUnloadFunction(DreamMugenStateController* tController) { unloadEnvColorController(tController); }
void envShakeUnloadFunction(DreamMugenStateController* tController) { unloadEnvironmentShakeController(tController); }
void explodUnloadFunction(DreamMugenStateController* tController) { unloadExplodController(tController); }
void explodBindTimeUnloadFunction(DreamMugenStateController* tController) { unloadExplodBindTimeController(tController); }
void forceFeedbackUnloadFunction(DreamMugenStateController* tController) { unloadForceFeedbackController(tController); }
void fallEnvShakeUnloadFunction(DreamMugenStateController* tController) { unloadNullController(tController); }
void gameMakeAnimUnloadFunction(DreamMugenStateController* tController) { unloadGameMakeAnimController(tController); }
void gravityUnloadFunction(DreamMugenStateController* tController) { unloadNullController(tController); }
void helperUnloadFunction(DreamMugenStateController* tController) { unloadHelperController(tController); }
void hitAddUnloadFunction(DreamMugenStateController* tController) { unloadSingleRequiredValueController(tController); }
void hitByUnloadFunction(DreamMugenStateController* tController) { unloadNotHitByController(tController); }
void hitDefUnloadFunction(DreamMugenStateController* tController) { unloadHitDefinitionController(tController); }
void hitFallDamageUnloadFunction(DreamMugenStateController* tController) { unloadNullController(tController); }
void hitFallSetUnloadFunction(DreamMugenStateController* tController) { unloadHitFallSetController(tController); }
void hitFallVelUnloadFunction(DreamMugenStateController* tController) { unloadNullController(tController); }
void hitOverrideUnloadFunction(DreamMugenStateController* tController) { unloadHitOverrideController(tController); }
void hitVelSetUnloadFunction(DreamMugenStateController* tController) { unload2DPhysicsController(tController); }
void lifeAddUnloadFunction(DreamMugenStateController* tController) { unloadLifeAddController(tController); }
void lifeSetUnloadFunction(DreamMugenStateController* tController) { unloadSingleRequiredValueController(tController); }
void makeDustUnloadFunction(DreamMugenStateController* tController) { unloadMakeDustController(tController); }
void modifyExplodUnloadFunction(DreamMugenStateController* tController) { unloadModifyExplodController(tController); }
void moveHitResetUnloadFunction(DreamMugenStateController* tController) { unloadNullController(tController); }
void notHitByUnloadFunction(DreamMugenStateController* tController) { unloadNotHitByController(tController); }
void nullUnloadFunction(DreamMugenStateController* tController) { unloadNullController(tController); }
void offsetUnloadFunction(DreamMugenStateController* tController) { unload2DPhysicsController(tController); }
void palFXUnloadFunction(DreamMugenStateController* tController) { unloadPalFXController(tController); }
void parentVarAddUnloadFunction(DreamMugenStateController* tController) { unloadVarSetController(tController); }
void parentVarSetUnloadFunction(DreamMugenStateController* tController) { unloadVarSetController(tController); }
void pauseUnloadFunction(DreamMugenStateController* tController) { unloadPauseController(tController); }
void playerPushUnloadFunction(DreamMugenStateController* tController) { unloadSingleRequiredValueController(tController); }
void playSndUnloadFunction(DreamMugenStateController* tController) { unloadPlaySoundController(tController); }
void posAddUnloadFunction(DreamMugenStateController* tController) { unload2DPhysicsController(tController); }
void posFreezeUnloadFunction(DreamMugenStateController* tController) { unloadPositionFreezeController(tController); }
void posSetUnloadFunction(DreamMugenStateController* tController) { unload2DPhysicsController(tController); }
void powerAddUnloadFunction(DreamMugenStateController* tController) { unloadSingleRequiredValueController(tController); }
void powerSetUnloadFunction(DreamMugenStateController* tController) { unloadSingleRequiredValueController(tController); }
void projectileUnloadFunction(DreamMugenStateController* tController) { unloadProjectileController(tController); }
void remapPalUnloadFunction(DreamMugenStateController* tController) { unloadRemapPaletteController(tController); }
void removeExplodUnloadFunction(DreamMugenStateController* tController) { unloadRemoveExplodController(tController); }
void reversalDefUnloadFunction(DreamMugenStateController* tController) { unloadReversalDefinitionController(tController); }
void screenBoundUnloadFunction(DreamMugenStateController* tController) { unloadScreenBoundController(tController); }
void selfStateUnloadFunction(DreamMugenStateController* tController) { unloadChangeStateController(tController); }
void sprPriorityUnloadFunction(DreamMugenStateController* tController) { unloadSpritePriorityController(tController); }
void stateTypeSetUnloadFunction(DreamMugenStateController* tController) { unloadStateTypeSetController(tController); }
void sndPanUnloadFunction(DreamMugenStateController* tController) { unloadSoundPanController(tController); }
void stopSndUnloadFunction(DreamMugenStateController* tController) { unloadStopSoundController(tController); }
void superPauseUnloadFunction(DreamMugenStateController* tController) { unloadSuperPauseController(tController); }
void targetBindUnloadFunction(DreamMugenStateController* tController) { unloadBindController(tController); }
void targetDropUnloadFunction(DreamMugenStateController* tController) { unloadTargetDropController(tController); }
void targetFacingUnloadFunction(DreamMugenStateController* tController) { unloadSetTargetFacingController(tController); }
void targetLifeAddUnloadFunction(DreamMugenStateController* tController) { unloadTargetLifeAddController(tController); }
void targetPowerAddUnloadFunction(DreamMugenStateController* tController) { unloadTargetPowerAddController(tController); }
void targetStateUnloadFunction(DreamMugenStateController* tController) { unloadTargetChangeStateController(tController); }
void targetVelAddUnloadFunction(DreamMugenStateController* tController) { unloadTarget2DPhysicsController(tController); }
void targetVelSetUnloadFunction(DreamMugenStateController* tController) { unloadTarget2DPhysicsController(tController); }
void transUnloadFunction(DreamMugenStateController* tController) { unloadTransparencyController(tController); }
void turnUnloadFunction(DreamMugenStateController* tController) { unloadNullController(tController); }
void varAddUnloadFunction(DreamMugenStateController* tController) { unloadVarSetController(tController); }
void varRandomUnloadFunction(DreamMugenStateController* tController) { unloadVarRandomController(tController); }
void varRangeSetUnloadFunction(DreamMugenStateController* tController) { unloadVarRangeSetController(tController); }
void varSetUnloadFunction(DreamMugenStateController* tController) { unloadVarSetController(tController); }
void velAddUnloadFunction(DreamMugenStateController* tController) { unload2DPhysicsController(tController); }
void velMulUnloadFunction(DreamMugenStateController* tController) { unload2DPhysicsController(tController); }
void velSetUnloadFunction(DreamMugenStateController* tController) { unload2DPhysicsController(tController); }
void victoryQuoteUnloadFunction(DreamMugenStateController* tController) { unloadVictoryQuoteController(tController); }
void widthUnloadFunction(DreamMugenStateController* tController) { unloadWidthController(tController); }

static void setupStateControllerUnloaders() {
	gMugenStateControllerVariableHandler.mStateControllerUnloaders.clear();

	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_AFTER_IMAGE] = afterImageUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_AFTER_IMAGE_TIME] = afterImageTimeUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_PALETTE_EFFECT_ALL] = allPalFXUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_ADD_ANGLE] = angleAddUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_DRAW_ANGLE] = angleDrawUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_MUL_ANGLE] = angleMulUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_SET_ANGLE] = angleSetUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_APPEND_TO_CLIPBOARD] = appendToClipboardUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_ASSERT_SPECIAL] = assertSpecialUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_SET_ATTACK_DISTANCE] = attackDistUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_SET_ATTACK_MULTIPLIER] = attackMulSetUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_PALETTE_EFFECT_BACKGROUND] = bgPalFXUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_BIND_TO_PARENT] = bindToParentUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_BIND_TO_ROOT] = bindToRootUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_BIND_TO_TARGET] = bindToTargetUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_CHANGE_ANIMATION] = changeAnimUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_CHANGE_ANIMATION_2] = changeAnim2UnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_CHANGE_STATE] = changeStateUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_CLEAR_CLIPBOARD] = clearClipboardUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_SET_CONTROL] = ctrlSetUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_SET_DEFENSE_MULTIPLIER] = defenceMulSetUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_DESTROY_SELF] = destroySelfUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_DISPLAY_TO_CLIPBOARD] = displayToClipboardUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_ENVIRONMENT_COLOR] = envColorUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_ENVIRONMENT_SHAKE] = envShakeUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_EXPLOD] = explodUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_EXPLOD_BIND_TIME] = explodBindTimeUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_FORCE_FEEDBACK] = forceFeedbackUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_FALL_ENVIRONMENT_SHAKE] = fallEnvShakeUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_MAKE_GAME_ANIMATION] = gameMakeAnimUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_GRAVITY] = gravityUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_HELPER] = helperUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_ADD_HIT] = hitAddUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_HIT_BY] = hitByUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_HIT_DEFINITION] = hitDefUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_HIT_FALL_DAMAGE] = hitFallDamageUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_SET_HIT_FALL] = hitFallSetUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_HIT_FALL_VELOCITY] = hitFallVelUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_HIT_OVERRIDE] = hitOverrideUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_SET_HIT_VELOCITY] = hitVelSetUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_ADD_LIFE] = lifeAddUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_SET_LIFE] = lifeSetUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_MAKE_DUST] = makeDustUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_MODIFY_EXPLOD] = modifyExplodUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_RESET_MOVE_HIT] = moveHitResetUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_NOT_HIT_BY] = notHitByUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_NULL] = nullUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_SET_OFFSET] = offsetUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_PALETTE_EFFECT] = palFXUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_PARENT_ADD_VARIABLE] = parentVarAddUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_SET_PARENT_VARIABLE] = parentVarSetUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_PAUSE] = pauseUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_PLAYER_PUSH] = playerPushUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_PLAY_SOUND] = playSndUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_ADD_POSITION] = posAddUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_FREEZE_POSITION] = posFreezeUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_SET_POSITION] = posSetUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_ADD_POWER] = powerAddUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_SET_POWER] = powerSetUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_PROJECTILE] = projectileUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_REMAP_PALETTE] = remapPalUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_REMOVE_EXPLOD] = removeExplodUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_REVERSAL_DEFINITION] = reversalDefUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_SCREEN_BOUND] = screenBoundUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_SET_SELF_STATE] = selfStateUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_SPRITE_PRIORITY] = sprPriorityUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_SET_STATE_TYPE] = stateTypeSetUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_PAN_SOUND] = sndPanUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_STOP_SOUND] = stopSndUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_SUPER_PAUSE] = superPauseUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_BIND_TARGET] = targetBindUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_DROP_TARGET] = targetDropUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_SET_TARGET_FACING] = targetFacingUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_ADD_TARGET_LIFE] = targetLifeAddUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_ADD_TARGET_POWER] = targetPowerAddUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_SET_TARGET_STATE] = targetStateUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_TARGET_ADD_VELOCITY] = targetVelAddUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_TARGET_SET_VELOCITY] = targetVelSetUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_TRANSPARENCY] = transUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_TURN] = turnUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_ADD_VARIABLE] = varAddUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_SET_VARIABLE_RANDOM] = varRandomUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_SET_VARIABLE_RANGE] = varRangeSetUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_SET_VARIABLE] = varSetUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_ADD_VELOCITY] = velAddUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_MULTIPLY_VELOCITY] = velMulUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_SET_VELOCITY] = velSetUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_VICTORY_QUOTE] = victoryQuoteUnloadFunction;
	gMugenStateControllerVariableHandler.mStateControllerUnloaders[MUGEN_STATE_CONTROLLER_TYPE_WIDTH] = widthUnloadFunction;
}

void setupDreamMugenStateControllerHandler(MemoryStack* tMemoryStack) {
	setupStateControllerParsers();
	setupStateControllerHandlers();
	setupStateControllerUnloaders();
	gMugenStateControllerVariableHandler.mMemoryStack = tMemoryStack;
}


typedef struct {
	DreamMugenAssignment* mID;

	DreamMugenAssignment* mAnimation;
	DreamMugenAssignment* mIsLooping;
	DreamMugenAssignment* mPosition;
	DreamMugenAssignment* mIsBoundToStage;

	int mHasShadow;
	DreamMugenAssignment* mShadowBasePositionY;

} CreateAnimationStoryController;

static void parseCreateAnimationStoryController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	CreateAnimationStoryController* e = (CreateAnimationStoryController*)allocMemoryOnMemoryStackOrMemory(sizeof(CreateAnimationStoryController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("anim", tGroup, &e->mAnimation);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("loop", tGroup, &e->mIsLooping);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("pos", tGroup, &e->mPosition);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("stage", tGroup, &e->mIsBoundToStage);
	e->mHasShadow = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("shadow", tGroup, &e->mShadowBasePositionY);

	tController->mType = MUGEN_STORY_STATE_CONTROLLER_TYPE_CREATE_ANIMATION;
	tController->mData = e;
}

typedef struct {
	DreamMugenAssignment* mID;
	DreamMugenAssignment* mAnimation;

	int mHasTarget;
	DreamMugenAssignment* mTarget;
} ChangeAnimationStoryController;

static void parseChangeAnimationStoryController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup, DreamMugenStateControllerType tType) {
	ChangeAnimationStoryController* e = (ChangeAnimationStoryController*)allocMemoryOnMemoryStackOrMemory(sizeof(ChangeAnimationStoryController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("anim", tGroup, &e->mAnimation);
	e->mHasTarget = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("target", tGroup, &e->mTarget);

	tController->mType = tType;
	tController->mData = e;

}

typedef struct {
	DreamMugenAssignment* mID;
	DreamMugenAssignment* mPosition;

	DreamMugenAssignment* mTextOffset;

	int mHasBackgroundSprite;
	DreamMugenAssignment* mBackgroundSprite;
	int mHasBackgroundAnimation;
	DreamMugenAssignment* mBackgroundAnimation;
	DreamMugenAssignment* mBackgroundOffset;

	int mHasFaceSprite;
	DreamMugenAssignment* mFaceSprite;
	int mHasFaceAnimation;
	DreamMugenAssignment* mFaceAnimation;
	DreamMugenAssignment* mFaceOffset;

	int mHasContinueSprite;
	DreamMugenAssignment* mContinueSprite;
	int mHasContinueAnimation;
	DreamMugenAssignment* mContinueAnimation;
	DreamMugenAssignment* mContinueOffset;

	int mHasName;
	DreamMugenAssignment* mName;
	DreamMugenAssignment* mNameFont;
	DreamMugenAssignment* mNameOffset;

	DreamMugenAssignment* mText;
	DreamMugenAssignment* mFont;
	DreamMugenAssignment* mWidth;

	DreamMugenAssignment* mIsBuildingUp;

	int mHasNextState;
	DreamMugenAssignment* mNextState;
} CreateTextStoryController;

static void parseCreateTextStoryController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	CreateTextStoryController* e = (CreateTextStoryController*)allocMemoryOnMemoryStackOrMemory(sizeof(CreateTextStoryController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("pos", tGroup, &e->mPosition);

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("text.offset", tGroup, &e->mTextOffset);

	e->mHasBackgroundSprite = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("bg.spr", tGroup, &e->mBackgroundSprite);
	e->mHasBackgroundAnimation = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("bg.anim", tGroup, &e->mBackgroundAnimation);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("bg.offset", tGroup, &e->mBackgroundOffset);

	e->mHasFaceSprite = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("face.spr", tGroup, &e->mFaceSprite);
	e->mHasFaceAnimation = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("face.anim", tGroup, &e->mFaceAnimation);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("face.offset", tGroup, &e->mFaceOffset);

	e->mHasContinueSprite = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("continue.spr", tGroup, &e->mContinueSprite);
	e->mHasContinueAnimation = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("continue.anim", tGroup, &e->mContinueAnimation);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("continue.offset", tGroup, &e->mContinueOffset);

	e->mHasName = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("name", tGroup, &e->mName);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("name.font", tGroup, &e->mNameFont);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("name.offset", tGroup, &e->mNameOffset);


	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("text", tGroup, &e->mText);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("font", tGroup, &e->mFont, "1 , 0 , 0");
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("width", tGroup, &e->mWidth);

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("buildup", tGroup, &e->mIsBuildingUp);

	e->mHasNextState = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("nextstate", tGroup, &e->mNextState);

	tController->mType = MUGEN_STORY_STATE_CONTROLLER_TYPE_CREATE_TEXT;
	tController->mData = e;
}

typedef struct {
	DreamMugenAssignment* mID;
} RemoveElementStoryController;

static void parseRemoveElementStoryController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup, DreamMugenStateControllerType tType) {
	RemoveElementStoryController* e = (RemoveElementStoryController*)allocMemoryOnMemoryStackOrMemory(sizeof(RemoveElementStoryController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);
	
	tController->mType = tType;
	tController->mData = e;
}

typedef struct {
	DreamMugenAssignment* mID;
	int mHasTarget;
	DreamMugenAssignment* mTarget;

	int mDoesChangePosition;
	DreamMugenAssignment* mPosition;

	int mDoesChangeBackgroundSprite;
	DreamMugenAssignment* mBackgroundSprite;
	int mDoesChangeBackgroundOffset;
	DreamMugenAssignment* mBackgroundOffset;

	int mDoesChangeFaceSprite;
	DreamMugenAssignment* mFaceSprite;
	int mDoesChangeFaceOffset;
	DreamMugenAssignment* mFaceOffset;

	int mDoesChangeContinueAnimation;
	DreamMugenAssignment* mContinueAnimation;
	int mDoesChangeContinueOffset;
	DreamMugenAssignment* mContinueOffset;

	int mDoesChangeName;
	DreamMugenAssignment* mName;
	int mDoesChangeNameFont;
	DreamMugenAssignment* mNameFont;
	int mDoesChangeNameOffset;
	DreamMugenAssignment* mNameOffset;

	int mDoesChangeText;
	DreamMugenAssignment* mText;

	int mDoesChangeTextOffset;
	DreamMugenAssignment* mTextOffset;

	int mDoesChangeNextState;
	DreamMugenAssignment* mNextState;

	DreamMugenAssignment* mIsBuildingUp;
} ChangeTextStoryController;

static void parseChangeTextStoryController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	ChangeTextStoryController* e = (ChangeTextStoryController*)allocMemoryOnMemoryStackOrMemory(sizeof(ChangeTextStoryController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);
	e->mHasTarget = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("target", tGroup, &e->mTarget);
	e->mDoesChangePosition = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("pos", tGroup, &e->mPosition);

	e->mDoesChangeBackgroundSprite = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("bg.spr", tGroup, &e->mBackgroundSprite);
	e->mDoesChangeBackgroundOffset = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("bg.offset", tGroup, &e->mBackgroundOffset);

	e->mDoesChangeFaceSprite = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("face.spr", tGroup, &e->mFaceSprite);
	e->mDoesChangeFaceOffset = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("face.offset", tGroup, &e->mFaceOffset);

	e->mDoesChangeContinueAnimation = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("continue.anim", tGroup, &e->mContinueAnimation);
	e->mDoesChangeContinueOffset = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("continue.offset", tGroup, &e->mContinueOffset);

	e->mDoesChangeName = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("name", tGroup, &e->mName);
	e->mDoesChangeNameFont = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("name.font", tGroup, &e->mNameFont);
	e->mDoesChangeNameOffset = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("name.offset", tGroup, &e->mNameOffset);

	e->mDoesChangeText = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("text", tGroup, &e->mText);
	e->mDoesChangeTextOffset = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("text.offset", tGroup, &e->mTextOffset);

	e->mDoesChangeNextState = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("nextstate", tGroup, &e->mNextState);

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("buildup", tGroup, &e->mIsBuildingUp);

	tController->mType = MUGEN_STORY_STATE_CONTROLLER_TYPE_CHANGE_TEXT;
	tController->mData = e;
}

typedef struct {
	DreamMugenAssignment* mDuration;
	DreamMugenAssignment* mColor;

} FadeStoryController;

static void parseFadeStoryController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup, DreamMugenStateControllerType tType) {
	FadeStoryController* e = (FadeStoryController*)allocMemoryOnMemoryStackOrMemory(sizeof(FadeStoryController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("duration", tGroup, &e->mDuration);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("color", tGroup, &e->mColor, "0,0,0");

	tController->mType = tType;
	tController->mData = e;
}

typedef struct {
	DreamMugenAssignment* mID;
	DreamMugenAssignment* mFacing;
	
	int mHasTarget;
	DreamMugenAssignment* mTarget;

} AnimationSetFaceDirectionStoryController;

static void parseAnimationSetFaceDirectionStoryController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup, DreamMugenStateControllerType tType) {
	AnimationSetFaceDirectionStoryController* e = (AnimationSetFaceDirectionStoryController*)allocMemoryOnMemoryStackOrMemory(sizeof(AnimationSetFaceDirectionStoryController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("facing", tGroup, &e->mFacing);
	e->mHasTarget = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("target", tGroup, &e->mTarget);

	tController->mType = tType;
	tController->mData = e;
}

static void parseAnimationAngleStoryController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup, DreamMugenStateControllerType tType) {
	AnimationSetFaceDirectionStoryController* e = (AnimationSetFaceDirectionStoryController*)allocMemoryOnMemoryStackOrMemory(sizeof(AnimationSetFaceDirectionStoryController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("angle", tGroup, &e->mFacing);
	e->mHasTarget = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("target", tGroup, &e->mTarget);

	tController->mType = tType;
	tController->mData = e;
}

typedef struct {
	DreamMugenAssignment* mID;
	DreamMugenAssignment* mValue;
	int mHasTarget;
	DreamMugenAssignment* mTarget;

} AnimationSetSingleValueStoryController;

static void parseAnimationSetColorStoryController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup, DreamMugenStateControllerType tType) {
	AnimationSetSingleValueStoryController* e = (AnimationSetSingleValueStoryController*)allocMemoryOnMemoryStackOrMemory(sizeof(AnimationSetSingleValueStoryController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("color", tGroup, &e->mValue, "1,1,1");
	e->mHasTarget = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("target", tGroup, &e->mTarget);

	tController->mType = tType;
	tController->mData = e;
}

static void parseAnimationSetOpacityStoryController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup, DreamMugenStateControllerType tType) {
	AnimationSetSingleValueStoryController* e = (AnimationSetSingleValueStoryController*)allocMemoryOnMemoryStackOrMemory(sizeof(AnimationSetSingleValueStoryController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("opacity", tGroup, &e->mValue, "1");
	e->mHasTarget = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("target", tGroup, &e->mTarget);

	tController->mType = tType;
	tController->mData = e;
}

typedef struct {
	DreamMugenAssignment* mID;

	DreamMugenAssignment* mName;
	DreamMugenAssignment* mPreferredPalette;
	DreamMugenAssignment* mStartAnimationNumber;
	DreamMugenAssignment* mPosition;
	DreamMugenAssignment* mIsBoundToStage;

	int mHasShadow;
	DreamMugenAssignment* mShadowBasePositionY;
} CreateCharacterStoryController;

static void parseCreateCharStoryController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	CreateCharacterStoryController* e = (CreateCharacterStoryController*)allocMemoryOnMemoryStackOrMemory(sizeof(CreateCharacterStoryController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("name", tGroup, &e->mName);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("palette", tGroup, &e->mPreferredPalette);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("anim", tGroup, &e->mStartAnimationNumber);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("pos", tGroup, &e->mPosition);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("stage", tGroup, &e->mIsBoundToStage);
	e->mHasShadow = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("shadow", tGroup, &e->mShadowBasePositionY);

	tController->mType = MUGEN_STORY_STATE_CONTROLLER_TYPE_CREATE_CHARACTER;
	tController->mData = e;
}

typedef struct {
	DreamMugenAssignment* mID;
	DreamMugenAssignment* mState;
} CreateHelperStoryController;

static void parseCreateHelperStoryController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	CreateHelperStoryController* e = (CreateHelperStoryController*)allocMemoryOnMemoryStackOrMemory(sizeof(CreateHelperStoryController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("state", tGroup, &e->mState);

	tController->mType = MUGEN_STORY_STATE_CONTROLLER_TYPE_CREATE_HELPER;
	tController->mData = e;
}

typedef struct {
	DreamMugenAssignment* mID;
	DreamMugenAssignment* mCharacterID;
	DreamMugenAssignment* mOffset;
} LockTextToCharacterStoryController;

static void parseLockTextToCharacterStoryController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	LockTextToCharacterStoryController* e = (LockTextToCharacterStoryController*)allocMemoryOnMemoryStackOrMemory(sizeof(LockTextToCharacterStoryController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("character", tGroup, &e->mCharacterID);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("offset", tGroup, &e->mOffset);

	tController->mType = MUGEN_STORY_STATE_CONTROLLER_TYPE_LOCK_TEXT_TO_CHARACTER;
	tController->mData = e;
}

typedef struct {
	DreamMugenAssignment* mID;
	DreamMugenAssignment* mName;
} NameTextStoryController;

static void parseNameIDStoryController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	NameTextStoryController* e = (NameTextStoryController*)allocMemoryOnMemoryStackOrMemory(sizeof(NameTextStoryController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);
	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("name", tGroup, &e->mName);

	tController->mType = MUGEN_STORY_STATE_CONTROLLER_TYPE_NAME_ID;
	tController->mData = e;
}


enum StoryVarSetType : uint8_t {
	STORY_VAR_SET_TYPE_INTEGER,
	STORY_VAR_SET_TYPE_FLOAT,
	STORY_VAR_SET_TYPE_STRING,
};

typedef struct {
	DreamMugenAssignment* mID;
	DreamMugenAssignment* mAssignment;

	uint8_t mType;
} StoryVarSetControllerEntry;

typedef struct {
	Vector mStoryVarSets;
} StoryVarSetController;

typedef struct {
	StoryVarSetController* mController;
	MugenDefScriptGroup* mGroup;
} StoryVarSetControllerCaller;

static void parseSingleStoryVarSetControllerEntry(StoryVarSetControllerCaller* tCaller, const string& tName, MugenDefScriptGroupElement& tData) {
	StoryVarSetControllerCaller* caller = (StoryVarSetControllerCaller*)tCaller;
	(void)tData;

	int isEntry = tName.find('(') != string::npos;
	if (!isEntry) return;

	StoryVarSetControllerEntry* e = (StoryVarSetControllerEntry*)allocMemoryOnMemoryStackOrMemory(sizeof(StoryVarSetControllerEntry));

	char name[100];
	strcpy(name, tName.data());
	char* start = strchr(name, '(');
	assert(start != NULL);
	*start = '\0';

	char value[100];
	strcpy(value, start + 1);
	char* end = strchr(value, ')');
	assert(end != NULL);
	*end = '\0';


	if (!strcmp("var", name)) {
		e->mType = STORY_VAR_SET_TYPE_INTEGER;
	}
	else if (!strcmp("fvar", name)) {
		e->mType = STORY_VAR_SET_TYPE_FLOAT;
	}
	else if (!strcmp("svar", name)) {
		e->mType = STORY_VAR_SET_TYPE_STRING;
	}
	else {
		logWarningFormat("Unrecognized variable setting name %s. Default to var.", name);
		e->mType = STORY_VAR_SET_TYPE_INTEGER;
	}
	
	e->mID = parseDreamMugenAssignmentFromString(value);
	fetchDreamAssignmentFromGroupAndReturnWhetherItExists(tName.data(), caller->mGroup, &e->mAssignment);

	vector_push_back_owned(&caller->mController->mStoryVarSets, e);
}

static void loadSingleOriginalStoryVarSetController(Vector* tDst, MugenDefScriptGroup* tGroup, MugenDefScriptGroupElement* tIDElement, StoryVarSetType tType) {
	StoryVarSetControllerEntry* e = (StoryVarSetControllerEntry*)allocMemoryOnMemoryStackOrMemory(sizeof(StoryVarSetControllerEntry));
	e->mType = tType;
	fetchDreamAssignmentFromGroupAsElement(tIDElement, &e->mID);
	fetchDreamAssignmentFromGroupAndReturnWhetherItExists("value", tGroup, &e->mAssignment);

	vector_push_back_owned(tDst, e);
}

static void unloadSingleStoryVarSetEntry(void* tCaller, void* tData) {
	(void)tCaller;
	StoryVarSetControllerEntry* e = (StoryVarSetControllerEntry*)tData;
	destroyDreamMugenAssignment(e->mAssignment);
}

static void parseStoryVarSetController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup, DreamMugenStoryStateControllerType tType) {
	int isIntegerVersion = stl_string_map_contains_array(tGroup->mElements, "v");
	int isFloatVersion = stl_string_map_contains_array(tGroup->mElements, "fv");
	int isStringVersion = stl_string_map_contains_array(tGroup->mElements, "sv");

	StoryVarSetController* e = (StoryVarSetController*)allocMemoryOnMemoryStackOrMemory(sizeof(StoryVarSetController));
	e->mStoryVarSets = new_vector();

	if (isIntegerVersion) {
		loadSingleOriginalStoryVarSetController(&e->mStoryVarSets, tGroup, &tGroup->mElements["v"], STORY_VAR_SET_TYPE_INTEGER);
	}
	else if (isFloatVersion) {
		loadSingleOriginalStoryVarSetController(&e->mStoryVarSets, tGroup, &tGroup->mElements["fv"], STORY_VAR_SET_TYPE_FLOAT);
	}
	else if (isStringVersion) {
		loadSingleOriginalStoryVarSetController(&e->mStoryVarSets, tGroup, &tGroup->mElements["sv"], STORY_VAR_SET_TYPE_STRING);
	}
	else {
		StoryVarSetControllerCaller caller;
		caller.mController = e;
		caller.mGroup = tGroup;

		stl_string_map_map(tGroup->mElements, parseSingleStoryVarSetControllerEntry, &caller);
	}

	if (vector_size(&e->mStoryVarSets) != 1) {
		logWarning("Unable to parse StoryVarSetController. Missing elements. Defaulting to Null controller.");
		vector_map(&e->mStoryVarSets, unloadSingleStoryVarSetEntry, NULL);
		delete_vector(&e->mStoryVarSets);
		//freeMemory(e); // TOOD: free _maybe_?
		parseNullController(tController, MUGEN_STATE_CONTROLLER_TYPE_NULL);
		return;
	}

	tController->mType = tType;
	tController->mData = e;
}

typedef struct {
	DreamMugenAssignment* mID;

	int mHasTarget;
	DreamMugenAssignment* mTarget;
	DreamMugenAssignment* x;
	DreamMugenAssignment* y;

	uint8_t mIsSettingX;
	uint8_t mIsSettingY;
} StoryTarget2DPhysicsController;

static void parseStoryTarget2DPhysicsController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup, DreamMugenStateControllerType tType) {
	StoryTarget2DPhysicsController* e = (StoryTarget2DPhysicsController*)allocMemoryOnMemoryStackOrMemory(sizeof(StoryTarget2DPhysicsController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("id", tGroup, &e->mID);
	e->mIsSettingX = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("x", tGroup, &e->x);
	e->mIsSettingY = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("y", tGroup, &e->y);
	e->mHasTarget = fetchDreamAssignmentFromGroupAndReturnWhetherItExists("target", tGroup, &e->mTarget);

	tController->mType = tType;
	tController->mData = e;
}

typedef struct {
	DreamMugenAssignment* mPath;
} StoryPlayMusicController;

static void parseStoryPlayMusicController(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) {
	StoryPlayMusicController* e = (StoryPlayMusicController*)allocMemoryOnMemoryStackOrMemory(sizeof(StoryPlayMusicController));

	fetchAssignmentFromGroupAndReturnWhetherItExistsDefaultString("path", tGroup, &e->mPath);

	tController->mType = MUGEN_STORY_STATE_CONTROLLER_TYPE_PLAY_MUSIC;
	tController->mData = e;
}


void nullStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* /*tGroup*/) { parseNullController(tController, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_NULL); }
void createAnimationStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseCreateAnimationStoryController(tController, tGroup); }
void removeAnimationStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseRemoveElementStoryController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_REMOVE_ANIMATION); }
void changeAnimationStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseChangeAnimationStoryController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_CHANGE_ANIMATION); }
void createTextStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseCreateTextStoryController(tController, tGroup); }
void removeTextStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseRemoveElementStoryController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_REMOVE_TEXT); }
void changeTextStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseChangeTextStoryController(tController, tGroup); }
void lockTextToCharacterStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseLockTextToCharacterStoryController(tController, tGroup); }
void textPositionAddStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseTarget2DPhysicsController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_TEXT_ADD_POSITION); }
void nameIDStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseNameIDStoryController(tController, tGroup); }
void changeStateStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseSingleRequiredValueController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_CHANGE_STATE); }
void changeStateRootStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseSingleRequiredValueController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_CHANGE_STATE_ROOT); }
void fadeInStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseFadeStoryController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_FADE_IN); }
void fadeOutStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseFadeStoryController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_FADE_OUT); }
void gotoStoryStepStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseSingleRequiredValueController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_GOTO_STORY_STEP); }
void gotoIntroStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* /*tGroup*/) { parseNullController(tController, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_GOTO_INTRO); }
void gotoTitleStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* /*tGroup*/) { parseNullController(tController, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_GOTO_TITLE); }
void animationSetPositionStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseStoryTarget2DPhysicsController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_ANIMATION_SET_POSITION); }
void animationAddPositionStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseStoryTarget2DPhysicsController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_ANIMATION_ADD_POSITION); }
void animationSetScaleStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseTarget2DPhysicsController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_ANIMATION_SET_SCALE); }
void animationSetFaceDirectionStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseAnimationSetFaceDirectionStoryController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_ANIMATION_SET_FACEDIRECTION); }
void animationSetAngleStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseAnimationAngleStoryController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_ANIMATION_SET_ANGLE); }
void animationAddAngleStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseAnimationAngleStoryController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_ANIMATION_ADD_ANGLE); }
void animationSetColorStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseAnimationSetColorStoryController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_ANIMATION_SET_COLOR); }
void animationSetOpacityStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseAnimationSetOpacityStoryController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_ANIMATION_SET_OPACITY); }
void endStoryboardStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseSingleRequiredValueController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_END_STORYBOARD); }
void moveStageStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parse2DPhysicsController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_MOVE_STAGE); }
void createCharStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseCreateCharStoryController(tController, tGroup); }
void removeCharStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseRemoveElementStoryController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_REMOVE_CHARACTER);  }
void charChangeAnimStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseChangeAnimationStoryController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_CHARACTER_CHANGE_ANIMATION); }
void charSetPosStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseStoryTarget2DPhysicsController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_CHARACTER_SET_POSITION); }
void charAddPosStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseStoryTarget2DPhysicsController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_CHARACTER_ADD_POSITION); }
void charSetScaleStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseStoryTarget2DPhysicsController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_CHARACTER_SET_SCALE); }
void charSetFaceDirectionStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseAnimationSetFaceDirectionStoryController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_CHARACTER_SET_FACEDIRECTION); }
void charSetColorStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseAnimationSetColorStoryController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_CHARACTER_SET_COLOR); }
void charSetOpacityStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseAnimationSetOpacityStoryController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_CHARACTER_SET_OPACITY); }
void charSetAngleStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseAnimationAngleStoryController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_CHARACTER_SET_ANGLE); }
void charAddAngleStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseAnimationAngleStoryController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_CHARACTER_ADD_ANGLE); }
void createHelperStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseCreateHelperStoryController(tController, tGroup); }
void removeHelperStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseRemoveElementStoryController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_REMOVE_HELPER); }
void varSetStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseStoryVarSetController(tController, tGroup, MUGEN_STORY_STATE_CONTROLLER_TYPE_VAR_SET); }
void varAddStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseStoryVarSetController(tController, tGroup, MUGEN_STORY_STATE_CONTROLLER_TYPE_VAR_ADD); }
void globalVarSetStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseStoryVarSetController(tController, tGroup, MUGEN_STORY_STATE_CONTROLLER_TYPE_GLOBAL_VAR_SET); }
void globalVarAddStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseStoryVarSetController(tController, tGroup, MUGEN_STORY_STATE_CONTROLLER_TYPE_GLOBAL_VAR_ADD); }
void playMusicStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseStoryPlayMusicController(tController, tGroup); }
void stopMusicStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* /*tGroup*/) { parseNullController(tController, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_STOP_MUSIC); }
void pauseMusicStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* /*tGroup*/) { parseNullController(tController, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_PAUSE_MUSIC); }
void resumeMusicStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* /*tGroup*/) { parseNullController(tController, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_RESUME_MUSIC); }
void destroySelfStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* /*tGroup*/) { parseNullController(tController, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_DESTROY_SELF); }
void cameraFocusStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parse2DPhysicsController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_CAMERA_FOCUS); }
void cameraZoomStoryParseFunction(DreamMugenStateController* tController, MugenDefScriptGroup* tGroup) { parseSingleRequiredValueController(tController, tGroup, (DreamMugenStateControllerType)MUGEN_STORY_STATE_CONTROLLER_TYPE_CAMERA_ZOOM); }

static void setupStoryStateControllerParsers() {
	gMugenStateControllerVariableHandler.mStateControllerParsers.clear();
	
	gMugenStateControllerVariableHandler.mStateControllerParsers["null"] = nullStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["createanim"] = createAnimationStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["removeanim"] = removeAnimationStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["changeanim"] = changeAnimationStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["createtext"] = createTextStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["removetext"] = removeTextStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["changetext"] = changeTextStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["locktext"] = lockTextToCharacterStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["textposadd"] = textPositionAddStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["nameid"] = nameIDStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["changestate"] = changeStateStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["changestateroot"] = changeStateRootStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["fadein"] = fadeInStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["fadeout"] = fadeOutStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["gotostorystep"] = gotoStoryStepStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["gotointro"] = gotoIntroStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["gototitle"] = gotoTitleStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["animposset"] = animationSetPositionStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["animposadd"] = animationAddPositionStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["animscaleset"] = animationSetScaleStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["animsetfacing"] = animationSetFaceDirectionStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["animsetangle"] = animationSetAngleStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["animaddangle"] = animationAddAngleStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["animsetcolor"] = animationSetColorStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["animsetopacity"] = animationSetOpacityStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["endstoryboard"] = endStoryboardStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["movestage"] = moveStageStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["createchar"] = createCharStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["removechar"] = removeCharStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["charchangeanim"] = charChangeAnimStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["charposset"] = charSetPosStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["charposadd"] = charAddPosStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["charscaleset"] = charSetScaleStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["charsetfacing"] = charSetFaceDirectionStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["charsetcolor"] = charSetColorStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["charsetopacity"] = charSetOpacityStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["charsetangle"] = charSetAngleStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["charaddangle"] = charAddAngleStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["createhelper"] = createHelperStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["removehelper"] = removeHelperStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["varset"] = varSetStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["varadd"] = varAddStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["globalvarset"] = globalVarSetStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["globalvaradd"] = globalVarAddStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["playmusic"] = playMusicStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["stopmusic"] = stopMusicStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["pausemusic"] = pauseMusicStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["resumemusic"] = resumeMusicStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["destroyself"] = destroySelfStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["camerafocus"] = cameraFocusStoryParseFunction;
	gMugenStateControllerVariableHandler.mStateControllerParsers["camerazoom"] = cameraZoomStoryParseFunction;
}

static int getDolmexicaStoryIDFromAssignment(DreamMugenAssignment** tAssignment, StoryInstance* tInstance) {
	string val;
	evaluateDreamAssignmentAndReturnAsString(val, tAssignment, (DreamPlayer*)tInstance);
	return getDolmexicaStoryIDFromString(val.data(), tInstance);
}

static int handleCreateAnimationStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	CreateAnimationStoryController* e = (CreateAnimationStoryController*)tController->mData;

	int id, animation, isLooping, isBoundToStage;
	Position position = makePosition(0, 0, 0);

	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);
	getSingleIntegerValueOrDefault(&e->mAnimation, (DreamPlayer*)tInstance, &animation, 0);
	getSingleIntegerValueOrDefault(&e->mIsLooping, (DreamPlayer*)tInstance, &isLooping, 1);
	getSingleIntegerValueOrDefault(&e->mIsBoundToStage, (DreamPlayer*)tInstance, &isBoundToStage, 0);
	getTwoFloatValuesWithDefaultValues(&e->mPosition, (DreamPlayer*)tInstance, &position.x, &position.y, 0, 0);

	addDolmexicaStoryAnimation(tInstance, id, animation, position);
	if (e->mHasShadow) {
		double shadowBasePosition;
		getSingleFloatValueOrDefault(&e->mShadowBasePositionY, (DreamPlayer*)tInstance, &shadowBasePosition, 0);
		setDolmexicaStoryAnimationShadow(tInstance, id, shadowBasePosition);
	}
	setDolmexicaStoryAnimationLooping(tInstance, id, isLooping);
	setDolmexicaStoryAnimationBoundToStage(tInstance, id, isBoundToStage);


	return 0;
}

static int handleRemoveAnimationStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	RemoveElementStoryController* e = (RemoveElementStoryController*)tController->mData;

	int id;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);

	removeDolmexicaStoryAnimation(tInstance, id);

	return 0;
}

static int handleChangeAnimationStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	ChangeAnimationStoryController* e = (ChangeAnimationStoryController*)tController->mData;

	int id, animation;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);
	getSingleIntegerValueOrDefault(&e->mAnimation, (DreamPlayer*)tInstance, &animation, 0);

	changeDolmexicaStoryAnimation(tInstance, id, animation);

	return 0;
}

static void handleSingleStoryTextSpriteOrAnimation(int id, int tHasSprite, DreamMugenAssignment** tSprite, void(*tSpriteFunc)(StoryInstance*, int, Vector3DI, Position), int tHasAnimation, DreamMugenAssignment** tAnimation, void(*tAnimationFunc)(StoryInstance*, int, int, Position), DreamMugenAssignment** tOffset, StoryInstance* tInstance) {
	if (tHasAnimation) {
		int animation;
		Position offset = makePosition(0, 0, 0);
		getSingleIntegerValueOrDefault(tAnimation, (DreamPlayer*)tInstance, &animation, 0);
		getTwoFloatValuesWithDefaultValues(tOffset, (DreamPlayer*)tInstance, &offset.x, &offset.y, 0, 0);
		tAnimationFunc(tInstance, id, animation, offset);
	}
	else if (tHasSprite) {
		Vector3DI sprite = makeVector3DI(0, 0, 0);
		Position offset = makePosition(0, 0, 0);
		getTwoIntegerValuesWithDefaultValues(tSprite, (DreamPlayer*)tInstance, &sprite.x, &sprite.y, 0, 0);
		getTwoFloatValuesWithDefaultValues(tOffset, (DreamPlayer*)tInstance, &offset.x, &offset.y, 0, 0);
		tSpriteFunc(tInstance, id, sprite, offset);
	}
}

static int isStringEmptyOrWhitespace(const char * tString)
{
	for (; *tString; tString++) {
		if (*tString != ' ') return 0;
	}
	return 1;
}

static void handleSingleStoryTextName(int id, int tHasName, DreamMugenAssignment** tText, DreamMugenAssignment** tFont, DreamMugenAssignment** tOffset, StoryInstance* tInstance) {
	if (!tHasName) return;

	string text;
	evaluateDreamAssignmentAndReturnAsString(text, tText, (DreamPlayer*)tInstance);
	Vector3DI font = evaluateDreamAssignmentAndReturnAsVector3DI(tFont, (DreamPlayer*)tInstance);
	double x, y;
	getTwoFloatValuesWithDefaultValues(tOffset, (DreamPlayer*)tInstance, &x, &y, 0, 0);
	setDolmexicaStoryTextName(tInstance, id, text.data(), font, makePosition(x, y, 0));
}

static int handleCreateTextStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	CreateTextStoryController* e = (CreateTextStoryController*)tController->mData;

	int id;
	Position basePosition = makePosition(0, 0, 0);
	Position textOffset = makePosition(0, 0, 0);

	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);
	getTwoFloatValuesWithDefaultValues(&e->mPosition, (DreamPlayer*)tInstance, &basePosition.x, &basePosition.y, 0, 0);
	getTwoFloatValuesWithDefaultValues(&e->mTextOffset, (DreamPlayer*)tInstance, &textOffset.x, &textOffset.y, 0, 0);

	double width;
	getSingleFloatValueOrDefault(&e->mWidth, (DreamPlayer*)tInstance, &width, INF);
	Vector3DI font = evaluateDreamAssignmentAndReturnAsVector3DI(&e->mFont, (DreamPlayer*)tInstance);
	string text;
	evaluateDreamAssignmentAndReturnAsString(text, &e->mText, (DreamPlayer*)tInstance);
	int isEmpty = isStringEmptyOrWhitespace(text.data());
	addDolmexicaStoryText(tInstance, id, text.data(), font, basePosition, textOffset, width);

	handleSingleStoryTextSpriteOrAnimation(id, e->mHasBackgroundSprite, &e->mBackgroundSprite, setDolmexicaStoryTextBackground, e->mHasBackgroundAnimation, &e->mBackgroundAnimation, setDolmexicaStoryTextBackground, &e->mBackgroundOffset, tInstance);
	handleSingleStoryTextSpriteOrAnimation(id, e->mHasFaceSprite, &e->mFaceSprite, setDolmexicaStoryTextFace, e->mHasFaceAnimation, &e->mFaceAnimation, setDolmexicaStoryTextFace, &e->mFaceOffset, tInstance);
	handleSingleStoryTextSpriteOrAnimation(id, e->mHasContinueSprite, &e->mContinueSprite, setDolmexicaStoryTextContinue, e->mHasContinueAnimation, &e->mContinueAnimation, setDolmexicaStoryTextContinue, &e->mContinueOffset, tInstance);
	handleSingleStoryTextName(id, e->mHasName, &e->mName, &e->mNameFont, &e->mNameOffset, tInstance);

	int buildUp;
	getSingleIntegerValueOrDefault(&e->mIsBuildingUp, (DreamPlayer*)tInstance, &buildUp, 1);

	if (e->mHasNextState) {
		int nextState = evaluateDreamAssignmentAndReturnAsInteger(&e->mNextState, (DreamPlayer*)tInstance);
		setDolmexicaStoryTextNextState(tInstance, id, nextState);
	}

	if (!buildUp) {
		setDolmexicaStoryTextBuiltUp(tInstance, id);
	}

	if (isEmpty) {
		setDolmexicaStoryTextInactive(tInstance, id);
	}

	return 0;
}

static int handleRemoveTextStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	RemoveElementStoryController* e = (RemoveElementStoryController*)tController->mData;

	int id;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);
	removeDolmexicaStoryText(tInstance, id);

	return 0;
}

static StoryInstance* getTargetInstanceFromAssignment(DreamMugenAssignment** tAssignment, StoryInstance* tInstance) {
	string target;
	evaluateDreamAssignmentAndReturnAsString(target, tAssignment, (DreamPlayer*)tInstance);
	int targetNumber;
	if (target == "root") {
		targetNumber = -1;
	}
	else {
		targetNumber = atoi(target.data());
	}
	return getDolmexicaStoryHelperInstance(targetNumber);
}

static int handleChangeTextStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	ChangeTextStoryController* e = (ChangeTextStoryController*)tController->mData;

	int id;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);

	StoryInstance* targetInstance = tInstance;
	if (e->mHasTarget) {
		targetInstance = getTargetInstanceFromAssignment(&e->mTarget, tInstance);
	}
	if (e->mDoesChangePosition) {
		Position offset = evaluateDreamAssignmentAndReturnAsVector3D(&e->mPosition, (DreamPlayer*)tInstance);
		setDolmexicaStoryTextBasePosition(targetInstance, id, offset);
	}
	if (e->mDoesChangeText) {
		string text;
		evaluateDreamAssignmentAndReturnAsString(text, &e->mText, (DreamPlayer*)tInstance);
		setDolmexicaStoryTextText(targetInstance, id, text.data());
	}
	if (e->mDoesChangeTextOffset) {
		Position offset = evaluateDreamAssignmentAndReturnAsVector3D(&e->mTextOffset, (DreamPlayer*)tInstance);
		setDolmexicaStoryTextTextOffset(targetInstance, id, offset);
	}
	if (e->mDoesChangeBackgroundSprite) {
		Vector3DI sprite = evaluateDreamAssignmentAndReturnAsVector3DI(&e->mBackgroundSprite, (DreamPlayer*)tInstance);
		setDolmexicaStoryTextBackgroundSprite(targetInstance, id, sprite);
	}
	if (e->mDoesChangeBackgroundOffset) {
		Position offset = evaluateDreamAssignmentAndReturnAsVector3D(&e->mBackgroundOffset, (DreamPlayer*)tInstance);
		setDolmexicaStoryTextBackgroundOffset(targetInstance, id, offset);
	}
	if (e->mDoesChangeFaceSprite) {
		Vector3DI sprite = evaluateDreamAssignmentAndReturnAsVector3DI(&e->mFaceSprite, (DreamPlayer*)tInstance);
		setDolmexicaStoryTextFaceSprite(targetInstance, id, sprite);
	}
	if (e->mDoesChangeFaceOffset) {
		Position offset = evaluateDreamAssignmentAndReturnAsVector3D(&e->mFaceOffset, (DreamPlayer*)tInstance);
		setDolmexicaStoryTextFaceOffset(targetInstance, id, offset);
	}
	if (e->mDoesChangeContinueAnimation) {
		int anim = evaluateDreamAssignmentAndReturnAsInteger(&e->mContinueAnimation, (DreamPlayer*)tInstance);
		setDolmexicaStoryTextContinueAnimation(targetInstance, id, anim);
	}
	if (e->mDoesChangeContinueOffset) {
		Position offset = evaluateDreamAssignmentAndReturnAsVector3D(&e->mContinueOffset, (DreamPlayer*)tInstance);
		setDolmexicaStoryTextContinueOffset(targetInstance, id, offset);
	}
	if (e->mDoesChangeName) {
		string text;
		evaluateDreamAssignmentAndReturnAsString(text, &e->mName, (DreamPlayer*)tInstance);
		setDolmexicaStoryTextNameText(targetInstance, id, text.data());
	}
	if (e->mDoesChangeNameFont) {
		Vector3DI font = evaluateDreamAssignmentAndReturnAsVector3DI(&e->mNameFont, (DreamPlayer*)tInstance);
		setDolmexicaStoryTextNameFont(targetInstance, id, font);
	}
	if (e->mDoesChangeNameOffset) {
		Position offset = evaluateDreamAssignmentAndReturnAsVector3D(&e->mNameOffset, (DreamPlayer*)tInstance);
		setDolmexicaStoryTextNameOffset(targetInstance, id, offset);
	}
	if (e->mDoesChangeNextState) {
		int nextState = evaluateDreamAssignmentAndReturnAsInteger(&e->mNextState, (DreamPlayer*)tInstance);
		setDolmexicaStoryTextNextState(targetInstance, id, nextState);
	}


	int buildUp;
	getSingleIntegerValueOrDefault(&e->mIsBuildingUp, (DreamPlayer*)tInstance, &buildUp, 1);
	if (!buildUp) {
		setDolmexicaStoryTextBuiltUp(targetInstance, id);
	}

	return 0;
}

static int handleChangeStateStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	SingleRequiredValueController* e = (SingleRequiredValueController*)tController->mData;
	
	int val = evaluateDreamAssignmentAndReturnAsInteger(&e->mValue, (DreamPlayer*)tInstance);
	changeDolmexicaStoryState(tInstance, val);

	return 1;
}

static int handleChangeStateRootStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	SingleRequiredValueController* e = (SingleRequiredValueController*)tController->mData;

	int val = evaluateDreamAssignmentAndReturnAsInteger(&e->mValue, (DreamPlayer*)tInstance);
	changeDolmexicaStoryStateOutsideStateHandler(getDolmexicaStoryRootInstance(), val);
	return 0;
}

static int handleFadeInStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	FadeStoryController* e = (FadeStoryController*)tController->mData;

	double duration;
	getSingleFloatValueOrDefault(&e->mDuration, (DreamPlayer*)tInstance, &duration, 20);
	addFadeIn(duration, NULL, NULL);

	return 0;
}

static int handleFadeOutStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	FadeStoryController* e = (FadeStoryController*)tController->mData;

	double duration;
	Vector3D color;
	getSingleFloatValueOrDefault(&e->mDuration, (DreamPlayer*)tInstance, &duration, 20);
	color = evaluateDreamAssignmentAndReturnAsVector3D(&e->mColor, (DreamPlayer*)tInstance);
	setFadeColorRGB(color.x, color.y, color.z);
	addFadeOut(duration, NULL, NULL);

	return 0;
}

static int handleGotoStoryStepStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	SingleRequiredValueController* e = (SingleRequiredValueController*)tController->mData;
	int newStep = evaluateDreamAssignmentAndReturnAsInteger(&e->mValue, (DreamPlayer*)tInstance);
	endDolmexicaStoryboard(tInstance, newStep);

	return 0;
}

static int handleGotoIntroStoryController() {
	playIntroStoryboard();
	return 0;
}

static int handleGotoTitleStoryController() {
	setNewScreen(getDreamTitleScreen());
	return 0;
}

static int handleAnimationSetPositionStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	StoryTarget2DPhysicsController* e = (StoryTarget2DPhysicsController*)tController->mData;

	int id;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);

	StoryInstance* targetInstance = tInstance;
	if (e->mHasTarget) {
		targetInstance = getTargetInstanceFromAssignment(&e->mTarget, tInstance);
	}

	if (e->mIsSettingX) {
		double x = evaluateDreamAssignmentAndReturnAsFloat(&e->x, (DreamPlayer*)tInstance);
		setDolmexicaStoryAnimationPositionX(targetInstance, id, x);
	}

	if (e->mIsSettingY) {
		double y = evaluateDreamAssignmentAndReturnAsFloat(&e->y, (DreamPlayer*)tInstance);
		setDolmexicaStoryAnimationPositionY(targetInstance, id, y);
	}
	return 0;
}

static int handleAnimationAddPositionStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	StoryTarget2DPhysicsController* e = (StoryTarget2DPhysicsController*)tController->mData;

	int id;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);

	StoryInstance* targetInstance = tInstance;
	if (e->mHasTarget) {
		targetInstance = getTargetInstanceFromAssignment(&e->mTarget, tInstance);
	}

	if (e->mIsSettingX) {
		double x = evaluateDreamAssignmentAndReturnAsFloat(&e->x, (DreamPlayer*)tInstance);
		addDolmexicaStoryAnimationPositionX(targetInstance, id, x);
	}

	if (e->mIsSettingY) {
		double y = evaluateDreamAssignmentAndReturnAsFloat(&e->y, (DreamPlayer*)tInstance);
		addDolmexicaStoryAnimationPositionY(targetInstance, id, y);
	}
	return 0;
}

static int handleAnimationSetScaleStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	Target2DPhysicsController* e = (Target2DPhysicsController*)tController->mData;

	int id;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);

	if (e->mIsSettingX) {
		double x = evaluateDreamAssignmentAndReturnAsFloat(&e->x, (DreamPlayer*)tInstance);
		setDolmexicaStoryAnimationScaleX(tInstance, id, x);
	}

	if (e->mIsSettingY) {
		double y = evaluateDreamAssignmentAndReturnAsFloat(&e->y, (DreamPlayer*)tInstance);
		setDolmexicaStoryAnimationScaleY(tInstance, id, y);
	}
	return 0;
}

static int handleAnimationSetFaceDirectionStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	AnimationSetFaceDirectionStoryController* e = (AnimationSetFaceDirectionStoryController*)tController->mData;

	int id, faceDirection;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);
	getSingleIntegerValueOrDefault(&e->mFacing, (DreamPlayer*)tInstance, &faceDirection, 1);

	if (!faceDirection) return 0;

	setDolmexicaStoryAnimationIsFacingRight(tInstance, id, faceDirection == 1);

	return 0;
}

static int handleAnimationSetAngleStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	AnimationSetFaceDirectionStoryController* e = (AnimationSetFaceDirectionStoryController*)tController->mData;

	int id;
	double angle;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);
	getSingleFloatValueOrDefault(&e->mFacing, (DreamPlayer*)tInstance, &angle, 0);

	setDolmexicaStoryAnimationAngle(tInstance, id, angle);

	return 0;
}

static int handleAnimationAddAngleStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	AnimationSetFaceDirectionStoryController* e = (AnimationSetFaceDirectionStoryController*)tController->mData;

	int id;
	double angle;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);
	getSingleFloatValueOrDefault(&e->mFacing, (DreamPlayer*)tInstance, &angle, 0);

	addDolmexicaStoryAnimationAngle(tInstance, id, angle);

	return 0;
}

static int handleAnimationSetColorStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	AnimationSetSingleValueStoryController* e = (AnimationSetSingleValueStoryController*)tController->mData;

	int id;
	Vector3D color;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);
	color = evaluateDreamAssignmentAndReturnAsVector3D(&e->mValue, (DreamPlayer*)tInstance);

	setDolmexicaStoryAnimationColor(tInstance, id, color);

	return 0;
}

static int handleAnimationSetOpacityStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	AnimationSetSingleValueStoryController* e = (AnimationSetSingleValueStoryController*)tController->mData;

	int id;
	double opacity;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);
	opacity = evaluateDreamAssignmentAndReturnAsFloat(&e->mValue, (DreamPlayer*)tInstance);

	setDolmexicaStoryAnimationOpacity(tInstance, id, opacity);

	return 0;
}

static int handleEndStoryboardController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	SingleRequiredValueController* e = (SingleRequiredValueController*)tController->mData;

	int nextState;
	getSingleIntegerValueOrDefault(&e->mValue, (DreamPlayer*)tInstance, &nextState, 1);

	endDolmexicaStoryboard(tInstance, nextState);

	return 0;
}

static int handleMoveStageStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	Set2DPhysicsController* e = (Set2DPhysicsController*)tController->mData;

	if (e->mIsSettingX) {
		double x = evaluateDreamAssignmentAndReturnAsFloat(&e->x, (DreamPlayer*)tInstance);
		addDreamMugenStageHandlerCameraPositionX(x);
	}

	if (e->mIsSettingY) {
		double y = evaluateDreamAssignmentAndReturnAsFloat(&e->y, (DreamPlayer*)tInstance);
		addDreamMugenStageHandlerCameraPositionY(y);
	}
	return 0;
}

static int handleCreateCharacterStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	CreateCharacterStoryController* e = (CreateCharacterStoryController*)tController->mData;

	int id, animation, isBoundToStage, preferredPalette;
	Position position = makePosition(0, 0, 0);
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);
	getSingleIntegerValueOrDefault(&e->mStartAnimationNumber, (DreamPlayer*)tInstance, &animation, 0);
	getSingleIntegerValueOrDefault(&e->mIsBoundToStage, (DreamPlayer*)tInstance, &isBoundToStage, 0);
	getSingleIntegerValueOrDefault(&e->mPreferredPalette, (DreamPlayer*)tInstance, &preferredPalette, 1);
	getTwoFloatValuesWithDefaultValues(&e->mPosition, (DreamPlayer*)tInstance, &position.x, &position.y, 0, 0);
	string name;
	evaluateDreamAssignmentAndReturnAsString(name, &e->mName, (DreamPlayer*)tInstance);

	addDolmexicaStoryCharacter(tInstance, id, name.data(), preferredPalette, animation, position);
	if (e->mHasShadow) {
		double shadowBasePosition;
		getSingleFloatValueOrDefault(&e->mShadowBasePositionY, (DreamPlayer*)tInstance, &shadowBasePosition, 0);
		setDolmexicaStoryCharacterShadow(tInstance, id, shadowBasePosition);
	}
	setDolmexicaStoryCharacterBoundToStage(tInstance, id, isBoundToStage);
	

	return 0;
}

static int handleRemoveCharacterStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	RemoveElementStoryController* e = (RemoveElementStoryController*)tController->mData;
	
	int id;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);

	removeDolmexicaStoryCharacter(tInstance, id);

	return 0;
}

static int handleChangeCharacterAnimStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	ChangeAnimationStoryController* e = (ChangeAnimationStoryController*)tController->mData;

	int id, animation;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);
	getSingleIntegerValueOrDefault(&e->mAnimation, (DreamPlayer*)tInstance, &animation, 0);

	if (e->mHasTarget) {
		tInstance = getTargetInstanceFromAssignment(&e->mTarget, tInstance);
	}

	changeDolmexicaStoryCharacterAnimation(tInstance, id, animation);

	return 0;
}

static int handleSetCharacterPosStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	StoryTarget2DPhysicsController* e = (StoryTarget2DPhysicsController*)tController->mData;

	int id;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);

	StoryInstance* targetInstance = tInstance;
	if (e->mHasTarget) {
		targetInstance = getTargetInstanceFromAssignment(&e->mTarget, tInstance);
	}

	if (e->mIsSettingX) {
		double x = evaluateDreamAssignmentAndReturnAsFloat(&e->x, (DreamPlayer*)tInstance);
		setDolmexicaStoryCharacterPositionX(targetInstance, id, x);
	}

	if (e->mIsSettingY) {
		double y = evaluateDreamAssignmentAndReturnAsFloat(&e->y, (DreamPlayer*)tInstance);
		setDolmexicaStoryCharacterPositionY(targetInstance, id, y);
	}
	return 0;
}

static int handleAddCharacterPosStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	StoryTarget2DPhysicsController* e = (StoryTarget2DPhysicsController*)tController->mData;

	int id;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);

	StoryInstance* targetInstance = tInstance;
	if (e->mHasTarget) {
		targetInstance = getTargetInstanceFromAssignment(&e->mTarget, tInstance);
	}

	if (e->mIsSettingX) {
		double x = evaluateDreamAssignmentAndReturnAsFloat(&e->x, (DreamPlayer*)tInstance);
		addDolmexicaStoryCharacterPositionX(targetInstance, id, x);
	}

	if (e->mIsSettingY) {
		double y = evaluateDreamAssignmentAndReturnAsFloat(&e->y, (DreamPlayer*)tInstance);
		addDolmexicaStoryCharacterPositionY(targetInstance, id, y);
	}
	return 0;
}

static int handleSetCharacterScaleStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	StoryTarget2DPhysicsController* e = (StoryTarget2DPhysicsController*)tController->mData;

	int id;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);

	StoryInstance* targetInstance = tInstance;
	if (e->mHasTarget) {
		targetInstance = getTargetInstanceFromAssignment(&e->mTarget, tInstance);
	}

	if (e->mIsSettingX) {
		double x = evaluateDreamAssignmentAndReturnAsFloat(&e->x, (DreamPlayer*)tInstance);
		setDolmexicaStoryCharacterScaleX(targetInstance, id, x);
	}

	if (e->mIsSettingY) {
		double y = evaluateDreamAssignmentAndReturnAsFloat(&e->y, (DreamPlayer*)tInstance);
		setDolmexicaStoryCharacterScaleY(targetInstance, id, y);
	}
	return 0;
}

static int handleSetCharacterFaceDirectionStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	AnimationSetFaceDirectionStoryController* e = (AnimationSetFaceDirectionStoryController*)tController->mData;

	int id, faceDirection;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);
	getSingleIntegerValueOrDefault(&e->mFacing, (DreamPlayer*)tInstance, &faceDirection, 1);

	if (!faceDirection) return 0;

	StoryInstance* targetInstance = tInstance;
	if (e->mHasTarget) {
		targetInstance = getTargetInstanceFromAssignment(&e->mTarget, tInstance);
	}

	setDolmexicaStoryCharacterIsFacingRight(targetInstance, id, faceDirection == 1);
	
	return 0;
}

static int handleSetCharacterColorStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	AnimationSetSingleValueStoryController* e = (AnimationSetSingleValueStoryController*)tController->mData;

	StoryInstance* targetInstance = tInstance;
	if (e->mHasTarget) {
		targetInstance = getTargetInstanceFromAssignment(&e->mTarget, tInstance);
	}

	int id;
	Vector3D color;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);
	color = evaluateDreamAssignmentAndReturnAsVector3D(&e->mValue, (DreamPlayer*)tInstance);

	setDolmexicaStoryCharacterColor(targetInstance, id, color);

	return 0;
}

static int handleSetCharacterOpacityStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	AnimationSetSingleValueStoryController* e = (AnimationSetSingleValueStoryController*)tController->mData;

	StoryInstance* targetInstance = tInstance;
	if (e->mHasTarget) {
		targetInstance = getTargetInstanceFromAssignment(&e->mTarget, tInstance);
	}

	int id;
	double opacity;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);
	opacity = evaluateDreamAssignmentAndReturnAsFloat(&e->mValue, (DreamPlayer*)tInstance);

	setDolmexicaStoryCharacterOpacity(targetInstance, id, opacity);

	return 0;
}

static int handleSetCharacterAngleStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	AnimationSetFaceDirectionStoryController* e = (AnimationSetFaceDirectionStoryController*)tController->mData;

	int id;
	double angle;

	StoryInstance* targetInstance = tInstance;
	if (e->mHasTarget) {
		targetInstance = getTargetInstanceFromAssignment(&e->mTarget, tInstance);
	}
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);
	getSingleFloatValueOrDefault(&e->mFacing, (DreamPlayer*)tInstance, &angle, 0);

	setDolmexicaStoryCharacterAngle(targetInstance, id, angle);

	return 0;
}

static int handleAddCharacterAngleStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	AnimationSetFaceDirectionStoryController* e = (AnimationSetFaceDirectionStoryController*)tController->mData;

	int id;
	double angle;

	StoryInstance* targetInstance = tInstance;
	if (e->mHasTarget) {
		targetInstance = getTargetInstanceFromAssignment(&e->mTarget, tInstance);
	}
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);
	getSingleFloatValueOrDefault(&e->mFacing, (DreamPlayer*)tInstance, &angle, 0);

	addDolmexicaStoryCharacterAngle(targetInstance, id, angle);

	return 0;
}

static int handleCreateHelperStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	CreateHelperStoryController* e = (CreateHelperStoryController*)tController->mData;

	int id, state;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);
	getSingleIntegerValueOrDefault(&e->mState, (DreamPlayer*)tInstance, &state, 0);

	addDolmexicaStoryHelper(id, state, tInstance);

	return 0;
}

static int handleRemoveHelperStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	CreateHelperStoryController* e = (CreateHelperStoryController*)tController->mData;

	int id;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);

	removeDolmexicaStoryHelper(id);

	return 0;
}

static void getStoryCharacterAndHelperFromAssignment(DreamMugenAssignment** tCharacterAssignment, StoryInstance* tInstance, int& oCharacter, int& oHelper) {
	string text;
	evaluateDreamAssignmentAndReturnAsString(text, tCharacterAssignment, (DreamPlayer*)tInstance);
	char characterString[100], comma[20], helperString[100];
	int items = sscanf(text.data(), "%s %s %s", characterString, comma, helperString);
	assert(items >= 1);

	if (items == 3) {
		oHelper = atoi(helperString);
	}
	else {
		oHelper = -1;	
	}

	oCharacter = getDolmexicaStoryIDFromString(characterString, tInstance);
}

static int handleLockTextToCharacterStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	LockTextToCharacterStoryController* e = (LockTextToCharacterStoryController*)tController->mData;

	int id;
	int character, helper;
	double dX, dY;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);
	getStoryCharacterAndHelperFromAssignment(&e->mCharacterID, tInstance, character, helper);
	getTwoFloatValuesWithDefaultValues(&e->mOffset, (DreamPlayer*)tInstance, &dX, &dY, 0, 0);

	if (helper == -1) {
		setDolmexicaStoryTextLockToCharacter(tInstance, id, character, makePosition(dX, dY, 0));
	}
	else {
		setDolmexicaStoryTextLockToCharacter(tInstance, id, character, makePosition(dX, dY, 0), helper);
	}

	return 0;
}

static int handleTextAddPositionStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	Target2DPhysicsController* e = (Target2DPhysicsController*)tController->mData;

	int id;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);

	if (e->mIsSettingX) {
		double x = evaluateDreamAssignmentAndReturnAsFloat(&e->x, (DreamPlayer*)tInstance);
		addDolmexicaStoryTextPositionX(tInstance, id, x);
	}

	if (e->mIsSettingY) {
		double y = evaluateDreamAssignmentAndReturnAsFloat(&e->y, (DreamPlayer*)tInstance);
		addDolmexicaStoryTextPositionY(tInstance, id, y);
	}
	return 0;
}

static int handleNameIDStoryController(DreamMugenStateController* tController, StoryInstance* tInstance) {
	NameTextStoryController* e = (NameTextStoryController*)tController->mData;

	int id;
	id = getDolmexicaStoryIDFromAssignment(&e->mID, tInstance);

	string name;
	evaluateDreamAssignmentAndReturnAsString(name, &e->mName, (DreamPlayer*)tInstance);
	setDolmexicaStoryIDName(tInstance, id, name);

	return 0;
}

typedef struct {
	StoryInstance* mPlayer;
	StoryInstance* mTarget;

} StoryVarSetHandlingCaller;

static void handleSettingSingleStoryVariable(void* tCaller, void* tData) {
	StoryVarSetHandlingCaller* caller = (StoryVarSetHandlingCaller*)tCaller;
	StoryVarSetControllerEntry* e = (StoryVarSetControllerEntry*)tData;

	int id = evaluateDreamAssignmentAndReturnAsInteger(&e->mID, (DreamPlayer*)caller->mPlayer);

	if (e->mType == STORY_VAR_SET_TYPE_INTEGER) {
		int val = evaluateDreamAssignmentAndReturnAsInteger(&e->mAssignment, (DreamPlayer*)caller->mPlayer);
		setDolmexicaStoryIntegerVariable(caller->mTarget, id, val);
	}
	else if (e->mType == STORY_VAR_SET_TYPE_FLOAT) {
		double val = evaluateDreamAssignmentAndReturnAsFloat(&e->mAssignment, (DreamPlayer*)caller->mPlayer);
		setDolmexicaStoryFloatVariable(caller->mTarget, id, val);
	}
	else if (e->mType == STORY_VAR_SET_TYPE_STRING) {
		string val;
		evaluateDreamAssignmentAndReturnAsString(val, &e->mAssignment, (DreamPlayer*)caller->mPlayer);
		setDolmexicaStoryStringVariable(caller->mTarget, id, val);
	}
	else {
		logWarningFormat("Unrecognized story variable type %d. Ignoring.", e->mType);
	}
}

static int handleSettingStoryVariable(DreamMugenStateController* tController, StoryInstance* tPlayer, StoryInstance* tTarget) {
	if (!tPlayer) return 0;

	StoryVarSetController* e = (StoryVarSetController*)tController->mData;
	StoryVarSetHandlingCaller caller;
	caller.mPlayer = tPlayer;
	caller.mTarget = tTarget;

	vector_map(&e->mStoryVarSets, handleSettingSingleStoryVariable, &caller);

	return 0;
}

static void handleSettingSingleGlobalStoryVariable(void* tCaller, void* tData) {
	StoryVarSetHandlingCaller* caller = (StoryVarSetHandlingCaller*)tCaller;
	StoryVarSetControllerEntry* e = (StoryVarSetControllerEntry*)tData;

	int id = evaluateDreamAssignmentAndReturnAsInteger(&e->mID, (DreamPlayer*)caller->mPlayer);

	if (e->mType == STORY_VAR_SET_TYPE_INTEGER) {
		int val = evaluateDreamAssignmentAndReturnAsInteger(&e->mAssignment, (DreamPlayer*)caller->mPlayer);
		setGlobalVariable(id, val);
	}
	else if (e->mType == STORY_VAR_SET_TYPE_FLOAT) {
		double val = evaluateDreamAssignmentAndReturnAsFloat(&e->mAssignment, (DreamPlayer*)caller->mPlayer);
		setGlobalFloatVariable(id, val);
	}
	else if (e->mType == STORY_VAR_SET_TYPE_STRING) {
		string val;
		evaluateDreamAssignmentAndReturnAsString(val, &e->mAssignment, (DreamPlayer*)caller->mPlayer);
		setGlobalStringVariable(id, val);
	}
	else {
		logWarningFormat("Unrecognized story variable type %d. Ignoring.", e->mType);
	}
}

static int handleSettingGlobalStoryVariable(DreamMugenStateController* tController, StoryInstance* tPlayer) {
	if (!tPlayer) return 0;

	StoryVarSetController* e = (StoryVarSetController*)tController->mData;
	StoryVarSetHandlingCaller caller;
	caller.mPlayer = tPlayer;
	caller.mTarget = NULL;

	vector_map(&e->mStoryVarSets, handleSettingSingleGlobalStoryVariable, &caller);

	return 0;
}

static void handleAddingSingleStoryVariable(void* tCaller, void* tData) {
	StoryVarSetHandlingCaller* caller = (StoryVarSetHandlingCaller*)tCaller;
	StoryVarSetControllerEntry* e = (StoryVarSetControllerEntry*)tData;

	int id = evaluateDreamAssignmentAndReturnAsInteger(&e->mID, (DreamPlayer*)caller->mPlayer);

	if (e->mType == STORY_VAR_SET_TYPE_INTEGER) {
		int val = evaluateDreamAssignmentAndReturnAsInteger(&e->mAssignment, (DreamPlayer*)caller->mPlayer);
		addDolmexicaStoryIntegerVariable(caller->mTarget, id, val);
	}
	else if (e->mType == STORY_VAR_SET_TYPE_FLOAT) {
		double val = evaluateDreamAssignmentAndReturnAsFloat(&e->mAssignment, (DreamPlayer*)caller->mPlayer);
		addDolmexicaStoryFloatVariable(caller->mTarget, id, val);
	}
	else if (e->mType == STORY_VAR_SET_TYPE_STRING) {
		string val;
		evaluateDreamAssignmentAndReturnAsString(val, &e->mAssignment, (DreamPlayer*)caller->mPlayer);
		if (val[0] == '$') {
			int numVal = atoi(val.data() + 1);
			addDolmexicaStoryStringVariable(caller->mTarget, id, numVal);
		}
		else {
			addDolmexicaStoryStringVariable(caller->mTarget, id, val);
		}
	}
	else {
		logWarningFormat("Unrecognized story variable type %d. Ignoring.", e->mType);
	}
}

static int handleAddingStoryVariable(DreamMugenStateController* tController, StoryInstance* tPlayer, StoryInstance* tTarget) {
	if (!tPlayer) return 0;

	StoryVarSetController* e = (StoryVarSetController*)tController->mData;
	StoryVarSetHandlingCaller caller;
	caller.mPlayer = tPlayer;
	caller.mTarget = tTarget;

	vector_map(&e->mStoryVarSets, handleAddingSingleStoryVariable, &caller);

	return 0;
}

static void handleAddingSingleGlobalStoryVariable(void* tCaller, void* tData) {
	StoryVarSetHandlingCaller* caller = (StoryVarSetHandlingCaller*)tCaller;
	StoryVarSetControllerEntry* e = (StoryVarSetControllerEntry*)tData;

	int id = evaluateDreamAssignmentAndReturnAsInteger(&e->mID, (DreamPlayer*)caller->mPlayer);

	if (e->mType == STORY_VAR_SET_TYPE_INTEGER) {
		int val = evaluateDreamAssignmentAndReturnAsInteger(&e->mAssignment, (DreamPlayer*)caller->mPlayer);
		addGlobalVariable(id, val);
	}
	else if (e->mType == STORY_VAR_SET_TYPE_FLOAT) {
		double val = evaluateDreamAssignmentAndReturnAsFloat(&e->mAssignment, (DreamPlayer*)caller->mPlayer);
		addGlobalFloatVariable(id, val);
	}
	else if (e->mType == STORY_VAR_SET_TYPE_STRING) {
		string val;
		evaluateDreamAssignmentAndReturnAsString(val, &e->mAssignment, (DreamPlayer*)caller->mPlayer);
		if (val[0] == '$') {
			int numVal = atoi(val.data() + 1);
			addGlobalStringVariable(id, numVal);
		}
		else {
			addGlobalStringVariable(id, val);
		}
	}
	else {
		logWarningFormat("Unrecognized story variable type %d. Ignoring.", e->mType);
	}
}

static int handleAddingGlobalStoryVariable(DreamMugenStateController* tController, StoryInstance* tPlayer) {
	if (!tPlayer) return 0;

	StoryVarSetController* e = (StoryVarSetController*)tController->mData;
	StoryVarSetHandlingCaller caller;
	caller.mPlayer = tPlayer;
	caller.mTarget = NULL;

	vector_map(&e->mStoryVarSets, handleAddingSingleGlobalStoryVariable, &caller);

	return 0;
}

static int handlePlayMusicStoryController(DreamMugenStateController* tController, StoryInstance* tPlayer) {
	if (!tPlayer) return 0;

	StoryPlayMusicController* e = (StoryPlayMusicController*)tController->mData;
	string path;
	evaluateDreamAssignmentAndReturnAsString(path, &e->mPath, (DreamPlayer*)tPlayer);
	playDolmexicaStoryMusic(path);
	return 0;
}

static int handleStopMusicStoryController(StoryInstance* tPlayer) {
	if (!tPlayer) return 0;
	stopDolmexicaStoryMusic();
	return 0;
}

static int handlePauseMusicStoryController(StoryInstance* tPlayer) {
	if (!tPlayer) return 0;
	pauseDolmexicaStoryMusic();
	return 0;
}

static int handleResumeMusicStoryController(StoryInstance* tPlayer) {
	if (!tPlayer) return 0;
	resumeDolmexicaStoryMusic();
	return 0;
}

static int handleDestroySelfStoryController(StoryInstance* tPlayer) {
	if (!tPlayer) return 0;
	destroyDolmexicaStoryHelper(tPlayer);
	return 0;
}

static int handleCameraFocusStoryController(DreamMugenStateController* tController, StoryInstance* tPlayer) {
	if (!tPlayer) return 0;
	Set2DPhysicsController* e = (Set2DPhysicsController*)tController->mData;
	
	if (e->mIsSettingX) {
		double x = evaluateDreamAssignmentAndReturnAsFloat(&e->x, (DreamPlayer*)tPlayer);
		setDolmexicaStoryCameraFocusX(x);
	}

	if (e->mIsSettingY) {
		double y = evaluateDreamAssignmentAndReturnAsFloat(&e->y, (DreamPlayer*)tPlayer);
		setDolmexicaStoryCameraFocusY(y);
	}
	return 0;
}

static int handleCameraZoomStoryController(DreamMugenStateController* tController, StoryInstance* tPlayer) {
	if (!tPlayer) return 0;
	SingleRequiredValueController* e = (SingleRequiredValueController*)tController->mData;
	double zoom;
	getSingleFloatValueOrDefault(&e->mValue, (DreamPlayer*)tPlayer, &zoom, 1.0);
	setDolmexicaStoryCameraZoom(zoom);

	return 0;
}

int nullStoryHandleFunction(DreamMugenStateController* /*tController*/, DreamPlayer* /*tPlayer*/) { return handleNull(); }
int createAnimationStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleCreateAnimationStoryController(tController, (StoryInstance*)tPlayer); }
int removeAnimationStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleRemoveAnimationStoryController(tController, (StoryInstance*)tPlayer); }
int changeAnimationStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleChangeAnimationStoryController(tController, (StoryInstance*)tPlayer); }
int createTextStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleCreateTextStoryController(tController, (StoryInstance*)tPlayer); }
int removeTextStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleRemoveTextStoryController(tController, (StoryInstance*)tPlayer); }
int changeTextStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleChangeTextStoryController(tController, (StoryInstance*)tPlayer); }
int lockTextToCharacterStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleLockTextToCharacterStoryController(tController, (StoryInstance*)tPlayer); }
int textAddPositionStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleTextAddPositionStoryController(tController, (StoryInstance*)tPlayer); }
int nameIDStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleNameIDStoryController(tController, (StoryInstance*)tPlayer); }
int changeStateStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleChangeStateStoryController(tController, (StoryInstance*)tPlayer); }
int changeStateRootStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleChangeStateRootStoryController(tController, (StoryInstance*)tPlayer); }
int fadeInStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleFadeInStoryController(tController, (StoryInstance*)tPlayer); }
int fadeOutStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleFadeOutStoryController(tController, (StoryInstance*)tPlayer); }
int gotoStoryStepStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleGotoStoryStepStoryController(tController, (StoryInstance*)tPlayer); }
int gotoIntroStoryHandleFunction(DreamMugenStateController* /*tController*/, DreamPlayer* /*tPlayer*/) { return handleGotoIntroStoryController(); }
int gotoTitleStoryHandleFunction(DreamMugenStateController* /*tController*/, DreamPlayer* /*tPlayer*/) { return handleGotoTitleStoryController(); }
int animationSetPositionStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAnimationSetPositionStoryController(tController, (StoryInstance*)tPlayer); }
int animationAddPositionStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAnimationAddPositionStoryController(tController, (StoryInstance*)tPlayer); }
int animationSetScaleStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAnimationSetScaleStoryController(tController, (StoryInstance*)tPlayer); }
int animationSetFaceDirectionStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAnimationSetFaceDirectionStoryController(tController, (StoryInstance*)tPlayer); }
int animationSetAngleStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAnimationSetAngleStoryController(tController, (StoryInstance*)tPlayer); }
int animationAddAngleStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAnimationAddAngleStoryController(tController, (StoryInstance*)tPlayer); }
int animationSetColorStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAnimationSetColorStoryController(tController, (StoryInstance*)tPlayer); }
int animationSetOpacityStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAnimationSetOpacityStoryController(tController, (StoryInstance*)tPlayer); }
int endStoryboardStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleEndStoryboardController(tController, (StoryInstance*)tPlayer); }
int moveStageStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleMoveStageStoryController(tController, (StoryInstance*)tPlayer); }
int createCharacterStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleCreateCharacterStoryController(tController, (StoryInstance*)tPlayer); }
int removeCharacterStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleRemoveCharacterStoryController(tController, (StoryInstance*)tPlayer); }
int changeCharacterAnimStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleChangeCharacterAnimStoryController(tController, (StoryInstance*)tPlayer); }
int setCharacterPosStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleSetCharacterPosStoryController(tController, (StoryInstance*)tPlayer); }
int addCharacterPosStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAddCharacterPosStoryController(tController, (StoryInstance*)tPlayer); }
int setCharacterScaleStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleSetCharacterScaleStoryController(tController, (StoryInstance*)tPlayer); }
int setCharacterFaceDirectionStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleSetCharacterFaceDirectionStoryController(tController, (StoryInstance*)tPlayer); }
int setCharacterColorStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleSetCharacterColorStoryController(tController, (StoryInstance*)tPlayer); }
int setCharacterOpacityStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleSetCharacterOpacityStoryController(tController, (StoryInstance*)tPlayer); }
int setCharacterAngleStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleSetCharacterAngleStoryController(tController, (StoryInstance*)tPlayer); }
int addCharacterAngleStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAddCharacterAngleStoryController(tController, (StoryInstance*)tPlayer); }
int createHelperStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleCreateHelperStoryController(tController, (StoryInstance*)tPlayer); }
int removeHelperStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleRemoveHelperStoryController(tController, (StoryInstance*)tPlayer); }
int setVarStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleSettingStoryVariable(tController, (StoryInstance*)tPlayer, (StoryInstance*)tPlayer); }
int addVarStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAddingStoryVariable(tController, (StoryInstance*)tPlayer, (StoryInstance*)tPlayer); }
int setGlobalVarStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleSettingGlobalStoryVariable(tController, (StoryInstance*)tPlayer); }
int addGlobalVarStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleAddingGlobalStoryVariable(tController, (StoryInstance*)tPlayer); }
int playMusicStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handlePlayMusicStoryController(tController, (StoryInstance*)tPlayer); }
int stopMusicStoryHandleFunction(DreamMugenStateController* /*tController*/, DreamPlayer* tPlayer) { return handleStopMusicStoryController((StoryInstance*)tPlayer); }
int pauseMusicStoryHandleFunction(DreamMugenStateController* /*tController*/, DreamPlayer* tPlayer) { return handlePauseMusicStoryController((StoryInstance*)tPlayer); }
int resumeMusicStoryHandleFunction(DreamMugenStateController* /*tController*/, DreamPlayer* tPlayer) { return handleResumeMusicStoryController((StoryInstance*)tPlayer); }
int destroySelfStoryHandleFunction(DreamMugenStateController* /*tController*/, DreamPlayer* tPlayer) { return handleDestroySelfStoryController((StoryInstance*)tPlayer); }
int cameraFocusStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleCameraFocusStoryController(tController, (StoryInstance*)tPlayer); }
int cameraZoomStoryHandleFunction(DreamMugenStateController* tController, DreamPlayer* tPlayer) { return handleCameraZoomStoryController(tController, (StoryInstance*)tPlayer); }

static void setupStoryStateControllerHandlers() {
	gMugenStateControllerVariableHandler.mStateControllerHandlers.clear();
	
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_NULL] = nullStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_CREATE_ANIMATION] = createAnimationStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_REMOVE_ANIMATION] = removeAnimationStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_CHANGE_ANIMATION] = changeAnimationStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_CREATE_TEXT] = createTextStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_REMOVE_TEXT] = removeTextStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_CHANGE_TEXT] = changeTextStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_LOCK_TEXT_TO_CHARACTER] = lockTextToCharacterStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_TEXT_ADD_POSITION] = textAddPositionStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_NAME_ID] = nameIDStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_CHANGE_STATE] = changeStateStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_CHANGE_STATE_ROOT] = changeStateRootStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_FADE_IN] = fadeInStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_FADE_OUT] = fadeOutStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_GOTO_STORY_STEP] = gotoStoryStepStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_GOTO_INTRO] = gotoIntroStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_GOTO_TITLE] = gotoTitleStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_ANIMATION_SET_POSITION] = animationSetPositionStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_ANIMATION_ADD_POSITION] = animationAddPositionStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_ANIMATION_SET_SCALE] = animationSetScaleStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_ANIMATION_SET_FACEDIRECTION] = animationSetFaceDirectionStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_ANIMATION_SET_ANGLE] = animationSetAngleStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_ANIMATION_ADD_ANGLE] = animationAddAngleStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_ANIMATION_SET_COLOR] = animationSetColorStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_ANIMATION_SET_OPACITY] = animationSetOpacityStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_END_STORYBOARD] = endStoryboardStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_MOVE_STAGE] = moveStageStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_CREATE_CHARACTER] = createCharacterStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_REMOVE_CHARACTER] = removeCharacterStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_CHARACTER_CHANGE_ANIMATION] = changeCharacterAnimStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_CHARACTER_SET_POSITION] = setCharacterPosStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_CHARACTER_ADD_POSITION] = addCharacterPosStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_CHARACTER_SET_SCALE] = setCharacterScaleStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_CHARACTER_SET_FACEDIRECTION] = setCharacterFaceDirectionStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_CHARACTER_SET_COLOR] = setCharacterColorStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_CHARACTER_SET_OPACITY] = setCharacterOpacityStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_CHARACTER_SET_ANGLE] = setCharacterAngleStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_CHARACTER_ADD_ANGLE] = addCharacterAngleStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_CREATE_HELPER] = createHelperStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_REMOVE_HELPER] = removeHelperStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_VAR_SET] = setVarStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_VAR_ADD] = addVarStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_GLOBAL_VAR_SET] = setGlobalVarStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_GLOBAL_VAR_ADD] = addGlobalVarStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_PLAY_MUSIC] = playMusicStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_STOP_MUSIC] = stopMusicStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_PAUSE_MUSIC] = pauseMusicStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_RESUME_MUSIC] = resumeMusicStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_DESTROY_SELF] = destroySelfStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_CAMERA_FOCUS] = cameraFocusStoryHandleFunction;
	gMugenStateControllerVariableHandler.mStateControllerHandlers[MUGEN_STORY_STATE_CONTROLLER_TYPE_CAMERA_ZOOM] = cameraZoomStoryHandleFunction;

	
}

void setupDreamMugenStoryStateControllerHandler()
{
	setupStoryStateControllerParsers();
	setupStoryStateControllerHandlers();
}

void shutdownDreamMugenStateControllerHandler()
{
	gMugenStateControllerVariableHandler.mStateControllerParsers.clear();
	gMugenStateControllerVariableHandler.mStateControllerHandlers.clear();
	gMugenStateControllerVariableHandler.mStateControllerUnloaders.clear();
	gMugenStateControllerVariableHandler.mMemoryStack = NULL;
}
