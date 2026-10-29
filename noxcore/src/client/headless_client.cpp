// HeadlessClient: the protocol side of a 1.2b client without a screen. It joins (lobby, handshake,
// map check or download), feeds every server message to its World, answers what the vanilla client
// answers (IMPORTANT, aliases, transfers), and sends a frame's controls every 33 ms. The caller
// moves the datagrams; see headless_client.hpp.
#include "noxcore/client/headless_client.hpp"
#include "noxcore/server/player_save.hpp"

#include "noxcore/crypto/noxcrypt.hpp"
#include "noxcore/io/byte_reader.hpp"
#include "noxcore/io/file.hpp"
#include "noxcore/map/map_file.hpp"
#include "noxcore/map/objects.hpp"
#include "noxcore/map/sections.hpp"
#include "noxcore/modes/registry.hpp"
#include "noxcore/net/framing.hpp"
#include "noxcore/net/player_input.hpp"
#include "noxcore/server/map_vote.hpp"
#include "noxcore/strings/string_table.hpp"
#include "noxcore/things/object_class.hpp"
#include "noxcore/things/thing_db.hpp"
#include "noxcore/util/bytes.hpp"
#include "noxcore/util/text.hpp"
#include "noxcore/video/nxz.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <exception>
#include <map>

namespace nox::client {

using namespace nox::net;

namespace {

constexpr std::uint32_t kFrameMs = 33;          // the server runs 30 frames a second
constexpr std::uint32_t kFreshInputMs = 8;      // setFreshInput's sends at most this often
constexpr std::uint32_t kKeepAliveMs = 250;     // carry our ack at least this often
constexpr std::uint32_t kLobbyTimeoutMs = 10'000;
constexpr std::uint32_t kMaxDownloadedMap = 64u << 20; // a downloaded map's size at most (finishDownload)

// UTF-8 to UTF-16. Code points past 0xFFFF become '?'; continuation bytes aren't checked.
std::u16string fromUtf8(std::string_view s)
{
    std::u16string out;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        char32_t cp = c;
        std::size_t n = 1;
        if (c >= 0xF0 && i + 3 < s.size()) {
            cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3Fu);
            n = 4;
        } else if (c >= 0xE0 && i + 2 < s.size()) {
            cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu);
            n = 3;
        } else if (c >= 0xC0 && i + 1 < s.size()) {
            cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu);
            n = 2;
        }
        out.push_back(cp > 0xFFFF ? u'?' : static_cast<char16_t>(cp));
        i += n;
    }
    return out;
}

// Fills a fixed UTF-16LE field, zero-padded and always keeping a terminator.
template <std::size_t N>
void putUtf16(std::array<std::uint8_t, N>& out, std::u16string_view s)
{
    out.fill(0);
    const std::size_t chars = std::min(s.size(), N / 2 - 1); // keep a terminator
    for (std::size_t i = 0; i < chars; ++i) {
        out[2 * i] = static_cast<std::uint8_t>(s[i]);
        out[2 * i + 1] = static_cast<std::uint8_t>(s[i] >> 8);
    }
}

// Fills a fixed 8-bit field, zero-padded and always keeping a terminator.
template <std::size_t N>
void putAscii(std::array<std::uint8_t, N>& out, std::string_view s)
{
    out.fill(0);
    std::copy_n(s.begin(), std::min(s.size(), N - 1), out.begin());
}

// A zero-terminated UTF-16LE string from a fixed field.
using util::utf16Until;

// A zero-terminated 8-bit string from a fixed field.
std::string readAscii(std::span<const std::uint8_t> b)
{
    std::string s;
    for (auto c : b) {
        if (c == 0) {
            break;
        }
        s.push_back(static_cast<char>(c));
    }
    return s;
}

using util::u16At;
using util::u32At;

using util::lowered;

// The direction (dx, dy) in Nox facing units: 256 per turn, 0 east, 64 south (y grows downward).
std::uint8_t facingToward(double dx, double dy)
{
    const double turns = std::atan2(dy, dx) / (2.0 * 3.14159265358979323846);
    return static_cast<std::uint8_t>(static_cast<int>(std::lround(turns * 256.0)) & 0xFF);
}

} // namespace

HeadlessClient::HeadlessClient(PlayerProfile profile, const things::ThingDb* things, std::filesystem::path mapsDir,
                               std::filesystem::path mapCache)
    : profile_(std::move(profile))
    , things_(things)
    , mapsDir_(std::move(mapsDir))
    , mapCache_(std::move(mapCache))
    , world_(things)
{
    // Each registered mode's client side (its opcodes' handlers set as each is made; idle unless
    // the server plays its mode). Those that act for the player only where asked (addModeSide).
    for (const modes::Mode* mode : modes::registry().all()) {
        if (mode->makeClient && mode->everyClient) {
            modeSides_.emplace_back(mode->info.id, mode->makeClient(*this));
        }
    }
}

HeadlessClient::~HeadlessClient() = default;

modes::ClientMode* HeadlessClient::modeSide(std::string_view id) const
{
    for (const auto& [modeId, side] : modeSides_) {
        if (modeId == id) {
            return side.get();
        }
    }
    return nullptr;
}

modes::ClientMode* HeadlessClient::addModeSide(std::string_view id)
{
    if (modes::ClientMode* had = modeSide(id)) {
        return had;
    }
    const modes::Mode* mode = modes::registry().find(id);
    if (!mode || !mode->makeClient) {
        return nullptr;
    }
    modeSides_.emplace_back(mode->info.id, mode->makeClient(*this));
    return modeSides_.back().second.get();
}

modes::ClientMode* HeadlessClient::adoptModeSide(std::string id, std::unique_ptr<modes::ClientMode> side)
{
    if (modes::ClientMode* had = modeSide(id)) {
        return had;
    }
    if (!side) {
        return nullptr;
    }
    modeSides_.emplace_back(std::move(id), std::move(side));
    return modeSides_.back().second.get();
}

void HeadlessClient::event(ClientEvent::Kind kind, std::string text, std::uint16_t netCode, std::string name)
{
    pushEvent({kind, std::move(text), netCode, std::move(name)});
}

void HeadlessClient::pushEvent(ClientEvent e)
{
    // A client that never takes them keeps the newest (World's effects and sounds likewise).
    if (events_.size() >= kMaxEvents) {
        events_.erase(events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(kMaxEvents / 2));
    }
    events_.push_back(std::move(e));
}

// ------------------------------------------------------------------------------------ I/O

// The lobby join: TRY_JOIN with our name, class, serial and account; tick() takes it from there.
void HeadlessClient::start(std::uint32_t nowMs)
{
    msg::ServerTryJoin join;
    putUtf16(join.playerName, fromUtf8(profile_.name));
    join.playerClass = profile_.playerClass;
    putAscii(join.serial, profile_.serial);
    putAscii(join.account, profile_.account);
    conn_.tryJoin(join);
    stage_ = Stage::Joining;
    nowMs_ = nowMs;
    nextFrameAt_ = nowMs + kLobbyTimeoutMs; // reused as the lobby deadline until connected
    event(ClientEvent::Kind::Log, "sent TRY_JOIN");
}

// A lobby reply. A password request is answered with the profile's password, or fails without one.
void HeadlessClient::receiveLobby(std::span<const std::uint8_t> datagram)
{
    conn_.receiveLobby(datagram);
    if (conn_.state() == ClientConnection::State::PasswordRequired) {
        if (profile_.password.empty()) {
            stage_ = Stage::Failed;
            event(ClientEvent::Kind::Disconnected, "the server wants a password");
            return;
        }
        msg::ServerPassword pw;
        putUtf16(pw.password, fromUtf8(profile_.password));
        conn_.sendPassword(pw);
        event(ClientEvent::Kind::Log, "sent password");
    }
}

void HeadlessClient::receive(std::span<const std::uint8_t> datagram, std::uint32_t nowMs)
{
    nowMs_ = nowMs;
    ++datagramsReceived_;
    silence_.heard(nowMs); // (the client's packet handler: the time noted, the icon and lag window gone)
    conn_.receive(datagram, nowMs);
    for (const auto& payload : conn_.takeReceived()) {
        handlePayload(payload);
    }
}

void HeadlessClient::setWatchSilence(bool on)
{
    watchSilence_ = on;
    if (!on) {
        silence_.reset();
    }
}

std::uint32_t HeadlessClient::quietFor() const noexcept
{
    return stage_ == Stage::InGame ? silence_.quietFor(nowMs_) : 0;
}

void HeadlessClient::lagWait()
{
    silence_.wait(nowMs_);
}

void HeadlessClient::dropped(EndReason why, const char* text)
{
    stage_ = Stage::Failed;
    endReason_ = why;
    silence_.reset();
    event(ClientEvent::Kind::Disconnected, text);
}

void HeadlessClient::lagExit()
{
    // The lag window's Exit: Game_FinishQuit (the game left), then the Timeout notice.
    endReason_ = EndReason::LostConnection;
    quit();
}

