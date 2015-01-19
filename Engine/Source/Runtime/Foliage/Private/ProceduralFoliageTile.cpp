// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "FoliagePrivate.h"
#include "ProceduralFoliageTile.h"
#include "ProceduralFoliage.h"
#include "ProceduralFoliageBroadphase.h"
#include "ProceduralFoliageBlockingVolume.h"
#include "InstancedFoliageActor.h"

#define LOCTEXT_NAMESPACE "ProceduralFoliage"

UProceduralFoliageTile::UProceduralFoliageTile(const FObjectInitializer& ObjectInitializer)
: Super(ObjectInitializer)
{
}


bool UProceduralFoliageTile::HandleOverlaps(FProceduralFoliageInstance* Instance)
{
	//We now check what the instance overlaps. If any of its overlaps dominate we remove the instance and leave everything else alone.
	//If the instance survives we mark all dominated overlaps as pending removal. They will be removed from the broadphase and will not spread seeds or age.
	//Note that this introduces potential indeterminism! If the iteration order changes we could get different results. This is needed because it gives us huge performance savings.
	//Note that if the underlying data structures stay the same (i.e. no core engine changes) this should not matter. This gives us short term determinism, but not long term.

	bool bSurvived = true;
	TArray<FProceduralFoliageOverlap> Overlaps;
	Broadphase.GetOverlaps(Instance, Overlaps);

	//Check if the instance survives
	for (const FProceduralFoliageOverlap& Overlap : Overlaps)
	{
		FProceduralFoliageInstance* Dominated = FProceduralFoliageInstance::Domination(Overlap.A, Overlap.B, Overlap.OverlapType);
		if (Dominated == Instance)
		{
			bSurvived = false;
			break;
		}
	}

	if (bSurvived)
	{
		for (const FProceduralFoliageOverlap& Overlap : Overlaps)
		{
			if (FProceduralFoliageInstance* Dominated = FProceduralFoliageInstance::Domination(Overlap.A, Overlap.B, Overlap.OverlapType))
			{
				check(Dominated != Instance);	//we shouldn't be here if we survived
				MarkPendingRemoval(Dominated);	//We can't immediately remove because we're potentially iterating over existing instances.
			}
		}
	}
	else
	{
		//didn't survive so just die
		MarkPendingRemoval(Instance);
	}

	return bSurvived;
}

FProceduralFoliageInstance* UProceduralFoliageTile::NewSeed(const FVector& Location, float Scale, const UFoliageType_InstancedStaticMesh* Type, float InAge, bool bBlocker)
{
	const float InitRadius = Type->GetMaxRadius() * Scale;
	{
		FProceduralFoliageInstance* NewInst = new FProceduralFoliageInstance();
		NewInst->Location = Location;
		NewInst->Rotation = FQuat(FVector(0, 0, 1), RandomStream.FRandRange(0, 2.f*PI));
		NewInst->Age = InAge;
		NewInst->Type = Type;
		NewInst->Normal = FVector(0, 0, 1);
		NewInst->Scale = Scale;
		NewInst->bBlocker = bBlocker;
		
	

		Broadphase.Insert(NewInst);
		const bool bSurvived = HandleOverlaps(NewInst);
		return bSurvived ? NewInst : nullptr;
	}

	return nullptr;
}

float GetSeedMinDistance(const FProceduralFoliageInstance* Instance, const float NewInstanceAge, const int32 SimulationStep)
{
	const UFoliageType_InstancedStaticMesh* Type = Instance->Type;
	const int32 StepsLeft = Type->MaxAge - SimulationStep;
	const float InstanceMaxAge = Type->GetNextAge(Instance->Age, StepsLeft);
	const float NewInstanceMaxAge = Type->GetNextAge(NewInstanceAge, StepsLeft);

	const float InstanceMaxScale = Type->GetScaleForAge(InstanceMaxAge);
	const float NewInstanceMaxScale = Type->GetScaleForAge(NewInstanceMaxAge);

	const float InstanceMaxRadius = InstanceMaxScale * Type->GetMaxRadius();
	const float NewInstanceMaxRadius = NewInstanceMaxScale * Type->GetMaxRadius();

	return InstanceMaxRadius + NewInstanceMaxRadius;
}

