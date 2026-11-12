// Copyright (C) 2026 Joseph Wiginton.
// SPDX-License-Identifier: GPL-3.0-or-later
// Rules and constants of fourteen brushes follow Blender's sculpt source: Copyright Blender Authors, GPL-2.0-or-later.
//
// Sculpt Brush Pack. The brushes' rules (see SculptBrushPackBrushes.h).
// Blender anchors are blender/source/blender/editors/sculpt_paint/mesh/<file>:<line> in tag v5.2.3 unless another path or
// tag is given. No constant in the brushes from Clay to Relax is the pack's own: each is Blender's or the engine's, with
// its line. Glide, Tube, Noise and Sharpen are this project's own designs; what they take from the engine is cited.

#include "SculptBrushPackBrushes.h"

#include "Async/ParallelFor.h"
#include "DynamicMesh/MeshNormals.h"

using UE::Geometry::FDynamicMesh3;

namespace SculptBrushPack
{
namespace Private
{
	// The speed of the engine's displacement brushes (BrushSpeedTuning, MeshSculptBrushOps.h:39 and :98 in 5.8.4).
	constexpr double EngineStampSpeed = 6.0;

	// Blender's strength factors per brush (brush_strength, sculpt.cc).
	constexpr double ClayStrengthFactor = 0.25;      // sculpt.cc:2353-2356
	constexpr double ClayStripsStrengthFactor = 0.3; // sculpt.cc:2392-2395

	bool IsUsable(const FVector3d& Vector)
	{
		return !Vector.ContainsNaN();
	}

	/** The average normal of the brushed area when the tool gave one (Stamp.RegionPlane), else the stamp's own normal. */
	FVector3d AreaNormal(const FSculptBrushStamp& Stamp)
	{
		FVector3d Normal = Stamp.RegionPlane.Z();
		return IsUsable(Normal) && Normal.Normalize() ? Normal : Stamp.LocalFrame.Z();
	}

	/** The middle of the brushed area when the tool gave one, else the cursor's point. */
	FVector3d AreaCenter(const FSculptBrushStamp& Stamp)
	{
		return IsUsable(Stamp.RegionPlane.Origin) ? Stamp.RegionPlane.Origin : Stamp.LocalFrame.Origin;
	}

	/** The tool's falloff at Position, times its alpha when it has one. */
	double EvaluateWeight(const FMeshSculptBrushOp& Op, const FSculptBrushStamp& Stamp, const FVector3d& Position)
	{
		const double Falloff = Op.GetFalloff().Evaluate(Stamp, Position);
		return Stamp.HasAlpha() ? Falloff * Stamp.StampAlphaFunc(Stamp, Position) : Falloff;
	}

	/** The tool's falloff at a distance from the cursor given as a fraction of the radius. */
	double FalloffAtDistance(const FMeshSculptBrushOp& Op, const FSculptBrushStamp& Stamp, double UnitDistance)
	{
		return Op.GetFalloff().Evaluate(Stamp, Stamp.LocalFrame.Origin + (UnitDistance * Stamp.Radius) * Stamp.LocalFrame.X());
	}

	/**
	 * Crease's and Blob's movement of one point (crease.cc:101-114): toward the cursor's point by PinchStrength, with the
	 * part along the normal taken out so the points gather on a line and not on a point, then along the normal by Offset.
	 */
	FVector3d CreaseMove(const FVector3d& Position, const FVector3d& Center, const FVector3d& Normal, const FVector3d& Offset, double PinchStrength, double PointWeight)
	{
		FVector3d Move = (Center - Position) * (PointWeight * PinchStrength);
		Move -= Normal * Move.Dot(Normal);
		return Move + Offset * PointWeight;
	}

	/**
	 * The pinch's strength (crease.cc, do_crease_or_blob_brush): the stamp's strength with the Strength setting divided out
	 * and Pinch squared in its place. Blender divides by the setting squared because its strength is the setting squared
	 * (sculpt.cc:2338-2339); the engine's is the setting itself (MeshSculptToolBase.cpp:1338-1363 in 5.8.4).
	 */
	double PinchStrength(const FSculptBrushStamp& Stamp, float StrengthSetting, float Pinch)
	{
		const double Strength = StampStrength(Stamp) * Pinch * Pinch;
		return StrengthSetting > 0.0f ? Strength / StrengthSetting : Strength;
	}
}
using namespace Private;

double StampStrength(const FSculptBrushStamp& Stamp)
{
	return Stamp.Power * Stamp.DeltaTime * EngineStampSpeed;
}

double TipDistance(double AbsX, double AbsY, double Roundness)
{
	if (FMath::Max(AbsX, AbsY) > 1.0)
	{
		return 1.0;
	}
	const double Hardness = 1.0 - Roundness;
	const double RoundnessInverse = Roundness != 0.0 ? 1.0 / Roundness : 0.0;
	if (FMath::Min(AbsX, AbsY) > Hardness)
	{
		// A corner: the distance to the middle of the corner's circle.
		return FMath::Sqrt(FMath::Square(AbsX - Hardness) + FMath::Square(AbsY - Hardness)) * RoundnessInverse;
	}
	if (FMath::Max(AbsX, AbsY) > Hardness)
	{
		// A side.
		return (FMath::Max(AbsX, AbsY) - Hardness) * RoundnessInverse;
	}
	return 0.0;
}

void FClayBrushOp::ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut)
{
	const float PlaneOffset = GetPropertySetAs<USculptBrushPackClayOpProps>()->PlaneOffset;
	const FVector3d Normal = AreaNormal(Stamp);
	// clay.cc:169-185: the plane passes through the cursor's point, moved along the area normal; below it when inverted.
	const double Displace = FMath::Abs(Stamp.Radius * PlaneOffset) * Stamp.Direction;
	const FVector3d PlanePoint = Stamp.LocalFrame.Origin + Displace * Normal;
	const double Strength = ClayStrengthFactor * StampStrength(Stamp);

	ParallelFor(Vertices.Num(), [&](int32 k)
	{
		const FVector3d Position = Mesh->GetVertex(Vertices[k]);
		// clay.cc:34-46, :87-92: toward the nearest point of the plane.
		const double Side = (Position - PlanePoint).Dot(Normal);
		NewPositionsOut[k] = Position - (Side * Strength * EvaluateWeight(*this, Stamp, Position)) * Normal;
	});
}

