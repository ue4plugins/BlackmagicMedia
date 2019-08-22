// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class BlackmagicMediaOutput : ModuleRules
	{
		public BlackmagicMediaOutput(ReadOnlyTargetRules Target) : base(Target)
		{
			PublicDependencyModuleNames.AddRange(
				new string[] {
					"BlackmagicMedia",
					"MediaIOCore",
				});

			PrivateIncludePaths.AddRange(
				new string[]
				{
					"BlackmagicMediaOutput/Private",
				}
			);

			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"Blackmagic",
					"Core",
					"CoreUObject",
					"Engine",
					"RenderCore",
					"RHI",
					"Slate",
				}
            );
		}
	}
}
