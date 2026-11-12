// Copyright (C) 2026 Joseph Wiginton.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sculpt Brush Pack: Morph Targets. The brushes' rules (see SculptBrushPackMorphBrushes.h).

#include "SculptBrushPackMorphBrushes.h"

#include "Algo/Partition.h"
#include "AssetUtils/Texture2DUtil.h"
#include "Async/ParallelFor.h"
#include "Engine/Texture2D.h"
#include "IndexTypes.h"
#include "Math/RandomStream.h"
#include "VectorUtil.h"

#define LOCTEXT_NAMESPACE "SculptBrushPackMorph"

using UE::Geometry::FDynamicMesh3;

namespace SculptBrushPackMorph
{
namespace Private
{
	// Smooth Morph's most rounds: a stroke keeps one offset per vertex of the mesh and round.
	constexpr int32 MaxSmoothingRounds = 16;

	// Nu Wrinkle's most rounds of softening its compression.
	constexpr int32 MaxSoftenRounds = 8;

	// How fast a reach that builds up grows: the engine's own speed for a stamp (BrushSpeedTuning, MeshSculptBrushOps.h:39
	// in 5.8.4), so that a brush that builds up does so at the pace of the engine's own.
	constexpr double BuildUpSpeed = 6.0;

	// The rounds of multiplying by which the way the surface is squeezed is found from its shortened edges.
	constexpr int32 SqueezeRounds = 32;

	// The nearest-point finder looks at a range of this many points or fewer one by one, without splitting it further.
	constexpr int32 NearestLeafSize = 8;

	/** The tool's falloff at Position, times its alpha when it has one. */
	static double EvaluateWeight(const FMeshSculptBrushOp& Op, const FSculptBrushStamp& Stamp, const FVector3d& Position)
	{
		const double Falloff = Op.GetFalloff().Evaluate(Stamp, Position);
		return Stamp.HasAlpha() ? Falloff * Stamp.StampAlphaFunc(Stamp, Position) : Falloff;
	}

	static int32 ClampedRounds(int32 Rounds)
	{
		return FMath::Clamp(Rounds, 1, MaxSmoothingRounds);
	}

	/** Reads a texture as the engine's sculpt tools read a brush alpha (MeshVertexSculptTool.cpp:1770-1777) and keeps its red channel. */
	static bool ReadMapWithEngine(UTexture2D* Texture, UE::Geometry::TImageBuilder<float>& OutMap)
	{
		UE::Geometry::TImageBuilder<FVector4f> Pixels;
		if (!Texture || !UE::AssetUtils::ReadTexture(Texture, Pixels, /*bPreferPlatformData*/ false))
		{
			return false;
		}
		Pixels.Convert<float>([](const FVector4f& Pixel) { return Pixel.X; }, OutMap);
		return OutMap.GetDimensions().GetWidth() > 0 && OutMap.GetDimensions().GetHeight() > 0;
	}
}

FMorphBrushOp::FMorphBrushOp(TSharedPtr<IMorphBrushContext> InContext)
	: Context(MoveTemp(InContext))
{
}

void FMorphBrushOp::Resize(int32 MaxVertexID)
{
	if (Reach.Num() < MaxVertexID)
	{
		Reach.SetNumZeroed(MaxVertexID);
		// A target is written before it is read, so only the marks are cleared.
		Targets.SetNumUninitialized(MaxVertexID);
		bHasTarget.SetNumZeroed(MaxVertexID);
	}
}

void FMorphBrushOp::BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices)
{
	bEditable = Context.IsValid() && Context->BeginStroke(NeedsMirror());
	if (bEditable)
	{
		// Whatever kept an earlier stroke from acting no longer does.
		Context->ReportProblem(FText::GetEmpty());
	}
	Reach.Reset();
	Targets.Reset();
	bHasTarget.Reset();
	Resize(Mesh->MaxVertexID());
}

void FMorphBrushOp::ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut)
{
	// Mesh is the shown mesh from before the stroke.
	const FDynamicMesh3* Without = bEditable ? Context->GetMeshWithoutMorph() : nullptr;
	if (!Without)
	{
		for (int32 k = 0; k < Vertices.Num(); ++k)
		{
			NewPositionsOut[k] = Mesh->GetVertex(Vertices[k]);
		}
		return;
	}

	Resize(Mesh->MaxVertexID());
	if (NeedsOffsets())
	{
		Context->PrepareVertices(Vertices);
	}
	PrepareStamp(Stamp, *Mesh, *Without, Vertices);
	const bool bBuildsUp = BuildsUp();

	// The tool lists each vertex once, so each k reads and writes entries of its own.
	ParallelFor(Vertices.Num(), [&](int32 k)
	{
		const int32 Vertex = Vertices[k];
		const FVector3d Before = Mesh->GetVertex(Vertex);
		const double Given = Private::EvaluateWeight(*this, Stamp, Before) * Stamp.Power;
		Reach[Vertex] = bBuildsUp ? FMath::Min(1.0, Reach[Vertex] + Given * Stamp.DeltaTime * Private::BuildUpSpeed) : FMath::Max(Reach[Vertex], Given);
		if (!bHasTarget[Vertex])
		{
			const bool bHasBase = Without->IsVertex(Vertex);
			Targets[Vertex] = bHasBase ? FullTarget(Stamp, Vertex, Before, Without->GetVertex(Vertex)) : Before;
			bHasTarget[Vertex] = !bHasBase || IsTargetFinal(Vertex);
		}
		NewPositionsOut[k] = Before + (Targets[Vertex] - Before) * Reach[Vertex];
	});
}

void FMorphBrushOp::EndStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& FinalVertices)
{
	if (Context.IsValid())
	{
		Context->EndStroke();
	}
}

void FMorphBrushOp::CancelStroke()
{
	if (Context.IsValid())
	{
		Context->EndStroke();
	}
}

void FAmplifyMorphBrushOp::PrepareStamp(const FSculptBrushStamp& Stamp, const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices)
{
	Amount = GetPropertySetAs<USculptBrushPackAmplifyMorphOpProps>()->Amount;
}

FVector3d FAmplifyMorphBrushOp::FullTarget(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Base) const
{
	// Taking away stops at the place without the morph: the offset is never turned over.
	const double Factor = Stamp.Direction < 0.0 ? FMath::Max(0.0, 1.0 - Amount) : 1.0 + Amount;
	return Base + (Before - Base) * Factor;
}

