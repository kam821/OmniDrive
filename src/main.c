#include "defs.h"
#include "mmio.h"

#define ReadBDDIMemDWORD(offset) \
    *(DWORD*)((DWORD)(memoryStart + BDDIOffset + (DWORD)(offset)))

#define WriteBDDIMemBYTE(offset, value) \
    *(BYTE*)((DWORD)(memoryStart + BDDIOffset + (DWORD)(offset))) = (BYTE)(value)

#define WriteDiscStructMemBYTE(offset, value) \
    *(BYTE*)((DWORD)(memoryStart + discStructOffset + (DWORD)(offset))) = (BYTE)(value)

#define ReturnSenseMediumNotPresent() \
    ReturnSense(0x02, 0x3A, 0x00); // MEDIUM NOT PRESENT

#define ReturnSenseCannotReadMediumIncompatibleFormat() \
    ReturnSense(0x05, 0x30, 0x02); // CANNOT READ MEDIUM - INCOMPATIBLE FORMAT

#define ReturnSenseInvalidFieldInCDB() \
    ReturnSense(0x05, 0x24, 0x00); // INVALID FIELD IN CDB

BYTE IsReadCommand() {
    return (cdb[0] == 0x28 || cdb[0] == 0xA8 || cdb[0] == 0xB9 || cdb[0] == 0xBE || cdb[0] == 0xC0);
}

DWORD SetBDCharacteristicsHook() {
    if (ReadBDDIMemDWORD(0x0000) == 0) {
        // most likely Wii U disc, add DI PSN values
        // max PSN
//      WriteBDDIMemBYTE(0x14, 0x00);
        WriteBDDIMemBYTE(0x15, 0xCA);
        WriteBDDIMemBYTE(0x16, 0x73);
        WriteBDDIMemBYTE(0x17, 0xFF);
        // start PSN
//      WriteBDDIMemBYTE(0x18, 0x00);
        WriteBDDIMemBYTE(0x19, 0x10);
//      WriteBDDIMemBYTE(0x1A, 0x00);
//      WriteBDDIMemBYTE(0x1B, 0x00);
        // end PSN
//      WriteBDDIMemBYTE(0x1C, 0x00);
        WriteBDDIMemBYTE(0x1D, 0xCA);
        WriteBDDIMemBYTE(0x1E, 0x74);
//      WriteBDDIMemBYTE(0x1F, 0x00);
    }

    return SetBDCharacteristics();
}

void SetErrorModeHook(DWORD discType, DWORD mask, DWORD mode) {
    if (cdb[0] == 0xC0) {
        if (discType == 2) {
            // DVD
            mode &= ~0x01;
        } else if (discType == 3) {
            // BD
            mode &= ~0x06;
        }
    }
    SetErrorMode(discType, mask, mode);
}

void ChangeDiscRWModeHook(DWORD mode) {
    ChangeDiscRWMode(mode);

    if (cdb[0] == 0xC0 && !(cdb[1] & 0x10)) {
        // disable descrambling
        SCRAMBLE_REGISTER_UNK1 &= ~0x08;
        SCRAMBLE_REGISTER_UNK2 &= ~0x04;
    } else {
        SCRAMBLE_REGISTER_UNK1 |= 0x08;
        SCRAMBLE_REGISTER_UNK2 |= 0x04;
    }
}

static void EnableXGD() {
    // check DMI to see if first byte is 1 or 2
    BYTE dmiIndicator = ReadDiscStructMemDWORD(0x5800) >> 24;
    if (dmiIndicator != 1 && dmiIndicator != 2) {
        return;
    }

    seekLayer = 1;
    startAddress = 0xFD0210;

    DWORD retryCount = 0;
    while (!ReadDiscData(8, 0x10)) {
        retryCount++;
        startAddress += 0x40;
        if (retryCount > 4) {
            return;
        }
    }

    CopySectorToDiscStructMem(0x6800, 0xE);
    BYTE discIdentifier = ReadDiscStructMemDWORD(0x6800) >> 24;

    // check disc identifier to make sure sector is the security sector
    if (discIdentifier != 0xD1 && discIdentifier != 0xE1) {
        return;
    }

    layer0End = ReadDiscStructMemDWORD(0x680C);
    lastSector = ReadDiscStructMemDWORD(0x5008) | 0xFF000000;
    lastSector += 2 * (layer0End + 1);

    DWORD discSize = (lastSector + 1) - 0x30000;

    WriteDiscStructMemBYTE((TocOffset + 0xF),  (discSize & 0xFF0000) >> 16);
    WriteDiscStructMemBYTE((TocOffset + 0x10), (discSize & 0xFF00) >> 8);
    WriteDiscStructMemBYTE((TocOffset + 0x11), (discSize & 0xFF));
}

DWORD DVDMinusReadCheckHook() {
    /* ARMIPS BUG WORKAROUND */
    asm("nop");

    DWORD ret = DVDMinusReadCheck();

    // force read if using READ DISC RAW
    if (!ret && cdb[0] == 0xC0) {
        ret = 1;
    }

    return ret;
}

DWORD ReadDVDTOCHook(DWORD unk) {
    // patch compressed code in memory at runtime
    *DVDCharacteristicsPatchPtr = 0x19;
    *DVDTOCReadPatchPtr = 0x01;

    DWORD ret = ReadDVDTOC(unk);

    if (DiscIsMultiLayer() && !isDiscPTP) {
        EnableXGD();
    }

    // If Nintendo Disc, enable BCA
    if ((ReadDiscStructMemDWORD(0x5000) >> 24) == 0xFF) {
        WriteDiscStructMemBYTE(0x5010, 0x80);
    }

    return ret;
}

