// Copyright (C) 2026 Joseph Wiginton.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sculpt Brush Pack. Game target of its host project; not built by the Tools scripts.

using UnrealBuildTool;

public class BrushPackHost58Target : TargetRules
{
	public BrushPackHost58Target(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
	}
}
