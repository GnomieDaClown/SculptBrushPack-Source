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
// Clone Morph and Mirror Morph are out of the brush palette unless the plugin's console variable is on. Their rules are
// tested as the others'; a test that needs them in a tool switches the variable on for its length.

#include "CoreMinimal.h"
#include "Animation/AnimCurveMetadata.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "Engine/SkeletalMesh.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Commands/UICommandList.h"
#include "HAL/IConsoleManager.h"
#include "InteractiveToolManager.h"
#include "MeshDescription.h"
#include "MeshVertexSculptTool.h"
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
	};

	/**
	 * LiftAt gives PackLift's delta for a point of the cube in its reference pose; PackShift moves every point 2 along X.
	 * False when the tool could not be started; what was opened is closed by CloseSession either way.
	 */
	static bool OpenSession(FAutomationTestBase& Test, const TCHAR* PackageName, TFunctionRef<FVector3f(const FVector3f&)> LiftAt, FMorphToolSession& Session)
	{
		// A copy of the engine's skeletal cube in a package of its own: the engine's asset is not opened for editing.
		USkeletalMesh* Source = LoadObject<USkeletalMesh>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
		if (!Test.TestNotNull(TEXT("The engine's skeletal cube"), Source))
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
	static bool Stroke(UMeshVertexSculptTool& Tool, const FVector3d& Point, bool bFromAbove)
	{
		const FVector Direction = bFromAbove ? FVector(0, 0, -1) : FVector(0, 0, 1);
		const FRay Ray(FVector(Point) - Direction * 1000.0, Direction);
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
		const TCHAR* Which = bExpectCloneAndMirror ? TEXT("With Clone and Mirror Morph asked for") : TEXT("As the plugin is set");
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
		Test.TestTrue(FString::Printf(TEXT("%s: Clone Morph is in the palette only when asked for"), Which), Palette.Contains(BrushID::CloneMorph) == bExpectCloneAndMirror);
		Test.TestTrue(FString::Printf(TEXT("%s: Mirror Morph is in the palette only when asked for"), Which), Palette.Contains(BrushID::MirrorMorph) == bExpectCloneAndMirror);

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSculptBrushPackMorphEditorTest, "SculptBrushPackMorph.Hook.SculptMorphTargetInSkeletalMeshEditor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSculptBrushPackMorphEditorTest::RunTest(const FString& Parameters)
{
	using namespace SculptBrushPackMorphTestLocals;

	// As the plugin is set unless asked otherwise: Clone Morph and Mirror Morph are not in the palette.
	FScopedCloneAndMirror CloneAndMirror(false);
	TestTrue(TEXT("The plugin has its console variable for Clone Morph and Mirror Morph"), CloneAndMirror.IsFound());
	TestFalse(TEXT("The variable is off unless asked for"), CloneAndMirror.IsOnByDefault());
	TestTrue(TEXT("With it off the morph brushes are Amplify Morph and Smooth Morph"), GetBrushIdentifiers() == TArray<int32>({ BrushID::AmplifyMorph, BrushID::SmoothMorph }));

	// PackLift moves every point 1 up.
	FMorphToolSession Session;
	if (OpenSession(*this, TEXT("/Temp/SculptBrushPackMorph/HookCube"), [](const FVector3f&) { return FVector3f(0.0f, 0.0f, 1.0f); }, Session))
	{
		UMeshVertexSculptTool* Tool = Session.Tool;
		TestTrue(TEXT("It is the engine's morph sculpt tool"), IsMorphSculptTool(Tool));
		TestTrue(TEXT("The Sculpt Brush Pack knows the tool"), SculptBrushPack::IsSupportedTool(Tool));
		CheckPalette(*this, *Tool, /*bExpectCloneAndMirror*/ false);

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
				AddInfo(FString::Printf(TEXT("%d points; the other morph's offset differs from (2, 0, 0) by at most %.3g."), Vertices.Num(), LargestSource));
			}
			Context->EndStroke();
		}

		// With the console variable on, a tool that starts gets Clone Morph and Mirror Morph as well.
		CloneAndMirror.Set(true);
		TestTrue(TEXT("With the variable on the morph brushes are four"), GetBrushIdentifiers() == TArray<int32>({ BrushID::AmplifyMorph, BrushID::SmoothMorph, BrushID::CloneMorph, BrushID::MirrorMorph }));
		if (RestartTool(*this, Session))
		{
			CheckPalette(*this, *Session.Tool, /*bExpectCloneAndMirror*/ true);

			// A tool left on Mirror Morph, and the variable switched off: the next tool does not have that brush. It
			// starts on the engine's Move; asked for a brush it lacks it would fail an engine ensure, which this test
			// would report as an error.
			Session.Tool->SetActiveBrushType(BrushID::MirrorMorph);
			CloneAndMirror.Set(false);
			if (RestartTool(*this, Session))
			{
				TestEqual(TEXT("A tool that starts without Mirror Morph, after one left on it, starts on the engine's Move"), Session.Tool->SculptProperties->PrimaryBrushID, static_cast<int32>(EMeshVertexSculptBrushType::Move));
				CheckPalette(*this, *Session.Tool, /*bExpectCloneAndMirror*/ false);
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

	// The test strokes with all four brushes, so the two that are out of the palette are asked for.
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
		}
	}
	CloseSession(*this, Session);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
