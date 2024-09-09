
Debian
====================
This directory contains files used to package fortuneblockd/fortuneblock-qt
for Debian-based Linux systems. If you compile fortuneblockd/fortuneblock-qt yourself, there are some useful files here.

## fortuneblock: URI support ##


fortuneblock-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install fortuneblock-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your fortuneblock-qt binary to `/usr/bin`
and the `../../share/pixmaps/fortuneblock128.png` to `/usr/share/pixmaps`

fortuneblock-qt.protocol (KDE)

