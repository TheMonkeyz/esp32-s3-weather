#!/usr/bin/env python3
"""Inuktitut translation: one editable source, generated into the firmware, the settings page and the web flasher.

Source: docs/translations/iu.tsv, one row per string (tab-separated):
    key  english  inuktitut  confidence  note
  key         fw:T_ID (display, main/i18n_strings.h), web:key (settings page, main/web/index.html) or
              site:key (web flasher, web/flash/index.html)
  english     the English text (for the reviewer; refreshed by `skeleton`)
  inuktitut   Inuktitut in Latin letters (ICI standard roman orthography), or already in syllabics.
              Latin is converted to syllabics. Keep unchanged: %d %s %% \\n, digits, punctuation, and anything in
              `backticks` (product names, units: `Wi-Fi`, `km/h`, `OK`). {0} {1} are the page's placeholders.
              HTML tags (<b>, <a href="...">) and entities (&amp;) are kept as written; Latin words between tags
              still need backticks.
  confidence  high (from a dictionary / Microsoft terminology), medium (built from dictionary words), low (guess)
  note        source or what a reviewer should check

    python tools/i18n_iu.py skeleton   add rows for new strings (keeps translations), refresh the English column
    python tools/i18n_iu.py build      write the Inuktitut column into i18n_strings.h and the iu block of I18N in
                                       both pages, and docs/translations/iu-review.md (a table for a fluent reviewer)
    python tools/i18n_iu.py test       check the Latin -> syllabics converter on known dictionary words

Standard library only.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TSV = os.path.join(ROOT, 'docs', 'translations', 'iu.tsv')
STRINGS = os.path.join(ROOT, 'main', 'i18n_strings.h')
PAGE = os.path.join(ROOT, 'main', 'web', 'index.html')
SITE = os.path.join(ROOT, 'web', 'flash', 'index.html')
PAGES = (('web:', PAGE), ('site:', SITE))    # key prefix, page with an I18N = {en, fr, iu} table
REVIEW = os.path.join(ROOT, 'docs', 'translations', 'iu-review.md')

# ---------------- Latin -> syllabics (Inuktitut, ICI / Nunavut orthography) ----------------
# columns: i ii u uu a aa final
ROWS = {
    '':    'ᐃᐄᐅᐆᐊᐋ',
    'p':   'ᐱᐲᐳᐴᐸᐹᑉ',
    't':   'ᑎᑏᑐᑑᑕᑖᑦ',
    'k':   'ᑭᑮᑯᑰᑲᑳᒃ',
    'g':   'ᒋᒌᒍᒎᒐᒑᒡ',
    'm':   'ᒥᒦᒧᒨᒪᒫᒻ',
    'n':   'ᓂᓃᓄᓅᓇᓈᓐ',
    's':   'ᓯᓰᓱᓲᓴᓵᔅ',
    'l':   'ᓕᓖᓗᓘᓚᓛᓪ',
    'j':   'ᔨᔩᔪᔫᔭᔮᔾ',
    'v':   'ᕕᕖᕗᕘᕙᕚᕝ',
    'r':   'ᕆᕇᕈᕉᕋᕌᕐ',
    'q':   'ᕿᖀᖁᖂᖃᖄᖅ',
    'ng':  'ᖏᖐᖑᖒᖓᖔᖕ',
    'nng': 'ᙱᙲᙳᙴᙵᙶᖖ',
    'ł':   'ᖠᖡᖢᖣᖤᖥᖦ',
}
VOWELS = ['ii', 'uu', 'aa', 'i', 'u', 'a']
VCOL = {'i': 0, 'ii': 1, 'u': 2, 'uu': 3, 'a': 4, 'aa': 5}
CONS = sorted(ROWS, key=len, reverse=True)   # longest first: nng, ng, ...


def word_to_syl(w):
    w = w.lower().replace('lh', 'ł')
    out, i = [], 0
    while i < len(w):
        if w.startswith('qq', i):                 # qq is written q-final + k-syllable (ᖅᑭ)
            out.append('ᖅ')
            i += 1
            w = w[:i] + 'k' + w[i + 1:]
            continue
        c = next((c for c in CONS if c and w.startswith(c, i)), '')
        j = i + len(c)
        v = next((v for v in VOWELS if w.startswith(v, j)), None)
        if v:
            out.append(ROWS[c][VCOL[v]])
            i = j + len(v)
        elif c:
            out.append(ROWS[c][6])
            i = j
        elif w[i] == 'h':
            out.append('ᕼ')
            i += 1
        else:
            raise ValueError(f'cannot write "{w[i:]}" of "{w}" in syllabics')
    return ''.join(out)


TOKEN = re.compile(r"(`[^`]*`|<[^<>]*>|&#?\w+;|%%|%[0-9]*[sd]|\\n|\{\d\}|[A-Za-zł']+)")


def latin_to_syl(text):
    if any('᐀' <= ch <= 'ᙿ' for ch in text):
        return text                               # already syllabics
    parts = []
    for tok in TOKEN.split(text):
        if not tok:
            continue
        if tok.startswith('`'):
            parts.append(tok[1:-1])
        elif tok[0] in '<&' and len(tok) > 1:     # an HTML tag or entity
            parts.append(tok)
        elif re.fullmatch(r"[A-Za-zł']+", tok) and not re.fullmatch(r'%[0-9]*[sd]|%%', tok):
            parts.append(word_to_syl(tok.replace("'", '')))
        else:
            parts.append(tok)
    return ''.join(parts)


# ---------------- sources ----------------
def c_strings(s):
    return ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', s))


def parse_x_entries(src):
    """[(id, [arg strings as written], start, end)] for each X(...) entry, string-aware."""
    out = []
    for m in re.finditer(r'^X\((T_\w+),', src, re.M):
        i, depth, args, cur, in_str = m.end(), 1, [], '', False
        while depth:
            ch = src[i]
            if in_str:
                cur += ch
                if ch == '\\':
                    cur += src[i + 1]
                    i += 1
                elif ch == '"':
                    in_str = False
            elif ch == '"':
                in_str, cur = True, cur + ch
            elif ch == '(':
                depth += 1
                cur += ch
            elif ch == ')':
                depth -= 1
                if depth:
                    cur += ch
            elif ch == ',' and depth == 1:
                args.append(cur.strip())
                cur = ''
            else:
                cur += ch
            i += 1
        args.append(cur.strip())
        out.append((m.group(1), args, m.start(), i))
    return out


def page_dict(html, lang):
    i = html.index(f' {lang}: {{')
    j = html.index('\n },', i)
    block = html[i:j]
    d = {}
    for m in re.finditer(r"(\w+): ('(?:[^'\\]|\\.)*'|\"(?:[^\"\\]|\\.)*\")", block):
        v = m.group(2)
        d[m.group(1)] = v[1:-1].replace("\\'", "'").replace('\\"', '"')
    return d


def sources():
    items = []
    for tid, args, _, _ in parse_x_entries(open(STRINGS, encoding='utf-8').read()):
        items.append(('fw:' + tid, c_strings(args[0]).replace('\t', ' ')))
    for prefix, path in PAGES:
        for k, v in page_dict(open(path, encoding='utf-8').read(), 'en').items():
            items.append((prefix + k, v.replace('\t', ' ')))
    return items


def read_tsv():
    rows = []
    if os.path.exists(TSV):
        for line in open(TSV, encoding='utf-8').read().splitlines():
            if not line.strip() or line.startswith('#'):
                continue
            f = (line.split('\t') + [''] * 5)[:5]
            rows.append(f)
    return rows


def write_tsv(rows):
    head = ('# Inuktitut translation source (see tools/i18n_iu.py). Columns: key, english, inuktitut (Latin ICI or\n'
            '# syllabics), confidence (high / medium / low), note. Edit the inuktitut column, then run the build.\n')
    with open(TSV, 'w', encoding='utf-8', newline='\n') as f:
        f.write(head)
        for r in rows:
            f.write('\t'.join(r) + '\n')


# ---------------- commands ----------------
def skeleton():
    old = {r[0]: r for r in read_tsv()}
    rows = []
    for key, en in sources():
        r = old.get(key, [key, en, '', '', ''])
        r[1] = en
        rows.append(r)
    write_tsv(rows)
    print(f'{len(rows)} rows, {sum(1 for r in rows if not r[2])} untranslated')


def c_escape(s):
    return s.replace('\\n', '\n').encode('unicode_escape').decode('ascii') if False else \
        s.replace('"', '\\"')


def write_page_iu(path, prefix, rows, syl):
    """Replace the iu block of the page's I18N table (or add it after fr). Returns the number of strings."""
    html = open(path, encoding='utf-8').read()
    lines = []
    for key, en, iu, conf, note in rows:
        if key.startswith(prefix) and key in syl:
            v = syl[key].replace('\\', '\\\\').replace("'", "\\'")
            lines.append(f"  {key[len(prefix):]}: '{v}',")
    block = ' iu: {\n' + '\n'.join(lines) + '\n },\n'
    if ' iu: {' in html:
        i = html.index(' iu: {')
        html = html[:i] + block + html[html.index('\n },', i) + 4:].lstrip('\n')
    else:
        end = html.index('\n },', html.index(' fr: {')) + 4
        html = html[:end] + '\n' + block.rstrip('\n') + html[end:]
    open(path, 'w', encoding='utf-8', newline='').write(html)
    return len(lines)


