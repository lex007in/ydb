#include "retryless_writer.h"

#include <yt/cpp/mapreduce/interface/logging/yt_log.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

TRetrylessWriter::~TRetrylessWriter()
{
    NDetail::FinishOrDie(this, AutoFinish_, "TRetrylessWriter");
}

void TRetrylessWriter::DoFinish()
{
    if (!Running_) {
        return;
    }
    Running_ = false;
    Output_->Finish();
}

void TRetrylessWriter::DoWrite(const void* buf, size_t len)
{
    try {
        Output_->Write(buf, len);
    } catch (...) {
        Running_ = false;
        throw;
    }
}

void TRetrylessWriter::NotifyRowEnd()
{ }

void TRetrylessWriter::Abort()
{
    Running_ = false;
}

size_t TRetrylessWriter::GetBufferMemoryUsage() const
{
    return BufferSize_;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
