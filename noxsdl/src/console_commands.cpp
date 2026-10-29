// The console's commands (app.hpp): the game's table and noxsdl's own switches.
#include "app.hpp"
#include "effects_draw.hpp"

#include "noxcore/io/file.hpp"
#include "noxcore/lobby/listing.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/util/text.hpp"
#include "noxcore/version.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <utility>

namespace fs = std::filesystem;

namespace noxsdl {

/// The console's commands: the game's table (names, sub-commands and who
/// may run them, help texts ParseCmd.c:<words>help), what a client can do with them here, and
/// noxsdl's own debug switches (the command line's --no-light and such) under "noxsdl".
std::vector<Console::Command> consoleCommands(App& app)
{
    using C = Console::Command;
    using Args = Console::Args;
    Console& con = *app.console;
    // notYet: a stand-in for a command noxsdl doesn't do yet.
    const auto notYet = [&con](const char* what) {
        return [&con, what](const Args&) { con.print(std::string(what) + ": not in NoX Core yet", Grey); };
    };
    // The host's: run whole on the game we host (our player issuing it); joined to another's
    // server, nothing to run them on.
    const auto hostOnly = [&app, &con](const Args&) {
        if (!app.host) {
            con.print("a host's command: NoX Core isn't hosting this game", Grey);
            return;
        }
        const std::uint32_t me = app.live ? app.live->client().netCode() : 0;
        // (run on the server's thread; what it printed comes back at a frame's start)
        app.host->command(std::string(con.currentLine()), me, [&con](const std::vector<std::string>& lines) {
            for (const auto& line : lines) {
                con.print(line, White);
            }
        });
    };
    // onOff: a switch's new state: "on", "1", "yes" or "true" turn it on, any other word off; no word flips it.
    const auto onOff = [](const Args& a, bool now) {
        if (a.empty()) {
            return !now;
        }
        std::string v = a[0];
        std::ranges::transform(v, v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return v == "on" || v == "1" || v == "yes" || v == "true";
    };
    // join: the words from `from` on, joined by spaces.
    const auto join = [](const Args& a, std::size_t from = 0) {
        std::string s;
        for (std::size_t i = from; i < a.size(); ++i) {
            s += (s.empty() ? "" : " ") + a[i];
        }
        return s;
    };
    const auto columns = [&con](const std::vector<std::string>& names, std::size_t perLine) {
        // A few to a line, as list maps prints them (four).
        std::string line;
        for (std::size_t i = 0; i < names.size(); ++i) {
            line += names[i] + "  ";
            if ((i + 1) % perLine == 0) {
                con.print(line, Yellow);
                line.clear();
            }
        }
        if (!line.empty()) {
            con.print(line, Yellow);
        }
    };
    // thingsOf: the names of things of class `cls`; with a subMask, only those with (wantSub) or without
    // any of those subclass bits.
    const auto thingsOf = [&app](std::uint32_t cls, std::uint32_t subMask, bool wantSub) {
        std::vector<std::string> out;
        for (const auto& t : app.things.things) {
            const std::uint32_t c = nox::things::thingClasses(t);
            if (!(c & cls)) {
                continue;
            }
            const bool sub = (nox::things::thingSubclasses(t) & subMask) != 0;
            if (subMask == 0 || sub == wantSub) {
                out.push_back(t.name);
            }
        }
        return out;
    };
    namespace OC = nox::things::ObjectClass;
    constexpr std::uint32_t kStaffs = nox::things::ObjectSubclass::StaffWands;

    // The game's table in its order: name, who may run it (Console::Command's flags: 1 host, 2 client,
    // 0x10 cheat), help id (empty: filled in below), what runs, sub-commands.
    std::vector<C> c;
    // The cheat word (the table's first entry, its name kept scrambled).
    c.push_back({"racoiaws", 0x47, "noHelp", [&app, &con](const Args&) {
                     app.cheats = !app.cheats;
                     con.print(app.cheats ? "cheats on" : "cheats off", Yellow);
                 }});
    c.push_back({"allow", 1, "", {}, {{"user", 1, "", hostOnly}, {"IP", 1, "", hostOnly}}});
    c.push_back({"audtest", 3, "", notYet("audtest (there's no sound yet)")});
    c.push_back({"ban", 1, "", hostOnly});
    c.push_back({"bind", 3, "", [&app, &con, join](const Args& a) {
                     // A chat macro: the key says the line.
                     if (a.size() < 2 || !nox::config::findKey(a[0])) {
                         con.print(con.text("bindhelp"), Red);
                         return;
                     }
                     app.macros[a[0]] = join(a, 1);
                 }});
    c.push_back({"broadcast", 1, "", hostOnly});
    c.push_back({"cheat", 0x11, "", {},
                 {{"ability", 1, "", hostOnly},
                  {"goto", 1, "", hostOnly},
                  {"health", 1, "", hostOnly},
                  {"mana", 1, "", hostOnly},
                  {"level", 1, "", hostOnly},
                  {"spells", 1, "", hostOnly},
                  {"gold", 1, "", hostOnly},
                  {"re-enter", 1, "", hostOnly}}});
    c.push_back({"clear", 3, "", [&con](const Args&) { con.clear(); }});
    c.push_back({"exec", 1, "", hostOnly});
    c.push_back({"execrul", 3, "", [&app, &con](const Args& a) {
                     // Each line of a .rul file in the game folder run as typed.
                     if (a.empty()) {
                         con.print(con.text("execrulhelp"), Red);
                         return;
                     }
                     fs::path file = app.gameDir / a[0];
                     if (!file.has_extension()) {
                         file += ".rul";
                     }
                     std::ifstream in(file);
                     if (!in) {
                         con.print("can't read " + file.string(), Red);
                         return;
                     }
                     std::string line;
                     while (std::getline(in, line)) {
                         if (!line.empty() && line.back() == '\r') {
                             line.pop_back();
                         }
                         if (!line.empty()) {
                             con.run(line);
                         }
                     }
                 }});
    const auto leave = [&app](const Args&) {
        if (app.live) {
            app.live->quit(); // leave the game properly
        } else {
            app.quitRequested = true;
        }
    };
    c.push_back({"exit", 3, "", leave});
    c.push_back({"gamma", 3, "", notYet("gamma")});
    c.push_back({"help", 3, "", {}}); // (filled in below: it lists the table)
    c.push_back({"image", 3, "", notYet("image")});
    c.push_back({"kick", 1, "", hostOnly});
    c.push_back({"list", 3, "", {},
                 {{"armor", 3, "", [=](const Args&) { columns(thingsOf(OC::Armor, 0, false), 4); }},
                  {"maps", 3, "", [&app, columns](const Args&) {
                       std::vector<std::string> maps;
                       std::error_code ec;
                       for (const auto& d : fs::directory_iterator(app.gameDir / "maps", ec)) {
                           if (d.is_directory() && fs::exists(nox::resolveNoCase(d.path() / (d.path().filename().string() + ".map")))) {  // a map is a folder holding <name>.map (any case)
                               maps.push_back(d.path().filename().string());
                           }
                       }
                       std::ranges::sort(maps);
                       columns(maps, 4);
                   }},
                  {"spells", 3, "", [&app, columns](const Args&) {
                       std::vector<std::string> names;
                       for (const auto& s : app.things.spells) {
                           names.push_back(s.id);
                       }
                       columns(names, 3);
                   }},
                  {"staffs", 3, "", [=](const Args&) { columns(thingsOf(OC::Weapon, kStaffs, true), 3); }},
                  {"weapons", 3, "", [=](const Args&) { columns(thingsOf(OC::Weapon, kStaffs, false), 4); }},
                  {"users", 3, "", [&app, &con](const Args&) {
                       if (!app.live) {
                           return;
                       }
                       for (const auto& [code, p] : app.live->client().world().players()) {
                           con.print(p.name + "  (" + std::to_string(code) + ")", Yellow);
                       }
                   }}}});
    c.push_back({"lock", 3, "", notYet("lock")});
    c.push_back({"load", 0x11, "", hostOnly});
    c.push_back({"log", 0x13, "", {},
                 {{"console", 3, "", [&app, &con](const Args&) {
                       con.logTo((app.gameDir / "console.log").string());
                       con.print("logging to " + (app.gameDir / "console.log").string(), Yellow);
                   }},
                  {"file", 3, "", [&app, &con](const Args& a) {
                       const fs::path file = app.gameDir / (a.empty() ? std::string("console.log") : a[0]);
                       con.logTo(file.string());
                       con.print("logging to " + file.string(), Yellow);
                   }},
                  {"stop", 3, "", [&con](const Args&) { con.logTo({}); }}}});
    c.push_back({"macros", 3, "", {}, {{"on", 1, "", hostOnly}, {"off", 1, "", hostOnly}}});
    c.push_back({"menu", 3, "", {},
                 {{"vidopt", 3, "", [&app, &con](const Args&) {
                       con.close(app.window);
                       openOptions(app);
                   }},
                  {"options", 3, "", [&app, &con](const Args&) {
                       con.close(app.window);
                       openOptions(app);
                   }}}});
    // mute / unmute: a player in the game, by name in any case (playerByName),
    // gets status bit 8 here only; their chat and bubbles stop.
    const auto mutePlayer = [&app, &con, join](const Args& a, bool mute) {
        if (a.empty()) {
            con.print(con.text(mute ? "mutehelp" : "unmutehelp"), Red);
            return;
        }
        const std::string name = join(a);
        if (app.live) {
            for (const auto& [code, p] : app.live->client().world().players()) {
                if (nox::util::lowered(p.name) == nox::util::lowered(name)) {
                    if (mute) {
                        app.muted.insert(code);
                    } else {
                        app.muted.erase(code);
                    }
                    con.print(con.text(mute ? "Muted" : "UnMuted", {name}), Red);
                    return;
                }
            }
        }
        con.print(con.text("UserNotFound", {name}), Red);
    };
    c.push_back({"mute", 3, "", [mutePlayer](const Args& a) { mutePlayer(a, true); }});
    c.push_back({"quit", 3, "", leave});
    c.push_back({"say", 3, "", [&app, join](const Args& a) {
                     if (app.live && !a.empty()) {
                         app.live->client().say(join(a));
                     }
                 }});
    const C quality{"quality", 3, "", {},
                    {{"modem", 3, "", notYet("set quality")},
                     {"isdn", 3, "", notYet("set quality")},
                     {"cable", 3, "", notYet("set quality")},
                     {"T1", 3, "", notYet("set quality")},
                     {"LAN", 3, "", notYet("set quality")}}};
    c.push_back({"set", 3, "", {},
                 {{"armor", 1, "", hostOnly},       {"cycle", 1, "", hostOnly},
                  {"frameratelimiter", 0x11, "", hostOnly}, {"god", 0x11, "", hostOnly},
                  {"lessons", 1, "", hostOnly},     {"monsters", 0x11, "", hostOnly},
                  {"name", 1, "", hostOnly},        {"netdebug", 0x13, "", notYet("set netdebug")},
                  {"ob", 0x13, "", notYet("set ob")}, {"players", 1, "", hostOnly},
                  quality,                          {"sage", 0x11, "", hostOnly},
                  {"savedebugcmd", 1, "", hostOnly}, {"spell", 1, "", hostOnly},
                  {"spellpoints", 0x11, "", hostOnly}, {"staff", 0x11, "", hostOnly},
                  {"staffs", 0x11, "", hostOnly},   {"sysop", 1, "", hostOnly},
                  {"time", 1, "", hostOnly},        {"weapon", 1, "", hostOnly},
                  {"weapons", 0x11, "", hostOnly},  {"team", 0x31, "", hostOnly},
                  {"mode", 0x31, "", hostOnly}}});
    c.push_back({"show", 3, "", {},
                 {{"bindings", 3, "", [&app, &con](const Args&) {
                       // The chat macros.
                       for (const auto& [key, line] : app.macros) {
                           con.print(key + " = " + line, White);
                       }
                   }},
                  {"game", 3, "", [&app, &con](const Args&) {
                       con.print(std::string("NoX Core ") + nox::version(), Yellow);
                       if (app.live) {
                           const auto& client = app.live->client();
                           con.print("server " + app.liveHost + ", map " + client.mapName(), Yellow);
                           con.print(std::string("game ") +
                                         std::string(nox::lobby::gameModeName(client.world().game().gameFlags)) +
                                         ", " + std::to_string(client.world().players().size()) + " players",
                                     Yellow);
                       }
                   }},
                  {"motd", 3, "", [&app, &con](const Args&) {
                       if (app.live) {
                           con.print(app.live->motd(), White);
                       }
                   }},
                  {"rank", 3, "", [&app, &con](const Args&) {
                       if (!app.live) {
                           return;
                       }
                       for (const auto& [code, p] : app.live->client().world().players()) {
                           con.print(p.name + "  kills " + std::to_string(p.kills) + "  lessons " +
                                         std::to_string(p.lessons),
                                     Yellow);
                       }
                   }},
                  {"perfmon", 3, "", [&app](const Args&) { app.showFps = !app.showFps; }},
                  {"extents", 0x13, "", notYet("show extents")},
                  {"gui", 3, "", [&app](const Args&) { app.renderGui = !app.renderGui; }},
                  {"ai", 0x11, "", hostOnly},
                  {"info", 7, "", [&app, &con](const Args&) {
                       if (!app.live) {
                           return;
                       }
                       const auto& client = app.live->client();
                       if (const auto* self = client.world().drawable(client.netCode())) {
                           con.print("net code " + std::to_string(client.netCode()) + " at " + std::to_string(self->x) +
                                         "," + std::to_string(self->y) + ", frame " + std::to_string(client.frame()),
                                     Yellow);
                       }
                   }},
                  {"mem", 0x13, "", notYet("show mem")},
                  {"netstat", 3, "", [&app, &con](const Args&) {
                       if (!app.live) {
                           return;
                       }
                       for (const auto& [code, p] : app.live->client().world().players()) {
                           con.print(p.name + "  " + std::to_string(p.latency) + " ms", Yellow);
                       }
                   }},
                  {"mmx", 3, "", [&con](const Args&) { con.print("no MMX: not used here", Yellow); }},
                  {"seq", 0x13, "", notYet("show seq")}}});
    c.push_back({"sysop", 7, "", notYet("sysop")});
    c.push_back({"telnet", 1, "", {}, {{"on", 1, "", hostOnly}, {"off", 1, "", hostOnly}}});
    c.push_back({"unset", 3, "", {},
                 {{"god", 1, "", hostOnly},
                  {"frameratelimiter", 1, "", hostOnly},
                  {"netdebug", 3, "", notYet("unset netdebug")},
                  {"sage", 1, "", hostOnly}}});
    c.push_back({"unmute", 3, "", [mutePlayer](const Args& a) { mutePlayer(a, false); }});
    c.push_back({"unbind", 3, "", [&app](const Args& a) {
                     if (!a.empty()) {
                         app.macros.erase(a[0]);
                     }
                 }});
    c.push_back({"unlock", 3, "", notYet("unlock")});
    c.push_back({"watch", 3, "", notYet("watch")});
    c.push_back({"window", 3, "", notYet("window")});
    c.push_back({"startSoloQuest", 5, "", hostOnly});
    c.push_back({"ques", 3, "", {}}); // (as help)
#ifndef NOX_NO_TEST_COMMANDS
    // noxcpp's test commands on the game we host (nx help): spawn, give, god, heal, goto...
    // (Not in release builds: NOX_TEST_COMMANDS=OFF leaves them out, so a host can't cheat.)
    c.push_back({"nx", Console::kNoxsdl, "noxcpp's test commands on a game NoX Core hosts (nx help)", hostOnly});
    // (Tests) a weather drawn here as a mod's nox.weather would set it, in place of the server's.
    c.push_back({"weather", Console::kNoxsdl,
                 "rain|snow|storm|none [strength 0-1] [wind angle] [wind speed 0-1] [fade frames] | off: the weather drawn here (tests)",
                 [&app, &con](const Args& a) {
                     namespace wx = nox::modes::script::weather;
                     const std::string kind = a.empty() ? std::string() : a[0];
                     if (kind == "off") {
                         app.weatherTest.reset();
                         con.print("weather: the server's", Yellow);
                         return;
                     }
                     const wx::Kind k = kind == "rain" ? wx::Kind::Rain : kind == "snow" ? wx::Kind::Snow : kind == "storm" ? wx::Kind::Storm : wx::Kind::None;
                     if (k == wx::Kind::None && kind != "none") {
                         con.print("weather rain|snow|storm|none [strength 0-1] [wind angle] [wind speed 0-1] [fade frames] | off", Grey);
                         return;
                     }
                     const auto number = [&a](std::size_t i, double fallback) { return a.size() > i ? std::strtod(a[i].c_str(), nullptr) : fallback; };
                     wx::Told told;
                     told.state = wx::make(k, number(1, 1.0), number(2, 0.0), number(3, 0.0));
                     told.fade = static_cast<std::uint16_t>(std::clamp(number(4, 0.0), 0.0, 65535.0));
                     told.serial = (app.weatherTest ? app.weatherTest->serial : 0x80000000u) + 1;
                     app.weatherTest = told;
                     con.print("weather " + kind + (app.weather ? "" : " (not drawn: no GPU shaders)"), Yellow);
                 }});
#endif

    // noxsdl's own: the command line's debug switches, at any time.
    // A switch changed as the Options window would change it, so it's applied the same way.
    const auto extras = [&app](auto change) {
        OptionsWindow::Extras e = currentExtras(app);
        change(e);
        applyExtras(app, e);
    };
    const auto says = [&con](const char* what, bool on) { con.print(std::string(what) + (on ? " on" : " off"), Yellow); };
    // A plain switch: an App flag set (or flipped) and said.
    const auto toggle = [&app, onOff, says](const char* name, bool App::*flag) {
        return [&app, onOff, says, name, flag](const Args& a) {
            app.*flag = onOff(a, app.*flag);
            says(name, app.*flag);
        };
    };
    C noxsdl{"noxsdl", Console::kNoxsdl, "noxsdl's own switches (the command line's)", {}, {}};
    noxsdl.sub = {
#ifndef NOX_NO_TEST_COMMANDS
        // (Not in release builds: lighting and line of sight off, and the ambient light below,
        // would show a game's dark places and what's behind walls.)
        {"light", Console::kNoxsdl, "[on|off]: the lighting (--no-light)", toggle("light", &App::lighting)},
        {"sight", Console::kNoxsdl, "[on|off]: line of sight (--sight)", [&app, onOff, says](const Args& a) {
             const bool now = app.lineOfSight.value_or(app.live != nullptr);
             app.lineOfSight = onOff(a, now);
             says("sight", *app.lineOfSight);
         }},
#endif
        {"softedges", Console::kNoxsdl, "[on|off]: the sight's soft edge this session (Options' Soft Shadow Edge)",
         toggle("softedges", &App::softShadowEdge)},
        {"fade", Console::kNoxsdl, "[on|off]: what leaves sight fades out this session (Options' Fade Objects)",
         toggle("fade", &App::fadeObjects)},
        {"walls", Console::kNoxsdl, "classic [on|off]: the game's own wall culling (cuts at the view's edges, corners cut through)",
         [&app, onOff, says, &con](const Args& a) {
             if (a.empty() || a[0] != "classic") {
                 con.print("noxsdl walls classic [on|off]", Yellow);
                 return;
             }
             app.classicWalls = onOff(Args(a.begin() + 1, a.end()), app.classicWalls);
             says("walls classic", app.classicWalls);
         }},
        {"nameplates", Console::kNoxsdl, "[on|off]: names over players", toggle("nameplates", &App::namePlates)},
        {"pending", Console::kNoxsdl,
         "[shaded|final|hidden]: a map editor session's edits not yet published, in the game: greyed (shaded), as they will "
         "look (final), or not shown (hidden); remembered",
         [&app, &con](const Args& a) {
             if (!a.empty()) {
                 std::string all;
                 for (const auto& w : a) all += w; // ("as final")
                 if (const auto v = nox::mapedit::parsePendingView(all)) {
                     app.pendingView = *v;
                     saveExtras(app, currentExtras(app));
                 } else {
                     con.print("noxsdl pending [shaded|final|hidden]", Yellow);
                     return;
                 }
             }
             con.print(std::string("pending changes: ") + nox::mapedit::pendingViewName(app.pendingView) +
                           (app.editSession && app.editSession->inSession() ? "" : " (in a map editor session)"),
                       Yellow);
         }},
        {"fps", Console::kNoxsdl, "[on|off]: the frame-rate counter (--no-fps)", toggle("fps", &App::showFps)},
        {"killfeed", Console::kNoxsdl, "[on|off|test]: the kill feed (Options' Kill Feed); test shows three kills",
         [&app, onOff, says](const Args& a) {
             if (!a.empty() && a[0] == "test" && app.live) {
                 // Three made-up kills with us in them: a weapon, a damage type, a bolt's bow.
                 const auto me = app.live->client().netCode();
                 const auto frame = gameFrame(app);
                 // KillLine: attacker, assist, victim, weapon, kind (1 a thing type, 2 a damage type), frame.
                 app.kills.push_back({me, 0, me, typeIdOf(app.things, "WarHammer"), 1, frame});
                 app.kills.push_back({me, me, me, 1, 2, frame});
                 app.kills.push_back({0, 0, me, typeIdOf(app.things, "CrossBow"), 1, frame});
                 return;
             }
             app.killFeed = onOff(a, app.killFeed);
             says("killfeed", app.killFeed);
         }},
        {"banner", Console::kNoxsdl, "test [defeat]: the game-over banner with made-up words",
         [&app](const Args& a) {
             if (a.empty() || a[0] != "test" || !app.strings) {
                 return;
             }
             const bool defeat = a.size() > 1 && a[1] == "defeat";
             const auto text = [&](const char* id) { return tableText(app, id).value_or(id); };
             std::string words = text("cdecode.c:TimeLimitReached");
             words += defeat ? text("cdecode.c:DM_Loss") : text("cdecode.c:DM_MaleVictory");
             if (const auto at = words.find("%s"); at != std::string::npos) {
                 words.replace(at, 2, "Bramble");
             }
             app.banner = App::GameOverBanner{words, defeat, SDL_GetTicks()};
         }},
        {"charges", Console::kNoxsdl, "test [N MAX] | off: the weapon panel's charges bar and count with made-up numbers",
         [&app](const Args& a) {
             if (!app.inventory) {
                 return;
             }
             if (!a.empty() && a[0] == "test") {
                 const int n = a.size() > 1 ? std::atoi(a[1].c_str()) : 3;
                 const int max = a.size() > 2 ? std::atoi(a[2].c_str()) : 8;
                 app.inventory->setTestCharges(std::pair{n, max});
             } else {
                 app.inventory->setTestCharges(std::nullopt);
             }
         }},
        {"gamma", Console::kNoxsdl, "[1-10]: the brightness (Insert / Delete; noxcore.cfg's Gamma)",
         [&app, &con](const Args& a) {
             if (!a.empty()) {
                 app.gamma = std::clamp(std::atoi(a[0].c_str()), 1, 10);
             }
             con.print("gamma " + std::to_string(app.gamma) +
                           (app.shaders && app.shaders->gamma() ? "" : " (needs the GPU renderer's shaders)"),
                       Yellow);
         }},
        {"hdimages", Console::kNoxsdl, "[on|off]: images from the HD image pack (Options' Use HD Images, --hd-images)",
         [&app, &con, onOff, extras](const Args& a) {
             if (app.imagePack.empty()) {
                 con.print("no image pack (video_hd.nxp beside noxcore.exe or in the game folder, or --image-pack)", Grey);
                 return;
             }
             const bool on = onOff(a, app.sprites && app.sprites->usingPack());
             extras([on](OptionsWindow::Extras& e) { e.hdImages = on; });
             con.print(std::string("hdimages ") + (app.sprites && app.sprites->usingPack() ? "on" : "off"), Yellow);
         }},
        {"bloom", Console::kNoxsdl, "[on|off|0-1]: bloom, or how much (--no-bloom, --bloom N)",
         [&app, &con, onOff, says, extras](const Args& a) {
             if (!a.empty() && (std::isdigit(static_cast<unsigned char>(a[0][0])) || a[0][0] == '.')) {  // a number: the strength, and bloom on
                 app.bloomStrength = std::clamp(std::strtof(a[0].c_str(), nullptr), 0.0f, 1.0f);
                 app.bloom.reset(); // (made again with the new strength)
                 extras([](OptionsWindow::Extras& e) { e.bloom = true; });
                 con.print("bloom " + std::to_string(app.bloomStrength), Yellow);
                 return;
             }
             const bool on = onOff(a, app.bloom != nullptr);
             extras([on](OptionsWindow::Extras& e) { e.bloom = on; });
             says("bloom", app.bloom != nullptr);
         }},
        {"gridlight", Console::kNoxsdl, "[on|off]: the game's light grid instead of per-pixel (--grid-light)",
         [&app, onOff, says, extras](const Args& a) {
             const bool grid = onOff(a, !app.light.perPixel());
             extras([grid](OptionsWindow::Extras& e) { e.perPixelLight = !grid; });
             says("gridlight", !app.light.perPixel());
         }},
        {"stockeffects", Console::kNoxsdl, "[on|off]: the effects' pixel shapes and sprites: Smooth FX and HD Spell FX off (--stock-effects)",
         [onOff, says, extras](const Args& a) {
             const bool stock = onOff(a, !fxdraw::smoothOn() && !fxdraw::spellFxOn());
             extras([stock](OptionsWindow::Extras& e) {
                 e.smoothEffects = !stock;
                 e.hdSpellFx = !stock;
             });
             says("stockeffects", !fxdraw::smoothOn() && !fxdraw::spellFxOn());
         }},
#ifndef NOX_NO_TEST_COMMANDS
        {"ambient", Console::kNoxsdl, "R G B | off: the ambient light instead of the map's (--ambient)",
         [&app, &con](const Args& a) {
             if (a.size() >= 3) {
                 Rgb c{};
                 for (std::size_t i = 0; i < 3; ++i) {
                     c[i] = static_cast<std::uint8_t>(std::clamp(std::atoi(a[i].c_str()), 0, 255));
                 }
                 app.ambient = c;
             } else {
                 app.ambient.reset();
             }
             con.print(app.ambient ? "ambient set" : "ambient: the map's", Yellow);
         }},
        {"leavenotice", Console::kNoxsdl, "test | off: the leaving notice counting down, without leaving",
         [&app](const Args& a) {
             app.leavingNoticeTestMs = !a.empty() && a[0] == "test" ? std::max<std::uint64_t>(SDL_GetTicks(), 1) : 0;
         }},
#endif
        {"videomode", Console::kNoxsdl, "WxH: how much of the world a game shows (--video-mode)",
         [&app, &con](const Args& a) {
             const auto wh = a.empty() ? std::nullopt : parseSize(a[0]);
             if (!wh || !app.input) {
                 con.print("videomode " + std::to_string(app.viewWidth) + "x" + std::to_string(app.viewHeight), Yellow);
                 return;
             }
             auto config = app.input->config();
             config.set("VideoMode", std::to_string(wh->first) + " " + std::to_string(wh->second) + " 16");  // VideoMode's third word: the colour depth
             applySettings(app, config);
         }},
        {"videosize", Console::kNoxsdl, "N: the game window, 40-100% (--video-size)",
         [&app](const Args& a) {
             if (!a.empty()) {
                 stepWindowSize(app, std::atoi(a[0].c_str()) - app.windowSize);
             }
         }},
        {"zoom", Console::kNoxsdl, "Z: the viewer's zoom (--zoom; a game's view is fixed)",
         [&app, &con](const Args& a) {
             if (app.live) {
                 con.print("a game's view is fixed: videomode / videosize", Grey);
             } else if (!a.empty()) {
                 app.camera.zoom = std::clamp(std::strtof(a[0].c_str(), nullptr), 0.125f, 16.0f);
             }
         }},
        {"renderer", Console::kNoxsdl, "which one is drawing (--gpu / --no-gpu, at start)",
         [&app, &con](const Args&) {
             con.print(std::string("renderer ") + SDL_GetRendererName(app.renderer) +
                           (app.shaders ? ", with NoX Core's shaders" : ""),
                       Yellow);
         }},
    };
    c.push_back(noxsdl);
    // The world in first person (view3d.hpp); not listed by help.
    c.push_back({"view3d", Console::kNoxsdl, "[on|off] | tilt [DEGREES] | walls [PERCENT]: the world in first person",
                 [&app, &con, onOff, says](const Args& a) {
                     if (!a.empty() && (a[0] == "tilt" || a[0] == "walls")) {
                         // Its look: how far pictures lean back, how much taller walls stand.
                         const bool tilt = a[0] == "tilt";
                         if (a.size() > 1) {
                             const float v = std::strtof(a[1].c_str(), nullptr);
                             tilt ? app.view3d.setTilt(v) : app.view3d.setWallRaise(v / 100);
                         }
                         con.print(tilt ? "view3d tilt " + std::to_string(std::lround(app.view3d.tilt()))
                                        : "view3d walls " + std::to_string(std::lround(app.view3d.wallRaise() * 100)),
                                   Yellow);
                         return;
                     }
#ifndef NOX_NO_TEST_COMMANDS
                     if (!a.empty() && a[0] == "sight") {
                         // (Tests: the 3D view held to the 2D view's sight, as release builds always have it.)
                         app.view3dSight = onOff(Args(a.begin() + 1, a.end()), app.view3dSight);
                         says("view3d sight", app.view3dSight);
                         return;
                     }
#endif
                     const bool on = onOff(a, app.view3d.enabled());
                     app.view3d.setEnabled(on);
                     says("view3d", on);
                 }});

    // The help ids: the command's words joined, then "help" (ParseCmd.c:showbindingshelp...).
    const std::function<void(std::vector<C>&, const std::string&)> helpIds = [&](std::vector<C>& t, const std::string& at) {
        for (auto& cmd : t) {
            if (cmd.help.empty() && !(cmd.flags & Console::kNoxsdl)) {
                cmd.help = at + cmd.name + "help";
            }
            helpIds(cmd.sub, at + cmd.name);
        }
    };
    helpIds(c, "");
    // help [command]: the commands, or a command's sub-commands, with their help.
    const auto help = [&app, &con](const Args& a) {
        const std::vector<C>* table = &app.console->commands();
        std::string at;
        // Each word walks down the table (matched in any case): its help printed, its sub-commands next.
        for (const auto& word : a) {
            const auto it = std::ranges::find_if(*table, [&](const C& x) { return nox::util::equalsNoCase(x.name, word); });
            if (it == table->end()) {
                break;
            }
            con.print((it->flags & Console::kNoxsdl) ? it->help : con.text(it->help), Yellow);
            table = &it->sub;
            at += it->name + " ";
        }
        for (const auto& x : *table) {
            if (x.name == "racoiaws" || x.name == "view3d") {
                continue; // (they aren't listed)
            }
            const bool ours = (x.flags & Console::kNoxsdl) != 0;
            const bool usable = ours || (x.flags & (con.client() ? Console::kClient : Console::kHost));  // grey: not for us here (a host's on a client, or the reverse)
            con.print(at + x.name + (ours ? "  -  " + x.help : ""), usable ? White : Grey);
        }
    };
    for (auto& cmd : c) {
        if (cmd.name == "help" || cmd.name == "ques") {
            cmd.run = help;
        }
    }
    return c;
}

} // namespace noxsdl
