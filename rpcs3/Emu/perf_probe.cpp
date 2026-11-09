#include "stdafx.h"
#include "perf_probe.h"

#include "Utilities/stack_trace.h"
#include "Emu/CPU/CPUThread.h"

#include <cstdlib>

LOG_CHANNEL(probe_log, "PROBE");

namespace perf_probe
{
	std::array<counter, count_> g_counters{};
	atomic_t<u64> g_frames_over_20ms{};
	atomic_t<u64> g_frames_over_34ms{};

	static const bool s_enabled = std::getenv("RPCS3_PERF_PROBE") != nullptr;

	static atomic_t<u64> s_last_flip_ns{};
	static atomic_t<u64> s_last_report_ns{};

	bool enabled() noexcept
	{
		return s_enabled;
	}

	static const bool s_readback_fix_disabled = std::getenv("RPCS3_DISABLE_READBACK_FIX") != nullptr;

	bool readback_fix_disabled() noexcept
	{
		return s_readback_fix_disabled;
	}

	static const bool s_null_audio_clock = std::getenv("RPCS3_PROBE_AUDIO_CLOCK") != nullptr;

	bool null_audio_clock() noexcept
	{
		return s_null_audio_clock;
	}

	static const u32 s_rt_probe_addr = []() -> u32
	{
		const char* v = std::getenv("RPCS3_PROBE_RT_ADDR");
		return v ? static_cast<u32>(std::strtoul(v, nullptr, 16)) : 0;
	}();

	u32 rt_probe_addr() noexcept
	{
		return s_enabled ? s_rt_probe_addr : 0;
	}

	static constexpr const char* s_names[count_] =
	{
		"av_ppu", "av_spu", "av_rsx", "av_other", "av_flush",
		"mmap_map", "mmap_unmap", "rsx_iomap", "rsx_iounmap", "iolock_wait",
		"flush_rsx_wait", "flush_all", "flush_retransfer", "flush_event_wait", "flush_fast", "fast_no_tag", "fast_no_sync", "fast_pending", "fast_flush_always", "fast_excluded", "cpu_upload_readback", "cpu_upload_skip", "audio_underrun", "audio_skip", "audio_partial", "audio_wait", "frame",
	};

	static void report(u64 elapsed_ns) noexcept
	{
		std::string out = fmt::format("%.1fs:", elapsed_ns / 1e9);

		for (u32 i = 0; i < count_; i++)
		{
			auto& c = g_counters[i];
			const u64 n = c.count.exchange(0);
			const u64 total = c.total_ns.exchange(0);
			const u64 max = c.max_ns.exchange(0);

			if (!n)
			{
				continue;
			}

			fmt::append(out, " | %s n=%u total=%.1fms avg=%.1fus max=%.1fus", s_names[i], n, total / 1e6, total / 1e3 / n, max / 1e3);
		}

		fmt::append(out, " | frames>20ms=%u >34ms=%u", g_frames_over_20ms.exchange(0), g_frames_over_34ms.exchange(0));
		probe_log.notice("%s", out);
	}

	void log_backtrace_once(const char* tag, atomic_t<u32>& counter, u32 limit) noexcept
	{
		if (!s_enabled || counter++ >= limit)
		{
			return;
		}

		const auto cpu = get_current_cpu_thread();
		std::string out = fmt::format("backtrace '%s' on %s:", tag, cpu ? cpu->get_name() : thread_ctrl::get_name());

		for (const auto& line : utils::get_backtrace_symbols(utils::get_backtrace(24)))
		{
			fmt::append(out, "\n    %s", line);
		}

		probe_log.notice("%s", out);
	}

	void on_emu_flip() noexcept
	{
		if (!s_enabled)
		{
			return;
		}

		const u64 now = now_ns();
		const u64 last = s_last_flip_ns.exchange(now);

		if (last)
		{
			const u64 dt = now - last;
			add(frame, dt);

			if (dt > 20'000'000)
			{
				g_frames_over_20ms++;
			}

			if (dt > 34'000'000)
			{
				g_frames_over_34ms++;
			}
		}

		const u64 last_report = s_last_report_ns;

		if (!last_report)
		{
			s_last_report_ns = now;
		}
		else if (now - last_report >= 5'000'000'000 && s_last_report_ns.compare_and_swap_test(last_report, now))
		{
			report(now - last_report);
		}
	}
}
