#include "bench_script.h"

#include <cstdlib>
#include <sstream>

namespace replay::bench
{
    namespace
    {
        struct OpSpec
        {
            const char* name;
            std::size_t minArgs;
            std::size_t maxArgs;
        };

        // The scenario vocabulary; replay_bench.cpp executes each op.
        constexpr OpSpec kOps[] = {
            {"reset", 5, 5},       // x y z heading radius
            {"teleport", 3, 4},    // x y z [heading]
            {"time", 2, 2},        // hour minute
            {"weather", 1, 2},     // weather type name [transition seconds: SET_WEATHER_TYPE over time, as the missions]
            {"wait", 1, 1},        // ms
            {"walk", 3, 4},        // x y z [speed]
            {"record", 1, 1},      // start | stop
            {"play", 0, 0},
            {"stop_play", 0, 0},
            {"pause", 0, 0},
            {"seek", 1, 1},
            {"clean_view", 1, 1},  // 1 or 0: frames without the mod's and the game's HUD (camera preview)
            {"resume", 0, 0},
            {"scrub", 2, 3},       // back|forward ms [fine]
            {"mark", 1, 2},        // tag [hold ms, default 800: the host screenshot]
            {"dims", 1, 1},        // model: logs its bounding box, never fails the run
            {"spawn_ped", 5, 6},   // id model x y z [heading]
            {"spawn_object", 5, 7}, // id model x y z [heading [hidden]] (hidden = invisible)
            {"stage_object", 5, 7}, // id model x y z [heading [hidden]]: survives the scenario
            {"stage_clear", 0, 0},  // delete the stage
            {"purge_objects", 2, 2}, // model radius: delete leftovers of that model around the player
            {"require_stage", 0, 0}, // fail unless a stage is up
            {"give_weapon", 2, 3}, // id|player weapon [attach point 0..28, default 0: the primary hand]
            {"shoot_at", 5, 6},    // id|player x y z ms [firing pattern, default burst]
            {"shoot_ped", 3, 4},   // id|player target ms [firing pattern]: shoots at the entity
            {"fight", 2, 2},       // id target: TASK_COMBAT_PED, two hostile groups (unarmed = fist fight)
            {"disarm", 1, 1},      // id|player: REMOVE_ALL_PED_WEAPONS (a fight becomes a fist fight)
            {"puppet_collision", 1, 1}, // off | on (collision) | nofreeze (collision, not frozen): horse puppets during a replay
            {"still_pose", 2, 2},  // rule on|off, drawn check on|off (StillPoseEngine, ObjectDrawnCheck) for the next playbacks
            {"music", 1, 1},       // off = the open world score off (the replay's MusicStop natives), on = back
            {"freecam", 1, 1},     // on | off, the replay's free camera from the rendered view
            {"focus", 1, 1},       // clear (CLEAR_FOCUS) | log (BENCH_FOCUS line)
            {"player_far", 1, 1},  // metres: the hidden live player that far east of the rendered camera
            {"puppet_velocity", 1, 1}, // on|off: puppets get their own displacement as physical velocity (research: vegetation)
            {"unfreeze", 1, 1},    // id: a spawned object becomes dynamic (physics on)
            // A mission chest's lining and loot, attached
            // to it, and the skeleton the engine builds when it opens.
            {"attach_object", 5, 8}, // child parent|player x y z [rx ry rz]: ATTACH_ENTITY_TO_ENTITY of a spawned object to another (bone 0; or to the player)
            {"detach_object", 1, 1}, // id: DETACH_ENTITY of a spawned object
            {"object_skeleton", 1, 1}, // id: the object skeleton native (0x491439AEF410A2FC, CreateSkeleton) on a spawned object
            // The cooking script's call on a spawned object.
            {"object_burn", 2, 2}, // id level 0..1: _SET_OBJECT_BURN_LEVEL(object, level, affectAsh true), logs the block's level read back
            // A document's text on a spawned object, as the scripts put it.
            {"object_texture", 2, 4}, // id txd|none [p2 p3]: REQUEST_STREAMED_TXD (waits for the load, 5 s) and SET_CUSTOM_TEXTURES_ON_OBJECT; logs +0x420.. read back
            {"object_tint", 2, 2},    // id tint 0..63: SET_OBJECT_TINT_INDEX, logs the tint read back (+0xB8 bits 3..8)
            // A spawned object's bones animated
            // after it was recorded (the deputy's chair tilting).
            {"object_anim", 3, 4}, // id dict anim [loop 0|1, default 1]: REQUEST_ANIM_DICT and PLAY_ENTITY_ANIM on a spawned object
            {"disk_save", 1, 1},   // name: the last recording to ReplayClips\\name.rpclip
            {"disk_load", 1, 1},   // name: ReplayClips\\name.rpclip becomes the clip (playback stopped)
            {"where", 1, 1},       // id|player: logs position, health and life state (never fails the run)
            {"gore", 2, 2},        // id|player mask_hex: gameplay-equivalent gore event (bit 0 head, 4 l.arm, 7 r.arm, 10 l.leg, 13 r.leg)
            {"kill", 1, 1},
            {"timescale", 1, 1},   // scale: SET_TIME_SCALE (0.05..1), the world slows like Dead Eye
            {"kill_player", 0, 0}, // the player dies where he stands (safe exit by death)
            {"health", 2, 2},      // id hp: max health and health of a spawned ped (it survives more shots)
            {"carry", 2, 2},       // id|player target: pick up a carriable (body, dead animal), wait until carried
            {"skeleton", 1, 1},    // id|player: logs the pose skeleton (bone count) and the gore limbs
            {"fx_watch", 1, 1},    // effect hash (hex or decimal): snapshot its next gameplay and replay instance
            {"fx_registered", 1, 1}, // on|off: the registered-effect observer hook (research)
            {"explosion", 4, 5},   // type x y z [damage scale]
            {"fire", 3, 3},        // x y z
            {"fire_forget", 0, 0}, // research: the fire observer forgets the live fires it tracks (as if lit before it loaded)
            {"fire_untracked", 1, 1}, // on|off: FireUntrackedInitial for this session
            {"fire_entity", 5, 5}, // model x y z intensity: START_ENTITY_FIRE on the nearest object of that model (research)
            {"vehicle_part", 2, 3}, // veh wheel|door|log [index]: _BREAK_OFF_VEHICLE_WHEEL (0..5) or SET_VEHICLE_DOOR_BROKEN (0..9); logs the lost parts
            {"vehicle_look", 2, 3}, // veh tint [livery]: _SET_VEHICLE_TINT / _SET_VEHICLE_LIVERY (-1 keeps), logs the readback
            {"clean_world", 4, 4}, // x y z radius: fires, particles, decals and area cleared; logs recorded decals still alive
            {"fx_replay", 1, 1},   // on|off: playback of the fx, registered fx and decal tracks (control runs)
            {"fx_owner", 1, 1},    // on|off: the one-shot owner rule, A/B in one game
            {"fx_split", 1, 1},    // on|off: one registered fx track per creation
            {"fx_world", 1, 1},    // on|off: the world side of the owner rule
            {"research_windows", 1, 1}, // on|off: audio-path and fx-trigger evidence windows around shots, hits and speech (research, default off)
            {"restage", 0, 0},     // rebuild the stage objects in place (what a recording left on them goes)
            {"mem_snap", 2, 2},    // spawned_id tag: guarded memory graph of a spawned entity to a snapshot folder (research, read-only)
            {"clean_objects", 5, 5}, // x y z radius decals|scorch: per object in range, REMOVE_DECALS_FROM_OBJECT or SET_ENTITY_RENDER_SCORCHED false
            {"clear", 0, 0},       // delete everything the bench spawned
            {"play_marks", 0, 0},  // during playback: repeat every recorded mark at its timeline time
            {"env_lock", 2, 2},    // hour weather: frozen clock and weather, player invincible
            {"env_edit", 2, 2},    // clock|weather live|recorded: the playing clip's environment setting
            {"env_unlock", 0, 0},
            // The script timecycle modifier and the weather variations (timecycle_track.h).
            {"timecycle_modifier", 1, 2}, // NAME [strength 0..2] | off: SET_TIMECYCLE_MODIFIER (+ _STRENGTH) or CLEAR_TIMECYCLE_MODIFIER
            {"timecycle_transition", 2, 2}, // NAME seconds: SET_TRANSITION_TIMECYCLE_MODIFIER
            {"weather_variation", 2, 2}, // TYPE VARIATION|off: _SET_WEATHER_VARIATION or _CLEAR_WEATHER_VARIATION (immediate)
            {"timecycle_log", 1, 1},    // tag: the live modifier and variations (BENCH_TIMECYCLE, never fails the run)
            // A script sound by name on an entity, as the camp's cooking plays the
            // sizzle (PLAY_SOUND_FROM_ENTITY "cook_meat_sizzle_loop" on the meat, a weapon object).
            {"script_sound", 3, 3},     // id|player|weapon:id|weapon:player SOUND SET: PLAY_SOUND_FROM_ENTITY
            {"script_sound_var", 4, 4}, // VARIABLE value SOUND SET: _SET_VARIABLE_ON_SOUND_WITH_NAME
            {"script_sound_stop", 2, 2}, // SOUND SET: _STOP_SOUND_WITH_NAME
            // Named render targets and UI apps by live natives (render_target.h)
            // (a recording files them as the camp scripts' calls).
            {"render_target", 2, 3},    // register NAME [0|1] | link MODEL | release NAME | app_open APP [ENTRY] | app_close APP | log NAME [MODEL]
            {"moon_log", 1, 1},         // tag: the sky object's moon angles, clock and date (BENCH_MOON, never fails the run)
            {"moon_angle", 1, 1},       // RADIANS|up: the moon's turn angle set as a trainer leaves it; up = the highest moon at this clock
            {"spawn_vehicle", 5, 6}, // id model x y z [heading]
            {"mount", 2, 3},       // id|player horse [seat, default -1]: walk up and mount (0 = behind the saddle)
            {"set_on_mount", 2, 3}, // id|player horse [seat, default -1]: placed in the saddle (0 = the passenger seat)
            {"enter_vehicle", 3, 3}, // id|player vehicle seat (-1 driver)
            {"into_vehicle", 3, 3}, // id|player vehicle seat: placed in the seat
            {"drive_to", 6, 7},    // id|player vehicle x y z speed [coord|dest|straight|any|wander, default coord]
            {"goto", 5, 5},        // id|player x y z speed (on foot or mounted)
            {"knock_hat", 1, 1},   // id|player
            {"hold", 2, 2},        // INPUT_ACTION ms: player control held in the background
            {"host_input", 2, 2},  // spec wait_ms: the host tool sends real mouse/keys (see bench.py)
            {"clip", 1, 2},        // start [tag] | stop: the host tool records the game with OBS (see bench.py)
            {"aim_at", 2, 2},      // id|player target: turn to face it; for the player the gameplay camera follows (1 s)
            {"hand_over", 1, 1},   // max ms: a person records and plays by hand; ends after one full cycle
            {"rope_probe", 2, 2},
            {"safe_exit", 1, 1},   // exception|hold_exit|release_exit: the closed theater's safe exit faults
            {"rope_probe2", 3, 4},  // type length segments [force_length]
            {"rope_census", 0, 0}, // log every rope in the engine manager (leftover check)
            {"knockout", 2, 2},       // id ms: knocked out for ms
            {"hogtie", 2, 2},         // id|player target: waits until hogtied (12 s)
            {"place_on_mount", 3, 3}, // id|player carried horse: put the carried entity on the horse (12 s)
            {"place_at", 5, 5},       // id|player carried x y z: put it down (10 s)
            {"carry_state", 1, 1},    // id|player: hogtie, carry and attach state line
            {"trail_reset", 0, 0},    // clear the snow and mud track map
            {"buffer", 1, 1},
            {"marker_add", 1, 34},    // ms [key=value...]: a camera marker (with the Custom shake_* and Spline keys)
            {"spline_default", 1, 8}, // key=value...: the Spline defaults for this session (pacing alpha ease_in ease_out weight sp_pos sp_rot sp_fov)
            {"marker_lens", 2, 8},   // ms key=value...: near_clip fstop lens_mm focus_d dof attach_bone look_bone of the marker at ms
            {"marker_clear", 0, 0},
            {"fx_add", 1, 18},        // ms [key=value...]: a shake event (kind value mode; the old shake= shake_i= ... form still reads)
            {"fx_clear", 0, 0},
            {"dead_eye", 0, 0},       // the player's Dead Eye on
            {"cam_log", 0, 0},        // the rendered camera and the markers' evaluation now
            {"keep_play", 0, 0},      // the clip keeps playing when the run ends
            {"dof_probe", 1, 12},     // off | params s near far focal min max f1 f2 f3 f4 | hidof a b c d | focus d
            {"door_census", 1, 9},    // radius [doorhash...]: objects near the player, doors by hash
            {"door_watch", 2, 2},     // doorhash ms: log the door's rotation in the background
            {"through_door", 4, 4},   // id|player doorhash speed dist: in front of the door, walk through
            {"door_cam", 5, 5},       // doorhash side dist height fov: script camera on the door
            // The map state.
            {"ipl", 2, 2},            // hash|NAME on|off|toggle|log: REQUEST_IPL_HASH / REMOVE_IPL_HASH / state and sphere
            {"ipl_watch", 2, 2},      // hash|NAME ms: log the IPL's state every second in the background
            {"world_probe", 0, 0},    // candidate scan cost, active list and fingerprint, camp global, interior sets
            {"lasso", 2, 2},       // id|player target: waits until the target is lassoed (6 s)
            {"drop_weapon", 2, 2}, // id|player weapon
            {"cam", 1, 7},         // x y z look_x look_y look_z fov | off
            {"survey", 2, 2},      // radius step: ground and obstacle grid around the player
            {"topcam", 1, 2},      // height [fov] | off
            {"say", 4, 5},         // id|player context voice|- variation(-1..) [speech params name]: PLAY_PED_AMBIENT_SPEECH_NATIVE
            {"scripted_speech", 5, 5}, // id|player context voice|- variation delay_ms: create, then play after the delay
            {"vocal", 2, 3},       // id vocalization [0|1]: PLAY_ANIMAL_VOCALIZATION
            {"interact", 2, 2},    // id greet|antagonize|defuse: the player's focus prompt on that ped
            {"stop_speech", 1, 1}, // id|player: STOP_CURRENT_PLAYING_SPEECH
            {"horse_weapon", 3, 3}, // horse owner|player weapon: _ATTACH_WEAPON_TO_HORSE_HOLSTER
            // The player's own horse as an id (player_horse: his saddle
            // horse, else his active horse) and the weapon facts the PlayerWeaponBank restore must keep.
            {"ped_place", 4, 5},   // id|player_horse x y z [heading]: SET_ENTITY_COORDS on a ped or horse (not the player: teleport)
            {"weapon_state", 3, 3}, // id|player WEAPON_NAME tag: BENCH_WEAPON_STATE (held, owned, on person, ammo, attach points, horse holster)
            {"scenario", 2, 3},    // id|player SCENARIO_NAME [ms, default -1]: TASK_START_SCENARIO_IN_PLACE_HASH (props in hand)
            {"item_interaction", 2, 3}, // id|player INTERACTION_NAME [item|-]: START_TASK_ITEM_INTERACTION (eat, drink)
            {"attached_census", 2, 2}, // id|player radius: logs every pool object within radius (model, attached to, visible) and carriables
            {"item_prop", 1, 1},    // id|player: logs the item interaction task state and its prop (diagnostic)
            {"dismount", 1, 1},     // id|player: TASK_DISMOUNT_ANIMAL
            {"leave_vehicle", 1, 1}, // id|player: TASK_LEAVE_VEHICLE out of the vehicle it is in
            {"explosion_at", 2, 6}, // vehicle|object type [damage scale [x y z]]: ADD_EXPLOSION at that offset of the spawned entity now
            {"object_census", 1, 4}, // off | x y z radius: every mark also logs the recorded objects or their puppets near there
            {"object_state", 1, 2}, // id [visible|hidden|gone]: BENCH_OBJECT_STATE of a spawned object (exists, visible, collision, mission, script); fails when not as expected
            {"ped_visible", 2, 2},  // id|player on|off: SET_ENTITY_VISIBLE
            {"weapon_component", 3, 12}, // id|player WEAPON_NAME COMPONENT_NAME...: _GIVE_WEAPON_COMPONENT_TO_ENTITY (a weapon's look, live)
            {"strap_test", 1, 2},   // hide|show|auto|regive hash | clear | flags clearhex:weightA:weightB: puppet weapon strap state and components (which drawable is which)
            // Loot: ransack containers and loose items, as the player takes them.
            {"loot_points", 1, 1},  // radius: the scenario points around the player, nearest first (BENCH_LOOT_POINT: type, container entity)
            {"loot_use", 1, 3},     // radius [n|0xMODEL] [ms]: TASK_USE_SCENARIO_POINT on the n-th nearest point with a container; its state logged for ms (BENCH_LOOT_USE_TICK)
            {"loot_end", 0, 0},     // the player out of the scenario (CLEAR_PED_TASKS)
            {"loot_entity", 1, 3},  // radius [MODEL|0xhash|any] [ms]: TASK_LOOT_ENTITY on the nearest free visible object, until it is hidden or gone
            {"loot_watch", 1, 2},   // radius [ms] | off: every object around the player, one BENCH_LOOT_OBJ line per change
            // A worn shop item's wearable state (the bandana up or down), as
            // the game's clothing scripts set it (shop_barber.c: _UPDATE_SHOP_ITEM_WEARABLE_STATE after
            // _APPLY_SHOP_ITEM_TO_PED). Every item worn is logged (BENCH_WEARABLE_ITEM) with its states.
            {"wearable_state", 3, 3}, // id|player CATEGORY|any list|next|@index|STATE_NAME (never fails but on an unknown ped)
            {"ped_freeze", 2, 2},   // id on|off: FREEZE_ENTITY_POSITION of a spawned ped (a mission's hidden still copies)
            // Loot and pickups present before the take.
            {"pickup_spawn", 5, 5}, // id PICKUP_NAME x y z: CREATE_PICKUP (a weapon on the ground), its object logged
            {"pickup_remove", 1, 1}, // id: REMOVE_PICKUP (as when it is collected)
            // A prop set (the player camp's tent and
            // bed, PG_PLAYER_CAMPFIRE_ADD02X_TENTBED) as player_camp creates it.
            {"propset_spawn", 5, 6}, // id PG_NAME x y z [heading]: _REQUEST_PROP_SET, then _CREATE_PROP_SET(.., 1, heading, 1.5, false, false)
            {"propset_census", 1, 1}, // id: logs the prop set's objects (model, owner bits, mission, position): BENCH_PROPSET_OBJECT
            {"propset_delete", 1, 1}, // id: _DELETE_PROP_SET(id, true, true)
            {"world_visible", 3, 3}, // MODEL|nearest radius pick|off|on: pick takes the nearest free world object around
                                     // the player that no script owns and puts the player 1.2 m from it; off hides it
                                     // (what the engine does to a shelf's loot at the take); on shows it back
            {"pool_listing", 1, 1}, // on|off: off makes the replay's object/pickup pool listings and the recorder's ped/vehicle listings return 0 (large-fight and town paths)
            // Trains.
            {"train_spawn", 6, 6},  // id config|auto x y z direction(0|1): a mission train on the nearest track, the player 20 m off it
            {"train_speed", 2, 2},  // id m/s: SET_TRAIN_CRUISE_SPEED and SET_TRAIN_SPEED
            {"train_census", 1, 1}, // id: every car (handle, model, type, position, recorded id): BENCH_TRAIN_CENSUS
            {"train_door", 4, 4},   // id car door(0..9|all) open|shut|0..1: as the holdup scripts (SET_VEHICLE_DOOR_OPEN instant, SET_VEHICLE_DOOR_SHUT, SET_VEHICLE_DOOR_CONTROL), BENCH_TRAIN_DOOR
            {"no_random_trains", 1, 1}, // on|off: SET_RANDOM_TRAINS off|on (logs the engine flag before and after)
            {"throw_at", 4, 4},    // id|player x y z: TASK_THROW_PROJECTILE with the equipped thrown weapon
            // Non-firearm weapon cases.
            {"throw_ped", 2, 2},   // id|player target: TASK_THROW_PROJECTILE at the target's chest, read when the step runs
            {"face", 4, 4},        // id|player x y z: turn to face a point; for the player the gameplay camera follows (1.2 s)
            {"ammo", 2, 3},        // id|player AMMO_NAME [WEAPON_NAME, default the held weapon]: give and select that ammo type
            {"weapon_condition", 5, 5}, // id|player dirt soot damage degradation (0..1 or - to keep): the held weapon object
            {"equip", 2, 2},       // id|player WEAPON_NAME: hold a weapon the ped already owns (its components stay), clip no longer infinite
            {"clip_ammo", 2, 2},   // id|player rounds 0..200: clip no longer infinite, that many rounds in the held weapon's clip
            {"reload", 1, 1},      // id|player: reload the held weapon (MAKE_PED_RELOAD, TASK_RELOAD_WEAPON if refused)
            {"inventory", 2, 2},   // ITEM_NAME qty (1..99): the player's satchel (research: guid chain, logged)
            {"lasso_target", 2, 3}, // id target [ms 1000..30000, default 8000]: lasso equipped, hostile, TASK_LASSO_PED then TASK_COMBAT_PED
            {"melee", 3, 3},       // id|player target ACTION|0: TASK_MELEE (AR_TAKEDOWN_FRONT, AR_EXECUTION_FRONT or 0)
            {"animal_interaction", 4, 4}, // id|player animal INTERACTION model|-: TASK_ANIMAL_INTERACTION (brush, feed)
            // Bleeding.
            {"blood_pools", 1, 4},  // id|player [p1 size p3]: _ADD_BLOOD_POOLS_FOR_PED, _WITH_PARAMS when the three are given
            // Burned and charred bodies.
            {"fire_ped", 1, 3},     // id|player [intensity 0..100, default 1] [flags 0..255, default 0; 8 = scorched carcass]: START_ENTITY_FIRE
            {"stop_fire", 1, 1},    // id|player: STOP_ENTITY_FIRE
            {"scorch", 2, 2},       // id|player 0|1: SET_ENTITY_RENDER_SCORCHED alone (no fire), read back
            // Render export: a video export of the playing clip, as the editor's
            // Export dialog asks it; pending until the add-on finishes (needs [Export] Enabled=1).
            {"export", 1, 16},      // key=value: preset=h264|hevc|av1|preview|prores|dnxhr|ffv1|hevc-lossless|png fps=60[/1001]
                                    // samples=1 shutter=180 start=ms end=ms letterbox=0|2.39 grain=0 audio=0|1 out=folder name=file timeout=ms
        };

