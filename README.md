# Human AI for Hearts of Iron IV

Hand parts of your country to the game's own AI and keep the rest. For example, let the AI fight
the war on land, in the air and at sea while you run diplomacy and the national focus tree.
Switch any area back mid-game.

This isn't a scripted imitation. It runs Paradox's real country AI for the areas you choose, the
same AI that runs every other nation.

| Area | What the AI takes over |
|---|---|
| Diplomacy | Guarantees, factions, wars, lend-lease |
| Politics | National focus, laws, advisors, scientists |
| Economy | Trade, garrisons, equipment and ship designs |
| Land forces | Fronts, armies, army groups, invasions |
| Air force | Air bases and air missions |
| Navy | Naval objectives and fleets |
| Volunteers | Volunteer forces sent abroad |
| Cryptology | Code breaking |

## Using it

There are two ways to choose who controls what. Use either or both.

**Settings window.** Start the game as usual, and a small **Human AI** window opens next to it
without taking focus. Click **You** next to an area to hand it to the AI, and click **AI** to take
it back. Changes apply within a second.
- It stays on top of a borderless-fullscreen game. You can turn that off in the window.
- It closes by itself when the game exits.
- Its choices are kept in `human_ai.ini` in the game folder.

**In-game panel (optional mod).** With the **Human AI** mod in your playset, a **Human AI** button
sits in the bottom-left corner of the map. It opens the same controls in the game's own UI.
- The panel's choices are **saved with each campaign**.
- Loading a save, or opening the panel, applies that campaign's choices. The settings window
  updates to match.

## AI packs (optional)

Want your AI to be better than vanilla? Drop any AI mod's folder (for example a Steam Workshop AI
mod) into `human_ai_packs` in the game folder. The **Folder** button in the window opens it. Then
pick it under **AI pack**.

What Human AI takes from a pack:

| From the pack | When | Notes |
|---|---|---|
| AI settings (`NAI`/`NAITheatre` defines, including arrays) | when a game loads, and live when you switch packs | **None** restores the game's own values |
| AI strategies (`ai_strategy`, `ai_strategy_plans`) | at game start | **single-player only**; restart the game to change |
| AI division templates (`ai_templates`) | at game start | **single-player only**; restart the game to change |
| AI equipment designs (`ai_equipment`) and AI areas (`ai_areas`) | at game start | **single-player only**; restart the game to change |

- You don't enable the mod in the launcher, so your multiplayer checksum stays vanilla. The pack's
  files are read from outside the game's `common/` folder.
- Everything else in the pack is ignored on purpose: combat and supply settings, production buffs,
  events, scripted effects and modifiers. One-sided, they would desync a multiplayer game.
- **Who it affects:** in single-player, and when you host, the game runs every AI nation on your PC,
  so they use the pack too. As a multiplayer client, only your own country's AI runs on your PC.
- **Scripted triggers:** AI files that call the pack's own scripted triggers work. Human AI expands
  those calls inline into a prepared copy (`<pack>\.hai\`) instead of adding triggers to the game.
- Strategies that depend on flags the pack's own scripts would set do nothing, because those
  scripts aren't run. For Frontline AI that's its crisis-detection strategies.
- **Combining mods:** one pack can hold several AI mods. Copy them into the same folder, as long as
  their file names don't clash. Example: *Better Mechanics: Frontline AI*, *AI Divisions* and
  *AI Designs* together.
- **Strategies, templates and designs are single-player only.** The game evaluates AI strategies on
  every PC, and their values are part of the synced game state, so loading a pack's strategies on
  one PC desyncs a multiplayer game (tested: it changes the "AI Strategy" and "Supply Cache"
  checksums even with the AI switched off). Untick **Also its strategies, templates and designs**
  in the window (`pack_data=0`) and restart before playing online. The window warns you if you
  forget.
- **In multiplayer** (host or client), only AI settings that the AI alone reads are applied. Some
  `NAI` settings are also read by the game's simulation: for example
  `DEFAULT_SUPPLY_TRAIN_NEED_FACTOR` is read by the monthly update, and changing it on one PC
  desynced the supply cache. Those are skipped, and the log counts them as "multiplayer-unsafe".

## Testing multiplayer on your own

You can check for desync causes without a second player:
1. Set `mp_test=1` in `human_ai.ini` and load a **single-player** save. Your country's AI now runs
   exactly as it would on a multiplayer client, but every command it posts is dropped, so nothing it
   decides takes effect.
2. With every area set to **You**, unpause for one in-game week, pause, and type `dump_checksum` in
   the console (press `` ` `` to open it).