void FSmoothMorphBrushOp::ResizeRounds(int32 MaxVertexID)
{
	if (bHasSmoothed.Num() < MaxVertexID)
	{
		for (FRound& Round : Rounds)
		{
			Round.Offsets.SetNumUninitialized(MaxVertexID);
			Round.bKnown.SetNumZeroed(MaxVertexID);
		}
		Smoothed.SetNumUninitialized(MaxVertexID);
		bHasSmoothed.SetNumZeroed(MaxVertexID);
	}
}

void FSmoothMorphBrushOp::BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices)
{
	FMorphBrushOp::BeginStroke(Mesh, Stamp, InitialVertices);

	// The smoothing holds for the whole stroke.
	Rounds.Reset();
	Rounds.SetNum(Private::ClampedRounds(GetPropertySetAs<USculptBrushPackSmoothMorphOpProps>()->Smoothing));
	Smoothed.Reset();
	bHasSmoothed.Reset();
	ResizeRounds(Mesh->MaxVertexID());
}

FVector3d FSmoothMorphBrushOp::Averaged(const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, int32 Vertex, int32 RoundCount)
{
	if (RoundCount == 0)
	{
		return Without.IsVertex(Vertex) ? Mesh.GetVertex(Vertex) - Without.GetVertex(Vertex) : FVector3d::ZeroVector;
	}
	FRound& Round = Rounds[RoundCount - 1];
	if (!Round.bKnown[Vertex])
	{
		// The point's offset and its neighbors', each counted once, as the round before left them.
		FVector3d Sum = Averaged(Mesh, Without, Vertex, RoundCount - 1);
		int32 Count = 1;
		for (const int32 Neighbor : Mesh.VtxVerticesItr(Vertex))
		{
			Sum += Averaged(Mesh, Without, Neighbor, RoundCount - 1);
			++Count;
		}
		Round.Offsets[Vertex] = Sum / static_cast<double>(Count);
		Round.bKnown[Vertex] = true;
	}
	return Round.Offsets[Vertex];
}

void FSmoothMorphBrushOp::PrepareStamp(const FSculptBrushStamp& Stamp, const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices)
{
	if (Rounds.IsEmpty())
	{
		Rounds.SetNum(Private::ClampedRounds(GetPropertySetAs<USculptBrushPackSmoothMorphOpProps>()->Smoothing));
	}
	ResizeRounds(Mesh.MaxVertexID());

	// One point after another: a point's smoothed offset is built from its neighbors', which other points share.
	for (const int32 Vertex : Vertices)
	{
		if (!bHasSmoothed[Vertex])
		{
			Smoothed[Vertex] = Averaged(Mesh, Without, Vertex, Rounds.Num());
			bHasSmoothed[Vertex] = true;
		}
	}
}

FVector3d FSmoothMorphBrushOp::FullTarget(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Base) const
{
	return Base + Smoothed[Vertex];
}

void FCloneMorphBrushOp::BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices)
{
	FMorphBrushOp::BeginStroke(Mesh, Stamp, InitialVertices);
	if (!IsEditable())
	{
		return;
	}

	USculptBrushPackCloneMorphOpProps* Props = GetPropertySetAs<USculptBrushPackCloneMorphOpProps>();
	const TArray<FName> Sources = Context->GetSourceNames();
	// With exactly one other morph target there is nothing to choose.
	if (Sources.Num() == 1 && Props->Source != Sources[0])
	{
		Props->Source = Sources[0];
	}

	// Without a source to copy from the brush moves nothing; the user is told what to do about it.
	if (Sources.IsEmpty())
	{
		Context->ReportProblem(LOCTEXT("CloneNoOtherMorph", "Clone Morph copies from another morph target, and this mesh has no other."));
	}
	else if (Props->Source.IsNone())
	{
		Context->ReportProblem(LOCTEXT("CloneNoSource", "Clone Morph: choose the morph target to copy from in the brush's Source setting."));
	}
	else if (!Sources.Contains(Props->Source))
	{
		Context->ReportProblem(FText::Format(LOCTEXT("CloneMissingSource", "Clone Morph: this mesh has no morph target named {0}. Choose another Source."), FText::FromName(Props->Source)));
	}
	else if (Context->IsToolSymmetryOn())
	{
		Context->ReportProblem(LOCTEXT("CloneWithSymmetry", "The tool's Symmetry is on: it sets the -X side from the +X side after every stamp, so only the +X half of the source is cloned, onto both sides."));
	}
}

void FCloneMorphBrushOp::PrepareStamp(const FSculptBrushStamp& Stamp, const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices)
{
	const USculptBrushPackCloneMorphOpProps* Props = GetPropertySetAs<USculptBrushPackCloneMorphOpProps>();
	Source = Props->Source;
	Amount = Props->Amount;
	bAdd = Props->bAdd;
}

FVector3d FCloneMorphBrushOp::FullTarget(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Base) const
{
	FVector3d SourceOffset;
	if (Source.IsNone() || !Context->GetSourceOffset(Source, Vertex, SourceOffset))
	{
		return Before;
	}
	SourceOffset *= Amount;
	if (Stamp.Direction < 0.0)
	{
		return Before - SourceOffset;
	}
	return bAdd ? Before + SourceOffset : Base + SourceOffset;
}

void FMirrorMorphBrushOp::BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices)
{
	FMorphBrushOp::BeginStroke(Mesh, Stamp, InitialVertices);
	PointsWithMirror = 0;
	bToldOfNoPartner = false;
}

void FMirrorMorphBrushOp::PrepareStamp(const FSculptBrushStamp& Stamp, const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices)
{
	CopyFrom = GetPropertySetAs<USculptBrushPackMirrorMorphOpProps>()->CopyFrom;
}

FVector3d FMirrorMorphBrushOp::FullTarget(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Base) const
{
	FVector3d MirroredOffset;
	const EMorphMirror Mirror = Context->GetMirroredOffset(Vertex, MirroredOffset);
	if (Mirror == EMorphMirror::None)
	{
		return Before;
	}
	++PointsWithMirror;

	// A point on the side that is copied from keeps its shape.
	const bool bIsSource = (Mirror == EMorphMirror::PositiveX && CopyFrom == ESculptBrushPackMirrorSource::PositiveX)
		|| (Mirror == EMorphMirror::NegativeX && CopyFrom == ESculptBrushPackMirrorSource::NegativeX);
	return bIsSource ? Before : Base + MirroredOffset;
}

