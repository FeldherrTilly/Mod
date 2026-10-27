# Fantasy New World map generator

Generates `PrivateMaps/Fantasy New World Large.ColonizationWBSave` (80x110, world size HUGE for 18 players / 12 natives, 70°N–40°S, Europe east and west) plus a PNG preview. Python 3, standard library only.

```
python Tools/MapGenerator/gen.py [seed] [output dir]      # default: seed 1, Tools/MapGenerator/out
python Tools/MapGenerator/validate.py <file.ColonizationWBSave>
python Tools/MapGenerator/crop.py x0 x1 y0 y1 crop.png [preview.png]   # zoomed preview section
```

Copy the generated `.ColonizationWBSave` to `PrivateMaps/` and restart Colonization completely to see the change. The map text key is `TXT_KEY_WORLDBUILDER_FANTASY_NEW_WORLD_DESC`.

## Pipeline (`gen.py`)

1. Landmasses from designed polylines plus noise, coastline details (fjords, skerries, volcanic island of ~12 tiles with a stream, Caribbean island world of ~10-tile islands (at least two water tiles apart), climate-matching islands off other coasts, barrier islands with a shallow lagoon on the east coast, atolls).
2. Arctic archipelago in the far north with a winding, ice-free strait (Magellan-like) from the Atlantic to the Pacific. Mountain ranges with passes (plus low worn-down chains in the eastern settlement lands), hidden high valley, salt basin arcs, small inland massifs, sparse scattered hills, irregular mountain flanks, open altiplano floor.
3. Lakes (designed, glacial lake district, finger lakes, rainforest lakes, salt lake, altiplano lake).
4. Climate: temperature by latitude and altitude, precipitation by prevailing winds with orographic rain and rain shadows.
5. Drainage: large rivers (plot water, ruler-straight paths rejected) and edge rivers (corner flow network); afterwards an estuary (drowned lower course of an eastern river, deep main channel, shallow side arms), two large natural harbours and ~10 small irregular coves (3-5 water tiles; shallow, partly shallow or deep) are carved, away from large river mouths. Three more coves on the Atlantic (east) coast of the southern continent are carved at the very end, before the landmark names, with their own random stream so the rest of the map stays unchanged.
6. Biomes (fertile plains where grassland meets prairie), set pieces (deltas, green river valleys, oases, salt basin, altiplano, canyon, mesas, river island, gorge), clay hills on marsh/wetland, terrain smoothing, scattered lone hills in the open lowlands (several resources need hills).
7. Connectivity for land units (passes, fords) and deep-water lanes for large ships.
8. Features: forests by biome density, hardwood clusters (dense forest / ancient forest, not in jungle or savanna), a cluster of city ruins with fragments of winding, overgrown roads in the southern jungle (plus single ruins on the altiplano and in the isthmus), special features, meteor crater in the isthmus jungle, volcanoes, reefs, currents; Europe plots: Atlantic (EUROPE_EAST) and Pacific (EUROPE_WEST), both at least 5 tiles from land.
9. Southern extension: ten extra rows appended at the end (so the rest of the map is unaffected) - a cape behind a dense coastal cordillera with a coastal desert in front, a few islands, the tip of a polar continent (taiga and forest on its northern shore, room for two towns) reaching in from the bottom edge and an open southern passage to the Pacific between them.
9b. Caribbean revision (late, own random stream): the westernmost gulf island is replaced by a narrow strip of five Atlantic Europe plots, about three quarters of the rock reefs (FEATURE_STONE_REEF) of the island sea are removed (coral and pearl reefs stay), a narrow interrupted ocean current runs as a half circle from the east ocean through the island sea to the strip (arrows only on open ocean; plain deep coast without features is opened up where the arc crosses it), and about 30 % of the shrubland in the southern continent turns into desert, nearest to existing desert first (oases, river valleys and the cape in the far south are left alone).
10. Landmark names: map signs with realistic-sounding names (visible to all players, ASCII captions) for set pieces, highest summits, mountain chains, large rivers, lakes and large flat woodland regions (one sign per feature, at least 5 tiles apart); white dots in the preview.
11. Checks that every set piece has a usable city site (>= 6 workable land tiles in the 3x3 ring, >= 10 in 5x5 without corners).

## Engine notes

