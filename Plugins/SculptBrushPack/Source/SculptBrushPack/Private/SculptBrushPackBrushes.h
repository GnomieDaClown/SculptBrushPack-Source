// Copyright (C) 2026 Joseph Wiginton.
// SPDX-License-Identifier: GPL-3.0-or-later
// Rules and constants of fourteen brushes follow Blender's sculpt source: Copyright Blender Authors, GPL-2.0-or-later.
//
// Sculpt Brush Pack. The brushes it adds to Unreal's Vertex Sculpt tools. The plugin's README.md says what each does
// and where its rule comes from.
//
// Each brush from Clay to Relax takes its displacement rule and constants from Blender's sculpt source, cited as
// blender/<path>:<line> in tag v5.2.3 unless another tag is named. Glide, Tube, Noise and Sharpen are this project's own
// designs; their rules are stated at each class. All run on Unreal's own stroke: the stamp, its strength and its falloff
// come from the sculpt tool (Sculpting/MeshBrushOpBase.h in 5.8.4). Positions are in the tool's local space, in which
// Vertex Sculpt has baked the object's scale.
//
// Several brushes ask the tool for the surface as it was before the stroke (EStampInputMeshType::BaseMesh) and write
// each point's place from there. They depend on the tool keeping that surface current between strokes; the module sees
// to that (FPackBrushOpFactory in SculptBrushPackModule.cpp).

#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "Sculpting/MeshBrushOpBase.h"
#include "SculptBrushPackSettings.h"
#include "SculptBrushPackBrushes.generated.h"

namespace SculptBrushPack
{
	/**
	 * Brush identifiers in the sculpt tools. The engine's own brushes use EMeshVertexSculptBrushType, below 20 in 5.8.4
	 * (MeshVertexSculptTool.h:59-118).
	 */
	namespace BrushID
	{
		inline constexpr int32 Clay = 2001;
		inline constexpr int32 ClayStrips = 2002;
		inline constexpr int32 Crease = 2003;
		inline constexpr int32 Blob = 2004;
		inline constexpr int32 DamStandard = 2005;
		inline constexpr int32 SnakeHook = 2006;
		inline constexpr int32 MoveTopological = 2007;
		inline constexpr int32 Layer = 2008;
		inline constexpr int32 Fill = 2009;
		inline constexpr int32 Scrape = 2010;
		inline constexpr int32 Nudge = 2011;
		inline constexpr int32 Thumb = 2012;
		inline constexpr int32 TopologySlide = 2013;
		inline constexpr int32 Relax = 2014;
		inline constexpr int32 Glide = 2015;
		inline constexpr int32 Tube = 2016;
		inline constexpr int32 Noise = 2017;
		inline constexpr int32 Sharpen = 2018;
		inline constexpr int32 LegacyWrinkle = 2019;
	}

	/** Sharpen's most rounds of averaging, the upper limit of its Detail Size: a stroke keeps one place per vertex and round. */
	inline constexpr int32 MaxSharpenRounds = 16;

	/**
	 * The strength of one stamp of a displacement brush as the engine's own brushes have it, without the radius and the
	 * direction: Power x DeltaTime x 6 (MeshSculptBrushOps.h:39 and :71 in 5.8.4). It stands where Blender's rules have
	 * the brush strength.
	 */
	double StampStrength(const FSculptBrushStamp& Stamp);

	/**
	 * Blender's distance for a square brush tip with rounded corners, in the tip's own units: 0 inside the square, rising
	 * to 1 at its rim, and 1 outside it (calc_brush_cube_distances, blender/source/blender/editors/sculpt_paint/mesh/sculpt.cc:7503-7534).
	 * Roundness 1 is a round tip, 0 a hard square.
	 */
	double TipDistance(double AbsX, double AbsY, double Roundness);
}

UCLASS()
class USculptBrushPackClayOpProps : public USculptBrushPackOpProps
{
	GENERATED_BODY()
public:
	/**
	 * How far above the surface under the cursor the clay's plane lies, as a fraction of the brush radius. Points move
	 * toward that plane: 0 only levels the surface, more builds it up. Ctrl puts the plane below the surface.
	 */
	UPROPERTY(EditAnywhere, Category = Clay, meta = (DisplayName = "Plane Offset", UIMin = "0.0", UIMax = "1.0", ClampMin = "0.0"))
	float PlaneOffset = 0.25f;
};

UCLASS()
class USculptBrushPackClayStripsOpProps : public USculptBrushPackOpProps
{
	GENERATED_BODY()
public:
	/** How far above the surface the strip's plane lies, as a fraction of the brush radius. Points below it are raised. */
	UPROPERTY(EditAnywhere, Category = ClayStrips, meta = (DisplayName = "Plane Offset", UIMin = "-1.0", UIMax = "1.0"))
	float PlaneOffset = 0.18f;

