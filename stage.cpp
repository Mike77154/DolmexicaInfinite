#include "stage.h"

#include <stdio.h>
#include <assert.h>
#include <algorithm>

#include <prism/memoryhandler.h>
#include <prism/log.h>
#include <prism/system.h>
#include <prism/animation.h>
#include <prism/stagehandler.h>
#include <prism/math.h>
#include <prism/input.h>
#include <prism/mugendefreader.h>
#include <prism/mugenspritefilereader.h>
#include <prism/sound.h>

#include "playerdefinition.h"
#include "mugenstagehandler.h"
#include "mugenbackgroundstatehandler.h"
#include "mugensound.h"

using namespace std;

typedef struct {
	char mName[1024];
	char mDisplayName[1024];
	char mVersionDate[100];
	char mMugenVersion[100];
	char mAuthor[100];
} StageInfo;

typedef struct {
	Position mStartPosition;
	double mBoundLeft;
	double mBoundRight;
	double mBoundHigh;
	double mBoundLow;

	double mTension;
	double mTensionHigh;
	double mTensionLow;

	double mVerticalFollow;
	double mFloorTension;

	int mOverdrawHigh;
	int mOverdrawLow;
	int mCutHigh;
	int mCutLow;

	double mStartZoom;
	double mZoomOut;
	double mZoomIn;

} StageCamera;

typedef struct {
	Position mP1Start;
	Position mP2Start;

	int mP1Facing;
	int mP2Facing;

	double mLeftBound;
	double mRightBound;

} StagePlayerInfo;

typedef struct {
	double mScreenLeft;
	double mScreenRight;
} StageBound;

typedef struct {
	double mZOffset;
	int mZOffsetLink;
	int mAutoturn;
	int mResetBG;
	Vector3DI mLocalCoordinates;
	Vector3D mScale;

} StageStageInfo;

typedef struct {
	int mIntensity;
	Vector3DI mColor;
	double mScaleY;
	Vector3D mFadeRange;
	double mXShear;

} StageShadow;

typedef struct {
	int mReflect;
	int mIntensity;
} StageReflection;

typedef struct {
	char mBGMusic[1024];
	int mBGVolume;
} StageMusic;

typedef struct {
	char mSpritePath[1024];
	int mDebugBG;

} StageBackgroundDefinition;

typedef enum {
	STAGE_BACKGROUND_STATIC,
	STAGE_BACKGROUND_ANIMATED,
} StageBackgroundType;

typedef struct {
	StageBackgroundType mType;
	Vector3DI mSpriteNo;
	int mLayerNo;
	Position mStart;
	Position mDelta;
	int mMask;
	Vector3DI mTile;
	Vector3DI mTileSpacing;
	int mListPosition;
	BlendType mBlendType;

	Velocity mVelocity;

	double mStartScaleY;
	double mScaleDeltaY;

	int mActionNumber;
	int mID;
} StageBackgroundElement;

static struct {
	StageInfo mInfo;
	StageCamera mCamera;
	StagePlayerInfo mPlayerInfo;
	StageBound mBound;
	StageStageInfo mStageInfo;
	StageShadow mShadow;
	StageReflection mReflection;
	StageMusic mMusic;
	StageBackgroundDefinition mBackgroundDefinition;

	List mBackgroundElements;

	MugenSpriteFile mSprites;
	MugenAnimations mAnimations;

	int mIsCameraManual;
	char mDefinitionPath[1024];

	int mHasCustomMusicPath;
	char mCustomMusicPath[1024];
} gData;

static void loadStageInfo(MugenDefScript* s) {
	getMugenDefStringOrDefault(gData.mInfo.mName, s, "Info", "name", "");
	getMugenDefStringOrDefault(gData.mInfo.mDisplayName, s, "Info", "displayname", gData.mInfo.mName);
	getMugenDefStringOrDefault(gData.mInfo.mVersionDate, s, "Info", "versiondate", "1.0");
	getMugenDefStringOrDefault(gData.mInfo.mMugenVersion, s, "Info", "mugenversion", "1.0");
	getMugenDefStringOrDefault(gData.mInfo.mAuthor, s, "Info", "author", "John Doe");
}

