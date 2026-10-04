#include "suppressor.h"

#include <algorithm>
#include <cmath>

namespace roomy {

namespace {

	const Fft &sharedFft() {
		static const Fft fft(kFftSize);
		return fft;
	}

	/// Square root of a periodic Hann window: applied on analysis and synthesis it overlap-adds to unity
	const std::array< float, kWindow > &sharedWindow() {
		static const std::array< float, kWindow > window = [] {
			std::array< float, kWindow > w{};
			const double pi = 3.14159265358979323846;
			for (int i = 0; i < kWindow; ++i) {
				w[static_cast< std::size_t >(i)] =
					static_cast< float >(std::sqrt(0.5 - 0.5 * std::cos(2.0 * pi * i / kWindow)));
			}
			return w;
		}();
		return window;
	}

	// Bins between roughly 100 Hz and 8 kHz, used to judge who is talking
	constexpr int kSpeechLow  = 2;
	constexpr int kSpeechHigh = 171;

	/// A reference quieter than this (about -60 dBFS) is treated as silence
	constexpr float kActivePower = 5e-7f;
	/// How far back in time (on top of the microphone delay) a reference frame may have arrived and still count
	constexpr double kLookbackMs = 60.0;
	/// Per-frame decay (power) of the reference envelope, standing in for the room's reverberation
	constexpr float kEnvelopeDecay = 0.7f;
	/// A gap between calls longer than this means transmission was interrupted
	constexpr double kStreamGapMs = 150.0;
	constexpr float kLearnUp      = 0.2f;
	constexpr float kLearnDown    = 0.01f;
	constexpr float kGainRecovery = 0.4f;

