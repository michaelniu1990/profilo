// Copyright 2004-present Facebook. All Rights Reserved.

#include <errno.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <system_error>

#include <loom/writer/DeltaEncodingVisitor.h>
#include <loom/writer/TraceLifecycleVisitor.h>
#include <loom/writer/PrintEntryVisitor.h>
#include <loom/writer/StackTraceInvertingVisitor.h>
#include <loom/writer/TimestampTruncatingVisitor.h>

namespace facebook { namespace loom { namespace writer {

using namespace facebook::loom::entries;

namespace {

std::string getTraceID(int64_t trace_id) {
  const char* kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";
  const size_t kTraceIdStringLen = 11;

  if (trace_id < 0) {
    throw std::invalid_argument("trace_id must be non-negative");
  }
  char result[kTraceIdStringLen+1]{};
  for (ssize_t idx = kTraceIdStringLen - 1; idx >= 0; --idx) {
    result[idx] = kBase64Alphabet[trace_id % 64];
    trace_id /= 64;
  }
  return std::string(result);
}

std::string getTraceFilename(
  const std::string& trace_prefix,
  const std::string& trace_id
) {
  std::stringstream filename;
  filename << trace_prefix << "-" << getpid() << "-";

  auto now = time(nullptr);
  struct tm localnow{};
  if (localtime_r(&now, &localnow) == nullptr) {
    throw std::runtime_error("Could not localtime_r(3)");
  }

  filename
    << (1900 + localnow.tm_year) << "-"
    << (1 + localnow.tm_mon) << "-"
    << localnow.tm_mday
    << "T"
    << localnow.tm_hour << "-"
    << localnow.tm_min << "-"
    << localnow.tm_sec;

  filename << "-" << trace_id << ".tmp";
  return filename.str();
}

std::string sanitize(std::string input) {
  for (size_t idx = 0; idx < input.size(); ++idx) {
    char ch = input[idx];
    bool is_valid = (ch >= 'A' && ch <= 'Z') ||
      (ch >= 'a' && ch <= 'z') ||
      (ch >= '0' && ch <= '9') ||
      ch == '-' || ch == '_' || ch == '.';

    if (!is_valid) {
      input[idx] = '_';
    }
  }
  return input;
}

void ensureFolder(const char* folder){
  struct stat stat_out {};
  if (stat(folder, &stat_out)) {
    if (errno != ENOENT) {
      std::string error = std::string("Could not stat() folder ") + folder;
      throw std::system_error(errno, std::system_category(), error);
    }

    // errno == ENOENT, folder needs creating
    if (mkdirat(
      0 /*dirfd, assumes trace_folder is absolute,
          will error out otherwise*/,
      folder,
      S_IRWXU | S_IRWXG)) {

      // Time of check to time of usage race between processes.
      if (errno == EEXIST) {
        return;
      }

      std::string error = std::string("Could not mkdirat() folder ") + folder;
      throw std::system_error(errno, std::system_category(), error);
    }
  }
}

} //namespace anonymous

TraceLifecycleVisitor::TraceLifecycleVisitor(
  const std::string& folder,
  const std::string& trace_prefix,
  std::shared_ptr<TraceCallbacks> callbacks,
  const std::vector<std::pair<std::string, std::string>>& headers,
  int64_t trace_id):