static void loadStageCamera(MugenDefScript* s) {
	gData.mCamera.mStartPosition = makePosition(0, 0, 0);
	gData.mCamera.mStartPosition.x = getMugenDefFloatOrDefault(s, "Camera", "startx", 0);
	gData.mCamera.mStartPosition.y = getMugenDefFloatOrDefault(s, "Camera", "starty", 0);
	gData.mCamera.mBoundLeft = getMugenDefFloatOrDefault(s, "Camera", "boundleft", 0);
	gData.mCamera.mBoundRight = getMugenDefFloatOrDefault(s, "Camera", "boundright", 0);
	gData.mCamera.mBoundHigh = getMugenDefFloatOrDefault(s, "Camera", "boundhigh", 0);
	gData.mCamera.mBoundLow = getMugenDefFloatOrDefault(s, "Camera", "boundlow", 0);
	gData.mCamera.mTension = getMugenDefFloatOrDefault(s, "Camera", "tension", 0);
	gData.mCamera.mTensionHigh = getMugenDefFloatOrDefault(s, "Camera", "tensionhigh", 0);
	gData.mCamera.mTensionLow = getMugenDefFloatOrDefault(s, "Camera", "tensionlow", 0);
	gData.mCamera.mVerticalFollow = getMugenDefFloatOrDefault(s, "Camera", "verticalfollow", 0);
	gData.mCamera.mFloorTension = getMugenDefFloatOrDefault(s, "Camera", "floortension", 0);
	gData.mCamera.mOverdrawHigh = getMugenDefIntegerOrDefault(s, "Camera", "overdrawhigh", 0);
	gData.mCamera.mOverdrawLow = getMugenDefIntegerOrDefault(s, "Camera", "overdrawlow", 0);
	gData.mCamera.mCutHigh = getMugenDefIntegerOrDefault(s, "Camera", "cuthigh", 0);
	gData.mCamera.mCutLow = getMugenDefIntegerOrDefault(s, "Camera", "cutlow", 0);
	gData.mCamera.mStartZoom = getMugenDefFloatOrDefault(s, "Camera", "startzoom", 1);
	gData.mCamera.mZoomOut = getMugenDefFloatOrDefault(s, "Camera", "zoomout", 1);
	gData.mCamera.mZoomIn = getMugenDefFloatOrDefault(s, "Camera", "zoomin", 1);
}

static void loadStagePlayerInfo(MugenDefScript* s) {

	gData.mPlayerInfo.mP1Start = makePosition(0, 0, 0);
	gData.mPlayerInfo.mP1Start.x = getMugenDefFloatOrDefault(s, "PlayerInfo", "p1startx", 0);
	gData.mPlayerInfo.mP1Start.y = getMugenDefFloatOrDefault(s, "PlayerInfo", "p1starty", 0);

	gData.mPlayerInfo.mP2Start = makePosition(0, 0, 0);
	gData.mPlayerInfo.mP2Start.x = getMugenDefFloatOrDefault(s, "PlayerInfo", "p2startx", 0);
	gData.mPlayerInfo.mP2Start.y = getMugenDefFloatOrDefault(s, "PlayerInfo", "p2starty", 0);

	gData.mPlayerInfo.mP1Facing = getMugenDefIntegerOrDefault(s, "PlayerInfo", "p1facing", 0);
	gData.mPlayerInfo.mP2Facing = getMugenDefIntegerOrDefault(s, "PlayerInfo", "p2facing", 0);

	gData.mPlayerInfo.mLeftBound = getMugenDefFloatOrDefault(s, "PlayerInfo", "leftbound", 0);
	gData.mPlayerInfo.mRightBound = getMugenDefFloatOrDefault(s, "PlayerInfo", "rightbound", 0);
}

static void loadStageBound(MugenDefScript* s) {
	gData.mBound.mScreenLeft = getMugenDefFloatOrDefault(s, "Bound", "screenleft", 0);
	gData.mBound.mScreenRight = getMugenDefFloatOrDefault(s, "Bound", "screenright", 0);
}