	float bandSum(const PowerSpectrum &power) {
		float sum = 0.0f;
		for (int b = kSpeechLow; b < kSpeechHigh; ++b) {
			sum += power[static_cast< std::size_t >(b)];
		}
		return sum;
	}

} // namespace

Stft::Stft() : m_work(kFftSize) {
	reset();
}

void Stft::reset() {
	m_previous.fill(0.0f);
	m_overlap.fill(0.0f);
}

void Stft::analyze(const float *frame, Spectrum &spectrum) {
	const auto &window = sharedWindow();

	for (std::size_t i = 0; i < kFrame; ++i) {
		m_work[i]          = m_previous[i] * window[i];
		m_work[i + kFrame] = frame[i] * window[i + kFrame];
		m_previous[i]      = frame[i];
	}
	std::fill(m_work.begin() + kWindow, m_work.end(), std::complex< float >(0.0f, 0.0f));

	sharedFft().forward(m_work.data());

	std::copy(m_work.begin(), m_work.begin() + kBins, spectrum.begin());
}

void Stft::synthesize(const Spectrum &spectrum, float *frame) {
	const auto &window = sharedWindow();

	std::copy(spectrum.begin(), spectrum.end(), m_work.begin());
	for (std::size_t i = 1; i < kFftSize / 2; ++i) {
		m_work[kFftSize - i] = std::conj(spectrum[i]);
	}

	sharedFft().inverse(m_work.data());

	for (std::size_t i = 0; i < kWindow; ++i) {
		m_overlap[i] += m_work[i].real() * window[i];
	}

	std::copy(m_overlap.begin(), m_overlap.begin() + kFrame, frame);
	std::copy(m_overlap.begin() + kFrame, m_overlap.end(), m_overlap.begin());
	std::fill(m_overlap.begin() + kFrame, m_overlap.end(), 0.0f);
}

void Stft::power(const Spectrum &spectrum, PowerSpectrum &power) {
	const float scale = 1.0f / (static_cast< float >(kFftSize) * static_cast< float >(kFrame));
	for (std::size_t i = 0; i < kBins; ++i) {
		power[i] = std::norm(spectrum[i]) * scale;
	}
}


RefTrack::RefTrack() = default;

void RefTrack::push(const std::int16_t *pcm, std::uint32_t sequence, double nowMs) {
	std::array< float, kFrame > frame;
	for (std::size_t i = 0; i < kFrame; ++i) {
		frame[i] = static_cast< float >(pcm[i]) / 32768.0f;
	}

	std::lock_guard< std::mutex > lock(m_mutex);

	if (sequence != m_lastSequence + 1 || nowMs - m_lastArrivedMs > kStreamGapMs) {
		m_stft.reset();
	}
	m_lastSequence  = sequence;
	m_lastArrivedMs = nowMs;

	Spectrum spectrum;
	m_stft.analyze(frame.data(), spectrum);

	Entry &entry    = m_entries[m_next];
	entry.arrivedMs = nowMs;
	Stft::power(spectrum, entry.power);
	m_next = (m_next + 1) % m_entries.size();
}

bool RefTrack::envelope(double fromMs, double toMs, PowerSpectrum &out) const {
	out.fill(0.0f);
	bool any = false;

	std::lock_guard< std::mutex > lock(m_mutex);

	for (const Entry &entry : m_entries) {
		if (entry.arrivedMs < fromMs || entry.arrivedMs > toMs) {
			continue;
		}
		any = true;
		for (std::size_t i = 0; i < kBins; ++i) {
			out[i] = std::max(out[i], entry.power[i]);
		}
	}

	return any;
}


Suppressor::Suppressor(const SuppressorParams &params) : m_params(params) {
	m_params.delayFrames = std::max(0, m_params.delayFrames);
	m_delayLine.assign(static_cast< std::size_t >(m_params.delayFrames) * kFrame, 0.0f);
	m_gains.fill(1.0f);
}

void Suppressor::resetStream() {
	m_stft.reset();
	std::fill(m_delayLine.begin(), m_delayLine.end(), 0.0f);
	m_delayPos = 0;
	m_gains.fill(1.0f);
	for (auto &entry : m_peers) {
		entry.second.envelope.fill(0.0f);
	}
}

void Suppressor::process(float *frame, double nowMs, const RefInput *refs, std::size_t refCount) {
	if (nowMs - m_lastCallMs > kStreamGapMs) {
		resetStream();
	}
	m_lastCallMs = nowMs;

	// Hold the microphone back so that the reference describing the same moment has had time to arrive
	if (!m_delayLine.empty()) {
		for (std::size_t i = 0; i < kFrame; ++i) {
			std::swap(frame[i], m_delayLine[m_delayPos + i]);
		}
		m_delayPos = (m_delayPos + kFrame) % m_delayLine.size();
	}

	Spectrum spectrum;
	m_stft.analyze(frame, spectrum);

	PowerSpectrum micPower;
	Stft::power(spectrum, micPower);
	const float micBand = bandSum(micPower);

	PowerSpectrum bleed;
	bleed.fill(0.0f);
	bool anyActive = false;

	const double delayMs = m_params.delayFrames * kFrameMs;
	const float logMax   = std::log(m_params.maxCoupling);

	for (std::size_t r = 0; r < refCount; ++r) {
		PeerState &peer = m_peers[refs[r].id];
		if (peer.logCoupling.empty()) {
			peer.logCoupling.assign(kBins, std::log(std::min(m_params.initialCoupling, m_params.maxCoupling)));
		}

		PowerSpectrum recent;
		refs[r].track->envelope(nowMs - delayMs - kLookbackMs, nowMs, recent);
		for (std::size_t i = 0; i < kBins; ++i) {
			peer.envelope[i] = std::max(recent[i], peer.envelope[i] * kEnvelopeDecay);
		}

		const float refBand = bandSum(peer.envelope);
		if (refBand < kActivePower) {
			continue;
		}
		anyActive = true;

		// Only learn how the remote voice leaks into this microphone while the remote talker clearly dominates.
		// Otherwise the local voice would be mistaken for leak.
		if (micBand < m_params.dominanceRatio * refBand) {
			const float minBinPower = 0.01f * refBand / static_cast< float >(kSpeechHigh - kSpeechLow);
			for (std::size_t i = 0; i < kBins; ++i) {
				// Skip bins that only hold the decaying tail of the envelope: once the remote person pauses
				// there is nothing to measure, and learning from it would drag the estimate down.
				if (peer.envelope[i] < minBinPower || recent[i] < 0.5f * peer.envelope[i]) {
					continue;
				}
				const float target = std::min(std::log(micPower[i] / peer.envelope[i] + 1e-12f), logMax);
				const float rate   = target > peer.logCoupling[i] ? kLearnUp : kLearnDown;
				peer.logCoupling[i] += rate * (target - peer.logCoupling[i]);
			}
		}

		for (std::size_t i = 0; i < kBins; ++i) {
			bleed[i] += m_params.overSubtraction * std::exp(peer.logCoupling[i]) * peer.envelope[i];
		}
	}

	// If the whole frame holds no more than the expected leak, nobody is talking locally and the frame is
	// turned down as a whole. Judging single bins alone lets too much through, as their level fluctuates a lot.
	const bool bleedOnly =
		anyActive && micBand * m_params.overSubtraction < m_params.bleedOnlyFactor * bandSum(bleed);

	const float floorPower = m_params.floorGain * m_params.floorGain;
	for (std::size_t i = 0; i < kBins; ++i) {
		float gain = 1.0f;
		if (bleedOnly) {
			gain = m_params.floorGain;
		} else if (anyActive) {
			gain = std::sqrt(std::max(1.0f - bleed[i] / (micPower[i] + 1e-12f), floorPower));
		}
		// Drop immediately, recover gradually
		if (gain < m_gains[i]) {
			m_gains[i] = gain;
		} else {
			m_gains[i] += kGainRecovery * (gain - m_gains[i]);
		}
		spectrum[i] *= m_gains[i];
	}

	m_stft.synthesize(spectrum, frame);
}

} // namespace roomy
