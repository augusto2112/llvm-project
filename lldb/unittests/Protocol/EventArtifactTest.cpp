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
#include <vector>

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

/// Writes one event holding \p Text and returns what the file gives back for
/// it, or nullopt unless the event came back as exactly one parsable line.
/// Checking the escaping by reversing it rather than by matching escape
/// sequences leaves the choice of which characters to escape to JSON, where it
/// belongs.
std::optional<std::string> RoundTripText(const TestArtifact &Artifact,
                                         llvm::StringRef Text) {
  Artifact->Write(llvm::json::Object{{"text", Text}});

  llvm::Expected<std::string> Contents = Artifact->ReadContents();
  if (!Contents) {
    llvm::consumeError(Contents.takeError());
    return std::nullopt;
  }
  llvm::SmallVector<llvm::StringRef, 8> Lines = SplitLines(*Contents);
  if (Lines.size() != 1)
    return std::nullopt;

  llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(Lines[0]);
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    return std::nullopt;
  }
  const llvm::json::Object *Event = Parsed->getAsObject();
  if (!Event)
    return std::nullopt;
  std::optional<llvm::StringRef> Value = Event->getString("text");
  if (!Value)
    return std::nullopt;
  // Copied out while the buffer it was parsed from is still alive.
  return Value->str();
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

TEST(EventArtifactTest, ReadContentsFollowsTheFileThroughEveryStage) {
  TestArtifact Artifact(/*MaxEvents=*/2, EventArtifact::DefaultMaxBytes);
  ASSERT_TRUE(bool(Artifact)) << Artifact.GetError();

  // The file exists from Create onwards, so a read before any event succeeds
  // and is empty rather than failing.
  llvm::Expected<std::string> Empty = Artifact->ReadContents();
  ASSERT_TRUE(bool(Empty)) << llvm::toString(Empty.takeError());
  EXPECT_EQ(*Empty, "");

  Artifact->Write(llvm::json::Object{{"seq", 0}});
  llvm::Expected<std::string> One = Artifact->ReadContents();
  ASSERT_TRUE(bool(One)) << llvm::toString(One.takeError());
  EXPECT_EQ(*One, R"({"seq":0})"
                  "\n");

  Artifact->Write(llvm::json::Object{{"seq", 1}});
  Artifact->Write(llvm::json::Object{{"seq", 2}});
  ASSERT_TRUE(Artifact->HitInternalLimit());

  // A truncated artifact is still readable, and holds everything up to the
  // bound and nothing after it.
  llvm::Expected<std::string> Truncated = Artifact->ReadContents();
  ASSERT_TRUE(bool(Truncated)) << llvm::toString(Truncated.takeError());
  EXPECT_EQ(*Truncated, R"({"seq":0})"
                        "\n"
                        R"({"seq":1})"
                        "\n");
}

TEST(EventArtifactTest, AnArtifactOfExactlyTheEventLimitIsNotTruncated) {
  constexpr uint64_t TestMaxEvents = 3;
  TestArtifact Artifact(TestMaxEvents, EventArtifact::DefaultMaxBytes);
  ASSERT_TRUE(bool(Artifact)) << Artifact.GetError();

  for (uint64_t Seq = 0; Seq < TestMaxEvents; ++Seq)
    Artifact->Write(llvm::json::Object{{"seq", int64_t(Seq)}});

  // The bound is on what the file holds, so a run that ends having written
  // exactly that many events lost nothing and must not claim to have.
  EXPECT_EQ(Artifact->GetEventCount(), TestMaxEvents);
  EXPECT_FALSE(Artifact->HitInternalLimit());

  Artifact->Write(llvm::json::Object{{"seq", int64_t(TestMaxEvents)}});
  EXPECT_TRUE(Artifact->HitInternalLimit());
  EXPECT_EQ(Artifact->GetEventCount(), TestMaxEvents);
}

TEST(EventArtifactTest, TheByteLimitAdmitsTheLineThatExactlyFillsIt) {
  const llvm::json::Object Event{{"payload", std::string(64, 'x')}};

  // One line measured from a throwaway artifact, so that the bounds below can
  // be set to an exact multiple of it.
  uint64_t LineSize = 0;
  {
    TestArtifact Probe;
    ASSERT_TRUE(bool(Probe)) << Probe.GetError();
    Probe->Write(Event);
    ASSERT_EQ(Probe->GetEventCount(), 1u);
    LineSize = Probe.GetFileSize();
  }
  ASSERT_GT(LineSize, 0u);

  TestArtifact Exact(EventArtifact::DefaultMaxEvents, 3 * LineSize);
  ASSERT_TRUE(bool(Exact)) << Exact.GetError();
  for (int I = 0; I < 3; ++I)
    Exact->Write(Event);
  EXPECT_EQ(Exact->GetEventCount(), 3u);
  EXPECT_EQ(Exact.GetFileSize(), 3 * LineSize);
  EXPECT_FALSE(Exact->HitInternalLimit());

  Exact->Write(Event);
  EXPECT_TRUE(Exact->HitInternalLimit());
  EXPECT_EQ(Exact->GetEventCount(), 3u);
  EXPECT_EQ(Exact.GetFileSize(), 3 * LineSize);

  // One byte short of that, and the third line is the one that no longer fits:
  // the bound is never exceeded, not even by part of a line.
  TestArtifact OneShort(EventArtifact::DefaultMaxEvents, 3 * LineSize - 1);
  ASSERT_TRUE(bool(OneShort)) << OneShort.GetError();
  for (int I = 0; I < 3; ++I)
    OneShort->Write(Event);
  EXPECT_TRUE(OneShort->HitInternalLimit());
  EXPECT_EQ(OneShort->GetEventCount(), 2u);
  EXPECT_EQ(OneShort.GetFileSize(), 2 * LineSize);
}

