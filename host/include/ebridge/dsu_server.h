// DSU ("cemuhook") server: exposes the Switch controller to Eglantine without
// modifying it. Eglantine (like yuzu) already contains a DSU client that, by
// default, reads 127.0.0.1:26760 and shows up as "UDP Controller 0" in the
// controller settings, with buttons, sticks, motion and touch.
//
// Protocol details mirror Eglantine's src/input_common/helpers/udp_protocol.h
// and drivers/udp_client.cpp (packet layout, CRC, axis conventions, scale).
// DSU has no rumble channel.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "ebridge/host.h"

namespace ebridge {

class DsuServer {
public:
    DsuServer();
    ~DsuServer();
    DsuServer(const DsuServer&) = delete;
    DsuServer& operator=(const DsuServer&) = delete;

    // bind_address "127.0.0.1" keeps it local to this PC (Eglantine runs here).
    bool Start(uint16_t port = 26760, const std::string& bind_address = "127.0.0.1",
               std::string* error = nullptr);
    void Stop();

    // Sends the state to every subscribed DSU client. Call it for each input packet.
    void Update(const InputState& state);
    // Releases everything (console disconnected).
    void Neutral();

    // Number of DSU clients (Eglantine instances) currently subscribed.
    int ClientCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace ebridge
