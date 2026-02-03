#!/usr/bin/env python3
import sys
from pathlib import Path

"""
Script to perform search and replace operations on files.
Uses a global dictionary of replacements.
"""


# Global dictionary of replacements
CppReplacements = {
  '.set_params(&params)': '.set_params(params)',
  'void set_params(void const *params) override': 'void set_params(const json &params) override',
  '_params.merge_patch(*(json *)params)': '_params.merge_patch(params)'
  # Add more replacements as needed
}

def replace_in_file(file_path: str) -> None:
  """
  Perform all replacements in a single file.
  
  Args:
    file_path: Path to the file to process
  """
  file_path = Path(file_path)
  
  if not file_path.exists():
    print(f"Error: File '{file_path}' not found")
    return
  
  try:
    # Read file content
    content = file_path.read_text()
    original_content = content
    
    # Apply all replacements
    for old, new in CppReplacements.items():
      content = content.replace(old, new)
    
    # Write back only if content changed
    if content != original_content:
      file_path.write_text(content)
      print(f"Updated: {file_path}")
    else:
      print(f"No changes: {file_path}")
  
  except Exception as e:
    print(f"Error processing '{file_path}': {e}")


def main() -> None:
  """Main entry point."""
  if len(sys.argv) < 2:
    print("Update source of MADS plugins to be compatible with MADS v2.0.0")
    print("Usage: python update_plugin_source.py <file1> [file2] ...")
    sys.exit(1)
  
  for file_path in sys.argv[1:]:
    replace_in_file(file_path)


if __name__ == '__main__':
  main()