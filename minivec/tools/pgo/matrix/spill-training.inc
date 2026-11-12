// Build-only extension. Reuses the independent dyadic oracle, never DB headers.
constexpr uint64_t kSpillSeed = 2026111301ULL;
constexpr size_t kSpillRows = 4096, kSpillDimension = 128, kSpillTagBytes = 8192;

static void spill_query(Phase& phase, const Dataset& data, Random& random, size_t number) {
  const auto q = number % 11 == 0 ? data.rows[64].vector : random.vector(data.dim);
  constexpr size_t limits[]{1, 10, 100};
  const size_t k = limits[number % 3], mode = number % 5;
  const int64_t group = static_cast<int64_t>(number % 7);
  const int64_t low = static_cast<int64_t>(data.rows.size() / 4);
  const int64_t high = static_cast<int64_t>(3 * data.rows.size() / 4);
  std::string where;
  if (mode == 1) where = " WHERE g=" + std::to_string(group);
  if (mode == 2) where = " WHERE id>=" + std::to_string(low) + " AND id<" + std::to_string(high);
  if (mode == 3) where = " WHERE g>=4";
  if (mode == 4) where = " WHERE g=" + std::to_string(group) + " AND id>=" +
      std::to_string(low) + " AND id<" + std::to_string(high);
  struct Neighbor { double distance; const Row* row; };
  std::vector<Neighbor> ranked;
  for (const auto& row : data.rows) {
    if (((mode == 1 || mode == 4) && row.group != group) ||
        ((mode == 2 || mode == 4) && !(row.id >= low && row.id < high)) ||
        (mode == 3 && row.group < 4)) continue;
    ranked.push_back({squared(row.vector, q), &row});
  }
  std::sort(ranked.begin(), ranked.end(), [](const Neighbor& a, const Neighbor& b) {
    return a.distance < b.distance || (a.distance == b.distance && a.row->id < b.row->id);
  });
  Json rows = Json::array();
  for (size_t i = 0; i < std::min(k, ranked.size()); ++i)
    rows.push_back(Json::array({ranked[i].row->id, ranked[i].row->group}));
  phase.selected("SELECT id,g FROM " + data.table + where + " ORDER BY L2_DISTANCE(v," +
      sql_quote(vector_sql(q)) + ") LIMIT " + std::to_string(k), {"id", "g"}, std::move(rows));
}

static void spill_populate(Phase& phase, Dataset& data, Random& random) {
  phase.change("CREATE TABLE " + data.table + "(id BIGINT PRIMARY KEY,g BIGINT,s VARCHAR(8192),v VECTOR(128))");
  data.rows.reserve(kSpillRows);
  for (size_t i = 0; i < kSpillRows; ++i) {
    std::string tag = "spill-" + std::to_string(i + 1) + "-";
    tag.resize(kSpillTagBytes, static_cast<char>('a' + i % 26));
    Row row{static_cast<int64_t>(i + 1), static_cast<int64_t>(i % 7), std::move(tag), random.vector(data.dim)};
    if (i > 0 && i % 64 == 0) row.vector = data.rows.back().vector;
    data.rows.push_back(std::move(row));
  }
  for (size_t start = 0; start < data.rows.size(); start += 64) {
    std::string sql = "INSERT INTO " + data.table + " VALUES ";
    const size_t end = std::min(start + 64, data.rows.size());
    for (size_t i = start; i < end; ++i) {
      const Row& row = data.rows[i];
      if (i != start) sql += ',';
      sql += "(" + std::to_string(row.id) + "," + std::to_string(row.group) + "," +
          sql_quote(row.tag) + "," + sql_quote(vector_sql(row.vector)) + ")";
    }
    phase.change(sql, static_cast<int64_t>(end - start));
  }
}

