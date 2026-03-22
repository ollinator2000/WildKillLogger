#include "API/ARK/Ark.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

namespace WildKillLogger
{
    // =========================
    // Konfiguration
    // =========================
    //
    // Diese Struktur enthält alle Laufzeitoptionen des Plugins.
    // Die Werte werden beim Start aus ArkApi/Plugins/WildKillLogger/config.json
    // geladen. Falls die Datei fehlt, wird automatisch eine Default-Datei erzeugt.
    //
    struct Config
    {
        float kill_radius = 3000.0f;
        int position_update_ms = 2000;
        int player_fresh_seconds = 15;

        bool write_debug_log = true;
        bool write_only_high_confidence = true;
        bool debug_log_non_dino_destroy = false;
        bool debug_log_skipped_kills = true;

        std::string kill_csv_filename = "wild_kills.csv";
        std::string debug_log_filename = "wildkilllogger_debug.log";
    };

    // Snapshot eines aktuell bekannten Spielers.
    // Die Daten werden beim Join angelegt und danach zyklisch aktualisiert.
    struct PlayerSnapshot
    {
        AShooterPlayerController* controller = nullptr;
        AShooterCharacter* character = nullptr;
        std::string eos_id;
        std::string character_name;
        FVector position{0.f, 0.f, 0.f};
        bool has_position = false;
        std::time_t last_seen = 0;
    };

    std::mutex g_data_mutex;
    std::mutex g_file_mutex;

    std::unordered_map<uint64_t, PlayerSnapshot> g_players;

    std::atomic<bool> g_running{false};
    std::thread g_position_thread;

    bool g_destroy_hook_active = false;
    bool g_join_hook_active = false;
    bool g_kill_csv_header_written = false;

    Config g_config;

    // -------------------------
    // Pfade
    // -------------------------
    std::string GetPluginDir()
    {
        // Wichtig: Das Arbeitsverzeichnis des Plugins liegt zur Laufzeit
        // unter ShooterGame/Binaries/Win64.
        // Deshalb reicht ein relativer Pfad ab dort.
        return "ArkApi/Plugins/WildKillLogger/";
    }

    std::string GetConfigPath()
    {
        return GetPluginDir() + "config.json";
    }

    std::string GetKillCsvPath()
    {
        return GetPluginDir() + g_config.kill_csv_filename;
    }

    std::string GetDebugLogPath()
    {
        return GetPluginDir() + g_config.debug_log_filename;
    }

    // -------------------------
    // Utility
    // -------------------------
    std::string ToUtf8(const FString& str)
    {
        return TCHAR_TO_UTF8(*str);
    }

    std::string CsvEscape(const std::string& input)
    {
        std::string out = "\"";
        for (char c : input)
        {
            if (c == '"')
                out += "\"\"";
            else
                out += c;
        }
        out += "\"";
        return out;
    }

