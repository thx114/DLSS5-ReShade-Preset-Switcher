// DLSS5 Preset Switcher - a small ReShade add-on for live INI presets.
//
// Presets are ordinary UTF-8 INI files placed under the DLSS5-Presets\
// folder (scanned recursively). The add-on switches ReShade effect presets
// when an INI has
// effect sections, and writes RenoDX/DLSS5 sections through ReShade's config
// API so renodx-dlss.addon64 and renodx-dlss5.addon64 can observe the change.
//
// The add-on deliberately does not edit ReShade.ini directly. ReShade's
// ReShadeSetConfigValue API performs the write using the host's active config
// and avoids races with ReShade's own configuration writer.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#pragma execution_character_set("utf-8")
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
constexpr const char *kVersion = "1.2.1";
constexpr const char *kOverlayTitle = "DLSS5 Presets";
constexpr const char *kConfigFile = "dlss5-preset-switcher.ini";
constexpr const char *kPresetFolder = "DLSS5-Presets";

enum class Language {
    English,
    SimplifiedChinese,
    TraditionalChinese,
    Japanese,
    Korean,
    French,
    German,
    Spanish,
    Russian,
};

enum class TextId {
    OverlayTitle,
    DropHint,
    RenoHint,
    AutoReload,
    Refresh,
    NoPresets,
    Apply,
    Share,
    QuickShare,
    ImportFilename,
    CopyCode,
    PasteCode,
    ImportPreset,
    ImportAndApply,
    ShareActive,
    CodeLength,
    ClearRestore,
    Active,
    Ready,
    CannotShare,
    ShareCodeTooLarge,
    ShareGenerated,
    GenerateFirst,
    ShareCopied,
    ClipboardInvalid,
    SharePasted,
    InvalidShareCode,
    CreateDirectory,
    WriteImported,
    Imported,
    CannotRead,
    AutoReloaded,
    Applied,
    Cleared,
};

struct Translation {
    const char *text[9];
};

