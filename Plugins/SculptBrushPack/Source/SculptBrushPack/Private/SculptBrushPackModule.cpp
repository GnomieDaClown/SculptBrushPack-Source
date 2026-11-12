// Copyright (C) 2026 Joseph Wiginton.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sculpt Brush Pack. The module: it adds the pack's brushes to each Vertex Sculpt tool as the tool starts, wherever the
// tool runs (Modeling Mode in the level editor, the Skeletal Mesh Editor's editing tools).
//
// The engine has no public call that adds a brush to its sculpt tools: UMeshSculptToolBase::RegisterBrushType is protected
// (Sculpting/MeshSculptToolBase.h:387 in 5.8.4), meant for subclasses, and a subclass would need its own tool button in
// every editor. The pack instead listens to the tool managers (UInteractiveToolManager::OnToolPostBuild and
// OnToolPostSetup, InteractiveToolManager.h:360-364) and registers its brushes on the tool itself, through FToolAccess
// below. Engine anchors in this file are for 5.8.4.
//
// Reloading: Live Coding patches the module in place and is safe. If the module is unloaded while a tool that has its
// brushes is running, that tool is cancelled first, as it holds objects whose code lives here.

#include "SculptBrushPack.h"

#include "Brushes/SlateImageBrush.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "HAL/IConsoleManager.h"
#include "InteractiveTool.h"
#include "InteractiveToolManager.h"
#include "Interfaces/IPluginManager.h"
#include "MeshVertexSculptTool.h"
#include "Misc/CoreMisc.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "Runtime/Launch/Resources/Version.h"
#include "SculptBrushPackBrushes.h"
#include "Sculpting/MeshBrushOpBase.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "UObject/UObjectArray.h"
#include "UObject/UObjectIterator.h"

#include <atomic>
#include <type_traits>

#define LOCTEXT_NAMESPACE "SculptBrushPack"

DEFINE_LOG_CATEGORY_STATIC(LogSculptBrushPack, Log, All);

// FToolAccess names protected members of the engine's sculpt tool as they are in 5.8 (read at 5.8.4, changelist
// 58902052). Another engine version needs them read again before this is built for it.
#if ENGINE_MAJOR_VERSION != 5 || ENGINE_MINOR_VERSION != 8
#error "The Sculpt Brush Pack is written against the sculpt tools of Unreal Engine 5.8. See FToolAccess in SculptBrushPackModule.cpp."
#endif

namespace SculptBrushPack
{
namespace Private
{
	static TAutoConsoleVariable<bool> CVarEnabled(
		TEXT("SculptBrushPack.Enabled"), true,
		TEXT("true: sculpt tools that start get the Sculpt Brush Pack's brushes. false: they start as without the plugin."));

	// Brush identifiers below this are left to the engine, whose own are below 20 (MeshVertexSculptTool.h:59-118). The
	// pack's are 2001 and up; a provider's brush with a lower identifier is left out.
	constexpr int32 FirstExtraIdentifier = 1000;

	/** One brush of the pack. Name is the palette's label, and its source string names the icon. */
	struct FPackBrush
	{
		int32 Identifier;
		FText Name;
		const TCHAR* IconFile;
		UClass* (*SettingsClass)();
		TUniquePtr<FMeshSculptBrushOpFactory> (*MakeFactory)();
	};

	template<typename OpType>
	static TUniquePtr<FMeshSculptBrushOpFactory> MakeFactoryOf()
	{
		return MakeUnique<TBasicMeshSculptBrushOpFactory<OpType>>();
	}

