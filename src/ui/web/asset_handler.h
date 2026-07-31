#pragma once

namespace httplib {
class Server;
}

namespace cha::web {

// Keeps page and future bundled-asset serving separate from the JSON lobby
// API. This block intentionally supplies only a framework-neutral placeholder.
class AssetHandler {
public:
    void install(httplib::Server& server) const;
};

} // namespace cha::web
