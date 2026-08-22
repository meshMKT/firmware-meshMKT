#include "MeshMktModule.h"

#include "configuration.h"
#include "MeshService.h"
#include "main.h"

#include <FS.h>
#include <vector>

#if defined(ARDUINO_ARCH_ESP32)
#include <LittleFS.h>
#endif

// -------------------------
// Logging helpers
// -------------------------
#if defined(DEBUG_PORT) && !defined(DEBUG_MUTE)
#define MKT_I(...) LOG_INFO(__VA_ARGS__)
#define MKT_D(...) LOG_DEBUG(__VA_ARGS__)
#define MKT_W(...) LOG_WARN(__VA_ARGS__)
#else
#define MKT_I(...)
#define MKT_D(...)
#define MKT_W(...)
#endif

// Toggle noisy packet-level logs
static constexpr bool MESH_MKT_VERBOSE = true;

// TODO: later hash + store in prefs.
// For now simple passphrase for bootstrapping.
static const char *MESHMKT_ADMIN_PASS = "1234";

// -------------------------
// Small helpers
// -------------------------
static inline bool equalsIgnoreCase(const String &a, const char *b)
{
    return a.equalsIgnoreCase(b);
}

static inline String upperCopy(const String &s)
{
    String out = s;
    out.toUpperCase();
    return out;
}

static bool splitPipeFields(const String &line, std::vector<String> &out)
{
    out.clear();
    int start = 0;
    while (true)
    {
        int p = line.indexOf('|', start);
        if (p < 0)
        {
            out.push_back(line.substring(start));
            break;
        }
        out.push_back(line.substring(start, p));
        start = p + 1;
    }
    // Expect at least: code|item|desc|cat|qty|price|keywords|seller|ts
    return out.size() >= 4;
}

static bool containsIgnoreCase(const String &hay, const String &needleUpper)
{
    String h = hay;
    h.toUpperCase();
    return h.indexOf(needleUpper) >= 0;
}

// "FREE" heuristics
static bool isFreePrice(const String &priceField)
{
    String p = priceField;
    p.trim();
    if (p.length() == 0)
        return true;

    String u = p;
    u.toUpperCase();

    if (u == "FREE")
        return true;

    // numeric zero-ish
    if (u == "0" || u == "0.0" || u == "0.00" || u == "$0" || u == "$0.00")
        return true;

    return false;
}

MeshMktModule::MeshMktModule()
    : SinglePortModule("meshmkt", meshtastic_PortNum_TEXT_MESSAGE_APP)
{
    MKT_I("[MeshMkt] ctor: module created. MarketCh=%u requireMarketCh=%u allowDMAnyCh=%u",
          (unsigned)MESH_MKT_CHANNEL_INDEX,
          (unsigned)REQUIRE_MARKET_CHANNEL_FOR_BROADCAST,
          (unsigned)ALLOW_DMS_ANY_CHANNEL);
}

bool MeshMktModule::wantsPacket(const meshtastic_MeshPacket *p)
{
    if (!p)
        return false;

    // Debug-friendly selection: accept any decoded payload on TEXT port.
    // (You can tighten later.)
    const bool ok = (p->decoded.payload.size > 0);

    if (MESH_MKT_VERBOSE)
    {
        MKT_D("[MeshMkt] wantsPacket=%u from=0x%08lx to=0x%08lx ch=%u port=%u len=%u",
              (unsigned)ok,
              (unsigned long)p->from,
              (unsigned long)p->to,
              (unsigned)p->channel,
              (unsigned)p->decoded.portnum,
              (unsigned)p->decoded.payload.size);
    }

    return ok;
}

bool MeshMktModule::isDirectToMe(const meshtastic_MeshPacket &mp) const
{
    // broadcast is typically 0xffffffff
    return (mp.to != 0xffffffff && mp.to != 0x0);
}

