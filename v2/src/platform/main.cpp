// ---------------------------------------------------------------------------
// v2 -- an endless voxel pine forest, path traced on NVIDIA Falcor.
//
//   The world is 10 cm voxels, the same grid the pine_1..9 assets are authored
//   on, so a trunk stands in ground made of the same lattice it is. The terrain
//   is a height field evaluated as a pure function of position and meshed to
//   its exposed faces only -- interior voxels never become geometry. You walk
//   it as a person: 20 voxels to the eye, gravity, a jump, and a head bob.
//
//   WHAT IS NEW IS THE PIPELINE, and nothing else. The world, the walk, the
//   day/night clock, the sky, the BSDFs and the integrator are v2's, carried
//   across with their numbers untouched. What changed is that the renderer
//   underneath them stopped being an OptiX pipeline and became one compute
//   shader doing inline ray tracing.
//
//   THAT IS THE WHOLE POINT OF THIS ENGINE. OptiX is built for offline
//   rendering, and it shows in the shape it forces on a game: a shader binding
//   table that has to be allocated, published and kept in step with what is
//   resident; a device artefact on disk that has to match the exe field for
//   field or the next launch dies with an illegal address; a continuation stack
//   sized for the deepest path any thread might take. None of that is about
//   drawing a forest. Under DXR inline ray tracing there is no table, no
//   separate artefact, and no stack -- the path loop is an ordinary loop in an
//   ordinary kernel, and a ray is a call. See shaders/Trace.cs.slang.
//
// Run with no --out to walk around; with --out to render one frame and exit.
// Scene, camera and sun are all reproducible from --seed.
// ---------------------------------------------------------------------------
#include "Core/SampleApp.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#include "platform/app.h"
#include "render/streamline.h"

// D3D12 reads this out of the PROCESS IMAGE before anything of ours runs, so it
// has to be exported from the executable and cannot live inside Falcor.dll.
// Without it the redistributable D3D12 runtime beside the exe is ignored, the
// OS one is used instead, and Falcor says so at startup.
FALCOR_EXPORT_D3D12_AGILITY_SDK

using namespace v2;