// Drives the join's stages and the quit, and runs a game frame every kFrameMs once connected.
void HeadlessClient::tick(std::uint32_t nowMs)
{
    nowMs_ = nowMs;
    if (!seqPending_.empty()) {
        std::uint32_t frame = 0;
        drainSeqImportant(frame); // (one waited on too long goes now)
    }
    using CS = ClientConnection::State;
    // Joining: move on as the connection's lobby and handshake state does.
    if (stage_ == Stage::Joining) {
        switch (conn_.state()) {
        case CS::JoinOk: {
            msg::ClientAccept accept;
            putUtf16(accept.playerName, fromUtf8(profile_.name));
            accept.classId = profile_.playerClass;
            accept.isFemale = profile_.female ? 1 : 0;
            accept.colors = profile_.colors;
            accept.clothing = profile_.clothing;
            accept.screenWidth = profile_.screenWidth;
            accept.screenHeight = profile_.screenHeight;
            putAscii(accept.serial, profile_.serial);
            putAscii(accept.account, profile_.account);
            accept.flags = profile_.quest ? 0x80 : 0;  // bit 7: joining a quest game
            conn_.connect(accept, nowMs);
            event(ClientEvent::Kind::Log, "lobby accepted us; connecting");
            break;
        }
        case CS::Connected:
            queueReliable({static_cast<std::uint8_t>(Op::IncomingClient)});
            if (profile_.quest) {
                // A network quest client (not the host): our character's server
                // sections to the host (CliNet_SendSaveServerXfer: "SAVE_SERVER", action 3).
                sendCharacter();
            }
            stage_ = Stage::WaitingMap;
            nextFrameAt_ = nowMs;
            event(ClientEvent::Kind::Log, "connected as player " + std::to_string(conn_.playerId()) + "; waiting for the map");
            break;
        case CS::JoinRefused:
            stage_ = Stage::Failed;
            event(ClientEvent::Kind::Disconnected, "join refused (error " + std::to_string(conn_.joinError()) + ")");
            break;
        case CS::ConnectRefused:
            stage_ = Stage::Failed;
            event(ClientEvent::Kind::Disconnected, "connect refused or timed out");
            break;
        case CS::Joining:
        case CS::PasswordRequired:
            if (static_cast<std::int32_t>(nowMs - nextFrameAt_) >= 0) {
                stage_ = Stage::Failed;
                event(ClientEvent::Kind::Disconnected, "no answer from the server's lobby");
            }
            break;
        default:
            break;
        }
    }
    if (stage_ == Stage::Quitting) {
        // Done once the server has our OUTGOING_CLIENT (its ack moved past it) or closed us.
        // Sequence numbers wrap at 256: delivered once the ack is 1-127 past our OUTGOING_CLIENT's
        // (lastAck 0xFF: nothing acked yet).
        const auto acked = static_cast<std::uint8_t>(conn_.stream().lastAck() - quitSeq_);
        const bool delivered = conn_.stream().lastAck() != 0xFF && acked >= 1 && acked < 0x80;
        const bool closed = conn_.state() == CS::Closed;
        if (delivered || closed || static_cast<std::int32_t>(nowMs - quitDeadline_) >= 0) {
            conn_.disconnect(); // CLIENT_CLOSE, unreliable (no-op if the server already closed us)
            stage_ = Stage::Closed;
            event(ClientEvent::Kind::Disconnected, delivered || closed ? "left the game" : "left (no answer)");
        }
        conn_.poll(nowMs);
        return;
    }
    const bool connected = stage_ == Stage::WaitingMap || stage_ == Stage::Downloading || stage_ == Stage::InGame;
    if (connected && conn_.state() == CS::Closed) {
        // SERVER_CLOSE (the transport's 0x21 to the game: Game_FinishQuit, back to the menus).
        closingForSave_ = false;
        dropped(EndReason::ServerClosed, "the server closed the connection");
    }
    // The server gone quiet: the icon, then the lag window, which asks the server for a timestamp
    // (CliNet_ShowLagWindowAndRequestTimestamp: MSG_NEED_TIMESTAMP, reliably).
    if (stage_ == Stage::InGame && watchSilence_ && silence_.tick(nowMs)) {
        queueReliable({static_cast<std::uint8_t>(Op::NeedTimestamp)});
        event(ClientEvent::Kind::Log, "the server has stopped answering (" + std::to_string(silence_.quietFor(nowMs) / 1000) + " s)");
    }
    // A game frame every kFrameMs; between frames, noxcpp's fresh input (setFreshInput).
    if (connected && stage_ != Stage::Failed && static_cast<std::int32_t>(nowMs - nextFrameAt_) >= 0) {
        gameFrame(nowMs);
        nextFrameAt_ = nowMs + kFrameMs;
        if (sends_) {
            sends_->frame(frame_); // (our sends' chunks, and their retries)
        }
    } else if (freshInput_ && stage_ == Stage::InGame && nowMs - freshAt_ >= kFreshInputMs) {
        sendFreshInput();
    }
    conn_.poll(nowMs);
}

void HeadlessClient::sendFreshInput()
{
    // noxcpp's (setFreshInput): what changed of the continuous controls since they were last
    // sent, as gameFrame would send them (a move faces its way). Walking to a point and hunting
    // steer on the input frames.
    if (walkTarget_ || huntTarget_) {
        return;
    }
    const std::uint8_t facing = moveDirection_ ? *moveDirection_ : facing_;
    const bool moving = moveDirection_.has_value();
    const auto cursor = cursorToSend();
    const bool cursorMoved = cursor && cursor != sentCursor_;
    if (facing == sentFacing_ && moving == sentMoving_ && !cursorMoved) {
        return;
    }
    if (facing != sentFacing_ || moving != sentMoving_) {
        std::vector<ControlEntry> controls;
        if (moving) {
            controls.push_back({ControlCode::MoveForward, 1});
        }
        controls.push_back({ControlCode::Orientation, facing});
        queueControls(controls);
        sentFacing_ = facing;
        sentMoving_ = moving;
    }
    if (cursorMoved) {
        queueMouse(cursor->first, cursor->second);
        sentCursor_ = cursor;
    }
    freshAt_ = nowMs_;
}

void HeadlessClient::queueControls(std::span<const ControlEntry> controls)
{
    const auto entries = encodeControls(controls);
    std::vector<std::uint8_t> m = {static_cast<std::uint8_t>(Op::PlayerInput), static_cast<std::uint8_t>(entries.size())};
    m.insert(m.end(), entries.begin(), entries.end());
    queueUnreliable(std::move(m));
}

void HeadlessClient::queueMouse(int x, int y)
{
    queueUnreliable({static_cast<std::uint8_t>(Op::Mouse), static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(x >> 8),
                     static_cast<std::uint8_t>(y), static_cast<std::uint8_t>(y >> 8)});
}

std::vector<ClientConnection::Datagram> HeadlessClient::takeOutgoing()
{
    return conn_.takeOutgoing();
}

std::vector<ClientEvent> HeadlessClient::takeEvents()
{
    return std::exchange(events_, {});
}

// -------------------------------------------------------------------------------- actions

void HeadlessClient::say(std::string_view utf8, bool teamOnly)
{
    std::u16string text = fromUtf8(utf8);
    if (text.size() > 200) {
        text.resize(200);
    }
    const bool narrow = std::all_of(text.begin(), text.end(), [](char16_t c) { return c < 0x80; });
    // CliNet_SendChatText: 2 = 8-bit text, 4 = UTF-16, | 1 = team only.
    std::vector<std::uint8_t> m = {static_cast<std::uint8_t>(Op::TextMessage), static_cast<std::uint8_t>(netCode_),
                                   static_cast<std::uint8_t>(netCode_ >> 8),
                                   static_cast<std::uint8_t>((narrow ? 2 : 4) | (teamOnly ? 1 : 0))};
    // After the flags: x, y (0xFFFF when unknown), the length in characters with the terminator,
    // two zero bytes (the duration field), then the text, 8-bit or UTF-16.
    const int x = position_ ? position_->first : 0xFFFF;
    const int y = position_ ? position_->second : 0xFFFF;
    m.insert(m.end(), {static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(x >> 8), static_cast<std::uint8_t>(y),
                       static_cast<std::uint8_t>(y >> 8), static_cast<std::uint8_t>(text.size() + 1), 0, 0});
    util::appendText(m, text, narrow);
    queueUnreliable(std::move(m));
}

void HeadlessClient::move(std::optional<std::uint8_t> direction)
{
    moveDirection_ = direction;
    walkTarget_.reset();
}

void HeadlessClient::spawn()
{
    spawnPending_ = true;
}

void HeadlessClient::walkTo(int x, int y)
{
    walkTarget_ = std::pair{x, y};
    path_.clear();
}

// Walks toward walkTarget_: plans a path, re-plans after 1.5 s without 8 units of progress,
// drops waypoints within 12 units, and turns moveDirection_ toward the next one.
void HeadlessClient::steer(std::uint32_t nowMs)
{
    const auto at = world_.camera();
    if (!at) {
        return;
    }
    const auto distance = [](std::pair<int, int> a, std::pair<int, int> b) {
        return std::hypot(double(a.first - b.first), double(a.second - b.second));
    };
    // Plan, and re-plan when we stop making progress (an object in the way, a door).
    const bool stuck = !path_.empty() && nowMs - progressAt_ > 1500;
    if (stuck && distance(*at, *walkTarget_) <= 40.0) {
        // Objects are solid (a flag, a chest): close enough is there.
        path_.clear();
        walkTarget_.reset();
        moveDirection_.reset();
        event(ClientEvent::Kind::Log, "arrived (as close as it gets)");
        return;
    }
    if (path_.empty() || stuck) {
        path_ = nav_.empty() ? std::vector{*walkTarget_} : nav_.findPath(*at, *walkTarget_);
        if (stuck) {
            event(ClientEvent::Kind::Log, "stuck; re-planned");
        }
        if (path_.empty()) {
            walkTarget_.reset();
            moveDirection_.reset();
            event(ClientEvent::Kind::Log, "no path there");
            return;
        }
        progressAt_ = nowMs;
        progressFrom_ = *at;
    }
    if (distance(*at, progressFrom_) > 8.0) {
        progressAt_ = nowMs;
        progressFrom_ = *at;
    }
    while (!path_.empty() && distance(*at, path_.front()) <= 12.0) {
        path_.erase(path_.begin());
    }
    if (path_.empty()) {
        walkTarget_.reset();
        moveDirection_.reset();
        event(ClientEvent::Kind::Log, "arrived");
        return;
    }
    moveDirection_ = facingToward(double(path_.front().first - at->first), double(path_.front().second - at->second));
}

namespace {

// An opcode followed by a u16 net code.
std::vector<std::uint8_t> withNetCode(Op op, std::uint16_t netCode)
{
    return {static_cast<std::uint8_t>(op), static_cast<std::uint8_t>(netCode), static_cast<std::uint8_t>(netCode >> 8)};
}

} // namespace

