// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "LetMeTakeASelfie.h"
#include "UnrealTournament.h"
#include "UTPlayerController.h"
#include "UTGameState.h"

#include "vpx/vpx_encoder.h"
#include "vpx/vp8cx.h"
#include "vpx/video_writer.h"
#include "vpx/tools_common.h"
#include "vpx/webmenc.h"

DEFINE_LOG_CATEGORY_STATIC(LogUTSelfie, Log, All);

ALetMeTakeASelfie::ALetMeTakeASelfie(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

FLetMeTakeASelfie::FLetMeTakeASelfie()
{
	SelfieWorld = nullptr;
	bTakingSelfie = false;
	bTakingAnimatedSelfie = false;
	SelfieTimeWaited = 0;
	PreviousImage = nullptr;
	SelfieFrameDelay = 1.0f / 15.f;
	SelfieFramesMax = 6.0f / SelfieFrameDelay;
	SelfieDeltaTimeAccum = 0;
	SelfieFrames = 0;
	bStartedAnimatedWritingTask = false;
	bWaitingOnSelfieSurfData = false;
	bSelfieSurfDataReady = false;
}

void FLetMeTakeASelfie::OnWorldCreated(UWorld* World, const UWorld::InitializationValues IVS)
{
	USceneCaptureComponent2D* CaptureComponent = NewObject<USceneCaptureComponent2D>();
	CaptureComponent->UpdateBounds();
	CaptureComponent->AddToRoot();
	CaptureComponent->TextureTarget = NewObject<UTextureRenderTarget2D>();
	CaptureComponent->TextureTarget->InitCustomFormat(1280, 720, PF_B8G8R8A8, false);
	CaptureComponent->TextureTarget->ClearColor = FLinearColor::Black;
	WorldToSceneCaptureComponentMap.Add(World, CaptureComponent);

	if (IVS.bInitializeScenes)
	{
		CaptureComponent->RegisterComponentWithWorld(World);
	}
}

void FLetMeTakeASelfie::OnWorldDestroyed(UWorld* World)
{
	USceneCaptureComponent2D* CaptureComponent = WorldToSceneCaptureComponentMap.FindChecked(World);

	if (CaptureComponent->IsRegistered())
	{
		CaptureComponent->UnregisterComponent();
	}

	WorldToSceneCaptureComponentMap.Remove(World);
	CaptureComponent->RemoveFromRoot();

	if (SelfieWorld == World)
	{
		bTakingSelfie = false;
		bTakingAnimatedSelfie = false;
		SelfieWorld = nullptr;
	}
}

bool FLetMeTakeASelfie::Exec(UWorld* Inworld, const TCHAR* Cmd, FOutputDevice& Ar)
{
	if (FParse::Command(&Cmd, TEXT("SELFIE")))
	{
		if (bTakingSelfie || bTakingAnimatedSelfie)
		{
			return true;
		}

		SelfieWorld = Inworld;
		bTakingSelfie = true;
		
		return true;
	}
	if (FParse::Command(&Cmd, TEXT("SELFIEANIM")))
	{
		if (bTakingSelfie || bTakingAnimatedSelfie)
		{
			return true;
		}

		SelfieWorld = Inworld;
		bTakingAnimatedSelfie = true;
		bStartedAnimatedWritingTask = false;

		return true;
	}

	return false;
}

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
FWriteWebMSelfieWorker* FWriteWebMSelfieWorker::Runnable = nullptr;

// This is not good, do not do long operations in task graph threads, this should be FRunnable
class FWriteAnimatedSelfieTask
{
public:

	FWriteAnimatedSelfieTask(FLetMeTakeASelfie* InLetMeTakeASelfie)
		: LetMeTakeASelfie(InLetMeTakeASelfie)
	{ }

	void DoTask(ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
	{
		UE_LOG(LogUTSelfie, Display, TEXT("Selfie writing beginning!"));

		FString Path = FPaths::GameSavedDir() / TEXT("anim.gif");
		FILE* SelfieGifFile = fopen(TCHAR_TO_ANSI(*Path), "wb");

		gdRect CropRegion;
		CropRegion.x = 365;
		CropRegion.width = 550;
		CropRegion.y = 70;
		CropRegion.height = 650;
		gdRect DestImage;
		DestImage.x = DestImage.y = 0;
		DestImage.width = 480;
		DestImage.height = 570;
		
		TArray<gdImagePtr> ResizedImages;
		double StartTime = FPlatformTime::Seconds();
		for (int i = 0; i < LetMeTakeASelfie->SelfieImages.Num(); i++)
		{
			gdImagePtr CroppedImage = gdImageCreateTrueColor(DestImage.width, DestImage.height);
			gdImageCopyResampled(CroppedImage, LetMeTakeASelfie->SelfieImages[i], DestImage.x, DestImage.y, CropRegion.x, CropRegion.y, DestImage.width, DestImage.height, CropRegion.width, CropRegion.height);
			ResizedImages.Add(CroppedImage);
			gdImageDestroy(LetMeTakeASelfie->SelfieImages[i]);
		}
		LetMeTakeASelfie->SelfieImages.Empty();

		UE_LOG(LogUTSelfie, Display, TEXT("Image resizing took %5.3f seconds"), FPlatformTime::Seconds() - StartTime);
		
		StartTime = FPlatformTime::Seconds();
		gdImageGifAnimBegin(ResizedImages[0], SelfieGifFile, 0, 0);

		for (int i = 0; i < ResizedImages.Num(); i++)
		{
			gdImagePtr PreviousImage = i > 0 ? ResizedImages[i - 1] : nullptr;
			gdImageGifAnimAdd(ResizedImages[i], SelfieGifFile, 1, 0, 0, LetMeTakeASelfie->SelfieFrameDelay * 100, 1, PreviousImage);
		}

		for (int i = 0; i < ResizedImages.Num(); i++)
		{
			gdImageDestroy(ResizedImages[i]);
		}

		gdImageGifAnimEnd(SelfieGifFile);
		fclose(SelfieGifFile);
		
		UE_LOG(LogUTSelfie, Display, TEXT("Image writing took %5.3f seconds"), FPlatformTime::Seconds() - StartTime);

		SelfieGifFile = nullptr;
		LetMeTakeASelfie->bTakingAnimatedSelfie = false;
	}

	static ESubsequentsMode::Type GetSubsequentsMode() { return ESubsequentsMode::FireAndForget; }
	ENamedThreads::Type GetDesiredThread() { return ENamedThreads::AnyThread; }
	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FWriteAnimatedSelfieTask, STATGROUP_TaskGraphTasks);
	}

private:

	FLetMeTakeASelfie* LetMeTakeASelfie;
};

