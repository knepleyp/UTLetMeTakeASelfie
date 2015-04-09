// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "LetMeTakeASelfie.h"
#include "UnrealTournament.h"
#include "UTPlayerController.h"
#include "UTGameState.h"
#include "UTCTFGameState.h"

#include "SlateBasics.h"
#include "ScreenRendering.h"
#include "RenderCore.h"
#include "RHIStaticStates.h"
#include "RendererInterface.h"

#include "vpx/vpx_encoder.h"
#include "vpx/vp8cx.h"
#include "vpx/video_writer.h"
#include "vpx/webmenc.h"

DEFINE_LOG_CATEGORY_STATIC(LogUTSelfie, Log, All);

ALetMeTakeASelfie::ALetMeTakeASelfie(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

FLetMeTakeASelfie::FLetMeTakeASelfie()
{
	SelfieWorld = nullptr;
	bTakingAnimatedSelfie = false;
	SelfieTimeWaited = 0;
	PreviousImage = nullptr;
	SelfieFrameRate = 30;
	const float SelfieLength = 6.0f;
	SelfieFrameDelay = 1.0f / SelfieFrameRate;
	SelfieFramesMax = SelfieLength / SelfieFrameDelay;
	SelfieDeltaTimeAccum = 0;
	SelfieFrames = 0;
	HeadFrame = 0;
	bStartedAnimatedWritingTask = false;
	bWaitingOnSelfieSurfData = false;
	bSelfieSurfDataReady = false;
	RecordedNumberOfScoringPlayers = 0;
	DelayedEventWriteTimer = 0;
	bFirstPerson = false;

	SelfieWidth = 1280;
	SelfieHeight = 720;
	//SelfieWidth = 1024;
	//SelfieHeight = 576;

	bRegisteredSlateDelegate = false;
	ReadbackTextureIndex = 0;
	ReadbackBufferIndex = 0;
	ReadbackBuffers[0] = nullptr;
	ReadbackBuffers[1] = nullptr;
}

void FLetMeTakeASelfie::OnWorldCreated(UWorld* World, const UWorld::InitializationValues IVS)
{
	USceneCaptureComponent2D* CaptureComponent = NewObject<USceneCaptureComponent2D>();
	CaptureComponent->UpdateBounds();
	CaptureComponent->AddToRoot();
	CaptureComponent->TextureTarget = NewObject<UTextureRenderTarget2D>();
	CaptureComponent->TextureTarget->InitCustomFormat(SelfieWidth, SelfieHeight, PF_B8G8R8A8, false);
	CaptureComponent->TextureTarget->ClearColor = FLinearColor::Black;
	CaptureComponent->SetVisibility(false);
	WorldToSceneCaptureComponentMap.Add(World, CaptureComponent);

	if (!bRegisteredSlateDelegate)
	{
		FSlateRenderer* SlateRenderer = FSlateApplication::Get().GetRenderer().Get();
		SlateRenderer->OnSlateWindowRendered().AddRaw(this, &FLetMeTakeASelfie::OnSlateWindowRenderedDuringCapture);
		bRegisteredSlateDelegate = true;

		// Setup readback buffer textures
		{
			for (int32 TextureIndex = 0; TextureIndex < 2; ++TextureIndex)
			{
				FRHIResourceCreateInfo CreateInfo;
				ReadbackTextures[TextureIndex] = RHICreateTexture2D(
					SelfieWidth,
					SelfieHeight,
					PF_B8G8R8A8,
					1,
					1,
					TexCreate_CPUReadback,
					CreateInfo
					);
			}

			ReadbackTextureIndex = 0;

			ReadbackBuffers[0] = nullptr;
			ReadbackBuffers[1] = nullptr;
			ReadbackBufferIndex = 0;
		}
	}

	if (IVS.bInitializeScenes)
	{
		CaptureComponent->RegisterComponentWithWorld(World);
	}

	// Allocate the frames once, allocation can be very slow
	for (int32 i = 0; SelfieImages.Num() < SelfieFramesMax && i < SelfieFramesMax; i++)
	{
		SelfieImages.Add(gdImageCreateTrueColor(SelfieWidth, SelfieHeight));
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
		bTakingAnimatedSelfie = false;
		RecordedNumberOfScoringPlayers = 0;
		SelfieWorld = nullptr;
	}
}

FWriteWebMSelfieWorker* FWriteWebMSelfieWorker::Runnable = nullptr;
\
bool FLetMeTakeASelfie::Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar)
{
	if (FParse::Command(&Cmd, TEXT("SELFIEANIM")))
	{
		if (bTakingAnimatedSelfie && SelfieWorld != InWorld)
		{
			// Already in a different world
			return true;
		}

		SelfieWorld = InWorld;
		bTakingAnimatedSelfie = true;

		if (FParse::Command(&Cmd, TEXT("FPS")))
		{
			bFirstPerson = true;
		}
		else
		{
			bFirstPerson = false;
		}

		USceneCaptureComponent2D* CaptureComponent = WorldToSceneCaptureComponentMap.FindChecked(InWorld);
		if (bFirstPerson)
		{
			CaptureComponent->SetVisibility(false);
		}
		else
		{
			CaptureComponent->SetVisibility(true);
		}

		return true;
	}

	if (FParse::Command(&Cmd, TEXT("SELFIEWRITE")))
	{
		if (!bTakingAnimatedSelfie || bStartedAnimatedWritingTask)
		{
			return true;
		}
		bStartedAnimatedWritingTask = true;
		FWriteWebMSelfieWorker::RunWorkerThread(this);

		return true;
	}

	return false;
}

