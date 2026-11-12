// Copyright (C) 2026 Joseph Wiginton.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sculpt Brush Pack: Morph Targets. The module: it hands the Sculpt Brush Pack its brushes for the engine's Sculpt Morph
// Target tool, and gives them what they need to know of the morph target the tool edits.
//
// That knowledge is the tool's own state: the mesh without the edited morph, the pose and weights the shown mesh was
// posed with, and its copy of the asset's mesh with every morph target's deltas. The tool offers no public way to read
// it, so it is read through SculptBrushPackMorphToolAccess.h. Engine anchors are for 5.8.4.

#include "SculptBrushPackMorph.h"

#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicSubmesh3.h"
#include "HAL/IConsoleManager.h"
#include "InteractiveToolManager.h"
#include "Interfaces/IPluginManager.h"
#include "MatrixTypes.h"
#include "MeshVertexSculptTool.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "MorphTargetVertexSculptTool.h"
#include "Parameterization/MeshPlanarSymmetry.h"
#include "SculptBrushPack.h"
#include "SculptBrushPackMorphBrushes.h"
#include "SculptBrushPackMorphToolAccess.h"
#include "SkeletalMesh/SkeletalMeshEditingInterface.h"
#include "SkeletalMesh/SkeletalMeshToolsHelper.h"
#include "UDynamicMesh.h"

#define LOCTEXT_NAMESPACE "SculptBrushPackMorph"

DEFINE_LOG_CATEGORY_STATIC(LogSculptBrushPackMorph, Log, All);

namespace SculptBrushPackMorph
{
namespace Private
{
	using UE::Geometry::FDynamicMesh3;
	using UE::Geometry::FDynamicMeshMorphTargetAttribute;
	using UE::Geometry::FDynamicSubmesh3;
	using UE::Geometry::FMatrix3d;
	using UE::Geometry::FMeshPlanarSymmetry;

	static const FName ProviderName(TEXT("SculptBrushPackMorph"));

	// Clone Morph and Mirror Morph are left out of the brush palette unless this is on. It is read when a tool starts.
	static TAutoConsoleVariable<bool> CVarCloneAndMirror(
		TEXT("SculptBrushPackMorph.CloneAndMirror"), false,
		TEXT("false: Sculpt Morph Target tools that start get Amplify Morph and Smooth Morph. true: they also get Clone Morph and Mirror Morph."));

	/**
	 * The brushes' knowledge of the edited morph, read from the tool. One per tool, shared by its morph brushes.
	 *
	 * A morph target's deltas are kept for the mesh in its reference pose, and the tool shows the mesh posed. What posing
	 * does to a delta at a vertex is a 3 x 3 matrix, the linear part of that vertex's skinning. The engine exports its
	 * un-posing (SkeletalMeshToolsHelper::GetUnposedMesh, SkeletalMeshToolsHelper.cpp:52-119), which for one vertex is
	 * place -> A x place + shift; un-posing the origin and the three unit points gives A, and its inverse is the matrix
	 * wanted. So no skinning is worked out here: the engine's own is asked.
	 *
	 * The pointers kept for a stroke (the tool's mesh, the morph's deltas, the vertex map, the symmetry) are the tool's
	 * and the editor's. They are taken when a stroke begins and used only during it; the tool changes none of them while
	 * a stroke is under way.
	 */
	class FMorphToolContext : public IMorphBrushContext
	{
	public:
		explicit FMorphToolContext(UMorphTargetVertexSculptTool& InTool)
			: Tool(&InTool)
		{
		}

