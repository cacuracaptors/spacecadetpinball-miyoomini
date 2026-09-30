#include "pch.h"
#include "high_score.h"

#include "options.h"
#include "pb.h"
#include "score.h"
#include "translations.h"
#include "gdrv.h" // Miyoo Mini patch (MiyooDrawOverlay)
#include "render.h"
#include "fullscrn.h"
#include "winmain.h"

bool high_score::dlg_enter_name;
bool high_score::ShowDialog = false;
high_score_entry high_score::DlgData;
std::vector<high_score_entry> high_score::ScoreQueue;
high_score_struct high_score::highscore_table[5];

int high_score::read()
{
	char Buffer[20];

	int checkSum = 0;
	clear_table();
	for (auto position = 0; position < 5; ++position)
	{
		auto& tablePtr = highscore_table[position];

		snprintf(Buffer, sizeof Buffer, "%d", position);
		strcat(Buffer, ".Name");
		auto name = options::GetSetting(Buffer, "");
		strncpy(tablePtr.Name, name.c_str(), sizeof tablePtr.Name);

		snprintf(Buffer, sizeof Buffer, "%d", position);
		strcat(Buffer, ".Score");
		tablePtr.Score = options::get_int(Buffer, tablePtr.Score);

		for (int i = static_cast<int>(strlen(tablePtr.Name)); --i >= 0; checkSum += tablePtr.Name[i])
		{
		}
		checkSum += tablePtr.Score;
	}

	auto verification = options::get_int("Verification", 7);
	if (checkSum != verification)
		clear_table();
	return 0;
}

int high_score::write()
{
	char Buffer[20];

	int checkSum = 0;
	for (auto position = 0; position < 5; ++position)
	{
		auto& tablePtr = highscore_table[position];

		snprintf(Buffer, sizeof Buffer, "%d", position);
		strcat(Buffer, ".Name");
		options::SetSetting(Buffer, tablePtr.Name);

		snprintf(Buffer, sizeof Buffer, "%d", position);
		strcat(Buffer, ".Score");
		options::set_int(Buffer, tablePtr.Score);

		for (int i = static_cast<int>(strlen(tablePtr.Name)); --i >= 0; checkSum += tablePtr.Name[i])
		{
		}
		checkSum += tablePtr.Score;
	}

	options::set_int("Verification", checkSum);
	return 0;
}

void high_score::clear_table()
{
	for (auto& table : highscore_table)
	{
		table.Score = -999;
		table.Name[0] = 0;
	}
}

int high_score::get_score_position(int score)
{
	if (score <= 0)
		return -1;

	for (int position = 0; position < 5; position++)
	{
		if (highscore_table[position].Score < score)
			return position;
	}
	return -1;
}

void high_score::place_new_score_into(high_score_entry data)
{
	if (data.Position >= 0 && data.Position < 5)
	{
		for (int i = 4; i > data.Position; i--)
		{
			highscore_table[i] = highscore_table[i - 1];
		}

		data.Entry.Name[31] = 0;
		highscore_table[data.Position] = data.Entry;
	}
}

void high_score::show_high_score_dialog()
{
	ShowDialog = true;
}

void high_score::show_and_set_high_score_dialog(high_score_entry score)
{
	ScoreQueue.insert(ScoreQueue.begin(), score);
	ShowDialog = true;
}

// BEGIN Miyoo Mini high score patch
// The name is typed with the D-pad, the same way as in the Fallout ports:
// Up/Down cycle the letter shown in yellow, Left switches upper/lower case,
// Right adds a space, A keeps the letter, B erases, Start saves.
// The Menu key does nothing while the table is open, so the OnionOS
// Menu+Power screenshot combo works on it.
// Keycodes are the ones the Miyoo Mini buttons send.
static const char MiyooCharset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
static const int MiyooCharsetLen = static_cast<int>(sizeof MiyooCharset) - 1;
static const int MiyooMaxNameLen = 20;

enum MiyooActions { MiyooNone, MiyooOk, MiyooClose, MiyooClear };

static bool MiyooOpen = false; // the table is on the screen
static int MiyooAction = MiyooNone; // done on the next frame
static bool MiyooPreview = false; // a letter is being chosen (shown in yellow)
static int MiyooCycle = 0;
static bool MiyooLower = false;
static bool MiyooConfirmClear = false;
static char MiyooDefaultName[32];

