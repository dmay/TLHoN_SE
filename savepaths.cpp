#include "savepaths.h"
#include <QDir>

SavePaths::SavePaths(const QString &directoryOverride)
{
    const QString profile = qEnvironmentVariable("USERPROFILE", QDir::homePath());
    const QString directory = directoryOverride.isEmpty()
        ? QDir(profile).filePath("AppData/LocalLow/OverTheMoon/TLHON")
        : directoryOverride;
    saveDirectory = QDir(directory).absolutePath();
}

QString SavePaths::primary() const { return QDir(saveDirectory).filePath("SaveData.txt"); }
QString SavePaths::gameBackup() const { return primary() + ".bak"; }
QString SavePaths::legacyEditorBackup() const { return primary() + "_bckp"; }
