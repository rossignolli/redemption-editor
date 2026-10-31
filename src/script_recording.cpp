// script_recording.cpp: clips and recording: clip files, the clip list, the replay buffer, manual recording, clip saves, the recorded environment, the record gate.
// Split from script.cpp (moved code); shared internals in script_internal.h.

#include "../script_internal.h"

namespace
{
    // The recording gate's state (UpdateRecording).
    replay::gate::Gate g_recordGate;
    replay::gate::Hysteresis g_spanScriptCam;
    replay::gate::Hysteresis g_spanControlsOff;
    std::uint32_t g_gatePauseReasons = 0; // every reason seen while the clock stands
    std::uint32_t g_gateSignals = 0;      // the last tick's raw signals
    bool g_gateLogged = false;
    std::uint32_t g_gateSignalLogs = 0;
    std::uint32_t g_gateSampleLogs = 0;
    ULONGLONG g_gateNextSampleAt = 0;
    ULONGLONG g_bufferNextBlockAt = 0;
    std::size_t g_bufferPeakBytes = 0;
    std::uint64_t g_bufferTrimmedPose = 0;
    std::uint64_t g_bufferTrimmedFx = 0;
    std::uint64_t g_bufferTrimmedFxTracks = 0; // emptied registered fx tracks removed by the trim
    // A manual part being saved by the chain (not the stop that ends it): the
    // stop names the file with this part's number.
    bool g_manualSplitting = false;
    // The ring's budget this block (ReplayBufferMB) and its coverage verdict.
    std::size_t g_bufferBudgetBytes = 0;
    replay::buffercov::Limit g_bufferLimit = replay::buffercov::Limit::Start;
    std::uint64_t g_bufferLimitChanges = 0;
    std::vector<replay::window::PlaceSample> g_bufferPlaces; // the place over the ring
    constexpr std::uint32_t kBufferBlockMs = 2000; // the lifecycle block and pose keyframe interval
}

    void ImageRange(std::uintptr_t& base, std::size_t& size)
    {
        base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        size = nt->OptionalHeader.SizeOfImage;
        replay::clipfile::SetBuildIdentity(nt->FileHeader.TimeDateStamp, size);
    }

    // A clip file of the base folder (where new clips go).
    std::wstring ClipPath(const std::wstring& name)
    {
        return replay::storage::Folder(kClipFolder) + L"\\" + name + L".rpclip";
    }

    // Where the clip file `fileName` ("042-Valentine.rpclip") is: the base
    // folder, else the game folder (clips of an earlier start), else the base
    // path a new one would take.
    std::wstring FindClipFile(const std::wstring& fileName) { return replay::storage::Find(std::wstring(kClipFolder) + L"\\" + fileName); }
namespace
{

    // A clip opened from a folder that takes no writes (a Program Files game
    // folder) is saved to the base folder under the same name.
    std::wstring SavableClipPath(const std::wstring& path)
    {
        if (replay::storage::Current().gameWritable)
            return path;
        replay::storage::Folder(kClipFolder);
        return replay::storage::WritablePath(path);
    }
}

    std::string Narrow(const std::wstring& text)
    {
        std::string out;
        for (const wchar_t c : text)
            out += c < 128 ? static_cast<char>(c) : '?';
        return out;
    }

    // The last clip written, for the editor's thumbnail.
    std::string g_savedClipName;
    std::string g_lastWrittenClipName; // any save that reached the disk
    std::uint64_t g_savedClipSequence = 0;

    bool WriteClipFile(const std::wstring& name, std::string& error)
    {
        if (g_recording)
        {
            error = "recording";
            return false;
        }
        if (g_transport.DurationMs() == 0)
        {
            error = "no_clip";
            return false;
        }
        std::uintptr_t base = 0;
        std::size_t size = 0;
        ImageRange(base, size);
        const std::wstring path = ClipPath(name);
        const ULONGLONG startedAt = GetTickCount64();
        // A legacy shake still waiting is folded first (a save writes only the
        // markers' shake).
        if (g_cameraTrack.HasLegacyShake())
            replay::camera::mshake::Migrate(g_cameraTrack);
        const bool ok = replay::clipfile::Save(g_clip, replay::pose::capture::ExportRecords(), g_cameraTrack,
            g_transport.DurationMs(), base, size, path, error);
        WIN32_FILE_ATTRIBUTE_DATA info{};
        const bool sized = ok && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info);
        if (ok)
        {
            g_currentClipPath = path;
            g_memoryClipPath = path;
            g_clipEdited = false;
            g_savedClipMs = static_cast<std::uint32_t>(g_transport.DurationMs());
            g_savedClipName = Narrow(path.substr(path.find_last_of(L'\\') + 1));
            ++g_savedClipSequence;
            RebuildEditorClipList();
        }
        LogFile() << "CLIP_SAVE,path=" << Narrow(path) << ",ok=" << ok << ",bytes="
                  << (sized ? (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow : 0)
                  << ",ms=" << (GetTickCount64() - startedAt) << ",error=" << (ok ? "none" : error.c_str()) << '\n';
        return ok;
    }
    std::unique_ptr<ClipRead> g_clipRead;
    // The next clip of a project playing as a whole, read ahead on a thread while
    // the one before it plays. Handed to g_clipRead when its turn comes; until
    // then the compressor holds as for any read.
    std::unique_ptr<ClipRead> g_projectPreload;
    // Read aheads not wanted any more, kept until their thread ends (joining one in
    // flight would hold the script thread for the whole read).
    std::vector<std::unique_ptr<ClipRead>> g_projectPreloadDropped;
    bool ClipReadPending() { return g_clipRead != nullptr || g_projectPreload != nullptr || !g_projectPreloadDropped.empty(); }

    // The read itself, for a load (openClip: the compressor stays off that file
    // from now on) or a project preload (the clip open now stays the compressor's
    // open clip until the preloaded one is published).
    std::unique_ptr<ClipRead> BeginClipRead(const std::wstring& path, bool openClip)
    {
        auto read = std::make_unique<ClipRead>();
        read->path = path;
        read->startedAt = GetTickCount64();
        std::uintptr_t base = 0;
        std::size_t size = 0;
        ImageRange(base, size);
        // The compressor stays off this file and off the disk meanwhile
        // (ServiceClipCompressor keeps the hold while the read runs).
        if (openClip)
            replay::clipcompress::worker::SetOpenClip(path);
        replay::clipcompress::worker::SetHold(true);
        ClipRead* raw = read.get();
        raw->worker = std::thread([raw, base, size]() {
            replay::threads::Scope threadName("clip_read");
            // Load reports bad_alloc itself; anything else leaving the thread would be
            // std::terminate (the game closes).
            try
            {
                raw->ok = replay::clipfile::Load(raw->path, base, size, raw->recording, raw->poses, raw->camera,
                    raw->durationMs, raw->error, &raw->info);
                if (raw->ok && replay::pose::smooth::LoadSmoothing())
                {
                    const ULONGLONG smoothStart = GetTickCount64();
                    replay::pose::smooth::SmoothRecords(raw->poses, raw->smoothSummary);
                    raw->smoothMs = GetTickCount64() - smoothStart;
                    raw->posesSmoothed = true;
                }
            }
            catch (const std::exception& exception)
            {
                raw->ok = false;
                raw->error = std::string("exception_") + exception.what();
            }
            catch (...)
            {
                raw->ok = false;
                raw->error = "unknown_exception";
            }
            raw->done.store(true);
        });
        LogFile() << "CLIP_READ_START,path=" << Narrow(path) << (openClip ? "" : ",preload=1") << '\n';
        return read;
    }

    void StartClipRead(const std::wstring& path)
    {
        g_clipRead = BeginClipRead(path, true);
    }

    // The read in flight, waited for (unload, or a read no longer wanted).
    void CancelClipRead()
    {
        if (!g_clipRead)
            return;
        g_clipRead->worker.join();
        g_clipRead.reset();
    }

    // The clip files of the folder, newest first (by the file's write time;
    // names typed by the player carry no date).
    // Every read root's clips (storage_paths.h), a name in two of them once.
    std::vector<std::wstring> ListClipFiles()
    {
        std::vector<std::pair<std::uint64_t, std::wstring>> found;
        std::vector<std::wstring> folders;
        for (const replay::storage::Entry& entry : replay::storage::List(kClipFolder, L"*.rpclip", &folders))
            found.emplace_back(entry.stamp, folders[entry.root] + L"\\" + entry.name);
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<std::wstring> names;
        for (auto& item : found)
            names.push_back(std::move(item.second));
        return names;
    }

    // Clip list for the editor gallery: rebuilt on request and after a save, a
    // load or a delete; only the file header is read.

    // Weather by name: the game's weather type names, hashed at run time and
    // compared with the recorded hash.
    std::string WeatherName(std::uint32_t hash)
    {
        static const std::pair<const char*, const char*> kNames[] = {
            {"SUNNY", "Sunny"}, {"HIGHPRESSURE", "Clear"}, {"CLOUDS", "Cloudy"}, {"OVERCAST", "Overcast"},
            {"OVERCASTDARK", "Dark overcast"}, {"RAIN", "Rain"}, {"THUNDERSTORM", "Thunderstorm"}, {"THUNDER", "Thunder"},
            {"DRIZZLE", "Drizzle"}, {"SHOWER", "Showers"}, {"FOG", "Fog"}, {"MISTY", "Mist"}, {"SLEET", "Sleet"},
            {"HAIL", "Hail"}, {"SNOW", "Snow"}, {"SNOWLIGHT", "Light snow"}, {"SNOWCLEARING", "Snow clearing"},
            {"BLIZZARD", "Blizzard"}, {"GROUNDBLIZZARD", "Ground blizzard"}, {"WHITEOUT", "Whiteout"},
            {"HURRICANE", "Hurricane"}, {"SANDSTORM", "Sandstorm"}};
        for (const auto& [key, label] : kNames)
        {
            if (static_cast<std::uint32_t>(MISC::GET_HASH_KEY(key)) == hash)
                return label;
        }
        char text[16];
        std::snprintf(text, sizeof(text), "0x%08X", hash);
        return text;
    }
namespace
{

    // A zone's name for people: the community name whose hash is the zone's
    // (computed with GET_HASH_KEY), map text prefixes dropped
    // ("P_4_THIEVES_LANDING" -> "Thieves Landing", "HennigansStead" ->
    // "Hennigans Stead"). Empty when the zone is not in the table. Hashed once
    // (285 names), then looked up.
    const char* FindZoneName(std::uint32_t hash)
    {
        static std::unordered_map<std::uint32_t, const char*> table;
        if (table.empty())
        {
            const auto add = [](const auto& names) {
                for (const char* name : names)
                    table.emplace(static_cast<std::uint32_t>(MISC::GET_HASH_KEY(name)), name);
            };
            add(replay::zones::kPrinted);
            add(replay::zones::kWritten);
            add(replay::zones::kTowns);
            add(replay::zones::kDistricts);
            add(replay::zones::kStates);
        }
        const auto found = table.find(hash);
        return found == table.end() ? nullptr : found->second;
    }

    std::string ZoneDisplayName(std::uint32_t hash)
    {
        if (!hash)
            return {};
        const char* name = FindZoneName(hash);
        if (!name)
            return {};
        std::string text(name);
        // Map text names carry a "P_4_" or "W_4_" style prefix.
        if (text.size() > 4 && (text[0] == 'P' || text[0] == 'p' || text[0] == 'W' || text[0] == 'w') && text[1] == '_')
        {
            const std::size_t second = text.find('_', 2);
            if (second != std::string::npos)
                text = text.substr(second + 1);
        }
        std::string out;
        bool newWord = true;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            char c = text[i];
            if (c == '_')
            {
                if (!out.empty() && out.back() != ' ')
                    out += ' ';
                newWord = true;
                continue;
            }
            const bool upper = c >= 'A' && c <= 'Z';
            const bool prevLower = i > 0 && text[i - 1] >= 'a' && text[i - 1] <= 'z';
            if (upper && prevLower && !out.empty() && out.back() != ' ')
            {
                out += ' ';
                newWord = true;
            }
            if (newWord && c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
            else if (!newWord && upper)
                c = static_cast<char>(c - 'A' + 'a');
            out += c;
            newWord = false;
        }
        return out;
    }

