// Unit tests for the mads-director GUI adapter: ProcessManager (src/gui/director)
// must present exactly what the shared MadsCore parser produced for
// director.toml -- instance names, base names, expanded commands, dependencies
// and flags -- so that the GUI and `mads up` stay in sync on the same file.
//
// These tests never start a process: launch_ready_processes(), start_process()
// and tick() are deliberately not called. ProcessManager's destructor calls
// stop_all(), which is a no-op when nothing was started.
//
// Only built when MADS_ENABLE_GUI and MADS_BUILD_APPS are ON (see
// tests/CMakeLists.txt), since it links mads_director_core.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

#include "director_config.hpp"
#include "example_config.hpp"
#include "process_manager.hpp"

namespace fs = std::filesystem;

namespace {

fs::path fixture(const std::string &name) {
  return fs::path(MADS_TEST_FIXTURES_DIR) / "director" / name;
}

const Mads::ProcessConfig *find_proc(const Mads::DirectorConfig &config,
                                     const std::string &name) {
  for (const auto &p : config.processes) {
    if (p.name == name) return &p;
  }
  return nullptr;
}

const ProcessRuntimeView *find_view(const ProcessManager &pm,
                                    const std::string &name) {
  for (std::size_t i = 0; i < pm.process_count(); ++i) {
    const auto *view = pm.process_at(i);
    if (view != nullptr && view->name == name) return view;
  }
  return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Every fixture that the parser accepts is mirrored one-to-one by the view.
// ---------------------------------------------------------------------------

TEST_CASE("ProcessManager mirrors the parser output for each fixture",
         "[director_adapter]") {
  auto file = GENERATE(as<std::string>{},
                       "simple.toml",
                       "scale_templating.toml",
                       "base_instance_id.toml",
                       "multi_after.toml",
                       "unknown_keys.toml",
                       "with_ready.toml");
  std::string error;
  auto cfg = Mads::load_director_config(fixture(file).string(), &error);
  REQUIRE(cfg.has_value());

  ProcessManager pm;
  REQUIRE(pm.initialize(*cfg, &error));
  REQUIRE(pm.process_count() == cfg->processes.size());

  for (std::size_t i = 0; i < cfg->processes.size(); ++i) {
    const auto &expected = cfg->processes[i];
    const auto *view = pm.process_at(i);
    REQUIRE(view != nullptr);
    CHECK(view->name == expected.name);
    CHECK(view->base_name == expected.base_name);
    CHECK(view->command == expected.command);
    CHECK(view->enabled == expected.enabled);
    CHECK(view->relaunch == expected.relaunch);
    CHECK(view->tty == expected.tty);
    CHECK(view->dependencies == expected.after);
    // Nothing was started, so the view must report it as idle.
    CHECK(view->running == false);
    CHECK(view->ever_started == false);
  }
}

// ---------------------------------------------------------------------------
// scale + ${PWD}/${ID} templating, as seen through the GUI view.
// ---------------------------------------------------------------------------

TEST_CASE("scale_templating.toml: scaled names and expanded ${ID} reach the view",
         "[director_adapter]") {
  std::string error;
  auto cfg = Mads::load_director_config(
      fixture("scale_templating.toml").string(), &error);
  REQUIRE(cfg.has_value());

  ProcessManager pm;
  REQUIRE(pm.initialize(*cfg, &error));
  REQUIRE(pm.process_count() == 4);

  // db is not scaled: bare name, ${PWD} and ${ID} (0) already expanded.
  const auto *db_cfg = find_proc(*cfg, "db");
  REQUIRE(db_cfg != nullptr);
  const auto *db = find_view(pm, "db");
  REQUIRE(db != nullptr);
  CHECK(db->base_name == "db");
  CHECK(db->command ==
        "./bin/db --port 5432 --wd=" + db_cfg->workdir + " --id=0");

  // api scale=3 expands to api[1]..api[3] with ${ID} 0..2 and a bare-name
  // base; the instance names stay 1-based while ${ID} stays 0-based.
  REQUIRE(find_view(pm, "api") == nullptr);
  const auto *api1 = find_view(pm, "api[1]");
  const auto *api2 = find_view(pm, "api[2]");
  const auto *api3 = find_view(pm, "api[3]");
  REQUIRE(api1 != nullptr);
  REQUIRE(api2 != nullptr);
  REQUIRE(api3 != nullptr);
  CHECK(api1->base_name == "api");
  CHECK(api2->base_name == "api");
  CHECK(api3->base_name == "api");
  CHECK(api1->command.find("--id=0") != std::string::npos);
  CHECK(api2->command.find("--id=1") != std::string::npos);
  CHECK(api3->command.find("--id=2") != std::string::npos);
  // Each instance depends on db, as declared by `after = "db"`.
  CHECK(api1->dependencies == std::vector<std::string>{"db"});
  CHECK(api2->dependencies == std::vector<std::string>{"db"});
  CHECK(api3->dependencies == std::vector<std::string>{"db"});
  CHECK(api2->relaunch == false);
}

// ---------------------------------------------------------------------------
// reload() replaces the process set with the new config.
// ---------------------------------------------------------------------------

TEST_CASE("reload replaces the views with a different config",
         "[director_adapter]") {
  std::string error;
  auto cfg1 = Mads::load_director_config(fixture("simple.toml").string(), &error);
  REQUIRE(cfg1.has_value());
  auto cfg2 =
      Mads::load_director_config(fixture("scale_templating.toml").string(), &error);
  REQUIRE(cfg2.has_value());

  ProcessManager pm;
  REQUIRE(pm.initialize(*cfg1, &error));
  REQUIRE(pm.process_count() == cfg1->processes.size());

  REQUIRE(pm.reload(*cfg2, &error));
  REQUIRE(pm.process_count() == cfg2->processes.size());
  CHECK(find_view(pm, "worker") == nullptr);
  CHECK(find_view(pm, "api[1]") != nullptr);
}

// ---------------------------------------------------------------------------
// The built-in director.toml template must parse with the shared parser.
// ---------------------------------------------------------------------------

TEST_CASE("example template is valid for the shared parser",
         "[director_adapter]") {
  struct TempFile {
    fs::path path;
    ~TempFile() {
      std::error_code ec;
      fs::remove(path, ec);
    }
  };
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  TempFile tmp{fs::temp_directory_path() /
               ("mads_director_template_" + std::to_string(stamp) + ".toml")};

  std::string error;
  REQUIRE(write_example_config(tmp.path, false, &error));

  auto cfg = Mads::load_director_config(tmp.path.string(), &error);
  REQUIRE(cfg.has_value());
  REQUIRE(!cfg->processes.empty());

  std::string exists_error;
  REQUIRE_FALSE(write_example_config(tmp.path, false, &exists_error));
  CHECK(exists_error.find("File already exists") != std::string::npos);
  REQUIRE(write_example_config(tmp.path, true, &error));
}
