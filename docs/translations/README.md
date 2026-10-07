# Translations

## Inuktitut (ᐃᓄᒃᑎᑐᑦ), draft

**Status: a draft that no fluent speaker has reviewed yet.** It was made from published terminology. It should not
be released as finished until someone fluent has read [iu-review.md](iu-review.md).

| File | What it is |
|---|---|
| `iu.tsv` | The source: key, English, Inuktitut in Latin ICI spelling (or syllabics), confidence, note. **Edit this file.** Keys: `fw:` the display, `web:` the phone settings page, `site:` the web flasher. |
| `iu-review.md` | Generated review sheet: every string with syllabics, romanization, confidence and the reasoning. |
| `../../tools/i18n_iu.py` | The generator. `skeleton` adds new or changed English strings to the TSV and keeps translations. `build` writes the syllabics into `main/i18n_strings.h` (3rd argument of each `X()` line), `main/web/index.html` and `web/flash/index.html` (the `iu:` block of `I18N`), then rewrites the review sheet. `test` checks the Latin→syllabics converter on known words. |
| `i18n.lock.json` | For each French and Inuktitut text of the web flasher, a hash of the English it was translated from. Written by `tools/i18n_check.py site --update`; don't edit it. |
| `../../tools/i18n_check.py` | Sync check (CI runs `site`). `site` fails when a flasher text is missing in French or Inuktitut, unknown, or written in the page outside a translated element (names kept in every language carry `translate="no"`), or when the page's HTML and `I18N.en` disagree; it lists the translations whose English changed since. `guide` lists the README sections that changed since the French owner's guide (`docs/guide.fr.md`) was last brought up to date; after updating the guide, run `guide --update`. |

Run them with `export PYTHONIOENCODING=utf-8` (the Windows console is cp1252):

```bash
python tools/i18n_iu.py skeleton   # after adding display, page or flasher strings
python tools/i18n_iu.py build
python tools/i18n_iu.py test
python tools/i18n_check.py site    # the flasher: every text in fr and iu, none stale
```

When an English text of the flasher changes, `i18n_check.py site` lists its French and Inuktitut as stale. Fix the
French in the page and the `site:` row here, run `build`, then `python tools/i18n_check.py site --update`.

### How it was made

- **Sources.** *high* = a word taken as-is from the Tusaalanga glossary (Pirurvik Centre) or from Microsoft's
  Inuktitut (Latin) terminology, which was made with Pirurvik. *medium* = built from such words with common
  affixes (-kuluk "a little", -aluk "a lot", -liq- "starting", -runnanngit- "cannot"). *low* = a best guess.
  Counts at the time of writing: 53 high, 170 medium, 202 low (the flasher's 110 texts: 10, 42, 58).
- **Dialect and spelling.** Nunavut (Qikiqtaaluk) vocabulary as in Tusaalanga. Syllabics follow the ICI standard
  (qq is written ᖅᑭ, ng/nng are ᖏ/ᙱ, ai is written ᐊᐃ). A Nunavik reviewer may prefer other words.
- **Kept in Latin:** Wi-Fi, Radar, OK, UV, QR, API, Android, Easy Connect, Beta, unit symbols (s, min, h, km/h, dB,
  dBm, g), and pollen tree names (shown only in Europe). On the flasher also: USB, USB-C, BOOT, RESET, COM, HTTPS,
  Chrome, Edge, ESP Web Tools and its English *Erase device* box, the board's name, agency names (Environment
  Canada) and the map credit. In the TSV, text between backticks is kept as written; HTML tags pass through.
- **Dates:** full weekday names (Tusaalanga: ᓇᒡᒐᔾᔭᐅ Monday … ᓈᑦᓰᖑᔭᖅ Sunday; no usual short forms, so the short
  names are the full ones) and the borrowed month names (ᔮᓐᓄᐊᕆ …), in English order: "ᐱᖓᑦᓯᖅ, ᐅᑐᐱᕆ 1".
