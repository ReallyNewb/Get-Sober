#include <Geode/Geode.hpp>
#include <Geode/modify/CCTouchDispatcher.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/CCMouseDispatcher.hpp>

using namespace geode::prelude;

static std::string wineToLinuxPath(const std::filesystem::path& winPath) {
    std::string s = utils::string::pathToString(winPath);

    if (s.size() < 2 || s[1] != ':')
        return s;

    char drive = std::tolower(s[0]);
    std::string rest = s.substr(2);
    for (auto& c : rest) if (c == '\\') c = '/';

    const char* prefixEnv = std::getenv("WINEPREFIX");
    std::string prefix = prefixEnv ? prefixEnv : std::string(std::getenv("HOME")) + "/.wine";

    std::string drivePath;

    if (drive == 'z') {
        drivePath = "/";
    } else {
        drivePath = prefix + "/drive_" + drive;
    }

    std::string fullPath = drivePath;
    size_t start = 0;
    while (start < rest.size()) {
        size_t end = rest.find('/', start);
        if (end == std::string::npos) end = rest.size();
        std::string part = rest.substr(start, end - start);
        if (!part.empty()) {
            if (fullPath.back() != '/') fullPath += "/";
            fullPath += part;
        }
        start = end + 1;
    }

    return fullPath;
}

struct PickerState {
    std::function<void(Result<std::filesystem::path>)> fileCallback;
    std::function<void(Result<std::vector<std::filesystem::path>>)> filesCallback;
    std::function<void()> cancelledCallback;
};

static std::mutex s_stateMtx;
static std::shared_ptr<PickerState> s_state;
static std::atomic_bool s_pickerActive = false;

enum class PickMode {
    OpenFile,
    SaveFile,
    OpenFolder,
    OpenMultipleFiles,
    BrowseFiles
};

static void runOpenFileScript(const std::string& startPath, PickMode pickMode, const std::vector<std::string>& filters) {
    std::string command = "/tmp/GeometryDash/openFile.exe";

    command += " \"";
    command += startPath;
    command += "\"";

    command += " \"";
    switch (pickMode) {
        case PickMode::OpenFile: {
            command += "Select a file";
            break;
        }
        case PickMode::SaveFile: {
            command += "Save...";
            break;
        }
        case PickMode::OpenFolder: {
            command += "Select a folder";
            break;
        }
        case PickMode::BrowseFiles: {
            command += "Browse";
            break;
        }
        case PickMode::OpenMultipleFiles: {
            command += "Select files";
            break;
        }
    }
    command += "\"";

    command += " \"";
    switch (pickMode) {
        case PickMode::OpenFile: {
            command += "single";
            break;
        }
        case PickMode::SaveFile: {
            command += "save";
            break;
        }
        case PickMode::OpenFolder: {
            command += "dir";
            break;
        }
        case PickMode::BrowseFiles: {
            command += "browse";
            break;
        }
        case PickMode::OpenMultipleFiles: {
            command += "multi";
            break;
        }
    }
    command += "\"";

    for (const auto& param : filters) {
        command += " \"";
        command += param;
        command += "\"";
    }

    system(command.c_str());
}

std::vector<std::string> generateExtensionStrings(const std::vector<utils::file::FilePickOptions::Filter>& filters) {
    std::vector<std::string> strings;
    for (const auto& filter : filters) {
        std::string extStr = utils::string::trim(filter.description);
        extStr += "|";
        for (const auto& extension : filter.files) {
            extStr += utils::string::trim(extension);
            extStr += " ";
        }
        strings.push_back(utils::string::trim(extStr));
    }
    return strings;
}

bool file_openFolder_h(std::filesystem::path const& path) {
    if (std::filesystem::is_directory(path)) {
        runOpenFileScript(wineToLinuxPath(path), PickMode::BrowseFiles, {});
        return true;
    }
    return false;
}

