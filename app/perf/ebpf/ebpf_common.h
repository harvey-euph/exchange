#ifndef EBPF_COMMON_H
#define EBPF_COMMON_H

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

struct recv_ctx {
    struct sock *sk;
    uint8_t iter_type;
    uint8_t padding[7]; // align to 8-byte boundary
    size_t iov_offset;
    void *ubuf;
    const struct iovec *iov;
    uint32_t nr_segs;
    uint64_t ts0;
};

static __always_inline void copy_iov_iter(void *dst, uint8_t iter_type, size_t iov_offset, void *ubuf, const struct iovec *iov, uint32_t nr_segs)
{
    __builtin_memset(dst, 0, 512);

    if (iter_type == 0) { // ITER_UBUF
        if (ubuf) {
            bpf_probe_read_user(dst, 512, ubuf + iov_offset);
        }
    } else if (iter_type == 1) { // ITER_IOVEC
        if (iov) {
            struct iovec local_iov[2];
            __builtin_memset(local_iov, 0, sizeof(local_iov));

            bpf_probe_read_kernel(local_iov, sizeof(local_iov), iov);

            size_t offset = iov_offset;
            size_t copied = 0;

            // Segment 0
            if (offset < local_iov[0].iov_len) {
                void *base = local_iov[0].iov_base + offset;
                size_t avail = local_iov[0].iov_len - offset;
                size_t to_copy = avail;
                if (to_copy > 512) {
                    to_copy = 512;
                }
                bpf_probe_read_user(dst, to_copy, base);
                copied = to_copy;
                offset = 0;
            } else {
                offset -= local_iov[0].iov_len;
            }

            // Segment 1
            if (copied < 512 && offset < local_iov[1].iov_len) {
                void *base = local_iov[1].iov_base + offset;
                size_t avail = local_iov[1].iov_len - offset;
                size_t to_copy = 512 - copied;
                if (avail < to_copy) {
                    to_copy = avail;
                }

                unsigned int copy_offset = copied & 15;
                if (to_copy > 497) {
                    to_copy = 497;
                }
                bpf_probe_read_user((char *)dst + copy_offset, to_copy, base);
            }
        }
    }
}

// BPF-side safe unmasking readers for FlatBuffers
static __always_inline bool read_unmasked_1(const uint8_t *payload, uint32_t len, uint32_t payload_offset, uint32_t idx, uint32_t masking_key, bool mask, uint8_t *out) {
    uint32_t byte_pos = payload_offset + idx;
    if (byte_pos > 511 || byte_pos >= len) return false;
    uint8_t val = payload[byte_pos];
    if (mask) {
        uint32_t shift = (idx & 3) * 8;
        uint8_t mask_val = (uint8_t)(masking_key >> shift);
        val ^= mask_val;
    }
    *out = val;
    return true;
}

static __always_inline bool read_unmasked_2(const uint8_t *payload, uint32_t len, uint32_t payload_offset, uint32_t idx, uint32_t masking_key, bool mask, uint16_t *out) {
    uint32_t byte_pos = payload_offset + idx;
    if (byte_pos > 510 || byte_pos + 2 > len) return false;
    uint16_t val = *(const uint16_t*)(&payload[byte_pos]);
    if (mask) {
        uint32_t shift = (idx & 3) * 8;
        uint16_t mask_val = (uint16_t)(masking_key >> shift);
        if ((idx & 3) == 3) {
            mask_val = (uint16_t)(masking_key >> 24) | ((uint16_t)(masking_key & 0xFF) << 8);
        }
        val ^= mask_val;
    }
    *out = val;
    return true;
}

static __always_inline bool read_unmasked_4(const uint8_t *payload, uint32_t len, uint32_t payload_offset, uint32_t idx, uint32_t masking_key, bool mask, uint32_t *out) {
    uint32_t byte_pos = payload_offset + idx;
    if (byte_pos > 508 || byte_pos + 4 > len) return false;
    uint32_t val = *(const uint32_t*)(&payload[byte_pos]);
    if (mask) {
        uint32_t shift = (idx & 3) * 8;
        uint32_t rotated_mask = (masking_key >> shift) | (masking_key << (32 - shift));
        val ^= rotated_mask;
    }
    *out = val;
    return true;
}

static __always_inline bool read_unmasked_8(const uint8_t *payload, uint32_t len, uint32_t payload_offset, uint32_t idx, uint32_t masking_key, bool mask, uint64_t *out) {
    uint32_t low = 0, high = 0;
    if (!read_unmasked_4(payload, len, payload_offset, idx, masking_key, mask, &low)) return false;
    if (!read_unmasked_4(payload, len, payload_offset, idx + 4, masking_key, mask, &high)) return false;
    *out = ((uint64_t)high << 32) | low;
    return true;
}

