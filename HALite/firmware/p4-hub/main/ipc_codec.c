#include "ipc_codec.h"

#include <string.h>

#include "halite_crc.h"

size_t ipc_encode_frame(uint8_t *out, size_t out_len, uint8_t type, uint8_t flags, uint16_t seq,
                        const void *payload, uint16_t payload_len)
{
    if (!out || payload_len > HALITE_IPC_MAX_PAYLOAD) {
        return 0;
    }
    const size_t total = (size_t)HALITE_IPC_HDR_LEN + payload_len + HALITE_IPC_CRC_LEN;
    if (out_len < total) {
        return 0;
    }

    uint8_t hdr_wo[HALITE_IPC_HDR_LEN];
    hdr_wo[0] = (uint8_t)(HALITE_IPC_MAGIC & 0xFF);
    hdr_wo[1] = (uint8_t)(HALITE_IPC_MAGIC >> 8);
    hdr_wo[2] = HALITE_IPC_VERSION;
    hdr_wo[3] = type;
    hdr_wo[4] = flags;
    hdr_wo[5] = 0;
    hdr_wo[6] = (uint8_t)(seq & 0xFF);
    hdr_wo[7] = (uint8_t)(seq >> 8);
    hdr_wo[8] = (uint8_t)(payload_len & 0xFF);
    hdr_wo[9] = (uint8_t)(payload_len >> 8);

    uint8_t hcrc = halite_crc8(hdr_wo, 5);
    memcpy(out, hdr_wo, HALITE_IPC_HDR_LEN);
    out[5] = hcrc;
    if (payload_len && payload) {
        memcpy(out + HALITE_IPC_HDR_LEN, payload, payload_len);
    }

    uint8_t crc_in[HALITE_IPC_HDR_LEN + HALITE_IPC_MAX_PAYLOAD];
    memcpy(crc_in, hdr_wo, HALITE_IPC_HDR_LEN);
    if (payload_len && payload) {
        memcpy(crc_in + HALITE_IPC_HDR_LEN, payload, payload_len);
    }
    uint16_t c16 = halite_crc16_ccitt(crc_in, (size_t)HALITE_IPC_HDR_LEN + payload_len);
    out[HALITE_IPC_HDR_LEN + payload_len] = (uint8_t)(c16 & 0xFF);
    out[HALITE_IPC_HDR_LEN + payload_len + 1] = (uint8_t)(c16 >> 8);
    return total;
}

esp_err_t ipc_decode_frame(const uint8_t *buf, size_t buf_len, ipc_frame_view_t *view)
{
    if (!buf || !view || buf_len < HALITE_IPC_HDR_LEN + HALITE_IPC_CRC_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint16_t magic = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    if (magic != HALITE_IPC_MAGIC) {
        return ESP_ERR_INVALID_ARG;
    }
    if (buf[2] != HALITE_IPC_VERSION) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint8_t type = buf[3];
    uint8_t flags = buf[4];
    uint8_t hcrc = buf[5];
    uint16_t seq = (uint16_t)buf[6] | ((uint16_t)buf[7] << 8);
    uint16_t len = (uint16_t)buf[8] | ((uint16_t)buf[9] << 8);
    if (len > HALITE_IPC_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (buf_len < (size_t)HALITE_IPC_HDR_LEN + len + HALITE_IPC_CRC_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t hdr_wo[HALITE_IPC_HDR_LEN];
    memcpy(hdr_wo, buf, HALITE_IPC_HDR_LEN);
    hdr_wo[5] = 0;
    if (halite_crc8(hdr_wo, 5) != hcrc) {
        return ESP_ERR_INVALID_CRC;
    }

    uint8_t crc_in[HALITE_IPC_HDR_LEN + HALITE_IPC_MAX_PAYLOAD];
    memcpy(crc_in, hdr_wo, HALITE_IPC_HDR_LEN);
    if (len) {
        memcpy(crc_in + HALITE_IPC_HDR_LEN, buf + HALITE_IPC_HDR_LEN, len);
    }
    uint16_t expect = halite_crc16_ccitt(crc_in, (size_t)HALITE_IPC_HDR_LEN + len);
    uint16_t got = (uint16_t)buf[HALITE_IPC_HDR_LEN + len] | ((uint16_t)buf[HALITE_IPC_HDR_LEN + len + 1] << 8);
    if (expect != got) {
        return ESP_ERR_INVALID_CRC;
    }

    view->type = type;
    view->flags = flags;
    view->seq = seq;
    view->len = len;
    view->payload = len ? buf + HALITE_IPC_HDR_LEN : NULL;
    return ESP_OK;
}

size_t ipc_try_parse(const uint8_t *buf, size_t buf_len, ipc_frame_view_t *view, size_t *skip)
{
    if (skip) {
        *skip = 0;
    }
    if (!buf || buf_len < 2) {
        return 0;
    }
    for (size_t i = 0; i + 1 < buf_len; i++) {
        uint16_t magic = (uint16_t)buf[i] | ((uint16_t)buf[i + 1] << 8);
        if (magic != HALITE_IPC_MAGIC) {
            continue;
        }
        if (i > 0) {
            if (skip) {
                *skip = i;
            }
            return 0;
        }
        if (buf_len < HALITE_IPC_HDR_LEN) {
            return 0;
        }
        uint16_t len = (uint16_t)buf[8] | ((uint16_t)buf[9] << 8);
        size_t need = (size_t)HALITE_IPC_HDR_LEN + len + HALITE_IPC_CRC_LEN;
        if (len > HALITE_IPC_MAX_PAYLOAD) {
            if (skip) {
                *skip = 2;
            }
            return 0;
        }
        if (buf_len < need) {
            return 0;
        }
        if (ipc_decode_frame(buf, need, view) == ESP_OK) {
            return need;
        }
        if (skip) {
            *skip = 2;
        }
        return 0;
    }
    if (skip) {
        *skip = buf_len > 1 ? buf_len - 1 : 0;
    }
    return 0;
}
