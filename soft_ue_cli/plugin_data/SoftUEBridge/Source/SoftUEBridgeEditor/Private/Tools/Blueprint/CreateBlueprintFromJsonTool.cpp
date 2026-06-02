// Copyright ShineTheSky 2026. All Rights Reserved.

#include "Tools/Blueprint/CreateBlueprintFromJsonTool.h"
#include "Utils/BridgeAssetModifier.h"
#include "Utils/BridgePropertySerializer.h"
#include "SoftUEBridgeEditorModule.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_MakeArray.h"
#include "K2Node_Message.h"
#include "K2Node_PromotableOperator.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Timeline.h"
#include "K2Node_BreakStruct.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/TimelineTemplate.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/RealCurve.h"
// K2Node_ForEachLoop removed in UE 5.7
#include "K2Node_Self.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "GameFramework/Actor.h"

// ── Aliases from the Python validator ───────────────────────────────────────
static const TMap<FString, FString> NodeTypeAliases = {
	{ TEXT("Event"),              TEXT("K2Node_Event") },
	{ TEXT("CustomEvent"),        TEXT("K2Node_CustomEvent") },
	{ TEXT("CallFunction"),       TEXT("K2Node_CallFunction") },
	{ TEXT("Branch"),             TEXT("K2Node_IfThenElse") },
	{ TEXT("IfThenElse"),         TEXT("K2Node_IfThenElse") },
	{ TEXT("If"),                 TEXT("K2Node_IfThenElse") },
	{ TEXT("VariableGet"),        TEXT("K2Node_VariableGet") },
	{ TEXT("Getter"),             TEXT("K2Node_VariableGet") },
	{ TEXT("VariableSet"),        TEXT("K2Node_VariableSet") },
	{ TEXT("Setter"),             TEXT("K2Node_VariableSet") },
	{ TEXT("DynamicCast"),        TEXT("K2Node_DynamicCast") },
	{ TEXT("Cast"),               TEXT("K2Node_DynamicCast") },
	{ TEXT("CastTo"),             TEXT("K2Node_DynamicCast") },
	{ TEXT("MakeArray"),          TEXT("K2Node_MakeArray") },
	// K2Node_ForEachLoop removed in UE 5.7
	{ TEXT("Self"),               TEXT("K2Node_Self") },
	{ TEXT("Sequence"),           TEXT("K2Node_ExecutionSequence") },
	{ TEXT("ExecutionSequence"),  TEXT("K2Node_ExecutionSequence") },
};

FString UCreateBlueprintFromJsonTool::ResolveNodeType(const FString& TypeName)
{
	if (const FString* Alias = NodeTypeAliases.Find(TypeName))
	{
		return *Alias;
	}
	return TypeName;
}

// ── Tool metadata ───────────────────────────────────────────────────────────

FString UCreateBlueprintFromJsonTool::GetToolDescription() const
{
	return TEXT("Create a complete Blueprint asset from a JSON description. "
		"Supports: K2Node_Event, K2Node_CustomEvent, K2Node_CallFunction, "
		"K2Node_IfThenElse, K2Node_VariableGet, K2Node_VariableSet, "
		"K2Node_DynamicCast, K2Node_ExecutionSequence, K2Node_MakeArray, "
		"K2Node_Self. "
		"Variables, nodes, pin defaults, and connections are all created in one call. "
		"Use validate-blueprint-json (offline) to check the JSON before calling this tool.");
}

TMap<FString, FBridgeSchemaProperty> UCreateBlueprintFromJsonTool::GetInputSchema() const
{
	TMap<FString, FBridgeSchemaProperty> Schema;

	FBridgeSchemaProperty ParentClass;
	ParentClass.Type = TEXT("string");
	ParentClass.Description = TEXT("Parent class for the Blueprint (e.g., 'Actor', 'AuraGameplayAbility')");
	ParentClass.bRequired = true;
	Schema.Add(TEXT("parent_class"), ParentClass);

	FBridgeSchemaProperty AssetPath;
	AssetPath.Type = TEXT("string");
	AssetPath.Description = TEXT("Asset path including name (e.g., '/Game/Blueprints/BP_NewAbility')");
	AssetPath.bRequired = true;
	Schema.Add(TEXT("asset_path"), AssetPath);

	FBridgeSchemaProperty Variables;
	Variables.Type = TEXT("array");
	Variables.Description = TEXT("Array of variable definitions: [{name, type, sub_type?, default_value?, is_array?, category?}]");
	Variables.bRequired = false;
	Schema.Add(TEXT("variables"), Variables);

	FBridgeSchemaProperty Nodes;
	Nodes.Type = TEXT("array");
	Nodes.Description = TEXT("Array of node definitions: [{id, type, function?, event?, variable?, position?, defaults?}]");
	Nodes.bRequired = true;
	Schema.Add(TEXT("nodes"), Nodes);

	FBridgeSchemaProperty Connections;
	Connections.Type = TEXT("array");
	Connections.Description = TEXT("Array of connections: [{from: \"node.pin\", to: \"node.pin\"}] or [[\"node.pin\", \"node.pin\"]]");
	Connections.bRequired = false;
	Schema.Add(TEXT("connections"), Connections);

	return Schema;
}

TArray<FString> UCreateBlueprintFromJsonTool::GetRequiredParams() const
{
	return { TEXT("parent_class"), TEXT("asset_path"), TEXT("nodes") };
}

// ── Main execution ──────────────────────────────────────────────────────────

