#!/usr/bin/env python3
"""NoX Core's translations: the English catalog, the checks that keep every language current, and
the worksheets for updating them.

usage: python noxsdl/l10n/l10n.py <command> [args]

  extract            read every NOX_TR / NOX_TRC / NOX_TRI / NOX_TRP / NOX_TRK call site and write
                     the English catalog, noxsdl/res/lang/en.txt (run it after changing English).
  check              fail (exit 1) when: en.txt isn't what the source says; a language lacks a key
                     (missing); a key's English changed since it was translated (stale: the hash on
                     its "#@" line differs); a language has a key the English doesn't (orphaned); a
                     translation's placeholders ({name}, printf's %d, Dear ImGui's ## id) or plural
                     forms differ from the English's; or player-facing English is written in the UI
                     code without NOX_TR (the lint: noxsdl/l10n/lint_allow.txt lists the exceptions).
  todo LANG|all [-o FILE]
                     a worksheet of LANG's missing and stale entries (the English, and the old
                     translation of a stale one), to fill in; to stdout, or FILE.
  apply LANG FILE [--reviewed]
                     merge a filled worksheet into LANG's file: each entry gets the hash of the
                     English it now translates; flagged "mt" (machine translation) unless its #@
                     line says otherwise or --reviewed is given.
  stats              entries, missing, stale and machine-translated counts per language.
  lint [--all]       the lint alone (--all: every hit, the allowlist ignored).

THE RULE: whoever adds or changes English text updates every language in the same commit. Change
the English, run `extract`, then `todo all -o work.txt`, translate (machine translation is fine:
keep the entry's "mt" flag so a native speaker knows to review it), `apply` each language, and
`check` must pass. ctest runs `check` (the l10n_check test) and so does the public build.
"""
import os
import re
import sys
from collections import OrderedDict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
LANG_DIR = os.path.join(ROOT, "noxsdl", "res", "lang")
ALLOW_FILE = os.path.join(HERE, "lint_allow.txt")

# Where call sites are read from (every program NoX Core ships), and the files skipped.
SOURCE_ROOTS = ["noxsdl/src", "videotools/src", "noxcore/src", "noxcore/include", "modes"]
SOURCE_SKIP = {"noxcore/include/noxcore/strings/l10n.hpp"}
# The UI code the lint reads: the client, the map editor and the Content Editor.
LINT_ROOTS = ["noxsdl/src", "videotools/src/gui", "videotools/src/content",
              # (the map editor's model and the script compiler: their labels, checks and diagnostics)
              "noxcore/src/mapedit", "noxcore/include/noxcore/mapedit", "noxcore/src/script", "noxcore/src/lobby",
              # (the built-in modes' client windows)
              "modes/delve/ui", "modes/tide/ui", "modes/coop/ui"]
# Single files the lint reads too (the modes' client glue beside their server code).
LINT_FILES = ["modes/delve/delve_client.cpp", "modes/tide/tide_client.cpp", "modes/coop/src/coop_client.cpp",
              "modes/script/src/script_ui.cpp"]

MACROS = {"NOX_TR": 2, "NOX_TRC": 2, "NOX_TRI": 2, "NOX_TRK": 2, "NOX_TRP": 3, "NOX_TRPK": 3}
PLURAL_MACROS = {"NOX_TRP", "NOX_TRPK"}
KEY_RX = re.compile(r"^[a-z0-9_]+(\.[a-z0-9_]+)+$")
PLACEHOLDER_RX = re.compile(r"\{([a-z_][a-z0-9_]*)\}")
PRINTF_RX = re.compile(r"%[-+#0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|z|j|t|L|I64)?[diouxXeEfgGcspn%]")

LANGUAGES = OrderedDict([
    ("ru", "Русский (Russian)"), ("ko", "한국어 (Korean)"), ("de", "Deutsch (German)"),
    ("fr", "Français (French)"), ("it", "Italiano (Italian)"), ("es", "Español (Spanish)"),
    ("pl", "Polski (Polish)"), ("cs", "Čeština (Czech)"), ("ja", "日本語 (Japanese)"),
])
# The plural forms each language's files must give (CLDR's integer rules, as l10n.cpp applies them).
PLURAL_FORMS = {"ru": ["one", "few", "many"], "pl": ["one", "few", "many"], "cs": ["one", "few", "other"],
                "ko": ["other"], "ja": ["other"], "zh": ["other"]}
