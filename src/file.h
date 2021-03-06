// Protocol Buffers - Google's data interchange format
// Copyright 2008 Google Inc.  All rights reserved.
// https://developers.google.com/protocol-buffers/
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// Author: kenton@google.com (Kenton Varda)
// emulates google3/file/base/file.h

#ifndef NCODE_FILE_H__
#define NCODE_FILE_H__

#include <functional>
#include <iostream>

#include "common.h"
#include "statusor.h"

namespace nc {

const int DEFAULT_FILE_MODE = 0777;

struct FileWriteOptions {
  bool append = false;
  bool create_parents = true;
};

// Protocol buffer code only uses a couple static methods of File, and only
// in tests.
class File {
 public:
  // Check if the file exists.
  static bool Exists(const std::string& name);

  // Extracts the file name from a given location so that '/some/random/file'
  // returns 'file'.
  static std::string ExtractFileName(const std::string& file_location);

  // Extracts the directory name from a given location so that
  // '/some/random/file' returns '/some/random/'.
  static std::string ExtractDirectoryName(const std::string& file_location);

  // If 'name' is a file returns true and sets 'directory' to false. If it is
  // directory it sets 'directory' to true. If it is neither returns false.
  static bool FileOrDirectory(const std::string& name, bool* directory);

  // Returns the size of the given file in bytes.
  static StatusOr<uint64_t> FileSize(const std::string& name);

  // Moves a file or crashes.
  static void MoveOrDie(const std::string& src, const std::string& dst);

  // Read an entire file to a string.  Return true if successful, false
  // otherwise.
  static bool ReadFileToString(const std::string& name, std::string* output);

  // Same as above, but crash on failure.
  static std::string ReadFileToStringOrDie(const std::string& name);

  // Create a file (if one does not exist) and write bytes to it.
  static Status WriteToFile(const void* contents, size_t num_bytes,
                            const std::string& filename,
                            FileWriteOptions options = {});

  static Status WriteStringToFile(const std::string& contents,
                                  const std::string& name,
                                  FileWriteOptions options = {}) {
    return WriteToFile(contents.c_str(), contents.size(), name, options);
  }

  // Same as above, but crash on failure.
  static void WriteStringToFileOrDie(const std::string& contents,
                                     const std::string& name,
                                     FileWriteOptions options = {}) {
    CHECK_OK(WriteToFile(contents.c_str(), contents.size(), name, options));
  }

  // Create a directory.
  static bool CreateDir(const std::string& name, int mode);

  // Create a directory and all parent directories if necessary.
  static bool RecursivelyCreateDir(const std::string& path, int mode);

  // Picks a non-existent file name of a given length in a directory.
  static std::string PickFileName(const std::string& dir, size_t len);

  // If "name" is a file, we delete it.  If it is a directory, we
  // call DeleteRecursively() for each file or directory (other than
  // dot and double-dot) within it, and then delete the directory itself.
  // The "dummy" parameters have a meaning in the original version of this
  // method but they are not used anywhere in protocol buffers.
  static void DeleteRecursively(const std::string& name, void* dummy1,
                                void* dummy2);

  // Change working directory to given directory.
  static bool ChangeWorkingDirectory(const std::string& new_working_directory);

  // Returns the current working directory.
  static std::string WorkingDirectoryOrDie();

  static bool GetContents(const std::string& name, std::string* output,
                          bool /*is_default*/) {
    return ReadFileToString(name, output);
  }

  static bool ReadLines(const std::string& name,
                        std::function<void(const std::string& line)> callback);

  static Status SetContents(const std::string& name,
                            const std::string& contents, bool /*is_default*/) {
    return WriteStringToFile(contents, name);
  }

  // Mimics Python's os.walk, never follows symlinks, always top-bottom.
  static void Walk(
      const std::string& starting_root,
      std::function<void(const std::string& root,
                         const std::vector<std::string>& filenames,
                         const std::vector<std::string>& dirnames)>);

  // Recursively returns all files that share a given extension from a root. If
  // 'root' points to a file and this file has the extension a vector of size
  // one is returned with the file.
  static std::vector<std::string> GetFilesWithExtension(
      const std::string& root, const std::string& extension);

 private:
  DISALLOW_COPY_AND_ASSIGN(File);
};

}  // namespace nc

#endif  // GOOGLE_PROTOBUF_TESTING_FILE_H__