static char MiyooCurrentChar()
{
	char c = MiyooCharset[MiyooCycle];
	if (MiyooLower && c >= 'A' && c <= 'Z')
		c = static_cast<char>(c - 'A' + 'a');
	return c;
}

// The game only writes its .ini when it closes; save right away so a new
// record survives the device being switched off.
static void MiyooSaveToDisk()
{
	high_score::write();
	ImGui::SaveIniSettingsToDisk(ImGui::GetIO().IniFilename);
}

bool high_score::MiyooDialogActive()
{
	return ShowDialog || MiyooOpen;
}

void high_score::MiyooKeyDown(int sym)
{
	if (!MiyooOpen)
		return; // the table opens on the next frame

	if (!dlg_enter_name)
	{
		// Only looking at the table
		if (MiyooConfirmClear)
		{
			if (sym == SDLK_SPACE) // A: yes
				MiyooAction = MiyooClear;
			else if (sym == SDLK_LCTRL) // B: no
				MiyooConfirmClear = false;
			return;
		}
		if (sym == SDLK_RCTRL) // Select: clear the table (asks first)
			MiyooConfirmClear = true;
		else if (sym == SDLK_SPACE || sym == SDLK_LCTRL || sym == SDLK_RETURN || sym == SDLK_DOWN)
			MiyooAction = MiyooClose;
		return;
	}

	auto name = DlgData.Entry.Name;
	auto len = static_cast<int>(strlen(name));
	switch (sym)
	{
	case SDLK_UP:
	case SDLK_DOWN:
		if (MiyooPreview)
			MiyooCycle = (MiyooCycle + (sym == SDLK_UP ? 1 : MiyooCharsetLen - 1)) % MiyooCharsetLen;
		else if (len < MiyooMaxNameLen)
			MiyooPreview = true;
		break;
	case SDLK_LEFT: // upper/lower case
		MiyooLower = !MiyooLower;
		break;
	case SDLK_RIGHT: // space
		if (len < MiyooMaxNameLen)
		{
			name[len] = ' ';
			name[len + 1] = 0;
		}
		MiyooPreview = false;
		MiyooCycle = 0;
		break;
	case SDLK_SPACE: // A: keep the letter, go to the next one
		if (MiyooPreview && len < MiyooMaxNameLen)
		{
			name[len] = MiyooCurrentChar();
			name[len + 1] = 0;
		}
		MiyooPreview = false;
		MiyooCycle = 0;
		break;
	case SDLK_LCTRL: // B: erase
		if (MiyooPreview)
			MiyooPreview = false;
		else if (len > 0)
			name[len - 1] = 0;
		MiyooCycle = 0;
		break;
	case SDLK_RETURN: // Start: save
		MiyooAction = MiyooOk;
		break;
	default:
		break;
	}
}

// ---- Drawing: a box made of the game's own bitmap font (the one of the
// messages on the right panel), drawn over the table after it is presented.

static gdrv_bitmap8* MiyooBox = nullptr;
static std::string MiyooBoxKey; // what is drawn in MiyooBox now

static const ColorRgba MiyooBg{16, 16, 32, 255};
static const ColorRgba MiyooRowBg{40, 40, 80, 255};
static const ColorRgba MiyooFrame{160, 160, 160, 255};
static const ColorRgba MiyooYellow{255, 255, 0, 255};

// Draws (dst != nullptr) or only measures (dst == nullptr) a text; returns its width.
static int MiyooText(gdrv_bitmap8* dst, int x, int y, const char* text, bool yellow = false)
{
	auto font = score::msg_fontp;
	auto startX = x;
	for (; *text; ++text)
	{
		auto ch = font->Chars[*text & 0x7F];
		if (!ch || !ch->BmpBufPtr1)
			continue;
		if (dst)
		{
			for (int py = 0; py < ch->Height; ++py)
			{
				auto dy = y + py;
				if (dy < 0 || dy >= dst->Height)
					continue;
				for (int px = 0; px < ch->Width; ++px)
				{
					auto dx = x + px;
					if (dx < 0 || dx >= dst->Width)
						continue;
					auto color = ch->BmpBufPtr1[py * ch->Stride + px];
					if (color.Color)
						dst->BmpBufPtr1[dy * dst->Stride + dx] = yellow ? MiyooYellow : color;
				}
			}
		}
		x += ch->Width + font->GapWidth;
	}
	return x - startX;
}

