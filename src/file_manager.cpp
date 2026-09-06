/*
 * Copyright 2023 Google Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "flatbuffers/file_manager.h"

#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace flatbuffers {

bool RealFileSaver::SaveFile(const char* name, const char* buf, size_t len,
                             bool binary) {
  std::ofstream ofs(name, binary ? std::ofstream::binary : std::ofstream::out);
  if (!ofs.is_open()) return false;
  ofs.write(buf, len);
  return !ofs.bad();
}

OutputManifestFileSaver::OutputManifestFileSaver(FileSaver* inner,
                                                 std::string manifest_path)
    : inner_(inner), manifest_path_(std::move(manifest_path)) {
  // A missing manifest is the normal first run, not an error.
  Load();
}

void OutputManifestFileSaver::SetCurrentSource(const std::string& source) {
  current_source_ = source;
  if (inner_) inner_->SetCurrentSource(source);
}

bool OutputManifestFileSaver::SaveFile(const char* name, const char* buf,
                                       size_t len, bool binary) {
  const std::string path(name);

  // Without a known producer there is nothing to attribute the write to, so
  // record nothing and let it through rather than guessing.
  if (!current_source_.empty()) {
    const auto existing = produced_by_.find(path);
    if (existing != produced_by_.end() && existing->second != current_source_) {
      std::cerr << "error: two schemas generate the same output file:\n"
                << "    " << path << "\n"
                << "        " << existing->second << "\n"
                << "        " << current_source_ << "\n\n"
                << "Output names come from the schema basename (and, for some\n"
                << "languages, the namespace) but never from its directory, so\n"
                << "these two schemas resolve to one file and the second write\n"
                << "would silently discard the first schema's types.\n\n"
                << "Rename one of the schemas so the basenames differ, or send\n"
                << "one to a different -o directory. If the earlier entry is\n"
                << "stale -- a schema that has since been renamed or removed --\n"
                << "delete " << manifest_path_ << " and regenerate.\n";
      return false;
    }
    produced_by_[path] = current_source_;
  }

  return inner_ ? inner_->SaveFile(name, buf, len, binary) : false;
}

void OutputManifestFileSaver::Finish() {
  if (!Store()) {
    std::cerr << "warning: could not write output manifest " << manifest_path_
              << "\n";
  }
  if (inner_) inner_->Finish();
}

bool OutputManifestFileSaver::Load() {
  std::ifstream ifs(manifest_path_.c_str());
  if (!ifs.is_open()) return false;

  std::string line;
  while (std::getline(ifs, line)) {
    if (line.empty() || line[0] == '#') continue;
    const size_t tab = line.find('\t');
    if (tab == std::string::npos) continue;
    produced_by_[line.substr(0, tab)] = line.substr(tab + 1);
  }
  return true;
}

bool OutputManifestFileSaver::Store() const {
  std::ofstream ofs(manifest_path_.c_str());
  if (!ofs.is_open()) return false;

  ofs << "# flatc output manifest: <generated file>\\t<schema that produced "
         "it>.\n"
      << "# Written by --output-manifest. Delete before a full regeneration.\n";
  for (const auto& entry : produced_by_) {
    ofs << entry.first << "\t" << entry.second << "\n";
  }
  return !ofs.bad();
}

}  // namespace flatbuffers
