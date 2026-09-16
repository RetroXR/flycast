#pragma once

/* Rate conversion for the Dreamcast microphone.
 *
 * The device asks for 8000 or 11025 Hz. The frontend is asked for 48000 instead
 * and the conversion happens here, because a frontend resampling 48000 down to
 * 11025 does it by picking the nearest samples with no filter at all: a 9 kHz
 * component folds back to 2025 Hz at full amplitude, which lands on the
 * formants of the two speech games this exists for.
 *
 * A windowed-sinc low-pass at the input rate, then linear interpolation between
 * FILTERED samples. Interpolating there is honest: by that point the signal is
 * band-limited well below half the output rate.
 *
 * Deliberately free of flycast headers -- no types.h, no u32 -- so the test
 * build, which is not a libretro build, can include it.
 */

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

/* Its own, rather than M_PI: that one is not standard C++ and MSVC hides it
 * behind _USE_MATH_DEFINES. */
static constexpr double MIC_PI = 3.14159265358979323846;

class MicDecimator
{
public:
	void configure(unsigned inRate, unsigned outRate)
	{
		if (inRate == 0 || outRate == 0)
			return;

		inRate_ = inRate;
		outRate_ = outRate;
		step_ = (double)inRate / (double)outRate;

		/* Leave a transition band of 15% of the output rate: wide enough to
		 * keep the taps cheap, narrow enough that speech is untouched. */
		const double transition = 0.15 * (double)outRate;
		int taps = (int)(3.3 * (double)inRate / transition);
		taps |= 1;                      /* odd, so the delay is a whole sample */
		if (taps < 31)
			taps = 31;
		else if (taps > 255)
			taps = 255;

		/* Cut off half a transition band below the output Nyquist, in cycles
		 * per input sample. */
		const double fc = (0.5 * (double)outRate - 0.5 * transition) / (double)inRate;

		taps_.assign((size_t)taps, 0.0f);
		const double mid = (taps - 1) / 2.0;
		double sum = 0.0;
		for (int i = 0; i < taps; i++)
		{
			const double t = (double)i - mid;
			const double sinc = (std::fabs(t) < 1e-9)
				? 2.0 * fc
				: std::sin(2.0 * MIC_PI * fc * t) / (MIC_PI * t);
			const double window = 0.54 - 0.46 * std::cos(2.0 * MIC_PI * (double)i / (double)(taps - 1));
			const double h = sinc * window;
			taps_[(size_t)i] = (float)h;
			sum += h;
		}
		if (sum != 0.0)
		{
			for (float &h : taps_)
				h = (float)(h / sum);   /* unity gain at DC */
		}

		reset();
	}

	void reset()
	{
		hist_.assign(taps_.size(), 0.0f);
		histPos_ = 0;
		phase_ = 0.0;
		prev_ = 0.0f;
		havePrev_ = false;
	}

	/* Upper bound on what process() can emit, for sizing the output buffer. */
	size_t maxOutput(size_t inCount) const
	{
		if (step_ <= 0.0)
			return 0;
		return (size_t)((double)inCount / step_) + 2;
	}

	size_t process(const int16_t *in, size_t inCount, int16_t *out)
	{
		if (taps_.empty() || in == nullptr || out == nullptr)
			return 0;

		const size_t n = taps_.size();
		size_t written = 0;

		for (size_t i = 0; i < inCount; i++)
		{
			hist_[histPos_] = (float)in[i];
			histPos_ = (histPos_ + 1) % n;

			float acc = 0.0f;
			size_t h = histPos_;
			for (size_t k = 0; k < n; k++)
			{
				acc += hist_[h] * taps_[k];
				h = (h + 1) % n;
			}

			if (!havePrev_)
			{
				prev_ = acc;
				havePrev_ = true;
				continue;
			}

			while (phase_ <= 1.0)
			{
				const float v = prev_ + (acc - prev_) * (float)phase_;
				out[written++] = clamp16(v);
				phase_ += step_;
			}
			phase_ -= 1.0;
			prev_ = acc;
		}

		return written;
	}

	unsigned inputRate() const { return inRate_; }
	unsigned outputRate() const { return outRate_; }
	size_t tapCount() const { return taps_.size(); }

private:
	static int16_t clamp16(float v)
	{
		if (v > 32767.0f)
			return 32767;
		if (v < -32768.0f)
			return -32768;
		return (int16_t)(v < 0.0f ? v - 0.5f : v + 0.5f);
	}

	std::vector<float> taps_;
	std::vector<float> hist_;
	size_t histPos_ = 0;
	double step_ = 1.0;
	double phase_ = 0.0;
	float prev_ = 0.0f;
	bool havePrev_ = false;
	unsigned inRate_ = 0;
	unsigned outRate_ = 0;
};
