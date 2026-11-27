#ifndef JSONPARSER_H
#define JSONPARSER_H

#include "savestore.h"

// App-level facade; no UI or filesystem work inside the envelope codec.
class JsonParser
{
public:
    JsonParser() = delete;
    static void configure(const SavePaths &paths);
    static bool init(QString &error);
    static void invalidate();
    static const SavePaths &paths();
    static const QList<SaveCharacter> &characters();
    static bool canEdit();
    static bool mutateCharacter(int index, const std::function<void(SaveCharacter &)> &mutation, QString &error);
};

#endif