namespace {

void usage() {
    std::printf(
        "v2 -- voxel pine forest, path traced on Falcor (DXR inline ray tracing)\n"
        "\n"
        "  With no --out, v2 opens a window and you can walk around in it.\n"
        "  With --out, it renders one frame offline and exits.\n"
        "\n"
        "  --width N --height N      image / window size    (default 1280x720)\n"
        "  --spp N                   samples per pixel      (default 64, offline only)\n"
        "  --spf N                   samples traced per displayed frame (viewer)\n"
        "  --shadow-rays N           sun samples per vertex           (default 1)\n"
        "  --shadow-ray-depth N      bounces that get them            (default 1)\n"
        "  --settle                  let a still camera converge; default holds the grain\n"
        "  --background              open minimised, never take focus or the mouse\n"
        "                            (and silent -- see --no-sound)\n"
        "  --ambience F              forest ambience gain, 0 = silent   (default 0.25)\n"
        "                            what you hear is this times the canopy overhead\n"
        "  --sound PATH              the ambience bed; any file Media Foundation reads\n"
        "  --no-sound                open no audio device at all\n"
        "  --axe PATH --pick PATH    the .vox models the hand carries\n"
        "  --shovel PATH             ...and the shovel, which takes the loose ground\n"
        "  --no-axe                  open empty-handed; H toggles, the wheel changes tool\n"
        "  --swing-log               print what each swing ran into\n"
        "  --swing-hold              hold the swing, as --shot-walk holds W\n"
        "  --fell-test               fell a tree with no window and print what the\n"
        "                            body does, frame by frame, then exit\n"
        "  --float-test              dig the ground out from under a tree and a rock\n"
        "                            with no window, and report whether they fall\n"
        "  --dig-test                with no window: what a column is made of, which\n"
        "                            tool takes which band, and twelve swings at one\n"
        "                            spot to prove a pit is dug rather than repeated\n"
        "  --locate-test             with no window: ask /locate for every animal it\n"
        "                            knows, print what each found and how far off, and\n"
        "                            check that standing next to one is dry and clear\n"
        "  --clip-test               with no window: tick every population for a minute\n"
        "  --hoe-test                with no window: swing the hoe at your own feet and\n"
        "  --shaft-test              with no window: dig four metres straight down\n"
        "                            and follow the spoil out of the hole\n"
        "                            check it turns the earth, refuses to dig itself\n"
        "                            deeper, and grows back over\n"
        "  --wheat-test              with no window: find a stand of wheat, swing at it,\n"
        "                            and check it breaks, pays one wheat and one seed, and\n"
        "                            that both can be walked over and absorbed\n"
        "  --palette-vox [FILE]      with no window: write the LIVE material table out as a\n"
        "                            MagicaVoxel plate and exit, so new art can be authored\n"
        "                            against the colours the table already holds. Defaults\n"
        "                            to game/assets/palette_v2.vox; add --stage or --level\n"
        "                            to include what those places register\n"
        "                            and report any creature that is inside a tree or a\n"
        "                            rock. Run it in BOTH woods -- see --pine\n"
        "  --tool N                  which tool the hand opens with (0 axe, 1 pick,\n"
        "                            2 shovel, 3 bow, 4 empty hand)\n"
        "  --bow PATH --arrow PATH   the bow's draw strip, and what it looses\n"
        "  --draw-hold               hold the draw, as --swing-hold holds the swing\n"
        "  --shot-loose N            ...and let go on frame N, so a shot can be filmed\n"
        "  --reload-frame N          empty the rifle on frame N and reload it, which is\n"
        "                            the only way to photograph the nine-frame cycle\n"
        "  --rifle-reload DIR        where those frames live (numeric .vox order)\n"
        "  --walk F                  offline: advance F m/frame, film resets as it would\n"
        "                            when walking\n"
        "  --depth N                 max path length        (default 10)\n"
        "  --rr N                    first bounce Russian roulette may kill (default 1)\n"
        "  --clamp F                 firefly ceiling on indirect light, 0 = off (default 24)\n"
        "  --out PATH                render offline to this png and exit\n"
        "  --scale F                 game resolution as a fraction of the window   (0.70)\n"
        "                            NOT the window size -- the frame is MADE this big\n"
        "                            and stretched up to fill the window. Under DLSS it\n"
        "                            is what gets produced; the mode picks what is traced.\n"
        "  --flare F                 sun glare and lens flare, 0 = off             (2.0)\n"
        "  --rec SECONDS             record a take this long from startup, then exit\n"
        "  --rec-fps N               recorder capture rate, frames/s                 (60)\n"
        "  --rec-width N             cap the recording width; H.264 stops at 4096  (3840)\n"
        "  --speed F                 walk speed, m/s; sprint is 1.85x it           (4.97)\n"
        "  --sensitivity F           mouse look, degrees of turn per pixel         (0.12)\n"
        "  --eye F                   eye height, metres -- 20 voxels               (2.00)\n"
        "  --seed N                  world seed            (default 20260904)\n"
        "  --view N                  chunks of 25.6 m kept resident, radius, max 24  (15)\n"
        "  --density F               how thick the wood is, 0..1                 (0.3210)\n"
        "  --butterflies N           how many are in the air at once, 0 = none       (24)\n"
        "  --sfx F                   tool and weapon volume; 0 silences them        (1.0)\n"
        "  --arrow-pos X Y Z         nudge the nocked arrow, in whole 10 cm voxels:\n"
        "                            across, along the shaft, up -- the .vox file's own\n"
        "                            axes, and the three the settings panel edits (0 0 0)\n"
        "  --grass F                 fraction of grass columns with a strand      (0.105)\n"
        "  --grass-rows MIN MAX      strand height in voxels                        (3 6)\n"
        "  --flowers F               how thick a flower bed is, 0..1               (0.45)\n"
        "  --rocks F                 rock density                                (0.0075)\n"
        "  --pines DIR               folder with pine_1..9.vox\n"
        "  --decor DIR               folder with rocks/ and flowers.vox\n"
        "  --font PATH               the face all text is drawn in; \"off\" for Consolas\n"
        "                            (default the game's own 3x3-pixel.otf)\n"
        "  --sun-az DEG --sun-el DEG sun position, offline\n"
        "  --no-atmosphere           the OLD Preetham fit instead of Hillaire\n"
        "                            scattering -- cheaper, and its sunset freezes\n"
        "                            once the sun is under the horizon\n"
        "  --night-floor V           airglow/starlight the sky never goes below\n"
        "  --night-brightness M      how dark the night gets -- one master over the\n"
        "                            moon's key light AND the floor above. 1 is as\n"
        "                            rendered, 0 is black, 3 is a readable midnight\n"
        "  --blue-noise              void-and-cluster sampling on the shallow\n"
        "                            dimensions -- same variance, less of it visible\n"
        "  --auto-exposure           a trimmed histogram sets the stop, and adapts\n"
        "  --exposure-key V          what the middle of the frame is aimed at\n"
        "  --bloom V                 lens bloom strength, 0 off\n"
        "  --bloom-threshold V       where highlights start to bloom, exposed units\n"
        "  --time H                  viewer start hour, 0-24\n"
        "  --cycle N                 day/night speed, negative rewinds (default 1)\n"
        "                            a day is 20 minutes at 1x; X + wheel changes it\n"
        "  --turbidity F             haze, 2 clear .. 8    (default 2.8)\n"
        "  --cam-x F --cam-z F       camera ground position -- naming either one also\n"
        "                            turns off the random spawn\n"
        "  --spawn N                 spawn seed; 0 (the default) is somewhere new every\n"
        "                            launch. The world is unchanged -- only where in it\n"
        "                            you wake up. Viewer only; --out keeps its camera.\n"
        "  --yaw DEG --pitch DEG     camera direction      (default 205, 7)\n"
        "  --fov DEG                 vertical fov          (default 80)\n"
        "  --aperture F              OFFLINE lens diameter, metres      (0.055)\n"
        "  --focus F                 offline focus distance      (default: auto)\n"
        "  --exposure F              tone-map exposure\n"
        "  --shadow-lift F           tone curve toe, 0.03 crushed .. 0.20 open  (0.100)\n"
        "  --fog F                   haze density         (default 0.00164)\n"
        "  --fog-sky-under F         how much sky light reaches fog under the\n"
        "                            canopy, 0-1. The sky fill used to be added\n"
        "                            unshadowed, which piled up along whichever\n"
        "                            sightline was longest and read as a sun\n"
        "                            glare that followed the camera. 1 is that\n"
        "                            old behaviour back            (default 0.65)\n"
        "  --demodulate              PHASE B: split the lighting from the texture\n"
        "                            before denoising, and multiply it back after.\n"
        "                            Needed by NRD and the Super Resolution route;\n"
        "                            Ray Reconstruction demodulates internally and\n"
        "                            does NOT want it\n"
        "  --check-demod             prove the split is lossless -- one sample in,\n"
        "                            divided and multiplied straight back, compared\n"
        "                            against the composited trace -- then exit\n"
        "  --no-dlss                 turn off DLSS Ray Reconstruction and accumulate\n"
        "                            instead -- unbiased, and far noisier while walking\n"
        "  --rr-preset N             which Ray Reconstruction model: 5 = preset E\n"
        "                            (the default), 4 = D, 0 = the driver's own pick\n"
        "  --dlss MODE               ultra-performance | performance | balanced |\n"
        "                            quality | dlaa            (default quality)\n"
        "  --shot PATH               run the viewer, write a png, quit -- the only way\n"
        "                            to see a TEMPORAL renderer, which --out cannot\n"
        "  --shot-frame N            how many frames first          (default 240)\n"
        "  --shot-walk               hold W while they run\n"
        "  --shot-ui PATH            photograph the WINDOW instead -- the crosshair and\n"
        "                            the menu live there, not in the render\n"
        "  --menu                    open the settings panel at startup\n"
        // Listed now that it is the ONLY way in -- the panel had a key (I, then
        // O, then L) and has none any more. See App::onKeyEvent.
        "  --water-ui                open the water panel at startup; it has no key\n"
        "  --level                   start in the BUILDING rather than the wood --\n"
        "                            what [O] opens, for a shot that needs no keypress\n"
        "  --stage                   ...and the same for the asset editor's deck\n"
        "  --ground-stats            print what the ground is made of, region by region,\n"
        "                            and what it is lit by -- sun against sky -- then exit\n"
        "  --fly                     start in fly mode -- no collision, so a scripted\n"
        "  --acadia                  ACADIA NATIONAL PARK: the birch wood on real\n"
        "                            USGS elevation of Mount Desert Island, 10 km at\n"
        "                            true scale with Cadillac Mountain in it. Sets\n"
        "                            --dem, --no-cover, --dem-scale 1 and --birch;\n"
        "                            anything after it on the line still wins\n"
        "  --birch                   pin the world to the birch wood: low rounded\n"
        "                            hills, one light green, beehives in 1%% of trees\n"
        "  --pine                    pin the world to the pine wood. WITHOUT either flag\n"
        "                            the two are BANDS you walk between -- T, /locate birch\n"
        "  --cherry                  pin the world to the cherry wood: the oak's own\n"
        "  --coords                  open with the x y z readout up (F3 toggles it)\n"
        "  --desert                  pin the world to the open sand: dunes, cacti,\n"
        "                            scrub and no water at all\n"
        "  --deathvalley             ...on the REAL thing -- 50 km of Death Valley,\n"
        "                            California, centred on the Mesquite Flat dunes\n"
        "                            ground with pink crowns, pink butterflies and\n"
        "                            the pink songbird. /locate cherry walks there\n"
        "                            walk cannot park itself against a trunk\n"
        "  --shot-dt F               simulated seconds per frame  (default 1/60), so\n"
        "                            two captures cover the same ground\n"
        "  --stats                   print per-frame timings in the viewer\n"
        "  --profile                 measure --shot-frame frames and print where the\n"
        "                            time went -- percentiles, hitches, and how much of\n"
        "                            the main thread the streamer took. No png needed.\n"
        "  --restir                  ReSTIR GI: resample the indirect bounce across\n"
        "                            pixels and frames instead of tracing it fresh\n"
        "  --no-restir-world         ...without the world-space reservoirs, which are\n"
        "                            filed against the voxel face they were found on\n"
        "                            so a disocclusion has something to fall back on\n"
        "  --restir-world-m N        how much confidence one of those may carry (16)\n"
        "  --sharc-stats             count cache occupancy and hit rate, and print\n"
        "                            them with --profile\n"
        "  --sharc-stale N           frames an unwritten cache entry survives (32)\n"
        "  --sharc-hash-grid         file the radiance cache under the SDK's\n"
        "                            distance-quantised hash instead of the exact\n"
        "                            voxel face -- for comparing the two\n"
        "  --nrc-freq                the neural cache's old frequency encoding\n"
        "  --nrc-frozen              load weights and stop training\n"
        "  --nrc-load PATH           start from weights trained earlier -- the voxel\n"
        "                            encoding is what makes these transfer between\n"
        "                            worlds at all\n"
        "  --nrc-save PATH           write the trained weights out on exit\n"
        "  --vulkan                  use Vulkan instead of D3D12\n"
        "  --debug                   turn on the graphics debug layer (slow)\n"
        "  --hdr                     also write a linear .pfm\n");
}

bool argFloat(int argc, char **argv, int &i, float *out) {
    if (i + 1 >= argc) return false;
    *out = std::strtof(argv[++i], nullptr);
    return true;
}
bool argInt(int argc, char **argv, int &i, int *out) {
    if (i + 1 >= argc) return false;
    *out = std::atoi(argv[++i]);
    return true;
}
// Seeds are unsigned and use the whole range. atoi would saturate the top half
// of it at INT_MAX, so a seed the engine printed could not be typed back in.
bool argUint(int argc, char **argv, int &i, uint32_t *out) {
    if (i + 1 >= argc) return false;
    *out = uint32_t(std::strtoul(argv[++i], nullptr, 10));
    return true;
}

// ---------------------------------------------------------------------------
// The cache and resampling flags, lifted OUT of the else-if chain below.
//
// NOT A TIDY-UP. That chain is one nested block per option as far as the
// compiler is concerned, and MSVC stops at 128 of them -- "compiler limit:
// blocks nested too deeply", which is exactly what adding these seven in line
// produced. Anything further of this kind belongs here too.
// ---------------------------------------------------------------------------
// The wood's life, the hand, and what they sound like -- lifted out of the
// else-if chain for the reason the note above gives, and because these six were
// what pushed it back over the limit.
bool parseLifeOpt(const std::string &a, int argc, char **argv, int &i, Options *o) {
    if (a == "--butterflies") { argInt(argc, argv, i, &o->butterflies); return true; }
    if (a == "--butterfly-dir") {
        if (i + 1 < argc) o->butterflyDir = argv[++i];
        return true;
    }
    if (a == "--sound-dir") {
        if (i + 1 < argc) o->soundDir = argv[++i];
        return true;
    }
    if (a == "--sfx") { argFloat(argc, argv, i, &o->sfx); return true; }
    // Three numbers, in whole voxels -- see ArrowOffset in render/bow.h.
    if (a == "--arrow-pos") {
        argInt(argc, argv, i, &o->arrowNudge.across);
        argInt(argc, argv, i, &o->arrowNudge.along);
        argInt(argc, argv, i, &o->arrowNudge.up);
        return true;
    }
    if (a == "--drop-frame") { argInt(argc, argv, i, &o->dropFrame); return true; }
    if (a == "--spark-frame") { argInt(argc, argv, i, &o->sparkFrame); return true; }
    if (a == "--fire-frame") { argInt(argc, argv, i, &o->fireFrame); return true; }
    if (a == "--reload-frame") { argInt(argc, argv, i, &o->reloadFrame); return true; }
    if (a == "--scroll") { argInt(argc, argv, i, &o->scroll); return true; }
    if (a == "--rifle-reload") {
        if (i + 1 < argc) o->rifleReload = argv[++i];
        return true;
    }
    if (a == "--hurt-frame") { argInt(argc, argv, i, &o->hurtFrame); return true; }
    if (a == "--hurt-dim") { o->hurtDim = true; return true; }
    if (a == "--spark-only") { o->sparkOnly = true; return true; }
    if (a == "--tear-only") { o->tearOnly = true; return true; }
    if (a == "--smoke-ior") { argFloat(argc, argv, i, &o->smokeIor); return true; }
    if (a == "--spark-emit") {
        argFloat(argc, argv, i, &o->sparkR);
        argFloat(argc, argv, i, &o->sparkG);
        argFloat(argc, argv, i, &o->sparkB);
        return true;
    }
    if (a == "--stage") { o->stageAtStart = true; return true; }
    // --level: arrive in the building rather than the wood. See --stage, and
    // Options::levelAtStart for why a key alone is not enough.
    if (a == "--level") { o->levelAtStart = true; return true; }
    // --gizmo move | rot: open the deck with the subject already selected and
    // that handle up. See Options::gizmoAtStart for why it is a flag.
    if (a == "--gizmo") {
        o->stageAtStart = true;
        o->gizmoAtStart = 1;
        if (i + 1 < argc && argv[i + 1][0] != '-') {
            const std::string g = argv[++i];
            o->gizmoAtStart = (g == "rot" || g == "rotate" || g == "ring") ? 2 : 1;
        }
        return true;
    }
    // A gain over the stride and breath in render/helditem.h -- see kHandSway.
    if (a == "--hand-sway") { argFloat(argc, argv, i, &o->handSway); return true; }
    if (a == "--bird-dir") { if (i + 1 < argc) o->birdDir = argv[++i]; return true; }
    if (a == "--dem") { if (i + 1 < argc) o->demPath = argv[++i]; return true; }
    // -----------------------------------------------------------------------
    // --acadia -- THE BIRCH WOOD ON MOUNT DESERT ISLAND.
    //
    // (user 2026-09-18: "can you import the acadia national park dataset, and
    // use our birch trees ontop of the terrain. add the mushroom and rocks of
    // course and the life, the standard things that come with the birch
    // forest.")
    //
    // FOUR SETTINGS, AND EVERY ONE OF THEM IS A CONSEQUENCE OF THE DATASET
    // rather than a taste:
    //
    //   the window   10 km on -68.2700, 44.3550 -- the park's mountain ridge,
    //                Cadillac (465.6 m) through Sargent and Penobscot, with
    //                Eagle Lake and Jordan Pond in it and only 7.8% open sea.
    //                Measured over candidates; an 8 km window is 2.9% sea and
    //                a 12 km is 20%, which is a fifth of the world flat.
    //
    //   --dem-scale 1  TRUE SCALE, and here that is the right answer where in
    //                Colorado it was not. The objection to 1:1 is that a 10 km
    //                mountain crosses a 300 m view disc as one featureless
    //                ramp; Acadia's landforms are 1-2 km wide and 400 m tall,
    //                so a disc holds real curvature. It also fixes the stand
    //                density for free -- see below -- and keeps /locate's band
    //                jump inside the data.
    //
    //   NO COVER, and this is a measurement, not an omission. naip2cov's
    //                classifier is tuned on Colorado and does not transfer:
    //                over this window it calls 41.9% of the island bare ROCK
    //                at a median elevation of 64 m, where the bare granite
    //                domes are all above 250 m. Cross-checked against the DEM
    //                it is draped on, which is the check that catches this.
    //                Trees would then be forbidden over half of a national
    //                park that is famously forested. The water class IS right
    //                (median 2 m -- the sea), so the file is kept beside the
    //                DEM for whenever the classifier learns about Maine.
    //
    //   --birch      "use our birch trees". The bands are 800 m wide and the
    //                world is 10 km, so unforced this would be stripes of
    //                pine, birch and oak across the island.
    //
    // WHY THE SCALE FIXES THE DENSITY: realStemsPerHa is an ALTITUDE table
    // built for Colorado, and everything below 1800 m gets its floor of 120
    // stems a hectare. The target is then divided by the shrink, so at 6 the
    // island would be thinned to 20 stems a world hectare against the 146 the
    // scatter lays down -- a seventh of a wood. At 1 it is 120 against 146,
    // which is the standard wood very nearly untouched.
    //
    // LAST WINS, so --acadia --dem-scale 3 is the island at a third scale and
    // --acadia --cover <file> puts the imagery back. That is why this sets
    // fields rather than being read at load.
    // -----------------------------------------------------------------------
    if (a == "--acadia") {
        o->demPath = "C:/voxelbit/v2/assets/dem/acadia10.vbdem";
        // -- THE IMAGERY, FOR THE WATER AND NOTHING ELSE --------------------
        //
        // The first cut of this dropped the cover entirely, and that was half
        // right in a way worth writing down: the classifier's GROUND is wrong
        // here and its WATER is not, and on a DEM world the imagery is the only
        // thing that makes water at all (waterAt returns kNoWater the moment a
        // DEM is loaded -- an invented sea over measured ground is a plane
        // cutting valleys into islands). So --no-cover bought a forest and paid
        // for it with the entire coast: no sea round an ISLAND, no Eagle Lake,
        // no Jordan Pond, and with them no fish, no ducks, no lily pads, no
        // dragonflies and no frogs -- a third of the life this wood comes with.
        //
        // MEASURED BEFORE TRUSTING IT: 13.0% of the window is water, half of it
        // under 3 m (the sea), the rest clustered at 83 m, which is where those
        // two ponds actually sit; mean grade 5%, and only 9.9% of it on ground
        // steeper than 15%. That is a real coastline, not a shadow artefact.
        o->coverPath = "C:/voxelbit/v2/assets/dem/acadia10.vbcov";
        o->coverGround = false;
        o->demScale = 1.0f;
        o->birch = true;
        // -- AND IT HAS TO SPAWN ON THE ISLAND ------------------------------
        //
        // The default camX/camZ are a lake shore in Rocky Mountain National
        // Park quoted in REAL metres from ITS window's centre -- 14.6 km out,
        // which in a 10 km window is not merely the wrong place, it is off the
        // data, on the flat ground heightM holds at the border. That looks
        // exactly like the DEM having failed to load, and it is the trap the
        // note beside the division in app_load.inl warns about from the other
        // direction.
        //
        // 31, 154 IS SCORED, NOT PICKED: over the window at 60 m steps, the
        // spot with ~130 m of relief inside one 300 m view disc, ground between
        // 60 and 180 m asl, under a 22% grade where the player actually stands,
        // no more than a third of the disc flat (which is how a pond reads) and
        // 2 km clear of the border. It comes out on the forested west flank of
        // the Sargent-Penobscot ridge at 44.3537, -68.2697: 152 m up, 129 m of
        // hillside in view, a 17% grade underfoot.
        //
        // NOT camGiven -- the spawn picker still wanders 30-400 m off this for
        // an open, sunlit spot, which is what it is for.
        o->camX = 31.0f;
        o->camZ = 154.0f;
        // NOT camPlace: --acadia picks a WORLD, and this coordinate is a
        // starting hint inside it -- the note above already says the picker is
        // expected to wander off it. Scored for RELIEF, which is why the spawn
        // line said "water is 400 m away" until the roam landed: a starting
        // point chosen for the view is not a starting point beside a lake, and
        // an island is the one window where a shore is never far.
        return true;
    }
    // ------------------------------------------------- THE OAK WOOD'S GROUND
    //
    // (user 2026-09-18: "can you retrieve a dataset for an oak forest
    // elevation. replace the current oak forest terrain with the new
    // dataset.")
    //
    // LAKE OUACHITA, ARKANSAS -- the Ouachita National Forest, 12 km on
    // -93.3000, 34.6600. A drowned dendritic river valley in oak-hickory
    // country: 339 m of relief, 79.1% of the window forest, and the lake's
    // northern arms running up every hollow.
    //
    // WHY NOT THE OZARKS, WHICH IS THE PURER OAK. The first cut of this was
    // the upper Buffalo River in the Boston Mountains -- 12 km on -93.4000,
    // 36.1000, off tile n37w094 -- and on paper it wins: 446 m of relief
    // against 339, and the same 77.7% forest. It was built, and it has NO
    // WATER: one flat body, 1 hectare, 0.01% of the window. The Boston
    // Mountains are an upland with no lakes in them, and the Buffalo is a
    // free-flowing river too narrow for a 10.29 m posting to hold.
    //
    // On a DEM world the imagery is the only thing that makes water at all
    // (waterAt returns kNoWater the moment a DEM is loaded), so that window is
    // a wood with no fish, no ducks, no lily pads, no dragonflies and no
    // frogs -- the same third of the life the note over --acadia weighs, and
    // it loses for the same reason.
    //
    // AND THE SPECIES ARGUMENT IS MOOT, which is what settles it. The Ouachita
    // is oak-hickory-PINE where the Boston Mountains are oak-hickory, and it
    // makes no difference to anything: the cover only ever says forest, meadow
    // or rock, and the trees that go in it are the oak models this engine
    // authored. The real mix chooses nothing here. So the trade is drama
    // against the whole water half of the wood, and 339 m is still more world
    // relief than rmnp50 has after its shrink.
    //
    // MEASURED BEFORE TRUSTING IT:
    //
    //   * 14.3% of the window is water, and ALL of it is at lake level --
    //     15.8% of the 174-258 m band and 0.0% of every band above it. Water
    //     that climbed would show up here as a band that never empties.
    //   * the ridges are 98.4%, 99.4% and 99.5% forest going up, so the
    //     classifier has not smeared rock over the high ground the way it did
    //     on Mount Desert Island -- rock is 3.6% of the whole window. That is
    //     why this one keeps coverGround.
    //   * the DEM water pass found 113,255 samples of lake the photograph had
    //     called something else, which is the reservoir surface the imagery
    //     reads as dark forest. Without that pass this window is a third of a
    //     lake.
    if (a == "--ouachita") {
        o->demPath = "C:/voxelbit/v2/assets/dem/ouachita12.vbdem";
        o->coverPath = "C:/voxelbit/v2/assets/dem/ouachita12.vbcov";
        o->demScale = 1.0f;     // 174-513 m asl: the stand table floors at 120
                                // stems/ha below 1800 m, so a shrink would thin
                                // the wood to a seventh of itself for nothing.
        o->oakOnly = true;
        // ON THE SHORE, IN THE WOOD, WITH A HILLSIDE BEHIND IT. Scored over the
        // window at 25 m steps for forest that is not itself water, the most
        // relief inside 150 m, and the lake within 250 m so the water and
        // everything living in it is a walk away. It comes out at 76 m of
        // hillside in view with the shore 50 m off.
        o->camX = -3950.0f;
        o->camZ = -425.0f;
        // NOT camPlace either, and for the same reason -- plus this one's own
        // note asks for "the lake within 250 m so the water and everything
        // living in it is a walk away", which a 60 m water gate serves better
        // than a single scored coordinate can.
        return true;
    }
    // The two ways back to the old world, kept deliberately.
    if (a == "--no-dem") { o->demPath.clear(); return true; }
    // A NO-OP NOW, AND KEPT SO EVERY SCRIPT AND NOTE THAT USES IT STILL RUNS.
    // The three bands are the default again -- see Options::pineOnly and the
    // 2026-09-18 "fix the birch forest. its not in the world".
    if (a == "--all-woods") { o->pineOnly = false; return true; }
    if (a == "--dem-base") { argFloat(argc, argv, i, &o->demBaseM); return true; }
    if (a == "--dem-scale") { argFloat(argc, argv, i, &o->demScale); return true; }
    if (a == "--dem-exag") { argFloat(argc, argv, i, &o->demExag); return true; }
    if (a == "--dem-detail") { argFloat(argc, argv, i, &o->demDetail); return true; }
    if (a == "--dem-rough") { argFloat(argc, argv, i, &o->demRough); return true; }
    if (a == "--oak-density") { argFloat(argc, argv, i, &o->oakDensity); return true; }
    if (a == "--inset" && i + 1 < argc) { o->insetPath = argv[++i]; return true; }
    if (a == "--inset-at" && i + 2 < argc) {
        argFloat(argc, argv, i, &o->insetX);
        argFloat(argc, argv, i, &o->insetZ);
        return true;
    }
    if (a == "--spawn-pick") { o->spawnPick = true; return true; }
    if (a == "--waves") { argFloat(argc, argv, i, &o->waves); return true; }
    if (a == "--stem-div") { argFloat(argc, argv, i, &o->stemDiv); return true; }
    // The two places worth standing in this window, by name rather than by
    // coordinate. Both are REAL metres from the window centre.
    // Places worth standing, by name. REAL metres from the window centre of
    // whichever .vbdem is loaded -- so --lake/--peak only mean anything with
    // the Front Range window (--dem .../front60.vbdem).
    if (a == "--lake") { o->camX = -10717.0f; o->camZ = -19649.0f; o->camPlace = true;
                         return true; }
    if (a == "--peak") { o->camX = 10316.0f; o->camZ = 18445.0f; o->camPlace = true;
                         return true; }
    if (a == "--front") { o->demPath = "C:/voxelbit/v2/assets/dem/front60.vbdem";
                          o->coverPath = "C:/voxelbit/v2/assets/dem/front60.vbcov";
                          o->camX = -10717.0f; o->camZ = -19649.0f; o->camPlace = true;
                          return true; }
    if (a == "--cover") { if (i + 1 < argc) o->coverPath = argv[++i]; return true; }
    if (a == "--no-cover") { o->coverPath.clear(); return true; }
    // The imagery for the WATER only -- its lakes and its coast, none of its
    // opinions about trees or bare ground. See VoxelTerrain::coverGround.
    if (a == "--cover-water") { o->coverGround = false; return true; }
    // ...and back again, so --acadia --cover-ground is the island WITH the
    // classifier's opinion of the ground -- which is worth being able to ask
    // for, if only to look at what it got wrong.
    if (a == "--cover-ground") { o->coverGround = true; return true; }
    return false;
}

bool parseCacheOpt(const std::string &a, int argc, char **argv, int &i, Options *o) {
    // The SDK's distance-quantised hash grid, in place of v2's exact voxel face
    // key. For A/B comparison of the two -- see shaders/Sharc.slang.
    if (a == "--sharc-hash-grid") { o->sharcHashGrid = true; return true; }
    // Cache instrumentation, and the eviction window it exists to tune.
    if (a == "--sharc-stats") { o->sharcStats = true; return true; }
    if (a == "--sharc-stale") { argInt(argc, argv, i, &o->sharcStale); return true; }
    if (a == "--sharc-entries") { argInt(argc, argv, i, &o->sharcEntries); return true; }
    // ReSTIR GI, and the world-space reservoirs inside it.
    if (a == "--restir") { o->restir = true; return true; }
    if (a == "--no-restir-world") { o->noRestirWorld = true; return true; }
    if (a == "--restir-world-m") { argInt(argc, argv, i, &o->restirWorldM); return true; }
    // The neural cache: which encoding, and where its weights live.
    if (a == "--nrc-freq") { o->nrcFreqEncoding = true; return true; }
    // Load weights and DO NOT keep learning. What makes "train once, ship the
    // weights" a measurable claim rather than an assertion.
    if (a == "--nrc-frozen") { o->nrcFrozen = true; return true; }
    if (a == "--nrc-sdk") { o->nrcSdk = true; return true; }
    if (a == "--nrc-sdk-radiance") { argFloat(argc, argv, i, &o->nrcSdkRadiance); return true; }
    if (a == "--nrc-sdk-builtin") { o->nrcSdkBuiltin = true; o->nrcSdk = true; return true; }
    if (a == "--nrc-sdk-debug") { argInt(argc, argv, i, &o->nrcSdkDebug); o->nrcSdk = true; return true; }
    // The step size the cache learns at. Exposed because the default it
    // shipped with drives BOTH encodings into the weight clamp -- see the
    // note on learningRate in gpu/nrc.h.
    if (a == "--nrc-lr") { argFloat(argc, argv, i, &o->nrcLr); return true; }
    if (a == "--nrc-load") { if (i + 1 < argc) o->nrcLoad = argv[++i]; return true; }
    if (a == "--nrc-save") { if (i + 1 < argc) o->nrcSave = argv[++i]; return true; }
    return false;
}

bool parse(int argc, char **argv, Options *o, bool *vulkan, bool *debugLayer) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        // OUTSIDE THE CHAIN BELOW, DELIBERATELY. That else-if ladder is at
        // MSVC's nesting ceiling -- adding one more rung is a hard
        // "compiler limit: blocks nested too deeply" (C1061), not a warning.
        // Anything new goes here, before it, and continues.
        if (a == "--water-ui") { o->waterPanelAtStart = true; continue; }
        // BEFORE THE CHAIN, like every flag added since the C1061 -- see the
        // note above. Puts the three pause buttons up as soon as the spawn has
        // settled, so --shot-ui and --out can photograph them with no
        // keystroke.
        if (a == "--room") { o->roomAtStart = true; continue; }
        // BEFORE THE CHAIN, like every flag added since the C1061 -- see the
        // note above. Surveys the /locate life table with no window at all.
        if (a == "--locate-test") { o->locateTest = true; continue; }
        // ...AND THE SAME, for the check that no animal is inside anything.
        if (a == "--clip-test") { o->clipTest = true; continue; }
        // DOES A HUNTER COME AT YOU AND COST YOU HEALTH -- see runBiteTest.
        if (a == "--bite-test") { o->biteTest = true; continue; }
        // BEFORE THE CHAIN, like every flag added since the C1061 -- see the
        // note above. Writes the live material table out as a MagicaVoxel
        // plate and exits; the optional path overrides where it goes.
        if (a == "--palette-vox") {
            o->paletteVox = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') o->paletteVoxOut = argv[++i];
            continue;
        }
        if (a == "--wheat-test") { o->wheatTest = true; continue; }
        if (a == "--food-test") { o->foodTest = true; continue; }
        if (a == "--float-audit") { o->floatAudit = true; continue; }
        if (a == "--float-sweep") { o->floatSweep = true; continue; }
        if (a == "--rip-test") { o->ripTest = true; continue; }
        if (a == "--pole-test") { o->poleTest = true; continue; }
        if (a == "--refresh-frame") { argInt(argc, argv, i, &o->refreshFrame); continue; }
        if (a == "--hoe-test") { o->hoeTest = true; continue; }
        if (a == "--shaft-test") { o->shaftTest = true; continue; }
        if (a == "--kill-test") { o->killTest = true; continue; }
        if (a == "--duck-test") { o->duckTest = true; continue; }
        // HERE, NOT IN THE else-if CHAIN BELOW -- that chain is at MSVC's
        // block nesting limit and one more arm is a C1061, not a warning.
        if (a == "--hitch") { o->hitch = true; continue; }
        if (a == "--lbug-test") { o->lbugTest = true; continue; }
        if (a == "--oak") { o->oakOnly = true; continue; }
        if (a == "--cherry") { o->cherryOnly = true; continue; }
        if (a == "--desert") { o->desertOnly = true; continue; }
        if (a == "--coords") { o->coords = true; continue; }
        // -- DEATH VALLEY, CALIFORNIA -------------------------------------
        //
        // (user 2026-09-19: "find a desert landscape dataset, somewhere in
        //  california maybe?")
        //
        // THE REAL GROUND UNDER THE DESERT BIOME, the way --acadia is the real
        // ground under the birch wood. A 50 km window on 36.61 N, -117.11 W --
        // the Mesquite Flat dune field, with the valley floor running south
        // east from it and the Grapevine and Panamint ranges either side.
        // Measured off the 3DEP tiles: -83.6 m at the salt pan to 2283 m on
        // the ridge, which is 2366 m of relief and the deepest ground in North
        // America inside the same window.
        //
        // IT FORCES THE BIOME, and that is not a convenience. A DEM world takes
        // its FLOOR from the aerial imagery (mat::GROUND_0..9) and there is no
        // .vbcov for this window -- so without the pin the ground would fall
        // through to a forest floor of soil and litter over Death Valley. The
        // desert biome paints sand by its own rule, which needs no imagery.
        //
        // ...AND THE COVER HAS TO BE CLEARED BY HAND, WHICH COST A LAKE.
        //
        // coverPath is not derived from demPath -- it is a separate option with
        // its own DEFAULT, and that default is Rocky Mountain's. Setting the
        // DEM alone therefore does not give a window with no imagery, it gives
        // Death Valley's ground wearing RMNP's land cover, and the first render
        // of this flag had open water on the horizon of Badwater Basin: the
        // cover raster's lakes, faithfully placed, 300 km from where they are.
        //
        // The biome pin hid half of it. A forced desert paints sand over
        // whatever the cover says the ground is, so the FLOOR looked right and
        // only the water gave it away.
        //
        // "" is the value the loader already understands -- app_load skips the
        // cover entirely when the path is empty -- so this is the flag saying
        // the true thing about its own window rather than a new switch.
        //
        // NO COVER MEANS NO MAPPED WATER, which is exactly right here: Badwater
        // is a salt pan, not a lake, and waterAt returns kNoWater through the
        // whole band anyway.
        if (a == "--deathvalley" || a == "--death-valley") {
            o->demPath = "C:/voxelbit/v2/assets/dem/deathvalley50.vbdem";
            o->coverPath = "";
            o->desertOnly = true;
            continue;
        }
        if (a == "--soil-test") { o->soilTest = true; continue; }
        // The same NINE bits the panel sets, for a scripted A/B: 511 is all on,
        // and clearing one proves that term and only that term moved. 507 is
        // what v2 ships with -- everything but the world reflection.
        //
        // THIS IS ONLY AS GOOD AS WHERE THE NUMBER IS APPLIED. It reached the
        // interactive path and not the offline one for as long as it existed,
        // because App::onLoad seeded the tracer two hundred lines past the
        // `--out` return -- so `--out --water-flags 0` and `--water-flags 511`
        // rendered pixel-for-pixel identical images and every scripted A/B of a
        // water term silently compared a picture with itself. Fixed there, and
        // the note at the seeding site says what else belongs above that
        // return.
        if (a == "--water-flags" && i + 1 < argc) {
            o->waterFlags = uint32_t(std::atoi(argv[++i]));
            continue;
        }
        if (a == "--help" || a == "-h") { usage(); return false; }
        else if (a == "--width") argInt(argc, argv, i, &o->r.width);
        else if (a == "--height") argInt(argc, argv, i, &o->r.height);
        else if (a == "--spp") argInt(argc, argv, i, &o->r.spp);
        else if (a == "--walk") argFloat(argc, argv, i, &o->walk);
        else if (a == "--spf") argInt(argc, argv, i, &o->samplesPerFrame);
        else if (a == "--shadow-rays") argInt(argc, argv, i, &o->r.shadowRays);
        else if (a == "--shadow-ray-depth") argInt(argc, argv, i, &o->r.shadowRayDepth);
        else if (a == "--settle") o->constantGrain = false;
        else if (a == "--background") o->background = true;
        else if (a == "--depth") argInt(argc, argv, i, &o->r.maxDepth);
        else if (a == "--rr") argInt(argc, argv, i, &o->r.rrStart);
        else if (a == "--clamp") argFloat(argc, argv, i, &o->r.clampIndirect);
        else if (a == "--grass") argFloat(argc, argv, i, &o->grass);
        else if (a == "--flowers") argFloat(argc, argv, i, &o->flowers);
        else if (a == "--rocks") argFloat(argc, argv, i, &o->rocks);
        else if (a == "--density") argFloat(argc, argv, i, &o->treeDensity);
        else if (a == "--view") argInt(argc, argv, i, &o->view);
        else if (a == "--grass-rows") {
            argInt(argc, argv, i, &o->grassMin);
            argInt(argc, argv, i, &o->grassMax);
        }
        else if (a == "--pines") { if (i + 1 < argc) o->pines = argv[++i]; }
        else if (a == "--decor") { if (i + 1 < argc) o->decor = argv[++i]; }
        else if (a == "--font") { if (i + 1 < argc) o->font = argv[++i]; }
        else if (a == "--sound") { if (i + 1 < argc) o->sound = argv[++i]; }
        else if (a == "--ambience") argFloat(argc, argv, i, &o->ambience);
        else if (a == "--no-sound") o->soundOn = false;
        // THE HAND TOOLS, IN ONE ARM.
        //
        // Three flags that differ only in which string they land in, and they are
        // written as one because MSVC has a HARD LIMIT on how deep an else-if chain
        // may nest -- 128 links, C1061 -- and this parser was one link under it.
        // Adding the shovel as a fourth reached it, and the error names a line at
        // the END of the function rather than the flag that was added, so it reads
        // like the file is broken rather than like the list is full.
        //
        // Folding the family into one arm gives the chain a link back instead of
        // taking one, so this is where the NEXT tool path goes too.
        else if (a == "--axe" || a == "--pick" || a == "--shovel") {
            std::string *dst = (a == "--axe")    ? &o->axe
                               : (a == "--pick") ? &o->pick
                                                 : &o->shovel;
            if (i + 1 < argc) *dst = argv[++i];
        }
        else if (a == "--tool") argInt(argc, argv, i, &o->tool);
        else if (a == "--bow") { if (i + 1 < argc) o->bow = argv[++i]; }
        else if (a == "--arrow") { if (i + 1 < argc) o->arrow = argv[++i]; }
        else if (a == "--draw-hold") o->drawHold = true;
        else if (a == "--shot-loose") argInt(argc, argv, i, &o->shotLoose);
        else if (a == "--no-axe") o->axeOn = false;
        else if (a == "--swing-log") o->swingLog = true;
        else if (a == "--fell-test") o->fellTest = true;
        else if (a == "--float-test") o->floatTest = true;
        else if (a == "--dig-test") o->digTest = true;
        else if (a == "--swing-hold") o->swingHold = true;
        else if (a == "--time") {
            float h = 7.0f;
            argFloat(argc, argv, i, &h);
            o->timeOfDay = (h / 24.0f) - floorf(h / 24.0f);
        }
        else if (a == "--cycle") argFloat(argc, argv, i, &o->cycleSpeed);
        else if (a == "--sun-az") argFloat(argc, argv, i, &o->sunAz);
        else if (a == "--sun-el") argFloat(argc, argv, i, &o->sunEl);
        else if (a == "--turbidity") argFloat(argc, argv, i, &o->turbidity);
        else if (a == "--cam-x") { argFloat(argc, argv, i, &o->camX); o->camGiven = true; }
        else if (a == "--cam-z") { argFloat(argc, argv, i, &o->camZ); o->camGiven = true; }
        else if (a == "--spawn") argUint(argc, argv, i, &o->spawnSeed);
        else if (a == "--yaw") { argFloat(argc, argv, i, &o->yaw); o->yawGiven = true; }
        else if (a == "--pitch") argFloat(argc, argv, i, &o->pitch);
        else if (a == "--eye") argFloat(argc, argv, i, &o->eye);
        else if (a == "--fov") argFloat(argc, argv, i, &o->fov);
        else if (a == "--pinecones") argInt(argc, argv, i, &o->pineconesPerTree);
        else if (a == "--collide-probe") o->collideProbe = true;
        else if (a == "--aperture") argFloat(argc, argv, i, &o->aperture);
        else if (a == "--focus") argFloat(argc, argv, i, &o->focus);
        else if (a == "--exposure") argFloat(argc, argv, i, &o->r.exposure);
        else if (a == "--shadow-lift") argFloat(argc, argv, i, &o->r.shadowLift);
        else if (a == "--deep-lift") argFloat(argc, argv, i, &o->r.deepLift);
        else if (a == "--deep-range") argFloat(argc, argv, i, &o->r.deepRange);
        else if (a == "--fog") argFloat(argc, argv, i, &o->r.fogDensity);
        else if (a == "--fog-aniso") { argFloat(argc, argv, i, &o->fogAniso); o->fogAnisoGiven = true; }
        else if (a == "--fog-ambient") { argFloat(argc, argv, i, &o->fogAmbient); o->fogAmbientGiven = true; }
        else if (a == "--fog-sky-under") { argFloat(argc, argv, i, &o->fogSkyUnder); o->fogSkyUnderGiven = true; }
        else if (a == "--cloud-cut") { argFloat(argc, argv, i, &o->cloudCut); o->cloudCutGiven = true; }
        else if (a == "--cloud-var") { argFloat(argc, argv, i, &o->cloudVar); o->cloudVarGiven = true; }
        else if (a == "--cloud-sun") { argFloat(argc, argv, i, &o->cloudSun); o->cloudSunGiven = true; }
        else if (a == "--atmosphere") o->atmosphere = true;
        else if (a == "--no-atmosphere") o->atmosphere = false;
        else if (a == "--night-floor") {
            argFloat(argc, argv, i, &o->nightFloor);
            o->nightFloorGiven = true;
        }
        // NO GIVEN-FLAG. It is a multiplier with a neutral value, so an unset
        // one is 1.0 and changes nothing -- there is no "unspecified" for it to
        // mean, which is the only thing a given-flag is ever for.
        else if (a == "--night-brightness") argFloat(argc, argv, i, &o->nightBrightness);
        else if (a == "--blue-noise") o->blueNoise = true;
        else if (a == "--no-blue-noise") o->blueNoise = false;
        else if (a == "--auto-exposure") o->autoExposure = true;
        else if (a == "--no-auto-exposure") o->autoExposure = false;
        else if (a == "--bloom") argFloat(argc, argv, i, &o->bloom);
        else if (a == "--bloom-threshold") {
            argFloat(argc, argv, i, &o->bloomThreshold);
            o->bloomThresholdGiven = true;
        }
        else if (a == "--exposure-key") {
            argFloat(argc, argv, i, &o->expKey);
            o->expKeyGiven = true;
        }
        else if (a == "--cloud-moon-key") { argFloat(argc, argv, i, &o->cloudMoonKey); o->cloudMoonKeyGiven = true; }
        else if (a == "--moon") { argFloat(argc, argv, i, &o->moonScale); o->moonScaleGiven = true; }
        else if (a == "--moon-key") { argFloat(argc, argv, i, &o->moonKey); o->moonKeyGiven = true; }
        else if (a == "--moon-phase") { argFloat(argc, argv, i, &o->moonPhase); o->moonPhaseGiven = true; }
        else if (a == "--demodulate") o->demodulate = true;
        else if (a == "--check-demod") { o->checkDemod = true; o->outGiven = true; }
        else if (a == "--no-dlss") o->dlss = false;
        else if (a == "--rr-preset") argInt(argc, argv, i, &o->rrPreset);
        else if (a == "--dlss") {
            if (i + 1 >= argc) { std::fprintf(stderr, "v2: --dlss needs a mode\n"); return false; }
            const std::string m = argv[++i];
            if (m == "ultra-performance") o->dlssQuality = DlssQuality::UltraPerformance;
            else if (m == "performance") o->dlssQuality = DlssQuality::Performance;
            else if (m == "balanced") o->dlssQuality = DlssQuality::Balanced;
            else if (m == "quality") o->dlssQuality = DlssQuality::Quality;
            else if (m == "dlaa") o->dlssQuality = DlssQuality::Dlaa;
            else {
                std::fprintf(stderr, "v2: unknown DLSS mode %s\n", m.c_str());
                return false;
            }
        }
        else if (a == "--shot") { if (i + 1 < argc) o->shotPath = argv[++i]; }
        else if (a == "--shot-frame") argInt(argc, argv, i, &o->shotFrame);
        else if (a == "--shot-walk") o->shotWalk = true;
        else if (a == "--shot-ui") { if (i + 1 < argc) o->shotUi = argv[++i]; }
        else if (a == "--menu") o->menuAtStart = true;
        else if (a == "--ground-stats") o->groundStats = true;
        else if (a == "--fly") o->startFly = true;
        else if (a == "--birch") o->birch = true;
        else if (a == "--pine") o->pineOnly = true;
        else if (a == "--shot-dt") argFloat(argc, argv, i, &o->shotDt);
        else if (a == "--stats") o->stats = true;
        else if (a == "--profile") o->profile = true;
        else if (a == "--hdr") o->writeHdr = true;
        else if (a == "--vulkan") *vulkan = true;
        else if (a == "--nrc") o->nrc = true;
        else if (parseCacheOpt(a, argc, argv, i, o)) { }
        else if (parseLifeOpt(a, argc, argv, i, o)) { }
        // Sky-dome next event estimation, and the irradiance cache. Both are on
        // by default; these turn them off or retune them without a rebuild,
        // which is also how a "did this change the picture" comparison is made.
        else if (a == "--sky-rays") argInt(argc, argv, i, &o->r.skyRays);
        else if (a == "--sky-ray-depth") argInt(argc, argv, i, &o->r.skyRayDepth);
        else if (a == "--no-sky-nee") o->r.skyRays = 0;
        // 0 none, 1 DDGI probes, 2 SHaRC.
        else if (a == "--gi") argInt(argc, argv, i, &o->r.giMode);
        else if (a == "--no-gi") o->r.giMode = 0;
        else if (a == "--gi-depth") argInt(argc, argv, i, &o->r.giDepth);
        else if (a == "--gi-strength") argFloat(argc, argv, i, &o->r.giStrength);
        else if (a == "--cluster-test") o->clusterTest = true;

