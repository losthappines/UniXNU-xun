// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Fts8203.h"
#include <string.h>

static uint16_t wire_crc(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0x8408 : 0);
        }
    }
    return crc;
}

static int exchange(struct Fts8203 *chip, size_t length, int read, size_t payload)
{
    int result = -5;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        memset(chip->rx, 0, length);
        result = chip->io.transfer(chip->io.context, chip->tx, chip->rx, length);
        chip->io.delay_us(chip->io.context, 150);
        chip->last_status = chip->rx[3];
        if (result) {
            continue;
        }
        if (chip->last_status & 0xa0) {
            result = -5;
            continue;
        }
        if (read) {
            uint16_t received = chip->rx[7 + payload] | (chip->rx[8 + payload] << 8);
            if (wire_crc(chip->rx + 7, payload) != received) {
                result = -74;
                continue;
            }
        }
        return 0;
    }
    return result;
}

static int write_command(struct Fts8203 *chip, uint8_t command, const uint8_t *data, size_t length)
{
    if (length > FTS8203_CHUNK || (length && !data)) {
        return -22;
    }
    size_t total = length ? 7 + length : 4;
    memset(chip->tx, 0, total);
    chip->tx[0] = command;
    chip->tx[2] = length >> 8;
    chip->tx[3] = length;
    if (length) {
        memcpy(chip->tx + 7, data, length);
    }
    return exchange(chip, total, 0, length);
}

int fts8203_sleep(struct Fts8203 *chip)
{
    const uint8_t mode = 3;
    return write_command(chip, 0xa5, &mode, 1);
}

int fts8203_read(struct Fts8203 *chip, uint8_t command, uint8_t *data, size_t length)
{
    if (!data || !length || length > FTS8203_CHUNK) {
        return -22;
    }
    memset(chip->tx, 0, length + 9);
    chip->tx[0] = command;
    chip->tx[1] = 0xa0;
    chip->tx[2] = length >> 8;
    chip->tx[3] = length;
    int result = exchange(chip, length + 9, 1, length);
    if (!result) {
        memcpy(data, chip->rx + 7, length);
    }
    return result;
}

static void stage(struct Fts8203 *chip, const char *name, unsigned value, int result)
{
    if (chip->io.stage) {
        chip->io.stage(chip->io.context, name, value, result);
    }
}

static int enter_rom(struct Fts8203 *chip)
{
    int result = -19;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        chip->io.reset(chip->io.context, 0);
        chip->io.delay_us(chip->io.context, 1000);
        chip->io.reset(chip->io.context, 1);
        chip->io.delay_us(chip->io.context, 8000);
        for (unsigned query = 0; query < 3; ++query) {
            result = write_command(chip, 0x55, NULL, 0);
            if (result) {
                continue;
            }
            chip->io.delay_us(chip->io.context, 8000);
            result = fts8203_read(chip, 0x90, chip->rom, 2);
            stage(chip, "rom-id", ((unsigned)chip->rom[0] << 8) | chip->rom[1], result);
            if (!result && chip->rom[0] == 0x82 && chip->rom[1] == 0x01) {
                return 0;
            }
            result = -19;
        }
    }
    return result;
}

static uint16_t firmware_ecc(const uint8_t *data, size_t length)
{
    uint16_t ecc = 0;
    for (size_t i = 0; i < length; i += 2) {
        ecc ^= ((uint16_t)data[i] << 8) | data[i + 1];
        for (unsigned bit = 0; bit < 16; ++bit) {
            ecc = (ecc >> 1) ^ ((ecc & 1) ? 0x8408 : 0);
        }
    }
    return ecc;
}

static int supported_app(unsigned app_id)
{
    switch (app_id) {
    case 0x8203:
    case 0x820a:
    case 0x820b:
        return 1;
    default:
        return 0;
    }
}

int fts8203_state(struct Fts8203 *chip, int probe_boot)
{
    uint8_t high = 0, low = 0, mode = 0, power = 0, gesture = 0;
    uint8_t interrupts = 0, flow = 0;
    int app_result = fts8203_read(chip, 0xa3, &high, 1);
    if (!app_result) {
        app_result = fts8203_read(chip, 0x9f, &low, 1);
    }
    unsigned app = ((unsigned)high << 8) | low;
    stage(chip, "state-app", app, app_result);
    int mode_result = fts8203_read(chip, 0x00, &mode, 1);
    int power_result = fts8203_read(chip, 0xa5, &power, 1);
    int gesture_result = fts8203_read(chip, 0xd0, &gesture, 1);
    stage(chip, "state-mode-power-gesture", ((unsigned)mode << 16) | ((unsigned)power << 8) | gesture,
        mode_result ? mode_result : (power_result ? power_result : gesture_result));
    int irq_result = fts8203_read(chip, 0x8f, &interrupts, 1);
    int flow_result = fts8203_read(chip, 0x91, &flow, 1);
    stage(chip, "state-irq-flow", ((unsigned)interrupts << 8) | flow,
        irq_result ? irq_result : flow_result);

    if (probe_boot) {
        uint8_t rom[2] = {0};
        int rom_result = fts8203_read(chip, 0x90, rom, sizeof(rom));
        stage(chip, "state-rom", ((unsigned)rom[0] << 8) | rom[1], rom_result);
        if (!rom_result && !gesture_result && rom[0] == 0x82 && rom[1] == 0x01 && gesture == 1) {
            return 1;
        }
    }
    if (!app_result && supported_app(app)) {
        return 0;
    }
    return app_result ? app_result : -19;
}