void FLetMeTakeASelfie::ReadPixelsAsync(FRenderTarget* RenderTarget)
{
	FIntRect InRect(0, 0, RenderTarget->GetSizeXY().X, RenderTarget->GetSizeXY().Y);
	SelfieSurfData.Empty(RenderTarget->GetSizeXY().X * RenderTarget->GetSizeXY().Y);
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
	
	if (SelfieTimeWaited < 0.5f)
	{
		SelfieTimeWaited += DeltaTime;
		return;
	}

	if (bStartedAnimatedWritingTask)
	{
		return;
	}
	
	if (DelayedEventWriteTimer > 0)
	{
		DelayedEventWriteTimer -= DeltaTime;
		if (DelayedEventWriteTimer < 0)
		{
			bStartedAnimatedWritingTask = true;
			FWriteWebMSelfieWorker::RunWorkerThread(this);
		}
	}

	if (bWaitingOnSelfieSurfData)
	{
		if (bSelfieSurfDataReady)
		{
			int i = 0;

			gdImagePtr ScreenshotImage = SelfieImages[HeadFrame];
			for (int y = 0; y < SelfieHeight; y++)
			for (int x = 0; x < SelfieWidth; x++)
			{
				int Color = gdTrueColor(SelfieSurfData[i].R, SelfieSurfData[i].G, SelfieSurfData[i].B);
				gdImageSetPixel(ScreenshotImage, x, y, Color);
				i++;
			}
			SelfieFrames = FMath::Min(SelfieFrames + 1, SelfieFramesMax);
			HeadFrame += 1;
			HeadFrame %= SelfieFramesMax;

			bWaitingOnSelfieSurfData = false;
			bSelfieSurfDataReady = false;
		}
	}

	if (bTakingAnimatedSelfie && SelfieWorld != nullptr)
	{
		USceneCaptureComponent2D* CaptureComponent = WorldToSceneCaptureComponentMap.FindChecked(SelfieWorld);

		AUTPlayerController* UTPC = Cast<AUTPlayerController>(GEngine->GetFirstLocalPlayerController(SelfieWorld));

		if (UTPC && UTPC->GetPawn())
		{
			APawn* Pawn = UTPC->GetPawn();

			// Projectile following wasn't as cool as I wanted it to be
			/*
			if (!FollowingProjectile.IsValid())
			{
				for (TActorIterator<AUTProjectile> It(SelfieWorld); It; ++It)
				{
					if (!It->bFakeClientProjectile)
					{
						if (It->Instigator == Pawn && !It->bExploded)
						{
							FollowingProjectile = *It;
						}
					}
				}
			}

			if (FollowingProjectile.IsValid())
			{
				FVector NewLocation = FollowingProjectile->GetActorLocation();
				FRotator NewRotation = FollowingProjectile->GetActorRotation();
				NewLocation += (NewRotation.RotateVector(FVector(-200, 0, 0)));
				CaptureComponent->SetWorldLocationAndRotation(NewLocation, NewRotation, false);
			}
			else
			{
				FVector NewLocation = Pawn->GetActorLocation();
				FRotator NewRotation = Pawn->GetActorRotation();
				NewLocation += (NewRotation.RotateVector(FVector(200, 0, 100)));
				NewRotation.Yaw += 180;
				NewRotation.Pitch = -20;
				CaptureComponent->SetWorldLocationAndRotation(NewLocation, NewRotation, false);
			}*/

			FVector NewLocation = Pawn->GetActorLocation();
			FRotator NewRotation = Pawn->GetActorRotation();
			NewLocation += (NewRotation.RotateVector(FVector(200, 0, 100)));
			NewRotation.Yaw += 180;
			NewRotation.Pitch = -20;
			CaptureComponent->SetWorldLocationAndRotation(NewLocation, NewRotation, false);
		}

		SelfieDeltaTimeAccum += DeltaTime;

		if (!bWaitingOnSelfieSurfData && (SelfieFrames == 0 || SelfieDeltaTimeAccum > SelfieFrameDelay))
		{
			if (!bFirstPerson)
			{
				FRenderTarget* RenderTarget = CaptureComponent->TextureTarget->GameThread_GetRenderTargetResource();
				ReadPixelsAsync(RenderTarget);
				SelfieDeltaTimeAccum = 0;
			}
		}
				
		// Try to autorecord if you cap the flag
		AUTCTFGameState* GS = Cast<AUTCTFGameState>(SelfieWorld->GetGameState());
		if (!bStartedAnimatedWritingTask && GS && UTPC->PlayerState)
		{
			if (RecordedNumberOfScoringPlayers < GS->GetScoringPlays().Num())
			{
				RecordedNumberOfScoringPlayers++;

				if (GS->GetScoringPlays()[GS->GetScoringPlays().Num() - 1].ScoredBy.GetPlayerName() == UTPC->PlayerState->PlayerName)
				{
					DelayedEventWriteTimer = 2.0f;
				}
			}
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

	// never deallocated due to laziness
	static unsigned char* pRGBData = new unsigned char[nRGBBytes * 3];
	
	int i = 0;
	for (int y = 0; y < nFrameHeight; y++)
	for (int x = 0; x < nFrameWidth; x++)
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
}

void FLetMeTakeASelfie::WriteWebM()
{		
	int32 width = SelfieWidth;
	int32 height = SelfieHeight;

	VpxVideoInfo info = { 0 };
	FILE* file = nullptr;
	vpx_codec_ctx_t      codec;
	vpx_codec_enc_cfg_t  cfg;
	vpx_codec_err_t      res;
	struct EbmlGlobal    ebml;
	int                  flags = 0;

#define interface (vpx_codec_vp8_cx())

	res = vpx_codec_enc_config_default(interface, &cfg, 0);
	if (res)
	{
		return;
	}
	UE_LOG(LogUTSelfie, Display, TEXT("Compressing with %s"), ANSI_TO_TCHAR(vpx_codec_iface_name(interface)));

	cfg.rc_target_bitrate = width * height * cfg.rc_target_bitrate / cfg.g_w / cfg.g_h;
	cfg.g_w = width;
	cfg.g_h = height;
	cfg.g_timebase.den = SelfieFrameRate;

#define VP8_FOURCC 0x30385056
	info.codec_fourcc = VP8_FOURCC;
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

	if (vpx_codec_enc_init(&codec, interface, &cfg, 0))
	{
		return;
	}
	
	FString BasePath = FPaths::ScreenShotDir();
	FString WebMPath = BasePath / TEXT("anim.webm");
	static int32 WebMIndex = 0;
	const int32 MaxTestWebMIndex = 65536;
	for (int32 TestWebMIndex = WebMIndex + 1; TestWebMIndex < MaxTestWebMIndex; ++TestWebMIndex)
	{
		const FString TestFileName = BasePath / FString::Printf(TEXT("UTSelfie%05i.webm"), TestWebMIndex);
		if (IFileManager::Get().FileSize(*TestFileName) < 0)
		{
			WebMIndex = TestWebMIndex;
			WebMPath = TestFileName;
			break;
		}
	}

	file = fopen(TCHAR_TO_ANSI(*WebMPath), "wb");
	ebml.stream = file;
	struct vpx_rational framerate = cfg.g_timebase;
	write_webm_file_header(&ebml, &cfg, &framerate, STEREO_FORMAT_MONO, VP8_FOURCC);
	if (!file)
	{
		return;
	}

	// write some frames
	int32 frame_cnt = 0;
	
	for (int i = 0; i < SelfieFrames; i++)
	{
		ARGB_To_YV12(SelfieImages[(HeadFrame + i) % SelfieFramesMax], width, height, raw.planes[VPX_PLANE_Y], raw.planes[VPX_PLANE_U], raw.planes[VPX_PLANE_V]);

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
	bStartedAnimatedWritingTask = false;
	SelfieFrames = 0;

	UE_LOG(LogUTSelfie, Display, TEXT("Selfie complete!"));
}

// Borrowed from GameLiveStreaming.cpp
void FLetMeTakeASelfie::OnSlateWindowRenderedDuringCapture(SWindow& SlateWindow, void* ViewportRHIPtr)
{
	UGameViewportClient* GameViewportClient = GEngine->GameViewport;
	if (!bStartedAnimatedWritingTask && bTakingAnimatedSelfie && bFirstPerson && GameViewportClient != nullptr)
	{
		if (GameViewportClient->GetWindow() == SlateWindow.AsShared())
		{
			if (SelfieDeltaTimeAccum > SelfieFrameDelay)
			{
				CopyCurrentFrameToSavedFrames();

				const FViewportRHIRef* ViewportRHI = (const FViewportRHIRef*)ViewportRHIPtr;
				StartCopyingNextGameFrame(*ViewportRHI);
				SelfieDeltaTimeAccum = 0;
			}
		}
	}
}

void FLetMeTakeASelfie::CopyCurrentFrameToSavedFrames()
{
	if (ReadbackBuffers[ReadbackBufferIndex] != nullptr)
	{
		// Have a new buffer from the GPU
		int i = 0;

		gdImagePtr ScreenshotImage = SelfieImages[HeadFrame];
		for (int y = 0; y < SelfieHeight; y++)
		for (int x = 0; x < SelfieWidth; x++)
		{
			FColor& SurfData = ((FColor*)ReadbackBuffers[ReadbackBufferIndex])[i];
			int Color = gdTrueColor(SurfData.R, SurfData.G, SurfData.B);
			gdImageSetPixel(ScreenshotImage, x, y, Color);
			i++;
		}
		SelfieFrames = FMath::Min(SelfieFrames + 1, SelfieFramesMax);
		HeadFrame += 1;
		HeadFrame %= SelfieFramesMax;

		// Unmap the buffer now that we've pushed out the frame
		{
			struct FReadbackFromStagingBufferContext
			{
				FLetMeTakeASelfie* This;
			};
			FReadbackFromStagingBufferContext ReadbackFromStagingBufferContext =
			{
				this
			};
			ENQUEUE_UNIQUE_RENDER_COMMAND_ONEPARAMETER(
				ReadbackFromStagingBuffer,
				FReadbackFromStagingBufferContext, Context, ReadbackFromStagingBufferContext,
				{
				RHICmdList.UnmapStagingSurface(Context.This->ReadbackTextures[Context.This->ReadbackTextureIndex]);
			});
		}
	}
}

void FLetMeTakeASelfie::StartCopyingNextGameFrame(const FViewportRHIRef& ViewportRHI)
{
	const FIntPoint ResizeTo(SelfieWidth, SelfieHeight);

	static const FName RendererModuleName("Renderer");
	IRendererModule& RendererModule = FModuleManager::GetModuleChecked<IRendererModule>(RendererModuleName);

	UGameViewportClient* GameViewportClient = GEngine->GameViewport;
	check(GameViewportClient != nullptr);

	struct FCopyVideoFrame
	{
		FViewportRHIRef ViewportRHI;
		IRendererModule* RendererModule;
		FIntPoint ResizeTo;
		FLetMeTakeASelfie* This;
	};
	FCopyVideoFrame CopyVideoFrame =
	{
		ViewportRHI,
		&RendererModule,
		ResizeTo,
		this
	};

	ENQUEUE_UNIQUE_RENDER_COMMAND_ONEPARAMETER(
		ReadSurfaceCommand,
		FCopyVideoFrame, Context, CopyVideoFrame,
		{
		FPooledRenderTargetDesc OutputDesc(FPooledRenderTargetDesc::Create2DDesc(Context.ResizeTo, PF_B8G8R8A8, TexCreate_None, TexCreate_RenderTargetable, false));

		const auto FeatureLevel = GMaxRHIFeatureLevel;

		TRefCountPtr<IPooledRenderTarget> ResampleTexturePooledRenderTarget;
		Context.RendererModule->RenderTargetPoolFindFreeElement(OutputDesc, ResampleTexturePooledRenderTarget, TEXT("ResampleTexture"));
		check(ResampleTexturePooledRenderTarget);

		const FSceneRenderTargetItem& DestRenderTarget = ResampleTexturePooledRenderTarget->GetRenderTargetItem();

		SetRenderTarget(RHICmdList, DestRenderTarget.TargetableTexture, FTextureRHIRef());
		RHICmdList.SetViewport(0, 0, 0.0f, Context.ResizeTo.X, Context.ResizeTo.Y, 1.0f);

		RHICmdList.SetBlendState(TStaticBlendState<>::GetRHI());
		RHICmdList.SetRasterizerState(TStaticRasterizerState<>::GetRHI());
		RHICmdList.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always>::GetRHI());

		FTexture2DRHIRef ViewportBackBuffer = RHICmdList.GetViewportBackBuffer(Context.ViewportRHI);

		auto ShaderMap = GetGlobalShaderMap(FeatureLevel);
		TShaderMapRef<FScreenVS> VertexShader(ShaderMap);
		TShaderMapRef<FScreenPS> PixelShader(ShaderMap);

		static FGlobalBoundShaderState BoundShaderState;
		SetGlobalBoundShaderState(RHICmdList, FeatureLevel, BoundShaderState, Context.RendererModule->GetFilterVertexDeclaration().VertexDeclarationRHI, *VertexShader, *PixelShader);

		if (Context.ResizeTo != FIntPoint(ViewportBackBuffer->GetSizeX(), ViewportBackBuffer->GetSizeY()))
		{
			// We're scaling down the window, so use bilinear filtering
			PixelShader->SetParameters(RHICmdList, TStaticSamplerState<SF_Bilinear>::GetRHI(), ViewportBackBuffer);
		}
		else
		{
			// Drawing 1:1, so no filtering needed
			PixelShader->SetParameters(RHICmdList, TStaticSamplerState<SF_Point>::GetRHI(), ViewportBackBuffer);
		}

		Context.RendererModule->DrawRectangle(
			RHICmdList,
			0, 0,		// Dest X, Y
			Context.ResizeTo.X, Context.ResizeTo.Y,	// Dest Width, Height
			0, 0,		// Source U, V
			1, 1,		// Source USize, VSize
			Context.ResizeTo,		// Target buffer size
			FIntPoint(1, 1),		// Source texture size
			*VertexShader,
			EDRF_Default);

		// Asynchronously copy render target from GPU to CPU
		const bool bKeepOriginalSurface = false;
		RHICmdList.CopyToResolveTarget(
			DestRenderTarget.TargetableTexture,
			Context.This->ReadbackTextures[Context.This->ReadbackTextureIndex],
			bKeepOriginalSurface,
			FResolveParams());
	});


	// Start mapping the newly-rendered buffer
	{
		struct FReadbackFromStagingBufferContext
		{
			FLetMeTakeASelfie* This;
		};
		FReadbackFromStagingBufferContext ReadbackFromStagingBufferContext =
		{
			this
		};
		ENQUEUE_UNIQUE_RENDER_COMMAND_ONEPARAMETER(
			ReadbackFromStagingBuffer,
			FReadbackFromStagingBufferContext, Context, ReadbackFromStagingBufferContext,
			{
			int32 UnusedWidth = 0;
			int32 UnusedHeight = 0;
			RHICmdList.MapStagingSurface(Context.This->ReadbackTextures[Context.This->ReadbackTextureIndex], Context.This->ReadbackBuffers[Context.This->ReadbackBufferIndex], UnusedWidth, UnusedHeight);

			// Ping pong between readback textures
			Context.This->ReadbackTextureIndex = (Context.This->ReadbackTextureIndex + 1) % 2;
		});
	}

	// Ping pong between readback buffers
	ReadbackBufferIndex = (ReadbackBufferIndex + 1) % 2;
}