import Foundation

/// Pure Swift MacBinary II format encoder.
/// Packages modern files into standard MacBinary II format (128-byte header + data fork + resource fork)
/// so that vintage 68k Mac OS clients receive native files with correct Finder icons, Type/Creator codes,
/// and resource forks intact.
public final class MacBinaryEncoder {

    public struct EncodedFile {
        public let header: Data
        public let dataFork: Data
        public let dataForkPaddedLength: Int
        public let resourceFork: Data
        public let resourceForkPaddedLength: Int
        public let totalStreamSize: Int64

        public var fullMacBinaryData: Data {
            var full = header
            full.append(dataFork)
            if dataForkPaddedLength > dataFork.count {
                full.append(Data(repeating: 0, count: dataForkPaddedLength - dataFork.count))
            }
            full.append(resourceFork)
            if resourceForkPaddedLength > resourceFork.count {
                full.append(Data(repeating: 0, count: resourceForkPaddedLength - resourceFork.count))
            }
            return full
        }
    }

    /// Prepares a file at URL for MacBinary streaming.
    /// If the file is already a valid MacBinary II/III container, it passes through directly.
    public static func encode(fileURL: URL) throws -> EncodedFile {
        let path = fileURL.path
        let filename = fileURL.lastPathComponent

        // Read Data Fork
        let dataFork = try Data(contentsOf: fileURL, options: .mappedIfSafe)

        // Check if file is ALREADY a valid MacBinary container (.bin / .macbinary)
        if isMacBinary(data: dataFork) {
            return EncodedFile(
                header: Data(),
                dataFork: dataFork,
                dataForkPaddedLength: dataFork.count,
                resourceFork: Data(),
                resourceForkPaddedLength: 0,
                totalStreamSize: Int64(dataFork.count)
            )
        }

        let dataLen = UInt32(dataFork.count)

        // Check for classic Mac Resource Fork via macOS named fork or AppleDouble
        var resourceFork = Data()
        let rsrcPath = "\(path)/..namedfork/rsrc"
        if let rsrcData = try? Data(contentsOf: URL(fileURLWithPath: rsrcPath)), !rsrcData.isEmpty {
            resourceFork = rsrcData
        } else if let xattrRsrc = readExtendedAttribute(name: "com.apple.ResourceFork", at: path), !xattrRsrc.isEmpty {
            resourceFork = xattrRsrc
        }
        let rsrcLen = UInt32(resourceFork.count)

        // Read Type and Creator
        let (fileType, fileCreator) = determineTypeAndCreator(fileURL: fileURL)

        // Read Creation & Modification dates
        let attrs = try? FileManager.default.attributesOfItem(atPath: path)
        let creationDate = attrs?[.creationDate] as? Date ?? Date()
        let modDate = attrs?[.modificationDate] as? Date ?? Date()

        // Classic Mac epoch is Jan 1, 1904 (2,082,844,800 seconds before Unix epoch Jan 1, 1970)
        let macEpochOffset: TimeInterval = 2_082_844_800
        let macCreationSec = UInt32(max(0, creationDate.timeIntervalSince1970 + macEpochOffset))
        let macModSec = UInt32(max(0, modDate.timeIntervalSince1970 + macEpochOffset))

        // Build 128-byte MacBinary II header
        var header = Data(count: 128)

        // Byte 0: Old version number (0)
        header[0] = 0

        // Byte 1: Filename length (max 31 chars in Classic Mac OS HFS)
        let cleanName = filename.prefix(31)
        let nameBytes = cleanName.data(using: .macOSRoman) ?? cleanName.data(using: .ascii) ?? Data("File".utf8)
        let nameLen = min(31, UInt8(nameBytes.count))
        header[1] = nameLen

        // Bytes 2..64: Filename in Mac OS Roman
        for i in 0..<Int(nameLen) {
            header[2 + i] = nameBytes[i]
        }

        // Bytes 65..68: File Type (4 bytes)
        let typeBytes = fileType.utf8.prefix(4)
        for (i, b) in typeBytes.enumerated() {
            header[65 + i] = b
        }

        // Bytes 69..72: File Creator (4 bytes)
        let creatorBytes = fileCreator.utf8.prefix(4)
        for (i, b) in creatorBytes.enumerated() {
            header[69 + i] = b
        }

        // Byte 73: Finder flags high byte
        header[73] = 0

        // Bytes 83..86: Data Fork Length (Big Endian)
        header[83] = UInt8((dataLen >> 24) & 0xFF)
        header[84] = UInt8((dataLen >> 16) & 0xFF)
        header[85] = UInt8((dataLen >> 8) & 0xFF)
        header[86] = UInt8(dataLen & 0xFF)

        // Bytes 87..90: Resource Fork Length (Big Endian)
        header[87] = UInt8((rsrcLen >> 24) & 0xFF)
        header[88] = UInt8((rsrcLen >> 16) & 0xFF)
        header[89] = UInt8((rsrcLen >> 8) & 0xFF)
        header[90] = UInt8(rsrcLen & 0xFF)

        // Bytes 91..94: Creation Date
        header[91] = UInt8((macCreationSec >> 24) & 0xFF)
        header[92] = UInt8((macCreationSec >> 16) & 0xFF)
        header[93] = UInt8((macCreationSec >> 8) & 0xFF)
        header[94] = UInt8(macCreationSec & 0xFF)

        // Bytes 95..98: Modification Date
        header[95] = UInt8((macModSec >> 24) & 0xFF)
        header[96] = UInt8((macModSec >> 16) & 0xFF)
        header[97] = UInt8((macModSec >> 8) & 0xFF)
        header[98] = UInt8(macModSec & 0xFF)

        // Bytes 102..105: MacBinary II Signature 'mBIN'
        header[102] = 0x6D // 'm'
        header[103] = 0x42 // 'B'
        header[104] = 0x49 // 'I'
        header[105] = 0x4E // 'N'

        // Bytes 122..123: MacBinary version (129 = 0x81)
        header[122] = 0x81
        header[123] = 0x81

        // Calculate CRC-16 for bytes 0..123 and store in bytes 124..125
        let crc = calculateCRC16(data: header.prefix(124))
        header[124] = UInt8((crc >> 8) & 0xFF)
        header[125] = UInt8(crc & 0xFF)

        // Calculate padded lengths (MacBinary pads forks to 128-byte multiples)
        let dataPadded = padTo128(Int(dataLen))
        let rsrcPadded = padTo128(Int(rsrcLen))
        let totalSize = Int64(128 + dataPadded + rsrcPadded)

        return EncodedFile(
            header: header,
            dataFork: dataFork,
            dataForkPaddedLength: dataPadded,
            resourceFork: resourceFork,
            resourceForkPaddedLength: rsrcPadded,
            totalStreamSize: totalSize
        )
    }

