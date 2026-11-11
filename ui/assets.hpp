#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QImageReader>
#include <QPixmap>

namespace loo_ui {
// A local installation may supply its own licensed artwork. Public binaries
// remain self-contained and use the project's original embedded artwork.
inline QString assetPath(const QString& fileName) {
    const auto local=QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("assets/")+fileName);
    if(QImageReader(local).canRead())return local;
    return QStringLiteral(":/assets/")+fileName;
}
inline QPixmap assetPixmap(const QString& fileName) { return QPixmap(assetPath(fileName)); }
}
