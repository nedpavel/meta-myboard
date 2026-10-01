#!/bin/bash
# Baut den Offscreen-Layouttest nativ (nur zur Entwicklung, nicht im Image).
set -e
NAT=/home/manolo/Embedded/build/tmp/work/corei7-64-poky-linux/myboard-gui/1.0/recipe-sysroot-native
SRC="$(dirname "$0")"
OUT=/tmp/layouttest
rm -rf "$OUT"; mkdir -p "$OUT"
cp "$SRC"/*.cpp "$SRC"/*.h "$SRC"/*.qrc "$SRC"/*.ui "$SRC"/*.tsv "$SRC"/*.jpg "$OUT/" 2>/dev/null || true
cd "$OUT"
rm -f main.cpp                      # eigener Einstiegspunkt im Test
"$NAT/usr/libexec/uic" infopage.ui -o ui_infopage.h
"$NAT/usr/libexec/moc" mainwindow.h -o moc_mainwindow.cpp
"$NAT/usr/libexec/moc" canreader.h  -o moc_canreader.cpp
"$NAT/usr/libexec/rcc" --name images images.qrc -o qrc_images.cpp
"$NAT/usr/libexec/rcc" --name can    can.qrc    -o qrc_can.cpp
g++ -std=c++17 -fPIC \
    -I"$NAT/usr/include" -I"$NAT/usr/include/QtCore" \
    -I"$NAT/usr/include/QtGui" -I"$NAT/usr/include/QtWidgets" -I. \
    _layouttest.cpp mainwindow.cpp canmatrix.cpp canreader.cpp canopen.cpp \
    moc_mainwindow.cpp moc_canreader.cpp qrc_images.cpp qrc_can.cpp \
    -L"$NAT/usr/lib" -lQt6Widgets -lQt6Gui -lQt6Core -o layouttest
export LD_LIBRARY_PATH="$NAT/usr/lib"
export QT_PLUGIN_PATH="$NAT/usr/lib/plugins"
export QT_QPA_PLATFORM=offscreen
./layouttest
