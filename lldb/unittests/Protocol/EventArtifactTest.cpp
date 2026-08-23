//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Plugins/Protocol/MCP/EventArtifact.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "gtest/gtest.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

using namespace lldb_private::mcp;

namespace {

/// Owns an artifact for the length of a test and removes its directory
/// afterwards, including when a failed assertion returns early.
class TestArtifact {
public:
  explicit TestArtifact(uint64_t MaxEvents = EventArtifact::DefaultMaxEvents,
                        uint64_t MaxBytes = EventArtifact::DefaultMaxBytes) {
    llvm::Expected<std::unique_ptr<EventArtifact>> Created =
        EventArtifact::Create(MaxEvents, MaxBytes);
    if (Created)
      m_artifact = std::move(*Created);
    else
      m_error = llvm::toString(Created.takeError());
  }

  ~TestArtifact() {
    if (!m_artifact)
      return;
    std::string Directory = GetDirectory();
    // The file is closed with the artifact, and a directory holding an open
    // file cannot be removed on every platform.
    m_artifact.reset();
    llvm::sys::fs::remove_directories(Directory);
  }

  TestArtifact(const TestArtifact &) = delete;
  TestArtifact &operator=(const TestArtifact &) = delete;

  explicit operator bool() const { return static_cast<bool>(m_artifact); }
  EventArtifact *operator->() const { return m_artifact.get(); }
  llvm::StringRef GetError() const { return m_error; }

  std::string GetDirectory() const {
    return llvm::sys::path::parent_path(m_artifact->GetPath()).str();
  }