void FLetMeTakeASelfie::ReadPixelsAsync(FRenderTarget* RenderTarget)
{
	SelfieSurfData.Empty(1280 * 720);

	FIntRect InRect(0, 0, RenderTarget->GetSizeXY().X, RenderTarget->GetSizeXY().Y);
	FReadSurfaceDataFlags InFlags(RCM_UNorm, CubeFace_MAX);

	// Read the render target surface data back.	
	struct FReadSurfaceContext
	{
		FRenderTarget* SrcRenderTarget;
		TArray<FColor>* OutData;
		FIntRect Rect;
		FReadSurfaceDataFlags Flags;
		bool* bFinishedPtr;
	};

	FReadSurfaceContext ReadSurfaceContext =
	{
		RenderTarget,
		&SelfieSurfData,
		InRect,
		InFlags,
		&bSelfieSurfDataReady
	};

	ENQUEUE_UNIQUE_RENDER_COMMAND_ONEPARAMETER(
		ReadSurfaceCommand,
		FReadSurfaceContext, Context, ReadSurfaceContext,
		{
			RHICmdList.ReadSurfaceData(
			Context.SrcRenderTarget->GetRenderTargetTexture(),
			Context.Rect,
			*Context.OutData,
			Context.Flags
			);

			*Context.bFinishedPtr = true;
	});

	bWaitingOnSelfieSurfData = true;
}