FBridgeToolResult UCreateBlueprintFromJsonTool::Execute(
	const TSharedPtr<FJsonObject>& Args,
	const FBridgeToolContext& Context)
{
	FString ParentClassName = GetStringArgOrDefault(Args, TEXT("parent_class"));
	FString AssetPath = GetStringArgOrDefault(Args, TEXT("asset_path"));

	if (ParentClassName.IsEmpty() || AssetPath.IsEmpty())
	{
		return FBridgeToolResult::Error(TEXT("parent_class and asset_path are required"));
	}

	const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
	if (!Args->TryGetArrayField(TEXT("nodes"), NodesArray))
	{
		return FBridgeToolResult::Error(TEXT("nodes array is required"));
	}

	const TArray<TSharedPtr<FJsonValue>>* VariablesArray = nullptr;
	Args->TryGetArrayField(TEXT("variables"), VariablesArray);

	const TArray<TSharedPtr<FJsonValue>>* ConnectionsArray = nullptr;
	Args->TryGetArrayField(TEXT("connections"), ConnectionsArray);

	UE_LOG(LogSoftUEBridgeEditor, Log, TEXT("create-blueprint-from-json: %s (parent=%s, nodes=%d)"),
		*AssetPath, *ParentClassName, NodesArray->Num());

	// ── Resolve parent class ────────────────────────────────────────────
	FString ClassError;
	UClass* ParentClass = FBridgePropertySerializer::ResolveClass(ParentClassName, ClassError);
	if (!ParentClass)
	{
		return FBridgeToolResult::Error(FString::Printf(TEXT("Parent class not found: %s (%s)"), *ParentClassName, *ClassError));
	}

	// ── Create or load blueprint ────────────────────────────────────────
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	const bool bIsNew = (Blueprint == nullptr);

	if (bIsNew)
	{
		FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);
		FString AssetName = FPackageName::GetShortName(AssetPath);

		UPackage* Pkg = CreatePackage(*AssetPath);
		if (!Pkg)
		{
			return FBridgeToolResult::Error(TEXT("Failed to create package"));
		}

		Blueprint = FKismetEditorUtilities::CreateBlueprint(
			ParentClass, Pkg, *AssetName,
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());

		if (!Blueprint)
		{
			return FBridgeToolResult::Error(TEXT("Failed to create Blueprint"));
		}

		FAssetRegistryModule::AssetCreated(Blueprint);
	}
	else
	{
		// Existing blueprint: clear the event graph
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (Graph)
			{
				Graph->Nodes.Empty();
			}
		}
	}

	FBridgeAssetModifier::MarkModified(Blueprint);
	TSharedPtr<FScopedTransaction> Transaction = FBridgeAssetModifier::BeginTransaction(
		NSLOCTEXT("MCP", "CreateBPFromJson", "Create Blueprint from JSON"));

	TArray<FString> Warnings;

	// ── Add variables ───────────────────────────────────────────────────
	if (VariablesArray)
	{
		for (const TSharedPtr<FJsonValue>& VarVal : *VariablesArray)
		{
			const TSharedPtr<FJsonObject>* VarObjPtr = nullptr;
			if (!VarVal->TryGetObject(VarObjPtr))
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& VarObj = *VarObjPtr;

			FString VarName = VarObj->GetStringField(TEXT("name"));
			FString VarType = VarObj->GetStringField(TEXT("type"));
			if (VarName.IsEmpty() || VarType.IsEmpty())
			{
				Warnings.Add(FString::Printf(TEXT("Skipping variable with missing name/type")));
				continue;
			}

			FBPVariableDescription NewVar;
			NewVar.VarName = FName(*VarName);
			NewVar.VarType.PinCategory = PinCategoryFromString(VarType);
			if (NewVar.VarType.PinCategory == UEdGraphSchema_K2::PC_Real)
				NewVar.VarType.PinSubCategory = UEdGraphSchema_K2::PC_Float;

			FString SubType;
			if (VarObj->TryGetStringField(TEXT("sub_type"), SubType) && !SubType.IsEmpty())
			{
				// Full path (new format): /Script/Engine.Actor
				UObject* SubObj = StaticFindObject(UObject::StaticClass(), nullptr, *SubType);
				if (!SubObj) // Old format: short name like "Actor"
					SubObj = FindFirstObject<UClass>(*SubType, EFindFirstObjectOptions::None);
				if (!SubObj) // Old format: try U-prefix (UActorComponent etc.)
					SubObj = FindFirstObject<UClass>(*(TEXT("U") + SubType), EFindFirstObjectOptions::None);
				if (!SubObj) // Old format: try A-prefix (AActor etc.)
					SubObj = FindFirstObject<UClass>(*(TEXT("A") + SubType), EFindFirstObjectOptions::None);
				if (!SubObj)
					SubObj = FindFirstObject<UScriptStruct>(*SubType, EFindFirstObjectOptions::ExactClass);
				if (!SubObj)
					SubObj = StaticFindObject(UScriptStruct::StaticClass(), nullptr, *SubType);
				if (!SubObj)
					SubObj = LoadObject<UObject>(nullptr, *SubType);
				if (SubObj)
					NewVar.VarType.PinSubCategoryObject = SubObj;
			}

			bool bIsArray = false;
			VarObj->TryGetBoolField(TEXT("is_array"), bIsArray);
			if (bIsArray)
			{
				NewVar.VarType.ContainerType = EPinContainerType::Array;
			}

			FString DefaultValue;
			if (VarObj->TryGetStringField(TEXT("default_value"), DefaultValue))
			{
				NewVar.DefaultValue = DefaultValue;
			}

			const TArray<TSharedPtr<FJsonValue>>* VarFlags = nullptr;
			if (VarObj->TryGetArrayField(TEXT("flags"), VarFlags))
			{
				for (const auto& FlagVal : *VarFlags)
				{
					FString Flag = FlagVal->AsString();
					if (Flag == TEXT("BlueprintReadOnly"))  NewVar.PropertyFlags |= CPF_BlueprintVisible | CPF_BlueprintReadOnly;
					if (Flag == TEXT("BlueprintReadWrite")) NewVar.PropertyFlags |= CPF_BlueprintVisible;
					if (Flag == TEXT("EditAnywhere"))       NewVar.PropertyFlags |= CPF_Edit;
					if (Flag == TEXT("EditDefaultsOnly"))   NewVar.PropertyFlags |= CPF_Edit | CPF_DisableEditOnInstance;
					if (Flag == TEXT("EditInstanceOnly"))   NewVar.PropertyFlags |= CPF_Edit | CPF_DisableEditOnTemplate;
					if (Flag == TEXT("ExposeOnSpawn"))      { NewVar.PropertyFlags |= CPF_ExposeOnSpawn; NewVar.SetMetaData(FBlueprintMetadata::MD_ExposeOnSpawn, TEXT("true")); }
				}
			}

			FString Category;
			if (VarObj->TryGetStringField(TEXT("category"), Category))
			{
				NewVar.Category = FText::FromString(Category);
			}

			// Default to BlueprintVisible if no explicit flags set
			if (NewVar.PropertyFlags == 0)
				NewVar.PropertyFlags = CPF_BlueprintVisible | CPF_Edit;
			Blueprint->NewVariables.Add(NewVar);
			UE_LOG(LogSoftUEBridgeEditor, Log, TEXT("  Added variable: %s (%s)"), *VarName, *VarType);
		}
	}

	// Regenerate skeleton class so VariableReferences can resolve during AllocateDefaultPins
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	// ── SCS Components (create before nodes so VariableGet finds them) ──
	const TArray<TSharedPtr<FJsonValue>>* ComponentsArray = nullptr;
	if (Args->TryGetArrayField(TEXT("components"), ComponentsArray))
	{
		if (!Blueprint->SimpleConstructionScript)
			Blueprint->SimpleConstructionScript = NewObject<USimpleConstructionScript>(Blueprint);
		for (const TSharedPtr<FJsonValue>& CompVal : *ComponentsArray)
		{
			const TSharedPtr<FJsonObject>* CompObj = nullptr;
			if (!CompVal->TryGetObject(CompObj)) continue;
			FString CompName = (*CompObj)->GetStringField(TEXT("name"));
			FString CompClass = (*CompObj)->GetStringField(TEXT("class"));
			if (CompName.IsEmpty() || CompClass.IsEmpty()) continue;

			FString Err;
			UClass* CompCls = FBridgePropertySerializer::ResolveClass(CompClass, Err);
			if (!CompCls) CompCls = FindFirstObject<UClass>(*CompClass, EFindFirstObjectOptions::None);
			if (!CompCls) CompCls = FindFirstObject<UClass>(*(TEXT("U") + CompClass), EFindFirstObjectOptions::None);
			if (!CompCls) continue;

			USCS_Node* NewNode = Blueprint->SimpleConstructionScript->CreateNode(CompCls, FName(*CompName));
			if (!NewNode) continue;

			FString ParentName;
			if ((*CompObj)->TryGetStringField(TEXT("parent"), ParentName) && !ParentName.IsEmpty())
			{
				if (USCS_Node* ParentNode = Blueprint->SimpleConstructionScript->FindSCSNode(FName(*ParentName)))
					ParentNode->AddChildNode(NewNode);
				else
					Blueprint->SimpleConstructionScript->AddNode(NewNode);
			}
			else
			{
				Blueprint->SimpleConstructionScript->AddNode(NewNode);
			}
		}
		FKismetEditorUtilities::CompileBlueprint(Blueprint);

		// Apply component template properties from JSON
		for (const TSharedPtr<FJsonValue>& CompVal : *ComponentsArray)
		{
			const TSharedPtr<FJsonObject>* CompObj = nullptr;
			if (!CompVal->TryGetObject(CompObj)) continue;
			FString CompName = (*CompObj)->GetStringField(TEXT("name"));
			const TSharedPtr<FJsonObject>* PropsObj = nullptr;
			if (!(*CompObj)->TryGetObjectField(TEXT("properties"), PropsObj)) continue;

			USCS_Node* Node = Blueprint->SimpleConstructionScript->FindSCSNode(FName(*CompName));
			if (!Node || !Node->ComponentTemplate) continue;
			UActorComponent* Template = Node->ComponentTemplate;
			UClass* CompClass = Template->GetClass();

			for (const auto& Pair : (*PropsObj)->Values)
			{
				FProperty* Prop = CompClass->FindPropertyByName(*Pair.Key);
				if (!Prop) continue;
				void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Template);
				if (!ValuePtr) continue;

				FString ValueStr;
				if (!Pair.Value->TryGetString(ValueStr)) continue;
				Prop->ImportText_Direct(*ValueStr, ValuePtr, Template, PPF_None);
			}
		}
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
	}

	// ── Graph nodes (skip for data-only blueprints) ────────────────────────────────────────
	TMap<FString, UEdGraphNode*> IdToNode;

	if (NodesArray->Num() > 0)
	{
		UEdGraph* TargetGraph = nullptr;
		if (Blueprint->UbergraphPages.Num() > 0)
			TargetGraph = Blueprint->UbergraphPages[0];
		if (!TargetGraph)
			return FBridgeToolResult::Error(TEXT("Blueprint has no event graph"));

		const UEdGraphSchema* Schema = TargetGraph->GetSchema();

				// ── Remove all auto-generated event nodes; JSON is the source of truth ──
		{
			TArray<UEdGraphNode*> NodesToRemove;
			for (UEdGraphNode* ExistingNode : TargetGraph->Nodes)
			{
				if (Cast<UK2Node_Event>(ExistingNode) || Cast<UK2Node_CustomEvent>(ExistingNode))
				{
					NodesToRemove.Add(ExistingNode);
				}
			}
			for (UEdGraphNode* NodeToRemove : NodesToRemove)
			{
				TargetGraph->RemoveNode(NodeToRemove);
			}
		}

		// ── Create nodes ────────────────────────────────────────────────────
TArray<FString> NodeErrors;

		// Create Timeline templates (before nodes so AllocateDefaultPins finds them)
	const TArray<TSharedPtr<FJsonValue>>* TimelinesArray = nullptr;
	if (Args->TryGetArrayField(TEXT("timelines"), TimelinesArray))
	{
		for (const TSharedPtr<FJsonValue>& TlVal : *TimelinesArray)
		{
			const TSharedPtr<FJsonObject>* TlObj = nullptr;
			if (!TlVal->TryGetObject(TlObj)) continue;

			FString TlName;
			(*TlObj)->TryGetStringField(TEXT("timeline_name"), TlName);
			if (TlName.IsEmpty()) continue;

			// Use the engine's proper timeline creation
			UTimelineTemplate* Template = FBlueprintEditorUtils::AddNewTimeline(Blueprint, FName(*TlName));
			if (!Template) continue;

			double Length;
			if ((*TlObj)->TryGetNumberField(TEXT("timeline_length"), Length))
				Template->TimelineLength = static_cast<float>(Length);

			bool bVal;
			if ((*TlObj)->TryGetBoolField(TEXT("b_auto_play"), bVal)) Template->bAutoPlay = bVal;
			if ((*TlObj)->TryGetBoolField(TEXT("b_loop"), bVal)) Template->bLoop = bVal;
			if ((*TlObj)->TryGetBoolField(TEXT("b_replicated"), bVal)) Template->bReplicated = bVal;
			if ((*TlObj)->TryGetBoolField(TEXT("b_ignore_time_dilation"), bVal)) Template->bIgnoreTimeDilation = bVal;

			const TArray<TSharedPtr<FJsonValue>>* TracksArray = nullptr;
			if ((*TlObj)->TryGetArrayField(TEXT("tracks"), TracksArray))
			{
				for (const TSharedPtr<FJsonValue>& TrVal : *TracksArray)
				{
					const TSharedPtr<FJsonObject>* TrObj = nullptr;
					if (!TrVal->TryGetObject(TrObj)) continue;

					FString TrName, TrType;
					(*TrObj)->TryGetStringField(TEXT("name"), TrName);
					(*TrObj)->TryGetStringField(TEXT("type"), TrType);

					if (TrType == TEXT("event"))
					{
						FTTEventTrack Track;
						Track.SetTrackName(FName(*TrName), Template);
						Track.CurveKeys = NewObject<UCurveFloat>(Blueprint, NAME_None, RF_Transactional);
						Template->EventTracks.Add(Track);
						Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_Event, Template->EventTracks.Num() - 1));
					}
					else if (TrType == TEXT("float"))
					{
						FTTFloatTrack Track;
						Track.SetTrackName(FName(*TrName), Template);
						Track.CurveFloat = NewObject<UCurveFloat>(Blueprint, NAME_None, RF_Transactional);
						Template->FloatTracks.Add(Track);
						Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_FloatInterp, Template->FloatTracks.Num() - 1));
					}
					else if (TrType == TEXT("vector"))
					{
						FTTVectorTrack Track;
						Track.SetTrackName(FName(*TrName), Template);
						Track.CurveVector = NewObject<UCurveVector>(Blueprint, NAME_None, RF_Transactional);
						Template->VectorTracks.Add(Track);
						Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_VectorInterp, Template->VectorTracks.Num() - 1));
					}
					else if (TrType == TEXT("linear_color"))
					{
						FTTLinearColorTrack Track;
						Track.SetTrackName(FName(*TrName), Template);
						Track.CurveLinearColor = NewObject<UCurveLinearColor>(Blueprint, NAME_None, RF_Transactional);
						Template->LinearColorTracks.Add(Track);
						Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_LinearColorInterp, Template->LinearColorTracks.Num() - 1));
					}
					// Restore curve keys and settings
				{
					auto RestoreRichCurve = [](FRichCurve& Curve, const TSharedPtr<FJsonObject>& TrObj) {
						const TSharedPtr<FJsonObject>* S = nullptr;
						if (TrObj->TryGetObjectField(TEXT("curve_settings"), S))
						{
							int32 IV; double DV;
							if ((*S)->TryGetNumberField(TEXT("pre_infinity_extrap"), IV))
								Curve.PreInfinityExtrap = static_cast<ERichCurveExtrapolation>(IV);
							if ((*S)->TryGetNumberField(TEXT("post_infinity_extrap"), IV))
								Curve.PostInfinityExtrap = static_cast<ERichCurveExtrapolation>(IV);
							if ((*S)->TryGetNumberField(TEXT("default_value"), DV))
								Curve.SetDefaultValue(static_cast<float>(DV));
						}
						const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
						if (TrObj->TryGetArrayField(TEXT("curve_keys"), Keys))
						{
							// Build a sorted array of FRichCurveKey and use SetKeys to avoid AutoSetTangents
							TArray<FRichCurveKey> NewKeys;
							for (const TSharedPtr<FJsonValue>& Kv : *Keys)
							{
								const TSharedPtr<FJsonObject>* K = nullptr;
								if (!Kv->TryGetObject(K)) continue;
								double T, V;
								if (!(*K)->TryGetNumberField(TEXT("time"), T)) continue;
								if (!(*K)->TryGetNumberField(TEXT("value"), V)) continue;
								FRichCurveKey NewKey(static_cast<float>(T), static_cast<float>(V));
								double DV;
								// Read all numeric fields as double, then cast to int for enums
								if ((*K)->TryGetNumberField(TEXT("interp_mode"), DV))
									NewKey.InterpMode = static_cast<ERichCurveInterpMode>(static_cast<int32>(DV));
								if ((*K)->TryGetNumberField(TEXT("tangent_mode"), DV))
									NewKey.TangentMode = static_cast<ERichCurveTangentMode>(static_cast<int32>(DV));
								if ((*K)->TryGetNumberField(TEXT("tangent_weight_mode"), DV))
									NewKey.TangentWeightMode = static_cast<ERichCurveTangentWeightMode>(static_cast<int32>(DV));
								if ((*K)->TryGetNumberField(TEXT("arrive_tangent"), DV))
									NewKey.ArriveTangent = static_cast<float>(DV);
								if ((*K)->TryGetNumberField(TEXT("leave_tangent"), DV))
									NewKey.LeaveTangent = static_cast<float>(DV);
								if ((*K)->TryGetNumberField(TEXT("arrive_tangent_weight"), DV))
									NewKey.ArriveTangentWeight = static_cast<float>(DV);
								if ((*K)->TryGetNumberField(TEXT("leave_tangent_weight"), DV))
									NewKey.LeaveTangentWeight = static_cast<float>(DV);
								NewKeys.Add(NewKey);
							}
							Curve.SetKeys(NewKeys);
							// SetKeys calls AutoSetTangents, re-apply original tangent values
							for (int32 Idx = 0; Idx < FMath::Min(NewKeys.Num(), Curve.Keys.Num()); ++Idx)
							{
								Curve.Keys[Idx].ArriveTangent = NewKeys[Idx].ArriveTangent;
								Curve.Keys[Idx].LeaveTangent = NewKeys[Idx].LeaveTangent;
								Curve.Keys[Idx].ArriveTangentWeight = NewKeys[Idx].ArriveTangentWeight;
								Curve.Keys[Idx].LeaveTangentWeight = NewKeys[Idx].LeaveTangentWeight;
							}
						}
					};

					int32 EvtIdx = 0, FltIdx = 0;
					for (const TSharedPtr<FJsonValue>& TrVal : *TracksArray)
					{
						const TSharedPtr<FJsonObject>* TrObj = nullptr;
						if (!TrVal->TryGetObject(TrObj)) continue;
						FString TrType;
						(*TrObj)->TryGetStringField(TEXT("type"), TrType);

						if (TrType == TEXT("event") && EvtIdx < Template->EventTracks.Num())
						{
							if (Template->EventTracks[EvtIdx].CurveKeys)
								RestoreRichCurve(Template->EventTracks[EvtIdx].CurveKeys->FloatCurve, *TrObj);
							EvtIdx++;
						}
						else if (TrType == TEXT("float") && FltIdx < Template->FloatTracks.Num())
						{
							if (Template->FloatTracks[FltIdx].CurveFloat)
								RestoreRichCurve(Template->FloatTracks[FltIdx].CurveFloat->FloatCurve, *TrObj);
							FltIdx++;
						}
					}
				}
			}
		}
	}

				}
			}
		}
	}

	for (const TSharedPtr<FJsonValue>& NodeVal : *NodesArray)
	{
		const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
		if (!NodeVal->TryGetObject(NodeObjPtr))
		{
			Warnings.Add(TEXT("Skipping non-object node entry"));
			continue;
		}
		const TSharedPtr<FJsonObject>& NodeObj = *NodeObjPtr;

		UEdGraphNode* Node = CreateNode(Blueprint, TargetGraph, NodeObj, IdToNode, NodeErrors);
		if (!Node)
		{
			continue;
		}

		FString NodeId = NodeObj->GetStringField(TEXT("id"));
		IdToNode.Add(NodeId, Node);

		// Fix up PromotableOperator pin types that AllocateDefaultPins resolves wrong
		if (Cast<UK2Node_PromotableOperator>(Node))
		{
			const TSharedPtr<FJsonObject>* PinTypesObj2 = nullptr;
			if (NodeObj->TryGetObjectField(TEXT("pin_types"), PinTypesObj2))
			{
				for (const auto& Pair : (*PinTypesObj2)->Values)
				{
					UEdGraphPin* Pin = FindPin(Node, Pair.Key);
					if (!Pin) continue;
					FString TypeStr = Pair.Value->AsString();
					FString Cat, Sub;
					if (TypeStr.Split(TEXT("/"), &Cat, &Sub))
					{
						Pin->PinType.PinCategory = FName(*Cat);
						Pin->PinType.PinSubCategory = Sub.IsEmpty() ? NAME_None : FName(*Sub);
						Pin->PinType.PinSubCategoryObject = nullptr;
						if (!Sub.IsEmpty() && Sub != TEXT("float"))
						{
							UObject* SubObj = StaticFindObject(UObject::StaticClass(), nullptr, *Sub);
							if (!SubObj) SubObj = FindFirstObject<UClass>(*Sub, EFindFirstObjectOptions::None);
							if (!SubObj) SubObj = FindFirstObject<UScriptStruct>(*Sub, EFindFirstObjectOptions::ExactClass);
							if (SubObj) Pin->PinType.PinSubCategoryObject = SubObj;
						}
					}
					else
					{
						Pin->PinType.PinCategory = FName(*TypeStr);
						Pin->PinType.PinSubCategory = NAME_None;
						Pin->PinType.PinSubCategoryObject = nullptr;
					}
				}
			}
		}

		// Apply pin default values

		// Apply pin default values
		const TSharedPtr<FJsonObject>* DefaultsObj = nullptr;
		if (NodeObj->TryGetObjectField(TEXT("defaults"), DefaultsObj))
		{
			for (const auto& Pair : (*DefaultsObj)->Values)
			{
				UEdGraphPin* Pin = FindPin(Node, Pair.Key);
				if (Pin)
				{
					FString StrVal;
					if (Pair.Value->Type == EJson::String)
					{
						StrVal = Pair.Value->AsString();
					}
					else if (Pair.Value->Type == EJson::Number)
					{
						StrVal = FString::Printf(TEXT("%g"), Pair.Value->AsNumber());
					}
					else if (Pair.Value->Type == EJson::Boolean)
					{
						StrVal = Pair.Value->AsBool() ? TEXT("true") : TEXT("false");
					}
					else
					{
						StrVal = Pair.Value->AsString();
					}

					if (!StrVal.IsEmpty())
					{
						Pin->DefaultValue = StrVal;
					}
				}
			}
		}
	}