void FClayStripsBrushOp::BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices)
{
	LastOrigin = Stamp.LocalFrame.Origin;
}

void FClayStripsBrushOp::ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut)
{
	const USculptBrushPackClayStripsOpProps* Props = GetPropertySetAs<USculptBrushPackClayStripsOpProps>();
	const double PlaneOffset = Props->PlaneOffset;
	const double Roundness = Props->TipRoundness;
	const double TipScale = Props->TipScale;
	const bool bPlaneTrim = Props->bPlaneTrim;
	const double PlaneTrim = Props->PlaneTrim;

	const FVector3d Delta = Stamp.LocalFrame.Origin - LastOrigin;
	LastOrigin = Stamp.LocalFrame.Origin;

	// clay_strips.cc:464-472: the tip's axes. X lies across the stroke, Y along it, and Z points below the plane (above
	// it when inverted), so that the points the brush moves have a positive depth.
	const FVector3d Normal = AreaNormal(Stamp);
	FVector3d AxisX = Normal.Cross(Delta);
	if (!AxisX.Normalize() || TipScale <= 0.0)
	{
		// clay_strips.cc:322-324: without a direction of movement the tip has no orientation, and nothing moves.
		for (int32 k = 0; k < Vertices.Num(); ++k)
		{
			NewPositionsOut[k] = Mesh->GetVertex(Vertices[k]);
		}
		return;
	}
	const FVector3d AxisY = Normal.Cross(AxisX);
	const FVector3d AxisZ = Stamp.Direction < 0.0 ? Normal : -Normal;
	// clay_strips.cc:493-513: the plane through the middle of the brushed area, moved along its normal.
	const FVector3d Center = AreaCenter(Stamp) + Normal * (Stamp.Radius * PlaneOffset * Stamp.Direction);
	// clay_strips.cc:328, with the strength factor of sculpt.cc:2392-2395.
	const FVector3d Offset = Normal * (Stamp.Direction * ClayStripsStrengthFactor * StampStrength(Stamp) * Stamp.Radius);
	const double InverseRadius = 1.0 / Stamp.Radius;
	const double InverseLength = 1.0 / (Stamp.Radius * TipScale);

	ParallelFor(Vertices.Num(), [&](int32 k)
	{
		const FVector3d Position = Mesh->GetVertex(Vertices[k]);
		const FVector3d Local = Position - Center;
		const double Depth = Local.Dot(AxisZ) * InverseRadius;
		// clay_strips.cc:104-115: depth x (1 - depth), nothing outside a depth of 0 to 1.
		double Factor = FMath::Max(0.0, Depth * (1.0 - Depth));
		// clay_strips.cc:121-138.
		if (bPlaneTrim && Depth > PlaneTrim)
		{
			Factor = 0.0;
		}
		// clay_strips.cc:174-183: the square tip's distance, and the falloff read at it.
		const double Distance = TipDistance(FMath::Abs(Local.Dot(AxisX)) * InverseRadius, FMath::Abs(Local.Dot(AxisY)) * InverseLength, Roundness);
		Factor = Distance < 1.0 ? Factor * FalloffAtDistance(*this, Stamp, Distance) : 0.0;
		NewPositionsOut[k] = Position + Factor * Offset;
	});
}

void FCreaseBrushOp::ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut)
{
	const USculptBrushPackCreaseOpProps* Props = GetPropertySetAs<USculptBrushPackCreaseOpProps>();
	const FVector3d Normal = AreaNormal(Stamp);
	const FVector3d Center = Stamp.LocalFrame.Origin;
	// Crease subtracts unless inverted (blender v4.2.0 source/blender/blenkernel/intern/brush.cc:1874-1876); Blob adds.
	const double Direction = bBlob ? Stamp.Direction : -Stamp.Direction;
	// crease.cc, do_crease_or_blob_brush: the push along the normal is radius x strength.
	const FVector3d Offset = Normal * (Stamp.Radius * Direction * StampStrength(Stamp));
	// Crease pinches and Blob spreads, whichever way the push goes.
	const double Pinch = PinchStrength(Stamp, Props->Strength, Props->Pinch) * (bBlob ? -1.0 : 1.0);

	ParallelFor(Vertices.Num(), [&](int32 k)
	{
		const FVector3d Position = Mesh->GetVertex(Vertices[k]);
		NewPositionsOut[k] = Position + CreaseMove(Position, Center, Normal, Offset, Pinch, EvaluateWeight(*this, Stamp, Position));
	});
}

void FDamStandardBrushOp::BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices)
{
	Moved.Init(FVector3d::ZeroVector, Mesh->MaxVertexID());
}

