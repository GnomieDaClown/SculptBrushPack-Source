// Copyright (C) 2026 Joseph Wiginton.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sculpt Brush Pack. An editor plugin for Unreal Engine 5.9 that depends on engine modules only.

using UnrealBuildTool;

public class SculptBrushPack : ModuleRules
{
	public SculptBrushPack(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// The tests compare the brushes' results with hand-worked values exactly. That needs the compiler to round each
		// operation as written, which is this setting; it is also what current build settings give an editor module.
		FPSemantics = FPSemanticsMode.Precise;

		// The brushes derive from the sculpt tools' brush operation classes (MeshModelingTools).
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"GeometryCore",
				"InteractiveToolsFramework",
				"MeshModelingTools",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Engine",
				"Projects",
				"Slate",
				"SlateCore",
				// The level editor's mode manager and the editor's start-up delegate; the tests also start the tools
				// through it, in Modeling Mode and in the Skeletal Mesh Editor.
				"UnrealEd",
				// The tests only: the dynamic mesh component they sculpt.
				"GeometryFramework",
			}
		);
	}
}
