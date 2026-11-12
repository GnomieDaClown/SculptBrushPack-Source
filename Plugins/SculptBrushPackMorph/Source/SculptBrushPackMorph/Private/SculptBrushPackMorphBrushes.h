// Copyright (C) 2026 Joseph Wiginton.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sculpt Brush Pack: Morph Targets. Brushes that edit a morph target's difference from the base, for the engine's
// Sculpt Morph Target tool in the Skeletal Mesh Editor. The plugin's README.md says how each is used. Engine anchors
// are for 5.9.0.
//
// The tool shows the mesh as posed, with every active morph target applied, and works the edited morph's deltas out of
// the sculpted mesh when a stroke ends (MorphTargetVertexSculptTool.cpp:540-623). So these brushes, like any other, only
// move the points of the shown mesh; what makes them morph brushes is that each knows where a point stands without the
// edited morph. A point's offset is its place less that place: what the edited morph adds there, as shown.

#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Sculpting/MeshBrushOpBase.h"
#include "SculptBrushPackSettings.h"

#include <atomic>

#include "SculptBrushPackMorphBrushes.generated.h"

namespace SculptBrushPackMorph
{
	/** Brush identifiers in the tool, after the Sculpt Brush Pack's own (2001 and up). */
	namespace BrushID
	{
		inline constexpr int32 AmplifyMorph = 2101;
		inline constexpr int32 SmoothMorph = 2102;
		inline constexpr int32 CloneMorph = 2103;
		inline constexpr int32 MirrorMorph = 2104;
	}

	/** What the mesh's symmetry gives the mirror brush for one vertex. The symmetry plane is X = 0 in the reference pose. */
	enum class EMorphMirror : uint8
	{
		/** Nothing: the mesh's symmetry is not known, or the vertex has no partner across the plane and is not on it. */
		None,
		/** The vertex lies on the plane. The offset given is the part of its own offset that lies in the plane. */
		OnPlane,
		/** The vertex is on the +X side. The offset given is its partner's, mirrored. */
		PositiveX,
		/** The vertex is on the -X side. The offset given is its partner's, mirrored. */
		NegativeX,
	};

	/**
	 * What a morph brush knows, for one stroke, of the morph target it edits. Places and offsets are those of the mesh as
	 * the tool shows it, in the sculpt mesh's vertex numbering. The module implements it over the engine's tool; the rule
	 * tests implement it by hand.
	 *
	 * Threading: a brush calls BeginStroke, EndStroke, GetMeshWithoutMorph, PrepareVertices, GetSourceNames,
	 * IsToolSymmetryOn and ReportProblem from the game thread only. GetSourceOffset and GetMirroredOffset are called from worker threads, for the vertices of the
	 * last PrepareVertices call, and must not change the context.
	 */
	class IMorphBrushContext
	{
	public:
		virtual ~IMorphBrushContext() = default;

		/**
		 * Called when a stroke begins; bMirror says whether mirrored offsets will be asked for. False when the morph
		 * cannot be edited now, for instance because its weight is zero, at which the tool drops the stroke
		 * (MorphTargetVertexSculptTool.cpp:555-559). The brush then moves nothing, and the context has told the user why
		 * where there is something the user can do about it.
		 */
		virtual bool BeginStroke(bool bMirror) = 0;

		/** Called when the stroke ends or is cancelled: what the context held of the tool for the stroke is let go. */
		virtual void EndStroke() {}

		/** The shown mesh without the edited morph, or null when not known. Called once per stamp. */
		virtual const FDynamicMesh3* GetMeshWithoutMorph() = 0;

		/** Called once per stamp, before the offsets below are asked for these vertices. */
		virtual void PrepareVertices(const TArray<int32>& Vertices) {}

		/** The offset a vertex would have if the edited morph had Source's shape there. False when not known. */
		virtual bool GetSourceOffset(FName Source, int32 Vertex, FVector3d& OutOffset) const = 0;