static void loadStageStageInfo(MugenDefScript* s) {
	gData.mStageInfo.mZOffset = getMugenDefFloatOrDefault(s, "StageInfo", "zoffset", 0);
	gData.mStageInfo.mZOffsetLink = getMugenDefIntegerOrDefault(s, "StageInfo", "zoffsetlink", 0);
	gData.mStageInfo.mAutoturn = getMugenDefIntegerOrDefault(s, "StageInfo", "autoturn", 1);
	gData.mStageInfo.mResetBG = getMugenDefIntegerOrDefault(s, "StageInfo", "resetBG", 0);
	gData.mStageInfo.mLocalCoordinates = getMugenDefVectorIOrDefault(s, "StageInfo", "localcoord", makeVector3DI(320, 240, 0));

	gData.mStageInfo.mScale = makePosition(1, 1, 1);
	gData.mStageInfo.mScale.x = getMugenDefFloatOrDefault(s, "StageInfo", "xscale", 1);
	gData.mStageInfo.mScale.y = getMugenDefFloatOrDefault(s, "StageInfo", "yscale", 1);
}

static void loadStageShadow(MugenDefScript* s) {
	gData.mShadow.mIntensity = getMugenDefIntegerOrDefault(s, "Shadow", "intensity", 128);
	gData.mShadow.mColor = getMugenDefVectorIOrDefault(s, "Shadow", "color", makeVector3DI(0, 0, 0));
	gData.mShadow.mScaleY = getMugenDefFloatOrDefault(s, "Shadow", "yscale", 1);
	gData.mShadow.mFadeRange = getMugenDefVectorOrDefault(s, "Shadow", "fade.range", makePosition(-1001, -1000, 0));
	if (gData.mShadow.mFadeRange.x >= gData.mShadow.mFadeRange.y) {
		gData.mShadow.mFadeRange = makePosition(-1001, -1000, 0);
	}
	gData.mShadow.mXShear = getMugenDefFloatOrDefault(s, "Shadow", "xshear", 0);
}

static void loadStageReflection(MugenDefScript* s) {
	gData.mReflection.mReflect = getMugenDefIntegerOrDefault(s, "Reflection", "reflect", 0);
	gData.mReflection.mIntensity = getMugenDefIntegerOrDefault(s, "Reflection", "intensity", 0);
}

static void loadStageMusic(MugenDefScript* s) {
	getMugenDefStringOrDefault(gData.mMusic.mBGMusic, s, "Music", "bgmusic", "");
	gData.mMusic.mBGVolume = getMugenDefIntegerOrDefault(s, "Music", "bgvolume", 0);
}

static MugenDefScriptGroup* loadStageBackgroundDefinitionAndReturnGroup(MugenDefScript* s) {
	MugenDefScriptGroup* bgdef;
	char name[100];
	if (stl_string_map_contains_array(s->mGroups, "BGDef")) {
		strcpy(name, "BGDef");
	} else if(stl_string_map_contains_array(s->mGroups, "BGdef")) {
		strcpy(name, "BGdef");
	}
	else {
		strcpy(name, "");
		logWarning("No background definition found. Defaulting to 0.");
		return NULL;
	}

	bgdef = &s->mGroups[name];

	getMugenDefStringOrDefault(gData.mBackgroundDefinition.mSpritePath, s, name, "spr", "");
	gData.mBackgroundDefinition.mDebugBG = getMugenDefIntegerOrDefault(s, name, "debugbg", 0);

	return bgdef;
}

static int isBackgroundElementGroup(MugenDefScriptGroup* tGroup) {
	return tGroup->mName[0] == 'B' && tGroup->mName[1] == 'G' && tGroup->mName[2] != 'C'; // TODO
}

static int isActionGroup(MugenDefScriptGroup* tGroup) {
	char firstW[100], secondW[100];
	int items = sscanf(tGroup->mName.data(), "%s %s", firstW, secondW);
	if (items < 2) return 0;

	turnStringLowercase(firstW);
	turnStringLowercase(secondW);
	return !strcmp("begin", firstW) && !strcmp("action", secondW);
}

static void addBackgroundElementToStageHandler(StageBackgroundElement* e, MugenAnimation* tAnimation, int tOwnsAnimation) {
	e->mStart.z = e->mListPosition*0.01 + e->mLayerNo * BACKGROUND_UPPER_BASE_Z;
	addDreamMugenStageHandlerAnimatedBackgroundElement(e->mStart, tAnimation, tOwnsAnimation, &gData.mSprites, e->mDelta, e->mTile, e->mTileSpacing, e->mBlendType, makeGeoRectangle(-INF / 2, -INF / 2, INF, INF), e->mVelocity, e->mStartScaleY, e->mScaleDeltaY, e->mLayerNo, e->mID, gData.mStageInfo.mLocalCoordinates);
}

