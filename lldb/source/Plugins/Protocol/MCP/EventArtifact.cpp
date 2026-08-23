//===- EventArtifact.cpp --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "EventArtifact.h"
#include "lldb/Utility/LLDBLog.h"
#include "lldb/Utility/Log.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <memory>
#include <string>
#include <system_error>
#include <utility>

using namespace lldb_private;
using namespace lldb_private::mcp;
using namespace llvm;

Expected<std::unique_ptr<EventArtifact>>
EventArtifact::Create(uint64_t MaxEvents, uint64_t MaxBytes) {
  SmallString<128> Prefix;
  sys::path::system_temp_directory(/*erasedOnReboot=*/true, Prefix);
  sys::path::append(Prefix, "lldb-observe");

  SmallString<128> Directory;
  if (std::error_code EC = sys::fs::createUniqueDirectory(Prefix, Directory))
    return createFileError(Prefix, EC);

  SmallString<128> Path = Directory;
  sys::path::append(Path, "events.jsonl");

  std::error_code EC;
  auto Stream = std::make_unique<raw_fd_ostream>(Path.str(), EC);
  if (EC)
    return createFileError(Path, EC);

  return std::unique_ptr<EventArtifact>(new EventArtifact(
      std::string(Path), std::move(Stream), MaxEvents, MaxBytes));
}

void EventArtifact::Write(const json::Object &Event) {
  if (m_hit_limit || !m_stream)
    return;

  std::string Line;
  {
    raw_string_ostream LineStream(Line);
    json::OStream J(LineStream);
    // json::Value cannot be built from a const Object &, so the properties go
    // out through OStream rather than through a per-event deep copy. Sorting
    // the keys reproduces the order `OS << json::Value` emits, which keeps two
    // runs over the same events byte-identical.
    J.object([&] {
      for (const json::Object::value_type *KV : json::sortedElements(Event))
        J.attribute(KV->first, KV->second);
    });
  }
  // A newline inside a string is escaped by JSON itself, so the terminator
  // added here is the only one on the line. That is the property that makes
  // the file addressable a line at a time.
  Line += '\n';

  // The line is measured before it is written so the byte bound is exact and
  // the file never exceeds it.
  if (m_event_count >= m_max_events ||
      m_written_bytes + Line.size() > m_max_bytes) {
    m_hit_limit = true;
    return;
  }

  *m_stream << Line;
  // Flushed per event rather than per buffer. The tail of the stream is the
  // most valuable part of it when a run ends abnormally, and buffering is
  // exactly what drops that tail.
  m_stream->flush();

  if (std::error_code EC = m_stream->error()) {
    LLDB_LOG(GetLog(LLDBLog::Host), "event artifact {0} failed to write: {1}",
             m_path, EC.message());
    // A raw_fd_ostream must not be destroyed with a pending error, and a file
    // that has failed once has nothing more to gain from further writes.
    m_stream->clear_error();
    m_stream.reset();
    return;
  }

  ++m_event_count;
  m_written_bytes += Line.size();
}

Expected<std::string> EventArtifact::ReadContents() const {
  ErrorOr<std::unique_ptr<MemoryBuffer>> Buffer = MemoryBuffer::getFile(
      m_path, /*IsText=*/false, /*RequiresNullTerminator=*/false);
  if (!Buffer)
    return createFileError(m_path, Buffer.getError());
  return (*Buffer)->getBuffer().str();
}
