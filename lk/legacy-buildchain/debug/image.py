"""Checked minimal-LK image layout shared with resident firmware packaging."""
import hashlib
import struct

SIZE = 0x500000
LEGACY_SHA = 'fe2c7b3f9384453aa9fe68c3646efd1705a221fe17a0e0d4005dd2fa89051a42'
LK = 0xb5000000
HEADER_OFFSET, CODE_OFFSET, CODE_LIMIT = 0x280, 0x300, 0x1200
CONFIG = struct.Struct('<8s7I32s')
SCREEN_CONFIG = struct.Struct('<2I')
MAGIC = b'LKDBG001'
HOOK_OFFSET, OEM_OFFSET, DISPATCH_OFFSET = 0xe3d0, 0x11410, 0x6870
FASTBOOT_OFFSET, DOWNLOAD_OFFSET = 0x12c80, 0xe750
FLASH_OFFSET = 0x100d4
WATCHDOG, USB_RECOVERY, SDK_OEM, SCREEN, FLASH = 1, 2, 4, 8, 16
ORIGINALS = {HOOK_OFFSET: 0xa9bd7bfd, OEM_OFFSET: 0xd10e83ff,
             DISPATCH_OFFSET: 0xd63f0000, FASTBOOT_OFFSET: 0xa9af7bfd,
             DOWNLOAD_OFFSET: 0xa9b07bfd, FLASH_OFFSET: 0xa9b67bfd}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def branch(pc, target, *, link=False):
    delta = target - pc
    if delta % 4 or not -0x8000000 <= delta < 0x8000000:
        raise ValueError('ARM64 branch out of range')
    return (0x94000000 if link else 0x14000000) | ((delta >> 2) & 0x3ffffff)


def patches(flags, jump, oem, dispatch, fastboot=0, download=0, flash=0):
    result = {OEM_OFFSET: branch(LK + OEM_OFFSET - 512, oem)}
    if flags & (WATCHDOG | SCREEN):
        result[HOOK_OFFSET] = branch(LK + HOOK_OFFSET - 512, jump)
    if flags & USB_RECOVERY:
        result[DISPATCH_OFFSET] = branch(LK + DISPATCH_OFFSET - 512, dispatch, link=True)
    if flags & SCREEN:
        result[FASTBOOT_OFFSET] = branch(LK + FASTBOOT_OFFSET - 512, fastboot)
    if flags & (SCREEN | FLASH):
        result[DOWNLOAD_OFFSET] = branch(LK + DOWNLOAD_OFFSET - 512, download)
    if flags & FLASH:
        result[FLASH_OFFSET] = branch(LK + FLASH_OFFSET - 512, flash)
    return result


