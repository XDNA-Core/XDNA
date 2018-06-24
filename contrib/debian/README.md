
Debian
====================
This directory contains files used to package xdnad/xdna-qt
for Debian-based Linux systems. If you compile xdnad/xdna-qt yourself, there are some useful files here.

## xdna: URI support ##


xdna-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install xdna-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your xdnaqt binary to `/usr/bin`
and the `../../share/pixmaps/xdna128.png` to `/usr/share/pixmaps`

xdna-qt.protocol (KDE)