		virtual bool BeginStroke(bool bMirror) override
		{
			ToolMesh = nullptr;
			Isolation = nullptr;
			Edited = nullptr;
			Symmetry = nullptr;
			EditedWeight = 0.0;
			BoneMatrices.Reset();
			Linear.Reset();
			bHasLinear.Reset();
			MirrorOf.Reset();
			bOnPlane.Reset();

			UMorphTargetVertexSculptTool* MorphTool = GetTool();
			// The tool poses its mesh on its first tick; until then there is nothing to read, and nothing to tell the user.
			if (!MorphTool || !HasPosedItsMesh(*MorphTool))
			{
				return false;
			}
			// The tool divides by this weight when the stroke ends, and drops the stroke when it is zero
			// (MorphTargetVertexSculptTool.cpp:485-492, :536). The test is the tool's own.
			const FName EditedName = FMorphToolAccess::MorphName(*MorphTool);
			const float* Weight = FMorphToolAccess::Weights(*MorphTool).Find(EditedName);
			if (!Weight || FMath::IsNearlyZero(*Weight))
			{
				ReportProblem(FText::Format(LOCTEXT("WeightIsNought", "The morph target {0} has a weight of 0, and the tool drops strokes made at that weight. Give it a weight above 0 in the Morph Targets tab."), FText::FromName(EditedName)));
				return false;
			}
			// With its Symmetry on, the tool itself sets the -X side from the +X side after every stamp, at full strength
			// (MeshVertexSculptTool.cpp:1167-1185), which is a mirror of its own and overrides this brush's.
			if (bMirror && FMorphToolAccess::IsSymmetryApplied(*MorphTool))
			{
				ReportProblem(LOCTEXT("SymmetryIsOn", "Mirror Morph does nothing while the tool's Symmetry is on: Symmetry already copies the +X side onto the -X side under every brush. Turn Symmetry off to mirror with this brush."));
				return false;
			}

			// Neither of these is expected of a tool that started: it would not have its morph target or could not pose
			// its mesh. They are logged, as there is nothing the user can do about them in the tool.
			const UDynamicMesh* MeshObject = FMorphToolAccess::ToolMesh(*MorphTool);
			const FDynamicMesh3* Mesh = MeshObject ? MeshObject->GetMeshPtr() : nullptr;
			const FDynamicMeshMorphTargetAttribute* Attribute = Mesh && Mesh->HasAttributes() ? Mesh->Attributes()->GetMorphTargetAttribute(EditedName) : nullptr;
			if (!Attribute || Mesh->Attributes()->GetSkinWeightsAttributes().IsEmpty())
			{
				UE_LOG(LogSculptBrushPackMorph, Warning, TEXT("The morph brushes cannot read the tool's mesh, and move nothing (%s)."), *DescribeMorphTool(MorphTool));
				return false;
			}

			ToolMesh = Mesh;
			Edited = Attribute;
			EditedWeight = *Weight;
			BoneMatrices = SkeletalMeshToolsHelper::ComputeBoneMatrices(FMorphToolAccess::RefPose(*MorphTool), FMorphToolAccess::Pose(*MorphTool));
			// With part of the mesh isolated the sculpt mesh numbers its vertices anew (MorphTargetVertexSculptTool.cpp:519-525).
			Isolation = FMorphToolAccess::Isolation(*MorphTool);
			if (bMirror)
			{
				FindPartners(*MorphTool);
			}
			return true;
		}

		virtual void EndStroke() override
		{
			// These point into the tool and the editor, which may change them once the stroke is over.
			ToolMesh = nullptr;
			Isolation = nullptr;
			Edited = nullptr;
			Symmetry = nullptr;
		}

		virtual bool IsToolSymmetryOn() const override
		{
			UMorphTargetVertexSculptTool* MorphTool = GetTool();
			return MorphTool && FMorphToolAccess::IsSymmetryApplied(*MorphTool);
		}

		virtual const FDynamicMesh3* GetMeshWithoutMorph() override
		{
			UMorphTargetVertexSculptTool* MorphTool = GetTool();
			// Before the tool has posed its mesh, its function below fails an engine check on the two poses' sizes
			// (SkeletalMeshToolsHelper.cpp:223).
			if (!MorphTool || !HasPosedItsMesh(*MorphTool))
			{
				return nullptr;
			}
			const TFunction<const FDynamicMesh3*()>& Function = FMorphToolAccess::WithoutMorph(*MorphTool);
			return Function ? Function() : nullptr;
		}