void HeadlessClient::pickup(std::uint16_t netCode)
{
    queueUnreliable(withNetCode(Op::TryGet, netCode)); // CliNet_SendPickup
}

void HeadlessClient::drop(std::uint16_t netCode)
{
    const auto at = world_.camera().value_or(std::pair{0, 0});
    dropAt(netCode, at.first, at.second);
}

void HeadlessClient::dropAt(std::uint16_t netCode, int x, int y)
{
    auto m = withNetCode(Op::TryDrop, netCode);
    m.insert(m.end(), {static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(x >> 8), static_cast<std::uint8_t>(y),
                       static_cast<std::uint8_t>(y >> 8)});
    queueUnreliable(std::move(m));
}

void HeadlessClient::use(std::uint16_t netCode)
{
    queueUnreliable(withNetCode(Op::TryUse, netCode));
}

void HeadlessClient::equip(std::uint16_t netCode)
{
    queueUnreliable(withNetCode(Op::TryEquip, netCode));
}

void HeadlessClient::unequip(std::uint16_t netCode)
{
    queueUnreliable(withNetCode(Op::TryDequip, netCode));
}

void HeadlessClient::collide(std::uint16_t netCode)
{
    queueUnreliable(withNetCode(Op::TryCollide, netCode)); // CliNet_SendTryCollide
}

void HeadlessClient::commandCreature(std::uint16_t netCode, std::uint8_t command)
{
    auto m = withNetCode(Op::TryCreatureCommand, netCode);
    m.push_back(command);
    queueUnreliable(std::move(m)); // (Net_QueueMsg)
}

namespace {

// MSG_TRADE with its sub-op, optionally followed by a u16 (a net code or a type id).
std::vector<std::uint8_t> tradeOp(std::uint8_t sub) { return {static_cast<std::uint8_t>(Op::Trade), sub}; }

std::vector<std::uint8_t> tradeOp(std::uint8_t sub, std::uint16_t value)
{
    return {static_cast<std::uint8_t>(Op::Trade), sub, static_cast<std::uint8_t>(value),
            static_cast<std::uint8_t>(value >> 8)};
}

} // namespace

void HeadlessClient::openShop(std::uint16_t netCode) { queueUnreliable(tradeOp(0x15, netCode)); }
void HeadlessClient::shopExit() { queueUnreliable(tradeOp(0x12)); }
void HeadlessClient::shopBuy(std::uint16_t netCode) { queueUnreliable(tradeOp(0x16, netCode)); }
void HeadlessClient::shopBuyStack(std::uint16_t typeId, std::uint8_t count)
{
    auto m = tradeOp(0x17, typeId);
    m.push_back(count);
    queueUnreliable(std::move(m));
}
void HeadlessClient::shopSell(std::uint16_t netCode) { queueUnreliable(tradeOp(0x18, netCode)); }
void HeadlessClient::shopSellStack(std::uint16_t typeId, std::uint8_t count)
{
    auto m = tradeOp(0x19, typeId);
    m.push_back(count);
    queueUnreliable(std::move(m));
}
void HeadlessClient::shopRepair(std::uint16_t netCode) { queueUnreliable(tradeOp(0x1A, netCode)); }
void HeadlessClient::shopAskSellPrice(std::uint16_t netCode) { queueUnreliable(tradeOp(0x1C, netCode)); }
void HeadlessClient::shopAskRepairCost(std::uint16_t netCode) { queueUnreliable(tradeOp(0x1E, netCode)); }
void HeadlessClient::tradeAccept() { queueUnreliable(tradeOp(0x11)); }
void HeadlessClient::tradeCancel() { queueUnreliable(tradeOp(0x0E)); }
void HeadlessClient::tradeWithdraw(std::uint16_t netCode) { queueUnreliable(tradeOp(0x10, netCode)); }

void HeadlessClient::dialogStart(std::uint16_t netCode)
{
    queueUnreliable({static_cast<std::uint8_t>(Op::Dialog), 0x01, static_cast<std::uint8_t>(netCode),
                     static_cast<std::uint8_t>(netCode >> 8)});
}

void HeadlessClient::dialogAnswer(std::uint8_t answer)
{
    queueUnreliable({static_cast<std::uint8_t>(Op::Dialog), 0x02, answer});
}

void HeadlessClient::askBook(std::uint16_t netCode, std::uint8_t kind)
{
    auto m = withNetCode(Op::InfoBookData, netCode);
    m.push_back(kind);
    queueReliable(std::move(m)); // Net_QueueImportantToServer in Gui_Tooltip_GetDrawableText
}

void HeadlessClient::hunt(std::optional<std::uint16_t> netCode)
{
    huntTarget_ = netCode;
    huntReplanAt_ = 0;
    if (!netCode) {
        walkTarget_.reset();
        moveDirection_.reset();
    }
}

void HeadlessClient::face(std::uint8_t direction)
{
    facing_ = direction;
}

void HeadlessClient::setAttack(bool held)
{
    attackHeld_ = held;
}

void HeadlessClient::jump()
{
    jumpPending_ = true;
}

void HeadlessClient::voteMap(int choice)
{
    if (mapVote_.open && choice >= 0 && static_cast<std::size_t>(choice) < mapVote_.candidates.size()) {
        queueReliable(server::mapvote::voteMessage(choice));
    }
}

void HeadlessClient::callMapVote()
{
    queueReliable(server::mapvote::callMessage()); // (noxcpp's MSG_NOXCPP_MAPVOTE kCall; a stock client says "?MapVote")
}

void HeadlessClient::sendControl(ControlCode code)
{
    if (controlPayloadSize(code) == 0) {
        pendingControls_.push_back(code);
    }
}

void HeadlessClient::setCursor(int x, int y)
{
    cursor_ = std::pair{std::clamp(x, 0, 0xFFFF), std::clamp(y, 0, 0xFFFF)};
}

std::optional<std::pair<int, int>> HeadlessClient::cursorToSend() const
{
    if (!cursor_) {
        return std::nullopt;
    }
    auto c = *cursor_;
    if (cursorSprite_) {
        if (const Drawable* d = world_.drawable(*cursorSprite_)) {
            c.second = std::clamp(static_cast<int>(d->y), 0, 0xFFFF); // (the cursor sprite's y)
        }
    }
    return c;
}

void HeadlessClient::castSpells(std::span<const std::uint32_t> spells, bool targetFlag)
{
    // Nothing if the first spell is empty.
    if (spells.empty() || spells[0] == 0) {
        return;
    }
    // Layout: the opcode, five u32 spell ids (0 for none), the target flag byte.
    std::vector<std::uint8_t> m{static_cast<std::uint8_t>(Op::TrySpell)};
    for (std::size_t i = 0; i < 5; ++i) {
        const std::uint32_t id = i < spells.size() ? spells[i] : 0;
        for (int b = 0; b < 4; ++b) {
            m.push_back(static_cast<std::uint8_t>(id >> (8 * b)));
        }
    }
    m.push_back(targetFlag ? 1 : 0);
    queueUnreliable(std::move(m));
}

void HeadlessClient::useAbility(std::uint8_t ability)
{
    if (ability != 0) {
        queueUnreliable({static_cast<std::uint8_t>(Op::TryAbility), ability});
    }
}

void HeadlessClient::joinTeam(std::uint32_t teamId)
{
    // CliNet_SendTeamJoinRequest only asks when we aren't on that team already: the 1.2b server
    // links the member again without checking, which can leave a stale link and crash it later.
    const auto team = world_.teams().find(teamId);
    if (team != world_.teams().end() && team->second.members.count(netCode_)) {
        event(ClientEvent::Kind::Log, "already on team " + std::to_string(teamId));
        return;
    }
    // The vanilla client sends it through its "important" queue (resent until acked); our
    // reliable transport does the same job.
    std::vector<std::uint8_t> m = {static_cast<std::uint8_t>(Op::TeamMsg), 0x0A};
    for (int i = 0; i < 4; ++i) {
        m.push_back(static_cast<std::uint8_t>(teamId >> (8 * i)));
    }
    m.insert(m.end(), {static_cast<std::uint8_t>(netCode_), static_cast<std::uint8_t>(netCode_ >> 8), 0, 0});
    queueReliable(std::move(m));
}

void HeadlessClient::observe()
{
    // CliNet_SendServerCmd(0, NULL) CliNet_SendServerCmd, as the quit menu's Observer Mode sends it: no
    // text, and the two extra bytes its length counts.
    queueReliable({static_cast<std::uint8_t>(Op::ServerCmd), 0, static_cast<std::uint8_t>(netCode_),
                   static_cast<std::uint8_t>(netCode_ >> 8), 0, 0, 0});
}

void HeadlessClient::quit()
{
    // Like the vanilla client: OUTGOING_CLIENT, reliable, and wait for it to be acked (up to
    // 20 s, cliSendOutgoingClient); the server runs the first stage of removal
    // (Plr_OnDisconnect). Only then CLIENT_CLOSE (session teardown), which makes the
    // server finish (Plr_RemoveFromGame). Sending both at once lets the second stage run first,
    // and the server later crashed on a stale team member; sending no CLIENT_CLOSE leaves the
    // player in the game until the network timeout.
    if (endReason_ == EndReason::None) {
        endReason_ = EndReason::Left;
    }
    silence_.reset(); // (Game_FinishQuit: the icon and lag window gone)
    if (stage_ != Stage::WaitingMap && stage_ != Stage::InGame) {
        stage_ = Stage::Closed;
        return;
    }
    queueReliable({static_cast<std::uint8_t>(Op::OutgoingClient)});
    quitSeq_ = static_cast<std::uint8_t>(conn_.stream().nextSeq() - 1);
    quitDeadline_ = nowMs_ + kQuitAckWaitMs;
    stage_ = Stage::Quitting;
    event(ClientEvent::Kind::Log, "leaving");
}

std::optional<std::uint32_t> HeadlessClient::quitWaitedMs() const noexcept
{
    if (stage_ != Stage::Quitting) {
        return std::nullopt;
    }
    return nowMs_ - (quitDeadline_ - kQuitAckWaitMs);
}

