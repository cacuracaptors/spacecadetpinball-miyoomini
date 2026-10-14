// miyoo_state.cpp -- save states, OnionOS's Game Switcher and the save at power-off,
// for the Miyoo Mini (Plus) port. Modelled on the Mario Kart 64 port's savestate_miyoo.c.
//
// What a state holds: the table and everything the game builds for it at run time.
//  - The heap: from pb::init() on, the game's C++ memory ("new") comes from a heap of its
//    own, a TLSF pool at a fixed address (kHeapBase), so a state can be put back at the same
//    addresses. Settings, High Scores and the buttons keep using the normal heap
//    (miyoo_state::NormalHeap in options.cpp, high_score.cpp, winmain.cpp): a state never
//    rolls them back. SDL, SDL_mixer and ImGui are C (malloc) and stay outside too.
//  - The static variables of the table's code: miyoo_state.ld gathers the .data and .bss of
//    those object files between __pb_state_start and __pb_state_end.
//  - Kept from the running game across a load: the SDL objects the table points to (the
//    screen texture, the sound effects), the data file's paths and the screen layout.
//  - In the file's header: what winmain and midi keep for the table (pause, demo, the
//    music track and its position).
// The executable is not position independent, so the code and the static variables sit at
// the same addresses in every run of the same build; a state records a checksum of the
// code and is refused by any other build. A state is only saved and loaded between two
// frames (Poll, from the main loop), and loaded once, right after the table is set up.
//
// The MENU key, as OnionOS's RetroArch: a press, a long press (0.7 s) and a double press
// (within 0.3 s) do what Tweaks > Shortcuts > "In-game: Menu button" sets
// (config/keymap.json; by default the Game Switcher, back to the menu, the previous game),
// the state being saved first. MENU with another key (Menu+Start: demo, Menu+Power:
// screenshot...) does none of them. Outside OnionOS a MENU press closes the game.
//
// Opened from the Game Switcher (OnionOS leaves "switcher" in /tmp/prev_state) the game
// goes on from saves/state_auto.pbs; opened from the game list it starts a new game.
//
// Power-off (OnionOS creates /tmp/.offOrder and sends SIGTERM; the signal does not always
// arrive, so the file is checked 4 times a second): the state is saved, saves/resume_boot
// is left and cmd_to_run.sh (which OnionOS removes) is put back, so OnionOS opens the game
// again at the next start and the game goes on from where it was. A script in
// .tmp_update/checkoff makes OnionOS wait for the save (20 s at most).
// saves/state_loading guards against a state that crashes the game: while it exists the
// next start does not resume.
//
// The Game Switcher's picture: /mnt/SDCARD/Saves/CurrentProfile/romScreens/<hash>.png,
// the hash being OnionOS's FNV1A_Pippip_Yurii of the game's path in the recent list (the
// .port shortcut), as screenshot_system() writes it for RetroArch.

#include "pch.h"
#include "miyoo_state.h"

#if defined(__linux__)

#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <csignal>
#include <new>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/input.h>

#include "tlsf.h"
#include "high_score.h"
#include "loader.h"
#include "midi.h"
#include "options.h"
#include "pb.h"
#include "render.h"
#include "winmain.h"

extern "C" char __pb_state_start[], __pb_state_end[];
extern "C" char __executable_start[], etext[];

namespace
{
	// ---- the table's heap ----------------------------------------------------------------

	// (between the program's own heap, low, and the libraries, high, in every run)
	const uintptr_t kHeapAddress = 0x60000000u;
#define kHeapBase (reinterpret_cast<char*>(kHeapAddress))
	const size_t kHeapSize = 64u << 20;
	const size_t kPage = 4096;
	const size_t kAlign = alignof(std::max_align_t);

	tlsf_t sTlsf;
	volatile bool sHeapOn;
	uintptr_t sHeapEnd;     // the end of the highest block ever handed out
	pthread_mutex_t sHeapLock = PTHREAD_MUTEX_INITIALIZER;
	bool sHeapFull;

	bool in_heap(const void* p)
	{
		return sTlsf && p >= kHeapBase && p < kHeapBase + kHeapSize;
	}

	void* heap_alloc(size_t n)
	{
		if (!n)
			n = 1;
		if (sHeapOn && sTlsf && !sHeapFull)
		{
			pthread_mutex_lock(&sHeapLock);
			auto p = tlsf_memalign(sTlsf, kAlign, n);
			if (p)
			{
				auto end = reinterpret_cast<uintptr_t>(p) + tlsf_block_size(p);
				if (end > sHeapEnd)
					sHeapEnd = end;
			}
			else
			{
				sHeapFull = true; // (from here on states are off: see save_state)
			}
			pthread_mutex_unlock(&sHeapLock);
			if (p)
				return p;
		}
		return malloc(n);
	}

	void heap_free(void* p)
	{
		if (!p)
			return;
		if (in_heap(p))
		{
			pthread_mutex_lock(&sHeapLock);
			tlsf_free(sTlsf, p);
			pthread_mutex_unlock(&sHeapLock);
		}
		else
		{
			free(p);
		}
	}

	void* heap_new(size_t n)
	{
		for (;;)
		{
			auto p = heap_alloc(n);
			if (p)
				return p;
			auto handler = std::set_new_handler(nullptr);
			std::set_new_handler(handler);
			if (!handler)
				throw std::bad_alloc();
			handler();
		}
	}