    // The zone part, also sampled over a running buffer (g_bufferPlaces) so a
    // saved window is named after where it starts.
    bool ReadPlaceZones(replay::Place& place)
    {
        const Ped player = PLAYER::PLAYER_PED_ID();
        if (!player || !ENTITY::DOES_ENTITY_EXIST(player))
            return false;
        const Vector3 at = ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE);
        // The zone hash is not a text label (GET_STRING_FROM_HASH_KEY gives empty
        // text): the name comes from the zone table.
        const auto zoneText = [&](int type, std::uint32_t& hash, char* out, std::size_t size) {
            hash = static_cast<std::uint32_t>(ZONE::_GET_MAP_ZONE_AT_COORDS(at.x, at.y, at.z, type));
            std::snprintf(out, size, "%s", ZoneDisplayName(hash).c_str());
        };
        // The place as the game's HUD names it: printed map text (a settlement
        // like Thieves Landing), else written map text, else the town.
        zoneText(11, place.zoneTown, place.town, sizeof(place.town));
        if (!place.zoneTown)
            zoneText(12, place.zoneTown, place.town, sizeof(place.town));
        if (!place.zoneTown)
            zoneText(1, place.zoneTown, place.town, sizeof(place.town));
        zoneText(10, place.zoneDistrict, place.district, sizeof(place.district));
        zoneText(0, place.zoneState, place.state, sizeof(place.state));
        place.recorded = true;
        return true;
    }

    // Where and who at the recording's start: the map zones at the player with the
    // game's own text for them, the character, and the weather by name.
    void RecordPlace()
    {
        replay::Place& place = g_clip.place;
        place = replay::Place{};
        if (!ReadPlaceZones(place))
        {
            place = replay::Place{};
            return;
        }
        const Ped player = PLAYER::PLAYER_PED_ID();
        place.playerModel = static_cast<std::uint32_t>(ENTITY::GET_ENTITY_MODEL(player));
        const char* character = place.playerModel == static_cast<std::uint32_t>(MISC::GET_HASH_KEY("player_zero")) ? "Arthur"
            : place.playerModel == static_cast<std::uint32_t>(MISC::GET_HASH_KEY("player_three")) ? "John" : "Other";
        std::snprintf(place.character, sizeof(place.character), "%s", character);
        const replay::Environment& environment = g_clip.environment;
        const std::string weather = WeatherName(environment.weatherMix >= 0.5f ? environment.weatherTo : environment.weatherFrom);
        std::snprintf(place.weather, sizeof(place.weather), "%s", weather.c_str());
        place.recorded = true;
        LogFile() << "CLIP_PLACE,town=0x" << std::hex << place.zoneTown << ":" << place.town << ",district=0x" << place.zoneDistrict
                  << ":" << place.district << ",state=0x" << place.zoneState << ":" << place.state << std::dec
                  << ",character=" << place.character << ",weather=" << place.weather << '\n';
    }

    // The gallery's details of a clip, read from its sections without loading it:
    // the environment and place sections of the clip's summary
    // (clipcompress::ReadHead; for a plain file the section payloads are skipped
    // on disk, a compressed one carries its summary).
    void ReadClipListDetails(const std::vector<std::uint8_t>& summary, replay::editor::ClipInfo& info)
    {
        std::uint32_t environmentTag = 0, placeTag = 0, worldInfoTag = 0;
        for (const replay::clipfile::SectionInfo& section : replay::clipfile::Sections())
        {
            if (std::strcmp(section.name, "environment") == 0)
                environmentTag = section.tag;
            if (std::strcmp(section.name, "place") == 0)
                placeTag = section.tag;
            if (std::strcmp(section.name, "world info") == 0)
                worldInfoTag = section.tag;
        }
        info.chapter = replay::world::kChapterUnknown;
        info.camp = replay::world::kCampUnknown;
        std::size_t at = 20; // the clip header
        for (int guard = 0; guard < 64 && at + 14 <= summary.size(); ++guard)
        {
            const std::uint8_t* header = summary.data() + at;
            std::uint32_t tag = 0;
            std::uint64_t length = 0;
            std::memcpy(&tag, header, 4);
            std::memcpy(&length, header + 6, 8);
            if (length > 4096 || at + 14 + length + 8 > summary.size())
                return;
            const std::uint8_t* payload = header + 14;
            at += 14 + static_cast<std::size_t>(length) + 8;
            if (tag == environmentTag && length >= 21 && payload[0])
            {
                std::uint32_t from = 0, to = 0;
                float mix = 0.0f;
                std::memcpy(&info.clockStartS, payload + 1, 4);
                std::memcpy(&from, payload + 9, 4);
                std::memcpy(&to, payload + 13, 4);
                std::memcpy(&mix, payload + 17, 4);
                info.hasClock = 1;
                if (!info.weather[0])
                    std::snprintf(info.weather, sizeof(info.weather), "%s", WeatherName(mix >= 0.5f ? to : from).c_str());
            }
            // World info (WINF): u8 recorded, u8 camp, u8 chapter, ...
            if (tag == worldInfoTag && length >= 20 && payload[0])
            {
                info.camp = payload[1];
                info.chapter = payload[2] <= replay::world::kChapterEpilogue2 ? payload[2] : replay::world::kChapterUnknown;
            }
            if (tag == placeTag && length >= 1 + 16 + 48 * 3 + 16 + 24 && payload[0])
            {
                const char* town = reinterpret_cast<const char*>(payload + 17);
                const char* district = town + 48;
                const char* state = district + 48;
                std::uint32_t hashes[3] = {};
                std::memcpy(hashes, payload + 1, sizeof(hashes));
                std::string region;
                const char* parts[3] = {town, district, state};
                for (int n = 0; n < 3; ++n)
                {
                    // Older clips have the hash but no text.
                    std::string text(parts[n], strnlen(parts[n], 48));
                    if (text.empty())
                        text = ZoneDisplayName(hashes[n]);
                    if (!text.empty())
                        region += (region.empty() ? "" : ", ") + text;
                }
                std::snprintf(info.region, sizeof(info.region), "%s", region.c_str());
                std::snprintf(info.character, sizeof(info.character), "%.15s", reinterpret_cast<const char*>(payload + 17 + 48 * 3));
                std::snprintf(info.weather, sizeof(info.weather), "%.23s", reinterpret_cast<const char*>(payload + 17 + 48 * 3 + 16));
            }
        }
    }
}
    std::unique_ptr<ClipListRead> g_clipListRead;
namespace
{
    bool g_clipListAgain = false; // asked again while a read ran (a save, a delete)
}
    bool ClipListLoading() { return g_clipListRead != nullptr; }

    // The sorted list for the gallery from the list as read.
    void ApplyClipSort()
    {
        g_editorClips = g_editorClipsByDate;
        if (g_clipSort == 1)
            std::stable_sort(g_editorClips.begin(), g_editorClips.end(),
                [](const replay::editor::ClipInfo& a, const replay::editor::ClipInfo& b) { return a.durationMs > b.durationMs; });
        for (std::size_t index = 0; index < g_editorClips.size(); ++index)
            g_editorClips[index].listIndex = static_cast<std::uint32_t>(index);
        ++g_editorClipListSequence;
    }

    void RebuildEditorClipList()
    {
        if (g_clipListRead)
        {
            g_clipListAgain = true;
            return;
        }
        g_clipListRead = std::make_unique<ClipListRead>();
        ClipListRead* read = g_clipListRead.get();
        read->startedAt = GetTickCount64();
        read->worker = std::thread([read]() {
            replay::threads::Scope threadName("clip_list_read");
            // An exception leaving a std::thread is std::terminate, which closes the game
            // (e.g. a bad_alloc with a large clip folder); it is reported instead.
            try
            {
                read->paths = ListClipFiles();
                read->heads.resize(read->paths.size());
                read->read.resize(read->paths.size());
                for (std::size_t n = 0; n < read->paths.size(); ++n)
                {
                    if (InterlockedCompareExchange(&g_moduleDetaching, 0, 0) != 0)
                        break;
                    read->read[n] = replay::clipcompress::ReadHead(read->paths[n], read->heads[n]) ? 1 : 0;
                }
            }
            catch (const std::exception& exception)
            {
                read->error = exception.what();
            }
            catch (...)
            {
                read->error = "unknown_exception";
            }
            if (!read->error.empty())
            {
                read->paths.clear();
                read->heads.clear();
                read->read.clear();
            }
            read->done.store(true);
        });
    }

    // Each tick (and with `wait` on unload): the read list goes in.
    void ServiceClipListRead(bool wait)
    {
        if (!g_clipListRead || (!wait && !g_clipListRead->done.load()))
            return;
        g_clipListRead->worker.join();
        ClipListRead& read = *g_clipListRead;
        std::vector<replay::editor::ClipInfo> clips;
        std::uint64_t diskBytes = 0, clipBytes = 0;
        std::uint32_t compressedClips = 0;
        for (std::size_t n = 0; n < read.paths.size(); ++n)
        {
            const std::wstring& path = read.paths[n];
            const replay::clipcompress::FileHead& head = read.heads[n];
            replay::editor::ClipInfo info{};
            const std::string name = Narrow(path.substr(path.find_last_of(L'\\') + 1));
            std::snprintf(info.name, sizeof(info.name), "%s", name.c_str());
            info.bytes = head.diskBytes;
            info.clipBytes = head.clipBytes;
            info.compressed = head.compressed ? 1u : 0u;
            diskBytes += head.diskBytes;
            clipBytes += head.clipBytes ? head.clipBytes : head.diskBytes;
            compressedClips += head.compressed ? 1u : 0u;
            if (read.read[n] && head.summary.size() >= 20 &&
                std::memcmp(head.summary.data(), replay::clipfile::kMagic, sizeof(replay::clipfile::kMagic)) == 0)
            {
                std::memcpy(&info.formatVersion, head.summary.data() + 8, 4);
                std::memcpy(&info.durationMs, head.summary.data() + 12, 8);
                info.loadable = info.formatVersion == replay::clipfile::kFormatVersion ? 1u : 0u;
                if (info.loadable)
                    ReadClipListDetails(head.summary, info);
            }
            clips.push_back(info);
        }
        const ULONGLONG tookMs = GetTickCount64() - read.startedAt;
        const std::string listError = read.error.empty() ? std::string("none") : read.error;
        g_clipListRead.reset();
        g_editorClipsByDate = std::move(clips);
        g_clipsDiskBytes = diskBytes;
        g_clipsBytes = clipBytes;
        g_compressedClips = compressedClips;
        ApplyClipSort();
        LogFile() << "CLIP_LIST,clips=" << g_editorClipsByDate.size() << ",ms=" << tookMs
                  << ",error=" << listError << '\n';
        if (g_clipListAgain && !wait)
        {
            g_clipListAgain = false;
            RebuildEditorClipList();
        }
    }

    // The pose finalizer belongs to one mod at a time (the Pose Editor shares it):
    // false, with a notice, when the other has it.
    bool TakePoseFinalizer()
    {
        std::string error;
        if (replay::pose::capture::AcquireFinalizer(error))
            return true;
        LogFile() << "POSE_FINALIZER,refused=" << error << '\n';
        replay::hud::ShowNotice(TR("hud.pose_editor", "Redemption Editor: close the Pose Editor first (it is using the character poses)."));
        return false;
    }

    // Every detour of the mod installed at load with a single freeze
    // (MH_ApplyQueued): enabled one by one they would cost 60 to 160 ms of frozen
    // game each.
    void WarmUpHooks()
    {
        const std::uint64_t startedAt = replay::prof::Now();
        // The batch opened before the modules attached (fire and decal
        // install there).
        if (!replay::hooks::BatchOpen())
            replay::hooks::BeginBatch();
        replay::events::WarmUp();
        replay::objects::WarmUp();
        std::string vegetationError;
        const bool vegetation = replay::vegetation::Install(vegetationError);
        // The weather audio gate.
        std::string windError;
        const bool wind = replay::windreplay::Install(windError);
        // The category resolver and the speech duck driver.
        std::string catsError;
        const bool cats = replay::catstate::Install(catsError);
        std::string duckError;
        const bool duck = replay::speechduck::Install(duckError);
        std::string finalizerError;
        const bool finalizer = replay::pose::capture::WarmUpFinalizer(finalizerError);
        std::string vehicleError;
        const bool vehicle = replay::pose::capture::WarmUpVehiclePreRender(vehicleError);
        // The ped pre-render detour (rider follows mount); installed whatever
        // MountPreRender says (0 measures).
        std::string pedPreRenderError;
        const bool pedPreRender = replay::mount::Install(pedPreRenderError);
        // Closed theater: spawn funnel, mix scenes, speech rules (forward, record; deny only while owning).
        std::string theatreReport;
        replay::theatre::Install(g_recConfig.theatre.hooks, theatreReport);
        // The animal bank picker (animal_bank.h); it only forwards until a playback
        // publishes holds.
        replay::theatre::InstallAnimalBank(theatreReport);
        int applied = 0;
        MH_STATUS status = MH_OK;
        const double freezeMs = replay::hooks::EndBatch(applied, status);
        g_lastWarmUpStatus = status; // SESSION_CHECK
        g_lastWarmUpApplied = applied;
        LogFile() << "HOOK_WARMUP,applied=" << applied << ",status=" << MH_StatusToString(status)
            << ",freeze_ms=" << freezeMs << ",total_ms=" << TicksMs(replay::prof::Now() - startedAt)
            << ",vegetation=" << (vegetation ? 1 : 0) << ",finalizer=" << (finalizer ? "held" : finalizerError.c_str())
            << ",wind_gate=" << (wind ? "installed" : windError.c_str())
            << ",category_resolver=" << (cats ? "installed" : catsError.c_str())
            << ",speech_duck=" << (duck ? "installed" : duckError.c_str())
            << ",vehicle_prerender=" << (vehicle ? "installed" : vehicleError.c_str())
            << ",ped_prerender=" << (pedPreRender ? "installed" : pedPreRenderError.c_str())
            << ",theatre=" << theatreReport
            << '\n';
        LogFile() << replay::pools::Describe() << '\n';
        LogFile() << replay::entities::Describe() << '\n';
        LogFile() << replay::trains::Describe() << '\n';
        LogFile() << replay::vehdoors::Describe() << '\n';
    }

    // The Pose Editor asks for the pose finalizer through the shared slot;
    // it gets it while nothing is recorded or played, and the replay takes
    // it back when the Pose Editor lets it go.
    void ServicePoseFinalizer()
    {
        // The always-on buffer is not a reason to keep the Pose Editor out:
        // it stops (no file) and resumes once the Pose Editor let go.
        if (g_recording && g_recordKind == RecordKind::Buffer && replay::pose::capture::FinalizerWanted())
        {
            LogFile() << "BUFFER_PAUSE,reason=pose_editor\n";
            StopRecording(false);
            g_bufferResumeAt = GetTickCount64() + 3000;
        }
        const bool busy = g_recording || g_transport.Playing() || !g_context.puppets.list.empty();
        std::string error;
        switch (replay::pose::capture::ServiceFinalizer(busy, error))
        {
        case replay::pose::capture::FinalizerEvent::Yielded:
            LogFile() << "POSE_FINALIZER,event=yielded_to_pose_editor\n";
            break;
        case replay::pose::capture::FinalizerEvent::Reclaimed:
            LogFile() << "POSE_FINALIZER,event=reclaimed\n";
            break;
        case replay::pose::capture::FinalizerEvent::Failed:
            LogFile() << "POSE_FINALIZER,event=reclaim_failed,error=" << error << '\n';
            break;
        default:
            break;
        }
    }
