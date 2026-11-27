#ifndef SAVEDOCUMENT_H
#define SAVEDOCUMENT_H

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

struct SaveCharacter
{
    QJsonObject character;
    QJsonObject info;
    QJsonObject quests;
};

// Pure envelope codec. Unknown sections and unchanged JSON stay byte-exact.
// Character indices are file slot indices; mutations never insert/remove slots.
class SaveDocument
{
public:
    static bool parse(const QByteArray &bytes, SaveDocument &result, QString &error);
    QByteArray serialize() const;
    const QJsonObject &settings() const { return settingsObject; }
    const QList<SaveCharacter> &characters() const { return data; }
    bool hasCharacter(int index) const { return index >= 0 && index < data.size(); }
    SaveCharacter &editCharacter(int index) { return data[index]; }

private:
    struct ObjectSpan
    {
        qsizetype start;
        qsizetype length;
        QJsonObject original;
    };
    QByteArray source;
    QJsonObject settingsObject;
    QList<SaveCharacter> data;
    QList<ObjectSpan> spans;
};

#endif
