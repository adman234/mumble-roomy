// Links two network endpoints over the loopback interface and checks that audio
// sent by one of them ends up in the other's reference track.

#include "net.h"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace roomy;

namespace {

int failures = 0;

void check(bool ok, const char *what) {
	std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
	if (!ok) {
		++failures;
	}
}

bool waitForLink(Net &net, std::uint32_t expectedUser) {
	for (int i = 0; i < 60; ++i) {
		for (const LinkEvent &event : net.takeEvents()) {
			if (event.linked && event.userId == expectedUser) {
				return true;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	return false;
}

} // namespace

int main() {
	check(isPrivateAddress("192.168.1.20"), "192.168.1.20 is private");
	check(isPrivateAddress("10.0.0.5"), "10.0.0.5 is private");
	check(isPrivateAddress("172.31.255.1"), "172.31.255.1 is private");
	check(!isPrivateAddress("172.32.0.1"), "172.32.0.1 is not private");
	check(!isPrivateAddress("8.8.8.8"), "8.8.8.8 is not private");
	check(!isPrivateAddress("not an address"), "garbage is not private");

	Net alice, bob, stranger;
	check(alice.start(0) && bob.start(0) && stranger.start(0), "sockets open");
	check(alice.port() != 0 && alice.port() != bob.port(), "ports are assigned");
	check(alice.nonce() != bob.nonce(), "nonces differ");

	// Alice is user 1, Bob is user 2
	alice.addPeer(2, bob.nonce(), { "127.0.0.1" }, bob.port());
	bob.addPeer(1, alice.nonce(), { "127.0.0.1" }, alice.port());
	// The stranger knows where Bob is but Bob has never heard of it
	stranger.addPeer(2, bob.nonce(), { "127.0.0.1" }, bob.port());

	check(waitForLink(alice, 2), "Alice links with Bob");
	check(waitForLink(bob, 1), "Bob links with Alice");

	LinkedRef refs[kMaxPeers];
	check(bob.linked(refs, kMaxPeers) == 1 && refs[0].userId == 1, "Bob only accepts Alice");

	std::int16_t pcm[kFrame];
	for (int i = 0; i < kFrame; ++i) {
		pcm[i] = static_cast< std::int16_t >((i % 48 < 24) ? 8000 : -8000);
	}
	const double start = nowMs();
	for (int i = 0; i < 5; ++i) {
		alice.sendAudio(pcm);
		stranger.sendAudio(pcm);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	PowerSpectrum envelope;
	const bool received = refs[0].track->envelope(start - 1.0, nowMs(), envelope);
	float total         = 0.0f;
	for (float value : envelope) {
		total += value;
	}
	check(received && total > 0.01f, "Alice's audio arrives at Bob");

	LinkedRef aliceRefs[kMaxPeers];
	check(alice.linked(aliceRefs, kMaxPeers) == 1, "Alice has one link");
	PowerSpectrum silent;
	check(!aliceRefs[0].track->envelope(start - 1.0, nowMs(), silent), "Bob sent nothing, Alice received nothing");

	alice.stop();
	bob.stop();
	stranger.stop();

	return failures == 0 ? 0 : 1;
}
