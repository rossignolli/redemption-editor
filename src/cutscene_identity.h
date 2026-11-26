#pragma once

namespace replay::cutsceneidentity
{
    struct Snapshot
    {
        bool inCutscene = false;
        char mission[64]{};
        char cutscene[64]{};
    };

    // Reads the scene globals verified for RDR2 1.0.1491.50. False means the
    // game build or one of the guarded reads was not supported.
    bool Read(Snapshot& snapshot);
}
