/*
	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	Flycast is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */

/* The Dreamcast microphone (HKT-7200), wired to the frontend's.
 *
 * Two threads meet here and each is allowed exactly one thing:
 *
 *   the retro_run thread   owns every libretro microphone call, and is the
 *                          queue's only producer
 *   the emulation thread   calls StartAudioRecording / RecordAudio /
 *                          StopAudioRecording from inside maple DMA, and is
 *                          the queue's only consumer
 *
 * So the emulation side never calls the frontend, never takes a lock and never
 * waits: it moves atomics and drains a queue. A game that asks for samples
 * before any have arrived is told none are ready, which is what the protocol's
 * count byte is for -- 240 samples at 11025 Hz is 21.8 ms, longer than a frame,
 * so a full read is impossible by construction rather than by accident.
 */

#include "types.h"
#include "audio/audiostream.h"

#include "MicrophoneInterface.h"
#include "MicrophoneQueue.h"
#include "MicrophoneResampler.h"
#include "microphone.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <vector>

extern retro_environment_t environ_cb;

/* What the frontend is asked for. Not the device's rate: see
 * MicrophoneResampler.h for why the decimation happens here. */
static constexpr unsigned HOST_RATE = 48000;

/* Owned by the retro_run thread. */
static retro_microphone_interface iface;
static bool iface_ok;
static retro_microphone_t *handle;
static unsigned host_rate;
static unsigned open_rate;
static MicDecimator decimator;
static std::vector<int16_t> host_scratch;
static std::vector<int16_t> out_scratch;
static std::chrono::steady_clock::time_point last_pump;
static std::chrono::steady_clock::time_point next_open_try;
static bool open_failed_logged;
static int64_t silent_samples;
static bool silence_logged;

/* Shared. Atomics only. */
static std::atomic<int> mic_users { 0 };
static std::atomic<unsigned> want_rate { 11025 };
static std::atomic<uint32_t> generation { 0 };
static std::atomic<bool> stream_live { false };

/* Written by the retro_run thread, drained by the emulation thread. */
static MicrophoneQueue queue;

static void closeHandle()
{
	stream_live.store(false, std::memory_order_release);
	if (handle == nullptr)
		return;
	if (iface.set_mic_state != nullptr)
		iface.set_mic_state(handle, false);
	if (iface.close_mic != nullptr)
		iface.close_mic(handle);
	handle = nullptr;
}

static void openHandle(unsigned deviceRate)
{
	retro_microphone_params_t params;
	params.rate = HOST_RATE;

	handle = iface.open_mic(&params);
	if (handle == nullptr)
	{
		if (!open_failed_logged)
		{
			WARN_LOG(AUDIO, "microphone: the frontend would not open one");
			open_failed_logged = true;
		}
		next_open_try = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		return;
	}

	host_rate = HOST_RATE;
	retro_microphone_params_t actual;
	actual.rate = 0;
	if (iface.get_params != nullptr && iface.get_params(handle, &actual) && actual.rate != 0)
		host_rate = actual.rate;

	if (iface.set_mic_state == nullptr || !iface.set_mic_state(handle, true))
	{
		WARN_LOG(AUDIO, "microphone: the frontend would not switch it on");
		closeHandle();
		next_open_try = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		return;
	}

	open_rate = deviceRate;
	decimator.configure(host_rate, deviceRate);
	host_scratch.assign(host_rate / 5, 0);                       /* 200 ms */
	out_scratch.assign(decimator.maxOutput(host_scratch.size()), 0);

	queue.setDeviceRate(deviceRate);

	silent_samples = 0;
	silence_logged = false;
	open_failed_logged = false;
	stream_live.store(true, std::memory_order_release);
	INFO_LOG(AUDIO, "microphone: open, %u Hz from the frontend -> %u Hz for the game, %d taps",
			host_rate, deviceRate, (int)decimator.tapCount());
}

