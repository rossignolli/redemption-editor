// replay_editor_addon_internal.h: the state and helpers the replay_editor_addon*.cpp files share.
// Internal to those files, not a public header.
#pragma once

#include <imgui.h>
#include <reshade.hpp>

#include "bench_shot.h"
#include "display_font.h"
#include "clip_thumbnails.h"
#include "seek_freeze.h"
#include "editor_protocol.h"
#include "project_name.h" // CleanName, CheckRename: the name screen shows what the core does
#include "env_override.h" // the weather names
#include "editor_input.h"
#include "editor_ui_qa.h"
#include "editor_cursor.h" // the cursor under ReShade 6.8
#include "world_notice.h"
#include "render_export/export_controller.h" // render export
#include "qa_marks.h" // the note box's time text
#include "qa_shots.h" // QA kit: screenshot ring, mark pictures, note box
#include "mod_identity.h" // RedemptionEditor.ini
#include "editor_keys_ini.h" // [Keys], the table the core loads too
#include "camera_overlay_channel.h" // the camera markers in the world
#include "camera_shake_custom.h" // the Custom shake's ranges and presets
#include "camera_effects.h" // Simple Camera's presets; the marker's shake rows
#include "camera_bones.h" // the bone list of the attach and Look At rows
#include "ui_layout_check.h" // the key bar's rows, the label fit
#include "ui_probe.h" // UI_PROBE_* tell the UI harness what is drawn (empty in the add-on)
#include "ui_art.h" // the painted panel edges and timeline (RedemptionEditor\ui)
#include "crash_notice.h" // the notice after a crash
#include "i18n.h" // TR, TRF: the texts in the player's language
#include "qa_report.h" // the report folder's name, for the crash notice
#include "rec_indicator.h" // Replay Settings' "Recording indicator"
#include "rec_indicator_window.h" // its window that screen capture leaves out
#include "ui_scale.h" // Replay Settings' "Interface scale"

#include <Windows.h>
#include <dxgi.h>
#include <shellapi.h> // SHQueryUserNotificationState
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