static void MiyooRect(gdrv_bitmap8* dst, int x, int y, int w, int h, ColorRgba color)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > dst->Width) w = dst->Width - x;
	if (y + h > dst->Height) h = dst->Height - y;
	if (w > 0 && h > 0)
		gdrv::fill_bitmap(dst, w, h, x, y, color);
}

void high_score::MiyooDrawOverlay()
{
	if (!MiyooOpen || !score::msg_fontp || !render::vscreen)
		return;

	auto font = score::msg_fontp;
	const int pad = 10, lineH = font->Height + 4;
	auto cursorOn = (SDL_GetTicks() / 500) % 2 == 0;

	// Rows of the table, with the new score in its place while it is typed
	struct Row { const char* name; int score; bool editing; };
	Row rows[5];
	for (int offset = 0, row = 0; row < 5; row++)
	{
		if (dlg_enter_name && DlgData.Position == row)
		{
			offset = -1;
			rows[row] = {DlgData.Entry.Name, DlgData.Entry.Score, true};
		}
		else
		{
			auto& entry = highscore_table[row + offset];
			rows[row] = {entry.Name, entry.Score, false};
		}
	}

	const char* help[3] = {nullptr, nullptr, nullptr};
	char menuLine[64];
	if (dlg_enter_name)
	{
		help[0] = "Up/Down: letter   Left: a/A   Right: space";
		help[1] = "A: next letter   B: erase   Start: save";
		snprintf(menuLine, sizeof menuLine, "Empty name: saved as %s", MiyooDefaultName);
		help[2] = menuLine;
	}
	else if (MiyooConfirmClear)
	{
		help[0] = pb::get_rc_string(Msg::STRING141);
		help[1] = "A: yes   B: no";
	}
	else
	{
		help[0] = "A: close   Select: clear table";
	}

	// Redraw only when something changed (typing, blinking cursor...)
	char buf[64];
	std::string key;
	for (auto& row : rows)
	{
		key += row.name;
		key += '|';
		key += std::to_string(row.score);
		key += '|';
	}
	for (auto line : help)
		if (line)
			key += line;
	snprintf(buf, sizeof buf, "|%d%d%d%c", dlg_enter_name, MiyooPreview, cursorOn, MiyooCurrentChar());
	key += buf;

	// Size of the box
	auto title = pb::get_rc_string(Msg::HIGHSCORES_Caption);
	auto rankW = MiyooText(nullptr, 0, 0, "5.") + pad;
	auto nameW = MiyooText(nullptr, 0, 0, "N") * MiyooMaxNameLen;
	auto scoreW = MiyooText(nullptr, 0, 0, "000,000,000");
	auto width = rankW + nameW + pad + scoreW;
	width = std::max(width, MiyooText(nullptr, 0, 0, title));
	for (auto line : help)
		if (line)
			width = std::max(width, MiyooText(nullptr, 0, 0, line));
	int helpLines = 0;
	for (auto line : help)
		if (line)
			helpLines++;
	auto boxW = std::min(width + pad * 2, render::vscreen->Width - 8);
	auto boxH = std::min(pad * 3 + lineH * (1 + 5 + helpLines) + pad, render::vscreen->Height - 8);

	if (!MiyooBox || MiyooBox->Width != boxW || MiyooBox->Height != boxH)
	{
		delete MiyooBox;
		MiyooBox = new gdrv_bitmap8(boxW, boxH, false);
		MiyooBox->CreateTexture("nearest", SDL_TEXTUREACCESS_STREAMING);
		MiyooBoxKey.clear();
	}

	if (key != MiyooBoxKey)
	{
		MiyooBoxKey = key;
		auto bmp = MiyooBox;
		MiyooRect(bmp, 0, 0, boxW, boxH, MiyooFrame);
		MiyooRect(bmp, 2, 2, boxW - 4, boxH - 4, MiyooBg);

		auto y = pad;
		MiyooText(bmp, (boxW - MiyooText(nullptr, 0, 0, title)) / 2, y, title);
		y += lineH + pad;

		for (int row = 0; row < 5; row++)
		{
			auto& r = rows[row];
			if (r.editing)
				MiyooRect(bmp, pad / 2, y - 2, boxW - pad, lineH, MiyooRowBg);

			snprintf(buf, sizeof buf, "%d.", row + 1);
			MiyooText(bmp, pad, y, buf);

			auto x = pad + rankW;
			x += MiyooText(bmp, x, y, r.name);
			if (r.editing)
			{
				// The letter being chosen (yellow, underlined), or a blinking cursor
				auto charW = MiyooText(nullptr, 0, 0, "N");
				if (MiyooPreview)
				{
					char preview[2] = {MiyooCurrentChar(), 0};
					auto w = MiyooText(bmp, x, y, preview, true);
					MiyooRect(bmp, x, y + font->Height, std::max(w, charW), 2, MiyooYellow);
				}
				else if (cursorOn)
				{
					MiyooRect(bmp, x, y + font->Height, charW, 2, MiyooFrame);
				}
			}

			score::string_format(r.score, buf);
			MiyooText(bmp, boxW - pad - MiyooText(nullptr, 0, 0, buf), y, buf);
			y += lineH;
		}

		y += pad / 2;
		MiyooRect(bmp, pad, y, boxW - pad * 2, 1, MiyooFrame);
		y += pad / 2 + 2;
		for (auto line : help)
		{
			if (!line)
				continue;
			MiyooText(bmp, pad, y, line);
			y += lineH;
		}

		bmp->BlitToTexture();
	}

	// Centered on the game screen, scaled like the game itself
	SDL_Rect rect{(render::vscreen->Width - boxW) / 2, (render::vscreen->Height - boxH) / 2, boxW, boxH};
	auto dst = fullscrn::GetScreenRectFromPinballRect(rect);
	SDL_RenderCopy(winmain::Renderer, MiyooBox->Texture, nullptr, &dst);
}

