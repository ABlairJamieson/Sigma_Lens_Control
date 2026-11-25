#include "../include/TiffWriter.h"

#include <QFile>
#include <tiffio.h>
#include <memory>
#include <vector>
#include <limits>
#include <QByteArray>

// Minimal TIFF writer for 16-bit grayscale, little-endian, uncompressed.
// Adds a few standard TIFF/EXIF tags.
namespace
{
#pragma pack(push, 1)
  struct TiffHeader
  {
    quint16 endian;   // 'II' = 0x4949
    quint16 magic;    // 42
    quint32 ifdOffset;
  };

  struct IfdEntry
  {
    quint16 tag;
    quint16 type;
    quint32 count;
    quint32 valueOrOffset;
  };
#pragma pack(pop)

  enum : quint16
  {
    TAG_ImageWidth                  = 256,
    TAG_ImageLength                 = 257,
    TAG_BitsPerSample               = 258,
    TAG_Compression                 = 259,
    TAG_PhotometricInterpretation   = 262,
    TAG_ImageDescription            = 270,
    TAG_Make                        = 271,
    TAG_Model                       = 272,
    TAG_StripOffsets                = 273,
    TAG_RowsPerStrip                = 278,
    TAG_StripByteCounts             = 279,
    TAG_Software                    = 305,
    TAG_DateTime                    = 306,
    TAG_SampleFormat                = 339,
    TAG_ExifIFD                     = 34665,

    TAG_ExposureTime                = 33434,
    TAG_FNumber                     = 33437,
    TAG_DateTimeOriginal            = 36867,

    TYPE_BYTE       = 1,
    TYPE_ASCII      = 2,
    TYPE_SHORT      = 3,
    TYPE_LONG       = 4,
    TYPE_RATIONAL   = 5
  };

  static void align4(QByteArray& ba)
  {
    while ((ba.size() % 4) != 0)
      ba.append('\0');
  }

  static bool asciiFitsInline(const QString& s)
  {
    return (s.toLatin1().size() + 1) <= 4; // include null terminator
  }

  static quint32 asciiInlineValue(const QString& s)
  {
    QByteArray txt = s.toLatin1();
    txt.append('\0');

    while (txt.size() < 4)
      txt.append('\0');

    quint32 v = 0;
    v |= static_cast<quint8>(txt[0]);
    v |= static_cast<quint8>(txt[1]) << 8;
    v |= static_cast<quint8>(txt[2]) << 16;
    v |= static_cast<quint8>(txt[3]) << 24;
    return v;
  }

  static quint32 appendAscii(QByteArray& extras, quint32 absoluteOffset, const QString& s)
  {
    QByteArray txt = s.toLatin1();
    txt.append('\0');
    const quint32 off = absoluteOffset + static_cast<quint32>(extras.size());
    extras.append(txt);
    align4(extras);
    return off;
  }

  static quint32 appendRational(QByteArray& extras, quint32 absoluteOffset, quint32 num, quint32 den)
  {
    const quint32 off = absoluteOffset + static_cast<quint32>(extras.size());
    extras.append(reinterpret_cast<const char*>(&num), sizeof(num));
    extras.append(reinterpret_cast<const char*>(&den), sizeof(den));
    align4(extras);
    return off;
  }
}