    /// Verifies if a buffer is already a valid MacBinary II or III container
    public static func isMacBinary(data: Data) -> Bool {
        guard data.count >= 128 else { return false }
        if data[0] != 0 { return false }
        let nameLen = data[1]
        if nameLen < 1 || nameLen > 63 { return false }
        if data[74] != 0 || data[82] != 0 { return false }

        let expectedCrc124 = (UInt16(data[124]) << 8) | UInt16(data[125])
        let actualCrc124 = calculateCRC16(data: data.prefix(124))
        if expectedCrc124 == actualCrc124 { return true }

        let expectedCrc126 = (UInt16(data[126]) << 8) | UInt16(data[127])
        let actualCrc126 = calculateCRC16(data: data.prefix(126))
        return expectedCrc126 == actualCrc126
    }

    private static func padTo128(_ length: Int) -> Int {
        if length == 0 { return 0 }
        let remainder = length % 128
        return remainder == 0 ? length : length + (128 - remainder)
    }

    /// Standard MacBinary CCITT CRC-16 checksum
    public static func calculateCRC16(data: Data) -> UInt16 {
        var crc: UInt16 = 0
        for byte in data {
            var x = UInt16(byte) << 8
            for _ in 0..<8 {
                if ((crc ^ x) & 0x8000) != 0 {
                    crc = (crc << 1) ^ 0x1021
                } else {
                    crc = crc << 1
                }
                x = x << 1
            }
        }
        return crc
    }

    /// Reads Mac OS extended attributes (FinderInfo, ResourceFork) preserved by macOS
    private static func readExtendedAttribute(name: String, at path: String) -> Data? {
        let size = getxattr(path, name, nil, 0, 0, 0)
        guard size > 0 else { return nil }
        var data = Data(count: size)
        let readSize = data.withUnsafeMutableBytes { ptr in
            getxattr(path, name, ptr.baseAddress, size, 0, 0)
        }
        return readSize > 0 ? data : nil
    }

    /// Resolves Type & Creator codes from FinderInfo or file extension
    private static func determineTypeAndCreator(fileURL: URL) -> (type: String, creator: String) {
        let path = fileURL.path
        if let finderInfo = readExtendedAttribute(name: "com.apple.FinderInfo", at: path), finderInfo.count >= 8 {
            let type = String(data: finderInfo.prefix(4), encoding: .macOSRoman) ?? "????"
            let creator = String(data: finderInfo.subdata(in: 4..<8), encoding: .macOSRoman) ?? "????"
            if type.trimmingCharacters(in: .whitespacesAndNewlines).count == 4 {
                return (type, creator)
            }
        }

        let ext = fileURL.pathExtension.lowercased()
        switch ext {
        case "sit":
            return ("SIT!", "SITx")
        case "cpt":
            return ("PACT", "CPT3")
        case "sea":
            return ("APPL", "aust")
        case "bin", "macbinary":
            return ("BINA", "maca")
        case "hqx":
            return ("TEXT", "BnHq")
        case "dsk", "img":
            return ("dImg", "dImg")
        case "txt", "text":
            return ("TEXT", "ttxt")
        case "pict":
            return ("PICT", "ttxt")
        case "wav", "aiff", "aif":
            return ("AIFF", "SCVT")
        case "mp3":
            return ("MPEG", "hook")
        default:
            return ("BINA", "maca")
        }
    }
}