def build():
    rows = read_tsv()
    syl = {}
    for key, en, iu, conf, note in rows:
        if iu:
            syl[key] = latin_to_syl(iu)
    # firmware: third argument of each X() line
    src = open(STRINGS, encoding='utf-8').read()
    out, pos, n = [], 0, 0
    for tid, args, a, b in parse_x_entries(src):
        out.append(src[pos:a])
        text = syl.get('fw:' + tid)
        base = src[a:b].rstrip()[:-1]              # the line as written, without ")"
        if len(args) > 2:                          # drop the old Inuktitut argument
            base = base[:base.rindex(args[2])].rstrip().rstrip(',')
        if text is not None:
            n += 1
            lit = '"' + text.replace('"', '\\"') + '"'
            out.append(base + f',\n                    {lit})')
        else:
            out.append(base + ')')
        pos = b
    out.append(src[pos:])
    open(STRINGS, 'w', encoding='utf-8', newline='').write(''.join(out))
    # pages: the iu block of each I18N table
    counts = {prefix: write_page_iu(path, prefix, rows, syl) for prefix, path in PAGES}
    # review sheet
    with open(REVIEW, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# Inuktitut translation: review sheet\n\n'
                'Generated by `python tools/i18n_iu.py build` from `docs/translations/iu.tsv` (edit that file, not this\n'
                'one). **This is a draft prepared without a fluent speaker.** Words marked *high* come from the\n'
                'Tusaalanga glossary or Microsoft\'s Inuktitut terminology (made with the Pirurvik Centre); *medium* are\n'
                'built from such words; *low* are best guesses. Please check meaning, grammar and dialect, and replace\n'
                'anything that reads wrong.\n\n'
                '| where | English | Inuktitut | romanization | confidence | note |\n|---|---|---|---|---|---|\n')
        for key, en, iu, conf, note in rows:
            cell = lambda s: s.replace('|', '\\|').replace('\\n', ' ⏎ ')
            f.write(f'| `{key}` | {cell(en)} | {cell(syl.get(key, ""))} | {cell(iu if iu and not any(chr(0x1400) <= ch <= chr(0x167f) for ch in iu) else "")} | {conf} | {cell(note)} |\n')
    missing = [r[0] for r in rows if not r[2]]
    print(f'firmware strings: {n}, settings page strings: {counts["web:"]}, flasher site strings: {counts["site:"]}, '
          f'untranslated: {len(missing)}')
    if missing:
        print('untranslated:', ', '.join(missing))