bool TiffWriter::writeGray16Tiff(const QString& path,
                                 int w,
                                 int h,
                                 const QByteArray& raw,
                                 const TiffMetadata* meta,
                                 QString* errOut)
{
  if (w <= 0 || h <= 0)
  {
    if (errOut) *errOut = "Invalid image dimensions";
    return false;
  }

  const qint64 expected = static_cast<qint64>(w) * static_cast<qint64>(h) * 2;
  if (raw.size() != expected)
  {
    if (errOut) *errOut = QString("Raw size mismatch: got %1, expected %2").arg(raw.size()).arg(expected);
    return false;
  }

  const bool hasImageDescription = (meta && !meta->imageDescription.isEmpty());
  const bool hasMake             = (meta && !meta->make.isEmpty());
  const bool hasModel            = (meta && !meta->model.isEmpty());
  const bool hasSoftware         = (meta && !meta->software.isEmpty());
  const bool hasDateTime         = (meta && !meta->dateTime.isEmpty());

  const bool hasExposureTime     = (meta && meta->hasExposureTime);
  const bool hasFNumber          = (meta && meta->hasFNumber);
  const bool hasExifIfd          = (hasExposureTime || hasFNumber || hasDateTime);

  quint16 numIfd0Entries = 9; // base image tags
  if (hasImageDescription) ++numIfd0Entries;
  if (hasMake)             ++numIfd0Entries;
  if (hasModel)            ++numIfd0Entries;
  if (hasSoftware)         ++numIfd0Entries;
  if (hasDateTime)         ++numIfd0Entries;
  if (hasExifIfd)          ++numIfd0Entries;

  quint16 numExifEntries = 0;
  if (hasExposureTime) ++numExifEntries;
  if (hasFNumber)      ++numExifEntries;
  if (hasDateTime)     ++numExifEntries; // DateTimeOriginal

  const quint32 ifd0Offset = sizeof(TiffHeader);
  const quint32 ifd0Size = 2 + numIfd0Entries * sizeof(IfdEntry) + 4;

  const quint32 exifIfdOffset = hasExifIfd ? (ifd0Offset + ifd0Size) : 0;
  const quint32 exifIfdSize = hasExifIfd ? (2 + numExifEntries * sizeof(IfdEntry) + 4) : 0;

  const quint32 dataAreaOffset = ifd0Offset + ifd0Size + exifIfdSize;

  QByteArray extras;
  extras.reserve(1024);

  quint32 imageDescriptionOffset = 0;
  quint32 makeOffset = 0;
  quint32 modelOffset = 0;
  quint32 softwareOffset = 0;
  quint32 dateTimeOffset = 0;
  quint32 dateTimeOriginalOffset = 0;
  quint32 exposureTimeOffset = 0;
  quint32 fNumberOffset = 0;

  if (hasImageDescription && !asciiFitsInline(meta->imageDescription))
    imageDescriptionOffset = appendAscii(extras, dataAreaOffset, meta->imageDescription);
  if (hasMake && !asciiFitsInline(meta->make))
    makeOffset = appendAscii(extras, dataAreaOffset, meta->make);
  if (hasModel && !asciiFitsInline(meta->model))
    modelOffset = appendAscii(extras, dataAreaOffset, meta->model);
  if (hasSoftware && !asciiFitsInline(meta->software))
    softwareOffset = appendAscii(extras, dataAreaOffset, meta->software);
  if (hasDateTime && !asciiFitsInline(meta->dateTime))
    dateTimeOffset = appendAscii(extras, dataAreaOffset, meta->dateTime);
  if (hasDateTime)
    dateTimeOriginalOffset = appendAscii(extras, dataAreaOffset, meta->dateTime);

  if (hasExposureTime)
    exposureTimeOffset = appendRational(extras, dataAreaOffset, meta->exposureTimeNum, meta->exposureTimeDen);
  if (hasFNumber)
    fNumberOffset = appendRational(extras, dataAreaOffset, meta->fNumberNum, meta->fNumberDen);

  const quint32 imageOffset = dataAreaOffset + static_cast<quint32>(extras.size());
  const quint32 imageBytes  = static_cast<quint32>(expected);

  QFile f(path);
  if (!f.open(QIODevice::WriteOnly))
  {
    if (errOut) *errOut = "Failed to open output file";
    return false;
  }

  TiffHeader hdr;
  hdr.endian    = 0x4949; // 'II'
  hdr.magic     = 42;
  hdr.ifdOffset = ifd0Offset;

  f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

  // ---- IFD0 ----
  f.write(reinterpret_cast<const char*>(&numIfd0Entries), sizeof(numIfd0Entries));

  auto writeEntry = [&](quint16 tag, quint16 type, quint32 count, quint32 valueOrOffset)
  {
    IfdEntry e;
    e.tag = tag;
    e.type = type;
    e.count = count;
    e.valueOrOffset = valueOrOffset;
    f.write(reinterpret_cast<const char*>(&e), sizeof(e));
  };

  // Write in ascending tag order
  writeEntry(TAG_ImageWidth, TYPE_LONG, 1, static_cast<quint32>(w));
  writeEntry(TAG_ImageLength, TYPE_LONG, 1, static_cast<quint32>(h));
  writeEntry(TAG_BitsPerSample, TYPE_SHORT, 1, 16);
  writeEntry(TAG_Compression, TYPE_SHORT, 1, 1);
  writeEntry(TAG_PhotometricInterpretation, TYPE_SHORT, 1, 1);

  if (hasImageDescription)
  {
    const quint32 count = static_cast<quint32>(meta->imageDescription.toLatin1().size()) + 1;
    writeEntry(TAG_ImageDescription, TYPE_ASCII, count,
               asciiFitsInline(meta->imageDescription) ? asciiInlineValue(meta->imageDescription)
                                                       : imageDescriptionOffset);
  }

  if (hasMake)
  {
    const quint32 count = static_cast<quint32>(meta->make.toLatin1().size()) + 1;
    writeEntry(TAG_Make, TYPE_ASCII, count,
               asciiFitsInline(meta->make) ? asciiInlineValue(meta->make)
                                           : makeOffset);
  }

  if (hasModel)
  {
    const quint32 count = static_cast<quint32>(meta->model.toLatin1().size()) + 1;
    writeEntry(TAG_Model, TYPE_ASCII, count,
               asciiFitsInline(meta->model) ? asciiInlineValue(meta->model)
                                            : modelOffset);
  }

  writeEntry(TAG_StripOffsets, TYPE_LONG, 1, imageOffset);
  writeEntry(TAG_RowsPerStrip, TYPE_LONG, 1, static_cast<quint32>(h));
  writeEntry(TAG_StripByteCounts, TYPE_LONG, 1, imageBytes);

  if (hasSoftware)
  {
    const quint32 count = static_cast<quint32>(meta->software.toLatin1().size()) + 1;
    writeEntry(TAG_Software, TYPE_ASCII, count,
               asciiFitsInline(meta->software) ? asciiInlineValue(meta->software)
                                               : softwareOffset);
  }

  if (hasDateTime)
  {
    const quint32 count = static_cast<quint32>(meta->dateTime.toLatin1().size()) + 1;
    writeEntry(TAG_DateTime, TYPE_ASCII, count,
               asciiFitsInline(meta->dateTime) ? asciiInlineValue(meta->dateTime)
                                               : dateTimeOffset);
  }

  writeEntry(TAG_SampleFormat, TYPE_SHORT, 1, 1);

  if (hasExifIfd)
    writeEntry(TAG_ExifIFD, TYPE_LONG, 1, exifIfdOffset);

  const quint32 nextIfd0 = 0;
  f.write(reinterpret_cast<const char*>(&nextIfd0), sizeof(nextIfd0));

  // ---- EXIF IFD ----
  if (hasExifIfd)
  {
    f.write(reinterpret_cast<const char*>(&numExifEntries), sizeof(numExifEntries));

    if (hasExposureTime)
      writeEntry(TAG_ExposureTime, TYPE_RATIONAL, 1, exposureTimeOffset);

    if (hasFNumber)
      writeEntry(TAG_FNumber, TYPE_RATIONAL, 1, fNumberOffset);

    if (hasDateTime)
      writeEntry(TAG_DateTimeOriginal, TYPE_ASCII,
                 static_cast<quint32>(meta->dateTime.toLatin1().size()) + 1,
                 dateTimeOriginalOffset);

    const quint32 nextExifIfd = 0;
    f.write(reinterpret_cast<const char*>(&nextExifIfd), sizeof(nextExifIfd));
  }

  // ---- extra data area ----
  if (!extras.isEmpty())
  {
    if (f.write(extras.constData(), extras.size()) != extras.size())
    {
      if (errOut) *errOut = "Failed writing TIFF metadata block";
      return false;
    }
  }

  // ---- image payload ----
  if (f.write(raw.constData(), raw.size()) != raw.size())
  {
    if (errOut) *errOut = "Failed writing image payload";
    return false;
  }

  f.close();
  return true;
}

