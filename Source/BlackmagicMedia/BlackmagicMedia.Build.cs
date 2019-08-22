// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

using System.IO;

namespace UnrealBuildTool.Rules
{
	using System.IO;

	public class BlackmagicMedia : ModuleRules
	{
		public BlackmagicMedia(ReadOnlyTargetRules Target) : base(Target)
		{
			PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

			DynamicallyLoadedModuleNames.AddRange(
				new string[] {
					"Media",
				});

			PublicDependencyModuleNames.AddRange(
				new string[] {
					"Core",
					"CoreUObject",
					"MediaAssets",
					"MediaIOCore",
					"TimeManagement",
				});

			PrivateDependencyModuleNames.AddRange(
				new string[] {
					"Blackmagic",
					"Engine",
					"MediaUtils",
					"Projects",
                    "UtilityShaders",
                });

			PrivateIncludePathModuleNames.AddRange(
				new string[] {
					"Media",
				});

			PrivateIncludePaths.AddRange(
				new string[] {
					"BlackmagicMedia/Private",
					"BlackmagicMedia/Private/Blackmagic",
					"BlackmagicMedia/Private/Assets",
					"BlackmagicMedia/Private/Player",
					"BlackmagicMedia/Private/Shared",
				});
		}
	}
}