ProcessMessage MeshMktModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    if (MESH_MKT_VERBOSE)
    {
        MKT_I("[MeshMkt] RX: from=0x%08lx to=0x%08lx ch=%u port=%u len=%u wantAck=%u",
              (unsigned long)mp.from,
              (unsigned long)mp.to,
              (unsigned)mp.channel,
              (unsigned)mp.decoded.portnum,
              (unsigned)mp.decoded.payload.size,
              (unsigned)mp.want_ack);
    }

    if (!mp.decoded.payload.bytes || mp.decoded.payload.size == 0)
    {
        MKT_D("[MeshMkt] RX: empty payload -> CONTINUE");
        return ProcessMessage::CONTINUE;
    }

    // Build message string safely
    String msg;
    msg.reserve(mp.decoded.payload.size + 1);
    for (uint32_t i = 0; i < mp.decoded.payload.size; i++)
    {
        msg += (char)mp.decoded.payload.bytes[i];
    }
    msg.trim();

    if (MESH_MKT_VERBOSE)
    {
        MKT_I("[MeshMkt] RX msg='%s'", msg.c_str());
    }

    if (msg.length() == 0)
    {
        MKT_D("[MeshMkt] RX: blank msg after trim -> CONTINUE");
        return ProcessMessage::CONTINUE;
    }

    const uint32_t from = mp.from;
    const uint8_t ch = mp.channel;
    const bool isDM = isDirectToMe(mp);

    MKT_D("[MeshMkt] classify: isDM=%u rxChannel=%u marketChannel=%u",
          (unsigned)isDM, (unsigned)ch, (unsigned)MESH_MKT_CHANNEL_INDEX);

    // 1) Not a DM: only listen on market channel
    if (!isDM)
    {
        if (REQUIRE_MARKET_CHANNEL_FOR_BROADCAST && ch != MESH_MKT_CHANNEL_INDEX)
        {
            MKT_D("[MeshMkt] broadcast on non-market channel -> IGNORE (CONTINUE)");
            return ProcessMessage::CONTINUE;
        }

        // On MeshMkt channel: if not our command, DM HELP
        if (!isMeshMktCommand(msg))
        {
            MKT_I("[MeshMkt] broadcast on market channel but not a meshMKT cmd -> DM HELP");
            sendDMsmart(from, ch, helpTextShort());
            return ProcessMessage::STOP;
        }

        MKT_I("[MeshMkt] broadcast meshMKT command -> handleCommand() then DM reply");
        handleCommand(from, ch, msg, /*isDM=*/false);
        return ProcessMessage::STOP;
    }

    // 2) DM: process regardless of channel (configurable)
    if (!ALLOW_DMS_ANY_CHANNEL && ch != MESH_MKT_CHANNEL_INDEX)
    {
        MKT_W("[MeshMkt] DM received on non-market channel (policy blocks) -> DM notice");
        sendDM(from, ch, "Please DM me on the MeshMkt channel.");
        return ProcessMessage::STOP;
    }

    MKT_I("[MeshMkt] DM command -> handleCommand()");
    handleCommand(from, ch, msg, /*isDM=*/true);
    return ProcessMessage::STOP;
}

bool MeshMktModule::isMeshMktCommand(const String &msg) const
{
    int sp = msg.indexOf(' ');
    String cmd = (sp >= 0) ? msg.substring(0, sp) : msg;
    cmd.trim();
    cmd.toUpperCase();

    const bool isCmd =
        (cmd == "HELP" ||
         cmd == "FIND" ||
         cmd == "ADD" ||
         cmd == "ADMIN" ||
         cmd == "LOGOUT" ||
         cmd == "WHOAMI");
    MKT_D("[MeshMkt] isMeshMktCommand('%s')=%u", cmd.c_str(), (unsigned)isCmd);
    return isCmd;
}

String MeshMktModule::helpTextShort() const
{
    String t;
    t += "meshMKT commands:\n";
    t += "HELP\n";
    t += "FIND MKT   (entrypoint from channel)\n";
    t += "FIND CAT   (list categories)\n";
    t += "FIND FREE  (free items)\n";
    t += "FIND <keyword|CODE8>\n";
    t += "ADMIN <pw>\n";
    t += "LOGOUT\n";
    t += "WHOAMI   (DM only)\n";
    t += "ADD item:<name> cat:<cat> qty:<#> desc:<desc> price:<amt> keywords:<k1 k2>\n";
    t += "Tip: Send 'FIND MKT' on MeshMkt channel to get this menu in a DM.";
    return t;
}