DEFAULT_FORMS = ["one", "other"]
FORM_NAMES = ["one", "few", "many", "other"]


# ----------------------------------------------------------------------------- C++ reading

def tokens(text):
    """C++ tokens: (kind, value, line) with kind 'str' (value: the literal's text, escapes
    decoded), 'id', 'punct' or 'pp' (a whole preprocessor line). Comments are dropped."""
    i, n, line = 0, len(text), 1
    out = []
    at_line_start = True
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
            at_line_start = True
            continue
        if c in " \t\r\f\v":
            i += 1
            continue
        if c == "#" and at_line_start:
            j = i
            while j < n:
                if text[j] == "\n" and text[j - 1] != "\\":
                    break
                j += 1
            out.append(("pp", text[i:j], line))
            line += text[i:j].count("\n")
            i = j
            continue
        at_line_start = False
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            line += text[i:j].count("\n")
            i = j
            continue
        if c.isalpha() or c == "_":
            j = i
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            word = text[i:j]
            if j < n and text[j] == '"' and word in ("u8", "L", "u", "U", "R", "u8R", "LR", "uR", "UR"):
                i = j
                if word.endswith("R"):
                    m = re.match(r'"([^()\\ ]{0,16})\(', text[i:])
                    delim = m.group(1)
                    end = text.find(")" + delim + '"', i)
                    val = text[i + len(delim) + 2:end]
                    out.append(("str", val, line))
                    line += val.count("\n")
                    i = end + len(delim) + 2
                continue
            out.append(("id", word, line))
            i = j
            continue
        if c.isdigit():
            j = i
            while j < n and (text[j].isalnum() or text[j] in "._'" or (text[j] in "+-" and text[j - 1] in "eEpP")):
                j += 1
            out.append(("num", text[i:j], line))
            i = j
            continue
        if c == '"':
            j = i + 1
            buf = []
            while j < n and text[j] != '"':
                if text[j] == "\\":
                    e = text[j + 1]
                    if e == "n": buf.append("\n"); j += 2
                    elif e == "t": buf.append("\t"); j += 2
                    elif e == "x":
                        m = re.match(r"[0-9a-fA-F]+", text[j + 2:])
                        buf.append(chr(int(m.group(0), 16))); j += 2 + len(m.group(0))
                    elif e in "01234567":
                        m = re.match(r"[0-7]{1,3}", text[j + 1:])
                        buf.append(chr(int(m.group(0), 8))); j += 1 + len(m.group(0))
                    elif e == "u":
                        buf.append(chr(int(text[j + 2:j + 6], 16))); j += 6
                    elif e == "\n":
                        line += 1; j += 2
                    else:
                        buf.append({"r": "\r", "0": "\0", "a": "\a", "b": "\b", "f": "\f", "v": "\v"}.get(e, e)); j += 2
                else:
                    buf.append(text[j])
                    j += 1
            s = "".join(buf)
            # \x escapes are bytes: a UTF-8 sequence written byte by byte becomes its character.
            try:
                s = s.encode("latin-1").decode("utf-8")
            except (UnicodeEncodeError, UnicodeDecodeError):
                pass
            out.append(("str", s, line))
            i = j + 1
            continue
        if c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            out.append(("chr", text[i:j + 1], line))
            i = j + 1
            continue
        out.append(("punct", c, line))
        i += 1
    return out


