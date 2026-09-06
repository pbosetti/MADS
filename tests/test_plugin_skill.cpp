/*
 Consistency checks for the `mads-plugin` agent skill (share/skills/mads-plugin).

 The skill is the runtime documentation an AI assistant reads when working on a
 plugin project, and it is trusted over the sources it describes: if it drifts
 from the loader it becomes actively misleading. These tests pin the few facts
 that can silently rot -- the protocol numbers it stamps, and the reference
 files it points at.
*/
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <regex>
#include <set>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

fs::path project_root() { return fs::path(MADS_PROJECT_SOURCE_DIR); }
fs::path skill_dir() { return project_root() / "share/skills/mads-plugin"; }

std::string slurp(const fs::path &p) {
  std::ifstream ifs(p);
  REQUIRE(ifs.good());
  std::stringstream ss;
  ss << ifs.rdbuf();
  return ss.str();
}

} // namespace

TEST_CASE("The skill and its reference files exist", "[plugin_skill]") {
  REQUIRE(fs::is_directory(skill_dir()));
  REQUIRE(fs::exists(skill_dir() / "SKILL.md.tpl"));
  REQUIRE(fs::is_directory(skill_dir() / "reference"));
}

TEST_CASE("SKILL.md declares Claude-skill front matter", "[plugin_skill]") {
  std::string skill = slurp(skill_dir() / "SKILL.md.tpl");
  // The directory name is the skill name; a mismatch makes it undiscoverable.
  REQUIRE(skill.rfind("---\n", 0) == 0);
  REQUIRE(skill.find("\nname: mads-plugin\n") != std::string::npos);
  REQUIRE(skill.find("\ndescription: ") != std::string::npos);
}

TEST_CASE("The skill stamps the protocol numbers instead of hardcoding them",
          "[plugin_skill]") {
  std::string skill = slurp(skill_dir() / "SKILL.md.tpl");
  REQUIRE(skill.find("{{plugin_protocol}}") != std::string::npos);
  REQUIRE(skill.find("{{plugin_min_protocol}}") != std::string::npos);
  REQUIRE(skill.find("{{mads_version}}") != std::string::npos);
}

TEST_CASE("plugin_deps.json mirrors the loader's minimum protocol",
          "[plugin_skill]") {
  std::string manifest_text = slurp(project_root() / "share/plugin_deps.json");
  json manifest = json::parse(manifest_text);
  REQUIRE(manifest.contains("plugin_min_protocol"));

  // MADS_PLUGIN_MIN_PROTOCOL is what every host actually enforces; the skill
  // quotes the manifest, so the two must agree.
  std::string loader = slurp(project_root() / "src/main/plugin_loader.cpp");
  std::smatch m;
  std::regex re(R"(#define\s+MADS_PLUGIN_MIN_PROTOCOL\s+(\d+))");
  REQUIRE(std::regex_search(loader, m, re));
  REQUIRE(manifest["plugin_min_protocol"].get<int>() == std::stoi(m[1].str()));
  REQUIRE(manifest["plugin_protocol"].get<int>() >=
          manifest["plugin_min_protocol"].get<int>());
}

TEST_CASE("The scaffolded pins match the newest migration step",
          "[plugin_skill]") {
  json manifest = json::parse(slurp(project_root() / "share/plugin_deps.json"));
  fs::path migrations = project_root() / "share/plugin_migrations";
  REQUIRE(fs::is_directory(migrations));

  int highest = -1;
  json newest;
  for (const auto &e : fs::directory_iterator(migrations)) {
    if (e.path().extension() != ".json")
      continue;
    json step = json::parse(slurp(e.path()));
    if (step.contains("to") && step["to"].get<int>() > highest) {
      highest = step["to"].get<int>();
      newest = step;
    }
  }
  REQUIRE(highest > 0);
  REQUIRE(manifest["plugin_protocol"].get<int>() == highest);
  if (newest.contains("cmake") && newest["cmake"].contains("plugin_git_tag")) {
    REQUIRE(manifest["plugin_git_tag"].get<std::string>() ==
            newest["cmake"]["plugin_git_tag"].get<std::string>());
  }
}

TEST_CASE("Every reference file is linked, and every link resolves",
          "[plugin_skill]") {
  std::string skill = slurp(skill_dir() / "SKILL.md.tpl");

  std::set<std::string> on_disk;
  for (const auto &e : fs::directory_iterator(skill_dir() / "reference")) {
    if (e.path().extension() == ".md")
      on_disk.insert(e.path().filename().string());
  }
  REQUIRE_FALSE(on_disk.empty());

  std::set<std::string> referenced;
  std::regex re(R"(reference/([A-Za-z0-9_\-]+\.md))");
  for (auto it = std::sregex_iterator(skill.begin(), skill.end(), re);
       it != std::sregex_iterator(); ++it) {
    referenced.insert((*it)[1].str());
  }

  // No dangling pointer, and no orphan file the assistant will never open.
  for (const auto &name : referenced) {
    INFO("SKILL.md points at reference/" << name);
    REQUIRE(on_disk.count(name) == 1);
  }
  for (const auto &name : on_disk) {
    INFO("reference/" << name << " is never mentioned in SKILL.md");
    REQUIRE(referenced.count(name) == 1);
  }
}

TEST_CASE("Reference files are copied verbatim, so they must be inja-free",
          "[plugin_skill]") {
  // `mads plugin` renders only SKILL.md.tpl; the reference files are copied
  // as-is precisely because they contain C++ brace-init code. An inja tag
  // sneaking in would ship an unrendered placeholder to every plugin project.
  for (const auto &e : fs::directory_iterator(skill_dir() / "reference")) {
    if (e.path().extension() != ".md")
      continue;
    std::string body = slurp(e.path());
    INFO(e.path().filename().string() << " contains an inja statement tag");
    REQUIRE(body.find("{%") == std::string::npos);
  }
}

TEST_CASE("No SKILL.md line is swallowed by the inja line statement",
          "[plugin_skill]") {
  // `mads plugin` renders SKILL.md.tpl with the line statement set to "%%"
  // (inja's default "##" would eat every markdown heading). A line starting
  // with "%%" would silently disappear from every scaffolded project.
  std::istringstream iss(slurp(skill_dir() / "SKILL.md.tpl"));
  std::string line;
  int n = 0;
  while (std::getline(iss, line)) {
    n++;
    INFO("SKILL.md.tpl line " << n << " starts with the line-statement token");
    REQUIRE(line.rfind("%%", 0) != 0);
  }
  REQUIRE(n > 0);
}
