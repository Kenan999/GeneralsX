#pragma once
#include "GameNetwork/Transport.h"
#include <cstdint>

class GenAuthorityTransport : public Transport
{
public:
    GenAuthorityTransport();
    ~GenAuthorityTransport() override;

    Bool init(AsciiString ip, UnsignedShort port) override;
    Bool init(UnsignedInt ip, UnsignedShort port) override;
    void reset() override;
    Bool update() override;
    Bool doRecv() override;
    Bool doSend() override;
    Bool queueSend(UnsignedInt addr, UnsignedShort port, const UnsignedByte* buf, Int len) override;

private:
    Bool refreshIdentity();
    Bool sendRegistration();

    UnsignedInt m_serverIP = 0;
    UnsignedShort m_serverPort = 47000;
    uint64_t m_matchID = 0;
    UnsignedByte m_localSlot = 0xff;
    Bool m_authority = FALSE;
    Bool m_registered = FALSE;
};