namespace
{

    std::size_t RecordingBytes()
    {
        const std::size_t bytes = replay::pose::capture::TrackBytes() + replay::buffer::ApproxBytes(g_clip);
        g_bufferPeakBytes = (std::max)(g_bufferPeakBytes, bytes);
        return bytes;
    }

    std::size_t ApplyBufferHead(std::uint32_t headMs)
    {
        if (headMs > g_bufferHeadMs)
        {
            g_bufferTrimmedFx += replay::buffer::TrimBefore(g_clip, headMs).fxFrames;
            // The registered fx tracks the trim emptied go too (their count is
            // capped, entity::kMaxRegisteredFxTracks; the live keys keep theirs).
            g_bufferTrimmedFxTracks += replay::events::DropTrimmedRegisteredFx(replay::buffer::ExportableBaseMs(g_clip, headMs));
            // The pose chunks keep from the same base as the other tracks (the block a
            // saved window starts at).
            g_bufferTrimmedPose += replay::pose::capture::DropBefore(replay::buffer::ExportableBaseMs(g_clip, headMs));
            g_bufferHeadMs = headMs;
            replay::window::DropPlacesBefore(g_bufferPlaces, replay::buffer::ExportableBaseMs(g_clip, headMs));
        }
        return RecordingBytes();
    }

    // A part saved at its limit while the manual recording goes on: said on
    // screen, not only in the log (testers took the split for a 1:30 cap).
    void ManualPartNotice(bool timeLimit)
    {
        const std::string part = std::to_string(g_manualPart);
        const std::string next = std::to_string(g_manualPart + 1);
        const std::string text = timeLimit
            ? TRF("hud.manual_part_time", "Redemption Editor: the recording reached its time limit, so part {part} is saved as its own clip "
                  "and the recording goes on in part {next}.", {{"part", part}, {"next", next}})
            : TRF("hud.manual_part_size", "Redemption Editor: the recording reached its size limit, so part {part} is saved as its own clip "
                  "and the recording goes on in part {next}.", {{"part", part}, {"next", next}});
        replay::hud::ShowNotice(text.c_str());
        LogFile() << "MANUAL_CHAIN,notice=" << (timeLimit ? "time" : "size") << ",part=" << g_manualPart << '\n';
    }

    // A manual recording inside the buffer at its part limit: the part from the
    // mark to now is saved and the next one starts now (chained recordings).
    void ChainManualInBuffer(std::uint32_t nowMs, bool timeLimit, std::size_t bytes, std::size_t ringBudget, std::size_t partBytes)
    {
        ++g_manualPart;
        LogFile() << "MANUAL_CHAIN,part=" << g_manualPart << ",reason=" << (timeLimit ? "time" : "memory") << ",buffer=1,from_ms="
                  << g_manualStartMs << ",to_ms=" << nowMs << ",mb=" << bytes / (1024 * 1024) << ",part_mb=" << partBytes / (1024 * 1024)
                  << ",manual_max_mb=" << g_recConfig.manualMaxMB << ",ring_budget_mb=" << ringBudget / (1024 * 1024) << '\n';
        // The part's job owns its cut before the head moves past it. A cut that
        // failed, or saves already failing, end the manual recording here (said, not
        // silent): the ring then trims as usual.
        const bool saved = SaveRecordingWindow(g_manualStartMs, nowMs, "manual_part");
        g_manualStartMs = nowMs;
        if (!saved || HeldClipSaves() > 0)
        {
            g_manualInBuffer = false;
            LogFile() << "MANUAL_CHAIN,stopped=" << (saved ? "saves_failing" : "cut_failed") << ",part=" << g_manualPart
                      << ",held=" << HeldClipSaves() << '\n';
            replay::hud::ShowNotice(saved ? TR("hud.saves_failing_manual", "Redemption Editor: saving is failing, so the manual recording stopped here. "
                                            "The parts not saved are kept in memory and saving is retried.")
                                          : TR("hud.cut_failed", "Redemption Editor: this part of the recording could not be cut, so the manual recording stopped."));
        }
        else
            ManualPartNotice(timeLimit);
    }

    // Every block (2 s): the manual part's limits (ManualMaxSeconds,
    // ManualMaxMB: a full part is saved and the recording goes on in the
    // next), and the buffer's ring (the head follows the window, and moves on
    // while over the budget; a manual mark inside it splits like a manual
    // recording, at the larger of ManualMaxMB and the ring's budget).
    void UpdateRecordingLimits()
    {
        const ULONGLONG now = GetTickCount64();
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        if (g_recordKind == RecordKind::Manual)
        {
            const bool timeCap = replay::buffercov::ManualTimeUp(g_recConfig.manualMaxSeconds, nowMs, 0);
            if (now < g_bufferNextBlockAt && !timeCap)
                return;
            g_bufferNextBlockAt = now + kBufferBlockMs;
            const std::size_t bytes = RecordingBytes();
            const std::size_t partBytes = replay::buffercov::ManualPartBytes(g_recConfig.manualMaxMB, 0);
            if (timeCap || bytes > partBytes)
            {
                ++g_manualPart;
                LogFile() << "MANUAL_CHAIN,part=" << g_manualPart << ",reason=" << (timeCap ? "time" : "memory") << ",buffer=0"
                          << ",now_ms=" << nowMs << ",mb=" << bytes / (1024 * 1024) << ",part_mb=" << partBytes / (1024 * 1024) << '\n';
                g_manualSplitting = true;
                StopRecording(true);
                g_manualSplitting = false;
                // While an earlier part failed to save (held in memory, retried), no next part
                // starts: the parts would pile up in memory. The player is told.
                if (HeldClipSaves() > 0)
                {
                    LogFile() << "MANUAL_CHAIN,stopped=saves_failing,held=" << HeldClipSaves() << '\n';
                    replay::hud::ShowNotice(TR("hud.saves_failing", "Redemption Editor: saving is failing, so the recording stopped here. "
                        "The parts not saved are kept in memory and saving is retried; free some disk space."));
                }
                else
                {
                    StartRecording(RecordKind::Manual);
                    if (g_recording)
                        ManualPartNotice(timeCap); // after the start's REC notice
                    else
                        LogFile() << "MANUAL_CHAIN,stopped=start_refused,part=" << g_manualPart << '\n';
                }
            }
            return;
        }
        if (now < g_bufferNextBlockAt)
            return;
        g_bufferNextBlockAt = now + kBufferBlockMs;
        {
            replay::Place here;
            if (ReadPlaceZones(here))
                replay::window::AddPlaceSample(g_bufferPlaces, nowMs, here);
        }
        const std::uint32_t windowMs = g_recConfig.bufferSeconds * 1000u;
        std::uint32_t head = nowMs > windowMs ? nowMs - windowMs : 0;
        if (g_manualInBuffer)
            head = (std::min)(head, g_manualStartMs);
        std::size_t bytes = ApplyBufferHead(head);
        // The budget: ReplayBufferMB.
        const std::size_t budget = static_cast<std::size_t>(g_recConfig.bufferMB) * 1024 * 1024;
        g_bufferBudgetBytes = budget;
        // A manual mark holds its part to the same limits as a manual recording
        // without the buffer: ManualMaxSeconds, and ManualMaxMB (or the ring's budget
        // when that is larger). The seconds before the mark stay the ring's, trimmed
        // to its own budget.
        const std::size_t partBytes = replay::buffercov::ManualPartBytes(g_recConfig.manualMaxMB, budget);
        if (g_manualInBuffer && replay::buffercov::ManualTimeUp(g_recConfig.manualMaxSeconds, nowMs, g_manualStartMs))
            ChainManualInBuffer(nowMs, true, bytes, budget, partBytes);
        int steps = 0;
        while (g_bufferHeadMs + 2 * kBufferBlockMs < nowMs && steps++ < 64)
        {
            const bool atMark = replay::buffercov::HeadAtManualMark(g_manualInBuffer, g_bufferHeadMs, g_manualStartMs);
            if (bytes <= replay::buffercov::RingLimitBytes(atMark, budget, partBytes))
                break;
            if (atMark)
                ChainManualInBuffer(nowMs, false, bytes, budget, partBytes);
            bytes = ApplyBufferHead(g_bufferHeadMs + kBufferBlockMs);
        }
        // What the ring really covers, and why it is short; a change of the reason is
        // logged at once.
        const replay::buffercov::Coverage coverage =
            replay::buffercov::Evaluate(nowMs, g_bufferHeadMs, g_recConfig.bufferSeconds, g_manualInBuffer, g_manualStartMs);
        if (coverage.limit != g_bufferLimit)
        {
            g_bufferLimit = coverage.limit;
            if (++g_bufferLimitChanges <= 400)
                LogFile() << "BUFFER_COVERAGE,phase=change,covered_ms=" << coverage.coveredMs << ",target_ms=" << coverage.targetMs
                          << ",limit=" << replay::buffercov::LimitText(coverage.limit) << ",mb=" << bytes / (1024 * 1024)
                          << ",budget_mb=" << budget / (1024 * 1024) << '\n';
        }
        static ULONGLONG nextLog = 0;
        if (now >= nextLog)
        {
            nextLog = now + 10000;
            // The pose records (all, still visited by the worker, sealed, removed so far)
            // and the process's private bytes, so a long buffer session shows whether
            // memory grows with the entities that left it.
            const replay::pose::capture::RecordCounts poseRecords = replay::pose::capture::RecordCountsSnapshot();
            PROCESS_MEMORY_COUNTERS_EX memory{};
            memory.cb = sizeof(memory);
            const std::uint64_t privateMb = K32GetProcessMemoryInfo(GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))
                ? static_cast<std::uint64_t>(memory.PrivateUsage) / (1024 * 1024) : 0;
            LogFile() << "BUFFER_STATE,now_ms=" << nowMs << ",head_ms=" << g_bufferHeadMs
                      << ",window_ms=" << (nowMs - g_bufferHeadMs) << ",mb=" << bytes / (1024 * 1024)
                      << ",pose_mb=" << replay::pose::capture::TrackBytes() / (1024 * 1024)
                      << ",peak_mb=" << g_bufferPeakBytes / (1024 * 1024)
                      << ",trimmed_pose_chunks=" << g_bufferTrimmedPose << ",trimmed_fx_frames=" << g_bufferTrimmedFx
                      << ",fx_tracks=" << g_clip.registeredFx.size() << ",trimmed_fx_tracks=" << g_bufferTrimmedFxTracks
                      << ",manual=" << (g_manualInBuffer ? 1 : 0) << ",quiet_dropped_lines=" << g_logFilter.Dropped()
                      << ",pose_records=" << poseRecords.records << ",pose_drain=" << poseRecords.drain
                      << ",pose_sealed=" << poseRecords.sealed << ",pose_pruned=" << poseRecords.pruned
                      // Rope records in use, freed by the ring, refused full.
                      << ",rope_records=" << g_clip.ropes.Records().size() << ",rope_freed=" << g_clip.ropes.FreedRecords()
                      << ",rope_refused_full=" << g_clip.ropes.RefusedFull() << ",rope_still_samples=" << g_clip.ropes.StillSamples()
                      << ",private_mb=" << privateMb
                      // The real window and its limit.
                      << ",covered_s=" << coverage.coveredMs / 1000u << ",target_s=" << coverage.targetMs / 1000u
                      << ",limit=" << replay::buffercov::LimitText(coverage.limit) << ",budget_mb=" << budget / (1024 * 1024) << '\n';
        }
    }
}

    // F4 (and the editor's record button): without the buffer, start and stop;
    // with it, F4 marks a manual start inside the running ring and the second F4
    // saves from the mark to now.
    void PressRecord()
    {
        if (g_recording && g_recordKind == RecordKind::Buffer)
        {
            const std::uint32_t nowMs = replay::pose::capture::NowMs();
            if (!g_manualInBuffer)
            {
                g_manualInBuffer = true;
                g_manualStartMs = nowMs;
                g_manualPart = 0;
                LogFile() << "MANUAL_MARK,at_ms=" << nowMs << '\n';
            }
            else
            {
                g_manualInBuffer = false;
                // The chain's final save is the next part.
                const int partsSaved = g_manualPart;
                g_manualPart = replay::buffercov::FinalPartNumber(partsSaved);
                if (partsSaved > 0)
                    LogFile() << "MANUAL_CHAIN,final_part=" << g_manualPart << ",parts=" << g_manualPart << ",buffer=1,from_ms=" << g_manualStartMs
                              << ",to_ms=" << nowMs << ",part_mb=" << replay::buffercov::ManualPartBytes(g_recConfig.manualMaxMB, g_bufferBudgetBytes) / (1024 * 1024)
                              << ",manual_max_mb=" << g_recConfig.manualMaxMB << ",ring_budget_mb=" << g_bufferBudgetBytes / (1024 * 1024) << '\n';
                SaveRecordingWindow(g_manualStartMs, nowMs, "manual");
                g_manualPart = 0; // a later Shift+F4 or chain starts unnumbered
            }
            return;
        }
        if (g_recording)
            StopRecording();
        else
        {
            g_manualPart = 0;
            StartRecording();
        }
    }

    // The editor's Replay Settings: applied now and kept in RedemptionEditor.ini
    // for the next load.
    void SetReplayBuffer(bool enabled, std::uint32_t seconds, std::uint32_t megabytes)
    {
        g_recConfig.buffer = enabled;
        g_recConfig.bufferSeconds = std::clamp(seconds, 10u, 600u);
        g_recConfig.bufferMB = replay::buffercov::ClampBufferMB(megabytes); // under the clip file's 1 GiB
        char workingDirectory[MAX_PATH]{};
        GetCurrentDirectoryA(MAX_PATH, workingDirectory);
        const std::string configPath = replay::identity::IniPathIn(workingDirectory);
        replay::identity::IniWrite("ReplayBuffer", enabled ? "1" : "0", configPath.c_str());
        replay::identity::IniWrite("ReplayBufferSeconds", std::to_string(g_recConfig.bufferSeconds).c_str(), configPath.c_str());
        replay::identity::IniWrite("ReplayBufferMB", std::to_string(g_recConfig.bufferMB).c_str(), configPath.c_str());
        if (!enabled && g_recording && g_recordKind == RecordKind::Buffer)
            StopRecording(false);
        LogFile() << "RECORDING_CONFIG,buffer=" << (enabled ? 1 : 0) << ",buffer_s=" << g_recConfig.bufferSeconds
                  << ",buffer_mb=" << g_recConfig.bufferMB << ",from=editor\n";
    }

    // Clip compression from the editor's Replay Settings, saved to the ini like the
    // buffer's settings.
    replay::clipcompress::worker::Settings CompressSettings()
    {
        replay::clipcompress::worker::Settings settings;
        settings.automatic = g_recConfig.compressClips;
        settings.days = g_recConfig.compressAfterDays;
        settings.level = g_recConfig.compressLevel;
        settings.format = g_recConfig.compressFormat == 1 ? replay::clipcompress::Format::Zstd : replay::clipcompress::Format::Packed;
        settings.upgrade = settings.format == replay::clipcompress::Format::Packed;
        return settings;
    }

    void SetClipCompression(bool enabled, std::uint32_t days)
    {
        g_recConfig.compressClips = enabled;
        g_recConfig.compressAfterDays = std::clamp(days, 1u, 60u);
        char workingDirectory[MAX_PATH]{};
        GetCurrentDirectoryA(MAX_PATH, workingDirectory);
        const std::string configPath = replay::identity::IniPathIn(workingDirectory);
        replay::identity::IniWrite("CompressClips", enabled ? "1" : "0", configPath.c_str());
        replay::identity::IniWrite("CompressAfterDays", std::to_string(g_recConfig.compressAfterDays).c_str(),
            configPath.c_str());
        replay::clipcompress::worker::Configure(CompressSettings());
        LogFile() << "RECORDING_CONFIG,compress_clips=" << (enabled ? 1 : 0) << ",compress_after_days="
                  << g_recConfig.compressAfterDays << ",from=editor\n";
    }

    // The compressor's side of the script tick: its log lines, the clip
    // list after a file was replaced, and the saves and the open clip it
    // must stay off.
    void ServiceClipCompressor()
    {
        replay::clipcompress::worker::SetHold(SavePending() || ClipReadPending()); // and a clip read
        static std::wstring openClip;
        // The open clip is the one playing (after Back to Game no clip stays "open":
        // it can be compressed, and the gallery's C works on it). A save of it holds
        // the compressor.
        const std::wstring open = g_transport.Playing() ? g_currentClipPath : std::wstring();
        if (openClip != open)
        {
            openClip = open;
            replay::clipcompress::worker::SetOpenClip(openClip);
        }
        std::vector<std::string> lines;
        replay::clipcompress::worker::DrainLog(lines);
        for (const std::string& line : lines)
            LogFile() << line << '\n';
        static std::uint64_t changes = 0;
        const std::uint64_t now = replay::clipcompress::worker::ChangeSequence();
        if (now != changes)
        {
            changes = now;
            RebuildEditorClipList();
        }
    }

    // Shift+F4: the buffer's whole window to a clip, the recording goes on.
    void SaveBuffer()
    {
        if (!g_recording || g_recordKind != RecordKind::Buffer)
        {
            replay::hud::ShowNotice(TR("hud.buffer_off", "Redemption Editor: the replay buffer is off."));
            return;
        }
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        // The save says what it covers.
        const replay::buffercov::Coverage coverage =
            replay::buffercov::Evaluate(nowMs, g_bufferHeadMs, g_recConfig.bufferSeconds, g_manualInBuffer, g_manualStartMs);
        const std::size_t bytes = RecordingBytes();
        const double mbPerSecond = coverage.coveredMs
            ? static_cast<double>(bytes) / (1024.0 * 1024.0) / (static_cast<double>(coverage.coveredMs) / 1000.0) : 0.0;
        const bool shortWindow = replay::buffercov::ShortWindowNotice(coverage);
        LogFile() << "BUFFER_COVERAGE,phase=save,covered_ms=" << coverage.coveredMs << ",target_ms=" << coverage.targetMs
                  << ",limit=" << replay::buffercov::LimitText(coverage.limit) << ",mb=" << bytes / (1024 * 1024)
                  << ",budget_mb=" << g_bufferBudgetBytes / (1024 * 1024) << ",mb_per_s=" << mbPerSecond
                  << ",mb_for_target=" << replay::buffercov::MegabytesFor(g_recConfig.bufferSeconds, mbPerSecond)
                  << ",notice=" << (shortWindow ? 1 : 0) << '\n';
        if (SaveRecordingWindow(g_bufferHeadMs, nowMs, "buffer") && shortWindow)
        {
            const std::string text = TRF("hud.buffer_short", "Redemption Editor: saved the last {saved} seconds, not {asked}: the replay buffer memory ({mb} MB) "
                "fills faster in busy places. Raise Memory in Replay Settings to keep more.",
                {{"saved", std::to_string(coverage.coveredMs / 1000u)}, {"asked", std::to_string(coverage.targetMs / 1000u)},
                    {"mb", std::to_string(static_cast<unsigned>(g_bufferBudgetBytes / (1024 * 1024)))}});
            replay::hud::ShowNotice(text.c_str());
        }
    }

    void UpdateBufferAutoStart(ULONGLONG now)
    {
        if (!g_recConfig.buffer || g_recording || g_transport.Playing() || !g_context.puppets.list.empty() || g_world.cleared ||
            now < g_bufferResumeAt || g_loadedClipHoldsBuffer || !g_pendingLoadPath.empty()) // nor while a clip opens
            return;
        const Player player = PLAYER::PLAYER_ID();
        if (!PLAYER::IS_PLAYER_PLAYING(player) || CAM::IS_SCREEN_FADED_OUT() || CAM::IS_SCREEN_FADING_OUT())
        {
            g_bufferResumeAt = now + 1000;
            return;
        }
        if (replay::pose::capture::FinalizerWanted())
        {
            g_bufferResumeAt = now + 2000;
            return;
        }
        std::string error;
        if (!replay::pose::capture::AcquireFinalizer(error))
        {
            g_bufferResumeAt = now + 5000; // the Pose Editor has it
            return;
        }
        StartRecording(RecordKind::Buffer);
    }
    replay::savequeue::Queue<ClipSaveData> g_clipSaves;
    std::unique_ptr<PendingSave> g_pendingSave;
    bool SavePending() { return g_pendingSave != nullptr; }
    std::size_t HeldClipSaves() { return g_clipSaves.HeldCount(); }

    std::string ClipFileName(const std::wstring& path)
    {
        return Narrow(path.substr(path.find_last_of(L'\\') + 1));
    }
