// MSG_NOXCPP_EDIT's framing, chunking and bodies (session_proto.hpp gives each layout).
#include "noxcore/mapedit/session_proto.hpp"

#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/byte_writer.hpp"
#include "noxcore/mapedit/mod_files.hpp"
#include "noxcore/video/nxz.hpp"

#include <algorithm>

namespace nox::mapedit::session {

namespace {

void str(ByteWriter& w, const std::string& s)
{
    const std::size_t n = std::min<std::size_t>(s.size(), 0xFFFF);
    w.u16(static_cast<std::uint16_t>(n));
    w.bytes({reinterpret_cast<const std::uint8_t*>(s.data()), n});
}
std::string str(ByteReader& r)
{
    const std::uint16_t n = r.u16();
    return r.rawString(n);
}
void blob(ByteWriter& w, std::span<const std::uint8_t> b)
{
    w.u32(static_cast<std::uint32_t>(b.size()));
    w.bytes(b);
}
std::vector<std::uint8_t> blob(ByteReader& r)
{
    const std::uint32_t n = r.u32();
    const auto b = r.bytes(n);
    return {b.begin(), b.end()};
}
void key(ByteWriter& w, const ElementKey& k)
{
    w.u8(static_cast<std::uint8_t>(k.kind));
    w.i32(k.a);
    w.i32(k.b);
    str(w, k.name);
}
ElementKey key(ByteReader& r)
{
    ElementKey k;
    const std::uint8_t kind = r.u8();
    if (kind > static_cast<std::uint8_t>(ElementKey::Kind::MapInfo)) {
        throw FormatError("map editor session: unknown element kind");
    }
    k.kind = static_cast<ElementKey::Kind>(kind);
    k.a = r.i32();
    k.b = r.i32();
    k.name = str(r);
    return k;
}
void keys(ByteWriter& w, const std::set<ElementKey>& ks)
{
    w.u32(static_cast<std::uint32_t>(ks.size()));
    for (const auto& k : ks) {
        key(w, k);
    }
}
std::set<ElementKey> keys(ByteReader& r)
{
    std::set<ElementKey> out;
    const std::uint32_t n = r.u32();
    for (std::uint32_t i = 0; i < n; ++i) {
        out.insert(key(r));
    }
    return out;
}
void done(const ByteReader& r)
{
    if (!r.atEnd()) {
        throw FormatError("map editor session: bytes left over in a message");
    }
}
Role role(std::uint8_t v)
{
    if (v > static_cast<std::uint8_t>(Role::Owner)) {
        throw FormatError("map editor session: unknown role");
    }
    return static_cast<Role>(v);
}

} // namespace

const char* roleName(Role r)
{
    switch (r) {
    case Role::Player: return "player";
    case Role::Viewer: return "viewer";
    case Role::Editor: return "editor";
    case Role::Owner: return "host";
    }
    return "?";
}

const char* activityName(Activity a)
{
    switch (a) {
    case Activity::Editing: return "editing";
    case Activity::Idle: return "idle";
    case Activity::Playing: return "playing";
    }
    return "?";
}

// --- Framing ----------------------------------------------------------------------------------

namespace {
std::vector<std::uint8_t> whole(std::uint8_t sub, std::span<const std::uint8_t> body)
{
    std::vector<std::uint8_t> m;
    m.reserve(body.size() + 4);
    m.push_back(kOp);
    m.push_back(sub);
    m.push_back(static_cast<std::uint8_t>(body.size()));
    m.push_back(static_cast<std::uint8_t>(body.size() >> 8));
    m.insert(m.end(), body.begin(), body.end());
    return m;
}
} // namespace

std::vector<std::vector<std::uint8_t>> frame(std::uint8_t sub, std::span<const std::uint8_t> body, std::uint32_t& transfer)
{
    std::vector<std::vector<std::uint8_t>> out;
    if (body.size() <= kMaxBody) {
        out.push_back(whole(sub, body));
        return out;
    }
    const std::uint32_t id = ++transfer;
    const std::size_t count = (body.size() + kChunkBytes - 1) / kChunkBytes;
    for (std::size_t i = 0; i < count; ++i) {
        ByteWriter w;
        w.u8(sub);
        w.u32(id);
        w.u16(static_cast<std::uint16_t>(i));
        w.u16(static_cast<std::uint16_t>(count));
        const std::size_t at = i * kChunkBytes;
        w.bytes(body.subspan(at, std::min(kChunkBytes, body.size() - at)));
        out.push_back(whole(kChunk, w.data()));
    }
    return out;
}

std::optional<Message> unframe(std::span<const std::uint8_t> message)
{
    if (message.size() < 4 || message[0] != kOp) {
        return std::nullopt;
    }
    const std::size_t len = message[2] | (std::size_t{message[3]} << 8);
    if (message.size() < 4 + len) {
        return std::nullopt;
    }
    return Message{message[1], {message.begin() + 4, message.begin() + 4 + static_cast<std::ptrdiff_t>(len)}};
}

std::optional<Message> Reassembler::feed(std::span<const std::uint8_t> message)
{
    auto m = unframe(message);
    if (!m || m->sub != kChunk) {
        return m;
    }
    try {
        ByteReader r(m->body, "map editor session chunk");
        const std::uint8_t sub = r.u8();
        const std::uint32_t id = r.u32();
        const std::uint16_t index = r.u16();
        const std::uint16_t count = r.u16();
        const auto bytes = r.bytes(r.remaining());
        if (count == 0 || index >= count) {
            return std::nullopt;
        }
        if (!parts_.contains(id) && parts_.size() >= kMaxPending) {
            parts_.erase(parts_.begin()); // (the oldest transfer: a sender's ids only go up)
        }
        Parts& p = parts_[id];
        if (!p.chunks.empty() && (p.count != count || p.sub != sub)) {
            p.chunks.clear(); // (a transfer that changed its mind: started afresh)
        }
        p.sub = sub;
        p.count = count;
        p.chunks[index].assign(bytes.begin(), bytes.end());
        if (p.chunks.size() < p.count) {
            return std::nullopt;
        }
        Message out{p.sub, {}};
        for (auto& [i, c] : p.chunks) {
            out.body.insert(out.body.end(), c.begin(), c.end());
        }
        parts_.erase(id);
        return out;
    } catch (const FormatError&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> pack(std::span<const std::uint8_t> raw)
{
    ByteWriter w;
    if (raw.size() > kPackAbove) {
        const auto z = video::nxzCompress(raw);
        if (z.size() < raw.size()) {
            w.u8(1);
            w.u32(static_cast<std::uint32_t>(raw.size()));
            w.bytes(z);
            return std::move(w).take();
        }
    }
    w.u8(0);
    w.u32(static_cast<std::uint32_t>(raw.size()));
    w.bytes(raw);
    return std::move(w).take();
}

std::vector<std::uint8_t> unpack(std::span<const std::uint8_t> packed)
{
    ByteReader r(packed, "map editor session packed bytes");
    const std::uint8_t deflated = r.u8();
    const std::uint32_t size = r.u32();
    const auto rest = r.bytes(r.remaining());
    if (deflated == 1) {
        return video::nxzDecompress(rest, size);
    }
    if (deflated != 0 || rest.size() != size) {
        throw FormatError("map editor session: bad packed bytes");
    }
    return {rest.begin(), rest.end()};
}

// --- Bodies -----------------------------------------------------------------------------------

std::vector<std::uint8_t> encode(const Hello& m)
{
    ByteWriter w;
    w.u32(m.session);
    w.u32(m.version);
    w.u8(static_cast<std::uint8_t>(m.editMode));
    w.u8(m.flags);
    w.u32(m.you);
    str(w, m.map);
    return std::move(w).take();
}
Hello decodeHello(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session Hello");
    Hello m;
    m.session = r.u32();
    m.version = r.u32();
    m.editMode = r.u8() ? EditMode::Approve : EditMode::HostOnly;
    m.flags = r.u8();
    m.you = r.u32();
    m.map = str(r);
    done(r);
    return m;
}

std::vector<std::uint8_t> encode(const std::vector<RosterEntry>& m)
{
    ByteWriter w;
    w.u16(static_cast<std::uint16_t>(m.size()));
    for (const auto& e : m) {
        w.u32(e.peer);
        w.u8(static_cast<std::uint8_t>(e.role));
        w.u8(e.colour);
        w.u8(e.stock ? 1 : 0);
        str(w, e.name);
    }
    return std::move(w).take();
}
std::vector<RosterEntry> decodeRoster(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session Roster");
    std::vector<RosterEntry> out(r.u16());
    for (auto& e : out) {
        e.peer = r.u32();
        e.role = role(r.u8());
        e.colour = r.u8();
        e.stock = r.u8() != 0;
        e.name = str(r);
    }
    done(r);
    return out;
}

std::vector<std::uint8_t> encode(const EditRequest& m)
{
    ByteWriter w;
    w.u32(m.peer);
    str(w, m.name);
    str(w, m.reason);
    w.u8(m.open ? 1 : 0);
    return std::move(w).take();
}
EditRequest decodeEditRequest(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session EditRequest");
    EditRequest m;
    m.peer = r.u32();
    m.name = str(r);
    m.reason = str(r);
    m.open = r.u8() != 0;
    done(r);
    return m;
}

std::vector<std::uint8_t> encode(const Answer& m)
{
    ByteWriter w;
    w.u32(m.peer);
    w.u8(m.decision);
    return std::move(w).take();
}
Answer decodeAnswer(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session Answer");
    Answer m;
    m.peer = r.u32();
    m.decision = r.u8();
    done(r);
    return m;
}

std::vector<std::uint8_t> encode(const RoleMsg& m)
{
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(m.role));
    str(w, m.text);
    return std::move(w).take();
}
RoleMsg decodeRole(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session Role");
    RoleMsg m;
    m.role = role(r.u8());
    m.text = str(r);
    done(r);
    return m;
}

std::vector<std::uint8_t> encode(const OpMsg& m)
{
    ByteWriter w;
    w.u32(m.seq);
    blob(w, pack(m.op));
    w.u32(m.seen);
    return std::move(w).take();
}
OpMsg decodeOp(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session Op");
    OpMsg m;
    m.seq = r.u32();
    m.op = unpack(blob(r));
    m.seen = r.u32();
    done(r);
    return m;
}

std::vector<std::uint8_t> encode(const Committed& m)
{
    ByteWriter w;
    w.u32(m.index);
    w.u32(m.author);
    w.u32(m.seq);
    w.u8(m.catchUp ? 1 : 0);
    blob(w, pack(m.op));
    return std::move(w).take();
}
Committed decodeCommitted(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session Committed");
    Committed m;
    m.index = r.u32();
    m.author = r.u32();
    m.seq = r.u32();
    m.catchUp = r.u8() != 0;
    m.op = unpack(blob(r));
    done(r);
    return m;
}

std::vector<std::uint8_t> encode(const Rejected& m)
{
    ByteWriter w;
    w.u32(m.seq);
    w.u8(static_cast<std::uint8_t>(m.reason));
    str(w, m.text);
    return std::move(w).take();
}
Rejected decodeRejected(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session Rejected");
    Rejected m;
    m.seq = r.u32();
    const std::uint8_t reason = r.u8();
    if (reason < 1 || reason > 4) {
        throw FormatError("map editor session: unknown reject reason");
    }
    m.reason = static_cast<Reject>(reason);
    m.text = str(r);
    done(r);
    return m;
}

std::vector<std::uint8_t> encode(const Base& m)
{
    ByteWriter w;
    w.u32(m.version);
    blob(w, pack(m.document));
    return std::move(w).take();
}
Base decodeBase(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session Base");
    Base m;
    m.version = r.u32();
    m.document = unpack(blob(r));
    done(r);
    return m;
}

std::vector<std::uint8_t> encode(const LockMsg& m)
{
    ByteWriter w;
    w.u32(m.seq);
    keys(w, m.keys);
    return std::move(w).take();
}
LockMsg decodeLock(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session Lock");
    LockMsg m;
    m.seq = r.u32();
    m.keys = keys(r);
    done(r);
    return m;
}

std::vector<std::uint8_t> encode(const Presence& m)
{
    ByteWriter w;
    w.u32(m.peer);
    w.f32(m.x);
    w.f32(m.y);
    w.u8(m.tool);
    w.f32(m.radius);
    w.u8(m.inView ? 1 : 0);
    return std::move(w).take();
}
Presence decodePresence(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session Presence");
    Presence m;
    m.peer = r.u32();
    m.x = r.f32();
    m.y = r.f32();
    m.tool = r.u8();
    m.radius = r.f32();
    m.inView = r.u8() != 0;
    done(r);
    return m;
}

std::vector<std::uint8_t> encode(const Published& m)
{
    ByteWriter w;
    w.u32(m.version);
    w.u32(m.crc);
    str(w, m.name);
    w.u8(m.full ? 1 : 0);
    return std::move(w).take();
}
Published decodePublished(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session Published");
    Published m;
    m.version = r.u32();
    m.crc = r.u32();
    m.name = str(r);
    m.full = r.u8() != 0;
    done(r);
    return m;
}

std::vector<std::uint8_t> encode(const ScriptDiag& m)
{
    ByteWriter w;
    w.u8(m.kind);
    w.u8(m.ok ? 1 : 0);
    w.u16(static_cast<std::uint16_t>(std::min<std::size_t>(m.diagnostics.size(), 0xFFFF)));
    for (std::size_t i = 0; i < m.diagnostics.size() && i < 0xFFFF; ++i) {
        const auto& d = m.diagnostics[i];
        w.u32(d.line);
        w.u32(d.column);
        w.u8(d.error ? 1 : 0);
        str(w, d.text);
    }
    str(w, m.summary);
    return std::move(w).take();
}
ScriptDiag decodeScriptDiag(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session ScriptDiag");
    ScriptDiag m;
    m.kind = r.u8();
    m.ok = r.u8() != 0;
    m.diagnostics.resize(r.u16());
    for (auto& d : m.diagnostics) {
        d.line = r.u32();
        d.column = r.u32();
        d.error = r.u8() != 0;
        d.text = str(r);
    }
    m.summary = str(r);
    done(r);
    return m;
}

std::vector<std::uint8_t> encode(const std::vector<LiveEntry>& m)
{
    ByteWriter w;
    w.u16(static_cast<std::uint16_t>(m.size()));
    for (const auto& e : m) {
        w.u16(e.id);
        w.u8(static_cast<std::uint8_t>((e.player ? 1 : 0) | (e.stock ? 2 : 0) | (e.observer ? 4 : 0)));
        w.u8(e.team);
        w.i16(e.x);
        w.i16(e.y);
        str(w, e.name);
        w.u16(e.type);
        w.u8(e.anim);
        w.u8(e.frame);
        if (e.player) {
            w.u8(e.state);
            w.u32(e.armor);
            w.u32(e.weapon);
        }
    }
    return std::move(w).take();
}
std::vector<LiveEntry> decodeLive(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session Live");
    std::vector<LiveEntry> out(r.u16());
    for (auto& e : out) {
        e.id = r.u16();
        const std::uint8_t f = r.u8();
        e.player = (f & 1) != 0;
        e.stock = (f & 2) != 0;
        e.observer = (f & 4) != 0;
        e.team = r.u8();
        e.x = r.i16();
        e.y = r.i16();
        e.name = str(r);
        e.type = r.u16();
        e.anim = r.u8();
        e.frame = r.u8();
        if (e.player) {
            e.state = r.u8();
            e.armor = r.u32();
            e.weapon = r.u32();
        }
    }
    done(r);
    return out;
}

std::vector<std::uint8_t> encode(const PlayTest& m)
{
    ByteWriter w;
    w.u8(m.start ? 1 : 0);
    w.u32(m.mode);
    w.u8(m.playersCanDie ? 1 : 0);
    return std::move(w).take();
}
PlayTest decodePlayTest(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session PlayTest");
    PlayTest m;
    m.start = r.u8() != 0;
    m.mode = r.u32();
    m.playersCanDie = r.u8() != 0;
    done(r);
    return m;
}

std::vector<std::uint8_t> encode(const Settings& m)
{
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>(m.editMode));
    w.u8(m.flags);
    return std::move(w).take();
}
Settings decodeSettings(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session Settings");
    Settings m;
    m.editMode = r.u8() ? EditMode::Approve : EditMode::HostOnly;
    m.flags = r.u8();
    done(r);
    return m;
}

std::vector<std::uint8_t> encode(const Snapshot& m)
{
    ByteWriter w;
    w.u32(m.version);
    w.u32(m.index);
    w.u32(m.nextObjId);
    w.u32(m.nextPolygonKey);
    w.u32(m.nextGroupKey);
    blob(w, pack(m.op));
    return std::move(w).take();
}
Snapshot decodeSnapshot(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session Snapshot");
    Snapshot m;
    m.version = r.u32();
    m.index = r.u32();
    m.nextObjId = r.u32();
    m.nextPolygonKey = r.u32();
    m.nextGroupKey = r.u32();
    m.op = unpack(blob(r));
    done(r);
    return m;
}

std::vector<std::uint8_t> encode(const Park& m)
{
    ByteWriter w;
    w.u8(static_cast<std::uint8_t>((m.on ? 1 : 0) | (m.spot ? 2 : 0)));
    w.f32(m.x);
    w.f32(m.y);
    str(w, m.text);
    return std::move(w).take();
}
Park decodePark(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session Park");
    Park m;
    const std::uint8_t f = r.u8();
    m.on = (f & 1) != 0;
    m.spot = (f & 2) != 0;
    m.x = r.f32();
    m.y = r.f32();
    m.text = str(r);
    done(r);
    return m;
}

std::vector<std::uint8_t> encode(const Problem& m)
{
    ByteWriter w;
    w.u32(m.key);
    w.u8(m.open ? 1 : 0);
    str(w, m.text);
    return std::move(w).take();
}
Problem decodeProblem(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session Problem");
    Problem m;
    m.key = r.u32();
    m.open = r.u8() != 0;
    m.text = str(r);
    done(r);
    return m;
}

std::vector<std::uint8_t> encode(const std::vector<EditorState>& m)
{
    ByteWriter w;
    const std::size_t n = std::min<std::size_t>(m.size(), 0xFFFF);
    w.u16(static_cast<std::uint16_t>(n));
    for (std::size_t i = 0; i < n; ++i) {
        w.u32(m[i].peer);
        w.u16(m[i].netCode);
        w.u8(static_cast<std::uint8_t>(m[i].activity));
    }
    return std::move(w).take();
}
std::vector<EditorState> decodeEditors(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session Editors");
    std::vector<EditorState> out(r.u16());
    for (auto& e : out) {
        e.peer = r.u32();
        e.netCode = r.u16();
        const std::uint8_t a = r.u8();
        e.activity = a <= 2 ? static_cast<Activity>(a) : Activity::Editing;
    }
    done(r);
    return out;
}

std::vector<std::uint8_t> encodeText(const std::string& text)
{
    ByteWriter w;
    str(w, text);
    return std::move(w).take();
}
std::string decodeText(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session text");
    std::string s = str(r);
    done(r);
    return s;
}
std::vector<std::uint8_t> encodeU32(std::uint32_t v)
{
    ByteWriter w;
    w.u32(v);
    return std::move(w).take();
}
std::uint32_t decodeU32(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session u32");
    const std::uint32_t v = r.u32();
    done(r);
    return v;
}
std::vector<std::uint8_t> encodeKey(const ElementKey& k)
{
    ByteWriter w;
    key(w, k);
    return std::move(w).take();
}
ElementKey decodeKey(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session element");
    ElementKey k = key(r);
    done(r);
    return k;
}

// --- The mod project ----------------------------------------------------------------------------

namespace {

std::string modPath(ByteReader& r)
{
    std::string p = str(r);
    if (!modPathValid(p)) {
        throw FormatError("session mod: a path outside the mod's folder");
    }
    return p;
}

} // namespace

std::vector<std::uint8_t> encode(const ModProject& m)
{
    ByteWriter w;
    str(w, m.id);
    str(w, m.name);
    const std::size_t n = std::min(m.files.size(), kMaxModFiles);
    w.u16(static_cast<std::uint16_t>(n));
    for (std::size_t i = 0; i < n; ++i) {
        const ModFileInfo& f = m.files[i];
        str(w, f.path);
        w.u32(f.size);
        w.u32(f.published);
        w.u32(f.working);
        w.u32(f.holder);
        w.u32(f.author);
    }
    return std::move(w).take();
}
ModProject decodeModProject(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session ModProject");
    ModProject m;
    m.id = str(r);
    m.name = str(r);
    if (m.id.empty() || m.id.size() > 32 || m.name.size() > 64) {
        throw FormatError("session mod: a bad id or name");
    }
    const std::uint16_t n = r.u16();
    if (n > kMaxModFiles) {
        throw FormatError("session mod: too many files");
    }
    std::size_t total = 0;
    for (std::uint16_t i = 0; i < n; ++i) {
        ModFileInfo f;
        f.path = modPath(r);
        f.size = r.u32();
        f.published = r.u32();
        f.working = r.u32();
        f.holder = r.u32();
        f.author = r.u32();
        total += f.size;
        if (f.size > kMaxModFile || total > kMaxModBytes) {
            throw FormatError("session mod: over the caps");
        }
        m.files.push_back(std::move(f));
    }
    done(r);
    return m;
}

std::vector<std::uint8_t> encode(const ModFile& m)
{
    ByteWriter w;
    str(w, m.path);
    w.u32(m.version);
    w.u8(m.deleted ? 1 : 0);
    w.bytes(pack(m.bytes));
    return std::move(w).take();
}
ModFile decodeModFile(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session ModFile");
    ModFile m;
    m.path = modPath(r);
    m.version = r.u32();
    const std::uint8_t deleted = r.u8();
    if (deleted > 1) {
        throw FormatError("session mod: a bad file flag");
    }
    m.deleted = deleted == 1;
    // The packed content's size checked before it's unpacked ({u8 deflated, u32 raw size, bytes}).
    const auto packed = r.bytes(r.remaining());
    if (packed.size() < 5) {
        throw FormatError("session mod: a file cut short");
    }
    const std::uint32_t raw = packed[1] | packed[2] << 8 | packed[3] << 16 | static_cast<std::uint32_t>(packed[4]) << 24;
    if (raw > kMaxModFile || (m.deleted && raw != 0)) {
        throw FormatError("session mod: a file over the caps");
    }
    m.bytes = unpack(packed);
    if (m.bytes.size() != raw) {
        throw FormatError("session mod: a file's size doesn't match");
    }
    return m;
}

std::vector<std::uint8_t> encode(const ModLock& m)
{
    ByteWriter w;
    str(w, m.path);
    w.u8(static_cast<std::uint8_t>((m.on ? 1 : 0) | (m.force ? 2 : 0)));
    return std::move(w).take();
}
ModLock decodeModLock(std::span<const std::uint8_t> b)
{
    ByteReader r(b, "session ModLock");
    ModLock m;
    m.path = modPath(r);
    const std::uint8_t f = r.u8();
    if (f > 3) {
        throw FormatError("session mod: a bad lock flag");
    }
    m.on = (f & 1) != 0;
    m.force = (f & 2) != 0;
    done(r);
    return m;
}

std::vector<std::uint8_t> encodeJoin(std::uint32_t features)
{
    return encodeU32(features);
}
std::uint32_t decodeJoin(std::span<const std::uint8_t> b)
{
    if (b.size() < 4) {
        return 0; // (a client before features: an empty Join)
    }
    ByteReader r(b, "session Join");
    return r.u32(); // (more after it: a later client's, not read)
}

} // namespace nox::mapedit::session
