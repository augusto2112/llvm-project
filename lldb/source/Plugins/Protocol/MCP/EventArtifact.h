//===- EventArtifact.h ----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_PROTOCOL_MCP_EVENTARTIFACT_H
#define LLDB_SOURCE_PLUGINS_PROTOCOL_MCP_EVENTARTIFACT_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace lldb_private::mcp {

/// A newline-delimited JSON file carrying one event per line.
///
/// This is the overflow path for output too large to inline in a response, so
/// it is written for a reader who may only ever get to see the tail: every
/// event reaches the file as it is written, which leaves the artifact complete
/// up to the last event even when the observed process dies mid-run. Surviving
/// process death needs no more than that; surviving a machine crash would
/// additionally require fsync, which is not worth a syscall per event.
class EventArtifact {
public:
  /// Writing stops at whichever of these bounds is reached first. They are
  /// defaults rather than part of the tool's input schema, because a caller
  /// there has no basis on which to pick a value; the only part of the decision
  /// it needs back is whether a bound was reached, which HitInternalLimit
  /// reports. Create takes them as arguments so a test can reach a bound
  /// without writing half a gigabyte.
  static constexpr uint64_t DefaultMaxEvents = 1000000;
  static constexpr uint64_t DefaultMaxBytes = 500 * 1024 * 1024;

  /// Creates a directory of its own and opens `events.jsonl` inside it. The
  /// fresh directory is what keeps concurrent runs from appending into one
  /// file.
  static llvm::Expected<std::unique_ptr<EventArtifact>>
  Create(uint64_t MaxEvents = DefaultMaxEvents,
         uint64_t MaxBytes = DefaultMaxBytes);

  EventArtifact(const EventArtifact &) = delete;
  EventArtifact &operator=(const EventArtifact &) = delete;

  /// Appends \p Event as one line. Once the internal limit is reached this
  /// does nothing: the event count and HitInternalLimit together are what tell
  /// a truncated artifact from a complete one.
  void Write(const llvm::json::Object &Event);

  /// How many events are on the file, which past the limit is fewer than the
  /// number of Write calls.
  uint64_t GetEventCount() const { return m_event_count; }

  /// The path of the JSONL file itself, not of its directory.
  llvm::StringRef GetPath() const { return m_path; }

  bool HitInternalLimit() const { return m_hit_limit; }

  /// Reads the file back. Everything written so far is included, since events
  /// are flushed as they are written.
  llvm::Expected<std::string> ReadContents() const;

private:
  EventArtifact(std::string Path, std::unique_ptr<llvm::raw_fd_ostream> Stream,
                uint64_t MaxEvents, uint64_t MaxBytes)
      : m_path(std::move(Path)), m_stream(std::move(Stream)),
        m_max_events(MaxEvents), m_max_bytes(MaxBytes) {}

  std::string m_path;
  std::unique_ptr<llvm::raw_fd_ostream> m_stream;
  uint64_t m_max_events;
  uint64_t m_max_bytes;
  uint64_t m_event_count = 0;
  uint64_t m_written_bytes = 0;
  bool m_hit_limit = false;
};

} // namespace lldb_private::mcp

#endif
