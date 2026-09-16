#include <gtest/gtest.h>

#include "../../../shell/libretro/MicrophoneResampler.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace {

constexpr double PI = 3.14159265358979323846;

std::vector<int16_t> sine(size_t count, double sampleRate, double freq, double amplitude)
{
	std::vector<int16_t> out(count);
	for (size_t i = 0; i < count; i++)
		out[i] = (int16_t)(32767.0 * amplitude * std::sin(2.0 * PI * freq * (double)i / sampleRate));
	return out;
}

/* Magnitude of one frequency bin, so a tone can be measured before and after. */
double toneLevel(const int16_t *x, size_t n, double sampleRate, double freq)
{
	if (n == 0)
		return 0.0;
	const double w = 2.0 * PI * freq / sampleRate;
	const double k = 2.0 * std::cos(w);
	double s1 = 0.0, s2 = 0.0;
	for (size_t i = 0; i < n; i++)
	{
		const double s0 = (double)x[i] + k * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	const double real = s1 - s2 * std::cos(w);
	const double imag = s2 * std::sin(w);
	return std::sqrt(real * real + imag * imag) * 2.0 / (double)n;
}

double rms(const int16_t *x, size_t n)
{
	double acc = 0.0;
	for (size_t i = 0; i < n; i++)
		acc += (double)x[i] * (double)x[i];
	return (n == 0) ? 0.0 : std::sqrt(acc / (double)n);
}

} // namespace

/* The assertion the whole design rests on. Asking the frontend for 11025 Hz
 * gets nearest-sample decimation with no filter, which folds a 9 kHz component
 * back to |9000 - 11025| = 2025 Hz at full amplitude -- onto the formants of
 * the speech this device exists for. A naive decimator scores about 0 dB here. */
TEST(MicrophoneResampler, RejectsAudioAboveTheDeviceNyquist)
{
	MicDecimator d;
	d.configure(48000, 11025);

	const std::vector<int16_t> in = sine(48000, 48000.0, 9000.0, 0.8);
	std::vector<int16_t> out(d.maxOutput(in.size()));
	const size_t n = d.process(in.data(), in.size(), out.data());
	ASSERT_GT(n, 0u);

	const double before = toneLevel(in.data(), in.size(), 48000.0, 9000.0);
	const double alias = toneLevel(out.data(), n, 11025.0, 2025.0);
	const double db = 20.0 * std::log10((alias + 1e-9) / (before + 1e-9));

	EXPECT_LT(db, -40.0) << "9 kHz folded back at " << db << " dB";
}

TEST(MicrophoneResampler, PassesTheSpeechBand)
{
	MicDecimator d;
	d.configure(48000, 11025);

	const std::vector<int16_t> in = sine(48000, 48000.0, 1000.0, 0.5);
	std::vector<int16_t> out(d.maxOutput(in.size()));
	const size_t n = d.process(in.data(), in.size(), out.data());
	ASSERT_GT(n, 0u);

	const double db = 20.0 * std::log10((rms(out.data(), n) + 1e-9) / (rms(in.data(), in.size()) + 1e-9));
	EXPECT_NEAR(db, 0.0, 1.0) << "1 kHz came through at " << db << " dB";
}

TEST(MicrophoneResampler, ProducesTheRightNumberOfSamples)
{
	const std::vector<int16_t> in = sine(48000, 48000.0, 440.0, 0.3);

	for (const unsigned rate : { 11025u, 8000u })
	{
		MicDecimator d;
		d.configure(48000, rate);
		std::vector<int16_t> out(d.maxOutput(in.size()));
		const size_t n = d.process(in.data(), in.size(), out.data());
		EXPECT_NEAR((double)n, (double)rate, 2.0) << "at " << rate << " Hz";
		EXPECT_LE(n, out.size());
	}
}

TEST(MicrophoneResampler, NeverOverflowsOrProducesNaN)
{
	MicDecimator d;
	d.configure(48000, 11025);

	std::vector<int16_t> in(48000);
	for (size_t i = 0; i < in.size(); i++)
		in[i] = (i % 2 == 0) ? 32767 : -32768;      /* full-scale square */

	std::vector<int16_t> out(d.maxOutput(in.size()));
	const size_t n = d.process(in.data(), in.size(), out.data());
	for (size_t i = 0; i < n; i++)
	{
		EXPECT_GE(out[i], -32768);
		EXPECT_LE(out[i], 32767);
	}
}

/* A savestate load, or a stop and start, must not leak the delay line. */
TEST(MicrophoneResampler, ResetIsComplete)
{
	MicDecimator d;
	d.configure(48000, 11025);
	const std::vector<int16_t> in = sine(9600, 48000.0, 700.0, 0.6);

	std::vector<int16_t> first(d.maxOutput(in.size()));
	const size_t n1 = d.process(in.data(), in.size(), first.data());

	d.reset();
	std::vector<int16_t> second(d.maxOutput(in.size()));
	const size_t n2 = d.process(in.data(), in.size(), second.data());

	ASSERT_EQ(n1, n2);
	EXPECT_EQ(0, std::memcmp(first.data(), second.data(), n1 * sizeof(int16_t)));
}
