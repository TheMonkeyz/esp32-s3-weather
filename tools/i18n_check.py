#!/usr/bin/env python3
"""Translation sync checks: are the translations complete, and were they made from the current English?

    python tools/i18n_check.py site            check the web flasher page (web/flash/index.html)
    python tools/i18n_check.py site --update   after re-translating: record the English each translation now matches
    python tools/i18n_check.py site --strict   also fail on stale translations
    python tools/i18n_check.py guide           check the French owner's guide (docs/guide.fr.md) against README.md
    python tools/i18n_check.py guide --update  after updating the guide: record the README sections it now matches

`site` reads the I18N = {en, fr, iu} table of web/flash/index.html and checks that:
  - every key of I18N.en is in fr and iu, and fr / iu have no other key            (error)
  - fr / iu keep the {0} {1} placeholders of the English                           (error)
  - every key the HTML uses (data-i18n, data-i18n-html, data-i18n-aria, data-i18n-alt, data-key, data-lname) and
    every t('key') of the script is in I18N.en                                     (error)
  - the English written in the HTML (shown before the script runs) is the same as I18N.en (error)
  - no visible text sits outside a translated element, unless inside translate="no"     (error)
  - each translation was made from the current English                              (listed as stale)
  - every I18N.en key is used somewhere                                             (warning)
Exit status 1 on errors (and on stale translations with --strict), else 0.

Stale translations: docs/translations/i18n.lock.json records, for each translated key, a short hash of the English
text it was translated from. When an English text changes, its fr and iu translations show up as stale. Re-translate
them (fr: in the page; iu: the site:* row of docs/translations/iu.tsv, then `python tools/i18n_iu.py build`), then run
`--update` to record the new English. A translation added without `--update` is listed too.

`guide` reads the README sections named in the guide's first line,
`<!-- source: README.md @ <commit>; sections: What you need, Install, ... -->`, and lists those whose English changed
since the guide was last brought up to date (same lock file, one hash per section). A named section missing from the
README is an error. `--update` records the current sections and writes the last commit shared with origin/main into
that line (a branch's own commits vanish in a squash merge), so `git diff <commit> -- README.md` shows what changed
next time.

Adding another document: write a function that reads its English and translations as {key: text} dicts, pass them to
compare() with the command's name (its section of the lock file), and add the function to COMMANDS. Standard library
only.
"""
import argparse
import hashlib
import html
import json
import os
import re
import subprocess
import sys
from html.parser import HTMLParser

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SITE = os.path.join(ROOT, 'web', 'flash', 'index.html')
README = os.path.join(ROOT, 'README.md')
GUIDE = os.path.join(ROOT, 'docs', 'guide.fr.md')
LOCK = os.path.join(ROOT, 'docs', 'translations', 'i18n.lock.json')


# ---------------- shared ----------------
def digest(text):
    return hashlib.sha256(text.encode('utf-8')).hexdigest()[:12]


def squash(s):
    return re.sub(r'\s+', ' ', s).strip()


def placeholders(s):
    return sorted(set(re.findall(r'\{\d\}', s)))


def load_lock():
    if os.path.exists(LOCK):
        with open(LOCK, encoding='utf-8') as f:
            return json.load(f)
    return {}