void FDamStandardBrushOp::ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut)
{
	const USculptBrushPackDamStandardOpProps* Props = GetPropertySetAs<USculptBrushPackDamStandardOpProps>();
	const bool bSharpProfile = Props->bSharpProfile;
	if (Moved.Num() < Mesh->MaxVertexID())
	{
		Moved.SetNumZeroed(Mesh->MaxVertexID());
	}
	const FVector3d Normal = Stamp.LocalFrame.Z();
	const FVector3d Center = Stamp.LocalFrame.Origin;
	// It cuts in unless inverted, as Draw Sharp and Crease do (blender v4.2.0 brush.cc:1827-1828, :1874-1875).
	const FVector3d Offset = Normal * (Stamp.Radius * -Stamp.Direction * StampStrength(Stamp));
	const double Pinch = PinchStrength(Stamp, Props->Strength, Props->Pinch);

	// Each vertex is in Vertices once, so each k writes its own entry of Moved.
	ParallelFor(Vertices.Num(), [&](int32 k)
	{
		const int32 Vertex = Vertices[k];
		// Mesh is the mesh from before the stroke: draw_sharp.cc:50-61 weights each point by where it stood then.
		const FVector3d Before = Mesh->GetVertex(Vertex);
		double PointWeight;
		if (bSharpProfile)
		{
			// Blender's sharp curve, (1 - distance / radius) to the fourth power (source/blender/blenkernel/intern/brush.cc:1651-1652).
			const double Nearness = 1.0 - UE::Geometry::Distance(Before, Center) / Stamp.Radius;
			PointWeight = Nearness > 0.0 ? FMath::Square(FMath::Square(Nearness)) : 0.0;
		}
		else
		{
			PointWeight = GetFalloff().Evaluate(Stamp, Before);
		}
		if (Stamp.HasAlpha())
		{
			PointWeight *= Stamp.StampAlphaFunc(Stamp, Before);
		}
		Moved[Vertex] += CreaseMove(Before + Moved[Vertex], Center, Normal, Offset, Pinch, PointWeight);
		NewPositionsOut[k] = Before + Moved[Vertex];
	});
}

void FSnakeHookBrushOp::BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices)
{
	LastOrigin = Stamp.LocalFrame.Origin;
}

void FSnakeHookBrushOp::ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut)
{
	const float PinchSetting = GetPropertySetAs<USculptBrushPackSnakeHookOpProps>()->Pinch;
	const FVector3d Origin = Stamp.LocalFrame.Origin;
	const FVector3d Delta = Origin - LastOrigin;
	LastOrigin = Origin;
	const double DeltaSquared = Delta.SquaredLength();
	// snake_hook.cc:95-100: no pinch at 0.5; otherwise in proportion to how far the cursor moved.
	const double Pinch = PinchSetting != 0.5f && DeltaSquared > 0.0 ? 2.0 * (0.5 - PinchSetting) * FMath::Sqrt(DeltaSquared) / Stamp.Radius : 0.0;

	ParallelFor(Vertices.Num(), [&](int32 k)
	{
		const FVector3d Position = Mesh->GetVertex(Vertices[k]);
		// snake_hook.cc:194-207: the cursor's movement, by the falloff and the strength.
		const double PointWeight = GetFalloff().Evaluate(Stamp, Position) * Stamp.Power;
		FVector3d Move = Delta * PointWeight;
		if (Pinch != 0.0)
		{
			// snake_hook.cc:104-127: the point's offset from where the cursor was, without its part along the movement,
			// shrinks (or grows, for a negative pinch); less near the line, so points do not collapse onto it.
			FVector3d FromLine = Position - Origin + Delta;
			FromLine -= Delta * (FromLine.Dot(Delta) / DeltaSquared);
			double Fade = Pinch * PointWeight;
			if (Pinch > 0.0)
			{
				Fade *= FMath::Square(FMath::Min(1.0, FromLine.Length() / Stamp.Radius));
			}
			Move -= FromLine * Fade;
		}
		NewPositionsOut[k] = Position + Move;
	});
}

void FMoveTopologicalBrushOp::BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices)
{
	FirstStamp = Stamp;
	Joined.Init(false, Mesh->MaxVertexID());

	// The point under the cursor: the nearest of the brushed points.
	int32 Active = INDEX_NONE;
	double Nearest = TNumericLimits<double>::Max();
	for (const int32 Vertex : InitialVertices)
	{
		const double DistanceSquared = UE::Geometry::DistanceSquared(Mesh->GetVertex(Vertex), Stamp.LocalFrame.Origin);
		if (DistanceSquared < Nearest)
		{
			Nearest = DistanceSquared;
			Active = Vertex;
		}
	}
	if (Active == INDEX_NONE)
	{
		return;
	}

	// sculpt_automasking.cc:1117-1153 with sculpt_flood_fill.cc:81-118: from that point over the mesh's edges; a point
	// that is reached moves, and the fill goes on through it only while it lies inside the brush around the first point
	// (sculpt.cc:877-891).
	const FVector3d ActivePosition = Mesh->GetVertex(Active);
	const double RadiusSquared = Stamp.Radius * Stamp.Radius;
	TArray<int32> Queue;
	Queue.Add(Active);
	Joined[Active] = true;
	while (!Queue.IsEmpty())
	{
		const int32 From = Queue.Pop(EAllowShrinking::No);
		for (const int32 To : Mesh->VtxVerticesItr(From))
		{
			if (Joined[To])
			{
				continue;
			}
			Joined[To] = true;
			if (UE::Geometry::DistanceSquared(Mesh->GetVertex(To), ActivePosition) < RadiusSquared)
			{
				Queue.Add(To);
			}
		}
	}
}

