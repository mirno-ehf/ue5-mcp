#include "BlueprintMCPServer.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

// Helpers for UTransBuffer in UE 5.7: UndoCount is the number of
// transactions already undone (redoable), not the number available
// to undo. There is no GetRedoCount().
static int32 AvailableUndoCount(const UTransBuffer* T)
{
	return T ? T->UndoBuffer.Num() - T->GetUndoCount() : 0;
}
static int32 AvailableRedoCount(const UTransBuffer* T)
{
	return T ? T->GetUndoCount() : 0;
}

// ============================================================
// HandleUndo — undo the last editor action
// ============================================================

FString FBlueprintMCPServer::HandleUndo(const FString& Body)
{
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: undo()"));

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("undo requires editor mode."));
	}

	if (!GEditor)
	{
		return MakeErrorJson(TEXT("Editor not available."));
	}

	UTransBuffer* TransBuffer = Cast<UTransBuffer>(GEditor->Trans);
	if (!TransBuffer)
	{
		return MakeErrorJson(TEXT("Transaction buffer not available."));
	}

	if (AvailableUndoCount(TransBuffer) == 0)
	{
		return MakeErrorJson(TEXT("Nothing to undo."));
	}

	FString UndoDescription = TransBuffer->GetUndoContext(false).Title.ToString();

	bool bSuccess = GEditor->UndoTransaction();

	if (GEditor)
	{
		GEditor->RedrawAllViewports(true);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), bSuccess);
	Result->SetStringField(TEXT("undoneAction"), UndoDescription);
	Result->SetNumberField(TEXT("remainingUndoCount"), AvailableUndoCount(TransBuffer));
	Result->SetNumberField(TEXT("redoCount"), AvailableRedoCount(TransBuffer));

	return JsonToString(Result);
}

// ============================================================
// HandleRedo — redo the last undone action
// ============================================================

FString FBlueprintMCPServer::HandleRedo(const FString& Body)
{
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: redo()"));

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("redo requires editor mode."));
	}

	if (!GEditor)
	{
		return MakeErrorJson(TEXT("Editor not available."));
	}

	UTransBuffer* TransBuffer = Cast<UTransBuffer>(GEditor->Trans);
	if (!TransBuffer)
	{
		return MakeErrorJson(TEXT("Transaction buffer not available."));
	}

	if (AvailableRedoCount(TransBuffer) == 0)
	{
		return MakeErrorJson(TEXT("Nothing to redo."));
	}

	FString RedoDescription = TransBuffer->GetUndoContext(true).Title.ToString();

	bool bSuccess = GEditor->RedoTransaction();

	if (GEditor)
	{
		GEditor->RedrawAllViewports(true);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), bSuccess);
	Result->SetStringField(TEXT("redoneAction"), RedoDescription);
	Result->SetNumberField(TEXT("undoCount"), AvailableUndoCount(TransBuffer));
	Result->SetNumberField(TEXT("remainingRedoCount"), AvailableRedoCount(TransBuffer));

	return JsonToString(Result);
}

// ============================================================
// HandleBeginTransaction — begin a named undo transaction
// ============================================================

FString FBlueprintMCPServer::HandleBeginTransaction(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body."));
	}

	FString Description;
	if (!Json->TryGetStringField(TEXT("description"), Description) || Description.IsEmpty())
	{
		return MakeErrorJson(TEXT("Missing required field: 'description' (human-readable description of the transaction)."));
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: begin_transaction('%s')"), *Description);

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("begin_transaction requires editor mode."));
	}

	if (!GEditor)
	{
		return MakeErrorJson(TEXT("Editor not available."));
	}

	int32 TransactionIndex = GEditor->BeginTransaction(FText::FromString(Description));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("description"), Description);
	Result->SetNumberField(TEXT("transactionIndex"), TransactionIndex);

	return JsonToString(Result);
}

// ============================================================
// HandleEndTransaction — end the current undo transaction
// ============================================================

FString FBlueprintMCPServer::HandleEndTransaction(const FString& Body)
{
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: end_transaction()"));

	if (!bIsEditor)
	{
		return MakeErrorJson(TEXT("end_transaction requires editor mode."));
	}

	if (!GEditor)
	{
		return MakeErrorJson(TEXT("Editor not available."));
	}

	int32 TransactionIndex = GEditor->EndTransaction();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("transactionIndex"), TransactionIndex);

	return JsonToString(Result);
}
