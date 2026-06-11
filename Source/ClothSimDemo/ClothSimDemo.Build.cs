// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class ClothSimDemo : ModuleRules
{
	public ClothSimDemo(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore"
		});

		// The cloth simulation lives entirely in the ClothSim plugin.
		PrivateDependencyModuleNames.AddRange(new string[] { });
	}
}
