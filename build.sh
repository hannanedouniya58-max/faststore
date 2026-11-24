#!/bin/bash
set -eux

sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --fix-missing \
  curl build-essential cmake pkg-config fonts-dejavu-core imagemagick unzip file dotnet-sdk-8.0 || true

API=https://api.github.com/repos/PacBrew/pacbrew-pacman/releases/latest
JSON=$(curl -sL "$API")
echo "$JSON" | head -c 500
ASSETS=$(echo "$JSON" | grep -oE '"browser_download_url": "[^"]+"' | cut -d'"' -f4 || true)
echo "$ASSETS"

DEB=$(echo "$ASSETS" | grep -iE '\.deb$' | head -n1 || true)
TAR=$(echo "$ASSETS" | grep -iE '\.(tar\.gz|tgz|tar\.xz)$' | head -n1 || true)

if [ -n "$DEB" ]; then
  curl -fL -o /tmp/p.deb "$DEB"
  sudo dpkg -i /tmp/p.deb || sudo apt-get -f install -y
elif [ -n "$TAR" ]; then
  curl -fL -o /tmp/p.tar "$TAR"
  mkdir -p /tmp/pt && tar -xf /tmp/p.tar -C /tmp/pt
  sudo cp -r /tmp/pt/* /usr/local/
else
  echo "AUCUN PAQUET TROUVE"; exit 1
fi

export PATH=/usr/local/bin:$PATH
command -v pacbrew-pacman
sudo pacbrew-pacman -Sy --noconfirm
sudo pacbrew-pacman -S --noconfirm ps4-openorbis ps4-openorbis-portlibs

export OO_PS4_TOOLCHAIN=/opt/pacbrew/ps4/openorbis
ls -la $OO_PS4_TOOLCHAIN/bin/linux/ || true

git clone --depth 1 https://github.com/DaveGamble/cJSON external/cJSON

make

mkdir -p stage/assets out
cp /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf stage/assets/font.ttf
printf 'APP_TYPE=fpkg\nAPP_VER=01.00\nCATEGORY=gd\nCONTENT_ID=IV0000-FST000001_00-FASTSTORE0000000\nTITLE=FastStore\nTITLE_ID=FST000001\nVERSION=01.00\n' > stage/param.sfo
convert -size 512x512 gradient:'#0e1016-#28beff' -gravity center -pointsize 96 -fill white -annotate 0 FS stage/icon0.png

$OO_PS4_TOOLCHAIN/bin/linux/create-gp4 --content-id=IV0000-FST000001_00-FASTSTORE0000000 --out=pkg.gp4 \
  --files="sce_sys/param.sfo;stage/param.sfo;sce_sys/icon0.png;stage/icon0.png;eboot.bin;eboot.bin;assets/font.ttf;stage/assets/font.ttf"

if [ -x "$OO_PS4_TOOLCHAIN/bin/linux/liborbispkg" ]; then
  $OO_PS4_TOOLCHAIN/bin/linux/liborbispkg pkg.gp4 out
elif command -v dotnet >/dev/null; then
  git clone --depth 1 https://github.com/maxton/LibOrbisPkg
  dotnet publish LibOrbisPkg -c Release -o /opt/lop
  dotnet /opt/lop/*.dll pkg.gp4 out
else
  echo "PAS DE PACKAGEUR"; exit 1
fi
