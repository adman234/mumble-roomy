// Roomy: removes the voices of people sitting in the same room from the local
// microphone signal. This file connects the suppressor and the network link to
// Mumble's plugin interface.

#include "MumblePlugin.h"

#include "config.h"
#include "net.h"
#include "suppressor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifndef ROOMY_VERSION_MAJOR
#	define ROOMY_VERSION_MAJOR 0
#	define ROOMY_VERSION_MINOR 0
#	define ROOMY_VERSION_PATCH 0
#endif

namespace {

using namespace roomy;

constexpr const char *kHelloId       = "roomy.hello";
constexpr std::size_t kMaxChannels   = 2;
constexpr std::size_t kMaxHelloBytes = 512;

MumbleAPI g_api;
mumble_plugin_id_t g_id = 0;

Config g_config;
Net g_net;
std::unique_ptr< Suppressor > g_suppressors[kMaxChannels];

// Only touched by Mumble's main thread
bool g_synchronized             = false;
mumble_connection_t g_connection = -1;
mumble_userid_t g_localUser      = 0;

MumbleStringWrapper staticString(const char *text) {
	MumbleStringWrapper wrapper;
	wrapper.data           = text;
	wrapper.size           = std::strlen(text);
	wrapper.needsReleasing = false;
	return wrapper;
}

void log(const std::string &message) {
	g_api.log(g_id, message.c_str());
}

std::string userName(mumble_connection_t connection, mumble_userid_t user) {
	const char *name = nullptr;
	if (g_api.getUserName(g_id, connection, user, &name) != MUMBLE_STATUS_OK || !name) {
		return "user " + std::to_string(user);
	}
	std::string result(name);
	g_api.freeMemory(g_id, name);
	return result;
}

/// Tells the given users how to reach this plugin instance directly
void sendHello(mumble_connection_t connection, const mumble_userid_t *users, std::size_t userCount, bool isReply) {
	if (!g_net.running() || userCount == 0) {
		return;
	}
	const std::vector< std::string > addresses = g_net.localAddresses();
	if (addresses.empty()) {
		return;
	}

	std::ostringstream message;
	message << "roomy1 " << (isReply ? 1 : 0) << ' ' << std::hex << g_net.nonce() << std::dec << ' ' << g_net.port();
	for (const std::string &address : addresses) {
		message << ' ' << address;
	}
	const std::string text = message.str();

	g_api.sendData(g_id, connection, users, userCount, reinterpret_cast< const uint8_t * >(text.data()), text.size(),
				   kHelloId);
}

/// Introduces this plugin instance to everybody else in the local user's channel
void announce(mumble_connection_t connection) {
	mumble_channelid_t channel = -1;
	if (g_api.getChannelOfUser(g_id, connection, g_localUser, &channel) != MUMBLE_STATUS_OK) {
		return;
	}

	mumble_userid_t *users = nullptr;
	std::size_t userCount  = 0;
	if (g_api.getUsersInChannel(g_id, connection, channel, &users, &userCount) != MUMBLE_STATUS_OK || !users) {
		return;
	}
	std::vector< mumble_userid_t > others;
	for (std::size_t i = 0; i < userCount; ++i) {
		if (users[i] != g_localUser) {
			others.push_back(users[i]);
		}
	}
	g_api.freeMemory(g_id, users);

	log("Roomy: looking for other Roomy users among the " + std::to_string(others.size())
		+ " other people in your channel.");
	sendHello(connection, others.data(), others.size(), false);
}

void handleHello(mumble_connection_t connection, mumble_userid_t sender, const std::string &text) {
	std::istringstream message(text);
	std::string tag;
	int isReply         = 0;
	std::uint64_t nonce = 0;
	unsigned int port   = 0;
	message >> tag >> isReply >> std::hex >> nonce >> std::dec >> port;
	if (!message || tag != "roomy1" || port == 0 || port > 65535 || nonce == 0) {
		return;
	}

	// Audio is only ever exchanged within the local network
	std::vector< std::string > addresses;
	std::string address;
	while (message >> address) {
		if (isPrivateAddress(address)) {
			addresses.push_back(address);
		}
	}
	if (addresses.empty()) {
		return;
	}

	log("Roomy: " + userName(connection, sender) + " is running Roomy, trying to reach them at " + addresses.front()
		+ ":" + std::to_string(port) + ".");
	g_net.addPeer(sender, nonce, addresses, static_cast< std::uint16_t >(port));

	if (!isReply) {
		sendHello(connection, &sender, 1, true);
	}
}

/// Reports what the network thread has found out in the meantime. Must be called from the main thread.
void reportLinkChanges() {
	if (!g_synchronized) {
		return;
	}
	for (const LinkEvent &event : g_net.takeEvents()) {
		const std::string name = userName(g_connection, event.userId);
		if (event.unreachable) {
			log("Roomy: " + name
				+ " is running Roomy but did not answer over the local network. Are you on the same network, and is "
				  "a firewall blocking Mumble?");
		} else if (event.linked) {
			log("Roomy: linked with " + name + ". Their voice is now removed from your microphone.");
		} else {
			log("Roomy: lost the link with " + name + ".");
		}
	}
}

std::int16_t toSample(float value) {
	return static_cast< std::int16_t >(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
}

} // namespace


mumble_error_t mumble_init(mumble_plugin_id_t id) {
	g_id     = id;
	g_config = loadConfig(configPath());

	const SuppressorParams params = g_config.suppressorParams();
	for (auto &suppressor : g_suppressors) {
		suppressor = std::make_unique< Suppressor >(params);
	}

	if (!g_config.enabled) {
		log("Roomy: disabled in roomy.ini.");
		return MUMBLE_STATUS_OK;
	}

	if (!g_net.start(static_cast< std::uint16_t >(g_config.port))) {
		log("Roomy: could not open a network port, so nothing will be removed from your microphone.");
		return MUMBLE_STATUS_OK;
	}

	std::string addressList;
	for (const std::string &address : g_net.localAddresses()) {
		addressList += (addressList.empty() ? "" : ", ") + address;
	}
	log("Roomy " + std::to_string(ROOMY_VERSION_MAJOR) + "." + std::to_string(ROOMY_VERSION_MINOR) + "."
		+ std::to_string(ROOMY_VERSION_PATCH) + " is active, listening on UDP port " + std::to_string(g_net.port())
		+ ". Local addresses: " + (addressList.empty() ? "none found, other users can't reach you" : addressList)
		+ ".");

	// The plugin may have been loaded while already connected to a server
	mumble_connection_t connection = -1;
	bool synchronized              = false;
	if (g_api.getActiveServerConnection(g_id, &connection) == MUMBLE_STATUS_OK
		&& g_api.isConnectionSynchronized(g_id, connection, &synchronized) == MUMBLE_STATUS_OK && synchronized) {
		mumble_onServerSynchronized(connection);
	}

	return MUMBLE_STATUS_OK;
}

void mumble_shutdown() {
	g_net.stop();
	g_synchronized = false;
}

struct MumbleStringWrapper mumble_getName() {
	return staticString("Roomy");
}

mumble_version_t mumble_getAPIVersion() {
	return MUMBLE_PLUGIN_API_VERSION;
}

void mumble_registerAPIFunctions(void *apiStruct) {
	g_api = MUMBLE_API_CAST(apiStruct);
}

void mumble_releaseResource(const void *) {
	// All strings handed to Mumble are static
}

mumble_version_t mumble_getVersion() {
	return { ROOMY_VERSION_MAJOR, ROOMY_VERSION_MINOR, ROOMY_VERSION_PATCH };
}

struct MumbleStringWrapper mumble_getAuthor() {
	return staticString("adman234");
}

struct MumbleStringWrapper mumble_getDescription() {
	return staticString("Removes the voices of people sitting in the same room from your microphone. "
						"Everybody in the room has to run this plugin and be on the same local network.");
}

uint32_t mumble_getFeatures() {
	return MUMBLE_FEATURE_AUDIO;
}

uint32_t mumble_deactivateFeatures(uint32_t features) {
	// Audio processing is all this plugin does, it can't be switched off separately
	return features;
}

void mumble_onServerSynchronized(mumble_connection_t connection) {
	if (g_api.getLocalUserID(g_id, connection, &g_localUser) != MUMBLE_STATUS_OK) {
		return;
	}
	g_connection   = connection;
	g_synchronized = true;
	announce(connection);
}

void mumble_onServerDisconnected(mumble_connection_t connection) {
	if (connection != g_connection) {
		return;
	}
	g_synchronized = false;
	g_connection   = -1;
	g_net.clearPeers();
}

void mumble_onChannelEntered(mumble_connection_t connection, mumble_userid_t userID, mumble_channelid_t,
							 mumble_channelid_t newChannelID) {
	if (!g_synchronized || connection != g_connection) {
		return;
	}

	if (userID == g_localUser) {
		announce(connection);
	} else {
		mumble_channelid_t ownChannel = -1;
		if (g_api.getChannelOfUser(g_id, connection, g_localUser, &ownChannel) == MUMBLE_STATUS_OK
			&& ownChannel == newChannelID) {
			sendHello(connection, &userID, 1, false);
		}
	}

	reportLinkChanges();
}

void mumble_onUserRemoved(mumble_connection_t connection, mumble_userid_t userID) {
	if (connection == g_connection) {
		g_net.removePeer(userID);
	}
}

void mumble_onUserTalkingStateChanged(mumble_connection_t, mumble_userid_t, mumble_talking_state_t) {
	// Nothing to do with the talking state itself. This merely is an event that fires often enough on the main
	// thread to report link changes in a timely manner.
	reportLinkChanges();
}

bool mumble_onReceiveData(mumble_connection_t connection, mumble_userid_t sender, const uint8_t *data,
						  size_t dataLength, const char *dataID) {
	if (!dataID || std::strcmp(dataID, kHelloId) != 0) {
		return false;
	}

	if (g_synchronized && connection == g_connection && sender != g_localUser && data && dataLength <= kMaxHelloBytes) {
		handleHello(connection, sender, std::string(reinterpret_cast< const char * >(data), dataLength));
		reportLinkChanges();
	}

	return true;
}

bool mumble_onAudioInput(short *inputPCM, uint32_t sampleCount, uint16_t channelCount, uint32_t sampleRate, bool) {
	if (!g_net.running() || sampleRate != static_cast< uint32_t >(kSampleRate)
		|| sampleCount != static_cast< uint32_t >(kFrame) || channelCount == 0 || channelCount > kMaxChannels) {
		return false;
	}

	// The others get the unprocessed signal: it is what their microphone picks up from this user
	std::int16_t mono[kFrame];
	for (std::size_t i = 0; i < kFrame; ++i) {
		int sum = 0;
		for (std::size_t c = 0; c < channelCount; ++c) {
			sum += inputPCM[i * channelCount + c];
		}
		mono[i] = static_cast< std::int16_t >(sum / channelCount);
	}
	g_net.sendAudio(mono);

	LinkedRef linked[kMaxPeers];
	const std::size_t linkedCount = g_net.linked(linked, kMaxPeers);
	if (linkedCount == 0) {
		return false;
	}

	RefInput refs[kMaxPeers];
	for (std::size_t i = 0; i < linkedCount; ++i) {
		refs[i].id    = linked[i].userId;
		refs[i].track = linked[i].track.get();
	}

	const double now = nowMs();
	float frame[kFrame];
	for (std::size_t c = 0; c < channelCount; ++c) {
		for (std::size_t i = 0; i < kFrame; ++i) {
			frame[i] = static_cast< float >(inputPCM[i * channelCount + c]) / 32768.0f;
		}
		g_suppressors[c]->process(frame, now, refs, linkedCount);
		for (std::size_t i = 0; i < kFrame; ++i) {
			inputPCM[i * channelCount + c] = toSample(frame[i]);
		}
	}

	return true;
}