	/** The pack's brushes, in palette order. The icons are the nearest ones in the project's icon library. */
	static const TArray<FPackBrush>& GetPackBrushes()
	{
		static const TArray<FPackBrush> Brushes = {
			{ BrushID::Clay, LOCTEXT("Clay", "Clay"), TEXT("Sculpt_Displace.svg"), &USculptBrushPackClayOpProps::StaticClass, &MakeFactoryOf<FClayBrushOp> },
			{ BrushID::ClayStrips, LOCTEXT("ClayStrips", "ClayStrips"), TEXT("Sculpt_ClayStrips.svg"), &USculptBrushPackClayStripsOpProps::StaticClass, &MakeFactoryOf<FClayStripsBrushOp> },
			{ BrushID::Crease, LOCTEXT("Crease", "Crease"), TEXT("Sculpt_CreasePinch.svg"), &USculptBrushPackCreaseOpProps::StaticClass, &MakeFactoryOf<FCreaseBrushOp> },
			{ BrushID::Blob, LOCTEXT("Blob", "Blob"), TEXT("Sculpt_InflateBlob.svg"), &USculptBrushPackBlobOpProps::StaticClass, &MakeFactoryOf<FBlobBrushOp> },
			{ BrushID::DamStandard, LOCTEXT("DamStandard", "DamStandard"), TEXT("Sculpt_KelvinletSharp.svg"), &USculptBrushPackDamStandardOpProps::StaticClass, &MakeFactoryOf<FDamStandardBrushOp> },
			{ BrushID::SnakeHook, LOCTEXT("SnakeHook", "SnakeHook"), TEXT("Sculpt_GrabSnake.svg"), &USculptBrushPackSnakeHookOpProps::StaticClass, &MakeFactoryOf<FSnakeHookBrushOp> },
			{ BrushID::MoveTopological, LOCTEXT("MoveTopo", "MoveTopo"), TEXT("Sculpt_MoveBrush.svg"), &USculptBrushPackMoveTopologicalOpProps::StaticClass, &MakeFactoryOf<FMoveTopologicalBrushOp> },
			{ BrushID::Layer, LOCTEXT("Layer", "Layer"), TEXT("Sculpt_LayerAdd.svg"), &USculptBrushPackLayerOpProps::StaticClass, &MakeFactoryOf<FLayerBrushOp> },
			{ BrushID::Fill, LOCTEXT("Fill", "Fill"), TEXT("Sculpt_FillValley.svg"), &USculptBrushPackFillOpProps::StaticClass, &MakeFactoryOf<FPlaneSideBrushOp> },
			{ BrushID::Scrape, LOCTEXT("Scrape", "Scrape"), TEXT("Sculpt_ScrapePeaks.svg"), &USculptBrushPackScrapeOpProps::StaticClass, &MakeFactoryOf<FPlaneSideBrushOp> },
			{ BrushID::Nudge, LOCTEXT("Nudge", "Nudge"), TEXT("Sculpt_PullKelvin.svg"), &USculptBrushPackNudgeOpProps::StaticClass, &MakeFactoryOf<FNudgeBrushOp> },
			{ BrushID::Thumb, LOCTEXT("Thumb", "Thumb"), TEXT("Sculpt_FlattenTrim.svg"), &USculptBrushPackThumbOpProps::StaticClass, &MakeFactoryOf<FThumbBrushOp> },
			{ BrushID::TopologySlide, LOCTEXT("TopoSlide", "TopoSlide"), TEXT("Sculpt_ResampleBrush.svg"), &USculptBrushPackTopologySlideOpProps::StaticClass, &MakeFactoryOf<FTopologySlideBrushOp> },
			{ BrushID::Relax, LOCTEXT("Relax", "Relax"), TEXT("Sculpt_SmoothCotan.svg"), &USculptBrushPackRelaxOpProps::StaticClass, &MakeFactoryOf<FRelaxBrushOp> },
			{ BrushID::Glide, LOCTEXT("Glide", "Glide"), TEXT("Sculpt_KelvinletGrab.svg"), &USculptBrushPackGlideOpProps::StaticClass, &MakeFactoryOf<FGlideBrushOp> },
			{ BrushID::Tube, LOCTEXT("Tube", "Tube"), TEXT("Sculpt_InflateBrushVariants.svg"), &USculptBrushPackTubeOpProps::StaticClass, &MakeFactoryOf<FTubeBrushOp> },
			{ BrushID::Noise, LOCTEXT("Noise", "Noise"), TEXT("Deform_NoiseDisplace.svg"), &USculptBrushPackNoiseOpProps::StaticClass, &MakeFactoryOf<FNoiseBrushOp> },
			{ BrushID::Sharpen, LOCTEXT("Sharpen", "Sharpen"), TEXT("Sculpt_SculptMax.svg"), &USculptBrushPackSharpenOpProps::StaticClass, &MakeFactoryOf<FSharpenBrushOp> },
			{ BrushID::LegacyWrinkle, LOCTEXT("LegacyWrinkle", "LegacyWrinkle"), TEXT("SubD_Crease.svg"), &USculptBrushPackLegacyWrinkleOpProps::StaticClass, &MakeFactoryOf<FLegacyWrinkleBrushOp> },
		};
		return Brushes;
	}

