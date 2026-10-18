// The port's own texts in the game's language

#include "menu/port_text.h"

#include "menu/guest.h"
#include "menu/language.h"

#include <rex/logging.h>

#include <fmt/format.h>

#include <array>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

namespace sst::text
{
  namespace
  {
    using sst::menu::Load32;

    // English -> French, German, Spanish, Italian. Menu labels and values: plain A-Z, short (the label column is about 14 letters wide)
    struct Entry
    {
      const char *en, *fr, *de, *es, *it;
    };

    constexpr Entry kTexts[] = {
        // Settings > VIDEO (and the pause menu's VIDEO page)
        {"QUALITY PRESET", "QUALITE", "QUALITAET", "CALIDAD", "QUALITA"},
        {"PRESET", "QUALITE", "QUALITAET", "CALIDAD", "QUALITA"},
        {"FULLSCREEN", "PLEIN ECRAN", "VOLLBILD", "PANT. COMPLETA", "SCHERMO INTERO"},
        {"SHADOWS", "OMBRES", "SCHATTEN", "SOMBRAS", "OMBRE"},
        {"BLOOM", "BLOOM", "BLOOM", "RESPLANDOR", "BAGLIORE"},
        {"SHADER QUALITY", "SHADERS", "SHADER", "SHADERS", "SHADER"},
        {"RESOLUTION", "RESOLUTION", "AUFLOESUNG", "RESOLUCION", "RISOLUZIONE"},
        {"ASPECT RATIO", "FORMAT", "BILDFORMAT", "FORMATO", "FORMATO"},
        {"RENDER SCALE", "ECHELLE RENDU", "RENDERSKALA", "ESCALA RENDER", "SCALA RENDER"},
        {"ANTI-ALIASING", "ANTICRENELAGE", "KANTENGLAETT.", "ANTIALIASING", "ANTIALIASING"},
        {"AA LEVEL", "NIVEAU AA", "AA-STUFE", "NIVEL AA", "LIVELLO AA"},
        {"FPS LIMIT", "LIMITE FPS", "FPS-LIMIT", "LIMITE FPS", "LIMITE FPS"},
        {"FPS COUNTER", "COMPTEUR FPS", "FPS-ANZEIGE", "CONTADOR FPS", "CONTATORE FPS"},
        {"VSYNC", "VSYNC", "VSYNC", "VSYNC", "VSYNC"},
        {"BRIGHTNESS", "LUMINOSITE", "HELLIGKEIT", "BRILLO", "LUMINOSITA"},
        {"LOW", "BASSE", "NIEDRIG", "BAJA", "BASSA"},
        {"MEDIUM", "MOYENNE", "MITTEL", "MEDIA", "MEDIA"},
        {"HIGH", "HAUTE", "HOCH", "ALTA", "ALTA"},
        {"CUSTOM", "PERSO", "EIGENE", "PERSONAL.", "PERSONAL."},
        {"ON", "OUI", "AN", "SI", "SI"},
        {"OFF", "NON", "AUS", "NO", "NO"},
        {"FAST", "RAPIDE", "SCHNELL", "RAPIDO", "VELOCE"},
        {"ACCURATE", "PRECIS", "GENAU", "PRECISO", "PRECISO"},
        {"N/A", "N/D", "K.A.", "N/D", "N/D"},
        {"NONE", "AUCUN", "KEINE", "NINGUNO", "NESSUNO"},
        {"RESTART", "REDEMARRER", "NEUSTART", "REINICIAR", "RIAVVIO"},

        // Settings > GENERAL
        {"LANGUAGE", "LANGUE", "SPRACHE", "IDIOMA", "LINGUA"},

        // CONTROLS > KEY BINDINGS
        {"KEY BINDINGS", "TOUCHES", "TASTENBELEGUNG", "TECLAS", "TASTI"},
        {"Key Bindings", "Touches", "Tastenbelegung", "Teclas", "Tasti"},
        {"Reset to Defaults", "Par défaut", "Standard", "Predeterminado", "Predefiniti"},
        {"KEYBOARD PLAYER", "JOUEUR CLAVIER", "TASTATUR", "TECLADO", "TASTIERA"},
        {"SEPARATE", "SEPARE", "GETRENNT", "SEPARADO", "SEPARATO"},
        {"SHARED WITH 1P", "AVEC 1P", "MIT 1P", "CON 1P", "CON 1P"},
        {"PRESS A KEY", "APPUYEZ", "TASTE DRUECKEN", "PULSA TECLA", "PREMI TASTO"},
        {"MOVE UP", "MOUV. HAUT", "NACH OBEN", "MOVER ARRIBA", "MUOVI SU"},
        {"MOVE DOWN", "MOUV. BAS", "NACH UNTEN", "MOVER ABAJO", "MUOVI GIU"},
        {"MOVE LEFT", "MOUV. GAUCHE", "NACH LINKS", "MOVER IZQ.", "MUOVI SINISTRA"},
        {"MOVE RIGHT", "MOUV. DROITE", "NACH RECHTS", "MOVER DER.", "MUOVI DESTRA"},
        {"A BUTTON", "BOUTON A", "TASTE A", "BOTON A", "TASTO A"},
        {"B BUTTON", "BOUTON B", "TASTE B", "BOTON B", "TASTO B"},
        {"X BUTTON", "BOUTON X", "TASTE X", "BOTON X", "TASTO X"},
        {"Y BUTTON", "BOUTON Y", "TASTE Y", "BOTON Y", "TASTO Y"},
        {"LEFT BUMPER", "BOUTON LB", "TASTE LB", "BOTON LB", "TASTO LB"},
        {"RIGHT BUMPER", "BOUTON RB", "TASTE RB", "BOTON RB", "TASTO RB"},
        {"LEFT TRIGGER", "GACHETTE LT", "TRIGGER LT", "GATILLO LT", "GRILLETTO LT"},
        {"RIGHT TRIGGER", "GACHETTE RT", "TRIGGER RT", "GATILLO RT", "GRILLETTO RT"},
        {"BACK BUTTON", "BOUTON BACK", "TASTE BACK", "BOTON BACK", "TASTO BACK"},
        {"START BUTTON", "BOUTON START", "TASTE START", "BOTON START", "TASTO START"},
        {"D-PAD UP", "CROIX HAUT", "KREUZ OBEN", "CRUCETA ARRIBA", "CROCE SU"},
        {"D-PAD DOWN", "CROIX BAS", "KREUZ UNTEN", "CRUCETA ABAJO", "CROCE GIU"},
        {"D-PAD LEFT", "CROIX GAUCHE", "KREUZ LINKS", "CRUCETA IZQ.", "CROCE SINISTRA"},
        {"D-PAD RIGHT", "CROIX DROITE", "KREUZ RECHTS", "CRUCETA DER.", "CROCE DESTRA"},

        // Main menu
        {"CONTROLS", "COMMANDES", "STEUERUNG", "CONTROLES", "COMANDI"},
        {"ONLINE PLAY", "JEU EN LIGNE", "ONLINE-SPIEL", "JUEGO ONLINE", "GIOCO ONLINE"},
        {"QUIT", "QUITTER", "BEENDEN", "SALIR", "ESCI"},
        {"Close the game and return to the desktop.", "Fermer le jeu et revenir au bureau.",
         "Das Spiel beenden und zum Desktop zurückkehren.", "Cerrar el juego y volver al escritorio.",
         "Chiudi il gioco e torna al desktop."},
        {"Are you sure you want to quit to the desktop?", "Voulez-vous vraiment quitter et revenir au bureau ?",
         "Möchtest du das Spiel wirklich beenden?", "¿Seguro que quieres salir al escritorio?",
         "Vuoi davvero uscire e tornare al desktop?"},

        // Restart dialog
        {"RESTART REQUIRED", "REDÉMARRAGE REQUIS", "NEUSTART ERFORDERLICH", "REINICIO NECESARIO", "RIAVVIO NECESSARIO"},
        {"The game needs to restart to apply the new settings. Restart now?",
         "Le jeu doit red\u00e9marrer pour appliquer les nouveaux param\u00e8tres. Red\u00e9marrer maintenant ?",
         "Das Spiel muss neu gestartet werden, um die neuen Einstellungen zu \u00fcbernehmen. Jetzt neu starten?",
         "El juego debe reiniciarse para aplicar los nuevos ajustes. \u00bfReiniciar ahora?",
         "Il gioco deve essere riavviato per applicare le nuove impostazioni. Riavviare ora?"},
        {"The game will now restart to apply the new settings.", "Le jeu va redémarrer pour appliquer les nouveaux paramètres.",
         "Das Spiel wird neu gestartet, um die neuen Einstellungen zu übernehmen.",
         "El juego se reiniciará para aplicar los nuevos ajustes.",
         "Il gioco verrà riavviato per applicare le nuove impostazioni."},

        // Achievements
        {"ACHIEVEMENT UNLOCKED", "SUCCÈS DÉBLOQUÉ", "ERFOLG FREIGESCHALTET", "LOGRO DESBLOQUEADO", "OBIETTIVO SBLOCCATO"},
        {"UNLOCKED", "DÉBLOQUÉ", "FREIGESCHALTET", "DESBLOQUEADO", "SBLOCCATO"},
    };

