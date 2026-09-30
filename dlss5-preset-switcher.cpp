// DLSS5 Preset Switcher - a small ReShade add-on for live INI presets.
//
// Presets are ordinary UTF-8 INI files placed beside this add-on or under
// DLSS5-Presets\. The add-on switches ReShade effect presets when an INI has
// effect sections, and writes RenoDX/DLSS5 sections through ReShade's config
// API so renodx-dlss.addon64 and renodx-dlss5.addon64 can observe the change.
//
// The add-on deliberately does not edit ReShade.ini directly. ReShade's
// ReShadeSetConfigValue API performs the write using the host's active config
// and avoids races with ReShade's own configuration writer.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char *kName = "DLSS5 Preset Switcher";
constexpr const char *kVersion = "1.1.0";
constexpr const char *kOverlayTitle = "DLSS5 Presets";
constexpr const char *kConfigFile = "dlss5-preset-switcher.ini";
constexpr const char *kPresetFolder = "DLSS5-Presets";

fs::path g_addon_dir;
std::vector<fs::path> g_presets;
std::string g_status = "Ready";
std::string g_active_name;
fs::path g_active_path;
fs::file_time_type g_active_write_time{};
bool g_auto_reload = true;
bool g_registered = false;

struct IniEntry {
    std::string section;
    std::string key;
    std::string value;
};

struct IniFile {
    std::vector<IniEntry> entries;
    bool has_effect_sections = false;
    bool has_reshade_preset_keys = false;
};

struct BaselineValue {
    std::string section;
    std::string key;
    std::string value;
    bool existed = false;
};

std::map<std::string, BaselineValue> g_baseline;
std::string g_original_preset_path;
bool g_baseline_captured = false;

constexpr size_t kMaxSharePayload = 256 * 1024;
constexpr size_t kShareTextCapacity = 512 * 1024;
std::vector<char> g_share_code(kShareTextCapacity, '\0');
std::array<char, 128> g_import_name{};

bool ApplyPreset(reshade::api::effect_runtime *runtime, const fs::path &path, bool automatic);

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string Trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string Utf8(const fs::path &path)
{
    const std::wstring wide = path.wstring();
    if (wide.empty())
        return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), length,
        nullptr, nullptr);
    return result;
}

void Log(const std::string &message, reshade::log::level level = reshade::log::level::info)
{
    reshade::log::message(level, message.c_str());
}

void SetStatus(const std::string &message, reshade::log::level level = reshade::log::level::info)
{
    g_status = message;
    Log(std::string("[DLSS5 Preset Switcher] ") + message, level);
}

bool ReadText(const fs::path &path, std::string &text)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;

    text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        text.erase(0, 3);

    // Windows INI files are often saved as UTF-16. The configuration API is
    // UTF-8, so reject that format with a useful status rather than parsing
    // embedded NUL characters as keys.
    if (text.size() >= 2 && ((static_cast<unsigned char>(text[0]) == 0xFF &&
        static_cast<unsigned char>(text[1]) == 0xFE) || (static_cast<unsigned char>(text[0]) == 0xFE &&
        static_cast<unsigned char>(text[1]) == 0xFF)))
        return false;
    return true;
}

