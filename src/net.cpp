#include "net.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

#ifdef _WIN32
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <winsock2.h>
#	include <ws2tcpip.h>
using socklen_type = int;
#else
#	include <arpa/inet.h>
#	include <netdb.h>
#	include <netinet/in.h>
#	include <sys/socket.h>
#	include <sys/time.h>
#	include <unistd.h>
using socklen_type = socklen_t;
#endif

namespace roomy {

namespace {

	constexpr std::uint8_t kMagic[4]     = { 'R', 'M', 'Y', '1' };
	constexpr std::uint8_t kTypeProbe    = 1;
	constexpr std::uint8_t kTypeAudio    = 2;
	constexpr std::size_t kHeaderSize    = 28;
	constexpr std::size_t kAudioSize     = kHeaderSize + kFrame * 2;
	constexpr double kProbeIntervalMs    = 500.0;
	constexpr double kLinkTimeoutMs      = 5000.0;
	constexpr double kUnreachableMs     = 10000.0;
	constexpr double kUnreachableRepeatMs = 30000.0;
	constexpr std::size_t kMaxCandidates = 8;

	void closeSocket(std::intptr_t socket) {
#ifdef _WIN32
		closesocket(static_cast< SOCKET >(socket));
#else
		close(static_cast< int >(socket));
#endif
	}

	void put64(std::uint8_t *out, std::uint64_t value) {
		for (int i = 0; i < 8; ++i) {
			out[i] = static_cast< std::uint8_t >(value >> (8 * i));
		}
	}

	std::uint64_t get64(const std::uint8_t *in) {
		std::uint64_t value = 0;
		for (int i = 0; i < 8; ++i) {
			value |= static_cast< std::uint64_t >(in[i]) << (8 * i);
		}
		return value;
	}

	void put32(std::uint8_t *out, std::uint32_t value) {
		for (int i = 0; i < 4; ++i) {
			out[i] = static_cast< std::uint8_t >(value >> (8 * i));
		}
	}

	std::uint32_t get32(const std::uint8_t *in) {
		std::uint32_t value = 0;
		for (int i = 0; i < 4; ++i) {
			value |= static_cast< std::uint32_t >(in[i]) << (8 * i);
		}
		return value;
	}

	/// @param address The address in host byte order
	bool isPrivate(std::uint32_t address) {
		return (address >> 24) == 10 || (address >> 20) == ((172u << 4) | 1u) || (address >> 16) == ((192u << 8) | 168u)
			   || (address >> 16) == ((169u << 8) | 254u);
	}

	void addUnique(std::vector< std::string > &list, const in_addr &address) {
		if (!isPrivate(ntohl(address.s_addr))) {
			return;
		}
		char text[INET_ADDRSTRLEN] = {};
		if (!inet_ntop(AF_INET, &address, text, sizeof(text))) {
			return;
		}
		if (std::find(list.begin(), list.end(), text) == list.end()) {
			list.emplace_back(text);
		}
	}

#ifdef _WIN32
	struct SocketLibrary {
		SocketLibrary() {
			WSADATA data;
			WSAStartup(MAKEWORD(2, 2), &data);
		}
		~SocketLibrary() { WSACleanup(); }
	};
#endif

	void ensureSocketLibrary() {
#ifdef _WIN32
		static SocketLibrary library;
#endif
	}

} // namespace

double nowMs() {
	using namespace std::chrono;
	return duration< double, std::milli >(steady_clock::now().time_since_epoch()).count();
}

bool isPrivateAddress(const std::string &address) {
	in_addr parsed{};
	if (inet_pton(AF_INET, address.c_str(), &parsed) != 1) {
		return false;
	}
	return isPrivate(ntohl(parsed.s_addr));
}

Net::Net() {
	std::random_device random;
	m_nonce = (static_cast< std::uint64_t >(random()) << 32) ^ static_cast< std::uint64_t >(random());
	if (m_nonce == 0) {
		m_nonce = 1;
	}
}

Net::~Net() {
	stop();
}

bool Net::start(std::uint16_t port) {
	if (m_running.load()) {
		return true;
	}
	ensureSocketLibrary();

	const auto handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
	if (handle == INVALID_SOCKET) {
		return false;
	}
	// Wake up regularly so that the thread notices a shutdown and can do its housekeeping
	DWORD timeout = 200;
	setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast< const char * >(&timeout), sizeof(timeout));
#else
	if (handle < 0) {
		return false;
	}
	timeval timeout{};
	timeout.tv_usec = 200 * 1000;
	setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
	m_socket = static_cast< std::intptr_t >(handle);