// Apply pin default objects (class/object pins)
			const TSharedPtr<FJsonObject>* DefaultObjsObj = nullptr;
			if (NodeObj->TryGetObjectField(TEXT("default_objects"), DefaultObjsObj))
			{
				const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(Schema);
				if (K2Schema)
				{
					for (const auto& Pair : (*DefaultObjsObj)->Values)
					{
						UEdGraphPin* Pin2 = FindPin(Node, Pair.Key);
						if (Pin2)
						{
							FString ObjPath = Pair.Value->AsString();
							if (!ObjPath.IsEmpty())
							{
								K2Schema->SetPinDefaultValueAtConstruction(Pin2, ObjPath);
							}
						}
					}
				}
			}
		}
	}



	// ── Create connections ──────────────────────────────────────────────
	if (ConnectionsArray)
	{
		for (const TSharedPtr<FJsonValue>& ConnVal : *ConnectionsArray)
		{
			FString FromStr, ToStr;

			// Support both array ["from", "to"] and object {from, to} formats
			const TSharedPtr<FJsonObject>* ConnObjPtr = nullptr;
			if (ConnVal->TryGetObject(ConnObjPtr))
			{
				(*ConnObjPtr)->TryGetStringField(TEXT("from"), FromStr);
				(*ConnObjPtr)->TryGetStringField(TEXT("to"), ToStr);
			}
			else
			{
				const TArray<TSharedPtr<FJsonValue>>* ConnArr = nullptr;
				if (ConnVal->TryGetArray(ConnArr) && ConnArr->Num() >= 2)
				{
					FromStr = (*ConnArr)[0]->AsString();
					ToStr = (*ConnArr)[1]->AsString();
				}
			}

			if (FromStr.IsEmpty() || ToStr.IsEmpty())
			{
				Warnings.Add(TEXT("Skipping malformed connection"));
				continue;
			}

			// Parse "node_id.pin_name"
			int32 DotPos;
			FString FromNodeId, FromPinName, ToNodeId, ToPinName;

			if (!FromStr.FindChar('.', DotPos))
			{
				Warnings.Add(FString::Printf(TEXT("Invalid connection source: %s"), *FromStr));
				continue;
			}
			FromNodeId = FromStr.Left(DotPos);
			FromPinName = FromStr.RightChop(DotPos + 1);

			if (!ToStr.FindChar('.', DotPos))
			{
				Warnings.Add(FString::Printf(TEXT("Invalid connection target: %s"), *ToStr));
				continue;
			}
			ToNodeId = ToStr.Left(DotPos);
			ToPinName = ToStr.RightChop(DotPos + 1);

			UEdGraphNode** FromNodePtr = IdToNode.Find(FromNodeId);
			UEdGraphNode** ToNodePtr = IdToNode.Find(ToNodeId);

			if (!FromNodePtr)
			{
				Warnings.Add(FString::Printf(TEXT("Connection source node not found: %s"), *FromNodeId));
				continue;
			}
			if (!ToNodePtr)
			{
				Warnings.Add(FString::Printf(TEXT("Connection target node not found: %s"), *ToNodeId));
				continue;
			}

			UEdGraphPin* FromPin = FindPin(*FromNodePtr, FromPinName);
			UEdGraphPin* ToPin = FindPin(*ToNodePtr, ToPinName);

			if (!FromPin)
			{
				Warnings.Add(FString::Printf(TEXT("Source pin not found: %s.%s"), *FromNodeId, *FromPinName));
				continue;
			}
			if (!ToPin)
			{
				Warnings.Add(FString::Printf(TEXT("Target pin not found: %s.%s"), *ToNodeId, *ToPinName));
				continue;
			}

			FPinConnectionResponse Response = Schema->CanCreateConnection(FromPin, ToPin);
			if (Response.Response == CONNECT_RESPONSE_DISALLOW)
			{
				Warnings.Add(FString::Printf(TEXT("Cannot connect %s.%s -> %s.%s: %s"),
					*FromNodeId, *FromPinName, *ToNodeId, *ToPinName,
					*Response.Message.ToString()));
				continue;
			}

			Schema->TryCreateConnection(FromPin, ToPin);
		}
			}
	} // end graph node block

	// ── Apply CDO defaults ──────────────────────────────────────────────
	const TSharedPtr<FJsonObject>* DefaultsObj = nullptr;
	if (Args->TryGetObjectField(TEXT("defaults"), DefaultsObj))
	{
		UObject* CDO = Blueprint->GeneratedClass
			? Blueprint->GeneratedClass->GetDefaultObject()
			: nullptr;
		if (CDO)
		{
			const TArray<TSharedPtr<FJsonValue>>* DefaultProperties = nullptr;
			if ((*DefaultsObj)->TryGetArrayField(TEXT("properties"), DefaultProperties))
			{
				for (const TSharedPtr<FJsonValue>& PropVal : *DefaultProperties)
				{
					const TSharedPtr<FJsonObject>* PropObjPtr = nullptr;
					if (!PropVal->TryGetObject(PropObjPtr)) continue;
					const TSharedPtr<FJsonObject>& PropObj = *PropObjPtr;

					FString PropName = PropObj->GetStringField(TEXT("name"));
					FString DefaultValue;
					if (!PropObj->TryGetStringField(TEXT("default_value"), DefaultValue))
						PropObj->TryGetStringField(TEXT("value"), DefaultValue);
					if (PropName.IsEmpty() || DefaultValue.IsEmpty()) continue;

					FProperty* Prop = nullptr;
					void* Container = CDO;
					FString FindErr;
					if (FBridgeAssetModifier::FindPropertyByPath(CDO, PropName, Prop, Container, FindErr))
					{
					}
					else
					{
						Prop = CDO->GetClass()->FindPropertyByName(*PropName);
						Container = CDO;
					}
					if (!Prop) continue;

					void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Container);
					if (!ValuePtr) continue;

					Prop->ImportText_Direct(*DefaultValue, ValuePtr, CDO, PPF_None);
				}
			}
		}
	}

	// ── Compile ─────────────────────────────────────────────────────────
	FString CompileError;
	bool bCompiled = FBridgeAssetModifier::CompileBlueprint(Blueprint, CompileError);

	FBridgeAssetModifier::MarkPackageDirty(Blueprint);

	// ── Result ──────────────────────────────────────────────────────────
	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("parent_class"), ParentClassName);
	Result->SetBoolField(TEXT("is_new"), bIsNew);
	Result->SetBoolField(TEXT("compiled"), bCompiled);
	Result->SetNumberField(TEXT("node_count"), IdToNode.Num());

	TArray<TSharedPtr<FJsonValue>> NodeGuids;
	for (const auto& Pair : IdToNode)
	{
		TSharedPtr<FJsonObject> GuidObj = MakeShareable(new FJsonObject);
		GuidObj->SetStringField(TEXT("id"), Pair.Key);
		GuidObj->SetStringField(TEXT("guid"), Pair.Value->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		NodeGuids.Add(MakeShareable(new FJsonValueObject(GuidObj)));
	}
	Result->SetArrayField(TEXT("nodes"), NodeGuids);

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnVals;
		for (const FString& W : Warnings)
		{
			WarnVals.Add(MakeShareable(new FJsonValueString(W)));
		}
		Result->SetArrayField(TEXT("warnings"), WarnVals);
	}

	if (!CompileError.IsEmpty())
	{
		Result->SetStringField(TEXT("compile_error"), CompileError);
	}

	Result->SetBoolField(TEXT("needs_save"), true);

	UE_LOG(LogSoftUEBridgeEditor, Log, TEXT("create-blueprint-from-json: Created %d nodes in %s (compiled=%s)"),
		IdToNode.Num(), *AssetPath, bCompiled ? TEXT("true") : TEXT("false"));

	return FBridgeToolResult::Json(Result);
}

