// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "LetMeTakeASelfie.h"

#include "Core.h"
#include "Engine.h"
#include "ModuleManager.h"
#include "ModuleInterface.h"
#include "Engine/World.h"

class FLetMeTakeASelfiePlugin : public IModuleInterface
{
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};

IMPLEMENT_MODULE( FLetMeTakeASelfiePlugin, LetMeTakeASelfie )

void FLetMeTakeASelfiePlugin::StartupModule()
{
	// Make an actor that ticks
	FLetMeTakeASelfie* SelfieMachine = new FLetMeTakeASelfie();

	FWorldDelegates::FWorldInitializationEvent::FDelegate OnWorldCreatedDelegate = FWorldDelegates::FWorldInitializationEvent::FDelegate::CreateRaw(SelfieMachine, &FLetMeTakeASelfie::OnWorldCreated);
	FDelegateHandle OnWorldCreatedDelegateHandle = FWorldDelegates::OnPostWorldInitialization.Add(OnWorldCreatedDelegate);

	FWorldDelegates::FWorldEvent::FDelegate OnWorldDestroyedDelegate = FWorldDelegates::FWorldEvent::FDelegate::CreateRaw(SelfieMachine, &FLetMeTakeASelfie::OnWorldDestroyed);
	FDelegateHandle OnWorldDestroyedDelegateHandle = FWorldDelegates::OnPreWorldFinishDestroy.Add(OnWorldDestroyedDelegate);
}


void FLetMeTakeASelfiePlugin::ShutdownModule()
{
}