namespace
{

    // The next due job on its thread (none while one is written).
    void StartNextClipSave(ULONGLONG now)
    {
        if (g_pendingSave)
            return;
        const auto* job = g_clipSaves.Begin(now);
        if (!job)
            return;
        auto pending = std::make_unique<PendingSave>();
        pending->jobId = job->id;
        pending->data = job->payload;
        pending->attempt = job->failures + 1;
        pending->path = job->path;
        std::uintptr_t base = 0;
        std::size_t size = 0;
        ImageRange(base, size);
        // Bench only: RedemptionEditor.ini ClipSaveFailTest=N fails the first N writes
        // of this load (the held save, its retry and the notices checked without a
        // full disk). 0 by default; read only with Debug=1, the product never fails a
        // save on purpose.
        static int failTest = g_debugMode ? -1 : 0;
        if (failTest < 0)
        {
            char workingDirectory[MAX_PATH]{};
            GetCurrentDirectoryA(MAX_PATH, workingDirectory);
            const std::string configPath = replay::identity::IniPathIn(workingDirectory);
            failTest = static_cast<int>(replay::identity::DevIniInt("ClipSaveFailTest", 0, configPath.c_str()));
        }
        const bool fail = failTest > 0;
        if (fail)
            --failTest;
        PendingSave* save = pending.get();
        save->worker = std::thread([save, base, size, fail]() {
            replay::threads::Scope threadName("clip_save");
            const ClipSaveData& data = save->data;
            if (fail)
                save->error = "ClipSaveFailTest";
            else
            {
                // As the read above; a failed save is retried by the save queue.
                try
                {
                    replay::clipfile::SaveOptions options;
                    options.pack = true;
                    options.zstdLevel = replay::clipcompress::kSavePackLevel;
                    options.threads = replay::clipcompress::kSavePackThreads;
                    save->ok = replay::clipfile::Save(*data.recording, *data.poses, *data.camera, data.durationMs, base,
                        size, save->path, save->error, options, &save->saveInfo);
                }
                catch (const std::exception& exception)
                {
                    save->ok = false;
                    save->error = std::string("exception_") + exception.what();
                }
                catch (...)
                {
                    save->ok = false;
                    save->error = "unknown_exception";
                }
                // The generic sound report beside a saved new recording (pure formatting and
                // one file; no engine).
                if (save->ok && data.soundGenericStats && data.recording)
                {
                    try
                    {
                        save->reportOk = replay::soundgeneric::WriteReport(*data.recording, *data.soundGenericStats, save->path,
                            data.durationMs, save->reportPath, save->reportError);
                    }
                    catch (...)
                    {
                        save->reportOk = false;
                        save->reportError = "exception";
                    }
                }
            }
            save->done.store(true);
        });
        g_pendingSave = std::move(pending);
    }

    // A save asked for: its job owns `data` until the file is written.
    void QueueClipSave(const std::wstring& path, ClipSaveData data)
    {
        g_clipSaves.Add(path, std::move(data));
        StartNextClipSave(GetTickCount64());
    }
}

    // True with the snapshot of the newest save of `path` not written yet;
    // false with no error when there is none (the file is read).
    bool CopyQueuedClip(const std::wstring& path, replay::Recording& recording, std::vector<replay::pose::PoseRecordExport>& poses,
        replay::camera::Track& camera, std::uint64_t& durationMs, std::string& error)
    {
        error.clear();
        const auto* job = g_clipSaves.Find(path);
        if (!job)
            return false;
        try
        {
            recording = *job->payload.recording;
            poses = *job->payload.poses;
            camera = *job->payload.camera;
        }
        catch (const std::bad_alloc&)
        {
            error = "not enough memory to open the clip";
            return false;
        }
        durationMs = job->payload.durationMs;
        LogFile() << "CLIP_LOAD_FROM_SAVE,path=" << Narrow(path) << ",failures=" << job->failures << '\n';
        return true;
    }

    // wait: the write in progress is done when this returns (and no other
    // starts); otherwise a finished write is taken and the next due job
    // starts.
    void FinishPendingSave(bool wait)
    {
        if (g_pendingSave && (wait || g_pendingSave->done.load()))
        {
            g_pendingSave->worker.join();
            const std::unique_ptr<PendingSave> finished = std::move(g_pendingSave);
            const PendingSave& save = *finished;
            const ClipSaveData& data = save.data;
            replay::savequeue::Queue<ClipSaveData>::Job job;
            const replay::savequeue::Outcome outcome = g_clipSaves.Finish(save.jobId, save.ok, save.error, GetTickCount64(), &job);
            const std::string name = ClipFileName(save.path);
            // A failed save of the open clip leaves its changes unsaved (the flag was
            // cleared when the save began).
            if (!save.ok && (data.why == "editor" || data.why == "leave") && g_memoryClipPath == save.path)
                g_clipEdited = true;
            if (outcome == replay::savequeue::Outcome::Saved)
            {
                g_lastWrittenClipName = name;
                // Only a new recording's first write takes the editor's frames as its
                // thumbnail; an edit of a clip keeps its own (the frames kept are another
                // recording's), and so does a retried one.
                if (data.newRecording && save.attempt == 1)
                {
                    g_savedClipMs = data.durationMs;
                    g_savedClipName = name;
                    ++g_savedClipSequence;
                }
                if (save.attempt > 1)
                    replay::hud::ShowNotice(TRF("hud.saved_now", "Redemption Editor: {name} is saved now.", {{"name", name}}).c_str());
                RebuildEditorClipList();
            }
            else if (outcome == replay::savequeue::Outcome::Held && job.failures == 1)
            {
                // The notice is the editor's; the game's text line says a failure.
                replay::hud::ShowNotice(TRF("hud.save_held", "Redemption Editor: could not save {name} ({error}). The replay is kept in memory and saving is retried; "
                    "free some disk space.", {{"name", name}, {"error", save.error}}).c_str());
            }
            // The clip's entities and size on disk.
            WIN32_FILE_ATTRIBUTE_DATA written{};
            const double savedMb = save.ok && GetFileAttributesExW(save.path.c_str(), GetFileExInfoStandard, &written)
                ? static_cast<double>((static_cast<std::uint64_t>(written.nFileSizeHigh) << 32) | written.nFileSizeLow) / (1024.0 * 1024.0)
                : 0.0;
            char savedMbText[24]{};
            std::snprintf(savedMbText, sizeof(savedMbText), "%.1f", savedMb);
            if (data.soundGenericStats && save.ok)
                LogFile() << "SOUND_GENERIC_REPORT,clip=" << name << ",ok=" << (save.reportOk ? 1 : 0) << ",path="
                          << (save.reportOk ? Narrow(save.reportPath) : std::string("none")) << ",error="
                          << (save.reportError.empty() ? "none" : save.reportError.c_str()) << '\n';
            LogFile() << "BUFFER_SAVE,why=" << data.why << ",from_ms=" << data.fromMs << ",to_ms=" << data.toMs
                      << ",base_ms=" << data.baseMs << ",duration_ms=" << data.durationMs << ",ok=" << (save.ok ? 1 : 0)
                      << ",entities=" << (data.recording ? data.recording->created.size() : 0) << ",mb=" << savedMbText
                      << ",cut_ms=" << data.cutMs << ",total_ms=" << (GetTickCount64() - data.startedAt)
                      << ",packed=" << (save.saveInfo.packed ? 1 : 0) << ",clip_bytes=" << save.saveInfo.clipBytes
                      << ",encode_ms=" << save.saveInfo.encodeMs << ",pack_ms=" << save.saveInfo.packMs
                      << ",verify_ms=" << save.saveInfo.verifyMs << ",write_ms=" << save.saveInfo.writeMs
                      << ",pack_fallback=" << (save.saveInfo.fallback.empty() ? "none" : save.saveInfo.fallback.c_str())
                      << ",attempt=" << save.attempt << ",outcome=" << static_cast<int>(outcome)
                      << ",held=" << g_clipSaves.HeldCount() << ",queued=" << g_clipSaves.Size()
                      << ",path=" << Narrow(save.path) << ",error=" << (save.ok ? "none" : save.error.c_str()) << '\n';
        }
        if (!wait)
            StartNextClipSave(GetTickCount64());
    }

    // Unload: the saves not written yet (held ones too) get one last try each on
    // this thread; the data goes with the module.
    void FlushClipSaves()
    {
        FinishPendingSave(true);
        if (g_clipSaves.Empty())
            return;
        std::uintptr_t base = 0;
        std::size_t size = 0;
        ImageRange(base, size);
        for (const auto& job : g_clipSaves.Jobs())
        {
            std::string error;
            const ClipSaveData& data = job.payload;
            const bool ok = replay::clipfile::Save(*data.recording, *data.poses, *data.camera, data.durationMs, base, size, job.path, error);
            LogFile() << "CLIP_SAVE_FLUSH,path=" << Narrow(job.path) << ",ok=" << (ok ? 1 : 0) << ",failures_before=" << job.failures
                      << ",error=" << (ok ? "none" : error.c_str()) << '\n';
        }
        g_clipSaves = {};
    }