// ReShade stores its selected UI language as a BCP-47 locale name in
// [OVERLAY] Language. The add-on keeps its own small table because the public
// ReShade add-on SDK does not expose the host's localization resources.
const Translation kTranslations[] = {
    {{ "DLSS5 Presets", "DLSS5 预设", "DLSS5 預設", "DLSS5 プリセット", "DLSS5 프리셋", "Préréglages DLSS5", "DLSS5-Voreinstellungen", "Preajustes DLSS5", "Пресеты DLSS5" }},
    {{ "Drop UTF-8 .ini files into the DLSS5-Presets\\ folder", "将 UTF-8 .ini 文件放入 DLSS5-Presets\\ 文件夹", "將 UTF-8 .ini 檔案放入 DLSS5-Presets\\ 資料夾", "UTF-8 の .ini ファイルを DLSS5-Presets\\ フォルダーに置いてください", "UTF-8 .ini 파일을 DLSS5-Presets\\ 폭더에 넣으세요", "Placez les fichiers .ini UTF-8 dans le dossier DLSS5-Presets\\", "Legen Sie UTF-8-.ini-Dateien in den Ordner DLSS5-Presets\\", "Coloca los archivos .ini UTF-8 en la carpeta DLSS5-Presets\\", "Поместите UTF-8 .ini в папку DLSS5-Presets\\" }},
    {{ "RenoDX sections are applied through ReShade config API.", "RenoDX 配置段通过 ReShade 配置 API 应用。", "RenoDX 設定區段會透過 ReShade 設定 API 套用。", "RenoDX セクションは ReShade 設定 API 経由で適用されます。", "RenoDX 섹션은 ReShade 구성 API를 통해 적용됩니다.", "Les sections RenoDX sont appliquées via l'API de configuration de ReShade.", "RenoDX-Abschnitte werden über die ReShade-Konfigurations-API angewendet.", "Las secciones de RenoDX se aplican mediante la API de configuración de ReShade.", "Разделы RenoDX применяются через API конфигурации ReShade." }},
    {{ "Auto-reload active file", "自动重新加载当前文件", "自動重新載入目前檔案", "アクティブなファイルを自動再読み込み", "활성 파일 자동 다시 로드", "Recharger automatiquement le fichier actif", "Aktive Datei automatisch neu laden", "Recargar automáticamente el archivo activo", "Автоматически перезагружать активный файл" }},
    {{ "Refresh files", "刷新文件", "重新整理檔案", "ファイルを更新", "파일 새로 고침", "Actualiser les fichiers", "Dateien aktualisieren", "Actualizar archivos", "Обновить файлы" }},
    {{ "No .ini presets found.", "未找到 .ini 预设。", "找不到 .ini 預設。", ".ini プリセットが見つかりません。", ".ini 프리셋을 찾을 수 없습니다.", "Aucun préréglage .ini trouvé.", "Keine .ini-Voreinstellungen gefunden.", "No se encontraron preajustes .ini.", "Пресеты .ini не найдены." }},
    {{ "Apply", "应用", "套用", "適用", "적용", "Appliquer", "Anwenden", "Aplicar", "Применить" }},
    {{ "Share", "分享", "分享", "共有", "공유", "Partager", "Teilen", "Compartir", "Поделиться" }},
    {{ "Quick share (one preset per code; compressed and clipboard-safe):", "快速分享（每个分享码包含一个预设；已压缩且适合剪贴板）：", "快速分享（每個分享碼包含一個預設；已壓縮且適合剪貼簿）：", "クイック共有（1コードにつき1プリセット、圧縮・クリップボード対応）：", "빠른 공유 (코드 하나에 프리셋 하나, 압축 및 클립보드 안전):", "Partage rapide (un préréglage par code, compressé et adapté au presse-papiers) :", "Schnellfreigabe (eine Voreinstellung pro Code, komprimiert und zwischenablagegeeignet):", "Compartición rápida (un preajuste por código; comprimido y compatible con el portapapeles):", "Быстрая отправка (один пресет на код; сжатие и безопасная вставка):" }},
    {{ "Import filename", "导入文件名", "匯入檔名", "インポートするファイル名", "가져올 파일 이름", "Nom du fichier importé", "Importdateiname", "Nombre del archivo importado", "Имя импортируемого файла" }},
    {{ "Copy code", "复制代码", "複製代碼", "コードをコピー", "코드 복사", "Copier le code", "Code kopieren", "Copiar código", "Скопировать код" }},
    {{ "Paste code", "粘贴代码", "貼上代碼", "コードを貼り付け", "코드 붙여넣기", "Coller le code", "Pegar código", "Pegar código", "Вставить код" }},
    {{ "Import preset", "导入预设", "匯入預設", "プリセットをインポート", "프리셋 가져오기", "Importer le préréglage", "Voreinstellung importieren", "Importar preajuste", "Импортировать пресет" }},
    {{ "Import & apply", "导入并应用", "匯入並套用", "インポートして適用", "가져와서 적용", "Importer et appliquer", "Importieren und anwenden", "Importar y aplicar", "Импортировать и применить" }},
    {{ "Share active", "分享当前预设", "分享目前預設", "アクティブを共有", "활성 프리셋 공유", "Partager l'actif", "Aktive teilen", "Compartir activo", "Поделиться активным" }},
    {{ "Code length", "代码长度", "代碼長度", "コード長", "코드 길이", "Longueur du code", "Codelänge", "Longitud del código", "Длина кода" }},
    {{ "Clear / restore original", "清除 / 恢复原始设置", "清除 / 還原原始設定", "クリア / 元の設定を復元", "지우기 / 원래 설정 복원", "Effacer / restaurer l'original", "Löschen / Original wiederherstellen", "Borrar / restaurar original", "Очистить / восстановить исходные" }},
    {{ "Active: {0}", "当前：{0}", "目前：{0}", "アクティブ: {0}", "활성: {0}", "Actif : {0}", "Aktiv: {0}", "Activo: {0}", "Активный: {0}" }},
    {{ "Ready", "就绪", "就緒", "準備完了", "준비됨", "Prêt", "Bereit", "Listo", "Готово" }},
    {{ "Cannot share this preset (empty, unreadable, or larger than 256 KiB)", "无法分享此预设（为空、无法读取或大于 256 KiB）", "無法分享此預設（為空、無法讀取或大於 256 KiB）", "このプリセットを共有できません（空、読み取れない、または256 KiB超過）", "이 프리셋을 공유할 수 없습니다 (비어 있거나 읽을 수 없거나 256 KiB 초과)", "Impossible de partager ce préréglage (vide, illisible ou supérieur à 256 Kio)", "Diese Voreinstellung kann nicht geteilt werden (leer, nicht lesbar oder größer als 256 KiB)", "No se puede compartir este preajuste (vacío, ilegible o mayor de 256 KiB)", "Не удалось поделиться пресетом (пустой, нечитаемый или больше 256 КиБ)" }},
    {{ "Share code is too large", "分享码过大", "分享碼過大", "共有コードが大きすぎます", "공유 코드가 너무 큽니다", "Le code de partage est trop volumineux", "Freigabecode ist zu groß", "El código compartido es demasiado grande", "Код слишком большой" }},
    {{ "Share code generated: {0} ({1} chars)", "已生成分享码：{0}（{1} 个字符）", "已產生分享碼：{0}（{1} 個字元）", "共有コードを生成: {0}（{1}文字）", "공유 코드 생성됨: {0} ({1}자)", "Code généré : {0} ({1} caractères)", "Freigabecode erstellt: {0} ({1} Zeichen)", "Código generado: {0} ({1} caracteres)", "Код создан: {0} ({1} симв.)" }},
    {{ "Generate or paste a share code first", "请先生成或粘贴分享码", "請先產生或貼上分享碼", "先に共有コードを生成または貼り付けてください", "먼저 공유 코드를 생성하거나 붙여넣으세요", "Générez ou collez d'abord un code", "Erstellen oder fügen Sie zuerst einen Freigabecode ein", "Genera o pega primero un código", "Сначала создайте или вставьте код" }},
    {{ "Share code copied to clipboard", "分享码已复制到剪贴板", "分享碼已複製到剪貼簿", "共有コードをクリップボードにコピーしました", "공유 코드를 클립보드에 복사했습니다", "Code copié dans le presse-papiers", "Freigabecode in die Zwischenablage kopiert", "Código copiado al portapapeles", "Код скопирован в буфер обмена" }},
    {{ "Clipboard is empty or the share code is too large", "剪贴板为空或分享码过大", "剪貼簿為空或分享碼過大", "クリップボードが空か、共有コードが大きすぎます", "클립보드가 비어 있거나 공유 코드가 너무 큽니다", "Le presse-papiers est vide ou le code est trop volumineux", "Zwischenablage ist leer oder der Freigabecode ist zu groß", "El portapapeles está vacío o el código es demasiado grande", "Буфер обмена пуст или код слишком большой" }},
    {{ "Share code pasted; click Import preset to receive it", "分享码已粘贴；点击“导入预设”接收", "分享碼已貼上；點擊「匯入預設」接收", "共有コードを貼り付けました。「プリセットをインポート」をクリックしてください", "공유 코드를 붙여넣었습니다. 프리셋 가져오기를 클릭하세요", "Code collé ; cliquez sur « Importer le préréglage » pour le recevoir", "Freigabecode eingefügt; klicken Sie auf „Voreinstellung importieren“", "Código pegado; haz clic en «Importar preajuste» para recibirlo", "Код вставлен; нажмите «Импортировать пресет»" }},
    {{ "Invalid or corrupted D5P1 share code", "D5P1 分享码无效或已损坏", "D5P1 分享碼無效或已損毀", "D5P1共有コードが無効または破損しています", "잘못되었거나 손상된 D5P1 공유 코드", "Code D5P1 invalide ou corrompu", "Ungültiger oder beschädigter D5P1-Freigabecode", "Código D5P1 no válido o dañado", "Недействительный или повреждённый код D5P1" }},
    {{ "Cannot create DLSS5-Presets directory", "无法创建 DLSS5-Presets 文件夹", "無法建立 DLSS5-Presets 資料夾", "DLSS5-Presets フォルダーを作成できません", "DLSS5-Presets 폴더를 만들 수 없습니다", "Impossible de créer le dossier DLSS5-Presets", "Ordner DLSS5-Presets kann nicht erstellt werden", "No se puede crear la carpeta DLSS5-Presets", "Не удалось создать папку DLSS5-Presets" }},
    {{ "Cannot write imported preset", "无法写入导入的预设", "無法寫入匯入的預設", "インポートしたプリセットを書き込めません", "가져온 프리셋을 쓸 수 없습니다", "Impossible d'écrire le préréglage importé", "Importierte Voreinstellung kann nicht geschrieben werden", "No se puede escribir el preajuste importado", "Не удалось записать импортированный пресет" }},
    {{ "Imported: {0}", "已导入：{0}", "已匯入：{0}", "インポート済み: {0}", "가져옴: {0}", "Importé : {0}", "Importiert: {0}", "Importado: {0}", "Импортировано: {0}" }},
    {{ "Cannot read preset (use UTF-8 INI): {0}", "无法读取预设（请使用 UTF-8 INI）：{0}", "無法讀取預設（請使用 UTF-8 INI）：{0}", "プリセットを読み取れません（UTF-8 INIを使用）: {0}", "프리셋을 읽을 수 없습니다 (UTF-8 INI 사용): {0}", "Impossible de lire le préréglage (utilisez un INI UTF-8) : {0}", "Voreinstellung kann nicht gelesen werden (UTF-8-INI verwenden): {0}", "No se puede leer el preajuste (usa un INI UTF-8): {0}", "Не удалось прочитать пресет (используйте UTF-8 INI): {0}" }},
    {{ "Auto-reloaded: {0}", "已自动重新加载：{0}", "已自動重新載入：{0}", "自動再読み込み: {0}", "자동 다시 로드됨: {0}", "Rechargé automatiquement : {0}", "Automatisch neu geladen: {0}", "Recargado automáticamente: {0}", "Автоматически перезагружено: {0}" }},
    {{ "Applied: {0}", "已应用：{0}", "已套用：{0}", "適用済み: {0}", "적용됨: {0}", "Appliqué : {0}", "Angewendet: {0}", "Aplicado: {0}", "Применено: {0}" }},
    {{ "Cleared; original ReShade/RenoDX settings restored", "已清除；已恢复原始 ReShade/RenoDX 设置", "已清除；已還原原始 ReShade/RenoDX 設定", "クリアしました。元の ReShade/RenoDX 設定を復元しました", "지웠습니다. 원래 ReShade/RenoDX 설정을 복원했습니다", "Effacé ; paramètres ReShade/RenoDX d'origine restaurés", "Gelöscht; ursprüngliche ReShade/RenoDX-Einstellungen wiederhergestellt", "Borrado; se restauró la configuración original de ReShade/RenoDX", "Очищено; исходные настройки ReShade/RenoDX восстановлены" }},
};

