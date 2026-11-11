/**
 * @file        core/copyable_file_sink.h
 * @brief       The log file sink, which can let go of its file for a moment so the file can be copied
 *
 * Horizon's file system lets no one open a file that another handle has open for writing: on the Switch the
 * running game's log cannot be read, not even stat()ed, while its sink holds it. A copy therefore needs the file
 * closed. This is spdlog's rotating file sink (same file names, sizes and rotation), wrapped so that CopyTo() can
 * flush and close it, copy it, and reopen it for appending. Used through rex::CopyLogFile (logging.cpp), for a copy of
 * the game log while the game runs. Header-only, so it can be tested on the PC.
 *
 * Deferred mode (log_file_writer, logging.cpp). spdlog's sinks write the file while holding the sink's mutex, so when
 * an SD write is slow every thread that logs waits for it: a Switch Lite showed a 1.5 s frame with Main, the
 * Server, the ring and a job worker all in that mutex while its holder sat in fsdev_write (the file service runs on
 * the core the system keeps, which a 4-core forwarder lets the game load). In deferred mode the threads that log only
 * copy their message into a queue under the sink's mutex; a thread of the sink's own formats and writes them under a
 * second mutex that no logging thread takes. The queue never blocks: past kMaxQueuedBytes lines are dropped and
 * counted, and the count is written once the writer catches up. Drain() and CopyTo() write everything queued first.
 * Unlike spdlog's async logger (log_async, which made things worse: its queue blocked the logging threads when full,
 * and its thread could starve), nothing here waits for the writer.
 */

#pragma once

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/details/log_msg_buffer.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>

namespace rex {

class CopyableFileSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  // Throws spdlog::spdlog_ex when the file cannot be opened, as spdlog's own sink does. deferred: see the file header.
  CopyableFileSink(std::string path, size_t max_size, size_t max_files, bool deferred = false)
      : path_(std::move(path)), max_size_(max_size), max_files_(max_files), deferred_(deferred) {
    Open();
    if (deferred_) {
      writer_ = std::thread([this] { WriterMain(); });
    }
  }

