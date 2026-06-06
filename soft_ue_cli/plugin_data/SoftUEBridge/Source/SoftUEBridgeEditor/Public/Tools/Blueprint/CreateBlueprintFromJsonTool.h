// Copyright ShineTheSky 2026. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Tools/BridgeToolBase.h"
#include "CreateBlueprintFromJsonTool.generated.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

/**
 * Create a complete Blueprint asset from a JSON description.
 *
 * JSON format (see blueprint_json.py validator for full spec):
 * {
 *   "parent_class": "AuraGameplayAbility",
 *   "asset_path": "/Game/Blueprints/BP_New",
 *   "variables": [{ "name": "Speed", "type": "float", ... }],
 *   "nodes": [
 *     { "id": "OnBegin", "type": "K2Node_Event", "event": "ReceiveBeginPlay" },
 *     { "id": "Print",   "type": "K2Node_CallFunction", "function": "PrintString" }
 *   ],
 *   "connections": [
 *     { "from": "OnBegin.then", "to": "Print.execute" }
 *   ]
 * }
 */
UCLASS()
class SOFTUEBRIDGEEDITOR_API UCreateBlueprintFromJsonTool : public UBridgeToolBase
{
	GENERATED_BODY()

public:
	virtual FString GetToolName() const override { return TEXT("create-blueprint-from-json"); }
	virtual FString GetToolDescription() const override;
	virtual TMap<FString, FBridgeSchemaProperty> GetInputSchema() const override;
	virtual TArray<FString> GetRequiredParams() const override;
	virtual FBridgeToolResult Execute(
		const TSharedPtr<FJsonObject>& Arguments,
		const FBridgeToolContext& Context) override;

private:
	/** Resolve a human-readable node type alias to a UE K2Node class name. */
	static FString ResolveNodeType(const FString& TypeName);

	/** Find a UFunction by name, searching parent class and common utility classes. */
	static UFunction* FindFunctionByName(const FString& FunctionName, UClass* ParentClass);

	/** Map UE pin category name strings to their FName constants. */
	static FName PinCategoryFromString(const FString& CategoryName);

	/** Create a single Blueprint node via reflection (no per-type hardcoding). */
	UEdGraphNode* CreateNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TSharedPtr<FJsonObject>& NodeJson,
		TMap<FString, UEdGraphNode*>& IdMap,
		TArray<FString>& OutWarnings);

	/** Map friendly JSON fields (event, function, variable) to UE property references. */
	void _ApplyFriendlyProps(
		UEdGraphNode* Node,
		UBlueprint* Blueprint,
		const TSharedPtr<FJsonObject>& NodeJson,
		TArray<FString>& OutWarnings);

	/** Set UObject properties from a JSON object via FProperty reflection. */
	void _ApplyReflectionProperties(
		UObject* Target,
		const TSharedPtr<FJsonObject>& Props,
		TArray<FString>& OutWarnings);

	/** Restore pin types that AllocateDefaultPins cannot infer from serialized graph data. */
	static void ApplyPromotablePinTypes(
		UEdGraphNode* Node,
		const TSharedPtr<FJsonObject>& NodeJson);

	/** Apply serialized pin literal/object defaults to a node. */
	static void ApplySerializedPinDefaults(
		UEdGraphNode* Node,
		const TSharedPtr<FJsonObject>& NodeJson,
		const UEdGraphSchema* Schema,
		bool bCoerceClassPinObjects);

	/** Connect serialized node.pin references within a graph. */
	static void ConnectSerializedPins(
		const TArray<TSharedPtr<FJsonValue>>* Connections,
		const TMap<FString, UEdGraphNode*>& IdToNode,
		const UEdGraphSchema* Schema,
		TArray<FString>& OutWarnings,
		bool bWarnOnMalformed);

	/** Find a pin on a node by name (case-insensitive). */
	static UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName);
};
