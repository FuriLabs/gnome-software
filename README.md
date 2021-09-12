[![Build Status](https://gitlab.gnome.org/GNOME/gnome-software/badges/master/build.svg)](https://gitlab.gnome.org/GNOME/gnome-software/pipelines)

# Software

[Software](https://wiki.gnome.org/Apps/Software) allows users to easily find,
discover and install apps. It also keeps their OS, apps and devices up to date
without them having to think about it, and gives them confidence that their
system is up to date. It supports popular distributions, subject to those
distributions maintaining their own distro-specific integration code.

The specific use cases that Software covers are [documented in more detail](./doc/use-cases.md).

# Features

A plugin system is used to access software from different sources.
Plugins are provided for:
 - Traditional package installation via PackageKit (e.g. Debian package, RPM).
 - Next generation packages: [Flatpak](https://flatpak.org/) and [Snap](https://snapcraft.io/).
 - Firmware updates.
 - Ratings and reviews using [ODRS](https://odrs.gnome.org/).

Software supports showing metadata that closely matches the [AppStream](https://www.freedesktop.org/wiki/Distributions/AppStream/) format.

Software runs as a background service to provide update notifications and be a search provider for [GNOME Shell](https://wiki.gnome.org/Projects/GnomeShell).

# Contact

For questions about how to use Software, ask on [Discourse](https://discourse.gnome.org/tag/gnome-software).

Bug reports and merge requests should be filed on [GNOME GitLab](https://gitlab.gnome.org/GNOME/gnome-software).

For development discussion, join us on `#gnome-software` on [irc.gnome.org](https://wiki.gnome.org/Community/GettingInTouch/IRC).

# Building

Software uses a number of plugins and depending on your operating system you
may want to disable or enable certain ones. For example on Fedora Silverblue
you'd want to disable the packagekit plugin as it wouldn't work. See the list
in `meson_options.txt` and use e.g. `-Dpackagekit=false` in the `meson` command
below.

Build locally with:
```
$ meson --prefix $PWD/install build/
$ ninja -C build/ all install
$ killall gnome-software
# On Fedora, RHEL, etc:
$ XDG_DATA_DIRS=install/share:$XDG_DATA_DIRS LD_LIBRARY_PATH=install/lib64/:$LD_LIBRARY_PATH ./install/bin/gnome-software
# On Debian, Ubuntu, etc:
$ XDG_DATA_DIRS=install/share:$XDG_DATA_DIRS LD_LIBRARY_PATH=install/lib/x86_64-linux-gnu/:$LD_LIBRARY_PATH ./install/bin/gnome-software
```

# Debugging

Running with `--verbose` will give detailed logging information.

# Maintainers

Software is maintained by several co-maintainers, as listed in `gnome-software.doap`.
All changes to Software need to be reviewed by at least one co-maintainer (who
can’t review their own changes). Larger decisions need input from at least two
co-maintainers.
