#include "ebridge/dsu_server.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "ebridge_protocol.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t kBadSocket = INVALID_SOCKET;
static void CloseSocket(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kBadSocket = -1;
static void CloseSocket(socket_t s) { close(s); }
#endif

namespace ebridge {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint16_t kProtocolVersion = 1001;
constexpr uint32_t kClientMagic = 0x43555344;   // "DSUC"
constexpr uint32_t kServerMagic = 0x53555344;   // "DSUS"
constexpr uint32_t kTypeVersion = 0x00100000;
constexpr uint32_t kTypePortInfo = 0x00100001;
constexpr uint32_t kTypePadData = 0x00100002;
// Eglantine re-subscribes every 3 s; drop clients silent for longer than this.
constexpr auto kClientTimeout = std::chrono::seconds(10);
// Eglantine divides the gyro by 312 to get its internal unit (turns per second).
constexpr float kGyroScale = 312.0f;

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint16_t version;
    uint16_t payload_length;   // sizeof(data) + 4 (the type field)
    uint32_t crc;
    uint32_t id;
    uint32_t type;
};
struct PortInfo {
    uint8_t id, state, model, connection_type;
    uint8_t mac[6];
    uint8_t battery, is_active;
};
struct TouchPad {
    uint8_t is_active, id;
    uint16_t x, y;
};
struct PadData {
    PortInfo info;
    uint32_t packet_counter;
    uint16_t digital_button;
    uint8_t home, touch_hard_press;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t analog[12];   // dpad L/D/R/U, square, cross, circle, triangle, R1, L1, R2, L2
    TouchPad touch[2];
    uint64_t motion_timestamp;
    float accel_x, accel_y, accel_z;
    float gyro_pitch, gyro_yaw, gyro_roll;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 20);
static_assert(sizeof(PortInfo) == 12);
static_assert(sizeof(PadData) == 80);
static_assert(sizeof(Header) + sizeof(PadData) == 100);

uint32_t Crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

// DSU digital_button bit order, as read by Eglantine's udp_client.cpp:
// Share L3 R3 Options Up Right Down Left L2 R2 L1 R1 Triangle Circle Cross Square.
// Its auto-mapping then turns Circle->A, Cross->B, Triangle->X, Square->Y, so
// Switch buttons map back to their own position.
uint16_t ToDsuButtons(uint32_t b) {
    struct Pair { uint32_t eb; int bit; };
    static constexpr Pair map[] = {
        {EB_BTN_MINUS, 0},  {EB_BTN_LSTICK, 1}, {EB_BTN_RSTICK, 2}, {EB_BTN_PLUS, 3},
        {EB_BTN_UP, 4},     {EB_BTN_RIGHT, 5},  {EB_BTN_DOWN, 6},   {EB_BTN_LEFT, 7},
        {EB_BTN_ZL, 8},     {EB_BTN_ZR, 9},     {EB_BTN_L, 10},     {EB_BTN_R, 11},
        {EB_BTN_X, 12},     {EB_BTN_A, 13},     {EB_BTN_B, 14},     {EB_BTN_Y, 15},
    };
    uint16_t out = 0;
    for (const auto& m : map)
        if (b & m.eb) out |= static_cast<uint16_t>(1u << m.bit);
    return out;
}

// Eglantine reads a stick as (v - 127) / 127, +y = up.
uint8_t ToStick(float v) {
    return static_cast<uint8_t>(std::clamp(std::lround(127.0f + std::clamp(v, -1.0f, 1.0f) * 127.0f), 0L, 255L));
}

}  // namespace

struct DsuServer::Impl {
    socket_t sock = kBadSocket;
    std::thread thread;
    std::atomic<bool> running{false};
    uint32_t server_id = 0;
    const Clock::time_point start = Clock::now();

    struct Client {
        sockaddr_in addr;
        Clock::time_point last_request;
    };
    mutable std::mutex mutex;
    std::vector<Client> clients;
    uint32_t packet_counter = 0;

