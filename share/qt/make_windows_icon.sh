#!/bin/bash
# create multiresolution windows icon
#mainnet
ICON_SRC=../../src/qt/res/icons/xdna.png
ICON_DST=../../src/qt/res/icons/xdna.ico
convert ${ICON_SRC} -resize 16x16 xdna-16.png
convert ${ICON_SRC} -resize 32x32 xdna-32.png
convert ${ICON_SRC} -resize 48x48 xdna-48.png
convert xdna-16.png xdna-32.png xdna-48.png ${ICON_DST}
#testnet
ICON_SRC=../../src/qt/res/icons/xdna_testnet.png
ICON_DST=../../src/qt/res/icons/xdna_testnet.ico
convert ${ICON_SRC} -resize 16x16 xdna-16.png
convert ${ICON_SRC} -resize 32x32 xdna-32.png
convert ${ICON_SRC} -resize 48x48 xdna-48.png
convert xdna-16.png xdna-32.png xdna-48.png ${ICON_DST}
rm xdna-16.png xdna-32.png xdna-48.png