void HeadlessClient::questQuit()
{
    // client_quit in a quest, not the host: closing already, it's cleared and we go
    // (the quit itself); else our save asked for and closing set.
    if (closingForSave_) {
        closingForSave_ = false;
        quit();
        return;
    }
    queueReliable({static_cast<std::uint8_t>(Op::RequestSavePlayer), static_cast<std::uint8_t>(netCode_),
                   static_cast<std::uint8_t>(netCode_ >> 8)});
    closingForSave_ = true;
}

bool HeadlessClient::sendCharacter()
{
    if (saveServerFile_.empty()) {
        return false;
    }
    if (!sends_) {
        sends_ = std::make_unique<server::Xfers>([this](std::uint8_t, std::vector<std::uint8_t> bytes, bool reliable) {
            if (reliable) {
                queueReliable(std::move(bytes));
            } else {
                queueUnreliable(std::move(bytes));
            }
        });
    }
    const auto sent = server::saveServerSent(saveServerFile_);
    sends_->send(0, server::Xfers::SaveServer, "SAVE_SERVER", sent);
    return true;
}

void HeadlessClient::questContinue()
{
    queueReliable({static_cast<std::uint8_t>(Op::Gauntlet), 0x03}); // (Net_QueueImportantToServer)
}

// ------------------------------------------------------------------------------ incoming

std::optional<std::uint32_t> HeadlessClient::classOf(std::uint16_t typeId) const
{
    // Network type ids are thing.bin's THNG entries counted from 1 (loader).
    if (!things_ || typeId == 0 || typeId > things_->things.size()) {
        return std::nullopt;
    }
    return things::thingClasses(things_->things[typeId - 1u]);
}

std::uint32_t HeadlessClient::weaponBit(std::uint16_t typeId) const
{
    // Weapons and wands (staffs) alike, as the inventory's 0x1001000 tests.
    const auto cls = classOf(typeId);
    if (!cls || !(*cls & (things::ObjectClass::Weapon | things::ObjectClass::Wand))) {
        return 0;
    }
    const std::uint32_t bits = things::thingSubclasses(things_->things[typeId - 1u]);
    return bits != 0 ? bits : 0x80000000u; // (a weapon with no subclass still counts)
}

bool HeadlessClient::classCanUse(std::uint16_t typeId) const
{
    // modifier.bin's CLASSUSE (getProjectileClassById's class byte).
    if (!modifiers_ || !things_ || typeId == 0 || typeId > things_->things.size() || multiclass_) {
        return true; // (noxcpp's multiclass, a mode's: any class's weapon)
    }
    const auto* def = modifiers_->findWeapon(things_->things[typeId - 1u].name);
    if (!def || !def->classUse) {
        return true;
    }
    const auto* me = world_.player(netCode_);
    return !me || ((*def->classUse >> me->playerClass) & 1);
}

std::optional<std::uint16_t> HeadlessClient::equippedWeapon(bool quiver) const
{
    for (const auto& [netCode, item] : world_.self().inventory) {
        const std::uint32_t bit = item.equipped ? weaponBit(item.typeId) : 0;
        if (bit != 0 && (bit == 2) == quiver) {  // weapon bit 2: a quiver
            return netCode;
        }
    }
    return std::nullopt;
}

void HeadlessClient::social(Social kind)
{
    // MSG_SOCIAL and the emote, as the important message queues.
    queueReliable({static_cast<std::uint8_t>(Op::Social), static_cast<std::uint8_t>(kind)});
}

void HeadlessClient::voteKick(std::u16string_view name, bool withdraw)
{
    std::vector<std::uint8_t> m = {static_cast<std::uint8_t>(Op::Vote), static_cast<std::uint8_t>(withdraw ? 2 : 0)};
    // The name field: 25 UTF-16 characters, at most 24 of them text (a terminator kept).
    for (std::size_t i = 0; i < 25; ++i) {
        const char16_t c = i < name.size() && i < 24 ? name[i] : u'\0';
        m.push_back(static_cast<std::uint8_t>(c));
        m.push_back(static_cast<std::uint8_t>(c >> 8));
    }
    queueReliable(std::move(m));
}

void HeadlessClient::setSecondaryWeapon(std::uint16_t netCode)
{
    previousSecondary_ = world_.self().secondaryWeapon;
    world_.setSecondaryWeapon(netCode);
    queueReliable({static_cast<std::uint8_t>(Op::ReportSecondaryWeapon), static_cast<std::uint8_t>(netCode),
                   static_cast<std::uint8_t>(netCode >> 8)});
}

bool HeadlessClient::swapWeapons()
{
    const auto& inv = world_.self().inventory;
    const std::uint16_t secondary = world_.self().secondaryWeapon;
    const auto alt = secondary ? inv.find(secondary) : inv.end();
    // A quiver as the secondary swaps quivers; anything else, the weapon in hand.
    const bool quiver = alt != inv.end() && weaponBit(alt->second.typeId) == 2;
    if (const auto inHand = equippedWeapon(quiver)) {
        swappingOut_ = *inHand;
        unequip(*inHand);
        return true;
    }
    if (!quiver && alt != inv.end()) {
        equip(secondary);
        return true;
    }
    return false;
}

void HeadlessClient::putAwayWeapon(std::uint16_t netCode)
{
    const std::uint16_t secondary = world_.self().secondaryWeapon;
    if (secondary == 0 || !world_.self().inventory.contains(secondary)) {
        swappingOut_ = netCode; // it becomes the secondary once it's off
        unequip(netCode);
    } else {
        swapWeapons();
    }
}

void HeadlessClient::onEquipReport(std::span<const std::uint8_t> m)
{
    const auto& inv = world_.self().inventory;
    const std::uint16_t netCode = u16At(m, 1);
    const std::uint16_t secondary = world_.self().secondaryWeapon;
    switch (static_cast<Op>(m[0])) {
    case Op::ReportPickup:
    case Op::ReportModifiablePickup: { // filing the item
        // Picked up while a weapon is in hand and there's no secondary: a weapon (not a quiver)
        // or wand our class can use becomes the secondary.
        const auto item = inv.find(netCode);
        if (item == inv.end() || (secondary != 0 && inv.contains(secondary))) {
            break;
        }
        const std::uint32_t bit = weaponBit(item->second.typeId);
        const bool inHand = std::ranges::any_of(inv, [&](const auto& e) {
            return e.second.equipped && e.first != netCode && weaponBit(e.second.typeId) != 0;
        });
        if (bit != 0 && bit != 2 && inHand && classCanUse(item->second.typeId)) {  // 2: a quiver
            setSecondaryWeapon(netCode);
        }
        break;
    }
    case Op::ReportDequip: { // Gui_Inv_OnItemDequipped
        const auto item = inv.find(netCode);
        if (item == inv.end()) {
            break;
        }
        if (netCode == swappingOut_) {
            swappingOut_ = 0;
            if (secondary != 0 && inv.contains(secondary)) {
                pendingSecondary_ = netCode;
                equip(secondary);
            } else {
                setSecondaryWeapon(netCode);
            }
        } else if ((weaponBit(item->second.typeId) & 0xC) && secondary != 0 && inv.contains(secondary) &&  // 0xC: a bow (4) or crossbow (8)
                   weaponBit(inv.at(secondary).typeId) == 2) {
            setSecondaryWeapon(0); // the bow's gone: its quiver isn't a secondary
        }
        break;
    }
    case Op::ReportEquip: { // Gui_Inv_OnItemEquipped
        if (netCode == secondary) {
            setSecondaryWeapon(0); // the secondary is in hand now
        }
        const auto item = inv.find(netCode);
        const std::uint32_t bit = item != inv.end() ? weaponBit(item->second.typeId) : 0;
        if (bit == 2) {
            lastQuiver_ = netCode; // (g_guiInvLastQuiverNetCode)
        } else if (bit & 0xC) {
            // A bow or crossbow on: the last quiver worn, else the first carried, goes on with it
            // (the server took it off with the bow).
            std::optional<std::uint16_t> quiver;
            if (lastQuiver_ != 0 && inv.contains(lastQuiver_)) {
                quiver = lastQuiver_;
            } else {
                for (const auto& [nc, it] : inv) {
                    if (weaponBit(it.typeId) == 2) {
                        quiver = nc;
                        break;
                    }
                }
            }
            if (quiver) {
                equip(*quiver);
            }
        }
        if (pendingSecondary_ != 0) {
            if (inv.contains(pendingSecondary_)) {
                setSecondaryWeapon(pendingSecondary_);
            }
            pendingSecondary_ = 0;
        }
        break;
    }
    case Op::ReportSecondaryWeapon: { // Gui_Inv_SetSecondaryWeapon
        const bool refused = m.size() > 3 && m[3] == 1;
        if (netCode != 0 && inv.contains(netCode)) {
            break; // World took it
        }
        world_.setSecondaryWeapon(0);
        if (refused) {
            // guiinv.c prints GuiInv.c:Weapon2CantUse in the middle of the screen.
            event(ClientEvent::Kind::Notice, "can't use that as a secondary weapon");
            if (previousSecondary_ != 0 && inv.contains(previousSecondary_)) {
                setSecondaryWeapon(previousSecondary_);
                break;
            }
        }
        previousSecondary_ = 0;
        break;
    }
    default:
        break;
    }
}

