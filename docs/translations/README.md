# Translations

## Inuktitut (ᐃᓄᒃᑎᑐᑦ), draft

**Status: a draft that no fluent speaker has reviewed yet.** It was made from published terminology. It should not
be released as finished until someone fluent has read [iu-review.md](iu-review.md).

| File | What it is |
|---|---|
| `iu.tsv` | The source: key, English, Inuktitut in Latin ICI spelling (or syllabics), confidence, note. **Edit this file.** |
| `iu-review.md` | Generated review sheet: every string with syllabics, romanization, confidence and the reasoning. |
| `../../tools/i18n_iu.py` | The generator. `skeleton` adds new or changed English strings to the TSV and keeps translations. `build` writes the syllabics into `main/i18n_strings.h` (3rd argument of each `X()` line) and `main/web/index.html` (the `iu:` block of `I18N`), then rewrites the review sheet. `test` checks the Latin→syllabics converter on known words. |

Run them with `export PYTHONIOENCODING=utf-8` (the Windows console is cp1252):

```bash
python tools/i18n_iu.py skeleton   # after adding display or page strings
python tools/i18n_iu.py build
python tools/i18n_iu.py test
```

### How it was made

- **Sources.** *high* = a word taken as-is from the Tusaalanga glossary (Pirurvik Centre) or from Microsoft's
  Inuktitut (Latin) terminology, which was made with Pirurvik. *medium* = built from such words with common
  affixes (-kuluk "a little", -aluk "a lot", -liq- "starting", -runnanngit- "cannot"). *low* = a best guess.
  Counts at the time of writing: 43 high, 128 medium, 143 low.
- **Dialect and spelling.** Nunavut (Qikiqtaaluk) vocabulary as in Tusaalanga. Syllabics follow the ICI standard
  (qq is written ᖅᑭ, ng/nng are ᖏ/ᙱ, ai is written ᐊᐃ). A Nunavik reviewer may prefer other words.
- **Kept in Latin:** Wi-Fi, Radar, OK, UV, QR, API, Android, Easy Connect, Beta, unit symbols (s, min, h, km/h, dB,
  dBm, g), and pollen tree names (shown only in Europe). In the TSV, text between backticks is kept as written.
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

### Font

Montserrat has no syllabics. `main/syllabics.ttf` (Noto Sans Canadian Aboriginal, subset to U+1400–167F, 75 KB,
OFL) is set as each TinyTTF font's `fallback` in `mkfont()` (`ui.c`). It is drawn 5/4 larger because Noto's
syllabics are drawn at about x-height and looked small next to Montserrat's capitals. Browsers use their own
syllabics font for the page.

### Open questions for a reviewer

- Nunavut or Nunavik usage? Is the ICI spelling right for the intended readers?
- Words with no settled term: air quality levels, "feels like", moon phases, firmware, stable/beta channels,
  Wi-Fi network, error messages.
- The tone of commands: the draft uses the polite imperative (‑li, as Microsoft does) for buttons.