// Redemption Editor front end: home, clip gallery, alert, playback HUD and
// edit panel with timeline, drawn in ImGui with the look of the game's menus
// (dark translucent panels, warm white text, the red brush behind the
// selection). All UI text is English. The editor only talks to ReplayProbe
// through the shared transport (editor_protocol.h); it never touches the
// engine. While the editor is open ReShade keeps the keyboard from the game,
// so the core's own keys (K, , ., [ ], F11) do not reach it: the edit screen
// handles them itself. With the UI hidden (H) the keys go to the game and the
// core again.

    using namespace replay::editor;
    extern Snapshot g_snapshot;
    extern bool g_hasSnapshot;

    bool Send(CommandType type, const Command* payload = nullptr);
    bool SendNamed(CommandType type, const char* name);
    void PlayUi(UiSound sound);
    void PlayUiStep(int step);
    bool Flag(std::uint32_t flag);
    bool ClipListKnown();
    extern reshade::api::effect_runtime* g_runtime;
    bool MissedTap(int vk);
    bool KeyVk(int vk);
    bool MissedClick();
    bool MouseClicked();
    bool ItemClicked(bool pressed);

    // ImGui's press, or a whole press that fell inside the last frame.
    bool KeyPressed(ImGuiKey key, bool repeat = true);

    // The [Keys] table (editor_keys.h), loaded once from the same
    // RedemptionEditor.ini the core reads (its refusals are the core's log lines
    // and notice: same file, same pure Load, same result).
    using replay::keys::Action;
    namespace fixedkey = replay::keys::fixed; // the keys no [Keys] entry moves
    const replay::keys::Table& Keys();

    // A press of one of the action's keys with its modifiers (the same rule
    // as the core's, replay::keys::Matches); `repeat`: a held key repeats.
    bool ActionPressed(Action action, bool repeat = false);

    // The key bar's and the help's text of an action: "K", "F2 / E" with a
    // fixed key after, "-" when the action has no key.
    const char* KeyText(Action action, const char* extra = nullptr, const char* separator = " / ");
    const char* PairText(Action first, Action second);
    const char* JoinText(const char* first, const char* second);
    const char* KeyFill(const char* text);
    constexpr ImU32 kPanel = IM_COL32(12, 11, 10, 225);     // panel background
    constexpr ImU32 kPanelLine = IM_COL32(236, 230, 218, 60);
    constexpr ImU32 kText = IM_COL32(236, 230, 218, 255);   // warm white
    constexpr ImU32 kMuted = IM_COL32(150, 143, 131, 255);
    constexpr ImU32 kRed = IM_COL32(179, 22, 15, 255);      // the RDR2 red
    constexpr ImU32 kRedDim = IM_COL32(179, 22, 15, 120);
    constexpr ImU32 kGold = IM_COL32(226, 181, 79, 255);    // camera keyframes
    constexpr ImU32 kTrack = IM_COL32(236, 230, 218, 22);
    extern float g_scale;
    extern float g_vw;
    extern float g_vh;
    extern float g_ox;
    extern float g_oy;
    extern float g_footH;
    extern float g_uiScaleFactor;
    float S(float value);
    ImVec2 P(float x, float y);
    void MenuColumn(ImDrawList* draw, float solidWidth);

    // The inner padding of every panel that holds text (its content stands this
    // far from each edge): the ink edge's band (kInkEdge) and a clear margin
    // inside it.
    constexpr float kPanelPad = 28.0f;
    // Under a panel's last line: clear of the ink band (kInkEdge) too.
    constexpr float kPanelPadBottom = kPanelPad - 6.0f;

    // The panels' worn letterpress edge. The body is drawn inset by the edge; the
    // edge_ink strip (opaque inside, ragged outside) goes along the four sides in
    // the body's colour, tiled with every other tile mirrored, and the corners
    // are split on their diagonal between the two sides (a mitre: the two strips
    // never draw over each other). Without the art, a flat panel. Every text
    // panel draws through it.
    constexpr float kInkEdge = 16.0f;

    // A text panel from (x0, y0) to (x1, y1) in layout units; `edge` the ink
    // band (smaller on a small box, a quarter of its least side at most).
    // `corners` false: no corner pieces (a panel whose rows and buttons reach
    // into its corners).
    void InkPanel(ImDrawList* draw, float x0, float y0, float x1, float y1, ImU32 color, float edge = kInkEdge, bool corners = true);
    void Text(ImDrawList* draw, float x, float y, float size, ImU32 color, const char* text);
    float Paragraph(ImDrawList* draw, float x, float y, float size, ImU32 color, const char* text, float wrap);
    float ParagraphHeight(float size, const char* text, float wrap);
    float TextWidth(float size, const char* text);
    std::string Ellipsized(float size, const std::string& text, float maxWidth);
    void TextFit(ImDrawList* draw, float x, float y, float size, ImU32 color, const char* text, float maxWidth);
    float TitleWrapped(ImDrawList* draw, float x, float y, float size, ImU32 color, const char* text, float wrap);
    void Brush(ImDrawList* draw, float x, float y, float width, float height, ImU32 color);
    constexpr ImU32 kRowInkGrey = IM_COL32(206, 200, 190, 235);
    void TitleBanner(ImDrawList* draw, float x, float y, float minWidth, float extra, float height, float textX, float textY, float size, const char* title);
    void OrnamentDivider(ImDrawList* draw, float x, float y, float width);
    void SectionTitle(ImDrawList* draw, float x, float y, float width, const char* title);
    float KeyHint(ImDrawList* draw, float x, float y, const char* key, const char* label, bool on);
    float LabelHeight(float size);

    // A list row's label: in another language than English a long one is set
    // smaller (to 80 % of `size`) before it is cut with "..."; English draws
    // it as TextFit.
    // `shrink`: in English too (a row newer than the release layout).
    void RowTextFit(ImDrawList* draw, float x, float y, float size, ImU32 color, const char* text, float maxWidth, bool shrink = false);
    float LabelFit(ImDrawList* draw, float x, float y, float size, ImU32 color, const char* text, float maxWidth);

    // The busy cue: the cylinder turning one chamber at a time (a frame every
    // 50 ms, the loop 60 degrees), centred on (x, y). False without its art (the
    // caller keeps its text cue).
    bool LoadingCue(ImDrawList* draw, float x, float y, float size, ImU32 tint = IM_COL32(255, 255, 255, 255));
    void LoadingAfter(ImDrawList* draw, float x, float y, float textSize, const char* text, float maxRight);
    void DescriptionPanel(ImDrawList* draw, float x, float top, float minHeight, const char* heading, const char* body,
        const char* detail, const char* note, bool busy);

    // ---------------- The painted timeline ----------------
    // The track's torn strip spans kTrackTop to kTrackBottom from the bar's top
    // (y); the marks' first lane is its middle.
    constexpr float kTrackTop = -5.0f;
    constexpr float kTrackBottom = 7.0f;
    constexpr ImU32 kTrackTint = IM_COL32(150, 143, 131, 190);
    constexpr ImU32 kIconTint = IM_COL32(222, 222, 222, 255); // a mark at rest; hover and selection draw it at full brightness
    bool TimelineArtReady();
    void TornStrip(ImDrawList* draw, ui_art::Art art, float origin, float x0, float x1, float y0, float y1, ImU32 tint);
    void Icon(ImDrawList* draw, ui_art::Art art, float x, float y, float size, ImU32 tint);
    void OutlinedIcon(ImDrawList* draw, ui_art::Art art, float x, float y, float size, ImU32 tint);

    // The playhead: the watch crown and its needle, the needle's tip at
    // (x, tipY), the drawing `height` units tall (the art's 134 of 136 rows,
    // the tip on row 135 at column 67.5).
    constexpr float kPlayheadRows = 134.0f;
    void PlayheadIcon(ImDrawList* draw, float x, float tipY, float height);
    bool SeamMark(ImDrawList* draw, float x, float top, float bottom, float spacing);

    // A menu or tool icon centred on (x, y): gold, its white twin on the
    // selected row (`onBrush`), dimmed on a greyed item. Nothing without its art.
    void MenuIcon(ImDrawList* draw, ui_art::Art art, float x, float y, float size, bool onBrush, bool enabled = true);
    bool AnyArt(std::initializer_list<ui_art::Art> arts);
    void DrawPaintedMark(ImDrawList* draw, std::uint32_t n, float x, float my, float y, bool selected, bool hot, float numberX,
        float numberY);
    float KeyHintWidth(const char* key, const char* label);

    // ------------------------------------------------------------------
    // Screens
    // ------------------------------------------------------------------
    enum class Screen
    {
        Closed,
        Home,
        Gallery,
        Alert,
        Edit,
        Settings,
        Rename, // a clip's new name (the gallery's R)
        // Projects: clips put together into one montage.
        Projects,    // the project files: open, new, rename, delete
        Project,     // the open project's timeline: its clips in order
        ProjectName, // a new project's name, or a project's new name
        Export, // render export
        Count
    };
    extern Screen g_screen;
    bool ProjectFlag(std::uint32_t flag);
    bool ProjectIsOpen();
    Screen PlaybackScreen();
    bool UiHidden();

    // Alert: a message with Continue (optional action) and Cancel.
    struct Alert
    {
        std::string title;
        std::string body;
        std::string confirmLabel;
        CommandType action = CommandType::None;
        std::string actionName;
        Screen back = Screen::Home;
        Screen after = Screen::Count; // where Continue goes (Count: back)
    };
    extern Alert g_alert;
    void ShowAlert(const char* title, const std::string& body, const char* confirm, CommandType action,
        const std::string& name, Screen back);
    void FormatClock(std::uint64_t timeMs, char* out, std::size_t size, bool tenths);
    std::string ClipLabel(const char* name);
    void ShowDeleteClipAlert(const char* name, bool openNow);
    void ShowDeleteProjectAlert(const char* file, const char* shownName, bool openNow);
    const char* NameEmptyError();
    const char* NameCharsError();
    void FormatSize(std::uint64_t bytes, char* out, std::size_t size);
    extern replay::editor::uiqa::MenuPointer g_menuPointer;
    bool HoverShown(bool hovered);
    bool ListItem(ImDrawList* draw, const char* id, float x, float y, float width, float height, bool selected, bool& clicked,
        bool* pointed);
    void NavigateList(int& index, int count);
    extern bool g_helpOpen;
    extern bool g_helpSwallow;
    bool Confirmed();
    bool Back();

    void RefreshClipList(bool reread = true); // the gallery's window and sort kept (below)
    extern int g_homeIndex;
    constexpr int kHomeEditRow = 1; // New Project, Edit Clip or Edit Project
    extern bool g_galleryPicking;
    extern std::uint32_t g_pickInsertAt;
    extern ULONGLONG g_projectOpenAsked;
    void DrawHome(ImDrawList* draw);
    void DrawClipFade(ImDrawList* draw);
    extern ULONGLONG g_toastUntil;
    extern std::uint32_t g_toastClipMs;
    extern std::string g_loadName;
    extern int g_loadWaitFrames;
    extern bool g_openingAsked;
    extern bool g_openingSavesRecording;
    void RequestLoad(const std::string& name);
    void SendRequestedLoad();

    // ---------------- The recording indicator (src/rec_indicator.h) ----------------
    // Replay Settings' "Recording indicator": the mode the core keeps
    // (Snapshot::recordingIndicator) and the mode drawn this frame (hidden
    // shows as brief when its window cannot show over the game). In hidden
    // the recording notices go to the window of rec_indicator_window.h, which
    // screen capture leaves out, instead of the game frame.
    namespace recind = replay::recindicator;
    extern recind::BriefTimer g_recBrief;
    extern recind::ReplayOnTimer g_replayOn;
    extern recind::Choice g_recChoice;
    extern rec_window::Frame g_recRows;
    recind::Kind IndicatorKind();
    bool RecordingNoticeVisible();
    bool SeekCueWanted();
    bool PlaybackBusy();
    const char* LoadingBadgeCaption();
    void DrawLoadingBadge(ImDrawList* draw);
    void DrawRecordingNotices(ImDrawList* draw);
    extern bool g_worldNoticeArmed;
    extern ULONGLONG g_worldNoticeUntil;
    extern char g_worldNoticeText[512];
    void UpdateWorldNotice();
    bool WorldNoticeVisible();
    void DrawWorldNotice(ImDrawList* draw);
    extern char g_qaNoticeText[512];
    extern ULONGLONG g_qaNoticeUntil;
    bool QaNoticeVisible();
    void UpdateQaNotices(bool playing);
    void DrawQaNotice(ImDrawList* draw);

    // ---------------- Crash notice (src/crash_notice.h) ----------------
    // On the first frame the core answers, the crash dumps of
    // %LOCALAPPDATA%\RedemptionEditor\crashes are listed; a crash of an earlier
    // session not told yet shows this notice (the editor closed: top centre, over
    // the game) for kShowMs, once: the newest dump's time goes to
    // crash-notice.txt as it shows. The report key keeps working under it.
    struct CrashNoticeState
    {
        bool checked = false;
        ULONGLONG until = 0;
        // NoticeText's inputs: the text is built when drawn, in the language
        // of that frame.
        replay::crashnotice::Decision decision;
        std::string reportsFolder;
        bool includeOpenClip = false;
    };
    extern CrashNoticeState g_crashNotice;
    bool CrashNoticeVisible();
    void CheckCrashNotice();

    // When no folder takes writes (storage_paths.h Layout::failed: the game
    // folder, Documents and %LOCALAPPDATA% all refused), the player is told
    // where saving failed and why, at the game start (the editor closed, top
    // centre) for kStorageNoticeMs, instead of finding no clips later.
    struct StorageNoticeState
    {
        bool checked = false;
        ULONGLONG until = 0;
        std::string gameDetail;     // "C:\\...\\Red Dead Redemption 2\\: denied (create error 5)"
        std::string fallbackDetail; // the Documents and %LOCALAPPDATA% refusals
    };
    extern StorageNoticeState g_storageNotice;
    bool StorageNoticeVisible();
    void CheckStorageNotice();
    void DrawStorageNotice(ImDrawList* draw, float y0);
    void DrawCrashNotice(ImDrawList* draw);
    extern int g_galleryIndex;
    extern int g_gallerySort;
    extern std::string g_compressingClip;
    const ClipInfo* GalleryClip(int n);
    extern int g_projectIndex;