Language g_language = Language::English;
std::string g_language_source;

const char *Tr(TextId id)
{
    const size_t index = static_cast<size_t>(id);
    const size_t language = static_cast<size_t>(g_language);
    return kTranslations[index].text[language < 9 ? language : 0];
}

std::string FormatText(TextId id, const std::string &first = {}, const std::string &second = {})
{
    std::string result = Tr(id);
    const auto replace = [&result](const char *token, const std::string &value) {
        const size_t position = result.find(token);
        if (position != std::string::npos)
            result.replace(position, std::strlen(token), value);
    };
    replace("{0}", first);
    replace("{1}", second);
    return result;
}

fs::path g_addon_dir;
std::vector<fs::path> g_presets;
std::string g_status = "Ready";
std::string g_active_name;
fs::path g_active_path;
fs::file_time_type g_active_write_time{};
bool g_auto_reload = true;
bool g_registered = false;
std::string g_overlay_title;

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
    // Presets are only scanned inside the DLSS5-Presets subfolder, which is
    // scanned recursively so presets can be organized per game or author.
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

std::string PreferredSystemLanguage()
{
    ULONG count = 0;
    ULONG size = 0;
    if (!GetThreadPreferredUILanguages(MUI_LANGUAGE_NAME | MUI_UI_FALLBACK, &count, nullptr, &size) || size == 0)
        return "en-US";

    std::vector<wchar_t> languages(size);
    if (!GetThreadPreferredUILanguages(MUI_LANGUAGE_NAME | MUI_UI_FALLBACK, &count,
        languages.data(), &size) || count == 0)
        return "en-US";

    const wchar_t *first = languages.data();
    const int length = WideCharToMultiByte(CP_UTF8, 0, first, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
        return "en-US";
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, first, -1, result.data(), length, nullptr, nullptr);
    result.resize(static_cast<size_t>(length - 1));
    return result;
}