	/** What other modules add through the pack (AddBrushProvider), in the order they were added. */
	static TArray<TPair<FName, FExtraBrushProvider>>& GetProviders()
	{
		static TArray<TPair<FName, FExtraBrushProvider>> Providers;
		return Providers;
	}

	/** Every identifier the pack has registered on a tool in this editor session: its own brushes' and the providers'. */
	static TSet<int32>& GetRegisteredIdentifiers()
	{
		static TSet<int32> Identifiers;
		return Identifiers;
	}

	/** The type of a data member, from a pointer to it. Declared only: it is used inside decltype. */
	template<typename MemberType, typename ClassType>
	MemberType MemberTypeOf(MemberType ClassType::*);

	/**
	 * Names the sculpt tool's protected members for this module.
	 *
	 * The rule relied on: inside a member of a class derived from the tool, &FToolAccess::Member may name a protected
	 * member of the base class ([class.protected]). The result is a pointer to a member of the base class, and it is
	 * applied to the real tool. No object of this type is ever created and no tool is cast to it, so nothing here depends
	 * on a tool being something it is not; the constructor is deleted to keep it that way. Adding a constructor, a
	 * destructor or a virtual function would make the compiler emit code that needs the base class's own.
	 *
	 * Members used, all of UMeshSculptToolBase unless said otherwise (Sculpting/MeshSculptToolBase.h):
	 *  - RegisterBrushType (:387), called before the tool's Setup. Setup registers the tool's own brushes on top and
	 *    clears none (MeshSculptToolBase.cpp:415-436; MeshVertexSculptTool.cpp:380-460).
	 *  - RegisteredPrimaryBrushTypes, BrushOpFactories and BrushOpPropSets, re-filled after Setup to put the engine's
	 *    brushes first. This relies on TSet and TMap listing their elements in the order they were added while nothing
	 *    is removed in between, which holds for both.
	 *  - ToolPropertyObjects of UInteractiveTool, reordered after Setup and before the editor is told the tool has
	 *    started (InteractiveToolManager.cpp:203-230), so no panel has been built from it yet.
	 *  - GetPropertyCacheIdentifier (:334), the key under which the tool remembers its settings between sessions.
	 *
	 * A member that is renamed, removed or made private fails the build here; one whose type changes fails the checks
	 * in CheckMembers. A public way to register a brush on these tools would make all of this unnecessary.
	 */
	struct FToolAccess final : public UMeshVertexSculptTool
	{
		FToolAccess() = delete;

		static void Register(UMeshVertexSculptTool& Tool, int32 Identifier, const FText& Name, TUniquePtr<FMeshSculptBrushOpFactory> Factory, UMeshSculptBrushOpProps* Settings)
		{
			(Tool.*(&FToolAccess::RegisterBrushType))(Identifier, Name, MoveTemp(Factory), Settings);
		}

		static TSet<FBrushTypeInfo>& BrushTypes(UMeshVertexSculptTool& Tool) { return Tool.*(&FToolAccess::RegisteredPrimaryBrushTypes); }
		static TMap<int32, TUniquePtr<FMeshSculptBrushOpFactory>>& Factories(UMeshVertexSculptTool& Tool) { return Tool.*(&FToolAccess::BrushOpFactories); }
		static TMap<int32, TObjectPtr<UMeshSculptBrushOpProps>>& Settings(UMeshVertexSculptTool& Tool) { return Tool.*(&FToolAccess::BrushOpPropSets); }
		static TArray<TObjectPtr<UObject>>& PanelObjects(UMeshVertexSculptTool& Tool) { return Tool.*(&FToolAccess::ToolPropertyObjects); }
		static FString CacheKey(UMeshVertexSculptTool& Tool) { return (Tool.*(&FToolAccess::GetPropertyCacheIdentifier))(); }

	private:
		static void CheckMembers()
		{
			static_assert(std::is_same_v<decltype(MemberTypeOf(&FToolAccess::RegisteredPrimaryBrushTypes)), TSet<FBrushTypeInfo>>, "The sculpt tool's set of brush types has changed type.");
			static_assert(std::is_same_v<decltype(MemberTypeOf(&FToolAccess::BrushOpFactories)), TMap<int32, TUniquePtr<FMeshSculptBrushOpFactory>>>, "The sculpt tool's map of brush factories has changed type.");
			static_assert(std::is_same_v<decltype(MemberTypeOf(&FToolAccess::BrushOpPropSets)), TMap<int32, TObjectPtr<UMeshSculptBrushOpProps>>>, "The sculpt tool's map of brush settings has changed type.");
			static_assert(std::is_same_v<decltype(MemberTypeOf(&FToolAccess::ToolPropertyObjects)), TArray<TObjectPtr<UObject>>>, "The tool's list of settings objects has changed type.");
		}
	};