// Splits one datagram's payload into its messages and handles them in order; the update
// stream (the one message of variable size) is parsed here, the rest go to handleMessage.
void HeadlessClient::handlePayload(std::span<const std::uint8_t> payload)
{
    // The client decoder's per-datagram frame (0 until a timestamp in this datagram) is what
    // IMPORTANT acks carry.
    std::uint32_t datagramFrame = 0;
    // However the datagram ends: the client-predicted drawables to this frame.
    struct PredictAfter {
        World& world;
        const std::uint32_t& frame;
        ~PredictAfter() { world.predict(frame); }
    } predictAfter{world_, frame_};
    std::size_t off = 0;
    while (off < payload.size()) {
        const auto rest = payload.subspan(off);
        const std::size_t size = messageSize(rest, Direction::ServerToClient);
        if (size == kVariableSize) {
            if (!things_) {
                return;
            }
            try {
                const auto stream = readUpdateStream(rest, aliases_, frame_, [this](std::uint16_t nc, std::uint16_t type) {
                    return world_.drawableClass(nc, type);
                });
                // A datagram overtaken by a newer frame's (its timestamp behind ours): late. It
                // mustn't put us or anything back where the older frame had them.
                const bool late = datagramFrame != 0 && datagramFrame < frame_;
                world_.applyStream(stream, frame_, late);
                if (tracer_) {
                    tracer_(false, rest.first(stream.size));
                }
                if (late) {
                    ++lateUpdates_;
                } else {
                    position_ = std::pair{stream.header.x, stream.header.y};
                    substep_ = 0; // (a frame's update: whole)
                    ++snapshots_;
                    ++frameUpdates_;
                }
                for (const auto& alias : stream.newAliases) {
                    queueUnreliable(encode(alias, Direction::ClientToServer));
                }
                off += stream.size;
            } catch (const std::exception& ex) {
                event(ClientEvent::Kind::Log, std::string("update stream: ") + ex.what());
                return;
            }
            continue;
        }
        if (size == 0) {
            if (tracer_) {
                tracer_(false, rest); // what 1.2b would drop: an unknown or truncated message
            }
            // Logged at the 1st, 2nd, 4th, 8th... time per message: what follows it is lost (a
            // pickup's report once left the inventory without it).
            // (Not the transport's own injected notes, 0x21-0x26: they come alone and end here.)
            const bool injected = rest[0] >= 0x21 && rest[0] <= 0x26;
            const std::uint32_t n = injected ? 0 : ++dropped_[rest[0]];
            if (!injected && (n & (n - 1)) == 0) {
                static constexpr char kHex[] = "0123456789ABCDEF";
                std::string bytes;
                for (std::size_t i = 0; i < std::min<std::size_t>(rest.size(), 48); ++i) {
                    bytes += kHex[rest[i] >> 4];
                    bytes += kHex[rest[i] & 15];
                    bytes += ' ';
                }
                event(ClientEvent::Kind::Log, "dropped the rest of a datagram (x" + std::to_string(n) + ") at message 0x" +
                                                  std::string{kHex[rest[0] >> 4], kHex[rest[0] & 15]} + ", " +
                                                  std::to_string(rest.size()) + " bytes, after " +
                                                  std::to_string(off) + ": " + bytes);
            }
            return; // like 1.2b: an unknown or truncated message ends the datagram
        }
        if (tracer_) {
            tracer_(false, rest.first(size));
        }
        handleMessage(rest.first(size), datagramFrame);
        off += size;
    }
}

// One message: the world applies it first, then the client's own handling (the secondary
// weapon, timestamps, noxcpp's extensions, the map, chat, transfers, IMPORTANT acks).
void HeadlessClient::handleMessage(std::span<const std::uint8_t> m, std::uint32_t& datagramFrame)
{
    world_.apply(m, frame_);
    syncLiveWalls(); // (only when a wall message changed them)
    switch (static_cast<Op>(m[0])) {
    case Op::ReportPickup:
    case Op::ReportModifiablePickup:
    case Op::ReportEquip:
    case Op::ReportDequip:
    case Op::ReportSecondaryWeapon:
        onEquipReport(m);
        break;
    case Op::ReportLastQuiver: // (Gui_Inv_SetLastQuiver: g_guiInvLastQuiverNetCode,
                               // the net code less its bit 15: netClearHighBit)
        lastQuiver_ = static_cast<std::uint16_t>(u16At(m, 1) & 0x7FFFu);
        break;
    case Op::Timestamp: {
        // The low 16 bits, with the top two as a wrap counter.
        const std::uint16_t low = u16At(m, 1);
        std::uint32_t candidate = (frame_ & 0xFFFF0000u) + low;
        // Ours near the top of a 16-bit span (top bits 3) and the new one near its bottom (0): it wrapped.
        if ((low >> 14) == 0 && ((frame_ & 0xFFFF) >> 14) == 3) {
            candidate += 0x10000;
        }
        // Never moves our frame back (an older datagram's timestamp).
        if (candidate >= frame_) {
            frame_ = candidate;
        }
        datagramFrame = candidate;
        break;
    }
    case Op::FullTimestamp:
        frame_ = u32At(m, 1);
        datagramFrame = frame_;
        break;
    case Op::NoxcppMapVote:
        onMapVote(m);
        break;
    case Op::NoxcppModeF4:
    case Op::NoxcppModeF5:
    case Op::NoxcppEdit:
    case Op::NoxcppCoop:
    case Op::NoxcppModeUi: // (a script mode's UI: modes/script's UI client, when we asked for it)
        // (a mode's own, by its opcode: setNoxcppHandler)
        if (const auto h = noxcppHandlers_.find(m[0]); h != noxcppHandlers_.end() && h->second) {
            h->second(m);
        }
        break;
    case Op::NoxcppSubstep: {
        // noxcpp's (we asked: LiveSession's extended query): the important objects at substep k
        // of m after the frame we're at; one for another frame (late, or early) is let go.
        // Layout: k +1, m (substeps a frame) +2, count +3, the frame's low 16 bits +4, then count
        // records of {u16 net code, u16 x, u16 y} from +6.
        const int k = m[1];
        const int count = m[3];
        const int per = m[2];
        if (k == 0) {
            // Substep 0: the frame itself, after its stream (same datagram): our own unit at its
            // whole place, where the stream's header put it at an even one. A correction of the
            // frame just taken, not a snapshot of its own.
            if (u16At(m, 4) == static_cast<std::uint16_t>(frame_) && count >= 1 && m.size() >= 12) {
                const std::uint16_t nc = u16At(m, 6);
                const int x = u16At(m, 8);
                const int y = u16At(m, 10);
                if (nc == netCode_ && world_.setSubstepPosition(nc, x, y) && position_) {
                    position_ = std::pair{x, y};
                }
            }
            break;
        }
        if (per < 2 || k < 1 || k >= per || u16At(m, 4) != static_cast<std::uint16_t>(frame_)) {
            break;
        }
        for (int i = 0; i < count; ++i) {
            const std::size_t at = 6 + 6 * static_cast<std::size_t>(i);
            const std::uint16_t nc = u16At(m, at);
            const int x = u16At(m, at + 2);
            const int y = u16At(m, at + 4);
            if (world_.setSubstepPosition(nc, x, y) && nc == netCode_ && position_) {
                position_ = std::pair{x, y}; // (the camera's on us)
            }
        }
        substep_ = static_cast<double>(k) / per;
        substepsPerFrame_ = per;
        ++snapshots_;
        break;
    }
    case Op::UseMap:
        closingForSave_ = false; // (Cli_ResetWorldState: cleared)
        onUseMap(m);
        break;
    case Op::MapSendStart: // Cli_MapDownload_Start: u32 size at +4, name at +8
        if (stage_ == Stage::Downloading) {
            download_.size = u32At(m, 4);
            if (download_.size > kMaxDownloadedMap) {
                stage_ = Stage::Failed;
                event(ClientEvent::Kind::Disconnected, "map download failed: " + std::to_string(download_.size) + " bytes is too big for a map");
                break;
            }
            download_.data.clear();
            // (Room for what a map can be, not for any u32 a server names: finishDownload refuses
            // a map past kMaxDownloadedMap, and the data grows as its blocks come.)
            download_.data.reserve(std::min<std::size_t>(download_.size, kMaxDownloadedMap));
            download_.nextBlock = 1;
            download_.pending.clear();
            download_.pendingBytes = 0;
            download_.started = true;
            event(ClientEvent::Kind::Log, "downloading " + readAscii(m.subspan(8, 80)) + ", " +
                                              std::to_string(download_.size) + " bytes");
        }
        break;
    case Op::MapSendPacket:
        if (stage_ == Stage::Downloading) {
            onMapBlock(m);
        }
        break;
    case Op::MapSendAbort:
        if (stage_ == Stage::Downloading) {
            stage_ = Stage::Failed;
            event(ClientEvent::Kind::Disconnected, "the server aborted the map download (reason " +
                                                       std::to_string(m[1]) + ")");
        }
        break;
    case Op::JoinData:
        netCode_ = u16At(m, 1);
        break;
    case Op::NewPlayer: {
        const std::uint16_t nc = u16At(m, 1);
        const std::string name = strings::toUtf8(utf16Until(m.subspan(3, 50)));
        const bool isNew = players_.find(nc) == players_.end();
        players_[nc] = name;
        if (isNew) {
            event(ClientEvent::Kind::PlayerJoined, name, nc, name);
        }
        break;
    }
    case Op::PlayerQuit: {
        const std::uint16_t nc = u16At(m, 1);
        const std::string name = players_.count(nc) ? players_[nc] : std::string{};
        players_.erase(nc);
        event(ClientEvent::Kind::PlayerLeft, name, nc, name);
        break;
    }
    case Op::TextMessage:
        onText(m);
        break;
    case Op::Inform:
        onInform(m);
        break;
    case Op::Important: {
        // IMPORTANT_ACK carries the datagram's frame, u32 little-endian.
        std::vector<std::uint8_t> ack = {static_cast<std::uint8_t>(Op::ImportantAck)};
        for (int i = 0; i < 4; ++i) {
            ack.push_back(static_cast<std::uint8_t>(datagramFrame >> (8 * i)));
        }
        queueUnreliable(std::move(ack));
        break;
    }
    case Op::XferMsg:
        onXfer(m);
        break;
    case Op::SeqImportant: {
        // Once kept as a set of every id seen, never emptied: from the second map on (where the
        // server counts from 0 again) the important messages were thrown away as repeats, the
        // pickups' reports among them, so the inventory and potion slots stopped filling.
        const std::uint16_t id = u16At(m, 1);
        const auto nested = m.subspan(4);
        // u16 arithmetic: an id 1-10000 behind the next expected one is a repeat.
        const auto behind = static_cast<std::uint16_t>(seqNext_ - id);
        if (behind >= 1 && behind <= 10000) {
            break; // done already
        }
        // Kept only when the nested message is whole.
        if (!nested.empty() && messageSize(nested, Direction::ServerToClient) == nested.size()) {
            seqPending_.try_emplace(id, SeqPending{{nested.begin(), nested.end()}, nowMs_});
        }
        drainSeqImportant(datagramFrame);
        break;
    }
    case Op::KickNotification:
        dropped(EndReason::Kicked, "kicked");
        break;
    case Op::TimeoutNotification:
        dropped(EndReason::TimedOut, "timed out");
        break;
    case Op::ServerQuit:
        // CliNet_OnPacket's 0xC7: MSG_SERVER_QUIT_ACK sent at once on its own (Net_Send flags 2),
        // then Game_FinishQuit: the game left as any leave is (the client's teardown sends
        // OUTGOING_CLIENT and waits for its ack: quit()).
        if (stage_ == Stage::WaitingMap || stage_ == Stage::Downloading || stage_ == Stage::InGame) {
            const std::uint8_t ack[] = {static_cast<std::uint8_t>(Op::ServerQuitAck)};
            conn_.sendUnreliableNow(ack);
            endReason_ = EndReason::ServerQuit;
            event(ClientEvent::Kind::Log, "the host has ended the game");
            quit();
        }
        break;
    default:
        break;
    }
}

