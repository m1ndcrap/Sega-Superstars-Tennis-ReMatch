// The game is fully localized: every SumoTool scene has per-language text/font files (<stem>_EN / _US / _FR / _GE / _SP / _IT, plus unused _JP) in RESOURCE.PAC, the umpire speaks English, French, German, Italian and Spanish (male and female), and the title data (XDBF) has the achievements in
// English, German, French, Spanish and Italian. On the Xbox 360, the console's language picks them: sub_822F4640 (read by the game's setup sub_822C9000) returns the XLanguage, which becomes the index at 0x82643914 into the suffix table at 0x82615C28 {_EN, _EN, _US, _FR, _GE, _SP, _IT, _EN}
// (English: _US instead of _EN when the game region says so). The SDK always answers English; this module answers the player's choice instead. Choice: --sst_language, else "language = ..." in sst_settings.ini, else "auto" (the Windows display language when it is one of the game's, otherwise
// English). Names: auto, english, french, german, spanish, italian (or en / fr / de / es / it). The language is read once at boot: changing it needs a restart.

#pragma once

#include <cstdint>
#include <string>

namespace sst::lang
{
  // XLanguage values (rex::system::XLanguage)
  enum : uint32_t
  {
    kEnglish = 1,
    kGerman = 3,
    kFrench = 4,
    kSpanish = 5,
    kItalian = 6,
  };

  // The game's language (XLanguage), resolved once
  uint32_t Current();

  // "english", "french", ...
  const char *Name(uint32_t language);

  // Options > Settings > GENERAL > LANGUAGE: the choices (0 auto, 1 english, 2 french, 3 german, 4 spanish, 5 italian)
  constexpr int kChoices = 6;

  // The configured choice (--sst_language, else sst_settings.ini, else auto); unknown names count as auto
  int Choice();

  // The language a choice means at this boot (auto: the Windows display language, else English)
  uint32_t ChoiceLanguage(int choice);

  // The choice's name in its own language, in the menu's font (A-Z): AUTO, ENGLISH, FRANCAIS, DEUTSCH, ESPANOL, ITALIANO
  const char *ChoiceLabel(int choice);

  // Writes "language = <name>" to sst_settings.ini (other lines kept); used at the next start. False: --sst_language overrides it
  bool SaveChoice(int choice);
}