// Copyright Epic Games, Inc. All Rights Reserved.

#include "ClothSimActor.h"
#include "ClothSimComponent.h"

AClothSimActor::AClothSimActor()
{
	PrimaryActorTick.bCanEverTick = false;

	ClothSim = CreateDefaultSubobject<UClothSimComponent>(TEXT("ClothSim"));
	RootComponent = ClothSim;
}