namespace
{

    // Clip names "NNN-Place": the highest counter given out in this session; a
    // save not written yet is not on disk for the scan.
    std::uint32_t g_lastClipNumber = 0;

    // A new clip's file, with -2, -3... only when that name is on disk or held by
    // a save not written yet (two saves in one second). NNN-Place (042-Valentine,
    // the part's -pN after it): the counter is the highest NNN of the folder plus
    // one (clip-* names do not count), the place is the clip's PLAC text where the
    // recording started (clipfile::ClipPlaceTag, Wild without one).
    std::wstring NewClipPath(const replay::Place& place, const wchar_t* suffix = L"")
    {
        std::vector<std::wstring> names; // every read root's: the numbers go on after the game folder's clips
        for (const replay::storage::Entry& entry : replay::storage::List(kClipFolder, L"*.rpclip"))
            names.push_back(entry.name);
        const std::uint32_t number = (std::max)(replay::clipfile::NextClipNumber(names), g_lastClipNumber + 1);
        g_lastClipNumber = number;
        const std::wstring base = replay::clipfile::NumberedClipBase(number,
            replay::clipfile::ClipPlaceTag(place.town, place.district, place.state), suffix);
        std::wstring name = replay::clipfile::UniqueClipName(base, [](const std::wstring& candidate) {
            const std::wstring path = ClipPath(candidate);
            return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES ||
                replay::storage::Exists(FindClipFile(candidate + L".rpclip")) ||
                GetFileAttributesW((path + L".tmp").c_str()) != INVALID_FILE_ATTRIBUTES || g_clipSaves.Holds(path);
        });
        if (name.empty())
            name = std::wstring(base) + L"-" + std::to_wstring(GetTickCount64());
        return ClipPath(name);
    }
}

    // A window of the running recording to a clip: cut from the lifecycle block
    // holding fromMs, times from 0, saved while the recording goes on.
    bool SaveRecordingWindow(std::uint32_t fromMs, std::uint32_t toMs, const char* why)
    {
        // No wait for the save before this one: the cut is a job that follows it.
        const ULONGLONG startedAt = GetTickCount64();
        ClipSaveData data;
        replay::window::Result result;
        bool cutOk = false;
        try
        {
            // The frames still in the capture rings first (PoseWorkerPollMs above 5 leaves
            // them there longer); a stopped recording was drained by StopAll.
            replay::pose::capture::FlushRings();
            const std::vector<replay::pose::PoseRecordExport> poses = replay::pose::capture::ExportRecords();
            auto cut = std::make_shared<replay::Recording>();
            auto cutPoses = std::make_shared<std::vector<replay::pose::PoseRecordExport>>();
            cutOk = replay::window::Cut(g_clip, poses, toMs, fromMs, toMs, *cut, *cutPoses, result);
            // The PLAC (and the name) of the window's start, not of the buffer's.
            if (cutOk && g_recordKind == RecordKind::Buffer)
                replay::window::ApplyWindowPlace(cut->place, g_bufferPlaces, fromMs);
            data.recording = std::move(cut);
            data.poses = std::move(cutPoses);
            data.camera = std::make_shared<const replay::camera::Track>();
        }
        catch (const std::bad_alloc&)
        {
            cutOk = false;
            result.error = "not enough memory for the cut";
        }
        if (!cutOk)
        {
            LogFile() << "BUFFER_SAVE,why=" << why << ",ok=0,error=" << result.error << '\n';
            replay::hud::ShowNotice(TR("hud.save_failed", "Redemption Editor: could not save the replay."));
            return false;
        }
        data.durationMs = result.durationMs;
        data.why = why;
        data.newRecording = true;
        data.startedAt = startedAt;
        data.cutMs = GetTickCount64() - startedAt;
        data.fromMs = fromMs;
        data.toMs = toMs;
        data.baseMs = result.baseMs;
        data.soundGenericStats = replay::soundgeneric::StatsSnapshot();
        // The name's place is the window's own PLAC (the cut copies it). Only a manual
        // chain's saves carry the part (a Shift+F4 during a chain does not take the
        // part's name).
        const bool manualSave = std::strcmp(why, "manual") == 0 || std::strcmp(why, "manual_part") == 0;
        const std::wstring path = NewClipPath(data.recording->place,
            manualSave && g_manualPart > 0 ? (L"-p" + std::to_wstring(g_manualPart)).c_str() : L"");
        QueueClipSave(path, std::move(data));
        return true;
    }
namespace
{

    // The manual recording that just stopped, written on a thread like the
    // buffer's windows. The clip stays in memory for playback; the job owns a copy.
    void SaveStoppedRecording(const std::wstring& path)
    {
        const ULONGLONG startedAt = GetTickCount64();
        ClipSaveData data;
        std::uint64_t copyT[3] = {replay::prof::Now(), 0, 0}; // the copy's parts
        try
        {
            data.recording = std::make_shared<const replay::Recording>(g_clip);
            copyT[1] = replay::prof::Now();
            data.poses = std::make_shared<const std::vector<replay::pose::PoseRecordExport>>(replay::pose::capture::ExportRecords());
            copyT[2] = replay::prof::Now();
            data.camera = std::make_shared<const replay::camera::Track>(g_cameraTrack);
            LogFile() << "BUFFER_SAVE_COPY,why=manual_stop,recording_ms=" << static_cast<long long>(TicksMs(copyT[1] - copyT[0]))
                      << ",poses_ms=" << static_cast<long long>(TicksMs(copyT[2] - copyT[1])) << ",camera_ms=" << static_cast<long long>(TicksMs(replay::prof::Now() - copyT[2]))
                      << ",pose_records=" << data.poses->size() << '\n';
        }
        catch (const std::bad_alloc&)
        {
            // The copy did not fit; the recording stays in memory (playback, then the
            // editor's Ctrl+S writes it).
            LogFile() << "BUFFER_SAVE,why=manual_stop,ok=0,error=not enough memory for the copy\n";
            replay::hud::ShowNotice(replay::keys::Fill(TR("hud.no_memory_replay", "Redemption Editor: not enough memory to save the replay. Play it "
                                                                                  "and save it from the editor ({key_save})."),
                replay::keys::Current()).c_str());
            return;
        }
        data.why = "manual_stop";
        data.newRecording = true;
        data.durationMs = static_cast<std::uint32_t>(g_transport.DurationMs());
        data.soundGenericStats = replay::soundgeneric::StatsSnapshot();
        data.startedAt = startedAt;
        data.cutMs = GetTickCount64() - startedAt; // the copy
        data.toMs = data.durationMs;
        g_currentClipPath = path;
        g_memoryClipPath = path;
        g_clipEdited = false;
        QueueClipSave(path, std::move(data));
    }
}

    void SaveEditedClip(const char* why)
    {
        if (!g_transport.Playing() || g_transport.DurationMs() == 0)
            return;
        // A project's clip: its markers are the project's (edit markers live in the
        // montage, never in the clip file).
        if (ProjectOwnsPlayback())
        {
            SyncProjectMarkers();
            g_clipEdited = false;
            LogFile() << "CLIP_EDIT_SAVE,why=" << why << ",skipped=project_clip\n";
            return;
        }
        // A save asked for retries the held ones now (the disk may have room again);
        // no wait for the save before it.
        g_clipSaves.RetryNow();
        const bool ownFile = !g_memoryClipPath.empty();
        const std::wstring path = ownFile ? SavableClipPath(g_memoryClipPath) : NewClipPath(g_clip.place);
        const ULONGLONG startedAt = GetTickCount64();
        ClipSaveData data;
        try
        {
            data.recording = std::make_shared<const replay::Recording>(g_clip);
            data.poses = std::make_shared<const std::vector<replay::pose::PoseRecordExport>>(replay::pose::capture::ExportRecords());
            data.camera = std::make_shared<const replay::camera::Track>(g_cameraTrack);
        }
        catch (const std::bad_alloc&)
        {
            LogFile() << "CLIP_EDIT_SAVE,why=" << why << ",ok=0,error=not enough memory for the copy\n";
            replay::hud::ShowNotice(TR("hud.no_memory_changes", "Redemption Editor: not enough memory to save the changes. They stay in the open clip."));
            return; // the changes stay flagged
        }
        data.editorSave = ownFile;
        data.newRecording = !ownFile;
        data.why = why;
        data.durationMs = static_cast<std::uint32_t>(g_transport.DurationMs());
        data.startedAt = startedAt;
        data.cutMs = GetTickCount64() - startedAt;
        data.toMs = data.durationMs;
        g_memoryClipPath = path;
        g_currentClipPath = path;
        g_clipEdited = false;
        replay::clipcompress::worker::SetOpenClip(path); // never compressed while written
        QueueClipSave(path, std::move(data));
        LogFile() << "CLIP_EDIT_SAVE,why=" << why << ",path=" << Narrow(path) << ",markers=" << g_cameraTrack.Keyframes().size() << '\n';
    }

    // F7 or the editor's play with the buffer running: the buffer stops
    // (no file) and its window becomes the clip in memory.
    void StopBufferForPlayback()
    {
        const std::uint32_t headMs = g_bufferHeadMs;
        StopRecording(false);
        const std::uint64_t durationMs = g_transport.DurationMs();
        if (durationMs == 0)
            return;
        replay::Recording cut;
        std::vector<replay::pose::PoseRecordExport> cutPoses;
        replay::window::Result result;
        std::string error;
        const bool ok = replay::window::Cut(g_clip, replay::pose::capture::ExportRecords(), durationMs, headMs,
                            static_cast<std::uint32_t>(durationMs), cut, cutPoses, result) &&
            replay::pose::capture::ImportRecords(cutPoses, error);
        if (ok)
        {
            g_clip = std::move(cut);
            g_clip.fromDisk = true; // the live world's objects are not the recording's leftovers
            g_memoryClipPath.clear(); // no file yet
            g_currentClipPath.clear(); // the open clip is no file's (T, C, Delete looked at the last one)
            g_clipEdited = false;
            g_markerHistory = {};
            ++g_clipLoadSequence;
            g_cameraTrack = {};
            g_cameraTrackManualOverride = true;
            g_transport.SetDurationMs(result.durationMs);
        }
        // base_ms, the capture clock at the clip's 0 (the block that holds head_ms), so
        // a log reader can line the recording's times up with the playback's
        // (tools/volume_probe_diff.py).
        LogFile() << "BUFFER_TO_PLAYBACK,head_ms=" << headMs << ",base_ms=" << (ok ? result.baseMs : 0)
                  << ",duration_ms=" << (ok ? result.durationMs : 0) << ",ok=" << (ok ? 1 : 0) << ",error=" << (ok ? "none" : (error.empty() ? result.error : error).c_str()) << '\n';
    }
namespace
{