void MeshMktModule::handleCommand(uint32_t from, uint8_t channel, const String &msg, bool isDM)
{
    // split: first token command, rest args
    int sp = msg.indexOf(' ');
    String command = (sp >= 0) ? msg.substring(0, sp) : msg;
    String args = (sp >= 0) ? msg.substring(sp + 1) : "";
    command.trim();
    args.trim();
    command.toUpperCase();

    MKT_I("[MeshMkt] CMD: from=0x%08lx ch=%u isDM=%u cmd='%s' args='%s'",
          (unsigned long)from, (unsigned)channel, (unsigned)isDM, command.c_str(), args.c_str());

    if (command == "HELP")
    {
        cmdHelp(from, channel);
        return;
    }

    if (command == "WHOAMI")
    {
        // DM-only identity/status helper
        if (!isDM)
        {
            sendDMsmart(from, channel, "DM me 'WHOAMI' for your node/session info.", false);
            return;
        }

        bool adminActive = false;
        long ttlSec = 0;
        auto it = adminExpiryMs.find(from);
        if (it != adminExpiryMs.end())
        {
            const uint32_t now = millis();
            if ((int32_t)(it->second - now) > 0)
            {
                adminActive = true;
                ttlSec = (long)((it->second - now) / 1000UL);
            }
            else
            {
                adminExpiryMs.erase(it);
            }
        }

        char fromHex[11];
        snprintf(fromHex, sizeof(fromHex), "%08lx", (unsigned long)from);

        String reply;
        reply.reserve(140);
        reply += "WHOAMI\n";
        reply += "you=0x";
        reply += fromHex;
        reply += "\n";
        reply += "rx_ch=";
        reply += String((unsigned)channel);
        reply += "\n";
        reply += "admin=";
        reply += (adminActive ? "ACTIVE" : "OFF");
        if (adminActive)
        {
            reply += " (";
            reply += String(ttlSec);
            reply += "s left)";
        }

        sendDMsmart(from, channel, reply, false);
        return;
    }

    if (command == "FIND")
    {
        // Discovery entrypoint allowed from channel:
        if (equalsIgnoreCase(args, "MKT"))
        {
            MKT_I("[MeshMkt] FIND MKT -> DM HELP");
            sendDMsmart(from, channel, helpTextShort());
            return;
        }

        // Everything else is DM-only:
        if (!isDM)
        {
            MKT_I("[MeshMkt] FIND (non-MKT) requested on channel -> DM HELP");
            sendDM(from, channel, "DM me to search.\nSend 'FIND MKT' here to get the menu.");
            return;
        }

        cmdFind(from, channel, args);
        return;
    }

    if (command == "ADMIN")
    {
        // DM-only feels safer; but if you want to allow from channel, remove this gate.
        if (!isDM)
        {
            sendDM(from, channel, "Please DM me: ADMIN <pw>");
            return;
        }
        cmdAdmin(from, channel, args);
        return;
    }

    if (command == "LOGOUT")
    {
        if (!isDM)
        {
            sendDM(from, channel, "Please DM me: LOGOUT");
            return;
        }
        cmdLogout(from, channel);
        return;
    }

    if (command == "ADD")
    {
        if (!isDM)
        {
            // From channel, we should not leak admin-state; just push them to DM.
            sendDM(from, channel, "Please DM me to add listings.\nSend: FIND MKT");
            return;
        }
        cmdAdd(from, channel, args);
        return;
    }

    MKT_W("[MeshMkt] Unknown command -> DM HELP");
    sendDMsmart(from, channel, helpTextShort());
}

void MeshMktModule::cmdHelp(uint32_t to, uint8_t channel)
{
    MKT_D("[MeshMkt] cmdHelp -> DM");
    sendDMsmart(to, channel, helpTextShort());
}

void MeshMktModule::cmdAdmin(uint32_t from, uint8_t channel, const String &args)
{
    MKT_D("[MeshMkt] cmdAdmin: argsLen=%u", (unsigned)args.length());

    if (args.length() == 0)
    {
        sendDM(from, channel, "Usage: ADMIN <password>");
        return;
    }
    if (args != MESHMKT_ADMIN_PASS)
    {
        MKT_W("[MeshMkt] cmdAdmin: invalid password from=0x%08lx", (unsigned long)from);
        sendDM(from, channel, "❌ Invalid admin password.");
        return;
    }

    openAdminSession(from);
    MKT_I("[MeshMkt] cmdAdmin: session opened for=0x%08lx", (unsigned long)from);
    sendDM(from, channel, "🔐 Admin session active (10 min).");
}

void MeshMktModule::cmdLogout(uint32_t from, uint8_t channel)
{
    closeAdminSession(from);
    MKT_I("[MeshMkt] cmdLogout: session cleared for=0x%08lx", (unsigned long)from);
    sendDM(from, channel, "🔓 Logged out.");
}

