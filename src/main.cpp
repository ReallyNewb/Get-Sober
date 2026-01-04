#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include "FileExplorer.hpp"
#include "Console.hpp"
#include "Utils.hpp"

using namespace geode::prelude;

$execute {
    FileExplorer::get()->setup();
    Console::get()->setup();
}
