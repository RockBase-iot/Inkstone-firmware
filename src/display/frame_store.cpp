// Copyright (c) 2026 RockBase-IoT, Chengdu Rockbase Co., Ltd. All rights reserved.

#include "display/frame_store.h"
#include "boards/board.h"

#include <esp_partition.h>
#include <esp_rom_crc.h>

namespace framestore {

static const uint32_t MAGIC = 0x494E4B46;  // "INKF"

struct Header {
    uint32_t magic;
    uint32_t len;
    uint32_t crc32;
    uint32_t reserved;
};

static const esp_partition_t* part() {
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_ANY, "frame");
}

bool save(const uint8_t* frame) {
    const esp_partition_t* p = part();
    if (!p) { Serial.println("[frame] partition not found"); return false; }
    if (sizeof(Header) + EPD_FRAME_BYTES > p->size) {
        Serial.println("[frame] frame larger than partition");
        return false;
    }

    Header h{MAGIC, EPD_FRAME_BYTES,
             esp_rom_crc32_le(0, frame, EPD_FRAME_BYTES), 0};

    // Erase only the needed 4KB sectors (erasing the whole 2MB would be slow)
    size_t need = sizeof(Header) + EPD_FRAME_BYTES;
    size_t erase = (need + 4095) & ~4095UL;
    if (esp_partition_erase_range(p, 0, erase) != ESP_OK) {
        Serial.println("[frame] erase failed");
        return false;
    }
    if (esp_partition_write(p, 0, &h, sizeof(h)) != ESP_OK ||
        esp_partition_write(p, sizeof(h), frame, EPD_FRAME_BYTES) != ESP_OK) {
        Serial.println("[frame] write failed");
        return false;
    }
    Serial.printf("[frame] cached to flash partition (%u bytes)\n",
                  (unsigned)EPD_FRAME_BYTES);
    return true;
}

bool load(uint8_t* out) {
    const esp_partition_t* p = part();
    if (!p) return false;

    Header h{};
    if (esp_partition_read(p, 0, &h, sizeof(h)) != ESP_OK) return false;
    if (h.magic != MAGIC || h.len != EPD_FRAME_BYTES) return false;
    if (esp_partition_read(p, sizeof(h), out, EPD_FRAME_BYTES) != ESP_OK) {
        return false;
    }
    if (esp_rom_crc32_le(0, out, EPD_FRAME_BYTES) != h.crc32) {
        Serial.println("[frame] cache CRC mismatch, ignored");
        return false;
    }
    Serial.println("[frame] restored from flash cache");
    return true;
}

void clear() {
    const esp_partition_t* p = part();
    if (!p) return;
    // Erasing the first sector invalidates the header
    esp_partition_erase_range(p, 0, 4096);
}

} // namespace framestore