	/** Roundness of the square tip's corners: 0 is a hard square, 1 a round tip. */
	UPROPERTY(EditAnywhere, Category = ClayStrips, meta = (DisplayName = "Tip Roundness", UIMin = "0.0", UIMax = "1.0", ClampMin = "0.0", ClampMax = "1.0"))
	float TipRoundness = 0.18f;

	/**
	 * Length of the tip along the stroke, as a fraction of the brush radius. The tool hands a brush only the points within
	 * one radius of the cursor, so the tip cannot be longer than that.
	 */
	UPROPERTY(EditAnywhere, Category = ClayStrips, meta = (DisplayName = "Tip Scale", UIMin = "0.1", UIMax = "1.0", ClampMin = "0.01", ClampMax = "1.0"))
	float TipScale = 1.0f;

	/** Leave points deeper below the plane than Trim Depth alone. */
	UPROPERTY(EditAnywhere, Category = ClayStrips, meta = (DisplayName = "Plane Trim"))
	bool bPlaneTrim = false;

	/** The depth below the plane, as a fraction of the brush radius, past which points are left alone. */
	UPROPERTY(EditAnywhere, Category = ClayStrips, meta = (DisplayName = "Trim Depth", UIMin = "0.0", UIMax = "1.0", ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bPlaneTrim"))
	float PlaneTrim = 0.5f;
};

/** Settings of Crease, Blob and Dam Standard, which push along the surface normal and pinch or spread at once. */
UCLASS()
class USculptBrushPackCreaseOpProps : public USculptBrushPackOpProps
{
	GENERATED_BODY()
public:
	/** How strongly points are drawn toward the stroke (Crease, Dam Standard) or spread away from it (Blob). */
	UPROPERTY(EditAnywhere, Category = Pinch, meta = (DisplayName = "Pinch", UIMin = "0.0", UIMax = "1.0", ClampMin = "0.0", ClampMax = "1.0"))
	float Pinch = 0.5f;
};

UCLASS()
class USculptBrushPackBlobOpProps : public USculptBrushPackCreaseOpProps
{
	GENERATED_BODY()
};

UCLASS()
class USculptBrushPackDamStandardOpProps : public USculptBrushPackCreaseOpProps
{
	GENERATED_BODY()
public:
	/**
	 * On: the cut has a sharp profile of its own, steep at the middle and flat toward the rim, and the Falloff setting is
	 * not used. Off: the cut follows the tool's falloff.
	 */
	UPROPERTY(EditAnywhere, Category = Pinch, meta = (DisplayName = "Sharp Profile"))
	bool bSharpProfile = true;
};

UCLASS()
class USculptBrushPackSnakeHookOpProps : public USculptBrushPackOpProps
{
	GENERATED_BODY()
public:
	USculptBrushPackSnakeHookOpProps();

	/**
	 * Below 0.5 the pulled points are drawn toward the line of the stroke, above 0.5 they spread away from it; 0.5 does
	 * neither.
	 */
	UPROPERTY(EditAnywhere, Category = SnakeHook, meta = (DisplayName = "Pinch", UIMin = "0.0", UIMax = "1.0", ClampMin = "0.0", ClampMax = "1.0"))
	float Pinch = 0.5f;
};

UCLASS()
class USculptBrushPackMoveTopologicalOpProps : public USculptBrushPackOpProps
{
	GENERATED_BODY()
public:
	USculptBrushPackMoveTopologicalOpProps();
};

UCLASS()
class USculptBrushPackLayerOpProps : public USculptBrushPackOpProps
{
	GENERATED_BODY()
public:
	/**
	 * The layer's thickness: a stroke raises the surface by this much at most (lowers it with Ctrl). It is in the units
	 * the tool works in: world units (centimeters) in Vertex Sculpt, the mesh's own units in Sculpt Morph Target.
	 */
	UPROPERTY(EditAnywhere, Category = Layer, meta = (DisplayName = "Height", UIMin = "0.0", UIMax = "50.0", ClampMin = "0.0"))
	float Height = 5.0f;
};

/** Settings of Fill and Scrape, which move points toward a plane through the brushed area from one side of it. */
UCLASS(Abstract)
class USculptBrushPackPlaneSideOpProps : public USculptBrushPackOpProps
{
	GENERATED_BODY()
public:
	/** How far the plane lies above the middle of the brushed area, as a fraction of the brush radius. */
	UPROPERTY(EditAnywhere, Category = Plane, meta = (DisplayName = "Plane Offset", UIMin = "-1.0", UIMax = "1.0"))
	float PlaneOffset = 0.0f;

	/** How far above the plane the brush reaches, in brush radii: points up to there come down to it. 0 leaves them alone. */
	UPROPERTY(EditAnywhere, Category = Plane, meta = (DisplayName = "Height", UIMin = "0.0", UIMax = "1.0", ClampMin = "0.0", ClampMax = "1.0"))
	float Height = 0.0f;

	/** How far below the plane the brush reaches, in brush radii: points down to there come up to it. 0 leaves them alone. */
	UPROPERTY(EditAnywhere, Category = Plane, meta = (DisplayName = "Depth", UIMin = "0.0", UIMax = "1.0", ClampMin = "0.0", ClampMax = "1.0"))
	float Depth = 0.0f;
};

UCLASS()
class USculptBrushPackFillOpProps : public USculptBrushPackPlaneSideOpProps
{
	GENERATED_BODY()
public:
	USculptBrushPackFillOpProps();
};

UCLASS()
class USculptBrushPackScrapeOpProps : public USculptBrushPackPlaneSideOpProps
{
	GENERATED_BODY()
public:
	USculptBrushPackScrapeOpProps();
};

UCLASS()
class USculptBrushPackNudgeOpProps : public USculptBrushPackOpProps
{
	GENERATED_BODY()
public:
	USculptBrushPackNudgeOpProps();
};

UCLASS()
class USculptBrushPackThumbOpProps : public USculptBrushPackOpProps
{
	GENERATED_BODY()
public:
	USculptBrushPackThumbOpProps();
};

UCLASS()
class USculptBrushPackTopologySlideOpProps : public USculptBrushPackOpProps
{
	GENERATED_BODY()
};

UCLASS()
class USculptBrushPackRelaxOpProps : public USculptBrushPackOpProps
{
	GENERATED_BODY()
};

UCLASS()
class USculptBrushPackGlideOpProps : public USculptBrushPackOpProps
{
	GENERATED_BODY()
public:
	USculptBrushPackGlideOpProps();
};

/** Tube has a round profile of its own, so it has no falloff setting. */
UCLASS()
class USculptBrushPackTubeOpProps : public UMeshSculptBrushOpProps
{
	GENERATED_BODY()
public:
	/** Scales the bead's height; with a pen, pressing harder draws a thicker bead. */
	UPROPERTY(EditAnywhere, Category = Tube, meta = (DisplayName = "Strength", UIMin = "0.0", UIMax = "1.0", ClampMin = "0.0", ClampMax = "1.0", ModelingQuickEdit, ModelingQuickSettings = 200))
	float Strength = 1.0f;

	/**
	 * The bead's height at the middle of the brush, as a fraction of the brush radius: 1 is a half-round bead. It stops at
	 * 1: a point raised higher would leave the brush's reach, and a later stamp could no longer raise it.
	 */
	UPROPERTY(EditAnywhere, Category = Tube, meta = (DisplayName = "Height", UIMin = "0.0", UIMax = "1.0", ClampMin = "0.0", ClampMax = "1.0"))
	float Height = 0.5f;

	virtual float GetStrength() override { return Strength; }
	virtual void SetStrength(float NewStrength) override { Strength = FMath::Clamp(NewStrength, 0.0f, 1.0f); }
	virtual float GetFalloff() override { return 1.0f; }
	virtual bool SupportsStrengthPressure() override { return true; }
};

UCLASS()
class USculptBrushPackNoiseOpProps : public USculptBrushPackOpProps
{
	GENERATED_BODY()
public:
	USculptBrushPackNoiseOpProps();

	/**
	 * The most the noise raises or lowers the surface, in the units the tool works in: world units (centimeters) in
	 * Vertex Sculpt, the mesh's own units in Sculpt Morph Target.
	 */
	UPROPERTY(EditAnywhere, Category = Noise, meta = (DisplayName = "Height", UIMin = "0.0", UIMax = "10.0", ClampMin = "0.0"))
	float Height = 1.0f;

	/** The size of the noise's bumps, in the same units as Height. The pattern is fixed in space, whatever the brush size. */
	UPROPERTY(EditAnywhere, Category = Noise, meta = (DisplayName = "Scale", UIMin = "0.5", UIMax = "50.0", ClampMin = "0.01"))
	float Scale = 5.0f;
};

UCLASS()
class USculptBrushPackSharpenOpProps : public USculptBrushPackOpProps
{
	GENERATED_BODY()
public:
	USculptBrushPackSharpenOpProps();

	/** How much of each point's own relief is added at full reach: 1 doubles it. With Ctrl, 1 takes it away. */
	UPROPERTY(EditAnywhere, Category = Sharpen, meta = (DisplayName = "Amount", UIMin = "0.0", UIMax = "2.0", ClampMin = "0.0"))
	float Amount = 1.0f;

	/**
	 * How broad the detail is that counts as relief, in rounds of averaging each point with its neighbors: 1 picks out
	 * the finest detail, the mesh's own unevenness included; more rounds pick out broader forms. Read when a stroke begins.
	 */
	UPROPERTY(EditAnywhere, Category = Sharpen, meta = (DisplayName = "Detail Size", UIMin = "1", UIMax = "8", ClampMin = "1", ClampMax = "16"))
	int32 DetailSize = 4;
};

UCLASS()
class USculptBrushPackLegacyWrinkleOpProps : public USculptBrushPackOpProps
{
	GENERATED_BODY()
public:
	USculptBrushPackLegacyWrinkleOpProps();

	/**
	 * How strongly the lines are cut. The lines deepen for as long as the brush stays or passes again. With Ctrl the
	 * lines are raised as ridges instead.
	 */
	UPROPERTY(EditAnywhere, Category = LegacyWrinkle, meta = (DisplayName = "Depth", UIMin = "0.0", UIMax = "1.0", ClampMin = "0.0", ClampMax = "1.0"))
	float Depth = 0.25f;

	/** How many wrinkle lines the stroke lays side by side, along the way it moves. */
	UPROPERTY(EditAnywhere, Category = LegacyWrinkle, meta = (DisplayName = "Lines", UIMin = "1", UIMax = "9", ClampMin = "1", ClampMax = "9"))
	int32 Lines = 3;

	/** How far from the middle of the brush the outermost lines run, as a share of the brush radius. */
	UPROPERTY(EditAnywhere, Category = LegacyWrinkle, meta = (DisplayName = "Spread", UIMin = "0.0", UIMax = "1.0", ClampMin = "0.0", ClampMax = "1.0", EditCondition = "Lines > 1"))
	float Spread = 0.5f;

	/** The width of one line's cut, from its middle to its edge, as a share of the brush radius. */
	UPROPERTY(EditAnywhere, Category = LegacyWrinkle, meta = (DisplayName = "Line Width", UIMin = "0.02", UIMax = "0.5", ClampMin = "0.01", ClampMax = "1.0"))
	float LineWidth = 0.125f;

	/** How high the surface rises beside each cut, as a share of the cut's depth. At 0 the brush cuts without raising. */
	UPROPERTY(EditAnywhere, Category = LegacyWrinkle, meta = (DisplayName = "Lip", UIMin = "0.0", UIMax = "1.0", ClampMin = "0.0", ClampMax = "1.0"))
	float Lip = 0.25f;

	/** How strongly the points beside a line are drawn toward it, which tightens the cut. */
	UPROPERTY(EditAnywhere, Category = LegacyWrinkle, meta = (DisplayName = "Pinch", UIMin = "0.0", UIMax = "1.0", ClampMin = "0.0", ClampMax = "1.0"))
	float Pinch = 0.25f;
};

namespace SculptBrushPack
{
	/**
	 * Clay (blender/source/blender/editors/sculpt_paint/mesh/brushes/clay.cc:34-59, :155-188). A plane is laid through the
	 * cursor's point on the surface, raised along the average normal of the brushed area by Plane Offset radii (below the
	 * surface when inverted); every brushed point moves toward its nearest point on that plane, by the stamp's strength
	 * times Blender's factor for Clay (sculpt.cc:2353-2356) and the falloff.
	 */
	class FClayBrushOp : public FMeshSculptBrushOp
	{
	public:
		virtual void ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut) override;
		virtual bool WantsStampRegionPlane() const override { return true; }
		virtual bool UsesAlpha() const override { return true; }
		virtual bool SupportsStrokeType(EMeshSculptStrokeType StrokeType) const override { return IsKnownStrokeType(StrokeType); }
	};

	/**
	 * Clay Strips (clay_strips.cc:104-138, :140-194, :307-328, :457-513). A square tip is laid on a plane through the
	 * middle of the brushed area, raised by Plane Offset radii, with one side along the stroke. Points below the plane, to
	 * a depth of one radius, are raised along the normal: most at half that depth, not at all on the plane itself (the
	 * weight is depth x (1 - depth)), by the stamp's strength times Blender's factor for Clay Strips (sculpt.cc:2392-2395).
	 * The tool's falloff is read at the tip's own distance (0 inside the square, 1 at its rim). A stamp without movement
	 * does nothing, as the tip has no direction then.
	 */
	class FClayStripsBrushOp : public FMeshSculptBrushOp
	{
	public:
		virtual void BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices) override;
		virtual void ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut) override;
		virtual bool WantsStampRegionPlane() const override { return true; }
		virtual bool IgnoreZeroMovements() const override { return true; }
		virtual bool SupportsStrokeType(EMeshSculptStrokeType StrokeType) const override { return IsKnownStrokeType(StrokeType); }

