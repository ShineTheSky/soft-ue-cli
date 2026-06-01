// Copyright ShineTheSky 2026. All Rights Reserved.

#include "Tools/BehaviorTree/CreateBehaviorTreeFromJsonTool.h"
#include "Utils/BridgeAssetModifier.h"
#include "SoftUEBridgeEditorModule.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Composites/BTComposite_SimpleParallel.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "BehaviorTreeGraphNode_SimpleParallel.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ScopedTransaction.h"

static UClass* FindCompositeClass(const FString& TypeName)
{
	FString Lower = TypeName.ToLower();
	if (Lower == TEXT("selector"))       return UBTComposite_Selector::StaticClass();
	if (Lower == TEXT("sequence"))       return UBTComposite_Sequence::StaticClass();
	if (Lower == TEXT("simpleparallel") || Lower == TEXT("simple parallel"))
		return UBTComposite_SimpleParallel::StaticClass();
	FString FullName = TEXT("BTComposite_") + TypeName;
	if (UClass* Cls = FindFirstObject<UClass>(*FullName, EFindFirstObjectOptions::ExactClass))
		return Cls;
	return UBTComposite_Selector::StaticClass();
}

static UClass* FindBTClass(const FString& ClassName, UClass* BaseClass)
{
	if (UClass* Cls = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::ExactClass))
		if (Cls->IsChildOf(BaseClass)) return Cls;
	FString Prefixed = TEXT("U") + ClassName;
	if (UClass* Cls = FindFirstObject<UClass>(*Prefixed, EFindFirstObjectOptions::ExactClass))
		if (Cls->IsChildOf(BaseClass)) return Cls;
	FString BPrefixed = TEXT("B") + ClassName;
	if (UClass* Cls = FindFirstObject<UClass>(*BPrefixed, EFindFirstObjectOptions::ExactClass))
		if (Cls->IsChildOf(BaseClass)) return Cls;
	if (ClassName.EndsWith(TEXT("_C")))
	{
		FString BpName = ClassName.LeftChop(2);
		FAssetRegistryModule& ARMod = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		TArray<FAssetData> Matches;
		ARMod.Get().GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), Matches, true);
		for (const FAssetData& Asset : Matches)
		{
			if (Asset.AssetName.ToString() == BpName)
			{
				UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *Asset.GetObjectPathString());
				if (BP && BP->GeneratedClass && BP->GeneratedClass->IsChildOf(BaseClass))
					return BP->GeneratedClass;
			}
		}
		if (UClass* Cls = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::None))
			if (Cls->IsChildOf(BaseClass)) return Cls;
	}
	return nullptr;
}

static void SetObjectProperties(UObject* Instance, const TSharedPtr<FJsonObject>& Props)
{
	if (!Instance || !Props.IsValid()) return;
	for (const auto& Pair : Props->Values)
	{
		const FString& PropPath = Pair.Key;
		FProperty* Prop = nullptr;
		void* Container = Instance;
		FString FindErr;
		if (FBridgeAssetModifier::FindPropertyByPath(Instance, PropPath, Prop, Container, FindErr)) {}
		else
		{
			Prop = Instance->GetClass()->FindPropertyByName(*PropPath);
			Container = Instance;
		}
		if (!Prop) continue;
		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Container);
		if (!ValuePtr) continue;
		FString ValueStr;
		if (Pair.Value->Type == EJson::String)
			ValueStr = Pair.Value->AsString();
		else if (Pair.Value->Type == EJson::Number)
			ValueStr = FString::Printf(TEXT("%g"), Pair.Value->AsNumber());
		else if (Pair.Value->Type == EJson::Boolean)
			ValueStr = Pair.Value->AsBool() ? TEXT("true") : TEXT("false");
		else
			continue;
		Prop->ImportText_Direct(*ValueStr, ValuePtr, Instance, PPF_None);
	}
}

// Parent, Child, OutputPinIndex (0 for normal composites, 0=main/1=bg for SimpleParallel)
using FParentChildLink = TTuple<UBehaviorTreeGraphNode*, UBehaviorTreeGraphNode*, int32>;

