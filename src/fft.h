// Minimal in-place radix-2 complex FFT. Size must be a power of two.
#pragma once

#include <cmath>
#include <complex>
#include <cstddef>
#include <utility>
#include <vector>

namespace roomy {

class Fft {
public:
	explicit Fft(std::size_t size) : m_size(size), m_twiddles(size / 2), m_reversed(size) {
		const double pi = 3.14159265358979323846;
		for (std::size_t i = 0; i < size / 2; ++i) {
			const double angle = -2.0 * pi * static_cast< double >(i) / static_cast< double >(size);
			m_twiddles[i]      = std::complex< float >(static_cast< float >(std::cos(angle)),
                                                  static_cast< float >(std::sin(angle)));
		}

		std::size_t bits = 0;
		while ((std::size_t(1) << bits) < size) {
			++bits;
		}
		for (std::size_t i = 0; i < size; ++i) {
			std::size_t r = 0;
			for (std::size_t b = 0; b < bits; ++b) {
				if (i & (std::size_t(1) << b)) {
					r |= std::size_t(1) << (bits - 1 - b);
				}
			}
			m_reversed[i] = r;
		}
	}

	std::size_t size() const { return m_size; }

	void forward(std::complex< float > *data) const { transform(data, false); }

	/// Inverse transform including the 1/N scaling
	void inverse(std::complex< float > *data) const {
		transform(data, true);
		const float scale = 1.0f / static_cast< float >(m_size);
		for (std::size_t i = 0; i < m_size; ++i) {
			data[i] *= scale;
		}
	}

private:
	void transform(std::complex< float > *data, bool inverse) const {
		for (std::size_t i = 0; i < m_size; ++i) {
			if (i < m_reversed[i]) {
				std::swap(data[i], data[m_reversed[i]]);
			}
		}

		for (std::size_t len = 2; len <= m_size; len <<= 1) {
			const std::size_t half = len / 2;
			const std::size_t step = m_size / len;
			for (std::size_t start = 0; start < m_size; start += len) {
				for (std::size_t k = 0; k < half; ++k) {
					std::complex< float > w = m_twiddles[k * step];
					if (inverse) {
						w = std::conj(w);
					}
					const std::complex< float > a = data[start + k];
					const std::complex< float > b = data[start + k + half] * w;
					data[start + k]               = a + b;
					data[start + k + half]        = a - b;
				}
			}
		}
	}

	std::size_t m_size;
	std::vector< std::complex< float > > m_twiddles;
	std::vector< std::size_t > m_reversed;
};

} // namespace roomy