// noxcpp's map vote (server/map_vote.hpp): opened, its tally, closed.
void HeadlessClient::onMapVote(std::span<const std::uint8_t> m)
{
    const auto v = server::mapvote::parse(m);
    if (!v) {
        return;
    }
    const auto rest = v->second;
    MapVoteState& s = mapVote_;
    switch (v->first) {
    case server::mapvote::kOpen:
        if (rest.size() >= 4) {
            // Open: kind, u16 seconds left, candidate count, then each name as a length byte and its characters.
            s = MapVoteState{.version = s.version, .available = s.available};
            s.open = true;
            s.kind = rest[0];
            s.endsAtMs = nowMs_ + static_cast<std::uint32_t>(rest[1] | rest[2] << 8) * 1000u;
            std::size_t at = 4;
            for (int i = 0; i < rest[3] && at < rest.size(); ++i) {
                const std::size_t n = rest[at++];
                const std::size_t take = std::min(n, rest.size() - at);
                s.candidates.emplace_back(reinterpret_cast<const char*>(rest.data() + at), take);
                at += take;
            }
            s.tally.assign(s.candidates.size(), 0);
        }
        break;
    case server::mapvote::kTally:
        // Tally: count, that many tallies, the voters, our vote (0xFF none).
        if (!rest.empty()) {
            const std::size_t count = rest[0];
            if (rest.size() >= 1 + count + 2) {
                s.tally.assign(rest.begin() + 1, rest.begin() + 1 + static_cast<std::ptrdiff_t>(count));
                s.voters = rest[1 + count];
                const std::uint8_t mine = rest[2 + count];
                s.mine = mine == 0xFF ? std::nullopt : std::optional<int>{mine};
            }
        }
        break;
    case server::mapvote::kClose:
        // Close: byte 1 the chosen candidate (0xFF none).
        if (rest.size() >= 2) {
            s.open = false;
            s.result = rest[1] == 0xFF ? std::nullopt : std::optional<int>{rest[1]};
        }
        break;
    case server::mapvote::kAvailable:
        if (!rest.empty()) {
            s.available = rest[0] != 0;
            return; // (not the vote itself: its version stays)
        }
        break;
    default:
        break;
    }
    ++s.version;
}

// Handles the waiting SEQ_IMPORTANT messages in id order from seqNext_.
void HeadlessClient::drainSeqImportant(std::uint32_t& datagramFrame)
{
    // Stuck 30 s on a missing id, go on from the first one waiting.
    if (!seqPending_.empty() && !seqPending_.contains(seqNext_)) {
        auto first = seqPending_.begin();
        std::uint32_t oldest = first->second.atMs;
        for (auto it = seqPending_.begin(); it != seqPending_.end(); ++it) {
            // The id nearest ahead of seqNext_, in wrapping u16 order.
            if (static_cast<std::uint16_t>(it->first - seqNext_) < static_cast<std::uint16_t>(first->first - seqNext_)) {
                first = it;
            }
            oldest = std::min(oldest, it->second.atMs);
        }
        if (nowMs_ - oldest > 30'000) {
            seqNext_ = first->first;
        }
    }
    for (auto it = seqPending_.find(seqNext_); it != seqPending_.end(); it = seqPending_.find(seqNext_)) {
        const std::vector<std::uint8_t> bytes = std::move(it->second.bytes);
        seqPending_.erase(it);
        ++seqNext_;
        handleMessage(bytes, datagramFrame);
    }
}

// USE_MAP: our copy of the map whose header CRC matches (the solo path, the maps folder, the
// download cache), else the server is asked for it.
void HeadlessClient::onUseMap(std::span<const std::uint8_t> m)
{
    const auto useMap = decode<msg::UseMap>(m);
    mapName_ = readAscii(useMap.mapName);
    std::string stem = mapName_;
    if (const auto dot = stem.rfind('.'); dot != std::string::npos) {
        stem.resize(dot);
    }
    // mapValidateMB compares the CRC stored in the map file's header. The host's
    // own client of a solo game reads the solo path while the server says so (SoloMap); others
    // look in the game's maps folder, then in our download cache. (Should the solo path not
    // check, the exe's host goes ready on the map it shares; ours looks on.)
    if (soloMap_.path) {
        if (const auto path = soloMap_.path()) {
            try {
                if (const auto file = map::loadMapFile(*path); file.crc == useMap.crc) {
                    mapPath_ = *path;
                    mapReady(file);
                    return;
                }
            } catch (const std::exception&) {
            }
        }
    }
    // The name becomes a folder and a file name under the maps folder and the download cache: one
    // that isn't a plain name ("..\x", "C:\x") would reach outside them, so it's no map at all.
    if (!isPlainFileName(stem)) {
        stage_ = Stage::Failed;
        event(ClientEvent::Kind::Disconnected, "the server's map name isn't a file name: " + mapName_);
        return;
    }
    std::string problem;
    for (const auto& dir : {mapsDir_, mapCache_}) {
        if (dir.empty()) {
            continue;
        }
        const auto path = dir / stem / (stem + ".map");
        try {
            if (!std::filesystem::exists(resolveNoCase(path))) { // (in any case, as on Windows)
                problem = "not found";
            } else if (const auto file = map::loadMapFile(path); file.crc != useMap.crc) {
                problem = "has a different CRC";
            } else {
                mapPath_ = path;
                mapReady(file);
                return;
            }
        } catch (const std::exception& ex) {
            problem = std::string("can't be read: ") + ex.what();
        }
    }
    if (mapCache_.empty()) {
        stage_ = Stage::Failed;
        event(ClientEvent::Kind::Disconnected, "map " + mapName_ + " " + problem + " (downloading is off)");
        return;
    }
    // Like gameChangeMap: ask for it, then keep the connection
    // alive until it's here.
    download_ = Download{};
    download_.name = stem;
    download_.expectedCrc = useMap.crc;
    queueReliable({static_cast<std::uint8_t>(Op::RequestMap)});
    stage_ = Stage::Downloading;
    event(ClientEvent::Kind::Log, "map " + mapName_ + " " + problem + "; requesting it from the server");
}

std::vector<HeadlessClient::MapObject> HeadlessClient::mapObjectsOf(const map::MapFile& file, const things::ThingDb& things)
{
    std::vector<MapObject> out;
    const auto toc = map::decodeSection<map::ObjectTOC>(file);
    const auto objects = map::decodeSection<map::ObjectData>(file);
    std::map<std::uint16_t, std::uint16_t> types; // TOC id -> type id
    for (const auto& e : toc.entries) {
        if (const auto* t = things.findThing(e.name)) {
            types[e.id] = static_cast<std::uint16_t>(t - things.things.data() + 1);
        }
    }
    std::size_t inside = 0; // the previous object's inventory: not on the ground
    for (const auto& rec : objects.records) {
        if (inside > 0) {
            --inside;
            continue;
        }
        map::ObjectXfer x;
        try {
            x = map::ObjectXfer::parse(rec.data);
        } catch (const std::exception&) {
            continue;
        }
        inside = x.header.inventoryCount();
        if (const auto t = types.find(rec.tocId); t != types.end()) {
            // (No details: the object as its type makes it, enabled.)
            out.push_back({t->second, x.header.x, x.header.y, x.header.extent,
                           x.header.details ? x.header.details->flags : 0x1000000u,
                           x.header.details ? x.header.details->team : std::uint8_t{0}, std::move(x.extra)});
        }
    }
    return out;
}

std::vector<NavGrid::Link> HeadlessClient::ridesOf(const std::vector<MapObject>& objects, const things::ThingDb& things)
{
    // A teleporter (TransporterXfer: u32 its destination's object number, XFerTransporter) to where it
    // goes; an elevator (ElevatorXfer: u32 its shaft's, XFerElevator) to its shaft and back, a
    // couple of seconds' wait each way (updateElevator). Enabled ones only (object flag 0x1000000:
    // elevators wait for it).
    std::vector<NavGrid::Link> links;
    const auto byExtent = [&](std::uint32_t extent) -> const MapObject* {
        for (const auto& o : objects) {
            if (o.extent == extent) {
                return &o;
            }
        }
        return nullptr;
    };
    for (const auto& o : objects) {
        if (o.typeId == 0 || o.typeId > things.things.size() || o.xferExtra.size() < 4 || !(o.flags & 0x1000000u)) {
            continue;
        }
        const std::string_view xfer = things.things[o.typeId - 1u].attribute("XFER").value_or("");
        const std::uint32_t other = static_cast<std::uint32_t>(o.xferExtra[0]) | (o.xferExtra[1] << 8) |
                                    (o.xferExtra[2] << 16) | (static_cast<std::uint32_t>(o.xferExtra[3]) << 24);
        const MapObject* to = other != 0 ? byExtent(other) : nullptr;
        if (!to) {
            continue;
        }
        const std::pair<int, int> here{static_cast<int>(o.x), static_cast<int>(o.y)};
        const std::pair<int, int> there{static_cast<int>(to->x), static_cast<int>(to->y)};
        if (xfer == "TransporterXfer") {
            // (Waited on through a cooldown, stepping off and on: Link::restep.)
            links.push_back({here, there, 3, 150, true});
        } else if (xfer == "ElevatorXfer") {
            // (the ride and the wait: ~2 s of walking; waited for through a whole cycle, 15 s: the
            // car may be up there, or on its way, when we come)
            links.push_back({here, there, 20, 450});  // each way: cost 20 cells, 450 frames' patience
            links.push_back({there, here, 20, 450});
        }
    }
    return links;
}

