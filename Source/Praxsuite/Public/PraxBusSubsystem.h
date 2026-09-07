// The Prax Event Bus, as a Blueprint-facing subsystem.
//
// Ephemeral realtime between connected players: positions, cursors, "is typing", a lobby. State
// that is CHANGING, where losing a message is fine because a newer one is 100ms behind it.
//
// NOTHING IS PERSISTED. No history, no retry, no delivery to a player who was not connected. The
// test is one question: if this is lost, does it matter? Yes - a purchase, a score, an inventory
// grant - means a table via UPraxsuiteSubsystem, and a server-authoritative write at that. No,
// because a newer one is coming, means the bus.
//
// PAYLOADS ARE HOSTILE. The bus relays opaque JSON between PLAYERS and parses none of it, so every
// server-side check is bypassed. A position is a hint, never an authority.
//
// The wire format itself lives in Core/PraxBusWire.h, which is engine-free and covered by the
// offline suite. This file is only the socket and the Blueprint surface.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "PraxBusSubsystem.generated.h"

class IWebSocket;

// Forward-declared rather than included: the engine-facing header stays free of the
// portable core's headers, which is what keeps the core compilable without Unreal.
namespace Prax { class FJsonValue; }

/** Where the connection is. Reconnecting is normal and resolves itself. */
UENUM(BlueprintType)
enum class EPraxBusState : uint8
{
	Disconnected,
	Connecting,
	Connected,
	Reconnecting,
};

/** One peer's last known state within a bus, as delivered on join. */
USTRUCT(BlueprintType)
struct PRAXSUITE_API FPraxBusPeer
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Praxsuite|Bus")
	FString UserId;

	UPROPERTY(BlueprintReadOnly, Category = "Praxsuite|Bus")
	FString Event;

	/** Raw JSON, exactly as another player sent it. Parse it yourself, and do not trust it. */
	UPROPERTY(BlueprintReadOnly, Category = "Praxsuite|Bus")
	FString PayloadJson;
};

/**
 * An event relayed from another peer.
 *
 * FromUserId is stamped by the server from the validated token, never read from the payload, so a
 * peer cannot claim to be somebody else. PayloadJson is not.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FPraxBusEventSignature,
	const FString&, Bus, const FString&, EventName, const FString&, PayloadJson,
	const FString&, FromUserId);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPraxBusPeerSignature,
	const FString&, Bus, const FString&, UserId);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPraxBusStateSignature, EPraxBusState, State);

/**
 * Join, publish, leave.
 *
 * Requires a signed-in end user: the hub authenticates with the session token, not with the
 * workspace's publishable key. Sign in through UPraxsuiteSubsystem first.
 */
UCLASS()
class PRAXSUITE_API UPraxBusSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Fired for every event on every bus this client has joined. Filter on Bus and EventName. */
	UPROPERTY(BlueprintAssignable, Category = "Praxsuite|Bus")
	FPraxBusEventSignature OnBusEvent;

	/** Fires only when the topic has presence enabled. */
	UPROPERTY(BlueprintAssignable, Category = "Praxsuite|Bus")
	FPraxBusPeerSignature OnPeerJoined;

	UPROPERTY(BlueprintAssignable, Category = "Praxsuite|Bus")
	FPraxBusPeerSignature OnPeerLeft;

	/**
	 * The server removed this client from a bus, because the topic was disabled or re-scoped while
	 * the socket was open. The SDK does not re-join: that would be arguing with a decision the
	 * server has just made.
	 */
	UPROPERTY(BlueprintAssignable, Category = "Praxsuite|Bus")
	FPraxBusPeerSignature OnEvicted;

	UPROPERTY(BlueprintAssignable, Category = "Praxsuite|Bus")
	FPraxBusStateSignature OnStateChanged;

	UFUNCTION(BlueprintPure, Category = "Praxsuite|Bus")
	EPraxBusState GetBusState() const { return State; }

	/**
	 * Opens the connection. Join calls this for you; call it at start-up to fail fast instead of
	 * on the first join.
	 */
	UFUNCTION(BlueprintCallable, Category = "Praxsuite|Bus")
	void Connect();

	/** Closes the connection and stops reconnecting. Bindings are kept. */
	UFUNCTION(BlueprintCallable, Category = "Praxsuite|Bus")
	void Disconnect();

	/**
	 * Joins a bus, named "topic:instance" - "office:hq", "channel:{guid}".
	 *
	 * The topic must already be declared in the portal under API Gateway / Event Bus. An undeclared
	 * topic is refused, which is what stops another project's client squatting in your namespace.
	 *
	 * The peers already present arrive through OnBusEvent as their retained state, so a player who
	 * joins late sees the world rather than an empty one until somebody moves.
	 *
	 * Ticket is only consulted for topics whose access mode is Ticket. Leave it empty: nothing in
	 * the platform mints a ticket yet.
	 */
	UFUNCTION(BlueprintCallable, Category = "Praxsuite|Bus")
	void Join(const FString& BusKey, const FString& Ticket);

	/**
	 * Sends an event to every OTHER peer in the bus.
	 *
	 * PayloadJson is sent verbatim. A refusal - a rate limit, an oversized payload - is logged and
	 * dropped rather than raised: losing an ephemeral frame is ordinary, and a game loop that
	 * treats it as a failure is worse than one that skips a frame.
	 *
	 * You will not receive your own event back. Apply your own change locally.
	 */
	UFUNCTION(BlueprintCallable, Category = "Praxsuite|Bus")
	void Publish(const FString& BusKey, const FString& EventName, const FString& PayloadJson);

	/** Leaves a bus. Idempotent, and it stops the reconnect logic re-joining. */
	UFUNCTION(BlueprintCallable, Category = "Praxsuite|Bus")
	void Leave(const FString& BusKey);

	/** The player's own bus, "user:self", resolved server-side so it cannot name anybody else. */
	UFUNCTION(BlueprintCallable, Category = "Praxsuite|Bus")
	void JoinSelf() { Join(TEXT("user:self"), FString()); }

private:
	void SetState(EPraxBusState Next);
	void HandleFrame(const FString& Frame);
	void HandleServerEvent(const Prax::FJsonValue& Message);
	void SendInvocation(const FString& Target, const FString& ArgumentsJson);
	void ScheduleReconnect();
	void RejoinAll();

	TSharedPtr<IWebSocket> Socket;
	FString Buffer;
	int32 NextInvocation = 0;

	/** Buses the game wants to be in, so a reconnect can re-join them. */
	TSet<FString> Wanted;
	TSet<FString> Joined;

	EPraxBusState State = EPraxBusState::Disconnected;
	bool bHandshakeDone = false;
	bool bClosedByUs = false;
	bool bWarnedAboutRouting = false;
	float ReconnectDelaySeconds = 0.0f;
	FTimerHandle ReconnectTimer;
};