def validate(data):
    """Verify all unchanged bytes against the pinned, reproducible RAMLK."""
    if len(data) != SIZE:
        raise ValueError('LK image must be exactly 5 MiB')
    if sha(data) == LEGACY_SHA:
        return {'kind': 'legacy-ramlk', 'watchdog': False, 'usb_recovery': False,
                'sdk_oem': False, 'screen': False, 'fastboot_flash': False, 'watchdog_ms': None}
    magic, version, flags, timeout, count, jump, oem, dispatch, digest = CONFIG.unpack_from(data, HEADER_OFFSET)
    if (magic != MAGIC or version not in (1, 2, 3) or flags & ~31 or not flags & SDK_OEM or
            (version != 3 and bool(flags & SCREEN) != (version == 2)) or
            bool(flags & FLASH) != (version == 3) or
            not 1000 <= timeout <= 180000 or not 0 < count <= CODE_LIMIT - CODE_OFFSET):
        raise ValueError('unknown or corrupt minimal-LK configuration')
    low = LK + CODE_OFFSET - 512
    config_end = HEADER_OFFSET + CONFIG.size
    fastboot, download = SCREEN_CONFIG.unpack_from(data, config_end) if flags & (SCREEN | FLASH) else (0, 0)
    if flags & (SCREEN | FLASH):
        config_end += SCREEN_CONFIG.size
    flash = struct.unpack_from('<I', data, config_end)[0] if flags & FLASH else 0
    if flags & FLASH:
        config_end += 4
    if not flags & SCREEN and fastboot:
        raise ValueError('unexpected screen entry')
    entries = ((jump, oem, dispatch) + ((fastboot,) if flags & SCREEN else ()) +
               ((download,) if flags & (SCREEN | FLASH) else ()) +
               ((flash,) if flags & FLASH else ()))
    if any(not low <= address < low + count or address % 4 for address in entries):
        raise ValueError('LK extension entry outside the verified code')
    if hashlib.sha256(data[CODE_OFFSET:CODE_OFFSET+count]).digest() != digest:
        raise ValueError('LK extension checksum mismatch')
    expected = patches(flags, jump, oem, dispatch, fastboot, download, flash)
    for offset, original in ORIGINALS.items():
        if struct.unpack_from('<I', data, offset)[0] != expected.get(offset, original):
            raise ValueError('LK extension branch/configuration mismatch')
    normalized = bytearray(data)
    normalized[HEADER_OFFSET:CODE_LIMIT] = bytes(CODE_LIMIT - HEADER_OFFSET)
    for offset, original in ORIGINALS.items():
        struct.pack_into('<I', normalized, offset, original)
    if sha(normalized) != LEGACY_SHA:
        raise ValueError('LK bytes outside the extension differ from the pinned RAMLK')
    if any(data[config_end:CODE_OFFSET]) or any(data[CODE_OFFSET+count:CODE_LIMIT]):
        raise ValueError('unexpected LK boot-padding data')
    return {'kind': 'minimal-ramlk', 'watchdog': bool(flags & WATCHDOG),
            'usb_recovery': bool(flags & USB_RECOVERY), 'sdk_oem': True,
            'screen': bool(flags & SCREEN),
            'fastboot_flash': bool(flags & FLASH),
            'watchdog_ms': timeout if flags & WATCHDOG else None,
            'code_bytes': count, 'code_sha256': digest.hex()}


def compose(base, code, symbols, *, watchdog=True, usb_recovery=True, screen=False,
            fastboot_flash=False, watchdog_ms=30000):
    if sha(base) != LEGACY_SHA or len(base) != SIZE or any(base[HEADER_OFFSET:CODE_LIMIT]):
        raise ValueError('incorrect RAMLK or occupied boot padding')
    if not 0 < len(code) <= CODE_LIMIT - CODE_OFFSET:
        raise ValueError('LK extension exceeds boot padding')
    flags = (SDK_OEM | (WATCHDOG if watchdog else 0) | (USB_RECOVERY if usb_recovery else 0)
             | (SCREEN if screen else 0) | (FLASH if fastboot_flash else 0))
    jump, oem, dispatch = (symbols[n] for n in ('lk_debug_jump', 'lk_debug_oem', 'lk_debug_dispatch'))
    result = bytearray(base)
    result[HEADER_OFFSET:HEADER_OFFSET+CONFIG.size] = CONFIG.pack(
        MAGIC, 3 if fastboot_flash else (2 if screen else 1), flags, watchdog_ms, len(code), jump, oem, dispatch, hashlib.sha256(code).digest())
    fastboot = symbols['lk_debug_fastboot'] if screen else 0
    download = symbols['lk_debug_download'] if screen or fastboot_flash else 0
    if screen or fastboot_flash:
        SCREEN_CONFIG.pack_into(result, HEADER_OFFSET+CONFIG.size, fastboot, download)
    flash = symbols['lk_debug_flash'] if fastboot_flash else 0
    if fastboot_flash:
        struct.pack_into('<I', result, HEADER_OFFSET+CONFIG.size+SCREEN_CONFIG.size, flash)
    result[CODE_OFFSET:CODE_OFFSET+len(code)] = code
    for offset, word in patches(flags, jump, oem, dispatch, fastboot, download, flash).items():
        struct.pack_into('<I', result, offset, word)
    validate(result)
    return bytes(result)