	private:
		FVector3d LastOrigin = FVector3d::ZeroVector;
	};

	/**
	 * Crease (crease.cc:72-118 and its stroke function). Every brushed point is pushed along the average normal of the
	 * brushed area, into the surface unless inverted (Blender's Crease is set to subtract, blender v4.2.0
	 * source/blender/blenkernel/intern/brush.cc:1874-1876), by radius x stamp strength, and drawn toward the line through
	 * the cursor along that normal. The pinch has the stamp's strength with the Strength setting divided out and Pinch
	 * squared in its place, as in Blender.
	 */
	class FCreaseBrushOp : public FMeshSculptBrushOp
	{
	public:
		virtual void ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut) override;
		virtual bool WantsStampRegionPlane() const override { return true; }
		virtual bool UsesAlpha() const override { return true; }
		virtual bool SupportsStrokeType(EMeshSculptStrokeType StrokeType) const override { return IsKnownStrokeType(StrokeType); }

	protected:
		/** Blob: the pinch spreads, and the push is outward unless inverted (do_blob_brush, crease.cc:286-293). */
		bool bBlob = false;
	};

	class FBlobBrushOp : public FCreaseBrushOp
	{
	public:
		FBlobBrushOp() { bBlob = true; }
	};

	/**
	 * Dam Standard, ZBrush's name for a cut with pinched walls. Built from two of Blender's rules: Crease's push and pinch
	 * (crease.cc), weighted as Draw Sharp weights its points, by where each stood before the stroke (draw_sharp.cc:38-69),
	 * and with Draw Sharp's sharp profile, (1 - distance / radius) to the fourth power (blender v4.2.0 brush.cc:1827-1829;
	 * v5.2.3 source/blender/blenkernel/intern/brush.cc:1651-1652). It cuts into the surface unless inverted, and reads the
	 * surface as it was before the stroke, so the cut does not chase its own groove.
	 */
	class FDamStandardBrushOp : public FMeshSculptBrushOp
	{
	public:
		virtual void BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices) override;
		virtual void ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut) override;
		virtual ESculptBrushOpTargetType GetBrushTargetType() const override { return ESculptBrushOpTargetType::TargetMesh; }
		virtual EStampInputMeshType GetStampInputMeshType() const override { return EStampInputMeshType::BaseMesh; }
		virtual bool UsesAlpha() const override { return true; }
		virtual bool SupportsStrokeType(EMeshSculptStrokeType StrokeType) const override { return IsKnownStrokeType(StrokeType); }

	private:
		/** What the stroke has moved each vertex by so far; the mesh handed to ApplyStamp is the one from before the stroke. */
		TArray<FVector3d> Moved;
	};

	/**
	 * Snake Hook (snake_hook.cc:86-129, :160-223). The points under the brush follow each movement of the cursor, by the
	 * falloff times the strength, and the brush takes up whatever it passes over. Pinch draws the pulled points toward
	 * the line of the stroke or spreads them. Blender's rake rotation and its elastic variant are not built.
	 */
	class FSnakeHookBrushOp : public FMeshSculptBrushOp
	{
	public:
		virtual void BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices) override;
		virtual void ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut) override;
		virtual ESculptBrushOpTargetType GetBrushTargetType() const override { return ESculptBrushOpTargetType::ActivePlane; }
		virtual bool IgnoreZeroMovements() const override { return true; }

	private:
		FVector3d LastOrigin = FVector3d::ZeroVector;
	};

	/**
	 * Move Topological, ZBrush's name for a move that stays on the clicked piece of surface. Blender's Grab (grab.cc:58-94
	 * and its stroke function) with its topology limit (sculpt_automasking.cc:1117-1153, :142-158): the points move with
	 * the cursor, weighted by where they stood when the stroke began, and only points joined to the one under the cursor
	 * through edges that stay inside the brush move at all. The other lip of a closed mouth stays where it is.
	 */
	class FMoveTopologicalBrushOp : public FMeshSculptBrushOp
	{
	public:
		virtual void BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices) override;
		virtual void ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut) override;
		virtual ESculptBrushOpTargetType GetBrushTargetType() const override { return ESculptBrushOpTargetType::ActivePlane; }
		virtual bool IgnoreZeroMovements() const override { return true; }
		virtual EROIUpdateType GetROIUpdateType() const override { return EROIUpdateType::UseInitial; }
		virtual EStampInputMeshType GetStampInputMeshType() const override { return EStampInputMeshType::BaseMesh; }

		/** For tests: whether the stroke takes Vertex with it. */
		bool IsJoined(int32 Vertex) const { return Joined.IsValidIndex(Vertex) && Joined[Vertex]; }

	private:
		FSculptBrushStamp FirstStamp;
		TBitArray<> Joined;
	};

	/**
	 * Layer (blender/source/blender/editors/sculpt_paint/mesh/brushes/layer.cc:45-53, :74-105, :203-221). Each point keeps
	 * an amount for the stroke, between -1 and 1, which every stamp raises by weight x strength x (1.05 - |amount|); the
	 * point moves toward where it stood before the stroke plus Height x amount along its normal from then. However often
	 * the stroke passes, the surface rises by Height at most, an even layer. Blender's persistent base is not built.
	 */
	class FLayerBrushOp : public FMeshSculptBrushOp
	{
	public:
		virtual void BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices) override;
		virtual void ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut) override;
		virtual ESculptBrushOpTargetType GetBrushTargetType() const override { return ESculptBrushOpTargetType::TargetMesh; }
		virtual EStampInputMeshType GetStampInputMeshType() const override { return EStampInputMeshType::BaseMesh; }
		virtual bool UsesAlpha() const override { return true; }
		virtual bool SupportsStrokeType(EMeshSculptStrokeType StrokeType) const override { return IsKnownStrokeType(StrokeType); }

	private:
		void Resize(int32 MaxVertexID);

		/** Per vertex, for the stroke: the layer's amount, what the stroke has moved it by, and its normal before the stroke. */
		TArray<double> Amount;
		TArray<FVector3d> Moved;
		TArray<FVector3d> Normals;
		TArray<bool> bHasNormal;
	};

	/**
	 * Fill and Scrape, Blender's Plane brush with a reach on one side only (brushes/plane.cc:80-121, :133-154, :178-192,
	 * :378-419). A plane through the middle of the brushed area along its average normal; points within Height radii above
	 * it or Depth radii below it move toward it by their distance x strength, weighted by the falloff read at a distance
	 * that counts the height or depth as one radius. Fill reaches below only and Scrape above only; Ctrl swaps the two
	 * reaches. A stamp without movement does nothing, as in Blender.
	 */
	class FPlaneSideBrushOp : public FMeshSculptBrushOp
	{
	public:
		virtual void BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices) override;
		virtual void ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut) override;
		virtual bool WantsStampRegionPlane() const override { return true; }
		virtual bool IgnoreZeroMovements() const override { return true; }
		virtual bool SupportsStrokeType(EMeshSculptStrokeType StrokeType) const override { return IsKnownStrokeType(StrokeType); }

	private:
		FVector3d LastOrigin = FVector3d::ZeroVector;
	};

	/**
	 * Nudge (brushes/draw.cc:202-215). The points under the brush move with each movement of the cursor, but only by the
	 * part of it that lies along the surface (across the brushed area's average normal), times the strength and falloff.
	 */
	class FNudgeBrushOp : public FMeshSculptBrushOp
	{
	public:
		virtual void BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices) override;
		virtual void ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut) override;
		virtual bool WantsStampRegionPlane() const override { return true; }
		virtual bool IgnoreZeroMovements() const override { return true; }

	private:
		FVector3d LastOrigin = FVector3d::ZeroVector;
	};

	/**
	 * Thumb (brushes/thumb.cc:39-70, :134-146; the movement counts from the stroke's start, sculpt.cc:4170-4193). Like the
	 * engine's Move, on the points brushed when the stroke began and weighted by where they stood then, but only along the
	 * surface: the cursor's movement since the start without its part along the area's normal at the start.
	 */
	class FThumbBrushOp : public FMeshSculptBrushOp
	{
	public:
		virtual void BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices) override;
		virtual void ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut) override;
		virtual ESculptBrushOpTargetType GetBrushTargetType() const override { return ESculptBrushOpTargetType::ActivePlane; }
		virtual bool WantsStampRegionPlane() const override { return true; }
		virtual bool IgnoreZeroMovements() const override { return true; }
		virtual EROIUpdateType GetROIUpdateType() const override { return EROIUpdateType::UseInitial; }
		virtual EStampInputMeshType GetStampInputMeshType() const override { return EStampInputMeshType::BaseMesh; }

	private:
		FSculptBrushStamp FirstStamp;
		FVector3d Normal = FVector3d::UnitZ();
		bool bHasNormal = false;
	};

	/** What TopoSlide and Relax share: the mesh they are handed is the one from before the stroke, and they keep what the stroke has moved. */
	class FSurfaceMoveBrushOp : public FMeshSculptBrushOp
	{
	public:
		virtual void BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices) override;
		virtual ESculptBrushOpTargetType GetBrushTargetType() const override { return ESculptBrushOpTargetType::TargetMesh; }
		virtual EStampInputMeshType GetStampInputMeshType() const override { return EStampInputMeshType::BaseMesh; }
		virtual bool IgnoreZeroMovements() const override { return true; }
		virtual bool UsesAlpha() const override { return true; }

	protected:
		FVector3d Current(const FDynamicMesh3& Mesh, int32 Vertex) const { return Mesh.GetVertex(Vertex) + Moved[Vertex]; }

		TArray<FVector3d> Moved;
		FVector3d LastOrigin = FVector3d::ZeroVector;
	};

	/**
	 * TopoSlide, the drag of Blender's Slide Relax brush (brushes/topology_slide.cc:42-94, :157-204, :276-288; strength
	 * sculpt.cc:2383-2384). Each point moves toward those of its neighbors that lie in the direction the cursor moves:
	 * along each such edge by the edge's length in that direction, times twice the stamp's strength and the weight of
	 * where the point stood before the stroke. The points travel along the mesh's own edges, so the surface keeps its
	 * shape closely while its points shift.
	 */
	class FTopologySlideBrushOp : public FSurfaceMoveBrushOp
	{
	public:
		virtual void ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut) override;
	};

	/**
	 * Relax, the Shift mode of Blender's Slide Relax brush (brushes/relax.cc:469-503, :836-866; sculpt_smooth.cc:458-482,
	 * :511-582). Four times per stamp, each point moves toward the average of its neighbors, but only within its own
	 * tangent plane, by twice the stamp's strength times the weight: the spacing of the points evens out and the shape
	 * stays. A point with two neighbors or fewer stays; a point on an open border follows its border neighbors only.
	 */
	class FRelaxBrushOp : public FSurfaceMoveBrushOp
	{
	public:
		virtual void BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices) override;
		virtual void ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut) override;

	private:
		/** For a vertex in the stamp, its place in the stamp's list, else INDEX_NONE. */
		TArray<int32> SlotOfVertex;
	};

	/**
	 * Glide. A move that keeps the surface's shape: the points brushed when the stroke began
	 * follow each movement of the cursor, weighted by where they stood then, and after every stamp each is laid back onto
	 * the nearest point of the surface as it was before the stroke. Only the piece of surface joined to the point under
	 * the cursor moves, and the surface is kept for laying back within four brush radii of where the stroke began, the
	 * distance the engine's own brushes search for the surface (MeshSculptBrushOps.h:148 in 5.8.4); a point cannot glide
	 * past that. Beyond "joined by edges" it knows nothing of hidden surfaces or closed mouths.
	 */
	class FGlideBrushOp : public FMeshSculptBrushOp
	{
	public:
		virtual void BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices) override;
		virtual void ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut) override;
		virtual ESculptBrushOpTargetType GetBrushTargetType() const override { return ESculptBrushOpTargetType::ActivePlane; }
		virtual bool IgnoreZeroMovements() const override { return true; }
		virtual EROIUpdateType GetROIUpdateType() const override { return EROIUpdateType::UseInitial; }

		/** For tests: how many triangles of the surface from before the stroke are kept for laying points back. */
		int32 GetKeptTriangleCount() const { return Surface.IsValid() ? Surface->TriangleCount() : 0; }

	private:
		/** The piece of surface around the stroke's start, as it was then, and its search tree. */
		TUniquePtr<FDynamicMesh3> Surface;
		TUniquePtr<UE::Geometry::FDynamicMeshAABBTree3> SurfaceTree;
		/** Per vertex: its weight for the stroke; 0 for a point that does not move. */
		TArray<double> Weights;
		FVector3d LastOrigin = FVector3d::ZeroVector;
	};

	/**
	 * What Tube, Noise and Sharpen share: the result of a stroke depends on where the brush
	 * went and how hard, not on how long it stayed. Each point keeps its reach for the stroke, the largest any stamp has
	 * given it, and stands on its place from before the stroke plus reach x the brush's full height for that point, along
	 * its normal from before the stroke. A second pass, or holding still, adds nothing.
	 *
	 * The tool hands a stamp the points that lie within the brush now, after they have moved (MeshVertexSculptTool.cpp:736-745
	 * in 5.8.4), so a point raised farther than the brush radius is not reached again in the same stroke.
	 */
	class FStrokeReachBrushOp : public FMeshSculptBrushOp
	{
	public:
		virtual void BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices) override;
		virtual void ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut) override;
		virtual ESculptBrushOpTargetType GetBrushTargetType() const override { return ESculptBrushOpTargetType::TargetMesh; }
		virtual EStampInputMeshType GetStampInputMeshType() const override { return EStampInputMeshType::BaseMesh; }
		virtual bool SupportsStrokeType(EMeshSculptStrokeType StrokeType) const override { return IsKnownStrokeType(StrokeType); }

	protected:
		/**
		 * Called once per stamp, before its points are moved and not in parallel: reads the settings and works out what
		 * the points share. Mesh is the mesh from before the stroke.
		 */
		virtual void PrepareStamp(const FDynamicMesh3& Mesh, const TArray<int32>& Vertices) = 0;
		/** What this stamp gives a point that stood at Before. Called in parallel. */
		virtual double StampReach(const FSculptBrushStamp& Stamp, const FVector3d& Before) const = 0;
		/** The signed height, along Normal, of a point at full reach (a reach of 1). Called in parallel. */
		virtual double FullHeight(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Normal) const = 0;

	private:
		void Resize(int32 MaxVertexID);

		TArray<double> Reach;
		TArray<FVector3d> Normals;
		TArray<bool> bHasNormal;
	};

	/**
	 * Tube, the pack's own. A raised bead along the stroke with a round cross-section: a stamp reaches a point by
	 * radius x Height x strength x sqrt(1 - (distance / radius) squared), a half circle across the brush, and the point
	 * keeps the largest. Ctrl cuts a round channel instead. With a spaced stroke it draws a row of beads.
	 */
	class FTubeBrushOp : public FStrokeReachBrushOp
	{
	protected:
		virtual void PrepareStamp(const FDynamicMesh3& Mesh, const TArray<int32>& Vertices) override;
		virtual double StampReach(const FSculptBrushStamp& Stamp, const FVector3d& Before) const override;
		virtual double FullHeight(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Normal) const override;

	private:
		double Height = 0.0;
	};

	/**
	 * Noise, the pack's own. A stamp reaches a point by falloff x strength; at full reach the point stands
	 * Height x noise(place before the stroke / Scale) off the surface, where noise is the engine's 3D Perlin noise, from -1
	 * to 1 (FMath::PerlinNoise3D). The pattern is fixed in the mesh's space, so strokes of any size reveal the same bumps
	 * and passing twice does not pile them up. Ctrl turns the bumps over.
	 */
	class FNoiseBrushOp : public FStrokeReachBrushOp
	{
	public:
		virtual bool UsesAlpha() const override { return true; }

	protected:
		virtual void PrepareStamp(const FDynamicMesh3& Mesh, const TArray<int32>& Vertices) override;
		virtual double StampReach(const FSculptBrushStamp& Stamp, const FVector3d& Before) const override;
		virtual double FullHeight(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Normal) const override;

	private:
		double Height = 0.0;
		double Scale = 1.0;
	};

	/**
	 * Sharpen, the pack's own. A point's relief is how far it stood, before the stroke, from where Detail Size rounds of
	 * averaging would put it, measured along its normal; one round puts every point at the average of itself and its
	 * neighbors. A stamp reaches a point by falloff x strength; at full reach the point has Amount x its relief more
	 * (Ctrl: less, down to none of it at an Amount of 1). Bumps grow and hollows deepen, by a set share at most, so a
	 * stroke cannot run away as sharpening stamp upon stamp would.
	 */
	class FSharpenBrushOp : public FStrokeReachBrushOp
	{
	public:
		virtual void BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices) override;
		virtual bool UsesAlpha() const override { return true; }

	protected:
		virtual void PrepareStamp(const FDynamicMesh3& Mesh, const TArray<int32>& Vertices) override;
		virtual double StampReach(const FSculptBrushStamp& Stamp, const FVector3d& Before) const override;
		virtual double FullHeight(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Normal) const override;

	private:
		/** A point's place after RoundCount rounds of averaging on the mesh from before the stroke. */
		FVector3d Averaged(const FDynamicMesh3& Mesh, int32 Vertex, int32 RoundCount);
		void ResizeRelief(int32 MaxVertexID);

		struct FRound
		{
			TArray<FVector3d> Positions;
			TArray<bool> bKnown;
		};
		/** Rounds[r] holds the places after r + 1 rounds, for the points worked out so far in this stroke. */
		TArray<FRound> Rounds;
		/** Per vertex, once worked out: its place before the stroke less its averaged place. */
		TArray<FVector3d> Relief;
		TArray<bool> bHasRelief;
		double Amount = 0.0;
	};

	/**
	 * Legacy Wrinkle, the pack's own: a wrinkle brush of the familiar kind. It lays Lines wrinkle lines side by side along
	 * the way the stroke moves, and they deepen for as long as the brush stays or passes again.
	 *
	 * A stamp moves a point along the stamp's normal by radius x Depth x strength x weight x profile. Strength is the
	 * engine's own for a stamp, power x time x 6. Weight is the tool's falloff, times its alpha, where the point stood
	 * before the stroke. Profile is the cross-section at the point's place across the stroke: with d the distance to the
	 * nearest line as a share of Line Width, -(1 - d) squared inside the line (a cut, sharp at its middle),
	 * Lip x (1 - |2d - 3|) from d = 1 to 2 (the surface pushed up beside the cut), and nothing beyond. A single line runs
	 * through the middle of the brush; several are spread evenly from -Spread to Spread of the radius. Points within
	 * twice Line Width of their line are also drawn sideways toward it, by Pinch x strength x weight of their distance,
	 * each stamp. Ctrl turns the cuts into ridges.
	 *
	 * The way the stroke moves is the stamp's place less that of the stamp before it, with what lies along the normal
	 * taken out. The first stamp of a stroke has not moved and leaves no mark: the brush draws when it is dragged.
	 */
	class FLegacyWrinkleBrushOp : public FMeshSculptBrushOp
	{
	public:
		virtual void BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices) override;
		virtual void ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut) override;
		virtual ESculptBrushOpTargetType GetBrushTargetType() const override { return ESculptBrushOpTargetType::TargetMesh; }
		virtual EStampInputMeshType GetStampInputMeshType() const override { return EStampInputMeshType::BaseMesh; }
		virtual bool UsesAlpha() const override { return true; }
		virtual bool SupportsStrokeType(EMeshSculptStrokeType StrokeType) const override { return IsKnownStrokeType(StrokeType); }

		/**
		 * The cross-section's height at Across, a place across the stroke as a share of the radius: negative in a cut,
		 * positive on a lip. OutLinePlace is the place of the line the point belongs to, in the same measure.
		 */
		static double Profile(double Across, int32 Lines, double Spread, double LineWidth, double Lip, double& OutLinePlace);

	private:
		/** What the stroke has moved each vertex by so far; the mesh handed to ApplyStamp is the one from before the stroke. */
		TArray<FVector3d> Moved;
		/** The way the stroke last moved, of length 1; zero until it has moved. */
		FVector3d Along = FVector3d::ZeroVector;
	};
}