void FMoveTopologicalBrushOp::ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut)
{
	// grab.cc, do_grab_brush: the cursor's movement since the stroke began, by the strength; Mesh is the mesh from before
	// the stroke, and each point is weighted by where it stood then (grab.cc:71-90).
	const FVector3d Move = (Stamp.LocalFrame.Origin - FirstStamp.LocalFrame.Origin) * Stamp.Power;

	ParallelFor(Vertices.Num(), [&](int32 k)
	{
		const int32 Vertex = Vertices[k];
		const FVector3d Before = Mesh->GetVertex(Vertex);
		NewPositionsOut[k] = IsJoined(Vertex) ? Before + GetFalloff().Evaluate(FirstStamp, Before) * Move : Before;
	});
}

void FLayerBrushOp::Resize(int32 MaxVertexID)
{
	if (Amount.Num() < MaxVertexID)
	{
		Amount.SetNumZeroed(MaxVertexID);
		Moved.SetNumZeroed(MaxVertexID);
		Normals.SetNumZeroed(MaxVertexID);
		bHasNormal.SetNumZeroed(MaxVertexID);
	}
}

void FLayerBrushOp::BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices)
{
	Amount.Reset();
	Moved.Reset();
	Normals.Reset();
	bHasNormal.Reset();
	Resize(Mesh->MaxVertexID());
}

void FLayerBrushOp::ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut)
{
	const double Height = GetPropertySetAs<USculptBrushPackLayerOpProps>()->Height;
	Resize(Mesh->MaxVertexID());
	// Layer's strength has no factor of its own (sculpt.cc:2357-2360); it carries the direction.
	const double Strength = Stamp.Direction * StampStrength(Stamp);

	// Mesh is the mesh from before the stroke. Each vertex is in Vertices once, so each k writes its own entries.
	ParallelFor(Vertices.Num(), [&](int32 k)
	{
		const int32 Vertex = Vertices[k];
		const FVector3d Before = Mesh->GetVertex(Vertex);
		// layer.cc:153-163: weighted by where the point stood before the stroke.
		const double PointWeight = EvaluateWeight(*this, Stamp, Before);
		// layer.cc:45-53, :74-82: the amount grows toward 1 (or -1) and is kept within them.
		double& PointAmount = Amount[Vertex];
		PointAmount = FMath::Clamp(PointAmount + PointWeight * Strength * (1.05 - FMath::Abs(PointAmount)), -1.0, 1.0);
		if (!bHasNormal[Vertex])
		{
			// A face counts by its angle at the vertex, as in Blender's vertex normals.
			Normals[Vertex] = UE::Geometry::FMeshNormals::ComputeVertexNormal(*Mesh, Vertex, /*bWeightByArea*/ false, /*bWeightByAngle*/ true);
			bHasNormal[Vertex] = true;
		}
		// layer.cc:91-105: toward the place before the stroke plus the layer.
		const FVector3d Target = Before + Normals[Vertex] * (Height * PointAmount);
		Moved[Vertex] += (Target - (Before + Moved[Vertex])) * PointWeight;
		NewPositionsOut[k] = Before + Moved[Vertex];
	});
}

void FPlaneSideBrushOp::BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices)
{
	LastOrigin = Stamp.LocalFrame.Origin;
}

void FPlaneSideBrushOp::ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut)
{
	const USculptBrushPackPlaneSideOpProps* Props = GetPropertySetAs<USculptBrushPackPlaneSideOpProps>();
	const bool bMoved = !(Stamp.LocalFrame.Origin - LastOrigin).IsZero();
	LastOrigin = Stamp.LocalFrame.Origin;
	if (!bMoved)
	{
		// plane.cc:378-380: nothing without movement.
		for (int32 k = 0; k < Vertices.Num(); ++k)
		{
			NewPositionsOut[k] = Mesh->GetVertex(Vertices[k]);
		}
		return;
	}

	// Inverted, the two reaches swap, so Fill scrapes and Scrape fills (plane.cc:408-418; the mode Blender 4.2 gave both
	// brushes, blender v4.2.0 source/blender/blenkernel/intern/brush.cc:1884), and the plane's offset turns over (:387-390).
	const bool bInverted = Stamp.Direction < 0.0;
	const double Height = bInverted ? Props->Depth : Props->Height;
	const double Depth = bInverted ? Props->Height : Props->Depth;
	const FVector3d Normal = AreaNormal(Stamp);
	const FVector3d Center = AreaCenter(Stamp) + Normal * (Stamp.Radius * Props->PlaneOffset * (bInverted ? -1.0 : 1.0));
	// The Plane brush's strength has no factor of its own (sculpt.cc:2425-2429).
	const double Strength = StampStrength(Stamp);
	const double InverseRadius = 1.0 / Stamp.Radius;

	ParallelFor(Vertices.Num(), [&](int32 k)
	{
		const FVector3d Position = Mesh->GetVertex(Vertices[k]);
		const FVector3d Local = Position - Center;
		const double Along = Local.Dot(Normal);
		const double Above = Along * InverseRadius;
		// plane.cc:80-121: the distance counts the reach on the point's side as one radius; no reach, no effect.
		const double Reach = Above >= 0.0 ? Height : Depth;
		double Factor = 0.0;
		if (Reach != 0.0)
		{
			const FVector3d Across = (Local - Normal * Along) * InverseRadius;
			const double Distance = FMath::Sqrt(Across.SquaredLength() + FMath::Square(Above / Reach));
			if (Distance < 1.0)
			{
				// plane.cc:230-237 and :133-154.
				Factor = FalloffAtDistance(*this, Stamp, Distance);
				if (Reach != 1.0)
				{
					Factor *= Reach;
				}
			}
		}
		// plane.cc:178-192: toward the plane by the point's distance from it.
		NewPositionsOut[k] = Position - Normal * (Factor * Above * Stamp.Radius * Strength);
	});
}

