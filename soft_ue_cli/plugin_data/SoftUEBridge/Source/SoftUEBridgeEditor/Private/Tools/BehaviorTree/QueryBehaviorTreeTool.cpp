// Copyright ShineTheSky 2026. All Rights Reserved.

#include "Tools/BehaviorTree/QueryBehaviorTreeTool.h"
#include "SoftUEBridgeEditorModule.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "AIGraphNode.h"
#include "UObject/TextProperty.h"

FString UQueryBehaviorTreeTool::GetToolDescription() const
{
	return TEXT("Query a BehaviorTree asset: blackboard keys, full composite tree structure, "
		"decorators, services, and per-node UPROPERTY values.");
}

TMap<FString, FBridgeSchemaProperty> UQueryBehaviorTreeTool::GetInputSchema() const
{
	TMap<FString, FBridgeSchemaProperty> Schema;
	FBridgeSchemaProperty Path;
	Path.Type = TEXT("string");
	Path.Description = TEXT("Asset path to the BehaviorTree (e.g., /Game/AI/BT_Enemy)");
	Path.bRequired = true;
	Schema.Add(TEXT("asset_path"), Path);
	return Schema;
}

TArray<FString> UQueryBehaviorTreeTool::GetRequiredParams() const
{
	return { TEXT("asset_path") };
}

// ── Helpers ──────────────────────────────────────────────────────────────────

static FString CompositeTypeToString(UBehaviorTreeGraphNode_Composite* CompNode)
{
	if (!CompNode || !CompNode->NodeInstance)
	{
		return TEXT("Unknown");
	}
	FString ClassName = CompNode->NodeInstance->GetClass()->GetName();
	ClassName.RemoveFromStart(TEXT("BTComposite_"));
	return ClassName;
}

static TSharedPtr<FJsonObject> PropertiesToJson(UObject* Instance)
{
	TSharedPtr<FJsonObject> Props = MakeShareable(new FJsonObject);
	if (!Instance)
	{
		return Props;
	}

	for (TFieldIterator<FProperty> It(Instance->GetClass()); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop || Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient))
		{
			continue;
		}

		// Skip engine-internal / noisy properties
		FString PropName = Prop->GetName();
		static const TArray<FString> SkipProps = {
			TEXT("TreeAsset"), TEXT("ParentNode"), TEXT("Children"),
			TEXT("bApplyDecoratorScope"), TEXT("bShowPropertyDetails"),
			TEXT("bShowEventDetails"), TEXT("bIgnoreRestartSelf"),
			TEXT("TickInterval"), TEXT("bCallTickOnSearchStart"),
			TEXT("bRestartTimerOnEachActivation"),
		};
		bool bSkip = PropName.StartsWith(TEXT("bIs")) || PropName.StartsWith(TEXT("Node"))
			|| PropName.Contains(TEXT("Memory"));
		for (const FString& Skip : SkipProps)
		{
			if (PropName == Skip) { bSkip = true; break; }
		}
		if (bSkip)
		{
			continue;
		}

		FString ValueStr;
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Instance);
		if (ValuePtr)
		{
			Prop->ExportText_Direct(ValueStr, ValuePtr, ValuePtr, Instance, PPF_None);
			ValueStr.TrimStartAndEndInline();
		}
		if (!ValueStr.IsEmpty() && ValueStr != TEXT("None"))
		{
			Props->SetStringField(PropName, ValueStr);
		}
	}
	return Props;
}

// ── Runtime tree fallback (reads from UBehaviorTree asset directly) ──────────

static TSharedPtr<FJsonObject> BTCompositeToJson(UBTCompositeNode* Composite, int32 Depth);

static TSharedPtr<FJsonObject> BTTaskToJson(UBTTaskNode* Task, int32 Depth)
{
	TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject);
	if (!Task) return Json;

	Json->SetNumberField(TEXT("depth"), Depth);
	Json->SetStringField(TEXT("kind"), TEXT("Task"));
	Json->SetStringField(TEXT("class"), Task->GetClass()->GetName());
	FString Desc = Task->GetNodeName();
	if (Desc.IsEmpty()) Desc = Task->GetClass()->GetName();
	Json->SetStringField(TEXT("description"), Desc);
	Json->SetObjectField(TEXT("properties"), PropertiesToJson(Task));

	return Json;
}

