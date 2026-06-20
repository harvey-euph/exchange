#include "mmap_log.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <iomanip>
#include <sstream>

namespace mmaplog {

static std::string get_filename(const std::string& dir, uint32_t index) {
    std::ostringstream oss;
    oss << dir << "/journal_" << std::setw(8) << std::setfill('0') << index << ".log";
    return oss.str();
}

MmapWriter::MmapWriter(const std::string& dir, size_t max_file_size)
    : dir_(dir), max_file_size_(max_file_size), current_file_index_(0), fd_(-1), mapped_addr_(nullptr), current_offset_(0) {
    
    // For this simple implementation, we always start at file 0 and overwrite.
    // In a real system, you'd scan the directory to find the latest file.
    open_file(0);
}

MmapWriter::~MmapWriter() {
    if (mapped_addr_) munmap(mapped_addr_, max_file_size_);
    if (fd_ != -1) close(fd_);
}

void MmapWriter::open_file(uint32_t index) {
    if (mapped_addr_) {
        munmap(mapped_addr_, max_file_size_);
        close(fd_);
    }
    current_file_index_ = index;
    std::string path = get_filename(dir_, current_file_index_);
    
    // Open file
    fd_ = open(path.c_str(), O_RDWR | O_CREAT, 0666);
    if (fd_ == -1) throw std::runtime_error("Failed to open mmap file");
    
    // Pre-allocate to target size. This guarantees space and zero-fills.
    if (ftruncate(fd_, max_file_size_) == -1) {
        throw std::runtime_error("Failed to ftruncate mmap file");
    }

    // Map into memory
    mapped_addr_ = (uint8_t*)mmap(nullptr, max_file_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (mapped_addr_ == MAP_FAILED) throw std::runtime_error("Failed to mmap file");
    
    current_offset_ = 0;
}

void* MmapWriter::reserve(uint32_t len, uint64_t& out_offset) {
    if (len == 0 || len == EOF_MARKER) return nullptr; // Invalid payload size
    
    size_t required_space = sizeof(RecordHeader) + len;
    
    if (current_offset_ + required_space > max_file_size_ - sizeof(RecordHeader)) {
        // Not enough space. Write EOF marker to tell readers to switch files.
        RecordHeader* header = reinterpret_cast<RecordHeader*>(mapped_addr_ + current_offset_);
        header->published_length.store(EOF_MARKER, std::memory_order_release);
        
        // Rollover to the next file
        open_file(current_file_index_ + 1);
    }
    
    out_offset = (static_cast<uint64_t>(current_file_index_) << 32) | current_offset_;
    
    RecordHeader* header = reinterpret_cast<RecordHeader*>(mapped_addr_ + current_offset_);
    header->reserved_length = len; // Store len for commit to use later
    header->published_length.store(0, std::memory_order_relaxed); // Ensure it's 0 (it should be due to ftruncate)
    
    void* payload_ptr = mapped_addr_ + current_offset_ + sizeof(RecordHeader);
    
    current_offset_ += required_space;
    
    return payload_ptr;
}

void MmapWriter::commit(void* payload_ptr) {
    // Backtrack to the header
    RecordHeader* header = reinterpret_cast<RecordHeader*>(static_cast<uint8_t*>(payload_ptr) - sizeof(RecordHeader));
    
    // Release semantics ensure the payload modifications are visible to readers BEFORE the length is published.
    header->published_length.store(header->reserved_length, std::memory_order_release);
}

uint64_t MmapWriter::append(const void* data, uint32_t len) {
    uint64_t offset;
    void* ptr = reserve(len, offset);
    if (!ptr) { return INVALID_OFFSET; }
    
    std::memcpy(ptr, data, len);
    commit(ptr);
    return offset;
}

// ================= Reader implementation =================

MmapReader::MmapReader(const std::string& dir)
    : dir_(dir), current_file_index_(0), fd_(-1), mapped_addr_(nullptr), current_offset_(0), file_size_(0) {
    open_file(0);
}

MmapReader::~MmapReader() {
    if (mapped_addr_) munmap(mapped_addr_, file_size_);
    if (fd_ != -1) close(fd_);
}

void MmapReader::open_file(uint32_t index) {
    if (mapped_addr_) {
        munmap(mapped_addr_, file_size_);
        mapped_addr_ = nullptr;
    }
    if (fd_ != -1) {
        close(fd_);
        fd_ = -1;
    }
    
    current_file_index_ = index;
    std::string path = get_filename(dir_, current_file_index_);
    fd_ = open(path.c_str(), O_RDONLY);
    if (fd_ == -1) {
        return; 
    }
    
    struct stat st;
    if (fstat(fd_, &st) == -1) {
        close(fd_);
        fd_ = -1;
        return;
    }
    
    file_size_ = st.st_size;
    if (file_size_ == 0) {
        close(fd_);
        fd_ = -1;
        return;
    }
    
    mapped_addr_ = (uint8_t*)mmap(nullptr, file_size_, PROT_READ, MAP_SHARED, fd_, 0);
    if (mapped_addr_ == MAP_FAILED) {
        mapped_addr_ = nullptr;
        close(fd_);
        fd_ = -1;
        return;
    }
    current_offset_ = 0;
}

bool MmapReader::read_next(const void*& data, uint32_t& len) {
    if (!mapped_addr_) {
        open_file(current_file_index_);
        if (!mapped_addr_) return false; // Still no file
    }

    if (current_offset_ + sizeof(RecordHeader) > file_size_) return false; // Exceeded bounds

    RecordHeader* header = reinterpret_cast<RecordHeader*>(mapped_addr_ + current_offset_);
    
    // Acquire semantics ensure we don't read payload until length is safely visible
    uint32_t record_len = header->published_length.load(std::memory_order_acquire);
    
    if (record_len == 0) {
        // Not written yet by Producer
        return false;
    }
    
    if (record_len == EOF_MARKER) {
        // Producer marked EOF, jump to next file
        open_file(current_file_index_ + 1);
        return read_next(data, len); // Recursive retry on the next file
    }
    
    // Zero-copy: Pass the memory mapped pointer directly to the caller
    data = mapped_addr_ + current_offset_ + sizeof(RecordHeader);
    len = record_len;
    current_offset_ += sizeof(RecordHeader) + len;
    
    return true;
}

bool MmapReader::seek(uint64_t offset) {
    uint32_t target_file = static_cast<uint32_t>(offset >> 32);
    size_t target_offset = static_cast<size_t>(offset & 0xFFFFFFFF);
    
    if (target_file != current_file_index_ || !mapped_addr_) {
        open_file(target_file);
    }
    if (!mapped_addr_) return false; // File doesn't exist
    if (target_offset > file_size_) return false;
    
    current_offset_ = target_offset;
    return true;
}

} // namespace mmaplog
