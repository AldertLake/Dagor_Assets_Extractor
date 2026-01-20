#include "BinaryReader.h"

namespace DagorIO {

MemoryReader::MemoryReader(std::span<const uint8_t> data) : m_data(data) {}

MemoryReader::MemoryReader(const uint8_t* data, size_t size) : m_data(data, size) {}

template <typename T>
T MemoryReader::ReadType() {
    if (GetRemaining() < sizeof(T)) {
        throw std::out_of_range("MemoryReader out of range");
    }
    T value;
    std::memcpy(&value, m_data.data() + m_position, sizeof(T));
    m_position += sizeof(T);
    return value;
}

uint8_t MemoryReader::ReadUInt8() { return ReadType<uint8_t>(); }
int8_t MemoryReader::ReadInt8() { return ReadType<int8_t>(); }
uint16_t MemoryReader::ReadUInt16() { return ReadType<uint16_t>(); }
int16_t MemoryReader::ReadInt16() { return ReadType<int16_t>(); }
uint32_t MemoryReader::ReadUInt32() { return ReadType<uint32_t>(); }
int32_t MemoryReader::ReadInt32() { return ReadType<int32_t>(); }
uint64_t MemoryReader::ReadUInt64() { return ReadType<uint64_t>(); }
int64_t MemoryReader::ReadInt64() { return ReadType<int64_t>(); }
float MemoryReader::ReadFloat() { return ReadType<float>(); }
double MemoryReader::ReadDouble() { return ReadType<double>(); }

void MemoryReader::ReadBytes(void* dest, size_t count) {
    if (GetRemaining() < count) {
        throw std::out_of_range("MemoryReader out of range");
    }
    std::memcpy(dest, m_data.data() + m_position, count);
    m_position += count;
}

std::span<const uint8_t> MemoryReader::ReadSpan(size_t count) {
    if (GetRemaining() < count) {
        throw std::out_of_range("MemoryReader out of range");
    }
    auto span = m_data.subspan(m_position, count);
    m_position += count;
    return span;
}

std::string MemoryReader::ReadString(size_t length) {
    if (GetRemaining() < length) {
        throw std::out_of_range("MemoryReader out of range");
    }
    std::string str(reinterpret_cast<const char*>(m_data.data() + m_position), length);
    m_position += length;
    return str;
}

std::string MemoryReader::ReadNullTerminatedString() {
    size_t length = 0;
    while (m_position + length < m_data.size() && m_data[m_position + length] != '\0') {
        length++;
    }
    std::string str = ReadString(length);
    if (m_position < m_data.size() && m_data[m_position] == '\0') {
        m_position++; // skip null terminator
    }
    return str;
}

void MemoryReader::Seek(size_t position) {
    if (position > m_data.size()) {
        throw std::out_of_range("MemoryReader seek out of range");
    }
    m_position = position;
}

void MemoryReader::Skip(size_t count) {
    if (GetRemaining() < count) {
        throw std::out_of_range("MemoryReader skip out of range");
    }
    m_position += count;
}

size_t MemoryReader::GetPosition() const { return m_position; }
size_t MemoryReader::GetSize() const { return m_data.size(); }
size_t MemoryReader::GetRemaining() const { return m_data.size() - m_position; }
bool MemoryReader::IsEOF() const { return m_position >= m_data.size(); }

} // namespace DagorIO