        // Debug switches: the two halves of the reuse, separately, so a bias can
        // be attributed to one of them instead of guessed at.
        else if (a == "--fg") {
            if (i + 1 < argc) {
                const std::string m = argv[++i];
                o->frameGen = (m == "2x")   ? FrameGen::On2x
                              : (m == "3x") ? FrameGen::On3x
                              : (m == "4x") ? FrameGen::On4x
                                            : FrameGen::Off;
            }
        }
        else if (a == "--debug") *debugLayer = true;
        else if (a == "--seed") { int s = 0; argInt(argc, argv, i, &s); o->r.seed = uint32_t(s); }
        else if (a == "--scale") argFloat(argc, argv, i, &o->scale);
        else if (a == "--flare") { argFloat(argc, argv, i, &o->flare); o->flareGiven = true; }
        else if (a == "--rec") argFloat(argc, argv, i, &o->recSeconds);
        else if (a == "--rec-fps") argInt(argc, argv, i, &o->recFps);
        else if (a == "--rec-width") argInt(argc, argv, i, &o->recMaxWidth);
        else if (a == "--speed") argFloat(argc, argv, i, &o->speed);
        else if (a == "--sensitivity") argFloat(argc, argv, i, &o->sensitivity);
        else if (a == "--out" || a == "--render") {
            if (i + 1 < argc) { o->out = argv[++i]; o->outGiven = true; }
        }
        else {
            std::fprintf(stderr, "v2: unknown option %s\n", a.c_str());
            usage();
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    Options o;
    bool vulkan = false, debugLayer = false;
    if (!parse(argc, argv, &o, &vulkan, &debugLayer)) return 0;
    if (o.r.width < 8 || o.r.height < 8 || o.r.spp < 1) {
        std::fprintf(stderr, "v2: nonsensical image parameters\n");
        return 1;
    }
    o.scale = clampf(o.scale, 0.10f, 2.0f);

    // -- Streamline, BEFORE Falcor exists --------------------------------
    //
    // slInit has to run before the D3D12 device is created, because what
    // Streamline does is stand underneath device and swapchain creation. Once
    // Falcor has made a real device it is too late -- there is nothing left to
    // interpose on, and Frame Generation has nowhere to put a frame.
    //
    // That is why this is in main() and not in onLoad(): SampleApp creates the
    // device in its own constructor, so onLoad is already past the point of no
    // return. See gpu/streamline.h for the whole mechanism.
    //
    // Vulkan is skipped deliberately. This integration is D3D12 -- it asks for
    // sl::RenderAPI::eD3D12 and hands Streamline an ID3D12Device -- and calling
    // it on a Vulkan run would initialise a Streamline that then refuses every
    // feature, printing failures for something nobody asked for.
    // UNCONDITIONAL, matching v6. Guarding this on the backend looked tidy --
    // the integration is D3D12 -- but preInit only loads the interposer and
    // resolves entry points; it is init() that needs a device. Skipping it on
    // Vulkan changed nothing there and was one more way for the two engines to
    // differ while chasing why frame generation was inert.
    Streamline::preInit(Falcor::getRuntimeDirectory());

    SampleAppConfig c;
    c.deviceDesc.type = vulkan ? Falcor::Device::Type::Vulkan : Falcor::Device::Type::D3D12;
    c.deviceDesc.enableDebugLayer = debugLayer;
    c.windowDesc.width = o.r.width;
    c.windowDesc.height = o.r.height;
    c.windowDesc.title = o.background ? "v2 [background] -- pine forest (Falcor)"
                                      : "v2 -- pine forest (Falcor)";
    c.windowDesc.resizableWindow = true;
    // Straight to the taskbar rather than shown and then minimised: the latter
    // flashes a window across whatever the person at the keyboard is looking at.
    //
    // ...AND ANY AUTOMATED CAPTURE IMPLIES IT. A --shot or --shot-ui run is a
    // script taking a picture; it needs a window because it photographs the
    // swap chain WITH the interface on it, but it has no business appearing in
    // front of whoever is at the keyboard. Forgetting --background on one of
    // those is a mistake with no upside, so it is not a mistake that can be
    // made any more.
    if (o.background || !o.shotUi.empty() || !o.shotPath.empty())
        c.windowDesc.mode = Falcor::Window::WindowMode::Minimized;

    // -----------------------------------------------------------------------
    // NO WINDOW AT ALL FOR ANYTHING THAT ONLY PRINTS.
    //
    // SampleApp then skips the message loop and simply runs, which is what a
    // one-frame render wants -- and it is equally what the three HEADLESS
    // DIAGNOSTICS want. They were not on this list, so --fell-test,
    // --float-test and --dig-test each opened a real window, took the focus and
    // sat on top of whatever the user was doing, for a run whose entire output
    // is text on stdout. Reported, twice.
    //
    // A test that has to be watched is a test with a bug in it. If one of these
    // ever needs pixels it should take a --shot like everything else, which is
    // covered by the minimise above.
    // -----------------------------------------------------------------------
    c.headless = o.outGiven || o.fellTest || o.floatTest || o.digTest || o.locateTest ||
                 o.clipTest || o.wheatTest || o.hoeTest || o.foodTest || o.floatAudit || o.floatSweep || o.ripTest || o.poleTest ||
                 o.shaftTest || o.killTest || o.soilTest || o.duckTest || o.lbugTest;

    // Every device failure in this engine arrives as an exception carrying the
    // call that failed and the driver's own description of why. Catching it
    // here means the exit path prints that, rather than the "terminate called
    // after throwing" an uncaught one would.
    try {
        ForestApp app(c, o);
        std::printf("v2 -- Falcor 8.0, %s, on %s\n", vulkan ? "Vulkan" : "D3D12",
                    app.getDevice()->getInfo().adapterName.c_str());
        std::fflush(stdout);
        return app.run();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "\nv2: %s\n", e.what());
        return 1;
    }
}
