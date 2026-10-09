// Renders the client's non-video screens to BMP files, one per screen,
// so a UI change can be looked at without a Steam Deck or even a
// display: everything draws through SDL's software renderer onto an
// in-memory surface. Not shipped; built alongside dualdeck-client.
//
//   dualdeck-ui-preview [output-dir]   (default: ./ui-preview)

#include <SDL3/SDL.h>

#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "screens.h"

using namespace dualdeck::client;

namespace {

DiscoveredHost makeHost(const std::string& name, const std::string& address, const std::string& system,
                        const std::string& adapter) {
    DiscoveredHost host;
    host.hostName = name;
    host.address = address;
    host.controlPort = 8760;
    host.inputPort = 8761;
    host.videoPort = 8762;
    host.system.systemName = system;
    host.adapter.adapterName = adapter;
    return host;
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path outDir = argc > 1 ? argv[1] : "ui-preview";
    std::filesystem::create_directories(outDir);

    SDL_Surface* surface = SDL_CreateSurface(kWindowWidth, kWindowHeight, SDL_PIXELFORMAT_XRGB8888);
    if (!surface) {
        std::fprintf(stderr, "SDL_CreateSurface failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(surface);
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateSoftwareRenderer failed: %s\n", SDL_GetError());
        return 1;
    }

    const std::string version = "V1.2.3";
    const std::vector<DiscoveredHost> hosts = {
        makeHost("LIVING-ROOM-PC", "192.168.1.20", "NINTENDO DS", "MELONDS"),
        makeHost("BAZZITE-HTPC", "192.168.1.31", "NINTENDO 3DS", "AZAHAR"),
        makeHost("", "192.168.1.44", "NINTENDO WII U", "CEMU"),
    };

    const std::vector<ButtonHint> settingsHints = {
        {"D-PAD", "MOVE"}, {"LEFT/RIGHT", "CHANGE"}, {"A", "SELECT"}, {"B", "BACK"}};

    struct Shot {
        std::string name;
        std::function<void()> draw;
    };
    const std::vector<Shot> shots = {
        {"picker-searching", [&] { renderDiscoverySearching(renderer, version); }},
        {"picker-searching-tips", [&] { renderDiscoverySearching(renderer, version, 10, true); }},
        {"picker-list", [&] { renderDiscoveryList(renderer, hosts, 1, version, "192.168.1.20"); }},
        {"connecting", [&] { renderConnecting(renderer, "192.168.1.20"); }},
        {"host-control", [&] { renderHostControlScreen(renderer, "HOST CONTROL - LIVING-ROOM-PC"); }},
        {"menu", [&] {
             renderPauseMenu(renderer, {"RESUME", "CHANGE HOST", "SETTINGS", "EXIT EMULATION", "EXIT"}, 0,
                             "MENU", "", -1.0f, "NINTENDO DS - MELONDS");
         }},
        {"menu-exit-emulation", [&] {
             renderPauseMenu(renderer, {"EXIT ROM", "EXIT MELONDS ENTIRELY", "CANCEL"}, 2, "EXIT EMULATION");
         }},
        {"settings", [&] {
             renderPauseMenu(renderer,
                             {"AUTO UPDATE ON LAUNCH: ON", "VIDEO QUALITY: LOW (SLOWEST LINKS)",
                              "TRACKPAD AS NATIVE INPUT (EXPERIMENTAL): OFF",
                              "MIRROR HOST SCREEN (EXPERIMENTAL): OFF", "VIDEO CODEC (EXPERIMENTAL): JPEG",
                              "DEBUG OVERLAY: OFF", "RUN SETUP WIZARD", "MICROPHONE: SYSTEM DEFAULT",
                              "MIC: ON", "BACK"},
                             8, "SETTINGS", "", 0.4f, "", settingsHints);
         }},
        {"menu-scrolling", [&] {
             std::vector<std::string> items;
             for (int i = 1; i <= 16; ++i) items.push_back("ITEM " + std::to_string(i) + ": VALUE");
             renderPauseMenu(renderer, items, 12, "LONG LIST", "COULD NOT SAVE SETTINGS");
         }},
    };

    for (const auto& shot : shots) {
        shot.draw();
        const std::string path = (outDir / (shot.name + ".bmp")).string();
        if (!SDL_SaveBMP(surface, path.c_str())) {
            std::fprintf(stderr, "SDL_SaveBMP(%s) failed: %s\n", path.c_str(), SDL_GetError());
            return 1;
        }
        std::printf("%s\n", path.c_str());
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(surface);
    return 0;
}