int fts8203_start(struct Fts8203 *chip, const uint8_t *firmware, size_t size)
{
    if (!chip || !chip->io.transfer || !chip->io.reset || !chip->io.delay_us ||
        !firmware || size < 0x104) {
        return -22;
    }
    unsigned words = ((unsigned)firmware[0x100] << 8) | firmware[0x101];
    unsigned inverse = ((unsigned)firmware[0x102] << 8) | firmware[0x103];
    size_t length = (size_t)words * 2;
    if (words + inverse != 0xffff || length < 0x104 || length > 96 * 1024 || length > size) {
        stage(chip, "firmware-header", (unsigned)length, -22);
        return -22;
    }

    int result = enter_rom(chip);
    if (result) {
        return result;
    }
    for (size_t offset = 0; offset < length; offset += FTS8203_CHUNK) {
        size_t count = length - offset;
        if (count > FTS8203_CHUNK) {
            count = FTS8203_CHUNK;
        }
        uint8_t address[3] = {offset >> 16, offset >> 8, offset};
        result = write_command(chip, 0xad, address, sizeof(address));
        if (!result) {
            result = write_command(chip, 0xae, firmware + offset, count);
        }
        if (result) {
            stage(chip, "pram-write", (unsigned)offset, result);
            return result;
        }
    }
    stage(chip, "pram-written", (unsigned)length, 0);

    uint8_t request[6] = {0, 0, 0, length >> 16, length >> 8, length};
    result = write_command(chip, 0xcc, request, sizeof(request));
    if (result) {
        return result;
    }
    chip->io.delay_us(chip->io.context, 2000);
    uint8_t status = 0;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        result = fts8203_read(chip, 0xce, &status, 1);
        if (result) {
            return result;
        }
        if (status == 0xa5) {
            break;
        }
        chip->io.delay_us(chip->io.context, 1000);
    }
    if (status != 0xa5) {
        stage(chip, "ecc-timeout", status, -110);
        return -110;
    }
    uint8_t bytes[2];
    result = fts8203_read(chip, 0xcd, bytes, 2);
    if (result) {
        return result;
    }
    uint16_t expected = firmware_ecc(firmware, length);
    uint16_t device = ((uint16_t)bytes[0] << 8) | bytes[1];
    stage(chip, "firmware-ecc", ((unsigned)expected << 16) | device, expected == device ? 0 : -74);
    if (expected != device) {
        return -74;
    }
    result = write_command(chip, 0x08, NULL, 0);
    if (result) {
        return result;
    }
    chip->io.delay_us(chip->io.context, 210000);
    result = fts8203_read(chip, 0xa6, &chip->version, 1);
    if (result) {
        return result;
    }
    chip->io.delay_us(chip->io.context, 50000);
    result = fts8203_read(chip, 0xa3, chip->app, 1);
    if (!result) {
        result = fts8203_read(chip, 0x9f, chip->app + 1, 1);
    }
    unsigned app_id = ((unsigned)chip->app[0] << 8) | chip->app[1];
    if (!result) {
        // FocalTech type 0x21 declares 8203 and the two explicit CID aliases 820A/820B.
        if (!supported_app(app_id)) {
            result = -19;
        }
    }
    stage(chip, "app-id", app_id, result);
    stage(chip, "app-version", chip->version, result);
    return result;
}

int fts8203_frame(struct Fts8203 *chip, struct Fts8203Frame *out)
{
    // The stock input transaction includes 63 touch bytes and 28 gesture bytes.
    uint8_t data[91] = {1};
    int result = fts8203_read(chip, 1, data + 1, sizeof(data) - 1);
    chip->frame_head[0] = 1;
    memcpy(chip->frame_head + 1, chip->rx + 7, sizeof(chip->frame_head) - 1);
    if (result) {
        return result;
    }
    if (data[2] == 0xef && data[3] == 0xef && data[4] == 0xef) {
        return FTS8203_FRAME_BOOT;
    }
    if (data[2] == 0xff && data[3] == 0xff && data[4] == 0xff &&
        data[5] == 0xff && data[6] == 0xff) {
        return FTS8203_FRAME_EMPTY;
    }
    unsigned points = data[2] & 15;
    if (points > FTS8203_CONTACTS || (data[2] & 0xf0) == 0xb0) {
        return -71;
    }
    struct Fts8203Frame frame = {0};
    uint16_t seen = 0;
    for (unsigned i = 0; i < FTS8203_CONTACTS; ++i) {
        unsigned at = 3 + i * 6;
        unsigned id = data[at + 2] >> 4;
        if (id >= FTS8203_CONTACTS) {
            break;
        }
        if (seen & (1u << id)) {
            return -71;
        }
        seen |= 1u << id;
        unsigned event = data[at] >> 6;
        if (event == 1 || (data[at] & 0x10)) {
            frame.up |= 1u << id;
            continue;
        }
        if (event != 0 && event != 2) {
            return -71;
        }
        unsigned x = ((data[at] & 15) << 8) | data[at + 1];
        unsigned y = ((data[at + 2] & 15) << 8) | data[at + 3];
        if (!points || x >= 1600 || y >= 2560) {
            return -71;
        }
        frame.active |= 1u << id;
        if (event == 0) {
            frame.down |= 1u << id;
        }
        frame.x[id] = x;
        frame.y[id] = y;
    }
    *out = frame;
    return 0;
}