void FMirrorMorphBrushOp::ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut)
{
	FMorphBrushOp::ApplyStamp(Mesh, Stamp, Vertices, NewPositionsOut);

	// A stroke over points that have no partner across the plane does nothing; the user is told once per stroke.
	if (IsEditable() && !bToldOfNoPartner && Vertices.Num() > 0 && PointsWithMirror == 0)
	{
		bToldOfNoPartner = true;
		Context->ReportProblem(LOCTEXT("MirrorNoPartner", "Mirror Morph: the points under the brush have no partner across the mesh's symmetry plane (X = 0), so nothing was mirrored onto them."));
	}
}

void FNearestPointFinder::Reset()
{
	Points.Reset();
	Axes.Reset();
}

void FNearestPointFinder::Build(const TArray<FVector3d>& InPlaces, const TArray<int32>& InNumbers)
{
	Reset();
	const int32 Count = FMath::Min(InPlaces.Num(), InNumbers.Num());
	Points.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FVector3d& Place = InPlaces[Index];
		// A place that is not a number has no distance to anything, and would not let the ordering below end.
		if (FMath::IsFinite(Place.X) && FMath::IsFinite(Place.Y) && FMath::IsFinite(Place.Z))
		{
			Points.Add({ Place, InNumbers[Index] });
		}
	}
	Axes.SetNumZeroed(Points.Num());
	Order(0, Points.Num());
}

void FNearestPointFinder::Order(int32 Low, int32 High)
{
	if (High - Low <= Private::NearestLeafSize)
	{
		return;
	}

	// The range is split along the axis it reaches furthest along.
	FVector3d Least = Points[Low].Place;
	FVector3d Most = Least;
	for (int32 Index = Low + 1; Index < High; ++Index)
	{
		Least = Least.ComponentMin(Points[Index].Place);
		Most = Most.ComponentMax(Points[Index].Place);
	}
	const FVector3d Extent = Most - Least;
	const int32 Axis = Extent.X >= Extent.Y ? (Extent.X >= Extent.Z ? 0 : 2) : (Extent.Y >= Extent.Z ? 1 : 2);

	// The point that belongs in the middle is brought there: the range is parted about one of its own points until the
	// middle lies among the points that are as far along the axis as that one.
	const int32 Middle = Low + (High - Low) / 2;
	int32 From = Low;
	int32 To = High;
	while (To - From > 1)
	{
		const double First = Points[From].Place[Axis];
		const double Second = Points[From + (To - From) / 2].Place[Axis];
		const double Third = Points[To - 1].Place[Axis];
		// The middle one of three of the range's points, so that at least one point is as far along as it.
		const double Pivot = FMath::Max(FMath::Min(First, Second), FMath::Min(FMath::Max(First, Second), Third));
		const int32 FirstEqual = From + Algo::Partition(&Points[From], To - From, [Axis, Pivot](const FPoint& Point) { return Point.Place[Axis] < Pivot; });
		const int32 FirstFurther = FirstEqual + Algo::Partition(&Points[FirstEqual], To - FirstEqual, [Axis, Pivot](const FPoint& Point) { return Point.Place[Axis] == Pivot; });
		if (Middle < FirstEqual)
		{
			To = FirstEqual;
		}
		else if (Middle >= FirstFurther)
		{
			From = FirstFurther;
		}
		else
		{
			break;
		}
	}
	Axes[Middle] = static_cast<uint8>(Axis);
	Order(Low, Middle);
	Order(Middle + 1, High);
}

void FNearestPointFinder::Search(int32 Low, int32 High, const FVector3d& Place, double& BestSquared, int32& Best) const
{
	auto Consider = [&](int32 Index)
	{
		const double Squared = UE::Geometry::DistanceSquared(Points[Index].Place, Place);
		if (Squared < BestSquared || (Squared == BestSquared && (Best == INDEX_NONE || Points[Index].Number < Points[Best].Number)))
		{
			BestSquared = Squared;
			Best = Index;
		}
	};

	if (High - Low <= Private::NearestLeafSize)
	{
		for (int32 Index = Low; Index < High; ++Index)
		{
			Consider(Index);
		}
		return;
	}

	const int32 Middle = Low + (High - Low) / 2;
	Consider(Middle);
	// The half the place lies in is searched first. The other half holds nothing nearer than the place is to the split,
	// so it is searched only while that is no further than the best found; as far counts, for a point there may be
	// equally near and have a lower number.
	const double Across = Place[Axes[Middle]] - Points[Middle].Place[Axes[Middle]];
	if (Across < 0.0)
	{
		Search(Low, Middle, Place, BestSquared, Best);
		if (Across * Across <= BestSquared)
		{
			Search(Middle + 1, High, Place, BestSquared, Best);
		}
	}
	else
	{
		Search(Middle + 1, High, Place, BestSquared, Best);
		if (Across * Across <= BestSquared)
		{
			Search(Low, Middle, Place, BestSquared, Best);
		}
	}
}

int32 FNearestPointFinder::FindNearest(const FVector3d& Place, FVector3d* OutPlace) const
{
	double BestSquared = TNumericLimits<double>::Max();
	int32 Best = INDEX_NONE;
	Search(0, Points.Num(), Place, BestSquared, Best);
	if (Best == INDEX_NONE)
	{
		return INDEX_NONE;
	}
	if (OutPlace)
	{
		*OutPlace = Points[Best].Place;
	}
	return Points[Best].Number;
}

FVector3d FAsymmetryMorphBrushOp::SourcePlace(const FVector3d& Place, const FVector3d& Offset, bool& bOutOnPositiveSide)
{
	// The +X side sits at the mirror image of the -X side plus Offset. So the sides meet half way between a point and
	// its source, where X is half the Offset's, and each side is taken back to the other by the opposite step.
	bOutOnPositiveSide = Place.X >= 0.5 * Offset.X;
	return bOutOnPositiveSide
		? FVector3d(-(Place.X - Offset.X), Place.Y - Offset.Y, Place.Z - Offset.Z)
		: FVector3d(-Place.X + Offset.X, Place.Y + Offset.Y, Place.Z + Offset.Z);
}

