// Direct exchange of microphone audio between plugin instances on the same
// local network.
#pragma once

#include "suppressor.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace roomy {

/// Milliseconds on a monotonic clock
double nowMs();

/// Whether the given dotted IPv4 address belongs to a private or link-local network
bool isPrivateAddress(const std::string &address);

constexpr std::size_t kMaxPeers = 4;

struct LinkEvent {
	std::uint32_t userId;
	bool linked;
};

struct LinkedRef {
	std::uint32_t userId = 0;
	std::shared_ptr< RefTrack > track;
};

class Net {
public:
	Net();
	~Net();

	Net(const Net &) = delete;
	Net &operator=(const Net &) = delete;

	/// Opens the socket and starts the receiving thread. A port of 0 lets the system choose one.
	bool start(std::uint16_t port);
	void stop();

	bool running() const { return m_running.load(); }
	std::uint16_t port() const { return m_port; }
	/// Random value others have to present in order to be listened to
	std::uint64_t nonce() const { return m_nonce; }

	/// The private IPv4 addresses of this machine
	std::vector< std::string > localAddresses() const;

	/// Starts trying to reach the given peer on all of the given addresses
	void addPeer(std::uint32_t userId, std::uint64_t nonce, const std::vector< std::string > &addresses,
				 std::uint16_t port);
	void removePeer(std::uint32_t userId);
	void clearPeers();

	/// Sends one frame (kFrame samples) of mono audio to all linked peers. Safe to call from the audio thread.
	void sendAudio(const std::int16_t *pcm);

	/// Fetches the peers a working link exists with. Safe to call from the audio thread.
	/// @returns The amount of entries written
	std::size_t linked(LinkedRef *out, std::size_t max);

	/// Links that came up or went away since the last call
	std::vector< LinkEvent > takeEvents();

private:
	struct Endpoint {
		std::uint32_t address = 0; // network byte order
		std::uint16_t port    = 0; // host byte order
	};

	struct Peer {
		std::uint32_t userId = 0;
		std::uint64_t nonce  = 0;
		std::vector< Endpoint > candidates;
		bool linked = false;
		Endpoint endpoint;
		double lastHeardMs = 0.0;
		std::shared_ptr< RefTrack > track;
	};

	void run();
	void housekeeping(double now);
	void handlePacket(const std::uint8_t *data, std::size_t size, const Endpoint &from, double now);
	void sendPacket(std::uint8_t type, std::uint64_t peerNonce, const Endpoint &to, std::uint32_t sequence,
					const std::int16_t *pcm);

	std::atomic< bool > m_running{ false };
	std::intptr_t m_socket = -1;
	std::uint16_t m_port   = 0;
	std::uint64_t m_nonce  = 0;
	std::thread m_thread;

	std::mutex m_mutex;
	std::vector< Peer > m_peers;
	std::vector< LinkEvent > m_events;
	std::atomic< std::uint32_t > m_sequence{ 0 };
};

} // namespace roomy
