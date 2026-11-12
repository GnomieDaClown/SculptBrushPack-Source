// Copyright (C) 2026 Joseph Wiginton.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sculpt Brush Pack. The settings every brush of the pack has. Public because a brush that another module hands to the
// pack (SculptBrushPack.h, AddBrushProvider) may use it as the base of its own settings.

#pragma once

#include "CoreMinimal.h"
#include "Sculpting/MeshBrushOpBase.h"
#include "SculptBrushPackSettings.generated.h"

namespace SculptBrushPack
{
	/**
	 * Whether a stroke type is one of the three the engine has in 5.8.4 (EMeshSculptStrokeType). A brush that works with
	 * all of them names them through this, so that a type a later engine adds is not claimed without having been tried.
	 */
	inline bool IsKnownStrokeType(EMeshSculptStrokeType StrokeType)
	{
		return StrokeType == EMeshSculptStrokeType::Spacing || StrokeType == EMeshSculptStrokeType::Airbrush || StrokeType == EMeshSculptStrokeType::Dots;
	}
}

/** Strength and falloff, as every brush of the pack has them. */
UCLASS(Abstract)
class SCULPTBRUSHPACK_API USculptBrushPackOpProps : public UMeshSculptBrushOpProps
{
	GENERATED_BODY()
public:
	/** How strongly the brush acts. With a pen, pressure scales it. */
	UPROPERTY(EditAnywhere, Category = Brush, meta = (DisplayName = "Strength", UIMin = "0.0", UIMax = "1.0", ClampMin = "0.0", ClampMax = "1.0", ModelingQuickEdit, ModelingQuickSettings = 200))
	float Strength = 0.5f;

	/** How much of the brush, from its rim inward, fades out: 0 is full strength to the rim, 1 fades from the middle. */
	UPROPERTY(EditAnywhere, Category = Brush, meta = (DisplayName = "Falloff", UIMin = "0.0", UIMax = "1.0", ClampMin = "0.0", ClampMax = "1.0"))
	float Falloff = 1.0f;

	virtual float GetStrength() override { return Strength; }
	virtual void SetStrength(float NewStrength) override { Strength = FMath::Clamp(NewStrength, 0.0f, 1.0f); }
	virtual float GetFalloff() override { return Falloff; }
	virtual void SetFalloff(float NewFalloff) override { Falloff = FMath::Clamp(NewFalloff, 0.0f, 1.0f); }
	virtual bool SupportsStrengthPressure() override { return true; }
};
