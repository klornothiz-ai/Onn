// ============================================================================
// ProsperoLayer RDNA2 Core - real POSIX sockets test (round 28, HLE layer)
// ----------------------------------------------------------------------------
// Drives the libSceNet socket path end-to-end over the REAL host stack (the
// round-28 POSIX branches in libs/network.cpp -- previously every socket op
// was Win32-only and returned ENOSYS on Linux):
//
//   * a full loopback TCP lifecycle through the GUEST-FACING wrappers
//     (LibNet::NetSocket/Bind/Listen/Accept/Connect/Send/Recv/...): real
//     bytes over a real kernel socket pair;
//   * guest <-> host sockaddr conversion round-trips (family/port/addr);
//   * the FreeBSD-numbered errno translation (ECONNREFUSED on a refused
//     port, EBADF on a closed descriptor);
//   * byte-order helpers and MAC string parse/format round-trip;
//   * NID registration: every new symbol (sceNetConnect/Send/Recv/...) is
//     resolvable through the SymbolDatabase (the guest dlsym path).
// ============================================================================
#include "libs/network.h"
#include "libs/ps_errno.h"
#include "loader/symbolDatabase.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// Complete the forward-declared ether-address type (the definition in
// libs/network.cpp is not exported through the header; the layout is the
// 6-byte IEEE 802 MAC).
namespace Libs::Network::Net {
struct NetEtherAddr {
    uint8_t data[6] = {0};
};
} // namespace Libs::Network::Net

// The guest-facing libSceNet surface (defined in libs/libNet.cpp; the
// declarations mirror the definitions exactly).
namespace Libs::LibNet {
int KYTY_SYSV_ABI NetSocket(const char* name, int family, int type, int protocol);
int KYTY_SYSV_ABI NetBind(int s, const void* addr, uint32_t addrlen);
int KYTY_SYSV_ABI NetListen(int s, int backlog);
int KYTY_SYSV_ABI NetAccept(int s, void* addr, uint32_t* addrlen);
int KYTY_SYSV_ABI NetConnect(int s, const void* addr, uint32_t addrlen);
int64_t KYTY_SYSV_ABI NetSend(int s, const void* buf, uint64_t len, int flags);
int64_t KYTY_SYSV_ABI NetRecv(int s, void* buf, uint64_t len, int flags);
int KYTY_SYSV_ABI NetGetsockname(int s, void* addr, uint32_t* addrlen);
int KYTY_SYSV_ABI NetGetpeername(int s, void* addr, uint32_t* addrlen);
int KYTY_SYSV_ABI NetIoctl(int s, uint32_t request, void* arg);
int KYTY_SYSV_ABI NetShutdown(int s, int how);
int KYTY_SYSV_ABI NetSocketClose(int s);
int KYTY_SYSV_ABI NetSocketAbort(int s);
uint32_t KYTY_SYSV_ABI NetHtonl(uint32_t host32);
uint16_t KYTY_SYSV_ABI NetHtons(uint16_t host16);
uint64_t KYTY_SYSV_ABI NetHtonll(uint64_t host64);
int KYTY_SYSV_ABI NetEtherStrton(const char* str, Network::Net::NetEtherAddr* n);
KYTY_SYSV_ABI int* GetNetErrorAddr();
void InitNet_1_Net(Loader::SymbolDatabase* s);
} // namespace Libs::LibNet


// The aggregate InitNet_1 in libNet.cpp references sibling registrars that
// live in other translation units; this test only exercises the Net module,
// so the remaining symbols get empty definitions (never called).
namespace Libs::LibJson2 { void InitNet_1_Json2(Loader::SymbolDatabase*) {} }

namespace {
using namespace Libs;
int g_failures = 0, g_checks = 0;
bool Check(bool v, const char* e, int line) {
    ++g_checks;
    if (!v) { ++g_failures; std::cerr << "  [FAIL] line " << line << ": " << e << '\n'; }
    return v;
}
#define CHECK(e) Check((e), #e, __LINE__)

namespace Net = Network::Net;

// Guest (PS4/FreeBSD) sockaddr_in: len, family, port(BE), addr(BE).
#pragma pack(push, 1)
struct GuestSockaddrIn {
    uint8_t  len;
    uint8_t  family;      // 2 = AF_INET
    uint16_t port;        // big-endian
    uint32_t addr;        // big-endian
    uint8_t  zero[8];
};
#pragma pack(pop)
static_assert(sizeof(GuestSockaddrIn) == 16);

struct GuestTimeval {
    int64_t sec;
    int64_t usec;
};
} // namespace

