// The export controller of the UI harness. The editor's Export screen reads
// render_export::ExportController (the real header); this body answers from
// ui_harness::exportstub::State, set by the fixtures, and never touches a GPU,
// an encoder or the export channel.

#include "ui_harness.h"

namespace ui_harness::exportstub
{
    State& Get()
    {
        static State state;
        return state;
    }
}

namespace render_export
{
    namespace stub = ui_harness::exportstub;

    ExportController& ExportController::Get()
    {
        // Never destroyed: its members' destructors live in the export's
        // own sources, which the harness does not build.
        static ExportController* controller = new ExportController();
        return *controller;
    }

    void ExportController::LoadConfig()
    {
        m_enabled = stub::Get().enabled;
        m_folder = L"C:\\Users\\Player\\Documents\\Rockstar Games\\Red Dead Redemption 2\\Videos\\Redemption Editor";
    }

    void ExportController::StartProbe() {}

    bool ExportController::ProbeDone() const { return stub::Get().probed; }

    int ExportController::VariantFor(replay::exportchannel::VideoPreset) const { return stub::Get().allPresets ? 0 : -1; }

    bool ExportController::PresetAvailable(replay::exportchannel::VideoPreset preset) const
    {
        return stub::Get().allPresets || preset == replay::exportchannel::VideoPreset::H264Nvenc;
    }

    std::string ExportController::BackendLabel(replay::exportchannel::VideoPreset) const { return stub::Get().backend; }

    std::string ExportController::EncoderProblem() const { return stub::Get().encoderProblem; }

    std::string ExportController::ReShadeProblem() const { return stub::Get().reshadeProblem; }

    bool ExportController::RequestExport(const replay::exportchannel::Settings&, const std::vector<Chapter>&, std::uint32_t& requestSeq)
    {
        requestSeq = 0;
        return false;
    }

    bool ExportController::RequestProjectExport(const replay::exportchannel::Settings&, const std::vector<std::string>&, std::uint32_t& requestSeq)
    {
        requestSeq = 0;
        return false;
    }

    void ExportController::Cancel() {}

    ExportStatus ExportController::Status() const { return stub::Get().status; }

    bool ExportController::Busy() const { return stub::Get().busy; }

    void ExportController::OnOverlay(reshade::api::effect_runtime*) {}

    void ExportController::OnPresent(reshade::api::effect_runtime*) {}

    void ExportController::Shutdown(reshade::api::effect_runtime*) {}

    // The members' cleanup the controller's constructor references (never
    // run: the stub controller is never destroyed and never exports).
    void ProcessLoopbackCapture::Stop() {}
    ParallelCopy::~ParallelCopy() {}
}
