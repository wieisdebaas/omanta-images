# Install this photo-view version on Omarchy

Eén opdracht — pull, schoon bouwen, installeren, en schakelen naar omanta:

```
sudo pacman -S --needed base-devel git cmake ninja qt6-base qt6-declarative \
  qt6-svg qt6-imageformats glib2 gvfs libarchive tinysparql
git clone https://github.com/wieisdebaas/omanta-images.git
cd omanta-images
chmod +x bin/install
./bin/install
~/.local/bin/omanta-switch omanta
```

`.\bin\install` is idempotent: het valls een bestaande `build/` weg voordat het bouwt, dus een herrun heeft altijd een schone staat als uitkomst. Na een `git pull` volstaat `./bin/install` opnieuw — de symlink wijst altijd naar de nieuw gebouwde binary.

Om terug te gaan naar Nautilus zonder de build te verwijderen:

```
~/.local/bin/omanta-switch nautilus
```

## Uninstall

```
omanta-switch nautilus && omanta-switch remove-menu   # stock default, no menu row
sudo pacman -R omanta
```