Language LanguageFromCode(std::string code)
{
    code = Lower(Trim(code));
    std::replace(code.begin(), code.end(), '_', '-');
    if (code.rfind("zh-tw", 0) == 0 || code.rfind("zh-hk", 0) == 0 ||
        code.rfind("zh-hant", 0) == 0 || code.find("traditional") != std::string::npos)
        return Language::TraditionalChinese;
    if (code.rfind("zh", 0) == 0 || code.find("simplified") != std::string::npos)
        return Language::SimplifiedChinese;
    if (code.rfind("ja", 0) == 0)
        return Language::Japanese;
    if (code.rfind("ko", 0) == 0)
        return Language::Korean;
    if (code.rfind("fr", 0) == 0)
        return Language::French;
    if (code.rfind("de", 0) == 0)
        return Language::German;
    if (code.rfind("es", 0) == 0)
        return Language::Spanish;
    if (code.rfind("ru", 0) == 0)
        return Language::Russian;
    return Language::English;
}

void UpdateLanguage()
{
    std::string selected;
    if (!GetGlobalValue("OVERLAY", "Language", selected) || Trim(selected).empty())
        selected = PreferredSystemLanguage();
    selected = Trim(selected);
    if (selected.empty())
        selected = "en-US";

    if (selected == g_language_source)
        return;
    g_language_source = selected;
    g_language = LanguageFromCode(selected);
    // Do not leave a stale status message in the previous language after the
    // user changes ReShade's language in the overlay settings.
    g_status = Tr(TextId::Ready);
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
        SetStatus(Tr(TextId::CannotShare), reshade::log::level::warning);
        return;
    }
    if (!SetShareCodeText(EncodeShareCode(preset))) {
        SetStatus(Tr(TextId::ShareCodeTooLarge), reshade::log::level::warning);
        return;
    }
    SetStatus(FormatText(TextId::ShareGenerated, Utf8(path.filename()),
        std::to_string(ShareCodeText().size())));
}

