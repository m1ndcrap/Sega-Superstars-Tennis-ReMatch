// Game language

#include "menu/language.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_STRING(sst_language, "", "SST", "Game language: auto (Windows display language), english, french, german, spanish, italian. Empty: the \"language\" line of sst_settings.ini, else auto. Needs a restart");

namespace sst::lang
{
  namespace
  {
    std::string Lower(std::string s)
    {
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
                     { return char(std::tolower(c)); });

      const size_t a = s.find_first_not_of(" \t\r\""), b = s.find_last_not_of(" \t\r\"");
      return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }

    // 0: auto / unknown
    uint32_t FromName(const std::string &name)
    {
      const std::string n = Lower(name);

      if (n == "english" || n == "en" || n == "eng")
        return kEnglish;

      if (n == "french" || n == "fr" || n == "fra" || n == "francais")
        return kFrench;

      if (n == "german" || n == "de" || n == "ge" || n == "ger" || n == "deutsch")
        return kGerman;

      if (n == "spanish" || n == "es" || n == "sp" || n == "spa" || n == "espanol")
        return kSpanish;

      if (n == "italian" || n == "it" || n == "ita" || n == "italiano")
        return kItalian;

      return 0;
    }

    // "language = x" in sst_settings.ini (the VIDEO page keeps lines it does not own)
    std::string FromSettingsFile()
    {
      std::ifstream f(rex::filesystem::GetExecutableFolder() / "sst_settings.ini");
      std::string line;

      while (f && std::getline(f, line))
      {
        const size_t eq = line.find('=');

        if (eq == std::string::npos || line.empty() || line[0] == '#' || line[0] == ';')
          continue;

        if (Lower(line.substr(0, eq)) == "language")
          return Lower(line.substr(eq + 1));
      }

      return {};
    }

    // The Windows display language, when the game has it (0 otherwise)
    uint32_t FromSystem()
    {
#if defined(_WIN32)
      switch (PRIMARYLANGID(GetUserDefaultUILanguage()))
      {
      case LANG_ENGLISH:
        return kEnglish;

      case LANG_FRENCH:
        return kFrench;

      case LANG_GERMAN:
        return kGerman;

      case LANG_SPANISH:
        return kSpanish;

      case LANG_ITALIAN:
        return kItalian;

      default:
        return 0;
      }
#else
      return 0;
#endif
    }

    std::once_flag g_once;
    uint32_t g_language = kEnglish;

    void Resolve()
    {
      std::string choice = Lower(REXCVAR_GET(sst_language));
      const char *from = "--sst_language";

      if (choice.empty())
      {
        choice = FromSettingsFile();
        from = "sst_settings.ini";
      }

      if (choice.empty())
      {
        choice = "auto";
        from = "default";
      }

      uint32_t language = FromName(choice);

      if (!language)
      {
        if (choice != "auto")
          REXLOG_WARN("[sst-lang] unknown language '{}' (auto, english, french, german, spanish, italian); using auto", choice);

        language = FromSystem();
        g_language = language ? language : kEnglish;
        REXLOG_INFO("[sst-lang] language {} (auto from {}: {})", Name(g_language), from, language ? "the Windows display language" : "the Windows display language is not one of the game's, English");
        return;
      }

      g_language = language;
      REXLOG_INFO("[sst-lang] language {} ({})", Name(g_language), from);
    }
  }

  uint32_t Current()
  {
    std::call_once(g_once, Resolve);
    return g_language;
  }

  const char *Name(uint32_t language)
  {
    switch (language)
    {
    case kEnglish:
      return "english";

    case kFrench:
      return "french";

    case kGerman:
      return "german";

    case kSpanish:
      return "spanish";

    case kItalian:
      return "italian";

    default:
      return "?";
    }
  }

  namespace
  {
    constexpr uint32_t kChoiceLanguages[kChoices] = {0, kEnglish, kFrench, kGerman, kSpanish, kItalian};
    constexpr const char *kChoiceNames[kChoices] = {"auto", "english", "french", "german", "spanish", "italian"};
    constexpr const char *kChoiceLabels[kChoices] = {"AUTO", "ENGLISH", "FRANCAIS", "DEUTSCH", "ESPANOL", "ITALIANO"};
  }

  int Choice()
  {
    std::string choice = Lower(REXCVAR_GET(sst_language));

    if (choice.empty())
      choice = FromSettingsFile();

    const uint32_t language = FromName(choice);

    for (int i = 1; i < kChoices; ++i)
    {
      if (kChoiceLanguages[i] == language)
        return i;
    }

    return 0;
  }

  uint32_t ChoiceLanguage(int choice)
  {
    if (choice > 0 && choice < kChoices)
      return kChoiceLanguages[choice];

    const uint32_t system = FromSystem();
    return system ? system : kEnglish;
  }

  const char *ChoiceLabel(int choice)
  {
    return kChoiceLabels[std::clamp(choice, 0, kChoices - 1)];
  }

  bool SaveChoice(int choice)
  {
    choice = std::clamp(choice, 0, kChoices - 1);
    const auto path = rex::filesystem::GetExecutableFolder() / "sst_settings.ini";
    std::vector<std::string> lines;

    bool replaced = false;
    {
      std::ifstream in(path);
      std::string line;

      while (in && std::getline(in, line))
      {
        if (!line.empty() && line.back() == '\r')
          line.pop_back();

        const size_t eq = line.find('=');

        if (eq != std::string::npos && !line.empty() && line[0] != '#' && line[0] != ';' && Lower(line.substr(0, eq)) == "language")
        {
          if (replaced)
            continue;

          line = std::string("language = ") + kChoiceNames[choice];
          replaced = true;
        }

        lines.push_back(line);
      }
    }

    if (!replaced)
      lines.push_back(std::string("language = ") + kChoiceNames[choice]);

    std::ofstream out(path, std::ios::trunc);

    for (const std::string &line : lines)
      out << line << "\n";

    if (!out)
    {
      REXLOG_ERROR("[sst-lang] could not write {}", path.string());
      return false;
    }

    const bool overridden = !Lower(REXCVAR_GET(sst_language)).empty();
    REXLOG_INFO("[sst-lang] language set to {} (next start){}", kChoiceNames[choice], overridden ? "; --sst_language overrides it" : "");
    return !overridden;
  }
}

// The console's language (XLanguage) -> the player's choice. The game's setup (sub_822C9000) turns it into the language index at 0x82643914 (scene language files, umpire voice default, etc).
extern "C" REX_FUNC(sub_822F4640)
{
  ctx.r3.u64 = sst::lang::Current();
}