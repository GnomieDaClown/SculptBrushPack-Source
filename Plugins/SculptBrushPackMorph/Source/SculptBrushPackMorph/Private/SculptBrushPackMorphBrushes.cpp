// Copyright (C) 2026 Joseph Wiginton.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sculpt Brush Pack: Morph Targets. The brushes' rules (see SculptBrushPackMorphBrushes.h).

#include "SculptBrushPackMorphBrushes.h"

#include "AssetUtils/Texture2DUtil.h"
#include "Async/ParallelFor.h"
#include "Engine/Texture2D.h"
#include "IndexTypes.h"
#include "VectorUtil.h"

#define LOCTEXT_NAMESPACE "SculptBrushPackMorph"

using UE::Geometry::FDynamicMesh3;

namespace SculptBrushPackMorph
{
namespace Private
{
	// Smooth Morph's most rounds: a stroke keeps one offset per vertex of the mesh and round.
	constexpr int32 MaxSmoothingRounds = 16;

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
	PrepareStamp(*Mesh, *Without, Vertices);

	// The tool lists each vertex once, so each k reads and writes entries of its own.
	ParallelFor(Vertices.Num(), [&](int32 k)
	{
		const int32 Vertex = Vertices[k];
		const FVector3d Before = Mesh->GetVertex(Vertex);
		Reach[Vertex] = FMath::Max(Reach[Vertex], Private::EvaluateWeight(*this, Stamp, Before) * Stamp.Power);
		if (!bHasTarget[Vertex])
		{
			Targets[Vertex] = Without->IsVertex(Vertex) ? FullTarget(Stamp, Vertex, Before, Without->GetVertex(Vertex)) : Before;
			bHasTarget[Vertex] = true;
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

void FAmplifyMorphBrushOp::PrepareStamp(const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices)
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

void FSmoothMorphBrushOp::PrepareStamp(const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices)
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

void FCloneMorphBrushOp::PrepareStamp(const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices)
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

void FMirrorMorphBrushOp::PrepareStamp(const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices)
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

void FMushMorphBrushOp::PrepareStamp(const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices)
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

void FWrinkleMorphBrushOp::BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices)
{
	FMorphBrushOp::BeginStroke(Mesh, Stamp, InitialVertices);
	Rise.Reset();
	PointsSeen = 0;
	PointsCompressed = 0;
	bToldOfMap = false;
	if (!IsEditable())
	{
		return;
	}

	// The map is read once for a texture and kept while the brush is active.
	UTexture2D* Texture = GetPropertySetAs<USculptBrushPackWrinkleMorphOpProps>()->WrinkleMap;
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
		Context->ReportProblem(FText::Format(LOCTEXT("WrinkleMapUnreadable", "Wrinkle Morph could not read the texture {0} as a Wrinkle Map, so nothing is raised. Choose another texture or clear the map."), FText::FromString(Texture->GetName())));
	}
}

bool FWrinkleMorphBrushOp::ReadTextureAsMap(UTexture2D* Texture, FMapImage& OutMap)
{
	return Private::ReadMapWithEngine(Texture, OutMap);
}

double FWrinkleMorphBrushOp::Compression(const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, int32 Vertex) const
{
	// The lengths of the point's edges, in the pose and with the pose taken off; both without the edited morph.
	FVector3d RestPlace;
	if (!Context->GetUnposedPosition(Vertex, /*bWithEditedMorph*/ false, RestPlace))
	{
		return 0.0;
	}
	const FVector3d PosedPlace = Without.GetVertex(Vertex);
	double PosedLength = 0.0;
	double RestLength = 0.0;
	for (const int32 Neighbor : Mesh.VtxVerticesItr(Vertex))
	{
		FVector3d RestNeighbor;
		if (!Without.IsVertex(Neighbor) || !Context->GetUnposedPosition(Neighbor, /*bWithEditedMorph*/ false, RestNeighbor))
		{
			return 0.0;
		}
		PosedLength += UE::Geometry::Distance(Without.GetVertex(Neighbor), PosedPlace);
		RestLength += UE::Geometry::Distance(RestNeighbor, RestPlace);
	}
	if (!(RestLength > 0.0))
	{
		return 0.0;
	}
	const double Shorter = 1.0 - PosedLength / RestLength;
	if (Shorter <= StartsAt)
	{
		return 0.0;
	}
	return FullAt > StartsAt ? FMath::Min(1.0, (Shorter - StartsAt) / (FullAt - StartsAt)) : 1.0;
}

void FWrinkleMorphBrushOp::PrepareStamp(const FDynamicMesh3& Mesh, const FDynamicMesh3& Without, const TArray<int32>& Vertices)
{
	const USculptBrushPackWrinkleMorphOpProps* Props = GetPropertySetAs<USculptBrushPackWrinkleMorphOpProps>();
	Height = Props->Height;
	bOnlyWhereCompressed = Props->bOnlyWhereCompressed;
	StartsAt = Props->StartsAt;
	FullAt = Props->FullAt;
	bMidGreyIsFlat = Props->bMidGreyIsFlat;
	MapTiling = Props->MapTiling;
	UVChannel = Props->UVChannel;
	const bool bWantsMap = Props->WrinkleMap != nullptr;

	bool bLacksUV = false;
	for (const int32 Vertex : Vertices)
	{
		if (Rise.Contains(Vertex) || !Without.IsVertex(Vertex))
		{
			continue;
		}
		++PointsSeen;
		const double Share = bOnlyWhereCompressed ? Compression(Mesh, Without, Vertex) : 1.0;
		PointsCompressed += Share > 0.0 ? 1 : 0;

		// The pattern: the map's value at the point's place in the mesh's UVs, or 1 without a map.
		double Pattern = 1.0;
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
				Pattern = bMidGreyIsFlat ? 2.0 * Value - 1.0 : Value;
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
		Context->ReportProblem(FText::Format(LOCTEXT("WrinkleNoUVs", "Wrinkle Morph: the mesh has no UV channel {0} to read the Wrinkle Map with, so nothing is raised there. Choose another UV Channel or clear the map."), FText::AsNumber(UVChannel)));
	}
}

FVector3d FWrinkleMorphBrushOp::FullTarget(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Base) const
{
	const FVector3d* Up = Rise.Find(Vertex);
	if (!Up)
	{
		return Before;
	}
	return Stamp.Direction < 0.0 ? Before - *Up : Before + *Up;
}

void FWrinkleMorphBrushOp::EndStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& FinalVertices)
{
	// A stroke over surface that is nowhere compressed does nothing; the user is told what to do about it.
	if (IsEditable() && bOnlyWhereCompressed && PointsSeen > 0 && PointsCompressed == 0 && !bToldOfMap)
	{
		Context->ReportProblem(LOCTEXT("WrinkleNothingCompressed", "Wrinkle Morph raises wrinkles where the posed surface is shorter than at rest, and nothing under the brush was. Pose the mesh so that the surface bunches up, or turn off Only Where Compressed."));
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

USculptBrushPackMushMorphOpProps::USculptBrushPackMushMorphOpProps()
{
	Strength = 1.0f;
}

USculptBrushPackWrinkleMorphOpProps::USculptBrushPackWrinkleMorphOpProps()
{
	// At full strength the middle of the brush raises the whole Height.
	Strength = 1.0f;
}

#undef LOCTEXT_NAMESPACE
