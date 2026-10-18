// The game's texts come from its language files (menu/language.h). The port's own (VIDEO / GENERAL LANGUAGE / CONTROLS / KEY BINDINGS pages, the main menu's CONTROLS / ONLINE PLAY / QUIT, the restart and QUIT dialogs, the achievements' header, summary and dates) are written in English in the code
// and translated here (French, German, Spanish, Italian). The game's fonts carry only the characters each language file's texts use. Menu labels and values are translated in plain A-Z (as the game's own English menus); texts with accents (dialogs, descriptions) go through Fit, which checks them
// against the fonts of the scene they are drawn in (read from its loaded language file) and drops or simplifies characters the font lacks.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sst::text
{
  // `english` in the game's language (unchanged when there is no translation, or the language is English)
  std::string Tr(std::string_view english);

  // A scene file was read (lower-case name, guest buffer): language files (<stem>_<xx>.dat) are noted for Fit
  void OnSceneFile(const std::string &name, uint32_t buffer);

  // `text` in the characters the fonts of scene `stem` ("msgbox", "sstmainmenu", "pausemenu", "global") have: font `font`, or any of the file's fonts (0). Accented letters fall back to plain ones, lower case to upper case; other missing characters are dropped. Unchanged when the file isn't loaded (yet). `base` may be null (the guest memory's).
  std::string Fit(uint8_t *base, const std::string &text, std::string_view stem, uint32_t font = 0);

  // Whether scene `stem`'s font `font` is known (its language file was read)
  bool FontKnown(uint8_t *base, std::string_view stem, uint32_t font);

  // Whether it has character `cp` (true when the font isn't known)
  bool HasChar(uint8_t *base, std::string_view stem, uint32_t font, uint32_t cp);

  // An unlock time in the game's language's usual form ("10/14/2026  12:49 PM", "14/10/2026  12:49", "14.10.2026  12:49")
  std::string DateTime(int year, int month, int day, int hour, int minute);
}