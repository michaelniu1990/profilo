// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include <loom/LogEntry.h>
#include <loom/entries/EntryType.h>
#include <loom/entries/Entry.h>

#include "PacketLogger.h"
#include "RingBuffer.h"

#define LOOMEXPORT __attribute__((visibility("default")))

namespace facebook {
namespace loom {

using namespace entries;

class Logger {
  const int32_t TRACING_DISABLED = -1;
  const int32_t NO_MATCH = 0;
public:
  const size_t kMaxVariableLengthEntry = 1024;

  LOOMEXPORT static Logger& get();

  template<class T>
  int32_t write(T&& entry, uint16_t id_step = 1) {
    entry.id = nextID(id_step);

    auto size = T::calculateSize(entry);
    char payload[size];
    T::pack(entry, payload, size);

    logger_.write(payload, size);
    return entry.id;
  }

  template<class T>
  int32_t writeAndGetCursor(
    T&& entry,
    LoomBuffer::Cursor& cursor) {
    entry.id = nextID();

    auto size = T::calculateSize(entry);
    char payload[size];
    T::pack(entry, payload, size);

    cursor = logger_.writeAndGetCursor(payload, size);
    return entry.id;
  }

  LOOMEXPORT int32_t writeBytes(
    EntryType type,
    int32_t arg1,
    const uint8_t* arg2,
    size_t len);

  LOOMEXPORT void writeStackFrames(
      int32_t tid,
      int64_t time,
      const int64_t* methods,
      uint8_t depth,
      EntryType entry_type = entries::STACK_FRAME);


  LOOMEXPORT void writeTraceAnnotation(int32_t key, int64_t value);


 private:
  std::atomic<int32_t> entryID_;
  logger::PacketLogger logger_;

  Logger(logger::PacketBufferProvider provider):
    entryID_(0), logger_(provider) {}
  Logger(const Logger& other) = delete;

  inline int32_t nextID(uint16_t step = 1) {
    int32_t id;
    do {
      id = entryID_.fetch_add(step);
    } while (id == TRACING_DISABLED || id == NO_MATCH);
    return id;
  }
};

} // namespace loom
} // namespace facebook