void FLetMeTakeASelfie::Tick(float DeltaTime)
{
	if (GIsEditor)
	{
		return;
	}

	if (bTakingAnimatedSelfie && SelfieWorld != nullptr && !bStartedAnimatedWritingTask)
	{
		USceneCaptureComponent2D* CaptureComponent = WorldToSceneCaptureComponentMap.FindChecked(SelfieWorld);

		AUTPlayerController* UTPC = Cast<AUTPlayerController>(GEngine->GetFirstLocalPlayerController(SelfieWorld));

		if (UTPC && UTPC->GetPawn())
		{
			FVector NewLocation = UTPC->GetPawn()->GetActorLocation();
			FRotator NewRotation = UTPC->GetPawn()->GetActorRotation();
			NewLocation += (NewRotation.RotateVector(FVector(200, 0, 100)));
			NewRotation.Yaw += 180;
			NewRotation.Pitch = -20;
			CaptureComponent->SetWorldLocationAndRotation(NewLocation, NewRotation, false);
		}

		SelfieDeltaTimeAccum += DeltaTime;
		
		if (bWaitingOnSelfieSurfData)
		{
			if (bSelfieSurfDataReady)
			{
				int i = 0;
				if (SelfieImages.Num() - 1 < SelfieFrames)
				{
					SelfieImages.Add(gdImageCreateTrueColor(1280, 720));
				}
				gdImagePtr ScreenshotImage = SelfieImages[SelfieFrames];
				for (int y = 0; y < 720; y++)
				for (int x = 0; x < 1280; x++)
				{
					int Color = gdTrueColor(SelfieSurfData[i].R, SelfieSurfData[i].G, SelfieSurfData[i].B);
					gdImageSetPixel(ScreenshotImage, x, y, Color);
					i++;
				}
				SelfieFrames++;

				bWaitingOnSelfieSurfData = false;
				bSelfieSurfDataReady = false;
			}
		}

		if (SelfieFrames == SelfieFramesMax)
		{
			bStartedAnimatedWritingTask = true;
			SelfieFrames = 0;

			FWriteWebMSelfieWorker::RunWorkerThread(this);
		}
		else if (SelfieTimeWaited < 0.5f)
		{
			SelfieTimeWaited += DeltaTime;
		}
		else if (!bWaitingOnSelfieSurfData && (SelfieFrames == 0 || SelfieDeltaTimeAccum > SelfieFrameDelay))
		{
			double StartTime = FPlatformTime::Seconds();
			FRenderTarget* RenderTarget = CaptureComponent->TextureTarget->GameThread_GetRenderTargetResource();
			ReadPixelsAsync(RenderTarget);
			SelfieDeltaTimeAccum = 0;
		}
	}
	else if (bTakingSelfie && SelfieWorld != nullptr)
	{
		USceneCaptureComponent2D* CaptureComponent = WorldToSceneCaptureComponentMap.FindChecked(SelfieWorld);

		AUTPlayerController* UTPC = Cast<AUTPlayerController>(GEngine->GetFirstLocalPlayerController(SelfieWorld));

		if (UTPC && UTPC->GetPawn())
		{
			FVector NewLocation = UTPC->GetPawn()->GetActorLocation();
			FRotator NewRotation = UTPC->GetPawn()->GetActorRotation();
			NewLocation += (NewRotation.RotateVector(FVector(200, 0, 100)));
			NewRotation.Yaw += 180;
			NewRotation.Pitch = -20;
			CaptureComponent->SetWorldLocationAndRotation(NewLocation, NewRotation, false);
		}

		if (SelfieTimeWaited > 0.25f)
		{
			FRenderTarget* RenderTarget = CaptureComponent->TextureTarget->GameThread_GetRenderTargetResource();
			TArray<FColor> SurfData;
			RenderTarget->ReadPixels(SurfData);

			int i = 0;
			gdImagePtr ScreenshotImage = gdImageCreateTrueColor(1280, 720);
			for (int y = 0; y < 720; y++)
			for (int x = 0; x < 1280; x++)
			{
				int Color = gdTrueColor(SurfData[i].R, SurfData[i].G, SurfData[i].B);
				gdImageSetPixel(ScreenshotImage, x, y, Color);
				i++;
			}

			gdRect CropRegion;
			CropRegion.x = 365;
			CropRegion.width = 550;
			CropRegion.y = 70;
			CropRegion.height = 650;

			gdImagePtr CroppedImage = gdImageCreateTrueColor(CropRegion.width, CropRegion.height);
			gdImageCopy(CroppedImage, ScreenshotImage, 0, 0, CropRegion.x, CropRegion.y, CropRegion.width, CropRegion.height);
			
			gdImageDestroy(ScreenshotImage);
			
			FILE * RawScreenShotFile;
			FString ScreenshotPath = FPaths::GameSavedDir() / TEXT("screenshot.bmp");
			RawScreenShotFile = fopen(TCHAR_TO_ANSI(*ScreenshotPath), "wb");
			gdImageBmp(CroppedImage, RawScreenShotFile, 0);
			gdImageDestroy(CroppedImage);
			fclose(RawScreenShotFile);

			SelfieTimeWaited = 0;
			bTakingSelfie = false;
			SelfieWorld = nullptr;
		}
		else
		{
			SelfieTimeWaited += DeltaTime;
		}
	}
}

