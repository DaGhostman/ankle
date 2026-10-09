# ![Logo: a terminal with a foot shaped prompt](icons/hicolor/48x48/apps/ankle.png) ankle

A fast, lightweight Wayland terminal emulator with ligature support. A
fork of [foot](https://codeberg.org/dnkl/foot).

_Why "ankle"?_ It's the next thing after the foot.


## Index

1. [About this fork](#about-this-fork)
   1. [Made with AI](#made-with-ai)
   1. [What's different from foot](#what-s-different-from-foot)
   1. [Switching from foot](#switching-from-foot)
1. [Features](#features)
1. [Installing](#installing)
1. [Configuration](#configuration)
1. [Ligatures and font features](#ligatures-and-font-features)
1. [Troubleshooting](#troubleshooting)
1. [Fonts](#fonts)
1. [Shortcuts](#shortcuts)
   1. [Keyboard](#keyboard)
      1. [Normal mode](#normal-mode)
      1. [Scrollback search](#scrollback-search)
   1. [Mouse](#mouse)
   1. [Touchscreen](#touchscreen)
1. [Server (daemon) mode](#server-daemon-mode)
1. [URLs](#urls)
1. [Shell integration](#shell-integration)
   1. [Current working directory](#current-working-directory)
   1. [Jumping between prompts](#jumping-between-prompts)
   1. [Piping last command's output](#piping-last-command-s-output)
1. [Alt/meta](#alt-meta)
1. [Backspace](#backspace)
1. [Keypad](#keypad)
1. [DPI and font size](#dpi-and-font-size)
1. [Supported OSCs](#supported-oscs)
1. [Programmatically checking if running in ankle](#programmatically-checking-if-running-in-ankle)
1. [XTGETTCAP](#xtgettcap)
1. [Contributing](#contributing)
1. [Code of Conduct](#code-of-conduct)
1. [Bugs](#bugs)
1. [Credits](#credits)
1. [License](#license)


## About this fork

Ankle is a fork of [foot](https://codeberg.org/dnkl/foot), Daniel
Eklöf's Wayland terminal emulator, based on foot 1.28.0. It keeps
foot's speed, small footprint and feature set, and adds the features
listed [below](#what-s-different-from-foot), most notably ligatures.

Ankle is an independent project, not affiliated with or endorsed by
foot. **Please do not report ankle bugs to foot**; use [ankle's issue
tracker](https://github.com/DaGhostman/ankle/issues) instead.

Most of foot's documentation applies to ankle as well, including
[foot's wiki](https://codeberg.org/dnkl/foot/wiki), which this README
links to in several places.


### Made with AI

Ankle is developed with the help of AI coding assistants
(Anthropic's Claude, through Claude Code). The changes made on top of
foot, including the ligature renderer, the fcft patches, the
configuration reloading and the documentation, were written with AI
assistance, and reviewed and tested by the maintainer.

Commits with AI-written changes carry a `Co-Authored-By:` trailer
naming the model. Commits inherited from foot are unchanged, and keep
their original authors.

Contributions made with the help of AI tools are welcome; see
[Contributing](#contributing).


### What's different from foot

* **Ligatures**: runs of text are shaped together, so fonts can draw
  ligatures (`->`, `!=`, `===`, ...) and contextual alternates
  (e.g. Monaspace's _texture healing_). Enabled by default; set
  `ligatures=no` to disable, or `ligatures=except-cursor` to break
  ligatures up under the cursor. Applications can toggle ligatures
  with private mode `767371`.
* **OpenType font features**: `font-features`, and per-style
  `font-features-bold`, `-italic` and `-bold-italic` options, to
  enable or disable stylistic sets, character variants etc.
* **`--list-font-features`**: lists the OpenType features a font
  supports (`ankle --list-font-features="Monaspace Neon"`).
* **Configuration reloading**: the configuration (and the files it
  includes) are watched, and changes are applied to all open windows,
  including in server mode. Can also be triggered with the
  `config-reload` key binding. A broken configuration is not applied;
  you get a desktop notification instead.
* **Translucent fullscreen windows**: background transparency is kept
  when fullscreened (foot always makes fullscreen windows opaque). Set
  `opaque-fullscreen=yes` for foot's behavior.
* **Bundled, patched fcft**: ankle always builds the bundled fcft,
  which caches glyphs of shaped text runs, keeping ligature rendering
  fast.
* **Fixes** for a few bugs that are open upstream, including sixel
  memory safety issues.


### Switching from foot

* The executables are `ankle` and `ankleclient`, and the server socket
  is `$XDG_RUNTIME_DIR/ankle-$WAYLAND_DISPLAY.sock`.
* The configuration file is `~/.config/ankle/ankle.ini`. If it doesn't
  exist, ankle falls back to `~/.config/foot/foot.ini`, so an existing
  foot configuration works as-is.
* Ankle uses foot's terminfo (`TERM=foot`), and identifies itself as
  foot to applications (see [Programmatically checking if running in
  ankle](#programmatically-checking-if-running-in-ankle)). Applications
  that support foot support ankle.
* Ankle and foot can be installed side by side.


## Features

* Fast (see [benchmarks](doc/benchmark.md), and
  [performance](https://codeberg.org/dnkl/foot/wiki/Performance) in
  foot's wiki)
* Lightweight, in dependencies, on-disk and in-memory
* Wayland native
* DE agnostic
* Server/daemon mode
* Ligatures, and configurable OpenType font features
* Live configuration reloading
* User configurable font fallback
* On-the-fly font resize
* On-the-fly DPI font size adjustment
* Scrollback search
* Keyboard driven URL detection
* Color emoji support
* IME (via `text-input-v3`)
* Multi-seat
* True Color (24bpp)
* [Styled and colored underlines](https://sw.kovidgoyal.net/kitty/underlines/)
* [Synchronized Updates](https://gitlab.freedesktop.org/terminal-wg/specifications/-/merge_requests/2) support
* [Sixel image support](https://en.wikipedia.org/wiki/Sixel)

  ![tux-with-foot](doc/sixel-tux-foot.png "Sixel screenshot")


# Installing

See [INSTALL.md](INSTALL.md).


## Configuration

**ankle** can be configured by creating a file
`$XDG_CONFIG_HOME/ankle/ankle.ini` (defaulting to
`~/.config/ankle/ankle.ini`). A template for that can usually be
found in `/etc/xdg/ankle/ankle.ini` or [here](ankle.ini).

Further information can be found in the `ankle.ini(5)` man page.


## Ligatures and font features

Ligatures are enabled by default, but most programming fonts only
draw their ligatures when the right OpenType features are enabled. Use
`--list-font-features` to see what a font supports, and
`font-features` to enable them. For example, for
[Monaspace](https://monaspace.githubnext.com/):

```ini
[main]
font=Monaspace Neon:size=12
font-features=calt liga ss01 ss02 ss03 ss04 ss05 ss06 ss07 ss08 ss09 ss10
```

Characters are only shaped together when they use the same font and
style. For example, a `-` followed by a **bold** `>` is drawn as two
separate characters.

See `ligatures` and `font-features` in `ankle.ini(5)` for details,
including how to mix font families (e.g. Monaspace's _texture
healing_ in regular text, with a different family for italics).


## Troubleshooting

See [foot's wiki](https://codeberg.org/dnkl/foot/wiki#user-content-troubleshooting).
Ankle behaves like foot, except for the [differences listed
above](#what-s-different-from-foot).


## Fonts

**ankle** supports all fonts that can be loaded by _freetype_,
including **bitmap** fonts and **color emoji** fonts.

Ankle uses _fontconfig_ to locate and configure the font(s) to
use. Since fontconfig's fallback mechanism is imperfect, especially
for monospace fonts (it doesn't prefer monospace fonts even though the
requested font is one), ankle allows you, the user, to configure the
fallback fonts to use.

This also means you can configure _each_ fallback font individually;
you want _that_ fallback font to use _this_ size, and you want that
_other_ fallback font to be _italic_?  No problem!

If a glyph cannot be found in _any_ of the user configured fallback
fonts, _then_ fontconfig's list is used.


## Shortcuts

These are the default shortcuts. See `man ankle.ini` and the example
`ankle.ini` to see how these can be changed.


### Keyboard

#### Normal mode

<kbd>shift</kbd>+<kbd>page up</kbd>/<kbd>page down</kbd>
: Scroll up/down in history

<kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>c</kbd>, <kbd>XF86Copy</kbd>
: Copy selected text to the _clipboard_

<kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>v</kbd>, <kbd>XF86Paste</kbd>
: Paste from _clipboard_

<kbd>shift</kbd>+<kbd>insert</kbd>
: Paste from the _primary selection_

<kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>r</kbd>
: Start a scrollback search

<kbd>ctrl</kbd>+<kbd>+</kbd>, <kbd>ctrl</kbd>+<kbd>=</kbd>
: Increase font size

<kbd>ctrl</kbd>+<kbd>-</kbd>
: Decrease font size

<kbd>ctrl</kbd>+<kbd>0</kbd>
: Reset font size

<kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>n</kbd>
: Spawn a new terminal. If the shell has been [configured to emit the
  OSC 7 escape
  sequence](https://codeberg.org/dnkl/foot/wiki#user-content-spawning-new-terminal-instances-in-the-current-working-directory),
  the new terminal will start in the current working directory.

<kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>o</kbd>
: Enter URL mode, where all currently visible URLs are tagged with a
  jump label with a key sequence that will open the URL.

<kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>u</kbd>
: Enter Unicode input mode.

<kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>z</kbd>
: Jump to the previous, currently not visible, prompt. Requires [shell
  integration](https://codeberg.org/dnkl/foot/wiki#user-content-jumping-between-prompts).

<kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>x</kbd>
: Jump to the next prompt. Requires [shell
  integration](https://codeberg.org/dnkl/foot/wiki#user-content-jumping-between-prompts).


#### Scrollback search

<kbd>ctrl</kbd>+<kbd>r</kbd>
: Search _backward_ for next match

<kbd>ctrl</kbd>+<kbd>s</kbd>
: Search _forward_ for next match

<kbd>ctrl</kbd>+<kbd>w</kbd>
: Extend current selection (and thus the search criteria) to the end
  of the word, or the next word if currently at a word separating
  character.

<kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>w</kbd>
: Same as <kbd>ctrl</kbd>+<kbd>w</kbd>, except that the only word
  separating characters are whitespace characters.

<kbd>ctrl</kbd>+<kbd>v</kbd>, <kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>v</kbd>, <kbd>ctrl</kbd>+<kbd>y</kbd>, <kbd>XF86Paste</kbd>
: Paste from clipboard into the search buffer.

<kbd>shift</kbd>+<kbd>insert</kbd>
: Paste from primary selection into the search buffer.

<kbd>escape</kbd>, <kbd>ctrl</kbd>+<kbd>g</kbd>
: Cancel the search

<kbd>return</kbd>
: Finish the search and copy the current match to the primary
  selection


### URL mode

<kbd>t</kbd>
: Toggle whether the URL is displayed in the jump label or not

<kbd>escape</kbd>, <kbd>ctrl</kbd>+<kbd>c</kbd>, <kbd>ctrl</kbd>+<kbd>g</kbd>, <kbd>ctrl</kbd>+<kbd>d</kbd>
: Exit URL mode without launching any URLs


### Mouse

<kbd>left</kbd> - **single-click**
: Drag to select; when released, the selected text is copied to the
  _primary_ selection. This feature is **disabled** when client has
  enabled _mouse tracking_.
: Holding <kbd>shift</kbd> enables selection in mouse tracking enabled
  clients.
: Holding <kbd>ctrl</kbd> will create a block selection.

<kbd>left</kbd> - **double-click**
: Selects the _word_ (separated by spaces, period, comma, parenthesis
  etc) under the pointer. Hold <kbd>ctrl</kbd> to select everything
  under the pointer up to, and until, the next space characters.

<kbd>left</kbd> - **triple-click**
: Selects the everything between enclosing quotes, or the entire row
  if not inside a quote.

<kbd>left</kbd> - **quad-click**
: Selects the entire row.

<kbd>middle</kbd>
: Paste from _primary_ selection

<kbd>right</kbd>
: Extend current selection. Clicking immediately extends the
  selection, while hold-and-drag allows you to interactively resize
  the selection.

<kbd>ctrl</kbd>+<kbd>right</kbd>
: Extend the current selection, but force it to be character wise,
  rather than depending on the original selection mode.

<kbd>wheel</kbd>
: Scroll up/down in history

<kbd>ctrl</kbd>+<kbd>wheel</kbd>
: Increase/decrease font size


### Touchscreen

<kbd>tap</kbd>
: Emulates mouse left button click.

<kbd>drag</kbd>
: Scrolls up/down in history.
: Holding for a while before dragging (time delay can be configured)
  emulates mouse dragging with left button held.


## Server (daemon) mode

When run normally, **ankle** is a single-window application; if you
want another window, start another ankle process.

However, ankle can also be run in a _server_ mode. In this mode, one
process hosts multiple windows. All Wayland communication, VT parsing
and rendering is done in the server process.

New windows are opened by running `ankleclient`, which remains running
until the terminal window is closed, at which point it exits with the
exit value of the client process (typically the shell).

The point of this mode is **a)** reduced memory footprint - all
terminal windows will share fonts and glyph cache, and **b)** reduced
startup time - loading fonts and populating the glyph cache takes
time, but in server mode it only happens once.

The downside is a performance penalty; all windows' input and output
are multiplexed in the same thread (but each window will have its own
set of rendering threads). This means that if one window is very busy
with, for example, producing output, then other windows will suffer.

And of course, should the server process crash, **all** windows will
be gone.

Typical usage would be to start the server process (`ankle --server`)
when starting your Wayland compositor (i.e. logging in to your
desktop), and then run `ankleclient` instead of `ankle` whenever you
want to launch a new terminal.

Ankle supports socket activation, which means `ankle --server` will
only be started the first time you'll run `ankleclient`. systemd user
units are included (`ankle-server.socket` and `ankle-server.service`),
but it can work with other supervision suites. To start the server
when you log in:

```sh
systemctl --user enable --now ankle-server.socket ankle-server.service
```

Configuration changes are applied to all of the server's windows (see
`RELOADING` in `ankle.ini(5)`).


## URLs

Ankle supports URL detection. But, unlike many other terminal
emulators, where URLs are highlighted when they are hovered and opened
by clicking on them, ankle uses a keyboard driven approach.

Pressing <kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>o</kbd> enters _"URL
mode"_, where all currently visible URLs are underlined, and is
associated with a _"jump-label"_. The jump-label indicates the _key
sequence_ (e.g. **"AF"**) to use to activate the URL.

The key binding can, of course, be customized, like all other key
bindings in ankle. See `show-urls-launch` and `show-urls-copy` in the
`ankle.ini` man page.

`show-urls-launch` by default opens the URL with `xdg-open`. This can
be changed with the `url-launch` option.

`show-urls-copy` is an alternative to `show-urls-launch`, that changes
what activating a URL _does_; instead of opening it, it copies it to
the clipboard. It is unbound by default.

Jump label colors, the URL underline color, and the letters used in
the jump label key sequences can be configured.


## Shell integration

### Current working directory

New ankle terminal instances (bound to
<kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>n</kbd> by default) will open in
the current working directory, **if** the shell in the "parent"
terminal reports directory changes.

This is done with the OSC-7 escape sequence. Most shells can be
scripted to do this, if they do not support it natively. See [foot's
wiki](https://codeberg.org/dnkl/foot/wiki#user-content-spawning-new-terminal-instances-in-the-current-working-directory)
for details.


### Jumping between prompts

Ankle can move the current viewport to focus prompts of already
executed commands (bound to
<kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>z</kbd>/<kbd>x</kbd> by
default).

For this to work, the shell needs to emit an OSC-133;A
(`\E]133;A\E\\`) sequence before each prompt.

In zsh, one way to do this is to add a `precmd` hook:

```zsh
precmd() {
    print -Pn "\e]133;A\e\\"
}
```

See [foot's
wiki](https://codeberg.org/dnkl/foot/wiki#user-content-jumping-between-prompts)
for details, and examples for other shells.

### Piping last command's output

The key binding `pipe-command-output` can pipe the last command's
output to an application of your choice (similar to the other `pipe-*`
key bindings):

```ini
[key-bindings]
pipe-command-output=[sh -c "f=$(mktemp); cat - > $f; ankleclient emacsclient -nw $f; rm $f"] Control+Shift+g
```

When pressing <kbd>ctrl</kbd>+<kbd>shift</kbd>+<kbd>g</kbd>, the last
command's output is written to a temporary file, then an emacsclient
is started in a new ankleclient instance. The temporary file is
removed after the ankleclient instance has closed.

For this to work, the shell must emit an OSC-133;C (`\E]133;C\E\\`)
sequence before command output starts, and an OSC-133;D
(`\E]133;D\E\\`) when the command output ends.

In fish, one way to do this is to add `preexec` and `postexec` hooks:

```fish
function ankle_cmd_start --on-event fish_preexec
  echo -en "\e]133;C\e\\"
end

function ankle_cmd_end --on-event fish_postexec
  echo -en "\e]133;D\e\\"
end
```

See [foot's
wiki](https://codeberg.org/dnkl/foot/wiki#user-content-piping-last-command-s-output)
for details, and examples for other shells


## Alt/meta

By default, ankle prefixes _Meta characters_ with ESC. This
corresponds to XTerm's `metaSendsEscape` option set to `true`.

This can be disabled programmatically with `\E[?1036l` (and enabled
again with `\E[?1036h`).

When disabled, ankle will instead set the 8:th bit of meta character
and then UTF-8 encode it. This corresponds to XTerm's `eightBitMeta`
option set to `true`.

This can also be disabled programmatically with `rmm` (_reset meta
mode_, `\E[?1034l`), and enabled again with `smm` (_set meta mode_,
`\E[?1034h`).


## Backspace

Ankle transmits DEL (`^?`) on <kbd>backspace</kbd>. This corresponds
to XTerm's `backarrowKey` option set to `false`, and to
[`DECBKM`](https://vt100.net/docs/vt510-rm/DECBKM.html) being _reset_.

To instead transmit BS (`^H`), press
<kbd>ctrl</kbd>+<kbd>backspace</kbd>.

Note that ankle does **not** implement `DECBKM`, and that the behavior
described above **cannot** be changed.

Finally, pressing <kbd>alt</kbd> will prefix the transmitted byte with
ESC.


## Keypad

By default, <kbd>Num Lock</kbd> overrides the run-time configuration
keypad mode; when active, the keypad is always considered to be in
_numerical_ mode. This corresponds to XTerm's `numLock` option set to
`true`.

In this mode, the keypad keys always sends either numbers (<kbd>Num
Lock</kbd> is **active**) or cursor movement keys (<kbd>Up</kbd>,
<kbd>Down</kbd>, <kbd>Left</kbd>, <kbd>Right</kbd>, <kbd>Page
Up</kbd>, <kbd>Page Down</kbd> etc).

This can be disabled programmatically with `\E[?1035l` (and enabled
again with `\E[?1035h`).

When disabled, the keypad sends custom escape sequences instead of
numbers, when in _application_ mode.


## DPI and font size

Font sizes are apparently a complex thing. Many applications use a
fixed DPI of 96. They may also multiply it with the monitor's scale
factor.

This results in fonts with different **physical** sizes (i.e. if
measured by a ruler) when rendered on screens with different DPI
values. Even if the configured font size is the same.

This is not how it is meant to be. Fonts are measured in _point sizes_
**for a reason**; a given point size should have the same height on
all mediums, be it printers or monitors, regardless of their DPI.

That said, on Wayland, Hi-DPI monitors are typically handled by
configuring a _"scaling factor"_ in the compositor. This is usually
expressed as either a rational value (e.g. _1.5_), or as a percentage
(e.g. _150%_), by which all fonts and window sizes are supposed to be
multiplied.

For this reason, and because of the new _fractional scaling_ protocol
(see below for details), and because this is how Wayland applications
are expected to behave, ankle (like foot >= 1.15) defaults to scaling
fonts using the compositor's scaling factor, and **not** the monitor
DPI.

This means the (assuming the monitors are at the same viewing
distance) the font size will appear to change when you move the ankle
window across different monitors, **unless** you have configured the
monitors' scaling factors correctly in the compositor.

This can be changed by setting the `dpi-aware` option to `yes` in
`ankle.ini`. When enabled, fonts will **not** be sized using the
scaling factor, but will instead be sized using the monitor's
DPI. When the ankle window is moved across monitors, the font size is
updated for the current monitor's DPI.

This means that, assuming the monitors are **at the same viewing
distance**, the font size will appear to be the same, at all times.

_Note_: if you configure **pixelsize**, rather than **size**, then DPI
changes will **not** change the font size. Pixels are always pixels.


### Fractional scaling on Wayland

For a long time, there was no **true** support for _fractional
scaling_. That is, values like 1.5 (150%), 1.8 (180%) etc, only
integer values, like 2 (200%).

Compositors that _did_ support fractional scaling did so using a hack;
all applications were told to scale to 200%, and then the compositor
would down-scale the rendered image to e.g. 150%. This works OK for
everything **except fonts**, which ended up blurry.

With _wayland-protocols 1.32_, a new protocol was introduced, that
allows compositors to tell applications the _actual_ scaling
factor. Applications can then scale the image using a _viewport_
object, instead of setting a scale factor on the raw pixel buffer.


## Supported OSCs

OSC, _Operating System Command_, are escape sequences that interacts
with the terminal emulator itself. Ankle implements the following
OSCs:

* `OSC 0` - change window icon + title (but only title is actually
  supported)
* `OSC 2` - change window title
* `OSC 4` - change color palette
* `OSC 7` - report CWD (see [shell integration](#shell-integration))
* `OSC 8` - hyperlink
* `OSC 9` - desktop notification
* `OSC 10` - change (default) foreground color
* `OSC 11` - change (default) background color
* `OSC 12` - change cursor color
* `OSC 17` - change highlight (selection) background color
* `OSC 19` - change highlight (selection) foreground color
* `OSC 22` - set the xcursor (mouse) pointer
* `OSC 52` - copy/paste clipboard data
* `OSC 99` - desktop notification (kitty's notification protocol)
* `OSC 104` - reset color palette
* `OSC 110` - reset default foreground color
* `OSC 111` - reset default background color
* `OSC 112` - reset cursor color
* `OSC 117` - reset highlight background color
* `OSC 119` - reset highlight foreground color
* `OSC 133` - [shell integration](#shell-integration)
* `OSC 176` - set app ID
* `OSC 555` - flash screen (**foot specific**)
* `OSC 777` - desktop notification (only the `;notify` sub-command of
  OSC 777 is supported.)

See the **ankle-ctlseqs**(7) man page for a complete list of supported
control sequences.


## Programmatically checking if running in ankle

For compatibility, ankle identifies itself as **foot**: it uses foot's
terminfo, and answers the device attribute and version queries the
same way foot does. Applications that detect foot will therefore
detect ankle as foot, and enable the same features.

Ankle does **not** set any environment variables that can be used to
identify it (reading `TERM` is not reliable since the user may have
chosen to use a different terminfo).

You can instead use the escape sequences to read the _Secondary_ and
_Tertiary Device Attributes_ (secondary/tertiary DA, for short).

The tertiary DA response is always `\EP!|464f4f54\E\\`, where
`464f4f54` is `FOOT` in hex.

The secondary DA response is `\E[>1;XXYYZZ;0c`, where `XXYYZZ` is the
major, minor and patch version numbers (of the foot release ankle is
based on), in decimal, using two digits for each number. For example,
version 1.4.2 would respond with `\E[>1;010402;0c`.

**Note**: not all terminal emulators implement tertiary DA. Most
implement secondary DA, but not all. All _should_ however implement
_Primary DA_.

Thus, a safe way to query the terminal is to request the tertiary,
secondary and primary DA all at once, in that order. All terminals
should ignore escape sequences they do not recognize. You will have to
parse the response (which in ankle will consist of all three DA
responses, all at once) to determine which requests the terminal
emulator actually responded to.

Ankle also implements `XTVERSION`, to which it will reply with
`\EP>|foot(version)\E\\`. Version is e.g. "1.28.0" for a regular
release, or "1.28.0-36-g7db8e06f" for a git build.

To tell ankle apart from foot, query the ligature mode with `DECRQM`:
`\E[?767371$p`. Ankle replies with `\E[?767371;1$y` (ligatures
enabled), `\E[?767371;2$y` (disabled) or `\E[?767371;4$y`
(unsupported); foot replies with `\E[?767371;0$y` (unknown mode).


# XTGETTCAP

`XTGETTCAP` is an escape sequence initially introduced by XTerm, and
also implemented (and extended, to some degree) by Kitty.

It allows querying the terminal for terminfo
capabilities. Applications using this feature do not need to use the
classic, file-based, terminfo definition. For example, if all
applications used this feature, you would no longer have to install
foot's terminfo on remote hosts you SSH into.

XTerm's implementation (as of XTerm-370) only supports querying key
(as in keyboard keys) capabilities, and three custom capabilities:

* `TN` - terminal name
* `Co` - number of colors (alias for the `colors` capability)
* `RGB` - number of bits per color channel (different semantics from
  the `RGB` capability in file-based terminfo definitions!).

Kitty has extended this, and also supports querying all integer and
string capabilities.

Ankle (like foot) supports this, and extends it even further, to also
include boolean capabilities. This means the entire terminfo can be
queried via `XTGETTCAP`.

Note that both Kitty and ankle handles **responses** to
multi-capability queries slightly differently, compared to XTerm.

XTerm will send a single DCS reply, with `;`-separated
capability/value pairs. There are a couple of issues with this:

* The success/fail flag in the beginning of the response is always `1`
  (success), unless the very **first** queried capability is invalid.
* XTerm will not respond **at all** to an invalid capability, unless
  it's the first one in the `XTGETTCAP` query.
* XTerm will end the response at the first invalid capability.

In other words, if you send a large multi-capability query, you will
only get responses up to, but not including, the first invalid
capability. All subsequent capabilities will be dropped.

Kitty and ankle on the other hand, send one DCS response for **each**
capability in the multi query. This allows us to send a proper
success/fail flag for each queried capability. Responses for **all**
queried capabilities are **always** sent. No queries are ever dropped.

All replies are in `tigetstr()` format. That is, given the same
capability name, ankle's reply is identical to what `tigetstr()` would
have returned.

In addition to queries for terminfo entries, the `query-os-name` query
will be answered with a response of the form `uname=$(uname -s)`,
where `$(uname -s)` is the name of the OS ankle was compiled for.


# Contributing

See [Contributing](CONTRIBUTING.md)


# Code of Conduct

See [Code of Conduct](CODE_OF_CONDUCT.md)


# Bugs

Please report bugs to https://github.com/DaGhostman/ankle/issues, and
**not** to foot. If you can reproduce the bug in foot as well, it is
worth reporting it to foot too, following [foot's
guidelines](https://codeberg.org/dnkl/foot#bugs).

Before you open a new issue, please search existing bug reports, both
open **and** closed ones. Chances are someone else has already
reported the same issue.

The report should contain the following:

- Ankle version (`ankle --version`).
- Log output from ankle (run `ankle -d info` from another terminal).
- Which Wayland compositor (and version) you are running.
- If reporting a crash, please try to provide a `bt full` backtrace
  with symbols.
- Steps to reproduce. The more details the better.


# Credits

* [Daniel Eklöf](https://codeberg.org/dnkl) and [foot's
  contributors](https://codeberg.org/dnkl/foot/activity), for foot,
  which ankle is built on, and for
  [fcft](https://codeberg.org/dnkl/fcft) and
  [tllist](https://codeberg.org/dnkl/tllist). If you like ankle,
  consider [supporting foot's author](https://liberapay.com/dnkl).
* [Ordoviz](https://codeberg.org/Ordoviz), for designing foot's
  [logo](icons/hicolor/48x48/apps/ankle.png), which ankle currently
  uses.


# License

Ankle is released under the [MIT license](LICENSE), like foot.
