/*  This file is part of YUView - The YUV player with advanced analytics toolset
 *   <https://github.com/IENT/YUView>
 *   Copyright (C) 2015  Institut für Nachrichtentechnik, RWTH Aachen University, GERMANY
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   In addition, as a special exception, the copyright holders give
 *   permission to link the code of portions of this program with the
 *   OpenSSL library under certain conditions as described in each
 *   individual source file, and distribute linked combinations including
 *   the two.
 *
 *   You must obey the GNU General Public License in all respects for all
 *   of the code used other than OpenSSL. If you modify file(s) with this
 *   exception, you may extend this exception to your version of the
 *   file(s), but you are not obligated to do so. If you do not wish to do
 *   so, delete this exception statement from your version. If you delete
 *   this exception statement from all source files in the program, then
 *   also delete it here.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <common/Functions.h>
#include <common/Color.h>

#include <QDialog>

#include "ui_statisticsStyleControl_ColorMapEditor.h"

class StatisticsStyleControl_ColorMapEditor : public QDialog
{
  Q_OBJECT

public:
  explicit StatisticsStyleControl_ColorMapEditor(const std::map<int, Color> &colorMap,
                                                 const Color &               other,
                                                 QWidget *                   parent = 0);

  std::map<int, Color> getColorMap();
  Color                getOtherColor();

public slots:
  // Override from QDialog. Check for dublicate entries.
  virtual void done(int r) override;

private slots:
  void slotItemClicked(QTableWidgetItem *item);
  void slotItemChanged(QTableWidgetItem *item);
  void on_pushButtonAdd_clicked();
  void on_pushButtonDelete_clicked();

protected:
  Ui::statisticStyleControl_ColorMapEditor ui;

  virtual void keyPressEvent(QKeyEvent *keyEvent) override;
};