- **Not translated:** Environment Canada alerts (English or French only; Inuktitut shows the English text) and the
  release notes (English).

### Fit checks (done on the board, v1.9.0-iu.0 test build)

Syllabic words are 1.2 to 2.8 times wider than the English. Every screen was captured with `tools/snapshot.py`.
That includes `settings1..3` (Settings scrolled down), `phone` (settings QR) and `setup0` / `setup1` (Wi-Fi
setup), which were added to the snapshot endpoint for this. Strings that didn't fit were shortened, and the TSV note
says so. In those cases the label is less precise than the English:

| String | First try | Now | Why |
|---|---|---|---|
| Hourly column "Temp" | ᐆᓇᕐᓂᖅ | ° | wrapped in the narrow column |
| Settings "Done" | ᐱᔭᕇᖅᓯᒪᔪᖅ | ᐱᔭᕇᖅᑐᖅ | clipped by the circle |
| "Dim when quiet" | ᑖᓂᑭᓪᓕᖅ ᓂᐸᐃᑦᑐᒥ | ᑖᓂᑭᓪᓕᖅ ("dims") | ran into the switch |
| "Wake on pick-up" | ᐃᑭᑦᓯᖅ ᑭᕕᑦᑕᐅᒑᖓᑦ | ᑭᕕᑦᑕᐅᒑᖓᑦ ("when lifted") | ran into the switch |
| "Updates" | ᓄᑖᕈᕆᐊᕐᓃᑦ | ᓄᑖᑦ ("new things") | ran into "Check now >" |
| "Air quality" | ᓯᓚᐅᑉ ᓴᓗᒻᒪᓂᖓ | ᐊᓂᕐᓴᖅ ("air one breathes") | wrapped in the 160 px label column |
| Moon phases | ᑕᖅᑭᖅ … | without ᑕᖅᑭᖅ | the row label already says moon |
| Status Wi-Fi line | … ᐊᐅᓚᔪᖅ 1 min | … · 1 min | wrapped |

The settings page passes `tests/language.spec.js` › *the page in Inuktitut fits*. That test checks that no English is
left over, that no element overflows and that the page is no wider than a 390 px phone. Its screenshot is
`tools/webtest/shots/review_iu_page.png`.

### Web flasher (`site:` rows)

The flasher page (`web/flash/index.html`) has a language picker (English · Français · ᐃᓄᒃᑎᑐᑦ) and shows a *Draft*
note with a link here when Inuktitut is chosen. Its sentences are kept short and reuse the display's words (screen
names, Wi-Fi setup, Settings rows), so that a step names what the screen shows. Most rows are *low*: please read the
steps (`site:step*`, `site:trouble*`) first, since someone may flash a board by following them. The release notes
stay in English (a note says so). The page in Inuktitut was checked at 1280 and 375 px wide: no English left, nothing
clipped, no sideways scroll.

### Font

Montserrat has no syllabics. `main/syllabics.ttf` (Noto Sans Canadian Aboriginal, subset to U+1400–167F, 75 KB,
OFL) is set as each TinyTTF font's `fallback` in `mkfont()` (`ui.c`). It is drawn 5/4 larger because Noto's
syllabics are drawn at about x-height and looked small next to Montserrat's capitals. Browsers use their own
syllabics font for the pages: the flasher names Gadugi (Windows), Euphemia UCAS (Apple) and Noto Sans Canadian
Aboriginal (Android, Linux) after Montserrat, so Latin words keep the display's font.

### Open questions for a reviewer

- Nunavut or Nunavik usage? Is the ICI spelling right for the intended readers?
- Words with no settled term: air quality levels, "feels like", moon phases, firmware, stable/beta channels,
  Wi-Fi network, error messages.
- The tone of commands: the draft uses the polite imperative (‑li, as Microsoft does) for buttons.
