// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_BYTE_STREAM_H_
#define CONTENT_BROWSER_BYTE_STREAM_H_

#include <stddef.h>

#include <memory>

#include "base/callback.h"
#include "base/memory/ref_counted.h"
#include "base/task/post_task.h"
#include "content/common/content_export.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "net/base/io_buffer.h"

namespace base {
class SequencedTaskRunner;
}

namespace content {

// A byte stream is a pipe to transfer bytes between a source and a
// sink, which may be on different threads.  It is intended to be the
// only connection between source and sink; they need have no
// direct awareness of each other aside from the byte stream.  The source and
// the sink have different interfaces to a byte stream, |ByteStreamWriter|
// and |ByteStreamReader|.  A pair of connected interfaces is generated by
// calling |CreateByteStream|.
//
// The source adds bytes to the bytestream via |ByteStreamWriter::Write|
// and the sink retrieves bytes already written via |ByteStreamReader::Read|.
//
// When the source has no more data to add, it will call
// |ByteStreamWriter::Close| to indicate that.  Operation status at the source
// is indicated to the sink via an int passed to the Close() method and returned
// from the GetStatus() method. Source and sink must agree on the interpretation
// of this int.
//
// Normally the source is not managed after the relationship is setup;
// it is expected to provide data and then close itself.  If an error
// occurs on the sink, it is not signalled to the source via this
// mechanism; instead, the source will write data until it exausts the
// available space.  If the source needs to be aware of errors occuring
// on the sink, this must be signalled in some other fashion (usually
// through whatever controller setup the relationship).
//
// Callback lifetime management: No lifetime management is done in this
// class to prevent registered callbacks from being called after any
// objects to which they may refer have been destroyed.  It is the
// responsibility of the callers to avoid use-after-free references.
// This may be done by any of several mechanisms, including weak
// pointers, scoped_refptr references, or calling the registration
// function with a null callback from a destructor.  To enable the null
// callback strategy, callbacks will not be stored between retrieval and
// evaluation, so setting a null callback will guarantee that the
// previous callback will not be executed after setting.
//
// Class methods are virtual to allow mocking for tests; these classes
// aren't intended to be base classes for other classes.
//
// Sample usage (note that this does not show callback usage):
//
//    void OriginatingClass::Initialize() {
//      // Create a stream for sending bytes from IO->FILE threads.
//      std::unique_ptr<ByteStreamWriter> writer;
//      std::unique_ptr<ByteStreamReader> reader;
//      CreateByteStream(
//          GetIOThreadTaskRunner({}),
//          base::ThreadPool::CreateSequencedTaskRunner({base::MayBlock, ...}),
//          kStreamBufferSize /* e.g. 10240.  */,
//          &writer,
//          &reader);         // Presumed passed to FILE thread for reading.
//
//      // Setup callback for writing.
//      writer->RegisterCallback(base::BindRepeating(&SpaceAvailable, this));
//
//      // Do initial round of writing.
//      SpaceAvailable();
//    }
//
//    // May only be run on first argument task runner, in this case the IO
//    // thread.
//    void OriginatingClass::SpaceAvailable() {
//      while (<data available>) {
//        std::unique_ptr<net::IOBuffer> buffer;
//        size_t buffer_length;
//        // Create IOBuffer, fill in with data, and set buffer_length.
//        if (!writer->Write(buffer, buffer_length)) {
//          // No more space; return and we'll be called again
//          // when there is space.
//          return;
//        }
//      }
//      writer->Close(<operation status>);
//      writer.reset(NULL);
//    }
//
//    // On File thread; containing class setup not shown.
//
//    void ReceivingClass::Initialize() {
//      // Initialization
//      reader->RegisterCallback(base::BindRepeating(&DataAvailable, obj));
//    }
//
//    // Called whenever there's something to read.
//    void ReceivingClass::DataAvailable() {
//      scoped_refptr<net::IOBuffer> data;
//      size_t length = 0;
//
//      while (ByteStreamReader::STREAM_HAS_DATA ==
//             (state = reader->Read(&data, &length))) {
//        // Process |data|.
//      }
//
//      if (ByteStreamReader::STREAM_COMPLETE == state) {
//        int status = reader->GetStatus();
//        // Process error or successful completion in |status|.
//      }
//
//      // if |state| is STREAM_EMPTY, we're done for now; we'll be called
//      // again when there's more data.
//    }
class CONTENT_EXPORT ByteStreamWriter {
 public:
  // Inverse of the fraction of the stream buffer that must be full before
  // a notification is sent to paired Reader that there's more data.
  static const int kFractionBufferBeforeSending;