def source_files(roots):
    for r in roots:
        base = os.path.join(ROOT, r)
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = sorted(d for d in dirnames if d not in ("build", "tests", "third_party", "_deps"))
            for f in sorted(filenames):
                if f.endswith((".cpp", ".hpp", ".h", ".cc", ".inl")):
                    full = os.path.join(dirpath, f)
                    rel = os.path.relpath(full, ROOT).replace("\\", "/")
                    if rel not in SOURCE_SKIP:
                        yield rel, full


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def macro_calls(toks):
    """(macro, [literal args], line, start index, end index) for each NOX_TR* call."""
    i = 0
    while i < len(toks):
        k, v, line = toks[i]
        if k == "id" and v in MACROS and i + 1 < len(toks) and toks[i + 1][1] == "(":
            depth, j = 0, i + 1
            args, cur = [], []
            while j < len(toks):
                t = toks[j]
                if t[1] in "([{" and t[0] == "punct":
                    depth += 1
                    if depth > 1: cur.append(t)
                elif t[1] in ")]}" and t[0] == "punct":
                    depth -= 1
                    if depth == 0:
                        args.append(cur)
                        break
                    cur.append(t)
                elif t[1] == "," and t[0] == "punct" and depth == 1:
                    args.append(cur)
                    cur = []
                else:
                    cur.append(t)
                j += 1
            lits = []
            for a in args[:MACROS[v]]:
                if a and all(t[0] == "str" for t in a):
                    lits.append("".join(t[1] for t in a))
                else:
                    lits.append(None)
            yield v, lits, line, i, j
            i = j
        i += 1


def extract():
    """{key: {'en', 'other', 'plural', 'where': [file:line]}} and the errors."""
    cat, errors = {}, []
    for rel, full in source_files(SOURCE_ROOTS):
        text = read(full)
        if "NOX_TR" not in text:
            continue
        for macro, lits, line, _, _ in macro_calls(tokens(text)):
            where = f"{rel}:{line}"
            if any(l is None for l in lits) or len(lits) < MACROS[macro]:
                errors.append(f"{where}: {macro}'s key and English must be string literals")
                continue
            key, en = lits[0], lits[1]
            other = lits[2] if macro in PLURAL_MACROS else None
            if not KEY_RX.match(key):
                errors.append(f"{where}: bad key {key!r} (a-z, 0-9, _ in dotted parts: area.screen.name)")
                continue
            if key.startswith("test."):
                continue
            e = cat.get(key)
            if e is None:
                cat[key] = {"en": en, "other": other, "plural": macro in PLURAL_MACROS, "where": [where]}
            else:
                if e["en"] != en or e["other"] != other or e["plural"] != (macro in PLURAL_MACROS):
                    errors.append(f"{where}: key {key} has other English at {e['where'][0]}: {e['en']!r} vs {en!r}")
                e["where"].append(where)
    return dict(sorted(cat.items())), errors


# ----------------------------------------------------------------------------- language files

def fnv(en, other=None):
    h = 2166136261
    data = en.encode("utf-8")
    if other:
        data += b"\x1f" + other.encode("utf-8")
    for b in data:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return f"{h:08x}"


def english_hash(e):
    return fnv(e["en"], e["other"] if e["plural"] else None)


def unescape(s):
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s) and s[i + 1] in "nts\\":
            out.append({"n": "\n", "t": "\t", "s": " ", "\\": "\\"}[s[i + 1]])
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def escape(s):
    out = []
    for i, ch in enumerate(s):
        if ch == "\\": out.append("\\\\")
        elif ch == "\n": out.append("\\n")
        elif ch == "\t": out.append("\\t")
        elif ch == " " and (i == 0 or i == len(s) - 1): out.append("\\s")
        else: out.append(ch)
    return "".join(out)