		/**
		 * The offset a vertex would have if the edited morph had there the mirror image of what it has across the mesh's
		 * symmetry plane, as it was when the stroke began. OutOffset is set unless None is returned.
		 */
		virtual EMorphMirror GetMirroredOffset(int32 Vertex, FVector3d& OutOffset) const = 0;

		/** The other morph targets of the mesh, which a brush can take a shape from. */
		virtual TArray<FName> GetSourceNames() const = 0;

		/**
		 * Whether the tool's own Symmetry setting is in force. With it on, the tool moves every stamp to the +X side and
		 * then sets the -X side from the +X side at full strength, whatever the brush did there
		 * (MeshVertexSculptTool.cpp:1059-1061, :1167-1185).
		 */
		virtual bool IsToolSymmetryOn() const { return false; }

		/**
		 * Tells the user why a stroke does nothing, or what to know about what it does. An empty text says that what was
		 * told last no longer holds.
		 */
		virtual void ReportProblem(const FText& Problem) {}
	};
}

UCLASS()
class USculptBrushPackAmplifyMorphOpProps : public USculptBrushPackOpProps
{
	GENERATED_BODY()
public:
	USculptBrushPackAmplifyMorphOpProps();

	/**
	 * How much stronger the morph becomes where the brush reaches fully: 0.5 makes it half again as strong. With Ctrl
	 * the morph becomes that much weaker instead, down to nothing at 1.
	 */
	UPROPERTY(EditAnywhere, Category = AmplifyMorph, meta = (DisplayName = "Amount", UIMin = "0.0", UIMax = "2.0", ClampMin = "0.0"))
	float Amount = 0.5f;
};

UCLASS()
class USculptBrushPackSmoothMorphOpProps : public USculptBrushPackOpProps
{
	GENERATED_BODY()
public:
	USculptBrushPackSmoothMorphOpProps();

	/**
	 * How far the smoothing spreads: the number of times each point's share of the morph is averaged with its
	 * neighbors'. More rounds smooth more broadly. Read when a stroke begins.
	 */
	UPROPERTY(EditAnywhere, Category = SmoothMorph, meta = (DisplayName = "Smoothing", UIMin = "1", UIMax = "8", ClampMin = "1", ClampMax = "16"))
	int32 Smoothing = 2;
};

UCLASS()
class USculptBrushPackCloneMorphOpProps : public USculptBrushPackOpProps
{
	GENERATED_BODY()
public:
	USculptBrushPackCloneMorphOpProps();

	/**
	 * The morph target to copy from. The brush paints its shape into the morph target being edited. When the mesh has
	 * only one other morph target, that one is used. The choice is not kept between sessions of the tool, as another mesh
	 * has other morph targets.
	 */
	UPROPERTY(EditAnywhere, Category = CloneMorph, meta = (DisplayName = "Source", GetOptions = GetSourceOptions, NoResetToDefault, TransientToolProperty))
	FName Source = NAME_None;

	/** How much of the source's shape is painted in: 1 is all of it, 0.5 half as strong. */
	UPROPERTY(EditAnywhere, Category = CloneMorph, meta = (DisplayName = "Amount", UIMin = "0.0", UIMax = "2.0", ClampMin = "0.0"))
	float Amount = 1.0f;

	/**
	 * Off: where the brush reaches fully, the edited morph takes the source's shape in place of its own. On: the source's
	 * shape is added on top of what the edited morph has there. With Ctrl the source's shape is taken out instead.
	 */
	UPROPERTY(EditAnywhere, Category = CloneMorph, meta = (DisplayName = "Add To Shape"))
	bool bAdd = false;

	/** The names the Source setting offers. */
	UFUNCTION()
	TArray<FName> GetSourceOptions() const;

	/** Gives the morph targets to choose from. Set by whoever makes the settings for a tool; unset, the list is empty. */
	TFunction<TArray<FName>()> SourceNames;
};