	/**
	 * The tool as a Vertex Sculpt tool the pack adds brushes to, or null. These are the classes whose Setup is known to
	 * keep brushes registered before it; other subclasses choose their own brushes for a reason and are left alone.
	 */
	static UMeshVertexSculptTool* AsSupportedTool(UInteractiveTool* Tool)
	{
		UMeshVertexSculptTool* SculptTool = Cast<UMeshVertexSculptTool>(Tool);
		if (!SculptTool)
		{
			return nullptr;
		}
		if (SculptTool->GetClass() == UMeshVertexSculptTool::StaticClass())
		{
			return SculptTool;
		}
		// The engine's Sculpt Morph Target tool, whose class is not exported, and the Vertex Sculpt of the Unreal 3D
		// suite this plugin's repository also holds. Neither is a dependency: they are known by their class paths.
		static const FTopLevelAssetPath Others[] = {
			FTopLevelAssetPath(TEXT("/Script/SkeletalMeshMorphTargetEditingTools"), TEXT("MorphTargetVertexSculptTool")),
			FTopLevelAssetPath(TEXT("/Script/ModelingSuiteEditor"), TEXT("ModelingSuiteVertexSculptTool")),
		};
		const FTopLevelAssetPath ClassPath = SculptTool->GetClass()->GetClassPathName();
		for (const FTopLevelAssetPath& Other : Others)
		{
			if (Other == ClassPath)
			{
				return SculptTool;
			}
		}
		return nullptr;
	}

	/**
	 * The stock brush palette draws the icon "BrushTypeIcons.<brush name>" from Modeling Mode's style set
	 * (MeshVertexSculptToolCustomizations.cpp:330-331), which that module registers as "ModelingToolsStyle" and makes as
	 * an FSlateStyleSet (ModelingToolsEditorModeStyle.cpp:28-32, :58); the cast below rests on that. The pack's icons are
	 * added to that set, found by name so the pack does not depend on Modeling Mode. An icon that is there already is
	 * left as it is, so no engine icon is replaced. The set owns what is added and frees it with its own; nothing is
	 * taken out when this module shuts down.
	 */
	static void AddIcon(const FText& Name, const FString& File)
	{
		const FString* SourceName = FTextInspector::GetSourceString(Name);
		const ISlateStyle* Style = FSlateStyleRegistry::FindSlateStyle(TEXT("ModelingToolsStyle"));
		if (!SourceName || !Style)
		{
			return;
		}
		const FName Key(*(TEXT("BrushTypeIcons.") + *SourceName));
		if (Style->GetOptionalBrush(Key, nullptr, nullptr) == nullptr)
		{
			// The size of the engine's own brush icons (ModelingToolsEditorModeStyle.cpp:50).
			static_cast<FSlateStyleSet*>(const_cast<ISlateStyle*>(Style))->Set(Key, new FSlateVectorImageBrush(File, FVector2D(120.0, 120.0)));
		}
	}

	/**
	 * Builds a brush's operation and, for a brush that works from the surface as it was before the stroke, sees to it
	 * that the tool keeps that surface current.
	 *
	 * The tool builds an operation when its brush becomes the active one (MeshSculptToolBase.cpp:504). While the tool's
	 * Freeze Target is set it does not refresh the surface it hands to such brushes (MeshVertexSculptTool.cpp:1684-1693),
	 * and each of their strokes would then undo what earlier strokes did under it. The setting stays set when another
	 * brush is chosen, and cannot be changed while one of these brushes is active (:2004-2011), so it is cleared here;
	 * the tool refreshes the surface on its next tick (:1457).
	 */
	class FPackBrushOpFactory final : public FMeshSculptBrushOpFactory
	{
	public:
		FPackBrushOpFactory(UMeshVertexSculptTool& InTool, TUniquePtr<FMeshSculptBrushOpFactory> InInner)
			: Tool(&InTool)
			, Inner(MoveTemp(InInner))
		{
		}

