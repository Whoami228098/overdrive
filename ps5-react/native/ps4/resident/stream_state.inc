// Included inside the private engine's anonymous namespace, after its mutex.
struct StreamSelection {
  std::string path;
  lt::file_index_t file{0};
  bool requested = false, ready = false;
};
struct StreamRead {
  int id, piece;
  bool issued = false, done = false, background = false;
  Clock::time_point created = Clock::now();
  std::string error;
  std::shared_ptr<std::vector<char>> data;
};
struct CachedPiece { int id, piece; std::shared_ptr<std::vector<char>> data; };
std::map<int, lt::torrent_handle> stream_handles;
std::map<int, StreamSelection> stream_selections;
std::deque<std::shared_ptr<StreamRead>> stream_reads;
std::deque<CachedPiece> stream_cache;
std::condition_variable stream_wake;
constexpr int stream_piece_limit = 16 * 1024 * 1024;

void stream_priorities(Job& job) {
  std::lock_guard<std::mutex> lock(mutex);
  auto found = stream_selections.find(job.id);
  if (found == stream_selections.end() || found->second.ready) return;
  const auto info = job.handle.torrent_file();
  if (!info) return;
  job.handle.prioritize_pieces(std::vector<lt::download_priority_t>(info->num_pieces(), lt::dont_download));
  job.handle.unset_flags(lt::torrent_flags::upload_mode);
  found->second.ready = true;
  stream_wake.notify_all();
}
void stream_issue() {
  std::lock_guard<std::mutex> lock(mutex);
  for (const auto& request : stream_reads) {
    if (!request->done && Clock::now() - request->created > std::chrono::seconds(60)) {
      request->done = true; request->error = "Timed out waiting for a verified torrent piece";
      const auto handle = stream_handles.find(request->id);
      if (handle != stream_handles.end()) {
        handle->second.reset_piece_deadline(lt::piece_index_t(request->piece));
        handle->second.piece_priority(lt::piece_index_t(request->piece), lt::dont_download);
      }
      stream_wake.notify_all();
    }
    if (request->done || request->issued) continue;
    const auto handle = stream_handles.find(request->id);
    const auto selected = stream_selections.find(request->id);
    if (handle == stream_handles.end() || selected == stream_selections.end() || !selected->second.ready) continue;
    if (handle->second.have_piece(lt::piece_index_t(request->piece))) {
      // Completed PKGs may still be queried during promotion while paused.
      handle->second.read_piece(lt::piece_index_t(request->piece));
    } else {
      handle->second.piece_priority(lt::piece_index_t(request->piece), lt::top_priority);
      handle->second.set_piece_deadline(lt::piece_index_t(request->piece), 0, lt::torrent_handle::alert_when_available);
    }
    request->issued = true;
  }
}
void stream_deliver(Job& job, const lt::read_piece_alert& piece) {
  std::lock_guard<std::mutex> lock(mutex);
  std::string error;
  const auto info = job.handle.torrent_file();
  const int index = static_cast<int>(piece.piece);
  const bool verified = !piece.error && info && job.handle.have_piece(piece.piece) &&
    piece.size == info->piece_size(piece.piece) && piece.size > 0 && piece.size <= stream_piece_limit;
  if (!verified) error = piece.error ? piece.error.message() : "Piece is not complete and hash-verified";
  std::shared_ptr<std::vector<char>> bytes;
  if (verified) {
    bytes = std::make_shared<std::vector<char>>(piece.buffer.get(), piece.buffer.get() + piece.size);
    stream_cache.push_back({job.id, index, bytes});
    while (stream_cache.size() > 2) stream_cache.pop_front();
  }
  for (const auto& request : stream_reads) if (request->id == job.id && request->piece == index && !request->done) {
    request->data = bytes; request->error = error; request->done = true;
  }
  job.handle.reset_piece_deadline(piece.piece);
  job.handle.piece_priority(piece.piece, lt::dont_download);
  stream_wake.notify_all();
}
void stream_fail(int id, const std::string& error) {
  // Caller holds the engine mutex.
  for (const auto& request : stream_reads) if ((id == 0 || request->id == id) && !request->done) {
    request->done = true; request->error = error;
  }
  stream_reads.erase(std::remove_if(stream_reads.begin(), stream_reads.end(), [&](const auto& request) {
    return request->background && (id == 0 || request->id == id);
  }), stream_reads.end());
  stream_wake.notify_all();
}