static TSharedPtr<FJsonObject> BTCompositeToJson(UBTCompositeNode* Composite, int32 Depth)
{
	TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject);
	if (!Composite) return Json;

	Json->SetNumberField(TEXT("depth"), Depth);
	FString CompName = Composite->GetNodeName();
	// Runtime tree composites may return empty/garbled names — derive from class
	if (CompName.IsEmpty() || !FCString::IsPureAnsi(*CompName))
	{
		CompName = Composite->GetClass()->GetName();
		CompName.RemoveFromStart(TEXT("BTComposite_"));
	}
	Json->SetStringField(TEXT("kind"), TEXT("Composite"));
	Json->SetStringField(TEXT("type"), CompName);
	Json->SetStringField(TEXT("description"), CompName);

	// Properties
	Json->SetObjectField(TEXT("properties"), PropertiesToJson(Composite));

	// Services
	if (Composite->Services.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Services;
		for (UBTService* Svc : Composite->Services)
		{
			if (!Svc) continue;
			TSharedPtr<FJsonObject> SvcJson = MakeShareable(new FJsonObject);
			SvcJson->SetStringField(TEXT("class"), Svc->GetClass()->GetName());
			SvcJson->SetStringField(TEXT("description"), Svc->GetNodeName());
			SvcJson->SetObjectField(TEXT("properties"), PropertiesToJson(Svc));
			Services.Add(MakeShareable(new FJsonValueObject(SvcJson)));
		}
		Json->SetArrayField(TEXT("services"), Services);
	}

	// Children
	if (Composite->Children.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Children;
		for (const FBTCompositeChild& Child : Composite->Children)
		{
			// Each child can have a composite, a task, and decorators
			TSharedPtr<FJsonObject> ChildJson;
			if (Child.ChildComposite)
			{
				ChildJson = BTCompositeToJson(Child.ChildComposite, Depth + 1);
			}
			else if (Child.ChildTask)
			{
				ChildJson = BTTaskToJson(Child.ChildTask, Depth + 1);
			}
			else
			{
				continue;
			}

			// Decorators on this child
			if (Child.Decorators.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> Decorators;
				for (UBTDecorator* Dec : Child.Decorators)
				{
					if (!Dec) continue;
					TSharedPtr<FJsonObject> DecJson = MakeShareable(new FJsonObject);
					DecJson->SetStringField(TEXT("class"), Dec->GetClass()->GetName());
					DecJson->SetStringField(TEXT("description"), Dec->GetNodeName());
					DecJson->SetObjectField(TEXT("properties"), PropertiesToJson(Dec));
					FString FlowStr;
					switch (Dec->GetFlowAbortMode())
					{
					case EBTFlowAbortMode::None:          FlowStr = TEXT("None"); break;
					case EBTFlowAbortMode::LowerPriority:  FlowStr = TEXT("LowerPriority"); break;
					case EBTFlowAbortMode::Self:           FlowStr = TEXT("Self"); break;
					case EBTFlowAbortMode::Both:           FlowStr = TEXT("Both"); break;
					default:                               FlowStr = TEXT("None"); break;
					}
					if (FlowStr != TEXT("None")) DecJson->SetStringField(TEXT("flow_control"), FlowStr);
					Decorators.Add(MakeShareable(new FJsonValueObject(DecJson)));
				}
				if (Decorators.Num() > 0)
				{
					ChildJson->SetArrayField(TEXT("decorators"), Decorators);
				}
			}

			Children.Add(MakeShareable(new FJsonValueObject(ChildJson)));
		}
		Json->SetArrayField(TEXT("children"), Children);
	}

	return Json;
}

// ── Graph-based tree reader ──────────────────────────────────────────────────

