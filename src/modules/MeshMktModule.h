#pragma once

#include "SinglePortModule.h"
#include <Arduino.h>
#include <unordered_map>

class MeshMktModule : public SinglePortModule
{
public:
    MeshMktModule();

protected:
    // NOTE: do NOT mark override unless base class actually declares it virtual
    bool wantsPacket(const meshtastic_MeshPacket *p);
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

private:
    // ---- Top-level routing ----
    void handleCommand(uint32_t from, uint8_t channel, const String &msg, bool isDM);

    // ---- Command detection + help ----
    bool isMeshMktCommand(const String &msg) const;
    String helpTextShort() const;
    void cmdHelp(uint32_t to, uint8_t channel);

    // ---- Admin session ----
    void cmdAdmin(uint32_t from, uint8_t channel, const String &args);
    void cmdLogout(uint32_t from, uint8_t channel);
    bool isAdminSessionValid(uint32_t from);
    void openAdminSession(uint32_t from);
    void closeAdminSession(uint32_t from);

    // ---- Core commands ----
    void cmdFind(uint32_t to, uint8_t channel, const String &args);
    void cmdAdd(uint32_t from, uint8_t channel, const String &args);

    // ---- Storage ----
    bool ensureFS();
    String listingsPath() const;
    String genCode8() const;

    // ---- KV parsing: key:value pairs ----
    bool kvGet(const String &args, const String &key, String &out) const;
    int kvGetInt(const String &args, const String &key, int defaultVal) const;

    // ---- Send helper ----
    void sendDM(uint32_t to, uint8_t channel, const String &text, bool wantAck = false);
    void sendDMsmart(uint32_t to, uint8_t channel, const String &text, boolean wantAck = false);

    // ---- DM detection ----
    bool isDirectToMe(const meshtastic_MeshPacket &mp) const;

private:
    std::unordered_map<uint32_t, uint32_t> adminExpiryMs;

    static constexpr uint32_t ADMIN_SESSION_MS = 10UL * 60UL * 1000UL; // 10 minutes

    static constexpr uint8_t MESH_MKT_CHANNEL_INDEX = 1;
    static constexpr bool REQUIRE_MARKET_CHANNEL_FOR_BROADCAST = true;
    static constexpr bool ALLOW_DMS_ANY_CHANNEL = true;
};