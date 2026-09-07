// Copyright (c) 2026 Tesseract Softwares SpA. Praxsuite Open SDK Licence.

using UnrealBuildTool;

public class Praxsuite : ModuleRules
{
	public Praxsuite(ReadOnlyTargetRules Target) : base(Target)
	{
		// The core under Private/Core and Public/Core is portable C++17 that includes no engine
		// headers, which is what lets CI compile and test it without Unreal. Unreal's default
		// build settings work against that in two ways, so both are turned off for this module.
		//
		// Shared PCHs force CoreMinimal.h into every translation unit. A file that compiles only
		// because a PCH silently supplied the engine to it is not a portable file, and the day CI
		// stops being able to build it, nobody finds out from the engine side.
		PCHUsage = ModuleRules.PCHUsageMode.NoSharedPCHs;

		// Unity builds concatenate sources, so an include missing from one file is masked by its
		// neighbour. That is exactly the mistake CI's separate compilation would catch and Unreal's
		// would hide - and a consumer building non-unity then gets an error we never saw.
		bUseUnity = false;

		// C++17, matching the core. Unreal 5.3 defaults to C++17 anyway; stating it means the module
		// does not silently change language version when the engine's default moves.
		CppStandard = CppStandardVersion.Cpp17;

		PublicIncludePaths.AddRange(new string[]
		{
			// So a consumer can include "Core/PraxJson.h" and reach the portable layer directly if
			// they want typed access rather than the Blueprint-facing conversions.
			ModuleDirectory + "/Public",
		});

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			// HTTP is the transport. Deliberately NOT the Json module: the SDK bundles its own codec
			// so the core stays engine-free and testable. See Public/Core/PraxJson.h.
			"HTTP",
			// UDeveloperSettings, so workspace configuration lives in Project Settings rather than
			// being hardcoded in a Blueprint someone forgets to change before shipping.
			"DeveloperSettings",
			// The Event Bus. An engine module, not a third-party package - it ships with Unreal,
			// so declaring it costs a consumer nothing. The bus half of the SDK is otherwise
			// unreachable: SignalR runs over a WebSocket and HTTP cannot carry it.
			"WebSockets",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
		});
	}
}
