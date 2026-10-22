// ../pub-game-store/native/state/archive-password.js
// reconstructed from the esbuild bundle in eboot.bin


  function archivePassword(release) {
    const sources = [release, ...(release.parts || []).flatMap((part) => part.sources)];
    const passwords = /* @__PURE__ */ new Set();
    for (const source2 of sources) {
      const value = source2.archivePassword;
      if (value == null || value === "") continue;
      if (typeof value !== "string" || value.length > 1024 || value.includes("\0"))
        throw new Error("Invalid archive password metadata.");
      passwords.add(value);
    }
    if (passwords.size > 1) throw new Error("Archive volumes have conflicting passwords. Choose another complete source.");
    return passwords.values().next().value;
  }

  // A saved queue may predate catalog enrichment. Match the exact source, never
  // a title or a neighboring edition, before copying its published password.
  function refreshArchiveMetadata(release, current, identity = (value) => value) {
    const url = (item) => item?.magnet || item?.url;
    const same = (a,b) => {
      if (!url(a) || !url(b) || a.browserUrl !== b.browserUrl) return false;
      try { return identity(url(a)) === identity(url(b)); } catch { return false; }
    };
    const copy = (stored, source) => {
      if (Object.prototype.hasOwnProperty.call(source,'archivePassword')) stored.archivePassword = source.archivePassword;
      if (source.archiveMetadata) stored.archiveMetadata = source.archiveMetadata;
      if (source.topicUrl) stored.topicUrl = source.topicUrl;
      if (source.archiveFile) stored.archiveFile = source.archiveFile;
      if (typeof source.platform === 'string') stored.platform = source.platform;
    };
    const match = current.find(item => same(release,item));
    if (match) copy(release,match);
    const sources = current.flatMap(item => [item,...(item.parts || []).flatMap(part=>part.sources)]);
    for (const source of (release.parts || []).flatMap(part=>part.sources)) {
      const found = sources.find(item=>same(source,item));
      if (found) copy(source,found);
    }
    return archivePassword(release);
  }

export { archivePassword, refreshArchiveMetadata };