static TSharedPtr<FJsonObject> NodeToJson(UAIGraphNode* GraphNode, int32 Depth)
{
	TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject);
	if (!GraphNode)
	{
		return Json;
	}

	Json->SetNumberField(TEXT("depth"), Depth);

	// ── Root ──────────────────────────────────────────────────────────────
	if (GraphNode->IsA<UBehaviorTreeGraphNode_Root>())
	{
		Json->SetStringField(TEXT("kind"), TEXT("Root"));
		Json->SetStringField(TEXT("name"), TEXT("Root"));
		Json->SetStringField(TEXT("description"), GraphNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());

		TArray<TSharedPtr<FJsonValue>> Children;
		for (UAIGraphNode* Child : GraphNode->SubNodes)
		{
			if (Child && !Child->IsA<UBehaviorTreeGraphNode_Decorator>()
				&& !Child->IsA<UBehaviorTreeGraphNode_Service>())
			{
				TSharedPtr<FJsonObject> ChildJson = NodeToJson(Child, Depth + 1);
				if (ChildJson->Values.Num() > 0)
				{
					Children.Add(MakeShareable(new FJsonValueObject(ChildJson)));
				}
			}
		}
		if (Children.Num() > 0)
		{
			Json->SetArrayField(TEXT("children"), Children);
		}
		return Json;
	}

	// ── Composite ─────────────────────────────────────────────────────────
	if (UBehaviorTreeGraphNode_Composite* CompNode = Cast<UBehaviorTreeGraphNode_Composite>(GraphNode))
	{
		Json->SetStringField(TEXT("kind"), TEXT("Composite"));
		Json->SetStringField(TEXT("type"), CompositeTypeToString(CompNode));
		Json->SetStringField(TEXT("description"), GraphNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());

		// Decorators on this composite
		TArray<TSharedPtr<FJsonValue>> Decorators;
		// Services on this composite
		TArray<TSharedPtr<FJsonValue>> Services;

		for (UAIGraphNode* Sub : GraphNode->SubNodes)
		{
			if (!Sub) continue;

			// Check for dedicated service graph node first
			if (UBehaviorTreeGraphNode_Service* SvcNode = Cast<UBehaviorTreeGraphNode_Service>(Sub))
			{
				TSharedPtr<FJsonObject> Svc = MakeShareable(new FJsonObject);
				Svc->SetStringField(TEXT("kind"), TEXT("Service"));
				Svc->SetStringField(TEXT("class"), SvcNode->NodeInstance
					? SvcNode->NodeInstance->GetClass()->GetName()
					: SvcNode->GetClass()->GetName());
				Svc->SetStringField(TEXT("description"), SvcNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
				if (SvcNode->NodeInstance)
				{
					Svc->SetObjectField(TEXT("properties"), PropertiesToJson(SvcNode->NodeInstance));
				}
				Services.Add(MakeShareable(new FJsonValueObject(Svc)));
			}
			else if (UBehaviorTreeGraphNode_Decorator* DecNode = Cast<UBehaviorTreeGraphNode_Decorator>(Sub))
			{
				// Check if it's actually a service (services use the same graph node type
				// but their NodeInstance is a UBTService)
				if (DecNode->NodeInstance && DecNode->NodeInstance->IsA<UBTService>())
				{
					TSharedPtr<FJsonObject> Svc = MakeShareable(new FJsonObject);
					Svc->SetStringField(TEXT("class"), DecNode->NodeInstance->GetClass()->GetName());
					Svc->SetStringField(TEXT("description"), DecNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
					Svc->SetObjectField(TEXT("properties"), PropertiesToJson(DecNode->NodeInstance));
					Services.Add(MakeShareable(new FJsonValueObject(Svc)));
				}
				else
				{
					TSharedPtr<FJsonObject> Dec = MakeShareable(new FJsonObject);
					Dec->SetStringField(TEXT("class"), DecNode->NodeInstance
						? DecNode->NodeInstance->GetClass()->GetName()
						: DecNode->GetClass()->GetName());
					Dec->SetStringField(TEXT("description"), DecNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
					if (DecNode->NodeInstance)
					{
						Dec->SetObjectField(TEXT("properties"), PropertiesToJson(DecNode->NodeInstance));

						// Flow control
						if (UBTDecorator* Decorator = Cast<UBTDecorator>(DecNode->NodeInstance))
						{
							FString FlowStr;
							switch (Decorator->GetFlowAbortMode())
							{
							case EBTFlowAbortMode::None:          FlowStr = TEXT("None"); break;
							case EBTFlowAbortMode::LowerPriority:  FlowStr = TEXT("LowerPriority"); break;
							case EBTFlowAbortMode::Self:           FlowStr = TEXT("Self"); break;
							case EBTFlowAbortMode::Both:           FlowStr = TEXT("Both"); break;
							default:                               FlowStr = TEXT("None"); break;
							}
							if (FlowStr != TEXT("None"))
							{
								Dec->SetStringField(TEXT("flow_control"), FlowStr);
							}
							// IsObserver() removed in UE 5.7 — flow_control suffices
						}
					}
					Decorators.Add(MakeShareable(new FJsonValueObject(Dec)));
				}
			}
		}

		if (Decorators.Num() > 0)
		{
			Json->SetArrayField(TEXT("decorators"), Decorators);
		}
		if (Services.Num() > 0)
		{
			Json->SetArrayField(TEXT("services"), Services);
		}

		// Children (non-decorator, non-service sub-nodes)
		TArray<TSharedPtr<FJsonValue>> Children;
		for (UAIGraphNode* Child : GraphNode->SubNodes)
		{
			if (Child && !Child->IsA<UBehaviorTreeGraphNode_Decorator>()
				&& !Child->IsA<UBehaviorTreeGraphNode_Service>())
			{
				TSharedPtr<FJsonObject> ChildJson = NodeToJson(Child, Depth + 1);
				if (ChildJson->Values.Num() > 0)
				{
					Children.Add(MakeShareable(new FJsonValueObject(ChildJson)));
				}
			}
		}
		if (Children.Num() > 0)
		{
			Json->SetArrayField(TEXT("children"), Children);
		}
		return Json;
	}

	// ── Task ──────────────────────────────────────────────────────────────
	if (UBehaviorTreeGraphNode_Task* TaskNode = Cast<UBehaviorTreeGraphNode_Task>(GraphNode))
	{
		Json->SetStringField(TEXT("kind"), TEXT("Task"));
		Json->SetStringField(TEXT("class"), TaskNode->NodeInstance
			? TaskNode->NodeInstance->GetClass()->GetName()
			: TaskNode->GetClass()->GetName());
		Json->SetStringField(TEXT("description"), TaskNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		if (TaskNode->NodeInstance)
		{
			Json->SetObjectField(TEXT("properties"), PropertiesToJson(TaskNode->NodeInstance));
		}

		// Decorators on this task
		TArray<TSharedPtr<FJsonValue>> Decorators;
		for (UAIGraphNode* Sub : GraphNode->SubNodes)
		{
			if (UBehaviorTreeGraphNode_Decorator* DecNode = Cast<UBehaviorTreeGraphNode_Decorator>(Sub))
			{
				if (DecNode->NodeInstance && !DecNode->NodeInstance->IsA<UBTService>())
				{
					TSharedPtr<FJsonObject> Dec = MakeShareable(new FJsonObject);
					Dec->SetStringField(TEXT("class"), DecNode->NodeInstance->GetClass()->GetName());
					Dec->SetStringField(TEXT("description"), DecNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
					Dec->SetObjectField(TEXT("properties"), PropertiesToJson(DecNode->NodeInstance));
					Decorators.Add(MakeShareable(new FJsonValueObject(Dec)));
				}
			}
		}
		if (Decorators.Num() > 0)
		{
			Json->SetArrayField(TEXT("decorators"), Decorators);
		}
		return Json;
	}

	// ── Fallback ───────────────────────────────────────────────────────────
	Json->SetStringField(TEXT("kind"), TEXT("Unknown"));
	Json->SetStringField(TEXT("class"), GraphNode->GetClass()->GetName());
	Json->SetStringField(TEXT("description"), GraphNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
	return Json;
}

static TSharedPtr<FJsonObject> BlackboardToJson(UBlackboardData* BB)
{
	TSharedPtr<FJsonObject> Json = MakeShareable(new FJsonObject);
	if (!BB)
	{
		return Json;
	}

	Json->SetStringField(TEXT("name"), BB->GetName());
	Json->SetStringField(TEXT("path"), BB->GetPathName());

	TArray<TSharedPtr<FJsonValue>> Keys;
	for (const FBlackboardEntry& Entry : BB->Keys)
	{
		TSharedPtr<FJsonObject> KeyObj = MakeShareable(new FJsonObject);
		KeyObj->SetStringField(TEXT("name"), Entry.EntryName.ToString());
		if (Entry.KeyType)
		{
			KeyObj->SetStringField(TEXT("type"), Entry.KeyType->GetClass()->GetName());
			KeyObj->SetBoolField(TEXT("is_instance_editable"), Entry.KeyType->HasAnyFlags(RF_NeedPostLoad)); // rough
		}
		Keys.Add(MakeShareable(new FJsonValueObject(KeyObj)));
	}
	Json->SetArrayField(TEXT("keys"), Keys);
	Json->SetNumberField(TEXT("key_count"), Keys.Num());

	return Json;
}

// ── Execute ──────────────────────────────────────────────────────────────────

FBridgeToolResult UQueryBehaviorTreeTool::Execute(
	const TSharedPtr<FJsonObject>& Arguments,
	const FBridgeToolContext& Context)
{
	FString AssetPath = GetStringArgOrDefault(Arguments, TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		return FBridgeToolResult::Error(TEXT("Missing required parameter: asset_path"));
	}

	UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
	if (!BT)
	{
		return FBridgeToolResult::Error(FString::Printf(TEXT("BehaviorTree not found: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetStringField(TEXT("name"), BT->GetName());
	Result->SetStringField(TEXT("path"), AssetPath);

	// ── Blackboard ────────────────────────────────────────────────────────
	if (BT->BlackboardAsset)
	{
		UBlackboardData* BB = BT->BlackboardAsset;
		Result->SetObjectField(TEXT("blackboard"), BlackboardToJson(BB));
	}

	// ── Tree structure ────────────────────────────────────────────────────
	UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(BT->BTGraph);
	if (!Graph)
	{
		return FBridgeToolResult::Error(TEXT("BehaviorTree has no graph"));
	}

	// Find root node
	UBehaviorTreeGraphNode_Root* RootNode = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		RootNode = Cast<UBehaviorTreeGraphNode_Root>(Node);
		if (RootNode) break;
	}

	if (!RootNode)
	{
		return FBridgeToolResult::Error(TEXT("BehaviorTree has no root node"));
	}

	TSharedPtr<FJsonObject> TreeJson = NodeToJson(RootNode, 0);
	// Fallback: if graph SubNodes are empty, read from the BT asset's runtime tree
	const TArray<TSharedPtr<FJsonValue>>* GraphChildren = nullptr;
		if ((!TreeJson->TryGetArrayField(TEXT("children"), GraphChildren) || GraphChildren->Num() == 0) && BT->RootNode)
	{
		TSharedPtr<FJsonObject> RuntimeRoot = BTCompositeToJson(BT->RootNode, 0);
		if (RuntimeRoot.IsValid())
		{
			RuntimeRoot->SetStringField(TEXT("kind"), TEXT("Root"));
			RuntimeRoot->SetStringField(TEXT("name"), TEXT("Root"));
			TreeJson = RuntimeRoot;
		}
	}
	Result->SetObjectField(TEXT("root"), TreeJson);

	return FBridgeToolResult::Json(Result);
}