void BDReadCmdHook() {
    if (ReadBDDIMemDWORD(0x0000) == 0) {
        ReturnSenseCannotReadMediumIncompatibleFormat();
        return;
    }
    BDReadCmd();
}

void DVDReadCmdHook() {
    if ((ReadDiscStructMemDWORD(0x5000) >> 24) == 0xFF) {
        ReturnSenseCannotReadMediumIncompatibleFormat();
        return;
    }
    DVDReadCmd();
}

static void ReadBDRaw() {
    // CD specific
    if (cdb[10]) {
        ReturnSenseInvalidFieldInCDB();
        return;
    }

    startAddress = (cdb[2] << 24) | (cdb[3] << 16) | (cdb[4] << 8) | (cdb[5]);
    transferLength = (cdb[6] << 24) | (cdb[7] << 16) | (cdb[8] << 8) | (cdb[9]);

    if (!(cdb[1] & 0x04)) {
        // if not using raw addressing
        startAddress += 0x100000;
    } else {
        // drive expects LBA+0x100000 rather than PSN, so correct for this
        if (DiscIsTripleLayer()) {
            if (startAddress >= (0xBFFFFFF - layer2End)) {
                startAddress = startAddress + (2 * layer2End) - 0xBFFFFFE;
            } else if (startAddress >= (0x7FFFFFF - layer1End)) {
                startAddress = startAddress + (2 * layer1End) - 0x7FFFFFE;
            } else if (startAddress >= (0x3FFFFFF - layer0End)) {
                startAddress = startAddress + (2 * layer0End) - 0x3FFFFFE;
            }
        } else if (DiscIsMultiLayer()) {
            if (startAddress >= (0x1FFFFFF - layer0End)) {
                startAddress = startAddress + (2 * layer0End) - 0x1FFFFFE;
            }
        }
    }

    DISC_SECTOR_REGISTER = 0xF8; // return full sector

    ReadBDData(cdb[1] & 0x08);
}

static void ReadDVDRaw() {
    // CD specific
    if (cdb[10]) {
        ReturnSenseInvalidFieldInCDB();
        return;
    }

    startAddress = (cdb[2] << 24) | (cdb[3] << 16) | (cdb[4] << 8) | (cdb[5]);
    transferLength = (cdb[6] << 24) | (cdb[7] << 16) | (cdb[8] << 8) | (cdb[9]);

    if (!(cdb[1] & 0x04)) {
        // if not using raw addressing
        if (DiscIsDVDRAM()) {
            startAddress += 0x31000;
        } else {
            startAddress += 0x30000;
        }
    } else {
        // if using raw addressing
        if (!isDiscPTP && DiscIsMultiLayer() && (startAddress & 0x800000)) {
            startAddress = startAddress - ~layer0End + layer0End + 1;
        }
    }

    DVDCheckLayer(2);

    DISC_SECTOR_REGISTER = 0x98; // return full sector

    if (DiscIsDVDRAM()) {
        ReadDVDRAMData(cdb[1] & 0x08);
    } else {
        ReadDVDData(cdb[1] & 0x08);
    }
}

static void ReadCDRaw() {
    // CD does not support descrambling
    if (cdb[1] & 0x10) {
        ReturnSenseInvalidFieldInCDB();
        return;
    }

    startAddress = (cdb[2] << 24) | (cdb[3] << 16) | (cdb[4] << 8) | (cdb[5]);
    transferLength = (cdb[6] << 24) | (cdb[7] << 16) | (cdb[8] << 8) | (cdb[9]);

    if (!(cdb[1] & 0x04)) {
        // if not using raw addressing
        startAddress += 150;
    } else {
        startAddress = MSFtoLBA(startAddress);
    }

    if (cdb[1] & 0x08) {
        forceUnitAccess = 0xFF;
    }

    DISC_SECTOR_REGISTER = 0x10; // user data, required for reading as raw

    BYTE subChannels = cdb[10] & 0b11;
    if (subChannels == 0x02) {
        SetCDType(0, 1); // Audio with SubQ only
    } else {
        SetCDType(0, 0); // Audio
    }

    if (subChannels) {
        DISC_SECTOR_REGISTER |= 0x01; // enable subs
    }

    if (cdb[10] & 0x04) {
        DISC_SECTOR_REGISTER |= 0x02; // enable C2
    }

    ReadCDDA();
}

void CmdOmniDriveReadDiscRaw() {
    if (!mediaType) {
        ReturnSenseMediumNotPresent();
    }

    readTimeCounter = 0;

    BYTE DiscType = cdb[1] & 0x03;
    switch (DiscType) {
    case 0: {
        if (DiscIsCD()) {
            ReadCDRaw();
        } else {
            ReturnSenseCannotReadMediumIncompatibleFormat();
        }
        break;
    }
    case 1: {
        if (DiscIsDVD()) {
            ReadDVDRaw();
        } else {
            ReturnSenseCannotReadMediumIncompatibleFormat();
        }
        break;
    }
    case 2: {
        if (DiscIsBD()) {
            ReadBDRaw();
        } else {
            ReturnSenseCannotReadMediumIncompatibleFormat();
        }
        break;
    }
    default: {
        ReturnSenseInvalidFieldInCDB();
        break;
    }
    }
}
