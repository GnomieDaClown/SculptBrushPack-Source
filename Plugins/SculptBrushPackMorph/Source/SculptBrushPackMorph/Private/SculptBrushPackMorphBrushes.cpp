// Copyright (C) 2026 Joseph Wiginton.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sculpt Brush Pack: Morph Targets. The brushes' rules (see SculptBrushPackMorphBrushes.h).

#include "SculptBrushPackMorphBrushes.h"

#include "Async/ParallelFor.h"

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
	// 5.9.0), with which the inner half of the brush reaches fully.
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

#undef LOCTEXT_NAMESPACE
