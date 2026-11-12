// Copyright (C) 2026 Joseph Wiginton.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sculpt Brush Pack: Morph Targets. Names the engine's morph sculpt tool's protected members for this module.
//
// What the brushes need to know of the edited morph target is that tool's own state, and the tool offers no public way
// to read it (MorphTargetVertexSculptTool.h:97-140 in 5.8.4). See FMorphToolAccess below for the rule relied on, the
// members read and what each is taken to mean.

#pragma once

#include "CoreMinimal.h"
#include "DynamicSubmesh3.h"
#include "MorphTargetVertexSculptTool.h"
#include "Runtime/Launch/Resources/Version.h"
#include "SculptBrushPackMorphBrushes.h"
#include "SkeletalMesh/SkeletalMeshEditingInterface.h"
#include "UDynamicMesh.h"

#include <type_traits>

// The members named below are those of the engine's morph sculpt tool in 5.8 (read at 5.8.4, changelist 58902052). The
// tool belongs to a plugin the engine marks experimental, and its class is not exported: its members can change in any
// release, and a binary of this plugin built for one engine build must not be loaded in another.
#if ENGINE_MAJOR_VERSION != 5 || ENGINE_MINOR_VERSION != 8
#error "Sculpt Brush Pack: Morph Targets is written against the morph sculpt tool of Unreal Engine 5.8. See FMorphToolAccess."
#endif

namespace SculptBrushPackMorph::Private
{
	/**
	 * What the brushes read of the morph target the tool edits, or null when the tool is not Sculpt Morph Target. The
	 * brushes of a tool share one, made when the tool starts; tests ask for one of their own.
	 */
	TSharedPtr<IMorphBrushContext> MakeToolContext(UMeshVertexSculptTool* Tool);

	/**
	 * What the tool holds now of the morph it edits, in words: the morph's name and weight, whether the tool has posed its
	 * mesh, how many bones and morph targets it knows. For the log and for a test's report.
	 */
	FString DescribeMorphTool(UMeshVertexSculptTool* Tool);

	/** The type of a data member, from a pointer to it. Declared only: it is used inside decltype. */
	template<typename MemberType, typename ClassType>
	MemberType MemberTypeOf(MemberType ClassType::*);

	/**
	 * Names the morph sculpt tool's protected members.
	 *
	 * The rule relied on: inside a member of a class derived from the tool, &FMorphToolAccess::Member may name a
	 * protected member of the base class ([class.protected]). The result is a pointer to a member of the base class, and
	 * it is applied to the real tool. No object of this type is ever created and no tool is cast to it; the constructor
	 * is deleted to keep it that way. The tool's class is not exported, so this links only while the compiler has no
	 * reason to emit a constructor, a destructor or a virtual table for this type: none may be added.
	 *
	 * Members read, and what each is taken to mean (MorphTargetVertexSculptTool.h and .cpp):
	 *  - GetMeshWithoutCurrentMorphFunc: gives the shown mesh without the edited morph, in the sculpt mesh's vertex
	 *    numbering. Set when the tool registers its brushes (.cpp:327-334); valid once the tool has posed its mesh.
	 *  - ToolDynamicMesh: the tool's copy of the asset's mesh in its reference pose, with every morph target's deltas
	 *    as attributes. The tool writes the edited morph's deltas into it when a stroke ends (.cpp:513-556).
	 *  - ToolMorphTargetName: the morph target being edited.
	 *  - MorphTargetWeightsForPosedMesh and ComponentSpaceTransformsForPosedMesh: the weights and the pose the shown
	 *    mesh was last posed with; both are empty until the tool's first tick (.cpp:719-765).
	 *  - ComponentSpaceTransformsRefPose: the reference pose, as many bones as the pose above once that is set.
	 *  - IsolationSubmesh: the map between the sculpt mesh's vertices and the tool mesh's when part of the mesh is
	 *    isolated (.cpp:519-525, :733-751).
	 *  - EditorContext: the editor's context object, asked for the symmetry of the asset's mesh.
	 *  - bApplySymmetry of UMeshVertexSculptTool (MeshVertexSculptTool.h:499): whether the tool's Symmetry is in force.
	 *
	 * A member that is renamed, removed or made private fails the build here; one whose type changes fails the checks
	 * in CheckMembers. What a member means cannot be checked: it has to be read again for another engine version.
	 */
	struct FMorphToolAccess final : public UMorphTargetVertexSculptTool
	{
		FMorphToolAccess() = delete;

