// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class ClothSim : ModuleRules
{
	public ClothSim(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects",     // IPluginManager, to locate our Shaders/ directory
			"RenderCore",   // FGlobalShader, RDG (FRDGBuilder), FComputeShaderUtils
			"RHI"           // FRHIGPUBufferReadback, buffer descriptors
		});
	}
}