// ── Generic node creation (reflection-driven, no per-type hardcoding) ────────

UEdGraphNode* UCreateBlueprintFromJsonTool::CreateNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const TSharedPtr<FJsonObject>& NodeJson,
	TMap<FString, UEdGraphNode*>& IdMap,
	TArray<FString>& OutWarnings)
{
	FString NodeId = NodeJson->GetStringField(TEXT("id"));
	FString NodeType = ResolveNodeType(NodeJson->GetStringField(TEXT("type")));
	if (NodeId.IsEmpty() || NodeType.IsEmpty())
	{
		OutWarnings.Add(TEXT("Skipping node with missing id or type"));
		return nullptr;
	}

	// 1. Resolve UE class
	FString ClassError;
	UClass* NodeClass = FBridgePropertySerializer::ResolveClass(NodeType, ClassError);
	if (!NodeClass)
	{
		NodeClass = FindFirstObject<UClass>(*NodeType, EFindFirstObjectOptions::ExactClass);
	}
	if (!NodeClass)
	{
		OutWarnings.Add(FString::Printf(TEXT("Node class not found: %s"), *NodeType));
		return nullptr;
	}
	if (!NodeClass->IsChildOf<UEdGraphNode>())
	{
		OutWarnings.Add(FString::Printf(TEXT("'%s' is not an EdGraphNode subclass"), *NodeType));
		return nullptr;
	}

	// 2. Create node
	UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph, NodeClass, NAME_None, RF_Transactional);
	Node->CreateNewGuid();

	// ── Pre-pin reference setup (generic FMemberReference detection) ───
	// Single loop handles Event, CallFunction, Message, VariableGet/Set —
	// any K2Node that stores its target in a FMemberReference UPROPERTY.
	{
		bool bHandledByGenericLoop = false;

		// CustomEvent: needs both CustomFunctionName (FName) AND EventReference
		if (UK2Node_CustomEvent* CustomNode = Cast<UK2Node_CustomEvent>(Node))
		{
			FString EventName;
			if (NodeJson->TryGetStringField(TEXT("event"), EventName) && !EventName.IsEmpty())
			{
				CustomNode->CustomFunctionName = FName(*EventName);
			}
			// Fall through to generic loop so EventReference gets set too
		}

		for (TFieldIterator<FStructProperty> It(Node->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			FStructProperty* StructProp = *It;
			if (StructProp->Struct != FMemberReference::StaticStruct())
				continue;

			FString PropName = StructProp->GetName();       // e.g. "EventReference"
			FString JsonField = PropName;
			JsonField.RemoveFromEnd(TEXT("Reference"));     // "Event"
			JsonField.ToLowerInline();                      // "event"

			FString MemberName;
			if (!NodeJson->TryGetStringField(JsonField, MemberName) || MemberName.IsEmpty())
				continue;

			FMemberReference* Ref = StructProp->ContainerPtrToValuePtr<FMemberReference>(Node);

			// Resolve class: try _class_path first, then _parent_class (legacy)
			UClass* MemberClass = nullptr;
			FString ClassPath;
			if (NodeJson->TryGetStringField(JsonField + TEXT("_class_path"), ClassPath) && !ClassPath.IsEmpty())
			{
				MemberClass = LoadClass<UObject>(nullptr, *ClassPath);
			}
			else if (NodeJson->TryGetStringField(JsonField + TEXT("_parent_class"), ClassPath) && !ClassPath.IsEmpty())
			{
				MemberClass = LoadClass<UObject>(nullptr, *ClassPath);
			}

			if (MemberClass)
			{
				Ref->SetExternalMember(FName(*MemberName), MemberClass);
			}
			else if (JsonField == TEXT("variable"))
			{
				// Variables default to self context (defined in this blueprint)
				Ref->SetSelfMember(FName(*MemberName));
			}
			else
			{
				// Events/functions: search parent class hierarchy
				UFunction* Func = FindFunctionByName(MemberName, Blueprint->ParentClass);
				if (Func)
					Ref->SetExternalMember(FName(*MemberName), Func->GetOuterUClass());
				else
					Ref->SetExternalMember(FName(*MemberName), Blueprint->ParentClass);
			}
		}

		// Timeline: set timeline name before AllocateDefaultPins
		if (UK2Node_Timeline* TimelineNode = Cast<UK2Node_Timeline>(Node))
		{
			FString Val;
			if (NodeJson->TryGetStringField(TEXT("timeline_name"), Val) && !Val.IsEmpty())
				TimelineNode->TimelineName = FName(*Val);
		}

		// BreakStruct: set struct type before AllocateDefaultPins
		if (UK2Node_BreakStruct* BreakNode = Cast<UK2Node_BreakStruct>(Node))
		{
			FString Val;
			if (NodeJson->TryGetStringField(TEXT("struct_type"), Val) && !Val.IsEmpty())
				BreakNode->StructType = Cast<UScriptStruct>(StaticFindObject(UScriptStruct::StaticClass(), nullptr, *Val));
		}

		// ComponentBoundEvent: set component/delegate properties before AllocateDefaultPins
		if (UK2Node_ComponentBoundEvent* CompNode = Cast<UK2Node_ComponentBoundEvent>(Node))
		{
			FString Val;
			if (NodeJson->TryGetStringField(TEXT("component_name"), Val) && !Val.IsEmpty())
				CompNode->ComponentPropertyName = FName(*Val);
			if (NodeJson->TryGetStringField(TEXT("delegate_name"), Val) && !Val.IsEmpty())
				CompNode->DelegatePropertyName = FName(*Val);
			if (NodeJson->TryGetStringField(TEXT("delegate_owner_class"), Val) && !Val.IsEmpty())
				CompNode->DelegateOwnerClass = LoadClass<UObject>(nullptr, *Val);
		}

		// DynamicCast: JSON "cast_to" != property name "TargetType"
		if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
		{
			FString CastTo;
			if (NodeJson->TryGetStringField(TEXT("cast_to"), CastTo) && !CastTo.IsEmpty())
			{
				FString Err;
				UClass* TargetType = FBridgePropertySerializer::ResolveClass(CastTo, Err);
				// Full path: /Script/Engine.Something or /Game/.../BP_Name.BP_Name_C
				if (!TargetType) TargetType = FindObject<UClass>(nullptr, *CastTo);
				if (!TargetType) TargetType = LoadClass<UObject>(nullptr, *CastTo);
				// Old format: short name
				if (!TargetType) TargetType = FindFirstObject<UClass>(*CastTo, EFindFirstObjectOptions::None);
				if (TargetType) CastNode->TargetType = TargetType;
			}
		}
	}

	// 3. Apply properties BEFORE AllocateDefaultPins (generic)

	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (NodeJson->TryGetObjectField(TEXT("properties"), PropsObj))
	{
		_ApplyReflectionProperties(Node, *PropsObj, OutWarnings);
	}

	// Ensure ProxyFactoryClass and ProxyFactoryFunctionName are set for latent/async nodes.
	// Set directly from JSON properties (protected members, ImportText can't reach them).
	{
		FClassProperty* P = FindFProperty<FClassProperty>(Node->GetClass(), TEXT("ProxyFactoryClass"));
		FNameProperty* NP = FindFProperty<FNameProperty>(Node->GetClass(), TEXT("ProxyFactoryFunctionName"));
		if (P && NP)
		{
			UClass** Ptr = P->ContainerPtrToValuePtr<UClass*>(Node);
			FName* NamePtr = NP->ContainerPtrToValuePtr<FName>(Node);

			// Read from JSON properties directly (bypass ImportText which might not find protected members)
			static const FString ClassPrefix = TEXT("/Script/CoreUObject.Class'");
			const TSharedPtr<FJsonObject>* PropsObj2 = nullptr;
			if (NodeJson->TryGetObjectField(TEXT("properties"), PropsObj2))
			{
				FString PFCVal;
				if ((*PropsObj2)->TryGetStringField(TEXT("ProxyFactoryClass"), PFCVal) && !PFCVal.IsEmpty())
				{
					// Strip ExportText wrapper: /Script/CoreUObject.Class'/Path' -> /Path
					if (PFCVal.StartsWith(ClassPrefix))
					{
						PFCVal = PFCVal.Mid(ClassPrefix.Len());
						if (PFCVal.EndsWith(TEXT("'"))) PFCVal = PFCVal.LeftChop(1);
					}
					*Ptr = LoadClass<UObject>(nullptr, *PFCVal);
				}
				FString PFFNVal;
				if ((*PropsObj2)->TryGetStringField(TEXT("ProxyFactoryFunctionName"), PFFNVal) && !PFFNVal.IsEmpty())
					*NamePtr = FName(*PFFNVal);
				// ProxyClass: specific to K2Node_LatentAbilityCall
				FClassProperty* PC = FindFProperty<FClassProperty>(Node->GetClass(), TEXT("ProxyClass"));
				if (PC)
				{
					FString PCVal;
					if ((*PropsObj2)->TryGetStringField(TEXT("ProxyClass"), PCVal) && !PCVal.IsEmpty())
					{
						UClass** PCPtr = PC->ContainerPtrToValuePtr<UClass*>(Node);
						if (PCVal.StartsWith(ClassPrefix))
						{
							PCVal = PCVal.Mid(ClassPrefix.Len());
							if (PCVal.EndsWith(TEXT("'"))) PCVal = PCVal.LeftChop(1);
						}
						*PCPtr = LoadClass<UObject>(nullptr, *PCVal);
						if (!*PCPtr)
						{
							UFunction* FoundFunc = FindFirstObject<UFunction>(*NamePtr->ToString(), EFindFirstObjectOptions::None);
							if (FoundFunc) *PCPtr = FoundFunc->GetOuterUClass();
						}
					}
				}

			}

				// Fallback: resolve ProxyFactoryClass via UFUNCTION search
				if (!*Ptr && !NamePtr->IsNone())
				{
					FString FuncName = NamePtr->ToString();
					UFunction* FoundFunc = FindFirstObject<UFunction>(*FuncName, EFindFirstObjectOptions::None);
					if (FoundFunc) *Ptr = FoundFunc->GetOuterUClass();
					if (!*Ptr)
					{
						FString Derived = FuncName;
						Derived.RemoveFromStart(TEXT("Create"));
						*Ptr = FindFirstObject<UClass>(*Derived, EFindFirstObjectOptions::None);
						if (!*Ptr) *Ptr = FindFirstObject<UClass>(*(TEXT("U") + Derived), EFindFirstObjectOptions::None);
						if (!*Ptr) *Ptr = LoadClass<UObject>(nullptr, *Derived);
					}
				}
		}
	}

	// Ensure MacroGraphReference is set for macro instance nodes.
	// Read from JSON properties (FGraphReference not reachable via ImportText).
	{
		FStructProperty* RefProp = FindFProperty<FStructProperty>(Node->GetClass(), TEXT("MacroGraphReference"));
		if (RefProp && RefProp->Struct == FGraphReference::StaticStruct())
		{
			FGraphReference* Ref = RefProp->ContainerPtrToValuePtr<FGraphReference>(Node);
			const TSharedPtr<FJsonObject>* PropsObj3 = nullptr;
			if (NodeJson->TryGetObjectField(TEXT("properties"), PropsObj3))
			{
				FString BPVal;
				if ((*PropsObj3)->TryGetStringField(TEXT("MacroGraphReference.GraphBlueprint"), BPVal) && !BPVal.IsEmpty())
				{
					UBlueprint* MacroBP = LoadObject<UBlueprint>(nullptr, *BPVal);
					if (MacroBP)
					{
						FString GraphName;
						(*PropsObj3)->TryGetStringField(TEXT("MacroGraphReference.MacroGraph"), GraphName);
						for (UEdGraph* Graph : MacroBP->MacroGraphs)
						{
							if (Graph->GetName() == GraphName)
							{
								Ref->SetGraph(Graph);
								break;
							}
						}
					}
				}
			}
		}
	}

	// 4. Allocate pins + add to graph
	if (UK2Node* K2Node = Cast<UK2Node>(Node))
	{
		K2Node->AllocateDefaultPins();
	}
	else
	{
		Node->AllocateDefaultPins();
	}
	Graph->AddNode(Node, false, false);

	// 4. Apply properties via reflection (generic, works for ALL node types)
	//    Accepts both:  "variable": "Health"  (friendly, mapped to VariableReference)
	//              and: "properties": {"VariableReference.MemberName": "Health"} (advanced)


	// 5. Position & comment ──────────────────────────────────────────────
	const TArray<TSharedPtr<FJsonValue>>* PosArray = nullptr;
	if (NodeJson->TryGetArrayField(TEXT("position"), PosArray) && PosArray->Num() >= 2)
	{
		Node->NodePosX = FMath::RoundToInt((*PosArray)[0]->AsNumber());
		Node->NodePosY = FMath::RoundToInt((*PosArray)[1]->AsNumber());
	}
	FString Comment;
	if (NodeJson->TryGetStringField(TEXT("comment"), Comment))
	{
		Node->NodeComment = Comment;
	}

	return Node;
}