TEST(EventArtifactTest, BoundsOfZeroAdmitNothing) {
  const llvm::json::Object Event{{"label", "loop"}};

  TestArtifact NoEvents(/*MaxEvents=*/0, EventArtifact::DefaultMaxBytes);
  ASSERT_TRUE(bool(NoEvents)) << NoEvents.GetError();
  NoEvents->Write(Event);
  EXPECT_TRUE(NoEvents->HitInternalLimit());
  EXPECT_EQ(NoEvents->GetEventCount(), 0u);
  EXPECT_EQ(NoEvents.GetFileSize(), 0u);

  TestArtifact NoBytes(EventArtifact::DefaultMaxEvents, /*MaxBytes=*/0);
  ASSERT_TRUE(bool(NoBytes)) << NoBytes.GetError();
  NoBytes->Write(Event);
  EXPECT_TRUE(NoBytes->HitInternalLimit());
  EXPECT_EQ(NoBytes->GetEventCount(), 0u);
  EXPECT_EQ(NoBytes.GetFileSize(), 0u);
}

TEST(EventArtifactTest, AnEventCarriesEveryJSONType) {
  TestArtifact Artifact;
  ASSERT_TRUE(bool(Artifact)) << Artifact.GetError();

  Artifact->Write(
      llvm::json::Object{{"null", nullptr},
                         {"bool", false},
                         {"int", int64_t(-9007199254740993)},
                         {"double", 1.5},
                         {"string", "text"},
                         {"array", llvm::json::Array{1, "two", true, nullptr}},
                         {"object", llvm::json::Object{{"nested", 2}}}});
  ASSERT_EQ(Artifact->GetEventCount(), 1u);

  llvm::Expected<std::string> Contents = Artifact->ReadContents();
  ASSERT_TRUE(bool(Contents)) << llvm::toString(Contents.takeError());
  llvm::SmallVector<llvm::StringRef, 8> Lines = SplitLines(*Contents);
  ASSERT_EQ(Lines.size(), 1u);
  llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(Lines[0]);
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  const llvm::json::Object *Event = Parsed->getAsObject();
  ASSERT_NE(Event, nullptr);

  const llvm::json::Value *Null = Event->get("null");
  ASSERT_NE(Null, nullptr);
  EXPECT_EQ(Null->kind(), llvm::json::Value::Null);
  EXPECT_EQ(Event->getBoolean("bool"), std::optional<bool>(false));
  // Past the range a double represents exactly, which is where a rendering that
  // went through one would round.
  EXPECT_EQ(Event->getInteger("int"),
            std::optional<int64_t>(-9007199254740993));
  EXPECT_EQ(Event->getNumber("double"), std::optional<double>(1.5));
  ASSERT_TRUE(Event->getString("string").has_value());
  EXPECT_EQ(Event->getString("string")->str(), "text");

  const llvm::json::Array *Array = Event->getArray("array");
  ASSERT_NE(Array, nullptr);
  ASSERT_EQ(Array->size(), 4u);
  EXPECT_EQ((*Array)[0].getAsInteger(), std::optional<int64_t>(1));
  EXPECT_EQ((*Array)[3].kind(), llvm::json::Value::Null);

  const llvm::json::Object *Nested = Event->getObject("object");
  ASSERT_NE(Nested, nullptr);
  EXPECT_EQ(Nested->getInteger("nested"), std::optional<int64_t>(2));
}

TEST(EventArtifactTest, EscapesQuotesAndBackslashesWithinAnEvent) {
  TestArtifact Artifact;
  ASSERT_TRUE(bool(Artifact)) << Artifact.GetError();

  const std::string Text = "he said \"go\" \\ then\ttabbed";
  const std::optional<std::string> Read = RoundTripText(Artifact, Text);
  ASSERT_TRUE(Read.has_value());
  EXPECT_EQ(*Read, Text);
}