bool ParseIni(const fs::path &path, IniFile &result)
{
    std::string text;
    if (!ReadText(path, text))
        return false;

    std::istringstream stream(text);
    std::string line;
    std::string section;
    while (std::getline(stream, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        const auto equal = line.find('=');
        if (equal == std::string::npos || section.empty())
            continue;
        const std::string key = Trim(line.substr(0, equal));
        if (key.empty())
            continue;
        const std::string value = Trim(line.substr(equal + 1));
        result.entries.push_back({ section, key, value });

        const std::string lower_section = Lower(section);
        const std::string lower_key = Lower(key);
        if (lower_section == "general" && (lower_key == "techniques" ||
            lower_key == "techniquesorting" || lower_key == "preprocessordefinitions"))
            result.has_reshade_preset_keys = true;
        if (lower_section.size() >= 3 && lower_section.substr(lower_section.size() - 3) == ".fx")
            result.has_effect_sections = true;
    }
    return true;
}

bool IsReserved(const fs::path &path)
{
    const std::string name = Lower(path.filename().string());
    return name == "reshade.ini" || name == "reshade2.ini" || name == Lower(kConfigFile) ||
        name == "dlss5-preset-switcher.log";
}

void AddPreset(const fs::path &path, std::set<std::string> &seen)
{
    if (!fs::is_regular_file(path) || Lower(path.extension().string()) != ".ini" || IsReserved(path))
        return;
    std::error_code error;
    const fs::path canonical = fs::weakly_canonical(path, error);
    const std::string identity = Lower(Utf8(error ? path : canonical));
    if (seen.insert(identity).second)
        g_presets.push_back(path);
}

void ScanPresetDirectory(const fs::path &directory, bool recursive, std::set<std::string> &seen)
{
    std::error_code error;
    if (!fs::is_directory(directory, error))
        return;
    if (recursive) {
        for (const auto &entry : fs::recursive_directory_iterator(directory, error)) {
            if (error)
                break;
            AddPreset(entry.path(), seen);
        }
    } else {
        for (const auto &entry : fs::directory_iterator(directory, error)) {
            if (error)
                break;
            AddPreset(entry.path(), seen);
        }
    }
}

void RefreshPresets()
{
    g_presets.clear();
    std::set<std::string> seen;
    // Direct INIs preserve the requested "put it next to the plugin" workflow.
    ScanPresetDirectory(g_addon_dir, false, seen);
    // The subfolder is recommended and can be organized recursively.
    ScanPresetDirectory(g_addon_dir / kPresetFolder, true, seen);
    std::sort(g_presets.begin(), g_presets.end(), [](const fs::path &a, const fs::path &b) {
        return Lower(a.filename().string()) < Lower(b.filename().string());
    });
}

bool IsProtectedSection(const std::string &section)
{
    const std::string lower = Lower(section);
    // These are host-level settings, not per-preset visual/DLSS settings. A
    // shared preset must not silently disable add-ons or change input keys.
    static const char *const protected_sections[] = {
        "addon", "general", "input", "overlay", "screenshot", "style", "depth"
    };
    for (const char *protected_section : protected_sections)
        if (lower == protected_section)
            return true;
    return false;
}

bool IsGlobalPresetSection(const std::string &section)
{
    // RenoDX names its sections RENODX-DLSS and RENODX-DLSS5. DLSS5 feed and
    // future RenoDX sections are intentionally accepted by prefix as well.
    const std::string lower = Lower(section);
    return lower.rfind("renodx-", 0) == 0 || lower.rfind("renodx_", 0) == 0 ||
        lower.rfind("dlss5", 0) == 0 || (!IsProtectedSection(section) &&
        lower.find("renodx") != std::string::npos);
}

bool GetGlobalValue(const std::string &section, const std::string &key, std::string &value)
{
    char buffer[4096] = {};
    size_t size = sizeof(buffer);
    if (!reshade::get_config_value(nullptr, section.c_str(), key.c_str(), buffer, &size))
        return false;
    value.assign(buffer);
    return true;
}

std::string ConfigIdentity(const std::string &section, const std::string &key)
{
    return Lower(section) + "\n" + Lower(key);
}

void SnapshotBaseline(const std::string &section, const std::string &key)
{
    const std::string identity = ConfigIdentity(section, key);
    if (g_baseline.find(identity) != g_baseline.end())
        return;
    BaselineValue baseline{ section, key, {}, false };
    baseline.existed = GetGlobalValue(section, key, baseline.value);
    g_baseline.emplace(identity, std::move(baseline));
}

void RestoreBaseline()
{
    for (const auto &[identity, baseline] : g_baseline) {
        // ReShade's public config API has no delete operation. Empty is the
        // least surprising representation for a key that was absent.
        reshade::set_config_value(nullptr, baseline.section.c_str(), baseline.key.c_str(),
            baseline.existed ? baseline.value.c_str() : "");
    }
}

uint32_t Crc32(const std::string &data)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (const unsigned char byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

uint16_t Hash3(const std::string &data, size_t position)
{
    return static_cast<uint16_t>((static_cast<uint32_t>(static_cast<unsigned char>(data[position])) * 251u +
        static_cast<uint32_t>(static_cast<unsigned char>(data[position + 1])) * 17u +
        static_cast<uint32_t>(static_cast<unsigned char>(data[position + 2]))) & 0xFFFFu);
}

std::vector<uint8_t> CompressPreset(const std::string &input)
{
    std::vector<uint8_t> output;
    output.reserve(input.size());
    std::vector<int> head(65536, -1);
    std::vector<int> previous(input.size(), -1);

    auto add_position = [&](size_t position) {
        if (position + 2 >= input.size())
            return;
        const uint16_t hash = Hash3(input, position);
        previous[position] = head[hash];
        head[hash] = static_cast<int>(position);
    };

    size_t position = 0;
    while (position < input.size()) {
        const size_t control_index = output.size();
        output.push_back(0);
        uint8_t controls = 0;

        for (int bit = 0; bit < 8 && position < input.size(); ++bit) {
            size_t best_length = 0;
            size_t best_offset = 0;
            if (position + 2 < input.size()) {
                const uint16_t hash = Hash3(input, position);
                int candidate = head[hash];
                size_t candidates = 0;
                const size_t oldest = position > 4096 ? position - 4096 : 0;
                while (candidate >= static_cast<int>(oldest) && candidate >= 0 && candidates++ < 64) {
                    size_t length = 0;
                    while (length < 18 && position + length < input.size() &&
                        static_cast<size_t>(candidate) + length < input.size() &&
                        input[static_cast<size_t>(candidate) + length] == input[position + length])
                        ++length;
                    if (length > best_length && length >= 3) {
                        best_length = length;
                        best_offset = position - static_cast<size_t>(candidate);
                        if (length == 18)
                            break;
                    }
                    candidate = previous[static_cast<size_t>(candidate)];
                }
            }

            if (best_length >= 3) {
                const uint16_t token = static_cast<uint16_t>(((best_offset - 1) << 4) | (best_length - 3));
                output.push_back(static_cast<uint8_t>(token & 0xFF));
                output.push_back(static_cast<uint8_t>(token >> 8));
                for (size_t i = 0; i < best_length; ++i)
                    add_position(position++);
            } else {
                controls |= static_cast<uint8_t>(1u << bit);
                output.push_back(static_cast<uint8_t>(input[position]));
                add_position(position++);
            }
        }
        output[control_index] = controls;
    }
    return output;
}

bool DecompressPreset(const std::vector<uint8_t> &input, size_t expected_size, std::string &output)
{
    output.clear();
    output.reserve(expected_size);
    size_t position = 0;
    while (position < input.size() && output.size() < expected_size) {
        const uint8_t controls = input[position++];
        for (int bit = 0; bit < 8 && output.size() < expected_size; ++bit) {
            if (controls & (1u << bit)) {
                if (position >= input.size())
                    return false;
                output.push_back(static_cast<char>(input[position++]));
                continue;
            }
            if (position + 1 >= input.size())
                return false;
            const uint16_t token = static_cast<uint16_t>(input[position]) |
                (static_cast<uint16_t>(input[position + 1]) << 8);
            position += 2;
            const size_t offset = static_cast<size_t>(token >> 4) + 1;
            const size_t length = static_cast<size_t>(token & 0x0F) + 3;
            if (offset > output.size() || output.size() + length > expected_size)
                return false;
            for (size_t i = 0; i < length; ++i)
                output.push_back(output[output.size() - offset]);
        }
    }
    return output.size() == expected_size && position == input.size();
}

const char *const kShareAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string EncodeShareBase64(const std::vector<uint8_t> &data)
{
    std::string output;
    output.reserve((data.size() * 4 + 2) / 3);
    for (size_t i = 0; i < data.size(); i += 3) {
        const uint32_t value = static_cast<uint32_t>(data[i]) << 16 |
            (i + 1 < data.size() ? static_cast<uint32_t>(data[i + 1]) << 8 : 0) |
            (i + 2 < data.size() ? data[i + 2] : 0);
        output.push_back(kShareAlphabet[(value >> 18) & 63]);
        output.push_back(kShareAlphabet[(value >> 12) & 63]);
        if (i + 1 < data.size())
            output.push_back(kShareAlphabet[(value >> 6) & 63]);
        if (i + 2 < data.size())
            output.push_back(kShareAlphabet[value & 63]);
    }
    return output;
}

int ShareBase64Value(char character)
{
    if (character >= 'A' && character <= 'Z')
        return character - 'A';
    if (character >= 'a' && character <= 'z')
        return character - 'a' + 26;
    if (character >= '0' && character <= '9')
        return character - '0' + 52;
    if (character == '-' || character == '+')
        return 62;
    if (character == '_' || character == '/')
        return 63;
    return -1;
}

bool DecodeShareBase64(const std::string &text, std::vector<uint8_t> &output)
{
    output.clear();
    uint32_t value = 0;
    int bits = 0;
    for (const unsigned char character : text) {
        if (std::isspace(character) || character == '=')
            continue;
        const int decoded = ShareBase64Value(static_cast<char>(character));
        if (decoded < 0)
            return false;
        value = (value << 6) | static_cast<uint32_t>(decoded);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<uint8_t>((value >> bits) & 0xFF));
        }
        if (output.size() > kMaxSharePayload + 16)
            return false;
    }
    return bits < 6;
}