int main() {
    std::printf("[net-sock] A: NID registration (the dlsym path)\n");
    {
        Loader::SymbolDatabase db;
        LibNet::InitNet_1_Net(&db);
        // The round-28 data path, NIDs verified against the public shadPS4
        // libSceNet registration table.
        CHECK(db.FindSymbol("OXXX4mUk3uk") != nullptr);   // sceNetConnect
        CHECK(db.FindSymbol("beRjXBn-z+o") != nullptr);   // sceNetSend
        CHECK(db.FindSymbol("gvD1greCu0A") != nullptr);   // sceNetSendto
        CHECK(db.FindSymbol("2eKbgcboJso") != nullptr);   // sceNetSendmsg
        CHECK(db.FindSymbol("9wO9XrMsNhc") != nullptr);   // sceNetRecv
        CHECK(db.FindSymbol("304ooNZxWDY") != nullptr);   // sceNetRecvfrom
        CHECK(db.FindSymbol("wvuUDv0jrMI") != nullptr);   // sceNetRecvmsg
        CHECK(db.FindSymbol("xphrZusl78E") != nullptr);   // sceNetGetsockopt
        CHECK(db.FindSymbol("TCkRD0DWNLg") != nullptr);   // sceNetGetpeername
        CHECK(db.FindSymbol("zJGf8xjFnQE") != nullptr);   // sceNetSocketAbort
        CHECK(db.FindSymbol("ghqRRVQxqKo") != nullptr);   // sceNetIoctl
        CHECK(db.FindSymbol("cTGkc6-TBlI") != nullptr);   // sceNetTerm
        CHECK(db.FindSymbol("3CHi1K1wsCQ") != nullptr);   // sceNetHtonll
        CHECK(db.FindSymbol("tOrRi-v3AOM") != nullptr);   // sceNetNtohll
        CHECK(db.FindSymbol("b-bFZvNV59I") != nullptr);   // sceNetEtherStrton
        // The pre-existing surface stays registered.
        CHECK(db.FindSymbol("Q4qBuN-c0ZM") != nullptr);   // sceNetSocket
        CHECK(db.FindSymbol("kOj1HiAGE54") != nullptr);   // sceNetListen
        CHECK(db.FindSymbol("PIWqhn9oSxc") != nullptr);   // sceNetAccept
        CHECK(db.FindSymbol("bErx49PgxyY") != nullptr);   // sceNetBind
    }

    std::printf("[net-sock] B: byte order + MAC helpers\n");
    {
        CHECK(Net::NetInit() == 0);
        CHECK(LibNet::NetHtonl(0x11223344u) == 0x44332211u);
        CHECK(LibNet::NetHtons(0x1122u) == 0x2211u);
        CHECK(LibNet::NetHtonll(0x1122334455667788ull) == 0x8877665544332211ull);

        Network::Net::NetEtherAddr mac{};
        const std::string macstr = "01:23:45:67:89:ab";
        CHECK(LibNet::NetEtherStrton(macstr.c_str(), &mac) == 0);
        CHECK(mac.data[0] == 0x01 && mac.data[1] == 0x23 && mac.data[5] == 0xab);
        char buf[18] = {};
        CHECK(Net::NetEtherNtostrReal(&mac, buf, sizeof(buf)) == 0);
        CHECK(std::string(buf) == macstr);
        CHECK(LibNet::NetEtherStrton("not-a-mac", &mac) != 0);
    }

    std::printf("[net-sock] C: loopback TCP lifecycle (real kernel sockets)\n");
    {
        // listener: bind to 127.0.0.1:0 -> the kernel picks a free port
        const int lsock = LibNet::NetSocket("test-listener", 2, 1, 0);
        CHECK(lsock >= 0);
        GuestSockaddrIn addr{};
        addr.len    = 16;
        addr.family = 2;
        addr.port   = 0;
        addr.addr   = 0x0100007fu;          // 127.0.0.1 in network order
        CHECK(LibNet::NetBind(lsock, &addr, sizeof(addr)) == 0);

        GuestSockaddrIn got{};
        uint32_t gotlen = sizeof(got);
        CHECK(LibNet::NetGetsockname(lsock, &got, &gotlen) == 0);
        CHECK(got.family == 2);
        CHECK(got.addr == 0x0100007fu);
        const uint16_t port = got.port;
        CHECK(port != 0);

        CHECK(LibNet::NetListen(lsock, 1) == 0);

        int accepted = -1;
        std::thread server([&]() {
            GuestSockaddrIn peer{};
            uint32_t plen = sizeof(peer);
            accepted = LibNet::NetAccept(lsock, &peer, &plen);
        });

        // client: connect + send + recv
        const int csock = LibNet::NetSocket("test-client", 2, 1, 0);
        CHECK(csock >= 0);
        GuestSockaddrIn caddr = addr;
        caddr.port = port;
        CHECK(LibNet::NetConnect(csock, &caddr, sizeof(caddr)) == 0);

        const char msg[] = "prospero-round28";
        CHECK(LibNet::NetSend(csock, msg, sizeof(msg), 0) ==
              static_cast<int64_t>(sizeof(msg)));

        server.join();
        CHECK(accepted >= 0);

        // the peer name from the client side is the listener's port
        GuestSockaddrIn peer{};
        uint32_t plen = sizeof(peer);
        CHECK(LibNet::NetGetpeername(csock, &peer, &plen) == 0);
        CHECK(peer.family == 2);
        CHECK(peer.addr == 0x0100007fu);
        CHECK(peer.port == port);

        // echo round-trip
        char buf[64] = {};
        CHECK(LibNet::NetRecv(accepted, buf, sizeof(buf), 0) ==
              static_cast<int64_t>(sizeof(msg)));
        CHECK(std::string(buf) == msg);
        CHECK(LibNet::NetSend(accepted, "ack", 4, 0) == 4);
        char cbuf[16] = {};
        CHECK(LibNet::NetRecv(csock, cbuf, sizeof(cbuf), 0) == 4);
        CHECK(std::string(cbuf) == "ack");

        // select: writable client (1), drained readable (0). The guest
        // fd_set is a 1024-bit bitmap (16 x uint64 words) -- sockets live
        // at descriptor numbers >= 128.
        uint64_t wset[16] = {};
        wset[csock / 64] |= (1ull << (csock % 64));
        GuestTimeval tv{0, 0};
        CHECK(Net::Select(csock + 1, nullptr, wset, nullptr, &tv) == 1);
        uint64_t rset[16] = {};
        CHECK(Net::Select(csock + 1, rset, nullptr, nullptr, &tv) == 0);

        // ioctl FIONREAD on the drained socket: 0 bytes pending
        int pending = -1;
        CHECK(LibNet::NetIoctl(csock, 0x4004667fu, &pending) == 0);
        CHECK(pending == 0);

        // abort is accepted (shutdown-based semantics)
        CHECK(LibNet::NetSocketAbort(csock) == 0);

        // shutdown + close, then a recv on the dead descriptor reports EBADF
        CHECK(LibNet::NetShutdown(csock, 2) == 0);
        CHECK(LibNet::NetSocketClose(csock) == 0);
        char dead[8] = {};
        CHECK(LibNet::NetRecv(csock, dead, sizeof(dead), 0) < 0);
        CHECK(*LibNet::GetNetErrorAddr() == Network::NET_ERROR_EBADF);
        CHECK(LibNet::NetSocketClose(accepted) == 0);
        CHECK(LibNet::NetSocketClose(lsock) == 0);
    }

    std::printf("[net-sock] D: errno translation (FreeBSD numbering)\n");
    {
        // bind a probe socket, read its port, close it -> connect there
        // hits a refused port -> ECONNREFUSED in FreeBSD numbering.
        const int probe = LibNet::NetSocket("probe", 2, 1, 0);
        CHECK(probe >= 0);
        GuestSockaddrIn a{};
        a.len = 16; a.family = 2; a.port = 0; a.addr = 0x0100007fu;
        CHECK(LibNet::NetBind(probe, &a, sizeof(a)) == 0);
        GuestSockaddrIn name{};
        uint32_t nlen = sizeof(name);
        CHECK(LibNet::NetGetsockname(probe, &name, &nlen) == 0);
        CHECK(LibNet::NetSocketClose(probe) == 0);

        const int s = LibNet::NetSocket("refused", 2, 1, 0);
        CHECK(s >= 0);
        const int cr = LibNet::NetConnect(s, &name, sizeof(name));
        CHECK(cr != 0);                       // refused
        CHECK(*LibNet::GetNetErrorAddr() ==
              Network::NET_ERROR_ECONNREFUSED);
        CHECK(LibNet::NetSocketClose(s) == 0);
    }

    std::printf("[net-sock] %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