// In-order leaf assignment: leaves get sequential X positions (guaranteeing same-depth
// spacing), then internal nodes are centered over their children's X range.
// Returns the next available X after placing this subtree.
static int32 LayoutAssignLeaves(
	UBehaviorTreeGraphNode* Node,
	TMap<UBehaviorTreeGraphNode*, TArray<UBehaviorTreeGraphNode*>>& ChildrenMap,
	int32 NextX,
	int32 LeafSpacing)
{
	TArray<UBehaviorTreeGraphNode*>* Children = ChildrenMap.Find(Node);
	if (!Children || Children->Num() == 0)
	{
		Node->NodePosX = NextX;
		return NextX + LeafSpacing;
	}

	// Process children left-to-right; each gets a contiguous X range
	for (UBehaviorTreeGraphNode* Child : *Children)
		NextX = LayoutAssignLeaves(Child, ChildrenMap, NextX, LeafSpacing);

	// Center this node over the X range of its children
	int32 FirstChildX = (*Children)[0]->NodePosX;
	int32 LastChildX = (*Children)[Children->Num() - 1]->NodePosX;
	Node->NodePosX = (FirstChildX + LastChildX) / 2;

	return NextX;
}

static UBehaviorTreeGraphNode* CreateBTNode(
	UBehaviorTreeGraph* Graph,
	const TSharedPtr<FJsonObject>& NodeJson,
	TArray<FParentChildLink>& OutLinks,
	TArray<FString>& OutWarnings,
	int32 ParentX = 0,
	int32 ParentY = 0)
{
	FString Kind = NodeJson->GetStringField(TEXT("kind"));

	if (Kind == TEXT("Composite") || Kind == TEXT("Root"))
	{
		FString TypeName = NodeJson->GetStringField(TEXT("type"));
		UClass* CompClass = FindCompositeClass(TypeName);

		bool bIsSimpleParallel = (TypeName.ToLower() == TEXT("simpleparallel") ||
			TypeName.ToLower() == TEXT("simple parallel"));
		UClass* GraphNodeClass = bIsSimpleParallel
			? UBehaviorTreeGraphNode_SimpleParallel::StaticClass()
			: UBehaviorTreeGraphNode_Composite::StaticClass();

		UBehaviorTreeGraphNode_Composite* CompNode = NewObject<UBehaviorTreeGraphNode_Composite>(
			Graph, GraphNodeClass, NAME_None, RF_Transactional);
		CompNode->CreateNewGuid();

		UBTCompositeNode* CompositeInstance = NewObject<UBTCompositeNode>(
			CompNode, CompClass, NAME_None, RF_Transactional);
		CompNode->NodeInstance = CompositeInstance;

		const TSharedPtr<FJsonObject>* Props = nullptr;
		if (NodeJson->TryGetObjectField(TEXT("properties"), Props))
			SetObjectProperties(CompositeInstance, *Props);

		CompNode->AllocateDefaultPins();
		Graph->AddNode(CompNode, false, false);
		CompNode->PostPlacedNewNode();
		CompNode->NodePosY = ParentY;
		CompNode->NodePosX = ParentX;

		// Services → SubNodes + Services arrays
		const TArray<TSharedPtr<FJsonValue>>* Services = nullptr;
		if (NodeJson->TryGetArrayField(TEXT("services"), Services))
		{
			for (const auto& SvcVal : *Services)
			{
				const TSharedPtr<FJsonObject>* SvcObj = nullptr;
				if (SvcVal->TryGetObject(SvcObj))
				{
					FString SvcClass = (*SvcObj)->GetStringField(TEXT("class"));
					UClass* SvcType = FindBTClass(SvcClass, UBTService::StaticClass());
					if (SvcType)
					{
						UBehaviorTreeGraphNode_Service* SvcNode = NewObject<UBehaviorTreeGraphNode_Service>(
							Graph, UBehaviorTreeGraphNode_Service::StaticClass(), NAME_None, RF_Transactional);
						SvcNode->CreateNewGuid();
						SvcNode->bIsSubNode = true;
						UBTService* SvcInst = NewObject<UBTService>(SvcNode, SvcType, NAME_None, RF_Transactional);
						const TSharedPtr<FJsonObject>* SvcProps = nullptr;
						if ((*SvcObj)->TryGetObjectField(TEXT("properties"), SvcProps))
							SetObjectProperties(SvcInst, *SvcProps);
						SvcNode->NodeInstance = SvcInst;
						SvcNode->AllocateDefaultPins();
						// Service is a sub-node, not a standalone graph node
						SvcNode->PostPlacedNewNode();
						SvcNode->ParentNode = CompNode;
						CompNode->SubNodes.Add(SvcNode);
						CompNode->Services.Add(SvcNode);
					}
					else
						OutWarnings.Add(FString::Printf(TEXT("Service class not found: %s"), *SvcClass));
				}
			}
		}

		// Decorators → SubNodes + Decorators arrays
		const TArray<TSharedPtr<FJsonValue>>* Decorators = nullptr;
		if (NodeJson->TryGetArrayField(TEXT("decorators"), Decorators))
		{
			for (const auto& DecVal : *Decorators)
			{
				const TSharedPtr<FJsonObject>* DecObj = nullptr;
				if (DecVal->TryGetObject(DecObj))
				{
					FString DecClass = (*DecObj)->GetStringField(TEXT("class"));
					UClass* DecType = FindBTClass(DecClass, UBTDecorator::StaticClass());
					if (DecType)
					{
						UBehaviorTreeGraphNode_Decorator* DecNode = NewObject<UBehaviorTreeGraphNode_Decorator>(
							Graph, UBehaviorTreeGraphNode_Decorator::StaticClass(), NAME_None, RF_Transactional);
						DecNode->CreateNewGuid();
						DecNode->bIsSubNode = true;
						UBTDecorator* DecInst = NewObject<UBTDecorator>(DecNode, DecType, NAME_None, RF_Transactional);
						const TSharedPtr<FJsonObject>* DecProps = nullptr;
						if ((*DecObj)->TryGetObjectField(TEXT("properties"), DecProps))
							SetObjectProperties(DecInst, *DecProps);
						DecNode->NodeInstance = DecInst;
						DecNode->AllocateDefaultPins();
						// Decorator is a sub-node, not a standalone graph node
						DecNode->PostPlacedNewNode();
						CompNode->SubNodes.Add(DecNode);
						DecNode->ParentNode = CompNode;
					CompNode->Decorators.Add(DecNode);
					}
					else
						OutWarnings.Add(FString::Printf(TEXT("Decorator class not found: %s"), *DecClass));
				}
			}
		}

		// Children → pin links (not SubNodes). SimpleParallel pins: 0=main, 1=background
		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		if (NodeJson->TryGetArrayField(TEXT("children"), Children))
		{
			const int32 SubNodeCount = CompNode->Services.Num() + CompNode->Decorators.Num();
			const int32 ChildY = ParentY + (SubNodeCount + 1) * 200;
			int32 ChildIdx = 0;
			for (const auto& ChildVal : *Children)
			{
				const TSharedPtr<FJsonObject>* ChildObj = nullptr;
				if (ChildVal->TryGetObject(ChildObj))
				{
					// Pass ParentX as temporary X; LayoutBTSubtree will reposition later
					UBehaviorTreeGraphNode* ChildNode = CreateBTNode(Graph, *ChildObj, OutLinks, OutWarnings, ParentX, ChildY);
					if (ChildNode)
					{
						int32 PinIdx = bIsSimpleParallel ? ChildIdx : 0;
						OutLinks.Add(FParentChildLink(CompNode, ChildNode, PinIdx));
						ChildIdx++;
					}
				}
			}
		}

		return CompNode;
	}

	if (Kind == TEXT("Task"))
	{
		FString TaskClass = NodeJson->GetStringField(TEXT("class"));
		UClass* TaskType = FindBTClass(TaskClass, UBTTaskNode::StaticClass());
		if (!TaskType)
		{
			OutWarnings.Add(FString::Printf(TEXT("Task class not found: %s"), *TaskClass));
			return nullptr;
		}

		UBehaviorTreeGraphNode_Task* TaskNode = NewObject<UBehaviorTreeGraphNode_Task>(
			Graph, UBehaviorTreeGraphNode_Task::StaticClass(), NAME_None, RF_Transactional);
		TaskNode->CreateNewGuid();

		UBTTaskNode* TaskInst = NewObject<UBTTaskNode>(TaskNode, TaskType, NAME_None, RF_Transactional);
		TaskNode->NodeInstance = TaskInst;

		const TSharedPtr<FJsonObject>* Props = nullptr;
		if (NodeJson->TryGetObjectField(TEXT("properties"), Props))
			SetObjectProperties(TaskInst, *Props);

		TaskNode->AllocateDefaultPins();
		Graph->AddNode(TaskNode, false, false);
		TaskNode->PostPlacedNewNode();
		TaskNode->NodePosY = ParentY;
		TaskNode->NodePosX = ParentX;

		// Decorators on task
		const TArray<TSharedPtr<FJsonValue>>* Decorators = nullptr;
		if (NodeJson->TryGetArrayField(TEXT("decorators"), Decorators))
		{
			for (const auto& DecVal : *Decorators)
			{
				const TSharedPtr<FJsonObject>* DecObj = nullptr;
				if (DecVal->TryGetObject(DecObj))
				{
					FString DecClass = (*DecObj)->GetStringField(TEXT("class"));
					UClass* DecType = FindBTClass(DecClass, UBTDecorator::StaticClass());
					if (DecType)
					{
						UBehaviorTreeGraphNode_Decorator* DecNode = NewObject<UBehaviorTreeGraphNode_Decorator>(
							Graph, UBehaviorTreeGraphNode_Decorator::StaticClass(), NAME_None, RF_Transactional);
						DecNode->CreateNewGuid();
						DecNode->bIsSubNode = true;
						UBTDecorator* DI = NewObject<UBTDecorator>(DecNode, DecType, NAME_None, RF_Transactional);
						const TSharedPtr<FJsonObject>* DecProps = nullptr;
						if ((*DecObj)->TryGetObjectField(TEXT("properties"), DecProps))
							SetObjectProperties(DI, *DecProps);
						DecNode->NodeInstance = DI;
						DecNode->AllocateDefaultPins();
						// Decorator is a sub-node, not a standalone graph node
						DecNode->PostPlacedNewNode();
						TaskNode->SubNodes.Add(DecNode);
						DecNode->ParentNode = TaskNode;
					TaskNode->Decorators.Add(DecNode);
					}
				}
			}
		}

		return TaskNode;
	}

	OutWarnings.Add(FString::Printf(TEXT("Unknown node kind: %s"), *Kind));
	return nullptr;
}