        const OpSpec* FindOp(const std::string& name)
        {
            for (const OpSpec& op : kOps)
            {
                if (name == op.name)
                    return &op;
            }
            return nullptr;
        }

        bool ValidArgs(const Step& step)
        {
            float f = 0.0f;
            int i = 0;
            if (step.op == "record")
                return step.args[0] == "start" || step.args[0] == "stop";
            if (step.op == "scrub")
                return (step.args[0] == "back" || step.args[0] == "forward") && ArgInt(step, 1, i) && i > 0 &&
                    (step.args.size() < 3 || step.args[2] == "fine");
            if (step.op == "wait")
                return ArgInt(step, 0, i) && i >= 0;
            if (step.op == "cam")
            {
                if (step.args[0] == "off")
                    return step.args.size() == 1;
                if (step.args.size() != 7)
                    return false;
                for (std::size_t n = 0; n < 7; ++n)
                    if (!ArgFloat(step, n, f))
                        return false;
                return true;
            }
            if (step.op == "hold")
                return step.args[0].rfind("INPUT_", 0) == 0 && ArgInt(step, 1, i) && i > 0 && i <= 20000;
            if (step.op == "purge_objects")
                return ArgFloat(step, 1, f) && f > 0.0f && f <= 300.0f;
            if (step.op == "rope_probe2")
                return ArgInt(step, 0, i) && i >= 0 && i < 64 && ArgFloat(step, 1, f) && f > 0.1f &&
                    f <= 60.0f && ArgInt(step, 2, i) && i > 0 && i <= 512 &&
                    (step.args.size() < 4 || (ArgFloat(step, 3, f) && f > 0.0f && f <= 60.0f));
            if (step.op == "rope_probe")
                return ArgInt(step, 0, i) && i >= 0 && i < 64 && ArgFloat(step, 1, f) && f > 0.1f && f <= 60.0f;
            if (step.op == "shoot_ped")
                return ArgFloat(step, 2, f) && f > 0.0f;
            if (step.op == "fx_registered" || step.op == "fx_replay" || step.op == "fx_owner" || step.op == "fx_split" || step.op == "fx_world" || step.op == "research_windows" || step.op == "puppet_velocity" || step.op == "music" ||
                step.op == "fire_untracked")
                return step.args[0] == "on" || step.args[0] == "off";
            if (step.op == "still_pose")
                return (step.args[0] == "on" || step.args[0] == "off") && (step.args[1] == "on" || step.args[1] == "off");
            if (step.op == "freecam")
                return step.args[0] == "on" || step.args[0] == "off";
            if (step.op == "focus")
                return step.args[0] == "clear" || step.args[0] == "log";
            if (step.op == "player_far")
                return ArgFloat(step, 0, f) && f >= 0.0f && f <= 2000.0f;
            if (step.op == "puppet_collision")
                return step.args[0] == "on" || step.args[0] == "off" || step.args[0] == "nofreeze";
            if (step.op == "clean_objects")
                return ArgFloat(step, 3, f) && f > 0.0f && f <= 300.0f && (step.args[4] == "decals" || step.args[4] == "scorch");
            if (step.op == "clean_world")
                return ArgFloat(step, 0, f) && ArgFloat(step, 1, f) && ArgFloat(step, 2, f) && ArgFloat(step, 3, f) && f > 0.0f && f <= 300.0f;
            if (step.op == "fire_entity")
                return ArgFloat(step, 1, f) && ArgFloat(step, 2, f) && ArgFloat(step, 3, f) && ArgFloat(step, 4, f) && f > 0.0f;
            if (step.op == "vehicle_look")
                return ArgInt(step, 1, i) && i >= -1 && i < 64 && (step.args.size() < 3 || (ArgInt(step, 2, i) && i >= -1 && i < 64));
            if (step.op == "vehicle_part")
            {
                if (step.args[1] == "log")
                    return step.args.size() == 2;
                return step.args.size() == 3 && ArgInt(step, 2, i) && i >= 0 &&
                    ((step.args[1] == "wheel" && i < 6) || (step.args[1] == "door" && i < 10));
            }
            if (step.op == "gore")
            {
                char* end = nullptr;
                const unsigned long mask = std::strtoul(step.args[1].c_str(), &end, 16);
                return end != step.args[1].c_str() && *end == '\0' && mask > 0 && mask <= 0xFFFF;
            }
            if (step.op == "say")
                return ArgInt(step, 3, i) && i >= -1 && i <= 99;
            if (step.op == "scripted_speech")
                return ArgInt(step, 3, i) && i >= -1 && i <= 99 && ArgInt(step, 4, i) && i >= 0 && i <= 20000;
            if (step.op == "vocal")
                return step.args.size() < 3 || step.args[2] == "0" || step.args[2] == "1";
            if (step.op == "interact")
                return step.args[1] == "greet" || step.args[1] == "antagonize" || step.args[1] == "defuse";
            if (step.op == "stop_speech")
                return true;
            if (step.op == "horse_weapon" || step.op == "item_interaction")
                return true;
            // ped_place: an id or player_horse, then x y z [heading]; weapon_state: id|player WEAPON_NAME tag.
            if (step.op == "ped_place")
                return ArgFloat(step, 1, f) && ArgFloat(step, 2, f) && ArgFloat(step, 3, f) &&
                    (step.args.size() < 5 || ArgFloat(step, 4, f));
            if (step.op == "weapon_state")
                return true;
            if (step.op == "scenario")
                return step.args.size() < 3 || (ArgInt(step, 2, i) && i >= -1);
            if (step.op == "attached_census")
                return ArgFloat(step, 1, f) && f > 0.0f && f <= 50.0f;
            if (step.op == "item_prop")
                return true;
            if (step.op == "dismount" || step.op == "leave_vehicle")
                return true;
            if (step.op == "explosion_at")
                return ArgInt(step, 1, i) && i >= 0 && i < 64 && (step.args.size() < 3 || ArgFloat(step, 2, f)) &&
                    (step.args.size() == 2 || step.args.size() == 3 ||
                        (step.args.size() == 6 && ArgFloat(step, 3, f) && ArgFloat(step, 4, f) && ArgFloat(step, 5, f)));
            if (step.op == "object_state")
                return step.args.size() == 1 || step.args[1] == "visible" || step.args[1] == "hidden" || step.args[1] == "gone";
            if (step.op == "object_census")
                return step.args.size() == 1 ? step.args[0] == "off"
                                             : step.args.size() == 4 && ArgFloat(step, 0, f) && ArgFloat(step, 1, f) &&
                        ArgFloat(step, 2, f) && ArgFloat(step, 3, f) && f > 0.0f && f <= 200.0f;
            if (step.op == "loot_points" || step.op == "loot_use" || step.op == "loot_entity")
                return ArgFloat(step, 0, f) && f > 0.0f && f <= 30.0f &&
                    (step.op != "loot_use" || step.args.size() < 2 || step.args[1].rfind("0x", 0) == 0 ||
                        (ArgInt(step, 1, i) && i >= 0 && i < 64)) &&
                    (step.args.size() < 3 || (ArgInt(step, 2, i) && i > 0 && i <= 60000));
            if (step.op == "loot_watch")
                return step.args[0] == "off" ? step.args.size() == 1
                                             : ArgFloat(step, 0, f) && f > 0.0f && f <= 30.0f &&
                        (step.args.size() < 2 || (ArgInt(step, 1, i) && i > 0 && i <= 600000));
            if (step.op == "ped_visible")
                return step.args[1] == "on" || step.args[1] == "off";
            if (step.op == "weapon_component")
                return true;
            if (step.op == "strap_test")
            {
                if (step.args[0] == "clear")
                    return step.args.size() == 1;
                if (step.args[0] == "flags")
                    return step.args.size() == 2;
                if (step.args.size() != 2 || (step.args[0] != "hide" && step.args[0] != "show" && step.args[0] != "auto" &&
                                                 step.args[0] != "regive"))
                    return false;
                char* end = nullptr;
                std::strtoul(step.args[1].c_str(), &end, 16);
                return end != step.args[1].c_str() && *end == '\0';
            }
            if (step.op == "wearable_state")
            {
                const std::string& what = step.args[2];
                // '@', not '#': a '#' starts a comment in a scenario line.
                if (what.size() > 1 && what[0] == '@')
                {
                    char* end = nullptr;
                    const long index = std::strtol(what.c_str() + 1, &end, 10);
                    return *end == '\0' && index >= 0 && index < 32;
                }
                return !what.empty() && what[0] != '@';
            }
            if (step.op == "ped_freeze") // a spawned ped only: the player is never left frozen
                return step.args[0] != "player" && (step.args[1] == "on" || step.args[1] == "off");
            if (step.op == "pickup_spawn")
                return step.args[1].rfind("PICKUP_", 0) == 0 && ArgFloat(step, 2, f) && ArgFloat(step, 3, f) && ArgFloat(step, 4, f);
            if (step.op == "pickup_remove")
                return true;
            if (step.op == "propset_spawn")
                return step.args[1].rfind("PG_", 0) == 0 && ArgFloat(step, 2, f) && ArgFloat(step, 3, f) && ArgFloat(step, 4, f) &&
                    (step.args.size() < 6 || (ArgFloat(step, 5, f) && f >= -360.0f && f <= 360.0f));
            if (step.op == "propset_census" || step.op == "propset_delete")
                return true;
            if (step.op == "world_visible")
                return ArgFloat(step, 1, f) && f > 0.0f && f <= 10.0f && (step.args[2] == "on" || step.args[2] == "off" || step.args[2] == "pick");
            if (step.op == "pool_listing")
                return step.args[0] == "on" || step.args[0] == "off";
            if (step.op == "train_spawn")
                return ArgFloat(step, 2, f) && ArgFloat(step, 3, f) && ArgFloat(step, 4, f) &&
                    (step.args[5] == "0" || step.args[5] == "1");
            if (step.op == "train_speed")
                return ArgFloat(step, 1, f) && f >= -40.0f && f <= 40.0f;
            if (step.op == "train_census")
                return true;
            if (step.op == "train_door")
                return ArgInt(step, 1, i) && i >= 0 && i < 64 &&
                    (step.args[2] == "all" || (ArgInt(step, 2, i) && i >= 0 && i <= 9)) &&
                    (step.args[3] == "open" || step.args[3] == "shut" || (ArgFloat(step, 3, f) && f >= 0.0f && f <= 1.0f));
            if (step.op == "no_random_trains")
                return step.args[0] == "on" || step.args[0] == "off";
            if (step.op == "throw_at" || step.op == "face")
                return ArgFloat(step, 1, f) && ArgFloat(step, 2, f) && ArgFloat(step, 3, f);
            if (step.op == "give_weapon")
                return step.args.size() < 3 || (ArgInt(step, 2, i) && i >= 0 && i <= 28);
            if (step.op == "drive_to")
            {
                for (std::size_t n = 2; n < 6; ++n)
                    if (!ArgFloat(step, n, f))
                        return false;
                if (step.args.size() < 7)
                    return true;
                const std::string& mode = step.args[6];
                return mode == "coord" || mode == "dest" || mode == "straight" || mode == "any" || mode == "wander";
            }
            if (step.op == "throw_ped" || step.op == "ammo" || step.op == "melee" || step.op == "animal_interaction")
                return true;
            if (step.op == "equip" || step.op == "reload")
                return true;
            if (step.op == "clip_ammo")
                return ArgInt(step, 1, i) && i >= 0 && i <= 200;
            if (step.op == "weapon_condition")
            {
                for (std::size_t n = 1; n < 5; ++n)
                    if (step.args[n] != "-" && !(ArgFloat(step, n, f) && f >= 0.0f && f <= 1.0f))
                        return false;
                return true;
            }
            if (step.op == "fire_ped")
                return (step.args.size() < 2 || (ArgFloat(step, 1, f) && f > 0.0f && f <= 100.0f)) &&
                    (step.args.size() < 3 || (ArgInt(step, 2, i) && i >= 0 && i <= 255));
            if (step.op == "stop_fire")
                return true;
            if (step.op == "scorch")
                return step.args[1] == "0" || step.args[1] == "1";
            if (step.op == "blood_pools")
            {
                if (step.args.size() == 1)
                    return true;
                if (step.args.size() != 4)
                    return false;
                for (std::size_t n = 1; n < 4; ++n)
                    if (!ArgFloat(step, n, f))
                        return false;
                return true;
            }
            if (step.op == "inventory")
                return ArgInt(step, 1, i) && i > 0 && i <= 99;
            if (step.op == "lasso_target")
                return step.args.size() < 3 || (ArgInt(step, 2, i) && i >= 1000 && i <= 30000);
            if (step.op == "skeleton" || step.op == "kill" || step.op == "carry")
                return true;
            if (step.op == "timescale")
                return ArgFloat(step, 0, f) && f >= 0.05f && f <= 1.0f;
            if (step.op == "health")
                return ArgInt(step, 1, i) && i > 0 && i <= 100000;
            if (step.op == "hand_over")
                return ArgInt(step, 0, i) && i > 0;
            if (step.op == "clip")
                return (step.args[0] == "start") || (step.args[0] == "stop" && step.args.size() == 1);
            if (step.op == "host_input")
                return ArgInt(step, 1, i) && i >= 0 && i <= 20000;
            if (step.op == "env_lock")
                return ArgInt(step, 0, i) && i >= 0 && i < 24;
            if (step.op == "timecycle_modifier")
                return step.args[0] == "off" ? step.args.size() == 1
                                             : step.args.size() == 1 || (ArgFloat(step, 1, f) && f >= 0.0f && f <= 2.0f);
            if (step.op == "timecycle_transition")
                return ArgFloat(step, 1, f) && f > 0.0f && f <= 60.0f;
            if (step.op == "weather_variation" || step.op == "timecycle_log")
                return true;
            if (step.op == "script_sound_var")
                return ArgFloat(step, 1, f) && f >= -1000.0f && f <= 1000.0f;
            if (step.op == "script_sound")
                return step.args[0] != "weapon:";
            if (step.op == "script_sound_stop")
                return true;
            if (step.op == "moon_log")
                return true;
            if (step.op == "moon_angle")
                return step.args[0] == "up" || (ArgFloat(step, 0, f) && f >= -64.0f && f <= 64.0f);
            if (step.op == "env_edit")
                return (step.args[0] == "clock" || step.args[0] == "weather") &&
                    (step.args[1] == "live" || step.args[1] == "recorded");
            // The 7th argument of an object is the word
            // hidden (created invisible, as a mission keeps its own objects).
            if (step.op == "attach_object")
            {
                for (std::size_t n = 2; n < step.args.size(); ++n)
                    if (!ArgFloat(step, n, f))
                        return false;
                return step.args[0] != step.args[1];
            }
            if (step.op == "detach_object" || step.op == "object_skeleton")
                return true;
            if (step.op == "object_burn")
                return ArgFloat(step, 1, f) && f >= 0.0f && f <= 1.0f;
            if (step.op == "object_tint")
                return ArgInt(step, 1, i) && i >= 0 && i <= 63;
            if (step.op == "object_texture") // names (joaat), 0x hex or a decimal hash
                return !step.args[1].empty() && step.args[1] != "0";
            if (step.op == "object_anim")
                return step.args.size() == 3 || step.args[3] == "0" || step.args[3] == "1";
            if ((step.op == "spawn_object" || step.op == "stage_object") && step.args.size() == 7)
            {
                for (std::size_t n = 2; n < 6; ++n)
                    if (!ArgFloat(step, n, f))
                        return false;
                return step.args[6] == "hidden";
            }
            if (step.op == "enter_vehicle" || step.op == "into_vehicle")
                return ArgInt(step, 2, i);
            // The engine's mount seat, -1 the saddle, 0
            // behind it (SET_PED_ONTO_MOUNT and TASK_MOUNT_ANIMAL in the scripts).
            if ((step.op == "mount" || step.op == "set_on_mount") && step.args.size() == 3)
                return ArgInt(step, 2, i) && i >= -1 && i <= 2;
            if (step.op == "mount" || step.op == "set_on_mount" || step.op == "knock_hat" || step.op == "lasso" ||
                step.op == "aim_at" || step.op == "fight" || step.op == "disarm" || step.op == "disk_save" || step.op == "disk_load" || step.op == "unfreeze" ||
                step.op == "drop_weapon" || step.op == "hogtie" ||
                step.op == "place_on_mount" || step.op == "carry_state" || step.op == "trail_reset" ||
                step.op == "marker_add" || step.op == "marker_lens" || step.op == "marker_clear" || step.op == "cam_log" || step.op == "keep_play" || step.op == "dead_eye" ||
                step.op == "fx_add" || step.op == "fx_clear" || step.op == "spline_default" ||
                (step.op == "dof_probe" && (step.args[0] == "off" || step.args[0] == "params" || step.args[0] == "hidof" || step.args[0] == "focus")) ||
                step.op == "door_census" || (step.op == "buffer" && (step.args[0] == "save" || step.args[0] == "press")) || step.op == "door_watch" || step.op == "through_door" || step.op == "door_cam")
                return true;
            if (step.op == "knockout")
                return ArgInt(step, 1, i) && i > 0 && i <= 60000;
            if (step.op == "ipl")
                return step.args[1] == "on" || step.args[1] == "off" || step.args[1] == "toggle" || step.args[1] == "log";
            if (step.op == "ipl_watch")
                return ArgInt(step, 1, i) && i > 0 && i <= 120000;
            if (step.op == "world_probe")
                return true;
            if (step.op == "export")
            {
                for (const std::string& arg : step.args)
                    if (arg.find('=') == std::string::npos)
                        return false;
                return true;
            }
            if (step.op == "place_at")
                return ArgFloat(step, 2, f) && ArgFloat(step, 3, f) && ArgFloat(step, 4, f);
            if (step.op == "topcam")
                return step.args[0] == "off" ? step.args.size() == 1 : ArgFloat(step, 0, f) &&
                    (step.args.size() < 2 || ArgFloat(step, 1, f));
            if (step.op == "survey")
                return ArgFloat(step, 0, f) && f > 0.0f && f <= 200.0f && ArgFloat(step, 1, f) && f >= 0.5f;
            if (step.op == "time")
                return ArgInt(step, 0, i) && i >= 0 && i < 24 && ArgInt(step, 1, i) && i >= 0 && i < 60;
            // The fault name is a word, not a number.
            if (step.op == "safe_exit")
                return step.args[0] == "exception" || step.args[0] == "hold_exit" || step.args[0] == "release_exit";
            std::size_t firstNumber = 0;
            if (step.op == "spawn_ped" || step.op == "spawn_object" || step.op == "stage_object" ||
                step.op == "spawn_vehicle" || step.op == "drive_to")
                firstNumber = 2;
            else if (step.op == "goto")
                firstNumber = 1;
            else if (step.op == "shoot_at")
            {
                for (std::size_t n = 1; n < 5; ++n)
                    if (!ArgFloat(step, n, f))
                        return false;
                return true;
            }
            else if (step.op == "mark")
                return step.args.size() < 2 || (ArgInt(step, 1, i) && i >= 0);
            else if (step.op == "weather") // the transition seconds, when given, a positive number
                return step.args.size() < 2 || (ArgFloat(step, 1, f) && f > 0.0f && f <= 600.0f);
            else if (step.op == "give_weapon" || step.op == "dims" || step.op == "where" || step.op == "fx_watch" ||
                step.op == "mem_snap")
                return true;
            for (std::size_t n = firstNumber; n < step.args.size(); ++n)
            {
                if (!ArgFloat(step, n, f))
                    return false;
            }
            return true;
        }
    }

