/*
 * LibrePCB - Professional EDA for everyone!
 * Copyright (C) 2013 LibrePCB Developers, see AUTHORS.md for contributors.
 * https://librepcb.org/
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

/*******************************************************************************
 *  Includes
 ******************************************************************************/
#include "board.h"

#include "../../application.h"
#include "../../exceptions.h"
#include "../../geometry/polygon.h"
#include "../../graphics/graphicsscene.h"
#include "../../library/cmp/component.h"
#include "../../library/dev/device.h"
#include "../../library/pkg/footprint.h"
#include "../../serialization/sexpression.h"
#include "../../types/gridproperties.h"
#include "../../types/lengthunit.h"
#include "../../utils/scopeguardlist.h"
#include "../../utils/toolbox.h"
#include "../circuit/circuit.h"
#include "../circuit/componentinstance.h"
#include "../circuit/netsignal.h"
#include "../erc/ercmsg.h"
#include "../project.h"
#include "boardairwiresbuilder.h"
#include "boarddesignrules.h"
#include "boardfabricationoutputsettings.h"
#include "boardlayerstack.h"
#include "boardplanefragmentsbuilder.h"
#include "boardselectionquery.h"
#include "items/bi_airwire.h"
#include "items/bi_device.h"
#include "items/bi_footprintpad.h"
#include "items/bi_hole.h"
#include "items/bi_netline.h"
#include "items/bi_netpoint.h"
#include "items/bi_netsegment.h"
#include "items/bi_plane.h"
#include "items/bi_polygon.h"
#include "items/bi_stroketext.h"
#include "items/bi_via.h"

#include <QtCore>
#include <QtWidgets>

#include <algorithm>

/*******************************************************************************
 *  Namespace
 ******************************************************************************/
