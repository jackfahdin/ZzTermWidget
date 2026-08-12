#ifndef CHARWIDTH_H
#define CHARWIDTH_H

#include <QString>
#include <QDebug>
#include <string>

#include "utf8proc.h"

#include <QFont>
#include <QFontMetrics>
#include <QFontDatabase>
#include <QApplication>

class CharWidth
{
public:
    CharWidth(QFont font);
    ~CharWidth();

    void setFont(QFont font);
    int font_width(char32_t ucs);
    int font_width(const QChar & c);
    int string_font_width( const std::u32string & wstr );
    int string_font_width( const QString & str );

    static int unicode_width(char32_t ucs, bool fix_width = true);
    static int unicode_width(const QChar & c, bool fix_width = true);
    static int string_unicode_width(const std::u32string & wstr, bool fix_width = true);
    static int string_unicode_width(const QString & str, bool fix_width = true);

private:
    QFontMetrics *fm;
};

#endif // CHARWIDTH_H
