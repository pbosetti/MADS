/*
  ____  _             _            ____  _    _ _ _
 |  _ \| |_   _  __ _(_)_ __      / ___|| | _(_) | |
 | |_) | | | | |/ _` | | '_ \     \___ \| |/ / | | |
 |  __/| | |_| | (_| | | | | |     ___) |   <| | | |
 |_|   |_|\__,_|\__, |_|_| |_|    |____/|_|\_\_|_|_|
                |___/

 Installs the `mads-plugin` agent skill into a plugin project, so that an AI
 coding assistant working on the plugin has the runtime context it cannot infer
 from the plugin API alone (call order, per-host effect of every return_type,
 settings injection, topics/blobs, deployment, migration).

 The source of truth is <prefix>/share/skills/mads-plugin: SKILL.md.tpl is
 rendered with the same inja data `mads plugin` uses for the other templates
 (so version and protocol pins are stamped), while the reference/ markdown files are copied
 verbatim -- they contain code samples whose braces must not be parsed as inja
 tags.

 Used by both `mads plugin <name>` (scaffolding) and `mads plugin --update`
 (refreshing an existing project after a protocol migration).

 Author(s): Paolo Bosetti
*/
#ifndef PLUGIN_SKILL_HPP
#define PLUGIN_SKILL_HPP

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <inja/inja.hpp>
#include <iostream>
#include <nlohmann/json.hpp>
#include <rang.hpp>
#include <string>
#include <vector>

namespace Mads {
namespace PluginSkill {

namespace fs = std::filesystem;
using json = nlohmann::json;

/// Where the skill lands inside a plugin project.
inline const char *skill_rel_dir() { return ".claude/skills/mads-plugin"; }

/*!
 * @brief Renders/copies the `mads-plugin` skill into a plugin project.
 *
 * @param skills_dir  source directory (<prefix>/share/skills/mads-plugin)
 * @param project_dir the plugin project to write into
 * @param data        inja data (mads_version, plugin_protocol, ...)
 * @param overwrite   replace files that already exist
 * @param verbose     report each file on stdout
 * @return the number of files written; 0 also means "nothing to do"
 */
inline int install(const fs::path &skills_dir, const fs::path &project_dir,
                   const json &data, bool overwrite, bool verbose = true) {
  using namespace rang;
  if (!fs::is_directory(skills_dir)) {
    std::cerr << fg::yellow << "Warning: skill sources not found in "
              << skills_dir << ", skipping agent documentation" << fg::reset
              << std::endl;
    return 0;
  }

  fs::path dest = project_dir / skill_rel_dir();
  std::error_code ec;
  fs::create_directories(dest / "reference", ec);
  if (ec) {
    std::cerr << fg::red << "Error: cannot create " << dest << ": "
              << ec.message() << fg::reset << std::endl;
    return 0;
  }

  int written = 0;
  auto report = [&](const fs::path &p, bool skipped) {
    if (!verbose)
      return;
    std::cout << "==> " << style::bold << p.string() << style::reset << ": ";
    if (skipped)
      std::cout << fg::red << "already exists, skipped; use -o to overwrite"
                << fg::reset << std::endl;
    else
      std::cout << fg::green << "created" << fg::reset << std::endl;
  };

  // SKILL.md is rendered: it carries the MADS version and protocol numbers.
  fs::path tpl = skills_dir / "SKILL.md.tpl";
  fs::path skill_md = dest / "SKILL.md";
  if (fs::exists(tpl)) {
    if (!overwrite && fs::exists(skill_md)) {
      report(skill_md, true);
    } else {
      try {
        inja::Environment env;
        // inja's default line statement is "##", which would swallow every
        // markdown heading in the skill; the same "%%" convention the README
        // template uses keeps headings intact.
        env.set_line_statement("%%");
        std::string rendered = env.render_file(tpl.string(), data);
        std::ofstream ofs(skill_md);
        ofs << rendered;
        ofs.close();
        written++;
        report(skill_md, false);
      } catch (const std::exception &e) {
        std::cerr << fg::red << "Error rendering " << tpl << ": " << e.what()
                  << fg::reset << std::endl;
      }
    }
  }

  // reference/*.md are copied verbatim.
  fs::path ref_src = skills_dir / "reference";
  if (fs::is_directory(ref_src)) {
    std::vector<fs::path> refs;
    for (const auto &e : fs::directory_iterator(ref_src))
      if (e.is_regular_file() && e.path().extension() == ".md")
        refs.push_back(e.path());
    std::sort(refs.begin(), refs.end());
    for (const auto &src : refs) {
      fs::path dst = dest / "reference" / src.filename();
      if (!overwrite && fs::exists(dst)) {
        report(dst, true);
        continue;
      }
      fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
      if (ec) {
        std::cerr << fg::red << "Error: cannot copy " << src << ": "
                  << ec.message() << fg::reset << std::endl;
        ec.clear();
        continue;
      }
      written++;
      report(dst, false);
    }
  }

  return written;
}

} // namespace PluginSkill
} // namespace Mads

#endif // PLUGIN_SKILL_HPP