Task<Result<std::filesystem::path>> file_pick_h(utils::file::PickMode mode, const utils::file::FilePickOptions& options) {
    using RetTask = Task<Result<std::filesystem::path>>;

    if (s_pickerActive.exchange(true))
        return RetTask::immediate(Err("File picker is already open"));

    auto state = std::make_shared<PickerState>();
    {
        std::lock_guard lock(s_stateMtx);
        s_state = state;
    }

    auto defaultPath =
        wineToLinuxPath(options.defaultPath.value_or(dirs::getGameDir()));

    runOpenFileScript(
        defaultPath,
        static_cast<PickMode>(mode),
        generateExtensionStrings(options.filters)
    );

    return RetTask::runWithCallback(
        [state](auto result, auto, auto cancelled) {
            state->fileCallback = result;
            state->cancelledCallback = cancelled;
        }
    );
}

Task<Result<std::vector<std::filesystem::path>>> file_pickMany_h(const utils::file::FilePickOptions& options) {
    using RetTask = Task<Result<std::vector<std::filesystem::path>>>;

    if (s_pickerActive.exchange(true))
        return RetTask::immediate(Err("File picker is already open"));

    auto state = std::make_shared<PickerState>();
    {
        std::lock_guard lock(s_stateMtx);
        s_state = state;
    }

    auto defaultPath =
        wineToLinuxPath(options.defaultPath.value_or(dirs::getGameDir()));

    runOpenFileScript(
        defaultPath,
        PickMode::OpenMultipleFiles,
        generateExtensionStrings(options.filters)
    );

    return RetTask::runWithCallback(
        [state](auto result, auto, auto cancelled) {
            state->filesCallback = result;
            state->cancelledCallback = cancelled;
        }
    );
}

static void notifySelectedFileChange() {
    const auto path = std::filesystem::path("/tmp/GeometryDash/selectedFile.txt");

    auto strRes = utils::file::readString(path);
    if (!strRes) return;

    std::string str = strRes.unwrap();
    utils::string::trimIP(str);


    if (str.empty()) return;

    std::shared_ptr<PickerState> state;
    {
        std::lock_guard lock(s_stateMtx);
        state = std::move(s_state);
    }

    if (!state) return;

    if (str == "-1") {
        if (state->cancelledCallback) state->cancelledCallback();
    }
    else if (state->fileCallback) {
        state->fileCallback(Ok(std::filesystem::path(str)));
    }
    else if (state->filesCallback) {
        auto parts = utils::string::split(str, "\n");

        std::vector<std::filesystem::path> paths;
        paths.reserve(parts.size());

        for (auto& p : parts) {
            if (!p.empty()) paths.emplace_back(p);
        }

        state->filesCallback(Ok(std::move(paths)));
    }

    s_pickerActive.store(false, std::memory_order_release);
}

static void watcherThread() {
    const auto path = std::filesystem::path("Z:\\tmp\\GeometryDash");

    HANDLE handle = CreateFileW(
        path.c_str(),
        GENERIC_READ | FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr
    );

    if (handle == INVALID_HANDLE_VALUE) {
        geode::log::error("Failed to open directory for watching: {}", GetLastError());
        return;
    }

    char buffer[1024];
    DWORD bytesReturned;

    while (true) {
        if (!ReadDirectoryChangesW(
            handle,
            buffer,
            sizeof(buffer),
            TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME |
            FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_ATTRIBUTES |
            FILE_NOTIFY_CHANGE_SIZE |
            FILE_NOTIFY_CHANGE_LAST_WRITE |
            FILE_NOTIFY_CHANGE_CREATION,
            &bytesReturned,
            nullptr,
            nullptr
        )) {
            log::error("Failed to read directory changes: {}", GetLastError());
            continue;
        }

        auto change = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);
        do {
            std::wstring wname(change->FileName, change->FileNameLength / sizeof(WCHAR));
            std::string name = utils::string::wideToUtf8(wname);

            if (name == "selectedFile.txt") {
                notifySelectedFileChange();
            }

            change = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
                reinterpret_cast<char*>(change) + change->NextEntryOffset
            );
        } while (change->NextEntryOffset != 0);
    }
}