def parse_lang(path):
    """{key: {'hash', 'mt', 'text', 'forms': {form: text}, 'line'}}, errors (as l10n.cpp reads)."""
    entries, errors = OrderedDict(), []
    pending, pending_mt, pending_key = None, False, None
    text = read(path)
    if text.startswith("\ufeff"):
        text = text[1:]
    for n, raw in enumerate(text.split("\n"), 1):
        line = raw.strip(" \t\r")
        if not line:
            continue
        if line.startswith("#@"):
            m = re.match(r"#@\s+([0-9a-f]{8})((?:\s+\w+)*)\s*:", line)
            if not m:
                errors.append(f"{n}: a '#@' line without an 8-digit hash")
                pending = None
                continue
            pending, pending_mt, pending_key = m.group(1), "mt" in m.group(2).split(), None
            continue
        if line.startswith("#"):
            continue
        if "=" not in line:
            errors.append(f"{n}: not 'key = text'")
            continue
        key, val = line.split("=", 1)
        key, val = key.strip(), unescape(val.strip(" \t\r"))
        form = None
        m = re.match(r"^(.*)\[(\w+)\]$", key)
        if m:
            key, form = m.group(1), m.group(2)
            if form not in FORM_NAMES:
                errors.append(f"{n}: unknown plural form [{form}]")
                continue
        if not KEY_RX.match(key):
            errors.append(f"{n}: bad key {key!r}")
            continue
        e = entries.setdefault(key, {"hash": None, "mt": False, "text": None, "forms": {}, "line": n})
        if pending and (pending_key is None or pending_key == key):
            e["hash"], e["mt"] = pending, pending_mt
        if form:
            e["forms"][form] = val
            pending_key = key
        else:
            if e["text"] is not None:
                errors.append(f"{n}: {key} given twice")
            e["text"] = val
            pending, pending_key = None, None
    return entries, errors


def lang_path(code):
    return os.path.join(LANG_DIR, code + ".txt")


def header(code, entries):
    total = len(entries)
    mt = sum(1 for e in entries.values() if e["mt"])
    name = LANGUAGES.get(code, code)
    status = ("machine translation, not yet reviewed by a native speaker" if mt == total and total else
              f"{total - mt} of {total} entries reviewed; those flagged \"mt\" are machine translations")
    return [
        f"# NoX Core's own text: {name}",
        f"# Status: {status}.",
        "# Each entry is \"key = text\", after a \"#@ <hash> [mt]: <English>\" line: <hash> is the",
        "# English it translates (noxsdl/l10n/l10n.py keeps it), \"mt\" marks a machine translation a",
        "# native speaker hasn't checked yet (drop it once checked). Keep every {placeholder}, %d and",
        "# ## id as the English has them. \\n is a new line, \\s a space at either end.",
        "",
    ]