	sockaddr_in local{};
	local.sin_family      = AF_INET;
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	local.sin_port        = htons(port);
	if (bind(handle, reinterpret_cast< const sockaddr * >(&local), sizeof(local)) != 0) {
		closeSocket(m_socket);
		m_socket = -1;
		return false;
	}

	socklen_type length = sizeof(local);
	if (getsockname(handle, reinterpret_cast< sockaddr * >(&local), &length) != 0) {
		closeSocket(m_socket);
		m_socket = -1;
		return false;
	}
	m_port = ntohs(local.sin_port);

	m_running.store(true);
	m_thread = std::thread(&Net::run, this);
	return true;
}

void Net::stop() {
	if (!m_running.exchange(false)) {
		return;
	}
	if (m_thread.joinable()) {
		m_thread.join();
	}
	closeSocket(m_socket);
	m_socket = -1;
	clearPeers();
}

std::vector< std::string > Net::localAddresses() const {
	ensureSocketLibrary();
	std::vector< std::string > result;

	// Asking which local address would be used to reach a remote one yields the main address of this machine.
	// Connecting a UDP socket does not send anything.
	const auto probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
	const bool valid = probe != INVALID_SOCKET;
#else
	const bool valid = probe >= 0;
#endif
	if (valid) {
		sockaddr_in remote{};
		remote.sin_family = AF_INET;
		remote.sin_port   = htons(9);
		inet_pton(AF_INET, "192.0.2.1", &remote.sin_addr);
		if (connect(probe, reinterpret_cast< const sockaddr * >(&remote), sizeof(remote)) == 0) {
			sockaddr_in local{};
			socklen_type length = sizeof(local);
			if (getsockname(probe, reinterpret_cast< sockaddr * >(&local), &length) == 0) {
				addUnique(result, local.sin_addr);
			}
		}
		closeSocket(static_cast< std::intptr_t >(probe));
	}

	char host[256] = {};
	if (gethostname(host, sizeof(host) - 1) == 0) {
		addrinfo hints{};
		hints.ai_family   = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		addrinfo *list    = nullptr;
		if (getaddrinfo(host, nullptr, &hints, &list) == 0) {
			for (addrinfo *entry = list; entry; entry = entry->ai_next) {
				if (entry->ai_family == AF_INET && entry->ai_addr) {
					addUnique(result, reinterpret_cast< const sockaddr_in * >(entry->ai_addr)->sin_addr);
				}
			}
			freeaddrinfo(list);
		}
	}

	if (result.size() > kMaxCandidates) {
		result.resize(kMaxCandidates);
	}
	return result;
}

bool Net::addPeer(std::uint32_t userId, std::uint64_t nonce, const std::vector< std::string > &addresses,
				  std::uint16_t port) {
	std::vector< Endpoint > candidates;
	for (const std::string &address : addresses) {
		in_addr parsed{};
		if (candidates.size() < kMaxCandidates && inet_pton(AF_INET, address.c_str(), &parsed) == 1) {
			Endpoint endpoint;
			endpoint.address = parsed.s_addr;
			endpoint.port    = port;
			candidates.push_back(endpoint);
		}
	}
	if (candidates.empty() || port == 0 || nonce == 0) {
		return false;
	}

	std::lock_guard< std::mutex > lock(m_mutex);

	for (Peer &peer : m_peers) {
		if (peer.userId != userId) {
			continue;
		}
		bool restarted = false;
		if (peer.nonce != nonce) {
			// The other client was restarted
			if (peer.linked) {
				m_events.push_back({ userId, false });
			}
			peer.linked       = false;
			peer.nonce        = nonce;
			peer.track        = std::make_shared< RefTrack >();
			peer.nextReportMs = nowMs() + kUnreachableMs;
			restarted         = true;
		}
		peer.candidates = candidates;
		return restarted;
	}

	if (m_peers.size() >= kMaxPeers) {
		return false;
	}
	Peer peer;
	peer.userId     = userId;
	peer.nonce      = nonce;
	peer.candidates = candidates;
	peer.track      = std::make_shared< RefTrack >();
	peer.nextReportMs = nowMs() + kUnreachableMs;
	m_peers.push_back(std::move(peer));
	return true;
}

