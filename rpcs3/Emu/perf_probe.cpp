#include "stdafx.h"
#include "perf_probe.h"

#include "Utilities/stack_trace.h"
#include "Emu/CPU/CPUThread.h"
#include "Emu/Io/pad_types.h"

#include <cstdlib>
#include <cstring>
#include <algorithm>

LOG_CHANNEL(probe_log, "PROBE");

extern atomic_t<bool> g_user_asked_for_screenshot;

namespace perf_probe
{
	std::array<counter, count_> g_counters{};
	atomic_t<u64> g_frames_over_20ms{};
	atomic_t<u64> g_frames_over_34ms{};
	atomic_t<u64> g_flip_count{};
	atomic_t<u64> g_last_submit_ns{};

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

	static const bool s_async_shader_compile_disabled = std::getenv("RPCS3_DISABLE_ASYNC_SHADER_COMPILE") != nullptr;

	bool async_shader_compile_disabled() noexcept
	{
		return s_async_shader_compile_disabled;
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

	// RPCS3_PROBE_SCREENSHOT_AT=<flip>[,<flip>...]: take a screenshot at these emulated flips (counted from boot or savestate load).
	// Screenshots are named by the second, so keep them at least 60 flips apart.
	static const std::vector<u64> s_screenshot_flips = []()
	{
		std::vector<u64> flips;

		if (const char* v = std::getenv("RPCS3_PROBE_SCREENSHOT_AT"))
		{
			for (char* end = nullptr; *v; v = *end ? end + 1 : end)
			{
				flips.push_back(std::strtoull(v, &end, 10));

				if (end == v)
				{
					break;
				}
			}
		}

		return flips;
	}();

	static constexpr const char* s_names[count_] =
	{
		"av_ppu", "av_spu", "av_rsx", "av_other", "av_flush",
		"mmap_map", "mmap_unmap", "rsx_iomap", "rsx_iounmap", "iolock_wait",
		"flush_rsx_wait", "flush_all", "flush_retransfer", "flush_event_wait", "flush_fast", "fast_no_tag", "fast_no_sync", "fast_pending", "fast_flush_always", "fast_excluded", "cpu_upload_readback", "cpu_upload_skip", "audio_underrun", "audio_skip", "audio_partial", "audio_wait", "shader_decompile", "shader_compile", "frame",
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

	struct scripted_press
	{
		u64 flip;
		u32 offset;
		u32 keycode;
	};

	static const std::vector<scripted_press> s_scripted_presses = []()
	{
		std::vector<scripted_press> presses;
		const char* v = std::getenv("RPCS3_PROBE_PAD");

		if (!v)
		{
			return presses;
		}

		static constexpr std::pair<const char*, std::pair<u32, u32>> names[] =
		{
			{ "cross",    { CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_CROSS } },
			{ "circle",   { CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_CIRCLE } },
			{ "square",   { CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_SQUARE } },
			{ "triangle", { CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_TRIANGLE } },
			{ "l1",       { CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_L1 } },
			{ "r1",       { CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_R1 } },
			{ "start",    { CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_START } },
			{ "select",   { CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_SELECT } },
			{ "up",       { CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_UP } },
			{ "down",     { CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_DOWN } },
			{ "left",     { CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_LEFT } },
			{ "right",    { CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_RIGHT } },
		};

		for (const char* p = v; *p;)
		{
			char* end = nullptr;
			const u64 flip = std::strtoull(p, &end, 10);

			if (end == p || *end != ':')
			{
				break;
			}

			const char* name = end + 1;
			const char* name_end = std::strchr(name, ',');
			const usz len = name_end ? name_end - name : std::strlen(name);

			for (const auto& [n, codes] : names)
			{
				if (std::strlen(n) == len && std::strncmp(n, name, len) == 0)
				{
					presses.push_back({ flip, codes.first, codes.second });
				}
			}

			if (!name_end)
			{
				break;
			}

			p = name_end + 1;
		}

		return presses;
	}();

	void apply_scripted_pad(Pad& pad) noexcept
	{
		if (!s_enabled || s_scripted_presses.empty())
		{
			return;
		}

		if (!pad.is_connected())
		{
			pad.m_port_status |= CELL_PAD_STATUS_CONNECTED | CELL_PAD_STATUS_ASSIGN_CHANGES;
			probe_log.notice("Scripted pad: connected player 1");
		}

		const u64 flip = g_flip_count;

		for (const auto& press : s_scripted_presses)
		{
			const bool down = flip >= press.flip && flip < press.flip + 12;

			static u64 s_last_logged = umax;

			if (flip == press.flip && s_last_logged != flip)
			{
				s_last_logged = flip;
				probe_log.notice("Scripted pad: press (offset %u, code 0x%x) at flip %u, handler %s, %u buttons", press.offset, press.keycode, flip, pad.m_pad_handler, pad.m_buttons.size());
			}

			auto it = std::find_if(pad.m_buttons.begin(), pad.m_buttons.end(), [&](const Button& b) { return b.m_offset == press.offset && b.m_outKeyCode == press.keycode; });

			if (it == pad.m_buttons.end())
			{
				if (!down)
				{
					continue;
				}

				pad.m_buttons.emplace_back(press.offset, std::vector<std::set<u32>>{}, press.keycode);
				it = std::prev(pad.m_buttons.end());
			}

			if (down || it->m_pressed)
			{
				it->m_pressed = down;
				it->m_value = down ? 255 : 0;
			}
		}
	}

	void on_emu_flip() noexcept
	{
		if (!s_enabled)
		{
			return;
		}

		const u64 now = now_ns();
		const u64 last = s_last_flip_ns.exchange(now);
		const u64 flip = ++g_flip_count;

		if (std::find(s_screenshot_flips.begin(), s_screenshot_flips.end(), flip) != s_screenshot_flips.end())
		{
			probe_log.notice("Screenshot requested at flip %u", flip);
			g_user_asked_for_screenshot = true;
		}

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
