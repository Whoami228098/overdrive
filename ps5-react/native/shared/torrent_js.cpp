// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "host_api.hpp"
#include "torrent_api.hpp"
#include <climits>
#include <cmath>
#include <cstring>

namespace {
bool text(JSContext* ctx, JSValueConst value, std::string& out, std::size_t max) {
  if (!JS_IsString(value)) return false;
  std::size_t size = 0;
  const char* input = JS_ToCStringLen(ctx, &size, value);
  if (!input) return false;
  const bool ok = size <= max && !std::memchr(input, 0, size);
  if (ok) out.assign(input, size);
  JS_FreeCString(ctx, input);
  return ok;
}
JSValue add(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  host::CallScope scope("torrents.add");
  std::string uri, name, path;
  double size = 0;
  char resolved[PATH_MAX];
  if (argc < 4 || !text(ctx, argv[0], uri, 16384) || !text(ctx, argv[1], name, 512) ||
      JS_ToFloat64(ctx, &size, argv[2]) || !std::isfinite(size) || size < 0 || size > 9007199254740991.0 ||
      !text(ctx, argv[3], path, PATH_MAX - 128) || !host::resolve_path(path.c_str(), resolved, sizeof resolved))
    return JS_ThrowTypeError(ctx, "torrents.add: expected magnet, name, byte size and an allowed save root");
  // resolve_path only maps sandbox aliases; all disk/network work belongs to the worker.
  const int id = torrent::add_magnet(uri.c_str(), name.c_str(), static_cast<long long>(size), resolved);
  if (!id) return JS_ThrowPlainError(ctx, "torrents.add %s: %s", path.c_str(), torrent::last_error());
  return JS_NewInt32(ctx, id);
}
JSValue control(JSContext* ctx, int argc, JSValueConst* argv, int operation) {
  host::CallScope scope("torrents.control");
  int id = 0;
  if (!argc || JS_ToInt32(ctx, &id, argv[0]) || id <= 0)
    return JS_ThrowTypeError(ctx, "torrents.control: expected a positive task id");
  if (operation == 0) torrent::pause(id);
  else if (operation == 1) torrent::resume(id);
  else torrent::remove(id, operation == 3 && argc > 1 && JS_ToBool(ctx, argv[1]) > 0);
  return JS_UNDEFINED;
}
JSValue pause(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) { return control(ctx, argc, argv, 0); }
JSValue resume(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) { return control(ctx, argc, argv, 1); }
// Cancel releases the handle only after its files and resume state are flushed; bytes remain on disk.
JSValue cancel(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) { return control(ctx, argc, argv, 2); }
JSValue remove(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) { return control(ctx, argc, argv, 3); }
JSValue poll(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  host::CallScope scope("torrents.poll");
  static const char* states[] = {"queued", "connecting", "verifying", "downloading", "paused", "completed", "failed", "cancelled"};
  torrent::Status snapshots[32];
  const int count = torrent::poll(snapshots, 32);
  JSValue out = JS_NewArray(ctx);
  for (int i = 0; i < count; ++i) {
    const auto& status = snapshots[i];
    JSValue item = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, item, "id", JS_NewInt32(ctx, status.id));
    JS_SetPropertyStr(ctx, item, "state", JS_NewString(ctx, states[status.state]));
    JS_SetPropertyStr(ctx, item, "stateCode", JS_NewInt32(ctx, status.state));
    JS_SetPropertyStr(ctx, item, "progress", JS_NewFloat64(ctx, status.progress));
    JS_SetPropertyStr(ctx, item, "total", status.total ? JS_NewFloat64(ctx, static_cast<double>(status.total)) : JS_NULL);
    JS_SetPropertyStr(ctx, item, "written", JS_NewFloat64(ctx, static_cast<double>(status.done)));
    JS_SetPropertyStr(ctx, item, "completedBytes", JS_NewFloat64(ctx, static_cast<double>(status.done)));
    JS_SetPropertyStr(ctx, item, "bytesPerSecond", JS_NewUint32(ctx, status.rate_down));
    JS_SetPropertyStr(ctx, item, "rateUp", JS_NewUint32(ctx, status.rate_up));
    JS_SetPropertyStr(ctx, item, "seeds", JS_NewInt32(ctx, status.seeds));
    JS_SetPropertyStr(ctx, item, "peers", JS_NewInt32(ctx, status.peers));
    JS_SetPropertyStr(ctx, item, "connections", JS_NewInt32(ctx, status.peers));
    JS_SetPropertyStr(ctx, item, "name", JS_NewString(ctx, status.name));
    JS_SetPropertyStr(ctx, item, "destination", JS_NewString(ctx, torrent::destination(status.id).c_str()));
    JS_SetPropertyStr(ctx, item, "error", JS_NewString(ctx, torrent::error(status.id).c_str()));
    if (status.state == torrent::completed) {
      JSValue files = JS_NewArray(ctx);
      std::uint32_t index = 0;
      for (const auto& file : torrent::files(status.id)) {
        JSValue value = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, value, "path", JS_NewString(ctx, file.path.c_str()));
        JS_SetPropertyStr(ctx, value, "size", JS_NewFloat64(ctx, static_cast<double>(file.size)));
        JS_SetPropertyUint32(ctx, files, index++, value);
      }
      JS_SetPropertyStr(ctx, item, "files", files);
    }
    JS_SetPropertyUint32(ctx, out, static_cast<std::uint32_t>(i), item);
  }
  return out;
}
}
JSValue ps5_react_torrent_api(JSContext* ctx) {
  JSValue api = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx, api, "add", JS_NewCFunction(ctx, add, "add", 4));
  JS_SetPropertyStr(ctx, api, "cancel", JS_NewCFunction(ctx, cancel, "cancel", 1));
  JS_SetPropertyStr(ctx, api, "pause", JS_NewCFunction(ctx, pause, "pause", 1));
  JS_SetPropertyStr(ctx, api, "resume", JS_NewCFunction(ctx, resume, "resume", 1));
  JS_SetPropertyStr(ctx, api, "remove", JS_NewCFunction(ctx, remove, "remove", 2));
  JS_SetPropertyStr(ctx, api, "poll", JS_NewCFunction(ctx, poll, "poll", 0));
  return api;
}
