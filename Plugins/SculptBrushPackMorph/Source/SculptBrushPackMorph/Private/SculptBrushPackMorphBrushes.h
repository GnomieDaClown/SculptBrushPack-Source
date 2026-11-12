// Copyright (C) 2026 Joseph Wiginton.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sculpt Brush Pack: Morph Targets. Brushes that edit a morph target's difference from the base, for the engine's
// Sculpt Morph Target tool in the Skeletal Mesh Editor. The plugin's README.md says how each is used. Engine anchors
// are for 5.8.4.
//
// The tool shows the mesh as posed, with every active morph target applied, and works the edited morph's deltas out of
// the sculpted mesh when a stroke ends (MorphTargetVertexSculptTool.cpp:473-556). So these brushes, like any other, only
// move the points of the shown mesh; what makes them morph brushes is that each knows where a point stands without the
// edited morph. A point's offset is its place less that place: what the edited morph adds there, as shown.

#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Image/ImageBuilder.h"
#include "Sculpting/MeshBrushOpBase.h"
#include "SculptBrushPackSettings.h"

#include <atomic>

#include "SculptBrushPackMorphBrushes.generated.h"

class UTexture2D;

namespace SculptBrushPackMorph
{
	/** Brush identifiers in the tool, after the Sculpt Brush Pack's own (2001 and up). */
	namespace BrushID
	{
		inline constexpr int32 AmplifyMorph = 2101;
		inline constexpr int32 SmoothMorph = 2102;
		inline constexpr int32 CloneMorph = 2103;
		inline constexpr int32 MirrorMorph = 2104;
		inline constexpr int32 MushMorph = 2105;
		inline constexpr int32 WrinkleMorph = 2106;
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
	 * GetUnposedPosition, GetUV, IsToolSymmetryOn and ReportProblem from the game thread only. GetSourceOffset and
	 * GetMirroredOffset are called from worker threads, for the vertices of the last PrepareVertices call, and must not
	 * change the context.
	 */
	class IMorphBrushContext
	{
	public:
		virtual ~IMorphBrushContext() = default;

		/**
		 * Called when a stroke begins; bMirror says whether mirrored offsets will be asked for. False when the morph
		 * cannot be edited now, for instance because its weight is zero, at which the tool drops the stroke
		 * (MorphTargetVertexSculptTool.cpp:488-492). The brush then moves nothing, and the context has told the user why
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
		 * A vertex's place in the mesh's reference pose, with the morph targets at the weights the shown mesh has: the
		 * shown mesh with its pose taken off. With bWithEditedMorph false, the edited morph is left out as well, which is
		 * GetMeshWithoutMorph's mesh with its pose taken off. As the mesh was when the stroke began. False when not known.
		 */
		virtual bool GetUnposedPosition(int32 Vertex, bool bWithEditedMorph, FVector3d& OutPosition) const { return false; }

		/**
		 * A vertex's place in one of the mesh's UV channels. A vertex on a UV seam has several; one of them is given. False
		 * when the mesh has no such channel or the vertex no place in it.
		 */
		virtual bool GetUV(int32 Vertex, int32 Channel, FVector2d& OutUV) const { return false; }

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

UCLASS()
class USculptBrushPackMushMorphOpProps : public USculptBrushPackOpProps
{
	GENERATED_BODY()
public:
	USculptBrushPackMushMorphOpProps();

	/**
	 * How broadly the surface is evened out: the number of times each point is averaged with its neighbors before the
	 * model's detail is put back. Few rounds mend small pinches; more rounds mend a whole collapsed joint. Read when a
	 * stroke begins.
	 */
	UPROPERTY(EditAnywhere, Category = MushMorph, meta = (DisplayName = "Smoothing", UIMin = "1", UIMax = "8", ClampMin = "1", ClampMax = "16"))
	int32 Smoothing = 4;
};

UCLASS()
class USculptBrushPackWrinkleMorphOpProps : public USculptBrushPackOpProps
{
	GENERATED_BODY()
public:
	USculptBrushPackWrinkleMorphOpProps();

	/**
	 * The wrinkle's height where it is whole, in the units the tool works in: world units (centimeters) for a mesh that
	 * is not scaled. With Ctrl the wrinkle is cut in instead of raised.
	 */
	UPROPERTY(EditAnywhere, Category = WrinkleMorph, meta = (DisplayName = "Height", UIMin = "0.0", UIMax = "5.0", ClampMin = "0.0"))
	float Height = 0.5f;