static BlendType getBackgroundBlendType(MugenDefScriptGroup* tGroup) {
	if (!isMugenDefStringVariableAsGroup(tGroup, "trans")) return BLEND_TYPE_NORMAL;

	BlendType ret;
	char* text = getAllocatedMugenDefStringVariableAsGroup(tGroup, "trans");
	turnStringLowercase(text);

	if (!strcmp("add", text) || !strcmp("addalpha", text)) {
		ret = BLEND_TYPE_ADDITION;
	}
	else if (!strcmp("sub", text)) {
		ret = BLEND_TYPE_SUBTRACTION;
	}
	else if (!strcmp("none", text)) {
		ret = BLEND_TYPE_NORMAL;
	}
	else {
		ret = BLEND_TYPE_NORMAL;
		logWarningFormat("Unknown transparency type %s. Defaulting to normal blending.", text);
	}

	freeMemory(text);

	return ret;
}

static void loadBackgroundElement(MugenDefScriptGroup* tGroup, int i) {
	StageBackgroundElement* e = (StageBackgroundElement*)allocMemory(sizeof(StageBackgroundElement));

	debugLog("Load background element.");
 	debugString(tName);

	char* type = getAllocatedMugenDefStringOrDefaultAsGroup(tGroup, "type", "unknown");
	turnStringLowercase(type);

	e->mSpriteNo = getMugenDefVectorIOrDefaultAsGroup(tGroup, "spriteno", makeVector3DI(0, 0, 0));
	e->mLayerNo = getMugenDefIntegerOrDefaultAsGroup(tGroup, "layerno", 0);
	e->mStart = getMugenDefVectorOrDefaultAsGroup(tGroup, "start", makePosition(0, 0, 0));
	e->mDelta = getMugenDefVectorOrDefaultAsGroup(tGroup, "delta", makePosition(1, 1, 1));
	e->mMask = getMugenDefIntegerOrDefaultAsGroup(tGroup, "mask", 0);
	e->mTile = getMugenDefVectorIOrDefaultAsGroup(tGroup, "tile", makeVector3DI(0, 0, 0));
	e->mTileSpacing = getMugenDefVectorIOrDefaultAsGroup(tGroup, "tilespacing", makeVector3DI(0, 0, 0));
	e->mVelocity = getMugenDefVectorOrDefaultAsGroup(tGroup, "velocity", makePosition(0, 0, 0));
	
	e->mBlendType = getBackgroundBlendType(tGroup);
	e->mListPosition = i;

	e->mStartScaleY = getMugenDefFloatOrDefaultAsGroup(tGroup, "yscalestart", 100) / 100.0;
	e->mScaleDeltaY = getMugenDefFloatOrDefaultAsGroup(tGroup, "yscaledelta", 0) / 100.0;

	e->mID = getMugenDefIntegerOrDefaultAsGroup(tGroup, "id", -1);

	if (!strcmp("normal", type) || !strcmp("parallax", type)) { // TODO: parallax
		e->mType = STAGE_BACKGROUND_STATIC;
		addBackgroundElementToStageHandler(e, createOneFrameMugenAnimationForSprite(e->mSpriteNo.x, e->mSpriteNo.y), 1);
	}
	else if (!strcmp("anim", type)) {
		e->mType = STAGE_BACKGROUND_ANIMATED;
		e->mActionNumber = getMugenDefIntegerOrDefaultAsGroup(tGroup, "actionno", -1);
		addBackgroundElementToStageHandler(e, getMugenAnimation(&gData.mAnimations, e->mActionNumber), 0);
	}
	else {
		logWarningFormat("Unknown type %s. Ignore.", type);
		freeMemory(e);
		freeMemory(type);
		return;
	}

	freeMemory(type);
	list_push_back_owned(&gData.mBackgroundElements, e);
}

static void loadBackgroundDefinitionGroup(MugenDefScript* s, MugenDefScriptGroup* tGroup, int i) {
	if (isBackgroundElementGroup(tGroup)) {
		loadBackgroundElement(tGroup, i);
	}
	else if (isActionGroup(tGroup)) {
		// TODO: ignore properly
	}
	else {
		//logWarningFormat("Unknown background definition group %s. Ignoring.", tGroup->mName); // TODO
	}
}