def write_lang(code, entries, cat):
    """Writes LANG's file: the English catalog's order, each entry after its #@ line."""
    lines = header(code, entries)
    last_area = None
    for key, e in cat.items():
        t = entries.get(key)
        if not t:
            continue
        area = key.split(".")[0]
        if last_area is not None and area != last_area:
            lines.append("")
        last_area = area
        en = e["en"] + (" | " + e["other"] if e["plural"] else "")
        lines.append(f"#@ {t['hash']}{' mt' if t['mt'] else ''}: {escape(en)}")
        if e["plural"]:
            for f in FORM_NAMES:
                if f in t["forms"]:
                    lines.append(f"{key}[{f}] = {escape(t['forms'][f])}")
        else:
            lines.append(f"{key} = {escape(t['text'] or '')}")
    with open(lang_path(code), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def write_english(cat):
    lines = [
        "# NoX Core's own text: English, the source (written by noxsdl/l10n/l10n.py extract from the",
        "# NOX_TR call sites; never edit it by hand). The program has this text built in and never",
        "# reads this file: it is the template for a new language (copy it to <code>.txt, translate",
        "# each entry and keep its #@ line) and what l10n.py check compares the languages with.",
        "",
    ]
    last_area = None
    for key, e in cat.items():
        area = key.split(".")[0]
        if last_area is not None and area != last_area:
            lines.append("")
        last_area = area
        lines.append(f"# {', '.join(e['where'][:3])}{' ...' if len(e['where']) > 3 else ''}")
        lines.append(f"#@ {english_hash(e)}: {escape(e['en'] + (' | ' + e['other'] if e['plural'] else ''))}")
        if e["plural"]:
            lines.append(f"{key}[one] = {escape(e['en'])}")
            lines.append(f"{key}[other] = {escape(e['other'])}")
        else:
            lines.append(f"{key} = {escape(e['en'])}")
    return "\n".join(lines) + "\n"


def languages():
    return [c for c in LANGUAGES if os.path.exists(lang_path(c))] + sorted(
        f[:-4] for f in os.listdir(LANG_DIR)
        if f.endswith(".txt") and f[:-4] not in LANGUAGES and f != "en.txt" and re.match(r"^[a-z]{2,3}\.txt$", f))


# ----------------------------------------------------------------------------- the checks

def marks(s):
    """What a translation must keep: its {placeholders} and printf directives (as multisets) and
    Dear ImGui's ## id suffix."""
    ph = sorted(PLACEHOLDER_RX.findall(s))
    pf = sorted(PRINTF_RX.findall(s))
    i = s.find("##")
    return ph, pf, s[i:] if i >= 0 else ""


def check_entry(code, key, e, t):
    problems = []
    if e["plural"]:
        need = PLURAL_FORMS.get(code, DEFAULT_FORMS)
        if t["text"] is not None:
            problems.append(f"{key}: a plural, given as one text (give {', '.join('[' + f + ']' for f in need)})")
        missing = [f for f in need if f not in t["forms"]]
        if missing:
            problems.append(f"{key}: plural forms missing: {', '.join(missing)}")
        want = marks(e["other"])
        for f, text in t["forms"].items():
            got = marks(text)
            # (a [one] form may say "1" or "one" in words instead of {n})
            if got != want and not (f == "one" and got[1:] == want[1:]
                                    and set(got[0]) <= set(want[0])):
                problems.append(f"{key}[{f}]: placeholders differ from the English's: {want} vs {got}")
    else:
        if t["text"] is None:
            problems.append(f"{key}: given as plural forms, but the English is one text")
            return problems
        want, got = marks(e["en"]), marks(t["text"])
        if want != got:
            problems.append(f"{key}: placeholders differ from the English's: {want} vs {got}")
    return problems


def cmd_check():
    cat, errors = extract()
    failures = list(errors)
    english = write_english(cat)
    en_path = lang_path("en")
    if not os.path.exists(en_path) or read(en_path).replace("\r\n", "\n") != english:
        failures.append("noxsdl/res/lang/en.txt is out of date: run `python noxsdl/l10n/l10n.py extract`")
    for code in languages():
        entries, errs = parse_lang(lang_path(code))
        failures += [f"{code}.txt:{e}" for e in errs]
        missing, stale, orphan = [], [], []
        for key, e in cat.items():
            t = entries.get(key)
            if t is None:
                missing.append(key)
                continue
            if t["hash"] is None:
                failures.append(f"{code}.txt:{t['line']}: {key} has no #@ line (the English it translates)")
            elif t["hash"] != english_hash(e):
                stale.append(key)
            failures += [f"{code}.txt:{t['line']}: {p}" for p in check_entry(code, key, e, t)]
        for key, t in entries.items():
            if key not in cat:
                orphan.append(key)
        for what, keys in (("missing", missing), ("stale (the English changed since)", stale), ("orphaned (no English)", orphan)):
            if keys:
                shown = ", ".join(keys[:8]) + (f" and {len(keys) - 8} more" if len(keys) > 8 else "")
                failures.append(f"{code}.txt: {len(keys)} {what}: {shown}")
    lint_hits, lint_notes = lint(use_allow=True)
    failures += lint_hits
    for f in failures:
        print(f)
    for note in lint_notes:
        print("note: " + note)
    if failures:
        print(f"\nl10n check: {len(failures)} problems. To fix: python noxsdl/l10n/l10n.py extract; "
              "python noxsdl/l10n/l10n.py todo all -o work.txt; translate; apply each language.")
        return 1
    print(f"l10n check: {len(cat)} keys, {len(languages())} languages, all current")
    return 0


def cmd_extract():
    cat, errors = extract()
    for e in errors:
        print(e)
    with open(lang_path("en"), "w", encoding="utf-8", newline="\n") as f:
        f.write(write_english(cat))
    print(f"en.txt: {len(cat)} keys")
    return 1 if errors else 0


def cmd_todo(args):
    out = None
    if "-o" in args:
        i = args.index("-o")
        out = args[i + 1]
        del args[i:i + 2]
    codes = languages() if args[0] == "all" else [args[0]]
    cat, _ = extract()
    lines = ["# l10n worksheet: fill in each entry's text after '=' (keep {placeholders}, %d, ## ids),",
             "# then: python noxsdl/l10n/l10n.py apply <lang> <this file> (one language's section each).",
             "# Leave \"mt\" on a machine translation's #@ line; drop it for one a native speaker wrote.", ""]
    count = 0
    for code in codes:
        p = lang_path(code)
        entries = parse_lang(p)[0] if os.path.exists(p) else {}
        section = []
        for key, e in cat.items():
            t = entries.get(key)
            if t is not None and t["hash"] == english_hash(e):
                continue
            count += 1
            en = e["en"] + (" | " + e["other"] if e["plural"] else "")
            section.append(f"# {e['where'][0]}{'  (stale: the English changed)' if t else ''}")
            if t:
                old = t["text"] if t["text"] is not None else " | ".join(f"[{f}] {v}" for f, v in t["forms"].items())
                section.append(f"# was: {escape(old)}")
            section.append(f"#@ {english_hash(e)} mt: {escape(en)}")
            if e["plural"]:
                for f in PLURAL_FORMS.get(code, DEFAULT_FORMS):
                    section.append(f"{key}[{f}] = ")
            else:
                section.append(f"{key} = ")
        if section:
            lines += [f"#### {code}", ""] + section + [""]
    text = "\n".join(lines) + "\n"
    if out:
        with open(out, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print(f"{count} entries to translate: {out}")
    else:
        sys.stdout.reconfigure(encoding="utf-8")
        print(text)
    return 0


def cmd_apply(args):
    code, sheet = args[0], args[1]
    cat, _ = extract()
    # A worksheet may hold several languages' sections ("#### de"): only this one's is read.
    text = read(sheet)
    if "\n#### " in "\n" + text:
        parts = re.split(r"(?m)^#### (\w+)\s*$", text)
        text = "".join(parts[i + 1] for i in range(1, len(parts) - 1, 2) if parts[i] == code)
    tmp = sheet + ".part"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    new, errs = parse_lang(tmp)
    os.remove(tmp)
    for e in errs:
        print(f"{sheet}: {e}")
    p = lang_path(code)
    entries = parse_lang(p)[0] if os.path.exists(p) else OrderedDict()
    applied, skipped = 0, []
    for key, t in new.items():
        e = cat.get(key)
        empty = (t["text"] is None or t["text"] == "") and not any(t["forms"].values())
        if e is None or empty:
            skipped.append(key)
            continue
        if not e["plural"]:
            t["forms"] = {}
        if t["hash"] is None:
            t["mt"] = "--reviewed" not in args  # (a bare "key = text": a machine translation unless said)
        t["hash"] = english_hash(e)
        entries[key] = t
        applied += 1
    write_lang(code, entries, cat)
    print(f"{code}.txt: {applied} entries applied" + (f", {len(skipped)} skipped (empty or no such key): "
                                                       + ", ".join(skipped[:10]) if skipped else ""))
    return 1 if errs else 0


def cmd_stats():
    cat, _ = extract()
    print(f"English: {len(cat)} keys")
    for code in languages():
        entries, _ = parse_lang(lang_path(code))
        have = [k for k in cat if k in entries]
        stale = [k for k in have if entries[k]["hash"] != english_hash(cat[k])]
        mt = [k for k in have if entries[k]["mt"]]
        print(f"{code}: {len(have)} of {len(cat)}, {len(cat) - len(have)} missing, {len(stale)} stale, "
              f"{len(mt)} machine-translated, {len(entries) - len(have)} orphaned")
    return 0


# ----------------------------------------------------------------------------- the lint

# Calls whose string arguments are never shown to a player: logs, errors for the log, lookups by
# name, comparisons, ids.
SKIP_CALLS = {
    "SDL_Log", "SDL_LogError", "SDL_LogWarn", "SDL_LogInfo", "SDL_LogDebug", "SDL_LogCritical", "SDL_LogVerbose",
    "SDL_LogMessage", "SDL_SetError", "SDL_SetHint", "SDL_GetHint", "SDL_SetAppMetadata", "SDL_SetAppMetadataProperty",
    "printf", "fprintf", "fputs", "puts", "perror", "logLine", "logf", "NOX_LOG", "log",
    "starts_with", "ends_with", "find", "rfind", "find_first_of", "find_last_of", "compare", "contains", "count",
    "strcmp", "strncmp", "_stricmp", "strcasecmp", "equalsNoCase", "startsWithNoCase", "getenv", "strstr",
    "attribute", "setting", "GetID", "PushID", "runtime_error", "invalid_argument", "logic_error", "out_of_range",
    "FormatError", "static_assert", "assert", "SDL_assert", "require", "REQUIRE", "CHECK",
    "SDL_GetPrefPath", "SDL_OpenURL", "fopen", "freopen", "path", "u8path", "extension", "replace_extension",
    "soundId", "findThing", "thingByName", "typeByName", "byName", "spellByName", "lookup", "imageByName",
    "BeginChild", "BeginTable", "BeginCombo_id", "PushStyleVar", "TableSetupScrollFreeze",
}
# The words at least one of which a literal must have to count as English text.
WORD_RX = re.compile(r"[A-Za-z][a-z]+|[A-Z]{2,}")


def looks_english(s):
    """Whether a literal reads as text for people: words with spaces, or a capitalised word."""
    t = PRINTF_RX.sub(" ", s)
    t = PLACEHOLDER_RX.sub(" ", t)
    t = t.split("##")[0]
    stripped = t.strip()
    if len(stripped) < 2:
        return False
    if re.fullmatch(r"[a-z0-9_.:/\\\-+*#=<>|&!?\[\]{}()@$^~;, ]*", stripped) and " " not in stripped:
        return False  # an id, key, path or operator
    if re.fullmatch(r"[\w.-]+\.[A-Za-z0-9]{1,5}", stripped):
        return False  # a file name
    if ("/" in stripped or "\\" in stripped) and " " not in stripped:
        return False
    if re.fullmatch(r"[A-Za-z][a-z0-9]+(?:[A-Z][a-z0-9]*)+", stripped) or re.fullmatch(r"[A-Z0-9_]+", stripped):
        return False  # CamelCase or CONSTANT: a name in the game's files
    if re.fullmatch(r"[a-z][a-z0-9_]*", stripped):
        return False
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)+", stripped):
        return False  # a dotted id ("window.mapInfo", "file.save")
    if re.fullmatch(r"[\w ]+\.(?:c|cpp|wnd|h):[\w%]*", stripped, re.I):
        return False  # a nox.csf label ("cdecode.c:PlayerJoined", "MainMenu.wnd:Solo")
    if re.fullmatch(r"(?:(?:Ctrl|Shift|Alt|Cmd)\+)+\S*", stripped):
        return False  # a shortcut's keys (shown as the keyboard has them)
    if re.fullmatch(r"\w+ ?=", stripped):
        return False  # a settings file's "Name = "
    words = WORD_RX.findall(stripped)
    if not words:
        return False
    if " " in stripped and len(words) >= 2:
        return True
    if re.search(r"\s-\s+[A-Z]{2,}\b", stripped):
        return True  # (" - WIP": a suffix put after shown text)
    return bool(re.match(r"[A-Z][a-z]+", stripped)) or (len(words) >= 2)


def allow_files():
    return sorted(os.path.join(HERE, f) for f in os.listdir(HERE) if re.match(r"lint_allow.*\.txt$", f))


def read_allow():
    files, literals = [], set()
    text = "\n".join(read(p) for p in allow_files())
    for raw in text.split("\n"):
        line = raw.rstrip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("file "):
            files.append(line[5:].split("#")[0].strip())
            continue
        m = re.match(r'^(\S+?): (".*")\s*$', line)
        if m:
            literals.add((m.group(1), m.group(2)))
    return files, literals


def quoted(s):
    return '"' + s.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t").replace('"', '\\"') + '"'


def file_allowed(rel, patterns):
    import fnmatch
    return any(fnmatch.fnmatch(rel, p) for p in patterns)


def lint_file(rel, text):
    """The literals in a file that look like English text and aren't in a NOX_TR call or a call
    whose text no player sees: [(line, literal)]."""
    toks = tokens(text)
    hits = []
    stack = []  # for each open '(' / '{' / '[': the call's name (or None)
    for idx, (k, v, line) in enumerate(toks):
        if k == "punct":
            if v in "([{":
                name = None
                if v == "(" and idx > 0 and toks[idx - 1][0] == "id":
                    name = toks[idx - 1][1]
                stack.append(name)
            elif v in ")]}":
                if stack:
                    stack.pop()
            continue
        if k != "str":
            continue
        if any(s in MACROS or s in SKIP_CALLS for s in stack if s):
            continue
        prev = toks[idx - 1] if idx > 0 else ("", "", 0)
        prev2 = toks[idx - 2] if idx > 1 else ("", "", 0)
        nxt = toks[idx + 1] if idx + 1 < len(toks) else ("", "", 0)
        nxt2 = toks[idx + 2] if idx + 2 < len(toks) else ("", "", 0)
        if prev[0] == "str":
            continue  # (a concatenation's later part: judged with the first)
        if (prev[1] == "=" and prev2[1] in "=!") or (nxt[1] in "=!" and nxt2[1] == "="):
            continue  # compared
        if prev[1] == "case" or prev[1] == "[" or (prev[0] == "id" and prev[1] == "operator"):
            continue
        # The whole concatenation.
        j, full = idx, ""
        while j < len(toks) and toks[j][0] == "str":
            full += toks[j][1]
            j += 1
        if looks_english(full):
            hits.append((line, full))
    return hits


def lint(use_allow=True):
    files, literals = read_allow() if use_allow else ([], set())
    hits, notes, seen = [], [], set()
    lint_sources = list(source_files(LINT_ROOTS))
    lint_sources += [(rel, os.path.join(ROOT, rel)) for rel in LINT_FILES
                     if os.path.isfile(os.path.join(ROOT, rel)) and all(rel != r for r, _ in lint_sources)]
    for rel, full in lint_sources:
        if file_allowed(rel, files):
            continue
        for line, s in lint_file(rel, read(full)):
            q = quoted(s)
            seen.add((rel, q))
            if (rel, q) in literals:
                continue
            hits.append(f"{rel}:{line}: player-facing English without NOX_TR: {q}"
                        " (wrap it, or add it to noxsdl/l10n/lint_allow.txt if no player sees it)")
    for rel, q in sorted(literals - seen):
        notes.append(f"lint_allow.txt lists {rel}: {q}, no longer in the source (remove it)")
    return hits, notes


def cmd_lint(args):
    hits, notes = lint(use_allow="--all" not in args)
    if "--baseline" in args:
        # Writes every hit as an allowed literal (the starting point, then reviewed by hand).
        files, literals = read_allow()
        rows = sorted({(h.split(":")[0], h.split(": player-facing English without NOX_TR: ")[1].rsplit(" (wrap it", 1)[0])
                       for h in hits})
        with open(ALLOW_FILE, "a", encoding="utf-8", newline="\n") as f:
            for rel, q in rows:
                f.write(f"{rel}: {q}\n")
        print(f"{len(rows)} literals added to lint_allow.txt")
        return 0
    sys.stdout.reconfigure(encoding="utf-8")
    for h in hits:
        print(h)
    for n in notes:
        print("note: " + n)
    print(f"{len(hits)} hits")
    return 1 if hits else 0


def main(argv):
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(__doc__)
        return 0
    cmd, args = argv[0], argv[1:]
    if cmd == "extract": return cmd_extract()
    if cmd == "check": return cmd_check()
    if cmd == "todo": return cmd_todo(args)
    if cmd == "apply": return cmd_apply(args)
    if cmd == "stats": return cmd_stats()
    if cmd == "lint": return cmd_lint(args)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
