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

#ifndef FLATBUFFERS_FILE_MANAGER_H_
#define FLATBUFFERS_FILE_MANAGER_H_

#include <cstddef>
#include <map>
#include <set>
#include <string>

namespace flatbuffers {

// A File interface to write data to file by default or
// save only file names
class FileSaver {
 public:
  FileSaver() = default;
  virtual ~FileSaver() = default;

  virtual bool SaveFile(const char* name, const char* buf, size_t len,
                        bool binary) = 0;

  bool SaveFile(const char* name, const std::string& buf, bool binary) {
    return SaveFile(name, buf.c_str(), buf.size(), binary);
  }

  virtual void Finish() {}

  // Tells the saver which schema is currently being compiled. flatc calls this
  // once per input file. Savers that do not care about provenance ignore it.
  virtual void SetCurrentSource(const std::string& /*source*/) {}

 private:
  // Copying is not supported.
  FileSaver(const FileSaver&) = delete;
  FileSaver& operator=(const FileSaver&) = delete;
  // Rule of 5
  FileSaver(FileSaver&&) = default;
  FileSaver& operator=(FileSaver&&) = default;
};

class RealFileSaver final : public FileSaver {
 public:
  bool SaveFile(const char* name, const char* buf, size_t len,
                bool binary) final;
};

// Fails the build when two different schemas would write the same output file.
//
// flatc derives most output names from the schema's BASENAME and, for some
// languages, its namespace -- never from the schema's directory. Two schemas in
// different directories that share a basename therefore resolve to one output
// path, and because flatc is normally invoked once per schema, the second
// invocation simply overwrites what the first produced. Both runs exit 0. The
// losing schema's types are then absent from the generated code with nothing
// anywhere to say so, and the gap typically surfaces much later as a missing
// type in a consumer that has no way to trace it back here.
//
// Provenance is what makes this detectable across separate invocations, so it
// is kept in a manifest of "output path -> schema that produced it". A write
// that would change a path's producer is refused. Rewriting a path from the
// SAME schema is ordinary regeneration and always allowed.
//
// The manifest describes one generated tree. Delete it when starting a full
// regeneration from a clean slate, otherwise entries for schemas that have since
// been renamed or removed linger and can accuse an innocent newcomer that
// legitimately took over the name.
class OutputManifestFileSaver final : public FileSaver {
 public:
  OutputManifestFileSaver(FileSaver* inner, std::string manifest_path);

  bool SaveFile(const char* name, const char* buf, size_t len,
                bool binary) final;

  void SetCurrentSource(const std::string& source) final;

  void Finish() final;

 private:
  bool Load();
  bool Store() const;

  FileSaver* inner_;
  std::string manifest_path_;
  std::string current_source_{};
  // output path -> the schema that produced it
  std::map<std::string, std::string> produced_by_{};
};

class FileNameSaver final : public FileSaver {
 public:
  bool SaveFile(const char* name, const char* buf, size_t len,
                bool binary) final;

  void Finish() final;

 private:
  std::set<std::string> file_names_{};
};

}  // namespace flatbuffers

#endif  // FLATBUFFERS_FILE_MANAGER_H_