	void heap_init()
	{
		auto p = mmap(kHeapBase, kHeapSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
		if (p == MAP_FAILED)
			return;
		if (p != kHeapBase)
		{
			munmap(p, kHeapSize);
			return;
		}
		sTlsf = tlsf_create_with_pool(kHeapBase, kHeapSize);
		sHeapEnd = reinterpret_cast<uintptr_t>(kHeapBase) + tlsf_size() + tlsf_pool_overhead();
	}

	// ---- files and paths ------------------------------------------------------------------

	const char* const kSaveDir = "saves";
	const char* const kAutoState = "saves/state_auto.pbs";
	const char* const kBootResume = "saves/resume_boot";
	const char* const kLoadingFlag = "saves/state_loading";
	const char* const kLogFile = "state_log.txt";
	const char* const kOnionDir = "/mnt/SDCARD/.tmp_update";
	const char* const kCmdToRun = "/mnt/SDCARD/.tmp_update/cmd_to_run.sh";
	const char* const kRunSwitcher = "/mnt/SDCARD/.tmp_update/.runGameSwitcher";
	const char* const kCheckoffDir = "/mnt/SDCARD/.tmp_update/checkoff";
	const char* const kCheckoffScript = "/mnt/SDCARD/.tmp_update/checkoff/pinball_save.sh";
	const char* const kRomscreenDir = "/mnt/SDCARD/Saves/CurrentProfile/romScreens";
	const char* const kRecentList = "/mnt/SDCARD/Roms/recentlist.json";
	const char* const kRecentHidden = "/mnt/SDCARD/Roms/recentlist-hidden.json";
	const char* const kKeymap = "/mnt/SDCARD/.tmp_update/config/keymap.json";

	FILE* sLog;

	void slog(const char* fmt, ...)
	{
		if (!sLog)
			return;
		va_list ap;
		va_start(ap, fmt);
		vfprintf(sLog, fmt, ap);
		va_end(ap);
		fflush(sLog);
	}

	double now_ms()
	{
		timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
	}

	bool exists(const char* path)
	{
		return access(path, F_OK) == 0;
	}

	void fsync_path(const char* path)
	{
		auto fd = open(path, O_RDONLY);
		if (fd >= 0)
		{
			fsync(fd);
			close(fd);
		}
	}

	// the whole file at once: beside the old one, then renamed over it
	bool write_file(const char* path, const void* data, size_t len)
	{
		char tmp[300];
		snprintf(tmp, sizeof tmp, "%s.tmp", path);
		auto fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fd < 0)
			return false;
		auto p = static_cast<const uint8_t*>(data);
		auto left = len;
		while (left)
		{
			auto w = write(fd, p, left);
			if (w <= 0)
			{
				close(fd);
				remove(tmp);
				return false;
			}
			p += w;
			left -= static_cast<size_t>(w);
		}
		fsync(fd);
		close(fd);
		if (rename(tmp, path) != 0)
		{
			remove(tmp);
			return false;
		}
		fsync_path(path);
		return true;
	}

	void touch(const char* path)
	{
		write_file(path, "", 0);
	}

	// ---- zlib, from the device at run time ------------------------------------------------
	// OnionOS ships it (/mnt/SDCARD/miyoo/lib/libz.so.1); without it the files are written
	// uncompressed. Only the stable part of zlib's interface is used (the z_stream layout,
	// deflateInit_ / inflateInit_, unchanged since zlib 1.0), so no zlib header is needed.

	struct ZStream
	{
		const unsigned char* next_in;
		unsigned avail_in;
		unsigned long total_in;
		unsigned char* next_out;
		unsigned avail_out;
		unsigned long total_out;
		const char* msg;
		void* state;
		void *zalloc, *zfree, *opaque;
		int data_type;
		unsigned long adler;
		unsigned long reserved;
	};

	const int Z_OK_ = 0, Z_STREAM_END_ = 1, Z_NO_FLUSH_ = 0, Z_FINISH_ = 4;

	int (*p_deflateInit_)(ZStream*, int, const char*, int);
	int (*p_deflate)(ZStream*, int);
	int (*p_deflateEnd)(ZStream*);
	int (*p_inflateInit_)(ZStream*, const char*, int);
	int (*p_inflate)(ZStream*, int);
	int (*p_inflateEnd)(ZStream*);
	int sZlib; // 0 not tried, 1 loaded, -1 missing

	bool zlib_available()
	{
		if (sZlib)
			return sZlib > 0;
		sZlib = -1;
		static const char* names[] = {
			"libz.so.1", "/mnt/SDCARD/miyoo/lib/libz.so.1", "/mnt/SDCARD/.tmp_update/lib/parasyte/libz.so.1",
			"/customer/lib/libz.so.1", "/lib/libz.so.1", "/usr/lib/libz.so.1", "libz.so",
		};
		void* h = nullptr;
		const char* used = nullptr;
		for (auto name : names)
		{
			h = SDL_LoadObject(name);
			used = name;
			if (h)
				break;
		}
		if (!h)
		{
			slog("state: zlib not found (files are not compressed)\n");
			return false;
		}
		p_deflateInit_ = reinterpret_cast<int (*)(ZStream*, int, const char*, int)>(SDL_LoadFunction(h, "deflateInit_"));
		p_deflate = reinterpret_cast<int (*)(ZStream*, int)>(SDL_LoadFunction(h, "deflate"));
		p_deflateEnd = reinterpret_cast<int (*)(ZStream*)>(SDL_LoadFunction(h, "deflateEnd"));
		p_inflateInit_ = reinterpret_cast<int (*)(ZStream*, const char*, int)>(SDL_LoadFunction(h, "inflateInit_"));
		p_inflate = reinterpret_cast<int (*)(ZStream*, int)>(SDL_LoadFunction(h, "inflate"));
		p_inflateEnd = reinterpret_cast<int (*)(ZStream*)>(SDL_LoadFunction(h, "inflateEnd"));
		auto ver = reinterpret_cast<const char* (*)()>(SDL_LoadFunction(h, "zlibVersion"));
		if (!p_deflateInit_ || !p_deflate || !p_deflateEnd || !p_inflateInit_ || !p_inflate || !p_inflateEnd || !ver ||
			ver()[0] != '1')
		{
			slog("state: incomplete zlib in %s (files are not compressed)\n", used);
			return false;
		}
		slog("state: zlib %s (%s)\n", ver(), used);
		sZlib = 1;
		return true;
	}

	bool z_deflate_init(ZStream* s, int level)
	{
		memset(s, 0, sizeof *s);
		return p_deflateInit_(s, level, "1.2.11", static_cast<int>(sizeof *s)) == Z_OK_;
	}

	bool z_inflate_init(ZStream* s)
	{
		memset(s, 0, sizeof *s);
		return p_inflateInit_(s, "1.2.11", static_cast<int>(sizeof *s)) == Z_OK_;
	}

	// ---- checksums ------------------------------------------------------------------------