    std::string IsoNowUtc()
    {
        std::time_t now = std::time(nullptr);
        std::tm utc_tm{};

    #ifdef _WIN32
        gmtime_s(&utc_tm, &now);
    #else
        gmtime_r(&now, &utc_tm);
    #endif

        char timestamp[32]{};
        std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc_tm);
        return timestamp;
    }

    bool Contains(const std::string& text, const std::string& needle)
    {
        return text.find(needle) != std::string::npos;
    }

    // -------------------------
    // Einfacher JSON-Reader
    // -------------------------
    //
    // Der Parser ist absichtlich klein gehalten und liest nur die konkreten
    // Felder, die das Plugin benötigt. So kommt das Plugin ohne zusätzliche
    // JSON-Bibliothek aus.
    //
    std::string ReadTextFile(const std::string& path)
    {
        std::ifstream file(path);
        if (!file.is_open())
            return "";

        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    std::string RegexExtractString(const std::string& text, const std::string& key, const std::string& fallback)
    {
        try
        {
            const std::regex re("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
            std::smatch match;
            if (std::regex_search(text, match, re) && match.size() > 1)
                return match[1].str();
        }
        catch (...)
        {
        }

        return fallback;
    }

    int RegexExtractInt(const std::string& text, const std::string& key, int fallback)
    {
        try
        {
            const std::regex re("\"" + key + "\"\\s*:\\s*(-?\\d+)");
            std::smatch match;
            if (std::regex_search(text, match, re) && match.size() > 1)
                return std::stoi(match[1].str());
        }
        catch (...)
        {
        }

        return fallback;
    }

    float RegexExtractFloat(const std::string& text, const std::string& key, float fallback)
    {
        try
        {
            const std::regex re("\"" + key + "\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?)");
            std::smatch match;
            if (std::regex_search(text, match, re) && match.size() > 1)
                return std::stof(match[1].str());
        }
        catch (...)
        {
        }

        return fallback;
    }

    bool RegexExtractBool(const std::string& text, const std::string& key, bool fallback)
    {
        try
        {
            const std::regex re("\"" + key + "\"\\s*:\\s*(true|false)");
            std::smatch match;
            if (std::regex_search(text, match, re) && match.size() > 1)
                return match[1].str() == "true";
        }
        catch (...)
        {
        }

        return fallback;
    }

    void WriteDefaultConfigIfMissing()
    {
        std::ifstream test(GetConfigPath());
        if (test.is_open())
            return;

        std::ofstream file(GetConfigPath(), std::ios::out);
        if (!file.is_open())
            return;

        file << "{\n"
             << "  \"kill_radius\": 3000.0,\n"
             << "  \"position_update_ms\": 2000,\n"
             << "  \"player_fresh_seconds\": 15,\n"
             << "  \"write_debug_log\": true,\n"
             << "  \"write_only_high_confidence\": true,\n"
             << "  \"debug_log_non_dino_destroy\": false,\n"
             << "  \"debug_log_skipped_kills\": true,\n"
             << "  \"kill_csv_filename\": \"wild_kills.csv\",\n"
             << "  \"debug_log_filename\": \"wildkilllogger_debug.log\"\n"
             << "}\n";
    }

    void LoadConfig()
    {
        WriteDefaultConfigIfMissing();

        const std::string text = ReadTextFile(GetConfigPath());
        if (text.empty())
            return;

        Config cfg;
        cfg.kill_radius = RegexExtractFloat(text, "kill_radius", cfg.kill_radius);
        cfg.position_update_ms = RegexExtractInt(text, "position_update_ms", cfg.position_update_ms);
        cfg.player_fresh_seconds = RegexExtractInt(text, "player_fresh_seconds", cfg.player_fresh_seconds);
        cfg.write_debug_log = RegexExtractBool(text, "write_debug_log", cfg.write_debug_log);
        cfg.write_only_high_confidence = RegexExtractBool(text, "write_only_high_confidence", cfg.write_only_high_confidence);
        cfg.debug_log_non_dino_destroy = RegexExtractBool(text, "debug_log_non_dino_destroy", cfg.debug_log_non_dino_destroy);
        cfg.debug_log_skipped_kills = RegexExtractBool(text, "debug_log_skipped_kills", cfg.debug_log_skipped_kills);
        cfg.kill_csv_filename = RegexExtractString(text, "kill_csv_filename", cfg.kill_csv_filename);
        cfg.debug_log_filename = RegexExtractString(text, "debug_log_filename", cfg.debug_log_filename);

        g_config = cfg;
    }

    // -------------------------
    // Debug Logging
    // -------------------------
    //
    // Schreibt bewusst in eine eigene Datei neben der CSV.
    // Damit bleiben Nutzdaten und Diagnose-Daten getrennt.
    //
    void DebugLog(const std::string& message)
    {
        if (!g_config.write_debug_log)
            return;

        const std::string timestamp = IsoNowUtc();

        std::lock_guard<std::mutex> lock(g_file_mutex);

        std::ofstream file(GetDebugLogPath(), std::ios::app);
        if (!file.is_open())
            return;

        file << "[" << timestamp << "] " << message << "\n";
    }

    // -------------------------
    // Dino-Filter
    // -------------------------
    //
    // ASA feuert im Destroy-Hook auch für viele Objekte, die keine Dinos sind:
    // Buffs, Crates, Dropped Items, AI Controller etc.
    // Deshalb filtern wir bewusst über Blueprint-Pfade.
    //
    bool IsLikelyDinoCharacterBlueprint(const std::string& blueprint)
    {
        if (!Contains(blueprint, "/Dinos/"))
            return false;

        if (!Contains(blueprint, "_Character_BP"))
            return false;

        if (Contains(blueprint, "AIController"))
            return false;

        if (Contains(blueprint, "Buff_"))
            return false;

        if (Contains(blueprint, "SupplyCrate"))
            return false;

        if (Contains(blueprint, "ArtifactCrate"))
            return false;

        if (Contains(blueprint, "DroppedItem"))
            return false;

        return true;
    }

    std::string GetBlueprintPath(AActor* actor)
    {
        if (!actor || !actor->ClassPrivateField())
            return "";

        try
        {
            return ToUtf8(AsaApi::GetApiUtils().GetClassBlueprint(actor->ClassPrivateField()));
        }
        catch (...)
        {
            return "";
        }
    }

    std::string GetPlayerName(AShooterPlayerController* controller)
    {
        if (!controller)
            return "";

        try
        {
            FString name;
            controller->GetPlayerCharacterName(&name);
            return ToUtf8(name);
        }
        catch (...)
        {
            return "";
        }
    }

    std::string GetPlayerEosId(AShooterPlayerController* controller)
    {
        if (!controller)
            return "";

        try
        {
            FString eos;
            controller->GetUniqueNetIdAsString(&eos);
            return ToUtf8(eos);
        }
        catch (...)
        {
            return "";
        }
    }

    bool TryGetActorPosition(AActor* actor, FVector& out_pos)
    {
        if (!actor)
            return false;

        try
        {
            if (actor->RootComponentField())
            {
                out_pos = actor->RootComponentField()->RelativeLocationField();
                return true;
            }
        }
        catch (...)
        {
        }

        return false;
    }

    bool TryGetPlayerPosition(AShooterCharacter* character, FVector& out_pos)
    {
        if (!character)
            return false;

        try
        {
            if (character->RootComponentField())
            {
                out_pos = character->RootComponentField()->RelativeLocationField();
                return true;
            }
        }
        catch (...)
        {
        }

        return false;
    }

    double Distance(const FVector& a, const FVector& b)
    {
        const double dx = static_cast<double>(a.X) - static_cast<double>(b.X);
        const double dy = static_cast<double>(a.Y) - static_cast<double>(b.Y);
        const double dz = static_cast<double>(a.Z) - static_cast<double>(b.Z);
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    // -------------------------
    // CSV
    // -------------------------
    //
    // In die CSV kommen nur verlässliche Kills.
    // Unsichere Fälle landen ausschließlich im Debug-Log.
    //
    void EnsureKillCsvHeader()
    {
        std::lock_guard<std::mutex> lock(g_file_mutex);

        if (g_kill_csv_header_written)
            return;

        std::ofstream file(GetKillCsvPath(), std::ios::app);
        if (!file.is_open())
        {
            Log::GetLog()->error("WildKillLogger: failed to open wild_kills.csv for header");
            return;
        }

        if (file.tellp() == 0)
        {
            file << "timestamp_utc,dino_blueprint,dino_x,dino_y,dino_z,killer_eos,killer_name,nearest_distance\n";
        }

        g_kill_csv_header_written = true;
    }

    void AppendReliableKill(
        const std::string& dino_blueprint,
        const FVector& dino_pos,
        const std::string& killer_eos,
        const std::string& killer_name,
        double nearest_distance)
    {
        EnsureKillCsvHeader();

        std::lock_guard<std::mutex> lock(g_file_mutex);

        std::ofstream file(GetKillCsvPath(), std::ios::app);
        if (!file.is_open())
        {
            Log::GetLog()->error("WildKillLogger: failed to append wild_kills.csv");
            DebugLog("ERROR failed to append wild_kills.csv");
            return;
        }

        file
            << CsvEscape(IsoNowUtc()) << ","
            << CsvEscape(dino_blueprint) << ","
            << dino_pos.X << ","
            << dino_pos.Y << ","
            << dino_pos.Z << ","
            << CsvEscape(killer_eos) << ","
            << CsvEscape(killer_name) << ","
            << nearest_distance
            << "\n";
    }

    // -------------------------
    // Player Tracking
    // -------------------------
    //
    // Beim Join speichern wir:
    // - Controller
    // - Character
    // - EOS ID
    // - Character Name
    // - Position
    //
    // Danach aktualisiert ein Thread regelmäßig die Positionen.
    //
    void AddOrUpdatePlayer(
        AShooterPlayerController* controller,
        AShooterCharacter* character)
    {
        if (!controller)
            return;

        PlayerSnapshot snapshot;
        snapshot.controller = controller;
        snapshot.character = character;
        snapshot.eos_id = GetPlayerEosId(controller);
        snapshot.character_name = GetPlayerName(controller);
        snapshot.last_seen = std::time(nullptr);
        snapshot.has_position = TryGetPlayerPosition(character, snapshot.position);

        const auto key = reinterpret_cast<uint64_t>(controller);

        {
            std::lock_guard<std::mutex> lock(g_data_mutex);
            g_players[key] = snapshot;
        }

        DebugLog(
            "PLAYER_TRACKED eos_id=" + snapshot.eos_id +
            " name=" + snapshot.character_name +
            " has_position=" + std::string(snapshot.has_position ? "true" : "false"));
    }

    void UpdatePlayerPositionsLoop()
    {
        while (g_running.load())
        {
            {
                std::lock_guard<std::mutex> lock(g_data_mutex);

                for (auto& [key, player] : g_players)
                {
                    if (!player.character)
                        continue;

                    FVector pos{0.f, 0.f, 0.f};
                    if (TryGetPlayerPosition(player.character, pos))
                    {
                        player.position = pos;
                        player.has_position = true;
                        player.last_seen = std::time(nullptr);

                        if (player.character_name.empty() && player.controller)
                            player.character_name = GetPlayerName(player.controller);

                        if (player.eos_id.empty() && player.controller)
                            player.eos_id = GetPlayerEosId(player.controller);
                    }
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(g_config.position_update_ms));
        }
    }

    // -------------------------
    // Kill Attribution
    // -------------------------
    //
    // Grundidee:
    // 1. Destroyed-Event eines Dinos
    // 2. Position des Dinos bestimmen
    // 3. Alle "frischen" Spieler im Radius suchen
    // 4. Wenn genau 1 Spieler im Radius ist -> high confidence
    // 5. Nur high confidence wird in die CSV geschrieben
    //
    void AttributeDinoDestroy(AActor* actor)
    {
        if (!actor)
            return;

        const std::string blueprint = GetBlueprintPath(actor);
        if (!IsLikelyDinoCharacterBlueprint(blueprint))
        {
            if (g_config.debug_log_non_dino_destroy)
                DebugLog("NON_DINO_DESTROY blueprint=" + blueprint);
            return;
        }

        FVector dino_pos{0.f, 0.f, 0.f};
        const bool has_dino_pos = TryGetActorPosition(actor, dino_pos);

        std::string killer_eos = "unknown";
        std::string killer_name = "unknown";
        std::string confidence = "unknown";
        int nearby_count = 0;
        double nearest_distance = -1.0;

        if (has_dino_pos)
        {
            std::lock_guard<std::mutex> lock(g_data_mutex);

            const std::time_t now = std::time(nullptr);

            double best_distance = 1e18;
            PlayerSnapshot* best_player = nullptr;

            for (auto& [key, player] : g_players)
            {
                if (!player.has_position)
                    continue;

                if ((now - player.last_seen) > g_config.player_fresh_seconds)
                    continue;

                const double dist = Distance(player.position, dino_pos);
                if (dist <= g_config.kill_radius)
                {
                    nearby_count++;

                    if (dist < best_distance)
                    {
                        best_distance = dist;
                        best_player = &player;
                    }
                }
            }

            if (best_player)
            {
                nearest_distance = best_distance;
                killer_eos = best_player->eos_id.empty() ? "unknown" : best_player->eos_id;
                killer_name = best_player->character_name.empty() ? "unknown" : best_player->character_name;

                if (nearby_count == 1)
                    confidence = "high";
                else
                    confidence = "low";
            }
        }

        DebugLog(
            "DINO_DESTROY blueprint=" + blueprint +
            " x=" + std::to_string(dino_pos.X) +
            " y=" + std::to_string(dino_pos.Y) +
            " z=" + std::to_string(dino_pos.Z) +
            " killer_eos=" + killer_eos +
            " killer_name=" + killer_name +
            " confidence=" + confidence +
            " nearby_count=" + std::to_string(nearby_count) +
            " nearest_distance=" + std::to_string(nearest_distance));

        const bool should_write =
            (confidence == "high") ||
            (!g_config.write_only_high_confidence && confidence != "unknown");

        if (should_write)
        {
            AppendReliableKill(
                blueprint,
                dino_pos,
                killer_eos,
                killer_name,
                nearest_distance
            );

            DebugLog("KILL_WRITTEN blueprint=" + blueprint + " killer_name=" + killer_name);
            Log::GetLog()->info("WildKillLogger: reliable kill {} -> {}", blueprint, killer_name);
        }
        else if (g_config.debug_log_skipped_kills)
        {
            DebugLog("KILL_SKIPPED blueprint=" + blueprint + " reason=confidence_" + confidence);
        }
    }
}

// =========================
// Hooks
// =========================
DECLARE_HOOK(
    AShooterGameMode_HandleNewPlayer,
    bool,
    AShooterGameMode*,
    AShooterPlayerController*,
    UPrimalPlayerData*,
    AShooterCharacter*,
    bool
);

DECLARE_HOOK(AActor_Destroyed, void, AActor*);

bool Hook_AShooterGameMode_HandleNewPlayer(
    AShooterGameMode* _this,
    AShooterPlayerController* new_player,
    UPrimalPlayerData* player_data,
    AShooterCharacter* player_character,
    bool is_from_login)
{
    WildKillLogger::AddOrUpdatePlayer(new_player, player_character);

    return AShooterGameMode_HandleNewPlayer_original(
        _this,
        new_player,
        player_data,
        player_character,
        is_from_login
    );
}

void Hook_AActor_Destroyed(AActor* _this)
{
    try
    {
        WildKillLogger::AttributeDinoDestroy(_this);
    }
    catch (...)
    {
        Log::GetLog()->error("WildKillLogger: exception in Hook_AActor_Destroyed");
        WildKillLogger::DebugLog("ERROR exception in Hook_AActor_Destroyed");
    }

    AActor_Destroyed_original(_this);
}

// =========================
// Plugin Lifecycle
// =========================
extern "C" __declspec(dllexport) void Plugin_Init()
{
    Log::Get().Init("WildKillLogger");

    // Erst Konfiguration laden, damit Dateinamen und Laufzeitverhalten
    // schon vor dem ersten Debug-Eintrag bekannt sind.
    WildKillLogger::LoadConfig();

    Log::GetLog()->info("WildKillLogger loaded");
    WildKillLogger::DebugLog("PLUGIN_INIT");
    WildKillLogger::DebugLog(
        "CONFIG kill_radius=" + std::to_string(WildKillLogger::g_config.kill_radius) +
        " position_update_ms=" + std::to_string(WildKillLogger::g_config.position_update_ms) +
        " player_fresh_seconds=" + std::to_string(WildKillLogger::g_config.player_fresh_seconds) +
        " write_only_high_confidence=" + std::string(WildKillLogger::g_config.write_only_high_confidence ? "true" : "false"));

    try
    {
        AsaApi::GetHooks().SetHook(
            "AShooterGameMode.HandleNewPlayer_Implementation(AShooterPlayerController*,UPrimalPlayerData*,AShooterCharacter*,bool)",
            &Hook_AShooterGameMode_HandleNewPlayer,
            &AShooterGameMode_HandleNewPlayer_original
        );
        WildKillLogger::g_join_hook_active = true;
        Log::GetLog()->info("WildKillLogger: hook HandleNewPlayer set successfully");
        WildKillLogger::DebugLog("HOOK_OK HandleNewPlayer");
    }
    catch (...)
    {
        WildKillLogger::g_join_hook_active = false;
        Log::GetLog()->error("WildKillLogger: failed to set hook HandleNewPlayer");
        WildKillLogger::DebugLog("HOOK_FAIL HandleNewPlayer");
    }

    try
    {
        AsaApi::GetHooks().SetHook(
            "AActor.Destroyed()",
            &Hook_AActor_Destroyed,
            &AActor_Destroyed_original
        );
        WildKillLogger::g_destroy_hook_active = true;
        Log::GetLog()->info("WildKillLogger: hook Destroyed set successfully");
        WildKillLogger::DebugLog("HOOK_OK Destroyed");
    }
    catch (...)
    {
        WildKillLogger::g_destroy_hook_active = false;
        Log::GetLog()->error("WildKillLogger: failed to set Destroyed hook");
        WildKillLogger::DebugLog("HOOK_FAIL Destroyed");
    }

    WildKillLogger::g_running = true;
    WildKillLogger::g_position_thread = std::thread(WildKillLogger::UpdatePlayerPositionsLoop);
    WildKillLogger::DebugLog("POSITION_THREAD_STARTED");
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    WildKillLogger::DebugLog("PLUGIN_UNLOAD_BEGIN");

    WildKillLogger::g_running = false;

    if (WildKillLogger::g_position_thread.joinable())
        WildKillLogger::g_position_thread.join();

    if (WildKillLogger::g_join_hook_active)
    {
        try
        {
            AsaApi::GetHooks().DisableHook(
                "AShooterGameMode.HandleNewPlayer_Implementation(AShooterPlayerController*,UPrimalPlayerData*,AShooterCharacter*,bool)",
                &Hook_AShooterGameMode_HandleNewPlayer
            );
            WildKillLogger::DebugLog("HOOK_DISABLED HandleNewPlayer");
        }
        catch (...)
        {
            WildKillLogger::DebugLog("HOOK_DISABLE_FAIL HandleNewPlayer");
        }
    }

    if (WildKillLogger::g_destroy_hook_active)
    {
        try
        {
            AsaApi::GetHooks().DisableHook(
                "AActor.Destroyed()",
                &Hook_AActor_Destroyed
            );
            WildKillLogger::DebugLog("HOOK_DISABLED Destroyed");
        }
        catch (...)
        {
            WildKillLogger::DebugLog("HOOK_DISABLE_FAIL Destroyed");
        }
    }

    Log::GetLog()->info("WildKillLogger unloaded");
    WildKillLogger::DebugLog("PLUGIN_UNLOAD_END");
}
