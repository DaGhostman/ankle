# Contributing to ankle

Contributions are welcome: bug reports, feature requests and pull
requests on [GitHub](https://github.com/DaGhostman/ankle).


## AI-assisted contributions

Ankle itself is developed with the help of AI coding assistants (see
[Made with AI](README.md#made-with-ai)), and contributions made with
AI tools are welcome too. Please:

* Say so in the pull request, and add a `Co-Authored-By:` trailer
  naming the tool or model to commits containing AI-written changes.
* Review, build and test the changes yourself before submitting
  them. You are responsible for what you submit, whoever (or
  whatever) wrote it.
* Keep issues and pull request descriptions to the point; please don't
  paste unreviewed AI output into them.


## Pull requests

* Keep pull requests focused on one change, and split larger work
  into commits that each build and pass the tests.
* Follow the style of the surrounding code (see `.editorconfig`).
* Run the tests (`ninja -C <build-dir> test`) before submitting.
* Update the man pages in `doc/` when changing options, key bindings
  or escape sequences.


## Upstream (foot)

Ankle is a fork of [foot](https://codeberg.org/dnkl/foot), and is
not affiliated with it. Please do not send ankle bug reports or patches
to foot. Bugs that also occur in foot can be reported there too,
following foot's own contribution guidelines.