void FAsymmetryMorphBrushOp::BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices)
{
	FMorphBrushOp::BeginStroke(Mesh, Stamp, InitialVertices);
	Sources.Reset();
	Mirrored.Reset();
	bCanAct = false;
	if (!IsEditable())
	{
		return;
	}

	// The settings hold for the whole stroke: a point's source is looked for once.
	const USculptBrushPackAsymmetryMorphOpProps* Props = GetPropertySetAs<USculptBrushPackAsymmetryMorphOpProps>();
	Offset = FVector3d(Props->Offset);
	CopyFrom = Props->CopyFrom;

	// With its Symmetry on, the tool sets the -X side from the +X side after every stamp, whatever a brush did there.
	if (Context->IsToolSymmetryOn())
	{
		Context->ReportProblem(LOCTEXT("AsymmetryWithSymmetry", "Asymmetry Morph does nothing while the tool's Symmetry is on: Symmetry sets the -X side from the +X side under every brush. Turn Symmetry off to use this brush."));
		return;
	}

	// The whole mesh's points as they stand now. The deltas are those of this stroke's beginning; the places do not
	// change while the tool runs, so the tree over them is built anew only when they do.
	const int32 Count = Context->GetReferencePointCount();
	TArray<FVector3d> NewPlaces;
	TArray<bool> bNewInUse;
	NewPlaces.SetNumZeroed(Count);
	bNewInUse.SetNumZeroed(Count);
	Deltas.SetNumZeroed(Count, EAllowShrinking::No);
	int32 InUse = 0;
	for (int32 Point = 0; Point < Count; ++Point)
	{
		FVector3d Place;
		FVector3d Delta;
		if (Context->GetReferencePoint(Point, Place, Delta))
		{
			NewPlaces[Point] = Place;
			Deltas[Point] = Delta;
			bNewInUse[Point] = true;
			++InUse;
		}
	}
	if (InUse == 0)
	{
		Context->ReportProblem(LOCTEXT("AsymmetryNoPoints", "Asymmetry Morph could not read the mesh's points in its reference pose, and moved nothing."));
		return;
	}
	if (Finder.Num() == 0 || NewPlaces != Places || bNewInUse != bInUse)
	{
		Places = MoveTemp(NewPlaces);
		bInUse = MoveTemp(bNewInUse);
		TArray<FVector3d> TreePlaces;
		TArray<int32> TreeNumbers;
		TreePlaces.Reserve(InUse);
		TreeNumbers.Reserve(InUse);
		for (int32 Point = 0; Point < Count; ++Point)
		{
			if (bInUse[Point])
			{
				TreePlaces.Add(Places[Point]);
				TreeNumbers.Add(Point);
			}
		}
		Finder.Build(TreePlaces, TreeNumbers);
	}
	bCanAct = Finder.Num() > 0;
}

void FAsymmetryMorphBrushOp::PrepareStamp(const FSculptBrushStamp& Stamp, const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices)
{
	if (!bCanAct)
	{
		return;
	}
	if (Sources.Num() < Mesh.MaxVertexID())
	{
		Sources.SetNumZeroed(Mesh.MaxVertexID());
		Mirrored.SetNumUninitialized(Mesh.MaxVertexID());
	}

	// The stamp's points whose source has not been looked for: where each stands in the whole mesh, and where its source
	// is looked for. A point on the side that is copied from keeps its shape.
	struct FSought
	{
		int32 Vertex;
		int32 Point;
		FVector3d Place;
	};
	TArray<FSought> Sought;
	for (const int32 Vertex : Vertices)
	{
		if (Sources[Vertex] != ESource::Unknown)
		{
			continue;
		}
		const int32 Point = Context->GetReferencePointOf(Vertex);
		if (!bInUse.IsValidIndex(Point) || !bInUse[Point])
		{
			Sources[Vertex] = ESource::Keeps;
			continue;
		}
		bool bOnPositiveSide = false;
		const FVector3d Place = SourcePlace(Places[Point], Offset, bOnPositiveSide);
		const bool bIsCopiedFrom = (bOnPositiveSide && CopyFrom == ESculptBrushPackMirrorSource::PositiveX)
			|| (!bOnPositiveSide && CopyFrom == ESculptBrushPackMirrorSource::NegativeX);
		if (bIsCopiedFrom)
		{
			Sources[Vertex] = ESource::Keeps;
			continue;
		}
		Sought.Add({ Vertex, Point, Place });
	}

	// Each has a vertex of its own, so each writes entries of its own.
	ParallelFor(Sought.Num(), [&](int32 Index)
	{
		const FSought& One = Sought[Index];
		FVector3d FoundAt;
		const int32 Source = Finder.FindNearest(One.Place, &FoundAt);
		if (Source == INDEX_NONE)
		{
			Sources[One.Vertex] = ESource::Keeps;
			return;
		}
		// A point whose source stands where it stands itself is its own mirror image, and so are the points that share
		// its place: it keeps what lies in the plane of its own delta.
		const bool bIsItsOwn = FoundAt == Places[One.Point];
		const FVector3d& Delta = Deltas[bIsItsOwn ? One.Point : Source];
		Mirrored[One.Vertex] = FVector3d(bIsItsOwn ? 0.0 : -Delta.X, Delta.Y, Delta.Z);
		Sources[One.Vertex] = ESource::Found;
	});
}

FVector3d FAsymmetryMorphBrushOp::FullTarget(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Base) const
{
	FVector3d Shown;
	if (!bCanAct || !Sources.IsValidIndex(Vertex) || Sources[Vertex] != ESource::Found || !Context->GetOffsetOfDelta(Vertex, Mirrored[Vertex], Shown))
	{
		return Before;
	}
	return Base + Shown;
}

void FMushMorphBrushOp::BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices)
{
	FMorphBrushOp::BeginStroke(Mesh, Stamp, InitialVertices);

	// The smoothing holds for the whole stroke.
	RoundCount = Private::ClampedRounds(GetPropertySetAs<USculptBrushPackMushMorphOpProps>()->Smoothing);
	for (FSmoothedShape& Shape : Shapes)
	{
		Shape.Places.Reset();
		Shape.Places.SetNum(RoundCount);
		Shape.Unknown.Reset();
		Shape.Unknown.SetNum(RoundCount);
	}
	Mushed.Reset();
	Unmushed.Reset();
	bToldOfUnmushed = false;
}