/** Which side of the mesh Mirror Morph copies from. The mesh's symmetry plane is X = 0. */
UENUM()
enum class ESculptBrushPackMirrorSource : uint8
{
	/** Every brushed point takes the shape of the point across from it: brush the side that should receive the shape. */
	OtherSide UMETA(DisplayName = "Other Side"),
	/** Only points on the -X side change: they take the +X side's shape. Brushing the +X side changes nothing. */
	PositiveX UMETA(DisplayName = "+X Side"),
	/** Only points on the +X side change: they take the -X side's shape. Brushing the -X side changes nothing. */
	NegativeX UMETA(DisplayName = "-X Side"),
};

UCLASS()
class USculptBrushPackMirrorMorphOpProps : public USculptBrushPackOpProps
{
	GENERATED_BODY()
public:
	USculptBrushPackMirrorMorphOpProps();

	/**
	 * Which side the brushed points take their shape from. Other Side: each brushed point takes the shape of the point
	 * across the mesh's symmetry plane, so brush the side that should receive. +X Side or -X Side: only points on the
	 * opposite side change, so the side copied from cannot be brushed away by mistake.
	 */
	UPROPERTY(EditAnywhere, Category = MirrorMorph, meta = (DisplayName = "Copy From"))
	ESculptBrushPackMirrorSource CopyFrom = ESculptBrushPackMirrorSource::OtherSide;
};

namespace SculptBrushPackMorph
{
	/**
	 * What the four brushes share. As with the Sculpt Brush Pack's Tube, Noise and Sharpen, a stroke's result depends on
	 * where the brush went and how hard, not on how long it stayed: each point keeps its reach for the stroke, the largest
	 * falloff x strength any stamp has given it, and stands that share of the way from its place before the stroke to the
	 * brush's target for it. A second pass adds nothing and a lighter pass takes nothing back. The target is worked out
	 * once per point, from the mesh as it was before the stroke.
	 */
	class FMorphBrushOp : public FMeshSculptBrushOp
	{
	public:
		explicit FMorphBrushOp(TSharedPtr<IMorphBrushContext> InContext);

