#include "PraxBusSubsystem.h"

#include "Core/PraxBusWire.h"
#include "Core/PraxJson.h"
#include "PraxsuiteLog.h"
#include "PraxsuiteSubsystem.h"

#include "Engine/GameInstance.h"
#include "IWebSocket.h"
#include "TimerManager.h"
#include "WebSocketsModule.h"

namespace
{
	/** The core speaks std::string; the engine speaks FString. One conversion pair, used both ways. */
	std::string ToStd(const FString& In)
	{
		return std::string(TCHAR_TO_UTF8(*In));
	}

	FString ToFString(const std::string& In)
	{
		return FString(UTF8_TO_TCHAR(In.c_str()));
	}

	/** JSON string literal, escaped. Small enough to do here rather than pull the codec across. */
	FString Quote(const FString& Value)
	{
		FString Escaped = Value;
		Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	}
}

void UPraxBusSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// The module must be loaded before a socket can be created, and on some platforms it is not
	// loaded by default.
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("WebSockets")))
	{
		FModuleManager::Get().LoadModule(TEXT("WebSockets"));
	}
}

void UPraxBusSubsystem::Deinitialize()
{
	Disconnect();
	Super::Deinitialize();
}

void UPraxBusSubsystem::Connect()
{
	if (State == EPraxBusState::Connected || State == EPraxBusState::Connecting)
	{
		return;
	}

	UPraxsuiteSubsystem* Praxsuite = GetGameInstance()
		? GetGameInstance()->GetSubsystem<UPraxsuiteSubsystem>()
		: nullptr;

	const FString AccessToken = Praxsuite ? Praxsuite->GetSessionTokenForBus() : FString();
	if (AccessToken.IsEmpty())
	{
		UE_LOG(LogPraxsuite, Error,
			TEXT("The Event Bus needs a signed-in end user - it authenticates with the session "
				 "token, not with the workspace key. Sign in first."));
		return;
	}

	// The host comes from the transport rather than from settings: a game that called
	// Configure() at runtime with a different host would otherwise have its bus pointed at the
	// stale configured one, and the wrong tier is a plain 404 nobody can diagnose.
	const std::string Url = Prax::BusWire::SocketUrl(
		ToStd(Praxsuite->GetGatewayHostForBus()), ToStd(AccessToken));

	bClosedByUs = false;
	bHandshakeDone = false;
	Buffer.Reset();
	SetState(State == EPraxBusState::Disconnected
		? EPraxBusState::Connecting
		: EPraxBusState::Reconnecting);

	// No protocol argument: SignalR negotiates its own protocol in the first frame, and naming a
	// WebSocket subprotocol here makes the handshake fail.
	Socket = FWebSocketsModule::Get().CreateWebSocket(ToFString(Url), TEXT(""));

	Socket->OnConnected().AddLambda([this]()
	{
		// The handshake goes first and nothing else may precede it.
		Socket->Send(ToFString(Prax::BusWire::Frame(Prax::BusWire::HandshakeFrame)));
	});

	Socket->OnMessage().AddLambda([this](const FString& Message)
	{
		Buffer += Message;

		const Prax::BusWire::FFrames Split = Prax::BusWire::SplitFrames(ToStd(Buffer));
		Buffer = ToFString(Split.Remainder);

		for (const std::string& Frame : Split.Frames)
		{
			HandleFrame(ToFString(Frame));
		}
	});

	Socket->OnConnectionError().AddLambda([this](const FString& Error)
	{
		UE_LOG(LogPraxsuite, Warning, TEXT("Event Bus connection failed: %s"), *Error);
		ScheduleReconnect();
	});

	Socket->OnClosed().AddLambda([this](int32 StatusCode, const FString& Reason, bool)
	{
		UE_LOG(LogPraxsuite, Log, TEXT("Event Bus closed (%d): %s"), StatusCode, *Reason);
		Joined.Reset();
		ScheduleReconnect();
	});

	Socket->Connect();
}

void UPraxBusSubsystem::Disconnect()
{
	bClosedByUs = true;
	Wanted.Reset();
	Joined.Reset();

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ReconnectTimer);
	}

	if (Socket.IsValid() && Socket->IsConnected())
	{
		Socket->Close();
	}
	Socket.Reset();

	SetState(EPraxBusState::Disconnected);
}