  uint64_t GetFileSize() const {
    uint64_t Size = 0;
    if (llvm::sys::fs::file_size(m_artifact->GetPath(), Size))
      return 0;
    return Size;
  }

private:
  std::unique_ptr<EventArtifact> m_artifact;
  std::string m_error;
};

/// Splits \p Contents on newlines, dropping the empty piece after the trailing
/// terminator.
llvm::SmallVector<llvm::StringRef, 8> SplitLines(llvm::StringRef Contents) {
  llvm::SmallVector<llvm::StringRef, 8> Lines;
  Contents.split(Lines, '\n', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
  return Lines;
}

} // namespace

TEST(EventArtifactTest, CreateOpensAnEmptyFile) {
  TestArtifact Artifact;
  ASSERT_TRUE(bool(Artifact)) << Artifact.GetError();

  EXPECT_EQ(llvm::sys::path::filename(Artifact->GetPath()).str(),
            "events.jsonl");
  EXPECT_TRUE(llvm::sys::fs::exists(Artifact->GetPath()));
  EXPECT_EQ(Artifact.GetFileSize(), 0u);
  EXPECT_EQ(Artifact->GetEventCount(), 0u);
  EXPECT_FALSE(Artifact->HitInternalLimit());
}

TEST(EventArtifactTest, WritesOneJSONObjectPerLine) {
  TestArtifact Artifact;
  ASSERT_TRUE(bool(Artifact)) << Artifact.GetError();

  for (int64_t Seq = 0; Seq < 3; ++Seq)
    Artifact->Write(llvm::json::Object{{"seq", Seq}, {"label", "loop"}});

  EXPECT_EQ(Artifact->GetEventCount(), 3u);
  EXPECT_FALSE(Artifact->HitInternalLimit());

  llvm::Expected<std::string> Contents = Artifact->ReadContents();
  ASSERT_TRUE(bool(Contents)) << llvm::toString(Contents.takeError());

  llvm::SmallVector<llvm::StringRef, 8> Lines = SplitLines(*Contents);
  ASSERT_EQ(Lines.size(), 3u);
  for (size_t Index = 0; Index < Lines.size(); ++Index) {
    llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(Lines[Index]);
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    const llvm::json::Object *Event = Parsed->getAsObject();
    ASSERT_NE(Event, nullptr);
    EXPECT_EQ(Event->getInteger("seq"),
              std::optional<int64_t>(static_cast<int64_t>(Index)));
    ASSERT_TRUE(Event->getString("label").has_value());
    EXPECT_EQ(Event->getString("label")->str(), "loop");
  }
}

TEST(EventArtifactTest, EscapesNewlinesWithinAnEvent) {
  TestArtifact Artifact;
  ASSERT_TRUE(bool(Artifact)) << Artifact.GetError();

  Artifact->Write(llvm::json::Object{{"text", "first\nsecond"}});
  EXPECT_EQ(Artifact->GetEventCount(), 1u);

  llvm::Expected<std::string> Contents = Artifact->ReadContents();
  ASSERT_TRUE(bool(Contents)) << llvm::toString(Contents.takeError());

  // One event stays one line whatever it carries, or the file cannot be read a
  // line at a time.
  EXPECT_EQ(llvm::StringRef(*Contents).count('\n'), 1u);

  llvm::SmallVector<llvm::StringRef, 8> Lines = SplitLines(*Contents);
  ASSERT_EQ(Lines.size(), 1u);
  llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(Lines[0]);
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  const llvm::json::Object *Event = Parsed->getAsObject();
  ASSERT_NE(Event, nullptr);
  ASSERT_TRUE(Event->getString("text").has_value());
  EXPECT_EQ(Event->getString("text")->str(), "first\nsecond");
}

TEST(EventArtifactTest, CreateUsesADistinctDirectoryPerArtifact) {
  TestArtifact First;
  ASSERT_TRUE(bool(First)) << First.GetError();
  TestArtifact Second;
  ASSERT_TRUE(bool(Second)) << Second.GetError();

  EXPECT_NE(First.GetDirectory(), Second.GetDirectory());
  EXPECT_NE(First->GetPath().str(), Second->GetPath().str());

  // Distinct paths are only worth having if the writes stay apart.
  First->Write(llvm::json::Object{{"label", "first"}});
  Second->Write(llvm::json::Object{{"label", "second"}});
  EXPECT_EQ(First->GetEventCount(), 1u);
  EXPECT_EQ(Second->GetEventCount(), 1u);

  llvm::Expected<std::string> FirstContents = First->ReadContents();
  ASSERT_TRUE(bool(FirstContents)) << llvm::toString(FirstContents.takeError());
  EXPECT_EQ(SplitLines(*FirstContents).size(), 1u);
  EXPECT_NE(FirstContents->find("first"), std::string::npos);
  EXPECT_EQ(FirstContents->find("second"), std::string::npos);
}

TEST(EventArtifactTest, ByteLimitStopsWritingAndIsReported) {
  // A small bound rather than the shipping one: the behaviour under test is the
  // bound, and reaching the real 500MB default would cost half a gigabyte of
  // disk per run of the test suite.
  constexpr uint64_t TestMaxBytes = 4096;
  TestArtifact Artifact(EventArtifact::DefaultMaxEvents, TestMaxBytes);
  ASSERT_TRUE(bool(Artifact)) << Artifact.GetError();

  llvm::json::Object Event{{"payload", std::string(256, 'x')}};

  Artifact->Write(Event);
  ASSERT_EQ(Artifact->GetEventCount(), 1u);
  const uint64_t LineSize = Artifact.GetFileSize();
  ASSERT_GT(LineSize, 0u);

  // Byte accounting is exact, so the number of events that fit is known and a
  // limit that never triggers shows up as a failure rather than as a hang.
  const uint64_t ExpectedEvents = TestMaxBytes / LineSize;
  while (!Artifact->HitInternalLimit()) {
    ASSERT_LE(Artifact->GetEventCount(), ExpectedEvents);
    Artifact->Write(Event);
  }

  EXPECT_EQ(Artifact->GetEventCount(), ExpectedEvents);
  const uint64_t SizeAtLimit = Artifact.GetFileSize();
  EXPECT_EQ(SizeAtLimit, ExpectedEvents * LineSize);
  EXPECT_LE(SizeAtLimit, TestMaxBytes);

  // Past the limit an event is dropped, not queued.
  Artifact->Write(Event);
  Artifact->Write(llvm::json::Object{{"label", "tiny"}});
  EXPECT_TRUE(Artifact->HitInternalLimit());
  EXPECT_EQ(Artifact->GetEventCount(), ExpectedEvents);
  EXPECT_EQ(Artifact.GetFileSize(), SizeAtLimit);
}

TEST(EventArtifactTest, EventLimitStopsWritingAndIsReported) {
  // The two bounds share one condition; this pins the count side of it.
  TestArtifact Artifact(/*MaxEvents=*/3, EventArtifact::DefaultMaxBytes);
  ASSERT_TRUE(bool(Artifact)) << Artifact.GetError();

  for (int I = 0; I < 5; ++I)
    Artifact->Write(llvm::json::Object{{"seq", I}});

  EXPECT_TRUE(Artifact->HitInternalLimit());
  EXPECT_EQ(Artifact->GetEventCount(), 3u);

  llvm::Expected<std::string> Contents = Artifact->ReadContents();
  ASSERT_TRUE(bool(Contents)) << llvm::toString(Contents.takeError());
  EXPECT_EQ(SplitLines(*Contents).size(), 3u);
}