    void Finish(std::vector<uint8_t>& pkt, uint32_t type) {
        Header h{kServerMagic, kProtocolVersion, static_cast<uint16_t>(pkt.size() - sizeof(Header) + 4), 0,
                 server_id, type};
        std::memcpy(pkt.data(), &h, sizeof h);
        h.crc = Crc32(pkt.data(), pkt.size());
        std::memcpy(pkt.data(), &h, sizeof h);
    }

    void SendTo(const std::vector<uint8_t>& pkt, const sockaddr_in& to) {
        sendto(sock, reinterpret_cast<const char*>(pkt.data()), static_cast<int>(pkt.size()), 0,
               reinterpret_cast<const sockaddr*>(&to), sizeof to);
    }

    static PortInfo SlotInfo(uint8_t slot) {
        PortInfo p{};
        p.id = slot;
        if (slot == 0) {   // one controller: the Switch
            p.state = 2;            // connected
            p.model = 2;            // full gyro
            p.connection_type = 1;  // "USB"
            const uint8_t mac[6] = {0x02, 0xEB, 0x00, 0x00, 0x00, 0x01};
            std::memcpy(p.mac, mac, 6);
            p.battery = 0x05;       // full
            p.is_active = 1;
        }
        return p;
    }

    void Remember(const sockaddr_in& from) {
        std::lock_guard lk(mutex);
        for (auto& c : clients) {
            if (c.addr.sin_addr.s_addr == from.sin_addr.s_addr && c.addr.sin_port == from.sin_port) {
                c.last_request = Clock::now();
                return;
            }
        }
        clients.push_back({from, Clock::now()});
    }

    void HandleRequest(const uint8_t* buf, int n, const sockaddr_in& from) {
        if (n < static_cast<int>(sizeof(Header))) return;
        Header h;
        std::memcpy(&h, buf, sizeof h);
        if (h.magic != kClientMagic || h.version != kProtocolVersion) return;
        const uint8_t* data = buf + sizeof h;
        const int len = n - static_cast<int>(sizeof h);
        if (h.type == kTypeVersion) {
            std::vector<uint8_t> pkt(sizeof(Header) + 2);
            const uint16_t v = kProtocolVersion;
            std::memcpy(pkt.data() + sizeof(Header), &v, 2);
            Finish(pkt, kTypeVersion);
            SendTo(pkt, from);
        } else if (h.type == kTypePortInfo && len >= 4) {
            uint32_t count;
            std::memcpy(&count, data, 4);
            count = std::min<uint32_t>(count, static_cast<uint32_t>(std::max(0, len - 4)));
            for (uint32_t i = 0; i < std::min<uint32_t>(count, 4); i++) {
                std::vector<uint8_t> pkt(sizeof(Header) + sizeof(PortInfo));
                const PortInfo p = SlotInfo(data[4 + i]);
                std::memcpy(pkt.data() + sizeof(Header), &p, sizeof p);
                Finish(pkt, kTypePortInfo);
                SendTo(pkt, from);
            }
        } else if (h.type == kTypePadData) {
            Remember(from);   // subscription: we stream PadData to it from now on
        }
    }

    void Loop() {
        uint8_t buf[256];
        while (running) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(sock, &fds);
            timeval tv{0, 200000};
            if (select(static_cast<int>(sock + 1), &fds, nullptr, nullptr, &tv) <= 0) continue;
            sockaddr_in from{};
            socklen_t from_len = sizeof from;
            const int n = recvfrom(sock, reinterpret_cast<char*>(buf), sizeof buf, 0,
                                   reinterpret_cast<sockaddr*>(&from), &from_len);
            if (n > 0) HandleRequest(buf, n, from);
        }
    }

    void Broadcast(const PadData& pad_in) {
        std::vector<sockaddr_in> targets;
        PadData pad = pad_in;
        {
            std::lock_guard lk(mutex);
            const auto now = Clock::now();
            clients.erase(std::remove_if(clients.begin(), clients.end(),
                                         [&](const Client& c) { return now - c.last_request > kClientTimeout; }),
                          clients.end());
            if (clients.empty()) return;
            for (const auto& c : clients) targets.push_back(c.addr);
            // Eglantine drops a packet whose counter equals the previous one.
            pad.packet_counter = ++packet_counter;
        }
        std::vector<uint8_t> pkt(sizeof(Header) + sizeof(PadData));
        std::memcpy(pkt.data() + sizeof(Header), &pad, sizeof pad);
        Finish(pkt, kTypePadData);
        for (const auto& t : targets) SendTo(pkt, t);
    }
};

