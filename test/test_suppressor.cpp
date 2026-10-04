// Runs the suppressor on two synthetic voices in a simulated room and checks how
// much of the other voice is removed and how much of the own voice survives.
//
// The voices are harmonic tones with a syllable rhythm, not real speech. This
// verifies the mechanics, it does not predict quality on real microphones.

#include "suppressor.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace roomy;

namespace {

const double kPi = 3.14159265358979323846;

std::vector< float > makeVoice(double pitch, double syllableRate, unsigned seed, double seconds) {
	const std::size_t count = static_cast< std::size_t >(seconds * kSampleRate);
	std::vector< float > out(count);
	std::srand(seed);

	const double formants[3] = { 500.0 + 40.0 * (seed % 5), 1500.0 + 90.0 * (seed % 7), 2600.0 };

	for (std::size_t n = 0; n < count; ++n) {
		const double t       = static_cast< double >(n) / kSampleRate;
		// Phase of the fundamental with the pitch wobbling by 1% five times a second
		const double phase = 2.0 * kPi * pitch * t - 0.01 * pitch / 5.0 * std::cos(2.0 * kPi * 5.0 * t);
		double sample        = 0.0;
		for (int h = 1; h * pitch < 7000.0; ++h) {
			const double freq = h * pitch;
			double amp        = 0.02;
			for (double formant : formants) {
				const double d = (freq - formant) / 250.0;
				amp += std::exp(-0.5 * d * d);
			}
			sample += amp / h * std::sin(h * phase + h);
		}
		const double syllable = std::max(0.0, std::sin(2.0 * kPi * syllableRate * t));
		const double noise    = (std::rand() / static_cast< double >(RAND_MAX) - 0.5) * 0.02;
		out[n]                = static_cast< float >(0.08 * sample * syllable * syllable + 0.002 * noise);
	}
	return out;
}

/// What a voice sounds like in the other person's microphone: quieter, later and with a few reflections
std::vector< float > leak(const std::vector< float > &voice, double gain) {
	const std::size_t taps[4] = { 288, 1104, 2256, 4100 }; // 6, 23, 47 and 85 ms
	const double weights[4]   = { 1.0, 0.5, 0.3, 0.15 };
	std::vector< float > out(voice.size(), 0.0f);
	for (int k = 0; k < 4; ++k) {
		for (std::size_t n = taps[k]; n < voice.size(); ++n) {
			out[n] += static_cast< float >(gain * weights[k] * voice[n - taps[k]]);
		}
	}
	return out;
}

void mute(std::vector< float > &signal, double fromSeconds, double toSeconds) {
	const std::size_t from = static_cast< std::size_t >(fromSeconds * kSampleRate);
	const std::size_t to   = std::min(signal.size(), static_cast< std::size_t >(toSeconds * kSampleRate));
	for (std::size_t n = from; n < to; ++n) {
		signal[n] = 0.0f;
	}
}

double energy(const std::vector< float > &signal, double fromSeconds, double toSeconds) {
	const std::size_t from = static_cast< std::size_t >(fromSeconds * kSampleRate);
	const std::size_t to   = std::min(signal.size(), static_cast< std::size_t >(toSeconds * kSampleRate));
	double sum             = 0.0;
	for (std::size_t n = from; n < to; ++n) {
		sum += static_cast< double >(signal[n]) * signal[n];
	}
	return sum + 1e-20;
}

double toDb(double ratio) {
	return 10.0 * std::log10(ratio);
}

int failures = 0;

void check(bool ok, const char *what) {
	std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
	if (!ok) {
		++failures;
	}
}

} // namespace