void FNudgeBrushOp::BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices)
{
	LastOrigin = Stamp.LocalFrame.Origin;
}

void FNudgeBrushOp::ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut)
{
	const FVector3d Delta = Stamp.LocalFrame.Origin - LastOrigin;
	LastOrigin = Stamp.LocalFrame.Origin;
	// draw.cc:210-214: the cursor's movement without its part along the area's normal, by the strength. Blender's
	// strength here is its setting squared (sculpt.cc:2448-2450); the engine's is the setting.
	const FVector3d Normal = AreaNormal(Stamp);
	const FVector3d Move = (Delta - Normal * Delta.Dot(Normal)) * Stamp.Power;

	ParallelFor(Vertices.Num(), [&](int32 k)
	{
		const FVector3d Position = Mesh->GetVertex(Vertices[k]);
		NewPositionsOut[k] = Position + Move * GetFalloff().Evaluate(Stamp, Position);
	});
}

void FThumbBrushOp::BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices)
{
	FirstStamp = Stamp;
	bHasNormal = false;
}

void FThumbBrushOp::ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut)
{
	// The area's normal as the stroke began: the first stamp's, taken before anything has moved.
	if (!bHasNormal)
	{
		Normal = AreaNormal(Stamp);
		bHasNormal = true;
	}
	// thumb.cc:144-146: the cursor's movement since the stroke began, without its part along that normal.
	const FVector3d Delta = Stamp.LocalFrame.Origin - FirstStamp.LocalFrame.Origin;
	const FVector3d Move = (Delta - Normal * Delta.Dot(Normal)) * Stamp.Power;

	// Mesh is the mesh from before the stroke; each point is weighted by where it stood then (thumb.cc:51-66).
	ParallelFor(Vertices.Num(), [&](int32 k)
	{
		const FVector3d Before = Mesh->GetVertex(Vertices[k]);
		NewPositionsOut[k] = Before + Move * GetFalloff().Evaluate(FirstStamp, Before);
	});
}

void FSurfaceMoveBrushOp::BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices)
{
	Moved.Init(FVector3d::ZeroVector, Mesh->MaxVertexID());
	LastOrigin = Stamp.LocalFrame.Origin;
}

void FTopologySlideBrushOp::ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut)
{
	if (Moved.Num() < Mesh->MaxVertexID())
	{
		Moved.SetNumZeroed(Mesh->MaxVertexID());
	}
	// topology_slide.cc:49-51: the direction the cursor moved in since the last stamp.
	FVector3d Direction = Stamp.LocalFrame.Origin - LastOrigin;
	LastOrigin = Stamp.LocalFrame.Origin;
	const bool bHasDirection = Direction.Normalize();
	// sculpt.cc:2383-2384: twice the strength.
	const double Strength = 2.0 * StampStrength(Stamp);

	// Every point's move is worked out from where the points are now, then all are applied.
	TArray<FVector3d> Moves;
	Moves.SetNumZeroed(Vertices.Num());
	if (bHasDirection)
	{
		ParallelFor(Vertices.Num(), [&](int32 k)
		{
			const int32 Vertex = Vertices[k];
			const FVector3d Here = Current(*Mesh, Vertex);
			// topology_slide.cc:65-93: toward each neighbor that lies ahead, by the edge's length in the direction.
			FVector3d Sum = FVector3d::ZeroVector;
			for (const int32 Neighbor : Mesh->VtxVerticesItr(Vertex))
			{
				const FVector3d Offset = Current(*Mesh, Neighbor) - Here;
				FVector3d Toward = Offset;
				if (Toward.Normalize() && Direction.Dot(Toward) > 0.0)
				{
					Sum += Toward * Direction.Dot(Offset);
				}
			}
			// topology_slide.cc:176-186, :200: weighted by where the point stood before the stroke.
			Moves[k] = Sum * (EvaluateWeight(*this, Stamp, Mesh->GetVertex(Vertex)) * Strength);
		});
	}
	for (int32 k = 0; k < Vertices.Num(); ++k)
	{
		Moved[Vertices[k]] += Moves[k];
		NewPositionsOut[k] = Current(*Mesh, Vertices[k]);
	}
}

void FRelaxBrushOp::BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices)
{
	FSurfaceMoveBrushOp::BeginStroke(Mesh, Stamp, InitialVertices);
	SlotOfVertex.Init(INDEX_NONE, Mesh->MaxVertexID());
}