	/**
	 * On: wrinkles rise only where the posed surface is shorter than at rest, as skin bunches up on the inside of a
	 * bend, and by how much shorter. Off: they rise everywhere the brush goes, which is the way to use the brush on a
	 * mesh in its reference pose.
	 */
	UPROPERTY(EditAnywhere, Category = WrinkleMorph, meta = (DisplayName = "Only Where Compressed"))
	bool bOnlyWhereCompressed = true;

	/**
	 * How much shorter than at rest the surface around a point must be before anything rises there: 0.02 is two
	 * hundredths shorter. It keeps the brush off surface that is as good as at rest.
	 */
	UPROPERTY(EditAnywhere, Category = WrinkleMorph, meta = (DisplayName = "Starts At", UIMin = "0.0", UIMax = "0.5", ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bOnlyWhereCompressed"))
	float StartsAt = 0.02f;

	/** How much shorter than at rest the surface around a point is where the wrinkle is whole: 0.25 is a quarter shorter. */
	UPROPERTY(EditAnywhere, Category = WrinkleMorph, meta = (DisplayName = "Full At", UIMin = "0.01", UIMax = "1.0", ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bOnlyWhereCompressed"))
	float FullAt = 0.25f;

	/**
	 * A texture that gives the wrinkles their pattern: it is read at each point's place in the mesh's UVs, and its red
	 * channel is the share of Height the point rises by. Black leaves the point, white raises it fully. Without a map the
	 * rise is even, and the brush's alpha alone gives it a pattern. The texture is read when the brush is first used
	 * with it; pick the brush again after changing the texture's pixels.
	 */
	UPROPERTY(EditAnywhere, Category = WrinkleMorph, meta = (DisplayName = "Wrinkle Map"))
	TObjectPtr<UTexture2D> WrinkleMap = nullptr;

	/**
	 * For a map painted around mid grey, as displacement maps are: mid grey leaves the point, white raises it by Height
	 * and black lowers it by Height.
	 */
	UPROPERTY(EditAnywhere, Category = WrinkleMorph, meta = (DisplayName = "Mid Grey Is Flat", EditCondition = "WrinkleMap != nullptr"))
	bool bMidGreyIsFlat = false;

	/** How many times the map repeats across the mesh's UVs. */
	UPROPERTY(EditAnywhere, Category = WrinkleMorph, meta = (DisplayName = "Map Tiling", UIMin = "0.25", UIMax = "16.0", ClampMin = "0.01", EditCondition = "WrinkleMap != nullptr"))
	float MapTiling = 1.0f;

	/** The UV channel of the mesh the map is read with. */
	UPROPERTY(EditAnywhere, Category = WrinkleMorph, meta = (DisplayName = "UV Channel", UIMin = "0", UIMax = "7", ClampMin = "0", ClampMax = "7", EditCondition = "WrinkleMap != nullptr"))
	int32 UVChannel = 0;
};

namespace SculptBrushPackMorph
{
	/**
	 * What the brushes share. As with the Sculpt Brush Pack's Tube, Noise and Sharpen, a stroke's result depends on
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

	/**
	 * Mush Morph. A corrective for what skinning does to the surface in a pose. At full reach a point stands where the
	 * smoothed posed surface, with the model's detail put back on it, would have it. Both the shown mesh and the same
	 * mesh with its pose taken off are smoothed by Smoothing rounds of averaging, as Smooth Morph averages. A point's
	 * detail is its unposed place less its smoothed unposed place, measured along the smoothed unposed surface's own
	 * directions there: its normal and two directions across it. It is put back along the same directions of the smoothed
	 * posed surface. So a pinched elbow or a collapsed shoulder is evened out and the model's own detail stays. Where the
	 * mesh stands as in its reference pose the two shapes are one, and the point stays where it is. This is the delta
	 * mush technique (Mancewicz, Derksen, Rijpkema and Wilson, 2014), painted into the morph target with a brush.
	 */
	class FMushMorphBrushOp : public FMorphBrushOp
	{
	public:
		using FMorphBrushOp::FMorphBrushOp;
		virtual void BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices) override;

	protected:
		virtual void PrepareStamp(const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices) override;
		virtual FVector3d FullTarget(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Base) const override;

	private:
		/** The two shapes that are smoothed: the mesh as shown, and the same mesh with its pose taken off. */
		enum EShape : int32
		{
			Posed = 0,
			Unposed = 1,
		};

		/** A shape's places after each round of averaging, for the points worked out so far in this stroke. */
		struct FSmoothedShape
		{
			/** Places[r] holds the places after r + 1 rounds. */
			TArray<TMap<int32, FVector3d>> Places;
			/** Unknown[r] holds the points whose place after r + 1 rounds cannot be known. */
			TArray<TSet<int32>> Unknown;
		};

		/** A point's place in a shape after Round rounds of averaging. False when it cannot be known. Game thread only. */
		bool Place(const FDynamicMesh3& Mesh, EShape Shape, int32 Vertex, int32 Round, FVector3d& OutPlace);
		/**
		 * The smoothed surface's directions at a point: OutAxes[2] its normal, OutAxes[0] the direction across it toward
		 * the neighbor InOutToward (chosen here when INDEX_NONE), OutAxes[1] square to both. False where they cannot be told.
		 */
		bool Directions(const FDynamicMesh3& Mesh, EShape Shape, int32 Vertex, int32& InOutToward, FVector3d& OutPlace, FVector3d OutAxes[3]);

		int32 RoundCount = 0;
		FSmoothedShape Shapes[2];
		/** Where each point worked out so far stands at full reach. */
		TMap<int32, FVector3d> Mushed;
		/** The points worked out so far for which no such place could be told; they stay where they are. */
		TSet<int32> Unmushed;
		bool bToldOfUnmushed = false;
	};

	/**
	 * Wrinkle Morph. Raises wrinkles where skin bunches up in a pose. At full reach a point rises along the surface's
	 * normal by Height times its compression times its pattern.
	 *
	 * Compression is how much shorter the edges around the point are in the pose than at rest: the lengths of its edges in
	 * the shown mesh without the edited morph, summed, against the same sum with the pose taken off. Nothing below Starts
	 * At, all of it from Full At. With Only Where Compressed off it is 1 everywhere.
	 *
	 * The pattern is the Wrinkle Map read at the point's place in the mesh's UVs, where there is a map, and 1 where there
	 * is none. The brush's alpha shapes the wrinkles as it shapes any stroke of these brushes: through each point's reach.
	 *
	 * The rise is measured from the point's place before the stroke, so a stroke adds to what is there. With Ctrl the
	 * wrinkle is cut in. A stroke that found nothing compressed under it says so when it ends.
	 */
	class FWrinkleMorphBrushOp : public FMorphBrushOp
	{
	public:
		using FMorphBrushOp::FMorphBrushOp;
		using FMapImage = UE::Geometry::TImageBuilder<float>;

		virtual void BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices) override;
		virtual void EndStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& FinalVertices) override;

		/**
		 * Reads a texture into a map of one value per texel: the texture's red channel, read as the engine's sculpt tools
		 * read a brush alpha. False when the texture cannot be read. Game thread.
		 */
		static bool ReadTextureAsMap(UTexture2D* Texture, FMapImage& OutMap);

		/** How a texture is read into a map. Unset, ReadTextureAsMap is used; tests of the brush's rule set one of their own. */
		TFunction<bool(UTexture2D*, FMapImage&)> ReadMap;

	protected:
		virtual void PrepareStamp(const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices) override;
		virtual FVector3d FullTarget(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Base) const override;

	private:
		/**
		 * The share of the wrinkle a point's compression gives it, from 0 to 1. Mesh gives the point's neighbors, Without
		 * their places.
		 */
		double Compression(const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, int32 Vertex) const;

		double Height = 0.0;
		bool bOnlyWhereCompressed = true;
		double StartsAt = 0.0;
		double FullAt = 0.0;
		bool bMidGreyIsFlat = false;
		double MapTiling = 1.0;
		int32 UVChannel = 0;

		/** The map as read, kept while the brush is active and the same texture is set. */
		FMapImage Map;
		TWeakObjectPtr<UTexture2D> MapTexture;
		bool bHasMap = false;

		/** How far each point worked out so far rises at full reach, as a vector. */
		TMap<int32, FVector3d> Rise;
		int32 PointsSeen = 0;
		int32 PointsCompressed = 0;
		bool bToldOfMap = false;
	};
}