void AppendUint32(std::vector<uint8_t> &data, uint32_t value)
{
    data.push_back(static_cast<uint8_t>(value));
    data.push_back(static_cast<uint8_t>(value >> 8));
    data.push_back(static_cast<uint8_t>(value >> 16));
    data.push_back(static_cast<uint8_t>(value >> 24));
}

uint32_t ReadUint32(const std::vector<uint8_t> &data, size_t offset)
{
    return static_cast<uint32_t>(data[offset]) |
        (static_cast<uint32_t>(data[offset + 1]) << 8) |
        (static_cast<uint32_t>(data[offset + 2]) << 16) |
        (static_cast<uint32_t>(data[offset + 3]) << 24);
}

std::string EncodeShareCode(const std::string &preset)
{
    const std::vector<uint8_t> compressed = CompressPreset(preset);
    const bool use_compression = compressed.size() + 1 < preset.size();
    std::vector<uint8_t> packet;
    packet.reserve((use_compression ? compressed.size() : preset.size()) + 9);
    packet.push_back(use_compression ? 1 : 0);
    AppendUint32(packet, static_cast<uint32_t>(preset.size()));
    AppendUint32(packet, Crc32(preset));
    if (use_compression)
        packet.insert(packet.end(), compressed.begin(), compressed.end());
    else
        packet.insert(packet.end(), preset.begin(), preset.end());
    return std::string("D5P1") + EncodeShareBase64(packet);
}

