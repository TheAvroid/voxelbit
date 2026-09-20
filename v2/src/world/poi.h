// poi.h -- the places worth going, FOUND IN THE DATA.
//
// Builds a list of summits and lakes from whatever .vbdem/.vbcov is loaded, so
// /locate works in any window without a table of coordinates per map.
//
// WHY IT IS NOT A TABLE OF COORDINATES. Over one session, recalled positions
// for real landmarks were wrong three separate times: the Mummy Range summits
// by ~0.05 degrees of latitude, Bear and Sprague Lake by ~300 m, and a "lake
// surface" that turned out to be a flat forested bench. Every time the DEM was
// right. A summit is where the ground is highest and a lake is where it is flat
// and wet, and both of those are things the data already knows.
//
// Names are attached AFTERWARDS and only where the data agrees: a landmark in
// kNamed is matched to the nearest found feature, and a feature no landmark
// reaches is DROPPED (user 2026-09-19: "I want actual mountain/lake names in
// the locate command"). A wrong name is worse than no name, because it is the
// kind of wrong that gets believed -- and a place called `lake3` is worse than
// no place, because there is nothing in the world to recognise it by.
//
// So the two halves of this file answer to different authorities and that is
// deliberate: WHERE a feature is comes from the DEM, and WHAT IT IS CALLED
// comes from USGS GNIS. Neither is allowed to invent the other.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "world/cover.h"
#include "world/dem.h"

class PoiIndex {
  public:
    struct Poi {
        std::string name;
        float x = 0, z = 0;   // world metres
        float m = 0;          // metres above sea level
        bool lake = false;
        bool named = false;    // a real landmark matched it
        float size = 0;       // lake: samples. peak: prominence-ish drop.
        float radiusM = 0;    // lake: how far its own water reaches, REAL metres
        // -- WHAT THIS FEATURE COVERS, IN WORLD METRES --------------------
        //
        // A summit is a point and this is that point. A LAKE IS NOT, and the
        // radius above cannot stand in for its shape: Lake Ouachita is a
        // hundred square kilometres of drowned valley, and the gazetteer's
        // coordinate for it sits six kilometres from the arm this window
        // looks at -- a perfectly good published position that no tolerance
        // hung off a centroid will ever reach. Matching a name against the
        // water's own EXTENT is the honest test, because the question a name
        // answers is "is this that lake", not "is this near the middle of it".
        float x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    };

    // A landmark is only named if a found feature sits within this of it, IN
    // REAL METRES. That unit is the whole point: the constant was being
    // compared against world coordinates, where the dataset is shrunk 6x, so
    // 900 m of tolerance was really 5.4 km of it -- wide enough that Longs and
    // Meeker both reached the same summit and the second one to be checked
    // renamed it. A tolerance quoted in the wrong unit does not fail, it just
    // quietly labels the wrong mountain.
    static constexpr float kNameSnapM = 900.0f;
    // ...AND IT IS NOW A TEST A FEATURE CAN FAIL OUT OF THE INDEX. Every
    // coordinate in kNamed is GNIS, so a name that does not reach anything is
    // saying the window has no such landmark in it -- and the feature that
    // nothing reached is saying it has no name. Both are ordinary answers.

    // How far a summit must see nothing higher, in REAL metres. 1.2 km is about
    // the scale that separates a named peak from its own shoulder in this
    // range; below it, Longs and its east flank are two entries.
    static constexpr float kDomM = 1200.0f;
    // How far a water cell may sit off its component's MEDIAN height and still
    // count as being on the surface, in real metres. This is a definition, not
    // a fudge factor: a lake surface is level, and 2.5 m covers the DEM noise
    // and hydro-flattening error on one.
    static constexpr float kLakeFlatM = 2.5f;
    // ...and how much of the component has to be on that surface. This is what
    // actually separates a lake from a creek or a shadow smear: a lake is
    // almost entirely its own surface, and a thing running downhill is almost
    // entirely not.
    static constexpr float kLakeOnSurface = 0.75f;
    // How much of its own bounding box a lake fills. A lake is a blob and a
    // river is a ribbon, and a slow reach across a flat valley floor holds one
    // height for hundreds of metres, so level alone does not separate them.
    static constexpr float kLakeFillFrac = 0.20f;