bool FMushMorphBrushOp::Place(const FDynamicMesh3& Mesh, EShape Shape, int32 Vertex, int32 Round, FVector3d& OutPlace)
{
	if (Round == 0)
	{
		if (Shape == Posed)
		{
			OutPlace = Mesh.GetVertex(Vertex);
			return true;
		}
		return Context->GetUnposedPosition(Vertex, /*bWithEditedMorph*/ true, OutPlace);
	}

	FSmoothedShape& Smoothed = Shapes[Shape];
	if (const FVector3d* Known = Smoothed.Places[Round - 1].Find(Vertex))
	{
		OutPlace = *Known;
		return true;
	}
	if (Smoothed.Unknown[Round - 1].Contains(Vertex))
	{
		return false;
	}

	// The point's place and its neighbors', each counted once, as the round before left them.
	FVector3d Sum;
	bool bKnown = Place(Mesh, Shape, Vertex, Round - 1, Sum);
	int32 Count = 1;
	if (bKnown)
	{
		for (const int32 Neighbor : Mesh.VtxVerticesItr(Vertex))
		{
			FVector3d NeighborPlace;
			if (!Place(Mesh, Shape, Neighbor, Round - 1, NeighborPlace))
			{
				bKnown = false;
				break;
			}
			Sum += NeighborPlace;
			++Count;
		}
	}
	if (!bKnown)
	{
		Smoothed.Unknown[Round - 1].Add(Vertex);
		return false;
	}
	OutPlace = Sum / static_cast<double>(Count);
	Smoothed.Places[Round - 1].Add(Vertex, OutPlace);
	return true;
}

bool FMushMorphBrushOp::Directions(const FDynamicMesh3& Mesh, EShape Shape, int32 Vertex, int32& InOutToward, FVector3d& OutPlace, FVector3d OutAxes[3])
{
	if (!Place(Mesh, Shape, Vertex, RoundCount, OutPlace))
	{
		return false;
	}

	// The smoothed surface's normal at the point: its triangles' normals, each weighted by the triangle's area.
	FVector3d Normal = FVector3d::Zero();
	for (const int32 Triangle : Mesh.VtxTrianglesItr(Vertex))
	{
		const UE::Geometry::FIndex3i Corners = Mesh.GetTriangle(Triangle);
		FVector3d A, B, C;
		if (!Place(Mesh, Shape, Corners.A, RoundCount, A) || !Place(Mesh, Shape, Corners.B, RoundCount, B) || !Place(Mesh, Shape, Corners.C, RoundCount, C))
		{
			return false;
		}
		Normal += UE::Geometry::VectorUtil::NormalDirection(A, B, C);
	}
	const double NormalLength = Normal.Length();
	if (!(NormalLength > 0.0))
	{
		return false;
	}
	Normal /= NormalLength;

	// A direction across the surface: toward a neighbor, with what lies along the normal taken out. Both shapes look
	// toward the same neighbor, the lowest-numbered one that does not lie along the unposed shape's normal.
	const FVector3d Origin = OutPlace;
	auto Across = [this, &Mesh, Shape, &Normal, &Origin](int32 Neighbor, FVector3d& OutAcross)
	{
		FVector3d NeighborPlace;
		if (!Place(Mesh, Shape, Neighbor, RoundCount, NeighborPlace))
		{
			return false;
		}
		FVector3d Toward = NeighborPlace - Origin;
		Toward -= Normal * Toward.Dot(Normal);
		const double Length = Toward.Length();
		if (!(Length > 0.0))
		{
			return false;
		}
		OutAcross = Toward / Length;
		return true;
	};

	if (InOutToward == INDEX_NONE)
	{
		TArray<int32, TInlineAllocator<16>> Neighbors;
		for (const int32 Neighbor : Mesh.VtxVerticesItr(Vertex))
		{
			Neighbors.Add(Neighbor);
		}
		Neighbors.Sort();
		for (const int32 Neighbor : Neighbors)
		{
			if (Across(Neighbor, OutAxes[0]))
			{
				InOutToward = Neighbor;
				break;
			}
		}
		if (InOutToward == INDEX_NONE)
		{
			return false;
		}
	}
	else if (!Across(InOutToward, OutAxes[0]))
	{
		return false;
	}
	OutAxes[1] = Normal.Cross(OutAxes[0]);
	OutAxes[2] = Normal;
	return true;
}

void FMushMorphBrushOp::PrepareStamp(const FSculptBrushStamp& Stamp, const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices)
{
	if (RoundCount == 0)
	{
		RoundCount = Private::ClampedRounds(GetPropertySetAs<USculptBrushPackMushMorphOpProps>()->Smoothing);
		for (FSmoothedShape& Shape : Shapes)
		{
			Shape.Places.SetNum(RoundCount);
			Shape.Unknown.SetNum(RoundCount);
		}
	}

	// One point after another: a point's smoothed places are built from its neighbors', which other points share.
	for (const int32 Vertex : Vertices)
	{
		if (Mushed.Contains(Vertex) || Unmushed.Contains(Vertex))
		{
			continue;
		}
		int32 Toward = INDEX_NONE;
		FVector3d UnposedPlace, UnposedSmoothed, PosedSmoothed;
		FVector3d UnposedAxes[3];
		FVector3d PosedAxes[3];
		if (Context->GetUnposedPosition(Vertex, /*bWithEditedMorph*/ true, UnposedPlace)
			&& Directions(Mesh, Unposed, Vertex, Toward, UnposedSmoothed, UnposedAxes)
			&& Directions(Mesh, Posed, Vertex, Toward, PosedSmoothed, PosedAxes))
		{
			// The model's detail at the point, measured along the smoothed unposed surface's directions and put back
			// along the smoothed posed surface's.
			const FVector3d Detail = UnposedPlace - UnposedSmoothed;
			Mushed.Add(Vertex, PosedSmoothed
				+ PosedAxes[0] * Detail.Dot(UnposedAxes[0])
				+ PosedAxes[1] * Detail.Dot(UnposedAxes[1])
				+ PosedAxes[2] * Detail.Dot(UnposedAxes[2]));
		}
		else
		{
			Unmushed.Add(Vertex);
		}
	}

	if (!bToldOfUnmushed && Mushed.IsEmpty() && !Unmushed.IsEmpty())
	{
		bToldOfUnmushed = true;
		Context->ReportProblem(LOCTEXT("MushNothingKnown", "Mush Morph could not measure the surface under the brush against the mesh's reference pose, and moved nothing."));
	}
}

