// Copyright (C) 2026 Joseph Wiginton.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sculpt Brush Pack. What the module offers besides the brushes themselves (SculptBrushPackBrushes.h).

#pragma once

#include "CoreMinimal.h"
#include "Sculpting/MeshBrushOpBase.h"

class UInteractiveTool;
class UMeshVertexSculptTool;

namespace SculptBrushPack
{
	/** Identifiers of the pack's brushes, in the order the brush palette lists them. */
	SCULPTBRUSHPACK_API TArray<int32> GetBrushIdentifiers();

	/** Whether the pack adds its brushes to this tool: a Vertex Sculpt tool of one of the classes it knows. */
	SCULPTBRUSHPACK_API bool IsSupportedTool(const UInteractiveTool* Tool);

	/**
	 * Starts listening to every tool manager that exists now. The module does this itself on the frame after a tool
	 * manager is made; code that makes one and starts a tool in the same frame calls this in between. Game thread only.
	 */
	SCULPTBRUSHPACK_API void WatchToolManagers();

	/** A brush that another module adds, through the pack, to the sculpt tools it chooses. */
	struct FExtraBrush
	{
		/**
		 * Its number in the tool. The engine's own are below 20 and the pack's are 2001 and up; a brush whose number is
		 * below 1000, or in use on the tool, is left out with a warning in the log.
		 */
		int32 Identifier = 0;
		/** The palette's label. Its source string names the icon, as for the pack's own brushes. */
		FText Name;
		/** The icon's SVG file, with its full path. */
		FString IconFile;
		/** Makes the brush's settings object for the tool, with the tool as its outer. The tool keeps it alive. */
		TFunction<UMeshSculptBrushOpProps*(UMeshVertexSculptTool&)> MakeSettings;
		/** Makes the factory of the brush's operation. The tool owns the factory and the operations it builds. */
		TFunction<TUniquePtr<FMeshSculptBrushOpFactory>()> MakeFactory;
	};

	/**
	 * Called on the game thread for each sculpt tool the pack adds its brushes to, after the tool is built and before its
	 * Setup: the tool has no mesh, no settings and no brushes of its own yet, so a provider can tell what kind of tool it
	 * is and no more. What it puts in OutBrushes is added after the pack's own brushes, in that order. A provider that
	 * has nothing for a tool leaves the list as it is.
	 */
	using FExtraBrushProvider = TFunction<void(UMeshVertexSculptTool& Tool, TArray<FExtraBrush>& OutBrushes)>;

	/**
	 * Adds a provider under a name of its own, or replaces the one of that name. Providers are asked in the order added.
	 * Game thread only. A module that adds a provider removes it when it shuts down.
	 */
	SCULPTBRUSHPACK_API void AddBrushProvider(FName ProviderName, FExtraBrushProvider Provider);
	SCULPTBRUSHPACK_API void RemoveBrushProvider(FName ProviderName);
}