		virtual TUniquePtr<FMeshSculptBrushOp> Build() override
		{
			TUniquePtr<FMeshSculptBrushOp> Op = Inner->Build();
			UMeshVertexSculptTool* SculptTool = Tool.Get();
			if (Op.IsValid() && SculptTool && SculptTool->SculptProperties && SculptTool->SculptProperties->bFreezeTarget
				&& Op->GetStampInputMeshType() == FMeshSculptBrushOp::EStampInputMeshType::BaseMesh)
			{
				SculptTool->SculptProperties->bFreezeTarget = false;
				UE_LOG(LogSculptBrushPack, Display, TEXT("Freeze Target was turned off: the brush now active works from the surface as it is before each stroke."));
			}
			return Op;
		}

	private:
		TWeakObjectPtr<UMeshVertexSculptTool> Tool;
		TUniquePtr<FMeshSculptBrushOpFactory> Inner;
	};

	static void Register(UMeshVertexSculptTool& Tool, int32 Identifier, const FText& Name, const FString& IconFile, TUniquePtr<FMeshSculptBrushOpFactory> Factory, UMeshSculptBrushOpProps* Settings, TArray<int32>& Added)
	{
		AddIcon(Name, IconFile);
		FToolAccess::Register(Tool, Identifier, Name, MakeUnique<FPackBrushOpFactory>(Tool, MoveTemp(Factory)), Settings);
		Added.Add(Identifier);
		GetRegisteredIdentifiers().Add(Identifier);
	}

	/**
	 * Before the tool's Setup: the brushes are registered now, so that when Setup activates the brush used last time
	 * (MeshVertexSculptTool.cpp:360) it finds a pack brush if that was one. Returns the identifiers added, in palette
	 * order.
	 */
	static TArray<int32> AddBrushes(UMeshVertexSculptTool& Tool)
	{
		TArray<int32> Added;
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SculptBrushPack"));
		const FString IconFolder = Plugin.IsValid() ? Plugin->GetBaseDir() / TEXT("Resources/Icons") : FString();
		for (const FPackBrush& Brush : GetPackBrushes())
		{
			Register(Tool, Brush.Identifier, Brush.Name, IconFolder / Brush.IconFile, Brush.MakeFactory(), NewObject<UMeshSculptBrushOpProps>(&Tool, Brush.SettingsClass()), Added);
		}

		// Then what other modules add for this tool.
		for (const TPair<FName, FExtraBrushProvider>& Provider : GetProviders())
		{
			TArray<FExtraBrush> Extra;
			Provider.Value(Tool, Extra);
			for (const FExtraBrush& Brush : Extra)
			{
				UMeshSculptBrushOpProps* Settings = nullptr;
				TUniquePtr<FMeshSculptBrushOpFactory> Factory;
				if (Brush.Identifier >= FirstExtraIdentifier && !Added.Contains(Brush.Identifier) && Brush.MakeSettings && Brush.MakeFactory)
				{
					Settings = Brush.MakeSettings(Tool);
					Factory = Brush.MakeFactory();
				}
				if (Settings && Factory.IsValid())
				{
					Register(Tool, Brush.Identifier, Brush.Name, Brush.IconFile, MoveTemp(Factory), Settings, Added);
				}
				else
				{
					UE_LOG(LogSculptBrushPack, Warning, TEXT("The brush %d (%s) from the provider %s was left out: its identifier is below %d or in use, or it gave no settings or no factory."),
						Brush.Identifier, *Brush.Name.ToString(), *Provider.Key.ToString(), FirstExtraIdentifier);
				}
			}
		}
		return Added;
	}

	/**
	 * Before the tool's Setup, for every Vertex Sculpt tool, whether it gets the pack's brushes or not.
	 *
	 * The tools remember the brush used last, under a key that the engine's Vertex Sculpt and its morph sculpt tool share
	 * (MeshVertexSculptTool.cpp:287, :468-471), and Setup activates it. A tool asked for a brush it does not have fails
	 * an ensure and falls back to the first brush it knows (MeshSculptToolBase.cpp:489-496; MeshVertexSculptTool.cpp:1974-1981).
	 * That would happen whenever the brush remembered is one the pack registered on another tool, or registered before it
	 * was switched off. So when the remembered brush is the pack's or a provider's and this tool is not getting it, the
	 * engine's Move is remembered in its place: every one of these tools has it.
	 */
	static void ForgetBrushThisToolLacks(UMeshVertexSculptTool& Tool, const TArray<int32>& Added)
	{
		UVertexBrushSculptProperties* Remembered = NewObject<UVertexBrushSculptProperties>(GetTransientPackage());
		const FString CacheKey = FToolAccess::CacheKey(Tool);
		Remembered->RestoreProperties(&Tool, CacheKey);
		if (GetRegisteredIdentifiers().Contains(Remembered->PrimaryBrushID) && !Added.Contains(Remembered->PrimaryBrushID))
		{
			Remembered->PrimaryBrushID = static_cast<int32>(EMeshVertexSculptBrushType::Move);
			Remembered->SaveProperties(&Tool, CacheKey);
		}
	}