    struct Named { const char *name; double lat, lon; bool lake; };

    bool ok() const { return !poi_.empty(); }
    const std::vector<Poi> &all() const { return poi_; }

    // ----------------------------------------------------------------- build
    void build(const DemField &dem, const CoverField &cov) {
        poi_.clear();
        if (!dem.ok()) return;
        const int w = dem.w(), h = dem.h();
        const float sh = dem.shrink();

        // ------------------------------------------------------- the summits
        // Local maxima on a coarse stride, kept apart so one mountain does not
        // become nine entries along its own ridge, AND required to dominate
        // their own surroundings.
        //
        // SEPARATION ALONE IS NOT A SUMMIT TEST, which poi_test caught: the
        // second entry came out 253 world metres from Longs, just past the
        // separation and sitting on Longs' own east flank with the ground still
        // climbing towards the real peak. That is a place the ground is high,
        // not a place it stops going up. So each survivor is also checked
        // against every sample within kDomM of it: if anything there is higher,
        // this is a shoulder and the higher point is the mountain.
        std::vector<Poi> peaks;
        {
            const float cut = dem.minM() + (dem.maxM() - dem.minM()) * 0.72f;
            std::vector<std::pair<float, long>> cand;
            for (int j = 4; j < h - 4; j += 7)
                for (int i = 4; i < w - 4; i += 7) {
                    const float v = sampleAsl(dem, i, j);
                    if (v < cut) continue;
                    cand.push_back({v, long(j) * w + i});
                }
            std::sort(cand.begin(), cand.end(),
                      [](const auto &a, const auto &b) { return a.first > b.first; });
            const float sepW = 1500.0f / sh;     // world metres between summits
            const int domR = std::max(4, int(kDomM / dem.metresPerSampleX()));
            for (const auto &c : cand) {
                const int i = int(c.second % w), j = int(c.second / w);
                float x, z;
                worldOf(dem, i, j, &x, &z);
                // NOT `far`: MSVC still reserves it from the near/far pointer
                // days, and the error it gives ("no variable declared before
                // '='") names neither the word nor the reason.
                bool apart = true;
                for (const Poi &p : peaks)
                    if ((p.x - x) * (p.x - x) + (p.z - z) * (p.z - z) < sepW * sepW) {
                        apart = false;
                        break;
                    }
                if (!apart) continue;
                if (!dominates(dem, i, j, c.first, domR)) continue;
                Poi p;
                p.x = x; p.z = z; p.m = c.first; p.lake = false;
                p.x0 = p.x1 = x; p.z0 = p.z1 = z;   // a summit IS its point
                peaks.push_back(p);
                if (peaks.size() >= 12) break;
            }
        }

        // --------------------------------------------------------- the lakes
        // A LAKE IS A LEVEL SURFACE THAT THE IMAGERY CALLS WATER. Both halves
        // are load-bearing, and the first three versions of this were wrong
        // about one or the other -- poi_test caught each one:
        //
        //   1. It read at(), which jitters its sample point by up to a cell so
        //      the cover does not draw as squares. Right for asking about a
        //      point, wrong for walking a grid: it gives cells neighbours they
        //      do not have, so scattered water chains across a hillside into one
        //      component and the mean of that is nowhere. It reads atGrid() now.
        //
        //   2. It took connectivity as the whole answer. Nine of ten entries had
        //      tens of metres -- one of them 179 -- of relief within their own
        //      shoreline, because misclassified shadow and every creek in the
        //      park are water-coloured and connected.
        //
        //   3. It then measured flatness FROM THE SEED, which is whichever cell
        //      the scan reached first and so usually a shore cell. A tolerance
        //      hung off a bad anchor cuts the real lake in half: at 2.5 m Grand
        //      Lake came apart into pieces and the name landed on the wrong one.
        //
        // What it does instead: fill by connectivity, take the component's
        // MEDIAN height as the surface -- a robust statistic, which a seed is
        // not -- and keep the cells that sit on it. A lake is nearly all such
        // cells. A creek reach, a wet meadow or a shadow smear is not, and is
        // dropped by the fraction test rather than by a threshold that has to be
        // tuned against a particular hillside.
        std::vector<Poi> lakes;
        if (cov.ok()) {
            const int step = 4;
            const int gw = (w + step - 1) / step, gh = (h + step - 1) / step;
            std::vector<uint8_t> wet(size_t(gw) * gh, 0);
            std::vector<float> elev(size_t(gw) * gh, 0.0f);
            for (int j = 0; j < gh; ++j)
                for (int i = 0; i < gw; ++i) {
                    const size_t s = size_t(j) * gw + i;
                    wet[s] = (cov.atGrid(i * step, j * step) == CoverField::Water) ? 1 : 0;
                    if (wet[s]) elev[s] = sampleAsl(dem, i * step, j * step);
                }
            std::vector<uint8_t> seen(wet.size(), 0);
            std::vector<int> stack, cells, flat;
            std::vector<float> hs;
            for (int j0 = 0; j0 < gh; ++j0)
                for (int i0 = 0; i0 < gw; ++i0) {
                    const size_t s0 = size_t(j0) * gw + i0;
                    if (!wet[s0] || seen[s0]) continue;
                    stack.clear();
                    cells.clear();
                    stack.push_back(int(s0));
                    seen[s0] = 1;
                    while (!stack.empty()) {
                        const int s = stack.back();
                        stack.pop_back();
                        cells.push_back(s);
                        const int i = s % gw, j = s / gw;
                        const int di[4] = {1, -1, 0, 0}, dj[4] = {0, 0, 1, -1};
                        for (int d = 0; d < 4; ++d) {
                            const int ni = i + di[d], nj = j + dj[d];
                            if (ni < 0 || nj < 0 || ni >= gw || nj >= gh) continue;
                            const size_t ns = size_t(nj) * gw + ni;
                            if (wet[ns] && !seen[ns]) { seen[ns] = 1; stack.push_back(int(ns)); }
                        }
                    }
                    if (int(cells.size()) < 24) continue;      // a puddle, not a lake

                    hs.clear();
                    for (int s : cells) hs.push_back(elev[size_t(s)]);
                    std::nth_element(hs.begin(), hs.begin() + hs.size() / 2, hs.end());
                    const float surf = hs[hs.size() / 2];

                    flat.clear();
                    for (int s : cells)
                        if (std::fabs(elev[size_t(s)] - surf) <= kLakeFlatM) flat.push_back(s);
                    const int n = int(flat.size());
                    if (n < 24) continue;
                    if (float(n) < float(cells.size()) * kLakeOnSurface) continue;

                    // A LAKE IS A BLOB AND A RIVER IS A RIBBON. Level alone does
                    // not separate them: a slow reach through a flat valley
                    // floor holds one height for hundreds of metres.
                    int ia = gw, ib = 0, ja = gh, jb = 0;
                    double sx = 0, sz = 0;
                    for (int s : flat) {
                        const int i = s % gw, j = s / gw;
                        ia = std::min(ia, i); ib = std::max(ib, i);
                        ja = std::min(ja, j); jb = std::max(jb, j);
                        float x, z;
                        worldOf(dem, i * step, j * step, &x, &z);
                        sx += x; sz += z;
                    }
                    if (double(n) < double(ib - ia + 1) * double(jb - ja + 1) * kLakeFillFrac)
                        continue;

                    // THE POINT IS A MEMBER CELL, NEVER THE CENTROID. A bent or
                    // horseshoe lake has its mean on the bank, and /locate would
                    // put you in the trees looking at the water.
                    const float cx = float(sx / n), cz = float(sz / n);
                    float bx = cx, bz = cz, bd = 1e18f;
                    for (int s : flat) {
                        float x, z;
                        worldOf(dem, (s % gw) * step, (s / gw) * step, &x, &z);
                        const float d = (x - cx) * (x - cx) + (z - cz) * (z - cz);
                        if (d < bd) { bd = d; bx = x; bz = z; }
                    }
                    Poi p;
                    p.x = bx; p.z = bz;
                    p.m = surf; p.lake = true; p.size = float(n);
                    // The water's own footprint, in world metres -- see
                    // Poi::x0. ib/jb are the LAST cell, so the far corner is
                    // one cell further out.
                    worldOf(dem, ia * step, ja * step, &p.x0, &p.z0);
                    worldOf(dem, (ib + 1) * step, (jb + 1) * step, &p.x1, &p.z1);
                    // The radius of a disc of the same area. Used to size the
                    // name tolerance below, and it is the only honest way to
                    // say how big a bent lake is in one number.
                    p.radiusM = std::sqrt(float(n) * float(step * dem.metresPerSampleX()) *
                                          float(step * dem.metresPerSampleX()) / 3.14159265f);
                    lakes.push_back(p);
                }
            std::sort(lakes.begin(), lakes.end(),
                      [](const Poi &a, const Poi &b) { return a.size > b.size; });
            if (lakes.size() > 10) lakes.resize(10);
        }

        poi_ = peaks;
        poi_.insert(poi_.end(), lakes.begin(), lakes.end());

        // =====================================================================
        // ...AND EVERY ONE OF THEM IS A REAL PLACE OR IT IS NOT A PLACE.
        //
        // (user 2026-09-19: "remove all of the environment locations from the
        //  /locate command. I want you to remove the lake2, lake3, and give
        //  real actual names of the location. same thing for the mountains. I
        //  want actual mountain/lake names in the locate command.")
        //
        // There used to be a `peak%d` / `lake%d` fallback here and the menu was
        // eighteen of those against four real names. It is gone: a feature this
        // pass cannot name is ERASED below rather than numbered, so /locate
        // only ever offers somewhere with a name.
        //
        // WHICH MEANT THE GAZETTEER HAD TO STOP BEING RECALLED. The list that
        // stood here was written from memory, and its own note says what that
        // cost -- "recalled coordinates for this park were wrong three separate
        // times in one session". Measured against the data, most of it never
        // matched anything: Mummy Mountain was out by 5.2 km (the entry said
        // 40.4308 N, the mountain is at 40.4752 N), Specimen Mountain by 1.1
        // km, and eleven more named peaks are not in this window's found list
        // at all. Four names of twenty-five attached.
        //
        // EVERY COORDINATE BELOW IS USGS GNIS -- the Domestic Names file for
        // the state, which is the authority the maps are drawn from. Each was
        // then checked the way this file has always asked: matched to the
        // nearest feature the DEM found of the same kind, and the elevation the
        // DEM reports there compared against the published one. Both numbers
        // are in the comments, and every summit but Parika lands within a
        // hundred metres of the peak the data found. That agreement is the
        // check: a name that is right about WHICH mountain agrees about its
        // height as well, and a recalled coordinate has no reason to do either.
        //
        // A NAME WITH NO FEATURE COSTS NOTHING, and most of these will never
        // match -- they are the landmarks of six windows and only one is
        // loaded. A FEATURE WITH NO NAME now costs the whole entry, which is
        // the standard this list has to be good enough for.
        // =====================================================================
        static const Named kNamed[] = {
            // -- rmnp50: Rocky Mountain National Park ---------------------
            // The summits. The first figure is published, `data` is what this
            // DEM reports at the feature the name attached to, and the last is
            // how far apart the two positions are.
            {"longs",        40.2549, -105.6162, false},  // 4346 m, data 4343, 81 m
            {"mummy",        40.4752, -105.6236, false},  // 4092 m, data 4086, 14 m
            {"audubon",      40.0990, -105.6163, false},  // 4030 m, data 4026, 37 m
            {"isolation",    40.2023, -105.6777, false},  // 3998 m, data 3989, 32 m
            {"ida",          40.3718, -105.7794, false},  // 3926 m, data 3922, 48 m
            {"howard",       40.4271, -105.8989, false},  // 3904 m, data 3907, 27 m
            {"cumulus",      40.4103, -105.9023, false},  // 3878 m, data 3878, 82 m
            {"sprague",      40.3466, -105.7364, false},  // 3875 m, data 3871, 99 m
            {"lead",         40.4486, -105.8971, false},  // 3821 m, data 3818,  6 m
            {"bowen",        40.3605, -105.9334, false},  // 3817 m, data 3815, 12 m
            {"specimen",     40.4446, -105.8085, false},  // 3807 m, data 3802, 32 m
            {"parika",       40.3851, -105.9466, false},  // 3778 m, data 3778, 190 m
            // ...and the water.
            {"granby",       40.1555, -105.8484, true},   // Lake Granby
            {"shadow",       40.2275, -105.8425, true},   // Shadow Mountain Lake
            {"grand",        40.2436, -105.8147, true},   // Grand Lake
            {"willowcreek",  40.1472, -105.9502, true},   // Willow Creek Reservoir
            {"longdraw",     40.4991, -105.7866, true},   // Long Draw Reservoir
            {"monarch",      40.1057, -105.7418, true},   // Monarch Lake
            {"estes",        40.3764, -105.4965, true},   // Lake Estes
            {"beaver",       40.1171, -105.5232, true},   // Beaver Reservoir
            // -- acadia10: Mount Desert Island, the birch wood ------------
            {"sargent",      44.3429,  -68.2732, false},  // Sargent Mountain, 39 m
            {"eagle",        44.3635,  -68.2502, true},   // Eagle Lake
            {"jordan",       44.3314,  -68.2553, true},   // Jordan Pond
            {"bubble",       44.3446,  -68.2390, true},   // Bubble Pond
            {"auntbetty",    44.3702,  -68.2749, true},   // Aunt Betty Pond
            {"upperhadlock", 44.3216,  -68.2876, true},   // Upper Hadlock Pond
            {"lowerhadlock", 44.3108,  -68.2895, true},   // Lower Hadlock Pond
            // -- ouachita12: the oak wood --------------------------------
            //
            // ONE NAME, AND IT IS THE RESERVOIR. Every water body this window
            // finds sits at 174 m: they are arms of Lake Ouachita, cut into
            // separate components by the ridges between them, and the published
            // point is 6 km from the nearest of them -- which is why a name has
            // to be matched against the water's EXTENT as well as its middle.
            // See Poi::x0. The window's own high point is an unnamed ridge
            // crest with nothing in GNIS within 2.6 km of it, so it is dropped
            // rather than called after a summit two valleys away.
            {"ouachita",     34.6065,  -93.3478, true},   // Lake Ouachita
            // -- elbert40: the Sawatch ------------------------------------
            {"elbert",       39.1179, -106.4453, false},  // Mount Elbert, 11 m
            {"oxford",       38.9648, -106.3388, false},  // Mount Oxford, 18 m
            {"huron",        38.9455, -106.4381, false},  // Huron Peak, 24 m
            {"grizzly",      39.0425, -106.5975, false},  // Grizzly Peak, 65 m
            {"casco",        39.1141, -106.4938, false},  // Casco Peak, 11 m
            {"oklahoma",     39.1786, -106.5062, false},  // Mount Oklahoma, 19 m
            {"deer",         39.1575, -106.5212, false},  // Deer Mountain, 7 m
            {"blaurock",     39.0113, -106.4537, false},  // Mount Blaurock, 24 m
            // -- front60 and cheesman30: the Front Range ------------------
            {"bison",        39.2385, -105.4979, false},  // Bison Mountain, 15 m
            {"sheep",        38.7930, -105.0545, false},  // Sheep Mountain, 33 m
            {"windy",        39.3023, -105.4398, false},  // Windy Peak, 13 m
            {"buffalo",      39.2755, -105.3679, false},  // Buffalo Peak, 70 m
            {"green",        39.3053, -105.3002, false},  // Green Mountain, 41 m
            {"cheesman",     39.1955, -105.2846, true},   // Cheesman Lake
            {"rampart",      38.9808, -104.9700, true},   // Rampart Reservoir
            {"lakegeorge",   38.9787, -105.3641, true},   // Lake George
            {"wrights",      38.7996, -105.2713, true},   // Wrights Reservoir
            {"wilson",       38.8166, -105.0710, true},   // Wilson Reservoir
        };
        // ONE NAME PER FEATURE, NEAREST CLAIM WINS. Two landmarks a kilometre
        // apart can both be closest to the same found summit, and a loop that
        // just assigns leaves whichever was checked last -- so Longs answered to
        // "meeker". Every pairing inside tolerance is scored first and they are
        // settled in order of distance, which makes the result independent of
        // the order kNamed happens to be written in.
        //
        // TWO WAYS TO CLAIM, AND THE SECOND IS WHAT A BIG LAKE NEEDS.
        //
        //   * WITHIN REACH OF THE POINT -- kNameSnapM plus the feature's own
        //     radius. A summit is a point and 900 m round it is generous; a
        //     small lake keeps a small tolerance because its radius is small,
        //     which is what makes this safe rather than merely looser.
        //
        //   * ...OR STANDING ON THE WATER ITSELF. A published coordinate for a
        //     reservoir is one point on a shape tens of kilometres long and it
        //     is under no obligation to sit near the arm a window happens to
        //     hold: Lake Ouachita's is 6 km from the nearest of the eight
        //     components this engine finds of it, and Lake Granby's is 2.4 km
        //     from its own. Inside a component's footprint IS that lake
        //     whatever the distance to the piece's middle -- and a footprint
        //     belongs to one component, so no two can swallow the same point.
        //
        // Both still rank by distance, so where several arms of one reservoir
        // qualify the name goes to the one whose middle is nearest the
        // published position and the rest stay unnamed. Which is right: the
        // name belongs to the lake, and the lake is one of them.
        struct Claim { float d; float size; int poi; int name; };
        std::vector<Claim> claims;
        for (int k = 0; k < int(sizeof kNamed / sizeof kNamed[0]); ++k) {
            float tx = 0, tz = 0;
            dem.worldOf(kNamed[k].lon, kNamed[k].lat, &tx, &tz);
            for (int i = 0; i < int(poi_.size()); ++i) {
                const Poi &q = poi_[i];
                if (q.lake != kNamed[k].lake) continue;
                // HOW FAR OUTSIDE THIS FEATURE THE PUBLISHED POINT FALLS, which
                // for a summit is simply how far away it is (its box is the
                // point) and for a lake is ZERO anywhere on the water. That is
                // the measure that settles the arms -- see the note above.
                const float bx = tx < q.x0 ? q.x0 - tx : (tx > q.x1 ? tx - q.x1 : 0.0f);
                const float bz = tz < q.z0 ? q.z0 - tz : (tz > q.z1 ? tz - q.z1 : 0.0f);
                const float box = bx * bx + bz * bz;
                const float snapW = (kNameSnapM + q.radiusM) / sh;
                const float mid = (q.x - tx) * (q.x - tx) + (q.z - tz) * (q.z - tz);
                if (box <= 0.0f || mid < snapW * snapW)
                    claims.push_back({box, q.size, i, k});
            }
        }
        // ...AND WHERE TWO PIECES BOTH HOLD IT, THE BIGGER ONE IS THE LAKE.
        // Eight arms of Lake Ouachita all qualify and all of them ARE Lake
        // Ouachita; the name belongs on the one with the shore, the fish and
        // the room to swim, not on whichever 64-cell inlet the coordinate
        // happened to land nearest.
        std::sort(claims.begin(), claims.end(), [](const Claim &a, const Claim &b) {
            return a.d != b.d ? a.d < b.d : a.size > b.size;
        });
        std::vector<uint8_t> nameTaken(sizeof kNamed / sizeof kNamed[0], 0);
        for (const Claim &c : claims) {
            if (poi_[c.poi].named || nameTaken[size_t(c.name)]) continue;
            poi_[c.poi].name = kNamed[c.name].name;
            poi_[c.poi].named = true;
            nameTaken[size_t(c.name)] = 1;
        }

        // -- AND THE SEA IS A PLACE WITHOUT BEING IN ANY GAZETTEER ---------
        //
        // The Acadia window is an ISLAND, and the biggest body of water in it
        // is the one all round it: 2.8 km across the part this window holds,
        // with a surface the data puts at sea level. GNIS names the coves and
        // the sound, not the ocean, so nothing in the list above can claim it
        // and the erase below would take it -- the one destination in that
        // world with fish, ducks, lily pads and dragonflies in it.
        //
        // ITS OWN MEDIAN SURFACE IS THE EVIDENCE, which is how everything else
        // in this file is decided. Anything at or below the datum is the sea; a
        // lake in the mountains is two kilometres above it.
        for (Poi &p : poi_)
            if (p.lake && !p.named && p.m <= 1.0f) {
                p.name = "sea";
                p.named = true;
                break;   // one window, one sea -- and the list is biggest first
            }

        // -- ...AND WHAT IS LEFT OVER IS NOT A PLACE ----------------------
        //
        // The erase that makes the rule at the top of this block true. A summit
        // the data found on an unnamed ridge, and an arm of a reservoir whose
        // name is already on another arm, are both real features; neither is
        // somewhere a player can be sent BY NAME, which is all /locate does.
        poi_.erase(std::remove_if(poi_.begin(), poi_.end(),
                                  [](const Poi &p) { return !p.named; }),
                   poi_.end());
    }