def save_lock(lock):
    lock['about'] = ('Written by tools/i18n_check.py --update. For each translated key: a hash of the English text '
                     'it was translated from (sha256, 12 hex digits). Do not edit by hand.')
    with open(LOCK, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(dict(sorted(lock.items())), f, indent=1, ensure_ascii=False, sort_keys=True)
        f.write('\n')


class Report:
    def __init__(self):
        self.errors, self.stale, self.warnings = [], [], []

    def show(self, strict):
        for kind, items in (('ERROR', self.errors), ('STALE', self.stale), ('warning', self.warnings)):
            for line in items:
                print(f'{kind}  {line}')
        bad = bool(self.errors) or (strict and bool(self.stale))
        print(f'{"FAILED" if bad else "OK"}: {len(self.errors)} error(s), {len(self.stale)} stale, '
              f'{len(self.warnings)} warning(s)')
        return 1 if bad else 0


def compare(name, en, translations, lock, report, update=False):
    """Check translations ({lang: {key: text}}) against the English ({key: text}); `name` is the lock section."""
    section = lock.setdefault(name, {})
    for lang, tr in translations.items():
        for k in en:
            if k not in tr:
                report.errors.append(f'{lang}: missing key "{k}" (English: {en[k][:70]!r})')
        for k in tr:
            if k not in en:
                report.errors.append(f'{lang}: unknown key "{k}" (not in the English)')
            elif placeholders(tr[k]) != placeholders(en[k]):
                report.errors.append(f'{lang}: "{k}" has placeholders {placeholders(tr[k])}, the English '
                                     f'{placeholders(en[k])}')
        made_from = section.setdefault(lang, {})
        if update:
            made_from.clear()
            made_from.update({k: digest(en[k]) for k in sorted(tr) if k in en})
            continue
        for k in tr:
            if k not in en:
                continue
            if k not in made_from:
                report.stale.append(f'{lang}: "{k}" is not recorded yet (check it, then run --update)')
            elif made_from[k] != digest(en[k]):
                report.stale.append(f'{lang}: "{k}": the English changed after it was translated. '
                                    f'English now: {en[k][:90]!r}')


# ---------------- site: web/flash/index.html ----------------
def js_unescape(s):
    return re.sub(r'\\(u[0-9a-fA-F]{4}|.)',
                  lambda m: chr(int(m.group(1)[1:], 16)) if len(m.group(1)) == 5 else
                  {'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), s)


def i18n_tables(src, path, report):
    """The I18N table of a page: {lang: {key: text}}, with the script outside the table."""
    start = src.find('const I18N = {')
    end = src.find('\n};', start)
    if start < 0 or end < 0:
        report.errors.append(f'{path}: no "const I18N = {{ ... }};" table')
        return {}, src
    table, rest = src[start:end], src[:start] + src[end:]
    out = {}
    pair = re.compile(r"(\w+): ('(?:[^'\\\n]|\\.)*'|\"(?:[^\"\\\n]|\\.)*\")")
    for m in re.finditer(r'^ (\w+): \{(.*?)\n \},', table, re.S | re.M):
        d = {}
        for km in pair.finditer(m.group(2)):
            if km.group(1) in d:
                report.errors.append(f'{m.group(1)}: key "{km.group(1)}" appears twice')
            d[km.group(1)] = js_unescape(km.group(2)[1:-1])
        out[m.group(1)] = d
        left = re.sub(r'//[^\n]*|[\s,]', '', pair.sub('', m.group(2)))   # anything else: a quoting mistake
        if left:
            report.errors.append(f'{m.group(1)}: cannot read the table near {left[:60]!r} (an unescaped quote?)')
    return out, rest


class PageText(HTMLParser):
    """Keys used by the HTML, and the English written in it, for each data-i18n / data-i18n-html element."""
    VOID = {'area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'source', 'track', 'wbr'}
    UNSHOWN = {'script', 'style', 'template', 'noscript'}
    KEY_ATTRS = ('data-i18n', 'data-i18n-html', 'data-i18n-aria', 'data-i18n-alt', 'data-key', 'data-lname')

    def __init__(self, src):
        super().__init__(convert_charrefs=True)
        self.src = src
        self.line_at = [0]
        for line in src.splitlines(keepends=True):
            self.line_at.append(self.line_at[-1] + len(line))
        self.stack = []          # [tag, inner start, (kind, key) or None, translate="no"]
        self.used = []           # (attribute, key)
        self.texts = []          # (kind, key, text in the HTML): kind = text / html / aria / alt / title / desc
        self.loose = []          # (line, text): visible text outside every translated element
        self.in_title = False
        self.feed(src)

    def here(self):
        line, col = self.getpos()
        return self.line_at[line - 1] + col

    def attrs_seen(self, tag, attrs):
        a = dict(attrs)
        for name in self.KEY_ATTRS:
            if a.get(name):
                self.used.append((name, a[name]))
        if a.get('data-i18n-aria'):
            self.texts.append(('aria', a['data-i18n-aria'], a.get('aria-label') or ''))
        if a.get('data-i18n-alt'):
            self.texts.append(('alt', a['data-i18n-alt'], a.get('alt') or ''))
        if tag == 'meta' and a.get('name') == 'description':
            self.texts.append(('desc', 'pageDesc', a.get('content') or ''))
        return a

    def handle_starttag(self, tag, attrs):
        a = self.attrs_seen(tag, attrs)
        if tag == 'title':
            self.in_title = True
        if tag in self.VOID:
            return
        what = ('text', a['data-i18n']) if a.get('data-i18n') else \
            ('html', a['data-i18n-html']) if a.get('data-i18n-html') else None
        self.stack.append([tag, self.here() + len(self.get_starttag_text()), what, a.get('translate') == 'no'])

    def handle_startendtag(self, tag, attrs):
        self.attrs_seen(tag, attrs)

    def handle_endtag(self, tag):
        if tag == 'title':
            self.in_title = False
        at = self.here()
        while self.stack:
            t, inner, what, _ = self.stack.pop()
            if what:
                self.texts.append((what[0], what[1], self.src[inner:at]))
            if t == tag:
                break

    def handle_data(self, data):
        if self.in_title:
            self.texts.append(('title', 'pageTitle', data))
        elif re.search(r'[^\W\d_]', data) and not any(what or t in self.UNSHOWN or translate
                                                     for t, _, what, translate in self.stack):
            self.loose.append((self.getpos()[0], squash(data)))


def check_site(args, lock, report):
    path = os.path.relpath(SITE, ROOT).replace(os.sep, "/")
    src = open(SITE, encoding='utf-8').read()
    tables, script = i18n_tables(src, path, report)
    en = tables.get('en')
    if not en:
        report.errors.append(f'{path}: no I18N.en block')
        return
    for lang in ('fr', 'iu'):
        if lang not in tables:
            report.errors.append(f'{path}: no I18N.{lang} block')
    print(f'{path}: {len(en)} English texts; ' + ', '.join(f'{k} {len(v)}' for k, v in tables.items() if k != 'en'))
    compare('site', en, {k: v for k, v in tables.items() if k != 'en'}, lock, report, update=args.update)

    page = PageText(src)
    for attr, key in page.used:
        if key not in en:
            report.errors.append(f'html: {attr}="{key}" is not in I18N.en')
    # the page's own script, without the I18N table and comments
    script = re.sub(r'(^|\s)//\s.*$', '', script[script.rfind('<script>'):], flags=re.M)
    calls = set(re.findall(r"\bt\('(\w+)'", script))
    for key in sorted(calls - set(en)):
        report.errors.append(f"script: t('{key}') is not in I18N.en")
    checked = 0
    for kind, key, text in page.texts:
        if key not in en:
            continue
        checked += 1
        want = squash(en[key])
        got = squash(text if kind == 'html' else html.unescape(text))
        if kind == 'text' and '<' in text:
            report.errors.append(f'html: data-i18n="{key}" holds markup: use data-i18n-html')
        elif got != want:
            report.errors.append(f'html: the English of "{key}" ({kind}) differs from I18N.en\n'
                                 f'         html: {got[:110]!r}\n         en:   {want[:110]!r}')
    print(f'{path}: {checked} English texts in the HTML compared with I18N.en')
    for line, text in page.loose:
        report.errors.append(f'html line {line}: text outside any data-i18n element (give it a key, or '
                             f'translate="no"): {text[:70]!r}')
    quoted = set(re.findall(r"""['"](\w+)['"]""", script)) | {k for _, k in page.used}
    for key in en:
        if key not in quoted:
            report.warnings.append(f'I18N.en "{key}" is not used by the page')


# ---------------- guide: docs/guide.fr.md, translated from sections of README.md ----------------
GUIDE_SOURCE = re.compile(r'<!--\s*source:\s*README\.md\s*@\s*([0-9a-f]{7,40})\s*;\s*sections:\s*(.*?)\s*-->')


def readme_sections(src):
    """{title: text} of the README's ## sections, titles without their leading emoji."""
    out, title, lines, fence = {}, None, [], False
    for line in src.splitlines():
        if line.startswith('```'):
            fence = not fence
        if not fence and line.startswith('## '):
            if title:
                out[title] = '\n'.join(lines).strip()
            title, lines = re.sub(r'^[^\w(]+', '', line[3:]).strip(), []
        elif title:
            lines.append(line)
    if title:
        out[title] = '\n'.join(lines).strip()
    return out


def check_guide(args, lock, report):
    path = os.path.relpath(GUIDE, ROOT).replace(os.sep, '/')
    if not os.path.exists(GUIDE):
        report.errors.append(f'{path}: not found')
        return
    guide = open(GUIDE, encoding='utf-8').read()
    m = GUIDE_SOURCE.search(guide)
    if not m:
        report.errors.append(f'{path}: no "<!-- source: README.md @ <commit>; sections: ... -->" line')
        return
    names = [s.strip() for s in m.group(2).split(',') if s.strip()]
    sections = readme_sections(open(README, encoding='utf-8').read())
    for name in names:
        if name not in sections:
            report.errors.append(f'{path}: README.md has no "## {name}" section (renamed? update the source line)')
    en = {n: sections[n] for n in names if n in sections}
    print(f'{path}: {len(names)} README sections, translated from {m.group(1)[:7]}')
    compare('guide', en, {'fr': {n: '' for n in en}}, lock, report, update=args.update)
    known = subprocess.run(['git', 'cat-file', '-e', m.group(1) + '^{commit}'], cwd=ROOT,
                           capture_output=True).returncode == 0
    if report.stale and not args.update:
        report.stale.append(f'see what changed: git diff {m.group(1)[:7]} -- README.md' if known else
                            f'{m.group(1)[:7]} is not in this repository (a branch commit lost to a squash merge?): '
                            f'compare with git log -p -- README.md')
    if args.update and not report.errors:
        try:   # the last commit on main: a branch commit disappears when the pull request is squash-merged
            head = subprocess.check_output(['git', 'merge-base', 'HEAD', 'origin/main'], cwd=ROOT, text=True).strip()
        except Exception:
            return
        line = m.group(0).replace(m.group(1), head)
        with open(GUIDE, 'w', encoding='utf-8', newline='') as f:
            f.write(guide.replace(m.group(0), line, 1))
        print(f'{path}: source line now @ {head[:7]}')


COMMANDS = {'site': check_site, 'guide': check_guide}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', choices=sorted(COMMANDS))
    ap.add_argument('--update', action='store_true', help='record the current English as the source of each translation')
    ap.add_argument('--strict', action='store_true', help='stale translations are errors too')
    args = ap.parse_args()
    lock, report = load_lock(), Report()
    COMMANDS[args.command](args, lock, report)
    if args.update:
        if report.errors:
            print('Not updating the lock file: fix the errors first.')
        else:
            save_lock(lock)
            print(f'Updated {os.path.relpath(LOCK, ROOT).replace(os.sep, "/")} ({args.command}).')
    return report.show(args.strict)


if __name__ == '__main__':
    sys.exit(main())
