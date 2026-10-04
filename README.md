# Install this photo-view version on Omarchy

## First install (after clone)

```bash
sudo pacman -S --needed base-devel git cmake ninja qt6-base qt6-declarative \
  qt6-svg qt6-imageformats glib2 gvfs libarchive tinysparql opencv
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

## Faces

Ctrl+3 can name people in local folders. The faces button on that toolbar is off until you turn it on. Turning it on indexes the folder you are viewing.

A row of faces appears above the photo grid.

- Click a face to keep only that person's photos. Click it again to show the whole folder.
- Click the caption to type a name. The other faces already grouped with them start checked. Uncheck one that is someone else; those leave together as a new unnamed person.
- Click two faces, then **Merge**. The first face you clicked is the one that stays.
- Right-click a face and choose **Mark as stranger**. They join the **?** tile at the front of the row. Later photos of those same faces are filed there too. Click the tile to see those photos.
- Open a photo and the faces are boxed. **Not this person** pulls that one face out as its own chip.

**Pause** sits on the row while a folder is indexing. F5 looks through the folder again and skips photos that have not changed. A file you deleted drops its faces.

## People

**People** in the sidebar lists everyone you have already named, from every local folder already indexed. Click one face for their photos, or two faces for photos that contain both. Strangers stay on the **?** tile in the folder view.
