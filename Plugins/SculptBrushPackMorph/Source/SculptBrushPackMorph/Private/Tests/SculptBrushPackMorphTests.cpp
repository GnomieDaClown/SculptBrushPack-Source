// Copyright (C) 2026 Joseph Wiginton.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sculpt Brush Pack: Morph Targets. Tests of the brushes' rules and of their place in the Skeletal Mesh Editor.
//
// The rule tests hand each brush a few stamps on a few points, with what a brush knows of the edited morph written out by
// hand, and compare exactly with values worked out by hand: a strength of 1 or a half, a falloff of 1 everywhere, whole
// numbers and halves. No tolerance is used. The editor tests open the Skeletal Mesh Editor on a copy of the engine's
// skeletal cube with two morph targets, start Sculpt Morph Target, and make strokes through the tool.
//
// Clone Morph and Mirror Morph leave the brush palette when the plugin's console variable is switched off. A test that
// needs them in a tool sets the variable for its length, whatever it was.

#include "CoreMinimal.h"
#include "Animation/AnimCurveMetadata.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Commands/UICommandList.h"
#include "HAL/IConsoleManager.h"
#include "InteractiveToolManager.h"
#include "MeshDescription.h"
#include "MeshVertexSculptTool.h"
#include "ReferenceSkeleton.h"
#include "Misc/AutomationTest.h"
#include "SculptBrushPack.h"
#include "SculptBrushPackMorph.h"
#include "SculptBrushPackMorphBrushes.h"
#include "SculptBrushPackMorphToolAccess.h"
#include "SkeletalMeshAttributes.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Tools/UEdMode.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "VectorUtil.h"

#include <initializer_list>

#if WITH_DEV_AUTOMATION_TESTS

namespace SculptBrushPackMorphTestLocals
{
	using namespace SculptBrushPackMorph;
	using UE::Geometry::FDynamicMesh3;
	using UE::Geometry::FFrame3d;

	/** What a brush knows of the edited morph, written out by hand. */
	class FTestContext : public IMorphBrushContext
	{
	public:
		bool bEditable = true;
		bool bSymmetryOn = false;
		bool bAskedForMirror = false;
		int32 StrokesBegun = 0;
		FDynamicMesh3 Without;
		TMap<FName, TArray<FVector3d>> SourceOffsets;
		/** What the mesh's symmetry gives each vertex. Empty: nothing is known of the mesh's symmetry. */
		TArray<TPair<EMorphMirror, FVector3d>> Mirrors;
		/** The shown mesh's places with its pose taken off, by vertex. Empty: not known. */
		TArray<FVector3d> Unposed;
		/** The same without the edited morph. Empty: not known. */
		TArray<FVector3d> UnposedWithout;
		/** Places in the mesh's UV channel 0, by vertex. Empty: the mesh has no UVs. */
		TArray<FVector2d> UVs;
		/** The warning that stands for the user, empty when none does. */
		FText Problem;

		virtual bool BeginStroke(bool bMirror) override
		{
			bAskedForMirror = bMirror;
			++StrokesBegun;
			return bEditable;
		}
		virtual const FDynamicMesh3* GetMeshWithoutMorph() override { return &Without; }
		virtual bool GetSourceOffset(FName Source, int32 Vertex, FVector3d& OutOffset) const override
		{
			const TArray<FVector3d>* Offsets = SourceOffsets.Find(Source);
			if (!Offsets || !Offsets->IsValidIndex(Vertex))
			{
				return false;
			}
			OutOffset = (*Offsets)[Vertex];
			return true;
		}
		virtual EMorphMirror GetMirroredOffset(int32 Vertex, FVector3d& OutOffset) const override
		{
			if (!Mirrors.IsValidIndex(Vertex))
			{
				return EMorphMirror::None;
			}
			OutOffset = Mirrors[Vertex].Value;
			return Mirrors[Vertex].Key;
		}
		virtual bool IsToolSymmetryOn() const override { return bSymmetryOn; }
		virtual void ReportProblem(const FText& InProblem) override { Problem = InProblem; }
		virtual TArray<FName> GetSourceNames() const override
		{
			TArray<FName> Names;
			SourceOffsets.GetKeys(Names);
			return Names;
		}
		virtual bool GetUnposedPosition(int32 Vertex, bool bWithEditedMorph, FVector3d& OutPosition) const override
		{
			const TArray<FVector3d>& Places = bWithEditedMorph ? Unposed : UnposedWithout;
			if (!Places.IsValidIndex(Vertex))
			{
				return false;
			}
			OutPosition = Places[Vertex];
			return true;
		}
		virtual bool GetUV(int32 Vertex, int32 Channel, FVector2d& OutUV) const override
		{
			if (Channel != 0 || !UVs.IsValidIndex(Vertex))
			{
				return false;
			}
			OutUV = UVs[Vertex];
			return true;
		}
	};

	/** A stamp at Origin of radius 8 and strength 1. */
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

	/**
	 * A fan around vertex 0 at Center, with seven rim points that add up to zero. Averaging counts a point and its
	 * neighbors once each: eight for the middle, four for a rim point (itself, the middle and two of the rim).
	 */
	static FDynamicMesh3 MakeFan(const FVector3d& Center)
	{
		const FVector3d Rim[7] = { FVector3d(8, 0, 0), FVector3d(4, 8, 0), FVector3d(-4, 8, 0), FVector3d(-8, 4, 0), FVector3d(-8, -4, 0), FVector3d(-4, -8, 0), FVector3d(12, -8, 0) };
		FDynamicMesh3 Mesh;
		Mesh.AppendVertex(Center);
		for (const FVector3d& Point : Rim)
		{
			Mesh.AppendVertex(Point);
		}
		for (int32 Corner = 0; Corner < 7; ++Corner)
		{
			Mesh.AppendTriangle(0, 1 + (Corner + 1) % 7, 1 + Corner);
		}
		return Mesh;
	}

	/**
	 * Four triangles around vertex 0 at Center: arms reach HalfX along X and 4 along Y, in the plane Z = 0. The middle's
	 * four edges lie along the axes, so their lengths are whole numbers.
	 */
	static FDynamicMesh3 MakeCross(const FVector3d& Center, double HalfX)
	{
		const FVector3d Arms[4] = { FVector3d(HalfX, 0, 0), FVector3d(0, 4, 0), FVector3d(-HalfX, 0, 0), FVector3d(0, -4, 0) };
		FDynamicMesh3 Mesh;
		Mesh.AppendVertex(Center);
		for (const FVector3d& Arm : Arms)
		{
			Mesh.AppendVertex(Arm);
		}
		for (int32 Corner = 0; Corner < 4; ++Corner)
		{
			Mesh.AppendTriangle(0, 1 + (Corner + 1) % 4, 1 + Corner);
		}
		return Mesh;
	}

	/** A mesh's places, by vertex ID. */
	static TArray<FVector3d> PlacesOf(const FDynamicMesh3& Mesh)
	{
		TArray<FVector3d> Places;
		Places.SetNum(Mesh.MaxVertexID());
		for (const int32 Vertex : Mesh.VertexIndicesItr())
		{
			Places[Vertex] = Mesh.GetVertex(Vertex);
		}
		return Places;
	}

	/** The mesh turned a quarter turn about X, which takes (x, y, z) to (x, -z, y), and then moved by Offset. */
	static FDynamicMesh3 TurnedAndMoved(const FDynamicMesh3& Mesh, const FVector3d& Offset)
	{
		FDynamicMesh3 Result = Mesh;
		for (const int32 Vertex : Mesh.VertexIndicesItr())
		{
			const FVector3d Place = Mesh.GetVertex(Vertex);
			Result.SetVertex(Vertex, FVector3d(Place.X, -Place.Z, Place.Y) + Offset);
		}
		return Result;
	}