/** Generates a random number with a normal distribution with mean=0 and variance = 1. Uses Box-Muller transformation http://mathworld.wolfram.com/Box-MullerTransformation.html */
float UProceduralFoliageTile::GetRandomGaussian()
{
	const float Rand1 = FMath::Max<float>(RandomStream.FRand(), SMALL_NUMBER);
	const float Rand2 = FMath::Max<float>(RandomStream.FRand(), SMALL_NUMBER);
	const float SqrtLn = FMath::Sqrt(-2.f * FMath::Loge(Rand1));
	const float Rand2TwoPi = Rand2 * 2.f * PI;
	const float Z1 = SqrtLn * FMath::Cos(Rand2TwoPi);
	return Z1;
}

FVector UProceduralFoliageTile::GetSeedOffset(const UFoliageType_InstancedStaticMesh* Type, float MinDistance)
{
	//We want 10% of seeds to be the max distance so we use a z score of +- 1.64
	const float MaxZScore = 1.64f;
	const float Z1 = GetRandomGaussian();
	const float Z1Clamped = FMath::Clamp(Z1, -MaxZScore, MaxZScore);
	const float VariationDistance = Z1Clamped * Type->SpreadVariance / MaxZScore;
	const float AverageDistance = MinDistance + Type->AverageSpreadDistance;
	
	const float RandRad = FMath::Max<float>(RandomStream.FRand(), SMALL_NUMBER) * PI * 2.f;
	const FVector Dir = FVector(FMath::Cos(RandRad), FMath::Sin(RandRad), 0);
	return Dir * (AverageDistance + VariationDistance);
}

void UProceduralFoliageTile::AgeSeeds()
{
	TArray<FProceduralFoliageInstance*> NewSeeds;
	for (FProceduralFoliageInstance* Instance : Instances)
	{
		if (Instance->IsAlive())
		{
			const UFoliageType_InstancedStaticMesh* Type = Instance->Type;
			if (SimulationStep <= Type->NumSteps)
			{
				const float CurrentAge = Instance->Age;
				const float NewAge = Type->GetNextAge(Instance->Age, 1);
				const float NewScale = Type->GetScaleForAge(NewAge);

				const FVector Location = Instance->Location;

				MarkPendingRemoval(Instance);
				if (FProceduralFoliageInstance* Inst = NewSeed(Location, NewScale, Type, NewAge))
				{
					NewSeeds.Add(Inst);
				}
			}
		}
	}

	for (FProceduralFoliageInstance* Seed : NewSeeds)
	{
		Instances.Add(Seed);
	}

	FlushPendingRemovals();
	
}

void UProceduralFoliageTile::SpreadSeeds(TArray<FProceduralFoliageInstance*>& NewSeeds)
{
	for (FProceduralFoliageInstance* Inst : Instances)
	{
		if (Inst->IsAlive() == false)	//The instance has been killed so don't bother spreading seeds. Note this introduces potential indeterminism if the order of instance traversal changes (implementation details of TSet for example)
		{
			continue;
		}

		const UFoliageType_InstancedStaticMesh* Type = Inst->Type;

		if (SimulationStep <= Type->NumSteps)
		{
			for (int32 i = 0; i < Type->SeedsPerStep; ++i)
			{
				//spread new seeds
				const float NewAge = Type->GetInitAge(RandomStream);
				const float NewScale = Type->GetScaleForAge(NewAge);
				const float MinDistanceToClear = GetSeedMinDistance(Inst, NewAge, SimulationStep);
				const FVector GlobalOffset = GetSeedOffset(Type, MinDistanceToClear);
				
				if (GlobalOffset.SizeSquared2D() + SMALL_NUMBER > MinDistanceToClear*MinDistanceToClear)
				{
					const FVector NewLocation = GlobalOffset + Inst->Location;
					if (FProceduralFoliageInstance* Inst = NewSeed(NewLocation, NewScale, Type, NewAge))
					{
						NewSeeds.Add(Inst);
					}
				}
			}
		}
	}
}

void UProceduralFoliageTile::AddRandomSeeds(TArray<FProceduralFoliageInstance*>& OutInstances)
{
	const float SizeTenM2 = (ProceduralFoliage->TileSize * ProceduralFoliage->TileSize) / (1000.f * 1000.f);

	for ( const FProceduralFoliageTypeData& Data : ProceduralFoliage->GetTypes() )
	{
		UFoliageType_InstancedStaticMesh* TypeInstance = Data.TypeInstance;
		if( TypeInstance )
		{
			const float NumSeeds = TypeInstance->GetSeedDensitySquared() * SizeTenM2;
			for(int32 i = 0; i < NumSeeds; ++i)
			{
				const float X = RandomStream.FRandRange(0, ProceduralFoliage->TileSize);
				const float Y = RandomStream.FRandRange(0, ProceduralFoliage->TileSize);
				const float NewAge = TypeInstance->GetInitAge(RandomStream);
				const float Scale = TypeInstance->GetScaleForAge(NewAge);

				if(FProceduralFoliageInstance* NewInst = NewSeed(FVector(X,Y,0.f), Scale, TypeInstance, NewAge))
				{
					OutInstances.Add(NewInst);
				}
			}
		}
	}
}

