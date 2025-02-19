/*
 * Copyright (C) 2015-2024 Département de l'Instruction Publique (DIP-SEM)
 *
 * This file is part of OpenBoard.
 *
 * OpenBoard is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3 of the License,
 * with a specific linking exception for the OpenSSL project's
 * "OpenSSL" library (or with modified versions of it that use the
 * same license as the "OpenSSL" library).
 *
 * OpenBoard is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OpenBoard. If not, see <http://www.gnu.org/licenses/>.
 */


#pragma once

#include <QLabel>

// forward
class QPropertyAnimation;

class UBSnapIndicator : public QLabel
{
    Q_OBJECT
    Q_PROPERTY(int alpha READ alpha WRITE setAlpha)

public:
    UBSnapIndicator(QWidget* parent);

    void appear(Qt::Corner corner, QPointF snapPoint);

    int alpha() const;
    void setAlpha(int opacity);

    void setColor(const QColor& color);

protected:
    virtual void paintEvent(QPaintEvent* event) override;

private:
    Qt::Corner mCorner{Qt::TopLeftCorner};
    int mAlpha;
    QColor mColor{0x62a7e0};
    QPropertyAnimation* mAnimation;
};