	static bool TestPoint(FAutomationTestBase* Test, const FString& What, const FVector3d& Actual, const FVector3d& Expected)
	{
		return Test->TestTrue(FString::Printf(TEXT("%s: (%.17g, %.17g, %.17g), expected (%.17g, %.17g, %.17g)"), *What,
			Actual.X, Actual.Y, Actual.Z, Expected.X, Expected.Y, Expected.Z), Actual == Expected);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackMorphAmplifyTest, "SculptBrushPackMorph.Brushes.AmplifyMorph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackMorphAmplifyTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackMorphTestLocals;

	// Three points: the morph raises the first by 2, lowers the second by 1 and leaves the third.
	const TSharedRef<FTestContext> Context = MakeShared<FTestContext>();
	Context->Without = MakePoints({ FVector3d(0, 0, 0), FVector3d(4, 0, 0), FVector3d(0, 4, 1) });
	const FDynamicMesh3 Shown = MakePoints({ FVector3d(0, 0, 2), FVector3d(4, 0, -1), FVector3d(0, 4, 1) });

	FAmplifyMorphBrushOp Amplify(Context);
	USculptBrushPackAmplifyMorphOpProps* Settings = Prepare<FAmplifyMorphBrushOp, USculptBrushPackAmplifyMorphOpProps>(Amplify);
	TestEqual(TEXT("Amplify Morph's Amount is a half"), Settings->Amount, 0.5f, 0.0f);
	TestEqual(TEXT("and its Strength 1"), Settings->Strength, 1.0f, 0.0f);

	// At full reach each offset is half again as large.
	const FSculptBrushStamp Stamp = MakeStamp(FVector3d::Zero());
	Amplify.BeginStroke(&Shown, Stamp, {});
	TArray<FVector3d> Moved = Apply(Amplify, Shown, Stamp);
	TestPoint(this, TEXT("A point the morph raises by 2 is raised by 3"), Moved[0], FVector3d(0, 0, 3));
	TestPoint(this, TEXT("A point it lowers by 1 is lowered by 1.5"), Moved[1], FVector3d(4, 0, -1.5));
	TestPoint(this, TEXT("A point it leaves alone stays"), Moved[2], FVector3d(0, 4, 1));
	TestPoint(this, TEXT("A second pass adds nothing"), Apply(Amplify, Shown, Stamp)[0], FVector3d(0, 0, 3));
	TestFalse(TEXT("It does not ask for the mesh's symmetry"), Context->bAskedForMirror);

	// A point keeps the most any stamp of the stroke gave it.
	FSculptBrushStamp Half = Stamp;
	Half.Power = 0.5;
	Amplify.BeginStroke(&Shown, Half, {});
	TestPoint(this, TEXT("At half strength half of it is added"), Apply(Amplify, Shown, Half)[0], FVector3d(0, 0, 2.5));
	TestPoint(this, TEXT("A full stamp adds all of it"), Apply(Amplify, Shown, Stamp)[0], FVector3d(0, 0, 3));
	TestPoint(this, TEXT("A lighter pass takes nothing back"), Apply(Amplify, Shown, Half)[0], FVector3d(0, 0, 3));

	// Inverted, the offset is made smaller, and never turned over.
	FSculptBrushStamp Inverted = Stamp;
	Inverted.Direction = -1.0;
	Amplify.BeginStroke(&Shown, Inverted, {});
	TestPoint(this, TEXT("Inverted, half the offset is taken away"), Apply(Amplify, Shown, Inverted)[0], FVector3d(0, 0, 1));
	Settings->Amount = 1.0f;
	Amplify.BeginStroke(&Shown, Inverted, {});
	TestPoint(this, TEXT("Inverted at an Amount of 1, all of it"), Apply(Amplify, Shown, Inverted)[0], FVector3d(0, 0, 0));
	Settings->Amount = 2.0f;
	Amplify.BeginStroke(&Shown, Inverted, {});
	TestPoint(this, TEXT("Inverted at an Amount of 2, still no more than all of it"), Apply(Amplify, Shown, Inverted)[0], FVector3d(0, 0, 0));

	// When the morph cannot be edited the brush moves nothing.
	Context->bEditable = false;
	Settings->Amount = 0.5f;
	Amplify.BeginStroke(&Shown, Stamp, {});
	TestPoint(this, TEXT("A morph that cannot be edited is left alone"), Apply(Amplify, Shown, Stamp)[0], FVector3d(0, 0, 2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackMorphSmoothTest, "SculptBrushPackMorph.Brushes.SmoothMorph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackMorphSmoothTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackMorphTestLocals;

	// The model has a bump of its own, 3 high, at the middle of a fan; the morph raises the middle by 8 more and leaves
	// the rim.
	const TSharedRef<FTestContext> Context = MakeShared<FTestContext>();
	Context->Without = MakeFan(FVector3d(0, 0, 3));
	const FDynamicMesh3 Shown = MakeFan(FVector3d(0, 0, 11));

	FSmoothMorphBrushOp Smooth(Context);
	USculptBrushPackSmoothMorphOpProps* Settings = Prepare<FSmoothMorphBrushOp, USculptBrushPackSmoothMorphOpProps>(Smooth);
	TestEqual(TEXT("Smooth Morph's Smoothing is 2 rounds"), Settings->Smoothing, 2);
	TestEqual(TEXT("and its Strength 1"), Settings->Strength, 1.0f, 0.0f);

	// One round: the middle's offset becomes (8 + 0) / 8 = 1 and a rim point's (0 + 8) / 4 = 2. The model's own bump is
	// not smoothed: the middle stands at 3 + 1.
	const FSculptBrushStamp Stamp = MakeStamp(FVector3d::Zero());
	Settings->Smoothing = 1;
	Smooth.BeginStroke(&Shown, Stamp, {});
	TArray<FVector3d> Moved = Apply(Smooth, Shown, Stamp);
	TestPoint(this, TEXT("One round: the middle keeps the model's bump and an offset of 1"), Moved[0], FVector3d(0, 0, 4));
	TestPoint(this, TEXT("One round: a rim point takes an offset of 2"), Moved[1], FVector3d(8, 0, 2));
	TestPoint(this, TEXT("A second pass adds nothing"), Apply(Smooth, Shown, Stamp)[0], FVector3d(0, 0, 4));

	// Two rounds: the middle's offset becomes (1 + 7 x 2) / 8 = 1.875.
	Settings->Smoothing = 2;
	Smooth.BeginStroke(&Shown, Stamp, {});
	TestPoint(this, TEXT("Two rounds: the middle's offset is 1.875"), Apply(Smooth, Shown, Stamp)[0], FVector3d(0, 0, 4.875));

	// At half strength the point goes half the way.
	Settings->Smoothing = 1;
	FSculptBrushStamp Half = Stamp;
	Half.Power = 0.5;
	Smooth.BeginStroke(&Shown, Half, {});
	TestPoint(this, TEXT("At half strength the middle goes half the way"), Apply(Smooth, Shown, Half)[0], FVector3d(0, 0, 7.5));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackMorphCloneTest, "SculptBrushPackMorph.Brushes.CloneMorph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackMorphCloneTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackMorphTestLocals;

	// Two points. The edited morph raises the first by 2; the morph Smile would move the first by 1 along X and raise the
	// second by 4.
	const FName Smile(TEXT("Smile"));
	const TSharedRef<FTestContext> Context = MakeShared<FTestContext>();
	Context->Without = MakePoints({ FVector3d(0, 0, 0), FVector3d(4, 0, 0) });
	const FName Wink(TEXT("Wink"));
	Context->SourceOffsets.Add(Smile, { FVector3d(1, 0, 0), FVector3d(0, 0, 4) });
	Context->SourceOffsets.Add(Wink, { FVector3d(0, 1, 0), FVector3d(0, 1, 0) });
	const FDynamicMesh3 Shown = MakePoints({ FVector3d(0, 0, 2), FVector3d(4, 0, 0) });

	FCloneMorphBrushOp Clone(Context);
	USculptBrushPackCloneMorphOpProps* Settings = Prepare<FCloneMorphBrushOp, USculptBrushPackCloneMorphOpProps>(Clone);
	TestEqual(TEXT("Clone Morph's Amount is 1"), Settings->Amount, 1.0f, 0.0f);
	TestEqual(TEXT("Its Falloff is the engine's Move brush's 0.5: the inner half of the brush copies fully"), Settings->Falloff, 0.5f, 0.0f);
	TestFalse(TEXT("It replaces the shape unless told to add"), Settings->bAdd);
	TestTrue(TEXT("It has no source until one is chosen"), Settings->Source.IsNone());
	Settings->SourceNames = [Context]() { return Context->GetSourceNames(); };
	TestTrue(TEXT("Its Source lists the other morph targets"), Settings->GetSourceOptions() == TArray<FName>({ Smile, Wink }));

	const FSculptBrushStamp Stamp = MakeStamp(FVector3d::Zero());
	Clone.BeginStroke(&Shown, Stamp, {});
	TestPoint(this, TEXT("Without a source nothing moves"), Apply(Clone, Shown, Stamp)[0], FVector3d(0, 0, 2));
	TestTrue(TEXT("and the user is told to choose one"), Context->Problem.ToString().Contains(TEXT("Source")));

	// At full reach the edited morph has the source's shape in place of its own.
	Settings->Source = Smile;
	Clone.BeginStroke(&Shown, Stamp, {});
	TestTrue(TEXT("With a source chosen the warning is taken back"), Context->Problem.IsEmpty());
	TArray<FVector3d> Moved = Apply(Clone, Shown, Stamp);
	TestPoint(this, TEXT("The first point takes Smile's offset in place of its own"), Moved[0], FVector3d(1, 0, 0));
	TestPoint(this, TEXT("The second takes Smile's offset"), Moved[1], FVector3d(4, 0, 4));

	FSculptBrushStamp Half = Stamp;
	Half.Power = 0.5;
	Clone.BeginStroke(&Shown, Half, {});
	TestPoint(this, TEXT("At half strength the point goes half the way"), Apply(Clone, Shown, Half)[0], FVector3d(0.5, 0, 1));

	Settings->Amount = 0.5f;
	Clone.BeginStroke(&Shown, Stamp, {});
	TestPoint(this, TEXT("At an Amount of a half, half of Smile's offset"), Apply(Clone, Shown, Stamp)[0], FVector3d(0.5, 0, 0));
	Settings->Amount = 1.0f;

	// Added to the shape, and taken out of it.
	Settings->bAdd = true;
	Clone.BeginStroke(&Shown, Stamp, {});
	TestPoint(this, TEXT("Added, Smile's offset comes on top of the point's own"), Apply(Clone, Shown, Stamp)[0], FVector3d(1, 0, 2));
	Settings->bAdd = false;
	FSculptBrushStamp Inverted = Stamp;
	Inverted.Direction = -1.0;
	Clone.BeginStroke(&Shown, Inverted, {});
	TestPoint(this, TEXT("Inverted, Smile's offset is taken out of the point's own"), Apply(Clone, Shown, Inverted)[0], FVector3d(-1, 0, 2));

	// A source the mesh does not have.
	Settings->Source = FName(TEXT("Frown"));
	Clone.BeginStroke(&Shown, Stamp, {});
	TestPoint(this, TEXT("A source that is not there moves nothing"), Apply(Clone, Shown, Stamp)[0], FVector3d(0, 0, 2));
	TestTrue(TEXT("and the user is told which name the mesh does not have"), Context->Problem.ToString().Contains(TEXT("Frown")));

	// A mesh with no other morph target to copy from.
	Context->SourceOffsets.Reset();
	Clone.BeginStroke(&Shown, Stamp, {});
	TestTrue(TEXT("With no other morph target the user is told that there is none"), Context->Problem.ToString().Contains(TEXT("no other")));

	// With the tool's Symmetry on the brush works, and says what Symmetry does to the result.
	Context->SourceOffsets.Add(Smile, { FVector3d(1, 0, 0), FVector3d(0, 0, 4) });
	Context->SourceOffsets.Add(Wink, { FVector3d(0, 1, 0), FVector3d(0, 1, 0) });
	Settings->Source = Smile;
	Context->bSymmetryOn = true;
	Clone.BeginStroke(&Shown, Stamp, {});
	TestPoint(this, TEXT("With Symmetry on the brush still clones"), Apply(Clone, Shown, Stamp)[0], FVector3d(1, 0, 0));
	TestTrue(TEXT("and the user is told what Symmetry does"), Context->Problem.ToString().Contains(TEXT("Symmetry")));
	Context->bSymmetryOn = false;

	// With exactly one other morph target there is nothing to choose: it is the source.
	Context->SourceOffsets.Remove(Wink);
	Settings->Source = NAME_None;
	Clone.BeginStroke(&Shown, Stamp, {});
	TestTrue(TEXT("The only other morph target becomes the source"), Settings->Source == Smile);
	TestTrue(TEXT("and nothing is reported"), Context->Problem.IsEmpty());
	Context->SourceOffsets.Reset();

	// A morph that cannot be edited: the context has said why, and the brush adds nothing to it.
	Context->bEditable = false;
	Context->Problem = FText::FromString(TEXT("the context's own reason"));
	Clone.BeginStroke(&Shown, Stamp, {});
	TestEqual(TEXT("The brush leaves the context's reason standing"), Context->Problem.ToString(), FString(TEXT("the context's own reason")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackMorphMirrorTest, "SculptBrushPackMorph.Brushes.MirrorMorph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackMorphMirrorTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackMorphTestLocals;

	// A point on each side of the plane X = 0, one on it and one that has no partner. The morph raises the +X one by 1
	// and leaves the -X one; it moves the one on the plane by 1 along X and 3 up, and the lone one by 1 up. Mirrored, the
	// -X one is raised by 1, the +X one is left, and the one on the plane keeps what lies in the plane: 3 up.
	const TSharedRef<FTestContext> Context = MakeShared<FTestContext>();
	Context->Without = MakePoints({ FVector3d(2, 0, 0), FVector3d(-2, 0, 0), FVector3d(0, 0, 0), FVector3d(5, 5, 5) });
	Context->Mirrors = {
		{ EMorphMirror::PositiveX, FVector3d(0, 0, 0) },
		{ EMorphMirror::NegativeX, FVector3d(0, 0, 1) },
		{ EMorphMirror::OnPlane, FVector3d(0, 0, 3) },
		{ EMorphMirror::None, FVector3d::ZeroVector } };
	const FDynamicMesh3 Shown = MakePoints({ FVector3d(2, 0, 1), FVector3d(-2, 0, 0), FVector3d(1, 0, 3), FVector3d(5, 5, 6) });

	FMirrorMorphBrushOp Mirror(Context);
	USculptBrushPackMirrorMorphOpProps* Settings = Prepare<FMirrorMorphBrushOp, USculptBrushPackMirrorMorphOpProps>(Mirror);
	TestEqual(TEXT("Mirror Morph's Strength is 1"), Settings->Strength, 1.0f, 0.0f);
	TestEqual(TEXT("Its Falloff is the engine's Move brush's 0.5"), Settings->Falloff, 0.5f, 0.0f);
	TestTrue(TEXT("It copies from the other side of each brushed point"), Settings->CopyFrom == ESculptBrushPackMirrorSource::OtherSide);

	// Other Side: every brushed point takes the shape of the point across from it, so brushing both sides swaps them.
	const FSculptBrushStamp Stamp = MakeStamp(FVector3d::Zero());
	Mirror.BeginStroke(&Shown, Stamp, {});
	TestTrue(TEXT("It asks for the mesh's symmetry"), Context->bAskedForMirror);
	TArray<FVector3d> Moved = Apply(Mirror, Shown, Stamp);
	TestPoint(this, TEXT("The -X point takes the +X one's offset, mirrored"), Moved[1], FVector3d(-2, 0, 1));
	TestPoint(this, TEXT("The +X point takes the -X one's: none"), Moved[0], FVector3d(2, 0, 0));
	TestPoint(this, TEXT("The point on the plane keeps what lies in the plane"), Moved[2], FVector3d(0, 0, 3));
	TestPoint(this, TEXT("The point without a partner stays"), Moved[3], FVector3d(5, 5, 6));
	TestTrue(TEXT("Nothing is reported when points were mirrored"), Context->Problem.IsEmpty());

	FSculptBrushStamp Half = Stamp;
	Half.Power = 0.5;
	Mirror.BeginStroke(&Shown, Half, {});
	TestPoint(this, TEXT("At half strength the -X point goes half the way"), Apply(Mirror, Shown, Half)[1], FVector3d(-2, 0, 0.5));

	// +X Side: only the -X side changes, so the side that is copied from cannot be brushed away.
	Settings->CopyFrom = ESculptBrushPackMirrorSource::PositiveX;
	Mirror.BeginStroke(&Shown, Stamp, {});
	Moved = Apply(Mirror, Shown, Stamp);
	TestPoint(this, TEXT("From +X: the -X point takes the +X one's offset"), Moved[1], FVector3d(-2, 0, 1));
	TestPoint(this, TEXT("From +X: the +X point keeps its own"), Moved[0], FVector3d(2, 0, 1));
	TestPoint(this, TEXT("From +X: the point on the plane keeps what lies in the plane"), Moved[2], FVector3d(0, 0, 3));

	// -X Side: only the +X side changes.
	Settings->CopyFrom = ESculptBrushPackMirrorSource::NegativeX;
	Mirror.BeginStroke(&Shown, Stamp, {});
	Moved = Apply(Mirror, Shown, Stamp);
	TestPoint(this, TEXT("From -X: the +X point takes the -X one's offset: none"), Moved[0], FVector3d(2, 0, 0));
	TestPoint(this, TEXT("From -X: the -X point keeps its own"), Moved[1], FVector3d(-2, 0, 0));

	// A mesh whose symmetry gives the brushed points nothing: nothing moves, and the user is told.
	Settings->CopyFrom = ESculptBrushPackMirrorSource::OtherSide;
	Context->Mirrors.Reset();
	Mirror.BeginStroke(&Shown, Stamp, {});
	TestPoint(this, TEXT("Without partners nothing moves"), Apply(Mirror, Shown, Stamp)[1], FVector3d(-2, 0, 0));
	TestTrue(TEXT("and the user is told that the points have no partner"), Context->Problem.ToString().Contains(TEXT("no partner")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackMorphMushTest, "SculptBrushPackMorph.Brushes.MushMorph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackMorphMushTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackMorphTestLocals;

	// With its pose taken off, the model has a bump 8 high at the middle of a fan. One round of averaging leaves the
	// middle at 8 / 8 = 1 and every rim point at 8 / 4 = 2, so the smoothed surface's normal at the middle points
	// straight up, and the bump's detail is 8 - 1 = 7 along it. The direction across the surface looks toward rim point
	// 1, which one round leaves at (6, 0, 2): along X.
	const FDynamicMesh3 Rest = MakeFan(FVector3d(0, 0, 8));
	const TSharedRef<FTestContext> Context = MakeShared<FTestContext>();
	Context->Without = Rest;
	Context->Unposed = PlacesOf(Rest);

	FMushMorphBrushOp Mush(Context);
	USculptBrushPackMushMorphOpProps* Settings = Prepare<FMushMorphBrushOp, USculptBrushPackMushMorphOpProps>(Mush);
	TestEqual(TEXT("Mush Morph's Smoothing is 4 rounds"), Settings->Smoothing, 4);
	TestEqual(TEXT("and its Strength 1"), Settings->Strength, 1.0f, 0.0f);
	Settings->Smoothing = 1;
	const FSculptBrushStamp Stamp = MakeStamp(FVector3d::Zero());

	// A pose that only turns and moves the mesh leaves nothing to mend: the smoothed posed middle is the smoothed
	// unposed middle turned and moved, (0, -1, 0) + (1, 2, 3), its normal is the unposed normal turned, (0, -1, 0), and
	// the detail of 7 along it gives back the posed middle, (1, -6, 3).
	const FDynamicMesh3 OnlyMoved = TurnedAndMoved(Rest, FVector3d(1, 2, 3));
	Mush.BeginStroke(&OnlyMoved, Stamp, {});
	TestPoint(this, TEXT("A mesh that is only turned and moved: the middle stays"), Apply(Mush, OnlyMoved, Stamp)[0], FVector3d(1, -6, 3));

	// A pose that flattens the bump: the shown mesh is the fan with its middle at 0. The smoothed posed surface is the
	// flat fan, and the detail of 7 is put back along its normal.
	const FDynamicMesh3 Flattened = MakeFan(FVector3d(0, 0, 0));
	Mush.BeginStroke(&Flattened, Stamp, {});
	TestPoint(this, TEXT("A flattened bump: the middle gets its detail of 7 back"), Apply(Mush, Flattened, Stamp)[0], FVector3d(0, 0, 7));
	TestPoint(this, TEXT("A second pass adds nothing"), Apply(Mush, Flattened, Stamp)[0], FVector3d(0, 0, 7));
	TestTrue(TEXT("and nothing is reported"), Context->Problem.IsEmpty());

	// Two rounds leave the unposed middle at (1 + 7 x 2) / 8 = 1.875, so the detail is 8 - 1.875 = 6.125.
	Settings->Smoothing = 2;
	Mush.BeginStroke(&Flattened, Stamp, {});
	TestPoint(this, TEXT("Two rounds: the detail put back is 6.125"), Apply(Mush, Flattened, Stamp)[0], FVector3d(0, 0, 6.125));

	// At half strength the point goes half the way.
	Settings->Smoothing = 1;
	FSculptBrushStamp Half = Stamp;
	Half.Power = 0.5;
	Mush.BeginStroke(&Flattened, Half, {});
	TestPoint(this, TEXT("At half strength the middle goes half the way"), Apply(Mush, Flattened, Half)[0], FVector3d(0, 0, 3.5));

	// Without the mesh's places in its reference pose nothing can be measured: nothing moves, and the user is told.
	Context->Unposed.Reset();
	Mush.BeginStroke(&Flattened, Stamp, {});
	TestPoint(this, TEXT("Without the reference pose the middle stays"), Apply(Mush, Flattened, Stamp)[0], FVector3d(0, 0, 0));
	TestTrue(TEXT("and the brush says it could not measure the surface"), Context->Problem.ToString().Contains(TEXT("could not measure")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackMorphWrinkleTest, "SculptBrushPackMorph.Brushes.WrinkleMorph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackMorphWrinkleTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackMorphTestLocals;

	// At rest the middle's four edges are 4 long each: 16 in all. The pose pulls the two arms along X in to 3: 14 in all,
	// which is an eighth shorter. The edited morph has raised the middle by a half.
	const TSharedRef<FTestContext> Context = MakeShared<FTestContext>();
	Context->Without = MakeCross(FVector3d(0, 0, 0), 3);
	Context->UnposedWithout = PlacesOf(MakeCross(FVector3d(0, 0, 0), 4));
	const FDynamicMesh3 Shown = MakeCross(FVector3d(0, 0, 0.5), 3);

	FWrinkleMorphBrushOp Wrinkle(Context);
	USculptBrushPackWrinkleMorphOpProps* Settings = Prepare<FWrinkleMorphBrushOp, USculptBrushPackWrinkleMorphOpProps>(Wrinkle);
	TestEqual(TEXT("Wrinkle Morph's Height is 0.5"), Settings->Height, 0.5f, 0.0f);
	TestTrue(TEXT("it raises only where the surface is compressed"), Settings->bOnlyWhereCompressed);
	TestEqual(TEXT("from 0.02 shorter"), Settings->StartsAt, 0.02f, 0.0f);
	TestEqual(TEXT("fully at 0.25 shorter"), Settings->FullAt, 0.25f, 0.0f);
	TestNull(TEXT("it has no Wrinkle Map"), Settings->WrinkleMap.Get());
	TestEqual(TEXT("and its Strength is 1"), Settings->Strength, 1.0f, 0.0f);

	// An eighth shorter, with the wrinkle whole at a quarter: half of a Height of 2, along the normal, which is straight
	// up, from the middle's place before the stroke.
	const FSculptBrushStamp Stamp = MakeStamp(FVector3d::Zero());
	Settings->Height = 2.0f;
	Settings->StartsAt = 0.0f;
	Settings->FullAt = 0.25f;
	Wrinkle.BeginStroke(&Shown, Stamp, {});
	TestPoint(this, TEXT("An eighth shorter, whole at a quarter: the middle rises by half the Height"), Apply(Wrinkle, Shown, Stamp)[0], FVector3d(0, 0, 1.5));
	TestPoint(this, TEXT("A second pass adds nothing"), Apply(Wrinkle, Shown, Stamp)[0], FVector3d(0, 0, 1.5));
	Wrinkle.EndStroke(&Shown, Stamp, {});
	TestTrue(TEXT("and nothing is reported"), Context->Problem.IsEmpty());

	// Starting at a sixteenth and whole at three sixteenths, an eighth is half the way.
	Settings->StartsAt = 0.0625f;
	Settings->FullAt = 0.1875f;
	Wrinkle.BeginStroke(&Shown, Stamp, {});
	TestPoint(this, TEXT("Half the way from Starts At to Full At: half the Height"), Apply(Wrinkle, Shown, Stamp)[0], FVector3d(0, 0, 1.5));

	// Nothing rises until the surface is more than Starts At shorter.
	Settings->StartsAt = 0.125f;
	Settings->FullAt = 0.25f;
	Wrinkle.BeginStroke(&Shown, Stamp, {});
	TestPoint(this, TEXT("At Starts At exactly: the middle stays"), Apply(Wrinkle, Shown, Stamp)[0], FVector3d(0, 0, 0.5));

	// With Ctrl the wrinkle is cut in; at half strength the point goes half the way.
	Settings->StartsAt = 0.0f;
	FSculptBrushStamp Inverted = Stamp;
	Inverted.Direction = -1.0;
	Wrinkle.BeginStroke(&Shown, Inverted, {});
	TestPoint(this, TEXT("With Ctrl the middle is cut in by half the Height"), Apply(Wrinkle, Shown, Inverted)[0], FVector3d(0, 0, -0.5));
	FSculptBrushStamp Half = Stamp;
	Half.Power = 0.5;
	Wrinkle.BeginStroke(&Shown, Half, {});
	TestPoint(this, TEXT("At half strength the middle goes half the way"), Apply(Wrinkle, Shown, Half)[0], FVector3d(0, 0, 1.0));

	// The brush's alpha shapes the wrinkle through each point's reach: an alpha of a quarter gives a quarter of the rise.
	FSculptBrushStamp WithAlpha = Stamp;
	WithAlpha.StampAlphaFunc = [](const FSculptBrushStamp&, const FVector3d&) { return 0.25; };
	Wrinkle.BeginStroke(&Shown, WithAlpha, {});
	TestPoint(this, TEXT("An alpha of a quarter: a quarter of the rise"), Apply(Wrinkle, Shown, WithAlpha)[0], FVector3d(0, 0, 0.75));

	// A mesh that stands as at rest is nowhere compressed: nothing rises, and when the stroke ends the user is told.
	const TSharedRef<FTestContext> AtRest = MakeShared<FTestContext>();
	AtRest->Without = MakeCross(FVector3d(0, 0, 0), 4);
	AtRest->UnposedWithout = PlacesOf(AtRest->Without);
	const FDynamicMesh3 ShownAtRest = MakeCross(FVector3d(0, 0, 0.5), 4);
	FWrinkleMorphBrushOp WrinkleAtRest(AtRest);
	USculptBrushPackWrinkleMorphOpProps* AtRestSettings = Prepare<FWrinkleMorphBrushOp, USculptBrushPackWrinkleMorphOpProps>(WrinkleAtRest);
	AtRestSettings->Height = 2.0f;
	WrinkleAtRest.BeginStroke(&ShownAtRest, Stamp, {});
	TestPoint(this, TEXT("At rest nothing is compressed: the middle stays"), Apply(WrinkleAtRest, ShownAtRest, Stamp)[0], FVector3d(0, 0, 0.5));
	TestTrue(TEXT("Nothing is said while the stroke is under way"), AtRest->Problem.IsEmpty());
	WrinkleAtRest.EndStroke(&ShownAtRest, Stamp, {});
	TestTrue(TEXT("When it ends the brush says nothing was compressed"), AtRest->Problem.ToString().Contains(TEXT("Only Where Compressed")));

	// With Only Where Compressed off the wrinkle rises everywhere, by the whole Height.
	AtRestSettings->bOnlyWhereCompressed = false;
	WrinkleAtRest.BeginStroke(&ShownAtRest, Stamp, {});
	TestTrue(TEXT("A stroke that can act takes the report back"), AtRest->Problem.IsEmpty());
	TestPoint(this, TEXT("Only Where Compressed off: the middle rises by the Height"), Apply(WrinkleAtRest, ShownAtRest, Stamp)[0], FVector3d(0, 0, 2.5));
	WrinkleAtRest.EndStroke(&ShownAtRest, Stamp, {});
	TestTrue(TEXT("and nothing is reported"), AtRest->Problem.IsEmpty());

	// A Wrinkle Map of two by two texels, read at each point's place in the UVs. The test gives the map itself; the
	// texture only says that there is one.
	//   top row:    0.25  0.5
	//   bottom row: 0.75  1
	int32 TimesRead = 0;
	WrinkleAtRest.ReadMap = [&TimesRead](UTexture2D*, FWrinkleMorphBrushOp::FMapImage& OutMap)
	{
		++TimesRead;
		OutMap.SetDimensions(UE::Geometry::FImageDimensions(2, 2));
		OutMap.SetPixel(0, 0, 0.25f);
		OutMap.SetPixel(1, 0, 0.5f);
		OutMap.SetPixel(0, 1, 0.75f);
		OutMap.SetPixel(1, 1, 1.0f);
		return true;
	};
	AtRestSettings->WrinkleMap = NewObject<UTexture2D>(GetTransientPackage());
	// The middle sits at the center of the top right texel; the arms at the center of the top left.
	AtRest->UVs.Init(FVector2d(0.25, 0.25), 5);
	AtRest->UVs[0] = FVector2d(0.75, 0.25);
	WrinkleAtRest.BeginStroke(&ShownAtRest, Stamp, {});
	TArray<FVector3d> Mapped = Apply(WrinkleAtRest, ShownAtRest, Stamp);
	TestPoint(this, TEXT("A map value of a half: the middle rises by half the Height"), Mapped[0], FVector3d(0, 0, 1.5));
	TestPoint(this, TEXT("A map value of a quarter: an arm rises by a quarter of the Height"), Mapped[1], FVector3d(4, 0, 0.5));

	// A map painted around mid grey: a half leaves the point, 1 raises it by the Height.
	AtRestSettings->bMidGreyIsFlat = true;
	WrinkleAtRest.BeginStroke(&ShownAtRest, Stamp, {});
	TestPoint(this, TEXT("Mid Grey Is Flat: a half leaves the middle"), Apply(WrinkleAtRest, ShownAtRest, Stamp)[0], FVector3d(0, 0, 0.5));
	AtRest->UVs[0] = FVector2d(0.75, 0.75);
	WrinkleAtRest.BeginStroke(&ShownAtRest, Stamp, {});
	TestPoint(this, TEXT("Mid Grey Is Flat: white raises the middle by the Height"), Apply(WrinkleAtRest, ShownAtRest, Stamp)[0], FVector3d(0, 0, 2.5));

	// A tiling of 2 reads the map twice as fast: (0.375, 0.125) falls where (0.75, 0.25) fell.
	AtRestSettings->bMidGreyIsFlat = false;
	AtRestSettings->MapTiling = 2.0f;
	AtRest->UVs[0] = FVector2d(0.375, 0.125);
	WrinkleAtRest.BeginStroke(&ShownAtRest, Stamp, {});
	TestPoint(this, TEXT("Map Tiling 2: the middle reads the top right texel"), Apply(WrinkleAtRest, ShownAtRest, Stamp)[0], FVector3d(0, 0, 1.5));
	TestEqual(TEXT("The texture was read once for all these strokes"), TimesRead, 1);

	// A mesh without UVs cannot place the map: nothing rises, and the user is told.
	AtRest->UVs.Reset();
	WrinkleAtRest.BeginStroke(&ShownAtRest, Stamp, {});
	TestPoint(this, TEXT("Without UVs the middle stays"), Apply(WrinkleAtRest, ShownAtRest, Stamp)[0], FVector3d(0, 0, 0.5));
	TestTrue(TEXT("and the brush says the mesh has no such UV channel"), AtRest->Problem.ToString().Contains(TEXT("UV channel")));

	// A texture that cannot be read: nothing rises, and the user is told when the stroke begins.
	const TSharedRef<FTestContext> Unreadable = MakeShared<FTestContext>();
	Unreadable->Without = AtRest->Without;
	Unreadable->UnposedWithout = AtRest->UnposedWithout;
	Unreadable->UVs.Init(FVector2d(0.25, 0.25), 5);
	FWrinkleMorphBrushOp WrinkleUnreadable(Unreadable);
	USculptBrushPackWrinkleMorphOpProps* UnreadableSettings = Prepare<FWrinkleMorphBrushOp, USculptBrushPackWrinkleMorphOpProps>(WrinkleUnreadable);
	UnreadableSettings->bOnlyWhereCompressed = false;
	UnreadableSettings->WrinkleMap = NewObject<UTexture2D>(GetTransientPackage());
	WrinkleUnreadable.ReadMap = [](UTexture2D*, FWrinkleMorphBrushOp::FMapImage&) { return false; };
	WrinkleUnreadable.BeginStroke(&ShownAtRest, Stamp, {});
	TestTrue(TEXT("A texture that cannot be read is reported as the stroke begins"), Unreadable->Problem.ToString().Contains(TEXT("could not read")));
	TestPoint(this, TEXT("and the middle stays"), Apply(WrinkleUnreadable, ShownAtRest, Stamp)[0], FVector3d(0, 0, 0.5));
	return true;
}

namespace SculptBrushPackMorphTestLocals
{
	const FName LiftName(TEXT("PackLift"));
	const FName ShiftName(TEXT("PackShift"));

	/**
	 * Sets, for a test's length, whether Sculpt Morph Target tools that start get Clone Morph and Mirror Morph: the
	 * plugin's console variable SculptBrushPackMorph.CloneAndMirror. The variable is put back when the test ends.
	 */
	class FScopedCloneAndMirror
	{
	public:
		explicit FScopedCloneAndMirror(bool bOn)
			: Variable(IConsoleManager::Get().FindConsoleVariable(TEXT("SculptBrushPackMorph.CloneAndMirror")))
			, bBefore(Variable && Variable->GetBool())
		{
			Set(bOn);
		}

		~FScopedCloneAndMirror()
		{
			Set(bBefore);
		}

		UE_NONCOPYABLE(FScopedCloneAndMirror)

		bool IsFound() const { return Variable != nullptr; }
		bool IsOnByDefault() const { return Variable && Variable->GetDefaultValue().ToBool(); }

		void Set(bool bOn)
		{
			if (Variable)
			{
				// Whoever set the variable last keeps that standing; the test changes its value only.
				Variable->SetWithCurrentPriority(bOn);
			}
		}

	private:
		IConsoleVariable* Variable;
		bool bBefore;
	};

	/**
	 * The Skeletal Mesh Editor open on a copy of the engine's skeletal cube that has two morph targets, PackLift and
	 * PackShift, with Sculpt Morph Target started on PackLift at full weight. Nothing is saved.
	 */
	struct FMorphToolSession
	{
		USkeletalMesh* Mesh = nullptr;
		UPackage* Package = nullptr;
		IAssetEditorInstance* Editor = nullptr;
		UInteractiveToolManager* ToolManager = nullptr;
		UMeshVertexSculptTool* Tool = nullptr;
		/** The editor's preview component, whose animation instance gives the tools their morph weights and whose bones give them the pose. */
		USkeletalMeshComponent* Preview = nullptr;
	};

	const TCHAR* const SkeletalCubePath = TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube");
	/** A character from the engine's tutorial content: stiff pieces, one to a bone, each a joined surface of its own. */
	const TCHAR* const CharacterPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP");

	/**
	 * LiftAt gives PackLift's delta for a point of the cube in its reference pose; PackShift moves every point 2 along X.
	 * False when the tool could not be started; what was opened is closed by CloseSession either way.
	 */
	static bool OpenSession(FAutomationTestBase& Test, const TCHAR* PackageName, TFunctionRef<FVector3f(const FVector3f&)> LiftAt, FMorphToolSession& Session, const TCHAR* SourcePath = SkeletalCubePath)
	{
		// A copy of the engine's mesh in a package of its own: the engine's asset is not opened for editing.
		USkeletalMesh* Source = LoadObject<USkeletalMesh>(nullptr, SourcePath);
		if (!Test.TestNotNull(TEXT("The engine's skeletal mesh"), Source))
		{
			return false;
		}
		Session.Package = CreatePackage(*(FString(PackageName) + TEXT("_") + FGuid::NewGuid().ToString()));
		Session.Mesh = DuplicateObject<USkeletalMesh>(Source, Session.Package, TEXT("SkeletalCubeCopy"));
		FMeshDescription* Description = Session.Mesh ? Session.Mesh->GetMeshDescription(0) : nullptr;
		if (!Test.TestNotNull(TEXT("A copy of the skeletal cube with a mesh description"), Description))
		{
			return false;
		}
		Session.Mesh->ModifyMeshDescription(0, /*bInAlwaysMarkPackageDirty*/ false);
		{
			FSkeletalMeshAttributes Attributes(*Description);
			const bool bRegistered = Attributes.RegisterMorphTargetAttribute(LiftName, /*bIncludeNormals*/ false) && Attributes.RegisterMorphTargetAttribute(ShiftName, /*bIncludeNormals*/ false);
			if (!Test.TestTrue(TEXT("Two morph targets are added to the copy"), bRegistered))
			{
				return false;
			}
			TVertexAttributesRef<FVector3f> LiftDeltas = Attributes.GetVertexMorphPositionDelta(LiftName);
			TVertexAttributesRef<FVector3f> ShiftDeltas = Attributes.GetVertexMorphPositionDelta(ShiftName);
			for (const FVertexID Vertex : Description->Vertices().GetElementIDs())
			{
				LiftDeltas.Set(Vertex, LiftAt(Description->GetVertexPosition(Vertex)));
				ShiftDeltas.Set(Vertex, FVector3f(2.0f, 0.0f, 0.0f));
			}
		}
		Session.Mesh->CommitMeshDescription(0);
		// The animation system gives a curve to a morph target only when the curve is marked as a morph target's
		// (AnimCurveMetadata.h:162-176 in 5.8.4); an importer marks it, so the test does.
		UAnimCurveMetaData* Curves = Session.Mesh->GetAssetUserData<UAnimCurveMetaData>();
		if (!Curves)
		{
			Curves = NewObject<UAnimCurveMetaData>(Session.Mesh, NAME_None, RF_Transactional);
			Session.Mesh->AddAssetUserData(Curves);
		}
		for (const FName& Morph : { LiftName, ShiftName })
		{
			Curves->AddCurveMetaData(Morph, /*bInTransact*/ false);
			Curves->SetCurveMetaDataMorphTarget(Morph, true);
		}
		Session.Mesh->PostEditChange();

		UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		const bool bOpened = AssetEditors->OpenEditorForAsset(Session.Mesh);
		Session.Editor = bOpened ? AssetEditors->FindEditorForAsset(Session.Mesh, /*bFocusIfOpen*/ false) : nullptr;
		if (!Test.TestNotNull(TEXT("The Skeletal Mesh Editor opened on the copy"), Session.Editor)
			|| !Test.TestEqual(TEXT("The editor that opened is the Skeletal Mesh Editor"), Session.Editor->GetEditorName(), FName(TEXT("SkeletalMeshEditor"))))
		{
			return false;
		}
		// The editor's editing tools are a mode of their own. The editor switches it on as it opens unless the user
		// switched it off last time (SkeletalMeshModelingToolsModule.cpp:196-205); otherwise this presses the toolbar's
		// Editing Tools button (SkeletalMeshModelingToolsCommands.cpp:21).
		FAssetEditorToolkit* Toolkit = static_cast<FAssetEditorToolkit*>(Session.Editor);
		if (!Toolkit->GetEditorModeManager().IsModeActive(FEditorModeID(TEXT("SkeletalMeshModelingToolsEditorMode"))))
		{
			const TSharedPtr<FUICommandInfo> Toggle = FInputBindingManager::Get().FindCommandInContext(TEXT("SkeletalMeshModelingTools"), TEXT("ToggleEditingToolsMode"));
			if (Toggle.IsValid())
			{
				Toolkit->GetToolkitCommands()->TryExecuteAction(Toggle.ToSharedRef());
			}
		}
		UEdMode* Mode = Toolkit->GetEditorModeManager().GetActiveScriptableMode(FEditorModeID(TEXT("SkeletalMeshModelingToolsEditorMode")));
		Session.ToolManager = Mode ? Mode->GetToolManager() : nullptr;
		if (!Test.TestNotNull(TEXT("The editing tools' tool manager"), Session.ToolManager))
		{
			return false;
		}

		// The morph target to edit is picked with a radio button in the Morph Targets tab; the test sets what the button
		// sets (SkeletalMeshModelingToolsEditorMode.h:239-240). The tools read weights from the animation instance of the
		// editor's preview component (SkeletalMeshEditingCache.cpp:609-638, :853-857), so the morph gets its full weight
		// there, as a preview curve.
		FNameProperty* Editing = FindFProperty<FNameProperty>(Mode->GetClass(), TEXT("EditingMorphTarget"));
		if (!Test.TestNotNull(TEXT("The editing tools' EditingMorphTarget property"), Editing))
		{
			return false;
		}
		Editing->SetPropertyValue_InContainer(Mode, LiftName);
		for (TObjectIterator<USkeletalMeshComponent> It; It; ++It)
		{
			UAnimSingleNodeInstance* Preview = It->GetSkeletalMeshAsset() == Session.Mesh ? Cast<UAnimSingleNodeInstance>(It->GetAnimInstance()) : nullptr;
			if (Preview)
			{
				Preview->SetPreviewCurveOverride(LiftName, 1.0f, /*bRemoveIfZero*/ false);
				Session.Preview = *It;
				It->TickAnimation(0.0f, /*bNeedsValidRootMotion*/ false);
				It->RefreshBoneTransforms();
			}
		}

		// The pack starts listening to a new tool manager on the frame after it was made; a test that starts a tool at
		// once asks it to look now.
		SculptBrushPack::WatchToolManagers();
		const bool bStarted = Session.ToolManager->SelectActiveToolType(EToolSide::Left, TEXT("Sculpt Morph Target")) && Session.ToolManager->ActivateTool(EToolSide::Left);
		Session.Tool = Cast<UMeshVertexSculptTool>(Session.ToolManager->GetActiveTool(EToolSide::Left));
		return Test.TestTrue(TEXT("Sculpt Morph Target started in the Skeletal Mesh Editor"), bStarted) && Test.TestNotNull(TEXT("Its sculpt tool"), Session.Tool);
	}

	static void CloseSession(FAutomationTestBase& Test, FMorphToolSession& Session)
	{
		if (Session.Tool && Session.ToolManager)
		{
			Session.Tool->SetActiveBrushType(static_cast<int32>(EMeshVertexSculptBrushType::Move));
			Session.ToolManager->DeactivateTool(EToolSide::Left, EToolShutdownType::Cancel);
		}
		if (Session.Editor)
		{
			// The copy is thrown away. The editing tools are switched off first, so that they take their preview down
			// while the editor's scene is still there: closed with its tools on, the editor itself logs two warnings
			// ("World has no context") as its scene goes. The editor is then closed without its question about level of
			// detail changes (SkeletalMeshEditor.cpp:207-229).
			if (Session.Editor->GetEditorName() == FName(TEXT("SkeletalMeshEditor")))
			{
				static_cast<FAssetEditorToolkit*>(Session.Editor)->GetEditorModeManager().DeactivateMode(FEditorModeID(TEXT("SkeletalMeshModelingToolsEditorMode")));
			}
			Session.Editor->CloseWindow(EAssetEditorCloseReason::AssetForceDeleted);
			Test.TestNull(TEXT("The editor closed"), GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->FindEditorForAsset(Session.Mesh, /*bFocusIfOpen*/ false));
		}
		// Nothing keeps the copy once the test is over.
		if (Session.Package)
		{
			Session.Package->SetDirtyFlag(false);
		}
		if (Session.Mesh)
		{
			Session.Mesh->ClearFlags(RF_Standalone);
			Session.Mesh->MarkAsGarbage();
		}
		Session = FMorphToolSession();
	}

	/**
	 * One stroke that stays in one place: the ray comes straight down (or up, for a point of the underside) onto Point of
	 * the shown mesh. The tool applies a stroke's stamps on its ticks (MeshSculptToolBase.cpp:647-692).
	 */
	static bool StrokeAlong(UMeshVertexSculptTool& Tool, const FRay& Ray);

	static bool Stroke(UMeshVertexSculptTool& Tool, const FVector3d& Point, bool bFromAbove)
	{
		const FVector Direction = bFromAbove ? FVector(0, 0, -1) : FVector(0, 0, 1);
		return StrokeAlong(Tool, FRay(FVector(Point) - Direction * 1000.0, Direction));
	}

	/** One stroke that stays in one place, where Ray meets the shown mesh. */
	static bool StrokeAlong(UMeshVertexSculptTool& Tool, const FRay& Ray)
	{
		FHitResult Hit;
		if (!Tool.HitTest(Ray, Hit))
		{
			return false;
		}
		Tool.OnBeginDrag(Ray);
		Tool.Tick(0.03f);
		Tool.OnUpdateDrag(Ray);
		Tool.Tick(0.03f);
		Tool.OnEndDrag(Ray);
		Tool.Tick(0.03f);
		return true;
	}

	/** Ends the session's tool and starts Sculpt Morph Target again, as choosing it again in the editor does. */
	static bool RestartTool(FAutomationTestBase& Test, FMorphToolSession& Session)
	{
		Session.ToolManager->DeactivateTool(EToolSide::Left, EToolShutdownType::Cancel);
		Session.Tool = nullptr;
		const bool bStarted = Session.ToolManager->SelectActiveToolType(EToolSide::Left, TEXT("Sculpt Morph Target")) && Session.ToolManager->ActivateTool(EToolSide::Left);
		Session.Tool = Cast<UMeshVertexSculptTool>(Session.ToolManager->GetActiveTool(EToolSide::Left));
		return Test.TestTrue(TEXT("Sculpt Morph Target started again"), bStarted) && Test.TestNotNull(TEXT("Its sculpt tool"), Session.Tool);
	}

	/**
	 * The tool's palette: the engine's own brushes, then the pack's, then the morph brushes, with Clone Morph and Mirror
	 * Morph among them only when expected; and each morph brush shows its own settings when it is the active brush.
	 */
	static void CheckPalette(FAutomationTestBase& Test, UMeshVertexSculptTool& Tool, bool bExpectCloneAndMirror)
	{
		const TCHAR* Which = bExpectCloneAndMirror ? TEXT("As the plugin is set") : TEXT("With Clone and Mirror Morph switched off");
		TArray<int32> Palette;
		for (const UMeshSculptToolBase::FBrushTypeInfo& Type : Tool.GetAvailableBrushTypes())
		{
			Palette.Add(Type.Identifier);
		}
		TArray<int32> Expected = SculptBrushPack::GetBrushIdentifiers();
		const int32 PackBrushes = Expected.Num();
		Expected.Append(GetBrushIdentifiers());
		const int32 EngineBrushes = Palette.Num() - Expected.Num();
		Test.AddInfo(FString::Printf(TEXT("%s, %s: %d engine brushes, then %d of the pack, then %d morph brushes."), Which, *Tool.GetClass()->GetName(), EngineBrushes, PackBrushes, Expected.Num() - PackBrushes));
		// Six in 5.8.4 (MorphTargetVertexSculptTool.cpp:287-320).
		Test.TestTrue(FString::Printf(TEXT("%s: the engine's morph brushes are still there"), Which), EngineBrushes >= 6);
		bool bInOrder = EngineBrushes >= 0;
		for (int32 Index = 0; bInOrder && Index < Expected.Num(); ++Index)
		{
			bInOrder = Palette[EngineBrushes + Index] == Expected[Index];
		}
		Test.TestTrue(FString::Printf(TEXT("%s: the pack's brushes and then the morph brushes follow the engine's, in order"), Which), bInOrder);
		Test.TestTrue(FString::Printf(TEXT("%s: Clone Morph is in the palette unless switched off"), Which), Palette.Contains(BrushID::CloneMorph) == bExpectCloneAndMirror);
		Test.TestTrue(FString::Printf(TEXT("%s: Mirror Morph is in the palette unless switched off"), Which), Palette.Contains(BrushID::MirrorMorph) == bExpectCloneAndMirror);

		for (const int32 Identifier : GetBrushIdentifiers())
		{
			Tool.SetActiveBrushType(Identifier);
			// Its settings, and no other brush's of this plugin, are the ones the panel shows.
			int32 OwnSettings = 0;
			for (UObject* Object : Tool.GetToolProperties(/*bEnabledOnly*/ true))
			{
				OwnSettings += Object && Object->IsA<UMeshSculptBrushOpProps>() && Object->GetClass()->GetOutermost()->GetFName() == FName(TEXT("/Script/SculptBrushPackMorph")) ? 1 : 0;
			}
			Test.TestEqual(FString::Printf(TEXT("%s: with morph brush %d active, one settings object of this plugin's shows"), Which, Identifier), OwnSettings, 1);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackMorphWrinkleMapTest, "SculptBrushPackMorph.Brushes.WrinkleMapFromATexture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackMorphWrinkleMapTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackMorph;

	// A texture of two by two texels, black but for the second texel of the first row, which is white. The engine's
	// reader needs a texture that has both its source pixels, which it reads in the editor, and its platform data
	// (Texture2DUtil.cpp:286-293 in 5.8.4).
	UTexture2D* Texture = UTexture2D::CreateTransient(2, 2, PF_B8G8R8A8);
	if (!TestNotNull(TEXT("A texture of two by two texels"), Texture))
	{
		return true;
	}
	Texture->SRGB = false;
	const uint8 Pixels[16] = { 0, 0, 0, 255,  255, 255, 255, 255,  0, 0, 0, 255,  0, 0, 0, 255 };
	Texture->Source.Init(2, 2, 1, 1, TSF_BGRA8, Pixels);

	FWrinkleMorphBrushOp::FMapImage Map;
	if (TestTrue(TEXT("The texture is read as a Wrinkle Map"), FWrinkleMorphBrushOp::ReadTextureAsMap(Texture, Map)))
	{
		TestEqual(TEXT("The map is two texels wide"), Map.GetDimensions().GetWidth(), 2);
		TestEqual(TEXT("and two high"), Map.GetDimensions().GetHeight(), 2);
		TestEqual(TEXT("The first texel of the first row is black: 0"), Map.GetPixel(0, 0), 0.0f, 0.0f);
		TestEqual(TEXT("The second texel of the first row is white: 1"), Map.GetPixel(1, 0), 1.0f, 0.0f);
		TestEqual(TEXT("The first texel of the second row is black: 0"), Map.GetPixel(0, 1), 0.0f, 0.0f);
		TestEqual(TEXT("The second texel of the second row is black: 0"), Map.GetPixel(1, 1), 0.0f, 0.0f);
		// The white texel's center is at (0.75, 0.25) of the map, where the brush reads a point with those UVs.
		TestEqual(TEXT("Read at the white texel's center, the map gives 1"), Map.BilinearSampleUV<float, UE::Geometry::EImageTilingMethod::Wrap>(FVector2d(0.75, 0.25), 0.0f), 1.0f, 0.0f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackMorphPosedTest, "SculptBrushPackMorph.Hook.MushAndWrinkleOnAPosedCharacter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackMorphPosedTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackMorphTestLocals;
	using SculptBrushPackMorph::Private::FMorphToolAccess;

	// The engine's tutorial character, with a morph target that moves every point an eighth up, at full weight.
	FMorphToolSession Session;
	if (OpenSession(*this, TEXT("/Temp/SculptBrushPackMorph/PosedCharacter"), [](const FVector3f&) { return FVector3f(0.0f, 0.0f, 0.125f); }, Session, CharacterPath))
	{
		UMeshVertexSculptTool* Tool = Session.Tool;
		UMorphTargetVertexSculptTool& MorphTool = static_cast<UMorphTargetVertexSculptTool&>(*Tool);
		const FReferenceSkeleton& Skeleton = Session.Mesh->GetRefSkeleton();
		const int32 Elbow = Skeleton.FindBoneIndex(TEXT("lowerarm_l"));
		AddInfo(FString::Printf(TEXT("The character has %d bones; lowerarm_l is bone %d."), Skeleton.GetNum(), Elbow));

		if (TestTrue(TEXT("The character has a left forearm"), Elbow != INDEX_NONE)
			&& TestNotNull(TEXT("The editor's preview component"), Session.Preview)
			&& TestEqual(TEXT("The preview component has a transform for every bone"), Session.Preview->GetComponentSpaceTransforms().Num(), Skeleton.GetNum()))
		{
			// The pose: the left forearm squashed to six tenths of its size along each of its bone's axes but one. The
			// character is made of stiff pieces, one to a bone, which share no points across a joint, so turning a joint
			// shortens no surface; scaling a bone squashes its piece, and that does. The tools take the pose from the
			// preview component's bone transforms (SkeletalMeshEditingCache.cpp:520-528 in 5.8.4), which an animation
			// would write; the test writes the one transform itself, in place, and nothing refreshes it before the tool
			// reads it on its next tick.
			TArray<FTransform>& Pose = const_cast<TArray<FTransform>&>(Session.Preview->GetComponentSpaceTransforms());
			Pose[Elbow].SetScale3D(FVector(1.0, 0.6, 0.6));
			Tool->Tick(0.03f);

			// Where the squashed forearm's surface is shortened most: measured as Wrinkle Morph measures it, with what the brushes
			// read from the tool.
			const UDynamicMesh* MeshObject = FMorphToolAccess::ToolMesh(MorphTool);
			const FDynamicMesh3* Mesh = MeshObject ? MeshObject->GetMeshPtr() : nullptr;
			const UE::Geometry::FDynamicMeshMorphTargetAttribute* Lift = Mesh && Mesh->HasAttributes() ? Mesh->Attributes()->GetMorphTargetAttribute(LiftName) : nullptr;
			const TSharedPtr<IMorphBrushContext> Context = Private::MakeToolContext(Tool);
			// The point to brush: the most shortened one that a ray down its own normal meets, as some lie inside a joint
			// behind another piece.
			struct FShortened
			{
				int32 Vertex = INDEX_NONE;
				double Shorter = 0.0;
				FVector3d Place = FVector3d::Zero();
				FVector3d Normal = FVector3d::Zero();
			};
			TArray<FShortened> Shortened;
			int32 Most = INDEX_NONE;
			double MostShorter = 0.0;
			FVector3d MostPlace = FVector3d::Zero();
			FVector3d MostNormal = FVector3d::Zero();
			if (TestNotNull(TEXT("The tool's mesh has the edited morph"), Lift) && TestTrue(TEXT("The tool gives the brushes a context"), Context.IsValid())
				&& TestTrue(TEXT("In the pose, with the morph at full weight, a stroke can begin"), Context->BeginStroke(/*bMirror*/ false)))
			{
				const FDynamicMesh3* Without = Context->GetMeshWithoutMorph();
				if (TestNotNull(TEXT("The posed mesh without the edited morph"), Without))
				{
					for (const int32 Vertex : Mesh->VertexIndicesItr())
					{
						FVector3d Rest;
						if (!Without->IsVertex(Vertex) || !Context->GetUnposedPosition(Vertex, /*bWithEditedMorph*/ false, Rest))
						{
							continue;
						}
						double PosedLength = 0.0;
						double RestLength = 0.0;
						bool bKnown = true;
						for (const int32 Neighbor : Mesh->VtxVerticesItr(Vertex))
						{
							FVector3d RestNeighbor;
							bKnown = bKnown && Without->IsVertex(Neighbor) && Context->GetUnposedPosition(Neighbor, /*bWithEditedMorph*/ false, RestNeighbor);
							if (!bKnown)
							{
								break;
							}
							PosedLength += UE::Geometry::Distance(Without->GetVertex(Neighbor), Without->GetVertex(Vertex));
							RestLength += UE::Geometry::Distance(RestNeighbor, Rest);
						}
						const double Shorter = bKnown && RestLength > 0.0 ? 1.0 - PosedLength / RestLength : 0.0;
						if (Shorter > 0.02)
						{
							FShortened Point;
							Point.Vertex = Vertex;
							Point.Shorter = Shorter;
							Point.Place = Without->GetVertex(Vertex);
							for (const int32 Triangle : Mesh->VtxTrianglesItr(Vertex))
							{
								const UE::Geometry::FIndex3i Corners = Mesh->GetTriangle(Triangle);
								Point.Normal += UE::Geometry::VectorUtil::NormalDirection(Without->GetVertex(Corners.A), Without->GetVertex(Corners.B), Without->GetVertex(Corners.C));
							}
							Point.Normal = UE::Geometry::Normalized(Point.Normal);
							Shortened.Add(Point);
						}
					}
				}
				Context->EndStroke();
			}
			// The ray comes down a point's normal from a little way off, and must meet the mesh within a unit of the
			// point; the brush is four units wide.
			Shortened.Sort([](const FShortened& A, const FShortened& B) { return A.Shorter > B.Shorter; });
			for (const FShortened& Point : Shortened)
			{
				FHitResult Hit;
				if (Tool->HitTest(FRay(FVector(Point.Place + Point.Normal * 10.0), FVector(-Point.Normal)), Hit) && FVector::Dist(Hit.ImpactPoint, FVector(Point.Place)) < 1.0)
				{
					Most = Point.Vertex;
					MostShorter = Point.Shorter;
					MostPlace = Point.Place;
					MostNormal = Point.Normal;
					break;
				}
			}
			AddInfo(FString::Printf(TEXT("Of %d points, %d are more than 0.02 shorter than at rest, the most by %.4g. The most shortened one a ray can meet is point %d, %.4g shorter, at (%.3f, %.3f, %.3f)."),
				Mesh ? Mesh->VertexCount() : 0, Shortened.Num(), Shortened.Num() > 0 ? Shortened[0].Shorter : 0.0, Most, MostShorter, MostPlace.X, MostPlace.Y, MostPlace.Z));

			USculptBrushPackWrinkleMorphOpProps* WrinkleSettings = nullptr;
			for (UObject* Object : Tool->GetToolProperties(/*bEnabledOnly*/ false))
			{
				if (USculptBrushPackOpProps* Settings = Cast<USculptBrushPackOpProps>(Object))
				{
					// Full weight to the brush's rim.
					Settings->Falloff = 0.0f;
				}
				WrinkleSettings = WrinkleSettings ? WrinkleSettings : Cast<USculptBrushPackWrinkleMorphOpProps>(Object);
			}
			if (TestTrue(TEXT("Squashing the forearm shortens its surface by more than Wrinkle Morph's Starts At, at a point a ray can meet"), Most != INDEX_NONE && MostShorter > 0.02)
				&& TestNotNull(TEXT("Wrinkle Morph's settings"), WrinkleSettings) && Lift)
			{
				auto DeltaOf = [Lift](int32 Vertex)
				{
					FVector3d Delta;
					Lift->GetValue(Vertex, Delta);
					return Delta;
				};
				auto ChangedPoints = [&](const TArray<FVector3d>& Before)
				{
					int32 Changed = 0;
					for (const int32 Vertex : Mesh->VertexIndicesItr())
					{
						Changed += DeltaOf(Vertex) != Before[Vertex] ? 1 : 0;
					}
					return Changed;
				};
				auto AllDeltas = [&]()
				{
					TArray<FVector3d> Deltas;
					Deltas.SetNum(Mesh->MaxVertexID());
					for (const int32 Vertex : Mesh->VertexIndicesItr())
					{
						Deltas[Vertex] = DeltaOf(Vertex);
					}
					return Deltas;
				};
				// The stroke: the ray that was tried above.
				const FRay Ray(FVector(MostPlace + MostNormal * 10.0), FVector(-MostNormal));
				Tool->BrushProperties->BrushSize.SizeType = EBrushToolSizeType::World;
				Tool->BrushProperties->BrushSize.WorldRadius = 4.0f;
				auto Brush = [&](int32 Identifier)
				{
					Tool->SetActiveBrushType(Identifier);
					Tool->Tick(0.03f);
					return StrokeAlong(*Tool, Ray);
				};
				const double HalfHeight = 0.5 * WrinkleSettings->Height;

				// Wrinkle Morph, only where compressed, whole at the shortening found: the most shortened point rises by
				// the Height. A point is judged to have risen when its delta moved by more than half the Height.
				WrinkleSettings->StartsAt = 0.0f;
				WrinkleSettings->FullAt = static_cast<float>(MostShorter);
				TArray<FVector3d> Before = AllDeltas();
				TestTrue(TEXT("Wrinkle Morph on the squashed forearm: the stroke met the mesh"), Brush(BrushID::WrinkleMorph));
				double Moved = UE::Geometry::Distance(DeltaOf(Most), Before[Most]);
				AddInfo(FString::Printf(TEXT("Wrinkle Morph, only where compressed: the most shortened point's delta moved by %.4g; %d points changed."), Moved, ChangedPoints(Before)));
				TestTrue(TEXT("Wrinkle Morph raises the most shortened point of the squashed forearm"), Moved > HalfHeight);

				// The same with a real texture as the Wrinkle Map: white raises as no map does, black raises nothing.
				auto MakeTexture = [](uint8 Value)
				{
					UTexture2D* Texture = UTexture2D::CreateTransient(4, 4, PF_B8G8R8A8);
					Texture->SRGB = false;
					TArray<uint8> Pixels;
					Pixels.Init(Value, 4 * 4 * 4);
					Texture->Source.Init(4, 4, 1, 1, TSF_BGRA8, Pixels.GetData());
					return Texture;
				};
				WrinkleSettings->WrinkleMap = MakeTexture(255);
				Before = AllDeltas();
				TestTrue(TEXT("Wrinkle Morph with a white map: the stroke met the mesh"), Brush(BrushID::WrinkleMorph));
				Moved = UE::Geometry::Distance(DeltaOf(Most), Before[Most]);
				AddInfo(FString::Printf(TEXT("Wrinkle Morph with a white Wrinkle Map: the point's delta moved by %.4g."), Moved));
				TestTrue(TEXT("With a white Wrinkle Map the point rises as without a map"), Moved > HalfHeight);

				WrinkleSettings->WrinkleMap = MakeTexture(0);
				Before = AllDeltas();
				TestTrue(TEXT("Wrinkle Morph with a black map: the stroke met the mesh"), Brush(BrushID::WrinkleMorph));
				Moved = UE::Geometry::Distance(DeltaOf(Most), Before[Most]);
				AddInfo(FString::Printf(TEXT("Wrinkle Morph with a black Wrinkle Map: the point's delta moved by %.4g."), Moved));
				TestTrue(TEXT("With a black Wrinkle Map nothing rises"), Moved < HalfHeight);
				WrinkleSettings->WrinkleMap = nullptr;

				// Mush Morph on the same place: on a joined surface in a pose it can measure, and it moves the point. How
				// far is reported; that the delta is no longer what it was is what is judged.
				Before = AllDeltas();
				TestTrue(TEXT("Mush Morph on the squashed forearm: the stroke met the mesh"), Brush(BrushID::MushMorph));
				Moved = UE::Geometry::Distance(DeltaOf(Most), Before[Most]);
				const int32 Mushed = ChangedPoints(Before);
				AddInfo(FString::Printf(TEXT("Mush Morph on the squashed forearm: the point's delta moved by %.4g; %d points changed."), Moved, Mushed));
				TestTrue(TEXT("Mush Morph moves points of the squashed forearm"), Mushed > 0);
				TestTrue(TEXT("and the most shortened point among them"), DeltaOf(Most) != Before[Most]);
			}
		}
	}
	CloseSession(*this, Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackMorphEditorTest, "SculptBrushPackMorph.Hook.SculptMorphTargetInSkeletalMeshEditor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackMorphEditorTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackMorphTestLocals;

	// As the plugin is set: all six morph brushes are in the palette.
	FScopedCloneAndMirror CloneAndMirror(true);
	TestTrue(TEXT("The plugin has its console variable for Clone Morph and Mirror Morph"), CloneAndMirror.IsFound());
	TestTrue(TEXT("The variable is on unless switched off"), CloneAndMirror.IsOnByDefault());
	TestTrue(TEXT("With it on the morph brushes are six"), GetBrushIdentifiers() == TArray<int32>({ BrushID::AmplifyMorph, BrushID::SmoothMorph, BrushID::MushMorph, BrushID::WrinkleMorph, BrushID::CloneMorph, BrushID::MirrorMorph }));

	// PackLift moves every point 1 up.
	FMorphToolSession Session;
	if (OpenSession(*this, TEXT("/Temp/SculptBrushPackMorph/HookCube"), [](const FVector3f&) { return FVector3f(0.0f, 0.0f, 1.0f); }, Session))
	{
		UMeshVertexSculptTool* Tool = Session.Tool;
		TestTrue(TEXT("It is the engine's morph sculpt tool"), IsMorphSculptTool(Tool));
		TestTrue(TEXT("The Sculpt Brush Pack knows the tool"), SculptBrushPack::IsSupportedTool(Tool));
		CheckPalette(*this, *Tool, /*bExpectCloneAndMirror*/ true);

		// What the brushes read from the tool. Before the tool's first tick it has not posed its mesh, and the brushes are
		// told so; after it they can read the mesh.
		const TSharedPtr<IMorphBrushContext> Context = Private::MakeToolContext(Tool);
		if (TestTrue(TEXT("The tool gives the brushes a context"), Context.IsValid()))
		{
			TestFalse(TEXT("Before the tool's first tick no stroke can begin"), Context->BeginStroke(/*bMirror*/ false));
			TestNull(TEXT("and there is no mesh without the morph yet"), Context->GetMeshWithoutMorph());
			AddInfo(TEXT("Before the tool's first tick: ") + Private::DescribeMorphTool(Tool));
			Tool->Tick(0.03f);
			AddInfo(TEXT("After it: ") + Private::DescribeMorphTool(Tool));

			TestTrue(TEXT("The other morph target can be cloned from"), Context->GetSourceNames() == TArray<FName>({ ShiftName }));
			// The tool's own Symmetry is a mirror from +X onto -X; while it is on, Mirror Morph stands back and says so. The
			// editor's tools context writes a tool's warning to the log, where the test expects it once.
			AddExpectedMessage(TEXT("Mirror Morph does nothing while the tool's Symmetry is on"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
			Tool->SymmetryProperties->bEnableSymmetry = true;
			Tool->Tick(0.03f);
			if (TestTrue(TEXT("The tool takes Symmetry for the cube"), Context->IsToolSymmetryOn()))
			{
				TestFalse(TEXT("With the tool's Symmetry on a mirror stroke is refused"), Context->BeginStroke(/*bMirror*/ true));
				TestTrue(TEXT("and other strokes are not"), Context->BeginStroke(/*bMirror*/ false));
			}
			Tool->SymmetryProperties->bEnableSymmetry = false;
			Tool->Tick(0.03f);
			TestFalse(TEXT("Symmetry is off again"), Context->IsToolSymmetryOn());

			const bool bEditable = Context->BeginStroke(/*bMirror*/ true);
			TestTrue(TEXT("After the tool's first tick, with the morph at full weight, a stroke can begin"), bEditable);
			const FDynamicMesh3* Without = Context->GetMeshWithoutMorph();
			if (TestNotNull(TEXT("The mesh without the edited morph"), Without) && bEditable)
			{
				TArray<int32> Vertices;
				for (const int32 Vertex : Without->VertexIndicesItr())
				{
					Vertices.Add(Vertex);
				}
				Context->PrepareVertices(Vertices);
				// The cube stands in its reference pose, so a delta is shown as it is kept, up to the rounding of the
				// engine's skinning; the largest difference is reported, not judged.
				double LargestSource = 0.0;
				int32 Known = 0;
				for (const int32 Vertex : Vertices)
				{
					FVector3d Offset;
					if (Context->GetSourceOffset(ShiftName, Vertex, Offset))
					{
						++Known;
						LargestSource = FMath::Max(LargestSource, (Offset - FVector3d(2, 0, 0)).GetAbsMax());
					}
				}
				TestEqual(TEXT("Every point knows the other morph's offset"), Known, Vertices.Num());

				// What Mush Morph and Wrinkle Morph read: each point's place with the pose taken off, with and without
				// the edited morph, and its place in the mesh's UVs. The cube stands in its reference pose, so the
				// unposed place without the morph is the shown place without the morph, up to the rounding of the
				// engine's skinning; the largest difference is reported, not judged. PackLift moves every point 1 up,
				// which is what the edited morph adds to the unposed place.
				int32 WithUV = 0;
				int32 WithPlace = 0;
				int32 LiftedByOne = 0;
				double LargestUnposed = 0.0;
				for (const int32 Vertex : Vertices)
				{
					FVector2d UV;
					WithUV += Context->GetUV(Vertex, 0, UV) ? 1 : 0;
					FVector3d WithMorph, WithoutMorph;
					if (Context->GetUnposedPosition(Vertex, /*bWithEditedMorph*/ true, WithMorph) && Context->GetUnposedPosition(Vertex, /*bWithEditedMorph*/ false, WithoutMorph))
					{
						++WithPlace;
						LiftedByOne += WithMorph - WithoutMorph == FVector3d(0, 0, 1) ? 1 : 0;
						LargestUnposed = FMath::Max(LargestUnposed, (WithoutMorph - Without->GetVertex(Vertex)).GetAbsMax());
					}
				}
				TestEqual(TEXT("Every point has a place in UV channel 0"), WithUV, Vertices.Num());
				FVector2d NoUV;
				TestFalse(TEXT("The cube has no UV channel 7"), Context->GetUV(Vertices[0], 7, NoUV));
				TestEqual(TEXT("Every point's place with the pose taken off is known"), WithPlace, Vertices.Num());
				TestEqual(TEXT("With the pose taken off, the edited morph adds its delta of 1 up to every point"), LiftedByOne, Vertices.Num());
				AddInfo(FString::Printf(TEXT("The unposed place without the morph differs from the shown one by at most %.3g."), LargestUnposed));
				AddInfo(FString::Printf(TEXT("%d points; the other morph's offset differs from (2, 0, 0) by at most %.3g."), Vertices.Num(), LargestSource));
			}
			Context->EndStroke();
		}

		// A tool left on Mirror Morph, and the variable switched off: the next tool does not have that brush. It starts
		// on the engine's Move; asked for a brush it lacks it would fail an engine ensure, which this test would report
		// as an error. Its palette holds the four other morph brushes.
		Session.Tool->SetActiveBrushType(BrushID::MirrorMorph);
		CloneAndMirror.Set(false);
		TestTrue(TEXT("With the variable off the morph brushes are Amplify, Smooth, Mush and Wrinkle Morph"), GetBrushIdentifiers() == TArray<int32>({ BrushID::AmplifyMorph, BrushID::SmoothMorph, BrushID::MushMorph, BrushID::WrinkleMorph }));
		if (RestartTool(*this, Session))
		{
			TestEqual(TEXT("A tool that starts without Mirror Morph, after one left on it, starts on the engine's Move"), Session.Tool->SculptProperties->PrimaryBrushID, static_cast<int32>(EMeshVertexSculptBrushType::Move));
			CheckPalette(*this, *Session.Tool, /*bExpectCloneAndMirror*/ false);

			// Switched on again, the next tool has all six.
			CloneAndMirror.Set(true);
			if (RestartTool(*this, Session))
			{
				CheckPalette(*this, *Session.Tool, /*bExpectCloneAndMirror*/ true);
			}
		}
	}
	CloseSession(*this, Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackMorphRememberedBrushTest, "SculptBrushPackMorph.Hook.RememberedBrushOnAnotherTool",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackMorphRememberedBrushTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackMorphTestLocals;

	// The sculpt tools remember the brush used last, and Sculpt Morph Target and the plain Sculpt tool remember it under
	// one key (MeshVertexSculptTool.cpp:287, :468-471 in 5.8.4). A tool asked for a brush it does not have fails an engine
	// ensure, which this test would report as an error.
	FMorphToolSession Session;
	if (OpenSession(*this, TEXT("/Temp/SculptBrushPackMorph/RememberCube"), [](const FVector3f&) { return FVector3f(0.0f, 0.0f, 1.0f); }, Session))
	{
		const FString SculptTool(TEXT("BeginSculptMeshTool"));
		const FString MorphTool(TEXT("Sculpt Morph Target"));
		const int32 Move = static_cast<int32>(EMeshVertexSculptBrushType::Move);
		const int32 Clay = SculptBrushPack::GetBrushIdentifiers()[0];

		// Sculpt Morph Target ends with a morph brush active; Sculpt, which has no morph brushes, starts on Move.
		Session.Tool->SetActiveBrushType(BrushID::SmoothMorph);
		Session.ToolManager->DeactivateTool(EToolSide::Left, EToolShutdownType::Cancel);
		Session.Tool = nullptr;
		bool bStarted = Session.ToolManager->SelectActiveToolType(EToolSide::Left, SculptTool) && Session.ToolManager->ActivateTool(EToolSide::Left);
		UMeshVertexSculptTool* Sculpt = Cast<UMeshVertexSculptTool>(Session.ToolManager->GetActiveTool(EToolSide::Left));
		if (TestTrue(TEXT("Sculpt started after Sculpt Morph Target"), bStarted) && TestNotNull(TEXT("The Sculpt tool"), Sculpt))
		{
			TestFalse(TEXT("It is not the morph sculpt tool"), IsMorphSculptTool(Sculpt));
			TestEqual(TEXT("Sculpt starts on the engine's Move, not on a morph brush it does not have"), Sculpt->SculptProperties->PrimaryBrushID, Move);

			// A brush both tools have is remembered from one to the other.
			Sculpt->SetActiveBrushType(Clay);
			Session.ToolManager->DeactivateTool(EToolSide::Left, EToolShutdownType::Cancel);
			bStarted = Session.ToolManager->SelectActiveToolType(EToolSide::Left, MorphTool) && Session.ToolManager->ActivateTool(EToolSide::Left);
			Session.Tool = Cast<UMeshVertexSculptTool>(Session.ToolManager->GetActiveTool(EToolSide::Left));
			if (TestTrue(TEXT("Sculpt Morph Target started again"), bStarted) && TestNotNull(TEXT("The morph sculpt tool"), Session.Tool))
			{
				TestEqual(TEXT("Sculpt Morph Target starts on the pack brush Sculpt was left on"), Session.Tool->SculptProperties->PrimaryBrushID, Clay);
			}
		}
	}
	CloseSession(*this, Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackMorphStrokesTest, "SculptBrushPackMorph.Hook.StrokesInSkeletalMeshEditor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackMorphStrokesTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackMorphTestLocals;
	using SculptBrushPackMorph::Private::FMorphToolAccess;

	// The test strokes with all six brushes, so Clone Morph and Mirror Morph are kept in the palette whatever the console
	// variable was.
	FScopedCloneAndMirror CloneAndMirror(true);

	// PackLift moves the points of the cube's +X half by (0.5, 0, 1) and leaves the -X half: a shape on one side only.
	const FVector3f LiftDelta(0.5f, 0.0f, 1.0f);
	FMorphToolSession Session;
	if (OpenSession(*this, TEXT("/Temp/SculptBrushPackMorph/StrokesCube"), [LiftDelta](const FVector3f& Position) { return Position.X > 0.0f ? LiftDelta : FVector3f::ZeroVector; }, Session))
	{
		UMeshVertexSculptTool* Tool = Session.Tool;
		Tool->Tick(0.03f);
		UMorphTargetVertexSculptTool& MorphTool = static_cast<UMorphTargetVertexSculptTool&>(*Tool);
		const UDynamicMesh* MeshObject = FMorphToolAccess::ToolMesh(MorphTool);
		const FDynamicMesh3* Mesh = MeshObject ? MeshObject->GetMeshPtr() : nullptr;
		const UE::Geometry::FDynamicMeshMorphTargetAttribute* Lift = Mesh && Mesh->HasAttributes() ? Mesh->Attributes()->GetMorphTargetAttribute(LiftName) : nullptr;
		if (TestNotNull(TEXT("The tool's mesh has the edited morph"), Lift))
		{
			const UE::Geometry::FAxisAlignedBox3d Bounds = Mesh->GetBounds(/*bParallel*/ false);
			AddInfo(FString::Printf(TEXT("The cube: %d points, from (%g, %g, %g) to (%g, %g, %g); the tool's symmetry is %s."), Mesh->VertexCount(),
				Bounds.Min.X, Bounds.Min.Y, Bounds.Min.Z, Bounds.Max.X, Bounds.Max.Y, Bounds.Max.Z, FMorphToolAccess::IsSymmetryApplied(MorphTool) ? TEXT("on") : TEXT("off")));

			// The points of one corner of the cube's top (several: the cube's faces do not share points), and the deltas
			// PackLift has there now.
			auto CornerAt = [&](bool bPositiveX, bool bPositiveY)
			{
				const FVector3d Corner(bPositiveX ? Bounds.Max.X : Bounds.Min.X, bPositiveY ? Bounds.Max.Y : Bounds.Min.Y, Bounds.Max.Z);
				TArray<int32> Vertices;
				for (const int32 Vertex : Mesh->VertexIndicesItr())
				{
					if (Mesh->GetVertex(Vertex) == Corner)
					{
						Vertices.Add(Vertex);
					}
				}
				return Vertices;
			};
			auto DeltaOf = [&](int32 Vertex)
			{
				FVector3d Delta;
				Lift->GetValue(Vertex, Delta);
				return Delta;
			};
			auto Describe = [&](const TArray<int32>& Vertices)
			{
				FString Text;
				for (const int32 Vertex : Vertices)
				{
					const FVector3d Delta = DeltaOf(Vertex);
					Text += FString::Printf(TEXT(" (%.6g, %.6g, %.6g)"), Delta.X, Delta.Y, Delta.Z);
				}
				return Text;
			};
			// Every delta of the corner is nearer to Wanted than to what it was before the stroke: no tolerance is needed
			// to say that the stroke did what the brush is for.
			auto AllNearer = [&](const TArray<int32>& Vertices, const TArray<FVector3d>& Before, const FVector3d& Wanted)
			{
				bool bNearer = Vertices.Num() > 0;
				for (int32 Index = 0; Index < Vertices.Num(); ++Index)
				{
					const FVector3d Now = DeltaOf(Vertices[Index]);
					bNearer &= UE::Geometry::DistanceSquared(Now, Wanted) < UE::Geometry::DistanceSquared(Now, Before[Index]);
				}
				return bNearer;
			};
			auto DeltasOf = [&](const TArray<int32>& Vertices)
			{
				TArray<FVector3d> Deltas;
				for (const int32 Vertex : Vertices)
				{
					Deltas.Add(DeltaOf(Vertex));
				}
				return Deltas;
			};

			// A brush that covers one corner and no other, at full weight to its rim.
			Tool->BrushProperties->BrushSize.SizeType = EBrushToolSizeType::World;
			Tool->BrushProperties->BrushSize.WorldRadius = static_cast<float>(0.25 * (Bounds.Max.X - Bounds.Min.X));
			for (UObject* Object : Tool->GetToolProperties(/*bEnabledOnly*/ false))
			{
				if (USculptBrushPackOpProps* Settings = Cast<USculptBrushPackOpProps>(Object))
				{
					Settings->Falloff = 0.0f;
				}
				if (USculptBrushPackCloneMorphOpProps* Clone = Cast<USculptBrushPackCloneMorphOpProps>(Object))
				{
					Clone->Source = ShiftName;
				}
			}
			// The ray lands a little inside the top face from the corner, so that it meets the face and not its edge.
			auto Brush = [&](int32 Identifier, const TArray<int32>& Corner)
			{
				if (Corner.IsEmpty())
				{
					return false;
				}
				Tool->SetActiveBrushType(Identifier);
				// The tool reads the active brush's falloff on its tick (MeshSculptToolBase.cpp:333-336); in the editor
				// ticks pass between choosing a brush and the first stroke.
				Tool->Tick(0.03f);
				const FVector3d At = Mesh->GetVertex(Corner[0]);
				const FVector3d Inside = At + (FVector3d(Bounds.Center().X, Bounds.Center().Y, At.Z) - At) * 0.125;
				return Stroke(*Tool, Inside, /*bFromAbove*/ true);
			};

			const TArray<int32> LeftBack = CornerAt(false, true);
			const TArray<int32> LeftFront = CornerAt(false, false);
			const TArray<int32> RightFront = CornerAt(true, false);
			const TArray<int32> RightBack = CornerAt(true, true);
			TestTrue(TEXT("The cube's top has four corners"), LeftBack.Num() > 0 && LeftFront.Num() > 0 && RightFront.Num() > 0 && RightBack.Num() > 0);
			const FVector3d LiftAsDouble(LiftDelta);
			AddInfo(TEXT("Before any stroke, -X corner:") + Describe(LeftBack) + TEXT("; +X corner:") + Describe(RightBack));

			// Mirror Morph on a -X corner: it takes the +X corner's delta, mirrored across X.
			TArray<FVector3d> Before = DeltasOf(LeftBack);
			const TArray<FVector3d> OtherSideBefore = DeltasOf(RightBack);
			TestTrue(TEXT("Mirror Morph: the stroke met the mesh"), Brush(BrushID::MirrorMorph, LeftBack));
			AddInfo(TEXT("After Mirror Morph on the -X corner:") + Describe(LeftBack));
			TestTrue(TEXT("Mirror Morph gives the brushed corner the other side's shape, mirrored"), AllNearer(LeftBack, Before, FVector3d(-LiftAsDouble.X, LiftAsDouble.Y, LiftAsDouble.Z)));
			TestTrue(TEXT("Mirror Morph leaves the side it copied from as it was"), DeltasOf(RightBack) == OtherSideBefore);

			// Mirror Morph set to copy from +X, on a +X corner: the corner is the side copied from and keeps its shape.
			USculptBrushPackMirrorMorphOpProps* MirrorSettings = nullptr;
			for (UObject* Object : Tool->GetToolProperties(/*bEnabledOnly*/ false))
			{
				MirrorSettings = MirrorSettings ? MirrorSettings : Cast<USculptBrushPackMirrorMorphOpProps>(Object);
			}
			if (TestNotNull(TEXT("Mirror Morph's settings"), MirrorSettings))
			{
				MirrorSettings->CopyFrom = ESculptBrushPackMirrorSource::PositiveX;
				Before = DeltasOf(RightFront);
				TestTrue(TEXT("Mirror Morph from +X: the stroke met the mesh"), Brush(BrushID::MirrorMorph, RightFront));
				AddInfo(TEXT("After Mirror Morph from +X on a +X corner:") + Describe(RightFront));
				bool bKept = true;
				for (int32 Index = 0; Index < RightFront.Num(); ++Index)
				{
					const FVector3d Now = DeltaOf(RightFront[Index]);
					bKept &= UE::Geometry::DistanceSquared(Now, Before[Index]) < UE::Geometry::DistanceSquared(Now, FVector3d::ZeroVector);
				}
				TestTrue(TEXT("Mirror Morph from +X leaves a +X corner its shape, where from the other side it would have none"), bKept);
				MirrorSettings->CopyFrom = ESculptBrushPackMirrorSource::OtherSide;
			}

			// Clone Morph on the other -X corner: it takes PackShift's delta.
			Before = DeltasOf(LeftFront);
			TestTrue(TEXT("Clone Morph: the stroke met the mesh"), Brush(BrushID::CloneMorph, LeftFront));
			AddInfo(TEXT("After Clone Morph on the other -X corner:") + Describe(LeftFront));
			TestTrue(TEXT("Clone Morph gives the brushed corner the source's shape"), AllNearer(LeftFront, Before, FVector3d(2, 0, 0)));

			// Amplify Morph on a +X corner: its delta becomes half again as large.
			Before = DeltasOf(RightFront);
			TestTrue(TEXT("Amplify Morph: the stroke met the mesh"), Brush(BrushID::AmplifyMorph, RightFront));
			AddInfo(TEXT("After Amplify Morph on a +X corner:") + Describe(RightFront));
			TestTrue(TEXT("Amplify Morph makes the brushed corner's shape half again as strong"), AllNearer(RightFront, Before, LiftAsDouble * 1.5));

			// Smooth Morph on the other +X corner: its points' deltas move toward their neighbors'.
			Before = DeltasOf(RightBack);
			TestTrue(TEXT("Smooth Morph: the stroke met the mesh"), Brush(BrushID::SmoothMorph, RightBack));
			AddInfo(TEXT("After Smooth Morph on the other +X corner:") + Describe(RightBack));
			TestTrue(TEXT("Smooth Morph changes the brushed corner"), DeltasOf(RightBack) != Before);

			// Wrinkle Morph with Only Where Compressed off: every point of the corner rises by the Height along its
			// own face's normal. A point is judged to have risen when it moved by more than half the Height, and to
			// have stayed when it moved by less: nearer to the one than to the other.
			USculptBrushPackWrinkleMorphOpProps* WrinkleSettings = nullptr;
			for (UObject* Object : Tool->GetToolProperties(/*bEnabledOnly*/ false))
			{
				WrinkleSettings = WrinkleSettings ? WrinkleSettings : Cast<USculptBrushPackWrinkleMorphOpProps>(Object);
			}
			if (TestNotNull(TEXT("Wrinkle Morph's settings"), WrinkleSettings))
			{
				const double HalfHeight = 0.5 * WrinkleSettings->Height;
				auto LeastAndMostMoved = [&](const TArray<int32>& Vertices, const TArray<FVector3d>& From)
				{
					TPair<double, double> LeastAndMost(TNumericLimits<double>::Max(), 0.0);
					for (int32 Index = 0; Index < Vertices.Num(); ++Index)
					{
						const double Moved = UE::Geometry::Distance(DeltaOf(Vertices[Index]), From[Index]);
						LeastAndMost.Key = FMath::Min(LeastAndMost.Key, Moved);
						LeastAndMost.Value = FMath::Max(LeastAndMost.Value, Moved);
					}
					return LeastAndMost;
				};

				WrinkleSettings->bOnlyWhereCompressed = false;
				Before = DeltasOf(LeftFront);
				TestTrue(TEXT("Wrinkle Morph: the stroke met the mesh"), Brush(BrushID::WrinkleMorph, LeftFront));
				AddInfo(TEXT("After Wrinkle Morph on a corner, Only Where Compressed off:") + Describe(LeftFront));
				TestTrue(TEXT("Wrinkle Morph raises every point of the brushed corner by its Height"), LeastAndMostMoved(LeftFront, Before).Key > HalfHeight);

				// With Only Where Compressed on, on a mesh in its reference pose: nothing is compressed, nothing rises,
				// and the brush says so once the stroke ends. The editor's tools context writes the warning to the log.
				AddExpectedMessage(TEXT("Wrinkle Morph raises wrinkles where the posed surface is shorter than at rest"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
				WrinkleSettings->bOnlyWhereCompressed = true;
				Before = DeltasOf(LeftBack);
				TestTrue(TEXT("Wrinkle Morph, only where compressed: the stroke met the mesh"), Brush(BrushID::WrinkleMorph, LeftBack));
				TestTrue(TEXT("Wrinkle Morph raises nothing on a mesh in its reference pose"), LeastAndMostMoved(LeftBack, Before).Value < HalfHeight);

				// Mush Morph on this cube cannot act, and says so. Each face of the cube is two triangles that share no
				// point with the next face, and one round of averaging brings such a face's four points onto one line:
				// the two on its diagonal meet at the face's middle, and the other two lie either side of it. A surface
				// without area has no normal to measure the detail along. So the stroke goes through the tool, the
				// brush reads the tool's mesh, reports that it could not measure the surface, and moves nothing. A stroke
				// that mends a posed surface is tested by the brush's rule test, not here.
				AddExpectedMessage(TEXT("Mush Morph could not measure the surface under the brush"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
				Before = DeltasOf(RightFront);
				TestTrue(TEXT("Mush Morph: the stroke met the mesh"), Brush(BrushID::MushMorph, RightFront));
				const TPair<double, double> Mushed = LeastAndMostMoved(RightFront, Before);
				AddInfo(FString::Printf(TEXT("After Mush Morph on a corner of the cube, its points moved by at most %.3g."), Mushed.Value));
				TestTrue(TEXT("Mush Morph moves nothing where it cannot measure the surface"), Mushed.Value < HalfHeight);
			}
		}
	}
	CloseSession(*this, Session);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