static void generate_spill(const fs::path& root) {
  require(!fs::exists(root), "spill corpus must be new");
  fs::create_directories(root);
  Random random; random.state = kSpillSeed;
  Dataset data{"train_spill", kSpillDimension, 24, {}};
  Json manifest{{"kind", "build_only_spill_PGO_extension"}, {"seed", kSpillSeed},
      {"rows", kSpillRows}, {"dimensions", kSpillDimension}, {"tag_bytes", kSpillTagBytes},
      {"minimum_vector_plus_string_bytes", kSpillRows * (kSpillTagBytes + 4 * kSpillDimension)},
      {"threads", 1}, {"epochs", 1}, {"queries_per_phase", {24, 8, 4}},
      {"phase_requests", Json::array()}, {"checkpoints", Json::array()},
      {"oracle", "independent exact dyadic double distance; complete response comparison"},
      {"limits", "Wide tag is only a spill trigger: no tag predicates or bulk tag projection. One additional single-thread lifecycle, not a new evaluation dataset."}};
  auto checkpoint = [&](const Phase& p, int phase, const char* stage) {
    manifest["checkpoints"].push_back({{"phase", phase}, {"request_id", p.current_request()}, {"stage", stage}});
  };
  {
    Phase phase(root, 0);
    spill_populate(phase, data, random);
    phase.change("CREATE VECTOR INDEX idx_spill ON train_spill(v)");
    checkpoint(phase, 0, "after-index-build");
    for (size_t i = 0; i < data.queries; ++i) {
      if (i == 12) mutations(phase, data, random);
      spill_query(phase, data, random, i);
      if (i == 12) checkpoint(phase, 0, "after-mutation-and-rebuild");
    }
    project(phase, data, 3); project(phase, data, 4);
    manifest["phase_requests"].push_back(phase.finish());
  }
  {
    Phase phase(root, 1);
    project(phase, data, 0); project(phase, data, 3); project(phase, data, 4);
    for (size_t i = 0; i < 8; ++i) spill_query(phase, data, random, i);
    checkpoint(phase, 1, "after-first-restart");
    phase.change("BEGIN");
    phase.change("UPDATE train_spill SET g=123 WHERE id=1", 1);
    phase.selected("SELECT g FROM train_spill WHERE id=1", {"g"}, Json::array({Json::array({123})}));
    manifest["phase_requests"].push_back(phase.finish());
  }
  {
    Phase phase(root, 2);
    project(phase, data, 0);
    for (size_t i = 0; i < 4; ++i) spill_query(phase, data, random, i);
    checkpoint(phase, 2, "after-shutdown-rollback-restart");
    manifest["phase_requests"].push_back(phase.finish());
  }
  std::ofstream output(root / "manifest.json"); output << manifest.dump(2) << '\n';
  require(output.good(), "cannot save spill manifest");
  std::cout << manifest.dump() << '\n';
}

