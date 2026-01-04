#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include "FileExplorer.hpp"
#include "Console.hpp"
#include "Utils.hpp"

using namespace geode::prelude;

$execute {
    if (sobriety::utils::isWine()) {
        FileExplorer::get()->setup();
        Console::get()->setup();
    }
    else {
        (void) Mod::get()->uninstall();
    }
}

class $modify(MenuLayer) {

    bool init() {
        if (!MenuLayer::init()) return false;

        queueInMainThread([] {
            if (!sobriety::utils::isWine()) {
                createQuickPopup("Windows User Detected!", "Sobriety only works on <cg>Linux</c> systems and will do nothing on <cb>Windows</c>.\nIt has been <cr>uninstalled</c>.", "OK", nullptr, nullptr);
            }
        });

        return true;
    }
};