static void loadStageTextures(char* tPath) {
	char path[1024];
	getPathToFile(path, tPath);
	char sffFile[1024];
	sprintf(sffFile, "%s%s", path, gData.mBackgroundDefinition.mSpritePath);
	if (!isFile(sffFile)) {
		sprintf(sffFile, "assets/%s", gData.mBackgroundDefinition.mSpritePath);
	}

	setMugenSpriteFileReaderToUsePalette(2);
	gData.mSprites = loadMugenSpriteFileWithoutPalette(sffFile);
	setMugenSpriteFileReaderToNotUsePalette();
}

static void loadStageBackgroundElements(char* tPath, MugenDefScript* s) {
	MugenDefScriptGroup* bgdef = loadStageBackgroundDefinitionAndReturnGroup(s);
	if (!bgdef) return;

	loadStageTextures(tPath);

	gData.mBackgroundElements = new_list();
	int i = 0;
	MugenDefScriptGroup* cur = bgdef->mNext;
	while (cur != NULL) {
		loadBackgroundDefinitionGroup(s, cur, i);
		i++;
		cur = cur->mNext;
	}

}

static void setStageCamera() {
	double sizeX = gData.mCamera.mBoundRight - gData.mCamera.mBoundLeft;
	double sizeY = gData.mCamera.mBoundLow - gData.mCamera.mBoundHigh;
	setDreamMugenStageHandlerCameraRange(makeGeoRectangle(gData.mCamera.mBoundLeft, gData.mCamera.mBoundHigh, sizeX, sizeY));
}

static void loadStage(void* tData)
{
	(void)tData;
	instantiateActor(getDreamMugenStageHandler());
	instantiateActor(getBackgroundStateHandler());

	gData.mAnimations = loadMugenAnimationFile(gData.mDefinitionPath);

	MugenDefScript s; 
	loadMugenDefScript(&s, gData.mDefinitionPath);

	loadStageInfo(&s);
	loadStageCamera(&s);
	loadStagePlayerInfo(&s);
	loadStageBound(&s);
	loadStageStageInfo(&s);
	loadStageShadow(&s);
	loadStageReflection(&s);
	loadStageMusic(&s);
	setDreamMugenStageHandlerCameraCoordinates(makeVector3DI(320, 240, 0)); // TODO

	setStageCamera();
	loadStageBackgroundElements(gData.mDefinitionPath, &s);
	gData.mIsCameraManual = 0;

	setBackgroundStatesFromScript(&s);

	unloadMugenDefScript(s);
}

static void unloadStage(void* tData)
{
	(void)tData;

	unloadMugenSpriteFile(&gData.mSprites);
	unloadMugenAnimationFile(&gData.mAnimations);
	delete_list(&gData.mBackgroundElements);
}

static void updateCameraMovementX() {
	double x1 = getPlayerPositionX(getRootPlayer(0), gData.mStageInfo.mLocalCoordinates.y);
	double x2 = getPlayerPositionX(getRootPlayer(1), gData.mStageInfo.mLocalCoordinates.y);
	double minX = min(x1, x2);
	double maxX = max(x1, x2);
	minX -= gData.mStageInfo.mLocalCoordinates.x / 2;
	maxX -= gData.mStageInfo.mLocalCoordinates.x / 2;

	double right = getDreamCameraPositionX(gData.mStageInfo.mLocalCoordinates.y) + gData.mStageInfo.mLocalCoordinates.x / 2;
	double left = getDreamCameraPositionX(gData.mStageInfo.mLocalCoordinates.y) - gData.mStageInfo.mLocalCoordinates.x / 2;

	double lx = (left + gData.mCamera.mTension) - minX;
	double rx = maxX - (right - gData.mCamera.mTension);

	if (lx > 0 && rx > 0) {
		// TODO
	}
	else if (rx > 0) {
		double delta = min(rx, -lx);
		addDreamMugenStageHandlerCameraPositionX(delta);
	}
	else if (lx > 0) {
		double delta = min(lx, -rx);
		addDreamMugenStageHandlerCameraPositionX(-delta);
	}
}

static void updateCameraMovementY() {
	double y1 = getPlayerPositionY(getRootPlayer(0), gData.mStageInfo.mLocalCoordinates.y);
	double y2 = getPlayerPositionY(getRootPlayer(1), gData.mStageInfo.mLocalCoordinates.y);
	double mini = min(y1, y2);

	double cameraY = mini*gData.mCamera.mVerticalFollow;
	setDreamMugenStageHandlerCameraPositionY(cameraY);
}

