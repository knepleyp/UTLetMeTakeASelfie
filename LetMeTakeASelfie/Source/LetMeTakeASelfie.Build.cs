// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

using System.IO;

namespace UnrealBuildTool.Rules
{
	public class LetMeTakeASelfie : ModuleRules
	{
        public LetMeTakeASelfie(TargetInfo Target)
        {
			PublicDependencyModuleNames.AddRange(
				new string[]
				{
					"Core",
					"CoreUObject",
                    "Engine",
                    "UnrealTournament",
					"InputCore",
					"Slate",
					"SlateCore",
					"ShaderCore",
					"RenderCore",
					"RHI"
				}
				);

            var LIBPath = Path.Combine("..", "..", "UnrealTournament", "Plugins", "LetMeTakeASelfie", "Source", "lib");

            //var GDLibPath = Path.Combine(LIBPath, "libgd.lib");
            var VPXLibPath = Path.Combine(LIBPath, "vpxmd.lib");
            //var VPXLibPath = Path.Combine(LIBPath, "vpxmdd.lib");
            
			// Lib file
            PublicLibraryPaths.Add(LIBPath);
            PublicAdditionalLibraries.Add(VPXLibPath);
		}
	}
}