bool MeshMktModule::isAdminSessionValid(uint32_t from)
{
    auto it = adminExpiryMs.find(from);
    if (it == adminExpiryMs.end())
    {
        MKT_D("[MeshMkt] adminSession: none for=0x%08lx", (unsigned long)from);
        return false;
    }

    const uint32_t now = millis();
    if ((int32_t)(it->second - now) <= 0)
    {
        MKT_W("[MeshMkt] adminSession: expired for=0x%08lx", (unsigned long)from);
        adminExpiryMs.erase(it);
        return false;
    }

    MKT_D("[MeshMkt] adminSession: valid for=0x%08lx ttlMs=%ld",
          (unsigned long)from, (long)(it->second - now));
    return true;
}

void MeshMktModule::openAdminSession(uint32_t from)
{
    adminExpiryMs[from] = millis() + ADMIN_SESSION_MS;
}

void MeshMktModule::closeAdminSession(uint32_t from)
{
    adminExpiryMs.erase(from);
}

bool MeshMktModule::ensureFS()
{
#if defined(ARDUINO_ARCH_ESP32)
    static bool mounted = false;
    if (mounted)
        return true;

    MKT_I("[MeshMkt] FS: mounting LittleFS...");
    if (!LittleFS.begin(true))
    {
        MKT_W("[MeshMkt] FS: LittleFS mount FAILED");
        return false;
    }
    mounted = true;
    MKT_I("[MeshMkt] FS: LittleFS mounted OK");
    return true;
#else
    MKT_W("[MeshMkt] FS: not supported on this arch");
    return false;
#endif
}

String MeshMktModule::listingsPath() const
{
    return "/meshmkt_listings.txt";
}

String MeshMktModule::genCode8() const
{
    static const char *alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
#if defined(ARDUINO_ARCH_ESP32)
    uint32_t r = esp_random();
#else
    uint32_t r = (uint32_t)millis();
#endif
    String out;
    out.reserve(8);
    for (int i = 0; i < 8; i++)
    {
        r ^= (r << 13);
        r ^= (r >> 17);
        r ^= (r << 5);
        out += alphabet[r % 32];
    }
    return out;
}

// kvGet parses "key:value" where value runs until next " <word>:"
bool MeshMktModule::kvGet(const String &args, const String &key, String &out) const
{
    String k = key;
    k.toLowerCase();
    int i = 0;
    const int n = args.length();

    while (i < n)
    {
        while (i < n && args[i] == ' ')
            i++;
        int start = i;
        while (i < n && args[i] != ' ')
            i++;
        String token = args.substring(start, i);

        int colon = token.indexOf(':');
        if (colon > 0)
        {
            String tk = token.substring(0, colon);
            tk.toLowerCase();

            if (tk == k)
            {
                String val = token.substring(colon + 1);

                int j = i;
                while (j < n)
                {
                    int save = j;
                    while (j < n && args[j] == ' ')
                        j++;
                    int ts = j;
                    while (j < n && args[j] != ' ')
                        j++;
                    String t2 = args.substring(ts, j);

                    int c2 = t2.indexOf(':');
                    if (c2 > 0)
                    {
                        j = save;
                        break;
                    } // next key begins

                    if (t2.length() > 0)
                    {
                        if (val.length() > 0)
                            val += " ";
                        val += t2;
                    }
                }

                out = val;
                out.trim();
                MKT_D("[MeshMkt] kvGet: key='%s' val='%s'", key.c_str(), out.c_str());
                return true;
            }
        }
    }
    return false;
}

int MeshMktModule::kvGetInt(const String &args, const String &key, int defaultVal) const
{
    String s;
    if (!kvGet(args, key, s))
        return defaultVal;
    s.trim();
    if (s.length() == 0)
        return defaultVal;
    return s.toInt();
}