static __always_inline bool parse_rx_exec_id(const uint8_t *payload, uint32_t len, uint64_t *out_exec_id) {
    uint32_t curr_offset = 0;

    for (int frame_idx = 0; frame_idx < 4; frame_idx++) {
        if (curr_offset > 510 || curr_offset + 2 > len) break;

        uint8_t opcode = payload[curr_offset] & 0x0F;
        if (opcode != 0x2) break; // Binary frame only

        uint8_t len_field = payload[curr_offset + 1];
        bool mask = (len_field & 0x80) >> 7;
        uint32_t payload_len_field = len_field & 0x7F;

        uint32_t header_len = 2;
        uint64_t actual_payload_len = payload_len_field;

        if (payload_len_field == 126) {
            if (curr_offset > 508 || curr_offset + 4 > len) break;
            uint16_t val16 = *(const uint16_t*)(&payload[curr_offset + 2]);
            actual_payload_len = __builtin_bswap16(val16);
            header_len = 4;
        } else if (payload_len_field == 127) {
            if (curr_offset > 502 || curr_offset + 10 > len) break;
            uint64_t val64 = *(const uint64_t*)(&payload[curr_offset + 2]);
            actual_payload_len = __builtin_bswap64(val64);
            header_len = 10;
        }

        uint32_t payload_offset = curr_offset + header_len + (mask ? 4 : 0);
        if (payload_offset >= len || payload_offset >= 512) break;

        uint32_t masking_key = 0;
        if (mask) {
            uint32_t pos = curr_offset + header_len;
            if (pos > 508 || pos + 4 > len) break;
            masking_key = *(const uint32_t*)(&payload[pos]);
        }

        // Parse FlatBuffers starting at payload_offset
        uint32_t root_offset = 0;
        if (!read_unmasked_4(payload, len, payload_offset, 0, masking_key, mask, &root_offset)) goto next_frame;
        
        uint32_t client_req_pos = root_offset;
        int32_t vtable_offset = 0;
        if (!read_unmasked_4(payload, len, payload_offset, client_req_pos, masking_key, mask, (uint32_t*)&vtable_offset)) goto next_frame;
        
        uint32_t vtable_pos = client_req_pos - vtable_offset;
        uint16_t data_type_offset = 0;
        if (!read_unmasked_2(payload, len, payload_offset, vtable_pos + 4, masking_key, mask, &data_type_offset)) goto next_frame;
        
        uint8_t data_type = 0;
        if (data_type_offset) {
            read_unmasked_1(payload, len, payload_offset, client_req_pos + data_type_offset, masking_key, mask, &data_type);
        }
        if (data_type != 1) goto next_frame; // ClientRequestData_OrderRequest
        
        uint16_t data_offset_field = 0;
        if (!read_unmasked_2(payload, len, payload_offset, vtable_pos + 6, masking_key, mask, &data_offset_field) || !data_offset_field) goto next_frame;
        
        uint32_t union_offset_val = 0;
        uint32_t data_field_pos = client_req_pos + data_offset_field;
        if (!read_unmasked_4(payload, len, payload_offset, data_field_pos, masking_key, mask, &union_offset_val)) goto next_frame;
        
        uint32_t order_req_pos = data_field_pos + union_offset_val;
        int32_t order_req_vtable_offset = 0;
        if (!read_unmasked_4(payload, len, payload_offset, order_req_pos, masking_key, mask, (uint32_t*)&order_req_vtable_offset)) goto next_frame;
        
        uint32_t order_req_vtable_pos = order_req_pos - order_req_vtable_offset;
        uint16_t exec_id_offset_field = 0;
        if (!read_unmasked_2(payload, len, payload_offset, order_req_vtable_pos + 6, masking_key, mask, &exec_id_offset_field) || !exec_id_offset_field) goto next_frame;
        
        uint64_t exec_id = 0;
        if (read_unmasked_8(payload, len, payload_offset, order_req_pos + exec_id_offset_field, masking_key, mask, &exec_id)) {
            *out_exec_id = exec_id;
            return true;
        }

    next_frame:
        curr_offset = payload_offset + (uint32_t)actual_payload_len;
        if (curr_offset >= 512) break;
    }
    return false;
}

