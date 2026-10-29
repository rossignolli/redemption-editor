#include "bench_script.h"

#include <cstdio>

namespace
{
    using namespace replay::bench;

    int g_benchFailures = 0;

    void Check(bool condition, const char* message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_benchFailures;
    }

    bool Parses(const char* text)
    {
        Script script;
        std::string error;
        return ParseScript(text, script, error);
    }
}

int RunBenchScriptTests()
{
    {
        Script script;
        std::string error;
        const bool ok = ParseScript(
            "# smoke\n"
            "reset -1751.1 -2335.7 41.5 90 30\n"
            "\n"
            "record start   # rec\n"
            "wait 5000\n"
            "scrub back 1500 fine\n"
            "mark rec_end\n",
            script, error);
        Check(ok, "a valid scenario parses");
        Check(script.steps.size() == 5, "comments and blank lines are not steps");
        Check(script.steps[0].line == 2 && script.steps[0].args.size() == 5, "steps keep line and args");
        Check(script.steps[1].op == "record" && script.steps[1].args[0] == "start", "trailing comment is cut");
        float x = 0.0f;
        int ms = 0;
        Check(ArgFloat(script.steps[0], 0, x) && x < -1751.0f && x > -1752.0f, "float argument");
        Check(ArgInt(script.steps[2], 0, ms) && ms == 5000, "int argument");
        Check(!ArgInt(script.steps[2], 1, ms), "missing argument is not read");
    }
    {
        Script script;
        std::string error;
        Check(!ParseScript("wait 10\nfly 1 2 3\n", script, error) && error.rfind("line 2:", 0) == 0,
            "unknown op fails with its line");
    }
    // The wagon destruction bench.
    Check(Parses("leave_vehicle player\n"), "leave_vehicle parses");
    Check(Parses("explosion_at w1 25\n") && Parses("explosion_at w1 25 1.0\n") && Parses("explosion_at w1 25 1.0 0 -1.8 -0.3\n"),
        "explosion_at parses");
    Check(!Parses("explosion_at w1 25 1.0 0.5\n"), "explosion_at takes the whole offset");
    Check(!Parses("explosion_at w1 boom\n") && !Parses("explosion_at w1 64\n"), "explosion_at needs a type 0..63");
    Check(Parses("object_census -763 -445 44 30\n") && Parses("object_census off\n"), "object_census parses");
    Check(Parses("object_state lamp\n") && Parses("object_state lamp visible\n") && Parses("object_state lamp gone\n") &&
              !Parses("object_state lamp shown\n") && !Parses("object_state\n"),
        "object_state takes an id and visible, hidden or gone");
    Check(!Parses("object_census on\n") && !Parses("object_census 1 2 3 0\n"), "object_census needs off or a center and radius");
    // safe_exit takes a fault name.
    Check(Parses("safe_exit hold_exit\n"), "safe_exit hold_exit parses");
    Check(Parses("safe_exit exception\n"), "safe_exit exception parses");
    Check(!Parses("safe_exit 3\n"), "safe_exit with an unknown fault fails");
    Check(!Parses(""), "empty scenario fails");
    Check(!Parses("wait\n"), "missing argument fails");
    Check(!Parses("wait 10 20\n"), "extra argument fails");
    Check(!Parses("wait abc\n"), "non-number fails");
    // Render export: the export op of a bench render scenario.
    Check(Parses("disk_load render-night\nclean_view 1\nplay\n"
                 "export preset=h264 fps=60 samples=16 shutter=180 start=2000 end=10000 out=H:\\x name=h264-blur16 timeout=1800000\n"
                 "export preset=h264 fps=60 samples=8 shutter=180 letterbox=2.39 grain=1.5 start=2000 end=10000 out=H:\\x name=lb timeout=1800000\n"
                 "export preset=prores fps=30000/1001 samples=1 audio=1\nstop_play\n"),
        "export op parses");
    Check(!Parses("export h264\n"), "export takes key=value arguments");
    // The train ops.
    Check(Parses("train_spawn t1 auto -290 780 118 0\ntrain_speed t1 8\ntrain_census t1\nno_random_trains on\n"),
        "train ops parse");
    Check(Parses("train_spawn t1 0x3260CE89 -290 780 118 1\n"), "train_spawn takes a config hash");
    Check(!Parses("train_spawn t1 auto -290 780 118 2\n"), "train_spawn direction is 0 or 1");
    Check(!Parses("train_spawn t1 auto -290 780\n"), "train_spawn needs the point");
    // The door of a train car.
    Check(Parses("train_door t1 3 all open\ntrain_door t1 0 2 shut\ntrain_door t1 5 0 0.5\n"), "train_door: open, shut, a ratio");
    Check(!Parses("train_door t1 3 12 open\n"), "train_door: doors 0..9");
    Check(!Parses("train_door t1 3 0 1.5\n"), "train_door: a ratio is 0..1");
    Check(!Parses("train_speed t1 90\n"), "train_speed is bounded");
    Check(!Parses("no_random_trains maybe\n"), "no_random_trains is on or off");
    // The map state ops.
    Check(Parses("ipl DES_STR_JAIL toggle\nipl -1437554707 off\nipl_watch 0x7F6C18B1 30000\nworld_probe\n"),
        "ipl, ipl_watch and world_probe parse");
    Check(!Parses("ipl DES_STR_JAIL maybe\n"), "ipl with an unknown action fails");
    Check(!Parses("ipl_watch DES_STR_JAIL 0\n"), "ipl_watch needs a duration");
    Check(!Parses("record begin\n"), "record takes start or stop");
    Check(!Parses("scrub sideways 100\n"), "scrub takes back or forward");
    Check(!Parses("scrub back 100 coarse\n"), "scrub third arg is fine only");
    Check(!Parses("time 25 0\n"), "hour is checked");
    Check(!Parses("teleport 1 2 z\n"), "coordinates are numbers");
    Check(Parses("spawn_ped a1 A_M_M_RANCHER_01 1 2 3 90\nshoot_at a1 1 2 3 2000\nexplosion 25 1 2 3\n"),
        "spawn, shoot and explosion parse");
    Check(Parses("spawn_object stage P_GEN_AVPLATFORM_WOOD 1 2 3\n") && !Parses("spawn_object stage P 1 2\n"),
        "spawn_object takes id, model and coordinates");
    Check(Parses("survey 100 2\ntopcam 120\ntopcam 120 60\ntopcam off\n"), "survey and topcam parse");
    Check(!Parses("survey 500 2\n") && !Parses("survey 100 0.1\n") && !Parses("topcam off 60\n"),
        "survey limits and topcam off take no fov");
    Check(Parses("mark a\nmark b 0\nmark c 1500\ndims p_dock01x\n") && !Parses("mark a b\n"),
        "mark hold and dims parse");
    Check(Parses("stage_object f1 p_foundationwood03x 1 2 3 0\nrequire_stage\nstage_clear\n") &&
        !Parses("stage_clear now\n"), "stage ops parse");
    Check(Parses("env_lock 12 SUNNY\nenv_unlock\nspawn_vehicle w CART01 1 2 3 90\nmount player h\n"
                 "enter_vehicle player w -1\ninto_vehicle a w 0\ndrive_to player w 1 2 3 5\ngoto h 1 2 3 3\n"
                 "knock_hat player\ndrop_weapon a WEAPON_REVOLVER_CATTLEMAN\ncam 1 2 3 4 5 6 50\ncam off\n"),
        "scene ops parse");
    Check(!Parses("cam 1 2 3\n") && !Parses("env_lock 30 SUNNY\n") && !Parses("enter_vehicle p w front\n") &&
        !Parses("goto p 1 2 z 3\n"), "scene op arguments are checked");
    Check(Parses("shoot_at a 1 2 3 2000 FIRING_PATTERN_FULL_AUTO\n") && !Parses("shoot_at a 1 2 3 x\n"),
        "shoot_at pattern is optional");
    Check(Parses("shoot_ped a1 a2 3000\nshoot_ped a1 a2 3000 FIRING_PATTERN_FULL_AUTO\nwhere a1\n") &&
        !Parses("shoot_ped a1 a2 0\n") && !Parses("shoot_ped a1 3000\n") && !Parses("where\n"),
        "shoot_ped takes shooter, target and a duration; where takes an id");
    Check(Parses("clean_world 1 2 3 80\nfx_replay off\nfx_replay on\n") && !Parses("clean_world 1 2 3 0\n") && !Parses("fx_replay maybe\n"),
        "clean_world takes a point and a radius, fx_replay on or off");
    Check(Parses("research_windows on\nresearch_windows off\n") && !Parses("research_windows 1\n"),
        "research_windows takes on or off");
    Check(Parses("kill a1\n") && !Parses("kill\n"), "kill takes an id");
    Check(Parses("timescale 0.2\n") && !Parses("timescale 2\n") && !Parses("timescale 0\n"), "timescale takes 0.05..1");
    Check(Parses("health a2 900\ncarry player deer\n") && !Parses("health a2 0\n") && !Parses("carry player\n"),
        "health takes an id and hp, carry a carrier and a target");
    Check(Parses("gore a1 0x10\ngore player 1\nskeleton a1\n") && !Parses("gore a1 zz\n") && !Parses("gore a1 0\n") &&
        !Parses("skeleton\n"), "gore takes an id and a hex mask, skeleton an id");
    Check(Parses("play_marks\n") && !Parses("play_marks now\n"), "play_marks parses");
    // The timecycle and weather variation ops.
    Check(Parses("timecycle_modifier WINTER2_mountainWind\ntimecycle_modifier winter2_camp_clouds1 0.7\ntimecycle_modifier off\n"
                 "timecycle_transition winter2_climb_clouds2 5\nweather_variation BLIZZARD blizzard_winter2\n"
                 "weather_variation BLIZZARD off\ntimecycle_log a\n"),
        "timecycle ops parse");
    Check(!Parses("timecycle_modifier off 1\n") && !Parses("timecycle_modifier X 3\n") && !Parses("timecycle_transition X 0\n") &&
              !Parses("weather_variation BLIZZARD\n") && !Parses("timecycle_log\n"),
        "timecycle op arguments are checked");
    // A script sound by name on an entity.
    Check(Parses("script_sound weapon:player cook_meat_sizzle_loop Player_Campfire_Sounds\n"
                 "script_sound_var distanceToFlame 1 cook_meat_sizzle_loop Player_Campfire_Sounds\n"
                 "script_sound_stop cook_meat_sizzle_loop Player_Campfire_Sounds\nscript_sound a1 X Y\n"),
        "script sound ops parse");
    Check(!Parses("script_sound player X\n") && !Parses("script_sound weapon: X Y\n") &&
              !Parses("script_sound_var v x S T\n") && !Parses("script_sound_stop X\n"),
        "script sound op arguments are checked");
    // The moon ops.
    Check(Parses("moon_log a_before\nmoon_angle up\nmoon_angle 3.1416\nmoon_angle -0.5\n"), "moon ops parse");
    Check(!Parses("moon_log\n") && !Parses("moon_angle\n") && !Parses("moon_angle high\n") && !Parses("moon_angle 100\n") &&
              !Parses("moon_angle up 1\n"),
        "moon op arguments are checked");
    Check(Parses("lasso player a2\n") && !Parses("lasso player\n"), "lasso takes lassoer and target");
    Check(Parses("hold INPUT_AIM 1500\n") && !Parses("hold AIM 1500\n") && !Parses("hold INPUT_AIM 0\n"),
        "hold takes an INPUT_ action and a duration");
    Check(Parses("host_input rmb:2500,lmb@1200 3000\n") && !Parses("host_input rmb\n"), "host_input parses");
    Check(Parses("aim_at player a2\n") && !Parses("aim_at player\n"), "aim_at takes two ids");
    Check(Parses("hand_over 600000\n") && !Parses("hand_over\n"), "hand_over takes a limit");
    Check(Parses("purge_objects p_foundationwood03x 80\n") && !Parses("purge_objects p 900\n"),
        "purge_objects takes a model and a radius");
    Check(Parses("rope_probe 2 6.5\n") && !Parses("rope_probe 2 0\n") && !Parses("rope_probe x 3\n"),
        "rope_probe takes a type and a length");
    Check(Parses("rope_probe2 0 20 129\n") && !Parses("rope_probe2 0 20 0\n"), "rope_probe2 parses");
    Check(Parses("clip start lasso\n") && Parses("clip stop\n") && !Parses("clip stop x\n") && !Parses("clip pause\n"),
        "clip starts with an optional tag and stops");
    Check(Parses("rope_probe2 0 20 129 4.5\n") && !Parses("rope_probe2 0 20 129 0\n"),
        "rope_probe2 takes an optional forced length");
    Check(Parses("rope_census\n"), "rope_census parses");
    Check(Parses("say a1 GREET_GENERAL_STRANGER - 0\nsay a1 CTX 0405_U_M_M_RhdSheriff_01 3 SPEECH_PARAMS_BEAT_SHOUTED_CLEAR\n"
                 "scripted_speech a1 CTX - 2 2000\nvocal dog BARK\nvocal dog BARK 0\ninteract a1 greet\n"
                 "interact a1 antagonize\ninteract a1 defuse\nstop_speech a1\n"),
        "speech ops parse");
    Check(!Parses("say a1 CTX -\n") && !Parses("say a1 CTX - x\n") && !Parses("say a1 CTX - -2\n") &&
        !Parses("scripted_speech a1 CTX - 1\n") && !Parses("scripted_speech a1 CTX - 1 -5\n") &&
        !Parses("vocal dog BARK 2\n") && !Parses("vocal dog\n") && !Parses("interact a1 wave\n") &&
        !Parses("interact a1\n") && !Parses("stop_speech\n"),
        "speech op arguments are checked");
    Check(Parses("give_weapon player WEAPON_REVOLVER_CATTLEMAN\nmark x\nweather SUNNY\n"),
        "name arguments are free text");
    // A transition over time in seconds.
    Check(Parses("weather GROUNDBLIZZARD 40\nweather SNOWLIGHT 0.5\n") && !Parses("weather GROUNDBLIZZARD x\n") &&
        !Parses("weather GROUNDBLIZZARD 0\n") && !Parses("weather GROUNDBLIZZARD -5\n") && !Parses("weather GROUNDBLIZZARD 40 1\n"),
        "weather takes optional positive transition seconds");
    Check(Parses("give_weapon n1 WEAPON_MELEE_LANTERN 11\ngive_weapon n1 WEAPON_MELEE_TORCH 0\n") &&
        !Parses("give_weapon n1 WEAPON_MELEE_LANTERN 29\n") && !Parses("give_weapon n1 WEAPON_MELEE_LANTERN x\n") &&
        !Parses("give_weapon n1 WEAPON_MELEE_LANTERN 11 0\n"), "give_weapon takes an optional attach point 0..28");
    Check(Parses("drive_to d1 w1 1 2 3 4\ndrive_to d1 w1 1 2 3 4 coord\ndrive_to d1 w1 1 2 3 4 dest\n"
                 "drive_to d1 w1 1 2 3 4 straight\ndrive_to d1 w1 1 2 3 4 any\ndrive_to d1 w1 1 2 3 4 wander\n") &&
        !Parses("drive_to d1 w1 1 2 3 4 fly\n") && !Parses("drive_to d1 w1 1 2 z 4 dest\n") &&
        !Parses("drive_to d1 w1 1 2 3 4 dest 5\n"), "drive_to takes an optional mode");
    Check(Parses("throw_at a1 1 2 3\nthrow_ped a1 t1\nthrow_ped player t1\n") && !Parses("throw_ped a1\n") &&
        !Parses("throw_ped a1 t1 3\n") && !Parses("throw_at a1 1 2\n"), "throw_ped takes a thrower and a target");
    Check(Parses("face player 1 2 3\n") && !Parses("face player 1 2\n") && !Parses("face player 1 2 z\n"),
        "face takes an id and a point");
    Check(Parses("ammo a1 AMMO_DYNAMITE_VOLATILE\nammo player AMMO_ARROW_FIRE WEAPON_BOW\n") && !Parses("ammo a1\n") &&
        !Parses("ammo a1 AMMO_ARROW_FIRE WEAPON_BOW 3\n"), "ammo takes an id, an ammo name and an optional weapon");
    Check(Parses("weapon_condition player 1 0.5 0 1\nweapon_condition player - - - -\nweapon_condition a1 0 - 0.25 -\n") &&
        !Parses("weapon_condition player 1 1 1\n") && !Parses("weapon_condition player 2 0 0 0\n") &&
        !Parses("weapon_condition player 0 -0.5 0 0\n") && !Parses("weapon_condition player x 0 0 0\n"),
        "weapon_condition takes four levels 0..1 or -");
    Check(Parses("equip player WEAPON_REVOLVER_CATTLEMAN\nclip_ammo player 2\nreload player\nclip_ammo a1 0\n") &&
        !Parses("equip player\n") && !Parses("clip_ammo player 201\n") && !Parses("clip_ammo player x\n") &&
        !Parses("reload player now\n"),
        "equip, clip_ammo and reload");
    Check(Parses("marker_lens 0 near_clip=0.02 fstop=1.4 lens_mm=50 focus_d=2 dof=custom\n") && !Parses("marker_lens\n"),
        "marker_lens takes a time and key=value pairs");
    // blood_pools id [p1 size p3].
    Check(Parses("blood_pools t1\nblood_pools player 1 0.5 2\n") && !Parses("blood_pools t1 1 2\n") &&
            !Parses("blood_pools t1 1 x 2\n") && !Parses("blood_pools\n"),
        "blood_pools takes an id and optionally three numbers");
    // fire_ped id [intensity [flags]], stop_fire id, scorch id 0|1.
    Check(Parses("fire_ped v1\nfire_ped player 1.5\nfire_ped h2 1 8\n") && !Parses("fire_ped\n") &&
            !Parses("fire_ped v1 0\n") && !Parses("fire_ped v1 101\n") && !Parses("fire_ped v1 1 256\n") &&
            !Parses("fire_ped v1 x\n") && !Parses("fire_ped v1 1 8 2\n"),
        "fire_ped takes an id, an intensity 0..100 and flags 0..255");
    Check(Parses("stop_fire v1\nstop_fire player\n") && !Parses("stop_fire\n") && !Parses("stop_fire v1 1\n"),
        "stop_fire takes one id");
    // fire_forget takes nothing.
    Check(Parses("fire_forget\n") && !Parses("fire_forget 1\n"), "fire_forget takes no argument");
    Check(Parses("fire_untracked on\nfire_untracked off\n") && !Parses("fire_untracked 1\n") && !Parses("fire_untracked\n"),
        "fire_untracked takes on or off");
    Check(Parses("scorch v3 1\nscorch player 0\n") && !Parses("scorch v3\n") && !Parses("scorch v3 2\n") &&
            !Parses("scorch v3 on\n"),
        "scorch takes an id and 0 or 1");
    Check(Parses("ped_place player_horse -751 -462 44.8 90\n") && Parses("ped_place h1 -751 -462 44.8\n") &&
        !Parses("ped_place player_horse -751 -462\n") && !Parses("ped_place h1 x -462 44.8\n"),
        "ped_place takes an id or player_horse, then x y z [heading]");
    Check(Parses("weapon_state player WEAPON_REPEATER_CARBINE before_play\n") && !Parses("weapon_state player WEAPON_REPEATER_CARBINE\n"),
        "weapon_state takes an id, a weapon name and a tag");
    Check(Parses("weapon_component player WEAPON_RIFLE_BOLTACTION COMPONENT_RIFLE_BOLTACTION_WRAP\n") &&
            !Parses("weapon_component player WEAPON_RIFLE_BOLTACTION\n"),
        "weapon_component takes an id, a weapon and at least one component");
    Check(Parses("strap_test hide 549a7fc8\n") && Parses("strap_test auto 549a7fc8\n") && Parses("strap_test clear\n") &&
            Parses("strap_test flags 1000:-1:-1\n") && !Parses("strap_test hide\n") && !Parses("strap_test hide zz\n") &&
            !Parses("strap_test clear 1\n") && !Parses("strap_test swap on\n"),
        "strap_test: hide|show|auto hash, clear, flags");
    Check(Parses("inventory CONSUMABLE_WHISKEY 2\n") && !Parses("inventory CONSUMABLE_WHISKEY 0\n") &&
        !Parses("inventory CONSUMABLE_WHISKEY 100\n") && !Parses("inventory CONSUMABLE_WHISKEY\n") &&
        !Parses("inventory CONSUMABLE_WHISKEY x\n"), "inventory takes an item and a quantity 1..99");
    Check(Parses("lasso_target a1 a2\nlasso_target a1 h1 12000\n") && !Parses("lasso_target a1\n") &&
        !Parses("lasso_target a1 a2 500\n") && !Parses("lasso_target a1 a2 60000\n"),
        "lasso_target takes a lassoer, a target and an optional limit");
    Check(Parses("melee player v1 AR_TAKEDOWN_FRONT\nmelee a1 v1 0\n") && !Parses("melee a1 v1\n"),
        "melee takes an attacker, a target and an action");
    Check(Parses("animal_interaction player h1 INTERACTION_BRUSH P_BRUSHHORSE02X\nanimal_interaction player h1 INTERACTION_FOOD -\n") &&
        !Parses("animal_interaction player h1 INTERACTION_BRUSH\n"), "animal_interaction takes a ped, an animal, an interaction and a model");
    // The mount seat, -1 the saddle, 0 behind it.
    Check(Parses("set_on_mount r1 h1\nset_on_mount p1 h1 0\nmount p1 h1 0\nmount r1 h1 -1\n") &&
            !Parses("set_on_mount p1 h1 3\n") && !Parses("set_on_mount p1 h1 -2\n") && !Parses("mount p1 h1 x\n") &&
            !Parses("set_on_mount p1 h1 0 1\n"),
        "mount and set_on_mount take an optional seat -1..2");
    // The chest's children and the skeleton that appears.
    Check(Parses("attach_object forro bau 0 0 0.25\nattach_object garrafa bau -0.2 0 0.35 0 0 90\n") &&
            !Parses("attach_object forro bau 0 0\n") && !Parses("attach_object forro bau 0 x 0.25\n") &&
            !Parses("attach_object bau bau 0 0 0\n"),
        "attach_object takes a child, another parent and an offset (and a rotation)");
    Check(Parses("detach_object garrafa\nobject_skeleton bau2\n") && !Parses("detach_object\n") && !Parses("object_skeleton a b\n"),
        "detach_object and object_skeleton take one object");
    // The cooking script's burn level on a spawned object, and an object
    // attached to the player.
    Check(Parses("object_burn carne 0\nobject_burn carne 0.5\nobject_burn carne 1\nattach_object carne player 0.1 0 0.3\n") &&
            !Parses("object_burn carne\n") && !Parses("object_burn carne 1.5\n") && !Parses("object_burn carne -0.1\n") &&
            !Parses("object_burn carne cozida\n") && !Parses("object_burn carne 0.5 1\n"),
        "object_burn takes an object and a level from 0 to 1; attach_object takes the player as the parent");
    // A document's text and a tint on a spawned object.
    Check(Parses("object_texture nota UI_LETTER_CATFISH\nobject_texture nota none\nobject_texture nota 0x5A0D6A3B none 0x11\n"
                 "object_tint nota 0\nobject_tint nota 63\n") &&
            !Parses("object_texture nota\n") && !Parses("object_texture nota 0\n") && !Parses("object_texture a b c d e\n") &&
            !Parses("object_tint nota 64\n") && !Parses("object_tint nota -1\n") && !Parses("object_tint nota azul\n"),
        "object_texture takes an object and a TXD (name, hex or none, two more optional); object_tint takes 0..63");
    // A spawned object's bones animated.
    Check(Parses("object_anim tenda cnv_camp@rchso@cnv@ccsnk_par_cnv2 TENT_BASE_INTRO\n") &&
            Parses("object_anim tenda cnv_camp@rchso@cnv@ccsnk_par_cnv2 TENT_BASE_INTRO 0\n") &&
            !Parses("object_anim tenda cnv_camp@rchso@cnv@ccsnk_par_cnv2\n") &&
            !Parses("object_anim tenda cnv_camp@rchso@cnv@ccsnk_par_cnv2 TENT_BASE_INTRO 2\n"),
        "object_anim takes an object, a dict, an anim and an optional loop flag");
    // A worn item's wearable state.
    Check(Parses("wearable_state player NECKWEAR list\nwearable_state player any next\nwearable_state n1 MASKS @2\n"
                 "wearable_state player NECKWEAR BANDANA_FULL_FACE\n") &&
            !Parses("wearable_state player NECKWEAR\n") && !Parses("wearable_state player NECKWEAR @x\n") &&
            !Parses("wearable_state player NECKWEAR @40\n") && !Parses("wearable_state player NECKWEAR next 1\n"),
        "wearable_state takes an id, a category and list, next, @index or a state name");
    // A spawned ped frozen in place (never the player).
    Check(Parses("ped_freeze p1 on\nped_freeze p1 off\n") && !Parses("ped_freeze player on\n") &&
            !Parses("ped_freeze p1 yes\n") && !Parses("ped_freeze p1\n"),
        "ped_freeze takes a spawned ped and on|off");
    return g_benchFailures;
}