void UProceduralFoliageTile::MarkPendingRemoval(FProceduralFoliageInstance* ToRemove)
{
	if (ToRemove->IsAlive())
	{
		Broadphase.Remove(ToRemove);	//we can remove from broadphase right away
		ToRemove->TerminateInstance();
		PendingRemovals.Add(ToRemove);
	}
}

void UProceduralFoliageTile::RemoveInstances()
{
	for (FProceduralFoliageInstance* Inst : Instances)
	{
		MarkPendingRemoval(Inst);
	}

	InstancesArray.Empty();
	FlushPendingRemovals();
}

void UProceduralFoliageTile::InstancesToArray()
{
	InstancesArray.Empty(Instances.Num());
	for (FProceduralFoliageInstance* FromInst : Instances)
	{
		if (FromInst->bBlocker == false)	//blockers do not get instantiated so don't bother putting it into array
		{
			new(InstancesArray)FProceduralFoliageInstance(*FromInst);
		}
	}
}

void UProceduralFoliageTile::RemoveInstance(FProceduralFoliageInstance* ToRemove)
{
	if (ToRemove->IsAlive())
	{
		Broadphase.Remove(ToRemove);
		ToRemove->TerminateInstance();
	}
	
	Instances.Remove(ToRemove);
	delete ToRemove;
}

void UProceduralFoliageTile::FlushPendingRemovals()
{
	for (FProceduralFoliageInstance* ToRemove : PendingRemovals)
	{
		RemoveInstance(ToRemove);
	}

	PendingRemovals.Empty();
}

void UProceduralFoliageTile::InitSimulation(const UProceduralFoliage* InProceduralFoliage, const int32 RandomSeed)
{
	RandomStream.Initialize(RandomSeed);
	ProceduralFoliage = InProceduralFoliage;
	SimulationStep = 0;
	Broadphase = FProceduralFoliageBroadphase(ProceduralFoliage->TileSize);
}

void UProceduralFoliageTile::StepSimulation()
{
	TArray<FProceduralFoliageInstance*> NewInstances;
	if (SimulationStep == 0)
	{
		AddRandomSeeds(NewInstances);
	}
	else
	{
		AgeSeeds();
		SpreadSeeds(NewInstances);
	}

	for (FProceduralFoliageInstance* Inst : NewInstances)
	{
		Instances.Add(Inst);
	}

	FlushPendingRemovals();
}

void UProceduralFoliageTile::Simulate(const UProceduralFoliage* InProceduralFoliage, const int32 RandomSeed, const int32 MaxNumSteps)
{
	InitSimulation(InProceduralFoliage, RandomSeed);

	int32 MaxSteps = 0;
	for(const FProceduralFoliageTypeData& Data : ProceduralFoliage->GetTypes())
	{
		const UFoliageType_InstancedStaticMesh* TypeInstance = Data.TypeInstance;
		if(TypeInstance)
		{
			MaxSteps = FMath::Max(MaxSteps, TypeInstance->NumSteps+1);
		}
	}

	if (MaxNumSteps >= 0)
	{
		MaxSteps = FMath::Min(MaxSteps, MaxNumSteps);	//only take as many steps as given
	}

	for (int32 Step = 0; Step < MaxSteps; ++Step)
	{
		StepSimulation();
		++SimulationStep;
	}

	InstancesToArray();
}


void UProceduralFoliageTile::BeginDestroy()
{
	Super::BeginDestroy();
	RemoveInstances();
}