FVector3d FMushMorphBrushOp::FullTarget(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Base) const
{
	const FVector3d* Target = Mushed.Find(Vertex);
	return Target ? *Target : Before;
}

void FNuWrinkleBrushOp::BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices)
{
	FMorphBrushOp::BeginStroke(Mesh, Stamp, InitialVertices);
	const USculptBrushPackNuWrinkleOpProps* Props = GetPropertySetAs<USculptBrushPackNuWrinkleOpProps>();

	Rise.Reset();
	OwnCompression.Reset();
	Seen.Reset();
	SqueezeFrom.Reset();
	// The softening holds for the whole stroke.
	SoftenedCompression.Reset();
	SoftenedCompression.SetNum(FMath::Clamp(Props->Soften, 0, Private::MaxSoftenRounds));
	bBuildUp = Props->bBuildUp;
	bHasFoldDirection = false;
	bFoldsInUnposedMesh = false;
	FoldDirection = FVector3d::ZeroVector;
	StrokeStart = Stamp.LocalFrame.Origin;
	for (double& Term : Squeeze)
	{
		Term = 0.0;
	}
	PointsCompressed = 0;
	bPointsWait = false;
	bToldOfMap = false;
	if (!IsEditable())
	{
		return;
	}

	// The map is read once for a texture and kept while the brush is active.
	UTexture2D* Texture = Props->bUseMap ? Props->WrinkleMap.Get() : nullptr;
	if (!Texture)
	{
		MapTexture = nullptr;
		bHasMap = false;
		Map = FMapImage();
	}
	else if (Texture != MapTexture.Get())
	{
		MapTexture = Texture;
		Map = FMapImage();
		bHasMap = ReadMap ? ReadMap(Texture, Map) : ReadTextureAsMap(Texture, Map);
	}
	if (Texture && !bHasMap)
	{
		bToldOfMap = true;
		Context->ReportProblem(FText::Format(LOCTEXT("WrinkleMapUnreadable", "Nu Wrinkle could not read the texture {0} as a Wrinkle Map, so nothing is raised. Choose another texture or turn off Use Wrinkle Map."), FText::FromString(Texture->GetName())));
	}
}

bool FNuWrinkleBrushOp::ReadTextureAsMap(UTexture2D* Texture, FMapImage& OutMap)
{
	return Private::ReadMapWithEngine(Texture, OutMap);
}

double FNuWrinkleBrushOp::FoldShape(ESculptBrushPackFoldProfile Profile, double Tent)
{
	switch (Profile)
	{
	case ESculptBrushPackFoldProfile::Waves:
		return Tent * Tent * (3.0 - 2.0 * Tent);
	case ESculptBrushPackFoldProfile::Ridges:
		return Tent * Tent;
	case ESculptBrushPackFoldProfile::Rolls:
	default:
		return 1.0 - FMath::Square(1.0 - Tent);
	}
}

double FNuWrinkleBrushOp::FoldVariation(int64 Fold)
{
	// The engine's random stream, seeded with the fold's number: the same fold always gets the same number.
	return FRandomStream(static_cast<int32>(Fold)).GetFraction();
}

bool FNuWrinkleBrushOp::GetFoldDirection(FVector3d& OutDirection, bool& bOutInUnposedMesh) const
{
	OutDirection = FoldDirection;
	bOutInUnposedMesh = bFoldsInUnposedMesh;
	return bHasFoldDirection;
}

double FNuWrinkleBrushOp::Compression(const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, int32 Vertex)
{
	if (const double* Known = OwnCompression.Find(Vertex))
	{
		return *Known;
	}

	// The lengths of the point's edges, in the pose and with the pose taken off; both without the edited morph. Where
	// they cannot be told the point counts as not compressed.
	double Share = 0.0;
	FVector3d RestPlace;
	if (Without.IsVertex(Vertex) && Context->GetUnposedPosition(Vertex, /*bWithEditedMorph*/ false, RestPlace))
	{
		const FVector3d PosedPlace = Without.GetVertex(Vertex);
		double PosedLength = 0.0;
		double RestLength = 0.0;
		bool bKnown = true;
		for (const int32 Neighbor : Mesh.VtxVerticesItr(Vertex))
		{
			FVector3d RestNeighbor;
			if (!Without.IsVertex(Neighbor) || !Context->GetUnposedPosition(Neighbor, /*bWithEditedMorph*/ false, RestNeighbor))
			{
				bKnown = false;
				break;
			}
			PosedLength += UE::Geometry::Distance(Without.GetVertex(Neighbor), PosedPlace);
			RestLength += UE::Geometry::Distance(RestNeighbor, RestPlace);
		}
		if (bKnown && RestLength > 0.0)
		{
			const double Shorter = 1.0 - PosedLength / RestLength;
			if (Shorter > StartsAt)
			{
				Share = FullAt > StartsAt ? FMath::Min(1.0, (Shorter - StartsAt) / (FullAt - StartsAt)) : 1.0;
			}
		}
	}
	OwnCompression.Add(Vertex, Share);
	return Share;
}

double FNuWrinkleBrushOp::Softened(const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, int32 Vertex, int32 Round)
{
	if (Round == 0)
	{
		return Compression(Mesh, Without, Vertex);
	}
	if (const double* Known = SoftenedCompression[Round - 1].Find(Vertex))
	{
		return *Known;
	}
	// The point's share and its neighbors', each counted once, as the round before left them.
	double Sum = Softened(Mesh, Without, Vertex, Round - 1);
	int32 Count = 1;
	for (const int32 Neighbor : Mesh.VtxVerticesItr(Vertex))
	{
		Sum += Softened(Mesh, Without, Neighbor, Round - 1);
		++Count;
	}
	const double Share = Sum / static_cast<double>(Count);
	SoftenedCompression[Round - 1].Add(Vertex, Share);
	return Share;
}

