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
		inline constexpr int32 NuWrinkle = 2106;
		inline constexpr int32 AsymmetryMorph = 2107;
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
	 * GetUnposedPosition, GetUV, GetReferencePointCount, GetReferencePoint, GetReferencePointOf, IsToolSymmetryOn and
	 * ReportProblem from the game thread only. GetSourceOffset, GetMirroredOffset and GetOffsetOfDelta are called from
	 * worker threads, for the vertices of the last PrepareVertices call, and must not change the context.
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
		 * How many point numbers the whole mesh has for GetReferencePoint: every number below it may be asked for. The
		 * whole mesh is the asset's. With part of it isolated it is more than the sculpt mesh, and it keeps its own
		 * numbering. 0 when not known.
		 */
		virtual int32 GetReferencePointCount() const { return 0; }

		/**
		 * A point of the whole mesh: its place in the reference pose without any morph target, and the edited morph's
		 * delta there, kept for the reference pose, as it was when the stroke began. False where the number is not in use.
		 */
		virtual bool GetReferencePoint(int32 Point, FVector3d& OutPlace, FVector3d& OutDelta) const { return false; }

		/** The number, for GetReferencePoint, of a vertex of the sculpt mesh. INDEX_NONE when not known. */
		virtual int32 GetReferencePointOf(int32 Vertex) const { return INDEX_NONE; }

		/**
		 * The offset that a delta kept for the reference pose makes at a vertex of the shown mesh, at the edited morph's
		 * weight: what posing does to it there. False when not known.
		 */
		virtual bool GetOffsetOfDelta(int32 Vertex, const FVector3d& Delta, FVector3d& OutOffset) const { return false; }

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

/** Which side of the mesh Mirror Morph and Asymmetry Morph copy from. The mesh's symmetry plane is X = 0. */
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
class USculptBrushPackAsymmetryMorphOpProps : public USculptBrushPackOpProps
{
	GENERATED_BODY()
public:
	USculptBrushPackAsymmetryMorphOpProps();

	/**
	 * Where the +X side of the mesh sits against a plain mirror image of its -X side, in the mesh's own units and along
	 * its own axes in the reference pose. A brushed point on the -X side takes its shape from the point nearest to its
	 * mirror place plus this Offset; a brushed point on the +X side, from the point nearest to the mirror place of
	 * itself less this Offset. At 0 the brush looks straight across the plane X = 0. With an X in it, the two sides meet
	 * where X is half of that. Change it until the shape lands where it should. Read when a stroke begins.
	 */
	UPROPERTY(EditAnywhere, Category = AsymmetryMorph, meta = (DisplayName = "Offset"))
	FVector Offset = FVector::ZeroVector;

	/**
	 * Which side the brushed points take their shape from. Other Side: each brushed point takes the shape of the point
	 * found across from it, so brush the side that should receive. +X Side or -X Side: only points on the opposite side
	 * change, so the side copied from cannot be brushed away by mistake.
	 */
	UPROPERTY(EditAnywhere, Category = AsymmetryMorph, meta = (DisplayName = "Copy From"))
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

/** The cross-section of Nu Wrinkle's folds. */
UENUM()
enum class ESculptBrushPackFoldProfile : uint8
{
	/** Rounded folds with a sharp crease between them, as skin folds. */
	Rolls,
	/** Smooth waves, rounded at the top and at the bottom. */
	Waves,
	/** Sharp crests with rounded hollows between them. */
	Ridges,
};

/** Which way Nu Wrinkle's folds run. */
UENUM()
enum class ESculptBrushPackFoldDirection : uint8
{
	/** The folds lie across the way the surface is squeezed, as real folds do. Where the brush meets nothing squeezed, they run along the stroke. */
	AcrossCompression UMETA(DisplayName = "Across Compression"),
	/** The folds run the way the stroke first moves. */
	AlongStroke UMETA(DisplayName = "Along Stroke"),
};

UCLASS()
class USculptBrushPackNuWrinkleOpProps : public USculptBrushPackOpProps
{
	GENERATED_BODY()
public:
	USculptBrushPackNuWrinkleOpProps();

	/**
	 * The wrinkle's height where it is whole, in the units the tool works in: world units (centimeters) for a mesh that
	 * is not scaled. With Ctrl the wrinkle is cut in instead of raised.
	 */
	UPROPERTY(EditAnywhere, Category = NuWrinkle, meta = (DisplayName = "Height", UIMin = "0.0", UIMax = "5.0", ClampMin = "0.0"))
	float Height = 0.5f;

