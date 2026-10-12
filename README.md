# BetterCheats -- StarRupture Plugin

A modular in-game cheat menu for [StarRupture](https://store.steampowered.com/app/1631270/StarRupture/), built on the StarRupture ModLoader. Provides a categorized ImGui panel for player, world, machine, and enemy cheats, with per-save persistence of your settings.

**Target:** Game client only

---

## What It Does

BetterCheats registers an in-game ImGui panel with a sidebar of cheat categories. The menu only appears during a single-player `ChimeraMain` world session -- it stays hidden on the main menu and other game modes.

| Category | Sub-tabs | Examples |
|---|---|---|
| World | Environment | World/environment manipulation, wave control |
| Player | Self, Item Spawner, Inventory, Weapon, Movement, Teleport, Building, Skills, Tools | God mode / attribute locks, spawn items, resize the inventory grid, weapon tweaks, no-clip flight, teleport, free building, skill unlocks, tool tweaks |
| Machinery | Crafters, Power, Logistic Drones, Rail Drones | Instant crafting, infinite machine power, drone control |
| Enemies | Enemies | Enemy-related cheats |
| Misc | -- | Miscellaneous cheats |

Continuous effects (e.g. attribute locks, power overrides, wave control) are enforced every engine tick via `OnEngineTick`, so they keep applying even while the menu is closed.

### No Clip

**Player -> Movement** turns the character's capsule collision off and puts it into flying mode, so you pass through terrain and buildings. Steer as usual to fly horizontally, hold **Space** to rise and **Left Ctrl** to descend, and adjust the fly speed multiplier in the panel. It also has its own keybind (**F9** by default) so it can be toggled without opening the menu -- click the key next to **Toggle No Clip** in the panel to rebind it, holding Ctrl, Shift or Alt for a combo.

### Inventory grid

**Player -> Inventory** sets the player inventory grid to any columns x rows you want, down to the game's own minimum of 8 x 8.

The inventory frame doesn't scroll, so a taller grid just runs off the bottom of it. **Shrink slots to fit the inventory frame** scales every slot widget down against that 8 x 8 reference: 8 x 8 draws at 100%, 16 x 16 at 50%, 20 x 20 at 40%. The same factor applies to both axes, so slots stay square and a wide, short grid leaves space below it rather than stretching. Slots stop shrinking at 40%; past 20 x 20 the grid overflows rather than becoming unreadable.

The grid and the fit setting are saved per session and re-applied on load. The same resize is available from the mod console as `bc_invsize <columns> <rows>`.

The game refuses to shrink a grid below the slots that are currently in use, so empty the inventory out first if a smaller grid doesn't take.

### Building

**Player -> Building** lists every option with a short description under its title. All of them are off by default and saved per session.

| Option | What it does |
|---|---|
| **No Build Cost** | Place buildings without having the resources for them. |
| **No Stability Check** | Place buildings the stability check would otherwise refuse. |
| **Deconstruct Windows During Waves** | Lets a window piece (any of the four viewport pieces) be deconstructed while a wave has heated it. Other heated buildings stay protected. |
| **Build Windows Anywhere** | Puts a window into a habitat or Hub wall slot even when a machine or another building is in it. |
| **Build Benches Anywhere** | Puts a habitat machine into a habitat or Hub wall slot even when a window or another building is in it. |

The **Research** table below it has **Unlock All Buildings** and **Unlock All Recipes**. They warn that the changes may end up in your save file even though care is taken to avoid it.

**Build Windows Anywhere** and **Build Benches Anywhere** only change the game's collision checks for the piece you are placing. A window or machine that snaps to a habitat or the Hub counts as valid even when a building overlaps it, so the ghost goes green instead of red. Still refused:

- a player or a drone standing in the piece's collision box;
- a window that is not snapped to a habitat or the Hub (free-standing windows, or windows on open ground);
- a window into a slot that already holds a window, airlock or bridge;
- anything the game checks separately from collision: stability (unless **No Stability Check** is on), base-core areas, terrain, water and resources.

The benches are the Analyzer, Armory, Cloning Bed, Food Processor, Item Printer, Recipe Table, Recycler, Research Terminal, Suit Workshop and Upgrade Station. Because the game only records that a slot is taken, not by what, **Build Benches Anywhere** also lets a bench into a slot that holds a window, an airlock, a bridge or personal storage.

The placing player decides placement, so both options cover your own placements only, and do nothing for other players. In a multiplayer session they need `EnableCheatsInMultiplayer` (see [Configuration](#configuration)) before the menu opens at all.

### No Handheld Drill Overheat

**Player -> Tools -> Mining -> No Handheld Drill Overheat** stops the handheld drill from heating up, in single player. While it is on, BetterCheats keeps the game's own `Cheat.UnlimitedWeaponHeat` gameplay tag on your character, the tag the game's built-in unlimited-heat cheat sets, and the game's heat logic stops adding heat while the tag is there.

Switching the option on or off takes effect on the next tick, and a new character after a respawn or travel is tagged within a second. A drill that is already hot cools down normally instead of being cleared. BetterCheats only ever removes a tag it added itself: if the game's own cheat already holds the tag, it leaves it alone. The tag comes off when you switch the option off, when the world ends and when the plugin unloads.

### Grenade damage scale

Mod console command `bc_grenadedmg [scale]` multiplies the equipped grenade's base damage, to test whether its explosion reads that value. With no argument it prints the current scale. The scale is a number from 0 to 100, `1` (the default) leaves the grenade alone, and it applies while a grenade is equipped. It is restored when you switch grenades, on unload and on RELOAD, and it is not saved between launches. The **Grenade** tab keeps showing the game's own value, not the scaled one. Each change is written to `ModLoader.log` as a `GrenadeDamageProbe:` line, with the game's value, the expected value and the value read back a tick later.

### Per-save settings

Cheat toggles that should persist are saved to a JSON file per save session (`Plugins\BetterCheats\<SessionName>.json`). When a save finishes loading, BetterCheats reloads that session's config and re-applies any saved cheat states automatically -- including on plugin hot-reload into an already-active session.

---

## Configuration

Config is stored in `Plugins\config\BetterCheats.ini` and is generated on first launch.

| Section | Key | Default | Description |
|---|---|---|---|
| `General` | `Enabled` | `true` | `true` or `false` -- enables the plugin |
| `General` | `EnableCheatsInMultiplayer` | `false` | `true` or `false` -- lets the menu open in a multiplayer session. Cheats are not supported in multiplayer and may cause crashes |
| `Menu` | `ToggleKey` | `F10` | Key to open / close the BetterCheats menu |
| `Keybinds` | `NoClipKey` | `F9` | Key to toggle No Clip on / off without opening the menu |

Both keys can be rebound at runtime via the modloader's keybind settings, and the No Clip key can also be rebound from the **Player -> Movement** panel -- either way the change takes effect immediately and is written back to the .ini.

---

## Installation

1. Download the latest release ZIP from the [Releases](../../releases) page: `BetterCheats_Plugin-Client-*.zip`

2. Extract into your game's `Binaries\Win64\` folder. The ZIP contains a `Plugins\` folder -- it will sit alongside your existing `dwmapi.dll`.

3. Launch the game, then press **F10** (default) in-game to open the menu.

> **Requires [StarRupture-ModLoader](https://github.com/AlienXAXS/StarRupture-ModLoader)** to be installed first.

---

## Troubleshooting

| Problem | Solution |
|---|---|
| Menu doesn't appear | The menu only shows in a single-player world session. Confirm you're loaded into a save, and that `Enabled=true` in `BetterCheats.ini`. |
| Toggle key does nothing | Check the keybind in the modloader settings -- it may have been rebound. |
| Saved cheat settings not restored | Check `modloader.log` for errors loading `Plugins\BetterCheats\<SessionName>.json`. |
| Plugin not loading | Check `modloader.log` in `Binaries\Win64\` for errors. |
| Game updated, plugin broken | Some features use byte-pattern scanning. A game update may shift the patterns -- wait for a plugin update. |
| A Building option does nothing | **Deconstruct Windows During Waves**, **Build Windows Anywhere** and **Build Benches Anywhere** each need a byte pattern to match. If one misses after a game update, the plugin still loads, the option does nothing, and `modloader.log` has a line saying which one. |

---

## Building from Source

Requires Visual Studio 2022 and the [StarRupture-Plugin-SDK](https://github.com/AlienXAXS/StarRupture-Plugin-SDK) and [StarRupture-Game-SDK](https://github.com/AlienXAXS/StarRupture-Game-SDK), checked out alongside this repo.

Clone the repo, open `StarRupture-Plugin-BetterCheats.sln`, and build the `Client Release|x64` configuration:

```
msbuild StarRupture-Plugin-BetterCheats.sln /p:Configuration="Client Release" /p:Platform=x64
```

The output DLL is placed in `build\Client Release\Plugins\BetterCheats.dll`.

---

## Disclaimer

This is a single-player cheat menu intended for personal use. Use at your own risk -- the authors are not responsible for any damage caused by using this software, including loss of save data or issues caused by using cheats in multiplayer/server contexts.

---

## Credits

Additional features by @msantoro12, built with Claude Code.