void UProceduralFoliageTile::CreateInstancesToSpawn(TArray<FProceduralFoliageInstance>& OutInstances, const FTransform& WorldTM, UWorld* World, const float HalfHeight) const
{
	const FCollisionQueryParams Params(true);
	FHitResult Hit;

	OutInstances.Reserve(Instances.Num());
	for (const FProceduralFoliageInstance& Instance : InstancesArray)
	{
		//@todo ProceduralFoliage need a better method for calculating the ray
		FVector StartRay = Instance.Location + WorldTM.GetLocation();
		StartRay.Z += HalfHeight;
		FVector EndRay = StartRay;
		EndRay.Z -= HalfHeight*2.f;
		FCollisionShape SphereShape;
		SphereShape.SetSphere(Instance.GetMaxRadius());

		if (World->SweepSingle(Hit, StartRay, EndRay, FQuat::Identity, SphereShape, Params, FCollisionObjectQueryParams(ECC_WorldStatic)))
		{
			if (Hit.Actor.IsValid())	//if we hit the ProceduralFoliage blocking volume don't spawn instance
			{
				if (Cast<AProceduralFoliageBlockingVolume>(Hit.Actor.Get()) || Cast<AInstancedFoliageActor>(Hit.Actor.Get()))
				{
					continue;
				}
			}

			const UFoliageType_InstancedStaticMesh* Type = Instance.Type;
			if (Hit.ImpactPoint.Z >= Type->HeightMin && Hit.ImpactPoint.Z <= Type->HeightMax)
			{
				if (FMath::Cos(FMath::DegreesToRadians(Type->GroundSlope)) <= Hit.ImpactNormal.Z)
				{
					FProceduralFoliageInstance* NewInst = new(OutInstances)FProceduralFoliageInstance(Instance);
					NewInst->Location = FVector(StartRay.X, StartRay.Y, Hit.ImpactPoint.Z);	//take the x,y of the instance, but use the z of the impact point. This is because we never want to move the instance along xy or we'll get overlaps 
					NewInst->Normal = Hit.ImpactNormal;
					if (Hit.Component.IsValid())
					{
						NewInst->BaseComponent = Hit.Component.Get();
					}
				}
			}
		}
	}
}

void UProceduralFoliageTile::Empty()
{
	Broadphase.Empty();
	InstancesArray.Empty();
	
	for (FProceduralFoliageInstance* Inst : Instances)
	{
		delete Inst;
	}

	Instances.Empty();
	PendingRemovals.Empty();
}

SIZE_T UProceduralFoliageTile::GetResourceSize(EResourceSizeMode::Type Mode)
{
	SIZE_T TotalSize = 0;
	for (FProceduralFoliageInstance* Inst : Instances)
	{
		TotalSize += sizeof(FProceduralFoliageInstance);
	}
	
	//@TODO: account for broadphase
	return TotalSize;
}


void UProceduralFoliageTile::GetInstancesInAABB(const FBox2D& LocalAABB, TArray<FProceduralFoliageInstance*>& OutInstances, bool bOnTheBorder) const
{
	TArray<FProceduralFoliageInstance*> InstancesInAABB;
	Broadphase.GetInstancesInBox(LocalAABB, InstancesInAABB);

	OutInstances.Reserve(OutInstances.Num() + InstancesInAABB.Num());
	for (FProceduralFoliageInstance* Inst : InstancesInAABB)
{
		const float Rad = Inst->GetMaxRadius();
		const FVector& Location = Inst->Location;

		if (bOnTheBorder || (Location.X - Rad >= LocalAABB.Min.X && Location.X + Rad <= LocalAABB.Max.X && Location.Y - Rad >= LocalAABB.Min.Y && Location.Y + Rad <= LocalAABB.Max.Y))
		{
			OutInstances.Add(Inst);
		}
	}
}

void UProceduralFoliageTile::AddInstances(const TArray<FProceduralFoliageInstance*>& NewInstances, const FTransform& RelativeTM, const FBox2D& InnerLocalAABB)
{
	for (const FProceduralFoliageInstance* Inst : NewInstances)
	{
		const FVector& Location = Inst->Location;	//we need the local space because we're comparing it to the AABB
		//Instances in InnerLocalAABB or on the border of the max sides of the AABB will be visible and instantiated by this tile
		//Instances outside of the InnerLocalAABB are only used for rejection purposes. This is needed for overlapping tiles
		const float Radius = Inst->GetMaxRadius();
		bool bBlocker = false;
		if (Location.X + Radius <= InnerLocalAABB.Min.X || Location.X - Radius > InnerLocalAABB.Max.X || Location.Y + Radius <= InnerLocalAABB.Min.Y || Location.Y - Radius > InnerLocalAABB.Max.Y)
		{
			//Only used for blocking instances in case of overlap, a different tile will instantiate it
			bBlocker = true;
		}

		const FVector NewLocation = RelativeTM.TransformPosition(Inst->Location);
		if (FProceduralFoliageInstance* NewInst = NewSeed(NewLocation, Inst->Scale, Inst->Type, Inst->Age, bBlocker))
		{
			
			Instances.Add(NewInst);
		}
	}

	FlushPendingRemovals();
}

#undef LOCTEXT_NAMESPACE