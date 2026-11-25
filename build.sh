
#!/bin/bash
set -eux

# ==========================================
# 1. INSTALLATION DES DEPENDANCES
# ==========================================

sudo apt-get update

sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --fix-missing \
  curl build-essential cmake pkg-config fonts-dejavu-core \
  imagemagick unzip file zstd dotnet-sdk-8.0 || true

# ==========================================
# 2. TELECHARGEMENT DE PACBREW
# ==========================================

API=https://api.github.com/repos/PacBrew/pacbrew-pacman/releases/latest

JSON=$(curl -fsSL \
  -H "Authorization: Bearer $GH_TOKEN" \
  -H "Accept: application/vnd.github+json" "$API")

ASSETS=$(echo "$JSON" | grep -oE '"browser_download_url": "[^"]+"' | cut -d'"' -f4 || true)

DEB=$(echo "$ASSETS" | grep -iE '\.deb$' | head -n1 || true)
TAR=$(echo "$ASSETS" | grep -iE '\.(tar\.gz|tgz|tar\.xz|tar\.zst)$' | head -n1 || true)

if [ -z "$DEB" ] && [ -z "$TAR" ]; then
  for U in \
    "https://github.com/PacBrew/pacbrew-pacman/releases/latest/download/pacbrew-pacman_1.0.0_amd64.deb" \
    "https://github.com/PacBrew/pacbrew-pacman/releases/latest/download/pacbrew-pacman_1.0_amd64.deb"
  do
    if curl -fsSL -o /tmp/p.deb "$U"; then
      DEB="$U"
      break
    fi
  done
fi

if [ -n "$DEB" ]; then
  if [ ! -f /tmp/p.deb ]; then
    curl -fsSL -o /tmp/p.deb "$DEB"
  fi

  sudo dpkg -i /tmp/p.deb || sudo apt-get -f install -y

elif [ -n "$TAR" ]; then
  curl -fsSL -o /tmp/p.tar "$TAR"
  mkdir -p /tmp/pt
  tar -xf /tmp/p.tar -C /tmp/pt
  sudo cp -r /tmp/pt/* /usr/local/

else
  echo "AUCUN PAQUET PACBREW TROUVE"
  exit 1
fi

# ==========================================
# 3. CONFIGURATION PACBREW
# ==========================================

export PATH=/usr/local/bin:$PATH

command -v pacbrew-pacman

# Synchronisation avec 5 tentatives

sync_ok=0

for i in 1 2 3 4 5; do
  echo "Synchronisation Pacbrew : tentative $i/5"

  if sudo pacbrew-pacman -Sy --noconfirm; then
    sync_ok=1
    break
  fi

  echo "Serveur Pacbrew inaccessible."
  echo "Nouvelle tentative dans 20 secondes..."

  sleep 20
done

if [ "$sync_ok" -ne 1 ]; then
  echo "ERREUR : impossible de synchroniser Pacbrew."
  exit 1
fi

# Installation des outils PS4

sudo pacbrew-pacman -S --noconfirm \
  ps4-openorbis ps4-openorbis-portlibs

# ==========================================
# 4. CONFIGURATION OPENORBIS
# ==========================================

export OO_PS4_TOOLCHAIN=/opt/pacbrew/ps4/openorbis

ls -la "$OO_PS4_TOOLCHAIN/bin/linux/"

# ==========================================
# 5. DEPENDANCE CJSON
# ==========================================

git clone --depth 1 \
  https://github.com/DaveGamble/cJSON external/cJSON

# ==========================================
# 6. COMPILATION FASTSTORE
# ==========================================

make

# ==========================================
# 7. PREPARATION DU PKG
# ==========================================

mkdir -p stage/assets out

cp /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
  stage/assets/font.ttf

printf 'APP_TYPE=fpkg\nAPP_VER=01.00\nCATEGORY=gd\nCONTENT_ID=IV0000-FST000001_00-FASTSTORE0000000\nTITLE=FastStore\nTITLE_ID=FST000001\nVERSION=01.00\n' > stage/param.sfo

convert -size 512x512 \
  gradient:'#0e1016-#28beff' \
  -gravity center \
  -pointsize 96 \
  -fill white \
  -annotate 0 FS \
  stage/icon0.png

# ==========================================
# 8. GENERATION GP4
# ==========================================

"$OO_PS4_TOOLCHAIN/bin/linux/create-gp4" \
  --content-id=IV0000-FST000001_00-FASTSTORE0000000 \
  --out=pkg.gp4 \
  --files="sce_sys/param.sfo;stage/param.sfo;sce_sys/icon0.png;stage/icon0.png;eboot.bin;eboot.bin;assets/font.ttf;stage/assets/font.ttf"

# ==========================================
# 9. GENERATION DU PKG FINAL
# ==========================================

if [ -x "$OO_PS4_TOOLCHAIN/bin/linux/liborbispkg" ]; then

  "$OO_PS4_TOOLCHAIN/bin/linux/liborbispkg" pkg.gp4 out

elif command -v dotnet >/dev/null; then

  git clone --depth 1 \
    https://github.com/maxton/LibOrbisPkg

  dotnet publish LibOrbisPkg -c Release -o /opt/lop

  dotnet /opt/lop/*.dll pkg.gp4 out

else

  echo "ERREUR : AUCUN PACKAGEUR DISPONIBLE"
  exit 1

fi

# ==========================================
# 10. VERIFICATION FINALE
# ==========================================

echo "======================================"
echo "BUILD FASTSTORE TERMINE"
echo "======================================"

find out -type f -name "*.pkg" -ls

