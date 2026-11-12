// Copyright (C) 2026 Joseph Wiginton.
// SPDX-License-Identifier: GPL-3.0-or-later
// Rules and constants of fourteen brushes follow Blender's sculpt source: Copyright Blender Authors, GPL-2.0-or-later.
//
// Sculpt Brush Pack. Tests of the brushes' rules and of the hook into the engine's Vertex Sculpt tools.
//
// The rule tests hand each brush a few stamps on a few points and compare with values worked out by hand from the rule as
// the brush's class comment states it. Comparisons are exact: the stamps use powers of two (a strength of 1, a time step
// of 1/8, a radius of 8, so the stamp's strength is 0.75) and a falloff of 1 everywhere, and where a rule has a factor
// that is not a power of two the expected value is the same product written out in the same order. That holds while the
// compiler rounds each operation as written, which the module's build file asks for (FPSemantics). No tolerance is used.

#include "CoreMinimal.h"
#include "Components/DynamicMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/MeshNormals.h"
#include "DynamicMeshActor.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Commands/UICommandList.h"
#include "Generators/MinimalBoxMeshGenerator.h"
#include "InteractiveToolManager.h"
#include "MeshVertexSculptTool.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "SculptBrushPack.h"
#include "SculptBrushPackBrushes.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Tools/UEdMode.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"

#include <initializer_list>

#if WITH_DEV_AUTOMATION_TESTS

namespace SculptBrushPackTestLocals
{
	using namespace SculptBrushPack;
	using UE::Geometry::FDynamicMesh3;
	using UE::Geometry::FFrame3d;
	using UE::Geometry::FIndex3i;

	/** A stamp at Origin with +Z as its normal and as the brushed area's normal: radius 8, strength 0.75 per stamp. */
	static FSculptBrushStamp MakeStamp(const FVector3d& Origin)
	{
		FSculptBrushStamp Stamp;
		Stamp.LocalFrame = FFrame3d(Origin);
		Stamp.WorldFrame = Stamp.LocalFrame;
		Stamp.PrevLocalFrame = Stamp.LocalFrame;
		Stamp.PrevWorldFrame = Stamp.LocalFrame;
		Stamp.RegionPlane = FFrame3d(Origin);
		Stamp.Radius = 8.0;
		Stamp.Falloff = 1.0;
		Stamp.Power = 1.0;
		Stamp.Direction = 1.0;
		Stamp.Depth = 0.0;
		Stamp.DeltaTime = 0.125;
		return Stamp;
	}

	/** Gives the brush its settings and a falloff of 1 everywhere. */
	template<typename OpType, typename SettingsType>
	static SettingsType* Prepare(OpType& Op)
	{
		SettingsType* Settings = NewObject<SettingsType>(GetTransientPackage());
		Op.PropertySet = Settings;
		Op.Falloff = MakeShared<FMeshSculptFallofFunc>();
		Op.Falloff->FalloffFunc = [](const FSculptBrushStamp&, const FVector3d&) { return 1.0; };
		return Settings;
	}

	/** One stamp on every vertex of the mesh; returns the new positions by vertex ID. */
	static TArray<FVector3d> Apply(FMeshSculptBrushOp& Op, const FDynamicMesh3& Mesh, const FSculptBrushStamp& Stamp)
	{
		TArray<int32> Vertices;
		for (const int32 Vertex : Mesh.VertexIndicesItr())
		{
			Vertices.Add(Vertex);
		}
		TArray<FVector3d> NewPositions;
		NewPositions.SetNum(Vertices.Num());
		Op.ApplyStamp(&Mesh, Stamp, Vertices, NewPositions);
		TArray<FVector3d> ByVertex;
		ByVertex.SetNum(Mesh.MaxVertexID());
		for (int32 Index = 0; Index < Vertices.Num(); ++Index)
		{
			ByVertex[Vertices[Index]] = NewPositions[Index];
		}
		return ByVertex;
	}

	static FDynamicMesh3 MakePoints(std::initializer_list<FVector3d> Points)
	{
		FDynamicMesh3 Mesh;
		for (const FVector3d& Point : Points)
		{
			Mesh.AppendVertex(Point);
		}
		return Mesh;
	}

	/** One stamp on the given vertices only; returns their new positions in the same order. */
	static TArray<FVector3d> ApplyTo(FMeshSculptBrushOp& Op, const FDynamicMesh3& Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices)
	{
		TArray<FVector3d> NewPositions;
		NewPositions.SetNum(Vertices.Num());
		Op.ApplyStamp(&Mesh, Stamp, Vertices, NewPositions);
		return NewPositions;
	}

