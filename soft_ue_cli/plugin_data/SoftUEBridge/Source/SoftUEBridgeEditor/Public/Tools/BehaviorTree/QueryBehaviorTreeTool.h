// Copyright ShineTheSky 2026. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Tools/BridgeToolBase.h"
#include "QueryBehaviorTreeTool.generated.h"

/**
 * Query a BehaviorTree asset: read blackboard keys, composite tree,
 * decorators, services, and node UPROPERTY values.
 */
UCLASS()
class SOFTUEBRIDGEEDITOR_API UQueryBehaviorTreeTool : public UBridgeToolBase
{
	GENERATED_BODY()

public:
	virtual FString GetToolName() const override { return TEXT("query-behaviortree"); }
	virtual FString GetToolDescription() const override;
	virtual TMap<FString, FBridgeSchemaProperty> GetInputSchema() const override;
	virtual TArray<FString> GetRequiredParams() const override;
	virtual FBridgeToolResult Execute(
		const TSharedPtr<FJsonObject>& Arguments,
		const FBridgeToolContext& Context) override;
};