static std::string openFileScript = 
R"script(#!/bin/bash

TMP="/tmp/GeometryDash/selectedFile.txt"
> "$TMP"

START_PATH="$1"
shift
[ -z "$START_PATH" ] && START_PATH="$HOME"
[ -f "$START_PATH" ] && START_PATH="$(dirname "$START_PATH")"

TITLE="$1"
shift
[ -z "$TITLE" ] && TITLE="Select a file"

MODE="$1"
shift
[ -z "$MODE" ] && MODE="single"

FILTERS=("$@")

PICKER=""
DE="$XDG_CURRENT_DESKTOP"
if [[ "$DE" == *KDE* ]]; then
    PICKER="kdialog"
elif [[ "$DE" == *GNOME* ]]; then
    PICKER="zenity"
fi

if ! command -v "$PICKER" >/dev/null 2>&1; then
    if command -v kdialog >/dev/null 2>&1; then
        PICKER="kdialog"
    elif command -v zenity >/dev/null 2>&1; then
        PICKER="zenity"
    elif command -v yad >/dev/null 2>&1; then
        PICKER="yad"
    else
        PICKER="xdg-open"
    fi
fi

DEFAULT_FILE=""
if [ "$MODE" = "save" ] && [ "${#FILTERS[@]}" -gt 0 ]; then
    IFS='|' read -r desc exts <<< "${FILTERS[0]}"
    FIRST_EXT=$(echo "$exts" | awk '{print $1}')
    FIRST_EXT="${FIRST_EXT#\*}"
    DEFAULT_FILE="Untitled$FIRST_EXT"
fi

launch_picker() {
    FILE=""
    STATUS=0

    case "$PICKER" in
        zenity)
            CMD=(zenity --title="$TITLE" --filename="$START_PATH/$DEFAULT_FILE")
            case "$MODE" in
                single) CMD+=(--file-selection) ;;
                multi) CMD+=(--file-selection --multiple --separator=":") ;;
                dir) CMD+=(--file-selection --directory) ;;
                save) CMD+=(--file-selection --save) ;;
                browse) xdg-open "$START_PATH"; FILE=""; STATUS=0; return ;;
                *) CMD+=(--file-selection) ;;
            esac
            for f in "${FILTERS[@]}"; do
                IFS='|' read -r desc exts <<< "$f"
                CMD+=(--file-filter="$desc | $exts")
            done
            FILE=$("${CMD[@]}")
            STATUS=$?
            ;;
        kdialog)
            FILTER_STRING=""
            for f in "${FILTERS[@]}"; do
                IFS='|' read -r desc exts <<< "$f"
                [[ -n "$FILTER_STRING" ]] && FILTER_STRING+=" | "
                FILTER_STRING+="$exts | $desc"
            done
            case "$MODE" in
                single) FILE=$(kdialog --title "$TITLE" --getopenfilename "$START_PATH" "$FILTER_STRING") ;;
                multi) FILE=$(kdialog --title "$TITLE" --getopenfilenames "$START_PATH" "$FILTER_STRING") ;;
                dir) FILE=$(kdialog --title "$TITLE" --getexistingdirectory "$START_PATH") ;;
                save) FILE=$(kdialog --title "$TITLE" --getsavefilename "$START_PATH/$DEFAULT_FILE" "$FILTER_STRING") ;;
                browse) xdg-open "$START_PATH"; FILE=""; STATUS=0; return ;;
                *) FILE=$(kdialog --title "$TITLE" --getopenfilename "$START_PATH" "$FILTER_STRING") ;;
            esac
            STATUS=$?
            ;;
        yad)
            CMD=(yad --title="$TITLE" --filename="$START_PATH/$DEFAULT_FILE")
            case "$MODE" in
                single) CMD+=(--file-selection) ;;
                multi) CMD+=(--file-selection --multiple --separator=":") ;;
                dir) CMD+=(--file-selection --directory) ;;
                save) CMD+=(--file-selection --save) ;;
                browse) xdg-open "$START_PATH"; FILE=""; STATUS=0; return ;;
                *) CMD+=(--file-selection) ;;
            esac
            for f in "${FILTERS[@]}"; do
                IFS='|' read -r desc exts <<< "$f"
                CMD+=(--file-filter="$desc | $exts")
            done
            FILE=$("${CMD[@]}")
            STATUS=$?
            ;;
        xdg-open)
            xdg-open "$START_PATH"
            FILE=""
            STATUS=0
            ;;
    esac

    if [ -n "$FILE" ]; then
        case "$PICKER" in
            zenity|yad)
                if [ "$MODE" = "multi" ]; then
                    echo "$FILE" | tr ':' '\n' > "$TMP"
                else
                    echo "$FILE" > "$TMP"
                fi
                ;;
            kdialog)
                if [ "$MODE" = "multi" ]; then
                    echo "$FILE" | sed 's/"//g' | tr ' ' '\n' > "$TMP"
                else
                    echo "$FILE" > "$TMP"
                fi
                ;;
            xdg-open) ;;
        esac
    else
        [ "$STATUS" -ne 0 ] && echo "-1" > "$TMP"
    fi
}