- Peaks and large rivers are impassable for most land units; fords (`FEATURE_RIVER_FORD`) and passes keep regions connected.
- `FEATURE_VOLCANO` must be on flat land; on a peak the model is not visible.
- Shallow coast blocks large ships.
- Water regions below 50 plots without Europe become lakes when the map is loaded; lakes next to snow become ice lakes.
- Clay comes from hills on marsh, wetland or tundra only.
- The map uses `Randomize Features=false` (forests are placed by the generator); resources and goodies are randomized by the game.

## Flipped Europe (`gen_eporue.py`)

Second map "Eporue": Europe turned by 180 degrees (Atlantic in the east = Europe side, Scandinavia in the south, Mediterranean with a strip of North Africa in the north, Russia and Asia in the west), 107x125, world size HUGE, same style rules as above. Real coastlines are drawn in lon/lat and projected piecewise (`LON_KN`, `LAT_KN`: west and central Europe get the most tiles per degree); the climate is a free, slightly warmer variant (`LAT_SHIFT`). The east of Russia and Ukraine is replaced by an eastern sea with an island world (a volcanic island with a stream and a reef ring, settleable islands, shallow coast around them). Pipeline: land mask with fractal coastline and tile-scale ragged coast, seas and straits, ranges with passes (wavy, plus low broken chains, massifs and lone hills in the plains), lakes, large rivers with fords, climate, biomes, clay hills, features, volcanoes, canyon, mesas and gorge, edge rivers (small rivers, none across desert), natural harbours, coves and barrier islands with a lagoon, hardwood clusters, coastal water (shallow coast in the North Sea, Baltic, Adriatic, Persian Gulf, ...) with deep-water lanes, reefs and kelp (no currents), Europe plots, four Pacific columns on the left, map signs with real names, resources, native villages, WBSave writer, ASCII preview and PNG.

- Europe plots (`europe_zone`): open sea at least 8 tiles from land (5 in the North Sea and off Norway), joined to the east edge; in bays the zone takes the middle of the bay until the land of every open Atlantic coast is at most 8 tiles from it. Narrow straits (Gibraltar, Skagerrak, Dover) keep it out of the enclosed seas.
- Mediterranean tropics (late pass): the warm shores of the Mediterranean become a jungle belt (savannah or marsh with jungle and tropical groves, mangroves on wet coasts), behind it a ragged belt of savannah; peaks, the Sahara away from the coast, set pieces and special features stay.
- Resources are set by the generator (`Randomize Resources=false`): the rules of the game are read from `CIV4BonusInfos.xml` (terrain, feature, hills/flat/peaks, river side, coast, latitude = real latitude of the plot, whale spacing), the amount follows the game's formula (80 % on land; gold, silver and gems only 30-40 %, `PRECIOUS`). Order: first one resource at each real deposit of Europe (`DEPOSITS`, precious metals first: silver in the Harz, Erzgebirge, Kutna Hora, Schwaz, Sierra Morena and Kongsberg, gold in Transylvania, Kremnica, Las Medulas and the Tauern, coal in the Ruhr, iron at Kiruna, wine around Bordeaux, ...), then the rest by weight: a shared rich/poor noise (`RICHNESS`), large-scale noise per resource, extra weight near the deposits, small groups of one resource. Two different resources are normally not adjacent; about 10 % of the placements may touch another resource (`NEIGHBOUR_SHARE`). Gold, silver and gems lie mostly in the hills, seldom on peaks. Summit models: most remarkable mountains are the coal kind, only about 7 % each the gold and the silver kind.
- The map is a scenario with all players set (`eporue_players.txt`, taken from "The New World Gigantic - Historic"): 8 colonial powers start in the Europe zone off their own coast, 31 native tribes live in homelands that fit their way of life (`TRIBES`: Inuit in Lapland, Sioux in the Hungarian plain, Apache in the Sahara, Inca in the High Atlas, Arawak on Sardinia, Caribs on the islands of the eastern sea, ...). Tribal land grows from each home across the terrain, the villages (about 190, population 2-4, two or three braves each) are set by site quality and way of life. Native tribes in contact are neighbours. Game options and speed are fixed in `GAME_HEADER`; goodies stay random.

```
python Tools/MapGenerator/gen_eporue.py [seed] [output dir]    # writes Eporue Massive.ColonizationWBSave, ascii_terrain.txt, ascii_features.txt, preview.png
python Tools/MapGenerator/validate.py "<output dir>/Eporue Massive.ColonizationWBSave"
```

The generated file is also stored as `PrivateMaps/Eporue Massive.ColonizationWBSave`; the map text key is `TXT_KEY_WORLDBUILDER_EPORUE_DESC`.
