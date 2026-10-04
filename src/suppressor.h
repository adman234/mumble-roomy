// Spectral crosstalk suppressor: turns down the parts of the local microphone
// signal that are dominated by another person in the same room, using that
// person's own microphone signal as the reference.
#pragma once

#include "fft.h"

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace roomy {

constexpr int kSampleRate = 48000;
/// Samples per audio frame (10 ms), the unit Mumble hands to plugins
constexpr int kFrame = 480;
/// Analysis window: two frames with 50% overlap
constexpr int kWindow = 2 * kFrame;
constexpr int kFftSize = 1024;
constexpr int kBins    = kFftSize / 2 + 1;
constexpr double kFrameMs = 1000.0 * kFrame / kSampleRate;

using Spectrum      = std::array< std::complex< float >, kBins >;
using PowerSpectrum = std::array< float, kBins >;

/// Short-time Fourier transform with overlap-add resynthesis.
/// Each call consumes or produces one frame; the output lags the input by one frame.
class Stft {
public:
	Stft();

	void reset();

	/// Feeds one frame and computes the spectrum of the window made of the previous and the given frame
	void analyze(const float *frame, Spectrum &spectrum);

	/// Resynthesizes the given spectrum and writes the frame that is now complete (the previous input frame)
	void synthesize(const Spectrum &spectrum, float *frame);

	/// Power per bin, scaled such that the sum over all bins is half the mean square of the signal
	static void power(const Spectrum &spectrum, PowerSpectrum &power);

private:
	std::array< float, kFrame > m_previous;
	std::array< float, kWindow > m_overlap;
	std::vector< std::complex< float > > m_work;
};

/// Recent power spectra of one remote microphone.
/// Written by the network thread, read by the audio thread.
class RefTrack {
public:
	RefTrack();

	/// Adds one received frame of 16 bit mono audio
	void push(const std::int16_t *pcm, std::uint32_t sequence, double nowMs);

	/// Writes the per-bin maximum over all frames that arrived within [fromMs, toMs].
	/// @returns Whether there was at least one such frame
	bool envelope(double fromMs, double toMs, PowerSpectrum &out) const;

private:
	struct Entry {
		double arrivedMs = -1e18;
		PowerSpectrum power{};
	};

	mutable std::mutex m_mutex;
	Stft m_stft;
	std::array< Entry, 16 > m_entries;
	std::size_t m_next          = 0;
	std::uint32_t m_lastSequence = 0;
	double m_lastArrivedMs       = -1e18;
};

struct SuppressorParams {
	/// Factor (power) by which the estimated bleed is overestimated before it is removed
	float overSubtraction = 2.0f;
	/// Lowest gain (amplitude) ever applied to a bin
	float floorGain = 0.0316f;
	/// Frames the microphone is held back so that the reference is available in time
	int delayFrames = 2;
	/// The remote talker counts as dominant if local power is below this fraction of the reference power
	float dominanceRatio = 0.25f;
	/// Upper bound (power) for the learned leak from the remote microphone into the local one
	float maxCoupling = 0.25f;
	/// Leak (power) assumed before anything has been learned
	float initialCoupling = 0.0316f;
	/// A frame counts as holding nothing but leak if its power is below this multiple of the expected leak
	float bleedOnlyFactor = 2.0f;
};

struct RefInput {
	std::uint32_t id;
	const RefTrack *track;
};

class Suppressor {
public:
	explicit Suppressor(const SuppressorParams &params = SuppressorParams());

	/// Processes one frame in place. The output lags the input by latencyFrames() frames.
	void process(float *frame, double nowMs, const RefInput *refs, std::size_t refCount);

	int latencyFrames() const { return m_params.delayFrames + 1; }

	/// The per-bin gains applied by the most recent call to process()
	const PowerSpectrum &gains() const { return m_gains; }

	/// Forgets the audio history (but keeps what has been learned about the room)
	void resetStream();

private:
	struct PeerState {
		std::vector< float > logCoupling;
		PowerSpectrum envelope{};
	};

	SuppressorParams m_params;
	Stft m_stft;
	std::vector< float > m_delayLine;
	std::size_t m_delayPos = 0;
	std::map< std::uint32_t, PeerState > m_peers;
	PowerSpectrum m_gains;
	double m_lastCallMs = -1e18;
};

} // namespace roomy
