// Copyright (C) 2026 Joseph Wiginton.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sculpt Brush Pack: Morph Targets. A second plugin beside the Sculpt Brush Pack: brushes for the engine's experimental
// morph target sculpt tool in the Skeletal Mesh Editor. The pack itself stays free of that tool's plugin.

using UnrealBuildTool;

public class SculptBrushPackMorph : ModuleRules
{
	public SculptBrushPackMorph(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// The tests compare the brushes' results with hand-worked values exactly. That needs the compiler to round each
		// operation as written, which is this setting; it is also what current build settings give an editor module.
		FPSemantics = FPSemanticsMode.Precise;

		// The brushes derive from the sculpt tools' brush operation classes (MeshModelingTools) and their settings from
		// the pack's (SculptBrushPack).
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"GeometryCore",
				"InteractiveToolsFramework",
				"MeshModelingTools",
				"SculptBrushPack",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"DynamicMesh",
				"Engine",
				"GeometryFramework",
				"ModelingComponents",
				"Projects",
				// The engine's morph target sculpt tool, whose state the brushes read; its header includes MeshDescription's.
				// The posing helpers and the editor's context object are in MeshModelingToolsEditorOnly.
				"SkeletalMeshMorphTargetEditingTools",
				"MeshModelingToolsEditorOnly",
				"MeshDescription",
				// The tests only: they open the Skeletal Mesh Editor (UnrealEd), press its Editing Tools button when the tools
				// are off (Slate), and give a copy of the engine's skeletal cube a morph target through
				// FSkeletalMeshAttributes (SkeletalMeshDescription).
				"SkeletalMeshDescription",
				"Slate",
				"UnrealEd",
			}
		);
	}
}