QImage TiffWriter::readGray16Display(const QString& path, QString* errOut)
{
  auto fail = [errOut](const QString& reason) { if (errOut) *errOut=reason; return QImage(); };
  std::unique_ptr<TIFF, decltype(&TIFFClose)> file(TIFFOpen(QFile::encodeName(path).constData(), "r"), TIFFClose);
  if (!file) return fail("Cannot open TIFF");
  uint32_t width=0,height=0;
  uint16_t bits=0,samples=0,format=0,photo=0,orientation=0;
  TIFFGetField(file.get(), TIFFTAG_IMAGEWIDTH, &width);
  TIFFGetField(file.get(), TIFFTAG_IMAGELENGTH, &height);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_BITSPERSAMPLE, &bits);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_SAMPLESPERPIXEL, &samples);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_SAMPLEFORMAT, &format);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_PHOTOMETRIC, &photo);
  TIFFGetFieldDefaulted(file.get(), TIFFTAG_ORIENTATION, &orientation);
  if (!width || !height || uint64_t(width)*height>250000000 ||
      bits!=16 || samples!=1 || format!=SAMPLEFORMAT_UINT || photo!=PHOTOMETRIC_MINISBLACK ||
      orientation!=ORIENTATION_TOPLEFT || TIFFIsTiled(file.get()))
    return fail("Unsupported TIFF: expected a 16-bit grayscale camera capture");
  QImage image(int(width),int(height),QImage::Format_Grayscale8);
  if (image.isNull()) return fail("Cannot allocate captured image");
  const auto bytes=TIFFScanlineSize(file.get());
  if (bytes<qint64(width)*2 || bytes>std::numeric_limits<int>::max()) return fail("Invalid TIFF row size");
  std::vector<uint16_t> row((size_t(bytes)+1)/2);
  for (uint32_t y=0;y<height;++y) {
    if (TIFFReadScanline(file.get(),row.data(),y,0)<0) return fail("Cannot read TIFF pixels");
    auto out=image.scanLine(int(y));
    for (uint32_t x=0;x<width;++x) out[x]=uchar(row[x]>>8);
  }
  return image;
}