    // The translation column for the game's language (-1: English)
    int Column()
    {
      switch (sst::lang::Current())
      {
      case sst::lang::kFrench:
        return 0;

      case sst::lang::kGerman:
        return 1;

      case sst::lang::kSpanish:
        return 2;

      case sst::lang::kItalian:
        return 3;

      default:
        return -1;
      }
    }

    const std::unordered_map<std::string_view, const Entry *> &Index()
    {
      static const auto index = []
      {
        std::unordered_map<std::string_view, const Entry *> m;

        for (const Entry &e : kTexts)
          m.emplace(e.en, &e);

        return m;
      }();
      return index;
    }

    uint8_t *GuestBase()
    {
      auto *memory = rex::system::kernel_memory();
      return memory ? memory->virtual_membase() : nullptr;
    }

    // Language files and their fonts' characters
    struct Fonts
    {
      std::mutex mutex;
      std::map<std::string, uint32_t, std::less<>> files;                                // stem -> buffer (the last read)
      std::map<std::string, std::map<uint32_t, std::set<uint32_t>>, std::less<>> glyphs; // stem -> font (0: all) -> characters
    } g_fonts;

    constexpr uint32_t kChunkMagic = 0x44332211, kFontTag = 0x464F4E54; // "FONT"

    // The fonts of the language file at `buffer` -> characters per font (and all under 0); false when it doesn't look like one (not read yet, freed)
    bool ReadFonts(uint8_t *base, uint32_t buffer, std::map<uint32_t, std::set<uint32_t>> &out)
    {
      uint32_t at = buffer;

      for (int n = 0; n < 64; ++n)
      {
        const uint32_t tag = Load32(base, at), size = Load32(base, at + 4), data = Load32(base, at + 8);

        if (!tag || size < 16 || size > 0x400000 || Load32(base, at + 12) != kChunkMagic)
          return false;

        if (tag == kFontTag)
        {
          const uint32_t d = at + 16, count = Load32(base, d + 8);

          if (count == 0 || count > 64)
            return false;

          for (uint32_t f = 0; f < count; ++f)
          {
            const uint32_t e = d + 12 + 0x20 * f;
            const uint32_t hash = Load32(base, e), glyphs = Load32(base, e + 4) >> 16, go = Load32(base, e + 8);

            // (the glyph offset is chunk-relative in the file; the loader may have made it an address)
            const uint32_t glyph_base = go >= buffer ? go : d + go;

            if (glyphs > 0x800 || (go < buffer && go + 0x18 * glyphs > data))
              return false;

            for (uint32_t i = 0; i < glyphs; ++i)
            {
              const uint32_t ch = Load32(base, glyph_base + 0x18 * i + 4) >> 16;
              out[hash].insert(ch);
              out[0].insert(ch);
            }
          }

          return !out.empty();
        }

        at += size;
      }

      return false;
    }

