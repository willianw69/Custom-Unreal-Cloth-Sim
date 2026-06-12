// Copyright Epic Games, Inc. All Rights Reserved.

#include "ClothSceneViewExtension.h"

#include "FXRenderingUtils.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "Containers/StridedView.h"

// Render-thread-only cache. The view extension (render thread) writes it and the
// cloth dispatch (render thread) reads it, so no synchronization is needed.
static FClothGDFCache GClothGDFCache;

// Held for the lifetime of the registration.
static TSharedPtr<FClothSceneViewExtension, ESPMode::ThreadSafe> GClothViewExtension;

void ClothGDF::EnsureRegistered()
{
	check(IsInGameThread());
	if (!GClothViewExtension.IsValid())
	{
		GClothViewExtension = FSceneViewExtensions::NewExtension<FClothSceneViewExtension>();
	}
}

const FClothGDFCache& ClothGDF::Get()
{
	return GClothGDFCache;
}

FClothSceneViewExtension::FClothSceneViewExtension(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
}

void FClothSceneViewExtension::PostRenderBasePassDeferred_RenderThread(
	FRDGBuilder& GraphBuilder,
	FSceneView& InView,
	const FRenderTargetBindingSlots& RenderTargets,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
{
	// The GDF parameter data is shared across the scene's views; one view is enough.
	const TConstStridedView<FSceneView> Views = MakeStridedView(0, &InView, 1);
	const FGlobalDistanceFieldParameterData* Data = UE::FXRenderingUtils::GetGlobalDistanceFieldParameterData(Views);

	if (Data && Data->NumGlobalSDFClipmaps > 0 && InView.ViewUniformBuffer.IsValid())
	{
		GClothGDFCache.Data = *Data;
		GClothGDFCache.PreViewTranslation = FVector3f(InView.ViewMatrices.GetPreViewTranslation());
		GClothGDFCache.ViewUniformBuffer = InView.ViewUniformBuffer;
		GClothGDFCache.bValid = true;
	}
	else
	{
		GClothGDFCache.bValid = false;
		GClothGDFCache.ViewUniformBuffer.SafeRelease();
	}
}
