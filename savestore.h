#ifndef SAVESTORE_H
#define SAVESTORE_H

#include "savedocument.h"
#include "savepaths.h"
#include <functional>

class SaveStore
{
public:
    explicit SaveStore(SavePaths paths = SavePaths());
    const SavePaths &paths() const { return savePaths; }
    const SaveDocument &document() const { return current; }
    bool isLoaded() const { return loaded; }
    void invalidate() { loaded = false; }
    bool canEdit() const;
    bool load(QString &error);
    // Callback receives a detached candidate, published only after a checked
    // atomic write. Use a single callback for related character/info/quest edits.
    bool mutateCharacter(int index, const std::function<void(SaveCharacter &)> &mutation, QString &error);

    // False aborts the detached candidate before serialization or file staging.
    using CheckedMutation = std::function<bool(SaveCharacter &, QString &)>;
    bool mutateCharacterChecked(int index, const CheckedMutation &mutation, QString &error);

private:
    SavePaths savePaths;
    SaveDocument current;
    QByteArray loadedBytes;
    bool loaded = false;
};

#endif
