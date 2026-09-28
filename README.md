# Install this photo-view version on Omarchy

## First install (after clone)

```bash
sudo pacman -S --needed base-devel git cmake ninja qt6-base qt6-declarative \
  qt6-svg qt6-imageformats glib2 gvfs libarchive tinysparql
git clone https://github.com/wieisdebaas/omanta-images.git
cd omanta-images
chmod +x bin/install
./bin/install
~/.local/bin/omanta-switch omanta
```

That builds into `build/`, symlinks `~/.local/bin/omanta` to the new binary, and switches Omarchy to use omanta.

If you still have the packaged `/usr/bin/omanta`, remove it so PATH does not pick the old binary:

```bash
sudo pacman -R omanta
```

Then start with `~/.local/bin/omanta` (or close every omanta window and open a folder again).

## Rebuild / reinstall locally

From the repo root, after a `git pull` or any local change:

```bash
./bin/install
```

`./bin/install` is idempotent: it wipes `build/`, rebuilds clean, and refreshes the `~/.local/bin/omanta` symlink. Close open omanta windows first, then start again to pick up the new binary.

Faster incremental rebuild (symlink already points at `build/omanta`):

```bash
cmake --build build -j"$(nproc)"
```

## Switch back to Nautilus

```bash
~/.local/bin/omanta-switch nautilus
```

## Uninstall

```bash
omanta-switch nautilus && omanta-switch remove-menu   # stock default, no menu row
sudo pacman -R omanta
```
