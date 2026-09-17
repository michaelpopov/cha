#pragma once

namespace httplib { class Server; }
namespace cha::web {
class AudioDownloadManager;
struct WebSettings;

void install_audio_download_routes(httplib::Server& server, AudioDownloadManager& downloads,
    const WebSettings& settings);
}