void HeadlessClient::mapReady(const map::MapFile& file)
{
    nav_ = NavGrid::fromMap(file);
    if (things_) {
        try {
            nav_.addObstacles(file, *things_); // obelisks, columns, rocks: paths go round them
            nav_.addFloor(file);                // and stay on the floor
        } catch (const std::exception& ex) {
            event(ClientEvent::Kind::Log, std::string("map objects unreadable for paths: ") + ex.what());
        }
    }
    // The objects the map places, and the rides among them for paths.
    mapObjects_.clear();
    if (things_) {
        try {
            mapObjects_ = mapObjectsOf(file, *things_);
        } catch (const std::exception& ex) {
            event(ClientEvent::Kind::Log, std::string("map objects unreadable: ") + ex.what());
        }
        nav_.setLinks(ridesOf(mapObjects_, *things_));
    }
    ++mapLoads_;
    breakableCells_.clear();
    secretCells_.clear();
    try {
        for (const auto& p : map::decodeSection<map::DestructableWalls>(file).walls) {
            breakableCells_.emplace_back(p.x, p.y);
        }
    } catch (const std::exception&) {
        // none
    }
    try {
        for (const auto& s : map::decodeSection<map::SecretWalls>(file).walls) {
            secretCells_.push_back({{s.pos.x, s.pos.y}, (s.flags & 8) != 0}); // (8: starts open)
        }
    } catch (const std::exception&) {
        // none
    }
    navWallsVersion_ = world_.game().wallsVersion - 1; // (synced with the next message)
    syncLiveWalls();
    if (soloMap_.read) {
        soloMap_.read(); // (mapCliReadAll's end: gameSetNoMPFlag(0))
    }
    queueReliable({static_cast<std::uint8_t>(Op::ClientReady)});
    stage_ = Stage::InGame;
    event(ClientEvent::Kind::InGame, mapName_);
}

void HeadlessClient::syncLiveWalls()
{
    const auto& game = world_.game();
    if (nav_.empty() || game.wallsVersion == navWallsVersion_) {
        return;
    }
    navWallsVersion_ = game.wallsVersion;
    std::vector<std::pair<int, int>> raised;
    for (const auto& [cell, wall] : game.magicWalls) {
        raised.emplace_back(cell.first, cell.second);
    }
    std::vector<std::pair<int, int>> gone;
    for (const auto id : game.destroyedWalls) {
        if (id < breakableCells_.size()) {
            gone.push_back(breakableCells_[id]);
        }
    }
    for (std::size_t id = 0; id < secretCells_.size(); ++id) {
        const auto key = static_cast<std::uint16_t>(id);
        // A secret wall is gone while open: opened by the server, or open on the map and not closed since.
        if (game.openWalls.contains(key) || (secretCells_[id].second && !game.closedWalls.contains(key))) {
            gone.push_back(secretCells_[id].first);
        }
    }
    nav_.setLiveWalls(raised, gone);
}

void HeadlessClient::onMapBlock(std::span<const std::uint8_t> m)
{
    // Cli_MapDownload_OnBlock: u16 block at +2, u16 size at +4, data at +6. Blocks are
    // written in order; early ones wait in a list. An empty block is nothing (the exe's too).
    // (Ours, bounded whatever a server sends: nothing before MAP_SEND_START; what's written and
    // what waits never past the size it named; at most kMaxPendingBlocks waiting.)
    const std::uint16_t block = u16At(m, 2);
    const auto data = m.subspan(6);
    if (!download_.started || data.empty()) {
        return;
    }
    if (block == download_.nextBlock) {
        if (download_.data.size() + data.size() > download_.size) {
            stage_ = Stage::Failed;
            event(ClientEvent::Kind::Disconnected, "map download failed: the server sent more than the map's " +
                                                       std::to_string(download_.size) + " bytes");
            return;
        }
        download_.data.insert(download_.data.end(), data.begin(), data.end());
        ++download_.nextBlock;
    } else if (block > download_.nextBlock) {
        // (Held, or a repeat's bytes in place of the last ones; no room: dropped, as if it never came.)
        const auto held = download_.pending.find(block);
        const bool counted = held != download_.pending.end();
        const std::size_t others = download_.pendingBytes - (counted ? held->second.size() : 0);
        if ((counted || download_.pending.size() < kMaxPendingBlocks) && download_.data.size() + others + data.size() <= download_.size) {
            download_.pending[block].assign(data.begin(), data.end());
            download_.pendingBytes = others + data.size();
        }
    }
    for (auto it = download_.pending.find(download_.nextBlock); it != download_.pending.end();
         it = download_.pending.find(download_.nextBlock)) {
        download_.data.insert(download_.data.end(), it->second.begin(), it->second.end());
        download_.pendingBytes -= it->second.size();
        download_.pending.erase(it);
        ++download_.nextBlock;
    }
    if (download_.size != 0 && download_.data.size() >= download_.size) {
        finishDownload();
    }
}

void HeadlessClient::finishDownload()
{
    queueReliable({static_cast<std::uint8_t>(Op::ReceivedMap)}); // netMapReceived
    try {
        // A .nxz map: u32 size of the .map, then the NXZ stream.
        if (download_.data.size() < 4) {
            throw FormatError("map download too short");
        }
        const std::uint32_t mapSize = u32At(download_.data, 0);
        // (The biggest stock map is under 700 KB: a server's NXZ claiming more than this would
        // have the client decompress gigabytes from a few bytes.)
        if (mapSize > kMaxDownloadedMap) {
            throw FormatError("map download too big");
        }
        // The .map inside is the file as stored on disk: Blowfish-encrypted.
        const auto stored = video::nxzDecompress(std::span(download_.data).subspan(4), mapSize);
        auto plain = stored;
        crypto::cipherForKey(crypto::FileKey::Map).decrypt(plain);
        const auto file = map::parseMapFile(plain);
        if (file.crc != download_.expectedCrc) {
            throw FormatError("downloaded map has a different CRC");
        }
        const auto dir = mapCache_ / download_.name;
        std::filesystem::create_directories(dir);
        writeFileBytes(dir / (download_.name + ".nxz"), download_.data);
        writeFileBytes(dir / (download_.name + ".map"), stored);
        event(ClientEvent::Kind::Log, "map saved to " + (dir / (download_.name + ".map")).string());
        download_.data.clear();
        mapPath_ = dir / (download_.name + ".map");
        mapReady(file);
    } catch (const std::exception& ex) {
        stage_ = Stage::Failed;
        event(ClientEvent::Kind::Disconnected, std::string("map download failed: ") + ex.what());
    }
}

void HeadlessClient::onXfer(std::span<const std::uint8_t> m)
{
    const std::uint8_t sub = m[1];
    const auto reply = [this](std::initializer_list<std::uint8_t> b) { queueReliable(std::vector<std::uint8_t>(b)); };
    switch (sub) {
    case 0x00: { // start (Net_Xfer_OnRecvStart): answer accept with our receive id
        const auto start = decode<msg::xfer::Start>(m);
        // (Every one of the 256 receive ids taken: a server that starts sends and never ends them.
        // The search below would go round for good; the start is let go unanswered instead.)
        if (start.size == 0 || xfers_.size() >= 256) {
            break;
        }
        // The lowest receive id not in use.
        std::uint8_t id = 0;
        while (xfers_.count(id)) {
            ++id;
        }
        XferIn& x = xfers_[id];
        x.sendId = start.sendId;
        x.type = readAscii(start.type);
        x.size = start.size;
        reply({static_cast<std::uint8_t>(Op::XferMsg), 0x01, id, start.sendId});
        break;
    }
    case 0x02: { // data: ack every chunk; done when complete (Net_Xfer_OnRecvData)
        // Layout: our receive id +2, a byte echoed in the ack +3, u16 chunk +4, data from +8.
        const std::uint8_t id = m[2];
        const std::uint16_t chunk = u16At(m, 4);
        reply({static_cast<std::uint8_t>(Op::XferMsg), 0x03, id, m[3], static_cast<std::uint8_t>(chunk),
               static_cast<std::uint8_t>(chunk >> 8)});
        const auto it = xfers_.find(id);
        if (it == xfers_.end()) {
            break;
        }
        XferIn& x = it->second;
        // A transfer whose first chunk comes numbered 0 counts from 0.
        if (x.data.empty() && chunk == 0) {
            x.nextChunk = 0;
        }
        if (chunk == x.nextChunk) {
            const auto data = m.subspan(8);
            x.data.insert(x.data.end(), data.begin(), data.end());
            ++x.nextChunk;
        }
        if (x.data.size() >= x.size) {
            reply({static_cast<std::uint8_t>(Op::XferMsg), 0x04, id});
            const std::string type = lowered(x.type);
            if (type == "motd") {
                event(ClientEvent::Kind::Motd, readAscii(x.data));
            } else {
                event(ClientEvent::Kind::Log, "received a " + x.type + " transfer of " + std::to_string(x.size) + " bytes");
            }
            // The received file's SAVEDATA (action 2): kept (the character's file and
            // our own sections, by the handler), then in a quest (game flag 0x1000) either our
            // leave, when closing, or the host told
            // (CliNet_SendGauntletQuestReady: MSG_GAUNTLET 0x1B), which ends its 0x1C {1} lock on
            // the quest's screens. (Out of a quest, the leave after a save is the menu's.)
            if (type == "savedata") {
                const std::vector<std::uint8_t> data(x.data.begin(), x.data.begin() + x.size);
                xfers_.erase(it);
                if (saveData_) {
                    saveData_(data);
                }
                if (world_.game().gameFlags & 0x1000u) {  // 0x1000: a quest game
                    if (closingForSave_) {
                        quit();
                    } else {
                        reply({static_cast<std::uint8_t>(Op::Gauntlet), 0x1B});
                    }
                }
                break;
            }
            xfers_.erase(it);
        }
        break;
    }
    case 0x01: // accept, ack, done, abort: our own sends' (the server's receive answers them)
    case 0x03:
    case 0x04:
    case 0x06:
        if (sends_) {
            sends_->receive(0, m, frame_);
        }
        break;
    case 0x05: // a send of the server's given up: our receive stream dropped
        xfers_.erase(m[2]);
        break;
    default:
        break;
    }
}

