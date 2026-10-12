#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace FastBee {
enum class ModbusFrameAccess : uint8_t { Standard, RelayVendor, Maintenance };
enum class ModbusFrameTransform : uint8_t { None, RelayBatchToggle };
// Transaction-local copy. Do not multiply this payload across retained slots.
struct ModbusOutputFrame {
    static constexpr size_t capacity = 254;
    uint8_t bytes[capacity]{};
    uint16_t length = 0;
    ModbusFrameAccess access = ModbusFrameAccess::Standard;
    ModbusFrameTransform transform = ModbusFrameTransform::None;
    uint32_t hardwareWindowMs = 0;
    uint16_t address() const { return static_cast<uint16_t>((bytes[2] << 8) | bytes[3]); }
    uint16_t word() const { return static_cast<uint16_t>((bytes[4] << 8) | bytes[5]); }
    uint16_t quantity() const { return bytes[1] == 15 || bytes[1] == 16 ? word() : 1; }
    bool valid() const {
        if (length < 6 || length > capacity || bytes[0] > 247 || hardwareWindowMs > 26000) return false;
        if (hardwareWindowMs && (access==ModbusFrameAccess::Maintenance ||
            transform!=ModbusFrameTransform::None || (bytes[1]!=5 && bytes[1]!=6 && bytes[1]!=16))) return false;
        const uint8_t fc = bytes[1];
        const uint16_t value = word();
        if (transform != ModbusFrameTransform::None &&
            (transform != ModbusFrameTransform::RelayBatchToggle ||
             access != ModbusFrameAccess::Standard || fc != 6 || length != 6)) return false;
        if (access == ModbusFrameAccess::Maintenance) {
            if (transform != ModbusFrameTransform::None) return false;
            if (!bytes[0]) return fc == 16 && length == 9 && value == 1 && bytes[6] == 2 &&
                !bytes[7] && bytes[8] >= 1 && bytes[8] <= 247;
            return length == 6 && (fc == 6 ||
                (fc == 0xb0 && !bytes[2] && !bytes[3] && !bytes[5]));
        }
        if (!bytes[0]) return false;
        if (access == ModbusFrameAccess::RelayVendor)
            return transform == ModbusFrameTransform::None && length == 6 && fc == 5 &&
                value != 0 && (value & 0xffU) == 0;
        if (access != ModbusFrameAccess::Standard) return false;
        if (fc == 5) return length == 6 && (value == 0 || value == 0xff00);
        if (fc == 6) return length == 6;
        if (fc != 15 && fc != 16) return false;
        const uint32_t count = value;
        const uint32_t bytesNeeded = fc == 15 ? (count + 7) / 8 : count * 2;
        if (!count || count > (fc == 15 ? 1968U : 123U) ||
            uint32_t(address()) + count > 65536U || bytesNeeded > 247 ||
            length != bytesNeeded + 7 || bytes[6] != bytesNeeded) return false;
        // Trailing unused coil bits cannot smuggle an out-of-range output.
        return fc != 15 || !(count & 7U) ||
            (bytes[length - 1] & static_cast<uint8_t>(0xffU << (count & 7U))) == 0;
    }
    bool assign(const uint8_t* data, size_t size,
                ModbusFrameAccess scope = ModbusFrameAccess::Standard) {
        length = 0; access = scope; transform = ModbusFrameTransform::None; hardwareWindowMs=0;
        if (!data || size < 6 || size > capacity) return false;
        std::memmove(bytes, data, size); length = static_cast<uint16_t>(size);
        if (valid()) return true;
        length = 0; return false;
    }
    bool single(uint8_t slave, uint8_t function, uint16_t addressValue, uint16_t value,
                ModbusFrameAccess scope = ModbusFrameAccess::Standard) {
        const uint8_t data[6] = {slave,function,static_cast<uint8_t>(addressValue >> 8),
            static_cast<uint8_t>(addressValue),static_cast<uint8_t>(value >> 8),static_cast<uint8_t>(value)};
        return assign(data,sizeof(data),scope);
    }
    bool same(const ModbusOutputFrame& other) const {
        return length == other.length && length <= capacity && access == other.access &&
            transform == other.transform && hardwareWindowMs == other.hardwareWindowMs && std::memcmp(bytes,other.bytes,length) == 0;
    }
};
} // namespace FastBee