3. Load the same save again, set the areas you want to test to **AI** (and your pack), run the same
   week, pause, and type `compare_to_last_checksum`.

If the checksums match, your AI has no hidden side effects, and it should stay in sync online. If
they differ, the console names the part of the game that changed (such as the supply cache): send
that and `human_ai.log`. To rule out ordinary single-player randomness, run step 3 once with every
area still on **You** as well. If that already differs, compare against that run instead.

## Install

Hearts of Iron IV **1.19.x**, Windows, single-player.

1. Download `human_ai-<version>.zip` from [Releases](../../releases).
2. Copy `version.dll` and `HumanAI.exe` from `Hearts of Iron IV (game folder)` next to `hoi4.exe`.
   In Steam, right-click the game, then choose *Manage* → *Browse local files*.

   **This step is required.** It's what makes the mod work.
3. Optional, for the in-game panel: subscribe on the Steam Workshop, or copy the contents of
   `Documents/Paradox Interactive/Hearts of Iron IV/mod/` into your own
   `Documents\Paradox Interactive\Hearts of Iron IV\mod\`. Then add **Human AI** to your playset.

Steam Workshop can't install `version.dll`, so the Workshop mod on its own only shows the panel.
It does nothing until step 2 is done.

To uninstall, delete `version.dll` and `HumanAI.exe` (plus `human_ai.ini`, `human_ai_status.ini`
and `human_ai.log` if you want a clean folder), and remove the mod from your playset.

To stop the window opening by itself, set `launch_app=0` in `human_ai.ini`. You can still edit that
file by hand: `1` means the AI runs that area for you.

## How it works

HoI4's country AI is built from separate modules (foreign minister, political minister, generals
and so on), and the game can already run them for human players through the `human_ai` debug
command, though only all at once. `version.dll` loads with the game, turns that switch on, and
puts a filter in front of each module's update so that only the areas you picked run for your
country. AI nations are never affected.

Everything is located at startup from patterns in the game's code, not fixed addresses. If a game
update changes something it relies on, the mod writes the reason to `human_ai.log`, the window says
so, and the game stays vanilla. For the reverse-engineering notes and offsets, see
[native/README.md](native/README.md).

`HumanAI.exe` is a plain settings window. It reads and writes `human_ai.ini` and never touches the
game process.

## Known limitations

- **Single-player only.** It will desync multiplayer games.
- **Game updates can break it.** When that happens it switches itself off until it's updated.
- **The split isn't perfect.**
  - While any area is handed over, the game may also answer a few prompts for you that don't
    belong to a module, such as event options and some diplomatic replies.
  - Naval invasions are planned by the land AI.
- Tested on the DirectX 11 build of the game on Windows 11.

## Building

Needs Visual Studio with the C++ x64 tools and Python 3. The settings window is built with the C#
compiler that ships with Windows.

```bash
cmd /c native\build.bat
```

```bash
py tools/package.py
```

The first command produces `native/build/version.dll` and `native/build/HumanAI.exe`, and the
second packages `dist/human_ai-<version>.zip`.

## License

[GPL-3.0](LICENSE). Not affiliated with or endorsed by Paradox Interactive. Hearts of Iron is a
trademark of Paradox Interactive AB.
