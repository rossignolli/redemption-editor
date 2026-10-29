#pragma once

// Trains in the replay, recording and playback (the engine side of
// train_consist.h).
//
// Recording: a train car among the recorder's vehicle candidates never takes
// a vehicle place (no skeleton): its whole consist, walked through the
// engine's car links, enters as train car entities with a rigid world track
// at the capture rate (weapon tracks keyed by id), the consist links, cargo
// and lanterns (TRNC) and the head car's speed; it leaves when every car is
// past the exit radius or gone.
//
// Playback: each car is an inert puppet placed on its recorded matrix every
// frame: a vehicle of the model or, refused or moved by the
// engine, an object of the model. Never a mission train (the train system
// would drive it against the playhead). The world's trains are hidden while
// the replay plays (the ambient ones halted) and no random train is made;
// all of it is given back at the end. DELETE_ALL_TRAINS only as a fallback.

#include "replay_context.h"

#include "types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace replay::entity
{
    struct Candidate;
}

namespace replay::trains
{
    void Attach(Context& context);
    // The engine structure (car links, vehicle type, the car's audio entity,
    // the random trains flag, the train config table), checked by signature
    // once; a failed one turns only its own use off. One log line.
    bool Check(std::string& error);
    std::string Describe();
    // ini TrainPuppet: 0 vehicle, then object when refused or moved (default);
    // 1 vehicle only; 2 object only. TrainWorldClear: 0 hide the world's
    // trains (default); 1 also DELETE_ALL_TRAINS at playback start.
    void SetPuppetMode(int mode);
    void SetWorldClearMode(int mode);

    // Recording, called by the recorder core.
    void BeginRecording();
    // The vehicle scan's candidates: train cars are taken out and their
    // consists enter or leave here (whole).
    void TakeCandidates(std::vector<replay::entity::Candidate>& vehicles);
    // One world sample per car at the capture rate, the head cars' speed,
    // cars gone retired. Only while the recording clock runs.
    void SampleRecording();
    void EndRecording();
    bool IsTrainCar(Entity vehicle);

    // The recorded cars (fx and sound owners, the sound census).
    struct RecordedCarRef
    {
        Entity handle = 0;
        replay::entity::ReplayId id;
        std::uint64_t address = 0;
        std::uint64_t audio = 0; // the car's audio entity (0: none or not verified)
    };
    void RecordedCars(std::vector<RecordedCarRef>& out);
    replay::entity::ReplayId RecordedIdAt(std::uint64_t address);
    // The train audio entity of a car (the pointer at car+0x11E0 when the
    // vehicle type is 17..19, signature checked); 0 otherwise.
    std::uint64_t AudioEntityOf(const void* car);

    // Playback, called by the playback core.
    void BeginPlayback();
    void UpdatePuppets(std::uint32_t timeMs, std::uint32_t& creates);
    void DeletePuppet(playback::Puppet& puppet, const char* reason);
    // Every frame of the replay.
    void SuppressThisFrame();
    // A world vehicle found by the world clear: true when it is a train car
    // (hidden here, its train halted when ambient; never deleted one by one).
    bool ClearWorldCar(Entity vehicle);
    void EndPlayback();
    // A script restart with a playback open: the
    // hidden and halted world trains and the train puppets forgotten without
    // a native on them; only the global random trains flag goes back (the
    // count of what was forgotten).
    std::uint32_t ForgetPlaybackAfterRestart();
    // The recorded world transform of a train car at timeMs (its track).
    bool RecordedWorldAt(replay::entity::ReplayId id, std::uint32_t timeMs, float position[3]);

    // The capture slots (train_consist.h kPoseCars)
    // the train cars' pose records may hold (replay_objects keeps them out of
    // the weapons' spare slots).
    std::uint32_t PosePlaces();

    // Bench (replay_bench.cpp): Done 1, Pending 0, Failed -1.
    int BenchSpawn(const std::string& config, float x, float y, float z, bool direction, std::uint64_t startedAt,
        std::uint64_t now, Vehicle& train, std::string& detail);
    bool BenchSpeed(Vehicle train, float speed, std::string& detail);
    void BenchCensus(Vehicle train, const std::string& id, std::string& detail);
    // A door of car `car` (0 the front) opened,
    // shut or set to a ratio, as the holdup scripts do; before and after read.
    bool BenchDoor(Vehicle train, int car, const std::string& door, const std::string& action, std::string& detail);
    void BenchRandomTrains(bool off, std::string& detail);
    void BenchDelete(Vehicle& train);
}