    // Scene `stem`'s characters (font, or 0: all); null when unknown. Under g_fonts.mutex
    const std::set<uint32_t> *Glyphs(uint8_t *base, std::string_view stem, uint32_t font)
    {
      auto known = g_fonts.glyphs.find(stem);

      if (known == g_fonts.glyphs.end())
      {
        const auto file = g_fonts.files.find(stem);

        if (file == g_fonts.files.end())
          return nullptr;

        std::map<uint32_t, std::set<uint32_t>> fonts;

        if (!ReadFonts(base, file->second, fonts))
          return nullptr;

        REXLOG_INFO("[sst-text] {} language file: {} fonts", stem, fonts.size() - 1);
        known = g_fonts.glyphs.emplace(std::string(stem), std::move(fonts)).first;
      }

      const auto it = known->second.find(font);
      return it != known->second.end() ? &it->second : nullptr;
    }

    uint32_t NextCodePoint(const std::string &s, size_t &i)
    {
      const uint8_t c = uint8_t(s[i++]);

      if (c < 0x80)
        return c;

      const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2
                                    : c >= 0xC0   ? 1
                                                  : -1;

      if (extra < 0)
        return 0xFFFD;

      uint32_t cp = c & (0x3F >> extra);

      for (int k = 0; k < extra; ++k)
      {
        if (i >= s.size() || (uint8_t(s[i]) & 0xC0) != 0x80)
          return 0xFFFD;

        cp = (cp << 6) | (uint8_t(s[i++]) & 0x3F);
      }

      return cp;
    }