void FRelaxBrushOp::ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut)
{
	const int32 MaxVertexID = Mesh->MaxVertexID();
	if (Moved.Num() < MaxVertexID)
	{
		Moved.SetNumZeroed(MaxVertexID);
	}
	if (SlotOfVertex.Num() < MaxVertexID)
	{
		SlotOfVertex.Init(INDEX_NONE, MaxVertexID);
	}
	// relax.cc:849 with sculpt.cc:2383-2384: twice the strength.
	const double Strength = 2.0 * StampStrength(Stamp);

	const int32 Num = Vertices.Num();
	TArray<FVector3d> Positions, Next, Normals;
	TArray<double> Factors;
	Positions.SetNumUninitialized(Num);
	Next.SetNumUninitialized(Num);
	Normals.SetNumUninitialized(Num);
	Factors.SetNumUninitialized(Num);
	for (int32 k = 0; k < Num; ++k)
	{
		SlotOfVertex[Vertices[k]] = k;
		Positions[k] = Current(*Mesh, Vertices[k]);
	}
	// Where a vertex is now: in this stamp's list, or as the stroke left it.
	auto Place = [&](int32 Vertex)
	{
		const int32 Slot = SlotOfVertex[Vertex];
		return Slot != INDEX_NONE ? Positions[Slot] : Current(*Mesh, Vertex);
	};

	ParallelFor(Num, [&](int32 k)
	{
		const int32 Vertex = Vertices[k];
		// relax.cc:481-500: weighted by where the point stood before the stroke.
		Factors[k] = EvaluateWeight(*this, Stamp, Mesh->GetVertex(Vertex)) * Strength;
		// The point's normal as the mesh stands now, each face counting by its area (Blender's counts by angle).
		FVector3d Normal = FVector3d::ZeroVector;
		for (const int32 Triangle : Mesh->VtxTrianglesItr(Vertex))
		{
			const UE::Geometry::FIndex3i Corners = Mesh->GetTriangle(Triangle);
			const FVector3d A = Place(Corners.A);
			Normal += (Place(Corners.C) - A).Cross(Place(Corners.B) - A);
		}
		Normal.Normalize();
		Normals[k] = Normal;
	});

	// relax.cc:853: four passes per stamp. Each pass reads the last one's positions.
	for (int32 Pass = 0; Pass < 4; ++Pass)
	{
		ParallelFor(Num, [&](int32 k)
		{
			const int32 Vertex = Vertices[k];
			const FVector3d Here = Positions[k];
			Next[k] = Here;
			// sculpt_smooth.cc:532-543: a point with two neighbors or fewer is a corner and stays.
			if (Factors[k] == 0.0 || Mesh->GetVtxEdgeCount(Vertex) <= 2)
			{
				return;
			}
			// sculpt_smooth.cc:545-549: a point on an open border follows its border neighbors only.
			const bool bBorder = Mesh->IsBoundaryVertex(Vertex);
			FVector3d Sum = FVector3d::ZeroVector;
			FVector3d Bisector = FVector3d::ZeroVector;
			int32 Count = 0;
			for (const int32 Edge : Mesh->VtxEdgesItr(Vertex))
			{
				if (bBorder && !Mesh->IsBoundaryEdge(Edge))
				{
					continue;
				}
				const UE::Geometry::FIndex2i Ends = Mesh->GetEdgeV(Edge);
				const FVector3d There = Place(Ends.A == Vertex ? Ends.B : Ends.A);
				Sum += There;
				++Count;
				FVector3d Toward = There - Here;
				Toward.Normalize();
				Bisector += Toward;
			}
			if (Count == 0)
			{
				return;
			}
			// sculpt_smooth.cc:565-575, :471-482: along a border the plane's normal is the bisector of the two border edges.
			FVector3d PlaneNormal = Normals[k];
			if (bBorder && Count == 2)
			{
				PlaneNormal = Bisector;
				if (!PlaneNormal.Normalize())
				{
					return;
				}
			}
			// sculpt_smooth.cc:562, :458-469, :577-580: toward the neighbors' average, within the point's plane.
			const FVector3d Offset = Sum / static_cast<double>(Count) - Here;
			Next[k] = Here + (Offset - PlaneNormal * Offset.Dot(PlaneNormal)) * Factors[k];
		});
		Swap(Positions, Next);
	}

	for (int32 k = 0; k < Num; ++k)
	{
		const int32 Vertex = Vertices[k];
		Moved[Vertex] = Positions[k] - Mesh->GetVertex(Vertex);
		NewPositionsOut[k] = Positions[k];
		SlotOfVertex[Vertex] = INDEX_NONE;
	}
}

void FGlideBrushOp::BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices)
{
	LastOrigin = Stamp.LocalFrame.Origin;
	Weights.Init(0.0, Mesh->MaxVertexID());
	Surface = MakeUnique<FDynamicMesh3>();
	SurfaceTree.Reset();

	// The point under the cursor: the nearest of the brushed points.
	int32 Clicked = INDEX_NONE;
	double Nearest = TNumericLimits<double>::Max();
	for (const int32 Vertex : InitialVertices)
	{
		const double DistanceSquared = UE::Geometry::DistanceSquared(Mesh->GetVertex(Vertex), Stamp.LocalFrame.Origin);
		if (DistanceSquared < Nearest)
		{
			Nearest = DistanceSquared;
			Clicked = Vertex;
		}
	}
	if (Clicked == INDEX_NONE)
	{
		return;
	}

	// The surface joined to that point, out to four radii from the cursor (the engine's own brushes look for the surface
	// that far, MeshSculptBrushOps.h:148 in 5.8.4). Mesh is still the mesh from before the stroke here, and a copy of that
	// piece is kept, as the mesh itself moves under the stroke.
	const double KeepSquared = FMath::Square(4.0 * Stamp.Radius);
	TArray<int32> LocalOf;
	LocalOf.Init(INDEX_NONE, Mesh->MaxVertexID());
	TArray<int32> Kept;
	Kept.Add(Clicked);
	LocalOf[Clicked] = Surface->AppendVertex(Mesh->GetVertex(Clicked));
	for (int32 Next = 0; Next < Kept.Num(); ++Next)
	{
		const int32 From = Kept[Next];
		// The first points past four radii are kept, so that the triangles reaching them are; the walk stops at them.
		if (UE::Geometry::DistanceSquared(Mesh->GetVertex(From), Stamp.LocalFrame.Origin) >= KeepSquared)
		{
			continue;
		}
		for (const int32 To : Mesh->VtxVerticesItr(From))
		{
			if (LocalOf[To] == INDEX_NONE)
			{
				LocalOf[To] = Surface->AppendVertex(Mesh->GetVertex(To));
				Kept.Add(To);
			}
		}
	}
	// Each triangle whose three corners are kept, once: at its corner with the lowest number.
	for (const int32 Vertex : Kept)
	{
		for (const int32 Triangle : Mesh->VtxTrianglesItr(Vertex))
		{
			const UE::Geometry::FIndex3i Corners = Mesh->GetTriangle(Triangle);
			if (Vertex == FMath::Min3(Corners.A, Corners.B, Corners.C)
				&& LocalOf[Corners.A] != INDEX_NONE && LocalOf[Corners.B] != INDEX_NONE && LocalOf[Corners.C] != INDEX_NONE)
			{
				Surface->AppendTriangle(LocalOf[Corners.A], LocalOf[Corners.B], LocalOf[Corners.C]);
			}
		}
	}
	if (Surface->TriangleCount() == 0)
	{
		return;
	}
	SurfaceTree = MakeUnique<UE::Geometry::FDynamicMeshAABBTree3>(Surface.Get());

	// Each brushed point of that piece is weighted by where it stands now, at the stroke's start, as the engine's Move
	// weights its points (MeshMoveBrushOps.h:46-76 in 5.8.4).
	for (const int32 Vertex : InitialVertices)
	{
		if (LocalOf[Vertex] != INDEX_NONE)
		{
			Weights[Vertex] = GetFalloff().Evaluate(Stamp, Mesh->GetVertex(Vertex));
		}
	}
}

