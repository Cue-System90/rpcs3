#pragma once

#include "Emu/Audio/AudioBackend.h"
#include "Emu/perf_probe.h"

#include <chrono>
#include <thread>
#include <vector>

class NullAudioBackend final : public AudioBackend
{
public:
	NullAudioBackend() {}
	~NullAudioBackend() { Close(); }

	std::string_view GetName() const override { return "Null"sv; }

	bool Open(std::string_view /* dev_id */, AudioFreq /* freq */, AudioSampleSize /* sample_size */, AudioChannelCnt /* ch_cnt */, audio_channel_layout /*layout*/) override
	{
		Close();

		if (perf_probe::null_audio_clock())
		{
			// cellAudio buffering mixes into the backend layout, so it has to be a real one
			m_channels = 2;
			m_layout = audio_channel_layout::stereo;
		}

		return true;
	}
	void Close() override { Pause(); }

	f64 GetCallbackFrameLen() override { return 0.01; };

	void Play() override
	{
		if (m_playing)
		{
			return;
		}

		m_playing = true;

		// For measurements (RPCS3_PROBE_AUDIO_CLOCK): consume the audio in real time like a device would,
		// so cellAudio buffering and underruns behave as with a real backend, but nothing is heard
		if (perf_probe::null_audio_clock())
		{
			m_stop = false;
			m_clock = std::thread([this]()
			{
				const u32 frame_bytes = get_sample_size() * get_channels();
				const u32 frames = get_sampling_rate() / 100;
				std::vector<u8> sink(frame_bytes * frames);
				auto next = std::chrono::steady_clock::now();

				while (!m_stop)
				{
					next += std::chrono::milliseconds(10);
					std::this_thread::sleep_until(next);

					std::lock_guard lock(m_cb_mutex);

					if (m_write_callback)
					{
						m_write_callback(static_cast<u32>(sink.size()), sink.data());
					}
				}
			});
		}
	}

	void Pause() override
	{
		m_playing = false;
		m_stop = true;

		if (m_clock.joinable())
		{
			m_clock.join();
		}
	}

	bool IsPlaying() override { return m_playing; }

private:
	bool m_playing = false;
	atomic_t<bool> m_stop = true;
	std::thread m_clock;
};