	/**
	 * Off: one stroke gives each point at most the Height, however long the brush stays, and a second pass over the same
	 * place adds nothing. On: the wrinkle grows for as long as the brush stays or passes again, up to the Height.
	 */
	UPROPERTY(EditAnywhere, Category = NuWrinkle, meta = (DisplayName = "Build Up"))
	bool bBuildUp = false;

	/**
	 * On: wrinkles rise only where the posed surface is shorter than at rest, as skin bunches up on the inside of a
	 * bend, and by how much shorter. Off: they rise everywhere the brush goes, which is the way to use the brush on a
	 * mesh in its reference pose.
	 */
	UPROPERTY(EditAnywhere, Category = Compression, meta = (DisplayName = "Only Where Compressed"))
	bool bOnlyWhereCompressed = true;

	/**
	 * How much shorter than at rest the surface around a point must be before anything rises there: 0.02 is two
	 * hundredths shorter. It keeps the brush off surface that is as good as at rest.
	 */
	UPROPERTY(EditAnywhere, Category = Compression, meta = (DisplayName = "Starts At", UIMin = "0.0", UIMax = "0.5", ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bOnlyWhereCompressed"))
	float StartsAt = 0.02f;

	/** How much shorter than at rest the surface around a point is where the wrinkle is whole: 0.25 is a quarter shorter. */
	UPROPERTY(EditAnywhere, Category = Compression, meta = (DisplayName = "Full At", UIMin = "0.01", UIMax = "1.0", ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bOnlyWhereCompressed"))
	float FullAt = 0.25f;

	/**
	 * How many times each point's compression is averaged with its neighbors' before it is used, which evens out a
	 * patchy rise on a mesh whose edges are of very different lengths. 0 uses each point's own. Read when a stroke begins.
	 */
	UPROPERTY(EditAnywhere, Category = Compression, meta = (DisplayName = "Soften", UIMin = "0", UIMax = "4", ClampMin = "0", ClampMax = "8", EditCondition = "bOnlyWhereCompressed"))
	int32 Soften = 1;

	/**
	 * On: the brush lays folds of its own, side by side, and needs no texture for a pattern. Off: the rise is even, and
	 * only the Wrinkle Map and the brush's alpha shape it.
	 */
	UPROPERTY(EditAnywhere, Category = Folds, meta = (DisplayName = "Folds"))
	bool bFolds = true;

	/** Which way the folds run. Read with the first stamp of a stroke that needs it, and kept for the stroke. */
	UPROPERTY(EditAnywhere, Category = Folds, meta = (DisplayName = "Direction", EditCondition = "bFolds"))
	ESculptBrushPackFoldDirection Direction = ESculptBrushPackFoldDirection::AcrossCompression;

	/**
	 * The distance from one fold to the next, in the same units as Height. Folds finer than the mesh's own points cannot
	 * show: a brush moves the points there are.
	 */
	UPROPERTY(EditAnywhere, Category = Folds, meta = (DisplayName = "Spacing", UIMin = "0.25", UIMax = "20.0", ClampMin = "0.01", EditCondition = "bFolds"))
	float Spacing = 2.0f;

	/** The folds' cross-section. */
	UPROPERTY(EditAnywhere, Category = Folds, meta = (DisplayName = "Profile", EditCondition = "bFolds"))
	ESculptBrushPackFoldProfile Profile = ESculptBrushPackFoldProfile::Rolls;

	/**
	 * Off: the folds rise from the surface, which stays where it was between them. On: they rise by the Height at their
	 * middles and sink by the Height between, so that the surface keeps its level on the whole.
	 */
	UPROPERTY(EditAnywhere, Category = Folds, meta = (DisplayName = "Cut Between", EditCondition = "bFolds"))
	bool bCutBetween = false;

	/**
	 * How unlike each other the folds are: at 0 all are as high as each other, at 1 a fold may be anything from nothing
	 * to the whole Height. Which fold is how high is fixed in the mesh, so two strokes agree.
	 */
	UPROPERTY(EditAnywhere, Category = Folds, meta = (DisplayName = "Irregularity", UIMin = "0.0", UIMax = "1.0", ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bFolds"))
	float Irregularity = 0.25f;

	/**
	 * On: the Wrinkle Map below shapes the rise, on top of the folds where those are on. Off: the map is not read.
	 */
	UPROPERTY(EditAnywhere, Category = WrinkleMap, meta = (DisplayName = "Use Wrinkle Map"))
	bool bUseMap = false;

	/**
	 * A texture that gives the wrinkles a pattern, or shapes the folds: it is read at each point's place in the mesh's
	 * UVs, and its red channel is the share of the rise the point gets. Black leaves the point, white gives it all. The
	 * texture is read when the brush is first used with it; pick the brush again after changing the texture's pixels.
	 * Three maps come with the plugin, under its content's WrinkleMaps folder, and one of them stands here as an
	 * example: T_WrinkleMap_Folds (long folds side by side), T_WrinkleMap_Creases (a net of fine creases) and
	 * T_WrinkleMap_Crumple (deep wandering creases).
	 */
	UPROPERTY(EditAnywhere, Category = WrinkleMap, meta = (DisplayName = "Wrinkle Map", EditCondition = "bUseMap"))
	TObjectPtr<UTexture2D> WrinkleMap = nullptr;

	/**
	 * For a map painted around mid grey, as displacement maps are: mid grey leaves the point, white raises it and black
	 * lowers it.
	 */
	UPROPERTY(EditAnywhere, Category = WrinkleMap, meta = (DisplayName = "Mid Grey Is Flat", EditCondition = "bUseMap"))
	bool bMidGreyIsFlat = false;

	/** How many times the map repeats across the mesh's UVs. */
	UPROPERTY(EditAnywhere, Category = WrinkleMap, meta = (DisplayName = "Map Tiling", UIMin = "0.25", UIMax = "16.0", ClampMin = "0.01", EditCondition = "bUseMap"))
	float MapTiling = 1.0f;

	/** The UV channel of the mesh the map is read with. */
	UPROPERTY(EditAnywhere, Category = WrinkleMap, meta = (DisplayName = "UV Channel", UIMin = "0", UIMax = "7", ClampMin = "0", ClampMax = "7", EditCondition = "bUseMap"))
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
	 *
	 * A brush that builds up (BuildsUp) keeps another reach: each stamp adds falloff x strength x time x 6 to it, the
	 * engine's own speed for a stamp (MeshSculptBrushOps.h:39), up to 1. The point then comes to its target as the brush
	 * stays or passes again.
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
		virtual void PrepareStamp(const FSculptBrushStamp& Stamp, const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices) {}
		/**
		 * Where a point stands at full reach. Before is its place before the stroke, Base its place without the morph.
		 * Called from worker threads, once per point and stroke unless IsTargetFinal says the point's target is not yet
		 * known; then it is asked again with the next stamp, and should answer Before meanwhile.
		 */
		virtual FVector3d FullTarget(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Base) const = 0;
		/** Whether the target FullTarget gives for a point now is the one it keeps for the stroke. Called from worker threads. */
		virtual bool IsTargetFinal(int32 Vertex) const { return true; }
		/** Whether a point's reach grows with every stamp, up to 1, and is not the largest any stamp gave. Read once per stamp. */
		virtual bool BuildsUp() const { return false; }

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
		virtual void PrepareStamp(const FSculptBrushStamp& Stamp, const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices) override;
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
		virtual void PrepareStamp(const FSculptBrushStamp& Stamp, const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices) override;
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
		virtual void PrepareStamp(const FSculptBrushStamp& Stamp, const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices) override;
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
		virtual void PrepareStamp(const FSculptBrushStamp& Stamp, const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices) override;
		virtual FVector3d FullTarget(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Base) const override;

	private:
		ESculptBrushPackMirrorSource CopyFrom = ESculptBrushPackMirrorSource::OtherSide;
		/** How many of the stroke's points the mesh's symmetry had something for. Counted from worker threads. */
		mutable std::atomic<int32> PointsWithMirror{ 0 };
		bool bToldOfNoPartner = false;
	};

	/**
	 * Finds, among a set of points, the one nearest to a place without looking at them all: the points are kept as a
	 * tree that halves them along their longest extent, again and again. The answer is exact. No point of the set is
	 * nearer than the one given, and of several equally near the one with the lowest number is given.
	 */
	class FNearestPointFinder
	{
	public:
		/** Takes the points: Places[i] is the place of the point numbered Numbers[i]. A place that is not finite is left out. */
		void Build(const TArray<FVector3d>& InPlaces, const TArray<int32>& InNumbers);
		void Reset();
		int32 Num() const { return Points.Num(); }

		/** The number of the point nearest to Place, and its place. INDEX_NONE when there is none. Any thread, once built. */
		int32 FindNearest(const FVector3d& Place, FVector3d* OutPlace = nullptr) const;

	private:
		struct FPoint
		{
			FVector3d Place;
			int32 Number;
		};

		/** Puts the points of a range in the tree's order. */
		void Order(int32 Low, int32 High);
		void Search(int32 Low, int32 High, const FVector3d& Place, double& BestSquared, int32& Best) const;

		/**
		 * The points in the tree's order. The middle point of a range of more than a few splits it along Axes[middle]:
		 * no point before it lies further along that axis than it does, and none after it less far.
		 */
		TArray<FPoint> Points;
		TArray<uint8> Axes;
	};

	/**
	 * Asymmetry Morph. Mirror Morph for a mesh whose two sides do not match point for point, so that the editor's
	 * symmetry cannot pair them: the artist says with an Offset where one side sits against the mirror image of the
	 * other, and the brush pairs the points by place.
	 *
	 * All of it is worked out in the mesh's reference pose without any morph target. A brushed point's source is looked
	 * for at its mirror place across X = 0, moved by the Offset: for a point on the -X side, mirror place + Offset; for a
	 * point on the +X side, the mirror place of (place - Offset). Either way round, two points that are each other's
	 * source stay so. The two sides meet where X is half the Offset's X. The source is the point of the whole mesh
	 * nearest to that place, the lowest numbered of several equally near; with part of the mesh isolated it may be a
	 * point that is not shown.
	 *
	 * At full reach a point's offset is the mirror image of the delta its source had when the stroke began. A point
	 * whose source stands at its own place keeps the part of its own delta that lies in the plane, as a point on the
	 * plane does with Mirror Morph. With Copy From at +X Side or -X Side only the points of the opposite side change.
	 * It does nothing while the tool's Symmetry is on, and says so.
	 */
	class FAsymmetryMorphBrushOp : public FMorphBrushOp
	{
	public:
		using FMorphBrushOp::FMorphBrushOp;
		virtual void BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices) override;

		/**
		 * Where the source of a point that stands at Place in the reference pose is looked for, and which side the point
		 * is on.
		 */
		static FVector3d SourcePlace(const FVector3d& Place, const FVector3d& Offset, bool& bOutOnPositiveSide);

	protected:
		virtual bool NeedsOffsets() const override { return true; }
		virtual void PrepareStamp(const FSculptBrushStamp& Stamp, const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices) override;
		virtual FVector3d FullTarget(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Base) const override;

	private:
		/** What is known of a vertex of the sculpt mesh in the stroke under way. */
		enum class ESource : uint8
		{
			/** Its source has not been looked for yet. */
			Unknown,
			/** It takes the delta in Mirrored. */
			Found,
			/** It keeps its shape: it is on the side copied from, or nothing is known of its place. */
			Keeps,
		};

		FVector3d Offset = FVector3d::ZeroVector;
		ESculptBrushPackMirrorSource CopyFrom = ESculptBrushPackMirrorSource::OtherSide;
		/** Whether the stroke under way can act: the tool's Symmetry is off and the mesh's points are known. */
		bool bCanAct = false;

		/** The whole mesh's points in the reference pose, by point number, and the edited morph's deltas there. */
		TArray<FVector3d> Places;
		TArray<FVector3d> Deltas;
		TArray<bool> bInUse;
		/** The tree over Places. Kept from stroke to stroke while the places are the same. */
		FNearestPointFinder Finder;

		/** Per vertex of the sculpt mesh: what is known of its source, and the source's delta, mirrored. */
		TArray<ESource> Sources;
		TArray<FVector3d> Mirrored;
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
		virtual void PrepareStamp(const FSculptBrushStamp& Stamp, const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices) override;
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
	 * Nu Wrinkle. Raises wrinkles where skin bunches up in a pose, and lays them as folds across the way it is squeezed.
	 * At full reach a point rises along the surface's normal by Height x compression x pattern.
	 *
	 * Compression is how much shorter the edges around the point are in the pose than at rest: the lengths of its edges in
	 * the shown mesh without the edited morph, summed, against the same sum with the pose taken off. Nothing up to Starts
	 * At, all of it from Full At; then averaged with the neighbors' Soften times. With Only Where Compressed off it is 1
	 * everywhere.
	 *
	 * The pattern is the folds times the Wrinkle Map; either may be switched off, and with neither the rise is even.
	 *  - Folds lie side by side along one direction, Spacing apart. A point's place along that direction, divided by
	 *    Spacing, gives the fold it is in (the whole part) and how far across that fold it is (the rest, from 0 to 1). A
	 *    tent of that, 0 at the fold's edges and 1 at its middle, is shaped by Profile: Rolls 1 - (1 - tent) squared,
	 *    Waves tent squared x (3 - 2 tent), Ridges tent squared. With Cut Between, twice that less 1. Each fold is then
	 *    made lower by Irregularity times a number from 0 to 1 that belongs to the fold.
	 *  - The direction, Across Compression: the way the surface is squeezed, measured with the first stamp that meets
	 *    squeezed surface. Every shortened edge under that stamp adds its shortening, as a share, along its own direction
	 *    in the unposed mesh; the direction they agree on most is taken (the principal axis of their sum). A point's place
	 *    along it is measured in the unposed mesh, so the folds are fixed in the mesh and two strokes over the same
	 *    squeeze agree. Where nothing squeezed has been met, and with Along Stroke: the direction is across the way the
	 *    stroke first moves, measured in the shown mesh from where the stroke began, and the folds run along the stroke.
	 *    Until the stroke has moved, the points that need it wait, and rise with a later stamp.
	 *  - The Wrinkle Map is read at the point's place in the mesh's UVs.
	 * The brush's alpha shapes the wrinkles as it shapes any stroke of these brushes: through each point's reach.
	 *
	 * The rise is measured from the point's place before the stroke, so a stroke adds to what is there. With Ctrl the
	 * wrinkle is cut in. With Build Up a point's reach grows with every stamp. A stroke that found nothing compressed
	 * under it, or that needed to move to lay its folds and did not, says so when it ends.
	 */
	class FNuWrinkleBrushOp : public FMorphBrushOp
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

		/** A fold's cross-section at Tent, which is 0 at the fold's edges and 1 at its middle. From 0 to 1. */
		static double FoldShape(ESculptBrushPackFoldProfile Profile, double Tent);
		/** The number from 0 to 1 that belongs to a fold and makes it lower than its neighbors, by Irregularity times it. */
		static double FoldVariation(int64 Fold);

		/** The folds' direction for the stroke under way, of length 1, and whether a point's place along it is measured in the unposed mesh. False while not yet known. */
		bool GetFoldDirection(FVector3d& OutDirection, bool& bOutInUnposedMesh) const;

	protected:
		virtual void PrepareStamp(const FSculptBrushStamp& Stamp, const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices) override;
		virtual FVector3d FullTarget(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Base) const override;
		virtual bool IsTargetFinal(int32 Vertex) const override { return Rise.Contains(Vertex); }
		virtual bool BuildsUp() const override { return bBuildUp; }

	private:
		/**
		 * The share of the wrinkle a point's own compression gives it, from 0 to 1. Mesh gives the point's neighbors,
		 * Without their places.
		 */
		double Compression(const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, int32 Vertex);
		/** That share after Round rounds of averaging with the neighbors'. Game thread only: it fills the rounds as it goes. */
		double Softened(const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, int32 Vertex, int32 Round);
		/** Adds to Squeeze what the shortened edges of a point say of the way the surface is squeezed there. */
		void AddSqueeze(const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, int32 Vertex);
		/** The direction the shortened edges agree on most, when there is one. */
		bool FindSqueezeDirection(FVector3d& OutDirection) const;

		double Height = 0.0;
		bool bBuildUp = false;
		bool bOnlyWhereCompressed = true;
		double StartsAt = 0.0;
		double FullAt = 0.0;
		int32 SoftenRounds = 0;
		bool bFolds = false;
		ESculptBrushPackFoldDirection Direction = ESculptBrushPackFoldDirection::AcrossCompression;
		double Spacing = 1.0;
		ESculptBrushPackFoldProfile Profile = ESculptBrushPackFoldProfile::Rolls;
		bool bCutBetween = false;
		double Irregularity = 0.0;
		bool bMidGreyIsFlat = false;
		double MapTiling = 1.0;
		int32 UVChannel = 0;

		/** The map as read, kept while the brush is active and the same texture is set. */
		FMapImage Map;
		TWeakObjectPtr<UTexture2D> MapTexture;
		bool bHasMap = false;

		/** How far each point whose rise is settled rises at full reach, as a vector. */
		TMap<int32, FVector3d> Rise;
		/** Each point's own compression, and that after each round of averaging, for the points worked out so far. */
		TMap<int32, double> OwnCompression;
		TArray<TMap<int32, double>> SoftenedCompression;
		/** The points this stroke has met, each once. */
		TSet<int32> Seen;

		/** The folds' direction, once known for the stroke. */
		bool bHasFoldDirection = false;
		bool bFoldsInUnposedMesh = false;
		FVector3d FoldDirection = FVector3d::ZeroVector;
		/** Where the stroke began, from which places along a direction in the shown mesh are measured. */
		FVector3d StrokeStart = FVector3d::ZeroVector;
		/** The shortened edges' sum so far: xx, yy, zz, xy, xz, yz of a symmetric three by three. */
		double Squeeze[6] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
		/** The points whose edges are in that sum, each once. */
		TSet<int32> SqueezeFrom;

		int32 PointsCompressed = 0;
		bool bPointsWait = false;
		bool bToldOfMap = false;
	};
}
