// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "BlueprintNodeSpawner.h"
#include "BlueprintBoundNodeSpawner.generated.h"

/**
 * Takes care of spawning various bound nodes. Acts as the 
 * "action" portion of certain FBlueprintActionMenuItems. 
 */
UCLASS(Transient)
class BLUEPRINTGRAPH_API UBlueprintBoundNodeSpawner : public UBlueprintNodeSpawner
{
	GENERATED_UCLASS_BODY()
	DECLARE_DELEGATE_RetVal_OneParam(bool, FCanBindObjectDelegate, UObject const*);
	DECLARE_DELEGATE_RetVal_TwoParams(bool, FOnBindObjectDelegate, UEdGraphNode*, UObject*);
	DECLARE_DELEGATE_RetVal_OneParam(FText, FOnGenerateMenuDescriptionDelegate, const IBlueprintNodeBinder::FBindingSet& );

public:
	/**
	 * @return A newly allocated instance of this class.
	 */
	static UBlueprintBoundNodeSpawner* Create(TSubclassOf<UEdGraphNode> NodeClass, UObject* Outer = nullptr);

	// UBlueprintNodeSpawner interface
	virtual FBlueprintNodeSignature GetSpawnerSignature() const override;
	virtual FText GetDefaultMenuName(FBindingSet const& Bindings) const override;
	// End UBlueprintNodeSpawner interface

	// FBlueprintNodeBinder interface
	virtual bool IsBindingCompatible(UObject const* BindingCandidate) const override;
	virtual bool BindToNode(UEdGraphNode* Node, UObject* Binding) const override;
	// End FBlueprintNodeBinder interface

public:
	/**
	 * A delegate to perform specialized node binding verification
	 */
	FCanBindObjectDelegate CanBindObjectDelegate;

	/**
	 * A delegate to perform specialized node setup during binding
	 */
	FOnBindObjectDelegate OnBindObjectDelegate;

	/**
	 * A delegate to generate a description for the menu item. Executed any time bindings change
	 */
	FOnGenerateMenuDescriptionDelegate OnGenerateMenuDescriptionDelegate;
};
