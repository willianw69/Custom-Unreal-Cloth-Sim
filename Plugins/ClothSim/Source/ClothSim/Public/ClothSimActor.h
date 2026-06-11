// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ClothSimActor.generated.h"

class UClothSimComponent;

/** Convenience Actor: drag into a level to get a GPU-simulated cloth grid. */
UCLASS()
class CLOTHSIM_API AClothSimActor : public AActor
{
	GENERATED_BODY()

public:
	AClothSimActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "ClothSim")
	TObjectPtr<UClothSimComponent> ClothSim;
};
