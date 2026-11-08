# EDOPro Genesys (Progression Series fork)

This branch (`genesys`) is EDOPro **41.0.2** plus Genesys points in the deck editor, for a private Yu-Gi-Oh!
Progression Series. Nothing else changes.

- `genesys.conf` in the EDOPRO folder: a `$genesys <cap>` line, then `<card code> <points>` lines (`#` comments).
  Loaded at startup and every time the deck editor opens. No file = stock behaviour.
- Each card with points gets an icon built like the limit icon (black disc, purple ring, yellow number): top right,
  or on the left edge under the limit icon in rows where cards overlap. `textures/genesys/<points>.png` is used when
  present (exact bold italic look), otherwise it's drawn. Alternate artworks use their main card's points.
- The Main Deck bar shows `Genesys <points>/<cap>` for Main + Extra (the Side Deck doesn't count), red when over.
- Ready is refused while the deck is over the cap when the deck's name or the room's banlist starts with `PROG - `
  (hosted rooms, LAN, LAN + AI). Nothing is shown during duels.

Builds: GitHub Actions on this branch (Windows x86 with the VS2019 toolset, macOS universal), dependencies pinned to
the 41.0.2 era. The card picture URLs are the official ones; the auto-updater URL is left out on purpose so the client
doesn't replace itself with the stock build. Install = replace only the program file (`EDOPro.exe` on Windows,
`EDOPro.app/Contents/MacOS/EDOPro` on macOS) after backing up the original.

---

# [Project Ignis](https://github.com/ProjectIgnis): EDOPro

The bleeding-edge automatic duel simulator, a fork of the [YGOPro client](https://github.com/Fluorohydride/ygopro).

All YGOPro forks and known automatic duel simulators are powered by the [YGOPro core (ocgcore)](https://github.com/Fluorohydride/ygopro-core), an automated scripting engine for the Yu-Gi-Oh! Official Card Game. EDOPro is powered by our own [ocgcore fork](https://github.com/edo9300/ygopro-core).

Due to many accumulated changes in this client and its core, it is incompatible with simulators not derived from this fork.

This repository is for the game client only. Related Ignis projects:
- [Canonical card script collection](https://github.com/ProjectIgnis/CardScripts)
- [Canonical card databases collection](https://github.com/ProjectIgnis/BabelCdb)
- [WindBot Ignite](https://github.com/ProjectIgnis/windbot/)

## Contributing

Please keep all usage questions and Windows and macOS bug reports on Discord; do not open an issue or pull request for this purpose.
We are not taking suggestions or feature requests and the issue tracker is not to be used for this purpose either.

Otherwise, pull requests are welcome! It might take some time for them to be evaluated since we are pretty swamped with a lot work to be done.

Check out the [wiki](https://github.com/edo9300/edopro/wiki/) for possibly outdated build instructions and a partial user manual.

## Project Ignis

We are an international, open-source collaboration staffed entirely by volunteers and we welcome support on our projects.
Reach out to us on Discord to learn how to contribute and join!

_Ignis_ is the fire and light of knowledge passed from the gods to humanity in Greco-Roman mythology.
This represents our vision for all of our projects and work and recognizes the contribution of every individual on the team.

[Debut announcement on Reddit](https://www.reddit.com/r/yugioh/comments/fvdn7v/presenting_project_ignis_edopro_the_opensource/).

## License

EDOPro is free/libre and open source software licensed under the GNU Affero General Public License, version 3 or later.
Dependencies and resources may be provided under different licenses.
Please see [LICENSE](https://github.com/edo9300/edopro/blob/master/LICENSE) and [COPYING](https://github.com/edo9300/edopro/blob/master/COPYING) for more details.

Yu-Gi-Oh! is a trademark of Shueisha and Konami. This project is not affiliated with or endorsed by Shueisha or Konami.