  virtual ~ByteStreamWriter() = 0;

  // Always adds the data passed into the ByteStream.  Returns true
  // if more data may be added without exceeding the class limit
  // on data.  Takes ownership of |buffer|.
  virtual bool Write(scoped_refptr<net::IOBuffer> buffer,
                     size_t byte_count) = 0;

  // Flushes contents buffered in this writer to the corresponding reader
  // regardless if buffer filling rate is greater than
  // kFractionBufferBeforeSending or not. Does nothing if there's no contents
  // buffered.
  virtual void Flush() = 0;

  // Signal that all data that is going to be sent, has been sent,
  // and provide a status.
  virtual void Close(int status) = 0;

  // Register a callback to be called when the stream transitions from
  // full to having space available.  The callback will always be
  // called on the task runner associated with the ByteStreamWriter.
  // This callback will only be called if a call to Write has previously
  // returned false (i.e. the ByteStream has been filled).
  // Multiple calls to this function are supported, though note that it
  // is the callers responsibility to handle races with space becoming
  // available (i.e. in the case of that race either of the before
  // or after callbacks may be called).
  // The callback will not be called after ByteStreamWriter destruction.
  virtual void RegisterCallback(base::RepeatingClosure source_callback) = 0;

  // Returns the number of bytes sent to the reader but not yet reported by
  // the reader as read.
  virtual size_t GetTotalBufferedBytes() const = 0;
};

class CONTENT_EXPORT ByteStreamReader {
 public:
  // Inverse of the fraction of the stream buffer that must be empty before
  // a notification is send to paired Writer that there's more room.
  static const int kFractionReadBeforeWindowUpdate;

  enum StreamState { STREAM_EMPTY, STREAM_HAS_DATA, STREAM_COMPLETE };

  virtual ~ByteStreamReader() = 0;

  // Returns STREAM_EMPTY if there is no data on the ByteStream and
  // Close() has not been called, and STREAM_COMPLETE if there
  // is no data on the ByteStream and Close() has been called.
  // If there is data on the ByteStream, returns STREAM_HAS_DATA
  // and fills in |*data| with a pointer to the data, and |*length|
  // with its length.
  virtual StreamState Read(scoped_refptr<net::IOBuffer>* data,
                           size_t* length) = 0;

  // Only valid to call if Read() has returned STREAM_COMPLETE.
  virtual int GetStatus() const = 0;

  // Register a callback to be called when data is added or the source
  // completes.  The callback will be always be called on the owning
  // task runner.  Multiple calls to this function are supported,
  // though note that it is the callers responsibility to handle races
  // with data becoming available (i.e. in the case of that race
  // either of the before or after callbacks may be called).
  // The callback will not be called after ByteStreamReader destruction.
  virtual void RegisterCallback(base::RepeatingClosure sink_callback) = 0;
};

CONTENT_EXPORT void CreateByteStream(
    scoped_refptr<base::SequencedTaskRunner> input_task_runner,
    scoped_refptr<base::SequencedTaskRunner> output_task_runner,
    size_t buffer_size,
    std::unique_ptr<ByteStreamWriter>* input,
    std::unique_ptr<ByteStreamReader>* output);

}  // namespace content

#endif  // CONTENT_BROWSER_BYTE_STREAM_H_