	/**
	 * After the tool's Setup, which registered the tool's own brushes after the pack's: the engine's brushes come first
	 * again, in the palette (it lists the brush types in their set's order) and among the factories (the tool falls back
	 * to its first one when it is later asked for a brush it does not have, MeshVertexSculptTool.cpp:1976-1979), and the
	 * pack's settings sit with the other brush settings in the panel instead of above everything.
	 */
	static void PutBrushesLast(UMeshVertexSculptTool& Tool, const TArray<int32>& Added)
	{
		TSet<UMeshSculptToolBase::FBrushTypeInfo>& Types = FToolAccess::BrushTypes(Tool);
		TArray<UMeshSculptToolBase::FBrushTypeInfo> Ordered;
		for (const UMeshSculptToolBase::FBrushTypeInfo& Type : Types)
		{
			if (!Added.Contains(Type.Identifier))
			{
				Ordered.Add(Type);
			}
		}
		const int32 EngineBrushes = Ordered.Num();
		for (const int32 Identifier : Added)
		{
			for (const UMeshSculptToolBase::FBrushTypeInfo& Type : Types)
			{
				if (Type.Identifier == Identifier)
				{
					Ordered.Add(Type);
				}
			}
		}
		// Registering before Setup rests on Setup keeping what was registered. If a later engine clears it, say so.
		if (Ordered.Num() - EngineBrushes != Added.Num())
		{
			UE_LOG(LogSculptBrushPack, Error, TEXT("%s kept %d of the %d brushes registered before its Setup; the Sculpt Brush Pack needs updating for this engine."),
				*Tool.GetClass()->GetName(), Ordered.Num() - EngineBrushes, Added.Num());
		}
		Types.Reset();
		Types.Append(Ordered);

		TMap<int32, TUniquePtr<FMeshSculptBrushOpFactory>>& Factories = FToolAccess::Factories(Tool);
		TArray<TPair<int32, TUniquePtr<FMeshSculptBrushOpFactory>>> Taken;
		for (TPair<int32, TUniquePtr<FMeshSculptBrushOpFactory>>& Pair : Factories)
		{
			Taken.Emplace(Pair.Key, MoveTemp(Pair.Value));
		}
		Factories.Reset();
		for (const bool bPack : { false, true })
		{
			for (TPair<int32, TUniquePtr<FMeshSculptBrushOpFactory>>& Pair : Taken)
			{
				if (Pair.Value.IsValid() && Added.Contains(Pair.Key) == bPack)
				{
					Factories.Add(Pair.Key, MoveTemp(Pair.Value));
				}
			}
		}

		// The panel shows the settings objects in this array's order; only the active brush's are visible.
		TArray<TObjectPtr<UObject>>& PanelObjects = FToolAccess::PanelObjects(Tool);
		const TMap<int32, TObjectPtr<UMeshSculptBrushOpProps>>& Settings = FToolAccess::Settings(Tool);
		TArray<UObject*> PackSettings;
		int32 LastEngineSettings = INDEX_NONE;
		for (const int32 Identifier : Added)
		{
			if (const TObjectPtr<UMeshSculptBrushOpProps>* Found = Settings.Find(Identifier))
			{
				PackSettings.Add(Found->Get());
			}
		}
		for (UObject* Object : PackSettings)
		{
			PanelObjects.Remove(Object);
		}
		for (const TPair<int32, TObjectPtr<UMeshSculptBrushOpProps>>& Pair : Settings)
		{
			if (!Added.Contains(Pair.Key))
			{
				LastEngineSettings = FMath::Max(LastEngineSettings, PanelObjects.IndexOfByKey(static_cast<UObject*>(Pair.Value.Get())));
			}
		}
		const int32 InsertAt = LastEngineSettings == INDEX_NONE ? PanelObjects.Num() : LastEngineSettings + 1;
		for (int32 Index = 0; Index < PackSettings.Num(); ++Index)
		{
			PanelObjects.Insert(PackSettings[Index], InsertAt + Index);
		}
	}