bool DecodeShareCode(const std::string &code, std::string &preset, uint32_t &checksum)
{
    const size_t first = code.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || code.compare(first, 4, "D5P1") != 0)
        return false;
    std::vector<uint8_t> packet;
    if (!DecodeShareBase64(code.substr(first + 4), packet) || packet.size() < 9)
        return false;
    const uint8_t flags = packet[0];
    const size_t expected_size = ReadUint32(packet, 1);
    checksum = ReadUint32(packet, 5);
    if (expected_size == 0 || expected_size > kMaxSharePayload)
        return false;
    if (flags == 0) {
        if (packet.size() - 9 != expected_size)
            return false;
        preset.assign(reinterpret_cast<const char *>(packet.data() + 9), expected_size);
    } else if (flags == 1) {
        std::vector<uint8_t> compressed(packet.begin() + 9, packet.end());
        if (!DecompressPreset(compressed, expected_size, preset))
            return false;
    } else {
        return false;
    }
    return !preset.empty() && preset.find('\0') == std::string::npos && Crc32(preset) == checksum;
}

std::string ShareChecksumName(uint32_t checksum)
{
    char name[32] = {};
    sprintf_s(name, "Shared-%08X.ini", checksum);
    return name;
}

bool SetShareCodeText(const std::string &code)
{
    if (code.size() >= g_share_code.size())
        return false;
    std::memcpy(g_share_code.data(), code.data(), code.size());
    g_share_code[code.size()] = '\0';
    return true;
}

std::string ShareCodeText()
{
    return std::string(g_share_code.data());
}

std::string SafePresetName(const std::string &requested, uint32_t checksum)
{
    std::string name = Trim(requested);
    for (char &character : name) {
        if (character == '/' || character == '\\' || character == ':' || character == '*' ||
            character == '?' || character == '"' || character == '<' || character == '>' || character == '|')
            character = '_';
    }
    if (name.empty() || name == "." || name == "..")
        name = ShareChecksumName(checksum);
    if (Lower(fs::path(name).extension().string()) != ".ini")
        name += ".ini";
    return fs::path(name).filename().string();
}

