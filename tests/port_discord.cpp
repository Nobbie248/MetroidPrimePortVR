#include "port_discord.h"
#include "port_json.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace {
int sLine = 0;
void Check(bool condition) {
  if (!condition) {
    std::fprintf(stderr, "discord regression failed at line %d\n", sLine);
    std::abort();
  }
}
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    sLine = __LINE__;                                                                              \
    Check(cond);                                                                                   \
  } while (0)

bool ParseOk(const std::string& text, PortJson::Value& out) {
  size_t offset = 0;
  const char* reason = nullptr;
  return PortJson::Parse(text, out, offset, &reason);
}
} // namespace

int main() {
  using namespace PortDiscord;

  // Frames: little-endian opcode and length, then the payload.
  {
    const std::string frame = EncodeFrame(kOp_Frame, "{}");
    CHECK(frame == std::string("\x01\x00\x00\x00\x02\x00\x00\x00{}", 10));
    std::string buffer = frame.substr(0, 9);
    Frame out;
    bool bad = true;
    CHECK(!TakeFrame(buffer, out, bad) && !bad);
    buffer += frame.substr(9) + EncodeFrame(kOp_Ping, "p");
    CHECK(TakeFrame(buffer, out, bad) && out.op == kOp_Frame && out.payload == "{}");
    CHECK(TakeFrame(buffer, out, bad) && out.op == kOp_Ping && out.payload == "p");
    CHECK(buffer.empty() && !TakeFrame(buffer, out, bad) && !bad);
    std::string big = EncodeFrame(kOp_Frame, std::string(300, 'x'));
    CHECK(static_cast<unsigned char>(big[5]) == 1 && static_cast<unsigned char>(big[4]) == 44);
    big[6] = 1; // 65836 bytes: over the limit
    CHECK(!TakeFrame(big, out, bad) && bad);
  }

  // JSON strings and field limits.
  {
    CHECK(JsonString("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\u000a\\u0001\"");
    CHECK(JsonString("Ph\xc3\xa9ndrana") == "\"Ph\xc3\xa9ndrana\"");
    CHECK(FitField("x") == "x ");
    CHECK(FitField("xy") == "xy");
    std::string longText(127, 'a');
    longText += "\xc3\xa9\xc3\xa9"; // the cut falls inside the first é
    CHECK(FitField(longText) == std::string(127, 'a'));
    CHECK(FitField(std::string(200, 'b')).size() == 128);
  }

  // Game text: tags stripped, && kept, UTF-16 to UTF-8, surrogates.
  {
    const char16_t text[] = u"&just=center;Phéndrana&&Drifts\n2";
    CHECK(GameTextToUtf8(text) == "Ph\xc3\xa9ndrana&Drifts 2");
    const char16_t pair[] = {0xd83d, 0xde00, 'a', 0xdc00, 'b', 0};
    CHECK(GameTextToUtf8(pair) == "\xf0\x9f\x98\x80" "ab");
    const char16_t open[] = u"A&unterminated";
    CHECK(GameTextToUtf8(open) == "A");
    CHECK(GameTextToUtf8(static_cast<const wchar_t*>(nullptr)).empty());
    const wchar_t wide[] = L"Landing Site";
    CHECK(GameTextToUtf8(wide) == "Landing Site");
  }

  // Payloads parse and carry what Discord expects.
  {
    PortJson::Value value;
    CHECK(ParseOk(HandshakePayload("123"), value));
    CHECK(value.Find("v")->AsInt() == 1 && value.StringOr("client_id") == "123");

    GameInfo info;
    info.world = "Tallon Overworld";
    info.area = "Landing Site";
    info.percent = 12;
    info.hard = true;
    info.energy = 199;
    info.missiles = 25;
    const Presence game = GamePresence(info, 1700000000);
    CHECK(game.details == "Landing Site");
    CHECK(game.state == "199 energy \xc2\xb7 25 missiles \xc2\xb7 12% items");
    CHECK(game.hover == "Tallon Overworld \xc2\xb7 Hard mode");
    CHECK(ParseOk(ActivityPayload(42, game, "7"), value));
    CHECK(value.StringOr("cmd") == "SET_ACTIVITY" && value.StringOr("nonce") == "7");
    const PortJson::Value* args = value.Find("args");
    CHECK(args != nullptr && args->Find("pid")->AsInt() == 42);
    const PortJson::Value* activity = args->Find("activity");
    CHECK(activity != nullptr && activity->StringOr("details") == "Landing Site");
    CHECK(activity->StringOr("state") == game.state);
    CHECK(activity->Find("timestamps")->Find("start")->AsInt() == 1700000000);
    CHECK(activity->Find("assets")->StringOr("large_image") == "logo");
    CHECK(activity->Find("assets")->StringOr("large_text") == game.hover);
    CHECK(game.image.empty() && activity->Find("assets")->Find("small_image") == nullptr);

    // World pictures (always on): the world's asset takes the large image, the logo the
    // small one. A world without one (the end cinema) keeps the logo.
    info.worldId = 0x83F6FF6Fu;
    const Presence pictured = GamePresence(info, 0);
    CHECK(pictured.image == "world_chozo");
    CHECK(ParseOk(ActivityPayload(42, pictured, "9"), value));
    const PortJson::Value* assets = value.Find("args")->Find("activity")->Find("assets");
    CHECK(assets->StringOr("large_image") == "world_chozo");
    CHECK(assets->StringOr("small_image") == "logo");
    CHECK(pictured != game);
    info.worldId = 0x13D79165u;
    CHECK(GamePresence(info, 0).image.empty());
    CHECK(std::strcmp(WorldImage(0x158EFE17u), "world_frigate") == 0);
    CHECK(std::strcmp(WorldImage(0xC13B09D1u), "world_crater") == 0);

    // Names still loading: the world alone, then a generic line. Without a
    // launcher there's no missile count; without a world, the default tooltip.
    GameInfo early;
    early.world = "Chozo Ruins";
    early.energy = 99;
    early.missiles = 1;
    CHECK(GamePresence(early, 0).details == "Chozo Ruins");
    CHECK(GamePresence(early, 0).state == "99 energy \xc2\xb7 1 missile \xc2\xb7 0% items");
    CHECK(GamePresence(early, 0).hover.empty());
    early.world.clear();
    early.missiles = -1;
    CHECK(GamePresence(early, 0).details == "In game");
    CHECK(GamePresence(early, 0).state == "99 energy \xc2\xb7 0% items");
    CHECK(ParseOk(ActivityPayload(1, GamePresence(early, 0), "8"), value));
    CHECK(value.Find("args")->Find("activity")->Find("assets")->StringOr("large_text") ==
          "Metroid Prime native port");

    Presence menu;
    menu.details = "In the menus";
    CHECK(ParseOk(ActivityPayload(1, menu, "1"), value));
    activity = value.Find("args")->Find("activity");
    CHECK(activity->Find("state") == nullptr && activity->Find("timestamps") == nullptr);

    CHECK(ParseOk(ClearPayload(9, "2"), value));
    CHECK(value.Find("args")->Find("activity") == nullptr);
  }

  // Socket search order.
#if !defined(_WIN32)
  {
    std::map<std::string, std::string> env = {{"TMPDIR", "/var/tmp/"}};
    auto get = [&](const char* name) -> const char* {
      auto it = env.find(name);
      return it == env.end() ? nullptr : it->second.c_str();
    };
    auto paths = SocketCandidates(get);
    CHECK(paths.size() == 30 && paths[0] == "/var/tmp/discord-ipc-0");
    CHECK(paths[10] == "/var/tmp/app/com.discordapp.Discord/discord-ipc-0");
    CHECK(paths[29] == "/var/tmp/snap.discord/discord-ipc-9");
    env["XDG_RUNTIME_DIR"] = "/run/user/1000";
    CHECK(SocketCandidates(get)[0] == "/run/user/1000/discord-ipc-0");
    env.clear();
    CHECK(SocketCandidates(get)[0] == "/tmp/discord-ipc-0");
  }
#endif

  std::puts("discord tests passed");
  return 0;
}