void MeshMktModule::cmdAdd(uint32_t from, uint8_t channel, const String &args)
{
    MKT_I("[MeshMkt] cmdAdd: from=0x%08lx ch=%u", (unsigned long)from, (unsigned)channel);

    if (!isAdminSessionValid(from))
    {
        MKT_W("[MeshMkt] cmdAdd: denied (no admin session)");
        sendDM(from, channel, "Admin auth required. Run: ADMIN <pw>");
        return;
    }

    if (!ensureFS())
    {
        sendDM(from, channel, "❌ FS not available (LittleFS mount failed).");
        return;
    }

    String item, desc, cat, price, keywords;
    (void)kvGet(args, "item", item);
    (void)kvGet(args, "desc", desc);
    (void)kvGet(args, "cat", cat);
    (void)kvGet(args, "price", price);
    (void)kvGet(args, "keywords", keywords);
    int qty = kvGetInt(args, "qty", 1);

    item.trim();
    desc.trim();
    cat.trim();
    price.trim();
    keywords.trim();

    MKT_D("[MeshMkt] cmdAdd fields: item='%s' cat='%s' qty=%d desc='%s' price='%s' keywords='%s'",
          item.c_str(), cat.c_str(), qty, desc.c_str(), price.c_str(), keywords.c_str());

    if (item.length() == 0 || cat.length() == 0)
    {
        sendDM(from, channel, "Usage: ADD item:<name> cat:<cat> qty:<#> desc:<desc> price:<amt> keywords:<k1 k2>");
        return;
    }
    if (qty <= 0)
        qty = 1;

    const String code = genCode8();
    const uint32_t ts = (uint32_t)(millis() / 1000UL);

#if defined(ARDUINO_ARCH_ESP32)
    const String path = listingsPath();
    MKT_I("[MeshMkt] cmdAdd: append file '%s'", path.c_str());

    File f = LittleFS.open(path, FILE_APPEND);
    if (!f)
    {
        MKT_W("[MeshMkt] cmdAdd: open append FAILED");
        sendDM(from, channel, "❌ Could not open listings file for append.");
        return;
    }

    char sellerHex[11];
    snprintf(sellerHex, sizeof(sellerHex), "%08lx", (unsigned long)from);

    String line;
    line.reserve(256);
    line += code;
    line += "|";
    line += item;
    line += "|";
    line += desc;
    line += "|";
    line += cat;
    line += "|";
    line += String(qty);
    line += "|";
    line += price;
    line += "|";
    line += keywords;
    line += "|";
    line += sellerHex;
    line += "|";
    line += String(ts);

    f.println(line);
    f.close();

    MKT_I("[MeshMkt] cmdAdd: wrote listing code=%s", code.c_str());
#endif

    sendDM(from, channel, "✅ Added listing: [" + code + "] " + item);
}