fs::path UniquePresetPath(const fs::path &directory, const std::string &name)
{
    fs::path result = directory / name;
    const fs::path stem = result.stem();
    const fs::path extension = result.extension();
    for (int suffix = 2; fs::exists(result); ++suffix)
        result = directory / (stem.string() + "-" + std::to_string(suffix) + extension.string());
    return result;
}

void SharePreset(const fs::path &path)
{
    std::string preset;
    if (!ReadText(path, preset) || preset.empty() || preset.size() > kMaxSharePayload) {
        SetStatus("Cannot share this preset (empty, unreadable, or larger than 256 KiB)", reshade::log::level::warning);
        return;
    }
    if (!SetShareCodeText(EncodeShareCode(preset))) {
        SetStatus("Share code is too large", reshade::log::level::warning);
        return;
    }
    SetStatus("Share code generated: " + Utf8(path.filename()) + " (" +
        std::to_string(ShareCodeText().size()) + " chars)");
}

void CopyShareCode()
{
    const std::string code = ShareCodeText();
    if (code.empty()) {
        SetStatus("Generate or paste a share code first", reshade::log::level::warning);
        return;
    }
    ImGui::SetClipboardText(code.c_str());
    SetStatus("Share code copied to clipboard");
}

void PasteShareCode()
{
    const char *clipboard = ImGui::GetClipboardText();
    if (clipboard == nullptr || !SetShareCodeText(clipboard)) {
        SetStatus("Clipboard is empty or the share code is too large", reshade::log::level::warning);
        return;
    }
    SetStatus("Share code pasted; click Import preset to receive it");
}

bool WritePreset(const fs::path &path, const std::string &preset)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file.write(preset.data(), static_cast<std::streamsize>(preset.size()));
    return file.good();
}

void ImportShareCode(reshade::api::effect_runtime *runtime, bool apply)
{
    std::string preset;
    uint32_t checksum = 0;
    if (!DecodeShareCode(ShareCodeText(), preset, checksum)) {
        SetStatus("Invalid or corrupted D5P1 share code", reshade::log::level::warning);
        return;
    }
    const fs::path directory = g_addon_dir / kPresetFolder;
    std::error_code error;
    fs::create_directories(directory, error);
    if (error) {
        SetStatus("Cannot create DLSS5-Presets directory", reshade::log::level::warning);
        return;
    }
    const fs::path target = UniquePresetPath(directory, SafePresetName(g_import_name.data(), checksum));
    if (!WritePreset(target, preset)) {
        SetStatus("Cannot write imported preset", reshade::log::level::warning);
        return;
    }
    RefreshPresets();
    if (apply)
        ApplyPreset(runtime, target, false);
    else
        SetStatus("Imported: " + Utf8(target.filename()));
}

std::string CurrentPresetPath(reshade::api::effect_runtime *runtime)
{
    if (runtime == nullptr)
        return {};
    char path[4096] = {};
    runtime->get_current_preset_path(path);
    return path;
}

bool ApplyPreset(reshade::api::effect_runtime *runtime, const fs::path &path, bool automatic)
{
    IniFile ini;
    if (!ParseIni(path, ini)) {
        SetStatus("Cannot read preset (use UTF-8 INI): " + Utf8(path.filename()), reshade::log::level::warning);
        return false;
    }

    if (!g_baseline_captured) {
        if (runtime != nullptr)
            g_original_preset_path = CurrentPresetPath(runtime);
        g_baseline_captured = true;
    }

    RestoreBaseline();
    for (const IniEntry &entry : ini.entries) {
        if (!IsGlobalPresetSection(entry.section))
            continue;
        SnapshotBaseline(entry.section, entry.key);
        reshade::set_config_value(nullptr, entry.section.c_str(), entry.key.c_str(), entry.value.c_str());
    }

    if (runtime != nullptr && (ini.has_effect_sections || ini.has_reshade_preset_keys)) {
        runtime->set_current_preset_path(Utf8(path).c_str());
    }

    g_active_path = path;
    g_active_name = Utf8(path.filename());
    std::error_code error;
    g_active_write_time = fs::last_write_time(path, error);
    SetStatus(std::string(automatic ? "Auto-reloaded: " : "Applied: ") + g_active_name);
    return true;
}