void CopyShareCode()
{
    const std::string code = ShareCodeText();
    if (code.empty()) {
        SetStatus(Tr(TextId::GenerateFirst), reshade::log::level::warning);
        return;
    }
    ImGui::SetClipboardText(code.c_str());
    SetStatus(Tr(TextId::ShareCopied));
}

void PasteShareCode()
{
    const char *clipboard = ImGui::GetClipboardText();
    if (clipboard == nullptr || !SetShareCodeText(clipboard)) {
        SetStatus(Tr(TextId::ClipboardInvalid), reshade::log::level::warning);
        return;
    }
    SetStatus(Tr(TextId::SharePasted));
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
        SetStatus(Tr(TextId::InvalidShareCode), reshade::log::level::warning);
        return;
    }
    const fs::path directory = g_addon_dir / kPresetFolder;
    std::error_code error;
    fs::create_directories(directory, error);
    if (error) {
        SetStatus(Tr(TextId::CreateDirectory), reshade::log::level::warning);
        return;
    }
    const fs::path target = UniquePresetPath(directory, SafePresetName(g_import_name.data(), checksum));
    if (!WritePreset(target, preset)) {
        SetStatus(Tr(TextId::WriteImported), reshade::log::level::warning);
        return;
    }
    RefreshPresets();
    if (apply)
        ApplyPreset(runtime, target, false);
    else
        SetStatus(FormatText(TextId::Imported, Utf8(target.filename())));
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
        SetStatus(FormatText(TextId::CannotRead, Utf8(path.filename())), reshade::log::level::warning);
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
    SetStatus(FormatText(automatic ? TextId::AutoReloaded : TextId::Applied, g_active_name));
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
    SetStatus(Tr(TextId::Cleared));
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
    UpdateLanguage();
    RefreshPresets();
    AutoReloadIfChanged(runtime);

    ImGui::Text("%s", Tr(TextId::DropHint));
    ImGui::Text("%s", Tr(TextId::RenoHint));
    ImGui::Checkbox(Tr(TextId::AutoReload), &g_auto_reload);
    ImGui::SameLine();
    if (ImGui::Button(Tr(TextId::Refresh)))
        RefreshPresets();
    ImGui::Separator();

    if (g_presets.empty()) {
        ImGui::TextDisabled("%s", Tr(TextId::NoPresets));
    } else {
        for (size_t i = 0; i < g_presets.size(); ++i) {
            const fs::path &path = g_presets[i];
            const bool active = !g_active_path.empty() && Lower(Utf8(path)) == Lower(Utf8(g_active_path));
            const std::string label = Utf8(path.lexically_relative(g_addon_dir));
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(label.c_str(), active))
                ApplyPreset(runtime, path, false);
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr(TextId::Apply)))
                ApplyPreset(runtime, path, false);
            ImGui::SameLine();
            if (ImGui::SmallButton(Tr(TextId::Share)))
                SharePreset(path);
            ImGui::PopID();
        }
    }

    ImGui::Separator();
    ImGui::Text("%s", Tr(TextId::QuickShare));
    ImGui::InputText(Tr(TextId::ImportFilename), g_import_name.data(), g_import_name.size());
    ImGui::InputTextMultiline("##dlss5_share_code", g_share_code.data(), g_share_code.size(), ImVec2(-1, 120));
    if (ImGui::Button(Tr(TextId::CopyCode)))
        CopyShareCode();
    ImGui::SameLine();
    if (ImGui::Button(Tr(TextId::PasteCode)))
        PasteShareCode();
    ImGui::SameLine();
    if (ImGui::Button(Tr(TextId::ImportPreset)))
        ImportShareCode(runtime, false);
    ImGui::SameLine();
    if (ImGui::Button(Tr(TextId::ImportAndApply)))
        ImportShareCode(runtime, true);
    if (!g_active_path.empty()) {
        ImGui::SameLine();
        if (ImGui::Button(Tr(TextId::ShareActive)))
            SharePreset(g_active_path);
    }
    ImGui::Text("%s: %zu", Tr(TextId::CodeLength), ShareCodeText().size());

    ImGui::Separator();
    if (ImGui::Button(Tr(TextId::ClearRestore)))
        ClearPreset(runtime);
    ImGui::SameLine();
    ImGui::Text("%s", g_status.c_str());
    if (!g_active_name.empty())
        ImGui::Text("%s", FormatText(TextId::Active, g_active_name).c_str());
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
        UpdateLanguage();
        g_overlay_title = Tr(TextId::OverlayTitle);
        reshade::register_overlay(g_overlay_title.c_str(), DrawOverlay);
        Log(std::string(kName) + " " + kVersion + " loaded; " + std::to_string(g_presets.size()) + " preset(s)");
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        if (g_registered) {
            reshade::unregister_overlay(g_overlay_title.c_str(), DrawOverlay);
            reshade::unregister_addon(module);
            g_registered = false;
        }
    }
    return TRUE;
}
