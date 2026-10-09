ankleclient(1)

# NAME
ankleclient - start new terminals in an ankle server

# SYNOPSIS
*ankleclient* [_OPTIONS_]++
*ankleclient* [_OPTIONS_] <_command_> [_COMMAND OPTIONS_]

All trailing (non-option) arguments are treated as a command, and its
arguments, to execute (instead of the default shell).

# DESCRIPTION

*ankleclient* is used together with *ankle*(1) in *--server*
mode.

Running it without arguments will open a new terminal window (hosted
in the ankle server), with your default shell. The exit code will be
that of the terminal. I.e *ankleclient* does not exit until the
terminal has terminated.

# OPTIONS

*-t*,*--term*=_TERM_
	Value to set the environment variable *TERM* to (see *TERMINFO*
	and *ENVIRONMENT*). Default: _@default_terminfo@_.

*-T*,*--title*=_TITLE_
	Initial window title. Default: _ankle_.

*-a*,*--app-id*=_ID_
	Value to set the *app-id* property on the Wayland window
	to. Default: _ankle_ (normal mode), or _ankleclient_ (server mode).

*--class*=_ID_
	Alias for *--app-id*.

*toplevel-tag*=_TAG_
	Value to set the *toplevel-tag* property on the Wayland window
	to. The compositor can use this value for session management,
	window rules etc. Default: _not set_

*-w*,*--window-size-pixels*=_WIDTHxHEIGHT_
	Set initial window width and height, in pixels. Default: _700x500_.

*-W*,*--window-size-chars*=_WIDTHxHEIGHT_
	Set initial window width and height, in characters. Default: _not set_.

*-m*,*--maximized*
	Start in maximized mode. If both *--maximized* and *--fullscreen*
	are specified, the _last_ one takes precedence.

*-F*,*--fullscreen*
	Start in fullscreen mode. If both *--maximized* and *--fullscreen*
	are specified, the _last_ one takes precedence.

*-L*,*--login-shell*
	Start a login shell, by prepending a '-' to argv[0].

*-D*,*--working-directory*=_DIR_
	Initial working directory for the client application. Default:
	_CWD of ankleclient_.

*-s*,*--server-socket*=_PATH_
	Connect to _PATH_ instead of
	*$XDG\_RUNTIME\_DIR/ankle-$WAYLAND\_DISPLAY.sock*.

*-H*,*--hold*
	Remain open after child process exits.

*-N*,*--no-wait*
	Detach the client process from the running terminal, exiting
	immediately.

*-o*,*--override*=[_SECTION_.]_KEY_=_VALUE_
	Override an option set in the configuration file. If _SECTION_ is not
	given, defaults to _main_.

*-E*,*--client-environment*
	The child process in the new terminal instance will use
	ankleclient's environment, instead of the server's.

	Environment variables listed in the *Variables set in the child
	process* section will be overwritten by the ankle server. For
	example, the new terminal will use *TERM* from the configuration,
	not ankleclient's environment.

*-d*,*--log-level*={*info*,*warning*,*error*,*none*}
	Log level, used both for log output on stderr as well as
	syslog. Default: _warning_.

*-l*,*--log-colorize*=[{*never*,*always*,*auto*}]
	Enables or disables colorization of log output on stderr.

*-v*,*--version*
	Show the version number and quit

*-e*
	Ignored; for compatibility with *xterm -e*. See *ankle*(1) for more
	details.

# EXIT STATUS

Ankleclient will exit with code 220 if there is a failure in ankleclient
itself (for example, the server socket does not exist).

If *-N*,*--no-wait* is used, ankleclient exits with code 0 as soon as
the ankle server has been instructed to open a new window.

If not, ankleclient may also exit with code 230. This indicates a
failure in the ankle server.

In all other cases the exit code is that of the client application
(i.e. the shell).

# TERMINFO

Client applications use the terminfo identifier specified by the
environment variable *TERM* (set by ankle) to determine terminal
capabilities.

Ankle uses foot's two terminfo definitions: *foot* and *foot-direct*, with
*ankle* being the default.

The difference between the two is in the number of colors they
describe; *foot* describes 256 colors and *foot-direct* 16.7 million
colors (24-bit truecolor).

Note that using the *foot* terminfo does not limit the number of
usable colors to 256; applications can still use 24-bit RGB colors. In
fact, most applications work best with *ankle* (including 24-bit
colors)). Using *\*-direct* terminfo entries has been known to crash
some ncurses applications even.

There are however applications that need a *\*-direct* terminfo entry
for 24-bit support. Emacs is one such example.

While using either *foot* or *foot-direct* is strongly recommended, it
is possible to use e.g. *xterm-256color* as well. This can be useful
when remoting to a system where foot's terminfo entries cannot easily
be installed.

Note that terminfo entries can be installed in the user's home
directory. I.e. if you do not have root access, or if there is no
distro package for foot's terminfo entries, you can install foot's
terminfo entries manually, by copying *foot* and *foot-direct* to
*~/.terminfo/f/*.

# ENVIRONMENT

## Variables used by ankleclient

*XDG\_RUNTIME\_DIR*
	Used to construct the default _PATH_ for the *--server-socket*
	option, when no explicit argument is given (see above).

*WAYLAND\_DISPLAY*
	Used to construct the default _PATH_ for the *--server-socket*
	option, when no explicit argument is given (see above).

If the socket at default _PATH_ does not exist, *ankleclient* will
fallback to the less specific path, with the following priority:
*$XDG\_RUNTIME\_DIR/ankle-$WAYLAND\_DISPLAY.sock*,
*$XDG\_RUNTIME\_DIR/ankle.sock*, */tmp/ankle.sock*.

## Variables set in the child process

*TERM*
	terminfo/termcap identifier. This is used by client applications
	to determine which capabilities a terminal supports. The value is
	set according to either the *--term* command-line option or the
	*term* config option in *ankle.ini*(5).

*COLORTERM*
	This variable is set to *truecolor*, to indicate to client
	applications that 24-bit RGB colors are supported.

*PWD*
	Current working directory (at the time of launching ankle)

*SHELL*
	Set to the launched shell, if the shell is valid (it is listed in
	*/etc/shells*).

In addition to the variables listed above, custom environment
variables may be defined in *ankle.ini*(5).

## Variables *unset* in the child process

*TERM_PROGRAM*
*TERM_PROGRAM_VERSION*
	These environment variables are set by certain other terminal
	emulators. We unset them, to prevent applications from
	misdetecting ankle.

In addition to the variables listed above, custom environment
variables to unset may be defined in *ankle.ini*(5).

# Signals

The following signals have special meaning in ankleclient:

- SIGUSR1: switch to the dark color theme (*[colors-dark]*).
- SIGUSR2: switch to the light color theme (*[colors-light]*).

When sending SIGUSR1/SIGUSR2 to an ankleclient instance, the theme is
changed in that instance only. This is different from when you send
SIGUSR1/SIGUSR2 to the server process, where all instances change the
theme.

Note: for obvious reasons, this is not supported when ankleclient is
started with *--no-wait*.

# SEE ALSO

*ankle*(1)
