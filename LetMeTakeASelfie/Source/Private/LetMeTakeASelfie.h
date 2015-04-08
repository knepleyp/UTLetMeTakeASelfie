// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Core.h"
#include "UnrealTournament.h"

#include "gd.h"

#include "LetMeTakeASelfie.generated.h"

UCLASS(Blueprintable, Meta = (ChildCanTick))
class ALetMeTakeASelfie : public AActor
{
	GENERATED_UCLASS_BODY()
	
};

struct FLetMeTakeASelfie : FTickableGameObject, FSelfRegisteringExec
{
	FLetMeTakeASelfie();
	virtual void Tick(float DeltaTime);
	virtual bool IsTickable() const { return true; }
	virtual bool IsTickableInEditor() const { return true; }

	// Put a real stat id here
	virtual TStatId GetStatId() const
	{
		return TStatId();
	}

	/** FSelfRegisteringExec implementation */
	virtual bool Exec(UWorld* Inworld, const TCHAR* Cmd, FOutputDevice& Ar) override;

	void OnWorldCreated(UWorld* World, const UWorld::InitializationValues IVS);
	void OnWorldDestroyed(UWorld* World);

	TMap< UWorld*, USceneCaptureComponent2D* > WorldToSceneCaptureComponentMap;
	UWorld* SelfieWorld;
	bool bTakingAnimatedSelfie;
	float SelfieTimeTotal;
	int32 SelfieFrames;
	gdImagePtr PreviousImage;
	int32 SelfieFramesMax;
	float SelfieFrameDelay;
	float SelfieDeltaTimeAccum;
	bool bStartedAnimatedWritingTask;
	int32 SelfieWidth;
	int32 SelfieHeight;
	int32 SelfieFrameRate;

	// Capturing in a ring buffer, this is the current head
	int32 HeadFrame;

	// Currently never cleaned up, gdImageFree was very expensive when run from a worker thread
	TArray<gdImagePtr> SelfieImages;

	float SelfieTimeWaited;

	TArray<FColor> SelfieSurfData;
	bool bWaitingOnSelfieSurfData;
	bool bSelfieSurfDataReady;
	void ReadPixelsAsync(FRenderTarget* RenderTarget);
	
	void WriteWebM();
};


/* Based on https://wiki.unrealengine.com/Multi-Threading:_How_to_Create_Threads_in_UE4 */
class FWriteWebMSelfieWorker : public FRunnable
{
	FWriteWebMSelfieWorker(FLetMeTakeASelfie* InLetMeTakeASelfie)
	: LetMeTakeASelfie(InLetMeTakeASelfie)
	{
		Thread = FRunnableThread::Create(this, TEXT("FWriteWebMSelfieWorker"), 0, TPri_BelowNormal);
	}

	~FWriteWebMSelfieWorker()
	{
		delete Thread;
		Thread = nullptr;

	}

	uint32 Run()
	{
		LetMeTakeASelfie->WriteWebM();

		return 0;
	}

public:
	static FWriteWebMSelfieWorker* RunWorkerThread(FLetMeTakeASelfie* InLetMeTakeASelfie)
	{
		if (Runnable)
		{
			delete Runnable;
			Runnable = nullptr;
		}

		if (Runnable == nullptr)
		{
			Runnable = new FWriteWebMSelfieWorker(InLetMeTakeASelfie);
		}

		return Runnable;
	}

private:
	FLetMeTakeASelfie* LetMeTakeASelfie;
	FRunnableThread* Thread;
	static FWriteWebMSelfieWorker* Runnable;
};