	uint32_t hash_words(const void* start, const void* end)
	{
		auto p = static_cast<const uint32_t*>(start);
		auto e = reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(end) & ~static_cast<uintptr_t>(3));
		uint32_t h = 2166136261u;
		for (; p < e; p++)
			h = (h ^ *p) * 16777619u;
		return h;
	}

	uint32_t code_hash()
	{
		static uint32_t h;
		if (!h)
		{
			h = hash_words(__executable_start, etext);
			if (!h)
				h = 1;
		}
		return h;
	}

	// OnionOS's FNV1A_Pippip_Yurii (src/common/utils/hash.h), reading through memcpy
	uint32_t onion_hash(const char* str, size_t len)
	{
		const uint32_t prime = 591798841u;
		uint64_t h = 14695981039346656037ULL;
		if (len > 8)
		{
			size_t cycles = ((len - 1) >> 4) + 1;
			const size_t ndhead = len - (cycles << 3);
			for (; cycles--; str += 8)
			{
				uint64_t a, b;
				memcpy(&a, str, 8);
				memcpy(&b, str + ndhead, 8);
				h = (h ^ a) * prime;
				h = (h ^ b) * prime;
			}
		}
		else
		{
			uint64_t a = 0;
			memcpy(&a, str, len);
			h = (h ^ a) * prime;
		}
		const auto h32 = static_cast<uint32_t>(h ^ (h >> 32));
		return h32 ^ (h32 >> 16);
	}

	// ---- OnionOS state ----------------------------------------------------------------------

	enum ExitKind { ExitQuit, ExitSwitcher, ExitPowerOff, ExitQuickSwitch };
	// OnionOS's in-game MENU actions (keymon's activate_RA_action)
	enum { MenuOff, MenuSwitcher, MenuExit, MenuQuickSwitch, MenuRaMenu };
	enum { PressSingle, PressLong, PressDouble };

	bool sOnion;
	bool sDisabled;
	char sDisabledWhy[96];
	volatile sig_atomic_t sOffSignal;
	bool sOffAtStart;
	double sNextOffPoll;
	char sCmd[4096];
	int sCmdLen = -1;
	bool sCheckoff;
	char sRomscreen[200];
	int sMenuAction[3] = {MenuSwitcher, MenuExit, MenuQuickSwitch}; // single, long, double
	bool sResumePending;
	double sLoadedAt;
	bool sQuitting;

	void put_back_cmd()
	{
		if (sCmdLen > 0 && !exists(kCmdToRun))
		{
			auto ok = write_file(kCmdToRun, sCmd, static_cast<size_t>(sCmdLen));
			slog("state: cmd_to_run.sh put back (%s)\n", ok ? "ok" : "FAILED");
		}
	}

	void make_checkoff_script()
	{
		char exe[256] = "SpaceCadetPinball";
		char path[512];
		auto n = readlink("/proc/self/exe", path, sizeof path - 1);
		if (n > 0)
		{
			path[n] = 0;
			auto slash = strrchr(path, '/');
			snprintf(exe, sizeof exe, "%s", slash ? slash + 1 : path);
		}
		mkdir(kCheckoffDir, 0777);
		char s[600];
		auto len = snprintf(s, sizeof s,
		                    "#!/bin/sh\n"
		                    "# 3D Pinball: OnionOS waits here (20 s at most) while the game saves at power-off\n"
		                    "i=0\n"
		                    "while grep -q '%s' /proc/%d/cmdline 2>/dev/null && [ $i -lt 200 ]; do sleep 0.1; i=$((i+1)); done\n",
		                    exe, static_cast<int>(getpid()));
		sCheckoff = write_file(kCheckoffScript, s, static_cast<size_t>(len));
	}

	void release_checkoff()
	{
		if (sCheckoff)
		{
			unlink(kCheckoffScript);
			sCheckoff = false;
		}
	}

	// the game's line in OnionOS's recent list: the path the Game Switcher hashes
	void find_romscreen()
	{
		char cwd[256];
		if (!getcwd(cwd, sizeof cwd))
			return;
		auto dir = strrchr(cwd, '/');
		dir = dir ? dir + 1 : cwd; // the game's folder: Pinball
		auto f = fopen(exists(kRecentHidden) ? kRecentHidden : kRecentList, "r");
		if (!f)
			return;
		static char line[8192];
		char path[512];
		while (fgets(line, sizeof line, f))
		{
			auto t = strstr(line, "\"type\":");
			auto r = strstr(line, "\"rompath\":\"");
			if (!t || !r)
				continue;
			auto type = atoi(t + 7);
			if (type != 5 && type != 17)
				continue;
			r += 11;
			auto e = strchr(r, '"');
			if (!e || e - r >= static_cast<int>(sizeof path))
				continue;
			memcpy(path, r, static_cast<size_t>(e - r));
			path[e - r] = 0;
			auto colon = strchr(path, ':'); // "launch:rompath" (the search panel)
			const char* rom = colon ? colon + 1 : path;
			// the .port shortcut names the folder (GameDir="Pinball")
			bool ours = false;
			auto s = fopen(rom, "r");
			if (s)
			{
				char buf[4096];
				auto n = fread(buf, 1, sizeof buf - 1, s);
				buf[n] = 0;
				fclose(s);
				char want[300];
				snprintf(want, sizeof want, "GameDir=\"%s\"", dir);
				ours = strstr(buf, want) != nullptr;
			}
			if (ours)
			{
				snprintf(sRomscreen, sizeof sRomscreen, "%s/%u.png", kRomscreenDir,
				         static_cast<unsigned>(onion_hash(rom, strlen(rom))));
				slog("state: in the recent list as %s -> %s\n", rom, sRomscreen);
				break;
			}
		}
		fclose(f);
	}

	// OnionOS's settings for MENU in a game (keymon: _settings_load_keymap and the old flags)
	int json_int(const char* json, const char* key, int fallback)
	{
		char k[64];
		snprintf(k, sizeof k, "\"%s\"", key);
		auto p = strstr(json, k);
		if (!p || !(p = strchr(p + strlen(k), ':')))
			return fallback;
		return atoi(p + 1);
	}

	void read_menu_settings()
	{
		auto f = fopen(kKeymap, "r");
		if (f)
		{
			char buf[2048];
			auto n = fread(buf, 1, sizeof buf - 1, f);
			buf[n] = 0;
			fclose(f);
			sMenuAction[PressSingle] = json_int(buf, "ingame_single_press", sMenuAction[PressSingle]);
			sMenuAction[PressLong] = json_int(buf, "ingame_long_press", sMenuAction[PressLong]);
			sMenuAction[PressDouble] = json_int(buf, "ingame_double_press", sMenuAction[PressDouble]);
		}
		if (exists("/mnt/SDCARD/.tmp_update/config/.menuInverted"))
		{
			sMenuAction[PressSingle] = MenuExit;
			sMenuAction[PressLong] = MenuSwitcher;
		}
		if (exists("/mnt/SDCARD/.tmp_update/config/.noGameSwitcher"))
		{
			sMenuAction[PressSingle] = MenuExit;
			sMenuAction[PressLong] = MenuOff;
		}
		slog("state: MENU (Tweaks): single %d, long %d, double %d (1 Game Switcher, 2 menu, 3 previous game)\n",
		     sMenuAction[PressSingle], sMenuAction[PressLong], sMenuAction[PressDouble]);
	}

	// the previous game, as keymon's quick switch does it (resumeGame(1), state.h): the second
	// game of the recent list moves to the top, its command goes into cmd_to_run.sh, and
	// /tmp/quick_switch has runtime.sh start it when this game ends. false = no previous game.
	bool quick_switch_prepare()
	{
		auto list = exists(kRecentHidden) ? kRecentHidden : kRecentList;
		auto f = fopen(list, "r");
		if (!f)
			return false;
		static char lines[64][2048];
		int n = 0, pick = -1, valid = 0;
		char cmd[2200] = "";
		while (n < 64 && fgets(lines[n], sizeof lines[n], f))
		{
			auto line = lines[n++];
			if (pick >= 0)
				continue;
			auto t = strstr(line, "\"type\":");
			auto r = strstr(line, "\"rompath\":\"");
			if (!t || !r)
				continue;
			auto type = atoi(t + 7);
			if (type != 5 && type != 17)
				continue;
			char rom[1024], launch[1024] = "";
			r += 11;
			auto e = strchr(r, '"');
			if (!e || e - r >= static_cast<int>(sizeof rom))
				continue;
			memcpy(rom, r, static_cast<size_t>(e - r));
			rom[e - r] = 0;
			auto colon = strchr(rom, ':');
			if (colon)
			{
				// "launch:rompath"
				*colon = 0;
				snprintf(launch, sizeof launch, "%s", rom);
				memmove(rom, colon + 1, strlen(colon + 1) + 1);
			}
			else
			{
				auto l = strstr(line, "\"launch\":\"");
				auto le = l ? strchr(l + 10, '"') : nullptr;
				if (le && le - (l + 10) < static_cast<int>(sizeof launch))
				{
					memcpy(launch, l + 10, static_cast<size_t>(le - (l + 10)));
					launch[le - (l + 10)] = 0;
				}
			}
			if (!exists(rom) || !launch[0] || !exists(launch))
				continue;
			if (valid++ == 1)
			{
				pick = n - 1;
				snprintf(cmd, sizeof cmd, "LD_PRELOAD=/mnt/SDCARD/miyoo/app/../lib/libpadsp.so \"%s\" \"%s\"", launch, rom);
			}
		}
		auto complete = feof(f);
		fclose(f);
		if (pick < 0)
			return false;
		if (pick > 0 && complete)
		{
			// (a list longer than read here is left as it is)
			static char out[64 * 2048];
			size_t len = 0;
			for (int i = -1; i < n; i++)
			{
				auto k = i < 0 ? pick : i;
				if (i == pick)
					continue;
				auto l = strlen(lines[k]);
				memcpy(out + len, lines[k], l);
				len += l;
			}
			write_file(list, out, len);
		}
		if (!write_file(kCmdToRun, cmd, strlen(cmd)))
			return false;
		touch("/tmp/quick_switch");
		touch("/tmp/force_auto_load_state");
		slog("state: previous game: %s\n", cmd);
		return true;
	}

	// ---- the Game Switcher's picture (PNG) -------------------------------------------------

	uint32_t sCrcTable[256];

	uint32_t crc32_of(uint32_t crc, const uint8_t* p, size_t n)
	{
		if (!sCrcTable[1])
		{
			for (uint32_t i = 0; i < 256; i++)
			{
				uint32_t c = i;
				for (int k = 0; k < 8; k++)
					c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
				sCrcTable[i] = c;
			}
		}
		crc = ~crc;
		while (n--)
			crc = sCrcTable[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
		return ~crc;
	}

	uint8_t* put32(uint8_t* p, uint32_t v)
	{
		p[0] = static_cast<uint8_t>(v >> 24);
		p[1] = static_cast<uint8_t>(v >> 16);
		p[2] = static_cast<uint8_t>(v >> 8);
		p[3] = static_cast<uint8_t>(v);
		return p + 4;
	}

	// raw (filter byte + RGB rows) -> zlib stream: deflate when zlib is there, stored blocks if not
	size_t png_zlib(const uint8_t* raw, size_t n, uint8_t* out, size_t cap)
	{
		if (zlib_available())
		{
			ZStream z;
			if (!z_deflate_init(&z, 1))
				return 0;
			z.next_in = raw;
			z.avail_in = static_cast<unsigned>(n);
			z.next_out = out;
			z.avail_out = static_cast<unsigned>(cap);
			auto r = p_deflate(&z, Z_FINISH_);
			auto len = static_cast<size_t>(z.total_out);
			p_deflateEnd(&z);
			return r == Z_STREAM_END_ ? len : 0;
		}
		auto p = out;
		*p++ = 0x78;
		*p++ = 0x01;
		uint32_t a = 1, b = 0;
		for (size_t i = 0; i < n; i++)
		{
			a = (a + raw[i]) % 65521u;
			b = (b + a) % 65521u;
		}
		for (size_t off = 0; off < n;)
		{
			auto len = n - off > 65535 ? 65535 : n - off;
			if (static_cast<size_t>(p - out) + 5 + len + 4 > cap)
				return 0;
			*p++ = off + len >= n ? 1 : 0;
			*p++ = static_cast<uint8_t>(len);
			*p++ = static_cast<uint8_t>(len >> 8);
			*p++ = static_cast<uint8_t>(~len);
			*p++ = static_cast<uint8_t>(~len >> 8);
			memcpy(p, raw + off, len);
			p += len;
			off += len;
		}
		return static_cast<size_t>(put32(p, (b << 16) | a) - out);
	}

	// The screen as the device shows it: the table (600x416) in the middle of the 640x480
	// panel, black around it.
	void write_romscreen()
	{
		auto vs = render::vscreen;
		if (!sRomscreen[0] || !vs || !vs->BmpBufPtr1)
			return;
		auto t0 = now_ms();
		const int W = 640, H = 480;
		int ow = vs->Width, oh = vs->Height;
		SDL_GetRendererOutputSize(winmain::Renderer, &ow, &oh);
		auto dx = (W - ow) / 2 + render::DestinationRect.x, dy = (H - oh) / 2 + render::DestinationRect.y;
		const size_t stride = W * 3 + 1, n = stride * H;
		const size_t cap = n + n / 8 + 1024;
		auto raw = static_cast<uint8_t*>(calloc(n, 1));
		auto z = static_cast<uint8_t*>(malloc(cap));
		auto file = static_cast<uint8_t*>(malloc(cap + 64));
		bool ok = false;
		if (raw && z && file)
		{
			for (int y = 0; y < vs->Height; y++)
			{
				auto py = y + dy;
				if (py < 0 || py >= H)
					continue;
				auto o = raw + py * stride + 1;
				auto s = vs->BmpBufPtr1 + y * vs->Stride;
				for (int x = 0; x < vs->Width; x++)
				{
					auto px = x + dx;
					if (px < 0 || px >= W)
						continue;
					auto c = s[x];
					o[px * 3] = c.GetRed();
					o[px * 3 + 1] = c.GetGreen();
					o[px * 3 + 2] = c.GetBlue();
				}
			}
			auto zlen = png_zlib(raw, n, z, cap);
			if (zlen)
			{
				static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
				auto p = file;
				memcpy(p, sig, 8);
				p += 8;
				auto c = p; // IHDR: w, h, 8 bits, RGB
				p = put32(p, 13);
				memcpy(p, "IHDR", 4);
				p += 4;
				p = put32(p, W);
				p = put32(p, H);
				*p++ = 8;
				*p++ = 2;
				*p++ = 0;
				*p++ = 0;
				*p++ = 0;
				p = put32(p, crc32_of(0, c + 4, 17));
				c = p;
				p = put32(p, static_cast<uint32_t>(zlen));
				memcpy(p, "IDAT", 4);
				p += 4;
				memcpy(p, z, zlen);
				p += zlen;
				p = put32(p, crc32_of(0, c + 4, zlen + 4));
				c = p;
				p = put32(p, 0);
				memcpy(p, "IEND", 4);
				p += 4;
				p = put32(p, crc32_of(0, c + 4, 4));
				mkdir("/mnt/SDCARD/Saves", 0777);
				mkdir("/mnt/SDCARD/Saves/CurrentProfile", 0777);
				mkdir(kRomscreenDir, 0777);
				ok = write_file(sRomscreen, file, static_cast<size_t>(p - file));
			}
		}
		free(raw);
		free(z);
		free(file);
		slog("state: Game Switcher picture %s (%.0f ms)\n", ok ? "written" : "FAILED", now_ms() - t0);
	}

	// ---- the state file ---------------------------------------------------------------------

	struct StateHeader
	{
		char magic[8];       // "PBSTATE1"
		uint32_t version;    // 1
		uint32_t codeHash;   // the build that wrote it
		uint32_t sectStart, sectSize; // the static variables (__pb_state_start..end)
		uint32_t heapBase, heapSize, heapEnd, page;
		uint32_t zlib;       // 1: one deflate stream; 0: as it is
		uint32_t rawBytes;   // what follows the page bitmap, uncompressed
		uint32_t fileBytes;  // what follows the page bitmap, in the file
		// winmain and midi's part of the table
		uint8_t singleStep, demoActive, launchBallEnabled, highScoresEnabled;
		int32_t musicTrack;
		uint8_t musicPlaying, pad[3];
		double musicPos;
	};

	size_t heap_pages()
	{
		return kHeapSize / kPage;
	}

	bool page_zero(const uint32_t* p)
	{
		for (unsigned i = 0; i < kPage / 4; i += 8)
		{
			if (p[i] | p[i + 1] | p[i + 2] | p[i + 3] | p[i + 4] | p[i + 5] | p[i + 6] | p[i + 7])
				return false;
		}
		return true;
	}

	// pages in use: from the start of the heap to the highest block handed out, and the last
	// page (the pool's sentinel block)
	size_t used_pages()
	{
		auto used = (sHeapEnd - reinterpret_cast<uintptr_t>(kHeapBase) + kPage - 1) / kPage;
		return used < heap_pages() ? used : heap_pages();
	}

	bool save_state(const char* path)
	{
		miyoo_state::NormalHeap guard; // (the buffers here are not the table's)
		if (!sDisabled && sHeapFull)
		{
			sDisabled = true;
			snprintf(sDisabledWhy, sizeof sDisabledWhy, "the table's heap ran out");
		}
		if (sDisabled)
		{
			slog("state: not saved (%s)\n", sDisabledWhy);
			return false;
		}
		auto t0 = now_ms();
		const auto npages = heap_pages(), nbm = (npages + 7) / 8, used = used_pages();
		std::vector<uint8_t> bm(nbm, 0);
		const auto sectSize = static_cast<size_t>(__pb_state_end - __pb_state_start);
		size_t raw = sectSize, pages = 0;
		for (size_t p = 0; p < npages; p++)
		{
			if (p >= used && p != npages - 1)
				continue;
			if (!page_zero(reinterpret_cast<const uint32_t*>(kHeapBase + p * kPage)))
			{
				bm[p >> 3] |= static_cast<uint8_t>(1u << (p & 7));
				raw += kPage;
				pages++;
			}
		}
		auto tScan = now_ms();

		StateHeader h{};
		memcpy(h.magic, "PBSTATE1", 8);
		h.version = 1;
		h.codeHash = code_hash();
		h.sectStart = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__pb_state_start));
		h.sectSize = static_cast<uint32_t>(sectSize);
		h.heapBase = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(kHeapBase));
		h.heapSize = static_cast<uint32_t>(kHeapSize);
		h.heapEnd = static_cast<uint32_t>(sHeapEnd - reinterpret_cast<uintptr_t>(kHeapBase));
		h.page = kPage;
		h.zlib = zlib_available() ? 1 : 0;
		h.rawBytes = static_cast<uint32_t>(raw);
		h.singleStep = winmain::single_step;
		h.demoActive = winmain::DemoActive;
		h.launchBallEnabled = winmain::LaunchBallEnabled;
		h.highScoresEnabled = winmain::HighScoresEnabled;
		int track = 0;
		double pos = 0;
		bool playing = false;
		midi::MiyooGetState(track, pos, playing);
		h.musicTrack = track;
		h.musicPos = pos;
		h.musicPlaying = playing;

		char tmp[300];
		snprintf(tmp, sizeof tmp, "%s.tmp", path);
		bool ok = false;
		const char* why = "open";
		auto f = fopen(tmp, "wb");
		std::vector<uint8_t> zbuf;
		if (f && fwrite(&h, sizeof h, 1, f) == 1 && fwrite(bm.data(), 1, nbm, f) == nbm)
		{
			why = "write";
			auto start = ftell(f);
			ok = true;
			// the static variables, then the heap's pages
			if (h.zlib)
			{
				ZStream z;
				zbuf.resize(256 * 1024);
				if (!z_deflate_init(&z, 1))
				{
					ok = false;
				}
				else
				{
					auto feed = [&](const void* data, size_t len, bool last)
					{
						z.next_in = static_cast<const unsigned char*>(data);
						z.avail_in = static_cast<unsigned>(len);
						int r;
						do
						{
							z.next_out = zbuf.data();
							z.avail_out = static_cast<unsigned>(zbuf.size());
							r = p_deflate(&z, last ? Z_FINISH_ : Z_NO_FLUSH_);
							auto got = zbuf.size() - z.avail_out;
							if (r < 0 || (got && fwrite(zbuf.data(), 1, got, f) != got))
								ok = false;
						}
						while (ok && (last ? r != Z_STREAM_END_ : z.avail_in != 0));
					};
					feed(__pb_state_start, sectSize, false);
					for (size_t p = 0; p < npages && ok; p++)
					{
						if (bm[p >> 3] & (1u << (p & 7)))
							feed(kHeapBase + p * kPage, kPage, false);
					}
					if (ok)
						feed(nullptr, 0, true);
					p_deflateEnd(&z);
				}
			}
			else
			{
				if (fwrite(__pb_state_start, 1, sectSize, f) != sectSize)
					ok = false;
				for (size_t p = 0; p < npages && ok; p++)
				{
					if ((bm[p >> 3] & (1u << (p & 7))) && fwrite(kHeapBase + p * kPage, 1, kPage, f) != kPage)
						ok = false;
				}
			}
			auto end = ftell(f);
			h.fileBytes = static_cast<uint32_t>(end - start);
			if (ok && (fseek(f, 0, SEEK_SET) != 0 || fwrite(&h, sizeof h, 1, f) != 1))
				ok = false;
		}
		auto tWrite = now_ms();
		if (f)
		{
			if (ok)
			{
				why = "fsync";
				if (fflush(f) != 0 || fsync(fileno(f)) != 0)
					ok = false;
			}
			if (fclose(f) != 0)
				ok = false;
		}
		if (ok)
		{
			why = "rename";
			ok = rename(tmp, path) == 0;
		}
		if (ok)
		{
			fsync_path(path);
			fsync_path(kSaveDir);
		}
		else
		{
			remove(tmp);
		}
		auto tEnd = now_ms();
		if (ok)
			slog("state: saved %s: %u of %u heap pages in use, %u KB -> %u KB %s | scan %.0f ms, %s %.0f ms, "
			     "rest %.0f ms, total %.0f ms\n",
			     path, static_cast<unsigned>(pages), static_cast<unsigned>(used), static_cast<unsigned>(raw / 1024),
			     static_cast<unsigned>(h.fileBytes / 1024), h.zlib ? "(zlib)" : "(not compressed)", tScan - t0,
			     h.zlib ? "compression+writing" : "writing", tWrite - tScan, tEnd - tWrite, tEnd - t0);
		else
			slog("state: FAILED to save %s (%s: %s); the old one was kept\n", path, why, strerror(errno));
		return ok;
	}

	bool header_ok(const StateHeader& h)
	{
		return !memcmp(h.magic, "PBSTATE1", 8) && h.version == 1 && h.codeHash == code_hash() &&
			h.sectStart == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__pb_state_start)) &&
			h.sectSize == static_cast<uint32_t>(__pb_state_end - __pb_state_start) &&
			h.heapBase == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(kHeapBase)) &&
			h.heapSize == kHeapSize && h.page == kPage && h.heapEnd <= kHeapSize;
	}

	// the header checked, nothing loaded: true = a state of this build
	bool state_usable(const char* path)
	{
		auto f = fopen(path, "rb");
		if (!f)
			return false;
		StateHeader h;
		auto ok = fread(&h, sizeof h, 1, f) == 1 && header_ok(h);
		fclose(f);
		return ok;
	}

	// what the table points to outside its memory, taken from the running game across a load
	template <class T>
	struct Keep
	{
		T* where;
		alignas(T) unsigned char bytes[sizeof(T)];

		explicit Keep(T* p) : where(p) { memcpy(bytes, p, sizeof(T)); }
		void put_back() { memcpy(static_cast<void*>(where), bytes, sizeof(T)); }
	};

	// 1 loaded; 0 no file; -1 not loaded (another build, a damaged file): the game is untouched
	int load_state(const char* path)
	{
		miyoo_state::NormalHeap guard; // (the buffers here are not the table's)
		auto t0 = now_ms();
		auto f = fopen(path, "rb");
		if (!f)
			return 0;
		const auto npages = heap_pages(), nbm = (npages + 7) / 8;
		StateHeader h;
		std::vector<uint8_t> bm(nbm), raw, cz;
		const char* why = nullptr;
		size_t pages = 0;
		if (fread(&h, sizeof h, 1, f) != 1 || memcmp(h.magic, "PBSTATE1", 8) || h.version != 1)
			why = "not a state";
		else if (!header_ok(h))
			why = "from another version of the game";
		else if (fread(bm.data(), 1, nbm, f) != nbm)
			why = "cut short";
		if (!why)
		{
			for (size_t p = 0; p < npages; p++)
				pages += (bm[p >> 3] >> (p & 7)) & 1;
			auto pos = ftell(f);
			fseek(f, 0, SEEK_END);
			if (h.sectSize + pages * kPage != h.rawBytes || ftell(f) != pos + static_cast<long>(h.fileBytes))
				why = "cut short";
			fseek(f, pos, SEEK_SET);
		}
		if (!why && h.zlib && !zlib_available())
			why = "compressed, and no zlib here";
		if (!why)
		{
			raw.resize(h.rawBytes);
			if (h.zlib)
			{
				cz.resize(h.fileBytes ? h.fileBytes : 1);
				ZStream z;
				if (fread(cz.data(), 1, h.fileBytes, f) != h.fileBytes)
				{
					why = "read error";
				}
				else if (!z_inflate_init(&z))
				{
					why = "zlib";
				}
				else
				{
					z.next_in = cz.data();
					z.avail_in = h.fileBytes;
					z.next_out = raw.data();
					z.avail_out = h.rawBytes;
					auto r = p_inflate(&z, Z_FINISH_);
					if (r != Z_STREAM_END_ || z.total_out != h.rawBytes)
						why = "damaged data";
					p_inflateEnd(&z);
				}
			}
			else if (fread(raw.data(), 1, h.rawBytes, f) != h.rawBytes)
			{
				why = "read error";
			}
		}
		fclose(f);
		std::vector<uint8_t>().swap(cz);
		auto tRead = now_ms();
		if (why)
		{
			slog("state: %s not loaded (%s)\n", path, why);
			return -1;
		}

		// what is the running game's, not the state's
		Keep<std::string> datFile(&pb::DatFileName), basePath(&pb::BasePath);
		Keep<bool> fullTilt(&pb::FullTiltMode), fullTiltDemo(&pb::FullTiltDemoMode);
		Keep<int> quick(&pb::quickFlag);
		Keep<SDL_Rect> destRect(&render::DestinationRect);
		size_t soundsSize;
		auto sounds = loader::MiyooSoundList(&soundsSize);
		std::vector<uint8_t> soundsKept(static_cast<uint8_t*>(sounds), static_cast<uint8_t*>(sounds) + soundsSize);
		auto screenTexture = render::vscreen ? render::vscreen->Texture : nullptr;
		const auto liveUsed = used_pages();

		// from here on the game's memory changes
		memcpy(__pb_state_start, raw.data(), h.sectSize);
		auto src = raw.data() + h.sectSize;
		sHeapEnd = reinterpret_cast<uintptr_t>(kHeapBase) + h.heapEnd;
		const auto snapUsed = used_pages();
		for (size_t p = 0; p < npages; p++)
		{
			auto pg = kHeapBase + p * kPage;
			if (bm[p >> 3] & (1u << (p & 7)))
			{
				memcpy(pg, src, kPage);
				src += kPage;
			}
			else if (p < liveUsed || p < snapUsed || p == npages - 1)
			{
				if (!page_zero(reinterpret_cast<const uint32_t*>(pg)))
					memset(pg, 0, kPage);
			}
		}
		std::vector<uint8_t>().swap(raw);

		datFile.put_back();
		basePath.put_back();
		fullTilt.put_back();
		fullTiltDemo.put_back();
		quick.put_back();
		destRect.put_back();
		memcpy(sounds, soundsKept.data(), soundsSize);
		if (render::vscreen)
			render::vscreen->Texture = screenTexture;
		render::MiyooDirty = true;

		winmain::single_step = h.singleStep != 0;
		winmain::DemoActive = h.demoActive != 0;
		winmain::LaunchBallEnabled = h.launchBallEnabled != 0;
		winmain::HighScoresEnabled = h.highScoresEnabled != 0;
		midi::MiyooRestoreState(h.musicTrack, h.musicPos, h.musicPlaying != 0, winmain::single_step);
		if (winmain::single_step)
			Mix_Pause(-1);

		slog("state: loaded %s (%u heap pages, %spaused, music track %d at %.1f s): reading %.0f ms, copy %.0f ms\n",
		     path, static_cast<unsigned>(pages), winmain::single_step ? "" : "not ", h.musicTrack, h.musicPos,
		     tRead - t0, now_ms() - tRead);
		return 1;
	}

	void save_settings()
	{
		// (the High Scores are written as soon as they are typed; the rest of the settings
		// only when the game closes)
		miyoo_state::NormalHeap guard;
		high_score::write();
		if (ImGui::GetCurrentContext() && ImGui::GetIO().IniFilename)
			ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
	}

	[[noreturn]] void finish()
	{
		release_checkoff();
		if (sLog)
			fclose(sLog);
		sync();
		_exit(0);
	}

	// Turned off while a name was being typed for a new record: the record is kept, with the
	// name as it is (the High Scores are not part of a state).
	void finish_name_entry()
	{
		for (int i = 0; i < 8 && high_score::MiyooDialogActive(); i++)
		{
			high_score::RenderHighScoreDialog(); // (opens it if it is still waiting)
			high_score::MiyooKeyDown(SDLK_RETURN);
			high_score::RenderHighScoreDialog();
		}
	}

	void exit_saving(ExitKind kind)
	{
		static const char* names[] = {"close", "Game Switcher", "power-off", "previous game"};
		slog("state: leaving (%s)\n", names[kind]);
		if (kind == ExitPowerOff && high_score::MiyooDialogActive())
		{
			slog("state: a name was being typed for the High Scores: saved as it was\n");
			finish_name_entry();
		}
		if (sOnion)
			write_romscreen();
		auto ok = save_state(kAutoState);
		if (ok)
			remove(kLoadingFlag);
		if (kind == ExitPowerOff)
		{
			if (ok)
				touch(kBootResume);
			put_back_cmd();
			save_settings();
			slog("state: closing for the power-off\n");
			finish();
		}
		if (kind == ExitSwitcher)
			touch(kRunSwitcher);
		if (kind == ExitQuickSwitch && !quick_switch_prepare())
			slog("state: no previous game in the list: back to the menu\n");
		sQuitting = true;
		SDL_Event quit{};
		quit.type = SDL_QUIT;
		SDL_PushEvent(&quit);
	}

	void on_sigterm(int)
	{
		sOffSignal = 1;
	}

	// ---- the MENU key ---------------------------------------------------------------------
	// Read from the key devices directly: MENU's own timing, and the keys that never reach
	// SDL on this device (POWER, volume) count as "another key" too. Reading them does not
	// take the keys away from SDL or OnionOS.

	const double kMenuLongMs = 700.0, kMenuDoubleMs = 300.0;
	int sKeyFd[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
	unsigned char sDown[KEY_MAX / 8 + 1];
	bool sMenuDown, sMenuMixed, sMenuLongDone, sMenuSingleWait, sMenuSkipRelease;
	double sMenuPressT;
	int sMenuPress = -1;

	bool others_down()
	{
		for (int i = 0; i < static_cast<int>(sizeof sDown); i++)
		{
			auto m = sDown[i];
			if (i == KEY_ESC / 8)
				m &= static_cast<unsigned char>(~(1u << (KEY_ESC % 8)));
			if (m)
				return true;
		}
		return false;
	}

	bool double_used()
	{
		return sOnion && sMenuAction[PressDouble] != MenuOff;
	}

	void menu_poll()
	{
		auto now = now_ms();
		for (auto fd : sKeyFd)
		{
			if (fd < 0)
				continue;
			input_event ev[32];
			for (;;)
			{
				auto r = read(fd, ev, sizeof ev);
				if (r <= 0)
					break;
				auto n = static_cast<int>(r / sizeof ev[0]);
				for (int i = 0; i < n; i++)
				{
					if (ev[i].type != EV_KEY)
						continue;
					int code = ev[i].code;
					if (code >= 0 && code < KEY_MAX)
					{
						if (ev[i].value)
							sDown[code / 8] |= static_cast<unsigned char>(1u << (code % 8));
						else
							sDown[code / 8] &= static_cast<unsigned char>(~(1u << (code % 8)));
					}
					if (code == KEY_ESC && ev[i].value == 1)
					{
						// MENU pressed
						if (sMenuSingleWait && now - sMenuPressT <= kMenuDoubleMs)
						{
							sMenuSingleWait = false; // the second press: double
							sMenuSkipRelease = true;
							sMenuPress = PressDouble;
						}
						else
						{
							sMenuSkipRelease = false;
						}
						sMenuDown = true;
						// (typing a name in the High Scores: MENU does nothing there)
						sMenuMixed = others_down() || high_score::MiyooDialogActive();
						sMenuLongDone = false;
						sMenuPressT = now;
					}
					else if (code == KEY_ESC && ev[i].value == 0)
					{
						// MENU released
						if (sMenuDown && !sMenuMixed && !sMenuLongDone && !sMenuSkipRelease && !others_down() &&
							!high_score::MiyooDialogActive())
						{
							if (double_used())
								sMenuSingleWait = true; // single, unless a second press comes
							else
								sMenuPress = PressSingle;
						}
						sMenuDown = false;
						sMenuSkipRelease = false;
					}
					else if (code != KEY_ESC && ev[i].value == 1 && sMenuDown)
					{
						sMenuMixed = true; // another key with MENU
					}
				}
				if (n < 32)
					break;
			}
		}
		if (sMenuDown && !sMenuMixed && !sMenuLongDone && !sMenuSkipRelease && now - sMenuPressT >= kMenuLongMs)
		{
			sMenuLongDone = true; // held: long press
			if (!others_down() && !high_score::MiyooDialogActive())
				sMenuPress = PressLong;
		}
		if (sMenuSingleWait && now - sMenuPressT > kMenuDoubleMs)
		{
			sMenuSingleWait = false;
			sMenuPress = PressSingle;
		}
	}
}

