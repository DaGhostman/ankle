# ![Logo: a chevron fused into a bracket, joined by a cursor](icons/hicolor/48x48/apps/ankle.png) ankle

[![CI](https://github.com/DaGhostman/ankle/actions/workflows/ci.yml/badge.svg)](https://github.com/DaGhostman/ankle/actions/workflows/ci.yml)

> **Ankle is a fork of [foot](https://codeberg.org/dnkl/foot)**, the
> fast, lightweight and minimalistic Wayland terminal emulator by
> Daniel Eklöf. It is an independent project, **not affiliated with or
> endorsed by foot**, and it is developed with the help of AI (see
> [Made with AI](#made-with-ai)). Please report ankle bugs
> [here](https://github.com/DaGhostman/ankle/issues), not to foot.

_Why "ankle"?_ It's the next thing after the foot.


## Why this fork exists

Foot is an excellent terminal: fast, small, and light on memory. What
it doesn't do is **ligatures**: it renders every cell on its own, so
programming fonts can't join `->`, `!=` or `===` into a single
symbol, or use contextual alternates such as Monaspace's _texture
healing_.

Ankle is foot with ligature support, plus a few other features that
make sense alongside it: OpenType font feature options, live
configuration reloading, and translucent fullscreen windows. The goal
is to stay as fast and lightweight as foot, and change nothing else.

Ankle is based on foot 1.28.0. Everything not listed below works
exactly as in foot.


## Status

Ankle is not released or packaged anywhere. To use it, [build it from
source](#installing).


## What's different from foot

* **Ligatures.** Runs of text are shaped together, so fonts can
  draw ligatures and contextual alternates. Enabled by default.
* **OpenType font features.** Enable or disable stylistic sets,
  character variants, etc. from the configuration, globally or per
  style (bold, italic, bold italic).
* **Listing a font's features.** `ankle --list-font-features="Monaspace
  Neon"` shows the OpenType features a font supports.
* **Configuration reloading.** The configuration, and the files it
  includes, are watched for changes, which are applied to all open
  windows, including in server mode. A configuration with errors is
  not applied; a desktop notification describes the error instead.
* **Translucent fullscreen windows.** Foot makes fullscreened windows
  opaque; ankle keeps the configured background transparency.
* **Bundled, patched fcft.** Ankle always builds its own copy of
  [fcft](https://codeberg.org/dnkl/fcft), patched to cache the glyphs
  of shaped text, which keeps ligature rendering fast.
* **Fixes** for a few memory safety and robustness bugs that are still
  open in foot (see the [changelog](CHANGELOG.md#ankle-unreleased)).


### New options

| Option | Section | Default | Description |
|---|---|---|---|
| `ligatures` | `[main]` | `yes` | `yes`, `no`, or `except-cursor` (ligatures are broken up under the cursor) |
| `font-features` | `[main]` | _empty_ | OpenType features, in HarfBuzz syntax, e.g. `calt ss01 -liga cv01=2` |
| `font-features-bold`, `font-features-italic`, `font-features-bold-italic` | `[main]` | _empty_ | Per-style features, applied on top of `font-features` |
| `auto-reload-config` | `[main]` | `yes` | Reload the configuration when its files change |
| `opaque-fullscreen` | `[main]` | `no` | Make fullscreened windows opaque, like foot does |
| `config-reload` | `[key-bindings]` | _none_ | Key binding that reloads the configuration |

Also new:

* `--list-font-features[=FONT]` command line option.
* Private mode `767371` (`\E[?767371h` / `\E[?767371l`), letting
  applications turn ligatures on and off. It supports `DECRQM`,
  `XTSAVE` and `XTRESTORE`.

All of these are documented in ankle's man pages: **ankle.ini**(5),
**ankle**(1) and **ankle-ctlseqs**(7).


### Example: Monaspace

Most programming fonts only draw their ligatures when the right
OpenType features are enabled. For
[Monaspace](https://monaspace.githubnext.com/):

```ini
[main]
font=Monaspace Neon:size=12
font-features=calt liga ss01 ss02 ss03 ss04 ss05 ss06 ss07 ss08 ss09 ss10
```

Characters are only shaped together when they use the same font and
style; e.g. a `-` followed by a **bold** `>` is drawn as two separate
characters.


## Switching from foot

* The executables are `ankle` and `ankleclient`, and the systemd units
  are `ankle-server.socket` and `ankle-server.service`. Ankle and foot
  can be installed side by side.
* The configuration file is `~/.config/ankle/ankle.ini`. If it doesn't
  exist, ankle uses `~/.config/foot/foot.ini`, so an existing foot
  configuration works as-is.
* Ankle uses foot's terminfo (`TERM=foot`), and identifies itself as
  foot (`XTVERSION`, device attributes), so applications that support
  foot treat ankle the same way. To tell the two apart, query the
  ligature mode: `\E[?767371$p`. Ankle replies with mode value `1`,
  `2` or `4`; foot replies with `0` (unknown mode).


## Documentation

Apart from the differences above, ankle behaves like foot, so foot's
documentation applies:

* [foot's README](https://codeberg.org/dnkl/foot#readme): features,
  shortcuts, server mode, URL mode, shell integration, fonts, DPI,
  supported escape sequences, and more.
* [foot's wiki](https://codeberg.org/dnkl/foot/wiki): troubleshooting,
  shell integration recipes, performance.

When reading them, substitute `ankle` for `foot`, `ankleclient` for
`footclient` and `ankle.ini` for `foot.ini`.

Ankle's own man pages (**ankle**(1), **ankleclient**(1),
**ankle.ini**(5), **ankle-ctlseqs**(7)) are installed with it, and
cover both foot's features and ankle's additions.


## Installing

See [INSTALL.md](INSTALL.md). In short, for a per-user install with
the server started on login:

```sh
meson setup bld/release --buildtype=release -Db_lto=true \
    --prefix="$HOME/.local" \
    -Dsystemd-units-dir="$HOME/.local/share/systemd/user"
ninja -C bld/release install
systemctl --user daemon-reload
systemctl --user enable --now ankle-server.socket ankle-server.service
```

Then use `ankleclient` to open terminal windows.


## Made with AI

Ankle is developed with the help of AI coding assistants
(Anthropic's Claude, through Claude Code). Everything ankle changes or
adds on top of foot, including the ligature renderer, the fcft
patches, configuration reloading and this documentation, was written
with AI assistance, and reviewed and tested by the maintainer.

Commits with AI-written changes carry a `Co-Authored-By:` trailer
naming the model. Commits inherited from foot are unchanged, and keep
their original authors.

Contributions made with the help of AI tools are welcome; see
[CONTRIBUTING.md](CONTRIBUTING.md).


## Bugs

Please report bugs to https://github.com/DaGhostman/ankle/issues, and
**not** to foot. If a bug also happens in foot, it is worth reporting
it to foot as well, following [foot's
guidelines](https://codeberg.org/dnkl/foot#bugs).

Please include the output of `ankle --version`, a log (`ankle -d
info`), your compositor and its version, and steps to reproduce.


## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) and the [Code of
Conduct](CODE_OF_CONDUCT.md).


## Credits

* [Daniel Eklöf](https://codeberg.org/dnkl) and [foot's
  contributors](https://codeberg.org/dnkl/foot/activity), for foot,
  which ankle is built on, and for
  [fcft](https://codeberg.org/dnkl/fcft) and
  [tllist](https://codeberg.org/dnkl/tllist). If you like ankle,
  consider [supporting foot's author](https://liberapay.com/dnkl).
* [Ordoviz](https://codeberg.org/Ordoviz), for designing foot's
  logo, which was ankle's original icon.


## License

Ankle is released under the [MIT license](LICENSE), like foot.