void UPraxBusSubsystem::Join(const FString& BusKey, const FString& Ticket)
{
	std::string Key;
	std::string Error;
	if (!Prax::BusWire::CheckBusKey(ToStd(BusKey), Key, Error))
	{
		UE_LOG(LogPraxsuite, Error, TEXT("Event Bus: %s"), *ToFString(Error));
		return;
	}

	const FString Normalized = ToFString(Key);
	Wanted.Add(Normalized);

	if (State != EPraxBusState::Connected)
	{
		// Connect re-joins everything in Wanted once the handshake lands, so this is not lost.
		Connect();
		return;
	}

	const FString Arguments = FString::Printf(TEXT("[%s,%s]"),
		*Quote(Normalized),
		Ticket.IsEmpty() ? TEXT("null") : *Quote(Ticket));

	SendInvocation(TEXT("JoinBus"), Arguments);
}

void UPraxBusSubsystem::Publish(const FString& BusKey, const FString& EventName,
								const FString& PayloadJson)
{
	std::string Key;
	std::string Error;
	if (!Prax::BusWire::CheckBusKey(ToStd(BusKey), Key, Error))
	{
		UE_LOG(LogPraxsuite, Error, TEXT("Event Bus: %s"), *ToFString(Error));
		return;
	}

	const FString Arguments = FString::Printf(TEXT("[%s,%s,%s]"),
		*Quote(ToFString(Key)),
		*Quote(EventName),
		PayloadJson.IsEmpty() ? TEXT("{}") : *PayloadJson);

	SendInvocation(TEXT("Publish"), Arguments);
}

void UPraxBusSubsystem::Leave(const FString& BusKey)
{
	const FString Normalized = ToFString(Prax::BusWire::NormalizeBusKey(ToStd(BusKey)));
	Wanted.Remove(Normalized);
	Joined.Remove(Normalized);

	if (State == EPraxBusState::Connected)
	{
		SendInvocation(TEXT("LeaveBus"), FString::Printf(TEXT("[%s]"), *Quote(Normalized)));
	}
}

void UPraxBusSubsystem::SendInvocation(const FString& Target, const FString& ArgumentsJson)
{
	if (!Socket.IsValid() || !Socket->IsConnected())
	{
		UE_LOG(LogPraxsuite, Warning, TEXT("Event Bus: %s dropped, not connected."), *Target);
		return;
	}

	const FString InvocationId = FString::FromInt(++NextInvocation);
	const std::string Frame = Prax::BusWire::Frame(Prax::BusWire::BuildInvocation(
		ToStd(InvocationId), ToStd(Target), ToStd(ArgumentsJson)));

	Socket->Send(ToFString(Frame));
}

void UPraxBusSubsystem::HandleFrame(const FString& Frame)
{
	// The handshake answer is the one frame with no type: {} on success, { error } on failure.
	if (!bHandshakeDone)
	{
		bHandshakeDone = true;

		Prax::FJsonValue Handshake;
		std::string ParseError;
		if (Prax::FJsonValue::Parse(ToStd(Frame), Handshake, ParseError)
			&& Handshake.Field("error").IsString())
		{
			UE_LOG(LogPraxsuite, Error, TEXT("The hub rejected the handshake: %s"),
				*ToFString(Handshake.Field("error").AsString()));
			Disconnect();
			return;
		}

		ReconnectDelaySeconds = 0.0f;
		SetState(EPraxBusState::Connected);
		RejoinAll();
		return;
	}

	Prax::FJsonValue Message;
	std::string ParseError;
	if (!Prax::FJsonValue::Parse(ToStd(Frame), Message, ParseError))
	{
		UE_LOG(LogPraxsuite, Warning, TEXT("Discarded an Event Bus frame that is not JSON."));
		return;
	}

	const int32 Type = static_cast<int32>(Message.Field("type").AsInt());

	if (Type == Prax::BusWire::MessagePing)
	{
		return;   // keepalive; never surfaced
	}

	if (Type == Prax::BusWire::MessageInvocation)
	{
		HandleServerEvent(Message);
		return;
	}

	if (Type == Prax::BusWire::MessageClose)
	{
		UE_LOG(LogPraxsuite, Warning, TEXT("The hub closed the connection: %s"),
			*ToFString(Message.Field("error").IsString()
				? Message.Field("error").AsString()
				: std::string("no reason given")));
		return;
	}

	if (Type != Prax::BusWire::MessageCompletion)
	{
		return;
	}

	const Prax::BusWire::FBusResult Result = Prax::BusWire::ParseBusResult(ToStd(Frame));

	if (!Result.bOk)
	{
		// A rejection arrives inside a SUCCESSFUL completion, so this is the only place it can be
		// seen at all. Logged rather than raised: dropping an ephemeral frame is ordinary, and a
		// game loop that treats a rate limit as a failure is worse than one that skips a frame.
		UE_LOG(LogPraxsuite, Warning, TEXT("Event Bus refused a call: %s"),
			*ToFString(Prax::BusWire::DescribeError(Result.Error)));
		return;
	}

	// A successful join replays every peer's retained state, which is what stops a player who
	// arrives late staring at an empty world until somebody moves.
	for (const Prax::BusWire::FPeerState& Peer : Result.Peers)
	{
		OnBusEvent.Broadcast(FString(), ToFString(Peer.Event), ToFString(Peer.PayloadJson),
			ToFString(Peer.UserId));
	}
}

