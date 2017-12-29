/*
 * LibrePCB - Professional EDA for everyone!
 * Copyright (C) 2013 LibrePCB Developers, see AUTHORS.md for contributors.
 * http://librepcb.org/
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*****************************************************************************************
 *  Includes
 ****************************************************************************************/
#include <QtCore>
#include "cmdboardpolygonremove.h"
#include "../board.h"
#include "../items/bi_polygon.h"

/*****************************************************************************************
 *  Namespace
 ****************************************************************************************/
namespace librepcb {
namespace project {

/*****************************************************************************************
 *  Constructors / Destructor
 ****************************************************************************************/

CmdBoardPolygonRemove::CmdBoardPolygonRemove(BI_Polygon& polygon) noexcept :
    UndoCommand(tr("Remove polygon from board")),
    mBoard(polygon.getBoard()), mPolygon(polygon)
{
}

CmdBoardPolygonRemove::~CmdBoardPolygonRemove() noexcept
{
}

/*****************************************************************************************
 *  Inherited from UndoCommand
 ****************************************************************************************/

bool CmdBoardPolygonRemove::performExecute()
{
    performRedo(); // can throw

    return true;
}

void CmdBoardPolygonRemove::performUndo()
{
    mBoard.addPolygon(mPolygon); // can throw
}

void CmdBoardPolygonRemove::performRedo()
{
    mBoard.removePolygon(mPolygon); // can throw
}

/*****************************************************************************************
 *  End of File
 ****************************************************************************************/

} // namespace project
} // namespace librepcb
