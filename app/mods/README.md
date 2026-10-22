# Local mod data

The Mods page is optional and lazy: no community index or per-title pack is
parsed until that page is opened. It does not depend on built-in game entries.
The build includes empty indexes. A separately prepared, pinned pack belongs at
`/data/homebrew/trapstar-mods` on PS5, or in `/app0/mods` in a PS4 package.

An entry binds the **console host**, game platform, exact title ID, update,
payload size and SHA-256. PS4-host cheats and PS4-on-PS5 cheats are separate.
The installed title/version is read from actual param.json or binary param.sfo,
not inferred from the store release. The installer rejects a different region
or update; an uninstalled game's pack may only be exported to app-owned data.

Supported data targets are CheatRunner/etaHEN cheats on PS5, CheatRunner XML
patches on PS5, and GoldHEN cheats/game_patch XML on PS4. Existing configured
directories indicate available files, **not that the loader is running**.
The store does not install plugins, inject payloads, edit loader settings or
automatically enable memory patches. The UI explains the required activation
and refresh step. Skin/file-replacement archives are not guessed from cheats;
they need real compatible assets and a supported loader before being offered.

Small regular-file payloads are capped at 1 MiB. Hashing yields between 16 KiB
chunks. Installation uses atomic native file copies, verified backups and
app-owned receipts. Restore only replaces a destination still matching the
installed payload, so another application's edits are preserved.

Community source URL, immutable revision and authors remain on every record.
The prepared pack includes its sources and notices. SHA-256 uses unmodified
@noble/hashes 2.0.1 modules (MIT), recorded in vendor/noble-hashes/PROVENANCE.json.

Run `node --test app/mods/core.test.mjs` from the repository root.