class TrainingChild {
 public:
  TrainingChild(const fs::path& binary, const fs::path& data, const fs::path& errors) {
    int request[2], response[2];
    require(::pipe(request) == 0, "request pipe failed");
    if (::pipe(response) != 0) { ::close(request[0]); ::close(request[1]); throw std::runtime_error("response pipe failed"); }
    const int error_fd = ::open(errors.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (error_fd < 0) {
      for (int fd : {request[0], request[1], response[0], response[1]}) ::close(fd);
      throw std::runtime_error("cannot create child stderr");
    }
    pid_ = ::fork();
    if (pid_ == 0) {
      if (::dup2(request[0], STDIN_FILENO) < 0 || ::dup2(response[1], STDOUT_FILENO) < 0 ||
          ::dup2(error_fd, STDERR_FILENO) < 0) _exit(126);
      for (int fd : {request[0], request[1], response[0], response[1], error_fd}) ::close(fd);
      ::execl(binary.c_str(), binary.c_str(), "--data", data.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    ::close(request[0]); ::close(response[1]); ::close(error_fd);
    if (pid_ < 0) { ::close(request[1]); ::close(response[0]); throw std::runtime_error("fork failed"); }
    input_ = request[1]; output_ = response[0];
  }
  TrainingChild(const TrainingChild&) = delete;
  ~TrainingChild() {
    if (input_ >= 0) ::close(input_);
    if (output_ >= 0) ::close(output_);
    if (pid_ > 0) { ::kill(pid_, SIGKILL); while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {} }
  }
  pid_t pid() const { return pid_; }
  void send(const std::string& line) {
    const std::string framed = line + '\n'; size_t offset = 0;
    while (offset < framed.size()) {
      const ssize_t n = ::write(input_, framed.data() + offset, framed.size() - offset);
      if (n < 0 && errno == EINTR) continue;
      require(n > 0, "child request write failed"); offset += static_cast<size_t>(n);
    }
  }
  bool line(std::string& result) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    for (;;) {
      const auto split = buffered_.find('\n');
      if (split != std::string::npos) { result = buffered_.substr(0, split); buffered_.erase(0, split + 1); return true; }
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      require(left > 0, "child response timeout");
      struct pollfd descriptor{output_, POLLIN, 0};
      const int ready = ::poll(&descriptor, 1, static_cast<int>(left));
      if (ready < 0 && errno == EINTR) continue;
      require(ready > 0, "child response poll failed or timed out");
      char chunk[65536]; const ssize_t n = ::read(output_, chunk, sizeof(chunk));
      if (n < 0 && errno == EINTR) continue;
      require(n >= 0, "child response read failed");
      if (n == 0) { require(buffered_.empty(), "partial trailing response"); return false; }
      buffered_.append(chunk, static_cast<size_t>(n));
      require(buffered_.size() <= 16u * 1024u * 1024u, "training response exceeded 16 MiB");
    }
  }
  void finish() {
    ::close(input_); input_ = -1;
    std::string unexpected; require(!line(unexpected), "unexpected response after shutdown");
    int status = 0; pid_t waited;
    do { waited = ::waitpid(pid_, &status, 0); } while (waited < 0 && errno == EINTR);
    require(waited == pid_, "waitpid failed"); pid_ = -1;
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "training child exit failure");
  }
 private:
  pid_t pid_ = -1; int input_ = -1, output_ = -1; std::string buffered_;
};

static Json active_spill_files(pid_t pid, const fs::path& data) {
  Json files = Json::array(); bool payload = false, flat = false;
  const std::string prefix = fs::absolute(data).string();
  for (const auto& entry : fs::directory_iterator("/proc/" + std::to_string(pid) + "/fd")) {
    std::error_code error;
    const std::string target = fs::read_symlink(entry.path(), error).string();
    if (error) continue;
    const bool is_payload = target.rfind(prefix + "/.payload-", 0) == 0;
    const bool is_flat = target.rfind(prefix + "/.flat-", 0) == 0;
    if (!is_payload && !is_flat) continue;
    struct stat state{};
    require(::stat(entry.path().c_str(), &state) == 0 && S_ISREG(state.st_mode), "cannot stat active spill fd");
    require(state.st_size >= 0, "invalid spill size");
    require(state.st_nlink == 0, "spill fd must refer to an unlinked temporary file");
    const uint64_t bytes = static_cast<uint64_t>(state.st_size);
    if (is_payload) { require(bytes > 32u * 1024u * 1024u, "payload file did not exceed spill threshold"); payload = true; }
    if (is_flat) { require(bytes >= kSpillRows * (kSpillDimension * 4 + 16), "flat file is incomplete"); flat = true; }
    files.push_back({{"kind", is_payload ? "payload" : "flat"}, {"fd", entry.path().filename().string()},
        {"target", target}, {"bytes", bytes}, {"unlinked", state.st_nlink == 0}});
  }
  require(payload && flat, "training did not prove both active .payload and .flat files");
  return files;
}

static void exercise_spill(const fs::path& binary, const fs::path& corpus,
                           const fs::path& data, const fs::path& evidence) {
  require(!fs::exists(data) && !fs::exists(evidence), "spill data/evidence directories must be new");
  fs::create_directories(data); fs::create_directories(evidence);
  std::signal(SIGPIPE, SIG_IGN);
  std::ifstream manifest_file(corpus / "manifest.json"); Json manifest; manifest_file >> manifest;
  require(manifest.at("seed") == kSpillSeed && manifest.at("rows") == kSpillRows &&
      manifest.at("dimensions") == kSpillDimension, "unexpected spill manifest");
  Json observations = Json::array(), timings = Json::array();
  auto save = [&]() {
    std::ofstream checkpoints(evidence / "checkpoints.json"); checkpoints << observations.dump(2) << '\n';
    std::ofstream durations(evidence / "timings.json"); durations << timings.dump(2) << '\n';
    require(checkpoints.good() && durations.good(), "cannot save live spill evidence");
  };
  save();
  for (int phase = 0; phase < 3; ++phase) {
    const auto started = std::chrono::steady_clock::now();
    const std::string stem = "phase-" + std::to_string(phase);
    std::ifstream input(corpus / (stem + ".jsonl"));
    require(input.good(), "missing spill requests");
    const fs::path actual = evidence / (stem + ".actual.jsonl");
    std::ofstream output(actual);
    require(output.good(), "cannot create spill responses");
    TrainingChild child(binary, data, evidence / (stem + ".stderr.log"));
    std::string request, response;
    require(child.line(response), "missing ready response"); output << response << '\n';
    require(Json::parse(response) == Json{{"type", "ready"}}, "spill child not ready");
    size_t requests = 0;
    while (std::getline(input, request)) {
      child.send(request); require(child.line(response), "missing spill response");
      output << response << '\n'; ++requests;
      const Json sent = Json::parse(request);
      for (const auto& checkpoint : manifest.at("checkpoints")) {
        if (checkpoint.at("phase") == phase && checkpoint.at("request_id") == sent.at("id")) {
          const Json got = Json::parse(response);
          require(got.value("ok", false), "spill checkpoint request failed");
          observations.push_back({{"phase", phase}, {"request_id", sent.at("id")},
              {"stage", checkpoint.at("stage")}, {"files", active_spill_files(child.pid(), data)}});
          save();
        }
      }
    }
    require(input.eof() && requests == manifest.at("phase_requests").at(phase).get<size_t>(), "spill request count mismatch");
    child.finish(); output.flush(); require(output.good(), "spill response write failed"); output.close();
    verify(corpus / (stem + ".expected.jsonl"), actual);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    timings.push_back({{"phase", phase}, {"seconds_including_oracle_and_fd_checks", seconds},
        {"requests", requests}, {"responses_verified", requests + 1}, {"status", "passed"}});
    save();
  }
  require(observations.size() == manifest.at("checkpoints").size(), "missing spill checkpoint");
}
