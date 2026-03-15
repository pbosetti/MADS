#!/usr/bin/env python3
import difflib
import re
import sys
from pathlib import Path

"""
Script to perform search and replace operations on files.
Uses a global dictionary of replacements.
"""

PluginGitTag = 'v2.0-P7'


CppReplacements = {
  '.set_params(&params)': '.set_params(params)',
  'void set_params(void const *params) override':
    'void set_params(const json &params) override',
  '_params.merge_patch(*(json *)params)': '_params.merge_patch(params)',
}

SignatureReplacements = (
  (
    re.compile(
      r'return_type\s+load_data\s*\(\s*.*?\)\s*override',
      re.DOTALL,
    ),
    'return_type load_data(json const &input, string topic = "", vector<unsigned char> const *blob = nullptr) override',
  ),
  (
    re.compile(
      r'return_type\s+load_data\s*\(\s*.*?\)',
      re.DOTALL,
    ),
    'return_type load_data(json const &input, string topic = "", vector<unsigned char> const *blob = nullptr)',
  ),
  (
    re.compile(
      r'return_type\s+process\s*\(\s*.*?\)',
      re.DOTALL,
    ),
    'return_type process(json &out, vector<unsigned char> *blob = nullptr)',
  ),
  (
    re.compile(
      r'return_type\s+get_output\s*\(\s*.*?\)',
      re.DOTALL,
    ),
    'return_type get_output(json &out, vector<unsigned char> *blob = nullptr)',
  ),
)

CmakePluginPopulatePattern = re.compile(
  r'FetchContent_Populate\s*\(\s*plugin\b(?P<body>.*?)\)',
  re.DOTALL,
)

CmakeGitTagPattern = re.compile(r'(\bGIT_TAG\b\s+)(\S+)')

CmakeJsonDeclarePattern = re.compile(
  r'FetchContent_Declare\s*\(\s*json\b(?P<body>.*?)\)',
  re.DOTALL,
)

JsonGitRepositoryPattern = re.compile(
  r'\bGIT_REPOSITORY\b\s+https://github\.com/nlohmann/json\.git\b'
)

JsonGitTagPattern = re.compile(r'\bGIT_TAG\b\s+v3\.11\.3\b')


def normalize_cmake_plugin_git_tag(content: str) -> tuple[str, int]:
  """Force GIT_TAG for FetchContent_Populate(plugin ...) blocks."""
  replacement_count = 0

  def update_populate_block(match: re.Match) -> str:
    nonlocal replacement_count
    body = match.group('body')

    def replace_git_tag(tag_match: re.Match) -> str:
      nonlocal replacement_count
      prefix = tag_match.group(1)
      old_value = tag_match.group(2)
      if old_value == PluginGitTag:
        return tag_match.group(0)
      replacement_count += 1
      return f'{prefix}{PluginGitTag}'

    updated_body = CmakeGitTagPattern.sub(replace_git_tag, body)
    return match.group(0).replace(body, updated_body, 1)

  updated_content = CmakePluginPopulatePattern.sub(update_populate_block, content)
  return updated_content, replacement_count


def normalize_cmake_json_fetch_content(content: str) -> tuple[str, int]:
  """Replace old nlohmann_json FetchContent blocks with the tarball form."""
  replacement_count = 0

  def replace_json_block(match: re.Match) -> str:
    nonlocal replacement_count
    body = match.group('body')
    if not JsonGitRepositoryPattern.search(body):
      return match.group(0)
    if not JsonGitTagPattern.search(body):
      return match.group(0)
    replacement_count += 1
    return (
      'FetchContent_Declare(json\n'
      '  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz\n'
      '  DOWNLOAD_EXTRACT_TIMESTAMP TRUE\n'
      ')'
    )

  updated_content = CmakeJsonDeclarePattern.sub(replace_json_block, content)
  return updated_content, replacement_count


def get_changed_lines(original_content: str, updated_content: str) -> list[tuple[int, str]]:
  """Return changed line numbers with their updated content."""
  original_lines = original_content.splitlines()
  updated_lines = updated_content.splitlines()
  matcher = difflib.SequenceMatcher(a=original_lines, b=updated_lines)
  changed_lines = []
  seen = set()

  for tag, _i1, _i2, j1, j2 in matcher.get_opcodes():
    if tag == 'equal':
      continue
    if tag in ('replace', 'insert'):
      for line_index in range(j1, j2):
        line_no = line_index + 1
        if line_no in seen:
          continue
        changed_lines.append((line_no, updated_lines[line_index]))
        seen.add(line_no)
    elif tag == 'delete':
      line_no = j1 + 1
      if line_no not in seen:
        changed_lines.append((line_no, '<deleted>'))
        seen.add(line_no)

  return changed_lines


def replace_in_file(file_path: str) -> int:
  """
  Perform all replacements in a single file.
  
  Args:
    file_path: Path to the file to process
  """
  file_path = Path(file_path)
  
  if not file_path.exists():
    print(f"Error: File '{file_path}' not found")
    return 0
  
  try:
    # Read file content
    content = file_path.read_text()
    original_content = content
    
    replacement_count = 0

    # Apply literal replacements
    for old, new in CppReplacements.items():
      count = content.count(old)
      if count > 0:
        content = content.replace(old, new)
        replacement_count += count

    # Normalize method signatures across spacing/newline variants
    for pattern, replacement in SignatureReplacements:
      content, count = pattern.subn(replacement, content)
      replacement_count += count

    # If this is CMakeLists.txt, patch plugin FetchContent GIT_TAG.
    if file_path.name == 'CMakeLists.txt':
      content, count = normalize_cmake_plugin_git_tag(content)
      replacement_count += count
      content, count = normalize_cmake_json_fetch_content(content)
      replacement_count += count
    
    # Write back only if content changed
    if content != original_content:
      file_path.write_text(content)
      changed_lines = get_changed_lines(original_content, content)
      print(f"Updated: {file_path} ({replacement_count} replacements)")
      print("Changed lines:")
      for line_no, line_text in changed_lines:
        print(f"  {line_no}: {line_text}")
    else:
      print(f"No changes: {file_path}")
    return replacement_count
  
  except Exception as e:
    print(f"Error processing '{file_path}': {e}")
    return 0


def main() -> None:
  """Main entry point."""
  if len(sys.argv) < 2:
    print("Update source of MADS plugins to be compatible with MADS v2.0.0")
    print("Usage: python update_plugin_source.py <file1> [file2] ...")
    sys.exit(1)
  
  total_replacements = 0
  for file_path in sys.argv[1:]:
    total_replacements += replace_in_file(file_path)

  print(f"Total replacements: {total_replacements}")


if __name__ == '__main__':
  main()