namespace exportui
{

    // ---------------- Export (render export) ----------------
    // The export dialog: format, frame rate, range, motion blur, look, audio;
    // Enter starts, A adds to the queue; while it runs: progress, time left, the
    // stage timings, Cancel. The UI is drawn after the capture point, so it never
    // reaches the video.
        namespace ec = replay::exportchannel;
        extern int g_index;
        extern int g_preset;
        extern int g_rate;
        extern int g_range;
        extern Screen g_back;
        extern int g_samples;
        extern std::wstring g_lutPath;
        extern bool g_configLoaded;

        struct Pending
        {
            ec::Settings settings;
            std::vector<render_export::Chapter> chapters;
            std::string label;
            bool project = false;            // the whole project (StartExport with enabled 1)
            std::vector<std::string> titles; // its clips, the chapters' names
            std::string identity;            // the clip and its edits (a segmented export's manifest)
            bool resume = false;             // continue `offer` (an unfinished segmented export)
            render_export::ResumeOffer offer;
        };
        extern std::vector<Pending> g_queue;
        extern std::string g_lastLabel;
        extern std::string g_note;
        extern std::uint64_t g_noteUntil;
        void Service();
}
    void DrawExport(ImDrawList* draw);
    float LengthColumnX(const char* text);

    // The details panel of the gallery and of the projects (its content
    // kPanelPad inside; the values' column).
    constexpr float kDetailLeft = 700.0f;
    constexpr float kDetailRight = 1200.0f;
    constexpr float kDetailValueX = 850.0f;
    void DrawGallery(ImDrawList* draw);
    extern std::string g_renameFrom;
    extern char g_renameText[48];
    extern bool g_renameFocus;
    extern const char* g_renameError;
    void DialogPanel(ImDrawList* draw, float bottom);

    // The name boxes (Rename, New/Rename project): the file line, the box, its
    // help and the refusal laid out under each other. A long file name wraps (at
    // any character) and pushes the rest down.
    struct NameBoxPlaces
    {
        float inputY;
        float helpY;
        float errorY;
    };
    NameBoxPlaces PlaceNameBox(const char* fileLine, const char* help);
    void DrawRename(ImDrawList* draw);
    void DrawQaNote(ImDrawList* draw);
    extern int g_projectsIndex;
    extern std::uint32_t g_projectMessageSeen;
    extern ULONGLONG g_projectMessageUntil;
    extern std::string g_projectMessageShown;
    extern int g_editAfterOpen;
    extern ULONGLONG g_editAfterOpenAt;
    extern int g_dragBlock;
    extern bool g_projectScrubbing;
    extern bool g_projectNameNew;
    extern std::string g_projectNameFile;
    extern char g_projectNameText[64];
    extern std::string g_projectsSelectFile;
    extern bool g_projectNameFocus;
    extern const char* g_projectNameError;
    void ShowProjectName(bool isNew, const char* file, const char* name, Screen back);
    void DrawProjectName(ImDrawList* draw);
    void DrawProjects(ImDrawList* draw);
    void DrawProject(ImDrawList* draw);
    void DrawPlaybackHud(ImDrawList* draw);
    extern std::uint32_t g_selectedKeyframeId;
    extern std::uint32_t g_draggingKeyframeId;
    extern std::uint64_t g_lastDragTimeMs;
    extern std::uint64_t g_dragKeyStartMs;
    extern float g_dragStartX;
    extern bool g_dragMoved;
    extern bool g_draggingPlayhead;
    const CameraKeyframe* FindKeyframe(std::uint32_t id);
    const char* TargetLabel(std::uint32_t id);
    extern bool g_selectAdded;
    extern const char* g_editMessage;
    extern ULONGLONG g_editMessageUntil;
    extern ULONGLONG g_saveAskedAt;
    extern bool g_saveSeen;

    void EditMessage(const char* text, ULONGLONG forMs = 2500);
    void AddKeyframeHere();
    void SelectAddedKeyframe();
    extern bool g_valueRowHovered;

    // The side panel's content (the panel stands kPanelPad around it) and its
    // rows.
    constexpr float kEditX = 40.0f;
    constexpr float kEditW = 300.0f;
    constexpr float kTypeErrorH = 20.0f; // the line a typed number's refusal takes under its row

    // "Label   < value >" row of the side panel; the arrows or a click
    // change the value. Returns -1, 0 or +1.
    int ValueRow(ImDrawList* draw, const char* id, float y, const char* label, const char* value, bool enabled, bool repeat = true,
        float lead = 0.0f, ui_art::Art icon = ui_art::Art::Count);

    // ---------------- Marker menus ----------------
    // A marker menu with Camera Options and Depth of Field submenus; up/down
    // picks a row, left/right changes it (PageUp/PageDown fast), Enter opens or
    // runs, Esc goes back; the focused row's help at the foot.
    enum class MarkerMenu { Root, Camera, Dof, Effects, Position, Tools, Spline, SplineDefaults, Shake, ShakeAdvanced };
    extern int g_menuReturnFocus;
    extern MarkerMenu g_markerMenu;
    extern int g_menuFocus;
    extern std::uint32_t g_menuMarker;

    struct MenuRow
    {
        std::string label;
        std::string value;
        bool enabled = true;
        int kind = 0; // 0 value, 1 submenu, 2 action, 3 a list line
        std::function<void(int)> step;
        std::function<void()> enter;
        std::string help;
        // Enter types the number (the Spline rows).
        std::function<void(double)> typed;
        std::string typeStart;
        float typeMin = 0.0f;
        float typeMax = 0.0f;
        ui_art::Art icon = ui_art::Art::Count; // the tool's icon before the label (none)
    };
    float EditIconLead();
    extern replay::editor::input::PendingValue<MarkerShake> g_pendingShake;
    extern int g_splineMenuReturn;
    extern int g_splineDefaultsReturn;
    struct RowTyping
    {
        bool on = false;
        std::string label;
        char text[24]{};
        bool focus = false;
        const char* error = "";
    };
    extern RowTyping g_rowTyping;
    extern bool g_fxTypingFrame;
    void ForgetPendingMarkerValues();
    void StepPlaybackSpeed(int step);

    // Rewind / FastForward in the editor, a seek from the time last asked (the
    // snapshot lags a frame or two behind each seek).
    constexpr std::int64_t kRewindStepMs = 500;
    void SeekBy(std::int64_t deltaMs);
    void DeleteMarker(std::uint32_t id);
    extern bool g_freeCamMenuOpen;
    extern bool g_splineDefaultsOpen;
    std::vector<MenuRow> BuildFreeCamRows();
    extern int g_shakeAdvancedReturn;
    std::vector<MenuRow> BuildSplineDefaultRows();
    std::vector<MenuRow> BuildSplineRows(const CameraKeyframe& k);
    void StartRowTyping(const MenuRow& row);
    void DrawRowTypeBox(ImDrawList* draw, float y, const MenuRow& row);
    std::vector<MenuRow> BuildMarkerRows(const CameraKeyframe& k, int index);

    // One menu row in its box: the label, "<  value  >" (or ">" for a
    // submenu), red ink around the focused row, grey ink around the row under
    // the mouse; a click on the left or right half steps.
    // `pointed` (a menu that does not scroll): set when the mouse moved onto
    // this row, which then takes the focus.
    int MenuRowDraw(ImDrawList* draw, int n, float y, const MenuRow& row, bool focused, bool& clickedEnter, float lead = 0.0f,
        bool* pointed = nullptr);
    float EnvironmentPanelH();
    float DrawEnvironmentRows(ImDrawList* draw, float y, bool playing);
    extern bool g_markerMenuActive;
    extern std::uint32_t g_seenSelection;
    extern ULONGLONG g_selectedAt;
    extern double g_timelineZoom;
    extern bool g_editOverlayArmed;
    void DrawEditCameraOverlay(ImDrawList* draw);
    void DrawEdit(ImDrawList* draw);

    // ---------------- Key bar ----------------
    // The keys of the state on screen, always at its foot, right-aligned at the
    // bottom, wrapped onto more rows when they do not fit. A key that does
    // nothing right now (X with no clip open, F7 with nothing playing) shows
    // muted. The same lists feed the F1 help (its helpKey and help text), so the
    // bar and the help never disagree. The bar is drawn in the editor's window
    // like the rest of its UI, after the render export's capture point: it never
    // reaches an exported video.
    struct KeyTip
    {
        const char* key;     // the bar's key cap
        const char* label;   // the bar's word
        const char* helpKey; // the F1 help's row (nullptr: on the bar only)
        const char* help;
        bool on;
    };
    using KeyTips = std::vector<KeyTip>;
    void ScreenTips(KeyTips& out);
    std::vector<std::size_t> KeyBarRows(const KeyTips& tips);
    float KeyBarHeight(std::size_t rows);
    void DrawKeyBar(ImDrawList* draw, const KeyTips& tips);
    void DrawHelp(ImDrawList* draw);

    // The cursor whenever the editor wants the mouse. Under ReShade 6.8 the
    // add-on's GetIO() is a converted copy (src/editor_cursor.h) and writes to
    // it never reach ReShade: no cursor unless ReShade's own window is open. So
    // the flag is read back each frame from a fresh GetIO() (the measure,
    // logged). With the painted ink arrow loaded (editor/ui/cursor_ink.png)
    // the editor draws it on the foreground list (over every window, after the
    // export's capture) in every ReShade, and writes io.MouseDrawCursor back
    // to false so ImGui's small arrow (ReShade 6.6 and 6.7, where the write
    // reaches) never doubles it; ReShade's own window keeps its cursor.
    // Without the art: ImGui's where the write reaches, the drawn arrow where
    // it is lost.
    namespace ecur = replay::editor::cursor;
    // ---------------- The camera markers in the world ----------------
    // The core projects them each tick (camera_overlay.h) into the overlay
    // channel; drawn here behind the editor's windows, after the export's
    // capture and the QA pictures (never in a video), gone with H.
    namespace oc = replay::overlaychannel;