		virtual void PrepareVertices(const TArray<int32>& Vertices) override
		{
			if (!ToolMesh)
			{
				return;
			}
			int32 Highest = INDEX_NONE;
			for (const int32 Vertex : Vertices)
			{
				Highest = FMath::Max(Highest, Vertex);
			}
			if (bHasLinear.Num() <= Highest)
			{
				Linear.SetNumUninitialized(Highest + 1);
				bHasLinear.SetNumZeroed(Highest + 1);
			}

			TArray<int32> SculptVertices;
			TArray<int32> ToolVertices;
			for (const int32 Vertex : Vertices)
			{
				if (Vertex >= 0 && !bHasLinear[Vertex])
				{
					const int32 ToolVertex = ToToolMesh(Vertex);
					if (ToolMesh->IsVertex(ToolVertex))
					{
						SculptVertices.Add(Vertex);
						ToolVertices.Add(ToolVertex);
					}
				}
			}
			// The engine's helper takes an empty list to mean every vertex.
			if (ToolVertices.IsEmpty())
			{
				return;
			}

			const FVector Points[4] = { FVector::ZeroVector, FVector::XAxisVector, FVector::YAxisVector, FVector::ZAxisVector };
			const TMap<FName, float> NoMorphs;
			TArray<FVector> Unposed[4];
			for (int32 Point = 0; Point < 4; ++Point)
			{
				TArray<FVector>& Out = Unposed[Point];
				Out.SetNumUninitialized(ToolVertices.Num());
				const FVector Place = Points[Point];
				// The helper calls back from worker threads, each with a list index of its own.
				SkeletalMeshToolsHelper::GetUnposedMesh(
					[&Out](SkeletalMeshToolsHelper::FVertInfo Info, const FVector& Position) { Out[Info.VertArrayIndex] = Position; },
					[Place](int32) { return Place; },
					*ToolMesh, BoneMatrices, NAME_None, NoMorphs, ToolVertices);
			}
			for (int32 Index = 0; Index < SculptVertices.Num(); ++Index)
			{
				const FMatrix3d Unpose(Unposed[1][Index] - Unposed[0][Index], Unposed[2][Index] - Unposed[0][Index], Unposed[3][Index] - Unposed[0][Index], /*bRows*/ false);
				// A vertex whose skinning cannot be turned back is treated as not skinned. The limit is the one below which
				// the engine's own inverse fails an ensure (MatrixTypes.h:186-203).
				const double Determinant = Unpose.Determinant();
				const bool bInvertible = FMath::IsFinite(Determinant) && FMath::Abs(Determinant) >= TMathUtil<double>::Epsilon;
				Linear[SculptVertices[Index]] = bInvertible ? Unpose.Inverse() : FMatrix3d::Identity();
				bHasLinear[SculptVertices[Index]] = true;
			}
		}

		virtual bool GetSourceOffset(FName Source, int32 Vertex, FVector3d& OutOffset) const override
		{
			if (!IsPrepared(Vertex))
			{
				return false;
			}
			const FDynamicMeshMorphTargetAttribute* Attribute = ToolMesh->Attributes()->GetMorphTargetAttribute(Source);
			if (!Attribute)
			{
				return false;
			}
			FVector3d Delta;
			Attribute->GetValue(ToToolMesh(Vertex), Delta);
			OutOffset = ShownOffset(Vertex, Delta);
			return true;
		}

		virtual EMorphMirror GetMirroredOffset(int32 Vertex, FVector3d& OutOffset) const override
		{
			const int32 ToolVertex = ToToolMesh(Vertex);
			if (!Symmetry || !IsPrepared(Vertex) || !MirrorOf.IsValidIndex(ToolVertex))
			{
				return EMorphMirror::None;
			}
			const FVector3d Position = ToolMesh->GetVertex(ToolVertex);
			const int32 Partner = MirrorOf[ToolVertex];
			if (Partner != INDEX_NONE)
			{
				FVector3d PartnerDelta;
				Edited->GetValue(Partner, PartnerDelta);
				OutOffset = ShownOffset(Vertex, Symmetry->GetMirroredAxis(PartnerDelta));
				// The editor's symmetry plane is X = 0 (SKMBackedDynaMeshComponent.cpp:1284), and the engine's own mirror
				// takes the side by the same test (SkeletalMeshToolsHelper.cpp:428).
				return Position.X >= 0.0 ? EMorphMirror::PositiveX : EMorphMirror::NegativeX;
			}
			if (bOnPlane[ToolVertex])
			{
				// A point on the plane keeps what lies in the plane, as in the engine's own mirror
				// (SkeletalMeshToolsHelper.cpp:416-427).
				FVector3d Delta;
				Edited->GetValue(ToolVertex, Delta);
				TArray<FVector3d> Constrained = { Position + Delta };
				Symmetry->ApplySymmetryPlaneConstraints({ ToolVertex }, Constrained);
				OutOffset = ShownOffset(Vertex, Constrained[0] - Position);
				return EMorphMirror::OnPlane;
			}
			return EMorphMirror::None;
		}