void UPraxBusSubsystem::HandleServerEvent(const Prax::FJsonValue& Message)
{
	const Prax::FJsonValue& Arguments = Message.Field("arguments");
	if (!Arguments.IsArray() || Arguments.AsArray().empty())
	{
		return;
	}

	const Prax::FJsonValue& First = Arguments.AsArray()[0];
	const FString Target = ToFString(Message.Field("target").AsString());
	const FString Bus = ToFString(First.Field("bus").IsString()
		? First.Field("bus").AsString()
		: std::string());

	if (Target == TEXT("bus-event"))
	{
		// Every outbound message names its bus. A gateway older than 2026-09-07 does not, and on
		// one of those a client holding two buses cannot tell their traffic apart at all - SignalR
		// reports which invocation arrived, never which group it came from.
		if (Bus.IsEmpty() && Joined.Num() > 1 && !bWarnedAboutRouting)
		{
			bWarnedAboutRouting = true;
			UE_LOG(LogPraxsuite, Warning,
				TEXT("This gateway sends bus messages without naming their bus, so events cannot "
					 "be routed to the bus they came from. Update the gateway, or hold one bus per "
					 "connection until you can."));
		}

		// The payload is re-serialised rather than handed over as a parsed tree: it is another
		// PLAYER's JSON, the bus parsed none of it, and the game must decide what it means.
		OnBusEvent.Broadcast(
			Bus,
			ToFString(First.Field("event").AsString()),
			ToFString(First.Field("payload").ToString()),
			ToFString(First.Field("fromUserId").AsString()));
		return;
	}

	if (Target == TEXT("peer-joined"))
	{
		OnPeerJoined.Broadcast(Bus, ToFString(First.Field("userId").AsString()));
		return;
	}

	if (Target == TEXT("peer-left"))
	{
		OnPeerLeft.Broadcast(Bus, ToFString(First.Field("userId").AsString()));
		return;
	}

	if (Target == TEXT("bus-evicted"))
	{
		// The topic was disabled or re-scoped while this socket was open. Membership is dropped
		// and NOT re-joined: that would be arguing with a decision the server has just made.
		Wanted.Remove(Bus);
		Joined.Remove(Bus);
		OnEvicted.Broadcast(Bus, FString());
	}
}
void UPraxBusSubsystem::RejoinAll()
{
	// Not optional bookkeeping. SignalR group membership does not survive a reconnect, so a client
	// that reconnects and stops there is connected and in no groups - receiving nothing, reporting
	// no error, and looking for all the world like a broken server.
	//
	// Re-joining calls JoinBus again, which re-runs the topic's access rule. The SDK never replays
	// a membership list for the server to take on faith.
	for (const FString& Key : Wanted)
	{
		if (Joined.Contains(Key))
		{
			continue;
		}
		SendInvocation(TEXT("JoinBus"), FString::Printf(TEXT("[%s,null]"), *Quote(Key)));
		Joined.Add(Key);
	}
}

void UPraxBusSubsystem::ScheduleReconnect()
{
	Socket.Reset();
	bHandshakeDone = false;

	if (bClosedByUs)
	{
		SetState(EPraxBusState::Disconnected);
		return;
	}

	SetState(EPraxBusState::Reconnecting);
	ReconnectDelaySeconds = ReconnectDelaySeconds <= 0.0f
		? 1.0f
		: FMath::Min(ReconnectDelaySeconds * 2.0f, 30.0f);

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(ReconnectTimer,
			FTimerDelegate::CreateUObject(this, &UPraxBusSubsystem::Connect),
			ReconnectDelaySeconds, false);
	}
}

void UPraxBusSubsystem::SetState(EPraxBusState Next)
{
	if (State == Next)
	{
		return;
	}
	State = Next;
	OnStateChanged.Broadcast(Next);
}