// ── Tool ─────────────────────────────────────────────────────────────────────

FString UCreateBehaviorTreeFromJsonTool::GetToolDescription() const
{
	return TEXT("Create a BehaviorTree asset from a JSON description.");
}

TMap<FString, FBridgeSchemaProperty> UCreateBehaviorTreeFromJsonTool::GetInputSchema() const
{
	TMap<FString, FBridgeSchemaProperty> Schema;
	FBridgeSchemaProperty AssetPath;
	AssetPath.Type = TEXT("string");
	AssetPath.Description = TEXT("Asset path (e.g., /Game/AI/BT_New)");
	AssetPath.bRequired = true;
	Schema.Add(TEXT("asset_path"), AssetPath);
	FBridgeSchemaProperty BlackboardPath;
	BlackboardPath.Type = TEXT("string");
	BlackboardPath.Description = TEXT("Path to existing BlackboardData");
	BlackboardPath.bRequired = false;
	Schema.Add(TEXT("blackboard_path"), BlackboardPath);
	FBridgeSchemaProperty Root;
	Root.Type = TEXT("object");
	Root.Description = TEXT("Root node of the behavior tree");
	Root.bRequired = true;
	Schema.Add(TEXT("root"), Root);
	return Schema;
}

TArray<FString> UCreateBehaviorTreeFromJsonTool::GetRequiredParams() const
{
	return { TEXT("asset_path"), TEXT("root") };
}

