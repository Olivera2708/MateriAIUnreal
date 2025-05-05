// Copyright Epic Games, Inc. All Rights Reserved.

#include "MateriAICommands.h"

#define LOCTEXT_NAMESPACE "FMateriAIModule"

void FMateriAICommands::RegisterCommands()
{
	UI_COMMAND(OpenPluginWindow, "MateriAI", "Bring up MateriAI window", EUserInterfaceActionType::Button, FInputChord());
}

#undef LOCTEXT_NAMESPACE