	/**
	 * A fan of four triangles: vertex 0 at Center, vertices 1 to 4 at (4,0,0), (0,4,0), (-4,0,0) and (0,-4,0). Its faces
	 * face +Z when the center lies in their plane (the engine's triangle normal is (V2 - V0) x (V1 - V0)).
	 */
	static FDynamicMesh3 MakeFan(const FVector3d& Center)
	{
		FDynamicMesh3 Mesh;
		Mesh.AppendVertex(Center);
		Mesh.AppendVertex(FVector3d(4, 0, 0));
		Mesh.AppendVertex(FVector3d(0, 4, 0));
		Mesh.AppendVertex(FVector3d(-4, 0, 0));
		Mesh.AppendVertex(FVector3d(0, -4, 0));
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			Mesh.AppendTriangle(0, 1 + (Corner + 1) % 4, 1 + Corner);
		}
		return Mesh;
	}

	/** A fan around vertex 0 at Center; vertices 1 and up are Rim, given anticlockwise seen from +Z. */
	static FDynamicMesh3 MakeFanOf(const FVector3d& Center, const TArray<FVector3d>& Rim)
	{
		FDynamicMesh3 Mesh;
		Mesh.AppendVertex(Center);
		for (const FVector3d& Point : Rim)
		{
			Mesh.AppendVertex(Point);
		}
		for (int32 Corner = 0; Corner < Rim.Num(); ++Corner)
		{
			Mesh.AppendTriangle(0, 1 + (Corner + 1) % Rim.Num(), 1 + Corner);
		}
		return Mesh;
	}

	/** The vertex of a sheet (MakeSheet) in the given column along X and row along Y, each 0 to 4. */
	static int32 SheetVertex(int32 Column, int32 Row)
	{
		return Column * 5 + Row;
	}

	/**
	 * A sheet of 5 x 5 points two apart, from -4 to 4 in X and Y, at the height HeightAtX gives for their X: 32 triangles,
	 * each square cut from its low corner to its high one.
	 */
	static FDynamicMesh3 MakeSheet(TFunctionRef<double(double)> HeightAtX)
	{
		FDynamicMesh3 Mesh;
		for (int32 Column = 0; Column < 5; ++Column)
		{
			for (int32 Row = 0; Row < 5; ++Row)
			{
				const double X = -4.0 + 2.0 * Column;
				Mesh.AppendVertex(FVector3d(X, -4.0 + 2.0 * Row, HeightAtX(X)));
			}
		}
		for (int32 Column = 0; Column < 4; ++Column)
		{
			for (int32 Row = 0; Row < 4; ++Row)
			{
				Mesh.AppendTriangle(SheetVertex(Column, Row), SheetVertex(Column + 1, Row + 1), SheetVertex(Column + 1, Row));
				Mesh.AppendTriangle(SheetVertex(Column, Row), SheetVertex(Column, Row + 1), SheetVertex(Column + 1, Row + 1));
			}
		}
		return Mesh;
	}

	static TArray<int32> AllVertices(const FDynamicMesh3& Mesh)
	{
		TArray<int32> Vertices;
		for (const int32 Vertex : Mesh.VertexIndicesItr())
		{
			Vertices.Add(Vertex);
		}
		return Vertices;
	}

	/** One stamp on the given vertices, written into the mesh, as the tool does for a brush that reads the mesh as it is. */
	static void ApplyAndStore(FMeshSculptBrushOp& Op, FDynamicMesh3& Mesh, const FSculptBrushStamp& Stamp, const TArray<int32>& Vertices)
	{
		const TArray<FVector3d> NewPositions = ApplyTo(Op, Mesh, Stamp, Vertices);
		for (int32 Index = 0; Index < Vertices.Num(); ++Index)
		{
			Mesh.SetVertex(Vertices[Index], NewPositions[Index]);
		}
	}

	/** The normal a brush of the pack takes for a point: the engine's, weighted by angle. */
	static FVector3d NormalOf(const FDynamicMesh3& Mesh, int32 Vertex)
	{
		return UE::Geometry::FMeshNormals::ComputeVertexNormal(Mesh, Vertex, /*bWeightByArea*/ false, /*bWeightByAngle*/ true);
	}

	/**
	 * What a sculpt tool that the pack has reached must show, in whichever editor it runs: every brush of the pack after
	 * the engine's own, each able to be the active brush with only its own settings showing. Leaves an engine brush active.
	 */
	static void CheckPackBrushes(FAutomationTestBase* Test, UMeshVertexSculptTool* Tool)
	{
		Test->TestTrue(TEXT("The pack knows the tool"), IsSupportedTool(Tool));

		// Every brush of the pack is in the tool's palette, after the engine's own, in the pack's order.
		TArray<int32> Palette;
		for (const UMeshSculptToolBase::FBrushTypeInfo& Type : Tool->GetAvailableBrushTypes())
		{
			Palette.Add(Type.Identifier);
		}
		const TArray<int32> Pack = GetBrushIdentifiers();
		const int32 EngineBrushes = Palette.Num() - Pack.Num();
		// Eighteen in 5.8.4, and Erase Sculpt Layer when the target has sculpt layers (MeshVertexSculptTool.cpp:380-460).
		Test->TestTrue(TEXT("The engine's brushes are still there"), EngineBrushes >= 18);
		Test->AddInfo(FString::Printf(TEXT("%s: %d engine brushes, then %d of the pack."), *Tool->GetClass()->GetName(), EngineBrushes, Pack.Num()));
		bool bInOrder = Palette.Num() >= Pack.Num();
		for (int32 Index = 0; bInOrder && Index < Pack.Num(); ++Index)
		{
			bInOrder = Palette[EngineBrushes + Index] == Pack[Index];
		}
		Test->TestTrue(TEXT("The pack's brushes follow the engine's, in the pack's order"), bInOrder);
		bool bEngineFirst = true;
		for (int32 Index = 0; Index < EngineBrushes && Index < Palette.Num(); ++Index)
		{
			bEngineFirst &= !Pack.Contains(Palette[Index]);
		}
		Test->TestTrue(TEXT("None of the engine's brushes was displaced"), bEngineFirst);

		// Each pack brush can be made the active one.
		for (const int32 Identifier : Pack)
		{
			Tool->SetActiveBrushType(Identifier);
			// Its settings, and no other brush's of this plugin, are the ones the panel shows.
			int32 OwnSettings = 0;
			for (UObject* Object : Tool->GetToolProperties(/*bEnabledOnly*/ true))
			{
				OwnSettings += Object && Object->IsA<UMeshSculptBrushOpProps>() && Object->GetClass()->GetOutermost()->GetFName() == FName(TEXT("/Script/SculptBrushPack")) ? 1 : 0;
			}
			Test->TestEqual(FString::Printf(TEXT("With brush %d active, one settings object of the pack's shows"), Identifier), OwnSettings, 1);
		}
		// Back to an engine brush, so the next sculpt session in this editor run starts as usual.
		Tool->SetActiveBrushType(static_cast<int32>(EMeshVertexSculptBrushType::Move));
	}

	static bool TestPoint(FAutomationTestBase* Test, const FString& What, const FVector3d& Actual, const FVector3d& Expected)
	{
		return Test->TestTrue(FString::Printf(TEXT("%s: (%.17g, %.17g, %.17g), expected (%.17g, %.17g, %.17g)"), *What,
			Actual.X, Actual.Y, Actual.Z, Expected.X, Expected.Y, Expected.Z), Actual == Expected);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackClayTest, "SculptBrushPack.Brushes.Clay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackClayTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackTestLocals;

	TestEqual(TEXT("A stamp's strength: power 1 x time step 1/8 x the engine's 6"), StampStrength(MakeStamp(FVector3d::Zero())), 0.75, 0.0);

	// Clay: the plane lies Plane Offset radii above the cursor's point (0.25 x 8 = 2); each point moves toward it by
	// 0.25 x 0.75 = 0.1875 of its distance.
	FClayBrushOp Clay;
	USculptBrushPackClayOpProps* Settings = Prepare<FClayBrushOp, USculptBrushPackClayOpProps>(Clay);
	TestEqual(TEXT("Clay's Plane Offset is Blender 4.2's 0.25"), Settings->PlaneOffset, 0.25f, 0.0f);
	const FDynamicMesh3 Mesh = MakePoints({ FVector3d(0, 0, 0), FVector3d(3, 0, 4), FVector3d(-1, 2, 2) });
	FSculptBrushStamp Stamp = MakeStamp(FVector3d::Zero());
	TArray<FVector3d> Moved = Apply(Clay, Mesh, Stamp);
	TestPoint(this, TEXT("A point on the surface rises toward the plane"), Moved[0], FVector3d(0, 0, 0.375));
	TestPoint(this, TEXT("A point above the plane comes down toward it"), Moved[1], FVector3d(3, 0, 3.625));
	TestPoint(this, TEXT("A point on the plane stays"), Moved[2], FVector3d(-1, 2, 2));

	// Inverted, the plane lies below the surface.
	Stamp.Direction = -1.0;
	Moved = Apply(Clay, Mesh, Stamp);
	TestPoint(this, TEXT("Inverted, a point on the surface sinks"), Moved[0], FVector3d(0, 0, -0.375));

	// Without an offset the brush only levels: a point on the cursor's plane stays, a bump comes down.
	Stamp.Direction = 1.0;
	Settings->PlaneOffset = 0.0f;
	Moved = Apply(Clay, Mesh, Stamp);
	TestPoint(this, TEXT("Without an offset a point on the surface stays"), Moved[0], FVector3d(0, 0, 0));
	TestPoint(this, TEXT("and a bump comes down"), Moved[1], FVector3d(3, 0, 3.25));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackClayStripsTest, "SculptBrushPack.Brushes.ClayStrips",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackClayStripsTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackTestLocals;

	// The square tip's distance: 0 inside, rising to 1 at the rim, 1 outside.
	TestEqual(TEXT("Inside the square"), TipDistance(0.25, 0.25, 0.5), 0.0, 0.0);
	TestEqual(TEXT("On a side, half way to the rim"), TipDistance(0.75, 0.0, 0.5), 0.5, 0.0);
	TestEqual(TEXT("In a corner, from the corner's circle"), TipDistance(0.75, 0.75, 0.5), FMath::Sqrt(0.125) * 2.0, 0.0);
	TestEqual(TEXT("Outside the tip"), TipDistance(1.5, 0.0, 0.5), 1.0, 0.0);
	TestEqual(TEXT("A hard square has no rim"), TipDistance(0.9375, 0.9375, 0.0), 0.0, 0.0);
	TestEqual(TEXT("Roundness 1 is a round tip"), TipDistance(0.0, 0.5, 1.0), 0.5, 0.0);

	FClayStripsBrushOp Strips;
	USculptBrushPackClayStripsOpProps* Settings = Prepare<FClayStripsBrushOp, USculptBrushPackClayStripsOpProps>(Strips);
	TestEqual(TEXT("Clay Strips' Plane Offset is Blender 4.2's 0.18"), Settings->PlaneOffset, 0.18f, 0.0f);
	TestEqual(TEXT("and its Tip Roundness Blender 4.2's 0.18"), Settings->TipRoundness, 0.18f, 0.0f);
	// Dyadic settings for exact expectations: the plane 0.25 x 8 = 2 above the brushed area's middle.
	Settings->PlaneOffset = 0.25f;
	Settings->TipRoundness = 0.5f;

	// The cursor moves along +X, so the tip lies along X: X across the stroke is the mesh's Y.
	const FDynamicMesh3 Mesh = MakePoints({
		FVector3d(0, 0, -2),	// 4 below the plane: depth 0.5, the most
		FVector3d(0, 0, 0),		// 2 below: depth 0.25
		FVector3d(0, 0, 2),		// on the plane
		FVector3d(0, 0, 3),		// above it
		FVector3d(0, 12, -2),	// beside the tip
		FVector3d(0, 6, -2) });	// on the tip's rim: across the stroke 0.75, a distance of 0.5
	FSculptBrushStamp First = MakeStamp(FVector3d(-1, 0, 0));
	First.RegionPlane = FFrame3d(FVector3d::Zero());
	Strips.BeginStroke(&Mesh, First, {});
	FSculptBrushStamp Stamp = MakeStamp(FVector3d::Zero());
	TArray<FVector3d> Moved = Apply(Strips, Mesh, Stamp);
	// The push is the normal x 0.3 x 0.75 x radius 8, weighted by depth x (1 - depth).
	const double Push = 0.3 * 0.75 * 8.0;
	TestPoint(this, TEXT("Half a radius below the plane: weight 0.25"), Moved[0], FVector3d(0, 0, -2.0 + 0.25 * Push));
	TestPoint(this, TEXT("A quarter below: weight 0.1875"), Moved[1], FVector3d(0, 0, 0.0 + 0.1875 * Push));
	TestPoint(this, TEXT("On the plane: nothing"), Moved[2], FVector3d(0, 0, 2));
	TestPoint(this, TEXT("Above the plane: nothing"), Moved[3], FVector3d(0, 0, 3));
	TestPoint(this, TEXT("Beside the tip: nothing"), Moved[4], FVector3d(0, 12, -2));
	TestPoint(this, TEXT("On the rim: the falloff is read there (1 in this test)"), Moved[5], FVector3d(0, 6, -2.0 + 0.25 * Push));

	// Plane Trim leaves the deep points alone.
	Settings->bPlaneTrim = true;
	Settings->PlaneTrim = 0.375f;
	Strips.BeginStroke(&Mesh, First, {});
	Moved = Apply(Strips, Mesh, Stamp);
	TestPoint(this, TEXT("Trimmed: the deep point stays"), Moved[0], FVector3d(0, 0, -2));
	TestPoint(this, TEXT("Trimmed: the shallow one still rises"), Moved[1], FVector3d(0, 0, 0.0 + 0.1875 * Push));
	Settings->bPlaneTrim = false;

	// A stamp without movement has no direction for the tip: nothing moves.
	Moved = Apply(Strips, Mesh, Stamp);
	TestPoint(this, TEXT("Without movement nothing moves"), Moved[0], FVector3d(0, 0, -2));

	// Inverted, the tip takes the points above the plane, which now lies 2 below the middle, and pushes them down.
	Strips.BeginStroke(&Mesh, First, {});
	Stamp.Direction = -1.0;
	Moved = Apply(Strips, Mesh, Stamp);
	TestPoint(this, TEXT("Inverted: a point 4 above the plane sinks"), Moved[2], FVector3d(0, 0, 2.0 - 0.25 * Push));
	TestPoint(this, TEXT("Inverted: a point on the plane stays"), Moved[0], FVector3d(0, 0, -2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackCreaseTest, "SculptBrushPack.Brushes.CreaseBlobDamStandard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackCreaseTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackTestLocals;

	const FDynamicMesh3 Mesh = MakePoints({ FVector3d(4, 0, 0), FVector3d(0, 0, 4) });
	const FSculptBrushStamp Stamp = MakeStamp(FVector3d::Zero());

	// Crease: the push is radius 8 x 0.75 = 6 into the surface; the pinch 0.75 x 0.5 x 0.5 = 0.1875 of the way to the
	// cursor's line.
	FCreaseBrushOp Crease;
	USculptBrushPackCreaseOpProps* Settings = Prepare<FCreaseBrushOp, USculptBrushPackCreaseOpProps>(Crease);
	Settings->Strength = 1.0f;
	TestEqual(TEXT("Pinch is Blender's 0.5"), Settings->Pinch, 0.5f, 0.0f);
	TArray<FVector3d> Moved = Apply(Crease, Mesh, Stamp);
	TestPoint(this, TEXT("Crease pushes in and draws toward the line"), Moved[0], FVector3d(3.25, 0, -6));
	TestPoint(this, TEXT("A point on the line is only pushed: the pinch gathers on a line, not a point"), Moved[1], FVector3d(0, 0, -2));

	// The pinch does not depend on the Strength setting; the push does.
	Settings->Strength = 0.5f;
	FSculptBrushStamp Half = Stamp;
	Half.Power = 0.5;
	Moved = Apply(Crease, Mesh, Half);
	TestPoint(this, TEXT("At half strength the push halves and the pinch stays"), Moved[0], FVector3d(3.25, 0, -3));

	// Blob pushes out and spreads.
	FBlobBrushOp Blob;
	Prepare<FBlobBrushOp, USculptBrushPackBlobOpProps>(Blob)->Strength = 1.0f;
	Moved = Apply(Blob, Mesh, Stamp);
	TestPoint(this, TEXT("Blob pushes out and spreads"), Moved[0], FVector3d(4.75, 0, 6));

	// Dam Standard: the same push and pinch, weighted by Blender's sharp curve where the point stood before the stroke:
	// (1 - 4/8) to the fourth power = 0.0625. The mesh it is handed never changes during a stroke.
	FDamStandardBrushOp Dam;
	USculptBrushPackDamStandardOpProps* DamSettings = Prepare<FDamStandardBrushOp, USculptBrushPackDamStandardOpProps>(Dam);
	DamSettings->Strength = 1.0f;
	Dam.BeginStroke(&Mesh, Stamp, {});
	Moved = Apply(Dam, Mesh, Stamp);
	TestPoint(this, TEXT("Dam Standard's first stamp"), Moved[0], FVector3d(3.953125, 0, -0.375));
	Moved = Apply(Dam, Mesh, Stamp);
	TestPoint(this, TEXT("Its second stamp adds to the first, weighted as before"), Moved[0], FVector3d(3.90679931640625, 0, -0.75));
	const FDynamicMesh3 Rim = MakePoints({ FVector3d(8, 0, 0) });
	Dam.BeginStroke(&Rim, Stamp, {});
	TestPoint(this, TEXT("A point at the rim does not move"), Apply(Dam, Rim, Stamp)[0], FVector3d(8, 0, 0));

	// With the sharp profile off it follows the tool's falloff (1 in this test).
	DamSettings->bSharpProfile = false;
	Dam.BeginStroke(&Mesh, Stamp, {});
	Moved = Apply(Dam, Mesh, Stamp);
	TestPoint(this, TEXT("Without the sharp profile"), Moved[0], FVector3d(3.25, 0, -6));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackSnakeHookTest, "SculptBrushPack.Brushes.SnakeHook",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackSnakeHookTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackTestLocals;

	const FDynamicMesh3 Mesh = MakePoints({ FVector3d(0, 3, 0) });
	FSnakeHookBrushOp Hook;
	USculptBrushPackSnakeHookOpProps* Settings = Prepare<FSnakeHookBrushOp, USculptBrushPackSnakeHookOpProps>(Hook);
	TestEqual(TEXT("Snake Hook's Strength is the engine's Move brush's 1"), Settings->Strength, 1.0f, 0.0f);

	// The cursor moves 2 along X: the point follows.
	Hook.BeginStroke(&Mesh, MakeStamp(FVector3d::Zero()), {});
	TArray<FVector3d> Moved = Apply(Hook, Mesh, MakeStamp(FVector3d(2, 0, 0)));
	TestPoint(this, TEXT("The point follows the cursor"), Moved[0], FVector3d(2, 3, 0));

	// The next stamp moves by the cursor's movement since the last one, not since the stroke began.
	Moved = Apply(Hook, Mesh, MakeStamp(FVector3d(3, 0, 0)));
	TestPoint(this, TEXT("A second stamp moves by the movement since the first"), Moved[0], FVector3d(1, 3, 0));

	// Pinch 0.25: 2 x 0.25 x movement 2 / radius 8 = 0.125, scaled back near the line by (3/8) squared.
	Settings->Pinch = 0.25f;
	Hook.BeginStroke(&Mesh, MakeStamp(FVector3d::Zero()), {});
	Moved = Apply(Hook, Mesh, MakeStamp(FVector3d(2, 0, 0)));
	TestPoint(this, TEXT("Pinch below 0.5 draws the point toward the stroke's line"), Moved[0], FVector3d(2, 2.947265625, 0));

	// Pinch 0.75 spreads by the same 0.125, without the scaling back.
	Settings->Pinch = 0.75f;
	Hook.BeginStroke(&Mesh, MakeStamp(FVector3d::Zero()), {});
	Moved = Apply(Hook, Mesh, MakeStamp(FVector3d(2, 0, 0)));
	TestPoint(this, TEXT("Pinch above 0.5 spreads it"), Moved[0], FVector3d(2, 3.375, 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackMoveTopologicalTest, "SculptBrushPack.Brushes.MoveTopological",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackMoveTopologicalTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackTestLocals;

	// A strip along X (two rows, x = 0, 2, 4 ... 12) and, just above its start, a separate triangle.
	FDynamicMesh3 Mesh;
	TArray<int32> Near, Far;
	for (int32 Column = 0; Column <= 6; ++Column)
	{
		Near.Add(Mesh.AppendVertex(FVector3d(2.0 * Column, 0, 0)));
		Far.Add(Mesh.AppendVertex(FVector3d(2.0 * Column, 1, 0)));
	}
	for (int32 Column = 0; Column < 6; ++Column)
	{
		Mesh.AppendTriangle(Near[Column], Near[Column + 1], Far[Column + 1]);
		Mesh.AppendTriangle(Near[Column], Far[Column + 1], Far[Column]);
	}
	const int32 OtherA = Mesh.AppendVertex(FVector3d(0, 0, 1));
	const int32 OtherB = Mesh.AppendVertex(FVector3d(1, 0, 1));
	const int32 OtherC = Mesh.AppendVertex(FVector3d(0, 1, 1));
	Mesh.AppendTriangle(OtherA, OtherB, OtherC);

	FMoveTopologicalBrushOp Move;
	USculptBrushPackMoveTopologicalOpProps* Settings = Prepare<FMoveTopologicalBrushOp, USculptBrushPackMoveTopologicalOpProps>(Move);
	TestEqual(TEXT("Its Strength is the engine's Move brush's 1"), Settings->Strength, 1.0f, 0.0f);

	// The stroke begins on the strip's first point with a radius of 5: every point of the mesh within it is brushed.
	FSculptBrushStamp First = MakeStamp(FVector3d::Zero());
	First.Radius = 5.0;
	TArray<int32> Brushed;
	for (const int32 Vertex : Mesh.VertexIndicesItr())
	{
		if (UE::Geometry::Distance(Mesh.GetVertex(Vertex), FVector3d::Zero()) < First.Radius)
		{
			Brushed.Add(Vertex);
		}
	}
	TestTrue(TEXT("The separate triangle is inside the brush"), Brushed.Contains(OtherA) && Brushed.Contains(OtherB) && Brushed.Contains(OtherC));
	Move.BeginStroke(&Mesh, First, Brushed);

	// Joined: the strip's points inside the brush, and the ones just past it that an edge from inside reaches (x = 6).
	TestTrue(TEXT("The clicked point moves"), Move.IsJoined(Near[0]));
	TestTrue(TEXT("Points joined to it inside the brush move"), Move.IsJoined(Near[2]) && Move.IsJoined(Far[2]));
	TestTrue(TEXT("A point one edge past the brush is reached"), Move.IsJoined(Near[3]));
	TestFalse(TEXT("The fill does not go on from outside the brush"), Move.IsJoined(Near[4]) || Move.IsJoined(Far[5]));
	TestFalse(TEXT("A separate piece inside the brush stays"), Move.IsJoined(OtherA) || Move.IsJoined(OtherB) || Move.IsJoined(OtherC));

	// The cursor moves 3 up: the joined points follow, the others stay where they were.
	FSculptBrushStamp Stamp = MakeStamp(FVector3d(0, 0, 3));
	Stamp.Radius = 5.0;
	const TArray<FVector3d> Moved = Apply(Move, Mesh, Stamp);
	TestPoint(this, TEXT("The clicked point follows the cursor"), Moved[Near[0]], FVector3d(0, 0, 3));
	TestPoint(this, TEXT("A joined point follows"), Moved[Far[2]], FVector3d(4, 1, 3));
	TestPoint(this, TEXT("The separate piece stays"), Moved[OtherA], FVector3d(0, 0, 1));
	TestPoint(this, TEXT("The far end of the strip stays"), Moved[Near[6]], FVector3d(12, 0, 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackLayerTest, "SculptBrushPack.Brushes.Layer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackLayerTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackTestLocals;

	// A flat fan facing +Z; the brush is handed its middle point. The mesh it is handed never changes during a stroke.
	const FDynamicMesh3 Mesh = MakeFan(FVector3d::Zero());
	const TArray<int32> Middle = { 0 };
	const FSculptBrushStamp Stamp = MakeStamp(FVector3d::Zero());
	FLayerBrushOp Layer;
	USculptBrushPackLayerOpProps* Settings = Prepare<FLayerBrushOp, USculptBrushPackLayerOpProps>(Layer);
	TestEqual(TEXT("Layer's Height is Blender 4.2's 0.05 m, in centimeters"), Settings->Height, 5.0f, 0.0f);
	Settings->Height = 8.0f;

	// The point's normal is +Z. Its length is the engine's to round (it scales by a reciprocal), so the expected heights
	// carry it instead of assuming exactly 1.
	const FVector3d Normal = UE::Geometry::FMeshNormals::ComputeVertexNormal(Mesh, 0, /*bWeightByArea*/ false, /*bWeightByAngle*/ true);
	TestTrue(TEXT("The middle point's normal is along +Z"), Normal.X == 0.0 && Normal.Y == 0.0 && Normal.Z > 0.0);

	// The amount after one stamp is weight 1 x strength 0.75 x (1.05 - 0); the point stands Height x amount up its normal.
	Layer.BeginStroke(&Mesh, Stamp, {});
	TestPoint(this, TEXT("The first stamp raises the point by Height x 0.7875"), ApplyTo(Layer, Mesh, Stamp, Middle)[0], FVector3d(0, 0, Normal.Z * (8.0 * (0.75 * 1.05))));
	// The amount stops at 1: 0.7875, then 0.984375, then past 1.
	ApplyTo(Layer, Mesh, Stamp, Middle);
	TestPoint(this, TEXT("After three stamps the layer is Height thick, no more"), ApplyTo(Layer, Mesh, Stamp, Middle)[0], FVector3d(0, 0, Normal.Z * 8.0));
	TestPoint(this, TEXT("and stays so"), ApplyTo(Layer, Mesh, Stamp, Middle)[0], FVector3d(0, 0, Normal.Z * 8.0));

	// Inverted, the layer is cut in.
	FSculptBrushStamp Inverted = Stamp;
	Inverted.Direction = -1.0;
	Layer.BeginStroke(&Mesh, Inverted, {});
	TestPoint(this, TEXT("Inverted, the first stamp lowers the point"), ApplyTo(Layer, Mesh, Inverted, Middle)[0], FVector3d(0, 0, Normal.Z * (8.0 * -(0.75 * 1.05))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackFillScrapeTest, "SculptBrushPack.Brushes.FillScrape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackFillScrapeTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackTestLocals;

	// The plane is z = 0 through the stamp; radius 8, strength 0.75.
	const FDynamicMesh3 Mesh = MakePoints({
		FVector3d(0, 0, -4),	// half a radius below
		FVector3d(0, 0, 4),		// half a radius above
		FVector3d(4, 0, -4),	// below and to the side, inside the brush
		FVector3d(8, 0, -4),	// below, outside it
		FVector3d(0, 0, -2) });	// a quarter radius below
	const FSculptBrushStamp Before = MakeStamp(FVector3d(-1, 0, 0));
	FSculptBrushStamp Stamp = MakeStamp(FVector3d::Zero());

	FPlaneSideBrushOp Fill;
	USculptBrushPackFillOpProps* FillSettings = Prepare<FPlaneSideBrushOp, USculptBrushPackFillOpProps>(Fill);
	TestTrue(TEXT("Fill reaches below the plane only"), FillSettings->Height == 0.0f && FillSettings->Depth == 1.0f && FillSettings->PlaneOffset == 0.0f);
	Fill.BeginStroke(&Mesh, Before, {});
	TArray<FVector3d> Moved = Apply(Fill, Mesh, Stamp);
	// A point 4 below moves up by its distance x 0.75.
	TestPoint(this, TEXT("Fill raises a point below the plane"), Moved[0], FVector3d(0, 0, -1));
	TestPoint(this, TEXT("and leaves a point above it"), Moved[1], FVector3d(0, 0, 4));
	TestPoint(this, TEXT("A point below and to the side rises too"), Moved[2], FVector3d(4, 0, -1));
	TestPoint(this, TEXT("A point outside the brush stays"), Moved[3], FVector3d(8, 0, -4));

	// A stamp without movement does nothing.
	Moved = Apply(Fill, Mesh, Stamp);
	TestPoint(this, TEXT("Without movement nothing moves"), Moved[0], FVector3d(0, 0, -4));

	// Inverted, Fill scrapes.
	Fill.BeginStroke(&Mesh, Before, {});
	Stamp.Direction = -1.0;
	Moved = Apply(Fill, Mesh, Stamp);
	TestPoint(this, TEXT("Inverted, Fill lowers a point above the plane"), Moved[1], FVector3d(0, 0, 1));
	TestPoint(this, TEXT("and leaves a point below it"), Moved[0], FVector3d(0, 0, -4));
	Stamp.Direction = 1.0;

	// A Depth of half a radius: the reach ends there, and the effect is scaled by it.
	FillSettings->Depth = 0.5f;
	Fill.BeginStroke(&Mesh, Before, {});
	Moved = Apply(Fill, Mesh, Stamp);
	TestPoint(this, TEXT("Within a Depth of 0.5 the point rises by half as much"), Moved[4], FVector3d(0, 0, -1.25));
	TestPoint(this, TEXT("At the end of the Depth it stays"), Moved[0], FVector3d(0, 0, -4));

	// Scrape is the other side.
	FPlaneSideBrushOp Scrape;
	USculptBrushPackScrapeOpProps* ScrapeSettings = Prepare<FPlaneSideBrushOp, USculptBrushPackScrapeOpProps>(Scrape);
	TestTrue(TEXT("Scrape reaches above the plane only"), ScrapeSettings->Height == 1.0f && ScrapeSettings->Depth == 0.0f);
	Scrape.BeginStroke(&Mesh, Before, {});
	Moved = Apply(Scrape, Mesh, Stamp);
	TestPoint(this, TEXT("Scrape lowers a point above the plane"), Moved[1], FVector3d(0, 0, 1));
	TestPoint(this, TEXT("and leaves a point below it"), Moved[0], FVector3d(0, 0, -4));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackNudgeThumbTest, "SculptBrushPack.Brushes.NudgeThumb",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackNudgeThumbTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackTestLocals;

	// The area's normal is +Z; the cursor moves 2 along X and 1 up. Only the part along the surface counts.
	const FDynamicMesh3 Mesh = MakePoints({ FVector3d(0, 3, 0) });
	FNudgeBrushOp Nudge;
	USculptBrushPackNudgeOpProps* NudgeSettings = Prepare<FNudgeBrushOp, USculptBrushPackNudgeOpProps>(Nudge);
	TestEqual(TEXT("Nudge's Strength is the engine's Move brush's 1"), NudgeSettings->Strength, 1.0f, 0.0f);
	Nudge.BeginStroke(&Mesh, MakeStamp(FVector3d::Zero()), {});
	TArray<FVector3d> Moved = Apply(Nudge, Mesh, MakeStamp(FVector3d(2, 0, 1)));
	TestPoint(this, TEXT("Nudge moves the point along the surface only"), Moved[0], FVector3d(2, 3, 0));
	Moved = Apply(Nudge, Mesh, MakeStamp(FVector3d(3, 0, 1)));
	TestPoint(this, TEXT("Its next stamp moves by the movement since the last one"), Moved[0], FVector3d(1, 3, 0));
	FSculptBrushStamp Half = MakeStamp(FVector3d(5, 0, 1));
	Half.Power = 0.5;
	Moved = Apply(Nudge, Mesh, Half);
	TestPoint(this, TEXT("At half strength it moves half as far"), Moved[0], FVector3d(1, 3, 0));

	// Thumb counts the movement from the stroke's start, on the mesh from before the stroke.
	FThumbBrushOp Thumb;
	USculptBrushPackThumbOpProps* ThumbSettings = Prepare<FThumbBrushOp, USculptBrushPackThumbOpProps>(Thumb);
	TestEqual(TEXT("Thumb's Strength is the engine's Move brush's 1"), ThumbSettings->Strength, 1.0f, 0.0f);
	Thumb.BeginStroke(&Mesh, MakeStamp(FVector3d::Zero()), {});
	Moved = Apply(Thumb, Mesh, MakeStamp(FVector3d(2, 0, 1)));
	TestPoint(this, TEXT("Thumb moves the point along the surface only"), Moved[0], FVector3d(2, 3, 0));
	Moved = Apply(Thumb, Mesh, MakeStamp(FVector3d(4, 0, 1)));
	TestPoint(this, TEXT("Its next stamp counts from the stroke's start"), Moved[0], FVector3d(4, 3, 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackSlideRelaxTest, "SculptBrushPack.Brushes.TopoSlideRelax",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackSlideRelaxTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackTestLocals;

	// A quarter power: the stamp's strength is 0.25 x 1/8 x 6 = 0.1875, and these brushes use twice that, 0.375.
	auto QuarterStamp = [](const FVector3d& Origin)
	{
		FSculptBrushStamp Stamp = MakeStamp(Origin);
		Stamp.Power = 0.25;
		return Stamp;
	};
	const TArray<int32> Middle = { 0 };

	// TopoSlide: the middle of a flat fan, the cursor moving along +X. Only the neighbor ahead, 4 away, draws it:
	// 4 x 0.375 = 1.5. The neighbors to the side and behind do not.
	{
		const FDynamicMesh3 Mesh = MakeFan(FVector3d::Zero());
		FTopologySlideBrushOp Slide;
		Prepare<FTopologySlideBrushOp, USculptBrushPackTopologySlideOpProps>(Slide);
		Slide.BeginStroke(&Mesh, QuarterStamp(FVector3d::Zero()), {});
		TestPoint(this, TEXT("TopoSlide moves the point toward the neighbor ahead"), ApplyTo(Slide, Mesh, QuarterStamp(FVector3d(1, 0, 0)), Middle)[0], FVector3d(1.5, 0, 0));
		// Now 2.5 from that neighbor: 2.5 x 0.375 = 0.9375 more.
		TestPoint(this, TEXT("The next stamp goes on from there"), ApplyTo(Slide, Mesh, QuarterStamp(FVector3d(2, 0, 0)), Middle)[0], FVector3d(2.4375, 0, 0));
		TestPoint(this, TEXT("A stamp without movement moves nothing"), ApplyTo(Slide, Mesh, QuarterStamp(FVector3d(2, 0, 0)), Middle)[0], FVector3d(2.4375, 0, 0));
	}

	// Relax: the middle point sits off-center in the fan's plane. Each of the four passes takes it 0.375 of the way to
	// its neighbors' average, the fan's center: 1 x 0.625 to the fourth power.
	{
		const FDynamicMesh3 Mesh = MakeFan(FVector3d(1, 0, 0));
		FRelaxBrushOp Relax;
		Prepare<FRelaxBrushOp, USculptBrushPackRelaxOpProps>(Relax);
		Relax.BeginStroke(&Mesh, QuarterStamp(FVector3d::Zero()), {});
		TestPoint(this, TEXT("Relax evens the point out within the surface"), ApplyTo(Relax, Mesh, QuarterStamp(FVector3d::Zero()), Middle)[0], FVector3d(0.152587890625, 0, 0));
	}

	// A point on a straight open border follows its border neighbors only, and on a straight line that leaves it where it
	// is: its two border edges point opposite ways, so there is no plane to move in. Its inner neighbors do not pull it.
	{
		FDynamicMesh3 Strip;
		for (const FVector3d& Point : { FVector3d(0, 0, 0), FVector3d(4, 0, 0), FVector3d(6, 0, 0), FVector3d(0, 4, 0), FVector3d(4, 4, 0), FVector3d(6, 4, 0) })
		{
			Strip.AppendVertex(Point);
		}
		Strip.AppendTriangle(0, 4, 1);
		Strip.AppendTriangle(0, 3, 4);
		Strip.AppendTriangle(1, 5, 2);
		Strip.AppendTriangle(1, 4, 5);
		FRelaxBrushOp Relax;
		Prepare<FRelaxBrushOp, USculptBrushPackRelaxOpProps>(Relax);
		const TArray<int32> BorderPoint = { 1 };
		Relax.BeginStroke(&Strip, QuarterStamp(FVector3d::Zero()), {});
		TestPoint(this, TEXT("A point on a straight border stays"), ApplyTo(Relax, Strip, QuarterStamp(FVector3d::Zero()), BorderPoint)[0], FVector3d(4, 0, 0));
	}

	// Relax keeps the shape: a point lifted straight above the fan's center is as far from its neighbors' average as it
	// can be, but only along its own normal, so it does not move. A smoothing brush would pull it down.
	{
		const FDynamicMesh3 Mesh = MakeFan(FVector3d(0, 0, 2));
		FRelaxBrushOp Relax;
		Prepare<FRelaxBrushOp, USculptBrushPackRelaxOpProps>(Relax);
		Relax.BeginStroke(&Mesh, QuarterStamp(FVector3d::Zero()), {});
		TestPoint(this, TEXT("Relax leaves a peak standing"), ApplyTo(Relax, Mesh, QuarterStamp(FVector3d::Zero()), Middle)[0], FVector3d(0, 0, 2));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackGlideTest, "SculptBrushPack.Brushes.Glide",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackGlideTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackTestLocals;

	FGlideBrushOp Glide;
	USculptBrushPackGlideOpProps* Settings = Prepare<FGlideBrushOp, USculptBrushPackGlideOpProps>(Glide);
	TestEqual(TEXT("Glide's Strength is the engine's Move brush's 1"), Settings->Strength, 1.0f, 0.0f);
	TestEqual(TEXT("and its Falloff that brush's 0.5"), Settings->Falloff, 0.5f, 0.0f);

	// A flat sheet, and a loose triangle hovering one above its middle.
	FDynamicMesh3 Mesh = MakeSheet([](double X) { return 0.0; });
	const int32 Loose = Mesh.AppendVertex(FVector3d(0, 0, 1));
	Mesh.AppendVertex(FVector3d(1, 0, 1));
	Mesh.AppendVertex(FVector3d(0, 1, 1));
	Mesh.AppendTriangle(Loose, Loose + 2, Loose + 1);
	const TArray<int32> All = AllVertices(Mesh);
	const int32 Middle = SheetVertex(2, 2);

	// The stroke begins on the sheet's middle point. The whole sheet lies within four radii of it and is kept; the loose
	// triangle is not joined to it.
	Glide.BeginStroke(&Mesh, MakeStamp(FVector3d::Zero()), All);
	TestEqual(TEXT("The sheet's 32 triangles are kept, the loose one is not"), Glide.GetKeptTriangleCount(), 32);

	// The cursor moves one along X and half off the sheet: each point follows it and is laid back on the sheet.
	ApplyAndStore(Glide, Mesh, MakeStamp(FVector3d(1, 0, 0.5)), All);
	TestPoint(this, TEXT("The middle point glides one along the sheet"), Mesh.GetVertex(Middle), FVector3d(1, 0, 0));
	TestPoint(this, TEXT("A point of the sheet's edge stays on the edge"), Mesh.GetVertex(SheetVertex(4, 3)), FVector3d(4, 2, 0));
	TestPoint(this, TEXT("The loose triangle stays"), Mesh.GetVertex(Loose), FVector3d(0, 0, 1));
	// The next stamp counts from the last, and moves the points on from where they are.
	ApplyAndStore(Glide, Mesh, MakeStamp(FVector3d(2, 1, 0.5)), All);
	TestPoint(this, TEXT("The next stamp moves the middle point on"), Mesh.GetVertex(Middle), FVector3d(2, 1, 0));
	TestPoint(this, TEXT("and the edge point along the edge"), Mesh.GetVertex(SheetVertex(4, 3)), FVector3d(4, 3, 0));

	// At half strength the points follow half as far.
	{
		const FDynamicMesh3 Flat = MakeSheet([](double X) { return 0.0; });
		Glide.BeginStroke(&Flat, MakeStamp(FVector3d::Zero()), AllVertices(Flat));
		FSculptBrushStamp Half = MakeStamp(FVector3d(2, 0, 0));
		Half.Power = 0.5;
		TestPoint(this, TEXT("At half strength the point follows half as far"), ApplyTo(Glide, Flat, Half, { Middle })[0], FVector3d(1, 0, 0));
	}

	// The surface is kept out to four radii. With a radius of a half that is two, the distance to the nearest points, so
	// the walk stops at the middle point's six neighbors and only the six triangles around it are kept.
	{
		const FDynamicMesh3 Flat = MakeSheet([](double X) { return 0.0; });
		FSculptBrushStamp Small = MakeStamp(FVector3d::Zero());
		Small.Radius = 0.5;
		Glide.BeginStroke(&Flat, Small, { Middle });
		TestEqual(TEXT("With a small brush only the triangles within four radii are kept"), Glide.GetKeptTriangleCount(), 6);
	}

	// A sheet folded along X = 0: flat where X is negative, falling one for one where X is positive. Its point at
	// (-2, 0, 0) is carried to (2, 1, 0), off the sheet. The nearest point of the sheet is on the slope: (1, 1, -1), the
	// square root of 2 away, where the flat part is 2 away. The numbers are chosen so that the engine's nearest-point
	// arithmetic is exact (DistPoint3Triangle3.h:49-195 in 5.8.4 divides by an edge's squared length, here 12, and
	// carrying the point to (2, 0.5, 0) gives a Y one rounding step from 0.5).
	{
		const FDynamicMesh3 Folded = MakeSheet([](double X) { return X > 0.0 ? -X : 0.0; });
		Glide.BeginStroke(&Folded, MakeStamp(FVector3d(-2, 0, 0)), AllVertices(Folded));
		TestPoint(this, TEXT("A point carried past a fold is laid on the slope beyond it"),
			ApplyTo(Glide, Folded, MakeStamp(FVector3d(2, 1, 0)), { SheetVertex(1, 2) })[0], FVector3d(1, 1, -1));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackTubeTest, "SculptBrushPack.Brushes.Tube",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackTubeTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackTestLocals;

	FTubeBrushOp Tube;
	USculptBrushPackTubeOpProps* Settings = Prepare<FTubeBrushOp, USculptBrushPackTubeOpProps>(Tube);
	TestEqual(TEXT("Tube's Height is half the radius"), Settings->Height, 0.5f, 0.0f);
	TestEqual(TEXT("and its Strength 1"), Settings->Strength, 1.0f, 0.0f);

	// A flat fan facing +Z: the middle point and four points 4 away. The mesh a brush of this kind is handed never changes
	// during a stroke. Each point's normal is +Z; its length is the engine's to round, so the expected heights carry it.
	const FDynamicMesh3 Mesh = MakeFan(FVector3d::Zero());
	TArray<FVector3d> Normals;
	for (int32 Vertex = 0; Vertex < 5; ++Vertex)
	{
		Normals.Add(NormalOf(Mesh, Vertex));
		TestTrue(TEXT("Each point's normal is along +Z"), Normals[Vertex].X == 0.0 && Normals[Vertex].Y == 0.0 && Normals[Vertex].Z > 0.0);
	}

	// One stamp on the middle: radius 8 x Height 0.5 x strength 1 = 4 high there; half way out, 4 x sqrt(1 - 0.25).
	const FSculptBrushStamp Stamp = MakeStamp(FVector3d::Zero());
	const double HalfWayOut = 4.0 * FMath::Sqrt(0.75);
	Tube.BeginStroke(&Mesh, Stamp, {});
	TArray<FVector3d> Moved = Apply(Tube, Mesh, Stamp);
	TestPoint(this, TEXT("The middle of the bead is half a radius high"), Moved[0], FVector3d(0, 0, Normals[0].Z * 4.0));
	TestPoint(this, TEXT("Half way out the bead follows the half circle"), Moved[1], FVector3d(4, 0, Normals[1].Z * HalfWayOut));
	TestPoint(this, TEXT("A second stamp in the same place adds nothing"), Apply(Tube, Mesh, Stamp)[0], FVector3d(0, 0, Normals[0].Z * 4.0));

	// The stroke goes on to the point at (4, 0, 0). That point is now the middle of a stamp; the first middle, half way
	// out of this one, keeps its height; the point at (-4, 0, 0), on this stamp's rim, keeps what the first stamp gave it.
	Moved = Apply(Tube, Mesh, MakeStamp(FVector3d(4, 0, 0)));
	TestPoint(this, TEXT("The bead is as high where the stroke went on to"), Moved[1], FVector3d(4, 0, Normals[1].Z * 4.0));
	TestPoint(this, TEXT("The first middle keeps its height"), Moved[0], FVector3d(0, 0, Normals[0].Z * 4.0));
	TestPoint(this, TEXT("A point on the new stamp's rim keeps what it had"), Moved[3], FVector3d(-4, 0, Normals[3].Z * HalfWayOut));

	// Pressing more lightly over the same place lowers nothing.
	FSculptBrushStamp Light = Stamp;
	Light.Power = 0.25;
	TestPoint(this, TEXT("A lighter pass lowers nothing"), Apply(Tube, Mesh, Light)[0], FVector3d(0, 0, Normals[0].Z * 4.0));

	// A lighter stroke of its own draws a thinner bead: 8 x 0.5 x 0.25 = 1.
	Tube.BeginStroke(&Mesh, Light, {});
	TestPoint(this, TEXT("A lighter stroke draws a thinner bead"), Apply(Tube, Mesh, Light)[0], FVector3d(0, 0, Normals[0].Z * 1.0));

	// Inverted, the stroke cuts a channel as deep.
	FSculptBrushStamp Inverted = Stamp;
	Inverted.Direction = -1.0;
	Tube.BeginStroke(&Mesh, Inverted, {});
	TestPoint(this, TEXT("Inverted, the stroke cuts a channel"), Apply(Tube, Mesh, Inverted)[0], FVector3d(0, 0, Normals[0].Z * -4.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackNoiseTest, "SculptBrushPack.Brushes.Noise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackNoiseTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackTestLocals;

	FNoiseBrushOp Noise;
	USculptBrushPackNoiseOpProps* Settings = Prepare<FNoiseBrushOp, USculptBrushPackNoiseOpProps>(Noise);
	TestEqual(TEXT("Noise's Height is 1"), Settings->Height, 1.0f, 0.0f);
	TestEqual(TEXT("its Scale 5"), Settings->Scale, 5.0f, 0.0f);
	TestEqual(TEXT("and its Strength 1"), Settings->Strength, 1.0f, 0.0f);
	Settings->Height = 2.0f;
	Settings->Scale = 8.0f;

	// A flat fan facing +Z. With a Scale of 8 its middle point lies on the noise's lattice, where the noise is zero, and
	// its other points lie between lattice points.
	const FDynamicMesh3 Mesh = MakeFanOf(FVector3d::Zero(), { FVector3d(4, 2, 0), FVector3d(-2, 4, 0), FVector3d(-4, -2, 0), FVector3d(2, -4, 0) });
	TArray<FVector3d> Normals;
	TArray<double> Pattern;
	bool bSomeBump = false;
	for (int32 Vertex = 0; Vertex < 5; ++Vertex)
	{
		Normals.Add(NormalOf(Mesh, Vertex));
		TestTrue(TEXT("Each point's normal is along +Z"), Normals[Vertex].X == 0.0 && Normals[Vertex].Y == 0.0 && Normals[Vertex].Z > 0.0);
		Pattern.Add(static_cast<double>(FMath::PerlinNoise3D(FVector(Mesh.GetVertex(Vertex) / 8.0))));
		bSomeBump |= Pattern[Vertex] != 0.0;
	}
	TestEqual(TEXT("The engine's noise is zero on its lattice"), Pattern[0], 0.0, 0.0);
	TestTrue(TEXT("and not zero everywhere between"), bSomeBump);

	// At full reach a point stands Height x the noise at its place off the surface, along its normal.
	auto Bumped = [&](int32 Vertex, double Reach, double Direction)
	{
		const FVector3d Before = Mesh.GetVertex(Vertex);
		return FVector3d(Before.X, Before.Y, Normals[Vertex].Z * (Reach * (Direction * 2.0 * Pattern[Vertex])));
	};
	const FSculptBrushStamp Stamp = MakeStamp(FVector3d::Zero());
	Noise.BeginStroke(&Mesh, Stamp, {});
	TArray<FVector3d> Moved = Apply(Noise, Mesh, Stamp);
	TestPoint(this, TEXT("A point on the lattice stays"), Moved[0], FVector3d(0, 0, 0));
	for (int32 Vertex = 1; Vertex < 5; ++Vertex)
	{
		TestPoint(this, TEXT("A point stands Height x the noise at its place off the surface"), Moved[Vertex], Bumped(Vertex, 1.0, 1.0));
	}
	Moved = Apply(Noise, Mesh, Stamp);
	TestPoint(this, TEXT("A second pass piles nothing up"), Moved[1], Bumped(1, 1.0, 1.0));

	// The pattern is fixed in space: a brush twice the size, somewhere else, shows the same bumps.
	FSculptBrushStamp Large = MakeStamp(FVector3d(1, 1, 0));
	Large.Radius = 16.0;
	Noise.BeginStroke(&Mesh, Large, {});
	Moved = Apply(Noise, Mesh, Large);
	TestPoint(this, TEXT("A larger brush elsewhere shows the same bump"), Moved[2], Bumped(2, 1.0, 1.0));

	// A point keeps the most any stamp of the stroke gave it: half, then all, and a lighter pass takes nothing back.
	FSculptBrushStamp Half = Stamp;
	Half.Power = 0.5;
	Noise.BeginStroke(&Mesh, Half, {});
	TestPoint(this, TEXT("At half strength half the bump shows"), Apply(Noise, Mesh, Half)[3], Bumped(3, 0.5, 1.0));
	TestPoint(this, TEXT("A full stamp shows all of it"), Apply(Noise, Mesh, Stamp)[3], Bumped(3, 1.0, 1.0));
	TestPoint(this, TEXT("A lighter pass takes nothing back"), Apply(Noise, Mesh, Half)[3], Bumped(3, 1.0, 1.0));

	// Inverted, the bumps are turned over.
	FSculptBrushStamp Inverted = Stamp;
	Inverted.Direction = -1.0;
	Noise.BeginStroke(&Mesh, Inverted, {});
	TestPoint(this, TEXT("Inverted, the bump is turned over"), Apply(Noise, Mesh, Inverted)[4], Bumped(4, 1.0, -1.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackSharpenTest, "SculptBrushPack.Brushes.Sharpen",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackSharpenTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackTestLocals;

	FSharpenBrushOp Sharpen;
	USculptBrushPackSharpenOpProps* Settings = Prepare<FSharpenBrushOp, USculptBrushPackSharpenOpProps>(Sharpen);
	TestEqual(TEXT("Sharpen's Amount is 1"), Settings->Amount, 1.0f, 0.0f);
	TestEqual(TEXT("its Detail Size 4 rounds"), Settings->DetailSize, 4);
	TestEqual(TEXT("and its Strength 1"), Settings->Strength, 1.0f, 0.0f);

	// A peak: the middle point 8 above a rim of seven points that add up to zero. Averaging counts the point and its
	// neighbors once each: eight for the peak, four for a rim point (itself, the peak and two of the rim).
	const FVector3d Peak(0, 0, 8);
	const FDynamicMesh3 Mesh = MakeFanOf(Peak, { FVector3d(8, 0, 0), FVector3d(4, 8, 0), FVector3d(-4, 8, 0), FVector3d(-8, 4, 0),
		FVector3d(-8, -4, 0), FVector3d(-4, -8, 0), FVector3d(12, -8, 0) });
	const TArray<int32> Middle = { 0 };
	const FVector3d Normal = NormalOf(Mesh, 0);
	TestTrue(TEXT("The peak's normal points up"), Normal.Z > 0.0);
	const FSculptBrushStamp Stamp = MakeStamp(Peak);

	// One round puts the peak at (8 + 0) / 8 = 1 high: its relief is 7, straight up. Along its normal that is 7 x the
	// normal's Z, and at full reach and an Amount of 1 the peak has that much more along its normal.
	Settings->DetailSize = 1;
	Sharpen.BeginStroke(&Mesh, Stamp, {});
	TestPoint(this, TEXT("One round: the peak gains its relief of 7 along its normal"), ApplyTo(Sharpen, Mesh, Stamp, Middle)[0], Peak + Normal * (7.0 * Normal.Z));
	TestPoint(this, TEXT("A second pass adds nothing"), ApplyTo(Sharpen, Mesh, Stamp, Middle)[0], Peak + Normal * (7.0 * Normal.Z));

	// Two rounds: after the first the peak is 1 high and every rim point (0 + 8) / 4 = 2 high, and the rim points still
	// add up to zero across; the second puts the peak at (1 + 7 x 2) / 8 = 1.875. Its relief is 6.125.
	Settings->DetailSize = 2;
	Sharpen.BeginStroke(&Mesh, Stamp, {});
	TestPoint(this, TEXT("Two rounds: the peak gains its relief of 6.125"), ApplyTo(Sharpen, Mesh, Stamp, Middle)[0], Peak + Normal * (6.125 * Normal.Z));

	// A point keeps the most any stamp of the stroke gave it.
	Settings->DetailSize = 1;
	FSculptBrushStamp Half = Stamp;
	Half.Power = 0.5;
	Sharpen.BeginStroke(&Mesh, Half, {});
	TestPoint(this, TEXT("At half strength the peak gains half its relief"), ApplyTo(Sharpen, Mesh, Half, Middle)[0], Peak + Normal * (0.5 * (7.0 * Normal.Z)));
	TestPoint(this, TEXT("A full stamp gives all of it"), ApplyTo(Sharpen, Mesh, Stamp, Middle)[0], Peak + Normal * (7.0 * Normal.Z));
	TestPoint(this, TEXT("A lighter pass takes nothing back"), ApplyTo(Sharpen, Mesh, Half, Middle)[0], Peak + Normal * (7.0 * Normal.Z));

	// Amount scales what is gained.
	Settings->Amount = 0.5f;
	Sharpen.BeginStroke(&Mesh, Stamp, {});
	TestPoint(this, TEXT("At an Amount of a half the peak gains half its relief"), ApplyTo(Sharpen, Mesh, Stamp, Middle)[0], Peak + Normal * (0.5 * (7.0 * Normal.Z)));

	// Inverted, the relief is taken away.
	Settings->Amount = 1.0f;
	FSculptBrushStamp Inverted = Stamp;
	Inverted.Direction = -1.0;
	Sharpen.BeginStroke(&Mesh, Inverted, {});
	TestPoint(this, TEXT("Inverted, the peak loses its relief"), ApplyTo(Sharpen, Mesh, Inverted, Middle)[0], Peak + Normal * (-7.0 * Normal.Z));

	// A flat surface has no relief: nothing moves, the edge points included, whose averaged places lie in the surface.
	const FDynamicMesh3 Flat = MakeFan(FVector3d::Zero());
	Settings->DetailSize = 2;
	Sharpen.BeginStroke(&Flat, MakeStamp(FVector3d::Zero()), {});
	const TArray<FVector3d> Moved = Apply(Sharpen, Flat, MakeStamp(FVector3d::Zero()));
	for (const int32 Vertex : Flat.VertexIndicesItr())
	{
		TestPoint(this, TEXT("A point of a flat surface stays"), Moved[Vertex], Flat.GetVertex(Vertex));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackHookTest, "SculptBrushPack.Hook.VertexSculptInModelingMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackHookTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackTestLocals;

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!TestNotNull(TEXT("Editor world"), World))
	{
		return false;
	}
	// The engine's Modeling Mode (UModelingToolsEditorMode::EM_ModelingToolsEditorModeId) and its Vertex Sculpt tool.
	const FEditorModeID ModelingMode(TEXT("EM_ModelingToolsEditorMode"));
	const FString SculptTool(TEXT("BeginSculptMeshTool"));
	GLevelEditorModeTools().ActivateDefaultMode();
	GLevelEditorModeTools().ActivateMode(ModelingMode);
	UEdMode* Mode = GLevelEditorModeTools().GetActiveScriptableMode(ModelingMode);
	UInteractiveToolManager* ToolManager = Mode ? Mode->GetToolManager() : nullptr;
	if (!TestNotNull(TEXT("Modeling Mode's tool manager"), ToolManager))
	{
		GLevelEditorModeTools().ActivateDefaultMode();
		return false;
	}

	// A box to sculpt: a dynamic mesh actor, so no asset is touched.
	FActorSpawnParameters SpawnParams;
	SpawnParams.ObjectFlags = RF_Transient;
	ADynamicMeshActor* Actor = World->SpawnActor<ADynamicMeshActor>(ADynamicMeshActor::StaticClass(), FTransform(FVector(0.0, 60000.0, 0.0)), SpawnParams);
	if (!TestNotNull(TEXT("A dynamic mesh actor"), Actor))
	{
		GLevelEditorModeTools().ActivateDefaultMode();
		return false;
	}
	UE::Geometry::FMinimalBoxMeshGenerator Box;
	Box.Box = UE::Geometry::FOrientedBox3d(FVector3d::Zero(), FVector3d(50.0, 50.0, 50.0));
	FDynamicMesh3 BoxMesh(&Box.Generate());
	Actor->GetDynamicMeshComponent()->SetMesh(MoveTemp(BoxMesh));
	GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
	GEditor->SelectActor(Actor, /*bInSelected*/ true, /*bNotify*/ true);

	const bool bStarted = ToolManager->SelectActiveToolType(EToolSide::Left, SculptTool) && ToolManager->ActivateTool(EToolSide::Left);
	UMeshVertexSculptTool* Tool = Cast<UMeshVertexSculptTool>(ToolManager->GetActiveTool(EToolSide::Left));
	if (TestTrue(TEXT("Vertex Sculpt started on the box"), bStarted) && TestNotNull(TEXT("The Vertex Sculpt tool"), Tool))
	{
		CheckPackBrushes(this, Tool);
		ToolManager->DeactivateTool(EToolSide::Left, EToolShutdownType::Cancel);
	}

	GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
	World->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ false);
	GLevelEditorModeTools().ActivateDefaultMode();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackSkeletalMeshEditorTest, "SculptBrushPack.Hook.SculptInSkeletalMeshEditor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackSkeletalMeshEditorTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackTestLocals;

	// A copy of the engine's skeletal cube in a package of its own: the engine's asset is not opened for editing, and
	// nothing is saved.
	USkeletalMesh* Source = LoadObject<USkeletalMesh>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	if (!TestNotNull(TEXT("The engine's skeletal cube"), Source))
	{
		return false;
	}
	UPackage* Package = CreatePackage(*(TEXT("/Temp/SculptBrushPack/Cube_") + FGuid::NewGuid().ToString()));
	USkeletalMesh* Mesh = DuplicateObject<USkeletalMesh>(Source, Package, TEXT("SkeletalCubeCopy"));
	if (!TestNotNull(TEXT("A copy of the skeletal cube"), Mesh))
	{
		return false;
	}

	UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	const bool bOpened = AssetEditors->OpenEditorForAsset(Mesh);
	IAssetEditorInstance* Instance = bOpened ? AssetEditors->FindEditorForAsset(Mesh, /*bFocusIfOpen*/ false) : nullptr;
	if (!TestNotNull(TEXT("The Skeletal Mesh Editor opened on the copy"), Instance))
	{
		return false;
	}
	if (TestEqual(TEXT("The editor that opened is the Skeletal Mesh Editor"), Instance->GetEditorName(), FName(TEXT("SkeletalMeshEditor"))))
	{
		FAssetEditorToolkit* Toolkit = static_cast<FAssetEditorToolkit*>(Instance);
		FEditorModeTools& Modes = Toolkit->GetEditorModeManager();

		// The editor's editing tools are a mode of their own (SkeletalMeshModelingToolsEditorMode.cpp:240 in 5.8.4). The
		// editor switches it on as it opens unless the user switched it off last time (SkeletalMeshModelingToolsModule.cpp:196-205);
		// otherwise this presses the toolbar's Editing Tools button (SkeletalMeshModelingToolsCommands.cpp:21).
		const FEditorModeID EditingTools(TEXT("SkeletalMeshModelingToolsEditorMode"));
		if (!Modes.IsModeActive(EditingTools))
		{
			const TSharedPtr<FUICommandInfo> Toggle = FInputBindingManager::Get().FindCommandInContext(TEXT("SkeletalMeshModelingTools"), TEXT("ToggleEditingToolsMode"));
			if (Toggle.IsValid())
			{
				Toolkit->GetToolkitCommands()->TryExecuteAction(Toggle.ToSharedRef());
			}
		}
		UEdMode* Mode = Modes.GetActiveScriptableMode(EditingTools);
		UInteractiveToolManager* ToolManager = Mode ? Mode->GetToolManager() : nullptr;
		if (TestNotNull(TEXT("The editing tools' tool manager"), ToolManager))
		{
			// The pack finds new tool managers on a timer; a test that starts a tool at once asks it to look now.
			WatchToolManagers();

			// Sculpt, in the Deform palette: the same tool class as Modeling Mode's Vertex Sculpt
			// (SkeletalMeshModelingToolsEditorMode.cpp:375-379).
			const bool bStarted = ToolManager->SelectActiveToolType(EToolSide::Left, TEXT("BeginSculptMeshTool")) && ToolManager->ActivateTool(EToolSide::Left);
			UMeshVertexSculptTool* Tool = Cast<UMeshVertexSculptTool>(ToolManager->GetActiveTool(EToolSide::Left));
			if (TestTrue(TEXT("Sculpt started in the Skeletal Mesh Editor"), bStarted) && TestNotNull(TEXT("Its sculpt tool"), Tool))
			{
				CheckPackBrushes(this, Tool);
				ToolManager->DeactivateTool(EToolSide::Left, EToolShutdownType::Cancel);
			}
		}
	}

	// The copy is thrown away. The editing tools are switched off first, so that they take their preview down while the
	// editor's scene is still there; the editor is then closed without its question about level of detail changes
	// (SkeletalMeshEditor.cpp:207-229).
	if (Instance->GetEditorName() == FName(TEXT("SkeletalMeshEditor")))
	{
		static_cast<FAssetEditorToolkit*>(Instance)->GetEditorModeManager().DeactivateMode(FEditorModeID(TEXT("SkeletalMeshModelingToolsEditorMode")));
	}
	Instance->CloseWindow(EAssetEditorCloseReason::AssetForceDeleted);
	TestNull(TEXT("The editor closed"), AssetEditors->FindEditorForAsset(Mesh, /*bFocusIfOpen*/ false));
	// Nothing keeps the copy once the test is over.
	Package->SetDirtyFlag(false);
	Mesh->ClearFlags(RF_Standalone);
	Mesh->MarkAsGarbage();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
