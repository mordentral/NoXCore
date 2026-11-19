# Contributing to NoX Core

Bug reports and pull requests are welcome on GitHub. Please keep a change focused (one fix or
feature per pull request), build it on Windows or Linux as BUILDING.txt says, and run `ctest`
before you send it.

## Text that players see: always through the translations

NoX Core shows its own text (menus, Options, the HUD, hosting screens, the map editor, the
Content Editor) in the player's language. The game's own text comes from the installed game's
nox.csf; NoX Core's comes from `noxsdl/res/lang/<code>.txt`, copied to a `lang` folder beside the
program. English is built in and is the source: every other language translates it.

**The rule: whoever adds or changes English text that a player sees updates every language file
in the same commit.** `ctest` (the `l10n_check` test) and the automatic build fail otherwise.

### Writing text in the code

Wrap the English where it is used, with a stable key (lower-case, dotted: `area.screen.name`),
with the macros of noxcore/include/noxcore/strings/l10n.hpp:

```cpp
w->text = NOX_TR("menus.main.game_modes", "Game Modes");          // const std::string&
ImGui::TextUnformatted(NOX_TRC("editor.props.none", "Nothing selected"));  // const char*
if (ImGui::Button(NOX_TRI("editor.common.save", "Save")))          // a Dear ImGui label: its id stays English
status(nox::l10n::format(NOX_TR("editor.status.saved", "Saved {name}"), {{"name", name}}));
text = nox::l10n::format(NOX_TRP("game.votes", "{n} vote", "{n} votes", n), {{"n", n}});
static constexpr nox::l10n::Key kNames[] = {NOX_TRK("options.weather.off", "Off"), ...};  // tables: tr(kNames[i])
```

- The key and the English must be string literals at the call site.
- Values go in named `{placeholders}`, never by gluing strings together, so a translation can put
  them in any order. Counted text uses `NOX_TRP` (each language has its own plural forms).
- Never pass translated text to printf as its format.
- Not translated: log lines, the developer console, command-line help, names from the game's files
  (things, maps, nox.csf labels) and text whose exact bytes a file format or the network needs
  (what is saved, sent to other players, compared or parsed). Text that is both shown and used as
  a key keeps its English as the key and is translated only where it is shown.

### Updating the languages

```
python noxsdl/l10n/l10n.py extract           # the English catalog, noxsdl/res/lang/en.txt
python noxsdl/l10n/l10n.py todo all -o work.txt   # what each language lacks or has stale
python noxsdl/l10n/l10n.py apply ru work.txt # once per language, after filling in work.txt
python noxsdl/l10n/l10n.py check             # must pass
```

`work.txt` lists each language's entries under a `#### <code>` line: write the translation after
each `key = ` (one line each; a plural has a line per form).

Each entry in a language file sits under a `#@ <hash> [mt]: <English>` line. The hash is that of
the English it translates: when the English changes, the entry is *stale*, the program shows the
English instead, and `check` fails until it is translated again. `mt` marks a machine translation
nobody fluent has checked yet: a translation you write yourself, or one you have checked, goes in
with `apply ... --reviewed` (or drop the `mt` by hand). Machine translation is fine for a first
version when you don't speak a language: keep its `mt` mark.

`check` fails when a key is missing from a language, stale, or orphaned (no English any more), when
a translation's placeholders, printf directives, `##` ids or plural forms differ from the English's,
and when new English appears in the UI code (the client, the map editor and its model, the
Content Editor) without `NOX_TR`. Text no player sees goes in
`noxsdl/l10n/lint_allow*.txt` with the reason.

### Adding a language

Copy `noxsdl/res/lang/en.txt` to `<code>.txt` (`de`, `pl`...: the two-letter code), translate
each entry, keep its `#@` line, and run `check`. NoX Core picks the language from `Language = <code>`
in noxcore.cfg, else the language nox.csf is in, else the system's; a language without a file
shows English. Its letters must be in NoX Core's fonts: Latin, Cyrillic, Greek and Hangul are.