TEST(EventArtifactTest, CarriesNonASCIITextThrough) {
  TestArtifact Artifact;
  ASSERT_TRUE(bool(Artifact)) << Artifact.GetError();

  // Escaped in the source so this file stays ASCII. What reaches the event is
  // multibyte UTF-8, which is what a captured string can hold.
  const std::string Text =
      "caf\u00e9 \u03b1\u03b2\u03b3 \u65e5\u672c\u8a9e \U0001F389";
  const std::optional<std::string> Read = RoundTripText(Artifact, Text);
  ASSERT_TRUE(Read.has_value());
  EXPECT_EQ(*Read, Text);
}

TEST(EventArtifactTest, CarriesADeeplyNestedObjectOnOneLine) {
  TestArtifact Artifact;
  ASSERT_TRUE(bool(Artifact)) << Artifact.GetError();

  constexpr unsigned Depth = 32;
  llvm::json::Value Nested = llvm::json::Object{{"leaf", 1}};
  for (unsigned I = 0; I < Depth; ++I)
    Nested = llvm::json::Object{{"child", std::move(Nested)}};
  Artifact->Write(llvm::json::Object{{"root", std::move(Nested)}});
  ASSERT_EQ(Artifact->GetEventCount(), 1u);

  llvm::Expected<std::string> Contents = Artifact->ReadContents();
  ASSERT_TRUE(bool(Contents)) << llvm::toString(Contents.takeError());
  llvm::SmallVector<llvm::StringRef, 8> Lines = SplitLines(*Contents);
  ASSERT_EQ(Lines.size(), 1u);
  llvm::Expected<llvm::json::Value> Parsed = llvm::json::parse(Lines[0]);
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  const llvm::json::Object *Event = Parsed->getAsObject();
  ASSERT_NE(Event, nullptr);

  const llvm::json::Object *Level = Event->getObject("root");
  ASSERT_NE(Level, nullptr);
  for (unsigned I = 0; I < Depth; ++I) {
    const llvm::json::Object *Child = Level->getObject("child");
    ASSERT_NE(Child, nullptr) << I;
    Level = Child;
  }
  EXPECT_EQ(Level->getInteger("leaf"), std::optional<int64_t>(1));
}

TEST(EventArtifactTest, InterleavedWritesToTwoArtifactsStayApart) {
  TestArtifact First;
  ASSERT_TRUE(bool(First)) << First.GetError();
  TestArtifact Second;
  ASSERT_TRUE(bool(Second)) << Second.GetError();

  for (int64_t Seq = 0; Seq < 3; ++Seq) {
    First->Write(llvm::json::Object{{"owner", "first"}, {"seq", Seq}});
    Second->Write(llvm::json::Object{{"owner", "second"}, {"seq", Seq}});
  }

  llvm::Expected<std::string> FirstContents = First->ReadContents();
  ASSERT_TRUE(bool(FirstContents)) << llvm::toString(FirstContents.takeError());
  EXPECT_EQ(*FirstContents, R"({"owner":"first","seq":0})"
                            "\n"
                            R"({"owner":"first","seq":1})"
                            "\n"
                            R"({"owner":"first","seq":2})"
                            "\n");

  llvm::Expected<std::string> SecondContents = Second->ReadContents();
  ASSERT_TRUE(bool(SecondContents))
      << llvm::toString(SecondContents.takeError());
  EXPECT_EQ(*SecondContents, R"({"owner":"second","seq":0})"
                             "\n"
                             R"({"owner":"second","seq":1})"
                             "\n"
                             R"({"owner":"second","seq":2})"
                             "\n");
}

TEST(EventArtifactTest, ThePathNamesAFileInsideTheArtifactsOwnDirectory) {
  TestArtifact Artifact;
  ASSERT_TRUE(bool(Artifact)) << Artifact.GetError();

  const std::string Directory = Artifact.GetDirectory();
  EXPECT_TRUE(llvm::sys::fs::is_directory(Directory));
  EXPECT_EQ(llvm::sys::path::parent_path(Artifact->GetPath()).str(), Directory);
  EXPECT_TRUE(llvm::sys::path::filename(Directory).starts_with("lldb-observe"));

  // The directory is the unit of isolation between concurrent runs, so it holds
  // the one file and nothing that could be confused for it.
  std::error_code EC;
  std::vector<std::string> Entries;
  for (llvm::sys::fs::directory_iterator It(Directory, EC), End; It != End;
       It.increment(EC)) {
    ASSERT_FALSE(bool(EC)) << EC.message();
    Entries.push_back(It->path());
  }
  ASSERT_FALSE(bool(EC)) << EC.message();
  ASSERT_EQ(Entries.size(), 1u);
  EXPECT_EQ(llvm::sys::path::filename(Entries.front()).str(), "events.jsonl");
  EXPECT_TRUE(llvm::sys::fs::equivalent(Entries.front(), Artifact->GetPath()));
}