// ── Friendly field → UE property path mapping ────────────────────────────────

static const TMap<FString, FString> FriendlyPropPaths = {
	{ TEXT("variable"),  TEXT("VariableReference.MemberName") },
};

void UCreateBlueprintFromJsonTool::_ApplyFriendlyProps(
	UEdGraphNode* Node,
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& NodeJson,
	TArray<FString>& OutWarnings)
{
	// Variable reference
	FString VarName;
	if (NodeJson->TryGetStringField(TEXT("variable"), VarName) && !VarName.IsEmpty())
	{
		FString VarParentClass;
		NodeJson->TryGetStringField(TEXT("variable_parent_class"), VarParentClass);
		if (!VarParentClass.IsEmpty())
		{
			UClass* ParentCls = LoadClass<UObject>(nullptr, *VarParentClass);
			if (ParentCls)
			{
				if (UK2Node_VariableGet* GetNode = Cast<UK2Node_VariableGet>(Node))
					GetNode->VariableReference.SetExternalMember(FName(*VarName), ParentCls);
				else if (UK2Node_VariableSet* SetNode = Cast<UK2Node_VariableSet>(Node))
					SetNode->VariableReference.SetExternalMember(FName(*VarName), ParentCls);
			}
		}
		else
		{
			if (UK2Node_VariableGet* GetNode = Cast<UK2Node_VariableGet>(Node))
				GetNode->VariableReference.SetSelfMember(FName(*VarName));
			else if (UK2Node_VariableSet* SetNode = Cast<UK2Node_VariableSet>(Node))
				SetNode->VariableReference.SetSelfMember(FName(*VarName));
		}
	}
}

