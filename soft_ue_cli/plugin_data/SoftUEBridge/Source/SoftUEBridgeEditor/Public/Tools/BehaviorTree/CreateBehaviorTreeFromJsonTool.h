// Copyright ShineTheSky 2026. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Tools/BridgeToolBase.h"
#include "CreateBehaviorTreeFromJsonTool.generated.h"

/**
 * Create a BehaviorTree asset from a JSON description.
 *
 * JSON format:
 * {
 *   "asset_path": "/Game/AI/BT_New",
 *   "blackboard_path": "/Game/AI/BB_Enemy",
 *   "root": { "kind": "Composite", "type": "Selector", "children": [...] }
 * }
 */
UCLASS()
class SOFTUEBRIDGEEDITOR_API UCreateBehaviorTreeFromJsonTool : public UBridgeToolBase
{
	GENERATED_BODY()

public:
	virtual FString GetToolName() const override { return TEXT("create-behaviortree-from-json"); }
	virtual FString GetToolDescription() const override;
	virtual TMap<FString, FBridgeSchemaProperty> GetInputSchema() const override;
	virtual TArray<FString> GetRequiredParams() const override;
	virtual FBridgeToolResult Execute(
		const TSharedPtr<FJsonObject>& Arguments,
		const FBridgeToolContext& Context) override;
};
