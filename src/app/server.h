// The local control-plane server.
//
// Serves the embedded web UI and the same /api/* surface the Flask build
// exposed, so the browser code is unchanged. Binds to loopback only.
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
    std::string base_url() const;

private:
    struct Impl;

    AppState*             state_;
    std::string           host_;
    int                   port_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace transcriptor::app