  ~CopyableFileSink() override {
    if (writer_.joinable()) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
      }
      wake_.notify_one();
      writer_.join();  // it writes what is left first
    }
  }

  const std::string& path() const { return path_; }

  // Copies the file as it is now (everything logged so far) to `to`. False if the copy failed.
  bool CopyTo(const std::filesystem::path& to) {
    if (deferred_) {
      WriteQueued(false);
      std::lock_guard<std::mutex> io(io_mutex_);
      return CopyLocked(to);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return CopyLocked(to);
  }

  // Everything logged so far in the file (rex::FlushLogging). In deferred mode it waits for the SD card: not for
  // the game's threads.
  void Drain() {
    if (!deferred_) {
      flush();
      return;
    }
    WriteQueued(true);
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    if (!deferred_) {
      if (inner_) {
        inner_->log(msg);
      }
      return;
    }
    // Under the sink's mutex, as spdlog calls it: only a copy into the queue.
    const size_t bytes = msg.payload.size() + msg.logger_name.size() + kOverheadBytes;
    if (queued_bytes_ + bytes > kMaxQueuedBytes) {
      ++dropped_;
      return;
    }
    queue_.emplace_back(msg);
    queued_bytes_ += bytes;
    if (queue_.size() == kWakeAt) {
      wake_.notify_one();
    }
  }
  void flush_() override {
    if (!deferred_) {
      if (inner_) {
        inner_->flush();
      }
      return;
    }
    flush_requested_ = true;  // the writer flushes; nothing here touches the file
    wake_.notify_one();
  }
  void set_pattern_(const std::string& pattern) override {
    pattern_ = pattern;
    std::lock_guard<std::mutex> io(io_mutex_);
    if (inner_) {
      inner_->set_pattern(pattern);
    }
  }
  void set_formatter_(std::unique_ptr<spdlog::formatter> formatter) override {
    formatter_ = std::move(formatter);
    pattern_.clear();
    std::lock_guard<std::mutex> io(io_mutex_);
    if (inner_) {
      inner_->set_formatter(formatter_->clone());
    }
  }

 private:
  static constexpr size_t kMaxQueuedBytes = size_t(8) << 20;  // past this, lines are dropped, never waited for
  static constexpr size_t kOverheadBytes = 96;                // a queued message's own size, roughly
  static constexpr size_t kWakeAt = 256;                      // messages that wake the writer before its 250 ms

  void Open() {
    inner_ = std::make_unique<spdlog::sinks::rotating_file_sink_st>(path_, max_size_, max_files_, false);
    if (!pattern_.empty()) {
      inner_->set_pattern(pattern_);
    } else if (formatter_) {
      inner_->set_formatter(formatter_->clone());
    }
  }

  // With the file's lock held (the sink's mutex, or io_mutex_ in deferred mode).
  bool CopyLocked(const std::filesystem::path& to) {
    if (!inner_) {
      return false;
    }
    inner_->flush();
    inner_.reset();  // closes the file
    const bool copied = CopyFile(path_, to);
    try {
      Open();  // appends: spdlog opens an existing log in "ab" mode
    } catch (const spdlog::spdlog_ex&) {
      // Nothing to log to: lines are dropped until the next copy reopens it, rather than ending the process.
    }
    return copied;
  }

  // Takes the queue (the sink's mutex, briefly) and writes it (io_mutex_ only). The two locks are never held
  // together in this order and the reverse, so set_pattern_ (sink's mutex, then io_mutex_) cannot deadlock with it.
  void WriteQueued(bool flush) {
    std::vector<spdlog::details::log_msg_buffer> batch;
    size_t dropped = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      batch.swap(queue_);
      queued_bytes_ = 0;
      dropped = std::exchange(dropped_, 0);
      flush |= std::exchange(flush_requested_, false);
    }
    Write(batch, dropped, flush);
  }

  void Write(const std::vector<spdlog::details::log_msg_buffer>& batch, size_t dropped, bool flush) {
    std::lock_guard<std::mutex> io(io_mutex_);
    if (!inner_) {
      return;
    }
    // spdlog throws on a failed write; on the logging thread its logger caught that, here nothing would.
    try {
      if (dropped) {
        const std::string text = std::to_string(dropped) + " log lines dropped: the log's queue was full";
        inner_->log(spdlog::details::log_msg(spdlog::source_loc{}, "log", spdlog::level::warn, text));
      }
      for (const auto& msg : batch) {
        inner_->log(msg);
      }
      if (flush) {
        inner_->flush();
      }
    } catch (const std::exception&) {
      // The lines of this batch are lost; the next one tries again.
    }
  }

  void WriterMain() {
    for (;;) {
      std::vector<spdlog::details::log_msg_buffer> batch;
      size_t dropped = 0;
      bool flush = false;
      bool stop = false;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(250),
                       [this] { return stop_ || flush_requested_ || queue_.size() >= kWakeAt; });
        batch.swap(queue_);
        queued_bytes_ = 0;
        dropped = std::exchange(dropped_, 0);
        flush = std::exchange(flush_requested_, false);
        stop = stop_;
      }
      Write(batch, dropped, flush || stop);
      if (stop) {
        return;
      }
    }
  }

  // Plain streams: std::filesystem::copy_file also copies permissions, which fails on the Switch's SD card.
  static bool CopyFile(const std::string& from, const std::filesystem::path& to) {
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!in || !out) {
      return false;
    }
    std::vector<char> buffer(256 * 1024);
    while (in) {
      in.read(buffer.data(), std::streamsize(buffer.size()));
      const std::streamsize got = in.gcount();
      if (got > 0 && !out.write(buffer.data(), got)) {
        return false;
      }
    }
    out.close();
    return !in.bad() && !out.fail();
  }

  std::string path_;
  size_t max_size_;
  size_t max_files_;
  // Deferred mode. The queue, its size, the dropped count and the flags are under the sink's mutex (mutex_).
  bool deferred_;
  std::string pattern_;
  std::unique_ptr<spdlog::sinks::rotating_file_sink_st> inner_;  // deferred: under io_mutex_
  std::mutex io_mutex_;
  std::condition_variable wake_;
  std::vector<spdlog::details::log_msg_buffer> queue_;
  size_t queued_bytes_ = 0;
  size_t dropped_ = 0;
  bool flush_requested_ = false;
  bool stop_ = false;
  std::thread writer_;
};

}  // namespace rex
