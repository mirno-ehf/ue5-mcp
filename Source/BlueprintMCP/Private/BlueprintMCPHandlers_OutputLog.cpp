#include "BlueprintMCPServer.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

// ============================================================
// Custom output device to capture log messages
// ============================================================

class FMCPLogCapture : public FOutputDevice
{
public:
	struct FLogEntry
	{
		FString Message;
		FString Category;
		ELogVerbosity::Type Verbosity;
		double Timestamp;
	};

	TArray<FLogEntry> Entries;
	FCriticalSection EntriesLock;
	int32 MaxEntries = 1000;
	bool bCapturing = false;

	void StartCapture()
	{
		FScopeLock Lock(&EntriesLock);
		Entries.Reset();
		bCapturing = true;
		GLog->AddOutputDevice(this);
	}

	void StopCapture()
	{
		bCapturing = false;
		GLog->RemoveOutputDevice(this);
	}

	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		if (!bCapturing) return;

		FScopeLock Lock(&EntriesLock);
		if (Entries.Num() >= MaxEntries)
		{
			// Remove oldest entries
			Entries.RemoveAt(0, Entries.Num() / 4);
		}

		FLogEntry Entry;
		Entry.Message = FString(V);
		Entry.Category = Category.ToString();
		Entry.Verbosity = Verbosity;
		Entry.Timestamp = FPlatformTime::Seconds();
		Entries.Add(MoveTemp(Entry));
	}
};

static FMCPLogCapture GLogCapture;

// ============================================================
// HandleGetOutputLog — get recent output log entries
// ============================================================

FString FBlueprintMCPServer::HandleGetOutputLog(const FString& Body)
{
	TSharedPtr<FJsonObject> Json = ParseBodyJson(Body);
	if (!Json.IsValid())
	{
		return MakeErrorJson(TEXT("Invalid JSON body."));
	}

	int32 MaxLines = 100;
	double MaxLinesDouble = 100;
	if (Json->TryGetNumberField(TEXT("maxLines"), MaxLinesDouble))
	{
		MaxLines = FMath::Clamp((int32)MaxLinesDouble, 1, 1000);
	}

	FString Filter;
	Json->TryGetStringField(TEXT("filter"), Filter);

	FString VerbosityFilter;
	Json->TryGetStringField(TEXT("verbosity"), VerbosityFilter);

	// Start capturing if not already
	if (!GLogCapture.bCapturing)
	{
		GLogCapture.StartCapture();
	}

	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: get_output_log(maxLines=%d)"), MaxLines);

	FScopeLock Lock(&GLogCapture.EntriesLock);

	auto VerbosityToString = [](ELogVerbosity::Type V) -> FString {
		switch (V)
		{
		case ELogVerbosity::Fatal:   return TEXT("Fatal");
		case ELogVerbosity::Error:   return TEXT("Error");
		case ELogVerbosity::Warning: return TEXT("Warning");
		case ELogVerbosity::Display: return TEXT("Display");
		case ELogVerbosity::Log:     return TEXT("Log");
		case ELogVerbosity::Verbose: return TEXT("Verbose");
		default:                     return TEXT("Unknown");
		}
	};

	TArray<TSharedPtr<FJsonValue>> LogArray;
	// Iterate from end to get most recent entries
	int32 StartIdx = FMath::Max(0, GLogCapture.Entries.Num() - MaxLines * 2); // overscan for filtering
	for (int32 i = GLogCapture.Entries.Num() - 1; i >= StartIdx && LogArray.Num() < MaxLines; --i)
	{
		const FMCPLogCapture::FLogEntry& Entry = GLogCapture.Entries[i];

		// Apply verbosity filter
		if (!VerbosityFilter.IsEmpty())
		{
			if (VerbosityFilter.Equals(TEXT("Error"), ESearchCase::IgnoreCase) &&
				Entry.Verbosity != ELogVerbosity::Error && Entry.Verbosity != ELogVerbosity::Fatal)
			{
				continue;
			}
			if (VerbosityFilter.Equals(TEXT("Warning"), ESearchCase::IgnoreCase) &&
				Entry.Verbosity != ELogVerbosity::Warning)
			{
				continue;
			}
		}

		// Apply text filter
		if (!Filter.IsEmpty() && !Entry.Message.Contains(Filter, ESearchCase::IgnoreCase) &&
			!Entry.Category.Contains(Filter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		TSharedRef<FJsonObject> EntryObj = MakeShared<FJsonObject>();
		EntryObj->SetStringField(TEXT("message"), Entry.Message);
		EntryObj->SetStringField(TEXT("category"), Entry.Category);
		EntryObj->SetStringField(TEXT("verbosity"), VerbosityToString(Entry.Verbosity));

		LogArray.Insert(MakeShared<FJsonValueObject>(EntryObj), 0); // Insert at front to maintain order
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), LogArray.Num());
	Result->SetNumberField(TEXT("totalCaptured"), GLogCapture.Entries.Num());
	Result->SetArrayField(TEXT("entries"), LogArray);
	Result->SetBoolField(TEXT("capturing"), GLogCapture.bCapturing);

	return JsonToString(Result);
}

// ============================================================
// HandleClearOutputLog — clear captured log entries
// ============================================================

FString FBlueprintMCPServer::HandleClearOutputLog(const FString& Body)
{
	UE_LOG(LogTemp, Display, TEXT("BlueprintMCP: clear_output_log()"));

	FScopeLock Lock(&GLogCapture.EntriesLock);
	int32 PreviousCount = GLogCapture.Entries.Num();
	GLogCapture.Entries.Reset();

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("clearedEntries"), PreviousCount);

	return JsonToString(Result);
}