	/**
	 * Listens to tool managers and adds the brushes to the sculpt tools they start.
	 *
	 * An editor makes its tool manager when its tools are switched on, and nothing announces that to other modules. The
	 * watcher is told of every object the engine creates and notes when one is a tool manager; on the next frame it
	 * starts listening to the managers it does not listen to yet. By hand, a tool can only be started a frame or more
	 * after its manager was made. Code can do both in one frame: for the level editor's modes the watcher listens at
	 * once when a mode is entered, and for other editors such code calls WatchToolManagers in between.
	 */
	class FToolWatcher final : public TSharedFromThis<FToolWatcher>, public FUObjectArray::FUObjectCreateListener
	{
	public:
		FToolWatcher()
		{
			GUObjectArray.AddUObjectCreateListener(this);
			bListeningForObjects = true;
		}

		virtual ~FToolWatcher() override
		{
			if (bListeningForObjects)
			{
				GUObjectArray.RemoveUObjectCreateListener(this);
			}
		}

		// Called for every object, from whichever thread creates it, before the object is constructed: only a note is made.
		virtual void NotifyUObjectCreated(const UObjectBase* Object, int32 Index) override
		{
			if (Object->GetClass()->IsChildOf(UInteractiveToolManager::StaticClass()))
			{
				bNewManager = true;
			}
		}

		virtual void OnUObjectArrayShutdown() override
		{
			GUObjectArray.RemoveUObjectCreateListener(this);
			bListeningForObjects = false;
		}

		void WatchAll()
		{
			bNewManager = false;
			Watched.RemoveAll([](const TWeakObjectPtr<UInteractiveToolManager>& Manager) { return !Manager.IsValid(); });
			for (TObjectIterator<UInteractiveToolManager> It; It; ++It)
			{
				UInteractiveToolManager* Manager = *It;
				if (!IsValid(Manager) || Manager->IsTemplate() || Watched.Contains(Manager))
				{
					continue;
				}
				Watched.Add(Manager);
				Manager->OnToolPostBuild.AddSP(this, &FToolWatcher::OnToolPostBuild);
				Manager->OnToolPostSetup.AddSP(this, &FToolWatcher::OnToolPostSetup);
			}
		}

		void OnLevelEditorModeChanged(const FEditorModeID& ModeID, bool bIsEntering)
		{
			// A mode that enters has made its tool manager by now (EditorModeManager.cpp:986-990).
			if (bIsEntering)
			{
				WatchAll();
			}
		}

		bool Tick(float DeltaTime)
		{
			if (bNewManager)
			{
				WatchAll();
			}
			return true;
		}

		/** Cancels the running tools that have the pack's brushes: their brushes' code is about to go. */
		void CancelToolsWithBrushes()
		{
			for (const TWeakObjectPtr<UInteractiveToolManager>& Manager : Watched)
			{
				UInteractiveTool* Tool = Manager.IsValid() ? Manager->GetActiveTool(EToolSide::Left) : nullptr;
				if (UMeshVertexSculptTool* SculptTool = AsSupportedTool(Tool))
				{
					UE_LOG(LogSculptBrushPack, Display, TEXT("%s is cancelled: the Sculpt Brush Pack is being unloaded."), *SculptTool->GetClass()->GetName());
					Manager->DeactivateTool(EToolSide::Left, EToolShutdownType::Cancel);
				}
			}
		}

	private:
		void OnToolPostBuild(UInteractiveToolManager* Manager, EToolSide Side, UInteractiveTool* Tool, UInteractiveToolBuilder* Builder, const FToolBuilderState& State)
		{
			UMeshVertexSculptTool* SculptTool = Cast<UMeshVertexSculptTool>(Tool);
			if (!SculptTool)
			{
				return;
			}
			TArray<int32> Added;
			if (CVarEnabled.GetValueOnGameThread() && AsSupportedTool(SculptTool))
			{
				Added = AddBrushes(*SculptTool);
				Pending.Emplace(SculptTool, Added);
			}
			ForgetBrushThisToolLacks(*SculptTool, Added);
		}