		virtual void BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices) override;
		virtual void ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut) override;
		virtual void EndStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& FinalVertices) override;
		virtual void CancelStroke() override;
		// The cursor is found on, and the brush is handed, the mesh from before the stroke, so a stroke does not chase its
		// own result.
		virtual ESculptBrushOpTargetType GetBrushTargetType() const override { return ESculptBrushOpTargetType::TargetMesh; }
		virtual EStampInputMeshType GetStampInputMeshType() const override { return EStampInputMeshType::BaseMesh; }
		virtual bool UsesAlpha() const override { return true; }
		virtual bool SupportsStrokeType(EMeshSculptStrokeType StrokeType) const override { return SculptBrushPack::IsKnownStrokeType(StrokeType); }

	protected:
		/** Whether the brush asks the context for source or mirrored offsets, which cost the context work per stamp. */
		virtual bool NeedsOffsets() const { return false; }
		/** Whether the brush asks the context for mirrored offsets. */
		virtual bool NeedsMirror() const { return false; }
		/**
		 * Called on the game thread once per stamp, before its points are moved: reads the settings and works out what the
		 * points share. Mesh is the shown mesh from before the stroke, Without the same without the edited morph.
		 */
		virtual void PrepareStamp(const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices) {}
		/**
		 * Where a point stands at full reach. Before is its place before the stroke, Base its place without the morph.
		 * Called from worker threads, once per point and stroke.
		 */
		virtual FVector3d FullTarget(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Base) const = 0;

		/** Whether the context let the stroke under way begin. */
		bool IsEditable() const { return bEditable; }

		TSharedPtr<IMorphBrushContext> Context;

	private:
		void Resize(int32 MaxVertexID);

		bool bEditable = false;
		TArray<double> Reach;
		TArray<FVector3d> Targets;
		TArray<bool> bHasTarget;
	};

	/**
	 * Amplify Morph. At full reach a point's offset is 1 + Amount times what it was; with Ctrl, 1 - Amount times, and
	 * never less than none of it. The morph grows stronger or weaker where it is brushed and keeps its shape.
	 */
	class FAmplifyMorphBrushOp : public FMorphBrushOp
	{
	public:
		using FMorphBrushOp::FMorphBrushOp;

	protected:
		virtual void PrepareStamp(const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices) override;
		virtual FVector3d FullTarget(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Base) const override;

	private:
		double Amount = 0.0;
	};

	/**
	 * Smooth Morph. At full reach a point's offset is what Smoothing rounds of averaging make of it; one round gives every
	 * point the average of its own offset and its neighbors'. The offsets are smoothed, not the surface, so the model's
	 * own detail stays: a morph's hard edge softens and the model's pores do not blur.
	 */
	class FSmoothMorphBrushOp : public FMorphBrushOp
	{
	public:
		using FMorphBrushOp::FMorphBrushOp;
		virtual void BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices) override;

	protected:
		virtual void PrepareStamp(const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices) override;
		virtual FVector3d FullTarget(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Base) const override;

	private:
		/** A point's offset after RoundCount rounds of averaging. Game thread only: it fills the rounds as it goes. */
		FVector3d Averaged(const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, int32 Vertex, int32 RoundCount);
		void ResizeRounds(int32 MaxVertexID);

		struct FRound
		{
			TArray<FVector3d> Offsets;
			TArray<bool> bKnown;
		};
		/** Rounds[r] holds the offsets after r + 1 rounds, for the points worked out so far in this stroke. */
		TArray<FRound> Rounds;
		TArray<FVector3d> Smoothed;
		TArray<bool> bHasSmoothed;
	};

	/**
	 * Clone Morph. At full reach a point's offset is the one the Source morph would give it, times Amount: the edited
	 * morph takes the source's shape where it is brushed. With Add To Shape the source's offset is added to the point's
	 * own; with Ctrl it is taken out of it, which leaves, of a whole shape, what the source does not do. Without a Source
	 * the brush moves nothing and says so. With the tool's Symmetry on, only the source's +X half is cloned, onto both
	 * sides, and the brush says that too.
	 */
	class FCloneMorphBrushOp : public FMorphBrushOp
	{
	public:
		using FMorphBrushOp::FMorphBrushOp;
		virtual void BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices) override;

	protected:
		virtual bool NeedsOffsets() const override { return true; }
		virtual void PrepareStamp(const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices) override;
		virtual FVector3d FullTarget(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Base) const override;

	private:
		FName Source = NAME_None;
		double Amount = 0.0;
		bool bAdd = false;
	};

	/**
	 * Mirror Morph. At full reach a point's offset is the mirror image of the offset its partner across the mesh's
	 * symmetry plane had when the stroke began. With Copy From at Other Side, brushing one side copies the other onto it
	 * and brushing both swaps them; at +X Side or -X Side only the points of the opposite side change. A point on the
	 * plane keeps the part of its offset that lies in the plane. Where no brushed point has a partner the brush says so.
	 * It does nothing while the tool's Symmetry is on, which is a mirror of its own from +X onto -X.
	 */
	class FMirrorMorphBrushOp : public FMorphBrushOp
	{
	public:
		using FMorphBrushOp::FMorphBrushOp;
		virtual void BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices) override;
		virtual void ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut) override;

	protected:
		virtual bool NeedsOffsets() const override { return true; }
		virtual bool NeedsMirror() const override { return true; }
		virtual void PrepareStamp(const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices) override;
		virtual FVector3d FullTarget(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Base) const override;

	private:
		ESculptBrushPackMirrorSource CopyFrom = ESculptBrushPackMirrorSource::OtherSide;
		/** How many of the stroke's points the mesh's symmetry had something for. Counted from worker threads. */
		mutable std::atomic<int32> PointsWithMirror{ 0 };
		bool bToldOfNoPartner = false;
	};
}