void MeshMktModule::cmdFind(uint32_t to, uint8_t channel, const String &args)
{
    MKT_I("[MeshMkt] cmdFind: to=0x%08lx ch=%u q='%s'",
          (unsigned long)to, (unsigned)channel, args.c_str());

    if (!ensureFS())
    {
        sendDM(to, channel, "❌ FS not available (LittleFS mount failed).");
        return;
    }

    String q = args;
    q.trim();

    if (q.length() == 0)
    {
        sendDM(to, channel, "Usage: FIND <keyword|CODE8>\nAlso: FIND CAT | FIND FREE");
        return;
    }

    // Special modes (DM-only enforced by handleCommand)
    if (equalsIgnoreCase(q, "CAT"))
    {
#if defined(ARDUINO_ARCH_ESP32)
        const String path = listingsPath();
        File f = LittleFS.open(path, FILE_READ);
        if (!f)
        {
            sendDM(to, channel, "📭 No listings yet (no categories).");
            return;
        }

        std::vector<String> cats;
        cats.reserve(16);

        std::vector<String> fields;
        while (f.available())
        {
            String line = f.readStringUntil('\n');
            line.trim();
            if (line.length() == 0)
                continue;

            if (!splitPipeFields(line, fields) || fields.size() < 4)
                continue;

            String cat = fields[3];
            cat.trim();
            if (cat.length() == 0)
                continue;

            bool exists = false;
            for (auto &c : cats)
            {
                if (c.equalsIgnoreCase(cat))
                {
                    exists = true;
                    break;
                }
            }
            if (!exists)
                cats.push_back(cat);

            if (cats.size() >= 25) // cap to keep messages short
                break;
        }
        f.close();

        if (cats.empty())
        {
            sendDM(to, channel, "📭 No categories yet.");
            return;
        }

        String out = "📚 Categories:\n";
        for (size_t i = 0; i < cats.size(); i++)
        {
            out += "- ";
            out += cats[i];
            out += "\n";
            if (out.length() > 180) // keep under packet-ish limits
            {
                sendDM(to, channel, out);
                out = "";
            }
        }
        if (out.length() > 0)
            sendDM(to, channel, out);
        return;
#else
        sendDM(to, channel, "CAT not supported on this build.");
        return;
#endif
    }

    if (equalsIgnoreCase(q, "FREE"))
    {
#if defined(ARDUINO_ARCH_ESP32)
        const String path = listingsPath();
        File f = LittleFS.open(path, FILE_READ);
        if (!f)
        {
            sendDM(to, channel, "📭 No listings file yet.");
            return;
        }

        int found = 0;
        std::vector<String> fields;

        while (f.available())
        {
            String line = f.readStringUntil('\n');
            line.trim();
            if (line.length() == 0)
                continue;

            if (!splitPipeFields(line, fields) || fields.size() < 6)
                continue;

            const String code = fields[0];
            const String item = fields[1];
            const String cat = (fields.size() > 3) ? fields[3] : "";
            const String price = (fields.size() > 5) ? fields[5] : "";

            if (!isFreePrice(price))
                continue;

            String msg = "🆓 [" + code + "] " + item;
            if (cat.length() > 0)
                msg += " (" + cat + ")";
            sendDMsmart(to, channel, msg);
            found++;
            if (found >= 5)
                break;
        }
        f.close();

        if (found == 0)
            sendDM(to, channel, "❌ No free items found.");
        return;
#else
        sendDM(to, channel, "FREE not supported on this build.");
        return;
#endif
    }

    // Normal FIND: keyword or CODE8
    String qU = upperCopy(q);

#if defined(ARDUINO_ARCH_ESP32)
    const String path = listingsPath();
    MKT_D("[MeshMkt] cmdFind: open read '%s'", path.c_str());

    File f = LittleFS.open(path, FILE_READ);
    if (!f)
    {
        MKT_I("[MeshMkt] cmdFind: no listings file yet");
        sendDM(to, channel, "📭 No listings file yet.");
        return;
    }

    int found = 0;
    std::vector<String> fields;

    while (f.available())
    {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0)
            continue;

        if (!splitPipeFields(line, fields) || fields.size() < 2)
            continue;

        const String code = fields[0];

        bool match = false;

        if (qU.length() == 8)
        {
            String cU = upperCopy(code);
            if (cU == qU)
                match = true;
        }

        if (!match)
        {
            if (containsIgnoreCase(line, qU))
                match = true;
        }

        if (match)
        {
            // Keep existing "everything after code" behavior, but label it nicely.
            int p1 = line.indexOf('|');
            String rest = (p1 >= 0) ? line.substring(p1 + 1) : "";
            sendDM(to, channel, "📦 [" + code + "] " + rest);
            found++;
            if (found >= 3)
                break; // keep reply short for now
        }
    }
    f.close();

    if (found == 0)
    {
        sendDM(to, channel, "❌ No items found for '" + q + "'");
    }
    else
    {
        MKT_I("[MeshMkt] cmdFind: found=%d", found);
    }
#endif
}

void MeshMktModule::sendDM(uint32_t to, uint8_t channel, const String &text, bool wantAck)
{
    if (!service)
    {
        MKT_W("[MeshMkt] sendDM: service=null (cannot send)");
        return;
    }

    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p)
    {
        MKT_W("[MeshMkt] sendDM: allocDataPacket failed");
        return;
    }

    p->to = to;
    p->channel = channel;
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p->want_ack = wantAck ? 1 : 0;

    const size_t maxLen = sizeof(p->decoded.payload.bytes);
    size_t n = text.length();
    if (n > maxLen)
        n = maxLen;

    p->decoded.payload.size = n;
    memcpy(p->decoded.payload.bytes, text.c_str(), n);

    MKT_I("[MeshMkt] TX DM: to=0x%08lx ch=%u ack=%u len=%u text='%s'",
          (unsigned long)to,
          (unsigned)channel,
          (unsigned)(wantAck ? 1 : 0),
          (unsigned)n,
          text.c_str());

    service->sendToMesh(p);
}

void MeshMktModule::sendDMsmart(uint32_t to,
                                uint8_t channel,
                                const String &text,
                                bool wantAck)
{
    // Safe payload size to avoid router/phone queue overflow
    static const size_t CHUNK = 180;

    if (text.length() <= CHUNK)
    {
        sendDM(to, channel, text, wantAck);
        return;
    }

    size_t len = text.length();
    for (size_t off = 0; off < len; off += CHUNK)
    {
        String part = text.substring(off, min(off + CHUNK, len));
        sendDM(to, channel, part, wantAck);
        delay(35); // critical to avoid NAK / tophone overflow
    }
}