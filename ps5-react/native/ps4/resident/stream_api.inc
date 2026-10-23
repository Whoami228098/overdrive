namespace stream {
std::string key(const std::string& uri, std::string& error) {
  lt::error_code ec;
  const auto parsed = lt::parse_magnet_uri(uri, ec);
  if (ec || (!parsed.info_hashes.has_v1() && !parsed.info_hashes.has_v2())) {
    error = ec ? ec.message() : "Magnet has no supported info hash"; return {};
  }
  error.clear(); return hash_key(parsed.info_hashes);
}
bool select(int id, const std::string& path, std::string& error) {
  std::lock_guard<std::mutex> lock(mutex);
  error.clear();
  auto selected = stream_selections.find(id);
  if (selected != stream_selections.end()) {
    if (selected->second.path != path) error = "A different package was already selected";
    return error.empty() && selected->second.ready;
  }
  const auto entry = entries.find(id);
  const auto found = stream_handles.find(id);
  if (entry == entries.end() || found == stream_handles.end()) return false;
  if (entry->second.status.state == failed) { error = entry->second.error; return false; }
  if (std::none_of(entry->second.files.begin(), entry->second.files.end(), [&](const File& f) { return f.path == path; })) return false;
  const auto info = found->second.torrent_file();
  if (!info) return false;
  if (info->piece_length() <= 0 || info->piece_length() > stream_piece_limit || info->num_pieces() > 4 * 1024 * 1024) {
    error = "Torrent pieces exceed the resident memory budget (16 MiB per piece)"; return false;
  }
  std::vector<lt::download_priority_t> priorities(info->num_files(), lt::dont_download);
  bool matched = false;
  lt::file_index_t file{0};
  for (const auto index : info->files().file_range()) {
    if ((fs::path(entry->second.destination) / info->files().file_path(index)).string() == path) {
      file = index; priorities[static_cast<int>(index)] = lt::default_priority; matched = true; break;
    }
  }
  if (!matched) { error = "Selected package is absent from validated metadata"; return false; }
  stream_selections.emplace(id, StreamSelection{path, file, true, false});
  // file_prio_alert acknowledges async disk priority setup before deadlines.
  found->second.prioritize_files(priorities);
  wake.notify_all(); return false;
}
bool read(int id, std::uint64_t offset, std::size_t limit, std::vector<char>& bytes, std::string& error) {
  bytes.clear(); error.clear();
  if (!limit || limit > 256 * 1024) { error = "Invalid bounded stream read"; return false; }
  std::unique_lock<std::mutex> lock(mutex);
  const auto selected = stream_selections.find(id);
  const auto found = stream_handles.find(id);
  if (stopping || !running || selected == stream_selections.end() || !selected->second.ready || found == stream_handles.end()) {
    error = "Package stream is not ready"; return false;
  }
  const auto info = found->second.torrent_file();
  if (!info || offset >= static_cast<std::uint64_t>(info->files().file_size(selected->second.file))) {
    error = "Package offset is outside metadata"; return false;
  }
  const auto slice = info->map_file(selected->second.file, static_cast<std::int64_t>(offset), 1);
  const int piece = static_cast<int>(slice.piece);
  const auto count = std::min<std::uint64_t>({limit,
    static_cast<std::uint64_t>(info->piece_size(slice.piece) - slice.start),
    static_cast<std::uint64_t>(info->files().file_size(selected->second.file)) - offset});
  std::shared_ptr<std::vector<char>> data;
  for (const auto& cached : stream_cache) if (cached.id == id && cached.piece == piece) { data = cached.data; break; }
  if (!data) {
    if (stream_reads.size() >= 4) { error = "Package stream request budget is full"; return false; }
    auto request = std::make_shared<StreamRead>(); request->id = id; request->piece = piece;
    stream_reads.push_back(request); wake.notify_all();
    const bool signalled = stream_wake.wait_for(lock, std::chrono::seconds(60), [&] { return request->done || stopping || !running; });
    stream_reads.erase(std::remove(stream_reads.begin(), stream_reads.end(), request), stream_reads.end());
    data = request->data;
    if (!signalled || !request->done || !data || !request->error.empty()) {
      error = !request->error.empty() ? request->error : "Timed out waiting for a verified torrent piece";
      // Demand priorities are cleared after timeout, including missing peers.
      const auto current = stream_handles.find(id);
      if (current != stream_handles.end()) {
        current->second.reset_piece_deadline(slice.piece);
        current->second.piece_priority(slice.piece, lt::dont_download);
      }
      return false;
    }
  }
  if (slice.start < 0 || static_cast<std::uint64_t>(slice.start) + count > data->size()) { error = "Invalid verified piece bounds"; return false; }
  bytes.assign(data->begin() + slice.start, data->begin() + slice.start + static_cast<std::size_t>(count));
  return true;
}
void suspend(int id) {
  std::lock_guard<std::mutex> lock(mutex);
  stream_fail(id, "Package stream paused");
  const auto found = stream_handles.find(id);
  if (found != stream_handles.end()) {
    found->second.clear_piece_deadlines();
    const auto info = found->second.torrent_file();
    if (info) found->second.prioritize_pieces(std::vector<lt::download_priority_t>(info->num_pieces(), lt::dont_download));
  }
}
bool peek(int id, std::uint64_t offset, std::size_t limit, std::vector<char>& bytes, std::string& error) {
  bytes.clear(); error.clear();
  if (!limit || limit > 0x440) { error = "Invalid bounded PKG prefix read"; return false; }
  std::lock_guard<std::mutex> lock(mutex);
  const auto selected = stream_selections.find(id); const auto found = stream_handles.find(id);
  if (stopping || !running || selected == stream_selections.end() || !selected->second.ready || found == stream_handles.end()) return false;
  const auto info = found->second.torrent_file();
  if (!info || offset >= static_cast<std::uint64_t>(info->files().file_size(selected->second.file))) { error = "Invalid PKG prefix offset"; return false; }
  const auto slice = info->map_file(selected->second.file, static_cast<std::int64_t>(offset), 1);
  const int piece = static_cast<int>(slice.piece);
  std::shared_ptr<std::vector<char>> data;
  for (auto it = stream_reads.begin(); it != stream_reads.end(); ++it) if ((*it)->background && (*it)->id == id && (*it)->piece == piece) {
    if (!(*it)->done) return false;
    data = (*it)->data; error = (*it)->error; stream_reads.erase(it);
    if (!error.empty() || !data) return false;
    break;
  }
  if (!data) for (const auto& cached : stream_cache) if (cached.id == id && cached.piece == piece) { data = cached.data; break; }
  if (!data) {
    const auto background = std::count_if(stream_reads.begin(), stream_reads.end(), [](const auto& request) { return request->background; });
    if (background >= 2 || stream_reads.size() >= 4) return false; // Reserve two data feeder slots.
    auto request = std::make_shared<StreamRead>(); request->id = id; request->piece = piece; request->background = true;
    stream_reads.push_back(request); wake.notify_all(); return false;
  }
  const auto count = std::min<std::uint64_t>({limit, static_cast<std::uint64_t>(info->piece_size(slice.piece) - slice.start),
    static_cast<std::uint64_t>(info->files().file_size(selected->second.file)) - offset});
  if (slice.start < 0 || static_cast<std::uint64_t>(slice.start) + count > data->size()) { error = "Invalid verified prefix bounds"; return false; }
  bytes.assign(data->begin() + slice.start, data->begin() + slice.start + static_cast<std::size_t>(count)); return true;
}
} // namespace stream