    int namedCount() const {
        int n = 0;
        for (const Poi &p : poi_) n += p.named ? 1 : 0;
        return n;
    }

    // EXACT FIRST, THEN A PREFIX. "/locate longs" is what the tab
    // completion hands you, so exact is the common path; the prefix pass is
    // for a half-typed name entered without pressing Tab, and it only
    // answers when ONE place starts that way -- an ambiguous stem is a miss,
    // because teleporting to whichever of two peaks sorted first is worse
    // than saying no.
    const Poi *find(const std::string &q) const {
        if (q.empty()) return nullptr;
        for (const Poi &p : poi_)
            if (p.name == q) return &p;
        const Poi *hit = nullptr;
        for (const Poi &p : poi_) {
            if (p.name.size() < q.size() || p.name.compare(0, q.size(), q) != 0) continue;
            if (hit) return nullptr;
            hit = &p;
        }
        return hit;
    }

    // The listing /locate prints with no argument.
    std::string menu() const {
        if (poi_.empty()) return std::string();
        std::string s = "  places (from the loaded elevation data):\n";
        char b[160];
        for (const Poi &p : poi_) {
            if (p.lake)
                std::snprintf(b, sizeof b, "    %-13s lake  %4.0f m asl, %.1f km across\n",
                              p.name.c_str(), p.m, p.radiusM * 2.0f / 1000.0f);
            else
                std::snprintf(b, sizeof b, "    %-13s peak  %4.0f m asl\n", p.name.c_str(), p.m);
            s += b;
        }
        return s;
    }

