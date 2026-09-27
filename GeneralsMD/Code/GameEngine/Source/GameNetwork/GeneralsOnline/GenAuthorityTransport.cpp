#include "PreRTS.h"
#include "Common/crc.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/GeneralsOnline/GenAuthorityTransport.h"
#include "GameNetwork/GeneralsOnline/NGMPGame.h"
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
constexpr uint32_t kMagic = 0x414e4547u;
constexpr uint16_t kVersion = 1;
constexpr size_t kHeader = 20;
enum : uint8_t { kRegister = 1, kData = 2 };
enum : uint8_t { kClient = 0, kAuthority = 1 };

void put16(std::vector<uint8_t>& b, uint16_t v){ b.push_back(v&0xff); b.push_back((v>>8)&0xff); }
void put32(std::vector<uint8_t>& b, uint32_t v){ for(int i=0;i<4;++i)b.push_back((v>>(8*i))&0xff); }
void put64(std::vector<uint8_t>& b, uint64_t v){ for(int i=0;i<8;++i)b.push_back((v>>(8*i))&0xff); }
uint16_t get16(const uint8_t* p){ return uint16_t(p[0]) | (uint16_t(p[1])<<8); }
uint32_t get32(const uint8_t* p){ uint32_t v=0; for(int i=0;i<4;++i)v|=uint32_t(p[i])<<(8*i); return v; }
uint64_t get64(const uint8_t* p){ uint64_t v=0; for(int i=0;i<8;++i)v|=uint64_t(p[i])<<(8*i); return v; }

std::vector<uint8_t> frame(uint8_t kind, uint8_t role, uint64_t match, uint8_t src, uint8_t dst,
                           const uint8_t* payload, size_t len)
{
    std::vector<uint8_t> b; b.reserve(kHeader+len);
    put32(b,kMagic); put16(b,kVersion); b.push_back(kind); b.push_back(role);
    put64(b,match); b.push_back(src); b.push_back(dst); put16(b,static_cast<uint16_t>(len));
    if(payload && len) b.insert(b.end(),payload,payload+len);
    return b;
}
}

GenAuthorityTransport::GenAuthorityTransport() = default;
GenAuthorityTransport::~GenAuthorityTransport(){ reset(); }

Bool GenAuthorityTransport::init(AsciiString, UnsignedShort port){ return init(0, port); }

Bool GenAuthorityTransport::init(UnsignedInt, UnsignedShort port)
{
    const char* host = std::getenv("GEN_AUTHORITY_HOST");
    if (!host || !*host) return FALSE;
    m_serverIP = ResolveIP(AsciiString(host));
    const char* portEnv = std::getenv("GEN_AUTHORITY_PORT");
    if (portEnv && *portEnv) m_serverPort = static_cast<UnsignedShort>(std::atoi(portEnv));
    const char* role = std::getenv("GEN_AUTHORITY_ROLE");
    m_authority = (role && stricmp(role, "authority") == 0) ? TRUE : FALSE;

    // Bind locally. Clients may share the historical 8888 default on different hosts;
    // authority can override with GEN_AUTHORITY_LOCAL_PORT.
    UnsignedShort localPort = port;
    const char* localPortEnv = std::getenv("GEN_AUTHORITY_LOCAL_PORT");
    if (localPortEnv && *localPortEnv) localPort = static_cast<UnsignedShort>(std::atoi(localPortEnv));
    if (!Transport::init(0, localPort)) return FALSE;
    return refreshIdentity() && sendRegistration();
}

void GenAuthorityTransport::reset()
{
    m_registered = FALSE;
    Transport::reset();
}

Bool GenAuthorityTransport::refreshIdentity()
{
    const char* match = std::getenv("GEN_AUTHORITY_MATCH_ID");
    if (match && *match) m_matchID = static_cast<uint64_t>(std::strtoull(match, nullptr, 10));
    else if (TheNGMPGame) m_matchID = static_cast<uint64_t>(TheNGMPGame->getGameID());

    const char* slot = std::getenv("GEN_AUTHORITY_SLOT");
    if (slot && *slot) m_localSlot = static_cast<UnsignedByte>(std::atoi(slot));
    else if (TheNGMPGame) {
        Int s = TheNGMPGame->getLocalSlotNum();
        if (s >= 0 && s < MAX_SLOTS) m_localSlot = static_cast<UnsignedByte>(s);
    }
    return m_serverIP != 0 && m_serverPort != 0 && m_matchID != 0 && m_localSlot < MAX_SLOTS;
}