void FNuWrinkleBrushOp::AddSqueeze(const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, int32 Vertex)
{
	bool bAlreadyAdded = false;
	SqueezeFrom.Add(Vertex, &bAlreadyAdded);
	FVector3d RestPlace;
	if (bAlreadyAdded || !Without.IsVertex(Vertex) || !Context->GetUnposedPosition(Vertex, /*bWithEditedMorph*/ false, RestPlace))
	{
		return;
	}
	const FVector3d PosedPlace = Without.GetVertex(Vertex);
	for (const int32 Neighbor : Mesh.VtxVerticesItr(Vertex))
	{
		FVector3d RestNeighbor;
		if (!Without.IsVertex(Neighbor) || !Context->GetUnposedPosition(Neighbor, /*bWithEditedMorph*/ false, RestNeighbor))
		{
			continue;
		}
		const FVector3d RestEdge = RestNeighbor - RestPlace;
		const double RestLength = RestEdge.Length();
		const double PosedLength = UE::Geometry::Distance(Without.GetVertex(Neighbor), PosedPlace);
		if (RestLength > 0.0 && PosedLength < RestLength)
		{
			// The edge's shortening, as a share of its length, along the edge's own direction in the unposed mesh.
			const double Shorter = 1.0 - PosedLength / RestLength;
			const FVector3d Way = RestEdge / RestLength;
			Squeeze[0] += Shorter * Way.X * Way.X;
			Squeeze[1] += Shorter * Way.Y * Way.Y;
			Squeeze[2] += Shorter * Way.Z * Way.Z;
			Squeeze[3] += Shorter * Way.X * Way.Y;
			Squeeze[4] += Shorter * Way.X * Way.Z;
			Squeeze[5] += Shorter * Way.Y * Way.Z;
		}
	}
}

bool FNuWrinkleBrushOp::FindSqueezeDirection(FVector3d& OutDirection) const
{
	auto Times = [this](const FVector3d& V)
	{
		return FVector3d(
			Squeeze[0] * V.X + Squeeze[3] * V.Y + Squeeze[4] * V.Z,
			Squeeze[3] * V.X + Squeeze[1] * V.Y + Squeeze[5] * V.Z,
			Squeeze[4] * V.X + Squeeze[5] * V.Y + Squeeze[2] * V.Z);
	};

	// The direction the sum stretches most is found by multiplying a start by the sum again and again. The start leans
	// on no axis; another is tried where the first happens to lie square to the direction sought.
	const FVector3d Starts[3] = { FVector3d(1, 1, 1), FVector3d(1, 0, 0), FVector3d(0, 1, 0) };
	for (const FVector3d& Start : Starts)
	{
		FVector3d Axis = Start;
		bool bFound = true;
		for (int32 Round = 0; Round < Private::SqueezeRounds && bFound; ++Round)
		{
			const FVector3d Next = Times(Axis);
			const double Length = Next.Length();
			bFound = Length > 0.0;
			if (bFound)
			{
				Axis = Next / Length;
			}
		}
		if (bFound)
		{
			// Of the two ways along that direction, always the same one: that whose largest part is positive.
			const FVector3d Size(FMath::Abs(Axis.X), FMath::Abs(Axis.Y), FMath::Abs(Axis.Z));
			const double Largest = Size.X >= Size.Y && Size.X >= Size.Z ? Axis.X : (Size.Y >= Size.Z ? Axis.Y : Axis.Z);
			OutDirection = Largest < 0.0 ? -Axis : Axis;
			return true;
		}
	}
	return false;
}

void FNuWrinkleBrushOp::PrepareStamp(const FSculptBrushStamp& Stamp, const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices)
{
	const USculptBrushPackNuWrinkleOpProps* Props = GetPropertySetAs<USculptBrushPackNuWrinkleOpProps>();
	Height = Props->Height;
	bBuildUp = Props->bBuildUp;
	bOnlyWhereCompressed = Props->bOnlyWhereCompressed;
	StartsAt = Props->StartsAt;
	FullAt = Props->FullAt;
	bFolds = Props->bFolds;
	Direction = Props->Direction;
	Spacing = Props->Spacing;
	Profile = Props->Profile;
	bCutBetween = Props->bCutBetween;
	Irregularity = Props->Irregularity;
	bMidGreyIsFlat = Props->bMidGreyIsFlat;
	MapTiling = Props->MapTiling;
	UVChannel = Props->UVChannel;
	const bool bWantsMap = Props->bUseMap && Props->WrinkleMap != nullptr;
	const bool bLaysFolds = bFolds && Spacing > 0.0;

	// The points whose rise this stamp may settle, each with the share its compression gives it.
	TArray<TPair<int32, double>> Unsettled;
	bool bNeedsDirection = false;
	for (const int32 Vertex : Vertices)
	{
		if (Rise.Contains(Vertex) || !Without.IsVertex(Vertex))
		{
			continue;
		}
		const double Share = bOnlyWhereCompressed ? Softened(Mesh, Without, Vertex, SoftenedCompression.Num()) : 1.0;
		bool bAlreadySeen = false;
		Seen.Add(Vertex, &bAlreadySeen);
		if (!bAlreadySeen && Share > 0.0)
		{
			++PointsCompressed;
		}
		bNeedsDirection |= bLaysFolds && Share > 0.0;
		Unsettled.Emplace(Vertex, Share);
	}

	// The folds' direction, found with the first stamp that needs it and kept for the stroke.
	if (bNeedsDirection && !bHasFoldDirection)
	{
		if (Direction == ESculptBrushPackFoldDirection::AcrossCompression)
		{
			// What the shortened edges around the points that will rise, and around their neighbors, agree on.
			for (const TPair<int32, double>& Point : Unsettled)
			{
				if (Point.Value > 0.0)
				{
					AddSqueeze(Mesh, Without, Point.Key);
					for (const int32 Neighbor : Mesh.VtxVerticesItr(Point.Key))
					{
						AddSqueeze(Mesh, Without, Neighbor);
					}
				}
			}
			if (FindSqueezeDirection(FoldDirection))
			{
				bHasFoldDirection = true;
				bFoldsInUnposedMesh = true;
			}
		}
		if (!bHasFoldDirection)
		{
			// Across the way the stroke has moved since it began; the folds then run along the stroke.
			const FVector3d Normal = Stamp.LocalFrame.Z();
			FVector3d Step = Stamp.LocalFrame.Origin - StrokeStart;
			Step -= Normal * Step.Dot(Normal);
			const double StepLength = Step.Length();
			if (StepLength > 0.0)
			{
				FoldDirection = Normal.Cross(Step / StepLength);
				bHasFoldDirection = true;
				bFoldsInUnposedMesh = false;
			}
		}
	}

	bool bLacksUV = false;
	for (const TPair<int32, double>& Point : Unsettled)
	{
		const int32 Vertex = Point.Key;
		const double Share = Point.Value;
		const bool bInAFold = bLaysFolds && Share > 0.0;
		if (bInAFold && !bHasFoldDirection)
		{
			// The point waits for the stroke to move; a later stamp settles it.
			bPointsWait = true;
			continue;
		}

		double Pattern = 1.0;
		if (bInAFold)
		{
			// The point's place along the folds' direction says which fold it is in and how far across it.
			double Place = 0.0;
			FVector3d Unposed;
			if (!bFoldsInUnposedMesh)
			{
				Place = (Without.GetVertex(Vertex) - StrokeStart).Dot(FoldDirection);
			}
			else if (Context->GetUnposedPosition(Vertex, /*bWithEditedMorph*/ false, Unposed))
			{
				Place = Unposed.Dot(FoldDirection);
			}
			const double Folds = Place / Spacing;
			const double Fold = FMath::FloorToDouble(Folds);
			const double Tent = 1.0 - FMath::Abs(2.0 * (Folds - Fold) - 1.0);
			double Shape = FoldShape(Profile, Tent);
			if (bCutBetween)
			{
				Shape = 2.0 * Shape - 1.0;
			}
			Pattern = Shape * (1.0 - Irregularity * FoldVariation(static_cast<int64>(Fold)));
		}

		// The Wrinkle Map's value at the point's place in the mesh's UVs.
		if (bWantsMap)
		{
			FVector2d UV;
			if (!bHasMap)
			{
				Pattern = 0.0;
			}
			else if (!Context->GetUV(Vertex, UVChannel, UV))
			{
				Pattern = 0.0;
				bLacksUV = true;
			}
			else
			{
				const double Value = Map.BilinearSampleUV<float, UE::Geometry::EImageTilingMethod::Wrap>(UV * MapTiling, 0.0f);
				Pattern *= bMidGreyIsFlat ? 2.0 * Value - 1.0 : Value;
			}
		}

		// The surface's normal at the point, without the edited morph: its triangles' normals, each weighted by the
		// triangle's area.
		FVector3d Normal = FVector3d::Zero();
		for (const int32 Triangle : Mesh.VtxTrianglesItr(Vertex))
		{
			const UE::Geometry::FIndex3i Corners = Mesh.GetTriangle(Triangle);
			if (Without.IsVertex(Corners.A) && Without.IsVertex(Corners.B) && Without.IsVertex(Corners.C))
			{
				Normal += UE::Geometry::VectorUtil::NormalDirection(Without.GetVertex(Corners.A), Without.GetVertex(Corners.B), Without.GetVertex(Corners.C));
			}
		}
		const double NormalLength = Normal.Length();
		Rise.Add(Vertex, NormalLength > 0.0 ? (Normal / NormalLength) * (Height * Share * Pattern) : FVector3d::Zero());
	}

	if (bLacksUV && !bToldOfMap)
	{
		bToldOfMap = true;
		Context->ReportProblem(FText::Format(LOCTEXT("WrinkleNoUVs", "Nu Wrinkle: the mesh has no UV channel {0} to read the Wrinkle Map with, so nothing is raised there. Choose another UV Channel or turn off Use Wrinkle Map."), FText::AsNumber(UVChannel)));
	}
}

