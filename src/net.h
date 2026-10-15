// Direct exchange of microphone audio between plugin instances on the same
// local network.
#pragma once

#include "suppressor.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
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
	/// Set (with linked false) when a peer has been known for a while without ever answering
	bool unreachable = false;
};

struct LinkedRef {
	std::uint32_t userId = 0;
	std::shared_ptr< RefTrack > track;
};

struct NetStats {
	std::uint64_t probesSent       = 0;
	/// Packets that were meant for this instance and came from a known peer
	std::uint64_t packetsAccepted = 0;
	/// Packets that arrived but were not usable (wrong format, wrong recipient or unknown sender)
	std::uint64_t packetsRejected = 0;
};

class Net {
public:
	using EventHandler = std::function< void(const LinkEvent &) >;

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

	/// Starts trying to reach the given peer on all of the given addresses.
	/// @returns Whether the peer was not known before (or has been restarted)
	bool addPeer(std::uint32_t userId, std::uint64_t nonce, const std::vector< std::string > &addresses,
				 std::uint16_t port);
	void removePeer(std::uint32_t userId);
	void clearPeers();

	/// Sends one frame (kFrame samples) of mono audio to all linked peers. Safe to call from the audio thread.
	void sendAudio(const std::int16_t *pcm);

	/// Fetches the peers a working link exists with. Safe to call from the audio thread.
	/// @returns The amount of entries written
	std::size_t linked(LinkedRef *out, std::size_t max);

	/// Registers a function that is called from the network thread whenever a link comes up, goes away or turns out
	/// to be unreachable. Without a handler the events are kept for takeEvents().
	void setEventHandler(EventHandler handler);

	/// Links that came up or went away since the last call
	std::vector< LinkEvent > takeEvents();

	NetStats stats() const;

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
		/// When to report next that this peer can't be reached
		double nextReportMs = 0.0;
		Endpoint endpoint;
		double lastHeardMs = 0.0;
		std::shared_ptr< RefTrack > track;
	};

	void run();
	void housekeeping(double now);
	void dispatchEvents();
	void handlePacket(const std::uint8_t *data, std::size_t size, const Endpoint &from, double now);
	void sendPacket(std::uint8_t type, std::uint64_t peerNonce, const Endpoint &to, std::uint32_t sequence,
					const std::int16_t *pcm);

	std::atomic< bool > m_running{ false };
	std::intptr_t m_socket = -1;
	std::uint16_t m_port   = 0;
	std::uint64_t m_nonce  = 0;
	std::thread m_thread;

	mutable std::mutex m_mutex;
	std::vector< Peer > m_peers;
	std::vector< LinkEvent > m_events;
	EventHandler m_handler;
	std::atomic< std::uint32_t > m_sequence{ 0 };
	std::atomic< std::uint64_t > m_probesSent{ 0 };
	std::atomic< std::uint64_t > m_accepted{ 0 };
	std::atomic< std::uint64_t > m_rejected{ 0 };
};

} // namespace roomy