    void AppendUtf8(std::string &out, uint32_t cp)
    {
      if (cp < 0x80)
      {
        out += char(cp);
      }
      else if (cp < 0x800)
      {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
      }
      else
      {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
      }
    }

    // Latin-1 letter -> its plain letter(s) ("e" for é, "ss" for ß); empty if none
    std::string Plain(uint32_t cp)
    {
      static const char *const kUpper = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPs"; // U+00C0..U+00DF
      static const char *const kLower = "aaaaaaaceeeeiiiidnooooo/ouuuuypy"; // U+00E0..U+00FF

      // (German umlauts: the usual AE / OE / UE)
      if (cp == 0xC4 || cp == 0xC6)
        return "AE";

      if (cp == 0xD6)
        return "OE";

      if (cp == 0xDC)
        return "UE";

      if (cp == 0xE4)
        return "ae";

      if (cp == 0xF6)
        return "oe";

      if (cp == 0xFC)
        return "ue";

      if (cp == 0xE6)
        return "ae";

      if (cp == 0xDF)
        return "ss";

      if (cp == 0x152)
        return "OE";

      if (cp == 0x153)
        return "oe";

      if (cp == 0xBF || cp == 0xA1)
        return "";

      if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7)
        return std::string(1, kUpper[cp - 0xC0]);

      if (cp >= 0xE0 && cp <= 0xFF && cp != 0xF7)
        return std::string(1, kLower[cp - 0xE0]);

      if (cp == 0x2019)
        return "'";

      return {};
    }

    uint32_t Upper(uint32_t cp)
    {
      if (cp >= 'a' && cp <= 'z')
        return cp - 32;

      if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7)
        return cp - 32;