void ClearPreset(reshade::api::effect_runtime *runtime)
{
    RestoreBaseline();
    if (runtime != nullptr && !g_original_preset_path.empty())
        runtime->set_current_preset_path(g_original_preset_path.c_str());
    g_active_name.clear();
    g_active_path.clear();
    g_original_preset_path.clear();
    g_baseline.clear();
    g_baseline_captured = false;
    SetStatus("Cleared; original ReShade/RenoDX settings restored");
}

void AutoReloadIfChanged(reshade::api::effect_runtime *runtime)
{
    if (!g_auto_reload || g_active_path.empty() || !fs::exists(g_active_path))
        return;
    std::error_code error;
    const auto write_time = fs::last_write_time(g_active_path, error);
    if (!error && write_time != g_active_write_time)
        ApplyPreset(runtime, g_active_path, true);
}

void DrawOverlay(reshade::api::effect_runtime *runtime)
{
    RefreshPresets();
    AutoReloadIfChanged(runtime);

    ImGui::Text("Drop UTF-8 .ini files beside the add-on or in DLSS5-Presets\\");
    ImGui::Text("RenoDX sections are applied through ReShade config API.");
    ImGui::Checkbox("Auto-reload active file", &g_auto_reload);
    ImGui::SameLine();
    if (ImGui::Button("Refresh files"))
        RefreshPresets();
    ImGui::Separator();

    if (g_presets.empty()) {
        ImGui::TextDisabled("No .ini presets found.");
    } else {
        for (size_t i = 0; i < g_presets.size(); ++i) {
            const fs::path &path = g_presets[i];
            const bool active = !g_active_path.empty() && Lower(Utf8(path)) == Lower(Utf8(g_active_path));
            const std::string label = Utf8(path.lexically_relative(g_addon_dir));
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(label.c_str(), active))
                ApplyPreset(runtime, path, false);
            ImGui::SameLine();
            if (ImGui::SmallButton("Apply"))
                ApplyPreset(runtime, path, false);
            ImGui::SameLine();
            if (ImGui::SmallButton("Share"))
                SharePreset(path);
            ImGui::PopID();
        }
    }

    ImGui::Separator();
    ImGui::Text("Quick share (one preset per code; compressed and clipboard-safe):");
    ImGui::InputText("Import filename", g_import_name.data(), g_import_name.size());
    ImGui::InputTextMultiline("##dlss5_share_code", g_share_code.data(), g_share_code.size(), ImVec2(-1, 120));
    if (ImGui::Button("Copy code"))
        CopyShareCode();
    ImGui::SameLine();
    if (ImGui::Button("Paste code"))
        PasteShareCode();
    ImGui::SameLine();
    if (ImGui::Button("Import preset"))
        ImportShareCode(runtime, false);
    ImGui::SameLine();
    if (ImGui::Button("Import & apply"))
        ImportShareCode(runtime, true);
    if (!g_active_path.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Share active"))
            SharePreset(g_active_path);
    }
    ImGui::Text("Code length: %zu characters", ShareCodeText().size());

    ImGui::Separator();
    if (ImGui::Button("Clear / restore original"))
        ClearPreset(runtime);
    ImGui::SameLine();
    ImGui::Text("%s", g_status.c_str());
    if (!g_active_name.empty())
        ImGui::Text("Active: %s", g_active_name.c_str());
}

} // namespace

extern "C" __declspec(dllexport) const char *NAME = kName;
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Live INI preset switcher for RenoDX DLSS and DLSS5 ReShade add-ons";

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        wchar_t module_path[MAX_PATH] = {};
        GetModuleFileNameW(module, module_path, MAX_PATH);
        g_addon_dir = fs::path(module_path).parent_path();
        RefreshPresets();

        if (!reshade::register_addon(module))
            return FALSE;
        g_registered = true;
        reshade::register_overlay(kOverlayTitle, DrawOverlay);
        Log(std::string(kName) + " " + kVersion + " loaded; " + std::to_string(g_presets.size()) + " preset(s)");
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        if (g_registered) {
            reshade::unregister_overlay(kOverlayTitle, DrawOverlay);
            reshade::unregister_addon(module);
            g_registered = false;
        }
    }
    return TRUE;
}
