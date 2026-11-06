const assert = require('node:assert/strict');
const {BOOT_SECTORS} = require('../../tools/boot-layout');

const blockSize = 2048;

function bootPayload(iso) {
    const pvd = iso.subarray(16 * blockSize, 17 * blockSize);
    const record = iso.subarray(17 * blockSize, 18 * blockSize);
    assert.equal(pvd.subarray(1, 6).toString(), 'CD001', 'ISO9660 volume descriptor');
    assert.equal(pvd.readUInt16LE(128), blockSize, 'ISO9660 logical block size');
    assert.equal(record.subarray(7, 30).toString(), 'EL TORITO SPECIFICATION', 'El Torito boot record');
    const catalogStart = record.readUInt32LE(71) * blockSize;
    const catalog = iso.subarray(catalogStart, catalogStart + blockSize);
    assert.equal(catalog.length, blockSize, 'El Torito catalog is outside the ISO');
    let sum = 0;
    for (let offset = 0; offset < 32; offset += 2) sum += catalog.readUInt16LE(offset);
    assert.equal(sum & 0xffff, 0, 'El Torito catalog checksum');
    assert.equal(catalog[32], 0x88, 'El Torito bootable entry');
    assert.equal(catalog[33], 3, 'El Torito 2.88 MB floppy emulation');

    const bootStart = catalog.readUInt32LE(40) * blockSize;
    const bootBytes = BOOT_SECTORS * 512;
    const payload = iso.subarray(bootStart, bootStart + bootBytes);
    assert.equal(payload.length, bootBytes, 'Boot image is outside the ISO');
    return payload;
}

module.exports = {bootPayload};