  private:
    std::vector<Poi> poi_;

    // DemField exposes heightM in WORLD metres with the datum removed; these
    // helpers go the other way for reporting, and index the grid directly.
    static void worldOf(const DemField &d, int i, int j, float *x, float *z) {
        *x = float((double(i) - d.w() * 0.5 + 0.5) * d.metresPerSampleX() / d.shrink());
        *z = float((double(j) - d.h() * 0.5 + 0.5) * d.metresPerSampleX() / d.shrink());
    }
    // Is (i,j) the high point of its own neighbourhood? Sampled on a coarse
    // stride: a peak that loses by less than the stride misses is not a peak
    // either way, and the full disc at a 10 m posting is 45k lookups a
    // candidate.
    static bool dominates(const DemField &d, int i, int j, float m, int r) {
        const int stride = std::max(1, r / 24);
        for (int dj = -r; dj <= r; dj += stride)
            for (int di = -r; di <= r; di += stride) {
                if (di * di + dj * dj > r * r) continue;
                const int ii = i + di, jj = j + dj;
                if (ii < 0 || jj < 0 || ii >= d.w() || jj >= d.h()) continue;
                if (sampleAsl(d, ii, jj) > m + 0.5f) return false;
            }
        return true;
    }

    static float sampleAsl(const DemField &d, int i, int j) {
        float x, z;
        worldOf(d, i, j, &x, &z);
        return d.worldToAsl(d.heightM(x, z));
    }
};
