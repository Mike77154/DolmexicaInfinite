#pragma once

#include <string>

#include "mugenassignment.h"
#include "playerdefinition.h"

void setupDreamAssignmentEvaluator();
void setupDreamStoryAssignmentEvaluator();
void shutdownDreamAssignmentEvaluator();

int evaluateDreamAssignment(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer);
double evaluateDreamAssignmentAndReturnAsFloat(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer);
int evaluateDreamAssignmentAndReturnAsInteger(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer);
void evaluateDreamAssignmentAndReturnAsString(std::string& oString, DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer);
Vector3D evaluateDreamAssignmentAndReturnAsVector3D(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer);
Vector3DI evaluateDreamAssignmentAndReturnAsVector3DI(DreamMugenAssignment** tAssignment, DreamPlayer* tPlayer);