  folder_(folder),
  trace_prefix_(trace_prefix),
  trace_headers_(headers),
  output_(nullptr),
  delegates_(),
  expected_trace_(trace_id),
  callbacks_(callbacks),
  done_(false)
{}

void TraceLifecycleVisitor::visit(const StandardEntry& entry) {
  switch (entry.type) {
    case entries::TRACE_END: {
      int64_t trace_id = entry.extra;
      if (trace_id != expected_trace_) {
        return;
      }
      // write before we clean up state
      if (hasDelegate()) {
        delegates_.back()->visit(entry);
      }
      onTraceEnd(trace_id);
      break;
    }
    case entries::TRACE_TIMEOUT:
    case entries::TRACE_ABORT: {
      int64_t trace_id = entry.extra;
      if (trace_id != expected_trace_) {
        return;
      }
      auto reason = entry.type == entries::TRACE_TIMEOUT ?
        AbortReason::TIMEOUT :
        AbortReason::CONTROLLER_INITIATED;

      // write before we clean up state
      if (hasDelegate()) {
        delegates_.back()->visit(entry);
      }
      onTraceAbort(trace_id, reason);
      break;
    }
    case entries::TRACE_BACKWARDS:
    case entries::TRACE_START: {
      onTraceStart(entry.extra, entry.matchid);
      if (hasDelegate()) {
        delegates_.back()->visit(entry);
      }
      break;
    }
    default : {
      if (hasDelegate()) {
        delegates_.back()->visit(entry);
      }
    }
  }
}

void TraceLifecycleVisitor::visit(const FramesEntry& entry) {
  if (hasDelegate()) {
    delegates_.back()->visit(entry);
  }
}

void TraceLifecycleVisitor::visit(const BytesEntry& entry) {
  if (hasDelegate()) {
    delegates_.back()->visit(entry);
  }
}

void TraceLifecycleVisitor::abort(AbortReason reason) {
  onTraceAbort(expected_trace_, reason);
}

void TraceLifecycleVisitor::onTraceStart(int64_t trace_id, int32_t flags) {
  if (trace_id != expected_trace_) {
    return;
  }

  if (output_ != nullptr) {
    // active trace with same ID, abort
    abort(AbortReason::NEW_START);
    return;
  }

  std::stringstream path_stream;
  std::string trace_id_string = getTraceID(trace_id);
  path_stream << folder_ << '/' << sanitize(trace_id_string);

  //
  // Note that the construction of this path must match the computation in
  // TraceOrchestrator.getSanitizedTraceFolder. Unfortunately, it's far too
  // gnarly to share this code at the moment.
  //
  std::string trace_folder = path_stream.str();
  try {
    ensureFolder(trace_folder.c_str());
  } catch (const std::system_error& ex) {
    // Add more diagnostics to the exception.
    // Namely, parent folder owner uid and gid, as
    // well as our own uid and gid.
    struct stat stat_out{};

    std::stringstream error;
    if (stat(folder_.c_str(), &stat_out)) {
      error << "Could not stat(" << folder_
        << ").\nOriginal exception: " << ex.what();
      throw std::system_error(errno, std::system_category(), error.str());
    }

    error << "Could not create trace folder " << trace_folder
      << ".\nOriginal exception: " << ex.what()
      << ".\nDebug info for " << folder_
      << ": uid=" << stat_out.st_uid << "; gid=" << stat_out.st_gid
      << "; proc euid=" << geteuid() << "; proc egid=" << getegid();
    throw std::system_error(ex.code(), error.str());
  }

  path_stream << '/'
    << sanitize(getTraceFilename(trace_prefix_, trace_id_string));

  std::string trace_file = path_stream.str();

  output_ = std::unique_ptr<zstr::ofstream>(
    new zstr::ofstream(trace_file));

  writeHeaders(*output_, trace_id_string);

  // outputTime = truncate(current) - truncate(prev)
  delegates_.emplace_back(new PrintEntryVisitor(*output_));
  delegates_.emplace_back(new DeltaEncodingVisitor(*delegates_.back()));
  delegates_.emplace_back(new TimestampTruncatingVisitor(
    *delegates_.back(),
    kTimestampPrecision));
  delegates_.emplace_back(new StackTraceInvertingVisitor(
    *delegates_.back()));

  if (callbacks_.get() != nullptr) {
    callbacks_->onTraceStart(trace_id, flags, trace_file);
  }

  done_ = false;
}

void TraceLifecycleVisitor::onTraceAbort(int64_t trace_id, AbortReason reason) {
  done_ = true;
  cleanupState();
  if (callbacks_.get() != nullptr) {
    callbacks_->onTraceAbort(trace_id, reason);
  }
}

void TraceLifecycleVisitor::onTraceEnd(int64_t trace_id) {
  done_ = true;
  cleanupState();
  if (callbacks_.get() != nullptr) {
    callbacks_->onTraceEnd(trace_id);
  }
}

void TraceLifecycleVisitor::cleanupState() {
  delegates_.clear();
  output_ = nullptr;
}

void TraceLifecycleVisitor::writeHeaders(std::ostream& output, std::string id) {
  output
    << "dt\n"
    << "ver|" << kTraceFormatVersion << "\n"
    << "id|" << id << "\n"
    << "prec|" << kTimestampPrecision << "\n";

  for (auto const& header: trace_headers_) {
    output << header.first << '|' << header.second << '\n';
  }

  output << '\n';
}

} // namespace writer
} // namespace loom
} // namespace facebook