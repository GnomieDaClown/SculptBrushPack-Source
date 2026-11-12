// Copyright (C) 2026 Joseph Wiginton.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sculpt Brush Pack: Morph Targets. What the module offers besides the brushes themselves (SculptBrushPackMorphBrushes.h).

#pragma once

#include "CoreMinimal.h"

class UInteractiveTool;

namespace SculptBrushPackMorph
{
	/**
	 * Identifiers of the morph brushes a Sculpt Morph Target tool started now would get, in the order the brush palette
	 * lists them, after the Sculpt Brush Pack's own: Amplify Morph, Smooth Morph, Mush Morph, Wrinkle Morph, Clone Morph
	 * and Mirror Morph; the last two only while the console variable SculptBrushPackMorph.CloneAndMirror is on, as it is
	 * unless switched off. Call on the game thread.
	 */
	SCULPTBRUSHPACKMORPH_API TArray<int32> GetBrushIdentifiers();

	/** Whether the tool is the engine's Sculpt Morph Target tool, the one tool these brushes are added to. */
	SCULPTBRUSHPACKMORPH_API bool IsMorphSculptTool(const UInteractiveTool* Tool);
}