		virtual TArray<FName> GetSourceNames() const override
		{
			TArray<FName> Names;
			UMorphTargetVertexSculptTool* MorphTool = GetTool();
			const UDynamicMesh* MeshObject = MorphTool ? FMorphToolAccess::ToolMesh(*MorphTool) : nullptr;
			const FDynamicMesh3* Mesh = MeshObject ? MeshObject->GetMeshPtr() : nullptr;
			if (Mesh && Mesh->HasAttributes())
			{
				const FName EditedName = FMorphToolAccess::MorphName(*MorphTool);
				for (const TPair<FName, TUniquePtr<FDynamicMeshMorphTargetAttribute>>& Pair : Mesh->Attributes()->GetMorphTargetAttributes())
				{
					if (Pair.Key != EditedName)
					{
						Names.Add(Pair.Key);
					}
				}
			}
			Names.Sort(FNameLexicalLess());
			return Names;
		}

		virtual void ReportProblem(const FText& Problem) override
		{
			// The editor shows a tool's warning in its panel until another replaces it
			// (SkeletalMeshModelingToolsEditorModeToolkit.cpp:664-668), so only a change is sent, and an empty text takes
			// back a warning of this context's own and no one else's.
			UMorphTargetVertexSculptTool* MorphTool = GetTool();
			UInteractiveToolManager* Manager = MorphTool ? MorphTool->GetToolManager() : nullptr;
			if (Manager && !Problem.EqualTo(LastProblem))
			{
				Manager->DisplayMessage(Problem, EToolMessageLevel::UserWarning);
				LastProblem = Problem;
			}
		}

	private:
		UMorphTargetVertexSculptTool* GetTool() const
		{
			return static_cast<UMorphTargetVertexSculptTool*>(Tool.Get());
		}

		/** Whether the tool has posed the mesh it shows: it then holds a pose of as many bones as the reference pose. */
		static bool HasPosedItsMesh(UMorphTargetVertexSculptTool& MorphTool)
		{
			const int32 Bones = FMorphToolAccess::RefPose(MorphTool).Num();
			return Bones > 0 && FMorphToolAccess::Pose(MorphTool).Num() == Bones;
		}

		int32 ToToolMesh(int32 Vertex) const
		{
			return Isolation ? Isolation->MapVertexToBaseMesh(Vertex) : Vertex;
		}

		bool IsPrepared(int32 Vertex) const
		{
			return ToolMesh && Edited && bHasLinear.IsValidIndex(Vertex) && bHasLinear[Vertex];
		}

		/** The offset a delta kept for the reference pose makes at a vertex of the shown mesh, at the edited morph's weight. */
		FVector3d ShownOffset(int32 Vertex, const FVector3d& Delta) const
		{
			return (Linear[Vertex] * Delta) * EditedWeight;
		}

		/**
		 * Notes, for every vertex of the tool's mesh, its partner across the symmetry plane or that it lies on the plane.
		 * The editor's symmetry of the asset's mesh tells a +X vertex its partner and a -X vertex only that it has one
		 * (MeshPlanarSymmetry.cpp:697-730), so both directions are noted here. It does not say which of the vertices
		 * without a partner lie on the plane, but it moves only those back onto it: a place off the plane is handed in
		 * for each, and the ones that come back changed are on the plane.
		 */
		void FindPartners(UMorphTargetVertexSculptTool& MorphTool)
		{
			USkeletalMeshEditorContextObjectBase* Editor = FMorphToolAccess::Editor(MorphTool);
			Symmetry = Editor ? Editor->GetBaseMeshSymmetry() : nullptr;
			if (!Symmetry)
			{
				return;
			}
			TArray<int32> All;
			All.Reserve(ToolMesh->VertexCount());
			for (const int32 Vertex : ToolMesh->VertexIndicesItr())
			{
				All.Add(Vertex);
			}
			TArray<int32> Partners;
			Symmetry->GetMirrorVertexROI(All, Partners, /*bForceSameSizeWithGaps*/ true);
			MirrorOf.Init(INDEX_NONE, ToolMesh->MaxVertexID());
			for (int32 Index = 0; Index < All.Num() && Index < Partners.Num(); ++Index)
			{
				const int32 Partner = Partners[Index];
				if (Partner != INDEX_NONE && Partner != All[Index] && MirrorOf.IsValidIndex(Partner))
				{
					MirrorOf[All[Index]] = Partner;
					MirrorOf[Partner] = All[Index];
				}
			}

			TArray<int32> Alone;
			TArray<FVector3d> OffPlane;
			const FVector3d AcrossPlane = Symmetry->GetMirroredAxis(FVector3d::UnitX()) - FVector3d::UnitX();
			for (const int32 Vertex : All)
			{
				if (MirrorOf[Vertex] == INDEX_NONE)
				{
					Alone.Add(Vertex);
					OffPlane.Add(ToolMesh->GetVertex(Vertex) + AcrossPlane);
				}
			}
			TArray<FVector3d> Constrained = OffPlane;
			Symmetry->ApplySymmetryPlaneConstraints(Alone, Constrained);
			bOnPlane.Init(false, ToolMesh->MaxVertexID());
			for (int32 Index = 0; Index < Alone.Num(); ++Index)
			{
				bOnPlane[Alone[Index]] = Constrained[Index] != OffPlane[Index];
			}
		}