FBridgeToolResult UCreateBehaviorTreeFromJsonTool::Execute(
	const TSharedPtr<FJsonObject>& Arguments,
	const FBridgeToolContext& Context)
{
	FString AssetPath = GetStringArgOrDefault(Arguments, TEXT("asset_path"));
	FString BlackboardPath = GetStringArgOrDefault(Arguments, TEXT("blackboard_path"));
	const TSharedPtr<FJsonObject>* RootObj = nullptr;
	if (!Arguments->TryGetObjectField(TEXT("root"), RootObj))
		return FBridgeToolResult::Error(TEXT("root node is required"));

	UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
	TArray<FString> Warnings;

	if (BlackboardPath.IsEmpty())
	{
		FString BBName = GetStringArgOrDefault(Arguments, TEXT("blackboard_name"));
		if (!BBName.IsEmpty())
		{
			FAssetRegistryModule& ARMod = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
			TArray<FAssetData> BBAssets;
			ARMod.Get().GetAssetsByClass(UBlackboardData::StaticClass()->GetClassPathName(), BBAssets, true);
			for (const FAssetData& Asset : BBAssets)
			{
				if (Asset.AssetName.ToString() == BBName)
				{
					BlackboardPath = Asset.GetObjectPathString();
					break;
				}
			}
			if (BlackboardPath.IsEmpty())
				Warnings.Add(FString::Printf(TEXT("Blackboard '%s' not found"), *BBName));
		}
	}

	if (BT)
	{
		if (BT->BTGraph) BT->BTGraph->Nodes.Empty();
	}
	else
	{
		UPackage* Pkg = CreatePackage(*AssetPath);
		if (!Pkg) return FBridgeToolResult::Error(TEXT("Failed to create package"));
		FString AssetName = FPackageName::GetShortName(AssetPath);
		BT = NewObject<UBehaviorTree>(Pkg, UBehaviorTree::StaticClass(), *AssetName, RF_Public | RF_Standalone);
		if (!BT) return FBridgeToolResult::Error(TEXT("Failed to create BehaviorTree"));
		FAssetRegistryModule::AssetCreated(BT);
	}

	BT->MarkPackageDirty();

	if (!BlackboardPath.IsEmpty())
	{
		UBlackboardData* BB = LoadObject<UBlackboardData>(nullptr, *BlackboardPath);
		if (BB) BT->BlackboardAsset = BB;
		else Warnings.Add(FString::Printf(TEXT("Blackboard not found: %s"), *BlackboardPath));
	}

	TSharedPtr<FScopedTransaction> Transaction = FBridgeAssetModifier::BeginTransaction(
		NSLOCTEXT("MCP", "CreateBT", "Create BehaviorTree from JSON"));

	// Create graph
	UBehaviorTreeGraph* Graph = NewObject<UBehaviorTreeGraph>(
		BT, UBehaviorTreeGraph::StaticClass(), NAME_None, RF_Transactional);
	BT->BTGraph = Graph;
	Graph->Schema = UEdGraphSchema_BehaviorTree::StaticClass();

	// Root graph node (anchor)
	UBehaviorTreeGraphNode_Root* RootNode = NewObject<UBehaviorTreeGraphNode_Root>(
		Graph, UBehaviorTreeGraphNode_Root::StaticClass(), NAME_None, RF_Transactional);
	RootNode->CreateNewGuid();
	RootNode->AllocateDefaultPins();
	Graph->AddNode(RootNode, false, false);
	RootNode->PostPlacedNewNode();

	// Build full tree. RootObj is the effective root composite (e.g., Selector).
	// It becomes the SINGLE child of RootNode — CreateBTFromGraph uses
	// RootNode.Pins[0].LinkedTo[0] as the BT->RootNode.
	TArray<FParentChildLink> Links;
	UBehaviorTreeGraphNode* RootCompNode = CreateBTNode(Graph, *RootObj, Links, Warnings, /*ParentX=*/0, /*ParentY=*/200);
	if (RootCompNode)
		Links.Add(FParentChildLink(RootNode, RootCompNode, 0));

	// Wire pin connections (respecting SimpleParallel dual-pin: 0=main, 1=background)
	const UEdGraphSchema* Schema = Graph->GetSchema();
	for (const auto& [Parent, Child, PinIdx] : Links)
	{
		UEdGraphPin* OutPin = Parent->GetOutputPin(PinIdx);
		UEdGraphPin* InPin = Child->GetInputPin();
		if (OutPin && InPin)
			Schema->TryCreateConnection(OutPin, InPin);
	}

	// Build children map for layout pass (skip RootNode → keep it at origin)
	TMap<UBehaviorTreeGraphNode*, TArray<UBehaviorTreeGraphNode*>> ChildrenMap;
	for (const auto& [Parent, Child, PinIdx] : Links)
		ChildrenMap.FindOrAdd(Parent).Add(Child);

	// Layout the tree: compute subtree widths bottom-up, center children under parents
	if (RootCompNode)
		LayoutAssignLeaves(RootCompNode, ChildrenMap, 0, /*LeafSpacing=*/350);

	Graph->NotifyGraphChanged();
	Graph->UpdateAsset();

	UE_LOG(LogSoftUEBridgeEditor, Log, TEXT("create-behaviortree: BT created with %d pin links"), Links.Num());

	FBridgeAssetModifier::MarkPackageDirty(BT);

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetBoolField(TEXT("success"), true);
	Result->SetBoolField(TEXT("needs_save"), true);
	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnVals;
		for (const FString& W : Warnings)
			WarnVals.Add(MakeShareable(new FJsonValueString(W)));
		Result->SetArrayField(TEXT("warnings"), WarnVals);
	}
	return FBridgeToolResult::Json(Result);
}
