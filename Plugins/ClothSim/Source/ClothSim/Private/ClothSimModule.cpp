// Copyright Epic Games, Inc. All Rights Reserved.

#include "ClothSimModule.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h" // AddShaderSourceDirectoryMapping

#define LOCTEXT_NAMESPACE "FClothSimModule"

void FClothSimModule::StartupModule()
{
	// Map the virtual shader directory "/ClothSim" -> <Plugin>/Shaders.
	// In .usf/.cpp we then reference shaders as "/ClothSim/ClothIntegrate.usf".
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ClothSim"));
	check(Plugin.IsValid());

	const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/ClothSim"), ShaderDir);
}

void FClothSimModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FClothSimModule, ClothSim)
