#include <gtest/gtest.h>

#include "../../../shell/libretro/MicrophoneQueue.h"

#include <vector>

namespace {

/* In place: the queue owns atomics, so it can be neither copied nor moved. */
void setUp(MicrophoneQueue &q, unsigned rate = 11025)
{
	q.setCapacity(32768);
	q.setDeviceRate(rate);
}

std::vector<int16_t> block(size_t count, int16_t value)
{
	return std::vector<int16_t>(count, value);
}

} // namespace

/* The game asks every frame whether there is anything to hear. There usually
 * is not -- 240 samples at 11025 Hz is 21.8 ms, longer than a frame -- and it
 * must be told so rather than made to wait. */
TEST(MicrophoneQueue, NeverHangsWhenEmpty)
{
	MicrophoneQueue q;
	setUp(q);
	int16_t buffer[240];

	for (int i = 0; i < 100000; i++)
	{
		std::memset(buffer, 0x5A, sizeof(buffer));
		const u32 got = q.read(buffer, 240, 1);
		ASSERT_EQ(got, 0u);
	}
}

TEST(MicrophoneQueue, DeliversWhatWasWrittenAndZeroesTheRest)
{
	MicrophoneQueue q;
	setUp(q);

	int16_t buffer[240];
	/* Settle on a generation first: the first read of a new one drops whatever
	 * is queued, which is what GenerationDrainsStaleAudio is about. */
	q.read(buffer, 240, 1);

	const std::vector<int16_t> in = block(100, 1234);
	ASSERT_TRUE(q.write(in.data(), in.size()));

	std::memset(buffer, 0x5A, sizeof(buffer));
	const u32 got = q.read(buffer, 240, 1);

	EXPECT_EQ(got, 100u);
	for (u32 i = 0; i < got; i++)
		EXPECT_EQ(buffer[i], 1234);
	/* The caller transmits a rounded-up byte count, so everything past what was
	 * delivered has to be zero rather than whatever was on the stack. */
	for (u32 i = got; i < 240; i++)
		EXPECT_EQ(buffer[i], 0) << "at " << i;
}

/* Ten seconds of a game reading faster than the microphone fills. Latency must
 * stay bounded and the stream must not run away. */
TEST(MicrophoneQueue, BoundedLatencyOverTenSeconds)
{
	const unsigned rate = 11025;
	MicrophoneQueue q;
	setUp(q, rate);

	const size_t perFrame = rate / 60;
	const std::vector<int16_t> in = block(perFrame, 7);
	u64 delivered = 0;
	u32 worstDepth = 0;

	for (int frame = 0; frame < 600; frame++)
	{
		q.write(in.data(), in.size());

		int16_t buffer[240];
		const u32 got = q.read(buffer, 240, 1);
		ASSERT_LE(got, 240u);
		delivered += got;
		worstDepth = std::max(worstDepth, q.depthBytes());
	}

	/* A quarter of a second of audio in hand, at the very worst. */
	EXPECT_LT(worstDepth, (u32)(rate / 4) * 2u + 4096u);
	/* And the game heard essentially everything that was captured. */
	EXPECT_GT((double)delivered, 0.95 * (double)(perFrame * 600));
}

/* A restored savestate, a reset or a fresh start: what is queued belongs to
 * before it and must not be replayed into the game. */
TEST(MicrophoneQueue, GenerationDrainsStaleAudio)
{
	MicrophoneQueue q;
	setUp(q);
	const std::vector<int16_t> in = block(5000, 99);
	ASSERT_TRUE(q.write(in.data(), in.size()));
	ASSERT_GT(q.depthBytes(), 0u);

	int16_t buffer[240];
	const u32 got = q.read(buffer, 240, 2);      /* generation moved */

	EXPECT_EQ(got, 0u);
	EXPECT_EQ(q.depthBytes(), 0u);
}

/* A full queue drops the newest block rather than letting the producer read. */
TEST(MicrophoneQueue, AFullQueueRefusesRatherThanRacing)
{
	MicrophoneQueue q;
	setUp(q);
	const std::vector<int16_t> in = block(4096, 3);

	bool refusedEventually = false;
	for (int i = 0; i < 16; i++)
	{
		if (!q.write(in.data(), in.size()))
		{
			refusedEventually = true;
			break;
		}
	}
	EXPECT_TRUE(refusedEventually);
	EXPECT_LE(q.depthBytes(), 32768u);
}