    void ResetRecordGate(); // below UpdateRecording
}

    void StartRecording(RecordKind kind)
    {
        // The cleared world only waits for the save reload (no recording in the
        // editor's world).
        if (g_world.cleared)
        {
            if (!g_world.recordRefusedLogged)
            {
                g_world.recordRefusedLogged = true;
                LogFile() << "WORLD_MODEL,phase=record_refused,kind=" << (kind == RecordKind::Buffer ? "buffer" : "manual") << '\n';
            }
            if (kind != RecordKind::Buffer)
                replay::hud::ShowNotice(TR("hud.leave_replay", "Redemption Editor: leave the replay first; the world comes back with your last save."));
            return;
        }
        const std::uint64_t startedAt = replay::prof::Now();
        g_loadedClipHoldsBuffer = false; // a new recording replaces the loaded clip
        if (g_transport.Playing())
            StopPlayback();
        if (!TakePoseFinalizer())
            return;
        replay::prof::ResetSession();

        g_cameraTrack.Clear();
        g_memoryClipPath.clear();
        g_clipEdited = false;
        g_markerHistory = {};
        ++g_clipLoadSequence;
        g_cameraTrackEnabled = true;
        g_cameraTrackManualOverride = false;


        g_recordingStartedAt = GetTickCount64();
        g_transport.SetDurationMs(0);
        g_recording = true;
        ResetRecordGate();

        std::ofstream& log = LogFile();
        log << "CAPTURE_MODE"
            << ",role=pose_track_v2"
            << ",task_native_actor_calls=0"
            << ",transform_actor_calls=0"
            << ",pose_track=v2_capture"
            << '\n';
        log.flush();
        replay::recorder::Start();
        replay::doors::BeginRecording(g_clip);
        // The clip's world: clock and weather at the start.
        g_clip.environment.clockStartS = static_cast<std::uint32_t>(
            CLOCK::GET_CLOCK_HOURS() * 3600 + CLOCK::GET_CLOCK_MINUTES() * 60 + CLOCK::GET_CLOCK_SECONDS());
        MISC::GET_CURR_WEATHER_STATE(reinterpret_cast<Hash*>(&g_clip.environment.weatherFrom),
            reinterpret_cast<Hash*>(&g_clip.environment.weatherTo), &g_clip.environment.weatherMix);
        RecordPlace();
        replay::world::BeginRecording(g_clip, log); // the map state at the start

        g_recordKind = kind;
        replay::pose::fidelity::SetBufferMode(kind == RecordKind::Buffer); // keeps its last 120 s
        g_bufferHeadMs = 0;
        g_bufferPlaces.clear();
        if (g_clip.place.recorded)
            replay::window::AddPlaceSample(g_bufferPlaces, 0, g_clip.place);
        g_manualInBuffer = false;
        g_bufferNextBlockAt = GetTickCount64() + kBufferBlockMs;
        g_bufferPeakBytes = 0;
        g_bufferTrimmedPose = g_bufferTrimmedFx = g_bufferTrimmedFxTracks = 0;
        g_recTiming = {};
        g_recTiming.startMs = TicksMs(replay::prof::Now() - startedAt);
        ++g_recordingsThisLoad;
        const replay::hooks::Stats hooks = replay::hooks::Snapshot();
        log << "REC_START_TIMING,ms=" << g_recTiming.startMs << ",recording=" << g_recordingsThisLoad
            << ",single_enables=" << hooks.enables << ",single_enable_ms=" << hooks.enableMsTotal
            << ",batched=" << hooks.queued << '\n';

        log << "RECORDING_START,kind=" << (kind == RecordKind::Buffer ? "buffer" : "manual") << ",part=" << g_manualPart << '\n';
        if (kind == RecordKind::Manual)
            ShowObjective(("Redemption Editor: REC (" + KeyText(replay::keys::Action::Record) + " stops)").c_str());
    }

    void StopRecording(bool save)
    {
        if (!g_recording)
            return;
        // The buffer never becomes a file by stopping (the bench, a clip
        // load, a playback stop it); only Shift+F4 or a manual mark saves.
        if (g_recordKind == RecordKind::Buffer)
            save = false;
        // Where the stop tick goes (a long clip's stop can hold the game for hundreds
        // of ms): report, recorder stop, save copy.
        const std::uint64_t stopT0 = replay::prof::Now();
        {
            std::ofstream& log = LogFile();
            replay::prof::WriteSessionReport(log, "record");
            log.flush();
        }
        const std::uint64_t stopT1 = replay::prof::Now();

        g_recording = false;
        // The timeline is the clock every record is stamped with (the capture clock),
        // capped like the recording itself. The wall clock started earlier: a
        // recording start that stalls (observer installs, 1.0 to 1.4 s) would make the
        // timeline that much longer than the tracks, and playback would freeze for the
        // difference at the end.
        const ULONGLONG wallMs = GetTickCount64() - g_recordingStartedAt;
        const ULONGLONG elapsedMs = replay::pose::capture::NowMs();
        g_transport.SetDurationMs(elapsedMs);

        g_clip.environment.clockEndS = static_cast<std::uint32_t>(
            CLOCK::GET_CLOCK_HOURS() * 3600 + CLOCK::GET_CLOCK_MINUTES() * 60 + CLOCK::GET_CLOCK_SECONDS());
        g_clip.environment.recorded = true;
        // With the recording clock paused (a mission's fade, the pause menu) the state
        // now is not the state at the end of the recorded timeline (a sample taken
        // after a fade can sweep the sun over the clip's last second). The last sample
        // before the pause stays the state at the end.
        if (!replay::pose::capture::IsClockPaused())
            RecordEnvironment(static_cast<std::uint32_t>(elapsedMs), true); // the state at the end
        else
            LogFile() << "ENV_TRACK,phase=record,why=stop_skipped_paused,time_ms=" << elapsedMs
                      << ",samples=" << g_clip.environment.track.size() << '\n';

        std::ofstream& log = LogFile();
        log << "REC_FRAME_GAPS,recording=" << g_recordingsThisLoad << ",ticks=" << g_recTiming.ticks
            << ",first_max_ms=" << g_recTiming.firstMaxMs << ",first_over33=" << g_recTiming.firstOver33
            << ",first_over100=" << g_recTiming.firstOver100 << ",max_ms=" << g_recTiming.maxMs
            << ",over100=" << g_recTiming.over100 << ",start_ms=" << g_recTiming.startMs << '\n';
        if (g_recordKind == RecordKind::Buffer)
            log << "BUFFER_STOP,save=" << (save ? 1 : 0) << ",head_ms=" << g_bufferHeadMs << ",now_ms=" << elapsedMs
                << ",peak_mb=" << g_bufferPeakBytes / (1024 * 1024) << ",trimmed_pose_chunks=" << g_bufferTrimmedPose
                << ",trimmed_fx_frames=" << g_bufferTrimmedFx << ",trimmed_fx_tracks=" << g_bufferTrimmedFxTracks << '\n';
        const std::uint64_t stopT2 = replay::prof::Now();
        replay::recorder::Stop(elapsedMs);
        const std::uint64_t stopT3 = replay::prof::Now();
        replay::doors::EndRecording(g_clip, log);
        replay::world::EndRecording(g_clip, log);
        for (replay::AbilitySpan& span : g_clip.abilities)
            span.open = false;
        replay::gate::CloseSpans(g_clip.spans, static_cast<std::uint32_t>(elapsedMs));
        log << "CAPTURE_STOP"
            << ",duration_ms=" << elapsedMs
            << ",wall_ms=" << wallMs
            << ",entities=" << PoseV2RecordedCount()
            << '\n';
        // The rope records of this recording, with Debug=0 too.
        // records: every rope recorded (none is given away any more); live_cap: the
        // engine's rope pool, the ropes alive at once; refused_full: ropes refused
        // with that many records live; freed: records of ropes the buffer's ring left.
        log << "POSE_V2_ROPE_RECORDS_SUMMARY,records=" << g_clip.ropes.Records().size()
            << ",live_cap=" << replay::ropes::kEngineRopePoolCapacity << ",max_live=" << g_clip.ropes.MaxLiveRecords()
            << ",refused_full=" << g_clip.ropes.RefusedFull() << ",freed=" << g_clip.ropes.FreedRecords()
            << ",still_samples=" << g_clip.ropes.StillSamples() << ",kb=" << replay::ropes::RopeRecordsBytes(g_clip.ropes.Records()) / 1024
            << '\n';
        log.flush();
        const std::uint64_t stopT4 = replay::prof::Now();
        if (save)
        {
            // A free name: NNN-Place. The parts of a manual recording saved in parts
            // end in -p1, -p2... like a chain inside the buffer: a split saves part
            // g_manualPart, the stop that ends the chain the next one.
            const int part = g_manualSplitting ? g_manualPart : replay::buffercov::FinalPartNumber(g_manualPart);
            SaveStoppedRecording(NewClipPath(g_clip.place, part > 0 ? (L"-p" + std::to_wstring(part)).c_str() : L""));
        }
        const std::uint64_t stopT5 = replay::prof::Now();
        g_manualInBuffer = false;
        if (!g_manualSplitting)
            g_manualPart = 0; // the chain ended; the next recording starts unnumbered
        replay::pose::capture::ReleaseFinalizer(); // the Pose Editor may take it now
        const auto stopMs = [](std::uint64_t ticks) { return static_cast<long long>(TicksMs(ticks)); };
        LogFile() << "REC_STOP_TIMING,save=" << (save ? 1 : 0) << ",report_ms=" << stopMs(stopT1 - stopT0)
                  << ",before_recorder_ms=" << stopMs(stopT2 - stopT1) << ",recorder_stop_ms=" << stopMs(stopT3 - stopT2)
                  << ",world_ms=" << stopMs(stopT4 - stopT3) << ",save_copy_ms=" << stopMs(stopT5 - stopT4)
                  << ",total_ms=" << stopMs(replay::prof::Now() - stopT0) << '\n';
    }
namespace
{

    // Dead Eye and Eagle Eye spans: the player's special abilities as the game
    // reports them, one span per activation.
    void RecordAbilities(std::uint32_t nowMs)
    {
        const Player player = PLAYER::PLAYER_ID();
        const bool active[2] = {PLAYER::_IS_SPECIAL_ABILITY_ACTIVE(player) != FALSE,
            PLAYER::_IS_SECONDARY_SPECIAL_ABILITY_ACTIVE(player) != FALSE};
        for (int kind = 0; kind < 2; ++kind)
        {
            replay::AbilitySpan* open = nullptr;
            for (replay::AbilitySpan& span : g_clip.abilities)
                if (span.open && static_cast<int>(span.kind) == kind)
                    open = &span;
            if (active[kind] && !open)
            {
                g_clip.abilities.push_back({nowMs, nowMs, static_cast<replay::AbilityKind>(kind), true});
                LogFile() << "ABILITY_RECORD,kind=" << (kind ? "eagle_eye" : "dead_eye") << ",start_ms=" << nowMs << '\n';
            }
            else if (open)
            {
                open->endMs = nowMs;
                if (!active[kind])
                {
                    open->open = false;
                    LogFile() << "ABILITY_RECORD,kind=" << (kind ? "eagle_eye" : "dead_eye") << ",end_ms=" << nowMs << '\n';
                }
            }
        }
    }

    // The weather manager's transition over time (weather_state.h), which
    // GET_CURR_WEATHER_STATE does not return: guarded reads at signature-checked
    // addresses. A mismatch (logged once a load) leaves the samples' transition
    // unknown, and the replay then leaves the engine's alone.
    bool g_weatherResolveLogged = false;
}
    bool WeatherStateReady()
    {
        std::string error;
        const bool ok = replay::weatherstate::Resolve(error);
        if (!g_weatherResolveLogged)
        {
            g_weatherResolveLogged = true;
            LogFile() << "WEATHER_TRANSITION,phase=resolve,ok=" << (ok ? 1 : 0) << ",error=" << (ok ? "none" : error.c_str())
                      << '\n';
        }
        return ok;
    }
namespace
{

    void ReadWeatherTransition(replay::EnvironmentSample& sample)
    {
        replay::weatherstate::Transition transition;
        if (!WeatherStateReady() || !replay::weatherstate::ReadTransition(transition) || !std::isfinite(transition.totalMs) ||
            !std::isfinite(transition.leftMs) || transition.totalMs < 0.0f)
            return;
        sample.transitionKnown = true;
        sample.transitionTo = transition.hash;
        sample.transitionTotalMs = transition.hash ? transition.totalMs : 0.0f;
        sample.transitionLeftMs = transition.hash ? transition.leftMs : 0.0f;
        sample.transitionFlag = transition.hash && transition.flag ? 1 : 0;
    }

    // The environment now (environment_track.h), for the track.
    replay::EnvironmentSample ReadEnvironmentNow(std::uint32_t nowMs)
    {
        replay::EnvironmentSample sample;
        sample.timeMs = nowMs;
        Hash from = 0, to = 0;
        float mix = 0.0f;
        MISC::GET_CURR_WEATHER_STATE(&from, &to, &mix);
        sample.weatherFrom = static_cast<std::uint32_t>(from);
        sample.weatherTo = static_cast<std::uint32_t>(to);
        sample.weatherMix = std::isfinite(mix) ? mix : 0.0f;
        sample.clockS = static_cast<std::uint32_t>(
            CLOCK::GET_CLOCK_HOURS() * 3600 + CLOCK::GET_CLOCK_MINUTES() * 60 + CLOCK::GET_CLOCK_SECONDS()) %
            replay::envtrack::kDaySeconds;
        const float wind = MISC::GET_WIND_SPEED();
        sample.windSpeed = std::isfinite(wind) ? wind : 0.0f;
        const Vector3 direction = MISC::GET_WIND_DIRECTION();
        const float heading = std::atan2(direction.y, direction.x);
        sample.windDirection = std::isfinite(heading) ? heading : 0.0f;
        const float rain = MISC::GET_RAIN_LEVEL();
        const float snow = MISC::GET_SNOW_LEVEL();
        sample.rain = std::isfinite(rain) ? rain : 0.0f;
        sample.snow = std::isfinite(snow) ? snow : 0.0f;
        ReadWeatherTransition(sample);
        return sample;
    }

