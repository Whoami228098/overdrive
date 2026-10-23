"""Generate the resident extension without editing the copied PS5 engine."""
from pathlib import Path
import hashlib
import json

def prepare(destination):
    here = Path(__file__).resolve().parent
    original = here.parent / 'torrent/torrent_api.cpp'
    text = original.read_text(encoding='utf-8')
    def replace(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise RuntimeError('PS5 streaming extension anchor changed: ' + old[:100])
        text = text.replace(old, new)
    replace('#include "torrent_api.hpp"', '#include "torrent_api.hpp"\n#include "stream.hpp"')
    replace('int next_id = 1;', 'int next_id = 1;\n#include "stream_state.inc"')
    replace('  job.handle.unset_flags(lt::torrent_flags::upload_mode);',
            '  // The resident waits for explicit package selection before payload writes.')
    replace('jobs.emplace(command.id, Job{command.id, handle, resume_path, destination.string()});',
            'jobs.emplace(command.id, Job{command.id, handle, resume_path, destination.string()});\n'
            '            { std::lock_guard<std::mutex> lock(mutex); stream_handles[command.id] = handle; }')
    replace('      std::vector<lt::alert*> alerts;', '      stream_issue();\n      std::vector<lt::alert*> alerts;')
    replace('          if (const auto* saved = lt::alert_cast<lt::save_resume_data_alert>(alert)) {',
            '          if (const auto* piece = lt::alert_cast<lt::read_piece_alert>(alert)) { stream_deliver(job, *piece); continue; }\n'
            '          if (lt::alert_cast<lt::file_prio_alert>(alert)) { stream_priorities(job); continue; }\n'
            '          if (const auto* saved = lt::alert_cast<lt::save_resume_data_alert>(alert)) {')
    replace('        if (!item.second) entries.at(id).status.state = removed;',
            '        stream_fail(id, "Torrent removed"); stream_handles.erase(id); stream_selections.erase(id);\n'
            '        if (!item.second) entries.at(id).status.state = removed;')
    replace('          if (status.is_seeding && !job.finishing) { job.finishing = job.pausing = true; job.handle.pause(); }',
            '          // BGFT completion, rather than wanted-piece count, owns resident completion.')
    replace('  std::lock_guard<std::mutex> lock(mutex); running = false;',
            '  std::lock_guard<std::mutex> lock(mutex); running = false;\n'
            '  stream_fail(0, "Resident torrent session stopped"); stream_handles.clear(); stream_selections.clear(); stream_cache.clear();')
    replace('} // namespace torrent', '#include "stream_api.inc"\n} // namespace torrent')
    target = Path(destination) / 'resident-torrent.cpp'
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists() or target.read_text(encoding='utf-8') != text:
        target.write_text(text, encoding='utf-8')
    (target.parent / 'stream-source.json').write_text(json.dumps({
        'original_sha256': hashlib.sha256(original.read_bytes()).hexdigest(),
        'generated_sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
        'original_unchanged': True}, indent=2) + '\n')
    return target
