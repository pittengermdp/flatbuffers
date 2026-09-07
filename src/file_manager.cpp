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
#include <cstdint>
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

// FNV-1a. Not cryptographic and does not need to be: it distinguishes one
// generator's output from another's, and both sides are produced by this same
// process from schemas the user controls.
std::string OutputManifestFileSaver::Digest(const char* buf, size_t len) {
  uint64_t hash = 14695981039346656037ULL;
  for (size_t i = 0; i < len; i++) {
    hash ^= static_cast<unsigned char>(buf[i]);
    hash *= 1099511628211ULL;
  }
  std::ostringstream out;
  out << len << "-" << std::hex << hash;
  return out.str();
}

bool OutputManifestFileSaver::SaveFile(const char* name, const char* buf,
                                       size_t len, bool binary) {
  const std::string path(name);

  // Without a known producer there is nothing to attribute the write to, so
  // record nothing and let it through rather than guessing.
  if (!current_source_.empty()) {
    const std::string digest = Digest(buf, len);
    const auto existing = produced_by_.find(path);
    // Same bytes is not a clobber, whoever wrote them. A schema re-emitting a
    // file that belongs to one it includes lands here constantly.
    if (existing != produced_by_.end() && existing->second.source != current_source_ &&
        existing->second.digest != digest) {
      std::cerr << "error: two schemas generate the same output file:\n"
                << "    " << path << "\n"
                << "        " << existing->second.source << "\n"
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
    // Record the LATEST accepted write, not the first. A single schema can write
    // one path more than once in one run -- ln2/value.fbs emits value.ts as both
    // its schema barrel and its namespace barrel -- and it is the last write that
    // survives on disk. Comparing a later schema against the first of those would
    // reject an identical file.
    produced_by_[path] = Output{current_source_, digest};
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
    const size_t tab2 = line.find('\t', tab + 1);
    if (tab2 == std::string::npos) continue;
    produced_by_[line.substr(0, tab)] =
        Output{line.substr(tab2 + 1), line.substr(tab + 1, tab2 - tab - 1)};
  }
  return true;
}

bool OutputManifestFileSaver::Store() const {
  std::ofstream ofs(manifest_path_.c_str());
  if (!ofs.is_open()) return false;

  ofs << "# flatc output manifest: <generated file>\\t<content digest>\\t"
         "<schema that produced it>.\n"
      << "# Written by --output-manifest. Delete before a full regeneration.\n";
  for (const auto& entry : produced_by_) {
    ofs << entry.first << "\t" << entry.second.digest << "\t"
        << entry.second.source << "\n";
  }
  return !ofs.bad();
}

}  // namespace flatbuffers