FVector3d FNuWrinkleBrushOp::FullTarget(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Base) const
{
	const FVector3d* Up = Rise.Find(Vertex);
	if (!Up)
	{
		return Before;
	}
	return Stamp.Direction < 0.0 ? Before - *Up : Before + *Up;
}

void FNuWrinkleBrushOp::EndStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& FinalVertices)
{
	// A stroke that could not act says why once it is over; the user is told what to do about it.
	if (IsEditable() && !bToldOfMap)
	{
		if (bPointsWait && !bHasFoldDirection)
		{
			Context->ReportProblem(LOCTEXT("NuWrinkleStrokeDidNotMove", "Nu Wrinkle lays its folds along the stroke here, and the stroke did not move. Drag the brush, or turn Folds off to raise the surface evenly."));
		}
		else if (bOnlyWhereCompressed && Seen.Num() > 0 && PointsCompressed == 0)
		{
			Context->ReportProblem(LOCTEXT("WrinkleNothingCompressed", "Nu Wrinkle raises wrinkles where the posed surface is shorter than at rest, and nothing under the brush was. Pose the mesh so that the surface bunches up, or turn off Only Where Compressed."));
		}
	}
	FMorphBrushOp::EndStroke(Mesh, Stamp, FinalVertices);
}
}

USculptBrushPackAmplifyMorphOpProps::USculptBrushPackAmplifyMorphOpProps()
{
	// At full strength the middle of the brush gets the whole Amount.
	Strength = 1.0f;
}

USculptBrushPackSmoothMorphOpProps::USculptBrushPackSmoothMorphOpProps()
{
	Strength = 1.0f;
}

USculptBrushPackCloneMorphOpProps::USculptBrushPackCloneMorphOpProps()
{
	// A copying brush wants a core where the copy is whole: the engine's Move brush's falloff (MeshMoveBrushOps.h:26 in
	// 5.8.4), with which the inner half of the brush reaches fully.
	Strength = 1.0f;
	Falloff = 0.5f;
}

TArray<FName> USculptBrushPackCloneMorphOpProps::GetSourceOptions() const
{
	return SourceNames ? SourceNames() : TArray<FName>();
}

USculptBrushPackMirrorMorphOpProps::USculptBrushPackMirrorMorphOpProps()
{
	// As for Clone Morph.
	Strength = 1.0f;
	Falloff = 0.5f;
}

USculptBrushPackAsymmetryMorphOpProps::USculptBrushPackAsymmetryMorphOpProps()
{
	// As for Clone Morph.
	Strength = 1.0f;
	Falloff = 0.5f;
}

USculptBrushPackMushMorphOpProps::USculptBrushPackMushMorphOpProps()
{
	Strength = 1.0f;
}

USculptBrushPackNuWrinkleOpProps::USculptBrushPackNuWrinkleOpProps()
{
	// At full strength the middle of the brush raises the whole Height.
	Strength = 1.0f;
	// One of the maps that come with the plugin stands in the Wrinkle Map as an example. Where the plugin's content is
	// not there, the map is simply none.
	WrinkleMap = LoadObject<UTexture2D>(nullptr, TEXT("/SculptBrushPackMorph/WrinkleMaps/T_WrinkleMap_Folds.T_WrinkleMap_Folds"), nullptr, LOAD_Quiet | LOAD_NoWarn);
}

#undef LOCTEXT_NAMESPACE