Bool GenAuthorityTransport::sendRegistration()
{
    if (!m_udpsock || !refreshIdentity()) return FALSE;
    auto b = frame(kRegister, m_authority ? kAuthority : kClient, m_matchID, m_localSlot, 0xff, nullptr, 0);
    const Int n = m_udpsock->Write(b.data(), static_cast<UnsignedInt>(b.size()), m_serverIP, m_serverPort);
    m_registered = n == static_cast<Int>(b.size());
    if (m_registered) {
        fprintf(stderr, "[GEN-AUTH] registered role=%s match=%llu slot=%u relay=%d.%d.%d.%d:%u\n",
            m_authority ? "authority" : "client", static_cast<unsigned long long>(m_matchID),
            static_cast<unsigned>(m_localSlot), PRINTF_IP_AS_4_INTS(m_serverIP), m_serverPort);
        fflush(stderr);
    }
    return m_registered;
}

Bool GenAuthorityTransport::update()
{
    if (!m_registered) sendRegistration();
    Bool ok = TRUE;
    if (!doRecv()) ok = FALSE;
    if (!doSend()) ok = FALSE;
    return ok;
}

Bool GenAuthorityTransport::doRecv()
{
    if (!m_udpsock) return FALSE;
    sockaddr_in from{};
    std::vector<uint8_t> raw(65536);
    Int n = 0;
    while ((n = m_udpsock->Read(raw.data(), static_cast<UnsignedInt>(raw.size()), &from)) > 0) {
        if (static_cast<size_t>(n) < kHeader || get32(raw.data()) != kMagic || get16(raw.data()+4) != kVersion)
            continue;
        const uint8_t kind=raw[6], role=raw[7], src=raw[16], dst=raw[17];
        const uint64_t match=get64(raw.data()+8);
        const uint16_t payloadLen=get16(raw.data()+18);
        if (kind != kData || match != m_matchID || dst != m_localSlot ||
            static_cast<size_t>(n) != kHeader + payloadLen || payloadLen <= sizeof(TransportMessageHeader) ||
            payloadLen > MAX_NETWORK_MESSAGE_LEN)
            continue;
        // Clients only accept authority output; authority only accepts client input.
        if ((!m_authority && role != kAuthority) || (m_authority && role != kClient)) continue;

        TransportMessage incoming{};
        std::memcpy(&incoming, raw.data()+kHeader, payloadLen);
        incoming.length = payloadLen - sizeof(TransportMessageHeader);
        if (!isGeneralsPacket(&incoming)) continue;

        for (Int i=0;i<MAX_MESSAGES;++i) {
            if (m_inBuffer[i].length > 0) continue;
            std::memcpy(&m_inBuffer[i], &incoming, payloadLen);
            m_inBuffer[i].length = incoming.length;
            m_inBuffer[i].addr = src; // ConnectionManager addresses are slot IDs in GEN mode.
            m_inBuffer[i].port = 8888;
            break;
        }
    }
    return n != -1;
}

Bool GenAuthorityTransport::doSend()
{
    if (!m_udpsock || !refreshIdentity()) return FALSE;
    Bool ok = TRUE;
    for (Int i=0;i<MAX_MESSAGES;++i) {
        if (m_outBuffer[i].length <= 0) continue;
        const size_t innerLen = static_cast<size_t>(m_outBuffer[i].length) + sizeof(TransportMessageHeader);
        auto b = frame(kData, m_authority ? kAuthority : kClient, m_matchID, m_localSlot,
                       static_cast<uint8_t>(m_outBuffer[i].addr),
                       reinterpret_cast<const uint8_t*>(&m_outBuffer[i]), innerLen);
        const Int n = m_udpsock->Write(b.data(), static_cast<UnsignedInt>(b.size()), m_serverIP, m_serverPort);
        if (n == static_cast<Int>(b.size())) m_outBuffer[i].length = 0;
        else ok = FALSE;
    }
    return ok;
}

Bool GenAuthorityTransport::queueSend(UnsignedInt addr, UnsignedShort port, const UnsignedByte* buf, Int len)
{
    if (!buf || len < 1 || len > MAX_PACKET_SIZE || addr >= MAX_SLOTS) return FALSE;
    for (Int i=0;i<MAX_MESSAGES;++i) {
        if (m_outBuffer[i].length > 0) continue;
        std::memset(&m_outBuffer[i],0,sizeof(m_outBuffer[i]));
        m_outBuffer[i].length=len; m_outBuffer[i].addr=addr; m_outBuffer[i].port=port;
        std::memcpy(m_outBuffer[i].data,buf,static_cast<size_t>(len));
        m_outBuffer[i].header.magic=GENERALS_MAGIC_NUMBER;
        CRC crc;
        crc.computeCRC(reinterpret_cast<unsigned char*>(&m_outBuffer[i].header.magic),
                       len + sizeof(TransportMessageHeader) - sizeof(UnsignedInt));
        m_outBuffer[i].header.crc=crc.get();
        return TRUE;
    }
    return FALSE;
}