void HeadlessClient::onInform(std::span<const std::uint8_t> m)
{
    // MSG_INFORM (client_handlePacketInform): notices the vanilla client prints.
    const auto name = [this](std::uint32_t nc) {
        const auto it = players_.find(static_cast<std::uint16_t>(nc));
        return it != players_.end() ? it->second : "#" + std::to_string(nc);
    };
    const std::uint8_t sub = m[1];
    // Most notices: a u32 at +2 (a player's net code or a team) and one at +6 (a team).
    const std::uint32_t a = m.size() >= 6 ? u32At(m, 2) : 0;
    const std::uint32_t b = m.size() >= 10 ? u32At(m, 6) : 0;
    std::string text;
    switch (sub) {
    case 3: text = name(a) + " timed out"; break;
    case 4: text = name(a) + " returned their flag"; break;
    case 5: text = name(a) + " captured team " + std::to_string(b) + "'s flag"; break;
    case 6: text = name(a) + " picked up team " + std::to_string(b) + "'s flag"; break;
    case 7: text = name(a) + " dropped team " + std::to_string(b) + "'s flag"; break;
    case 8: text = "team " + std::to_string(a) + "'s flag is back at base"; break;
    case 9: text = name(a) + " scored for team " + std::to_string(b); break;
    case 10: text = name(a) + " picked up the crown"; break;
    case 11: text = name(a) + " dropped the crown"; break;
    case 12: text = a ? "observer mode (press jump to join)" : "observer mode"; break;
    case 14: { // kill feed: attacker, assist, victim, weapon, kind
        const std::uint16_t attacker = u16At(m, 2);
        const std::uint16_t victim = u16At(m, 6);
        text = attacker ? name(attacker) + " killed " + name(victim) : name(victim) + " died";
        ClientEvent kill{ClientEvent::Kind::Kill, text, victim};
        kill.attacker = attacker;
        kill.assist = u16At(m, 4);
        if (m.size() >= 11) {
            kill.weapon = u16At(m, 8);
            kill.killKind = m[10];
        }
        pushEvent(std::move(kill));
        break;
    }
    case 15: text = "[" + readAscii(m.subspan(3)) + "]"; break;
    case 16: text = "wrong team"; break;
    case 18: text = name(a) + " left the game"; break;
    default: return;
    }
    event(ClientEvent::Kind::Notice, text);
    events_.back().informSub = sub;
}

void HeadlessClient::onText(std::span<const std::uint8_t> m)
{
    const std::uint16_t nc = u16At(m, 1);
    const std::uint8_t flags = m[3];
    const auto body = m.subspan(11);
    std::string text;
    // 0x0A: 8-bit text (2) or a string id (8); otherwise UTF-16.
    if (flags & 0x0A) {
        text = readAscii(body);
        if (flags & 0x08) {
            text = "[" + text + "]"; // a string-table id
        }
    } else {
        text = strings::toUtf8(utf16Until(body));
    }
    // The speaker: a player we've been told of, else one the world knows. The server relays a
    // line with whatever speaker its sender's client put on it (SrvNet_OnPacket 0xA8): someone
    // spectating sends one that's no player's, and the server's console its own 0. The server's
    // own messages are string ids (flag 8) or notices (flag 0x10); any other line without a
    // known speaker is still someone talking, "Someone", and can be answered.
    std::string name;
    if (const auto it = players_.find(nc); it != players_.end()) {
        name = it->second;
    } else if (const auto* p = world_.player(nc)) {
        name = p->name;
    } else if (!(flags & 0x18)) {  // 0x18: a string id or a notice, the server's own lines.
        name = "Someone";
    }
    event(ClientEvent::Kind::Chat, text, nc, name);
    // Team chat (flag 1) is "<Guirank.c:team>: text", but not a string id's (flag 8).
    events_.back().team = (flags & 0x01) && !(flags & 0x08);
    events_.back().textFlags = flags;
}

// ------------------------------------------------------------------------------ outgoing

// One game frame: while downloading, the keep-alive; in game, the hunt, the walk, this frame's
// controls and the cursor; then a keep-alive if nothing else carried our ack lately.
void HeadlessClient::gameFrame(std::uint32_t nowMs)
{
    if (stage_ == Stage::Downloading) {
        // The download's wait sends KEEP_ALIVE every frame while downloading (skipping frames that are
        // multiples of 30).
        if (frame_ % 30 != 0) {
            queueUnreliable({static_cast<std::uint8_t>(Op::KeepAlive)});
        }
        return;
    }
    if (stage_ == Stage::InGame) {
        std::vector<ControlEntry> controls;

        // Cli_ProcessControlEvents sends the facing every frame, moving or not. A
        // frame with no controls at all reads as a lagging client: the server keeps applying
        // the last movement for about 27 frames.
        bool attack = false;
        if (huntTarget_) {
            // Dead while hunting: press Action to respawn (about once a second), then carry on.
            const auto* me = world_.player(netCode_);
            if (me && me->dead) {
                if (frame_ % 30 == 0) {
                    spawnPending_ = true;
                }
                walkTarget_.reset();
                moveDirection_.reset();
            }
        }
        if (huntTarget_ && !spawnPending_) {
            const Drawable* target = world_.drawable(*huntTarget_);
            const auto at = world_.camera();
            if (!target || !at) {
                event(ClientEvent::Kind::Log, "hunt target lost");
                hunt(std::nullopt);
            } else {
                const double dx = target->x - at->first;
                const double dy = target->y - at->second;
                if (dx * dx + dy * dy <= 40.0 * 40.0) {
                    walkTarget_.reset();
                    path_.clear();
                    moveDirection_.reset();
                    facing_ = facingToward(dx, dy); // face the target, as steer() does
                    attack = true;
                } else if (!walkTarget_ || static_cast<std::int32_t>(nowMs - huntReplanAt_) >= 0) {
                    walkTo(target->x, target->y); // the target moves: re-plan now and then
                    huntReplanAt_ = nowMs + 500;
                }
            }
        }
        if (walkTarget_) {
            steer(nowMs);
        }
        if (moveDirection_) {
            facing_ = *moveDirection_;
        }
        // Same order as Cli_ProcessControlEvents: key-binding events first, the facing last.
        if (spawnPending_ || attack || attackHeld_) {
            controls.push_back({ControlCode::Action, 0}); // leave observer mode, or swing
            spawnPending_ = false;
        }
        if (jumpPending_) {
            controls.push_back({ControlCode::Jump, 0});
            jumpPending_ = false;
        }
        for (const ControlCode code : std::exchange(pendingControls_, {})) {
            controls.push_back({code, 0});
        }
        if (moveDirection_) {
            // 1, as the vanilla client always sends: its bit 1 comes from a flag (& 8)
            // nothing sets. Walking or running is the server's call, from how far the cursor
            // (MSG_MOUSE) is: running past 100 units, and once running, until the player stops
            // (Plr_UpdateState, its threshold 100^2).
            controls.push_back({ControlCode::MoveForward, 1});
        }
        controls.push_back({ControlCode::Orientation, facing_});
        sentFacing_ = facing_; // (setFreshInput's: what the server has)
        sentMoving_ = moveDirection_.has_value();
        queueControls(controls);
        // Then the cursor, when it has moved (MSG_MOUSE: u16 x, u16 y).
        if (const auto cursor = cursorToSend(); cursor && cursor != sentCursor_) {
            queueMouse(cursor->first, cursor->second);
            sentCursor_ = cursor;
        }
        lastInput_ = {moveDirection_.has_value(), facing_, attack || attackHeld_, sentCursor_};
        ++inputFrames_;
    }
    // Before we're in the game nothing else carries our ack. The vanilla client only sends
    // KEEP_ALIVE while downloading a map; the server accepts it any time.
    // (A deliberate deviation.)
    if (!queuedThisFrame_ && nowMs - lastQueuedAt_ >= kKeepAliveMs) {
        queueUnreliable({static_cast<std::uint8_t>(Op::KeepAlive)});
    }
    queuedThisFrame_ = false;
}

void HeadlessClient::queueUnreliable(std::vector<std::uint8_t> bytes)
{
    if (tracer_) {
        tracer_(true, bytes);
    }
    if (!conn_.sendUnreliable(bytes)) {
        event(ClientEvent::Kind::Log, "send buffer full; dropped a message");
    }
    lastQueuedAt_ = nowMs_;
    queuedThisFrame_ = true;
}

void HeadlessClient::queueReliable(std::vector<std::uint8_t> bytes)
{
    if (tracer_) {
        tracer_(true, bytes);
    }
    try {
        conn_.sendReliable(bytes);
    } catch (const std::exception& ex) {
        event(ClientEvent::Kind::Log, std::string("reliable send failed: ") + ex.what());
    }
}

} // namespace nox::client
