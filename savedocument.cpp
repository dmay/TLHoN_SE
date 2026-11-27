#include "savedocument.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <utility>

namespace {
const QByteArray settingsMarker = "~SettingsSplit~";
const QByteArray characterMarker = "~NoticeMeSenpaiNoticeMe~";

bool whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

void skipWhitespace(const QByteArray &bytes, qsizetype &position)
{
    while (position < bytes.size() && whitespace(bytes[position]))
        ++position;
}

bool at(const QByteArray &bytes, qsizetype position, const QByteArray &marker)
{
    return bytes.mid(position, marker.size()) == marker;
}

// Consume exactly one object, without treating semicolons or markers in JSON
// strings as envelope delimiters. Qt validates the entire object afterward.
bool readObject(const QByteArray &bytes, qsizetype &position, QJsonObject &object,
                qsizetype &start, qsizetype &length, QString &error)
{
    skipWhitespace(bytes, position);
    start = position;
    if (position >= bytes.size() || bytes[position] != '{') {
        error = QString("Expected a JSON object at byte %1.").arg(position);
        return false;
    }
    int depth = 0;
    bool quoted = false, escaped = false;
    while (position < bytes.size()) {
        const char c = bytes[position++];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') {
            quoted = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}' && --depth == 0) {
            length = position - start;
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(bytes.mid(start, length), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                error = QString("Invalid JSON at byte %1: %2.")
                    .arg(start + parseError.offset).arg(parseError.errorString());
                return false;
            }
            object = document.object();
            return true;
        }
    }
    error = QString("Unterminated JSON object at byte %1.").arg(start);
    return false;
}

bool consume(const QByteArray &bytes, qsizetype &position, const QByteArray &marker, QString &error)
{
    skipWhitespace(bytes, position);
    if (!at(bytes, position, marker)) {
        error = QString("Expected '%1' at byte %2.").arg(QString::fromLatin1(marker)).arg(position);
        return false;
    }
    position += marker.size();
    return true;
}

// Miscellaneous data is not an editor-owned schema. Validate the boundary and
// any object-shaped chunks, but retain opaque chunks and every separator byte.
bool readMisc(const QByteArray &bytes, qsizetype &position, QString &error)
{
    const qsizetype start = position;
    bool content = false;
    while (position < bytes.size()) {
        skipWhitespace(bytes, position);
        if (position == bytes.size() || at(bytes, position, characterMarker))
            break;
        if (at(bytes, position, settingsMarker)) {
            error = QString("Unexpected settings delimiter at byte %1.").arg(position);
            return false;
        }
        if (bytes[position] == '{') {
            QJsonObject ignored;
            qsizetype objectStart, length;
            if (!readObject(bytes, position, ignored, objectStart, length, error))
                return false;
            content = true;
            skipWhitespace(bytes, position);
            if (position == bytes.size() || at(bytes, position, characterMarker))
                break;
            if (!consume(bytes, position, ";", error))
                return false;
        } else if (bytes[position] == ';') {
            ++position;
        } else {
            // Opaque scalar/text chunks may contain quoted strings; only a
            // delimiter outside a quoted string can end the slot.
            bool quoted = false, escaped = false;
            while (position < bytes.size()) {
                if (!quoted && (bytes[position] == ';' ||
                       at(bytes, position, characterMarker) || at(bytes, position, settingsMarker)))
                    break;
                const char c = bytes[position++];
                if (quoted) {
                    if (escaped) escaped = false;
                    else if (c == '\\') escaped = true;
                    else if (c == '"') quoted = false;
                } else if (c == '"') {
                    quoted = true;
                }
            }
            if (quoted) {
                error = QString("Unterminated quoted miscellaneous data at byte %1.").arg(start);
                return false;
            }
            content = true;
        }
    }
    if (!content) {
        error = QString("Missing miscellaneous section at byte %1.").arg(start);
        return false;
    }
    return true;
}
}

bool SaveDocument::parse(const QByteArray &bytes, SaveDocument &result, QString &error)
{
    SaveDocument candidate;
    candidate.source = bytes;
    qsizetype position = bytes.startsWith("\xef\xbb\xbf") ? 3 : 0;
    if (bytes.trimmed().isEmpty()) {
        error = "Save file is empty.";
        return false;
    }
    qsizetype start, length;
    if (!readObject(bytes, position, candidate.settingsObject, start, length, error) ||
        !consume(bytes, position, settingsMarker, error))
        return false;

    skipWhitespace(bytes, position);
    // A settings-only save with the explicit settings delimiter represents
    // zero characters. Its editor controls will remain disabled.
    while (position < bytes.size()) {
        SaveCharacter character;
        for (auto *object : {&character.character, &character.info, &character.quests}) {
            if (!readObject(bytes, position, *object, start, length, error))
                return false;
            candidate.spans.append(ObjectSpan{start, length, *object});
            if (!consume(bytes, position, ";", error))
                return false;
        }
        if (!readMisc(bytes, position, error))
            return false;
        candidate.data.append(character);
        if (position == bytes.size())
            break;
        if (!consume(bytes, position, characterMarker, error))
            return false;
        skipWhitespace(bytes, position);
        if (position == bytes.size()) {
            error = "Missing character after character delimiter.";
            return false;
        }
    }
    result = std::move(candidate);
    error.clear();
    return true;
}

QByteArray SaveDocument::serialize() const
{
    QByteArray bytes;
    qsizetype cursor = 0, spanIndex = 0;
    for (const auto &slot : data) {
        for (const auto *object : {&slot.character, &slot.info, &slot.quests}) {
            const auto &span = spans[spanIndex++];
            bytes += source.mid(cursor, span.start - cursor);
            bytes += *object == span.original ? source.mid(span.start, span.length)
                : QJsonDocument(*object).toJson(QJsonDocument::Compact);
            cursor = span.start + span.length;
        }
    }
    bytes += source.mid(cursor);
    return bytes;
}