void FGlideBrushOp::ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut)
{
	const FVector3d Move = (Stamp.LocalFrame.Origin - LastOrigin) * Stamp.Power;
	LastOrigin = Stamp.LocalFrame.Origin;
	const UE::Geometry::FDynamicMeshAABBTree3* Tree = SurfaceTree.Get();

	ParallelFor(Vertices.Num(), [&](int32 k)
	{
		const int32 Vertex = Vertices[k];
		const FVector3d Position = Mesh->GetVertex(Vertex);
		const double PointWeight = Weights.IsValidIndex(Vertex) ? Weights[Vertex] : 0.0;
		// The point follows the cursor, then is laid back on the surface from before the stroke.
		NewPositionsOut[k] = Tree && PointWeight > 0.0 ? Tree->FindNearestPoint(Position + Move * PointWeight) : Position;
	});
}

void FStrokeReachBrushOp::Resize(int32 MaxVertexID)
{
	if (Reach.Num() < MaxVertexID)
	{
		Reach.SetNumZeroed(MaxVertexID);
		Normals.SetNumZeroed(MaxVertexID);
		bHasNormal.SetNumZeroed(MaxVertexID);
	}
}

void FStrokeReachBrushOp::BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices)
{
	Reach.Reset();
	Normals.Reset();
	bHasNormal.Reset();
	Resize(Mesh->MaxVertexID());
}

void FStrokeReachBrushOp::ApplyStamp(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices, TArray<FVector3d>& NewPositionsOut)
{
	Resize(Mesh->MaxVertexID());
	PrepareStamp(*Mesh, Vertices);

	// Mesh is the mesh from before the stroke. Each vertex is in Vertices once, so each k writes its own entries.
	ParallelFor(Vertices.Num(), [&](int32 k)
	{
		const int32 Vertex = Vertices[k];
		const FVector3d Before = Mesh->GetVertex(Vertex);
		Reach[Vertex] = FMath::Max(Reach[Vertex], StampReach(Stamp, Before));
		if (!bHasNormal[Vertex])
		{
			Normals[Vertex] = UE::Geometry::FMeshNormals::ComputeVertexNormal(*Mesh, Vertex, /*bWeightByArea*/ false, /*bWeightByAngle*/ true);
			bHasNormal[Vertex] = true;
		}
		const FVector3d& Normal = Normals[Vertex];
		NewPositionsOut[k] = Before + Normal * (Reach[Vertex] * FullHeight(Stamp, Vertex, Before, Normal));
	});
}

void FTubeBrushOp::PrepareStamp(const FDynamicMesh3& Mesh, const TArray<int32>& Vertices)
{
	Height = GetPropertySetAs<USculptBrushPackTubeOpProps>()->Height;
}

double FTubeBrushOp::StampReach(const FSculptBrushStamp& Stamp, const FVector3d& Before) const
{
	// A half circle across the brush: the bead's height at this distance from the stamp's middle.
	const double Unit = UE::Geometry::Distance(Before, Stamp.LocalFrame.Origin) / Stamp.Radius;
	return Stamp.Radius * Height * Stamp.Power * FMath::Sqrt(FMath::Max(0.0, 1.0 - Unit * Unit));
}

double FTubeBrushOp::FullHeight(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Normal) const
{
	// The reach is the height itself; the stroke's direction says which way.
	return Stamp.Direction;
}

void FNoiseBrushOp::PrepareStamp(const FDynamicMesh3& Mesh, const TArray<int32>& Vertices)
{
	const USculptBrushPackNoiseOpProps* Props = GetPropertySetAs<USculptBrushPackNoiseOpProps>();
	Height = Props->Height;
	Scale = FMath::Max(static_cast<double>(Props->Scale), UE_DOUBLE_SMALL_NUMBER);
}

double FNoiseBrushOp::StampReach(const FSculptBrushStamp& Stamp, const FVector3d& Before) const
{
	return EvaluateWeight(*this, Stamp, Before) * Stamp.Power;
}

