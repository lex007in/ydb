#pragma once

#include "public.h"

#include "error.h"

#include <library/cpp/yt/memory/ref.h>

namespace NYT::NKafka {

////////////////////////////////////////////////////////////////////////////////

#define READ_KAFKA_FIELD(field, method)                                                    \
    YT_LOG_TRACE("Parsing kafka data (Field: %v)", #field);                                \
    field = reader->method();                                                              \
    YT_LOG_TRACE("Parsing kafka data, value read (Field: %v, Value: %v)", #field, field);

#define WRITE_KAFKA_FIELD(kafkaWriter, method, field)                                             \
    YT_LOG_TRACE("Writing kafka data (Field: %v, Value: %v)", #field, field);                     \
    kafkaWriter->method(field);

////////////////////////////////////////////////////////////////////////////////

struct IKafkaProtocolReader
{
    virtual ~IKafkaProtocolReader() = default;

    virtual bool ReadBool() = 0;
    virtual char ReadByte() = 0;
    virtual i16 ReadInt16() = 0;
    virtual i32 ReadInt32() = 0;
    virtual i64 ReadInt64() = 0;
    virtual ui32 ReadUint32() = 0;

    virtual i32 ReadVarInt() = 0;
    virtual i64 ReadVarLong() = 0;
    virtual ui32 ReadUnsignedVarInt() = 0;

    virtual TGuid ReadUuid() = 0;

    virtual TString ReadString() = 0;
    virtual TString ReadCompactString() = 0;
    virtual std::optional<TString> ReadNullableString() = 0;
    virtual std::optional<TString> ReadCompactNullableString() = 0;
    virtual void ReadString(TString* result, int length) = 0;

    virtual TString ReadBytes() = 0;
    virtual TString ReadCompactBytes() = 0;

    virtual i32 StartReadBytes(bool needReadSize = true) = 0;
    virtual i32 StartReadCompactBytes(bool needReadCount = true) = 0;
    virtual i32 GetReadBytesCount() = 0;
    virtual void FinishReadBytes() = 0;

    virtual TSharedRef GetSuffix() const = 0;

    //! Returns true if input is fully consumed and false otherwise.
    virtual bool IsFinished() const = 0;
    //! Throws an error if input is not fully consumed. Does nothing otherwise.
    virtual void ValidateFinished() const = 0;
};

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IKafkaProtocolReader> CreateKafkaProtocolReader(TSharedRef data);

////////////////////////////////////////////////////////////////////////////////

struct IKafkaProtocolWriter
{
    virtual ~IKafkaProtocolWriter() = default;

    virtual void WriteBool(bool value) = 0;
    virtual void WriteByte(char value) = 0;
    virtual void WriteInt16(i16 value) = 0;
    virtual void WriteInt32(i32 value) = 0;
    virtual void WriteInt64(i64 value) = 0;
    virtual void WriteUint32(ui32 value) = 0;

    virtual void WriteVarInt(i32 value) = 0;
    virtual void WriteVarLong(i64 value) = 0;
    virtual void WriteUnsignedVarInt(ui32 value) = 0;

    virtual void WriteUuid(TGuid value) = 0;

    virtual void WriteErrorCode(NKafka::EErrorCode value) = 0;

    virtual void WriteString(TStringBuf value) = 0;
    virtual void WriteNullableString(std::optional<TStringBuf> value) = 0;
    virtual void WriteCompactString(TStringBuf value) = 0;
    virtual void WriteCompactNullableString(std::optional<TStringBuf> value) = 0;
    virtual void WriteBytes(TStringBuf value) = 0;
    virtual void WriteCompactBytes(TStringBuf value) = 0;
    virtual void WriteData(TStringBuf value) = 0;
    virtual void WriteData(TRef value) = 0;

    virtual void StartBytes() = 0;
    virtual void FinishBytes() = 0;

    virtual void StartCalculateChecksum() = 0;
    virtual void FinishCalculateChecksum() = 0;

    virtual i64 GetSize() const = 0;

    virtual TSharedRef Finish() = 0;
};

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<IKafkaProtocolWriter> CreateKafkaProtocolWriter();

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NKafka
