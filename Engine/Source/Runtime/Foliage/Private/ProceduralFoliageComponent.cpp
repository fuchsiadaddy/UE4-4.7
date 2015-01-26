// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "FoliagePrivate.h"
#include "ProceduralFoliageComponent.h"
#include "ProceduralFoliageTile.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "ProceduralFoliage.h"

#include "Async/Async.h"

#define LOCTEXT_NAMESPACE "ProceduralFoliage"

UProceduralFoliageComponent::UProceduralFoliageComponent(const FObjectInitializer& ObjectInitializer)
: Super(ObjectInitializer)
{
	TilesX = 1;
	TilesY = 1;
	Overlap = 0.f;
	HalfHeight = 10000.f;
	ProceduralGuid = FGuid::NewGuid();
}

void CopyTileInstances(const UProceduralFoliageTile* FromTile, UProceduralFoliageTile* ToTile, const FBox2D& InnerLocalAABB, const FTransform& ToLocalTM, const float Overlap)
{
	const FBox2D OuterLocalAABB(InnerLocalAABB.Min, InnerLocalAABB.Max + FVector2D(Overlap, Overlap));
	TArray<FProceduralFoliageInstance*> ToInstances;
	FromTile->GetInstancesInAABB(OuterLocalAABB, ToInstances, false);
	ToTile->AddInstances(ToInstances, ToLocalTM, InnerLocalAABB);
}

FBox2D GetTileRegion(const int32 X, const int32 Y, const int32 CountX, const int32 CountY, const float InnerSize, const float Overlap)
{
	const FVector2D Left(-Overlap, Overlap);
	const FVector2D Bottom(Overlap, -Overlap);

	FBox2D Region(FVector2D(Overlap,Overlap), FVector2D(InnerSize + Overlap, InnerSize + Overlap));
	if (X == 0)
	{
		Region += Left;
	}

	if (Y == 0)
	{
		Region += Bottom;
	}

	return Region;
}

void UProceduralFoliageComponent::SpawnTiles(TArray<FDesiredFoliageInstance>& OutInstances)
{
#if WITH_EDITOR
	if (ProceduralFoliage)
	{
		/** Constants for laying out the overlapping tile grid*/
		const float InnerTileSize = ProceduralFoliage->TileSize;
		const FVector2D InnerTileV(InnerTileSize, InnerTileSize);
		const FVector2D InnerTileX(InnerTileSize, 0.f);
		const FVector2D InnerTileY(0.f, InnerTileSize);

		const float OuterTileSize = ProceduralFoliage->TileSize + Overlap;
		const FVector2D OuterTileV(OuterTileSize, OuterTileSize);
		const FVector2D OuterTileX(OuterTileSize, 0.f);
		const FVector2D OuterTileY(0.f, OuterTileSize);

		const FVector2D OverlapX(Overlap, 0.f);
		const FVector2D OverlapY(0.f, Overlap);

		TArray<TFuture< TArray<FDesiredFoliageInstance>* >> Futures;
		FScopedSlowTask SlowTask(TilesX * TilesY, LOCTEXT("PlaceProceduralFoliage", "Placing ProceduralFoliage..."));
		SlowTask.MakeDialog();

		for (int32 X = 0; X < TilesX; ++X)
		{
			for (int32 Y = 0; Y < TilesY; ++Y)
			{
				
				//We have to get the tiles and create new one to build on main thread
				const UProceduralFoliageTile* Tile = ProceduralFoliage->GetRandomTile(X, Y);
				const UProceduralFoliageTile* RightTile = (X + 1 < TilesX) ? ProceduralFoliage->GetRandomTile(X + 1, Y) : nullptr;
				const UProceduralFoliageTile* TopTile = (Y + 1 < TilesY) ? ProceduralFoliage->GetRandomTile(X, Y+1) : nullptr;
				const UProceduralFoliageTile* TopRightTile = (RightTile && TopTile) ? ProceduralFoliage->GetRandomTile(X + 1, Y+1) : nullptr;

				UProceduralFoliageTile* JTile = ProceduralFoliage->CreateTempTile();

				Futures.Add(Async<TArray<FDesiredFoliageInstance>*>(EAsyncExecution::ThreadPool, [=]()
				{
					FTransform TileTM = ComponentToWorld;
					const FVector OrientedOffset = ComponentToWorld.TransformVectorNoScale(FVector(X, Y, 0.f) * FVector(InnerTileSize, InnerTileSize, 0.f));
					TileTM.AddToTranslation(OrientedOffset);

					//copy the inner tile
					const FBox2D InnerBox = GetTileRegion(X, Y, TilesX, TilesY, InnerTileSize, Overlap);
					CopyTileInstances(Tile, JTile, InnerBox, FTransform::Identity, Overlap);


					if (RightTile)
					{
						//Add overlap to the right
						const FBox2D RightBox(FVector2D(-Overlap, InnerBox.Min.Y), FVector2D(Overlap, InnerBox.Max.Y));
						const FTransform RightTM(FVector(InnerTileSize, 0.f, 0.f));
						CopyTileInstances(RightTile, JTile, RightBox, RightTM, Overlap);
					}

					if (TopTile)
					{
						//Add overlap to the top
						const FBox2D TopBox(FVector2D(InnerBox.Min.X, -Overlap), FVector2D(InnerBox.Max.X, Overlap));
						const FTransform TopTM(FVector(0.f, InnerTileSize, 0.f));
						CopyTileInstances(TopTile, JTile, TopBox, TopTM, Overlap);
					}

					if (TopRightTile)
					{
						//Add overlap to the top right
						const FBox2D TopRightBox(FVector2D(-Overlap, -Overlap), FVector2D(Overlap, Overlap));
						const FTransform TopRightTM(FVector(InnerTileSize, InnerTileSize, 0.f));
						CopyTileInstances(TopRightTile, JTile, TopRightBox, TopRightTM, Overlap);
					}

					TArray<FDesiredFoliageInstance>* DesiredInstances = new TArray<FDesiredFoliageInstance>();
					JTile->InstancesToArray();
					JTile->CreateInstancesToSpawn(*DesiredInstances, TileTM, ProceduralGuid, HalfHeight);
					JTile->Empty();

					return DesiredInstances;
					
				})
				);
			}
		}

		int32 FutureIdx = 0;
		for (int X = 0; X < TilesX; ++X)
		{
			for (int Y = 0; Y < TilesY; ++Y)
			{
				TArray<FDesiredFoliageInstance>* DesiredInstances = Futures[FutureIdx++].Get();
				OutInstances.Append(*DesiredInstances);
				delete DesiredInstances;
				SlowTask.EnterProgressFrame(1);
			}
		}
	}
#endif
}

void UProceduralFoliageComponent::SpawnProceduralContent(TArray <FDesiredFoliageInstance>& OutInstances)
{
#if WITH_EDITOR
	RemoveProceduralContent();
	SpawnTiles(OutInstances);
#endif
}

void UProceduralFoliageComponent::RemoveProceduralContent()
{
#if WITH_EDITOR
	UWorld* World = GetWorld();

	for (ULevel* Level : World->GetLevels())
	{
		if (Level)
		{
			AInstancedFoliageActor* IFA = AInstancedFoliageActor::GetInstancedFoliageActorForLevel(Level);
			IFA->DeleteInstancesForProceduralFoliageComponent(this);
		}
	}
#endif
}

#undef LOCTEXT_NAMESPACE