void high_score::RenderHighScoreDialog()
{
	// Miyoo Mini patch: replaces the original ImGui window (it does not show
	// up on this device). Same logic: scores waiting in ScoreQueue are asked
	// one at a time; with an empty queue it only shows the table.
	if (ShowDialog)
	{
		ShowDialog = false;
		if (!MiyooOpen)
		{
			dlg_enter_name = false;
			while (!ScoreQueue.empty())
			{
				DlgData = ScoreQueue.back();
				ScoreQueue.pop_back();
				if (DlgData.Position < 0 || DlgData.Position > 4)
				{
					DlgData.Position = get_score_position(DlgData.Entry.Score);
				}

				if (DlgData.Position != -1)
				{
					dlg_enter_name = true;
					break;
				}
			}

			// Fresh typing state; the name starts with the last one typed on
			// this device (B erases it).
			MiyooPreview = false;
			MiyooCycle = 0;
			MiyooLower = false;
			MiyooConfirmClear = false;
			MiyooAction = MiyooNone;
			if (dlg_enter_name)
			{
				strncpy(MiyooDefaultName, DlgData.Entry.Name, sizeof MiyooDefaultName - 1);
				MiyooDefaultName[sizeof MiyooDefaultName - 1] = 0;
				auto& lastName = options::GetSetting("Miyoo.LastName", "");
				strncpy(DlgData.Entry.Name, lastName.c_str(), MiyooMaxNameLen);
				DlgData.Entry.Name[MiyooMaxNameLen] = 0;
			}
			MiyooOpen = true;
		}
	}

	if (!MiyooOpen)
		return;

	auto action = MiyooAction;
	MiyooAction = MiyooNone;
	if (action == MiyooOk)
	{
		if (dlg_enter_name)
		{
			auto name = DlgData.Entry.Name;
			auto len = static_cast<int>(strlen(name));
			if (action == MiyooOk && MiyooPreview && len < MiyooMaxNameLen)
			{
				name[len] = MiyooCurrentChar();
				name[len + 1] = 0;
			}
			len = static_cast<int>(strlen(name));
			while (len > 0 && name[len - 1] == ' ')
				name[--len] = 0;

			if (name[0] == 0)
			{
				strncpy(name, MiyooDefaultName, sizeof DlgData.Entry.Name - 1);
				name[sizeof DlgData.Entry.Name - 1] = 0;
			}
			else
			{
				options::SetSetting("Miyoo.LastName", name);
			}
			place_new_score_into(DlgData);
			MiyooSaveToDisk();
		}
		MiyooPreview = false;
		MiyooOpen = false;
	}
	else if (action == MiyooClose)
	{
		MiyooOpen = false;
	}
	else if (action == MiyooClear)
	{
		clear_table();
		MiyooSaveToDisk();
		MiyooConfirmClear = false;
	}

	if (!MiyooOpen)
	{
		delete MiyooBox; // frees its texture too
		MiyooBox = nullptr;
		MiyooBoxKey.clear();

		// Next score in the queue (more than one player)
		if (!ScoreQueue.empty())
			ShowDialog = true;
	}
}
// END Miyoo Mini high score patch