double FNoiseBrushOp::FullHeight(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Normal) const
{
	return Stamp.Direction * Height * static_cast<double>(FMath::PerlinNoise3D(FVector(Before / Scale)));
}

void FSharpenBrushOp::ResizeRelief(int32 MaxVertexID)
{
	if (bHasRelief.Num() < MaxVertexID)
	{
		for (FRound& Round : Rounds)
		{
			// A place is written before it is read, so only the marks are cleared.
			Round.Positions.SetNumUninitialized(MaxVertexID);
			Round.bKnown.SetNumZeroed(MaxVertexID);
		}
		Relief.SetNumUninitialized(MaxVertexID);
		bHasRelief.SetNumZeroed(MaxVertexID);
	}
}

void FSharpenBrushOp::BeginStroke(const FDynamicMesh3* Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& InitialVertices)
{
	FStrokeReachBrushOp::BeginStroke(Mesh, Stamp, InitialVertices);

	// The detail size holds for the whole stroke.
	Rounds.Reset();
	Rounds.SetNum(FMath::Clamp(GetPropertySetAs<USculptBrushPackSharpenOpProps>()->DetailSize, 1, MaxSharpenRounds));
	Relief.Reset();
	bHasRelief.Reset();
	ResizeRelief(Mesh->MaxVertexID());
}

FVector3d FSharpenBrushOp::Averaged(const FDynamicMesh3& Mesh, int32 Vertex, int32 RoundCount)
{
	if (RoundCount == 0)
	{
		return Mesh.GetVertex(Vertex);
	}
	FRound& Round = Rounds[RoundCount - 1];
	if (!Round.bKnown[Vertex])
	{
		// The point and its neighbors, each counted once, as the round before left them.
		FVector3d Sum = Averaged(Mesh, Vertex, RoundCount - 1);
		int32 Count = 1;
		for (const int32 Neighbor : Mesh.VtxVerticesItr(Vertex))
		{
			Sum += Averaged(Mesh, Neighbor, RoundCount - 1);
			++Count;
		}
		Round.Positions[Vertex] = Sum / static_cast<double>(Count);
		Round.bKnown[Vertex] = true;
	}
	return Round.Positions[Vertex];
}

void FSharpenBrushOp::PrepareStamp(const FDynamicMesh3& Mesh, const TArray<int32>& Vertices)
{
	Amount = GetPropertySetAs<USculptBrushPackSharpenOpProps>()->Amount;
	if (Rounds.IsEmpty())
	{
		Rounds.SetNum(FMath::Clamp(GetPropertySetAs<USculptBrushPackSharpenOpProps>()->DetailSize, 1, MaxSharpenRounds));
	}
	ResizeRelief(Mesh.MaxVertexID());

	// One point after another: a point's averaged place is built from its neighbors', which other points share.
	for (const int32 Vertex : Vertices)
	{
		if (!bHasRelief[Vertex])
		{
			Relief[Vertex] = Mesh.GetVertex(Vertex) - Averaged(Mesh, Vertex, Rounds.Num());
			bHasRelief[Vertex] = true;
		}
	}
}

double FSharpenBrushOp::StampReach(const FSculptBrushStamp& Stamp, const FVector3d& Before) const
{
	return EvaluateWeight(*this, Stamp, Before) * Stamp.Power;
}

double FSharpenBrushOp::FullHeight(const FSculptBrushStamp& Stamp, int32 Vertex, const FVector3d& Before, const FVector3d& Normal) const
{
	return Stamp.Direction * Amount * Relief[Vertex].Dot(Normal);
}
}

USculptBrushPackSnakeHookOpProps::USculptBrushPackSnakeHookOpProps()
{
	// The engine's Move brush's own defaults (MeshMoveBrushOps.h:22 and :26 in 5.8.4).
	Strength = 1.0f;
	Falloff = 0.5f;
}

USculptBrushPackMoveTopologicalOpProps::USculptBrushPackMoveTopologicalOpProps()
{
	Strength = 1.0f;
	Falloff = 0.5f;
}

USculptBrushPackNudgeOpProps::USculptBrushPackNudgeOpProps()
{
	Strength = 1.0f;
	Falloff = 0.5f;
}

USculptBrushPackThumbOpProps::USculptBrushPackThumbOpProps()
{
	Strength = 1.0f;
	Falloff = 0.5f;
}

USculptBrushPackFillOpProps::USculptBrushPackFillOpProps()
{
	// Fill moves only what lies below the plane (blender v4.2.0 source/blender/editors/sculpt_paint/sculpt_brush_types.cc:247-334).
	Height = 0.0f;
	Depth = 1.0f;
}

USculptBrushPackScrapeOpProps::USculptBrushPackScrapeOpProps()
{
	// Scrape moves only what lies above the plane (the same file, :336-422).
	Height = 1.0f;
	Depth = 0.0f;
}

USculptBrushPackGlideOpProps::USculptBrushPackGlideOpProps()
{
	// The engine's Move brush's own defaults (MeshMoveBrushOps.h:22 and :26 in 5.8.4).
	Strength = 1.0f;
	Falloff = 0.5f;
}

USculptBrushPackNoiseOpProps::USculptBrushPackNoiseOpProps()
{
	// At full strength the middle of the brush shows the whole pattern.
	Strength = 1.0f;
}

USculptBrushPackSharpenOpProps::USculptBrushPackSharpenOpProps()
{
	// At full strength the middle of the brush gets the whole Amount.
	Strength = 1.0f;
}
