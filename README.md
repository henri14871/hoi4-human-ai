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

Click **Human AI** in the bottom-left corner of the map to open the panel. Each area shows who
controls it. Click **You** to hand it to the AI, or **AI** to take it back. Changes apply within
a second. Your choices are saved with the campaign, so every save keeps its own setup.

## Install

Hearts of Iron IV **1.19.x**, Windows, single-player.

1. Download `human_ai-<version>.zip` from [Releases](../../releases).
2. Copy `Hearts of Iron IV (game folder)/version.dll` next to `hoi4.exe`. In Steam, right-click
   the game, then choose *Manage* → *Browse local files*.
3. Copy the contents of `Documents/Paradox Interactive/Hearts of Iron IV/mod/` into your own
   `Documents\Paradox Interactive\Hearts of Iron IV\mod\` folder.
4. In the launcher, add **Human AI** to your playset.

To uninstall, delete `version.dll` and remove the mod from your playset.

The DLL does the actual work, and the mod adds the in-game panel. Without the mod you can still
set things in `human_ai.ini`, which is created in the game folder on first launch.

## How it works

HoI4's country AI is built from separate modules (foreign minister, political minister, generals
and so on), and the game can already run them for human players through the `human_ai` debug
command, though only all at once. `version.dll` loads with the game, turns that switch on, and
puts a filter in front of each module's update so that only the areas you picked run for your
country. AI nations are never affected.

Everything is located at startup from patterns in the game's code, not fixed addresses. If a game
update changes something it relies on, the mod writes the reason to `human_ai.log` and the game
stays vanilla. For the reverse-engineering notes and offsets, see [native/README.md](native/README.md).

## Known limitations

- **Single-player only.** It will desync multiplayer games.
- **Game updates can break it.** When that happens it switches itself off (see `human_ai.log`)
  until it's updated.
- **The split isn't perfect.**
  - While any area is handed over, the game may also answer a few prompts for you that don't
    belong to a module, such as event options and some diplomatic replies.
  - Naval invasions are planned by the land AI.
- Only the DirectX build of the game on Windows is tested.

## Building

Needs Visual Studio with the C++ x64 tools and Python 3.

```bash
cmd /c native\build.bat
```

```bash
py tools/package.py
```

The first command produces `native/build/version.dll`, and the second packages
`dist/human_ai-<version>.zip`. To regenerate the panel scripts after editing the area list, run
`py tools/gen_panel.py`.

## License

[GPL-3.0](LICENSE). Not affiliated with or endorsed by Paradox Interactive. Hearts of Iron is a
trademark of Paradox Interactive AB.