/* based on http://stackoverflow.com/questions/4765436/need-to-create-a-webm-video-from-rgb-frames */
/* I have no idea how YUV color space works */
void ARGB_To_YV12(gdImagePtr Image, int nFrameWidth, int nFrameHeight, void *pFullYPlane, void *pDownsampledUPlane, void *pDownsampledVPlane)
{
	int nRGBBytes = nFrameWidth * nFrameHeight * 3;
	unsigned char *pYPlaneOut = (unsigned char*)pFullYPlane;
	int nYPlaneOut = 0;

	unsigned char* pRGBData = new unsigned char[nRGBBytes * 3];
	
	int i = 0;
	for (int y = 0; y < 720; y++)
	for (int x = 0; x < 1280; x++)
	{
		// gd has image data stored in ARGB
		int PixelColor = gdImageGetTrueColorPixel(Image, x, y);
		unsigned char B = gdTrueColorGetBlue(PixelColor);
		unsigned char G = gdTrueColorGetGreen(PixelColor);
		unsigned char R = gdTrueColorGetRed(PixelColor);

		float y = (float)(R * 66 + G * 129 + B * 25 + 128) / 256 + 16;
		float u = (float)(R*-38 + G*-74 + B * 112 + 128) / 256 + 128;
		float v = (float)(R * 112 + G*-94 + B*-18 + 128) / 256 + 128;

		// NOTE: We're converting pRGBData to YUV in-place here as well as writing out YUV to pFullYPlane/pDownsampledUPlane/pDownsampledVPlane.
		pRGBData[i + 0] = (unsigned char)y;
		pRGBData[i + 1] = (unsigned char)u;
		pRGBData[i + 2] = (unsigned char)v;

		// Write out the Y plane directly here rather than in another loop.
		pYPlaneOut[nYPlaneOut++] = pRGBData[i + 0];

		i += 3;
	}

	// Downsample to U and V.
	int halfHeight = nFrameHeight >> 1;
	int halfWidth = nFrameWidth >> 1;

	unsigned char *pVPlaneOut = (unsigned char*)pDownsampledVPlane;
	unsigned char *pUPlaneOut = (unsigned char*)pDownsampledUPlane;

	for (int yPixel = 0; yPixel < halfHeight; yPixel++)
	{
		int iBaseSrc = ((yPixel * 2) * nFrameWidth * 3);

		for (int xPixel = 0; xPixel < halfWidth; xPixel++)
		{
			pVPlaneOut[yPixel * halfWidth + xPixel] = pRGBData[iBaseSrc + 2];
			pUPlaneOut[yPixel * halfWidth + xPixel] = pRGBData[iBaseSrc + 1];

			iBaseSrc += 6;
		}
	}
	delete [] pRGBData;
}

