# [Project Ignis](https://github.com/ProjectIgnis): EDOPro

The bleeding-edge automatic duel simulator, a fork of the [YGOPro client](https://github.com/Fluorohydride/ygopro).

All YGOPro forks and known automatic duel simulators are powered by the [YGOPro core (ocgcore)](https://github.com/Fluorohydride/ygopro-core), an automated scripting engine for the Yu-Gi-Oh! Official Card Game. EDOPro is powered by our own [ocgcore fork](https://github.com/edo9300/ygopro-core).

Due to many accumulated changes in this client and its core, it is incompatible with simulators not derived from this fork.

This repository is for the game client only. Related Ignis projects:
- [Canonical card script collection](https://github.com/ProjectIgnis/CardScripts)
- [Canonical card databases collection](https://github.com/ProjectIgnis/BabelCdb)
- [WindBot Ignite](https://github.com/ProjectIgnis/windbot/)

## Headless Room Hosting CLI

The client supports launching a duel room without opening the GUI:

```bash
ygopro --host-headless "Name=AI Room Port=7911 Mode=single"
```

Short flag `-H` is also supported.

Required launch keys:
- `Name` (room name)
- `Port` (1-65535)
- `Mode` (`single`, `match`, `tag`, or `rush`)

Optional launch keys:
- `Password`
- `BestOf` (integer >= 1)
- `ConfigFile` (JSON overrides for host settings)

Arguments can be passed as repeated `Key=Value` tokens after `--host-headless`.
For backward compatibility, a single quoted payload containing multiple `Key=Value` tokens is still accepted.
Unsupported keys, duplicate keys, or invalid values fail fast, emit an `ERROR` lifecycle event, and exit non-zero.

### Lifecycle Event Output (stdout JSON Lines)

Headless mode emits one JSON object per line to stdout:
- `ROOM_STARTED` with `detail.port`
- `CLIENT_JOINED` with `detail.name` and `detail.seat`
- `DUEL_STARTED`
- `DUEL_ENDED` with `detail.winner_seat` (`0`, `1`, or `null`)
- `ROOM_CLOSED` with `detail.reason` (`duel_ended`, `stopped`, or `error`)
- `ERROR` with `detail.reason`

Example:

```json
{"event":"ROOM_STARTED","timestamp":"2026-08-01T12:00:00Z","detail":{"port":7911}}
{"event":"CLIENT_JOINED","timestamp":"2026-08-01T12:00:05Z","detail":{"name":"[AI]Bot","seat":0}}
{"event":"DUEL_STARTED","timestamp":"2026-08-01T12:00:10Z","detail":{}}
{"event":"DUEL_ENDED","timestamp":"2026-08-01T12:05:00Z","detail":{"winner_seat":0}}
{"event":"ROOM_CLOSED","timestamp":"2026-08-01T12:05:01Z","detail":{"reason":"duel_ended"}}
```

On `SIGINT`/`SIGTERM`, the host disconnects connected players with a stop reason and emits `ROOM_CLOSED` with reason `stopped`.

### Headless Host Test Command

Run the headless-host regression suite from the repository root:

```bash
./tests/run_headless_host_tests.sh
```

This command executes production-code lifecycle checks against a built `ygopro` binary.
By default it uses `../ProjectIgnis/ygopro`; override with:

```bash
HEADLESS_HOST_BINARY=/absolute/path/to/ygopro ./tests/run_headless_host_tests.sh
```

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
