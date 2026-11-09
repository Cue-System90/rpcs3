#pragma once

// Lightweight timing probes for measuring emulation overhead in a running game.
// Enabled only when the environment variable RPCS3_PERF_PROBE is set; otherwise every probe is a single branch.
// Results are written to the log every 5 seconds (channel "PROBE").

#include "util/types.hpp"
#include "util/atomic.hpp"

#include <array>
#include <chrono>

namespace perf_probe
{
	enum id : u32
	{
		av_ppu,      // texture cache write-protection fault on a PPU thread
		av_spu,      // ... on an SPU thread
		av_rsx,      // ... on the RSX thread
		av_other,    // ... on any other thread
		av_flush,    // fault that had to flush GPU data back to memory
		mmap_map,    // sys_mmapper_map_shared_memory
		mmap_unmap,  // sys_mmapper_unmap_shared_memory
		rsx_iomap,   // sys_rsx_context_iomap
		rsx_iounmap, // sys_rsx_context_iounmap
		iolock_wait, // contended RSX IO map range lock
		flush_rsx_wait,   // faulting thread waits for the RSX thread to submit its command buffer
		flush_all,        // texture_cache::flush_all (transfer + readback)
		flush_retransfer, // flush had to record a new GPU->CPU copy
		flush_event_wait, // wait for the GPU to signal a readback
		flush_fast,       // attempt to complete a flush without a queue sync
		fast_no_tag, fast_no_sync, fast_pending, fast_flush_always, fast_excluded, // reasons the fast path was not taken
		cpu_upload_readback, // texture upload from CPU memory that first had to read back GPU-written data
		audio_underrun, // audio device wanted more samples than cellAudio had queued (audible gap); time = missing audio
		audio_skip,     // cellAudio gave up waiting for the game and dropped an audio period
		audio_partial,  // cellAudio mixed a period in which some ports were not written in time
		audio_wait,     // cellAudio waited ~1 ms for the game to write its audio
		frame,       // time between emulated flips
		count_
	};

	struct counter
	{
		atomic_t<u64> count{};
		atomic_t<u64> total_ns{};
		atomic_t<u64> max_ns{};

		void add(u64 ns) noexcept
		{
			count++;
			total_ns += ns;
			max_ns.fetch_op([ns](u64& v)
			{
				if (v >= ns)
				{
					return false;
				}

				v = ns;
				return true;
			});
		}
	};

	extern std::array<counter, count_> g_counters;
	extern atomic_t<u64> g_frames_over_20ms;
	extern atomic_t<u64> g_frames_over_34ms;

	bool enabled() noexcept;

	// A/B switch for testing: RPCS3_DISABLE_READBACK_FIX turns the readback latency changes off
	bool readback_fix_disabled() noexcept;

	// RPCS3_PROBE_AUDIO_CLOCK: the Null audio backend consumes audio in real time (silent stand-in for a device)
	bool null_audio_clock() noexcept;

	inline u64 now_ns() noexcept
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	inline void add(id i, u64 ns) noexcept
	{
		g_counters[i].add(ns);
	}

	inline void count(id i) noexcept
	{
		if (enabled())
		{
			g_counters[i].add(0);
		}
	}

	// Logs the caller's stack for the first few calls with this tag (diagnostics only)
	void log_backtrace_once(const char* tag, atomic_t<u32>& counter, u32 limit = 3) noexcept;

	// Called on every emulated flip: records the frame time and prints a report every 5 seconds
	void on_emu_flip() noexcept;

	struct scoped
	{
		id m_id;
		u64 m_start = 0;

		explicit scoped(id i) noexcept
			: m_id(i)
		{
			if (enabled())
			{
				m_start = now_ns();
			}
		}

		~scoped()
		{
			if (m_start)
			{
				add(m_id, now_ns() - m_start);
			}
		}
	};
}
