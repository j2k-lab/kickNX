#include <borealis.hpp>

#include "utils/config.hpp"
#include "utils/thread.hpp"
#include "utils/debug_log.hpp"
#include "utils/orientation.hpp"
#include "utils/theme.hpp"

#include "view/svg_image.hpp"
#include "view/custom_button.hpp"
#include "view/recycling_grid.hpp"
#include "view/h_recycling.hpp"
#include "view/video_progress_slider.hpp"
#include "view/gallery_view.hpp"
#include "view/video_view.hpp"
#include "view/selector_cell.hpp"
#include "view/button_close.hpp"
#include "view/text_box.hpp"
#include "view/icon_button.hpp"
#include "view/kick_loading_view.hpp"
#include "view/mpv_core.hpp"

#include "activity/hint_activity.hpp"
#include "tab/remote_view.hpp"
#include "activity/kicknx_home.hpp"

#if defined(__SDL2__)
#include <SDL2/SDL_main.h>
#endif

using namespace brls::literals;  // for _i18n

namespace {

void cleanWatchLaterCache(const std::string& directory) {
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (entry.is_regular_file(ec)) {
            std::filesystem::remove(entry.path(), ec);
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
#if defined(KICKNX_PLAYBACK_DEBUG)
    // switch_wrapper.c has already connected stdio to nxlink before main().
    // Disable buffering so the last record before a fatal crash reaches the PC.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
#endif
    std::vector<std::string> items;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-d") == 0) {
            brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
        } else if (std::strcmp(argv[i], "-v") == 0) {
            brls::Application::enableDebuggingView(true);
        } else if (std::strcmp(argv[i], "-t") == 0) {
            MPVCore::DEBUG = true;
        } else if (std::strcmp(argv[i], "-o") == 0) {
            const char* path = (i + 1 < argc) ? argv[++i] : "switchfin.log";
            brls::Logger::setLogOutput(std::fopen(path, "w+"));
        } else if (std::strcmp(argv[i], "-version") == 0) {
            brls::Logger::info("{} {}", AppVersion::getDeviceName(), AppVersion::getCommit());
            return 0;
        } else {
            items.push_back(argv[i]);
        }
    }

    std::setlocale(LC_ALL, "C.UTF-8");
    // Load cookies and settings
    auto& conf = AppConfig::instance();
    if (!conf.init()) {
        return 0;
    }

#if defined(KICKNX_PLAYBACK_DEBUG)
    static FILE* logFp = nullptr;
    kicknx::debug::cleanStaleLogs(conf.configDir());
    kicknx::debug::init(
        conf.configDir() + "/kicknx-playback-debug.log");
    kicknx::debug::log(
        "APP",
        "boot version=%s platform=%s commit=%s argc=%d",
        AppVersion::getVersion().c_str(),
        AppVersion::getPlatform().c_str(),
        AppVersion::getCommit().c_str(),
        argc);
    logFp = std::fopen((conf.configDir() + "/kicknx.log").c_str(), "w");
    if (logFp) {
        brls::Logger::setLogOutput(logFp);
    }
#endif

    // Init the app and i18n
    if (!brls::Application::init()) {
        brls::Logger::error("Unable to init application");
        return EXIT_FAILURE;
    }

    conf.initThemes();
    // Apply the saved colour preset after the default theme colours exist.
    kicknx::theme::load();

    // Clean watch-later cache on startup
    cleanWatchLaterCache(conf.configDir() + "/watch-later");

    // Return directly to the desktop when closing the application (only for NX)
    brls::Application::getPlatform()->exitToHomeMode(true);

    brls::Application::createWindow(fmt::format("{} for {}", AppVersion::getPackageName(), AppVersion::getPlatform()));

    // SwitchPlatform creates its InputManager inside createWindow(). Sensor
    // subscriptions are invalid before this point.
    kicknx::portrait::OrientationController::instance().init();

    // Have the application register an action on every activity that will quit when you press BUTTON_START
    brls::Application::setGlobalQuit(false);

    // Register custom views (including tabs, which are views)
    brls::Application::registerXMLView("SVGImage", SVGImage::create);
    brls::Application::registerXMLView("IconButton", IconButton::create);
    brls::Application::registerXMLView("CustomButton", CustomButton::create);
    brls::Application::registerXMLView("SelectorCell", SelectorCell::create);
    brls::Application::registerXMLView("TextBox", TextBox::create);
    brls::Application::registerXMLView("ButtonClose", ButtonClose::create);
    brls::Application::registerXMLView("RecyclingGrid", RecyclingGrid::create);
    brls::Application::registerXMLView("HRecyclerFrame", HRecyclerFrame::create);
    brls::Application::registerXMLView("GalleryView", GalleryView::create);
    brls::Application::registerXMLView("VideoProgressSlider", VideoProgressSlider::create);
    brls::Application::registerXMLView("KickLoadingView", kick::KickLoadingView::create);

    if (!brls::Application::getPlatform()->isApplicationMode()) {
        brls::Application::pushActivity(new HintActivity());
    } else {
        brls::Application::pushActivity(new brls::Activity(new KickNXHome()));
    }
    // KickNX: analytics disabled.
    // KickNX: Switchfin update checks disabled.
// Run the app
    while (brls::Application::mainLoop());

    kicknx::portrait::OrientationController::instance().shutdown();
    ThreadPool::instance().stop();
    cleanWatchLaterCache(conf.configDir() + "/watch-later");

    conf.checkRestart(argv);
#if defined(KICKNX_PLAYBACK_DEBUG)
    kicknx::debug::log("APP", "main loop exited normally");
    kicknx::debug::shutdown();
    if (logFp) {
        std::fclose(logFp);
        logFp = nullptr;
    }
#endif
    // Exit
    return EXIT_SUCCESS;
}
