#pragma once

/* The microphone's queue: written by the thread that calls retro_run, drained
 * by the emulation thread inside maple DMA.
 *
 * The policy lives here rather than in microphone.cpp so the test build, which
 * is not a libretro build, exercises the same code the core runs.
 *
 * Two rules keep it single-producer/single-consumer:
 *   - the producer never reads, not even to make room. A full queue drops the
 *     NEW block, because a producer calling read() would be a second reader.
 *   - the consumer does all the trimming, so latency is bounded without the
 *     producer ever touching the read cursor.
 */

#include "audio/audiostream.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

class MicrophoneQueue
{
public:
	/* Sized once and never again: resizing while the consumer is inside read()
	 * would be a use-after-free. */
	void setCapacity(u32 bytes)
	{
		ring.setCapacity(bytes);
	}

	/* Keep at most a quarter second, trim back to 70 ms. A game asks for 240
	 * samples at a time; more than that in hand is just latency. */
	void setDeviceRate(unsigned rate)
	{
		highWater = (u32)(rate / 4) * 2u;
		target = (u32)(rate * 7 / 100) * 2u;
	}

	/* Producer. Returns false when the block was dropped. */
	bool write(const int16_t *samples, size_t count)
	{
		if (samples == nullptr || count == 0)
			return true;
		return ring.write((const u8 *)samples, (u32)(count * sizeof(int16_t)));
	}

	/* Consumer. `generation` is bumped by a start, a reset or a restored
	 * savestate; whatever is queued then belongs to before it. Returns the
	 * number of samples delivered, which may be fewer than asked for and is
	 * routinely zero -- 240 samples is longer than a frame. */
	u32 read(void *buffer, u32 samples, uint32_t generation)
	{
		if (buffer == nullptr || samples == 0)
			return 0;

		if (generation != seenGeneration)
		{
			seenGeneration = generation;
			drain();
		}

		u32 avail = ring.readSize();
		if (avail > highWater)
		{
			u8 junk[512];
			while (avail > target + sizeof(junk) && ring.read(junk, sizeof(junk)))
				avail -= sizeof(junk);
		}

		u32 want = std::min<u32>((u32)(samples * sizeof(int16_t)), avail) & ~1u;
		if (want == 0 || !ring.read((u8 *)buffer, want))
			return 0;

		/* The caller transmits ceil(n/2)*4 bytes whatever it got back, so zero
		 * what it would otherwise read past the samples delivered. */
		std::memset((u8 *)buffer + want, 0, samples * sizeof(int16_t) - want);
		return want / (u32)sizeof(int16_t);
	}

	u32 depthBytes() { return ring.readSize(); }

	void drain()
	{
		/* Down to the last sample: read() is all-or-nothing, so draining in
		 * fixed blocks leaves whatever does not fill one behind -- and that
		 * remainder is pre-savestate audio the game would then hear. */
		u8 junk[512];
		for (;;)
		{
			const u32 avail = ring.readSize();
			if (avail == 0)
				break;
			const u32 take = std::min<u32>(avail, (u32)sizeof(junk));
			if (!ring.read(junk, take))
				break;
		}
	}

private:
	RingBuffer ring;
	uint32_t seenGeneration = 0;
	u32 highWater = 8192;
	u32 target = 3072;
};
