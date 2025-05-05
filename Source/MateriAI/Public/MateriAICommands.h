// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Framework/Commands/Commands.h"
#include "MateriAIStyle.h"

class FMateriAICommands : public TCommands<FMateriAICommands>
{
public:

	FMateriAICommands()
		: TCommands<FMateriAICommands>(TEXT("MateriAI"), NSLOCTEXT("Contexts", "MateriAI", "MateriAI Plugin"), NAME_None, FMateriAIStyle::GetStyleSetName())
	{
	}

	// TCommands<> interface
	virtual void RegisterCommands() override;

public:
	TSharedPtr< FUICommandInfo > OpenPluginWindow;
};