// Copyright ShineTheSky 2026. All Rights Reserved.

#include "Tools/Asset/ModifyEnumTool.h"
#include "SoftUEBridgeEditorModule.h"
#include "Engine/UserDefinedEnum.h"
#include "Kismet2/EnumEditorUtils.h"
#include "UObject/SavePackage.h"
#include "Editor.h"

FString UModifyEnumTool::GetToolDescription() const
{
	return TEXT("Modify a UserDefinedEnum: add, remove, or rename enumerators.");
}

TMap<FString, FBridgeSchemaProperty> UModifyEnumTool::GetInputSchema() const
{
	TMap<FString, FBridgeSchemaProperty> Schema;

	FBridgeSchemaProperty AssetPath;
	AssetPath.Type = TEXT("string");
	AssetPath.Description = TEXT("Asset path to the UserDefinedEnum");
	AssetPath.bRequired = true;
	Schema.Add(TEXT("asset_path"), AssetPath);

	FBridgeSchemaProperty Action;
	Action.Type = TEXT("string");
	Action.Description = TEXT("Action: 'add', 'remove', or 'rename'");
	Action.bRequired = true;
	Schema.Add(TEXT("action"), Action);

	FBridgeSchemaProperty Name;
	Name.Type = TEXT("string");
	Name.Description = TEXT("Enumerator name (authored name). For 'add': new name. For 'remove': name to remove. For 'rename': old name.");
	Name.bRequired = true;
	Schema.Add(TEXT("name"), Name);

	FBridgeSchemaProperty NewName;
	NewName.Type = TEXT("string");
	NewName.Description = TEXT("For 'rename' action only: new authored name.");
	NewName.bRequired = false;
	Schema.Add(TEXT("new_name"), NewName);

	return Schema;
}

TArray<FString> UModifyEnumTool::GetRequiredParams() const
{
	return { TEXT("asset_path"), TEXT("action"), TEXT("name") };
}

FBridgeToolResult UModifyEnumTool::Execute(
	const TSharedPtr<FJsonObject>& Arguments,
	const FBridgeToolContext& Context)
{
	FString AssetPath = GetStringArgOrDefault(Arguments, TEXT("asset_path"));
	FString Action = GetStringArgOrDefault(Arguments, TEXT("action"));
	FString Name = GetStringArgOrDefault(Arguments, TEXT("name"));
	FString NewName = GetStringArgOrDefault(Arguments, TEXT("new_name"));

	if (AssetPath.IsEmpty() || Action.IsEmpty())
	{
		return FBridgeToolResult::Error(TEXT("asset_path and action are required"));
	}

	UUserDefinedEnum* Enum = LoadObject<UUserDefinedEnum>(nullptr, *AssetPath);
	if (!Enum)
	{
		return FBridgeToolResult::Error(FString::Printf(TEXT("Failed to load UserDefinedEnum: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("action"), Action);

	const FString ActionLower = Action.ToLower();

	if (ActionLower == TEXT("add"))
	{
		if (Name.IsEmpty())
		{
			return FBridgeToolResult::Error(TEXT("'name' is required for 'add' action"));
		}

		// Check if name already exists
		const int32 NumEnums = Enum->NumEnums();
		for (int32 i = 0; i < NumEnums; ++i)
		{
			if (Enum->GetAuthoredNameStringByIndex(i).Equals(Name, ESearchCase::IgnoreCase))
			{
				return FBridgeToolResult::Error(FString::Printf(TEXT("Enumerator '%s' already exists"), *Name));
			}
		}

		Enum->Modify();

		// Add a new enumerator (generates NewEnumeratorN)
		FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);

		// New entry is second-to-last (UE adds before the _MAX sentinel)
		const int32 NewIndex = FMath::Max(0, Enum->NumEnums() - 2);
		FEnumEditorUtils::SetEnumeratorDisplayName(Enum, NewIndex, FText::FromString(Name));

		FEnumEditorUtils::EnsureAllDisplayNamesExist(Enum);

		Enum->MarkPackageDirty();

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("name"), Name);
		Result->SetNumberField(TEXT("index"), NewIndex);
		Result->SetStringField(TEXT("internal_name"), Enum->GetNameStringByIndex(NewIndex));
	}
	else if (ActionLower == TEXT("remove"))
	{
		if (Name.IsEmpty())
		{
			return FBridgeToolResult::Error(TEXT("'name' is required for 'remove' action"));
		}

		int32 FoundIndex = INDEX_NONE;
		const int32 NumEnums = Enum->NumEnums();
		for (int32 i = 0; i < NumEnums; ++i)
		{
			if (Enum->GetAuthoredNameStringByIndex(i).Equals(Name, ESearchCase::IgnoreCase))
			{
				FoundIndex = i;
				break;
			}
		}

		if (FoundIndex == INDEX_NONE)
		{
			return FBridgeToolResult::Error(FString::Printf(TEXT("Enumerator '%s' not found"), *Name));
		}

		Enum->Modify();
		FEnumEditorUtils::RemoveEnumeratorFromUserDefinedEnum(Enum, FoundIndex);
		FEnumEditorUtils::EnsureAllDisplayNamesExist(Enum);
		Enum->MarkPackageDirty();

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("removed"), Name);
	}
	else if (ActionLower == TEXT("rename"))
	{
		if (Name.IsEmpty() || NewName.IsEmpty())
		{
			return FBridgeToolResult::Error(TEXT("'name' and 'new_name' are required for 'rename' action"));
		}

		int32 FoundIndex = INDEX_NONE;
		const int32 NumEnums = Enum->NumEnums();
		for (int32 i = 0; i < NumEnums; ++i)
		{
			if (Enum->GetAuthoredNameStringByIndex(i).Equals(Name, ESearchCase::IgnoreCase))
			{
				FoundIndex = i;
				break;
			}
		}

		if (FoundIndex == INDEX_NONE)
		{
			return FBridgeToolResult::Error(FString::Printf(TEXT("Enumerator '%s' not found"), *Name));
		}

		Enum->Modify();
		FEnumEditorUtils::SetEnumeratorDisplayName(Enum, FoundIndex, FText::FromString(NewName));
		FEnumEditorUtils::EnsureAllDisplayNamesExist(Enum);
		Enum->MarkPackageDirty();

		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("old_name"), Name);
		Result->SetStringField(TEXT("new_name"), NewName);
	}
	else
	{
		return FBridgeToolResult::Error(FString::Printf(TEXT("Unknown action '%s'. Use 'add', 'remove', or 'rename'."), *Action));
	}

	return FBridgeToolResult::Json(Result);
}