void UCreateBlueprintFromJsonTool::_ApplyReflectionProperties(
	UObject* Target,
	const TSharedPtr<FJsonObject>& Props,
	TArray<FString>& OutWarnings)
{
	for (const auto& Pair : Props->Values)
	{
		const FString& PropPath = Pair.Key;
		const TSharedPtr<FJsonValue>& Value = Pair.Value;

		FProperty* Prop = nullptr;
		void* Container = Target;

		// Support "Nested.Path.PropertyName"
		FString FindErr;
		if (!FBridgeAssetModifier::FindPropertyByPath(Target, PropPath, Prop, Container, FindErr))
		{
			// Try direct name
			Prop = Target->GetClass()->FindPropertyByName(*PropPath);
			Container = Target;
		}
		if (!Prop)
		{
			OutWarnings.Add(FString::Printf(TEXT("Property not found: %s"), *PropPath));
			continue;
		}

		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Container);
		if (!ValuePtr) continue;

		FString ValueStr;
		if (Value->Type == EJson::String)
			ValueStr = Value->AsString();
		else if (Value->Type == EJson::Number)
			ValueStr = FString::Printf(TEXT("%g"), Value->AsNumber());
		else if (Value->Type == EJson::Boolean)
			ValueStr = Value->AsBool() ? TEXT("true") : TEXT("false");
		else
			continue;

		Prop->ImportText_Direct(*ValueStr, ValuePtr, Target, PPF_None);
	}
}