    bool ArgFloat(const Step& step, std::size_t index, float& value)
    {
        if (index >= step.args.size())
            return false;
        const char* text = step.args[index].c_str();
        char* end = nullptr;
        value = std::strtof(text, &end);
        return end != text && *end == '\0';
    }

    bool ArgInt(const Step& step, std::size_t index, int& value)
    {
        if (index >= step.args.size())
            return false;
        const char* text = step.args[index].c_str();
        char* end = nullptr;
        const long parsed = std::strtol(text, &end, 10);
        if (end == text || *end != '\0')
            return false;
        value = static_cast<int>(parsed);
        return true;
    }

    bool ParseScript(const std::string& text, Script& out, std::string& error)
    {
        out.steps.clear();
        std::istringstream lines(text);
        std::string line;
        int number = 0;
        while (std::getline(lines, line))
        {
            ++number;
            const std::size_t comment = line.find('#');
            if (comment != std::string::npos)
                line.resize(comment);
            std::istringstream tokens(line);
            Step step;
            step.line = number;
            if (!(tokens >> step.op))
                continue;
            std::string arg;
            while (tokens >> arg)
                step.args.push_back(arg);

            const OpSpec* spec = FindOp(step.op);
            if (!spec)
            {
                error = "line " + std::to_string(number) + ": unknown op '" + step.op + "'";
                return false;
            }
            if (step.args.size() < spec->minArgs || step.args.size() > spec->maxArgs)
            {
                error = "line " + std::to_string(number) + ": wrong argument count for '" + step.op + "'";
                return false;
            }
            if (!ValidArgs(step))
            {
                error = "line " + std::to_string(number) + ": bad arguments for '" + step.op + "'";
                return false;
            }
            out.steps.push_back(std::move(step));
        }
        if (out.steps.empty())
        {
            error = "empty scenario";
            return false;
        }
        return true;
    }
}
