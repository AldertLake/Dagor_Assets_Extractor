#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <cstring>

namespace DagorIO {

class MemoryReader {
public:
    explicit MemoryReader(std::span<const uint8_t> data);
    MemoryReader(const uint8_t* data, size_t size);

    // Primitive readers
    uint8_t ReadUInt8();
    int8_t ReadInt8();
    uint16_t ReadUInt16();
    int16_t ReadInt16();
    uint32_t ReadUInt32();
    int32_t ReadInt32();
    uint64_t ReadUInt64();
    int64_t ReadInt64();
    float ReadFloat();
    double ReadDouble();

    // Read blocks of data
    void ReadBytes(void* dest, size_t count);
    std::span<const uint8_t> ReadSpan(size_t count);
    
    // String readers
    std::string ReadString(size_t length);
    std::string ReadNullTerminatedString();

    // Navigation
    void Seek(size_t position);
    void Skip(size_t count);
    size_t GetPosition() const;
    size_t GetSize() const;
    size_t GetRemaining() const;
    bool IsEOF() const;

private:
    template <typename T>
    T ReadType();

    std::span<const uint8_t> m_data;
    size_t m_position = 0;
};

} // namespace DagorIO