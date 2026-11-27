#include "jsonparser.h"
#include "restoreservice.h"

namespace {
SaveStore store;
const QList<SaveCharacter> noCharacters;
}
void JsonParser::configure(const SavePaths &paths) { store = SaveStore(paths); }
bool JsonParser::init(QString &error)
{
    store.invalidate();
    if (!RestoreService::recoverPending(store.paths(), error)) return false;
    return store.load(error);
}
void JsonParser::invalidate() { store.invalidate(); }
const SavePaths &JsonParser::paths() { return store.paths(); }
const QList<SaveCharacter> &JsonParser::characters()
{
    return store.isLoaded() ? store.document().characters() : noCharacters;
}
bool JsonParser::canEdit() { return store.canEdit(); }
bool JsonParser::mutateCharacter(int index, const std::function<void(SaveCharacter &)> &mutation, QString &error)
{
    return store.mutateCharacter(index, mutation, error);
}

bool JsonParser::mutateCharacterChecked(int index, const SaveStore::CheckedMutation &mutation, QString &error)
{
    return store.mutateCharacterChecked(index, mutation, error);
}
