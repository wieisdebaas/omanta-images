# Recursive photo view (Ctrl+3)

Date: 2026-09-28  
Status: approved (relative-path labels + discovery 1–8 + thumbnail pipeline rules)

## Goal

Ctrl+3 shows a Geeqie-style photo browser for the **current location** (local or remote via GIO, including `smb://`). It lists every image under that location and its subfolders, with relative-path labels, without blocking the UI on large shares.

## Non-goals (v1)

- Videos in the same grid
- Live file monitors on every subfolder
- A toggle to disable recursion (Ctrl+3 is always recursive)
- Changing list/icon view behaviour
- A brand-new parallel thumbnail stack (extend the existing photo provider/cache)

## Approach

Add a dedicated **`PhotoModel`** (`QAbstractListModel`, same roles as `DirectoryModel` / `SearchModel`).

Do **not** recurse inside `DirectoryModel`, and do **not** overload `SearchModel` with an empty query.

The existing `FileSortFilterModel.recursive` flag is a stub; remove its photo-view usage once `PhotoModel` is wired.

Thumbnails use the **existing** `PhotoThumbnailProvider` + `PhotoThumbnailCache` (`~/.cache/omanta/photo-thumbnails/`, RAM LRU, size buckets, async pool). Discovery and decoding stay separate pipelines.

## Behaviour

### Listing (PhotoModel)

- Breadth-first GIO walk under the tab’s current path (same cancellable / generation pattern as `SearchModel`).
- Keep only entries whose content type starts with `image/`.
- Skip descending into symlink directories (loop safety).
- Hidden/backup handling follows the tab’s Ctrl+H / `showHidden` state.
- Few directories in flight at once (~4).
- **Batch row inserts** (per enumerator batch / flush) so sorting and GridView layout are not paid per file.
- `PhotoModel` streams **metadata only** — never decode images on the model or UI thread.

### Labels and identity

- `name` and `displayName` = path **relative to the photo root** (e.g. `Jan/DSC_0001.jpg`).
- `filePath` = full location via `Location::descend` so preview, open, and drag work on SMB.
- Relative `name` keeps Tab selection unique across folders that share basenames.

### Streaming and status

- Append rows as images are found (current folder first, then deeper).
- While scanning: status like `Scanning… 128 photos`.
- When done: `1 284 photos` (or `1 item` singular).
- If a cap is hit: note that in the status line (e.g. `10 000 photos (limit reached)`).

### Cancellation

Cancel discovery (and deprioritize/cancel obsolete thumbnail work) when:

- Path changes
- View mode leaves `photo`
- Tab / window teardown
- Explicit reload starts a new generation

### Caps (safety limits — not memory management)

| Cap | Default | Purpose |
|-----|---------|---------|
| Max photos | 10 000 | Avoid unbounded UI lists |
| Max dirs visited | 2 000 | SMB tree bound |
| Max depth | 20 | Runaway nesting |

RAM/disk thumbnail budgets are handled by the photo cache LRU and disk cache, not these caps.

### Live updates

- **No** recursive `GFileMonitor` in v1.
- F5 / reload restarts the walk.
- Navigating away and back also restarts.

### Sort

- When the tab enters photo mode, set sort to **modified, descending** (newest first).
- The user can change sort afterward via the view-options menu; that updates the shared tab sort as today.
- Switching back to list/icon keeps whatever sort the tab currently has (no separate per-mode sort store in v1).

### Mount / errors

- Same mount cue as directory listing if GIO reports “not mounted”.
- Unreadable subfolders are skipped; walk continues.
- Root failure sets an error message like `DirectoryModel`.

## Thumbnail pipeline (extend existing)

Architecture:

```
PhotoModel (metadata only)
  → Qt Quick virtualized PhotoGridView
  → image://photo/… (priority + bucket + mtime/size + path)
  → PhotoThumbnailProvider (async pool)
  → PhotoThumbnailCache RAM LRU + ~/.cache/omanta/photo-thumbnails/
```

Rules:

1. PhotoModel never decodes images.
2. Priorities: visible highest, near-viewport prefetch medium, background lowest (URL priority → thread pool).
3. Persistent disk cache under `~/.cache/omanta/photo-thumbnails/` (already).
4. Bounded in-memory LRU; cleared on app close; disk remains (already).
5. Buckets ~256 / 512 / 1024; pick smallest sufficient (already).
6. Generation/cancel aligned with provider responses; leaving photo mode drops obsolete requests.
7. Prefetch ~1–3 viewport heights in PhotoGridView; reprioritize on scroll.
8. Grid stays virtualized (`GridView`); never keep 10k decoded images.
9. Discovery continues while thumbnails generate independently.
10. Cache invalidation: canonical location + size + mtime (no full-file hash). Orientation/dimensions later.
11. Profile local + SMB for perceived latency and smooth scrolling; optimize that first.

Wire `PhotoGridView` to `Thumbnails.photoSource(...)` (not the generic freedesktop `Thumbnails.source`) so the photo cache and priorities apply.

## UI wiring

- `Tab.qml`: in photo mode, `files` → sort proxy over `PhotoModel` (active only while `viewMode === "photo"`).
- Remove the stub `photoProxy` over `dirModel` with `imagesOnly`/`recursive`.
- Status text uses PhotoModel scanning + count (+ capped).
- Shortcuts / tooltip already mention Ctrl+3.

## Tests

- Local tree: images in root + nested dirs appear with relative paths; non-images excluded.
- Hidden dir skipped unless show-hidden.
- Symlink to a parent dir is not descended (no loop / explosion).
- Cap: exceeding max photos sets `capped` and stops growing.
- Cancel: changing path mid-walk does not append into the new listing.
- Content types: only `image/*`.
- Existing `tst_thumbnails` photo-cache tests remain the guarantee for disk/RAM behaviour.

## Success criteria

- Opening `smb://…/Foto's 2011` (or a local album with subfolders) under Ctrl+3 fills the grid incrementally with all nested photos.
- Labels show relative paths.
- Leaving the view or folder stops SMB discovery promptly; scrolling stays smooth via async photo thumbs.
- List/icon views remain single-directory.