void FLetMeTakeASelfie::WriteWebM()
{		
	int32 width = 1280;
	int32 height = 720;

	VpxVideoInfo info = { 0 };
	const VpxInterface *encoder = NULL;
	FILE* file = nullptr;
	vpx_codec_ctx_t      codec;
	vpx_codec_enc_cfg_t  cfg;
	vpx_codec_err_t      res;
	struct EbmlGlobal    ebml;
	int                  flags = 0;

	encoder = get_vpx_encoder_by_name("vp8");
	if (!encoder)
	{
		return;
	}

	UE_LOG(LogUTSelfie, Display, TEXT("Compressing with %s"), ANSI_TO_TCHAR(vpx_codec_iface_name(encoder->codec_interface())));

	res = vpx_codec_enc_config_default(encoder->codec_interface(), &cfg, 0);
	if (res)
	{
		return;
	}

	cfg.rc_target_bitrate = width * height * cfg.rc_target_bitrate / cfg.g_w / cfg.g_h;
	cfg.g_w = width;
	cfg.g_h = height;
	cfg.g_timebase.den = 15;

	info.codec_fourcc = encoder->fourcc;
	info.frame_width = width;
	info.frame_height = height;
	info.time_base.numerator = cfg.g_timebase.num;
	info.time_base.denominator = cfg.g_timebase.den;

	ebml.last_pts_ns = -1;
	ebml.writer = NULL;
	ebml.segment = NULL;

	vpx_image_t raw;
	if (!vpx_img_alloc(&raw, VPX_IMG_FMT_YV12, width, height, 1))
	{
		return;
	}

	if (vpx_codec_enc_init(&codec, encoder->codec_interface(), &cfg, 0))
	{
		return;
	}

	FString WebMPath = FPaths::GameSavedDir() / TEXT("anim.webm");
	file = fopen(TCHAR_TO_ANSI(*WebMPath), "wb");
	ebml.stream = file;
	struct vpx_rational framerate = cfg.g_timebase;
	write_webm_file_header(&ebml, &cfg, &framerate, STEREO_FORMAT_MONO, encoder->fourcc);
	if (!file)
	{
		return;
	}

	// write some frames
	int32 frame_cnt = 0;
	
	for (int i = 0; i < SelfieImages.Num(); i++)
	{
		ARGB_To_YV12(SelfieImages[i], width, height, raw.planes[VPX_PLANE_Y], raw.planes[VPX_PLANE_U], raw.planes[VPX_PLANE_V]);

		vpx_codec_encode(&codec, &raw, frame_cnt, 1, flags, VPX_DL_GOOD_QUALITY);
		vpx_codec_iter_t iter = NULL;
		const vpx_codec_cx_pkt_t *pkt;
		while ((pkt = vpx_codec_get_cx_data(&codec, &iter)) != NULL)
		{
			switch (pkt->kind) 
			{
			case VPX_CODEC_CX_FRAME_PKT: 
				write_webm_block(&ebml, &cfg, pkt);
				break;                                                    
			default:
				break;
			}
		}
		frame_cnt++;
	}

	UE_LOG(LogUTSelfie, Display, TEXT("Writing complete"));

	// write final frame
	vpx_codec_encode(&codec, nullptr, frame_cnt, 1, flags, VPX_DL_GOOD_QUALITY);
	vpx_codec_iter_t iter = NULL;
	const vpx_codec_cx_pkt_t *pkt;
	pkt = vpx_codec_get_cx_data(&codec, &iter);
	while ((pkt = vpx_codec_get_cx_data(&codec, &iter)) != NULL)
	{
		switch (pkt->kind)
		{
		case VPX_CODEC_CX_FRAME_PKT:
			write_webm_block(&ebml, &cfg, pkt);
			break;
		default:
			break;
		}
	}

	vpx_img_free(&raw);                                                       //
	if (vpx_codec_destroy(&codec))
	{
		// failed to destroy
	}

	write_webm_file_footer(&ebml);

	UE_LOG(LogUTSelfie, Display, TEXT("Closing file"));
	fclose(file);

	SelfieTimeWaited = 0;
	bTakingAnimatedSelfie = false;

	UE_LOG(LogUTSelfie, Display, TEXT("Selfie complete!"));
}

// TODO: don't take a dependency on tools_common
// stub for tools_common
void usage_exit()
{

}