int main() {
	// 0-4 s: only the remote person talks. 4-8 s: only the local person talks. 8-12 s: both talk.
	const double seconds = 12.0;
	std::vector< float > local  = makeVoice(118.0, 3.1, 3, seconds);
	std::vector< float > remote = makeVoice(207.0, 2.3, 11, seconds);
	mute(local, 0.0, 4.0);
	mute(remote, 4.0, 8.0);

	// Both leak into each other's microphone at about -14 dB
	const std::vector< float > bleed  = leak(remote, 0.2);
	const std::vector< float > spill  = leak(local, 0.2);
	const std::size_t count           = local.size();
	const std::size_t frames          = count / kFrame;
	std::vector< float > remoteMic(count);
	for (std::size_t n = 0; n < count; ++n) {
		remoteMic[n] = remote[n] + spill[n];
	}

	const SuppressorParams params;
	Suppressor suppressor(params);
	RefTrack track;

	// The same gains are applied to both components separately, which shows what happened to each of them
	Stft localShadow, bleedShadow;
	std::vector< float > localDelay(static_cast< std::size_t >(params.delayFrames) * kFrame, 0.0f);
	std::vector< float > bleedDelay(localDelay.size(), 0.0f);

	std::vector< float > localIn(count, 0.0f), localOut(count, 0.0f), bleedIn(count, 0.0f), bleedOut(count, 0.0f);
	std::vector< float > output(count, 0.0f), reconstructed(count, 0.0f);

	const std::size_t latency = static_cast< std::size_t >(suppressor.latencyFrames()) * kFrame;

	for (std::size_t f = 0; f < frames; ++f) {
		const std::size_t offset = f * kFrame;
		const double nowMs       = (f + 1) * kFrameMs;

		// The reference arrives over the network with a varying delay
		std::int16_t pcm[kFrame];
		for (std::size_t i = 0; i < kFrame; ++i) {
			pcm[i] = static_cast< std::int16_t >(std::lround(remoteMic[offset + i] * 32767.0f));
		}
		track.push(pcm, static_cast< std::uint32_t >(f), nowMs + 1.0 + (f % 4) * 2.5);

		float mic[kFrame], localPart[kFrame], bleedPart[kFrame];
		for (std::size_t i = 0; i < kFrame; ++i) {
			localPart[i] = local[offset + i];
			bleedPart[i] = bleed[offset + i];
			mic[i]       = localPart[i] + bleedPart[i];
		}

		const RefInput ref = { 1, &track };
		suppressor.process(mic, nowMs, &ref, 1);

		for (std::size_t i = 0; i < kFrame; ++i) {
			output[offset + i] = mic[i];
		}

		struct Component {
			float *frame;
			std::vector< float > &delay;
			Stft &stft;
			std::vector< float > &out;
		};
		Component components[2] = { { localPart, localDelay, localShadow, localOut },
									{ bleedPart, bleedDelay, bleedShadow, bleedOut } };
		const std::size_t delayPos = localDelay.empty() ? 0 : offset % localDelay.size();
		for (Component &component : components) {
			if (!component.delay.empty()) {
				for (std::size_t i = 0; i < kFrame; ++i) {
					std::swap(component.frame[i], component.delay[delayPos + i]);
				}
			}
			Spectrum spectrum;
			component.stft.analyze(component.frame, spectrum);
			for (std::size_t i = 0; i < kBins; ++i) {
				spectrum[i] *= suppressor.gains()[i];
			}
			component.stft.synthesize(spectrum, component.frame);
			for (std::size_t i = 0; i < kFrame; ++i) {
				component.out[offset + i] = component.frame[i];
			}
		}

		// Time-aligned copies of the unprocessed components for comparison
		if (offset >= latency) {
			for (std::size_t i = 0; i < kFrame; ++i) {
				localIn[offset + i] = local[offset - latency + i];
				bleedIn[offset + i] = bleed[offset - latency + i];
			}
		}
		for (std::size_t i = 0; i < kFrame; ++i) {
			reconstructed[offset + i] = localOut[offset + i] + bleedOut[offset + i];
		}
	}

	// Sanity: the two shadow components add up to what the suppressor itself produced
	double mismatch = 0.0;
	for (std::size_t n = 0; n < count; ++n) {
		const double d = static_cast< double >(output[n]) - reconstructed[n];
		mismatch += d * d;
	}
	const double mismatchDb = toDb(mismatch / energy(output, 0.0, seconds) + 1e-20);

	const double remoteOnlyBleed = toDb(energy(bleedOut, 2.0, 4.0) / energy(bleedIn, 2.0, 4.0));
	const double localOnlyVoice  = toDb(energy(localOut, 4.5, 8.0) / energy(localIn, 4.5, 8.0));
	const double bothBleed       = toDb(energy(bleedOut, 8.5, 12.0) / energy(bleedIn, 8.5, 12.0));
	const double bothVoice       = toDb(energy(localOut, 8.5, 12.0) / energy(localIn, 8.5, 12.0));

	std::printf("latency: %d ms\n", static_cast< int >(suppressor.latencyFrames() * kFrameMs));
	std::printf("shadow mismatch:                     %7.1f dB\n", mismatchDb);
	std::printf("remote talks alone, bleed change:    %7.1f dB\n", remoteOnlyBleed);
	std::printf("local talks alone, own voice change: %7.1f dB\n", localOnlyVoice);
	std::printf("both talk, bleed change:             %7.1f dB\n", bothBleed);
	std::printf("both talk, own voice change:         %7.1f dB\n", bothVoice);

	check(mismatchDb < -60.0, "shadow components add up to the real output");
	check(remoteOnlyBleed < -15.0, "bleed is reduced by at least 15 dB while only the remote person talks");
	check(localOnlyVoice > -1.0, "own voice loses less than 1 dB while talking alone");
	check(bothBleed < -3.0, "bleed is reduced by at least 3 dB while both talk");
	check(bothVoice > -3.0, "own voice loses less than 3 dB while both talk");

	// Without any reference the signal must pass through unchanged apart from the delay
	{
		Suppressor passthrough(params);
		double error = 0.0, total = 0.0;
		for (std::size_t f = 0; f < 200; ++f) {
			const std::size_t offset = (400 + f) * kFrame;
			float mic[kFrame];
			for (std::size_t i = 0; i < kFrame; ++i) {
				mic[i] = local[offset + i];
			}
			passthrough.process(mic, (f + 1) * kFrameMs, nullptr, 0);
			if (f * kFrame < latency + kFrame) {
				continue;
			}
			for (std::size_t i = 0; i < kFrame; ++i) {
				const double expected = local[offset - latency + i];
				error += (mic[i] - expected) * (mic[i] - expected);
				total += expected * expected;
			}
		}
		const double errorDb = toDb(error / (total + 1e-20) + 1e-20);
		std::printf("passthrough error:                   %7.1f dB\n", errorDb);
		check(errorDb < -60.0, "signal passes through unchanged when there is no reference");
	}

	return failures == 0 ? 0 : 1;
}
