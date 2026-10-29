#pragma once

// Vegetation around replay puppets. Grass parts and bushes bend around an
// entity through its trail's vegetation modifier, which grows only with the
// entity's physical speed; the flattened-grass stamp is sized by the same
// speed. A puppet is frozen, so its physical speed is zero and the engine
// ignores SET_ENTITY_VELOCITY on it. Both consumers read the speed through
// 0xC32058(out, entity) (callers 0xC32108, 0xC489DC only). A detour,
// signature-checked, forwards every call and, for the puppets the script
// thread publishes, writes their recorded speed squared instead. It never
// calls natives and changes nothing for other entities. Build 1491.50.

#include <cstdint>
#include <string>

namespace replay::vegetation
{
    bool Install(std::string& error);
    void Shutdown();
    // Script thread, playback: a puppet's engine address and where it is
    // now; its speed is the displacement since its previous publish (zero
    // across a jump or while paused).
    void Publish(std::uint64_t entity, float x, float y, float z, std::uint64_t nowMs, bool normalPlay);
    // A puppet deleted: its address may come back as another entity.
    void Remove(std::uint64_t entity);
    // Playback end: nothing is overridden any more.
    void Clear();
    std::uint64_t Overrides();

    // The speed squared last published for a puppet (0 when unknown).
    float PublishedSpeedSquared(std::uint64_t entity);

    // The velocity vector of the same publish (m/s, smoothed like the speed, zero
    // across a jump: fx_motion_rule.h StepVelocity), false when the entity is not
    // published.
    bool PublishedVelocity(std::uint64_t entity, float out[3]);
    // FxPuppetVelocity: a detour on the particle entity interface's velocity
    // (vfunc +0xB0 of the vtable 0x3480CE0, 0x1405BA4; rdx the entity, r8 the
    // float4 out, al = not zero). The attach update 0x2C52254 multiplies it by the
    // step into the instance's +0x360 (0x2C523CE), the displacement the particles
    // inherit; a frozen puppet has none. For a published puppet the detour returns
    // the published velocity; any other entity goes to the original. No natives; a
    // table read.
    bool InstallFxVelocity(std::string& error);
    void SetFxVelocity(bool on);
    std::uint64_t FxVelocityOverrides();
}
