#include "Core/PraxBusWire.h"

#include "Core/PraxJson.h"
#include "Core/PraxRoutes.h"

#include <cctype>

namespace Prax
{
	namespace BusWire
	{
		namespace
		{
			std::string Trim(const std::string& Text)
			{
				const auto First = Text.find_first_not_of(" \t\r\n");
				if (First == std::string::npos)
				{
					return std::string();
				}
				const auto Last = Text.find_last_not_of(" \t\r\n");
				return Text.substr(First, Last - First + 1);
			}

			std::string Lower(const std::string& Text)
			{
				std::string Result = Text;
				for (char& Character : Result)
				{
					// The cast dance is not decoration: std::tolower takes an int that must be
					// representable as unsigned char, and passing a negative char from a UTF-8
					// byte is undefined behaviour that only bites on some platforms.
					Character = static_cast<char>(
						std::tolower(static_cast<unsigned char>(Character)));
				}
				return Result;
			}

			std::string PercentEncode(const std::string& Text)
			{
				static const char* Hex = "0123456789ABCDEF";
				std::string Out;
				Out.reserve(Text.size());

				for (const char Character : Text)
				{
					const unsigned char Byte = static_cast<unsigned char>(Character);
					const bool bUnreserved = (Byte >= 'A' && Byte <= 'Z')
						|| (Byte >= 'a' && Byte <= 'z')
						|| (Byte >= '0' && Byte <= '9')
						|| Byte == '-' || Byte == '.' || Byte == '_' || Byte == '~';

					if (bUnreserved)
					{
						Out.push_back(Character);
					}
					else
					{
						Out.push_back('%');
						Out.push_back(Hex[Byte >> 4]);
						Out.push_back(Hex[Byte & 0x0F]);
					}
				}
				return Out;
			}

			std::string TrimTrailingSlashes(const std::string& Host)
			{
				std::string Result = Host;
				while (!Result.empty() && Result.back() == '/')
				{
					Result.pop_back();
				}
				return Result;
			}
		}

		FFrames SplitFrames(const std::string& Buffer)
		{
			FFrames Result;
			if (Buffer.empty())
			{
				return Result;
			}

			std::string::size_type Start = 0;
			for (;;)
			{
				const auto Separator = Buffer.find(RecordSeparator, Start);
				if (Separator == std::string::npos)
				{
					break;
				}
				if (Separator > Start)
				{
					Result.Frames.push_back(Buffer.substr(Start, Separator - Start));
				}
				Start = Separator + 1;
			}

			Result.Remainder = Buffer.substr(Start);
			return Result;
		}

		std::string Frame(const std::string& Payload)
		{
			return Payload + RecordSeparator;
		}

		std::string NormalizeBusKey(const std::string& BusKey)
		{
			const std::string Key = Trim(BusKey);
			if (Key.empty())
			{
				return std::string();
			}

			const auto Separator = Key.find(':');
			if (Separator == std::string::npos || Separator == 0)
			{
				return Lower(Key);
			}

			return Lower(Key.substr(0, Separator)) + Key.substr(Separator);
		}

		bool CheckBusKey(const std::string& BusKey, std::string& OutKey, std::string& OutError)
		{
			const std::string Key = NormalizeBusKey(BusKey);

			if (Key.empty())
			{
				OutError = "A bus key is required. It looks like \"topic:instance\", "
						   "e.g. \"office:hq\".";
				return false;
			}

			// The group name is built by concatenation, so a key carrying the separator could
			// climb out of its own segment and name another workspace's group.
			if (Key.find("ws:") != std::string::npos)
			{
				OutError = "A bus key may not contain \"ws:\" (got \"" + BusKey
					+ "\"). The server refuses it.";
				return false;
			}

			if (Key.size() > 200)
			{
				OutError = "Bus key is too long (" + std::to_string(Key.size()) + " characters).";
				return false;
			}

			OutKey = Key;
			OutError.clear();
			return true;
		}

		std::string BuildInvocation(const std::string& InvocationId, const std::string& Target,
									const std::string& ArgumentsJson)
		{
			// Built by hand rather than through FJsonValue so the arguments array can be spliced
			// in exactly as the caller encoded it. Round-tripping a payload through the codec
			// would be lossless but pointless work on a path that runs per movement decision.
			std::string Out;
			Out.reserve(64 + ArgumentsJson.size());
			Out += "{\"type\":";
			Out += std::to_string(MessageInvocation);
			Out += ",\"invocationId\":";
			Out += FJsonValue(InvocationId).ToString();
			Out += ",\"target\":";
			Out += FJsonValue(Target).ToString();
			Out += ",\"arguments\":";
			Out += ArgumentsJson.empty() ? "[]" : ArgumentsJson;
			Out += "}";
			return Out;
		}