    // The clouds' accumulators at 10 Hz, every sample kept (they move all the
    // time), and their periods with the first. Guarded reads at signature-checked
    // addresses (sky_state.h); a mismatch leaves the clip without the track
    // (logged once a load).
    std::uint32_t g_skyNextSampleMs = 0;
    bool g_skyResolveLogged = false;
    replay::log::LogLimit g_skyRecordLogs;
}
    std::string SkyValuesText(const replay::SkySample& s)
    {
        using namespace replay::sky;
        std::ostringstream text;
        text << "noise=" << s.value[kNoiseX] << ';' << s.value[kNoiseY] << ';' << s.value[kNoiseZ]
             << ",position=" << s.value[kPositionX] << ';' << s.value[kPositionY] << ';' << s.value[kPositionZ]
             << ",layer0=" << s.value[kLayer0X] << ';' << s.value[kLayer0Y] << ",layer1=" << s.value[kLayer1X] << ';'
             << s.value[kLayer1Y] << ",drift=" << s.value[kDriftX] << ';' << s.value[kDriftY] << ",region=" << s.value[kRegionX]
             << ';' << s.value[kRegionY] << ",unpinned=" << s.unpinned[0] << ';' << s.unpinned[1] << ';' << s.unpinned[2];
        return text.str();
    }
namespace
{
    void RecordSky(std::uint32_t nowMs, bool force)
    {
        replay::Environment& environment = g_clip.environment;
        if (environment.sky.empty())
            g_skyNextSampleMs = 0;
        if (!force && nowMs < g_skyNextSampleMs)
            return;
        g_skyNextSampleMs = nowMs + replay::skytrack::kSampleMs;
        std::string error;
        if (!replay::skystate::Resolve(error))
        {
            if (!g_skyResolveLogged)
            {
                g_skyResolveLogged = true;
                LogFile() << "SKY_TRACK,phase=resolve,ok=0,error=" << error << '\n';
            }
            return;
        }
        if (environment.sky.empty() && !replay::skystate::ReadPeriods(environment.skyPeriods))
        {
            if (g_skyRecordLogs.Take(5))
                LogFile() << "SKY_TRACK,phase=record,error=period_read\n";
            return;
        }
        replay::SkySample sample;
        if (!replay::skystate::Read(sample))
        {
            if (g_skyRecordLogs.Take(5))
                LogFile() << "SKY_TRACK,phase=record,error=read,time_ms=" << nowMs << '\n';
            return;
        }
        sample.timeMs = nowMs;
        const std::size_t before = environment.sky.size();
        replay::skytrack::Add(environment.sky, sample);
        if ((before == 0 || force) && g_skyRecordLogs.Take(20))
        {
            const replay::SkyPeriods& p = environment.skyPeriods;
            LogFile() << "SKY_TRACK,phase=record,why=" << (before == 0 ? "first" : "stop") << ",time_ms=" << nowMs << ','
                      << SkyValuesText(sample) << ",period_noise=" << p.value[replay::sky::kPeriodNoise]
                      << ",period_position=" << p.value[replay::sky::kPeriodPosition]
                      << ",period_layer=" << p.value[replay::sky::kPeriodLayer]
                      << ",period_region=" << p.value[replay::sky::kPeriodRegion] << ",samples=" << environment.sky.size()
                      << '\n';
        }
    }

    // The script timecycle modifier and the weather variations, polled at 10 Hz
    // and kept when they move (always the first and the last). Guarded reads at
    // signature-checked addresses (timecycle_state.h); a mismatch leaves the clip
    // without the track (logged once a load).
    std::uint32_t g_tcNextSampleMs = 0;
    bool g_tcResolveLogged = false;
    replay::tctrack::Recorder g_tcRecorder;
    replay::log::LogLimit g_tcRecordLogs;
    std::string HashText(std::uint32_t hash)
    {
        char text[16];
        std::snprintf(text, sizeof(text), "0x%08X", hash);
        return text;
    }
}
    std::string TimecycleText(const replay::TimecycleSample& s)
    {
        std::ostringstream text;
        text << "mod=" << HashText(s.modifierKey) << ",mod_index=" << s.modifierIndex << ",strength=" << s.strength
             << ",trans=" << HashText(s.transitionKey) << ",trans_index=" << s.transitionIndex << ",progress=" << s.progress
             << ",speed=" << s.speed << ",fade=" << static_cast<int>(s.fadeOut) << ",variations=";
        if (!s.variationCount)
            text << "none";
        for (std::size_t n = 0; n < s.variationCount; ++n)
            text << (n ? ";" : "") << WeatherName(s.variations[n].type) << ':' << HashText(s.variations[n].variation);
        return text.str();
    }
namespace
{
    void RecordTimecycle(std::uint32_t nowMs, bool force)
    {
        std::vector<replay::TimecycleSample>& track = g_clip.environment.timecycle;
        if (track.empty())
        {
            g_tcNextSampleMs = 0;
            g_tcRecorder.Reset();
        }
        if (!force && nowMs < g_tcNextSampleMs)
            return;
        g_tcNextSampleMs = nowMs + replay::tctrack::kSampleMs;
        std::string error;
        if (!replay::tcstate::Resolve(error))
        {
            if (!g_tcResolveLogged)
            {
                g_tcResolveLogged = true;
                LogFile() << "TIMECYCLE_TRACK,phase=resolve,ok=0,error=" << error << '\n';
            }
            return;
        }
        replay::TimecycleSample sample;
        bool overflow = false;
        if (!replay::tcstate::Read(sample, &overflow))
        {
            if (g_tcRecordLogs.Take(5))
                LogFile() << "TIMECYCLE_TRACK,phase=record,error=read,time_ms=" << nowMs << '\n';
            return;
        }
        sample.timeMs = nowMs;
        const std::size_t before = track.size();
        const bool keysMoved = !track.empty() && !replay::tctrack::SameKeys(track.back(), sample);
        const bool kept = force ? g_tcRecorder.Finish(track, sample) : g_tcRecorder.Poll(track, sample);
        if (kept && (before == 0 || keysMoved || force || overflow) && g_tcRecordLogs.Take(40))
        {
            LogFile() << "TIMECYCLE_TRACK,phase=record,why=" << (before == 0 ? "first" : force ? "stop" : "change")
                      << ",time_ms=" << nowMs << ',' << TimecycleText(sample) << ",overflow=" << (overflow ? 1 : 0)
                      << ",modifiers=" << replay::tcstate::ModifierCount() << ",samples=" << track.size() << '\n';
        }
    }

    // The sky object's two moon angles, the drawn sun and moon and the clock date,
    // polled at 4 Hz and kept when an angle or the date moved (always the first
    // and the last). Guarded reads at the signature-checked object
    // (sun_control.h, no detour); a mismatch leaves the clip without the track
    // (logged once a load).
    std::uint32_t g_moonNextSampleMs = 0;
    bool g_moonResolveLogged = false;
    replay::moontrack::Recorder g_moonRecorder;
    replay::log::LogLimit g_moonRecordLogs;
}
    std::uint32_t LiveClockS()
    {
        return static_cast<std::uint32_t>(CLOCK::GET_CLOCK_HOURS() * 3600 + CLOCK::GET_CLOCK_MINUTES() * 60 + CLOCK::GET_CLOCK_SECONDS());
    }
    std::string MoonDateText(const replay::MoonSample& s)
    {
        std::ostringstream text;
        text << s.year << '-' << static_cast<int>(s.month) << '-' << static_cast<int>(s.day);
        return text.str();
    }
namespace
{
    std::string MoonText(const replay::MoonSample& s)
    {
        std::ostringstream text;
        text << "angle=" << s.angle << ",orbit=" << s.orbit << ",date=" << MoonDateText(s)
             << ",sun_el=" << replay::moontrack::ElevationDeg(s.sun) << ",moon_el=" << replay::moontrack::ElevationDeg(s.moon)
             << ",moon=" << s.moon[0] << ';' << s.moon[1] << ';' << s.moon[2];
        return text.str();
    }
}
    // The moon now (guarded reads; the date by the clock natives).
    bool ReadMoonNow(replay::MoonSample& sample)
    {
        replay::sunctl::MoonState state;
        if (!replay::sunctl::ReadMoon(state))
            return false;
        sample.angle = state.angle;
        sample.orbit = state.orbit;
        if (state.directions)
        {
            sample.sun[0] = state.sun.x;
            sample.sun[1] = state.sun.y;
            sample.sun[2] = state.sun.z;
            sample.moon[0] = state.moon.x;
            sample.moon[1] = state.moon.y;
            sample.moon[2] = state.moon.z;
        }
        sample.year = static_cast<std::uint16_t>(std::max(0, std::min(65535, CLOCK::GET_CLOCK_YEAR())));
        sample.month = static_cast<std::uint8_t>(std::max(0, std::min(12, CLOCK::GET_CLOCK_MONTH())));
        sample.day = static_cast<std::uint8_t>(std::max(0, std::min(31, CLOCK::GET_CLOCK_DAY_OF_MONTH())));
        return true;
    }
namespace
{
    void RecordMoon(std::uint32_t nowMs, bool force)
    {
        std::vector<replay::MoonSample>& track = g_clip.environment.moon;
        if (track.empty())
        {
            g_moonNextSampleMs = 0;
            g_moonRecorder.Reset();
        }
        if (!force && nowMs < g_moonNextSampleMs)
            return;
        g_moonNextSampleMs = nowMs + replay::moontrack::kSampleMs;
        const char* resolveError = replay::sunctl::ResolveError();
        if (std::strcmp(resolveError, "none") != 0)
        {
            if (!g_moonResolveLogged)
            {
                g_moonResolveLogged = true;
                LogFile() << "MOON_TRACK,phase=resolve,ok=0,error=" << resolveError << '\n';
            }
            return;
        }
        replay::MoonSample sample;
        if (!ReadMoonNow(sample))
        {
            if (g_moonRecordLogs.Take(5))
                LogFile() << "MOON_TRACK,phase=record,error=read,time_ms=" << nowMs << '\n';
            return;
        }
        sample.timeMs = nowMs;
        const std::size_t before = track.size();
        // A clock jump while recording (a mission's SET_CLOCK_TIME in the dark).
        const bool jumped = before && (std::fabs(replay::moontrack::WrapDelta(track.back().angle, sample.angle)) > replay::moontrack::kJump ||
                                          std::fabs(replay::moontrack::WrapDelta(track.back().orbit, sample.orbit)) > replay::moontrack::kJump);
        const bool kept = force ? g_moonRecorder.Finish(track, sample) : g_moonRecorder.Poll(track, sample);
        if (kept && (before == 0 || force || jumped) && g_moonRecordLogs.Take(40))
        {
            LogFile() << "MOON_TRACK,phase=record,why=" << (before == 0 ? "first" : force ? "stop" : "jump") << ",time_ms=" << nowMs
                      << ",clock_s=" << LiveClockS() << ',' << MoonText(sample) << ",samples=" << track.size() << '\n';
        }
    }

    // 4 Hz while the recording clock runs; a sample is kept when something moved
    // (and always the first and the last).
    std::uint32_t g_envNextSampleMs = 0;
    replay::log::LogLimit g_envRecordLogs; // per session
}
    void RecordEnvironment(std::uint32_t nowMs, bool force)
    {
        RecordSky(nowMs, force);
        RecordTimecycle(nowMs, force);
        RecordMoon(nowMs, force);
        std::vector<replay::EnvironmentSample>& track = g_clip.environment.track;
        if (track.empty())
            g_envNextSampleMs = 0;
        if (!force && nowMs < g_envNextSampleMs)
            return;
        g_envNextSampleMs = nowMs + replay::envtrack::kSampleMs;
        const replay::EnvironmentSample sample = ReadEnvironmentNow(nowMs);
        const std::size_t before = track.size();
        const bool weatherChanged = !track.empty() &&
            (track.back().weatherFrom != sample.weatherFrom || track.back().weatherTo != sample.weatherTo);
        // A transition started, ended or strayed.
        const bool transitionChanged = !track.empty() && replay::envtrack::TransitionChanged(track.back(), sample);
        if (!replay::envtrack::Add(track, sample) && force && !track.empty() && track.back().timeMs < nowMs)
            track.push_back(sample); // the last state at the stop
        if (track.size() != before && (before == 0 || weatherChanged || transitionChanged || force) && g_envRecordLogs.Take(40))
        {
            LogFile() << "ENV_TRACK,phase=record,time_ms=" << nowMs << ",from=" << WeatherName(sample.weatherFrom)
                      << ",to=" << WeatherName(sample.weatherTo) << ",mix=" << sample.weatherMix << ",clock_s=" << sample.clockS
                      << ",wind=" << sample.windSpeed << ",wind_dir=" << sample.windDirection << ",rain=" << sample.rain
                      << ",snow=" << sample.snow << ",tr_known=" << (sample.transitionKnown ? 1 : 0)
                      << ",tr_to=" << (sample.transitionTo ? WeatherName(sample.transitionTo) : std::string("none"))
                      << ",tr_total_ms=" << sample.transitionTotalMs << ",tr_left_ms=" << sample.transitionLeftMs
                      << ",tr_flag=" << static_cast<int>(sample.transitionFlag) << ",samples=" << track.size()
                      << ",why=" << (before == 0 ? "first" : force ? "stop" : weatherChanged ? "weather" : "transition") << '\n';
        }
    }
namespace
{