		static const TFunction<const FDynamicMesh3*()>& WithoutMorph(UMorphTargetVertexSculptTool& Tool) { return Tool.*(&FMorphToolAccess::GetMeshWithoutCurrentMorphFunc); }
		static const UDynamicMesh* ToolMesh(UMorphTargetVertexSculptTool& Tool) { return (Tool.*(&FMorphToolAccess::ToolDynamicMesh)).Get(); }
		static FName MorphName(UMorphTargetVertexSculptTool& Tool) { return Tool.*(&FMorphToolAccess::ToolMorphTargetName); }
		static const TMap<FName, float>& Weights(UMorphTargetVertexSculptTool& Tool) { return Tool.*(&FMorphToolAccess::MorphTargetWeightsForPosedMesh); }
		static const TArray<FTransform>& Pose(UMorphTargetVertexSculptTool& Tool) { return Tool.*(&FMorphToolAccess::ComponentSpaceTransformsForPosedMesh); }
		static const TArray<FTransform>& RefPose(UMorphTargetVertexSculptTool& Tool) { return Tool.*(&FMorphToolAccess::ComponentSpaceTransformsRefPose); }
		static const UE::Geometry::FDynamicSubmesh3* Isolation(UMorphTargetVertexSculptTool& Tool) { return (Tool.*(&FMorphToolAccess::IsolationSubmesh)).GetPtrOrNull(); }
		static USkeletalMeshEditorContextObjectBase* Editor(UMorphTargetVertexSculptTool& Tool) { return (Tool.*(&FMorphToolAccess::EditorContext)).Get(); }
		static bool IsSymmetryApplied(UMorphTargetVertexSculptTool& Tool) { return Tool.*(&FMorphToolAccess::bApplySymmetry); }

	private:
		static void CheckMembers()
		{
			static_assert(std::is_same_v<decltype(MemberTypeOf(&FMorphToolAccess::GetMeshWithoutCurrentMorphFunc)), TFunction<const FDynamicMesh3*()>>, "The morph sculpt tool's function for the mesh without the morph has changed type.");
			static_assert(std::is_same_v<decltype(MemberTypeOf(&FMorphToolAccess::ToolDynamicMesh)), TObjectPtr<UDynamicMesh>>, "The morph sculpt tool's mesh has changed type.");
			static_assert(std::is_same_v<decltype(MemberTypeOf(&FMorphToolAccess::ToolMorphTargetName)), FName>, "The morph sculpt tool's morph target name has changed type.");
			static_assert(std::is_same_v<decltype(MemberTypeOf(&FMorphToolAccess::MorphTargetWeightsForPosedMesh)), TMap<FName, float>>, "The morph sculpt tool's weights have changed type.");
			static_assert(std::is_same_v<decltype(MemberTypeOf(&FMorphToolAccess::ComponentSpaceTransformsForPosedMesh)), TArray<FTransform>>, "The morph sculpt tool's pose has changed type.");
			static_assert(std::is_same_v<decltype(MemberTypeOf(&FMorphToolAccess::ComponentSpaceTransformsRefPose)), TArray<FTransform>>, "The morph sculpt tool's reference pose has changed type.");
			static_assert(std::is_same_v<decltype(MemberTypeOf(&FMorphToolAccess::IsolationSubmesh)), TOptional<UE::Geometry::FDynamicSubmesh3>>, "The morph sculpt tool's isolation map has changed type.");
			static_assert(std::is_same_v<decltype(MemberTypeOf(&FMorphToolAccess::EditorContext)), TWeakObjectPtr<USkeletalMeshEditorContextObjectBase>>, "The morph sculpt tool's editor context has changed type.");
			static_assert(std::is_same_v<decltype(MemberTypeOf(&FMorphToolAccess::bApplySymmetry)), bool>, "The sculpt tool's symmetry flag has changed type.");
		}
	};
}
