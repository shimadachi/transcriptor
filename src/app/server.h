// The local control-plane server.
//
// Serves /api/* to the desktop app (desktop/), which starts this process and
// is its only client. There is no page here to open in a browser: every
// request has to carry the token below. Binds to loopback only.
#pragma once

#include <memory>
#include <string>

#include "app/state.h"

namespace transcriptor::app {

class Server {
public:
    Server(AppState* state, std::string host, int port);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Binds and starts serving on a background thread. Returns false if the
    // port could not be bound. `port` 0 picks a free port; read it back with
    // port().
    bool start();

    void stop();

    int         port() const { return port_; }
    // This run's secret, for whoever started the server to hand to its client.
    const std::string& token() const { return token_; }
    std::string base_url() const;

private:
    struct Impl;

    AppState*             state_;
    std::string           host_;
    int                   port_;
    // Minted by start() and demanded back on every request. Loopback is not a
    // boundary on its own: anything on this machine can reach it, including a
    // page the user did not open.
    std::string           token_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace transcriptor::app