static void updateCameraMovement() {
	if (gData.mIsCameraManual) return;

	updateCameraMovementX();
	updateCameraMovementY();
}

static void updateStage(void* tData) {
	(void)tData;
	updateCameraMovement();
}

ActorBlueprint getDreamStageBP() {
	return makeActorBlueprint(loadStage, unloadStage, updateStage);
}


void setDreamStageMugenDefinition(char * tPath, char* tCustomMusicPath)
{
	strcpy(gData.mDefinitionPath, tPath);
	strcpy(gData.mCustomMusicPath, tCustomMusicPath);
}

MugenAnimations * getStageAnimations()
{
	return &gData.mAnimations;
}

void playDreamStageMusic()
{
	if (isMugenBGMMusicPath(gData.mCustomMusicPath)) {
		playMugenBGMMusicPath(gData.mCustomMusicPath, 1);
	}
	else if (isMugenBGMMusicPath(gData.mMusic.mBGMusic)) {
		playMugenBGMMusicPath(gData.mMusic.mBGMusic, 1);
	}
}

double parseDreamCoordinatesToLocalCoordinateSystem(double tCoordinate, int tOtherCoordinateSystemAsP)
{
	ScreenSize sz = getScreenSize();
	int currentP = sz.y; 
	double fac = currentP / (double)tOtherCoordinateSystemAsP;

	return tCoordinate*fac;
}

Position getDreamPlayerStartingPosition(int i, int tCoordinateP)
{
	Position ret;
	if (!i) ret = gData.mPlayerInfo.mP1Start;
	else ret = gData.mPlayerInfo.mP2Start;

	ret = vecAdd(ret, makePosition(gData.mStageInfo.mLocalCoordinates.x / 2,0,0));

	return vecScale(ret, tCoordinateP / (double)gData.mStageInfo.mLocalCoordinates.y);
}

Position getDreamStageCoordinateSystemOffset(int tCoordinateP)
{
	Position ret = makePosition(0, gData.mStageInfo.mZOffset, 0);
	return vecScale(ret, tCoordinateP / (double)gData.mStageInfo.mLocalCoordinates.y);
}

int doesDreamPlayerStartFacingLeft(int i)
{
	if (!i) return gData.mPlayerInfo.mP1Facing == -1;
	else return gData.mPlayerInfo.mP2Facing == -1;
}

double getDreamCameraPositionX(int tCoordinateP)
{
	Position p = *getDreamMugenStageHandlerCameraPositionReference();
	ScreenSize sz = getScreenSize();
	p = transformDreamCoordinatesVector(p, sz.y, tCoordinateP);

	return p.x;
}

double getDreamCameraPositionY(int tCoordinateP)
{
	Position p = *getDreamMugenStageHandlerCameraPositionReference();
	ScreenSize sz = getScreenSize();
	p = transformDreamCoordinatesVector(p, sz.y, tCoordinateP);

	return p.y;
}

double getDreamCameraZoom(int tCoordinateP)
{
	(void)tCoordinateP;
	return 1.0; // TODO: implement zoom
}

double getDreamScreenFactorFromCoordinateP(int tCoordinateP)
{
	ScreenSize sz = getScreenSize();
	return sz.y / (double)tCoordinateP;
}

int getDreamStageCoordinateP()
{
	return gData.mStageInfo.mLocalCoordinates.y;
}

double getDreamStageLeftEdgeX(int tCoordinateP)
{
	Position center = getDreamStageCenterOfScreenBasedOnPlayer(tCoordinateP);
	return center.x - (getDreamGameWidth(tCoordinateP) / 2);
}

double getDreamStageRightEdgeX(int tCoordinateP)
{
	Position center = getDreamStageCenterOfScreenBasedOnPlayer(tCoordinateP);
	return center.x + (getDreamGameWidth(tCoordinateP) / 2);
}

double getDreamStageTopEdgeY(int tCoordinateP)
{
	return getDreamCameraPositionY(tCoordinateP);
}

double getDreamStageBottomEdgeY(int tCoordinateP)
{
	return getDreamCameraPositionY(tCoordinateP) + getDreamGameHeight(tCoordinateP);
}

double transformDreamCoordinates(double tVal, int tSrcP, int tDstP)
{
	return tVal * (tDstP / (double) tSrcP);
}