void mic_probe()
{
	std::memset(&iface, 0, sizeof(iface));
	iface.interface_version = RETRO_MICROPHONE_INTERFACE_VERSION;

	/* The version has to be set going in: a frontend refuses a zero. */
	iface_ok = environ_cb(RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE, &iface)
			&& iface.interface_version == RETRO_MICROPHONE_INTERFACE_VERSION
			&& iface.open_mic != nullptr
			&& iface.read_mic != nullptr;

	if (!iface_ok)
	{
		std::memset(&iface, 0, sizeof(iface));
		INFO_LOG(AUDIO, "microphone: this frontend offers none; the device will hear silence");
		return;
	}

	/* Once, and never again: resizing this while the emulation thread is
	 * inside read() would be a use-after-free. */
	queue.setCapacity(32768);
	handle = nullptr;
	stream_live.store(false, std::memory_order_release);
	INFO_LOG(AUDIO, "microphone: the frontend offers one");
}

void mic_pump()
{
	if (!iface_ok)
		return;

	const auto now = std::chrono::steady_clock::now();
	const bool want = mic_users.load(std::memory_order_acquire) > 0;
	const unsigned rate = want_rate.load(std::memory_order_relaxed);

	if (handle != nullptr && (!want || rate != open_rate))
		closeHandle();

	if (handle == nullptr)
	{
		if (!want || now < next_open_try)
			return;
		openHandle(rate);
		last_pump = now;
		return;
	}

	/* Pull at wall-clock rate. The clamp covers the frames retro_run returns
	 * early from, and the pauses either side of a savestate. */
	double dt = std::chrono::duration<double>(now - last_pump).count();
	last_pump = now;
	if (dt < 0.0)
		dt = 0.0;
	else if (dt > 0.1)
		dt = 0.1;

	size_t n = (size_t)((double)host_rate * dt + 0.5);
	if (n == 0)
		return;
	if (n > host_scratch.size())
		n = host_scratch.size();

	const int got = iface.read_mic(handle, host_scratch.data(), n);
	if (got < 0)
	{
		WARN_LOG(AUDIO, "microphone: the frontend dropped it");
		closeHandle();
		next_open_try = now + std::chrono::seconds(5);
		return;
	}
	if (got == 0)
		return;

	/* A live microphone always has a noise floor. Digital zero for seconds on
	 * end means nothing is reaching it -- most often a permission the player
	 * never granted, which no return value here can distinguish from a quiet
	 * room. Say it once; never act on it. */
	bool allZero = true;
	for (int i = 0; i < got; i++)
	{
		if (host_scratch[(size_t)i] != 0)
		{
			allZero = false;
			break;
		}
	}
	silent_samples = allZero ? silent_samples + got : 0;
	if (!silence_logged && silent_samples > (int64_t)host_rate * 3)
	{
		WARN_LOG(AUDIO, "microphone: nothing at all is reaching it -- check the microphone permission");
		silence_logged = true;
	}

	const size_t m = decimator.process(host_scratch.data(), (size_t)got, out_scratch.data());
	if (m == 0)
		return;

	queue.write(out_scratch.data(), m);
}

void mic_flush()
{
	/* The consumer drains on its next call rather than the producer touching
	 * the read cursor. */
	generation.fetch_add(1, std::memory_order_release);
}

void mic_shutdown()
{
	mic_users.store(0, std::memory_order_release);
	generation.fetch_add(1, std::memory_order_release);
	closeHandle();
}

/* ---- Called on the emulation thread, from inside maple DMA ---- */

void StartAudioRecording(bool eight_khz)
{
	want_rate.store(eight_khz ? 8000u : 11025u, std::memory_order_relaxed);
	mic_users.fetch_add(1, std::memory_order_acq_rel);
	generation.fetch_add(1, std::memory_order_release);
}

void StopAudioRecording()
{
	int n = mic_users.load(std::memory_order_acquire);
	while (n > 0 && !mic_users.compare_exchange_weak(n, n - 1, std::memory_order_acq_rel))
		;
	generation.fetch_add(1, std::memory_order_release);
}

u32 RecordAudio(void *buffer, u32 samples)
{
	if (buffer == nullptr || samples == 0)
		return 0;

	if (!stream_live.load(std::memory_order_acquire))
		return 0;

	return queue.read(buffer, samples, generation.load(std::memory_order_acquire));
}