static __always_inline bool parse_tx_exec_id(const uint8_t *payload, uint32_t len, uint64_t *out_exec_id, uint8_t *out_exec_type) {
    uint32_t curr_offset = 0;

    for (int frame_idx = 0; frame_idx < 4; frame_idx++) {
        if (curr_offset > 510 || curr_offset + 2 > len) break;

        uint8_t opcode = payload[curr_offset] & 0x0F;
        if (opcode != 0x2) break; // Binary frame only

        uint8_t len_field = payload[curr_offset + 1];
        bool mask = (len_field & 0x80) >> 7;
        uint32_t payload_len_field = len_field & 0x7F;

        uint32_t header_len = 2;
        uint64_t actual_payload_len = payload_len_field;

        if (payload_len_field == 126) {
            if (curr_offset > 508 || curr_offset + 4 > len) break;
            uint16_t val16 = *(const uint16_t*)(&payload[curr_offset + 2]);
            actual_payload_len = __builtin_bswap16(val16);
            header_len = 4;
        } else if (payload_len_field == 127) {
            if (curr_offset > 502 || curr_offset + 10 > len) break;
            uint64_t val64 = *(const uint64_t*)(&payload[curr_offset + 2]);
            actual_payload_len = __builtin_bswap64(val64);
            header_len = 10;
        }

        uint32_t payload_offset = curr_offset + header_len + (mask ? 4 : 0);
        if (payload_offset >= len || payload_offset >= 512) break;

        uint32_t masking_key = 0;
        if (mask) {
            uint32_t pos = curr_offset + header_len;
            if (pos > 508 || pos + 4 > len) break;
            masking_key = *(const uint32_t*)(&payload[pos]);
        }

        // Parse FlatBuffers starting at payload_offset
        uint32_t root_offset = 0;
        if (!read_unmasked_4(payload, len, payload_offset, 0, masking_key, mask, &root_offset)) goto next_frame;
        
        uint32_t client_resp_pos = root_offset;
        int32_t vtable_offset = 0;
        if (!read_unmasked_4(payload, len, payload_offset, client_resp_pos, masking_key, mask, (uint32_t*)&vtable_offset)) goto next_frame;
        
        uint32_t vtable_pos = client_resp_pos - vtable_offset;
        uint16_t data_type_offset = 0;
        if (!read_unmasked_2(payload, len, payload_offset, vtable_pos + 4, masking_key, mask, &data_type_offset)) goto next_frame;
        
        uint8_t data_type = 0;
        if (data_type_offset) {
            read_unmasked_1(payload, len, payload_offset, client_resp_pos + data_type_offset, masking_key, mask, &data_type);
        }
        if (data_type != 1) goto next_frame; // ClientResponseData_OrderResponse is 1
        
        uint16_t data_offset_field = 0;
        if (!read_unmasked_2(payload, len, payload_offset, vtable_pos + 6, masking_key, mask, &data_offset_field) || !data_offset_field) goto next_frame;
        
        uint32_t union_offset_val = 0;
        uint32_t data_field_pos = client_resp_pos + data_offset_field;
        if (!read_unmasked_4(payload, len, payload_offset, data_field_pos, masking_key, mask, &union_offset_val)) goto next_frame;
        
        uint32_t order_resp_pos = data_field_pos + union_offset_val;
        int32_t order_resp_vtable_offset = 0;
        if (!read_unmasked_4(payload, len, payload_offset, order_resp_pos, masking_key, mask, (uint32_t*)&order_resp_vtable_offset)) goto next_frame;
        
        uint32_t order_resp_vtable_pos = order_resp_pos - order_resp_vtable_offset;
        uint16_t exec_type_offset_field = 0;
        read_unmasked_2(payload, len, payload_offset, order_resp_vtable_pos + 4, masking_key, mask, &exec_type_offset_field);
        uint8_t exec_type = 0;
        if (exec_type_offset_field) {
            read_unmasked_1(payload, len, payload_offset, order_resp_pos + exec_type_offset_field, masking_key, mask, &exec_type);
        }
        
        uint16_t exec_id_offset_field = 0;
        if (!read_unmasked_2(payload, len, payload_offset, order_resp_vtable_pos + 10, masking_key, mask, &exec_id_offset_field) || !exec_id_offset_field) goto next_frame;
        
        uint64_t exec_id = 0;
        if (read_unmasked_8(payload, len, payload_offset, order_resp_pos + exec_id_offset_field, masking_key, mask, &exec_id)) {
            *out_exec_id = exec_id;
            *out_exec_type = exec_type;
            return true;
        }

    next_frame:
        curr_offset = payload_offset + (uint32_t)actual_payload_len;
        if (curr_offset >= 512) break;
    }
    return false;
}

#endif // EBPF_COMMON_H
