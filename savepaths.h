#ifndef SAVEPATHS_H
#define SAVEPATHS_H

#include <QString>

// All game filenames are resolved in one place. An explicit override is useful
// for disposable saves and for recovery tooling.
class SavePaths
{
public:
    explicit SavePaths(const QString &directoryOverride = {});
    QString directory() const { return saveDirectory; }
    QString primary() const;
    QString gameBackup() const;
    QString legacyEditorBackup() const;

private:
    QString saveDirectory;
};

#endif