DsuServer::DsuServer() : impl(std::make_unique<Impl>()) {}
DsuServer::~DsuServer() { Stop(); }

bool DsuServer::Start(uint16_t port, const std::string& bind_address, std::string* error) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);   // reference counted, paired with WSACleanup in Stop
#endif
    std::random_device rd;
    impl->server_id = rd();
    impl->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl->sock == kBadSocket) {
        if (error) *error = "DSU: socket() failed";
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, bind_address.c_str(), &addr.sin_addr);
    if (bind(impl->sock, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        if (error) *error = "DSU: UDP port " + std::to_string(port) + " in use (is another DSU server running?)";
        CloseSocket(impl->sock);
        impl->sock = kBadSocket;
        return false;
    }
    impl->running = true;
    impl->thread = std::thread([this] { impl->Loop(); });
    return true;
}

void DsuServer::Stop() {
    if (!impl->running.exchange(false)) return;
    if (impl->thread.joinable()) impl->thread.join();
    CloseSocket(impl->sock);
    impl->sock = kBadSocket;
#ifdef _WIN32
    WSACleanup();
#endif
}

void DsuServer::Update(const InputState& s) {
    PadData p{};
    p.info = Impl::SlotInfo(0);
    p.digital_button = ToDsuButtons(s.buttons);
    p.home = (s.buttons & EB_BTN_HOME) ? 1 : 0;
    p.touch_hard_press = (s.buttons & EB_BTN_CAPTURE) ? 1 : 0;   // Eglantine maps it to Screenshot
    p.left_x = ToStick(s.lx);
    p.left_y = ToStick(s.ly);
    p.right_x = ToStick(s.rx);
    p.right_y = ToStick(s.ry);
    p.analog[10] = (s.buttons & EB_BTN_ZR) ? 255 : 0;
    p.analog[11] = (s.buttons & EB_BTN_ZL) ? 255 : 0;

    // Eglantine scales touch with its default calibration: x 100..1800, y 50..850.
    for (int i = 0; i < 2 && i < s.touch_count; i++) {
        p.touch[i].is_active = 1;
        p.touch[i].id = static_cast<uint8_t>(s.touch[i].id);
        p.touch[i].x = static_cast<uint16_t>(100 + std::clamp(s.touch[i].x, 0.0f, 1.0f) * 1700);
        p.touch[i].y = static_cast<uint16_t>(50 + std::clamp(s.touch[i].y, 0.0f, 1.0f) * 800);
    }

    // Inverse of Eglantine's conversion:
    //   gyro_x = pitch/312, gyro_y = roll/312, gyro_z = -yaw/312
    //   accel_x = x, accel_y = -z, accel_z = y
    p.motion_timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - impl->start).count());
    p.gyro_pitch = s.gyro[0] * kGyroScale;
    p.gyro_roll = s.gyro[1] * kGyroScale;
    p.gyro_yaw = -s.gyro[2] * kGyroScale;
    p.accel_x = s.accel[0];
    p.accel_z = -s.accel[1];
    p.accel_y = s.accel[2];
    impl->Broadcast(p);
}

void DsuServer::Neutral() {
    InputState s{};
    s.accel = {0.0f, 0.0f, -1.0f};   // controller lying flat, still
    Update(s);
}

int DsuServer::ClientCount() const {
    std::lock_guard lk(impl->mutex);
    return static_cast<int>(impl->clients.size());
}

}  // namespace ebridge