      return cp;
    }
  }

  std::string Tr(std::string_view english)
  {
    const int column = Column();

    if (column < 0)
      return std::string(english);

    // "<text> (RESTART)": both parts
    constexpr std::string_view kRestart = " (RESTART)";

    if (english.size() > kRestart.size() && english.ends_with(kRestart))
      return Tr(english.substr(0, english.size() - kRestart.size())) + " (" + Tr("RESTART") + ")";

    const auto &index = Index();
    const auto it = index.find(english);

    if (it == index.end())
      return std::string(english);

    const Entry &e = *it->second;
    const char *const columns[4] = {e.fr, e.de, e.es, e.it};
    return columns[column];
  }

  void OnSceneFile(const std::string &name, uint32_t buffer)
  {
    // <stem>_<xx>.dat
    if (!buffer || name.size() < 8 || !name.ends_with(".dat") || name[name.size() - 7] != '_')
      return;

    std::lock_guard lock(g_fonts.mutex);
    const std::string stem = name.substr(0, name.size() - 7);

    if (!g_fonts.glyphs.count(stem))
      g_fonts.files[stem] = buffer;
  }

  bool FontKnown(uint8_t *base, std::string_view stem, uint32_t font)
  {
    if (!base)
      base = GuestBase();

    std::lock_guard lock(g_fonts.mutex);
    return Glyphs(base, stem, font) != nullptr;
  }

  bool HasChar(uint8_t *base, std::string_view stem, uint32_t font, uint32_t cp)
  {
    if (!base)
      base = GuestBase();

    if (!base)
      return true;

    std::lock_guard lock(g_fonts.mutex);
    const std::set<uint32_t> *glyphs = Glyphs(base, stem, font);
    return !glyphs || glyphs->count(cp) != 0;
  }

  std::string Fit(uint8_t *base, const std::string &text, std::string_view stem, uint32_t font)
  {
    if (!base)
      base = GuestBase();

    if (!base)
      return text;

    std::lock_guard lock(g_fonts.mutex);
    const std::set<uint32_t> *glyphs = Glyphs(base, stem, font);

    if (!glyphs)
      return text;

    auto has = [&](uint32_t cp)
    { return cp == ' ' || glyphs->count(cp) != 0; };

    std::string out;

    for (size_t i = 0; i < text.size();)
    {
      const uint32_t cp = NextCodePoint(text, i);

      if (has(cp))
      {
        AppendUtf8(out, cp);
        continue;
      }

      if (has(Upper(cp)))
      {
        AppendUtf8(out, Upper(cp));
        continue;
      }

      // An accented letter the font lacks: its plain letter(s), as they are or in upper case
      const std::string plain = Plain(cp);
      bool ok = !plain.empty();

      for (char c : plain)
        ok = ok && (has(uint8_t(c)) || has(Upper(uint8_t(c))));

      if (ok)
      {
        for (char c : plain)
          AppendUtf8(out, has(uint8_t(c)) ? uint32_t(uint8_t(c)) : Upper(uint8_t(c)));
      }
    }

    // No doubled or edge spaces from dropped characters
    std::string clean;

    for (char c : out)
    {
      if (c == ' ' && (clean.empty() || clean.back() == ' '))
        continue;

      clean += c;
    }

    while (!clean.empty() && clean.back() == ' ')
      clean.pop_back();

    return clean.empty() ? text : clean;
  }

  std::string DateTime(int year, int month, int day, int hour, int minute)
  {
    switch (sst::lang::Current())
    {
    case sst::lang::kGerman:
      return fmt::format("{:02}.{:02}.{}  {:02}:{:02}", day, month, year, hour, minute);

    case sst::lang::kFrench:

    case sst::lang::kSpanish:

    case sst::lang::kItalian:
      return fmt::format("{:02}/{:02}/{}  {:02}:{:02}", day, month, year, hour, minute);

    default:
    {
      const int h12 = hour % 12 == 0 ? 12 : hour % 12;
      return fmt::format("{:02}/{:02}/{}  {}:{:02} {}", month, day, year, h12, minute, hour < 12 ? "AM" : "PM");
    }
    }
  }
}