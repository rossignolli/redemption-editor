#pragma once

// Chapter names and the world notice text, shared by the core (logs), the
// editor (gallery and the notice on load) and the tests. No engine access, no
// other project header (the editor includes it).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "i18n.h" // the notice and the chapters in the player's language

namespace replay::world
{
    // Story chapter: 0 unknown, 1 to 6 the chapters, 7 and 8 the epilogues.
    enum Chapter : std::uint8_t
    {
        kChapterUnknown = 0,
        kChapterEpilogue1 = 7,
        kChapterEpilogue2 = 8,
    };

    constexpr std::uint8_t kCampUnknown = 255;
    constexpr std::uint8_t kCampGuarma = 4;

    // The camp (Global_40.f_4283) to the chapter. 0 to 2 from the journal (camp 1
    // -> JOURNAL_CHAPTER2, 2 -> JOURNAL_CHAPTER3, 3 -> JOURNAL_CHAPTER4 in the
    // scripts), the rest by the story order of the nine camp_* scripts
    // (hypothesis, not proven against saves of each chapter).
    inline std::uint8_t ChapterFromCamp(std::uint8_t camp)
    {
        static const std::uint8_t kChapters[9] = {1, 2, 3, 4, 5, 6, 6, kChapterEpilogue1, kChapterEpilogue2};
        return camp < 9 ? kChapters[camp] : kChapterUnknown;
    }

    inline const char* CampName(std::uint8_t camp)
    {
        static const char* kNames[9] = {"Colter", "Horseshoe Overlook", "Clemens Point", "Shady Belle", "Guarma",
            "Lakay", "Beaver Hollow", "Pronghorn Ranch", "Beecher's Hope"};
        return camp < 9 ? kNames[camp] : "";
    }

    // "Chapter 3", "Epilogue 1"; empty when unknown.
    inline void ChapterName(std::uint8_t chapter, char* out, std::size_t size)
    {
        if (!out || !size)
            return;
        if (chapter >= 1 && chapter <= 6)
            std::snprintf(out, size, "Chapter %u", static_cast<unsigned>(chapter));
        else if (chapter == kChapterEpilogue1 || chapter == kChapterEpilogue2)
            std::snprintf(out, size, "Epilogue %u", static_cast<unsigned>(chapter - 6));
        else
            out[0] = '\0';
    }

    // ChapterName in the player's language (the gallery, the notice; the logs
    // keep ChapterName).
    inline void ChapterText(std::uint8_t chapter, char* out, std::size_t size)
    {
        if (!out || !size)
            return;
        if (chapter >= 1 && chapter <= 6)
            replay::i18n::CopyTo(out, size, TRF("world.chapter", "Chapter {n}", {{"n", std::to_string(chapter)}}));
        else if (chapter == kChapterEpilogue1 || chapter == kChapterEpilogue2)
            replay::i18n::CopyTo(out, size, TRF("world.epilogue", "Epilogue {n}", {{"n", std::to_string(chapter - 6)}}));
        else
            out[0] = '\0';
    }

    // What the playback found (0 = nothing to say).
    enum NoticeKind : std::uint32_t
    {
        kNoticeNone = 0,
        kNoticeMayDiffer = 1, // the world was left as it is (restore off, mission, Guarma, far)
        kNoticeRestored = 2,  // the clip's map state was applied for the replay
    };

    // The notice on load, in the player's language. False when
    // there is nothing to say: same or unknown chapter and no difference in
    // the clip's area.
    inline bool NoticeText(std::uint8_t recordedChapter, std::uint8_t currentChapter, std::uint32_t differences,
        bool restored, char* out, std::size_t size)
    {
        if (!out || !size)
            return false;
        out[0] = '\0';
        const bool chapterDiffers = recordedChapter != kChapterUnknown && currentChapter != kChapterUnknown &&
            recordedChapter != currentChapter;
        if (!chapterDiffers && differences == 0)
            return false;
        if (chapterDiffers)
        {
            char recorded[96]{}, current[96]{};
            ChapterText(recordedChapter, recorded, sizeof(recorded));
            ChapterText(currentChapter, current, sizeof(current));
            replay::i18n::CopyTo(out, size, TRF("world.notice.chapter", "Recorded in {recorded}, you are in {current}: {what}",
                {{"recorded", recorded}, {"current", current},
                    {"what", restored ? TR("world.notice.restored", "the world was restored for this replay")
                                      : TR("world.notice.may_differ", "the world may look different")}}));
            return true;
        }
        replay::i18n::CopyTo(out, size, TRF("world.notice.story", "Recorded at a different story point: {what}",
            {{"what", restored ? TR("world.notice.restored", "the world was restored for this replay")
                               : TR("world.notice.buildings", "some buildings may differ")}}));
        return true;
    }
}
