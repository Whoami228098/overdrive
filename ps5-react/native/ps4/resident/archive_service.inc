// Copyright (C) 2026 PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Included inside the resident's private namespace. The resident, never the
// foreground UI, owns copying verified torrent bytes and archive decoding.
bool archived_package(const Job& job) {return !job.archive_source.empty()&&job.path!=job.archive_source;}
std::string archive_root(const Job& job){return std::string(base)+"/archives/"+job.key;}
std::string archive_cache(const Job& job){return archive_root(job)+"/input"+lower(fs::path(job.archive_source).extension().string());}
bool saved_archive(const Job& job){struct stat st{};return !job.archive_source.empty()&&::lstat(archive_cache(job).c_str(),&st)==0;}
void queue_archive(int id) {
  {std::lock_guard<std::mutex> lock(jobs_mutex);auto& current=jobs.at(id);current.archive_pending=true;++current.readers;}
  {std::lock_guard<std::mutex> lock(queues_mutex);archive_queue.push_back(id);}queue_wake.notify_all();
}
bool within_path(const fs::path& path,const fs::path& root) {
  const auto relative=path.lexically_relative(root);
  if(relative.empty()||relative.is_absolute())return false;
  for(const auto& piece:relative)if(piece==".."||piece==".")return false;
  return true;
}
bool archive_extension(const std::string& path) {
  const auto extension=lower(fs::path(path).extension().string());
  return extension==".zip"||extension==".rar"||extension==".7z";
}
std::string choose_archive(const Job& job,const std::vector<torrent::File>& files) {
  std::vector<std::string> candidates;
  for(const auto& file:files) {
    if(!archive_extension(file.path)||file.size<=0)continue;
    const auto relative=fs::path(file.path).lexically_relative(fs::path("/data/downloads/torrents")/job.key).generic_string();
    if(!job.requested_archive.empty()&&relative!=job.requested_archive)continue;
    const auto name=lower(relative);
    if(job.requested_archive.empty()&&(name.find("backport")!=name.npos||name.find("soundtrack")!=name.npos||
       name.find("original files")!=name.npos||name.find("/dlc/")!=name.npos||name.find("/fix/")!=name.npos))continue;
    candidates.push_back(file.path);
  }
  if(candidates.size()!=1)throw std::runtime_error("Select one exact PS4 game archive; ambiguous archives were preserved and not installed");
  // Multi-volume sets need all volumes. This backend refuses them rather than
  // treating the first part as a complete archive or concatenating other games.
  const auto selected=lower(candidates.front());
  if(selected.find(".part")!=selected.npos)throw std::runtime_error("PS4 resident multi-volume archives are not supported in this build");
  for(const auto& file:files) {
    const auto extension=lower(fs::path(file.path).extension().string());
    if(extension.size()==4&&extension[1]=='r'&&extension[2]>='0'&&extension[2]<='9'&&extension[3]>='0'&&extension[3]<='9')
      throw std::runtime_error("PS4 resident multi-volume archives are not supported in this build");
  }
  return candidates.front();
}
bool archive_continue(int id) {
  Job job;return alive&&job_copy(id,job)&&!job.want_paused&&job.state!="failed";
}
std::string file_hash(const std::string& path,std::uint64_t size,int id) {
  Fd file(::open(path.c_str(),O_RDONLY|O_NOFOLLOW));struct stat status{};
  if(file.value<0||::fstat(file.value,&status)||!S_ISREG(status.st_mode)||status.st_size<0||static_cast<std::uint64_t>(status.st_size)!=size)
    throw std::runtime_error("Saved archive is missing or changed; files were preserved");
  integrity::Hash digest;std::vector<char> buffer(256*1024);
  for(std::uint64_t offset=0;offset<size;) {
    if(!archive_continue(id))throw std::runtime_error("Archive task paused");
    const auto count=::read(file.value,buffer.data(),static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(),size-offset)));
    if(count<0&&errno==EINTR)continue;
    if(count<=0||!digest.update(buffer.data(),static_cast<std::size_t>(count)))throw std::runtime_error("Cannot hash saved archive");
    offset+=static_cast<std::uint64_t>(count);
  }
  const auto hash=digest.finish();if(hash.size()!=64)throw std::runtime_error("Archive SHA-256 failed");return hash;
}
void archive_transfer(Job& job,const std::string& cache) {
  const auto proof=cache+".sha256";
  struct stat status{};
  if(::lstat(cache.c_str(),&status)==0) {
    const auto expected=lines(read_file(proof,16384));
    if(expected.size()!=4||expected[0]!="ODAR1"||unhex(expected[1],4096)!=job.archive_source||
       number(expected[2])!=job.archive_bytes||file_hash(cache,job.archive_bytes,job.id)!=expected[3])
      throw std::runtime_error("Saved archive checksum differs; retained for inspection, no replacement was downloaded");
    return;
  }
  if(errno!=ENOENT)throw std::runtime_error("Cannot inspect resident archive cache");
  const auto partial=cache+".partial";
  if(::lstat(partial.c_str(),&status)==0) {
    // A service/process crash may interrupt a copy. Keep it recoverably; pieces
    // remain in libtorrent's cache and are verified again, without a new job.
    const auto parked=partial+".interrupted-"+std::to_string(Clock::now().time_since_epoch().count());
    if(::rename(partial.c_str(),parked.c_str()))throw std::runtime_error("Cannot preserve interrupted archive copy");
  } else if(errno!=ENOENT)throw std::runtime_error("Cannot inspect partial archive cache");
  Fd target(::open(partial.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600));
  if(target.value<0)throw std::runtime_error("Cannot create private archive cache");
  integrity::Hash digest;std::string error;std::vector<char> bytes;
  for(std::uint64_t offset=0;offset<job.archive_bytes;) {
    if(!archive_continue(job.id))throw std::runtime_error("Archive task paused");
    if(!torrent::stream::read(job.engine,offset,static_cast<std::size_t>(std::min<std::uint64_t>(256*1024,job.archive_bytes-offset)),bytes,error))
      throw std::runtime_error("Archive verified-piece read: "+error);
    if(bytes.empty()||bytes.size()>job.archive_bytes-offset||!digest.update(bytes.data(),bytes.size()))throw std::runtime_error("Invalid verified archive chunk");
    for(std::size_t done=0;done<bytes.size();) {
      const auto count=::write(target.value,bytes.data()+done,bytes.size()-done);
      if(count<0&&errno==EINTR)continue;
      if(count<=0)throw std::runtime_error("Archive cache write failed; check free internal storage");
      done+=static_cast<std::size_t>(count);
    }
    offset+=bytes.size();
    std::lock_guard<std::mutex> lock(jobs_mutex);auto& current=jobs.at(job.id);current.written=offset;
    current.state="downloading";current.status="Downloading the hash-verified PS4 game archive";
  }
  const auto hash=digest.finish();
  if(hash.size()!=64||::fsync(target.value))throw std::runtime_error("Cannot sync archive cache");
  atomic_file(proof,"ODAR1\n"+hex(job.archive_source)+"\n"+std::to_string(job.archive_bytes)+"\n"+hash+"\n");
  if(::rename(partial.c_str(),cache.c_str()))throw std::runtime_error("Cannot publish verified archive cache");
  Fd parent(::open(fs::path(cache).parent_path().c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW));
  if(parent.value<0||::fsync(parent.value))throw std::runtime_error("Cannot sync archive directory");
}
void extract_archive(Job job) {
  const auto root=archive_root(job);directory(root);
  const auto cache=archive_cache(job);
  std::string selection_error;
  while(!saved_archive(job)&&!torrent::stream::select(job.engine,job.archive_source,selection_error)) {
    if(!selection_error.empty())throw std::runtime_error(selection_error);
    if(!archive_continue(job.id))throw std::runtime_error("Archive task paused");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  archive_transfer(job,cache);
  if(!archive_continue(job.id))throw std::runtime_error("Archive task paused");
  // Preflight enforces decoder windows, entry count and expanded-byte budgets.
  // The shared extractor refuses unsafe paths, symlinks and conflicting output;
  // CRC and SHA-256 receipts precede publishing the final directory.
  std::string error;archives::Request request;request.sources={cache};request.destination=root+"/unpacked";request.password=job.password;
  request.max_bytes=1ULL<<40;
  const auto task=archives::enqueue(std::move(request),error);
  if(!task)throw std::runtime_error(error);
  std::vector<std::string> outputs;
  for(;;) {
    if(!archive_continue(job.id))archives::cancel(task);
    const auto states=archives::poll();
    if(!states.empty()) {
      const auto& state=states.front();
      {std::lock_guard<std::mutex> lock(jobs_mutex);auto& current=jobs.at(job.id);
       current.written=state.written;current.size=state.total;current.rate=0;
       current.status=state.phase=="finishing"?"Verifying and syncing extracted PS4 files":"Extracting the saved PS4 archive; closing the store is safe";}
      if(state.state=="completed"){outputs=state.artifacts;break;}
      if(state.state=="failed"||state.state=="cancelled")throw std::runtime_error(state.error.empty()?"Archive task paused":state.error);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  std::vector<torrent::File> packages;
  for(const auto& relative:outputs) {
    const auto output=(fs::path(root)/"unpacked"/relative).string();
    struct stat status{};
    if(fs::path(relative).is_absolute()||!within_path(output,root+"/unpacked"))throw std::runtime_error("Extracted package path escapes its verified archive");
    if(::lstat(output.c_str(),&status)||!S_ISREG(status.st_mode)||status.st_size<0x440)continue;
    if(lower(fs::path(output).extension().string())==".pkg")packages.push_back({output,status.st_size});
  }
  const auto package=choose(packages);struct stat status{};
  if(::lstat(package.c_str(),&status)||!S_ISREG(status.st_mode)||status.st_size<0x440)throw std::runtime_error("Archive has no validated PS4 package");
  {std::lock_guard<std::mutex> lock(jobs_mutex);auto& current=jobs.at(job.id);
   current.path=package;current.size=static_cast<std::uint64_t>(status.st_size);current.written=0;current.header_checked=false;
   current.state="queued";current.status="Archive verified; preparing the PS4 system installer";save(current);}
  async_log::write("[resident] archive job %d decoded and verified; original archive retained",job.id);
}
void archive_loop() noexcept {
  for(;;) {
    int id;
    {std::unique_lock<std::mutex> lock(queues_mutex);queue_wake.wait(lock,[]{return !alive||!archive_queue.empty();});
     if(!alive)return;id=archive_queue.front();archive_queue.pop_front();}
    Job job;
    try {if(job_copy(id,job))extract_archive(job);}
    catch(const std::exception& error) {
      bool paused=false;
      {std::lock_guard<std::mutex> lock(jobs_mutex);auto& current=jobs.at(id);paused=current.want_paused||!alive;
       if(paused){current.state="paused";current.rate=0;current.status="Archive paused; downloaded files are retained";save(current);}}
      if(!paused)fail(id,error.what());
    } catch(...) {fail(id,"Archive worker failed; original files were retained");}
    {std::lock_guard<std::mutex> lock(jobs_mutex);auto& current=jobs.at(id);current.archive_pending=false;if(current.readers)--current.readers;}
    controller_wake.notify_all();
  }
}