		void OnToolPostSetup(UInteractiveToolManager* Manager, EToolSide Side, UInteractiveTool* Tool)
		{
			UMeshVertexSculptTool* SculptTool = Cast<UMeshVertexSculptTool>(Tool);
			const int32 Index = SculptTool ? Pending.IndexOfByPredicate([SculptTool](const FPendingTool& Entry) { return Entry.Key == SculptTool; }) : INDEX_NONE;
			if (Index != INDEX_NONE)
			{
				const TArray<int32> Added = MoveTemp(Pending[Index].Value);
				Pending.RemoveAt(Index);
				PutBrushesLast(*SculptTool, Added);
				UE_LOG(LogSculptBrushPack, Log, TEXT("Added %d brushes to %s."), Added.Num(), *SculptTool->GetClass()->GetName());
			}
			Pending.RemoveAll([](const FPendingTool& Entry) { return !Entry.Key.IsValid(); });
		}

		TArray<TWeakObjectPtr<UInteractiveToolManager>> Watched;
		/** Tools that got the brushes before their Setup and have not finished it yet, each with the identifiers added. */
		using FPendingTool = TPair<TWeakObjectPtr<UMeshVertexSculptTool>, TArray<int32>>;
		TArray<FPendingTool> Pending;
		/** Set when a tool manager has been created that is not listened to yet. Starts set, for the ones there already. */
		std::atomic<bool> bNewManager{ true };
		bool bListeningForObjects = false;
	};

	static TSharedPtr<FToolWatcher> GToolWatcher;
}

TArray<int32> GetBrushIdentifiers()
{
	TArray<int32> Identifiers;
	for (const Private::FPackBrush& Brush : Private::GetPackBrushes())
	{
		Identifiers.Add(Brush.Identifier);
	}
	return Identifiers;
}

bool IsSupportedTool(const UInteractiveTool* Tool)
{
	return Private::AsSupportedTool(const_cast<UInteractiveTool*>(Tool)) != nullptr;
}

void WatchToolManagers()
{
	check(IsInGameThread());
	if (Private::GToolWatcher.IsValid())
	{
		Private::GToolWatcher->WatchAll();
	}
}

void AddBrushProvider(FName ProviderName, FExtraBrushProvider Provider)
{
	check(IsInGameThread());
	RemoveBrushProvider(ProviderName);
	if (Provider)
	{
		Private::GetProviders().Emplace(ProviderName, MoveTemp(Provider));
	}
}

void RemoveBrushProvider(FName ProviderName)
{
	check(IsInGameThread());
	Private::GetProviders().RemoveAll([ProviderName](const TPair<FName, FExtraBrushProvider>& Entry) { return Entry.Key == ProviderName; });
}

class FSculptBrushPackModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		if (!GIsEditor || IsRunningCommandlet())
		{
			return;
		}
		Private::GToolWatcher = MakeShared<Private::FToolWatcher>();
		// Every frame, and it does nothing unless a tool manager was created since the last one.
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(Private::GToolWatcher.ToSharedRef(), &Private::FToolWatcher::Tick));
		// The level editor's mode manager exists once the editor has started (UnrealEdGlobals.cpp:278-284, and the event
		// at the end of that start-up, UnrealEdMisc.cpp:619); asking for it earlier fails an engine check
		// (UnrealEdGlobals.cpp:193-200). A module loaded after that start-up does without this.
		EditorInitializedHandle = FEditorDelegates::OnEditorInitialized.AddRaw(this, &FSculptBrushPackModule::OnEditorInitialized);
		UE_LOG(LogSculptBrushPack, Log, TEXT("Sculpt Brush Pack started: %d brushes for the Vertex Sculpt tools."), Private::GetPackBrushes().Num());
	}

	virtual void ShutdownModule() override
	{
		if (!Private::GToolWatcher.IsValid())
		{
			return;
		}
		FEditorDelegates::OnEditorInitialized.Remove(EditorInitializedHandle);
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		// When the editor exits, its tools have ended already and its tool managers may be half gone; they are not touched.
		if (!IsEngineExitRequested())
		{
			Private::GToolWatcher->CancelToolsWithBrushes();
		}
		// The level editor's and the tool managers' delegates are bound through the watcher and stop firing once it is
		// gone; the level editor's mode manager may be gone already when the editor shuts down, so it is not touched.
		Private::GToolWatcher.Reset();
	}

private:
	void OnEditorInitialized(double Duration)
	{
		if (Private::GToolWatcher.IsValid())
		{
			GLevelEditorModeTools().OnEditorModeIDChanged().AddSP(Private::GToolWatcher.ToSharedRef(), &Private::FToolWatcher::OnLevelEditorModeChanged);
		}
	}

	FDelegateHandle EditorInitializedHandle;
	FTSTicker::FDelegateHandle TickerHandle;
};
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(SculptBrushPack::FSculptBrushPackModule, SculptBrushPack)