Vector3D transformDreamCoordinatesVector(Vector3D tVal, int tSrcP, int tDstP)
{
	return vecScale(tVal, (tDstP / (double)tSrcP));
}

double getDreamStageTopOfScreenBasedOnPlayer(int tCoordinateP)
{
	return getDreamCameraPositionY(tCoordinateP);
}

double getDreamStageTopOfScreenBasedOnPlayerInStageCoordinateOffset(int tCoordinateP)
{
	Position stageOffset = getDreamStageCoordinateSystemOffset(tCoordinateP);
	return getDreamStageTopOfScreenBasedOnPlayer(tCoordinateP) - stageOffset.y;
}

double getDreamStageLeftOfScreenBasedOnPlayer(int tCoordinateP)
{
	Position p = *getDreamMugenStageHandlerCameraPositionReference();
	ScreenSize sz = getScreenSize();
	p = transformDreamCoordinatesVector(p, sz.y, tCoordinateP);

	return p.x;
}

double getDreamStageRightOfScreenBasedOnPlayer(int tCoordinateP)
{
	Position p = *getDreamMugenStageHandlerCameraPositionReference();
	ScreenSize sz = getScreenSize();
	p = transformDreamCoordinatesVector(p, sz.y, tCoordinateP);
	double screenSize = transformDreamCoordinates(sz.x, sz.y, tCoordinateP);

	return p.x + screenSize;
}

Position getDreamStageCenterOfScreenBasedOnPlayer(int tCoordinateP)
{
	Position ret = *getDreamMugenStageHandlerCameraPositionReference();

	ret = vecAdd(ret, makePosition(gData.mStageInfo.mLocalCoordinates.x / 2, 0, 0));
	return vecScale(ret, tCoordinateP / (double)gData.mStageInfo.mLocalCoordinates.y);
}

int getDreamGameWidth(int tCoordinateP)
{
	return (int)transformDreamCoordinates(640, 480, tCoordinateP); // TODO: non-hardcoded + zoom
}

int getDreamGameHeight(int tCoordinateP)
{
	return (int)transformDreamCoordinates(480, 480, tCoordinateP); // TODO: non-hardcoded + zoom
}

int getDreamScreenWidth(int tCoordinateP)
{
	return (int)transformDreamCoordinates(640, 480, tCoordinateP); // TODO: non-hardcoded
}

int getDreamScreenHeight(int tCoordinateP)
{
	return (int)transformDreamCoordinates(480, 480, tCoordinateP); // TODO: non-hardcoded
}

char * getDreamStageAuthor()
{
	return gData.mInfo.mAuthor;
}

char * getDreamStageDisplayName()
{
	return gData.mInfo.mDisplayName;
}

char * getDreamStageName()
{
	return gData.mInfo.mName;
}

int getDreamStageLeftEdgeMinimumPlayerDistance(int tCoordinateP)
{
	return (int)transformDreamCoordinates(gData.mBound.mScreenLeft, getDreamStageCoordinateP(), tCoordinateP);
}

int getDreamStageRightEdgeMinimumPlayerDistance(int tCoordinateP)
{
	return (int)transformDreamCoordinates(gData.mBound.mScreenRight, getDreamStageCoordinateP(), tCoordinateP);
}

void setDreamStageCoordinates(Vector3DI tCoordinates)
{
	gData.mStageInfo.mLocalCoordinates = tCoordinates;
	gData.mStageInfo.mZOffset = 0;
}

double getDreamStageShadowTransparency()
{
	return gData.mShadow.mIntensity / 255.0;
}

Vector3D getDreamStageShadowColor()
{
	return makePosition(gData.mShadow.mColor.x / 255.0, gData.mShadow.mColor.y / 255.0, gData.mShadow.mColor.z / 255.0);
}

double getDreamStageShadowScaleY()
{
	return gData.mShadow.mScaleY;
}

Vector3D getDreamStageShadowFadeRange(int tCoordinateP)
{
	return transformDreamCoordinatesVector(gData.mShadow.mFadeRange, getDreamStageCoordinateP(), tCoordinateP);
}

double getDreamStageReflectionTransparency()
{
	return gData.mReflection.mIntensity / 256.0;
}

void setDreamStageNoAutomaticCameraMovement()
{
	gData.mIsCameraManual = 1;
}

void setDreamStageAutomaticCameraMovement()
{
	gData.mIsCameraManual = 0;
}