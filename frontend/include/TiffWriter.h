#pragma once

#include <QString>
#include <QByteArray>
#include <QImage>

namespace TiffWriter
{
  struct TiffMetadata
  {
    QString imageDescription;   // e.g. "Gain=0; FocusRaw=309; ApertureRaw=4352"
    QString make;               // e.g. "ZWO"
    QString model;              // e.g. "ASI6200MM Pro"
    QString software;           // e.g. "SigmaControl Qt frontend"
    QString dateTime;           // EXIF/TIFF format: "YYYY:MM:DD HH:MM:SS"

    bool hasExposureTime = false;
    quint32 exposureTimeNum = 0;   // ExposureTime as rational seconds
    quint32 exposureTimeDen = 1;   // e.g. 18000 us => 18/1000 or 9/500

    bool hasFNumber = false;
    quint32 fNumberNum = 0;        // e.g. f/2.8 => 28/10
    quint32 fNumberDen = 1;
  };

  // Read the full spatial resolution for display; the saved 16-bit data stays intact.
  QImage readGray16Display(const QString& path, QString* errOut = nullptr);

  // Save RAW16 little-endian grayscale image as 16-bit TIFF.
  // raw must contain w*h*2 bytes.
  bool writeGray16Tiff(const QString& path,
                       int w,
                       int h,
                       const QByteArray& raw,
                       const TiffMetadata* meta = nullptr,
                       QString* errOut = nullptr);
}