KNOWN = {  # dictionary pairs (Tusaalanga glossary) to check the converter
    'silaqqiqtuq': 'ᓯᓚᖅᑭᖅᑐᖅ', 'naatsiingujaq': 'ᓈᑦᓰᖑᔭᖅ', 'anuraaqtuq': 'ᐊᓄᕌᖅᑐᖅ', 'uqaalautiralaaq': 'ᐅᖄᓚᐅᑎᕋᓛᖅ',
    'naggajjau': 'ᓇᒡᒐᔾᔭᐅ', 'aippiq': 'ᐊᐃᑉᐱᖅ', 'pingatsiq': 'ᐱᖓᑦᓯᖅ', 'sitammiq': 'ᓯᑕᒻᒥᖅ', 'tallirmiq': 'ᑕᓪᓕᕐᒥᖅ',
    'sivataarvik': 'ᓯᕙᑖᕐᕕᒃ', 'jaannuari': 'ᔮᓐᓄᐊᕆ', 'viivvuari': 'ᕖᕝᕗᐊᕆ', 'aaggiisi': 'ᐋᒡᒌᓯ', 'tatsiqtuq': 'ᑕᑦᓯᖅᑐᖅ',
    'qanniqtuq': 'ᖃᓐᓂᖅᑐᖅ', 'silaluttuq': 'ᓯᓚᓗᑦᑐᖅ', 'siqiniq': 'ᓯᕿᓂᖅ', 'nuvujajuq': 'ᓄᕗᔭᔪᖅ', 'ullumi': 'ᐅᓪᓗᒥ',
    'piqsinngittuq': 'ᐱᖅᓯᙱᑦᑐᖅ', 'qallunaatitut': 'ᖃᓪᓗᓈᑎᑐᑦ', 'uivititut': 'ᐅᐃᕕᑎᑐᑦ',   # (the glossary's ikarraaq / ᐃᑲᕐᕋᖅ disagree: long aa vs short)
    'inuktitut': 'ᐃᓄᒃᑎᑐᑦ', 'nunalik': 'ᓄᓇᓕᒃ', 'siqiniq nuijuq': 'ᓯᕿᓂᖅ ᓄᐃᔪᖅ', 'qaumajuq': 'ᖃᐅᒪᔪᖅ',
}


def test():
    bad = 0
    for lat, syl in KNOWN.items():
        got = latin_to_syl(lat)
        if got != syl:
            bad += 1
            print(f'MISMATCH {lat}: {got} != {syl}')
    print(f'{len(KNOWN) - bad}/{len(KNOWN)} dictionary words converted exactly')
    return bad == 0


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if cmd == 'skeleton':
        skeleton()
    elif cmd == 'build':
        build()
    elif cmd == 'test':
        sys.exit(0 if test() else 1)
    else:
        sys.exit(__doc__)