void Net::removePeer(std::uint32_t userId) {
	std::lock_guard< std::mutex > lock(m_mutex);
	m_peers.erase(std::remove_if(m_peers.begin(), m_peers.end(), [userId](const Peer &p) { return p.userId == userId; }),
				  m_peers.end());
}

void Net::clearPeers() {
	std::lock_guard< std::mutex > lock(m_mutex);
	m_peers.clear();
	m_events.clear();
}

void Net::sendAudio(const std::int16_t *pcm) {
	if (!m_running.load()) {
		return;
	}

	struct Target {
		std::uint64_t nonce;
		Endpoint endpoint;
	};
	Target targets[kMaxPeers];
	std::size_t count = 0;
	{
		std::lock_guard< std::mutex > lock(m_mutex);
		for (const Peer &peer : m_peers) {
			if (peer.linked && count < kMaxPeers) {
				targets[count++] = { peer.nonce, peer.endpoint };
			}
		}
	}

	const std::uint32_t sequence = ++m_sequence;
	for (std::size_t i = 0; i < count; ++i) {
		sendPacket(kTypeAudio, targets[i].nonce, targets[i].endpoint, sequence, pcm);
	}
}

std::size_t Net::linked(LinkedRef *out, std::size_t max) {
	std::size_t count = 0;
	std::lock_guard< std::mutex > lock(m_mutex);
	for (const Peer &peer : m_peers) {
		if (peer.linked && count < max) {
			out[count].userId = peer.userId;
			out[count].track  = peer.track;
			++count;
		}
	}
	return count;
}

void Net::setEventHandler(EventHandler handler) {
	std::lock_guard< std::mutex > lock(m_mutex);
	m_handler = std::move(handler);
}

NetStats Net::stats() const {
	NetStats result;
	result.probesSent      = m_probesSent.load();
	result.packetsAccepted = m_accepted.load();
	result.packetsRejected = m_rejected.load();
	return result;
}

void Net::dispatchEvents() {
	EventHandler handler;
	std::vector< LinkEvent > events;
	{
		std::lock_guard< std::mutex > lock(m_mutex);
		if (!m_handler || m_events.empty()) {
			return;
		}
		handler = m_handler;
		events.swap(m_events);
	}
	for (const LinkEvent &event : events) {
		handler(event);
	}
}

std::vector< LinkEvent > Net::takeEvents() {
	std::lock_guard< std::mutex > lock(m_mutex);
	std::vector< LinkEvent > events;
	events.swap(m_events);
	return events;
}

void Net::sendPacket(std::uint8_t type, std::uint64_t peerNonce, const Endpoint &to, std::uint32_t sequence,
					 const std::int16_t *pcm) {
	std::uint8_t packet[kAudioSize] = {};
	std::memcpy(packet, kMagic, sizeof(kMagic));
	packet[4] = type;
	put64(packet + 8, peerNonce);
	put64(packet + 16, m_nonce);
	put32(packet + 24, sequence);

	std::size_t size = kHeaderSize;
	if (pcm) {
		for (std::size_t i = 0; i < kFrame; ++i) {
			const std::uint16_t sample   = static_cast< std::uint16_t >(pcm[i]);
			packet[kHeaderSize + 2 * i]     = static_cast< std::uint8_t >(sample & 0xff);
			packet[kHeaderSize + 2 * i + 1] = static_cast< std::uint8_t >(sample >> 8);
		}
		size = kAudioSize;
	}

	sockaddr_in address{};
	address.sin_family      = AF_INET;
	address.sin_addr.s_addr = to.address;
	address.sin_port        = htons(to.port);
#ifdef _WIN32
	sendto(static_cast< SOCKET >(m_socket), reinterpret_cast< const char * >(packet), static_cast< int >(size), 0,
		   reinterpret_cast< const sockaddr * >(&address), sizeof(address));
#else
	sendto(static_cast< int >(m_socket), packet, size, 0, reinterpret_cast< const sockaddr * >(&address),
		   sizeof(address));
#endif
}