		FBusResult ParseBusResult(const std::string& FrameJson)
		{
			FBusResult Result;

			FJsonValue Message;
			std::string ParseError;
			if (!FJsonValue::Parse(FrameJson, Message, ParseError))
			{
				Result.bOk = false;
				Result.Error = "malformed_frame";
				Result.bIsTransportError = true;
				return Result;
			}

			const FJsonValue& TransportError = Message.Field("error");
			if (TransportError.IsString() && !TransportError.AsString().empty())
			{
				Result.bOk = false;
				Result.Error = TransportError.AsString();
				Result.bIsTransportError = true;
				return Result;
			}

			const FJsonValue& Body = Message.Field("result");
			if (!Body.IsObject())
			{
				return Result;   // void, e.g. LeaveBus
			}

			const FJsonValue& Ok = Body.Field("ok");
			Result.bOk = !(Ok.IsBool() && !Ok.AsBool());

			const FJsonValue& Code = Body.Field("error");
			if (Code.IsString())
			{
				Result.Error = Code.AsString();
			}

			const FJsonValue& Recipients = Body.Field("recipients");
			if (Recipients.IsNumber())
			{
				Result.Recipients = static_cast<int>(Recipients.AsInt());
			}

			const FJsonValue& Peers = Body.Field("peers");
			if (Peers.IsArray())
			{
				for (const FJsonValue& Entry : Peers.AsArray())
				{
					if (!Entry.IsObject())
					{
						continue;
					}

					FPeerState Peer;
					const FJsonValue& UserId = Entry.Field("userId");
					const FJsonValue& Event = Entry.Field("event");
					Peer.UserId = UserId.IsString() ? UserId.AsString() : std::string();
					Peer.Event = Event.IsString() ? Event.AsString() : std::string();
					Peer.PayloadJson = Entry.Field("payload").ToString();
					Result.Peers.push_back(Peer);
				}
			}

			return Result;
		}

		std::string NegotiateUrl(const std::string& Host)
		{
			return TrimTrailingSlashes(Host) + BusPath + "/negotiate?negotiateVersion=1";
		}

		std::string SocketUrl(const std::string& Host, const std::string& AccessToken)
		{
			const std::string Base = TrimTrailingSlashes(Host);

			std::string Scheme = Base;
			if (Base.rfind("https://", 0) == 0)
			{
				Scheme = "wss://" + Base.substr(8);
			}
			else if (Base.rfind("http://", 0) == 0)
			{
				Scheme = "ws://" + Base.substr(7);
			}

			return Scheme + BusPath + "?access_token=" + PercentEncode(AccessToken);
		}

		std::string DescribeError(const std::string& Code)
		{
			if (Code.empty())
			{
				return "The bus refused the call.";
			}
			if (Code == "unknown_topic")
			{
				return "That topic is not declared in this workspace. Buses are never "
					   "auto-created - declare the topic under API Gateway / Event Bus first.";
			}
			if (Code == "denied")
			{
				return "The topic refused this user. Check its access mode: Workspace, Roles "
					   "(read straight from the JWT), or Grants (which needs a grant on this exact "
					   "bus instance).";
			}
			if (Code == "invalid_ticket")
			{
				return "The ticket was missing, expired, or minted for a different user, "
					   "workspace or bus.";
			}
			if (Code == "not_a_member")
			{
				return "Publish to a bus this connection has not joined. Join it first - "
					   "membership is the authorization check on the publish path.";
			}
			if (Code == "invalid_bus_key")
			{
				return "The key is malformed, or it named another user's \"user:\" bus. Only "
					   "\"user:self\" is addressable.";
			}
			if (Code == "invalid_event_name")
			{
				return "The event name was empty or too long.";
			}
			if (Code == "payload_too_large")
			{
				return "The payload is over this topic's byte limit.";
			}
			if (Code == "bus_full_or_too_many_buses")
			{
				return "The bus is at its peer limit, or this connection already holds as many "
					   "buses as it may.";
			}
			if (Code == "rate_limited")
			{
				return "Too many publishes. The limit is priced by RECIPIENTS, so a large bus "
					   "exhausts it faster than a small one.";
			}
			return Code;
		}
	}
}