		TWeakObjectPtr<UMeshVertexSculptTool> Tool;
		/** The warning last shown to the user, empty when none stands. */
		FText LastProblem;

		// For the stroke under way.
		const FDynamicMesh3* ToolMesh = nullptr;
		const FDynamicSubmesh3* Isolation = nullptr;
		const FDynamicMeshMorphTargetAttribute* Edited = nullptr;
		const FMeshPlanarSymmetry* Symmetry = nullptr;
		double EditedWeight = 0.0;
		TArray<FMatrix> BoneMatrices;
		/** Per vertex of the sculpt mesh: what posing does to a delta there, where bHasLinear. */
		TArray<FMatrix3d> Linear;
		TArray<bool> bHasLinear;
		/** Per vertex of the tool's mesh: its partner across the symmetry plane, or INDEX_NONE. */
		TArray<int32> MirrorOf;
		/** Per vertex of the tool's mesh: whether it has no partner because it lies on the plane. */
		TArray<bool> bOnPlane;
	};

	template<typename OpType>
	class TMorphBrushOpFactory : public FMeshSculptBrushOpFactory
	{
	public:
		explicit TMorphBrushOpFactory(TSharedPtr<IMorphBrushContext> InContext)
			: Context(MoveTemp(InContext))
		{
		}

		virtual TUniquePtr<FMeshSculptBrushOp> Build() override
		{
			return MakeUnique<OpType>(Context);
		}

	private:
		TSharedPtr<IMorphBrushContext> Context;
	};

	template<typename OpType, typename SettingsType>
	static SculptBrushPack::FExtraBrush MakeBrush(int32 Identifier, const FText& Name, const FString& IconFile, const TSharedPtr<IMorphBrushContext>& Context)
	{
		SculptBrushPack::FExtraBrush Brush;
		Brush.Identifier = Identifier;
		Brush.Name = Name;
		Brush.IconFile = IconFile;
		Brush.MakeSettings = [](UMeshVertexSculptTool& Tool) -> UMeshSculptBrushOpProps* { return NewObject<SettingsType>(&Tool); };
		Brush.MakeFactory = [Context]() -> TUniquePtr<FMeshSculptBrushOpFactory> { return MakeUnique<TMorphBrushOpFactory<OpType>>(Context); };
		return Brush;
	}

	/** The morph brushes for a tool, when it is Sculpt Morph Target. Icons are the project's own, the nearest in its library. */
	static void AddMorphBrushes(UMeshVertexSculptTool& Tool, TArray<SculptBrushPack::FExtraBrush>& OutBrushes)
	{
		const TSharedPtr<IMorphBrushContext> Context = MakeToolContext(&Tool);
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SculptBrushPackMorph"));
		if (!Context.IsValid() || !Plugin.IsValid())
		{
			return;
		}
		const FString Icons = Plugin->GetBaseDir() / TEXT("Resources/Icons");

		OutBrushes.Add(MakeBrush<FAmplifyMorphBrushOp, USculptBrushPackAmplifyMorphOpProps>(BrushID::AmplifyMorph, LOCTEXT("AmplifyMorph", "AmplifyMorph"), Icons / TEXT("Sculpt_KelvinletScale.svg"), Context));
		OutBrushes.Add(MakeBrush<FSmoothMorphBrushOp, USculptBrushPackSmoothMorphOpProps>(BrushID::SmoothMorph, LOCTEXT("SmoothMorph", "SmoothMorph"), Icons / TEXT("Rig_WeightSmooth.svg"), Context));
		if (!CVarCloneAndMirror.GetValueOnGameThread())
		{
			return;
		}

		SculptBrushPack::FExtraBrush Clone = MakeBrush<FCloneMorphBrushOp, USculptBrushPackCloneMorphOpProps>(BrushID::CloneMorph, LOCTEXT("CloneMorph", "CloneMorph"), Icons / TEXT("Rig_MorphTarget.svg"), Context);
		// The Source setting lists the other morph targets of the mesh, read when the list is opened.
		Clone.MakeSettings = [Context](UMeshVertexSculptTool& ForTool) -> UMeshSculptBrushOpProps*
		{
			USculptBrushPackCloneMorphOpProps* Settings = NewObject<USculptBrushPackCloneMorphOpProps>(&ForTool);
			Settings->SourceNames = [Context]() { return Context->GetSourceNames(); };
			return Settings;
		};
		OutBrushes.Add(MoveTemp(Clone));

		OutBrushes.Add(MakeBrush<FMirrorMorphBrushOp, USculptBrushPackMirrorMorphOpProps>(BrushID::MirrorMorph, LOCTEXT("MirrorMorph", "MirrorMorph"), Icons / TEXT("Rig_WeightMirror.svg"), Context));
	}