// ---- the C++ heap ---------------------------------------------------------------------------

void* operator new(std::size_t n)
{
	return heap_new(n);
}

void* operator new[](std::size_t n)
{
	return heap_new(n);
}

void* operator new(std::size_t n, const std::nothrow_t&) noexcept
{
	return heap_alloc(n);
}

void* operator new[](std::size_t n, const std::nothrow_t&) noexcept
{
	return heap_alloc(n);
}

void operator delete(void* p) noexcept
{
	heap_free(p);
}

void operator delete[](void* p) noexcept
{
	heap_free(p);
}

void operator delete(void* p, const std::nothrow_t&) noexcept
{
	heap_free(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept
{
	heap_free(p);
}

void operator delete(void* p, std::size_t) noexcept
{
	heap_free(p);
}

void operator delete[](void* p, std::size_t) noexcept
{
	heap_free(p);
}

// ---- the interface --------------------------------------------------------------------------

miyoo_state::NormalHeap::NormalHeap() : prev(sHeapOn)
{
	sHeapOn = false;
}

miyoo_state::NormalHeap::~NormalHeap()
{
	sHeapOn = prev;
}

void miyoo_state::Init()
{
	sOnion = exists(kOnionDir);
	sLog = fopen(kLogFile, "w");
	mkdir(kSaveDir, 0777);
	remove("saves/state_auto.pbs.tmp");

	heap_init();
	if (!sTlsf)
	{
		sDisabled = true;
		snprintf(sDisabledWhy, sizeof sDisabledWhy, "no room for the table's heap at %p", static_cast<void*>(kHeapBase));
	}
	// (the link must have gathered the table's variables, and only them: miyoo_state.ld)
	auto inBlock = [](const void* p) { return p >= __pb_state_start && p < __pb_state_end; };
	if (!inBlock(&pb::MainTable) || !inBlock(&render::vscreen) || !inBlock(&loader::loader_table) ||
		inBlock(&options::Options) || inBlock(&winmain::single_step))
	{
		sDisabled = true;
		snprintf(sDisabledWhy, sizeof sDisabledWhy, "the build did not gather the table's variables (miyoo_state.ld)");
	}
	slog("state: table's heap %s at %p (%u MB); static variables %u KB at %p\n", sTlsf ? "ready" : "NOT available",
	     static_cast<void*>(kHeapBase), static_cast<unsigned>(kHeapSize >> 20),
	     static_cast<unsigned>((__pb_state_end - __pb_state_start) / 1024), static_cast<void*>(__pb_state_start));
	if (sDisabled)
		slog("state: save states off (%s)\n", sDisabledWhy);

	for (int i = 0; i < 10; i++)
	{
		char path[32];
		snprintf(path, sizeof path, "/dev/input/event%d", i);
		sKeyFd[i] = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	}

	if (sOnion)
	{
		// SDL would turn SIGTERM into SDL_QUIT, and the game would close without saving
		SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
		signal(SIGPIPE, SIG_IGN);
		struct sigaction action{};
		action.sa_handler = on_sigterm;
		sigemptyset(&action.sa_mask);
		action.sa_flags = SA_RESTART;
		sigaction(SIGTERM, &action, nullptr);

		auto f = fopen(kCmdToRun, "rb");
		if (f)
		{
			auto n = fread(sCmd, 1, sizeof sCmd - 1, f);
			if (n > 0 && n < sizeof sCmd - 1)
				sCmdLen = static_cast<int>(n);
			fclose(f);
		}
		sOffAtStart = exists("/tmp/.offOrder");
		make_checkoff_script();
		find_romscreen();
		read_menu_settings();
	}

	// where the game starts
	char prev[32] = "";
	auto f = fopen("/tmp/prev_state", "r");
	if (f)
	{
		if (fgets(prev, sizeof prev, f))
			prev[strcspn(prev, "\r\n")] = 0;
		fclose(f);
	}
	if (exists(kLoadingFlag))
	{
		slog("state: the last resume did not get going (did it crash?): starting a new game\n");
		remove(kLoadingFlag);
		remove(kBootResume);
		return;
	}
	auto boot = exists(kBootResume);
	auto switcher = !strcmp(prev, "switcher") || !strcmp(prev, "game");
	if ((boot || switcher) && !sDisabled)
	{
		if (state_usable(kAutoState))
		{
			sResumePending = true;
		}
		else if (exists(kAutoState))
		{
			slog("state: %s is from another version of the game: removed\n", kAutoState);
			remove(kAutoState);
		}
	}
	remove(kBootResume);
	slog("state: opened by '%s'%s: %s\n", prev[0] ? prev : "?", boot ? " (turned off in the game)" : "",
	     sResumePending ? "going on from where it was" : "starting a new game");
}

void miyoo_state::HeapOn()
{
	if (sTlsf)
		sHeapOn = true;
}

bool miyoo_state::ResumePending()
{
	return sResumePending;
}

bool miyoo_state::Resume()
{
	sResumePending = false;
	touch(kLoadingFlag);
	if (load_state(kAutoState) == 1)
	{
		sLoadedAt = now_ms();
		return true;
	}
	remove(kLoadingFlag);
	return false;
}

void miyoo_state::Poll()
{
	if (sQuitting)
		return;
	auto now = now_ms();
	if (sLoadedAt && now - sLoadedAt >= 5000)
	{
		remove(kLoadingFlag); // 5 s after a resume: it did not crash the game
		sLoadedAt = 0;
	}

	// power-off: the signal, or OnionOS's file (an old one, there before the game, is ignored)
	auto off = sOffSignal != 0;
	if (!off && sOnion && !sOffAtStart && now >= sNextOffPoll)
	{
		sNextOffPoll = now + 250.0;
		off = exists("/tmp/.offOrder");
	}
	if (off)
		exit_saving(ExitPowerOff);

	menu_poll();
	if (sMenuPress >= 0)
	{
		auto press = sMenuPress;
		sMenuPress = -1;
		if (!sOnion)
		{
			// (outside OnionOS: MENU closes the game, as before)
			if (press == PressSingle)
			{
				sQuitting = true;
				SDL_Event quit{};
				quit.type = SDL_QUIT;
				SDL_PushEvent(&quit);
			}
		}
		else
		{
			switch (sMenuAction[press])
			{
			case MenuSwitcher: exit_saving(ExitSwitcher);
				break;
			case MenuExit: exit_saving(ExitQuit);
				break;
			case MenuQuickSwitch: exit_saving(ExitQuickSwitch);
				break;
			default: break; // (off, or RetroArch's own menu: nothing here)
			}
		}
	}
}

void miyoo_state::Shutdown()
{
	release_checkoff();
	slog("state: closed\n");
}

#else // not Linux: no save states, MENU is handled as before

miyoo_state::NormalHeap::NormalHeap() : prev(false) {}
miyoo_state::NormalHeap::~NormalHeap() {}
void miyoo_state::Init() {}
void miyoo_state::HeapOn() {}
bool miyoo_state::ResumePending() { return false; }
bool miyoo_state::Resume() { return false; }
void miyoo_state::Poll() {}
void miyoo_state::Shutdown() {}

#endif
