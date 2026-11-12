// Copyright (C) 2026 Joseph Wiginton.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sculpt Brush Pack. Editor target of its host project: a project target, so only the enabled plugins' modules are built
// and nothing is written into the engine folder.

using UnrealBuildTool;

public class BrushPackHost58EditorTarget : TargetRules
{
	public BrushPackHost58EditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
	}
}
