/*
    This source file is part of Konsole, a terminal emulator.

    SPDX-FileCopyrightText: 2007-2008 Robert Knight <robertknight@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "KeyboardTranslatorReader.h"

// Qt
#include <QBuffer>
#include <QKeySequence>
#include <QRegularExpression>

// KDE
#include <KLocalizedString>

using namespace Konsole;

// each line of the keyboard translation file is one of:
//
// - keyboard "name"
// - key KeySequence : "characters"
// - key KeySequence : CommandName
//
// KeySequence begins with the name of the key ( taken from the Qt::Key enum )
// and is followed by the keyboard modifiers and state flags ( with + or - in front
// of each modifier or flag to indicate whether it is required ).  All keyboard modifiers
// and flags are optional, if a particular modifier or state is not specified it is
// assumed not to be a part of the sequence.  The key sequence may contain whitespace
//
// eg:  "key Up+Shift : scrollLineUp"
//      "key PgDown-Shift : "\E[6~"
//
// (lines containing only whitespace are ignored, parseLine assumes that comments have
// already been removed)
//

KeyboardTranslatorReader::KeyboardTranslatorReader(QIODevice *source)
    : _source(source)
    , _description(QString())
    , _nextEntry()
    , _hasNext(false)
{
    // read input until we find the description
    while (_description.isEmpty() && !source->atEnd()) {
        QList<Token> tokens = tokenize(QString::fromLocal8Bit(source->readLine()));
        if (!tokens.isEmpty() && tokens.first().type == Token::TitleKeyword) {
            _description = i18n(tokens[1].text.toUtf8().constData());
        }
    }
    // read first entry (if any)
    readNext();
}

KeyboardTranslatorReader::~KeyboardTranslatorReader() = default;

void KeyboardTranslatorReader::readNext()
{
    // find next entry
    while (!_source->atEnd()) {
        const QList<Token> &tokens = tokenize(QString::fromLocal8Bit(_source->readLine()));
        if (!tokens.isEmpty() && tokens.first().type == Token::KeyKeyword) {
            KeyboardTranslator::States flags = KeyboardTranslator::NoState;
            KeyboardTranslator::States flagMask = KeyboardTranslator::NoState;
            Qt::KeyboardModifiers modifiers = Qt::NoModifier;
            Qt::KeyboardModifiers modifierMask = Qt::NoModifier;

            int keyCode = Qt::Key_unknown;

            decodeSequence(tokens[1].text.toLower(), keyCode, modifiers, modifierMask, flags, flagMask);

            KeyboardTranslator::Command command = KeyboardTranslator::NoCommand;
            QByteArray text;

            // get text or command
            if (tokens[2].type == Token::OutputText) {
                text = tokens[2].text.toLocal8Bit();
            } else if (tokens[2].type == Token::Command) {
                // identify command
                if (!parseAsCommand(tokens[2].text, command)) {
                    qCDebug(KonsoleKeyTrDebug) << "Key" << tokens[1].text << ", Command" << tokens[2].text << "not understood. ";
                }
            }

            KeyboardTranslator::Entry newEntry;
            newEntry.setKeyCode(keyCode);
            newEntry.setState(flags);
            newEntry.setStateMask(flagMask);
            newEntry.setModifiers(modifiers);
            newEntry.setModifierMask(modifierMask);
            newEntry.setText(text);
            newEntry.setCommand(command);

            _nextEntry = newEntry;

            _hasNext = true;

            return;
        }
    }

    _hasNext = false;
}

bool KeyboardTranslatorReader::parseAsCommand(const QString &text, KeyboardTranslator::Command &command)
{
    if (text.compare(QLatin1String("erase"), Qt::CaseInsensitive) == 0) {
        command = KeyboardTranslator::EraseCommand;
    } else if (text.compare(QLatin1String("scrollpageup"), Qt::CaseInsensitive) == 0) {
        command = KeyboardTranslator::ScrollPageUpCommand;
    } else if (text.compare(QLatin1String("scrollpagedown"), Qt::CaseInsensitive) == 0) {
        command = KeyboardTranslator::ScrollPageDownCommand;
    } else if (text.compare(QLatin1String("scrolllineup"), Qt::CaseInsensitive) == 0) {
        command = KeyboardTranslator::ScrollLineUpCommand;
    } else if (text.compare(QLatin1String("scrolllinedown"), Qt::CaseInsensitive) == 0) {
        command = KeyboardTranslator::ScrollLineDownCommand;
    } else if (text.compare(QLatin1String("scrolluptotop"), Qt::CaseInsensitive) == 0) {
        command = KeyboardTranslator::ScrollUpToTopCommand;
    } else if (text.compare(QLatin1String("scrolldowntobottom"), Qt::CaseInsensitive) == 0) {
        command = KeyboardTranslator::ScrollDownToBottomCommand;
    } else if (text.compare(QLatin1String("scrollpromptup"), Qt::CaseInsensitive) == 0) {
        command = KeyboardTranslator::ScrollPromptUpCommand;
    } else if (text.compare(QLatin1String("scrollpromptdown"), Qt::CaseInsensitive) == 0) {
        command = KeyboardTranslator::ScrollPromptDownCommand;
    } else {
        return false;
    }

    return true;
}

bool KeyboardTranslatorReader::decodeSequence(const QString &text,
                                              int &keyCode,
                                              Qt::KeyboardModifiers &modifiers,
                                              Qt::KeyboardModifiers &modifierMask,
                                              KeyboardTranslator::States &flags,
                                              KeyboardTranslator::States &flagMask)
{
    bool isWanted = true;
    QString buffer;

    Qt::KeyboardModifiers tempModifiers = modifiers;
    Qt::KeyboardModifiers tempModifierMask = modifierMask;
    KeyboardTranslator::States tempFlags = flags;
    KeyboardTranslator::States tempFlagMask = flagMask;

    for (int i = 0; i < text.count(); i++) {
        const QChar &ch = text[i];
        const bool isFirstLetter = (i == 0);
        const bool isLastLetter = (i == text.count() - 1);
        bool endOfItem = true;
        if (ch.isLetterOrNumber()) {
            endOfItem = false;
            buffer.append(ch);
        } else if (isFirstLetter) {
            buffer.append(ch);
        }

        if ((endOfItem || isLastLetter) && !buffer.isEmpty()) {
            Qt::KeyboardModifier itemModifier = Qt::NoModifier;
            int itemKeyCode = 0;
            KeyboardTranslator::State itemFlag = KeyboardTranslator::NoState;

            if (parseAsModifier(buffer, itemModifier)) {
                tempModifierMask |= itemModifier;

                if (isWanted) {
                    tempModifiers |= itemModifier;
                }
            } else if (parseAsStateFlag(buffer, itemFlag)) {
                tempFlagMask |= itemFlag;

                if (isWanted) {
                    tempFlags |= itemFlag;
                }
            } else if (parseAsKeyCode(buffer, itemKeyCode)) {
                keyCode = itemKeyCode;
            } else {
                qCDebug(KonsoleKeyTrDebug) << "Unable to parse key binding item:" << buffer;
            }

            buffer.clear();
        }

        // check if this is a wanted / not-wanted flag and update the
        // state ready for the next item
        if (ch == QLatin1Char('+')) {
            isWanted = true;
        } else if (ch == QLatin1Char('-')) {
            isWanted = false;
        }
    }

    modifiers = tempModifiers;
    modifierMask = tempModifierMask;
    flags = tempFlags;
    flagMask = tempFlagMask;

    return true;
}

bool KeyboardTranslatorReader::parseAsModifier(const QString &item, Qt::KeyboardModifier &modifier)
{
    if (item == QLatin1String("shift")) {
        modifier = Qt::ShiftModifier;
    } else if (item == QLatin1String("ctrl") || item == QLatin1String("control")) {
        modifier = Qt::ControlModifier;
    } else if (item == QLatin1String("alt")) {
        modifier = Qt::AltModifier;
    } else if (item == QLatin1String("meta")) {
        modifier = Qt::MetaModifier;
    } else if (item == QLatin1String("keypad")) {
        modifier = Qt::KeypadModifier;
    } else {
        return false;
    }

    return true;
}

bool KeyboardTranslatorReader::parseAsStateFlag(const QString &item, KeyboardTranslator::State &flag)
{
    if (item == QLatin1String("appcukeys") || item == QLatin1String("appcursorkeys")) {
        flag = KeyboardTranslator::CursorKeysState;
    } else if (item == QLatin1String("ansi")) {
        flag = KeyboardTranslator::AnsiState;
    } else if (item == QLatin1String("newline")) {
        flag = KeyboardTranslator::NewLineState;
    } else if (item == QLatin1String("appscreen")) {
        flag = KeyboardTranslator::AlternateScreenState;
    } else if (item == QLatin1String("anymod") || item == QLatin1String("anymodifier")) {
        flag = KeyboardTranslator::AnyModifierState;
    } else if (item == QLatin1String("appkeypad")) {
        flag = KeyboardTranslator::ApplicationKeypadState;
    } else {
        return false;
    }

    return true;
}

bool KeyboardTranslatorReader::parseAsKeyCode(const QString &item, int &keyCode)
{
    const QKeySequence sequence = QKeySequence::fromString(item);
    if (!sequence.isEmpty()) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        keyCode = sequence[0].toCombined();
#else
        keyCode = sequence[0];
#endif

        if (sequence.count() > 1) {
            qCDebug(KonsoleKeyTrDebug) << "Unhandled key codes in sequence: " << item;
        }
    } else {
        return false;
    }

    return true;
}

QString KeyboardTranslatorReader::description() const
{
    return _description;
}

bool KeyboardTranslatorReader::hasNextEntry() const
{
    return _hasNext;
}

KeyboardTranslator::Entry KeyboardTranslatorReader::createEntry(const QString &condition, const QString &result)
{
    QString entryString(QStringLiteral("keyboard \"temporary\"\nkey "));
    entryString.append(condition);
    entryString.append(QLatin1String(" : "));

    // if 'result' is the name of a command then the entry result will be that command,
    // otherwise the result will be treated as a string to echo when the key sequence
    // specified by 'condition' is pressed
    KeyboardTranslator::Command command;
    if (parseAsCommand(result, command)) {
        entryString.append(result);
    } else {
        entryString.append(QLatin1Char('\"') + result + QLatin1Char('\"'));
    }

    QByteArray array = entryString.toUtf8();
    QBuffer buffer(&array);
    buffer.open(QIODevice::ReadOnly);
    KeyboardTranslatorReader reader(&buffer);

    KeyboardTranslator::Entry entry;
    if (reader.hasNextEntry()) {
        entry = reader.nextEntry();
    }

    return entry;
}

KeyboardTranslator::Entry KeyboardTranslatorReader::nextEntry()
{
    Q_ASSERT(_hasNext);
    KeyboardTranslator::Entry entry = _nextEntry;
    readNext();
    return entry;
}

bool KeyboardTranslatorReader::parseError()
{
    return false;
}

QList<KeyboardTranslatorReader::Token> KeyboardTranslatorReader::tokenize(const QString &line)
{
    QString text = line;

    // remove comments
    bool inQuotes = false;
    int commentPos = -1;
    for (int i = text.length() - 1; i >= 0; i--) {
        QChar ch = text[i];
        if (ch == QLatin1Char('\"')) {
            inQuotes = !inQuotes;
        } else if (ch == QLatin1Char('#') && !inQuotes) {
            commentPos = i;
        }
    }
    if (commentPos != -1) {
        text.remove(commentPos, text.length());
    }

    text = text.simplified();

    QList<Token> list;

    if (text.isEmpty()) {
        return list;
    }

    // Example:
    // keyboard "Default (XFree 4)"
    static const QLatin1String prefix("keyboard");
    if (text.startsWith(prefix)) {
        text.remove(0, prefix.size()).remove(QLatin1Char('"'));
        text = text.simplified();
        if (!text.isEmpty()) {
            Token titleToken = {Token::TitleKeyword, QString()};
            Token textToken = {Token::TitleText, text};
            list << titleToken << textToken;
        }

        return list;
    }

    // Examples:
    // key Enter-NewLine                 : "\r"
    // key Home        -AnyMod-AppCuKeys : "\E[H"
    static const QRegularExpression key(QStringLiteral(R"(key\s+(.+?)\s*:\s*(\"(.*)\"|\w+))"));

    QRegularExpressionMatch keyMatch(key.match(text));
    if (!keyMatch.hasMatch()) {
        qCDebug(KonsoleKeyTrDebug) << "Line in keyboard translator file could not be parsed:" << text;
        return list;
    }

    Token keyToken = {Token::KeyKeyword, QString()};
    QString sequenceTokenString = keyMatch.captured(1);
    Token sequenceToken = {Token::KeySequence, sequenceTokenString.remove(QLatin1Char(' '))};

    list << keyToken << sequenceToken;

    // capturedTexts().at(3) is the output string
    const QStringView outText = keyMatch.capturedView(3);
    if (!outText.isEmpty()) {
        Token outputToken = {Token::OutputText, outText.toString()};
        list << outputToken;
    } else {
        // capturedTexts().at(2) is a command
        Token commandToken = {Token::Command, keyMatch.captured(2)};
        list << commandToken;
    }

    return list;
}