launch_picker &

)script";

$execute {

    HMODULE hModule = GetModuleHandleA("ntdll.dll");
    if (hModule) {
        FARPROC func = GetProcAddress(hModule, "wine_get_version");
        if (func) {
            /* 
                Normally, writing a bash script to a file and running it cannot be done via wine, as the file needs
                to be marked as executable. But, wine wants to run exes, so simply making the script have an "exe" file
                extension will allow it to be ran without being set as executable. This means we can bypass the limitation 
                and properly bridge between some linux based script and wine.
            */
            (void) utils::file::createDirectory("/tmp/GeometryDash/");
            auto scriptPath = std::filesystem::path("/tmp/GeometryDash/openFile.exe");
            (void) utils::file::writeString(scriptPath, openFileScript);

            std::thread(watcherThread).detach();

            (void) Mod::get()->hook(
                reinterpret_cast<void*>(addresser::getNonVirtual(&utils::file::pick)),
                &file_pick_h,
                "utils::file::pick"
            );
            (void) Mod::get()->hook(
                reinterpret_cast<void*>(addresser::getNonVirtual(&utils::file::pickMany)),
                &file_pickMany_h,
                "utils::file::pickMany"
            );
            (void) Mod::get()->hook(
                reinterpret_cast<void*>(addresser::getNonVirtual(&utils::file::openFolder)),
                &file_openFolder_h,
                "utils::file::openFolder"
            );
        }
        else {
            (void) Mod::get()->uninstall();
        }
    }
}   

/*
    These block inputs when file picker is active to mimic windows behavior. We do not want to actually
    block the main thread.
*/

class $modify(CCTouchDispatcher) {
    void touches(CCSet *pTouches, CCEvent *pEvent, unsigned int uIndex) {
        if (s_pickerActive) {
            if (uIndex == 0) MessageBeep(MB_ICONWARNING);
            return;
        }
        CCTouchDispatcher::touches(pTouches, pEvent, uIndex);
    }
};

class $modify(CCKeyboardDispatcher) {
	bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat) {
        if (s_pickerActive) {
            if (isKeyDown && !isKeyRepeat) MessageBeep(MB_ICONWARNING);
            return false;
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat);
    }
};

class $modify(CCMouseDispatcher) {
	bool dispatchScrollMSG(float x, float y) {
        if (s_pickerActive) {
            return false;
        }
        return CCMouseDispatcher::dispatchScrollMSG(x, y);
    }
};