void Net::handlePacket(const std::uint8_t *data, std::size_t size, const Endpoint &from, double now) {
	if (size < kHeaderSize || std::memcmp(data, kMagic, sizeof(kMagic)) != 0 || get64(data + 8) != m_nonce) {
		++m_rejected;
		return;
	}
	const std::uint64_t sender = get64(data + 16);

	std::shared_ptr< RefTrack > track;
	{
		std::lock_guard< std::mutex > lock(m_mutex);
		for (Peer &peer : m_peers) {
			if (peer.nonce != sender) {
				continue;
			}
			if (!peer.linked) {
				peer.linked              = true;
				m_events.push_back({ peer.userId, true });
			}
			peer.endpoint    = from;
			peer.lastHeardMs = now;
			track            = peer.track;
			break;
		}
	}

	if (track) {
		++m_accepted;
	} else {
		++m_rejected;
	}

	if (track && data[4] == kTypeAudio && size == kAudioSize) {
		std::int16_t pcm[kFrame];
		for (std::size_t i = 0; i < kFrame; ++i) {
			pcm[i] = static_cast< std::int16_t >(static_cast< std::uint16_t >(
				data[kHeaderSize + 2 * i] | (static_cast< std::uint16_t >(data[kHeaderSize + 2 * i + 1]) << 8)));
		}
		track->push(pcm, get32(data + 24), now);
	}
}

void Net::housekeeping(double now) {
	struct Probe {
		std::uint64_t nonce;
		Endpoint endpoint;
	};
	std::vector< Probe > probes;
	{
		std::lock_guard< std::mutex > lock(m_mutex);
		for (Peer &peer : m_peers) {
			if (peer.linked && now - peer.lastHeardMs > kLinkTimeoutMs) {
				peer.linked = false;
				peer.nextReportMs = now + kUnreachableMs;
				m_events.push_back({ peer.userId, false });
			}
			if (peer.linked) {
				// Keeps the link alive while nobody is talking
				probes.push_back({ peer.nonce, peer.endpoint });
			} else {
				if (now >= peer.nextReportMs) {
					peer.nextReportMs = now + kUnreachableRepeatMs;
					m_events.push_back({ peer.userId, false, true });
				}
				for (const Endpoint &candidate : peer.candidates) {
					probes.push_back({ peer.nonce, candidate });
				}
			}
		}
	}
	for (const Probe &probe : probes) {
		sendPacket(kTypeProbe, probe.nonce, probe.endpoint, 0, nullptr);
		++m_probesSent;
	}
}

void Net::run() {
	double lastHousekeeping = 0.0;
	std::uint8_t buffer[2048];

	while (m_running.load()) {
		sockaddr_in from{};
		socklen_type fromLength = sizeof(from);
		const double before     = nowMs();
#ifdef _WIN32
		const int received = recvfrom(static_cast< SOCKET >(m_socket), reinterpret_cast< char * >(buffer),
									  static_cast< int >(sizeof(buffer)), 0, reinterpret_cast< sockaddr * >(&from),
									  &fromLength);
#else
		const int received = static_cast< int >(recvfrom(static_cast< int >(m_socket), buffer, sizeof(buffer), 0,
														 reinterpret_cast< sockaddr * >(&from), &fromLength));
#endif
		const double now = nowMs();

		if (received > 0) {
			Endpoint endpoint;
			endpoint.address = from.sin_addr.s_addr;
			endpoint.port    = ntohs(from.sin_port);
			handlePacket(buffer, static_cast< std::size_t >(received), endpoint, now);
		}
		// Errors (including the regular timeout) are not acted upon. Windows for instance reports an error
		// here whenever a probe was sent to a port nobody listens on. Just make sure not to spin on them.
		if (received < 0 && now - before < 1.0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}

		if (now - lastHousekeeping >= kProbeIntervalMs) {
			lastHousekeeping = now;
			housekeeping(now);
		}

		dispatchEvents();
	}
}

} // namespace roomy