    // The recording gate's raw signals this tick (record_gate.h; every native
    // checked in inc/natives.h). A script camera counts only when it is not one of
    // ours (free camera, replay camera, the bench's).
    std::uint32_t ReadGateSignals()
    {
        using namespace replay::gate;
        std::uint32_t bits = 0;
        const Player player = PLAYER::PLAYER_ID();
        const Ped playerPed = PLAYER::PLAYER_PED_ID();
        if (HUD::IS_PAUSE_MENU_ACTIVE())
            bits |= SignalPauseMenu;
        if (CAM::_IS_ANIM_SCENE_CAM_ACTIVE())
            bits |= SignalAnimSceneCam;
        const Cam rendering = CAM::GET_RENDERING_CAM();
        if (rendering > 0 && CAM::DOES_CAM_EXIST(rendering) && rendering != g_freeCam && rendering != g_replayCam &&
            rendering != replay::bench::OwnCamera())
            bits |= SignalScriptCam;
        if (CAM::IS_GAMEPLAY_CAM_RENDERING())
            bits |= SignalGameplayCam;
        if (ANIMSCENE::IS_ENTITY_PLAYING_ANIM_SCENE(playerPed, -1))
            bits |= SignalPlayerAnimScene;
        if (CAM::HAS_LETTER_BOX())
            bits |= SignalLetterbox;
        if (!PLAYER::IS_PLAYER_CONTROL_ON(player))
            bits |= SignalControlOff;
        if (CAM::IS_SCREEN_FADED_OUT())
            bits |= SignalFadedOut;
        if (DLC::GET_IS_LOADING_SCREEN_ACTIVE() || SCRIPTS::IS_LOADING_SCREEN_VISIBLE())
            bits |= SignalLoading;
        if (CAM::IS_CINEMATIC_CAM_RENDERING())
            bits |= SignalCinematicCam;
        if (MISC::GET_MISSION_FLAG())
            bits |= SignalMission;
        return bits;
    }

    std::string GateSignalText(std::uint32_t bits)
    {
        using namespace replay::gate;
        char text[256]{};
        std::snprintf(text, sizeof(text),
            "bits=0x%x,menu=%u,anim_scene_cam=%u,script_cam=%u,gameplay_cam=%u,player_anim_scene=%u,letterbox=%u,"
            "control_off=%u,faded_out=%u,loading=%u,cinematic_cam=%u,mission=%u",
            bits, (bits & SignalPauseMenu) ? 1u : 0u, (bits & SignalAnimSceneCam) ? 1u : 0u, (bits & SignalScriptCam) ? 1u : 0u,
            (bits & SignalGameplayCam) ? 1u : 0u, (bits & SignalPlayerAnimScene) ? 1u : 0u, (bits & SignalLetterbox) ? 1u : 0u,
            (bits & SignalControlOff) ? 1u : 0u, (bits & SignalFadedOut) ? 1u : 0u, (bits & SignalLoading) ? 1u : 0u,
            (bits & SignalCinematicCam) ? 1u : 0u, (bits & SignalMission) ? 1u : 0u);
        return text;
    }

    void ResetRecordGate()
    {
        g_recordGate.Reset();
        g_spanScriptCam.Reset();
        g_spanControlsOff.Reset();
        g_gateReasons = g_gatePauseReasons = g_gateSignals = 0;
        g_gateLogged = false;
        g_gateSignalLogs = g_gateSampleLogs = 0;
        g_gateNextSampleAt = 0;
    }

    // One tick of the recording gate. The clock stops for the pause menu, a faded
    // out screen, a loading screen and (unless RecordCutscenes=1) a cutscene; it
    // goes on with a cut when the pause was more than the menu. The
    // restricted-camera spans are kept while the clock runs.
    void UpdateRecordGate()
    {
        using namespace replay::gate;
        const std::uint32_t signals = ReadGateSignals();
        const Decision decision = g_recordGate.Update(signals, g_recConfig.recordCutscenes);
        const bool wasPaused = replay::pose::capture::IsClockPaused();
        if (decision.paused)
            g_gatePauseReasons |= decision.reasons;
        std::ofstream& log = LogFile();
        bool cut = false;
        if (!decision.paused && wasPaused)
        {
            cut = PauseMakesCut(g_gatePauseReasons);
            if (cut)
            {
                replay::pose::capture::MarkCut();
                AddCut(g_clip.cuts, replay::pose::capture::NowMs());
            }
        }
        if (decision.paused != wasPaused)
            replay::pose::capture::SetClockPaused(decision.paused);
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        if (decision.paused != wasPaused || decision.reasons != g_gateReasons || !g_gateLogged)
        {
            g_gateLogged = true;
            log << "RECORD_PAUSE,paused=" << (decision.paused ? 1 : 0)
                << ",reason=" << ReasonName(decision.paused ? decision.reasons : (wasPaused ? g_gatePauseReasons : decision.reasons))
                << ",reasons=0x" << std::hex << decision.reasons << std::dec << ",cutscene=" << (decision.cutscene ? 1 : 0)
                << ",cut=" << (cut ? 1 : 0) << "," << GateSignalText(signals) << ",record_cutscenes="
                << (g_recConfig.recordCutscenes ? 1 : 0) << ",now_ms=" << nowMs
                << ",paused_total_ms=" << replay::pose::capture::PausedMsTotal() << '\n';
        }
        // The signal changes and the calibration samples are evidence for the bench
        // (Debug=1); RECORD_PAUSE stays in both.
        else if (signals != g_gateSignals && replay::log::Verbose() && g_gateSignalLogs < 200)
        {
            ++g_gateSignalLogs;
            log << "RECORD_GATE_SIGNALS," << GateSignalText(signals) << ",paused=" << (decision.paused ? 1 : 0)
                << ",now_ms=" << nowMs << '\n';
        }
        // Calibration: raw bits while a script camera renders, capped.
        const ULONGLONG wall = GetTickCount64();
        if ((signals & SignalScriptCam) && replay::log::Verbose() && wall >= g_gateNextSampleAt && g_gateSampleLogs < 120)
        {
            ++g_gateSampleLogs;
            g_gateNextSampleAt = wall + 1000;
            log << "RECORD_GATE_SAMPLE," << GateSignalText(signals) << ",cutscene=" << (decision.cutscene ? 1 : 0)
                << ",paused=" << (decision.paused ? 1 : 0) << ",now_ms=" << nowMs << '\n';
        }
        if (!decision.paused && wasPaused)
            g_gatePauseReasons = 0;
        g_gateReasons = decision.reasons;
        g_gateSignals = signals;

        // Spans: hysteresis every tick, spans only while the clock runs.
        g_spanScriptCam.Update((signals & SignalScriptCam) && !(signals & SignalGameplayCam));
        g_spanControlsOff.Update((signals & SignalControlOff) != 0);
        if (!decision.paused)
        {
            UpdateSpan(g_clip.spans, SpanKind::Cutscene, decision.cutscene && g_recConfig.recordCutscenes, nowMs);
            UpdateSpan(g_clip.spans, SpanKind::ScriptCamera, g_spanScriptCam.active, nowMs);
            UpdateSpan(g_clip.spans, SpanKind::ControlsOff, g_spanControlsOff.active, nowMs);
        }
    }
}

    // A tick that comes a second or more after the previous one means the game did
    // not run the script thread (its pause menu freezes it, so the gate never sees
    // the menu): that time leaves the recording clock as a pause. First thing in
    // the tick, before a save or a mark reads the clock.
    void ExcludeRecordingStall()
    {
        if (!g_recording)
            return;
        constexpr std::uint32_t kScriptStallMs = 1000;
        const std::uint32_t stallMs = replay::pose::capture::ExcludeScriptStall(kScriptStallMs);
        if (!stallMs)
            return;
        const std::uint32_t nowMs = replay::pose::capture::NowMs();
        const std::uint32_t pausedTotal = replay::pose::capture::PausedMsTotal();
        std::ofstream& log = LogFile();
        log << "RECORD_PAUSE,paused=1,reason=script_stall,stall_ms=" << stallMs << ",now_ms=" << nowMs
            << ",paused_total_ms=" << pausedTotal << '\n';
        log << "RECORD_PAUSE,paused=0,reason=script_stall,stall_ms=" << stallMs << ",now_ms=" << nowMs
            << ",paused_total_ms=" << pausedTotal << '\n';
    }
namespace
{

    // A first person scope (binoculars, a rifle scope, the camera kit) looked
    // through from third person play puts the rendered camera inside the
    // character's head; the clip records a third person camera there instead
    // (camera::ThirdPersonDuringScope). The game's own scripts test the binoculars
    // as the kit weapon plus IS_FIRST_PERSON_AIM_CAM_ACTIVE; full first person play
    // is left as recorded. The scope's blend in and out (the camera nearer the
    // head than kScopeHeadClearance) counts as scope too.
    replay::camera::ScopeAnchor g_scopeAnchor;

    void RecordThirdPersonCamera(replay::editor::CameraState& rendered, std::uint32_t cameraMs)
    {
        if (g_clip.gameCamera.empty())
            g_scopeAnchor = {}; // a new recording
        const Ped player = PLAYER::PLAYER_PED_ID();
        if (!player || !ENTITY::DOES_ENTITY_EXIST(player))
            return;
        const Vector3 root = ENTITY::GET_ENTITY_COORDS(player, TRUE, FALSE);
        const Vector3 head = PED::GET_PED_BONE_COORDS(player, 21030, 0.0f, 0.0f, 0.0f); // SKEL_Head
        const float dx = rendered.position.x - head.x, dy = rendered.position.y - head.y, dz = rendered.position.z - head.z;
        const bool nearHead = std::sqrt(dx * dx + dy * dy + dz * dz) < replay::camera::kScopeHeadClearance;
        const bool scope = CAM::IS_FIRST_PERSON_AIM_CAM_ACTIVE() && !CAM::_IS_IN_FULL_FIRST_PERSON_MODE();
        const replay::editor::Vec3 at{root.x, root.y, root.z};
        const bool wasInScope = g_scopeAnchor.inScope;
        if (scope || (wasInScope && nearHead))
        {
            const bool anchored = g_scopeAnchor.valid;
            rendered = replay::camera::ThirdPersonDuringScope(g_scopeAnchor, rendered, at);
            if (!wasInScope)
                LogFile() << "GAME_CAMERA_SCOPE,phase=start,ms=" << cameraMs << ",anchor=" << (anchored ? 1 : 0) << '\n';
            return;
        }
        if (wasInScope)
            LogFile() << "GAME_CAMERA_SCOPE,phase=end,ms=" << cameraMs << '\n';
        if (!nearHead)
            replay::camera::TrackThirdPerson(g_scopeAnchor, rendered, at);
        else
            g_scopeAnchor.inScope = false;
    }
}

    void UpdateRecording(const ProbeFrame& frame)
    {
        REPLAY_PROFILE_ZONE("tick.recording");
        if (!g_recording)
            return;

        // The recording gate (pause menu, cutscene, fade, loading screen). While it
        // holds, the recording clock and every observer stop (one check:
        // pose::capture::IsClockPaused); the detour queues are drained and discarded.
        UpdateRecordGate();

        replay::recorder::Update(frame.mount);
        if (!replay::pose::capture::IsClockPaused())
        {
            {
                const Vector3 at = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), TRUE, FALSE);
                replay::doors::UpdateRecording(g_clip, replay::pose::capture::NowMs(), at.x, at.y, at.z);
                // The map state, a slice of the candidates per tick.
                const Vector3 cam = CAM::GET_FINAL_RENDERED_CAM_COORD();
                const float player[3] = {at.x, at.y, at.z};
                const float camera[3] = {cam.x, cam.y, cam.z};
                replay::world::UpdateRecording(g_clip, replay::pose::capture::NowMs(), player, camera, LogFile());
            }
            RecordAbilities(replay::pose::capture::NowMs());
            RecordEnvironment(replay::pose::capture::NowMs(), false);

            // The rendered camera, one sample per captured frame: the clip's game camera.
            const std::uint32_t cameraMs = replay::pose::capture::NowMs();
            if (g_clip.gameCamera.empty() || g_clip.gameCamera.back().timeMs < cameraMs)
            {
                const Vector3 position = CAM::GET_FINAL_RENDERED_CAM_COORD();
                const Vector3 rotation = CAM::GET_FINAL_RENDERED_CAM_ROT(2);
                replay::editor::CameraState rendered{{position.x, position.y, position.z},
                    {rotation.x, rotation.y, rotation.z}, CAM::GET_FINAL_RENDERED_CAM_FOV()};
                RecordThirdPersonCamera(rendered, cameraMs);
                g_clip.gameCamera.push_back({cameraMs, rendered.position, rendered.rotation, rendered.fov});
            }
        }

        UpdateRecordingLimits();
    }
namespace
{
    float Lerp(float from, float to, float amount)
    {
        return from + (to - from) * amount;
    }
}
    void StopFreeCam()
    {
        g_freeCamVelocity = {};
        g_freeCamSmoothedLookX = 0.0f;
        g_freeCamSmoothedLookY = 0.0f;
        if (!g_freeCamEnabled && !g_freeCam)
            return;

        CAM::RENDER_SCRIPT_CAMS(FALSE, TRUE, 250, TRUE, TRUE, 0);
        if (g_freeCam && CAM::DOES_CAM_EXIST(g_freeCam))
            CAM::DESTROY_CAM(g_freeCam, FALSE);

        if (g_freeCamPlayer && ENTITY::DOES_ENTITY_EXIST(g_freeCamPlayer) && !g_freeCamPlayerWasFrozen)
            ENTITY::FREEZE_ENTITY_POSITION(g_freeCamPlayer, FALSE);

        g_freeCam = 0;
        g_freeCamPlayer = 0;
        g_freeCamEnabled = false;
        g_freeCamPlayerWasFrozen = false;
        g_cameraTrackManualOverride = false;
    }
