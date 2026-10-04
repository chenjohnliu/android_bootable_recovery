// Checked raw/sparse image I/O for the additional m11q image targets.
#pragma once
#include <sparse/sparse.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <string>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

inline std::string M11qLogicalImageVolume(const std::string& point) {
    for (const char* name : {"system", "vendor", "product", "odm"})
        if (point == std::string("/") + name + "_image") return std::string("/") + name;
    return "";
}
inline std::string M11qPhysicalImageBlock(const std::string& point) {
    for (const char* name : {"persist", "optics", "prism", "efs", "sec_efs"})
        if (point == std::string("/") + name + "_image")
            return std::string("/dev/block/bootdevice/by-name/") + name;
    return "";
}
inline std::string M11qImageVolume(const std::string& point) {
    const std::string logical = M11qLogicalImageVolume(point);
    if (!logical.empty()) return logical;
    for (const char* name : {"persist", "optics", "prism", "efs", "sec_efs"})
        if (point == std::string("/") + name + "_image") return std::string("/") + name;
    return "";
}
inline bool M11qExtraImageTarget(const std::string& point) {
    return point == "/dtbo" || !M11qImageVolume(point).empty();
}
inline bool M11qDeviceMounted(const std::string& mountinfo, unsigned device_major, unsigned device_minor) {
    std::istringstream input(mountinfo);
    std::string line;
    while (std::getline(input, line)) {
        unsigned found_major = 0, found_minor = 0;
        if (sscanf(line.c_str(), "%*u %*u %u:%u", &found_major, &found_minor) == 2 &&
            found_major == device_major && found_minor == device_minor) return true;
    }
    return false;
}

class M11qImageFile {
public:
    M11qImageFile() = default;
    M11qImageFile(const M11qImageFile&) = delete;
    M11qImageFile& operator=(const M11qImageFile&) = delete;
    ~M11qImageFile() {
        if (sparse_) sparse_file_destroy(sparse_);
        if (fd_ >= 0) close(fd_);
    }
    bool Open(const std::string& path, uint64_t capacity = UINT64_MAX) {
        if (fd_ >= 0) return false;
        fd_ = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        unsigned char magic[4];
        if (fd_ < 0 || fstat(fd_, &stat_) || !S_ISREG(stat_.st_mode) || stat_.st_size < 4 ||
            !ReadAt(fd_, magic, sizeof(magic), 0)) return false;
        bytes_ = stat_.st_size;
        if (magic[0] == 0x3a && magic[1] == 0xff && magic[2] == 0x26 && magic[3] == 0xed) {
            unsigned char header[28];
            if (!ReadAt(fd_, header, sizeof(header), 0)) { bytes_ = 0; return false; }
            auto le32 = [](const unsigned char* p) {
                return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
            };
            bytes_ = uint64_t(le32(header + 12)) * le32(header + 16);
            if (!bytes_ || bytes_ > capacity) return false;
            // Official parser validates all chunks before any destination is opened for writing.
            // CRC mode also reads RAW data, catching truncated final payloads.
            sparse_ = sparse_file_import(fd_, false, true);
            if (!sparse_) { bytes_ = 0; return false; }
            int64_t expanded = sparse_file_len(sparse_, false, false);
            if (expanded <= 0) { bytes_ = 0; return false; }
            bytes_ = static_cast<uint64_t>(expanded);
            uint64_t validated = 0;
            if (sparse_file_callback(sparse_, false, false, Validate, &validated) || validated != bytes_) {
                bytes_ = 0; return false;
            }
        }
        if (bytes_ > capacity) return false;
        valid_ = bytes_ > 0;
        return valid_;
    }
    uint64_t Bytes() const { return bytes_; }
    bool Fits(uint64_t capacity) const { return valid_ && bytes_ <= capacity; }
    bool OnDevice(dev_t device) const { return valid_ && stat_.st_dev == device; }
    bool WriteAndVerify(int target, uint64_t capacity,
                        const std::function<void(uint64_t)>& progress = {}) {
        if (!Fits(capacity)) return false;
        Context write{target, 0, bytes_, false, progress};
        if (!Run(write) || fsync(target)) return false;
        Context verify{target, 0, bytes_, true, {}};
        return Run(verify);
    }
private:
    static int Validate(void* opaque, const void*, size_t length) {
        auto& offset = *static_cast<uint64_t*>(opaque);
        if (length > UINT64_MAX - offset) return -EOVERFLOW;
        offset += length;
        return 0;
    }
    struct Context {
        int target;
        uint64_t offset, limit;
        bool verify;
        std::function<void(uint64_t)> progress;
    };
    static bool ReadAt(int fd, void* data, size_t length, uint64_t offset) {
        size_t done = 0;
        while (done < length) {
            ssize_t n = pread(fd, static_cast<unsigned char*>(data) + done, length - done, offset + done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            done += n;
        }
        return true;
    }
    static int Emit(void* opaque, const void* data, size_t length) {
        auto& ctx = *static_cast<Context*>(opaque);
        if (ctx.offset > ctx.limit || length > ctx.limit - ctx.offset) return -EOVERFLOW;
        if (data) {
            const auto* bytes = static_cast<const unsigned char*>(data);
            unsigned char buffer[65536];
            size_t done = 0;
            while (done < length) {
                size_t count = std::min(sizeof(buffer), length - done);
                if (ctx.verify) {
                    if (!ReadAt(ctx.target, buffer, count, ctx.offset + done) ||
                        memcmp(buffer, bytes + done, count)) return -EIO;
                    done += count;
                } else {
                    ssize_t n = pwrite(ctx.target, bytes + done, count, ctx.offset + done);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) return -EIO;
                    done += n;
                }
            }
        }
        // Sparse DONT_CARE ranges retain existing contents; verify only specified data.
        ctx.offset += length;
        if (ctx.progress) ctx.progress(ctx.offset);
        return 0;
    }
    bool Run(Context& ctx) {
        if (sparse_) {
            if (sparse_file_callback(sparse_, false, false, Emit, &ctx)) return false;
        } else {
            unsigned char buffer[65536];
            while (ctx.offset < bytes_) {
                size_t count = std::min<uint64_t>(sizeof(buffer), bytes_ - ctx.offset);
                if (!ReadAt(fd_, buffer, count, ctx.offset) || Emit(&ctx, buffer, count)) return false;
            }
        }
        return ctx.offset == bytes_;
    }
    int fd_ = -1;
    struct stat stat_{};
    struct sparse_file* sparse_ = nullptr;
    uint64_t bytes_ = 0;
    bool valid_ = false;
};
