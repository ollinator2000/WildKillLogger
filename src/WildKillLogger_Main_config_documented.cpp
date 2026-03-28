// Build marker:
// branch: perf_tuning
// commit: 2c456078f4450ecadf3678a9587d0a4d82747e09
// commit_timestamp: 2026-03-28T10:28:26+01:00

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
#include <deque>
#include <unordered_map>
#include <utility>
#include <vector>

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
        int stale_player_seconds = 86400;

        // Safety switch for Linux+Wine environments.
        // false = no background UE object access (safer, less precise tracking).
        bool enable_position_thread = false;
        bool safe_mode_no_background_ue_access = true;

        bool write_debug_log = false;
        bool write_only_high_confidence = true;
        bool debug_log_non_dino_destroy = false;
        bool debug_log_skipped_kills = true;
        int debug_log_sample_rate = 1;
        bool write_rejected_kills = false;
        bool write_forensics_log = true;
        int forensics_ring_size = 256;

        std::string kill_csv_filename = "wild_kills.csv";
        std::string rejected_csv_filename = "rejected_kills.csv";
        std::string debug_log_filename = "wildkilllogger_debug.log";
        std::string forensics_log_filename = "wildkilllogger_forensics.log";
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
        int failed_position_reads = 0;
    };

    std::mutex g_data_mutex;
    std::mutex g_file_mutex;
    std::mutex g_blueprint_cache_mutex;
    std::mutex g_forensics_mutex;

    std::unordered_map<uint64_t, PlayerSnapshot> g_players;
    std::unordered_map<uint64_t, std::pair<std::string, bool>> g_blueprint_cache;
    std::deque<std::string> g_forensics_ring;

    std::atomic<bool> g_running{false};
    std::thread g_position_thread;

    bool g_destroy_hook_active = false;
    bool g_join_hook_active = false;
    bool g_kill_csv_header_written = false;
    bool g_rejected_csv_header_written = false;
    bool g_compat_enable_thread_default_applied = false;
    std::atomic<uint64_t> g_dino_destroy_log_counter{0};
    std::atomic<uint64_t> g_kill_skipped_log_counter{0};
    std::atomic<uint64_t> g_forensics_event_counter{0};

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

    std::string GetRejectedCsvPath()
    {
        return GetPluginDir() + g_config.rejected_csv_filename;
    }

    std::string GetForensicsLogPath()
    {
        return GetPluginDir() + g_config.forensics_log_filename;
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
             << "  \"stale_player_seconds\": 86400,\n"
             << "  \"enable_position_thread\": false,\n"
             << "  \"safe_mode_no_background_ue_access\": true,\n"
             << "  \"write_debug_log\": false,\n"
             << "  \"write_only_high_confidence\": true,\n"
             << "  \"debug_log_non_dino_destroy\": false,\n"
             << "  \"debug_log_skipped_kills\": true,\n"
             << "  \"debug_log_sample_rate\": 1,\n"
             << "  \"write_rejected_kills\": false,\n"
             << "  \"write_forensics_log\": true,\n"
             << "  \"forensics_ring_size\": 256,\n"
             << "  \"kill_csv_filename\": \"wild_kills.csv\",\n"
             << "  \"rejected_csv_filename\": \"rejected_kills.csv\",\n"
             << "  \"debug_log_filename\": \"wildkilllogger_debug.log\",\n"
             << "  \"forensics_log_filename\": \"wildkilllogger_forensics.log\"\n"
             << "}\n";
    }

    void LoadConfig()
    {
        WriteDefaultConfigIfMissing();

        const std::string text = ReadTextFile(GetConfigPath());
        if (text.empty())
            return;

        g_compat_enable_thread_default_applied = false;

        Config cfg;
        cfg.kill_radius = RegexExtractFloat(text, "kill_radius", cfg.kill_radius);
        cfg.position_update_ms = RegexExtractInt(text, "position_update_ms", cfg.position_update_ms);
        cfg.player_fresh_seconds = RegexExtractInt(text, "player_fresh_seconds", cfg.player_fresh_seconds);
        cfg.stale_player_seconds = RegexExtractInt(text, "stale_player_seconds", cfg.stale_player_seconds);
        cfg.enable_position_thread = RegexExtractBool(text, "enable_position_thread", cfg.enable_position_thread);
        cfg.safe_mode_no_background_ue_access = RegexExtractBool(text, "safe_mode_no_background_ue_access", cfg.safe_mode_no_background_ue_access);
        cfg.write_debug_log = RegexExtractBool(text, "write_debug_log", cfg.write_debug_log);
        cfg.write_only_high_confidence = RegexExtractBool(text, "write_only_high_confidence", cfg.write_only_high_confidence);
        cfg.debug_log_non_dino_destroy = RegexExtractBool(text, "debug_log_non_dino_destroy", cfg.debug_log_non_dino_destroy);
        cfg.debug_log_skipped_kills = RegexExtractBool(text, "debug_log_skipped_kills", cfg.debug_log_skipped_kills);
        cfg.debug_log_sample_rate = RegexExtractInt(text, "debug_log_sample_rate", cfg.debug_log_sample_rate);
        cfg.write_rejected_kills = RegexExtractBool(text, "write_rejected_kills", cfg.write_rejected_kills);
        cfg.write_forensics_log = RegexExtractBool(text, "write_forensics_log", cfg.write_forensics_log);
        cfg.forensics_ring_size = RegexExtractInt(text, "forensics_ring_size", cfg.forensics_ring_size);
        cfg.kill_csv_filename = RegexExtractString(text, "kill_csv_filename", cfg.kill_csv_filename);
        cfg.rejected_csv_filename = RegexExtractString(text, "rejected_csv_filename", cfg.rejected_csv_filename);
        cfg.debug_log_filename = RegexExtractString(text, "debug_log_filename", cfg.debug_log_filename);
        cfg.forensics_log_filename = RegexExtractString(text, "forensics_log_filename", cfg.forensics_log_filename);

        // Backward compatibility:
        // older configs do not contain enable_position_thread.
        // In that case we preserve legacy behavior and enable it.
        if (!Contains(text, "\"enable_position_thread\""))
        {
            cfg.enable_position_thread = true;
            g_compat_enable_thread_default_applied = true;
        }

        g_config = cfg;
    }

    bool ShouldSampleLog(int sample_rate, std::atomic<uint64_t>& counter)
    {
        if (sample_rate <= 1)
            return true;
        const uint64_t value = counter.fetch_add(1, std::memory_order_relaxed) + 1;
        return (value % static_cast<uint64_t>(sample_rate)) == 0;
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

    void DebugLogIfEnabled(const std::string& message)
    {
        if (g_config.write_debug_log)
            DebugLog(message);
    }

    void ForensicsEvent(const std::string& message)
    {
        if (!g_config.write_forensics_log)
            return;

        const int ring_size = g_config.forensics_ring_size > 0 ? g_config.forensics_ring_size : 256;
        std::lock_guard<std::mutex> lock(g_forensics_mutex);
        if (static_cast<int>(g_forensics_ring.size()) >= ring_size)
            g_forensics_ring.pop_front();
        g_forensics_ring.push_back("[" + IsoNowUtc() + "] " + message);
    }

    void FlushForensics(const std::string& reason)
    {
        if (!g_config.write_forensics_log)
            return;

        std::vector<std::string> snapshot;
        {
            std::lock_guard<std::mutex> lock(g_forensics_mutex);
            snapshot.assign(g_forensics_ring.begin(), g_forensics_ring.end());
        }

        std::lock_guard<std::mutex> lock(g_file_mutex);
        std::ofstream file(GetForensicsLogPath(), std::ios::app);
        if (!file.is_open())
            return;

        file << "===== FORENSICS_FLUSH " << IsoNowUtc() << " reason=" << reason << " events=" << snapshot.size() << " =====\n";
        for (const auto& line : snapshot)
            file << line << "\n";
        file << "===== END_FORENSICS_FLUSH =====\n";
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
        if (!actor)
            return "";

#ifdef _WIN32
        __try
        {
            if (!actor->ClassPrivateField())
                return "";
            return ToUtf8(AsaApi::GetApiUtils().GetClassBlueprint(actor->ClassPrivateField()));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ForensicsEvent("SEH GetBlueprintPath");
            return "";
        }
#else
        if (!actor->ClassPrivateField())
            return "";

        try
        {
            return ToUtf8(AsaApi::GetApiUtils().GetClassBlueprint(actor->ClassPrivateField()));
        }
        catch (...)
        {
            return "";
        }
#endif
    }

    std::pair<std::string, bool> GetOrClassifyBlueprint(AActor* actor)
    {
        if (!actor)
            return {"", false};

        uint64_t class_key = 0;
#ifdef _WIN32
        __try
        {
            if (!actor->ClassPrivateField())
                return {"", false};
            class_key = reinterpret_cast<uint64_t>(actor->ClassPrivateField());
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ForensicsEvent("SEH GetOrClassifyBlueprint class key");
            return {"", false};
        }
#else
        if (!actor->ClassPrivateField())
            return {"", false};
        class_key = reinterpret_cast<uint64_t>(actor->ClassPrivateField());
#endif

        {
            std::lock_guard<std::mutex> lock(g_blueprint_cache_mutex);
            auto it = g_blueprint_cache.find(class_key);
            if (it != g_blueprint_cache.end())
                return it->second;
        }

        const std::string blueprint = GetBlueprintPath(actor);
        if (blueprint.empty())
            return {"", false};

        const bool is_dino = IsLikelyDinoCharacterBlueprint(blueprint);

        {
            std::lock_guard<std::mutex> lock(g_blueprint_cache_mutex);
            // Cache only confident classifications with a resolved blueprint.
            // This avoids sticky false negatives when class blueprint lookup
            // occasionally fails under load.
            if (is_dino)
                g_blueprint_cache[class_key] = {blueprint, true};
        }

        return {blueprint, is_dino};
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

#ifdef _WIN32
        __try
        {
            if (actor->RootComponentField())
            {
                out_pos = actor->RootComponentField()->RelativeLocationField();
                return true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ForensicsEvent("SEH TryGetActorPosition");
            return false;
        }
#else
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
#endif

        return false;
    }

    bool TryGetPlayerPosition(AShooterCharacter* character, FVector& out_pos)
    {
        if (!character)
            return false;

#ifdef _WIN32
        __try
        {
            if (character->RootComponentField())
            {
                out_pos = character->RootComponentField()->RelativeLocationField();
                return true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
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
#endif

        return false;
    }

    double DistanceSquared(const FVector& a, const FVector& b)
    {
        const double dx = static_cast<double>(a.X) - static_cast<double>(b.X);
        const double dy = static_cast<double>(a.Y) - static_cast<double>(b.Y);
        const double dz = static_cast<double>(a.Z) - static_cast<double>(b.Z);
        return (dx * dx + dy * dy + dz * dz);
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

        bool append_failed = false;
        {
            std::lock_guard<std::mutex> lock(g_file_mutex);

            std::ofstream file(GetKillCsvPath(), std::ios::app);
            if (!file.is_open())
            {
                Log::GetLog()->error("WildKillLogger: failed to append wild_kills.csv");
                append_failed = true;
            }
            else
            {
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
        }

        if (append_failed)
            DebugLogIfEnabled("ERROR failed to append wild_kills.csv");
    }

    void EnsureRejectedCsvHeader()
    {
        std::lock_guard<std::mutex> lock(g_file_mutex);

        if (g_rejected_csv_header_written)
            return;

        std::ofstream file(GetRejectedCsvPath(), std::ios::app);
        if (!file.is_open())
        {
            Log::GetLog()->error("WildKillLogger: failed to open rejected_kills.csv for header");
            return;
        }

        if (file.tellp() == 0)
        {
            file << "timestamp_utc,dino_blueprint,dino_x,dino_y,dino_z,killer_eos,killer_name,nearest_distance,confidence,nearby_count,reason\n";
        }

        g_rejected_csv_header_written = true;
    }

    void AppendRejectedKill(
        const std::string& dino_blueprint,
        const FVector& dino_pos,
        const std::string& killer_eos,
        const std::string& killer_name,
        double nearest_distance,
        const std::string& confidence,
        int nearby_count,
        const std::string& reason)
    {
        EnsureRejectedCsvHeader();

        bool append_failed = false;
        {
            std::lock_guard<std::mutex> lock(g_file_mutex);

            std::ofstream file(GetRejectedCsvPath(), std::ios::app);
            if (!file.is_open())
            {
                Log::GetLog()->error("WildKillLogger: failed to append rejected_kills.csv");
                append_failed = true;
            }
            else
            {
                file
                    << CsvEscape(IsoNowUtc()) << ","
                    << CsvEscape(dino_blueprint) << ","
                    << dino_pos.X << ","
                    << dino_pos.Y << ","
                    << dino_pos.Z << ","
                    << CsvEscape(killer_eos) << ","
                    << CsvEscape(killer_name) << ","
                    << nearest_distance << ","
                    << CsvEscape(confidence) << ","
                    << nearby_count << ","
                    << CsvEscape(reason)
                    << "\n";
            }
        }

        if (append_failed)
            DebugLogIfEnabled("ERROR failed to append rejected_kills.csv");
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
        snapshot.failed_position_reads = snapshot.has_position ? 0 : 1;

        const auto key = reinterpret_cast<uint64_t>(controller);

        {
            std::lock_guard<std::mutex> lock(g_data_mutex);
            g_players[key] = snapshot;
        }

        if (g_config.write_debug_log)
        {
            DebugLog(
                "PLAYER_TRACKED eos_id=" + snapshot.eos_id +
                " name=" + snapshot.character_name +
                " has_position=" + std::string(snapshot.has_position ? "true" : "false"));
        }
    }

    void UpdatePlayerPositionsLoop()
    {
        while (g_running.load())
        {
            size_t pruned = 0;
            {
                std::lock_guard<std::mutex> lock(g_data_mutex);

                const std::time_t now = std::time(nullptr);

                for (auto it = g_players.begin(); it != g_players.end();)
                {
                    if (g_config.stale_player_seconds > 0 &&
                        (now - it->second.last_seen) > g_config.stale_player_seconds)
                    {
                        it = g_players.erase(it);
                        ++pruned;
                    }
                    else
                    {
                        if (!g_config.safe_mode_no_background_ue_access)
                        {
                            PlayerSnapshot& player = it->second;
                            if (player.character)
                            {
                                FVector pos{0.f, 0.f, 0.f};
                                if (TryGetPlayerPosition(player.character, pos))
                                {
                                    player.position = pos;
                                    player.has_position = true;
                                    player.last_seen = now;
                                    player.failed_position_reads = 0;

                                    if (player.character_name.empty() && player.controller)
                                        player.character_name = GetPlayerName(player.controller);

                                    if (player.eos_id.empty() && player.controller)
                                        player.eos_id = GetPlayerEosId(player.controller);
                                }
                                else
                                {
                                    player.failed_position_reads++;
                                }
                            }
                        }

                        if (it->second.failed_position_reads >= 8)
                        {
                            it = g_players.erase(it);
                            ++pruned;
                            continue;
                        }
                        ++it;
                    }
                }
            }
            if (pruned > 0)
                DebugLogIfEnabled("PRUNE_STALE_PLAYERS count=" + std::to_string(pruned));

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

        const auto blueprint_info = GetOrClassifyBlueprint(actor);
        const std::string& blueprint = blueprint_info.first;
        if (!blueprint_info.second)
        {
            if (g_config.debug_log_non_dino_destroy)
                DebugLogIfEnabled("NON_DINO_DESTROY blueprint=" + blueprint);
            return;
        }

        FVector dino_pos{0.f, 0.f, 0.f};
        const bool has_dino_pos = TryGetActorPosition(actor, dino_pos);

        std::string killer_eos = "unknown";
        std::string killer_name = "unknown";
        std::string confidence = "unknown";
        int nearby_count = 0;
        double nearest_distance = -1.0;

        // In safe mode, refresh player snapshots only in hook context and
        // avoid touching UE objects from background worker threads.
        if (g_config.safe_mode_no_background_ue_access)
        {
            std::lock_guard<std::mutex> lock(g_data_mutex);
            const std::time_t now = std::time(nullptr);
            for (auto& entry : g_players)
            {
                PlayerSnapshot& player = entry.second;
                if (!player.character)
                    continue;

                FVector pos{0.f, 0.f, 0.f};
                if (TryGetPlayerPosition(player.character, pos))
                {
                    player.position = pos;
                    player.has_position = true;
                    player.last_seen = now;
                    player.failed_position_reads = 0;

                    if (player.character_name.empty() && player.controller)
                        player.character_name = GetPlayerName(player.controller);

                    if (player.eos_id.empty() && player.controller)
                        player.eos_id = GetPlayerEosId(player.controller);
                }
                else
                {
                    player.failed_position_reads++;
                }
            }

            for (auto it = g_players.begin(); it != g_players.end();)
            {
                if (it->second.failed_position_reads >= 8)
                    it = g_players.erase(it);
                else
                    ++it;
            }
        }

        if (has_dino_pos)
        {
            const std::time_t now = std::time(nullptr);
            const double radius_sq = static_cast<double>(g_config.kill_radius) * static_cast<double>(g_config.kill_radius);
            double best_distance_sq = 1e36;
            const PlayerSnapshot* best_player = nullptr;

            std::vector<PlayerSnapshot> snapshot_players;
            {
                std::lock_guard<std::mutex> lock(g_data_mutex);
                snapshot_players.reserve(g_players.size());
                for (const auto& entry : g_players)
                    snapshot_players.push_back(entry.second);
            }

            for (const auto& player : snapshot_players)
            {
                if (!player.has_position)
                    continue;

                // Freshness filtering is only meaningful when the background
                // position thread is enabled and actively updates positions.
                if (g_config.enable_position_thread &&
                    (now - player.last_seen) > g_config.player_fresh_seconds)
                    continue;

                const double dist_sq = DistanceSquared(player.position, dino_pos);
                if (dist_sq <= radius_sq)
                {
                    nearby_count++;

                    if (dist_sq < best_distance_sq)
                    {
                        best_distance_sq = dist_sq;
                        best_player = &player;
                    }
                }
            }

            if (best_player)
            {
                nearest_distance = std::sqrt(best_distance_sq);
                killer_eos = best_player->eos_id.empty() ? "unknown" : best_player->eos_id;
                killer_name = best_player->character_name.empty() ? "unknown" : best_player->character_name;

                if (nearby_count == 1)
                    confidence = "high";
                else
                    confidence = "low";
            }
        }

        if (g_config.write_debug_log &&
            ShouldSampleLog(g_config.debug_log_sample_rate, g_dino_destroy_log_counter))
        {
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
        }

        if (ShouldSampleLog(g_config.debug_log_sample_rate, g_forensics_event_counter))
        {
            ForensicsEvent(
                "DINO_ATTR blueprint=" + blueprint +
                " confidence=" + confidence +
                " nearby_count=" + std::to_string(nearby_count) +
                " should_write=" + std::string((confidence == "high") || (!g_config.write_only_high_confidence && confidence != "unknown") ? "true" : "false"));
        }

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

            if (g_config.write_debug_log)
                DebugLog("KILL_WRITTEN blueprint=" + blueprint + " killer_name=" + killer_name);
            Log::GetLog()->info("WildKillLogger: reliable kill {} -> {}", blueprint, killer_name);
        }
        else if (g_config.debug_log_skipped_kills)
        {
            if (g_config.write_rejected_kills)
            {
                AppendRejectedKill(
                    blueprint,
                    dino_pos,
                    killer_eos,
                    killer_name,
                    nearest_distance,
                    confidence,
                    nearby_count,
                    "confidence_" + confidence
                );
            }

            if (g_config.write_debug_log &&
                ShouldSampleLog(g_config.debug_log_sample_rate, g_kill_skipped_log_counter))
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
    WildKillLogger::ForensicsEvent("HOOK_NEW_PLAYER");
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
        WildKillLogger::DebugLogIfEnabled("ERROR exception in Hook_AActor_Destroyed");
        WildKillLogger::ForensicsEvent("EXCEPTION Hook_AActor_Destroyed");
        WildKillLogger::FlushForensics("Hook_AActor_Destroyed exception");
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
        " stale_player_seconds=" + std::to_string(WildKillLogger::g_config.stale_player_seconds) +
        " enable_position_thread=" + std::string(WildKillLogger::g_config.enable_position_thread ? "true" : "false") +
        " safe_mode_no_background_ue_access=" + std::string(WildKillLogger::g_config.safe_mode_no_background_ue_access ? "true" : "false") +
        " debug_log_sample_rate=" + std::to_string(WildKillLogger::g_config.debug_log_sample_rate) +
        " write_forensics_log=" + std::string(WildKillLogger::g_config.write_forensics_log ? "true" : "false") +
        " write_rejected_kills=" + std::string(WildKillLogger::g_config.write_rejected_kills ? "true" : "false") +
        " write_only_high_confidence=" + std::string(WildKillLogger::g_config.write_only_high_confidence ? "true" : "false"));
    WildKillLogger::ForensicsEvent("PLUGIN_INIT");

    if (WildKillLogger::g_compat_enable_thread_default_applied)
    {
        WildKillLogger::DebugLog("CONFIG_COMPAT enable_position_thread missing -> defaulting to true (legacy behavior)");
    }

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

    if (WildKillLogger::g_config.enable_position_thread)
    {
        WildKillLogger::g_running = true;
        WildKillLogger::g_position_thread = std::thread(WildKillLogger::UpdatePlayerPositionsLoop);
        WildKillLogger::DebugLog("POSITION_THREAD_STARTED");
        WildKillLogger::ForensicsEvent("POSITION_THREAD_STARTED");
    }
    else
    {
        WildKillLogger::g_running = false;
        WildKillLogger::DebugLog("POSITION_THREAD_DISABLED");
        WildKillLogger::ForensicsEvent("POSITION_THREAD_DISABLED");
    }
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
    WildKillLogger::FlushForensics("Plugin_Unload");
}
