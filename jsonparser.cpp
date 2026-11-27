#include "jsonparser.h"

namespace {
SaveStore store;
const QList<SaveCharacter> noCharacters;
}
void JsonParser::configure(const SavePaths &paths) { store = SaveStore(paths); }
bool JsonParser::init(QString &error) { return store.load(error); }
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