// ── Helpers ─────────────────────────────────────────────────────────────────

UEdGraphPin* UCreateBlueprintFromJsonTool::FindPin(UEdGraphNode* Node, const FString& PinName)
{
	if (!Node)
	{
		return nullptr;
	}

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
		{
			return Pin;
		}
	}
	return nullptr;
}

UFunction* UCreateBlueprintFromJsonTool::FindFunctionByName(const FString& FunctionName, UClass* ParentClass)
{
	// 1. Check parent class hierarchy
	if (ParentClass)
	{
		if (UFunction* Func = ParentClass->FindFunctionByName(*FunctionName))
		{
			return Func;
		}
	}

	// 2. Common utility classes
	static const TArray<FString> UtilityClassNames = {
		TEXT("KismetSystemLibrary"),
		TEXT("KismetMathLibrary"),
		TEXT("KismetArrayLibrary"),
		TEXT("KismetStringLibrary"),
		TEXT("KismetTextLibrary"),
		TEXT("GameplayStatics"),
		TEXT("KismetNodeHelperLibrary"),
		TEXT("BlueprintPlatformLibrary"),
		TEXT("KismetInputLibrary"),
		TEXT("KismetGuidLibrary"),
	};

	for (const FString& ClassName : UtilityClassNames)
	{
		UClass* UtilClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::ExactClass);
		if (!UtilClass)
		{
			// Try with U prefix
			UtilClass = FindFirstObject<UClass>(*(TEXT("U") + ClassName), EFindFirstObjectOptions::ExactClass);
		}
		if (UtilClass)
		{
			if (UFunction* Func = UtilClass->FindFunctionByName(*FunctionName))
			{
				return Func;
			}
		}
	}

	// 3. Global search (expensive but thorough)
	UFunction* Func = FindFirstObject<UFunction>(*FunctionName, EFindFirstObjectOptions::None);
	if (Func)
	{
		return Func;
	}

	// 4. Try with "K2_" prefix (common pattern for blueprint-callable wrappers)
	FString K2Name = TEXT("K2_") + FunctionName;
	Func = FindFirstObject<UFunction>(*K2Name, EFindFirstObjectOptions::None);
	if (Func) return Func;

	// 5. Search AbilityTask subclasses for static factory functions
	{
		UFunction* Found = FindFirstObject<UFunction>(*FunctionName, EFindFirstObjectOptions::None);
		if (Found)
		{
			UClass* OuterClass = Found->GetOuterUClass();
			if (OuterClass)
			{
				static UClass* AbilityTaskClass = FindFirstObject<UClass>(TEXT("/Script/GameplayAbilities.AbilityTask"), EFindFirstObjectOptions::ExactClass);
				static UClass* GameplayTaskClass = FindFirstObject<UClass>(TEXT("/Script/GameplayTasks.GameplayTask"), EFindFirstObjectOptions::ExactClass);
				if ((AbilityTaskClass && OuterClass->IsChildOf(AbilityTaskClass)) ||
					(GameplayTaskClass && OuterClass->IsChildOf(GameplayTaskClass)) ||
					OuterClass->GetName().Contains(TEXT("AbilityTask")))
				{
					return Found;
				}
			}
		}
	}


	return nullptr;
}