namespace librepcb {

/*******************************************************************************
 *  Constructors / Destructor
 ******************************************************************************/

Board::Board(Project& project,
             std::unique_ptr<TransactionalDirectory> directory,
             const QString& directoryName, const Version& fileFormat,
             bool create, const QString& newName)
  : QObject(&project),
    mProject(project),
    mDirectoryName(directoryName),
    mDirectory(std::move(directory)),
    mIsAddedToProject(false),
    mUuid(Uuid::createRandom()),
    mName("New Board") {
  if (mDirectoryName.isEmpty()) {
    throw LogicError(__FILE__, __LINE__);
  }

  try {
    mGraphicsScene.reset(new GraphicsScene());

    // try to open/create the board file
    if (create) {
      // set attributes
      mName = ElementName(newName);  // can throw
      mDefaultFontFileName = qApp->getDefaultStrokeFontName();

      // load default layer stack
      mLayerStack.reset(new BoardLayerStack(*this));

      // load default grid properties (smaller grid than in schematics to avoid
      // grid snap issues)
      mGridProperties.reset(new GridProperties(GridProperties::Type_t::Lines,
                                               PositiveLength(635000),
                                               LengthUnit::millimeters()));

      // load default design rules
      mDesignRules.reset(new BoardDesignRules());

      // load default fabrication output settings
      mFabricationOutputSettings.reset(new BoardFabricationOutputSettings());
    } else {
      const QString fp = "board.lp";
      const SExpression root =
          SExpression::parse(mDirectory->read(fp), mDirectory->getAbsPath(fp));

      // the board seems to be ready to open, so we will create all needed
      // objects

      mUuid = deserialize<Uuid>(root.getChild("@0"), fileFormat);
      mName = deserialize<ElementName>(root.getChild("name/@0"), fileFormat);
      if (const SExpression* child = root.tryGetChild("default_font")) {
        mDefaultFontFileName = child->getChild("@0").getValue();
      } else {
        mDefaultFontFileName = qApp->getDefaultStrokeFontName();
      }

      // Load grid properties
      mGridProperties.reset(
          new GridProperties(root.getChild("grid"), fileFormat));

      // Load layer stack
      mLayerStack.reset(
          new BoardLayerStack(*this, root.getChild("layers"), fileFormat));

      // load design rules
      mDesignRules.reset(
          new BoardDesignRules(root.getChild("design_rules"), fileFormat));

      // load fabrication output settings
      mFabricationOutputSettings.reset(new BoardFabricationOutputSettings(
          root.getChild("fabrication_output_settings"), fileFormat));

      // Load all device instances
      foreach (const SExpression& node, root.getChildren("device")) {
        BI_Device* device = new BI_Device(*this, node, fileFormat);
        if (getDeviceInstanceByComponentUuid(
                device->getComponentInstanceUuid())) {
          throw RuntimeError(
              __FILE__, __LINE__,
              QString("There is already a device of the component instance "
                      "\"%1\"!")
                  .arg(device->getComponentInstanceUuid().toStr()));
        }
        mDeviceInstances.insert(device->getComponentInstanceUuid(), device);
      }

      // Load all netsegments
      foreach (const SExpression& node, root.getChildren("netsegment")) {
        BI_NetSegment* netsegment = new BI_NetSegment(*this, node, fileFormat);
        if (getNetSegmentByUuid(netsegment->getUuid())) {
          throw RuntimeError(
              __FILE__, __LINE__,
              QString("There is already a netsegment with the UUID \"%1\"!")
                  .arg(netsegment->getUuid().toStr()));
        }
        mNetSegments.append(netsegment);
      }

      // Load all planes
      foreach (const SExpression& node, root.getChildren("plane")) {
        BI_Plane* plane = new BI_Plane(*this, node, fileFormat);
        mPlanes.append(plane);
      }

      // Load all polygons
      foreach (const SExpression& node, root.getChildren("polygon")) {
        BI_Polygon* polygon = new BI_Polygon(*this, node, fileFormat);
        mPolygons.append(polygon);
      }

      // Load all stroke texts
      foreach (const SExpression& node, root.getChildren("stroke_text")) {
        BI_StrokeText* text = new BI_StrokeText(*this, node, fileFormat);
        mStrokeTexts.append(text);
      }

      // Load all holes
      foreach (const SExpression& node, root.getChildren("hole")) {
        BI_Hole* hole = new BI_Hole(*this, node, fileFormat);
        mHoles.append(hole);
      }

      // load user settings
      try {
        const QString fp = "settings.user.lp";
        const SExpression root = SExpression::parse(mDirectory->read(fp),
                                                    mDirectory->getAbsPath(fp));
        for (const SExpression& child : root.getChildren("layer")) {
          const QString name = child.getChild("@0").getValue();
          if (GraphicsLayer* layer = mLayerStack->getLayer(name)) {
            layer->setColor(
                deserialize<QColor>(child.getChild("color/@0"), fileFormat));
            layer->setColorHighlighted(
                deserialize<QColor>(child.getChild("color_hl/@0"), fileFormat));
            layer->setVisible(
                deserialize<bool>(child.getChild("visible/@0"), fileFormat));
          }
        }
        foreach (const SExpression& node, root.getChildren("plane")) {
          const Uuid uuid = deserialize<Uuid>(node.getChild("@0"), fileFormat);
          if (BI_Plane* plane = getPlaneByUuid(uuid)) {
            plane->setVisible(
                deserialize<bool>(node.getChild("visible/@0"), fileFormat));
          }
        }
      } catch (const Exception&) {
        // Project user settings are normally not put under version control and
        // thus the likelyhood of parse errors is higher (e.g. when switching to
        // an older, now incompatible revision). To avoid frustration, we just
        // ignore these errors and load the default settings instead...
        qCritical() << "Could not open board user settings, defaults will be "
                       "used instead.";
      }
    }

    rebuildAllPlanes();
    updateErcMessages();
    updateIcon();

    // emit the "attributesChanged" signal when the project has emitted it
    connect(&mProject, &Project::attributesChanged, this,
            &Board::attributesChanged);

    connect(&mProject.getCircuit(), &Circuit::componentAdded, this,
            &Board::updateErcMessages);
    connect(&mProject.getCircuit(), &Circuit::componentRemoved, this,
            &Board::updateErcMessages);
  } catch (...) {
    // free the allocated memory in the reverse order of their allocation...
    qDeleteAll(mErcMsgListUnplacedComponentInstances);
    mErcMsgListUnplacedComponentInstances.clear();
    qDeleteAll(mAirWires);
    mAirWires.clear();
    qDeleteAll(mHoles);
    mHoles.clear();
    qDeleteAll(mStrokeTexts);
    mStrokeTexts.clear();
    qDeleteAll(mPolygons);
    mPolygons.clear();
    qDeleteAll(mPlanes);
    mPlanes.clear();
    qDeleteAll(mNetSegments);
    mNetSegments.clear();
    qDeleteAll(mDeviceInstances);
    mDeviceInstances.clear();
    mFabricationOutputSettings.reset();
    mDesignRules.reset();
    mGridProperties.reset();
    mLayerStack.reset();
    mGraphicsScene.reset();
    throw;  // ...and rethrow the exception
  }
}

Board::~Board() noexcept {
  Q_ASSERT(!mIsAddedToProject);

  qDeleteAll(mErcMsgListUnplacedComponentInstances);
  mErcMsgListUnplacedComponentInstances.clear();

  // delete all items
  qDeleteAll(mAirWires);
  mAirWires.clear();
  qDeleteAll(mHoles);
  mHoles.clear();
  qDeleteAll(mStrokeTexts);
  mStrokeTexts.clear();
  qDeleteAll(mPolygons);
  mPolygons.clear();
  qDeleteAll(mPlanes);
  mPlanes.clear();
  qDeleteAll(mNetSegments);
  mNetSegments.clear();
  qDeleteAll(mDeviceInstances);
  mDeviceInstances.clear();

  mFabricationOutputSettings.reset();
  mDesignRules.reset();
  mGridProperties.reset();
  mLayerStack.reset();
  mGraphicsScene.reset();
}

/*******************************************************************************
 *  Getters: General
 ******************************************************************************/

bool Board::isEmpty() const noexcept {
  return (mDeviceInstances.isEmpty() && mNetSegments.isEmpty() &&
          mPlanes.isEmpty() && mPolygons.isEmpty() && mStrokeTexts.isEmpty() &&
          mHoles.isEmpty());
}

QList<BI_NetPoint*> Board::getNetPointsAtScenePos(
    const Point& pos, const GraphicsLayer* layer,
    const QSet<const NetSignal*>& netsignals) const noexcept {
  QList<BI_NetPoint*> list;
  foreach (BI_NetSegment* segment, mNetSegments) {
    if (netsignals.isEmpty() || netsignals.contains(segment->getNetSignal())) {
      segment->getNetPointsAtScenePos(pos, layer, list);
    }
  }
  return list;
}

QList<BI_NetLine*> Board::getNetLinesAtScenePos(
    const Point& pos, const GraphicsLayer* layer,
    const QSet<const NetSignal*>& netsignals) const noexcept {
  QList<BI_NetLine*> list;
  foreach (BI_NetSegment* segment, mNetSegments) {
    if (netsignals.isEmpty() || netsignals.contains(segment->getNetSignal())) {
      segment->getNetLinesAtScenePos(pos, layer, list);
    }
  }
  return list;
}

QList<BI_Base*> Board::getAllItems() const noexcept {
  QList<BI_Base*> items;
  foreach (BI_Device* device, mDeviceInstances)
    items.append(device);
  foreach (BI_NetSegment* netsegment, mNetSegments)
    items.append(netsegment);
  foreach (BI_Plane* plane, mPlanes)
    items.append(plane);
  foreach (BI_Polygon* polygon, mPolygons)
    items.append(polygon);
  foreach (BI_StrokeText* text, mStrokeTexts)
    items.append(text);
  foreach (BI_Hole* hole, mHoles)
    items.append(hole);
  foreach (BI_AirWire* airWire, mAirWires)
    items.append(airWire);
  return items;
}

/*******************************************************************************
 *  Setters: General
 ******************************************************************************/

void Board::setGridProperties(const GridProperties& grid) noexcept {
  *mGridProperties = grid;
}

/*******************************************************************************
 *  DeviceInstance Methods
 ******************************************************************************/

BI_Device* Board::getDeviceInstanceByComponentUuid(const Uuid& uuid) const
    noexcept {
  return mDeviceInstances.value(uuid, nullptr);
}

void Board::addDeviceInstance(BI_Device& instance) {
  if (&instance.getBoard() != this) {
    throw LogicError(__FILE__, __LINE__);
  }
  // check if there is no device with the same component instance in the list
  if (getDeviceInstanceByComponentUuid(
          instance.getComponentInstance().getUuid())) {
    throw RuntimeError(
        __FILE__, __LINE__,
        QString("There is already a device with the component instance \"%1\"!")
            .arg(instance.getComponentInstance().getUuid().toStr()));
  }
  if (mIsAddedToProject) {
    instance.addToBoard();  // can throw
  }
  mDeviceInstances.insert(instance.getComponentInstanceUuid(), &instance);
  updateErcMessages();
  emit deviceAdded(instance);
}

void Board::removeDeviceInstance(BI_Device& instance) {
  if (!mDeviceInstances.contains(instance.getComponentInstanceUuid())) {
    throw LogicError(__FILE__, __LINE__);
  }
  if (mIsAddedToProject) {
    instance.removeFromBoard();  // can throw
  }
  mDeviceInstances.remove(instance.getComponentInstanceUuid());
  updateErcMessages();
  emit deviceRemoved(instance);
}

/*******************************************************************************
 *  NetSegment Methods
 ******************************************************************************/

BI_NetSegment* Board::getNetSegmentByUuid(const Uuid& uuid) const noexcept {
  foreach (BI_NetSegment* netsegment, mNetSegments) {
    if (netsegment->getUuid() == uuid) return netsegment;
  }
  return nullptr;
}

void Board::addNetSegment(BI_NetSegment& netsegment) {
  if ((mNetSegments.contains(&netsegment)) ||
      (&netsegment.getBoard() != this)) {
    throw LogicError(__FILE__, __LINE__);
  }
  // check if there is no netsegment with the same uuid in the list
  if (getNetSegmentByUuid(netsegment.getUuid())) {
    throw RuntimeError(
        __FILE__, __LINE__,
        QString("There is already a netsegment with the UUID \"%1\"!")
            .arg(netsegment.getUuid().toStr()));
  }
  if (mIsAddedToProject) {
    netsegment.addToBoard();  // can throw
  }
  mNetSegments.append(&netsegment);
}

void Board::removeNetSegment(BI_NetSegment& netsegment) {
  if (!mNetSegments.contains(&netsegment)) {
    throw LogicError(__FILE__, __LINE__);
  }
  if (mIsAddedToProject) {
    netsegment.removeFromBoard();  // can throw
  }
  mNetSegments.removeOne(&netsegment);
}

/*******************************************************************************
 *  Plane Methods
 ******************************************************************************/

BI_Plane* Board::getPlaneByUuid(const Uuid& uuid) const noexcept {
  foreach (BI_Plane* plane, mPlanes) {
    if (plane->getUuid() == uuid) return plane;
  }
  return nullptr;
}

void Board::addPlane(BI_Plane& plane) {
  if ((mPlanes.contains(&plane)) || (&plane.getBoard() != this)) {
    throw LogicError(__FILE__, __LINE__);
  }
  if (mIsAddedToProject) {
    plane.addToBoard();  // can throw
  }
  mPlanes.append(&plane);
}

void Board::removePlane(BI_Plane& plane) {
  if (!mPlanes.contains(&plane)) {
    throw LogicError(__FILE__, __LINE__);
  }
  if (mIsAddedToProject) {
    plane.removeFromBoard();  // can throw
  }
  mPlanes.removeOne(&plane);
}

void Board::rebuildAllPlanes() noexcept {
  QList<BI_Plane*> planes = mPlanes;
  std::sort(planes.begin(), planes.end(),
            [](const BI_Plane* p1, const BI_Plane* p2) {
              return !(*p1 < *p2);
            });  // sort by priority (highest priority first)
  foreach (BI_Plane* plane, planes) {
    BoardPlaneFragmentsBuilder builder(*plane);
    plane->setCalculatedFragments(builder.buildFragments());
  }
}

/*******************************************************************************
 *  Polygon Methods
 ******************************************************************************/

void Board::addPolygon(BI_Polygon& polygon) {
  if ((mPolygons.contains(&polygon)) || (&polygon.getBoard() != this)) {
    throw LogicError(__FILE__, __LINE__);
  }
  if (mIsAddedToProject) {
    polygon.addToBoard();  // can throw
  }
  mPolygons.append(&polygon);
}

void Board::removePolygon(BI_Polygon& polygon) {
  if (!mPolygons.contains(&polygon)) {
    throw LogicError(__FILE__, __LINE__);
  }
  if (mIsAddedToProject) {
    polygon.removeFromBoard();  // can throw
  }
  mPolygons.removeOne(&polygon);
}

/*******************************************************************************
 *  StrokeText Methods
 ******************************************************************************/

void Board::addStrokeText(BI_StrokeText& text) {
  if ((mStrokeTexts.contains(&text)) || (&text.getBoard() != this)) {
    throw LogicError(__FILE__, __LINE__);
  }
  if (mIsAddedToProject) {
    text.addToBoard();  // can throw
  }
  mStrokeTexts.append(&text);
}

void Board::removeStrokeText(BI_StrokeText& text) {
  if (!mStrokeTexts.contains(&text)) {
    throw LogicError(__FILE__, __LINE__);
  }
  if (mIsAddedToProject) {
    text.removeFromBoard();  // can throw
  }
  mStrokeTexts.removeOne(&text);
}

/*******************************************************************************
 *  Hole Methods
 ******************************************************************************/

void Board::addHole(BI_Hole& hole) {
  if ((mHoles.contains(&hole)) || (&hole.getBoard() != this)) {
    throw LogicError(__FILE__, __LINE__);
  }
  if (mIsAddedToProject) {
    hole.addToBoard();  // can throw
  }
  mHoles.append(&hole);
}

void Board::removeHole(BI_Hole& hole) {
  if (!mHoles.contains(&hole)) {
    throw LogicError(__FILE__, __LINE__);
  }
  if (mIsAddedToProject) {
    hole.removeFromBoard();  // can throw
  }
  mHoles.removeOne(&hole);
}

/*******************************************************************************
 *  AirWire Methods
 ******************************************************************************/

void Board::triggerAirWiresRebuild() noexcept {
  if (!mIsAddedToProject) {
    return;
  }

  try {
    foreach (NetSignal* netsignal, mScheduledNetSignalsForAirWireRebuild) {
      // remove old airwires
      while (BI_AirWire* airWire = mAirWires.take(netsignal)) {
        airWire->removeFromBoard();  // can throw
        delete airWire;
      }

      if (netsignal && netsignal->isAddedToCircuit()) {
        // calculate new airwires
        BoardAirWiresBuilder builder(*this, *netsignal);
        QVector<QPair<Point, Point>> airwires = builder.buildAirWires();

        // add new airwires
        foreach (const auto& points, airwires) {
          QScopedPointer<BI_AirWire> airWire(
              new BI_AirWire(*this, *netsignal, points.first, points.second));
          airWire->addToBoard();  // can throw
          mAirWires.insertMulti(netsignal, airWire.take());
        }
      }
    }
    mScheduledNetSignalsForAirWireRebuild.clear();
  } catch (const std::exception&
               e) {  // std::exception because of the many std containers...
    qCritical() << "Failed to build airwires:" << e.what();
  }
}

void Board::forceAirWiresRebuild() noexcept {
  mScheduledNetSignalsForAirWireRebuild.unite(
      Toolbox::toSet(mProject.getCircuit().getNetSignals().values()));
  mScheduledNetSignalsForAirWireRebuild.unite(Toolbox::toSet(mAirWires.keys()));
  triggerAirWiresRebuild();
}

/*******************************************************************************
 *  General Methods
 ******************************************************************************/

void Board::addDefaultContent() {
  // Add 100x80mm board outline (1/2 Eurocard size).
  addPolygon(*new BI_Polygon(
      *this,
      Polygon(Uuid::createRandom(),
              GraphicsLayerName(GraphicsLayer::sBoardOutlines),
              UnsignedLength(0), false, false,
              Path::rect(Point(0, 0), Point(100000000, 80000000)))));
}

void Board::copyFrom(const Board& other) {
  mDefaultFontFileName = other.getDefaultFontName();
  *mLayerStack = other.getLayerStack();
  *mGridProperties = other.getGridProperties();
  *mDesignRules = other.getDesignRules();
  *mFabricationOutputSettings = other.getFabricationOutputSettings();

  // Copy device instances.
  QHash<const BI_Device*, BI_Device*> devMap;
  foreach (const BI_Device* device, other.getDeviceInstances()) {
    BI_Device* copy = new BI_Device(
        *this, device->getComponentInstance(), device->getLibDevice().getUuid(),
        device->getLibFootprint().getUuid(), device->getPosition(),
        device->getRotation(), device->getMirrored(), false);
    copy->setAttributes(device->getAttributes());
    foreach (const BI_StrokeText* text, device->getStrokeTexts()) {
      copy->addStrokeText(*new BI_StrokeText(*this, text->getText()));
    }
    addDeviceInstance(*copy);
    devMap.insert(device, copy);
  }

  // Copy netsegments.
  foreach (const BI_NetSegment* netSegment, other.getNetSegments()) {
    BI_NetSegment* copy = new BI_NetSegment(*this, netSegment->getNetSignal());

    // Determine new pad anchors.
    QHash<const BI_NetLineAnchor*, BI_NetLineAnchor*> anchorsMap;
    for (auto it = devMap.begin(); it != devMap.end(); ++it) {
      const BI_Device& oldDev = *it.key();
      BI_Device& newDev = *it.value();
      foreach (const BI_FootprintPad* pad, oldDev.getPads()) {
        anchorsMap.insert(pad, newDev.getPad(pad->getLibPadUuid()));
      }
    }

    // Copy vias.
    QList<BI_Via*> vias;
    foreach (const BI_Via* via, netSegment->getVias()) {
      BI_Via* viaCopy =
          new BI_Via(*copy, Via(Uuid::createRandom(), via->getVia()));
      vias.append(viaCopy);
      anchorsMap.insert(via, viaCopy);
    }

    // Copy netpoints.
    QList<BI_NetPoint*> netPoints;
    foreach (const BI_NetPoint* netPoint, netSegment->getNetPoints()) {
      BI_NetPoint* netPointCopy =
          new BI_NetPoint(*copy, netPoint->getPosition());
      netPoints.append(netPointCopy);
      anchorsMap.insert(netPoint, netPointCopy);
    }

    // Copy netlines.
    QList<BI_NetLine*> netLines;
    foreach (const BI_NetLine* netLine, netSegment->getNetLines()) {
      BI_NetLineAnchor* start = anchorsMap.value(&netLine->getStartPoint());
      Q_ASSERT(start);
      BI_NetLineAnchor* end = anchorsMap.value(&netLine->getEndPoint());
      Q_ASSERT(end);
      GraphicsLayer* layer =
          getLayerStack().getLayer(netLine->getLayer().getName());
      if (!layer) {
        throw LogicError(__FILE__, __LINE__);
      }
      BI_NetLine* netLineCopy =
          new BI_NetLine(*copy, *start, *end, *layer, netLine->getWidth());
      netLines.append(netLineCopy);
    }

    copy->addElements(vias, netPoints, netLines);
    addNetSegment(*copy);
  }

  // Copy planes.
  foreach (const BI_Plane* plane, other.getPlanes()) {
    BI_Plane* copy =
        new BI_Plane(*this, Uuid::createRandom(), plane->getLayerName(),
                     plane->getNetSignal(), plane->getOutline());
    copy->setMinWidth(plane->getMinWidth());
    copy->setMinClearance(plane->getMinClearance());
    copy->setKeepOrphans(plane->getKeepOrphans());
    copy->setPriority(plane->getPriority());
    copy->setConnectStyle(plane->getConnectStyle());
    copy->setVisible(plane->isVisible());
    copy->setCalculatedFragments(plane->getFragments());
    addPlane(*copy);
  }

  // Copy polygons.
  foreach (const BI_Polygon* polygon, other.getPolygons()) {
    BI_Polygon* copy = new BI_Polygon(
        *this, Polygon(Uuid::createRandom(), polygon->getPolygon()));
    addPolygon(*copy);
  }

  // Copy stroke texts.
  foreach (const BI_StrokeText* text, other.getStrokeTexts()) {
    BI_StrokeText* copy = new BI_StrokeText(
        *this, StrokeText(Uuid::createRandom(), text->getText()));
    addStrokeText(*copy);
  }

  // Copy holes.
  foreach (const BI_Hole* hole, other.getHoles()) {
    BI_Hole* copy =
        new BI_Hole(*this, Hole(Uuid::createRandom(), hole->getHole()));
    addHole(*copy);
  }
}

void Board::addToProject() {
  if (mIsAddedToProject) {
    throw LogicError(__FILE__, __LINE__);
  }

  QList<BI_Base*> items = getAllItems();
  ScopeGuardList sgl(items.count());
  for (int i = 0; i < items.count(); ++i) {
    BI_Base* item = items.at(i);
    item->addToBoard();  // can throw
    sgl.add([item]() { item->removeFromBoard(); });
  }

  // Move directory atomically (last step which could throw an exception).
  if (mDirectory->getFileSystem() != mProject.getDirectory().getFileSystem()) {
    TransactionalDirectory dst(mProject.getDirectory(),
                               "boards/" % mDirectoryName);
    mDirectory->moveTo(dst);  // can throw
  }

  mIsAddedToProject = true;
  forceAirWiresRebuild();
  updateErcMessages();
  sgl.dismiss();
}

void Board::removeFromProject() {
  if (!mIsAddedToProject) {
    throw LogicError(__FILE__, __LINE__);
  }

  QList<BI_Base*> items = getAllItems();
  ScopeGuardList sgl(items.count());
  for (int i = items.count() - 1; i >= 0; --i) {
    BI_Base* item = items.at(i);
    item->removeFromBoard();  // can throw
    sgl.add([item]() { item->addToBoard(); });
  }

  // Move directory atomically (last step which could throw an exception).
  TransactionalDirectory tmp;
  mDirectory->moveTo(tmp);  // can throw

  mIsAddedToProject = false;
  updateErcMessages();
  sgl.dismiss();
}

void Board::save() {
  // save board file
  {
    SExpression root(serializeToDomElement("librepcb_board"));  // can throw
    mDirectory->write("board.lp",
                      root.toByteArray());  // can throw
  }

  // save user settings
  {
    SExpression root = SExpression::createList("librepcb_board_user_settings");
    for (const GraphicsLayer* layer : mLayerStack->getAllLayers()) {
      root.ensureLineBreak();
      SExpression& child = root.appendList("layer");
      child.appendChild(SExpression::createToken(layer->getName()));
      child.appendChild("color", layer->getColor(false));
      child.appendChild("color_hl", layer->getColor(true));
      child.appendChild("visible", layer->getVisible());
    }
    root.ensureLineBreak();
    foreach (BI_Plane* plane, mPlanes) {
      root.ensureLineBreak();
      SExpression node = SExpression::createList("plane");
      node.appendChild(plane->getUuid());
      node.appendChild("visible", plane->isVisible());
      root.appendChild(node);
    }
    root.ensureLineBreak();
    mDirectory->write("settings.user.lp", root.toByteArray());  // can throw
  }
}

void Board::selectAll() noexcept {
  foreach (BI_Device* device, mDeviceInstances) {
    device->setSelected(device->isSelectable());
  }
  foreach (BI_NetSegment* segment, mNetSegments) { segment->selectAll(); }
  foreach (BI_Plane* plane, mPlanes) {
    plane->setSelected(plane->isSelectable());
  }
  foreach (BI_Polygon* polygon, mPolygons) {
    polygon->setSelected(polygon->isSelectable());
  }
  foreach (BI_StrokeText* text, mStrokeTexts) {
    text->setSelected(text->isSelectable());
  }
  foreach (BI_Hole* hole, mHoles) { hole->setSelected(hole->isSelectable()); }
}

void Board::setSelectionRect(const Point& p1, const Point& p2,
                             bool updateItems) noexcept {
  mGraphicsScene->setSelectionRect(p1, p2);
  if (updateItems) {
    QRectF rectPx = QRectF(p1.toPxQPointF(), p2.toPxQPointF()).normalized();
    foreach (BI_Device* device, mDeviceInstances) {
      bool selectDevice = device->isSelectable() &&
          device->getGrabAreaScenePx().intersects(rectPx);
      device->setSelected(selectDevice);
      foreach (BI_FootprintPad* pad, device->getPads()) {
        bool selectPad =
            pad->isSelectable() && pad->getGrabAreaScenePx().intersects(rectPx);
        pad->setSelected(selectDevice || selectPad);
      }
      foreach (BI_StrokeText* text, device->getStrokeTexts()) {
        bool selectText = text->isSelectable() &&
            text->getGrabAreaScenePx().intersects(rectPx);
        text->setSelected(selectDevice || selectText);
      }
    }
    foreach (BI_NetSegment* segment, mNetSegments) {
      segment->setSelectionRect(rectPx);
    }
    foreach (BI_Plane* plane, mPlanes) {
      bool select = plane->isSelectable() &&
          plane->getGrabAreaScenePx().intersects(rectPx);
      plane->setSelected(select);
    }
    foreach (BI_Polygon* polygon, mPolygons) {
      bool select = polygon->isSelectable() &&
          polygon->getGrabAreaScenePx().intersects(rectPx);
      polygon->setSelected(select);
    }
    foreach (BI_StrokeText* text, mStrokeTexts) {
      bool select =
          text->isSelectable() && text->getGrabAreaScenePx().intersects(rectPx);
      text->setSelected(select);
    }
    foreach (BI_Hole* hole, mHoles) {
      bool select =
          hole->isSelectable() && hole->getGrabAreaScenePx().intersects(rectPx);
      hole->setSelected(select);
    }
  }
}

void Board::clearSelection() const noexcept {
  foreach (BI_Device* device, mDeviceInstances)
    device->setSelected(false);
  foreach (BI_NetSegment* segment, mNetSegments) { segment->clearSelection(); }
  foreach (BI_Plane* plane, mPlanes) { plane->setSelected(false); }
  foreach (BI_Polygon* polygon, mPolygons) { polygon->setSelected(false); }
  foreach (BI_StrokeText* text, mStrokeTexts) { text->setSelected(false); }
  foreach (BI_Hole* hole, mHoles) { hole->setSelected(false); }
}

std::unique_ptr<BoardSelectionQuery> Board::createSelectionQuery() const
    noexcept {
  return std::unique_ptr<BoardSelectionQuery>(new BoardSelectionQuery(
      mDeviceInstances, mNetSegments, mPlanes, mPolygons, mStrokeTexts, mHoles,
      const_cast<Board*>(this)));
}

/*******************************************************************************
 *  Inherited from AttributeProvider
 ******************************************************************************/

QString Board::getBuiltInAttributeValue(const QString& key) const noexcept {
  if (key == QLatin1String("BOARD")) {
    return *mName;
  } else if (key == QLatin1String("BOARD_DIRNAME")) {
    return mDirectory->getPath().split("/").last();
  } else if (key == QLatin1String("BOARD_INDEX")) {
    return QString::number(mProject.getBoardIndex(*this));
  } else {
    return QString();
  }
}

QVector<const AttributeProvider*> Board::getAttributeProviderParents() const
    noexcept {
  return QVector<const AttributeProvider*>{&mProject};
}

/*******************************************************************************
 *  Private Methods
 ******************************************************************************/

void Board::updateIcon() noexcept {
  mIcon = QIcon(mGraphicsScene->toPixmap(QSize(297, 210), Qt::white));
}

void Board::serialize(SExpression& root) const {
  root.appendChild(mUuid);
  root.ensureLineBreak();
  root.appendChild("name", mName);
  root.ensureLineBreak();
  root.appendChild("default_font", mDefaultFontFileName);
  root.ensureLineBreak();
  root.appendChild(mGridProperties->serializeToDomElement("grid"));
  root.ensureLineBreak();
  root.appendChild(mLayerStack->serializeToDomElement("layers"));
  root.ensureLineBreak();
  root.appendChild(mDesignRules->serializeToDomElement("design_rules"));
  root.ensureLineBreak();
  root.appendChild(mFabricationOutputSettings->serializeToDomElement(
      "fabrication_output_settings"));
  root.ensureLineBreak();
  serializePointerContainer(root, mDeviceInstances, "device");
  root.ensureLineBreak();
  serializePointerContainerUuidSorted(root, mNetSegments, "netsegment");
  root.ensureLineBreak();
  serializePointerContainerUuidSorted(root, mPlanes, "plane");
  root.ensureLineBreak();
  serializePointerContainerUuidSorted(root, mPolygons, "polygon");
  root.ensureLineBreak();
  serializePointerContainerUuidSorted(root, mStrokeTexts, "stroke_text");
  root.ensureLineBreak();
  serializePointerContainerUuidSorted(root, mHoles, "hole");
  root.ensureLineBreak();
}

void Board::updateErcMessages() noexcept {
  // type: UnplacedComponent (ComponentInstances without DeviceInstance)
  if (mIsAddedToProject) {
    const QMap<Uuid, ComponentInstance*>& componentInstances =
        mProject.getCircuit().getComponentInstances();
    foreach (const ComponentInstance* component, componentInstances) {
      if (component->getLibComponent().isSchematicOnly()) continue;
      BI_Device* device = mDeviceInstances.value(component->getUuid());
      ErcMsg* ercMsg =
          mErcMsgListUnplacedComponentInstances.value(component->getUuid());
      if ((!device) && (!ercMsg)) {
        ErcMsg* ercMsg = new ErcMsg(
            mProject, *this,
            QString("%1/%2").arg(mUuid.toStr(), component->getUuid().toStr()),
            "UnplacedComponent", ErcMsg::ErcMsgType_t::BoardError,
            QString("Unplaced Component: %1 (Board: %2)")
                .arg(*component->getName(), *mName));
        ercMsg->setVisible(true);
        mErcMsgListUnplacedComponentInstances.insert(component->getUuid(),
                                                     ercMsg);
      } else if ((device) && (ercMsg)) {
        delete mErcMsgListUnplacedComponentInstances.take(component->getUuid());
      }
    }
    foreach (const Uuid& uuid, mErcMsgListUnplacedComponentInstances.keys()) {
      if (!componentInstances.contains(uuid))
        delete mErcMsgListUnplacedComponentInstances.take(uuid);
    }
  } else {
    qDeleteAll(mErcMsgListUnplacedComponentInstances);
    mErcMsgListUnplacedComponentInstances.clear();
  }
}

/*******************************************************************************
 *  Static Methods
 ******************************************************************************/

Board* Board::create(Project& project,
                     std::unique_ptr<TransactionalDirectory> directory,
                     const QString& directoryName, const ElementName& name) {
  return new Board(project, std::move(directory), directoryName,
                   qApp->getFileFormatVersion(), true, *name);
}

/*******************************************************************************
 *  End of File
 ******************************************************************************/

}  // namespace librepcb