	TSharedPtr<IMorphBrushContext> MakeToolContext(UMeshVertexSculptTool* Tool)
	{
		if (!IsMorphSculptTool(Tool))
		{
			return nullptr;
		}
		return MakeShared<FMorphToolContext>(static_cast<UMorphTargetVertexSculptTool&>(*Tool));
	}

	FString DescribeMorphTool(UMeshVertexSculptTool* Tool)
	{
		if (!IsMorphSculptTool(Tool))
		{
			return TEXT("not the Sculpt Morph Target tool");
		}
		UMorphTargetVertexSculptTool& MorphTool = static_cast<UMorphTargetVertexSculptTool&>(*Tool);
		const FName EditedName = FMorphToolAccess::MorphName(MorphTool);
		const TMap<FName, float>& Weights = FMorphToolAccess::Weights(MorphTool);
		const float* Weight = Weights.Find(EditedName);
		const UDynamicMesh* MeshObject = FMorphToolAccess::ToolMesh(MorphTool);
		const FDynamicMesh3* Mesh = MeshObject ? MeshObject->GetMeshPtr() : nullptr;
		return FString::Printf(TEXT("morph %s; weight when posed %s; reference pose of %d bones, posed with %d; %d weights when posed; the tool's mesh has %d morph targets; the tool's symmetry is %s"),
			*EditedName.ToString(), Weight ? *FString::SanitizeFloat(*Weight) : TEXT("unknown"), FMorphToolAccess::RefPose(MorphTool).Num(), FMorphToolAccess::Pose(MorphTool).Num(), Weights.Num(),
			Mesh && Mesh->HasAttributes() ? Mesh->Attributes()->GetMorphTargetAttributes().Num() : 0, FMorphToolAccess::IsSymmetryApplied(MorphTool) ? TEXT("on") : TEXT("off"));
	}
}

TArray<int32> GetBrushIdentifiers()
{
	TArray<int32> Identifiers = { BrushID::AmplifyMorph, BrushID::SmoothMorph };
	if (Private::CVarCloneAndMirror.GetValueOnGameThread())
	{
		Identifiers.Append({ BrushID::CloneMorph, BrushID::MirrorMorph });
	}
	return Identifiers;
}

bool IsMorphSculptTool(const UInteractiveTool* Tool)
{
	// The engine's class is not exported, so it is known by its name and its module's package.
	static const FName ClassName(TEXT("MorphTargetVertexSculptTool"));
	static const FName PackageName(TEXT("/Script/SkeletalMeshMorphTargetEditingTools"));
	const UClass* Class = Tool ? Tool->GetClass() : nullptr;
	return Class && Class->GetFName() == ClassName && Class->GetOutermost()->GetFName() == PackageName && Tool->IsA<UMeshVertexSculptTool>();
}

class FSculptBrushPackMorphModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		SculptBrushPack::AddBrushProvider(Private::ProviderName, &Private::AddMorphBrushes);
	}

	virtual void ShutdownModule() override
	{
		SculptBrushPack::RemoveBrushProvider(Private::ProviderName);
	}
};
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(SculptBrushPackMorph::FSculptBrushPackMorphModule, SculptBrushPackMorph)