FName UCreateBlueprintFromJsonTool::PinCategoryFromString(const FString& CategoryName)
{
	FString Lower = CategoryName.ToLower();

	if (Lower == TEXT("bool"))            return UEdGraphSchema_K2::PC_Boolean;
	if (Lower == TEXT("byte"))            return UEdGraphSchema_K2::PC_Byte;
	if (Lower == TEXT("int") || Lower == TEXT("int32") || Lower == TEXT("integer"))
		return UEdGraphSchema_K2::PC_Int;
	if (Lower == TEXT("int64"))           return UEdGraphSchema_K2::PC_Int64;
	if (Lower == TEXT("float") || Lower == TEXT("real") || Lower == TEXT("double"))
		return UEdGraphSchema_K2::PC_Real;
	if (Lower == TEXT("string"))          return UEdGraphSchema_K2::PC_String;
	if (Lower == TEXT("name"))            return UEdGraphSchema_K2::PC_Name;
	if (Lower == TEXT("text"))            return UEdGraphSchema_K2::PC_Text;
	if (Lower == TEXT("object"))          return UEdGraphSchema_K2::PC_Object;
	if (Lower == TEXT("class"))           return UEdGraphSchema_K2::PC_Class;
	if (Lower == TEXT("soft_object"))     return UEdGraphSchema_K2::PC_SoftObject;
	if (Lower == TEXT("soft_class"))      return UEdGraphSchema_K2::PC_SoftClass;
	if (Lower == TEXT("struct"))          return UEdGraphSchema_K2::PC_Struct;
	if (Lower == TEXT("enum"))            return UEdGraphSchema_K2::PC_Enum;
	if (Lower == TEXT("interface"))       return UEdGraphSchema_K2::PC_Interface;
	if (Lower == TEXT("exec"))            return UEdGraphSchema_K2::PC_Exec;
	if (Lower == TEXT("wildcard"))        return UEdGraphSchema_K2::PC_Wildcard;

	return FName(*CategoryName);
}
