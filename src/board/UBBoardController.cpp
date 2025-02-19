/*
 * Copyright (C) 2015-2022 Département de l'Instruction Publique (DIP-SEM)
 *
 * Copyright (C) 2013 Open Education Foundation
 *
 * Copyright (C) 2010-2013 Groupement d'Intérêt Public pour
 * l'Education Numérique en Afrique (GIP ENA)
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




#include "UBBoardController.h"

#include <QtWidgets>

#include "frameworks/UBFileSystemUtils.h"
#include "frameworks/UBPlatformUtils.h"

#include "core/UBApplication.h"
#include "core/UBDisplayManager.h"
#include "core/UBSettings.h"
#include "core/UBSetting.h"
#include "core/UBPersistenceManager.h"
#include "core/UBApplicationController.h"
#include "core/UBDocumentManager.h"
#include "core/UBMimeData.h"
#include "core/UBDownloadManager.h"

#include "gui/UBMessageWindow.h"
#include "gui/UBToolbarButtonGroup.h"
#include "gui/UBMainWindow.h"
#include "gui/UBToolWidget.h"
#include "gui/UBKeyboardPalette.h"
#include "gui/UBMagnifer.h"
#include "gui/UBDockPaletteWidget.h"

#include "domain/UBGraphicsPixmapItem.h"
#include "domain/UBGraphicsItemUndoCommand.h"
#include "domain/UBGraphicsSvgItem.h"
#include "domain/UBGraphicsWidgetItem.h"
#include "domain/UBGraphicsMediaItem.h"
#include "domain/UBGraphicsPDFItem.h"
#include "domain/UBGraphicsTextItem.h"
#include "domain/UBPageSizeUndoCommand.h"
#include "domain/UBGraphicsGroupContainerItem.h"
#include "domain/UBGraphicsStrokesGroup.h"
#include "domain/UBItem.h"
#include "board/UBFeaturesController.h"

#include "gui/UBFeaturesWidget.h"

#include "tools/UBToolsManager.h"

#include "document/UBDocumentProxy.h"
#include "document/UBDocumentController.h"

#include "board/UBDrawingController.h"
#include "board/UBBoardView.h"

#include "podcast/UBPodcastController.h"

#include "adaptors/UBMetadataDcSubsetAdaptor.h"
#include "adaptors/UBSvgSubsetAdaptor.h"

#include "UBBoardPaletteManager.h"

#include "core/UBSettings.h"

#include "web/UBEmbedController.h"
#include "web/UBEmbedParser.h"

#include "core/memcheck.h"

UBBoardController::UBBoardController(UBMainWindow* mainWindow)
    : UBDocumentContainer(mainWindow->centralWidget())
    , mMainWindow(mainWindow)
    , mActiveScene(0)
    , mActiveSceneIndex(-1)
    , mPaletteManager(0)
    , mSoftwareUpdateDialog(0)
    , mMessageWindow(0)
    , mEmbedController(nullptr)
    , mControlView(0)
    , mDisplayView(0)
    , mControlContainer(0)
    , mControlLayout(0)
    , mZoomFactor(1.0)
    , mIsClosing(false)
    , mSystemScaleFactor(1.0)
    , mCleanupDone(false)
    , mCacheWidgetIsEnabled(false)
    , mDeletingSceneIndex(-1)
    , mMovingSceneIndex(-1)
    , mActionGroupText(tr("Group"))
    , mActionUngroupText(tr("Ungroup"))
    , mAutosaveTimer(0)
{
    mZoomFactor = UBSettings::settings()->boardZoomFactor->get().toDouble();

    int penColorIndex = UBSettings::settings()->penColorIndex();
    int markerColorIndex = UBSettings::settings()->markerColorIndex();

    mPenColorOnDarkBackground = UBSettings::settings()->penColors(true).at(penColorIndex);
    mPenColorOnLightBackground = UBSettings::settings()->penColors(false).at(penColorIndex);
    mMarkerColorOnDarkBackground = UBSettings::settings()->markerColors(true).at(markerColorIndex);
    mMarkerColorOnLightBackground = UBSettings::settings()->markerColors(false).at(markerColorIndex);

}


void UBBoardController::init()
{
    setupViews();
    setupToolbar();

    connect(UBApplication::undoStack, SIGNAL(canUndoChanged(bool))
            , this, SLOT(undoRedoStateChange(bool)));

    connect(UBApplication::undoStack, SIGNAL(canRedoChanged(bool))
            , this, SLOT(undoRedoStateChange(bool)));

    connect(UBDrawingController::drawingController(), SIGNAL(stylusToolChanged(int))
            , this, SLOT(setToolCursor(int)));

    connect(UBDrawingController::drawingController(), SIGNAL(stylusToolChanged(int))
            , this, SLOT(stylusToolChanged(int)));

    connect(UBApplication::app(), SIGNAL(lastWindowClosed())
            , this, SLOT(lastWindowClosed()));

    connect(UBDownloadManager::downloadManager(), SIGNAL(downloadModalFinished()), this, SLOT(onDownloadModalFinished()));
    connect(UBDownloadManager::downloadManager(), SIGNAL(addDownloadedFileToBoard(bool,QUrl,QUrl,QString,QByteArray,QPointF,QSize,bool)), this, SLOT(downloadFinished(bool,QUrl,QUrl,QString,QByteArray,QPointF,QSize,bool)));

    std::shared_ptr<UBDocumentProxy> doc = UBPersistenceManager::persistenceManager()->createNewDocument();

    if (doc)
    {
        mInitialDocumentScene = setActiveDocumentScene(doc);
    }

    connect(UBApplication::displayManager, &UBDisplayManager::screenRolesAssigned, this, [this](){
        initBackgroundGridSize();
    });

    undoRedoStateChange(true);

}


UBBoardController::~UBBoardController()
{
    delete mDisplayView;
}

/**
 * @brief Set the default background grid size to appear as roughly 1cm on screen
 */
void UBBoardController::initBackgroundGridSize()
{
    // Besides adjusting for DPI, we also need to scale the grid size by the ratio of the control view size
    // to document size. Here we approximate this ratio as (document resolution) / (screen resolution).
    // Later on, this is calculated by `updateSystemScaleFactor` and stored in `mSystemScaleFactor`.

    qreal dpi = UBApplication::displayManager->logicalDpi(ScreenRole::Control);

    //qDebug() << "dpi: " << dpi;

    qreal screenY = UBApplication::displayManager->screenSize(ScreenRole::Control).height();
    qreal documentY = mActiveScene->nominalSize().height();
    qreal resolutionRatio = documentY / screenY;

    //qDebug() << "resolution ratio: " << resolutionRatio;

    int gridSize = (resolutionRatio * 10. * dpi) / UBGeometryUtils::inchSize;

    UBSettings::settings()->crossSize = gridSize;
    UBSettings::settings()->defaultCrossSize = gridSize;
    mActiveScene->setBackgroundGridSize(gridSize);

    //qDebug() << "grid size: " << gridSize;
}

int UBBoardController::currentPage()
{
    return mActiveSceneIndex + 1;
}

void UBBoardController::setupViews()
{
    mControlContainer = new QWidget(mMainWindow->centralWidget());

    mControlLayout = new QHBoxLayout(mControlContainer);
    mControlLayout->setContentsMargins(0, 0, 0, 0);

    mControlView = new UBBoardView(this, mControlContainer, true, false);
    mControlView->setObjectName(CONTROLVIEW_OBJ_NAME);
    mControlView->setInteractive(true);
    mControlView->setMouseTracking(true);

    mControlView->grabGesture(Qt::SwipeGesture);

    mControlView->setTransformationAnchor(QGraphicsView::NoAnchor);

    mControlLayout->addWidget(mControlView);
    mControlContainer->setObjectName("ubBoardControlContainer");
    mMainWindow->addBoardWidget(mControlContainer);

    connect(mControlView, SIGNAL(resized(QResizeEvent*)), this, SLOT(boardViewResized(QResizeEvent*)));

    // TODO UB 4.x Optimization do we have to create the display view even if their is
    // only 1 screen
    //
    mDisplayView = new UBBoardView(this, UBItemLayerType::FixedBackground, UBItemLayerType::Tool, 0);
    mDisplayView->setInteractive(false);
    mDisplayView->setTransformationAnchor(QGraphicsView::NoAnchor);

    mPaletteManager = new UBBoardPaletteManager(mControlContainer, this);

    mMessageWindow = new UBMessageWindow(mControlContainer);
    mMessageWindow->hide();

    connect(this, SIGNAL(activeSceneChanged()), mPaletteManager, SLOT(activeSceneChanged()));
}


void UBBoardController::setupLayout()
{
    if(mPaletteManager)
        mPaletteManager->setupLayout();
}


void UBBoardController::setBoxing(QRect displayRect)
{
    if (displayRect.isNull())
    {
        mControlView->setBoxing({});
        return;
    }

    // compute boxing based on the assumed widget size for fullscreen
    QSize centralWidgetSize = mMainWindow->centralWidget()->size();
    QSize controlWindowSize = mMainWindow->size();
    QSize controlScreenSize = UBApplication::displayManager->screenSize(ScreenRole::Control);
    qreal controlWidth = controlScreenSize.width();
    qreal controlHeight = controlScreenSize.height() - controlWindowSize.height() + centralWidgetSize.height();
    qreal displayWidth = (qreal)displayRect.width();
    qreal displayHeight = (qreal)displayRect.height();

    qreal displayRatio = displayWidth / displayHeight;
    qreal controlRatio = controlWidth / controlHeight;

    if (displayRatio < controlRatio)
    {
        // Pillarboxing
        int boxWidth = (centralWidgetSize.width() - (displayWidth * (controlHeight / displayHeight))) / 2;

        if (boxWidth < 0)
        {
            boxWidth = 0;
        }

        mControlView->setBoxing({boxWidth, 0, boxWidth, 0});
    }
    else if (displayRatio > controlRatio)
    {
        // Letterboxing
        int boxHeight = (centralWidgetSize.height() - (displayHeight * (controlWidth / displayWidth))) / 2;

        if (boxHeight < 0)
        {
            boxHeight = 0;
        }

        mControlView->setBoxing({0, boxHeight, 0, boxHeight});
    }
    else
    {
        // No boxing
        mControlView->setBoxing({});
    }
}

void UBBoardController::setCursorFromAngle(qreal angle, const QPoint offset)
{
        QString displayedAngle = QString::number(angle, 'f', 1);
        QWidget *controlViewport = controlView()->viewport();

        QSize cursorSize(45,30);
        QSize bitmapSize = cursorSize;
        int hotX = -1;
        int hotY = -1;

        if (!offset.isNull())
        {
            bitmapSize.setWidth(std::max(bitmapSize.width(), 2 * std::abs(offset.x())));
            bitmapSize.setHeight(std::max(bitmapSize.height(), 2 * std::abs(offset.y())));
            hotX = bitmapSize.width() / 2 - offset.x();
            hotY = bitmapSize.height() / 2 - offset.y();
        }

        QSize origin = (bitmapSize - cursorSize) / 2;

        QImage mask_img(bitmapSize, QImage::Format_Mono);
        mask_img.fill(0xff);
        QPainter mask_ptr(&mask_img);
        mask_ptr.setBrush( QBrush( QColor(0, 0, 0) ) );
        mask_ptr.drawRoundedRect(origin.width(), origin.height(), cursorSize.width()-1, cursorSize.height()-1, 6, 6);
        QBitmap bmpMask = QBitmap::fromImage(mask_img);


        QPixmap pixCursor(bitmapSize);
        pixCursor.fill(QColor(Qt::white));

        QPainter painter(&pixCursor);

        painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
        painter.setBrush(QBrush(Qt::white));
        painter.setPen(QPen(QColor(Qt::black)));
        painter.drawRoundedRect(origin.width() + 1, origin.height() + 1,cursorSize.width()-2,cursorSize.height()-2,6,6);
        painter.setFont(QFont("Arial", 10));
        painter.drawText(origin.width() + 1, origin.height() + 1,cursorSize.width(),cursorSize.height(), Qt::AlignCenter, displayedAngle.append(QChar(176)));
        painter.end();

        pixCursor.setMask(bmpMask);
        controlViewport->setCursor(QCursor(pixCursor, hotX, hotY));
}


void UBBoardController::setupToolbar()
{
    UBSettings *settings = UBSettings::settings();

    // Setup color choice widget
    QList<QAction *> colorActions;
    colorActions.append(mMainWindow->actionColor0);
    colorActions.append(mMainWindow->actionColor1);
    colorActions.append(mMainWindow->actionColor2);
    colorActions.append(mMainWindow->actionColor3);
    colorActions.append(mMainWindow->actionColor4);

    UBToolbarButtonGroup *colorChoice =
            new UBToolbarButtonGroup(mMainWindow->boardToolBar, colorActions);
    colorChoice->setLabel(tr("Color"));

    mMainWindow->boardToolBar->insertWidget(mMainWindow->actionBackgrounds, colorChoice);

    connect(settings->appToolBarDisplayText, SIGNAL(changed(QVariant)), colorChoice, SLOT(displayText(QVariant)));
    connect(colorChoice, SIGNAL(activated(int)), this, SLOT(setColorIndex(int)));
    connect(UBDrawingController::drawingController(), SIGNAL(colorIndexChanged(int)), colorChoice, SLOT(setCurrentIndex(int)));
    connect(UBDrawingController::drawingController(), SIGNAL(colorIndexChanged(int)), UBDrawingController::drawingController(), SIGNAL(colorPaletteChanged()));
    connect(UBDrawingController::drawingController(), SIGNAL(colorPaletteChanged()), colorChoice, SLOT(colorPaletteChanged()));
    connect(UBDrawingController::drawingController(), SIGNAL(colorPaletteChanged()), this, SLOT(colorPaletteChanged()));

    colorChoice->displayText(QVariant(settings->appToolBarDisplayText->get().toBool()));
    colorChoice->colorPaletteChanged();
    colorChoice->setCurrentIndex(settings->penColorIndex());
    colorActions.at(settings->penColorIndex())->setChecked(true);

    // Setup line width choice widget
    QList<QAction *> lineWidthActions;
    lineWidthActions.append(mMainWindow->actionLineSmall);
    lineWidthActions.append(mMainWindow->actionLineMedium);
    lineWidthActions.append(mMainWindow->actionLineLarge);

    UBToolbarButtonGroup *lineWidthChoice =
            new UBToolbarButtonGroup(mMainWindow->boardToolBar, lineWidthActions);

    connect(settings->appToolBarDisplayText, SIGNAL(changed(QVariant)), lineWidthChoice, SLOT(displayText(QVariant)));

    connect(lineWidthChoice, SIGNAL(activated(int))
            , UBDrawingController::drawingController(), SLOT(setLineWidthIndex(int)));

    connect(UBDrawingController::drawingController(), SIGNAL(lineWidthIndexChanged(int))
            , lineWidthChoice, SLOT(setCurrentIndex(int)));

    lineWidthChoice->displayText(QVariant(settings->appToolBarDisplayText->get().toBool()));
    lineWidthChoice->setCurrentIndex(settings->penWidthIndex());
    lineWidthActions.at(settings->penWidthIndex())->setChecked(true);

    mMainWindow->boardToolBar->insertWidget(mMainWindow->actionBackgrounds, lineWidthChoice);

    //-----------------------------------------------------------//
    // Setup eraser width choice widget

    QList<QAction *> eraserWidthActions;
    eraserWidthActions.append(mMainWindow->actionEraserSmall);
    eraserWidthActions.append(mMainWindow->actionEraserMedium);
    eraserWidthActions.append(mMainWindow->actionEraserLarge);

    UBToolbarButtonGroup *eraserWidthChoice =
            new UBToolbarButtonGroup(mMainWindow->boardToolBar, eraserWidthActions);

    mMainWindow->boardToolBar->insertWidget(mMainWindow->actionBackgrounds, eraserWidthChoice);

    connect(settings->appToolBarDisplayText, SIGNAL(changed(QVariant)), eraserWidthChoice, SLOT(displayText(QVariant)));
    connect(eraserWidthChoice, SIGNAL(activated(int)), UBDrawingController::drawingController(), SLOT(setEraserWidthIndex(int)));

    eraserWidthChoice->displayText(QVariant(settings->appToolBarDisplayText->get().toBool()));
    eraserWidthChoice->setCurrentIndex(settings->eraserWidthIndex());
    eraserWidthActions.at(settings->eraserWidthIndex())->setChecked(true);

    mMainWindow->boardToolBar->insertSeparator(mMainWindow->actionBackgrounds);

    //-----------------------------------------------------------//

    UBApplication::app()->insertSpaceToToolbarBeforeAction(mMainWindow->boardToolBar, mMainWindow->actionBoard);

    UBApplication::app()->decorateActionMenu(mMainWindow->actionMenu);

    mMainWindow->actionBoard->setVisible(false);

    mMainWindow->webToolBar->hide();
    mMainWindow->documentToolBar->hide();

    connectToolbar();
    initToolbarTexts();

    UBApplication::app()->toolBarDisplayTextChanged(QVariant(settings->appToolBarDisplayText->get().toBool()));
}


void UBBoardController::setToolCursor(int tool)
{
    if (mActiveScene)
        mActiveScene->setToolCursor(tool);

    mControlView->setToolCursor(tool);
}


void UBBoardController::connectToolbar()
{
    connect(mMainWindow->actionAdd, SIGNAL(triggered()), this, SLOT(addItem()));
    connect(mMainWindow->actionNewPage, SIGNAL(triggered()), this, SLOT(addScene()));
    connect(mMainWindow->actionDuplicatePage, SIGNAL(triggered()), this, SLOT(duplicateScene()));

    connect(mMainWindow->actionClearPage, SIGNAL(triggered()), this, SLOT(clearScene()));
    connect(mMainWindow->actionEraseItems, SIGNAL(triggered()), this, SLOT(clearSceneItems()));
    connect(mMainWindow->actionEraseAnnotations, SIGNAL(triggered()), this, SLOT(clearSceneAnnotation()));
    connect(mMainWindow->actionEraseBackground,SIGNAL(triggered()),this,SLOT(clearSceneBackground()));

    connect(mMainWindow->actionUndo, SIGNAL(triggered()), UBApplication::undoStack, SLOT(undo()));
    connect(mMainWindow->actionRedo, SIGNAL(triggered()), UBApplication::undoStack, SLOT(redo()));
    connect(mMainWindow->actionRedo, SIGNAL(triggered()), this, SLOT(startScript()));
    connect(mMainWindow->actionBack, SIGNAL( triggered()), this, SLOT(previousScene()));
    connect(mMainWindow->actionForward, SIGNAL(triggered()), this, SLOT(nextScene()));
    connect(mMainWindow->actionSleep, SIGNAL(triggered()), this, SLOT(stopScript()));
    connect(mMainWindow->actionSleep, SIGNAL(triggered()), this, SLOT(blackout()));
    connect(mMainWindow->actionVirtualKeyboard, SIGNAL(triggered(bool)), this, SLOT(showKeyboard(bool)));
    connect(mMainWindow->actionImportPage, SIGNAL(triggered()), this, SLOT(importPage()));
}

void UBBoardController::startScript()
{
    freezeW3CWidgets(false);
}

void UBBoardController::stopScript()
{
    freezeW3CWidgets(true);
}

void UBBoardController::saveData(SaveFlags fls)
{
    bool verbose = fls | sf_showProgress;
    if (verbose) {
        UBApplication::showMessage(tr("Saving document..."));
    }
    if (mActiveScene && mActiveScene->isModified()) {
        persistCurrentScene(true);
    }
    if (verbose) {
        UBApplication::showMessage(tr("Document has just been saved..."));
    }
}

void UBBoardController::initToolbarTexts()
{
    QList<QAction*> allToolbarActions;

    allToolbarActions << mMainWindow->boardToolBar->actions();
    allToolbarActions << mMainWindow->webToolBar->actions();
    allToolbarActions << mMainWindow->documentToolBar->actions();

    foreach(QAction* action, allToolbarActions)
    {
        QString nominalText = action->text();
        QString shortText = truncate(nominalText, 48);
        QPair<QString, QString> texts(nominalText, shortText);

        mActionTexts.insert(action, texts);
    }
}


void UBBoardController::setToolbarTexts()
{
    QSize iconSize;

    if (mMainWindow->width() <= 1280)
        iconSize = QSize(24, 24);
    else
        iconSize = QSize(48, 32);

    mMainWindow->boardToolBar->setIconSize(iconSize);
    mMainWindow->webToolBar->setIconSize(iconSize);
    mMainWindow->documentToolBar->setIconSize(iconSize);

    foreach(QAction* action, mActionTexts.keys())
    {
        QPair<QString, QString> texts = mActionTexts.value(action);

        if (mMainWindow->width() <= 1024)
            action->setText(texts.second);
        else
            action->setText(texts.first);

        action->setToolTip(texts.first);
    }
}


QString UBBoardController::truncate(QString text, int maxWidth)
{
    QFontMetricsF fontMetrics(mMainWindow->font());
    return fontMetrics.elidedText(text, Qt::ElideRight, maxWidth);
}


void UBBoardController::stylusToolDoubleClicked(int tool)
{
    if (tool == UBStylusTool::ZoomIn || tool == UBStylusTool::ZoomOut)
    {
        zoomRestore();
    }
    else if (tool == UBStylusTool::Hand)
    {
        centerRestore();
        mActiveScene->setLastCenter(QPointF(0,0));
    }
}



void UBBoardController::addScene()
{
    QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
    persistViewPositionOnCurrentScene();
    persistCurrentScene(false,true);

    UBPersistenceManager::persistenceManager()->createDocumentSceneAt(selectedDocument(), mActiveSceneIndex+1);
    emit addThumbnailRequired(selectedDocument(), mActiveSceneIndex+1);
    if (UBApplication::documentController->selectedDocument() == selectedDocument())
    {
        UBApplication::documentController->insertThumbPage(mActiveSceneIndex +1);
        UBApplication::documentController->reloadThumbnails();
    }

    QDateTime now = QDateTime::currentDateTime();
    selectedDocument()->setMetaData(UBSettings::documentUpdatedAt, UBStringUtils::toUtcIsoDateTime(now));

    setActiveDocumentScene(mActiveSceneIndex + 1);
    QApplication::restoreOverrideCursor();

    UBPersistenceManager::persistenceManager()->persistDocumentMetadata(selectedDocument());
}

void UBBoardController::addScene(std::shared_ptr<UBGraphicsScene> scene, bool replaceActiveIfEmpty)
{
    if (scene)
    {
        std::shared_ptr<UBGraphicsScene> clone = scene->sceneDeepCopy();

        if (scene->document() && (scene->document() != selectedDocument()))
        {
            foreach(QUrl relativeFile, scene->relativeDependencies())
            {
                QString source = scene->document()->persistencePath() + "/" + relativeFile.path();
                QString destination = selectedDocument()->persistencePath() + "/" + relativeFile.path();

                UBFileSystemUtils::copy(source, destination, true);
            }
        }

        if (replaceActiveIfEmpty && mActiveScene->isEmpty())
        {
            UBPersistenceManager::persistenceManager()->insertDocumentSceneAt(selectedDocument(), clone, mActiveSceneIndex);
            emit addThumbnailRequired(selectedDocument(), mActiveSceneIndex);
            setActiveDocumentScene(mActiveSceneIndex);
            deleteScene(mActiveSceneIndex + 1);
        }
        else
        {
            persistCurrentScene(false,true);
            UBPersistenceManager::persistenceManager()->insertDocumentSceneAt(selectedDocument(), clone, mActiveSceneIndex + 1);
            emit addThumbnailRequired(selectedDocument(), mActiveSceneIndex + 1);
            setActiveDocumentScene(mActiveSceneIndex + 1);
        }

        QDateTime now = QDateTime::currentDateTime();
        selectedDocument()->setMetaData(UBSettings::documentUpdatedAt, UBStringUtils::toUtcIsoDateTime(now));
    }
}


void UBBoardController::addScene(std::shared_ptr<UBDocumentProxy> proxy, int sceneIndex, bool replaceActiveIfEmpty)
{
    std::shared_ptr<UBGraphicsScene> scene = UBPersistenceManager::persistenceManager()->loadDocumentScene(proxy, sceneIndex);

    if (scene)
    {
        addScene(scene, replaceActiveIfEmpty);
    }
}

void UBBoardController::duplicateScene(int nIndex)
{
    QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
    persistCurrentScene(false,true);

    duplicatePage(nIndex);
    emit addThumbnailRequired(selectedDocument(), nIndex + 1);
    if (UBApplication::documentController->selectedDocument() == selectedDocument())
    {
        UBApplication::documentController->insertThumbPage(nIndex +1);
        UBApplication::documentController->reloadThumbnails();
    }

    QDateTime now = QDateTime::currentDateTime();
    selectedDocument()->setMetaData(UBSettings::documentUpdatedAt, UBStringUtils::toUtcIsoDateTime(now));

    setActiveDocumentScene(nIndex + 1);
    QApplication::restoreOverrideCursor();
}

void UBBoardController::duplicateScene()
{
    if (UBApplication::applicationController->displayMode() != UBApplicationController::Board)
        return;
    duplicateScene(mActiveSceneIndex);
}

UBGraphicsItem *UBBoardController::duplicateItem(UBItem *item)
{
    if (!item)
        return NULL;

    UBGraphicsItem *retItem = NULL;

    mLastCreatedItem = NULL;

    QUrl sourceUrl;
    QByteArray pData;

    //common parameters for any item
    QPointF itemPos;
    QSizeF itemSize;

    QGraphicsItem *commonItem = dynamic_cast<QGraphicsItem*>(item);
    if (commonItem)
    {
        qreal shifting = UBSettings::settings()->objectFrameWidth;
        itemPos = commonItem->pos() + QPointF(shifting,shifting);
        itemSize = commonItem->boundingRect().size();
        commonItem->setSelected(false);

    }

    UBMimeType::Enum itemMimeType;

    QString srcFile = item->sourceUrl().toLocalFile();
    if (srcFile.isEmpty())
        srcFile = item->sourceUrl().toString();

    QString contentTypeHeader;
    if (!srcFile.isEmpty())
        contentTypeHeader = UBFileSystemUtils::mimeTypeFromFileName(srcFile);

    if(NULL != qgraphicsitem_cast<UBGraphicsGroupContainerItem*>(commonItem))
        itemMimeType = UBMimeType::Group;
    else
        itemMimeType = UBFileSystemUtils::mimeTypeFromString(contentTypeHeader);

    switch(static_cast<int>(itemMimeType))
    {
    case UBMimeType::AppleWidget:
    case UBMimeType::W3CWidget:
        {
            UBGraphicsWidgetItem *witem = dynamic_cast<UBGraphicsWidgetItem*>(item);
            if (witem)
            {
                sourceUrl = witem->getOwnFolder();
            }
        }break;

    case UBMimeType::Video:
    case UBMimeType::Audio:
        {
            UBGraphicsMediaItem *mitem = dynamic_cast<UBGraphicsMediaItem*>(item);
            if (mitem)
            {
                sourceUrl = mitem->mediaFileUrl();
                downloadURL(sourceUrl, srcFile, itemPos, QSize(itemSize.width(), itemSize.height()), false, false);
                return NULL; // async operation
            }
        }break;

    case UBMimeType::VectorImage:
        {
            UBGraphicsSvgItem *viitem = dynamic_cast<UBGraphicsSvgItem*>(item);
            if (viitem)
            {
                pData = viitem->fileData();
                sourceUrl = item->sourceUrl();
            }
        }break;

    case UBMimeType::RasterImage:
        {
            UBGraphicsPixmapItem *pixitem = dynamic_cast<UBGraphicsPixmapItem*>(item);
            if (pixitem)
            {
                 QBuffer buffer(&pData);
                 buffer.open(QIODevice::WriteOnly);
                 QString format = UBFileSystemUtils::extension(item->sourceUrl().toString(QUrl::DecodeReserved));
                 pixitem->pixmap().save(&buffer, format.toLatin1());
            }
        }break;

    case UBMimeType::Group:
    {
        UBGraphicsGroupContainerItem* groupItem = dynamic_cast<UBGraphicsGroupContainerItem*>(item);
        UBGraphicsGroupContainerItem* duplicatedGroup = NULL;

        QList<QGraphicsItem*> duplicatedItems;
        QList<QGraphicsItem*> children = groupItem->childItems();

        mActiveScene->setURStackEnable(false);
        foreach(QGraphicsItem* pIt, children){
            UBItem* pItem = dynamic_cast<UBItem*>(pIt);
            if(pItem)
            {
                QGraphicsItem * itemToGroup = dynamic_cast<QGraphicsItem *>(duplicateItem(pItem));
                if (itemToGroup)
                {
                    itemToGroup->setZValue(pIt->zValue());
                    itemToGroup->setData(UBGraphicsItemData::ItemOwnZValue, pIt->data(UBGraphicsItemData::ItemOwnZValue).toReal());
                    duplicatedItems.append(itemToGroup);
                }
            }
        }
        duplicatedGroup = mActiveScene->createGroup(duplicatedItems);
        duplicatedGroup->setTransform(groupItem->transform());
        groupItem->copyItemParameters(duplicatedGroup);
        groupItem->setSelected(false);


        retItem = dynamic_cast<UBGraphicsItem *>(duplicatedGroup);

        QGraphicsItem * itemToAdd = dynamic_cast<QGraphicsItem *>(retItem);
        if (itemToAdd)
        {
            mActiveScene->addItem(itemToAdd);
            itemToAdd->setSelected(true);
        }
        mActiveScene->setURStackEnable(true);
    }break;

    case UBMimeType::UNKNOWN:
        {
            QGraphicsItem *copiedItem = dynamic_cast<QGraphicsItem*>(item);
            QGraphicsItem *gitem = dynamic_cast<QGraphicsItem*>(item->deepCopy());
            if (gitem)
            {
                mActiveScene->addItem(gitem);

                if (copiedItem)
                {
                    if (mActiveScene->tools().contains(copiedItem))
                    {
                        mActiveScene->registerTool(gitem);
                    }
                }
                gitem->setPos(itemPos);

                mLastCreatedItem = gitem;
                gitem->setSelected(true);
            }
            retItem = dynamic_cast<UBGraphicsItem *>(gitem);
        }break;
    }

    if (retItem)
    {
        QGraphicsItem *graphicsRetItem = dynamic_cast<QGraphicsItem *>(retItem);
        if (mActiveScene->isURStackIsEnabled()) { //should be deleted after scene own undo stack implemented
             UBGraphicsItemUndoCommand* uc = new UBGraphicsItemUndoCommand(mActiveScene, 0, graphicsRetItem);
             UBApplication::undoStack->push(uc);
        }
        return retItem;
    }

    UBItem *createdItem = downloadFinished(true, sourceUrl, QUrl::fromLocalFile(srcFile), contentTypeHeader, pData, itemPos, QSize(itemSize.width(), itemSize.height()), false);
    if (createdItem)
    {
        createdItem->setSourceUrl(item->sourceUrl());
        item->copyItemParameters(createdItem);

        QGraphicsItem *createdGitem = dynamic_cast<QGraphicsItem*>(createdItem);
        if (createdGitem)
            createdGitem->setPos(itemPos);
        mLastCreatedItem = dynamic_cast<QGraphicsItem*>(createdItem);
        mLastCreatedItem->setSelected(true);

        retItem = dynamic_cast<UBGraphicsItem *>(createdItem);
    }

    return retItem;
}

void UBBoardController::deleteScene(int nIndex)
{
    if (selectedDocument()->pageCount()>=2)
    {
        mDeletingSceneIndex = nIndex;
        QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
        persistCurrentScene();
        UBApplication::showMessage(tr("Deleting page %1").arg(nIndex+1), true);

        QList<int> scIndexes;
        scIndexes << nIndex;
        UBPersistenceManager::persistenceManager()->deleteDocumentScenes(selectedDocument(), scIndexes);
        emit removeThumbnailRequired(nIndex);
        if (UBApplication::documentController->selectedDocument() == selectedDocument())
        {
            UBApplication::documentController->deleteThumbPage(nIndex);
        }

        QDateTime now = QDateTime::currentDateTime();
        selectedDocument()->setMetaData(UBSettings::documentUpdatedAt, UBStringUtils::toUtcIsoDateTime(now));
        UBMetadataDcSubsetAdaptor::persist(selectedDocument());

        if (nIndex >= pageCount())
            nIndex = pageCount()-1;
        setActiveDocumentScene(nIndex);
        UBApplication::showMessage(tr("Page %1 deleted").arg(nIndex+1));
        QApplication::restoreOverrideCursor();
        mDeletingSceneIndex = -1;
    }
}


void UBBoardController::clearScene()
{
    if (mActiveScene)
    {
        freezeW3CWidgets(true);
        mActiveScene->clearContent(UBGraphicsScene::clearItemsAndAnnotations);
        mActiveScene->setLastCenter(QPointF(0,0));
        mControlView->centerOn(mActiveScene->lastCenter());
        updateActionStates();
    }
}


void UBBoardController::clearSceneItems()
{
    if (mActiveScene)
    {
        freezeW3CWidgets(true);
        mActiveScene->clearContent(UBGraphicsScene::clearItems);
        updateActionStates();
    }
}


void UBBoardController::clearSceneAnnotation()
{
    if (mActiveScene)
    {
        mActiveScene->clearContent(UBGraphicsScene::clearAnnotations);
        updateActionStates();
    }
}

void UBBoardController::clearSceneBackground()
{
    if (mActiveScene)
    {
        mActiveScene->clearContent(UBGraphicsScene::clearBackground);
        updateActionStates();
    }
}

void UBBoardController::showDocumentsDialog()
{
    if (selectedDocument())
        persistCurrentScene();

    UBApplication::mainWindow->actionLibrary->setChecked(false);

}

void UBBoardController::libraryDialogClosed(int ret)
{
    Q_UNUSED(ret);

    mMainWindow->actionLibrary->setChecked(false);
}


void UBBoardController::blackout()
{
    UBApplication::applicationController->blackout();
}

void UBBoardController::showKeyboard(bool show)
{
    if(show)
        UBDrawingController::drawingController()->setStylusTool(UBStylusTool::Selector);

    if(UBSettings::settings()->useSystemOnScreenKeyboard->get().toBool())
        UBPlatformUtils::showOSK(show);
    else
        mPaletteManager->showVirtualKeyboard(show);
}


void UBBoardController::zoomIn(QPointF scenePoint)
{
    if (mControlView->transform().m11() > UB_MAX_ZOOM)
    {
        qApp->beep();
        return;
    }
    zoom(mZoomFactor, scenePoint);
}


void UBBoardController::zoomOut(QPointF scenePoint)
{
    if ((mControlView->horizontalScrollBar()->maximum() == 0) && (mControlView->verticalScrollBar()->maximum() == 0))
    {
        // Do not zoom out if we reached the maximum
        qApp->beep();
        return;
    }

    qreal newZoomFactor = 1 / mZoomFactor;

    zoom(newZoomFactor, scenePoint);
}


void UBBoardController::zoomRestore()
{
    QTransform tr;

    tr.scale(mSystemScaleFactor, mSystemScaleFactor);
    mControlView->setTransform(tr);

    centerRestore();

    emit zoomChanged(1.0);
    UBApplication::applicationController->adjustDisplayView();

    emit controlViewportChanged();
    mActiveScene->setBackgroundZoomFactor(mControlView->transform().m11());
}


void UBBoardController::centerRestore()
{
    centerOn(QPointF(0,0));
}


void UBBoardController::centerOn(QPointF scenePoint)
{
    // workaround: foreground not repainted after centerOn on Qt5 (fixed in Qt6)
    QPointF offset{1, 1};
    mControlView->centerOn(scenePoint - offset);
    mControlView->translate(offset.x(), offset.y());
    UBApplication::applicationController->adjustDisplayView();
}


void UBBoardController::zoom(const qreal ratio, QPointF scenePoint)
{

    QPointF viewCenter = mControlView->mapToScene(QRect(0, 0, mControlView->width(), mControlView->height()).center());
    QPointF offset = scenePoint - viewCenter;
    QPointF scalledOffset = offset / ratio;

    qreal currentZoom = ratio * mControlView->viewportTransform().m11() / mSystemScaleFactor;

    qreal usedRatio = ratio;
    if (currentZoom > UB_MAX_ZOOM)
    {
        currentZoom = UB_MAX_ZOOM;
        usedRatio = currentZoom * mSystemScaleFactor / mControlView->viewportTransform().m11();
    }

    mControlView->scale(usedRatio, usedRatio);

    QPointF newCenter = scenePoint - scalledOffset;

    mControlView->centerOn(newCenter);

    emit zoomChanged(currentZoom);
    UBApplication::applicationController->adjustDisplayView();

    emit controlViewportChanged();
    mActiveScene->setBackgroundZoomFactor(mControlView->transform().m11());
}


void UBBoardController::handScroll(qreal dx, qreal dy)
{
    qreal antiScaleRatio = 1/(mSystemScaleFactor * currentZoom());
    mControlView->translate(dx*antiScaleRatio, dy*antiScaleRatio);

    UBApplication::applicationController->adjustDisplayView();

    emit controlViewportChanged();
}

void UBBoardController::persistViewPositionOnCurrentScene()
{
    QRect rect = mControlView->rect();
    QPoint center(rect.x() + rect.width() / 2, rect.y() + rect.height() / 2);
    QPointF viewRelativeCenter = mControlView->mapToScene(center);
    mActiveScene->setLastCenter(viewRelativeCenter);
}

void UBBoardController::previousScene()
{
    if (mActiveSceneIndex > 0)
    {
        QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
        persistViewPositionOnCurrentScene();
// not necessary, done by setActiveDocumentScene
//        persistCurrentScene();
        setActiveDocumentScene(mActiveSceneIndex - 1);
        centerOn(mActiveScene->lastCenter());
        QApplication::restoreOverrideCursor();
    }

    updateActionStates();
}


void UBBoardController::nextScene()
{
    if (mActiveSceneIndex < selectedDocument()->pageCount() - 1)
    {
        QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
        persistViewPositionOnCurrentScene();

        setActiveDocumentScene(mActiveSceneIndex + 1);
        centerOn(mActiveScene->lastCenter());

        QApplication::restoreOverrideCursor();
    }

    updateActionStates();
}


void UBBoardController::firstScene()
{
    if (mActiveSceneIndex > 0)
    {
        QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
        persistViewPositionOnCurrentScene();

        setActiveDocumentScene(0);

        centerOn(mActiveScene->lastCenter());
        QApplication::restoreOverrideCursor();
    }

    updateActionStates();
}


void UBBoardController::lastScene()
{
    if (mActiveSceneIndex < selectedDocument()->pageCount() - 1)
    {
        QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
        persistViewPositionOnCurrentScene();
        setActiveDocumentScene(selectedDocument()->pageCount() - 1);
        centerOn(mActiveScene->lastCenter());
        QApplication::restoreOverrideCursor();
    }

    updateActionStates();
}

void UBBoardController::downloadURL(const QUrl& url, QString contentSourceUrl, const QPointF& pPos, const QSize& pSize, bool isBackground, bool internalData)
{
    QString sUrl = url.toString();
    qDebug() << "something has been dropped on the board! Url is: " << sUrl.left(255);

    QGraphicsItem *oldBackgroundObject = NULL;
    if (isBackground)
        oldBackgroundObject = mActiveScene->backgroundObject();

    if(url.scheme() == "openboardtool")
    {
        downloadFinished(true, url, QUrl(), "application/openboard-tool", QByteArray(), pPos, pSize, isBackground);
    }
    else if (url.scheme() == "file" || url.scheme() == "")
    {
        QUrl formedUrl = url.scheme() == "file" ? url : QUrl::fromLocalFile(sUrl);
        QString fileName = formedUrl.toLocalFile();
        QString contentType = UBFileSystemUtils::mimeTypeFromFileName(fileName);

        // directly add local file to document without copying
        QFile file(fileName);
        QByteArray data;

        if (file.open(QIODevice::ReadOnly))
        {
            data = file.readAll();
        }

        downloadFinished(true, formedUrl, QUrl(), contentType, data, pPos, pSize, isBackground, internalData);
        file.close();
    }
    else
    {
        // When we fall there, it means that we are dropping something from the web to the board
        sDownloadFileDesc desc;
        desc.modal = true;
        desc.srcUrl = sUrl;
        desc.currentSize = 0;
        desc.name = url.scheme() == "data" ? "Local data" : url.fileName();
        desc.totalSize = 0; // The total size will be retrieved during the download
        desc.pos = pPos;
        desc.size = pSize;
        desc.isBackground = isBackground;

        UBDownloadManager::downloadManager()->addFileToDownload(desc);
    }

    if (isBackground && oldBackgroundObject != mActiveScene->backgroundObject())
    {
        if (mActiveScene->isURStackIsEnabled()) { //should be deleted after scene own undo stack implemented
            UBGraphicsItemUndoCommand* uc = new UBGraphicsItemUndoCommand(mActiveScene, oldBackgroundObject, mActiveScene->backgroundObject());
            UBApplication::undoStack->push(uc);
        }
    }


}


UBItem *UBBoardController::downloadFinished(bool pSuccess, QUrl sourceUrl, QUrl contentUrl, QString pContentTypeHeader,
                                            QByteArray pData, QPointF pPos, QSize pSize,
                                            bool isBackground, bool internalData)
{
    QString mimeType = pContentTypeHeader;

    // In some cases "image/jpeg;charset=" is retourned by the drag-n-drop. That is
    // why we will check if an ; exists and take the first part (the standard allows this kind of mimetype)
    if(mimeType.isEmpty())
      mimeType = UBFileSystemUtils::mimeTypeFromFileName(sourceUrl.toString());

    int position=mimeType.indexOf(";");
    if(position != -1)
        mimeType=mimeType.left(position);

    UBMimeType::Enum itemMimeType = UBFileSystemUtils::mimeTypeFromString(mimeType);

    if (!pSuccess)
    {
        UBApplication::showMessage(tr("Downloading content %1 failed").arg(sourceUrl.toString()));
        return NULL;
    }


    mActiveScene->deselectAllItems();
    const QString scheme = sourceUrl.scheme();

    if (scheme != "file" && scheme != "openboardtool" && scheme != "data")
        UBApplication::showMessage(tr("Download finished"));

    if (UBMimeType::RasterImage == itemMimeType)
    {

        qDebug() << "accepting mime type" << mimeType << "as raster image";

        if (pData.length() == 0)
        {
            QFile file(sourceUrl.toLocalFile());

            if (file.open(QFile::ReadOnly))
            {
                pData = file.readAll();
                file.close();
            }
        }

        UBGraphicsPixmapItem* pixItem = mActiveScene->addImage(pData, nullptr, pPos, 1.);

        if (scheme == "data")
        {
            // create a shorter, but still unique URL using a hash function
            QCryptographicHash hash(QCryptographicHash::Md5);
            hash.addData(sourceUrl.toString().toLatin1());
            QByteArray result = hash.result();
            QString hashedUrl = "md5:" + result.toBase64();
            pixItem->setSourceUrl(hashedUrl);
        }
        else
        {
            pixItem->setSourceUrl(sourceUrl);
        }

        if (isBackground)
        {
            mActiveScene->setAsBackgroundObject(pixItem, true);
        }
        else
        {
            mActiveScene->scaleToFitDocumentSize(pixItem, true, UBSettings::objectInControlViewMargin);
            UBDrawingController::drawingController()->setStylusTool(UBStylusTool::Selector);
        }

        return pixItem;
    }
    else if (UBMimeType::VectorImage == itemMimeType)
    {
        qDebug() << "accepting mime type" << mimeType << "as vector image";

        UBGraphicsSvgItem* svgItem = mActiveScene->addSvg(sourceUrl, pPos, pData);
        svgItem->setSourceUrl(sourceUrl);

        if (isBackground)
        {
            mActiveScene->setAsBackgroundObject(svgItem);
        }
        else
        {
            mActiveScene->scaleToFitDocumentSize(svgItem, true, UBSettings::objectInControlViewMargin);
            UBDrawingController::drawingController()->setStylusTool(UBStylusTool::Selector);
        }

        return svgItem;
    }
    else if (UBMimeType::AppleWidget == itemMimeType) //mime type invented by us :-(
    {
        qDebug() << "accepting mime type" << mimeType << "as Apple widget";

        QUrl widgetUrl = sourceUrl;

        if (pData.length() > 0)
        {
            widgetUrl = expandWidgetToTempDir(pData, "wdgt");
        }

        UBGraphicsWidgetItem* appleWidgetItem = mActiveScene->addAppleWidget(widgetUrl, pPos);

        appleWidgetItem->setSourceUrl(sourceUrl);

        if (isBackground)
        {
            mActiveScene->setAsBackgroundObject(appleWidgetItem);
        }
        else
        {
            UBDrawingController::drawingController()->setStylusTool(UBStylusTool::Selector);
        }

        return appleWidgetItem;
    }
    else if (UBMimeType::W3CWidget == itemMimeType)
    {
        qDebug() << "accepting mime type" << mimeType << "as W3C widget";
        QUrl widgetUrl = sourceUrl;

        if (pData.length() > 0)
        {
            widgetUrl = expandWidgetToTempDir(pData);
        }

        UBGraphicsWidgetItem *w3cWidgetItem = addW3cWidget(widgetUrl, pPos);

        if (isBackground)
        {
            mActiveScene->setAsBackgroundObject(w3cWidgetItem);
        }
        else
        {
            UBDrawingController::drawingController()->setStylusTool(UBStylusTool::Selector);
        }

        return w3cWidgetItem;
    }
    else if (UBMimeType::Video == itemMimeType)
    {
        qDebug() << "accepting mime type" << mimeType << "as video";

        UBGraphicsMediaItem *mediaVideoItem = 0;
        QUuid uuid = QUuid::createUuid();
        if (pData.length() > 0)
        {
            QString destFile;
            bool b = UBPersistenceManager::persistenceManager()->addFileToDocument(selectedDocument(),
                sourceUrl.toString(),
                UBPersistenceManager::videoDirectory,
                uuid,
                destFile,
                &pData);
            if (!b)
            {
                UBApplication::showMessage(tr("Add file operation failed: file copying error"));
                return NULL;
            }

            QUrl url = QUrl::fromLocalFile(destFile);

            mediaVideoItem = mActiveScene->addMedia(url, false, pPos);
        }
        else
        {
            qDebug() << sourceUrl.toString();
            mediaVideoItem = addVideo(sourceUrl, false, pPos, true);
        }

        if(mediaVideoItem){
            if (contentUrl.isEmpty())
                mediaVideoItem->setSourceUrl(sourceUrl);
            else
                mediaVideoItem->setSourceUrl(contentUrl);
            mediaVideoItem->setUuid(uuid);
            connect(this, SIGNAL(activeSceneChanged()), mediaVideoItem, SLOT(activeSceneChanged()));
        }

        UBDrawingController::drawingController()->setStylusTool(UBStylusTool::Selector);

        return mediaVideoItem;
    }
    else if (UBMimeType::Audio == itemMimeType)
    {
        qDebug() << "accepting mime type" << mimeType << "as audio";

        UBGraphicsMediaItem *audioMediaItem = 0;

        QUuid uuid = QUuid::createUuid();
        if (pData.length() > 0)
        {
            QString destFile;
            bool b = UBPersistenceManager::persistenceManager()->addFileToDocument(selectedDocument(),
                sourceUrl.toString(),
                UBPersistenceManager::audioDirectory,
                uuid,
                destFile,
                &pData);
            if (!b)
            {
                UBApplication::showMessage(tr("Add file operation failed: file copying error"));
                return NULL;
            }

            QUrl url = QUrl::fromLocalFile(destFile);

            audioMediaItem = mActiveScene->addMedia(url, false, pPos);
        }
        else
        {
            audioMediaItem = addAudio(sourceUrl, false, pPos, true);
        }

        if(audioMediaItem){
            if (contentUrl.isEmpty())
                audioMediaItem->setSourceUrl(sourceUrl);
            else
                audioMediaItem->setSourceUrl(contentUrl);
            audioMediaItem->setUuid(uuid);
            connect(this, SIGNAL(activeSceneChanged()), audioMediaItem, SLOT(activeSceneChanged()));
        }

        UBDrawingController::drawingController()->setStylusTool(UBStylusTool::Selector);

        return audioMediaItem;
    }
    else if (UBMimeType::Flash == itemMimeType)
    {

        qDebug() << "accepting mime type" << mimeType << "as flash";

        QString sUrl = sourceUrl.toString();

        if (sUrl.startsWith("file://") || sUrl.startsWith("/"))
        {
            sUrl = sourceUrl.toLocalFile();
        }

        QSize size;

        if (pSize.height() > 0 && pSize.width() > 0)
            size = pSize;
        else
            size = mActiveScene->nominalSize() * .8;

        Q_UNUSED(internalData)

        QString widgetUrl = UBGraphicsW3CWidgetItem::createNPAPIWrapper(sUrl, mimeType, size);
        UBFileSystemUtils::deleteFile(sourceUrl.toLocalFile());
        emit npapiWidgetCreated(widgetUrl);

        if (widgetUrl.length() > 0)
        {
            UBGraphicsWidgetItem *widgetItem = mActiveScene->addW3CWidget(QUrl::fromLocalFile(widgetUrl), pPos);
            widgetItem->setUuid(QUuid::createUuid());
            widgetItem->setSourceUrl(QUrl::fromLocalFile(widgetUrl));
            qDebug() << widgetItem->getOwnFolder();
            qDebug() << widgetItem->getSnapshotPath();

            widgetItem->setSnapshotPath(widgetItem->getOwnFolder());

            UBDrawingController::drawingController()->setStylusTool(UBStylusTool::Selector);

            return widgetItem;
        }
    }
    else if (UBMimeType::PDF == itemMimeType)
    {
        qDebug() << "accepting mime type" << mimeType << "as PDF";
        qDebug() << "pdf data length: " << pData.size();
        qDebug() << "sourceurl : " + sourceUrl.toString();
        QString sUrl = sourceUrl.toString();

        int numberOfImportedDocuments = 0;
        int currentNumberOfThumbnails = selectedDocument()->pageCount();
        if(!sourceUrl.isEmpty() && (sUrl.startsWith("file://") || sUrl.startsWith("/")))
        {
            QStringList fileNames;
            fileNames << sourceUrl.toLocalFile();
            numberOfImportedDocuments = UBDocumentManager::documentManager()->addFilesToDocument(selectedDocument(), fileNames);
        }
        else if(pData.size()){
            QTemporaryFile pdfFile("XXXXXX.pdf");
            if (pdfFile.open())
            {
                pdfFile.write(pData);
                QStringList fileNames;
                fileNames << pdfFile.fileName();
                numberOfImportedDocuments = UBDocumentManager::documentManager()->addFilesToDocument(selectedDocument(), fileNames);

                pdfFile.close();
            }
        }

        if (numberOfImportedDocuments > 0)
        {

            QDateTime now = QDateTime::currentDateTime();
            selectedDocument()->setMetaData(UBSettings::documentUpdatedAt, UBStringUtils::toUtcIsoDateTime(now));
            updateActionStates();

            int numberOfThumbnailsToAdd =  selectedDocument()->pageCount() - currentNumberOfThumbnails;

            bool updateDocumentThumbnailsView = UBApplication::documentController->selectedDocument() ==  selectedDocument();
            for (int i = 0; i < numberOfThumbnailsToAdd; i++)
            {
                emit addThumbnailRequired(selectedDocument(), currentNumberOfThumbnails+i);

                if (updateDocumentThumbnailsView)
                {
                    UBApplication::documentController->insertThumbPage(currentNumberOfThumbnails+i);
                }
            }
            if (updateDocumentThumbnailsView)
            {
                UBApplication::documentController->reloadThumbnails();
            }
        }
    }
    else if (UBMimeType::OpenboardTool == itemMimeType)
    {
        qDebug() << "accepting mime type" << mimeType << "OpenBoard Tool";

        if (sourceUrl.toString() == UBToolsManager::manager()->compass.id)
        {
            mActiveScene->addCompass(pPos);
            UBDrawingController::drawingController()->setStylusTool(UBStylusTool::Selector);
        }
        else if (sourceUrl.toString() == UBToolsManager::manager()->ruler.id)
        {
            mActiveScene->addRuler(pPos);
            UBDrawingController::drawingController()->setStylusTool(UBStylusTool::Selector);
        }
        else if (sourceUrl.toString() == UBToolsManager::manager()->axes.id)
        {
            mActiveScene->addAxes(pPos);
            UBDrawingController::drawingController()->setStylusTool(UBStylusTool::Selector);
        }
        else if (sourceUrl.toString() == UBToolsManager::manager()->protractor.id)
        {
            mActiveScene->addProtractor(pPos);
            UBDrawingController::drawingController()->setStylusTool(UBStylusTool::Selector);
        }
        else if (sourceUrl.toString() == UBToolsManager::manager()->triangle.id)
        {
            mActiveScene->addTriangle(pPos);
            UBDrawingController::drawingController()->setStylusTool(UBStylusTool::Selector);
        }
        else if (sourceUrl.toString() == UBToolsManager::manager()->cache.id)
        {
            mActiveScene->addCache();
            UBDrawingController::drawingController()->setStylusTool(UBStylusTool::Selector);
        }
        else if (sourceUrl.toString() == UBToolsManager::manager()->magnifier.id)
        {
            UBMagnifierParams params;
            params.x = controlContainer()->geometry().width() / 2;
            params.y = controlContainer()->geometry().height() / 2;
            params.zoom = 2;
            params.sizePercentFromScene = 20;
            mActiveScene->addMagnifier(params);
            UBDrawingController::drawingController()->setStylusTool(UBStylusTool::Selector);
        }
        else if (sourceUrl.toString() == UBToolsManager::manager()->mask.id)
        {
            mActiveScene->addMask(pPos);
            UBDrawingController::drawingController()->setStylusTool(UBStylusTool::Selector);
        }
        else
        {
            UBApplication::showMessage(tr("Unknown tool type %1").arg(sourceUrl.toString()));
        }
    }
    else if (UBMimeType::Html == itemMimeType)
    {
        if (!mEmbedController)
        {
            mEmbedController = new UBEmbedController(mControlView);
        }

        static const QRegularExpression matchTitle("<title>([^<]*)</title>");

        QRegularExpressionMatch match = matchTitle.match(pData);
        QString title = match.hasMatch() ? match.captured(1) : tr("Untitled");

        mEmbedController->pageTitleChanged(title);
        mEmbedController->pageUrlChanged(sourceUrl);
        mEmbedController->showEmbedDialog();

        UBEmbedParser* parser = new UBEmbedParser(this);
        connect(parser, &UBEmbedParser::parseResult, this, [this,parser](bool hasEmbeddedContent){
            QList<UBEmbedContent> list = parser->embeddedContent();
            mEmbedController->updateListOfEmbeddableContent(list);
            parser->deleteLater();
        });

        parser->parse(pData);
    }
    else if (UBMimeType::Document == itemMimeType)
    {
        QString documentFolderName = sourceUrl.toString().section('/', -2, -2); //section before "/metadata.rdf" is documentFolderName

        std::shared_ptr<UBDocumentProxy> document = UBPersistenceManager::persistenceManager()->mDocumentTreeStructureModel->findDocumentByFolderName(documentFolderName);

        if (document)
        {
            setActiveDocumentScene(document, document->lastVisitedSceneIndex());
        }
        else
        {
            UBApplication::showMessage(tr("Could not find document."));
        }
    }
    else
    {
        UBApplication::showMessage(tr("Unknown content type %1").arg(pContentTypeHeader));
        qWarning() << "ignoring mime type" << pContentTypeHeader ;
    }

    return NULL;
}

std::shared_ptr<UBGraphicsScene> UBBoardController::setActiveDocumentScene(int pSceneIndex)
{
    return setActiveDocumentScene(selectedDocument(), pSceneIndex);
}

std::shared_ptr<UBGraphicsScene> UBBoardController::setActiveDocumentScene(std::shared_ptr<UBDocumentProxy> pDocumentProxy, const int pSceneIndex, bool forceReload, bool onImport)
{
    UBApplication::setOverrideCursor(QCursor(Qt::WaitCursor));
    saveViewState();

    bool documentChange = selectedDocument() != pDocumentProxy;

    int index = pSceneIndex;
    int sceneCount = pDocumentProxy->pageCount();
    if (index >= sceneCount && sceneCount > 0)
        index = sceneCount - 1;

    std::shared_ptr<UBGraphicsScene> targetScene = UBPersistenceManager::persistenceManager()->loadDocumentScene(pDocumentProxy, index);

    bool sceneChange = targetScene != mActiveScene;

    if (targetScene)
    {
        if (mActiveScene && !onImport)
        {
            persistCurrentScene();
            freezeW3CWidgets(true);
            ClearUndoStack();
        }else
        {
            UBApplication::undoStack->clear();
        }

        mActiveScene = targetScene;
        mActiveSceneIndex = index;

        setDocument(pDocumentProxy, forceReload);

        updateSystemScaleFactor();

        if (mControlView->scene())
        {
            disconnect(UBApplication::undoStack.data(), SIGNAL(indexChanged(int)), mControlView->scene().get(), SLOT(updateSelectionFrameWrapper(int)));
        }

        mControlView->setScene(mActiveScene.get());
        connect(UBApplication::undoStack.data(), SIGNAL(indexChanged(int)), mControlView->scene().get(), SLOT(updateSelectionFrameWrapper(int)));

        mDisplayView->setScene(mActiveScene.get());
        mActiveScene->setBackgroundZoomFactor(mControlView->transform().m11());
        pDocumentProxy->setDefaultDocumentSize(mActiveScene->nominalSize());
        updatePageSizeState();

        adjustDisplayViews();

        UBSettings::settings()->setDarkBackground(mActiveScene->isDarkBackground());
        UBSettings::settings()->setPageBackground(mActiveScene->pageBackground());

        freezeW3CWidgets(false);

        selectionChanged();

        updateBackgroundActionsState(mActiveScene->isDarkBackground(), mActiveScene->pageBackground());

        if (UBApplication::documentController
        && (UBApplication::documentController->selectedDocument() == selectedDocument()))
        {
            UBApplication::documentController->setActiveThumbnail(pSceneIndex);
        }

        if(documentChange)
        {
            UBGraphicsTextItem::lastUsedTextColor = QColor(Qt::black);
        }

        if (sceneChange)
        {
            emit activeSceneChanged();
        }

        pDocumentProxy->setLastVisitedSceneIndex(mActiveSceneIndex);

        UBFeaturesController* featuresController = paletteManager()->featuresWidget()->getFeaturesController();

        QUrl url = QUrl::fromLocalFile(pDocumentProxy->persistencePath() + "/metadata.rdf");
        QString documentFolderName = pDocumentProxy->documentFolderName();

        if (!featuresController->isDocumentInFavoriteList(documentFolderName) && !featuresController->isInRecentlyOpenDocuments(documentFolderName))
        {
            featuresController->addToFavorite(url, pDocumentProxy->name(), true);
        }
    }
    else
    {
        qWarning() << "could not load document scene : '" << pDocumentProxy->persistencePath() << "', page index : " << pSceneIndex;
    }
    UBApplication::restoreOverrideCursor();

    return targetScene;
}


void UBBoardController::moveSceneToIndex(int source, int target)
{
    if (selectedDocument())
    {
        persistCurrentScene(false,true);

        UBPersistenceManager::persistenceManager()->moveSceneToIndex(selectedDocument(), source, target);
        emit moveThumbnailRequired(source, target);
        if (UBApplication::documentController->selectedDocument() == selectedDocument())
        {
            UBApplication::documentController->moveThumbPage(source, target);
        }

        QDateTime now = QDateTime::currentDateTime();
        selectedDocument()->setMetaData(UBSettings::documentUpdatedAt, UBStringUtils::toUtcIsoDateTime(now));
        UBPersistenceManager::persistenceManager()->persistDocumentMetadata(selectedDocument());
        mMovingSceneIndex = source;
        mActiveSceneIndex = target;
        setActiveDocumentScene(target);
        mMovingSceneIndex = -1;

        emit activeSceneChanged();
        emit updateThumbnailsRequired();
    }
}

void UBBoardController::findUniquesItems(const QUndoCommand *parent, QSet<QGraphicsItem*> &items)
{
    if (parent->childCount()) {
        for (int i = 0; i < parent->childCount(); i++) {
            findUniquesItems(parent->child(i), items);
        }
    }

    // Undo command transaction macros. Process separatedly
    if (parent->text() == UBSettings::undoCommandTransactionName) {
        return;
    }

    const UBUndoCommand *undoCmd = static_cast<const UBUndoCommand*>(parent);
    if(undoCmd->getType() != UBUndoType::undotype_GRAPHICITEM)
        return;

    const UBGraphicsItemUndoCommand *cmd = dynamic_cast<const UBGraphicsItemUndoCommand*>(parent);

    // go through all added and removed objects, for create list of unique objects
    // grouped items will be deleted by groups, so we don't need do delete that items.
    QSetIterator<QGraphicsItem*> itAdded(cmd->GetAddedList());
    while (itAdded.hasNext())
    {
        QGraphicsItem* item = itAdded.next();
        if (!items.contains(item) &&
            !(item->parentItem() && UBGraphicsGroupContainerItem::Type == item->parentItem()->type()) &&
            !items.contains(item->parentItem())
            )
        {
            items.insert(item);
        }
    }

    QSetIterator<QGraphicsItem*> itRemoved(cmd->GetRemovedList());
    while (itRemoved.hasNext())
    {
        QGraphicsItem* item = itRemoved.next();
        if (!items.contains(item) &&
            !(item->parentItem() && UBGraphicsGroupContainerItem::Type == item->parentItem()->type()) &&
            !items.contains(item->parentItem())
            )
        {
            items.insert(item);
        }
    }
}

void UBBoardController::ClearUndoStack()
{
    QSet<QGraphicsItem*> uniqueItems;
    // go through all stack command
    for (int i = 0; i < UBApplication::undoStack->count(); i++) {
        findUniquesItems(UBApplication::undoStack->command(i), uniqueItems);
    }

    // Get items from clipboard in order not to delete an item that was cut
    // (using source URL of graphics items as a surrogate for equality testing)
    // This ensures that we can cut and paste a media item, widget, etc. from one page to the next.
    QClipboard *clipboard = QApplication::clipboard();
    const QMimeData* data = clipboard->mimeData();
    QList<QUrl> sourceURLs;

    if (data && data->hasFormat(UBApplication::mimeTypeUniboardPageItem)) {
        const UBMimeDataGraphicsItem* mimeDataGI = qobject_cast <const UBMimeDataGraphicsItem*>(data);

        if (mimeDataGI) {
            foreach (UBItem* sourceItem, mimeDataGI->items()) {
                sourceURLs << sourceItem->sourceUrl();
            }
        }
    }

    // go through all unique items, and check, if they are on scene, or not.
    // if not on scene, than item can be deleted
    QSetIterator<QGraphicsItem*> itUniq(uniqueItems);
    while (itUniq.hasNext())
    {
        QGraphicsItem* item = itUniq.next();
        UBGraphicsScene* scene = nullptr;
        if (item->scene()) {
            scene = dynamic_cast<UBGraphicsScene*>(item->scene());
        }

        bool inClipboard = false;
        UBItem* ubi = dynamic_cast<UBItem*>(item);
        if (ubi && sourceURLs.contains(ubi->sourceUrl()))
            inClipboard = true;

        if(!scene && !inClipboard)
        {
            if (!mActiveScene->deleteItem(item)){
                delete item;
                item = 0;
            }
        }
    }

    // clear stack, and command list
    UBApplication::undoStack->clear();
}

void UBBoardController::adjustDisplayViews()
{
    if (UBApplication::applicationController)
    {
        UBApplication::applicationController->adjustDisplayView();
        UBApplication::applicationController->adjustPreviousViews(mActiveSceneIndex, selectedDocument());
    }
}


int UBBoardController::autosaveTimeoutFromSettings()
{
    int value = UBSettings::settings()->autoSaveInterval->get().toInt();
    int minute = 60 * 1000;

    return value * minute;
}

void UBBoardController::changeBackground(bool isDark, UBPageBackground pageBackground)
{
    bool currentIsDark = mActiveScene->isDarkBackground();
    UBPageBackground currentBackgroundType = mActiveScene->pageBackground();

    if ((isDark != currentIsDark) || (currentBackgroundType != pageBackground))
    {
        UBSettings::settings()->setDarkBackground(isDark);
        UBSettings::settings()->setPageBackground(pageBackground);

        mActiveScene->setBackground(isDark, pageBackground);

        emit backgroundChanged();
    }
}

void UBBoardController::boardViewResized(QResizeEvent* event)
{
    Q_UNUSED(event);

    int innerMargin = UBSettings::boardMargin;
    int userHeight = mControlContainer->height() - (2 * innerMargin);

    mMessageWindow->move(innerMargin, innerMargin + userHeight - mMessageWindow->height());
    mMessageWindow->adjustSizeAndPosition();

    UBApplication::applicationController->initViewState(
                mControlView->horizontalScrollBar()->value(),
                mControlView->verticalScrollBar()->value());

    updateSystemScaleFactor();

    mControlView->centerOn(0,0);

    if (mDisplayView && UBApplication::displayManager->hasDisplay()) {
        UBApplication::applicationController->adjustDisplayView();
        mDisplayView->centerOn(0,0);
        setBoxing(mDisplayView->geometry());
    }

    mPaletteManager->containerResized();

    UBApplication::boardController->controlView()->scene()->moveMagnifier();

}


void UBBoardController::showMessage(const QString& message, bool showSpinningWheel)
{
    mMessageWindow->showMessage(message, showSpinningWheel);
}


void UBBoardController::hideMessage()
{
    mMessageWindow->hideMessage();
}


void UBBoardController::setDisabled(bool disable)
{
    mMainWindow->boardToolBar->setDisabled(disable);
    mControlView->setDisabled(disable);
}


void UBBoardController::selectionChanged()
{
    updateActionStates();
    emit pageSelectionChanged(activeSceneIndex());
}


void UBBoardController::undoRedoStateChange(bool canUndo)
{
    Q_UNUSED(canUndo);

    mMainWindow->actionUndo->setEnabled(UBApplication::undoStack->canUndo());
    mMainWindow->actionRedo->setEnabled(UBApplication::undoStack->canRedo());

    updateActionStates();
}


void UBBoardController::updateActionStates()
{
    mMainWindow->actionBack->setEnabled(selectedDocument() && (mActiveSceneIndex > 0));
    mMainWindow->actionForward->setEnabled(selectedDocument() && (mActiveSceneIndex < selectedDocument()->pageCount() - 1));
    mMainWindow->actionErase->setEnabled(mActiveScene && !mActiveScene->isEmpty());
}


std::shared_ptr<UBGraphicsScene> UBBoardController::activeScene() const
{
    return mActiveScene;
}


int UBBoardController::activeSceneIndex() const
{
    return mActiveSceneIndex;
}

void UBBoardController::setActiveSceneIndex(int i)
{
    mActiveSceneIndex = i;
}

void UBBoardController::documentSceneChanged(std::shared_ptr<UBDocumentProxy> pDocumentProxy, int pIndex)
{
    Q_UNUSED(pIndex);

    if(selectedDocument() == pDocumentProxy)
    {
        setActiveDocumentScene(mActiveSceneIndex);
    }
}

void UBBoardController::autosaveTimeout()
{
    if (UBApplication::applicationController->displayMode() != UBApplicationController::Board) {
        //perform autosave only in board mode
        return;
    }

    saveData(sf_showProgress);
    UBSettings::settings()->save();
}

void UBBoardController::appMainModeChanged(UBApplicationController::MainMode md)
{
    int autoSaveInterval = autosaveTimeoutFromSettings();
    if (!autoSaveInterval) {
        return;
    }

    if (!mAutosaveTimer) {
        mAutosaveTimer = new QTimer(this);
        connect(mAutosaveTimer, SIGNAL(timeout()), this, SLOT(autosaveTimeout()));
    }

    if (md == UBApplicationController::Board) {
        mAutosaveTimer->start(autoSaveInterval);
    } else if (mAutosaveTimer->isActive()) {
        mAutosaveTimer->stop();
    }
}

void UBBoardController::closing()
{
    mIsClosing = true;
    lastWindowClosed();
    ClearUndoStack();
#ifdef Q_OS_OSX
    if (!UBPlatformUtils::errorOpeningVirtualKeyboard)
        showKeyboard(false);
#else
        showKeyboard(false);
#endif
}

void UBBoardController::lastWindowClosed()
{
    if (!mCleanupDone)
    {
        if (initialDocumentScene() && initialDocumentScene()->document())
        {
            if (initialDocumentScene()->isEmpty() && (initialDocumentScene()->document()->documentDate() == initialDocumentScene()->document()->lastUpdate()))
            {
                // intial scene or document have not been modified at all, so we can delete the document.
                UBPersistenceManager::persistenceManager()->deleteDocument(initialDocumentScene()->document());

                //if current scene is not the initial document scene, we still need to persist it to ensure no data can be lost this way
                if (activeScene() != initialDocumentScene())
                {
                    persistCurrentScene();
                    UBPersistenceManager::persistenceManager()->persistDocumentMetadata(selectedDocument());
                }
            }
            else
            {
                // if intial scene or document changed, then rather the initial document scene is the current scene,
                // or current scene changed and initial document scene has already been persisted.
                // Now, we just persist the current scene before closing the app, to ensure no data can be lost this way.
                persistCurrentScene();
                UBPersistenceManager::persistenceManager()->persistDocumentMetadata(selectedDocument());
            }
        }
        else
        { //should not happen ?
            persistCurrentScene();
            UBPersistenceManager::persistenceManager()->persistDocumentMetadata(selectedDocument());
        }

        mCleanupDone = true;
    }
}



void UBBoardController::setColorIndex(int pColorIndex)
{
    UBDrawingController::drawingController()->setColorIndex(pColorIndex);

    if (UBDrawingController::drawingController()->stylusTool() != UBStylusTool::Marker &&
            UBDrawingController::drawingController()->stylusTool() != UBStylusTool::Line &&
            UBDrawingController::drawingController()->stylusTool() != UBStylusTool::Text &&
            UBDrawingController::drawingController()->stylusTool() != UBStylusTool::Selector)
    {
        UBDrawingController::drawingController()->setStylusTool(UBStylusTool::Pen);
    }

    if (UBDrawingController::drawingController()->stylusTool() == UBStylusTool::Pen ||
            UBDrawingController::drawingController()->stylusTool() == UBStylusTool::Line ||
            UBDrawingController::drawingController()->stylusTool() == UBStylusTool::Text ||
            UBDrawingController::drawingController()->stylusTool() == UBStylusTool::Selector)
    {
        mPenColorOnDarkBackground = UBSettings::settings()->penColors(true).at(pColorIndex);
        mPenColorOnLightBackground = UBSettings::settings()->penColors(false).at(pColorIndex);

        if (UBDrawingController::drawingController()->stylusTool() == UBStylusTool::Selector)
        {
            // If we are in mode board, then do that
            if(UBApplication::applicationController->displayMode() == UBApplicationController::Board)
            {
                UBDrawingController::drawingController()->setStylusTool(UBStylusTool::Pen);
                mMainWindow->actionPen->setChecked(true);
            }
        }

        emit penColorChanged();
    }
    else if (UBDrawingController::drawingController()->stylusTool() == UBStylusTool::Marker)
    {
        mMarkerColorOnDarkBackground = UBSettings::settings()->markerColors(true).at(pColorIndex);
        mMarkerColorOnLightBackground = UBSettings::settings()->markerColors(false).at(pColorIndex);
    }
}

void UBBoardController::colorPaletteChanged()
{
    mPenColorOnDarkBackground = UBSettings::settings()->penColor(true);
    mPenColorOnLightBackground = UBSettings::settings()->penColor(false);
    mMarkerColorOnDarkBackground = UBSettings::settings()->markerColor(true);
    mMarkerColorOnLightBackground = UBSettings::settings()->markerColor(false);
}


qreal UBBoardController::currentZoom()
{
    if (mControlView)
        return mControlView->viewportTransform().m11() / mSystemScaleFactor;
    else
        return 1.0;
}

void UBBoardController::removeTool(UBToolWidget* toolWidget)
{
    toolWidget->remove();
}

void UBBoardController::hide()
{
    UBApplication::mainWindow->actionLibrary->setChecked(false);
}

void UBBoardController::show()
{
    UBApplication::mainWindow->actionLibrary->setChecked(false);
}

void UBBoardController::persistCurrentScene(bool isAnAutomaticBackup, bool forceImmediateSave)
{
    if(UBPersistenceManager::persistenceManager()
            && selectedDocument() && mActiveScene && mActiveSceneIndex != mDeletingSceneIndex
            && (mActiveSceneIndex >= 0) && mActiveSceneIndex != mMovingSceneIndex)
    {
        mActiveScene->saveWidgetSnapshots();

        if (mActiveScene->isModified())
        {
            UBPersistenceManager::persistenceManager()->persistDocumentScene(selectedDocument(), mActiveScene, mActiveSceneIndex, isAnAutomaticBackup, forceImmediateSave);
            if (UBApplication::documentController->selectedDocument() == selectedDocument())
            {
                UBApplication::documentController->updateThumbPage(mActiveSceneIndex);
            }
        }
    }
}

void UBBoardController::updateSystemScaleFactor()
{
    qreal newScaleFactor = 1.0;

    if (mActiveScene)
    {
        QSize pageNominalSize = mActiveScene->nominalSize();
        //we're going to keep scale factor untouched if the size is custom
        QMap<DocumentSizeRatio::Enum, QSize> sizesMap = UBSettings::settings()->documentSizes;
      //  if(pageNominalSize == sizesMap.value(DocumentSizeRatio::Ratio16_9) || pageNominalSize == sizesMap.value(DocumentSizeRatio::Ratio4_3))
        {
            qreal hFactor = ((qreal)controlView()->size().width()) / ((qreal)pageNominalSize.width());
            qreal vFactor = ((qreal)controlView()->size().height()) / ((qreal)pageNominalSize.height());

            newScaleFactor = qMin(hFactor, vFactor);
        }
    }

    if (mSystemScaleFactor != newScaleFactor)
        mSystemScaleFactor = newScaleFactor;

    UBGraphicsScene::SceneViewState viewState = mActiveScene->viewState();

    QTransform scalingTransform;

    qreal scaleFactor = viewState.zoomFactor * mSystemScaleFactor;
    scalingTransform.scale(scaleFactor, scaleFactor);

    mControlView->setTransform(scalingTransform);
    mControlView->horizontalScrollBar()->setValue(viewState.horizontalPosition);
    mControlView->verticalScrollBar()->setValue(viewState.verticalPostition);
    mActiveScene->setBackgroundZoomFactor(mControlView->transform().m11());}


void UBBoardController::setWidePageSize(bool checked)
{
    Q_UNUSED(checked);
    QSize newSize = UBSettings::settings()->documentSizes.value(DocumentSizeRatio::Ratio16_9);

    if (mActiveScene->nominalSize() != newSize)
    {
        UBPageSizeUndoCommand* uc = new UBPageSizeUndoCommand(mActiveScene, mActiveScene->nominalSize(), newSize);
        UBApplication::undoStack->push(uc);

        setPageSize(newSize);
    }
}


void UBBoardController::setRegularPageSize(bool checked)
{
    Q_UNUSED(checked);
    QSize newSize = UBSettings::settings()->documentSizes.value(DocumentSizeRatio::Ratio4_3);

    if (mActiveScene->nominalSize() != newSize)
    {
        UBPageSizeUndoCommand* uc = new UBPageSizeUndoCommand(mActiveScene, mActiveScene->nominalSize(), newSize);
        UBApplication::undoStack->push(uc);

        setPageSize(newSize);
    }
}


void UBBoardController::setPageSize(QSize newSize)
{
    if (mActiveScene->nominalSize() != newSize)
    {
        mActiveScene->setNominalSize(newSize);

        saveViewState();

        updateSystemScaleFactor();
        updatePageSizeState();
        adjustDisplayViews();
        QDateTime now = QDateTime::currentDateTime();
        selectedDocument()->setMetaData(UBSettings::documentUpdatedAt, UBStringUtils::toUtcIsoDateTime(now));

        UBSettings::settings()->pageSize->set(newSize);
    }
}

void UBBoardController::notifyCache(bool visible)
{
    if(visible)
        emit cacheEnabled();

    mCacheWidgetIsEnabled = visible;
}

void UBBoardController::updatePageSizeState()
{
    if (mActiveScene->nominalSize() == UBSettings::settings()->documentSizes.value(DocumentSizeRatio::Ratio16_9))
    {
        mMainWindow->actionWidePageSize->setChecked(true);
    }
    else if(mActiveScene->nominalSize() == UBSettings::settings()->documentSizes.value(DocumentSizeRatio::Ratio4_3))
    {
        mMainWindow->actionRegularPageSize->setChecked(true);
    }
    else
    {
        mMainWindow->actionCustomPageSize->setChecked(true);
    }
}


void UBBoardController::saveViewState()
{
    if (mActiveScene)
    {
        mActiveScene->setViewState(UBGraphicsScene::SceneViewState(currentZoom(),
                                                                   mControlView->horizontalScrollBar()->value(),
                                                                   mControlView->verticalScrollBar()->value(),
                                                                   mActiveScene->lastCenter()));
    }
}

void UBBoardController::stylusToolChanged(int tool)
{
    if (UBPlatformUtils::hasVirtualKeyboard() && mPaletteManager->mKeyboardPalette)
    {
        UBStylusTool::Enum eTool = (UBStylusTool::Enum)tool;
        if(eTool != UBStylusTool::Selector && eTool != UBStylusTool::Text)
        {
            if(mPaletteManager->mKeyboardPalette->m_isVisible)
            {
#ifdef Q_OS_OSX
                if (!UBPlatformUtils::errorOpeningVirtualKeyboard)
                    UBApplication::mainWindow->actionVirtualKeyboard->activate(QAction::Trigger);
#else
                UBApplication::mainWindow->actionVirtualKeyboard->activate(QAction::Trigger);
#endif
            }
        }
    }

}


QUrl UBBoardController::expandWidgetToTempDir(const QByteArray& pZipedData, const QString& ext)
{
    QUrl widgetUrl;
    QTemporaryFile tmp;

    if (tmp.open())
    {
        tmp.write(pZipedData);
        tmp.flush();
        tmp.close();

        QString tmpDir = UBFileSystemUtils::createTempDir() + "." + ext;

        if (UBFileSystemUtils::expandZipToDir(tmp, tmpDir))
        {
            widgetUrl = QUrl::fromLocalFile(tmpDir);
        }
    }

    return widgetUrl;
}


void UBBoardController::grabScene(const QRectF& pSceneRect)
{
    if (mActiveScene)
    {
        QImage image(pSceneRect.width(), pSceneRect.height(), QImage::Format_ARGB32);
        image.fill(Qt::transparent);

        QRectF targetRect(0, 0, pSceneRect.width(), pSceneRect.height());
        QPainter painter(&image);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setRenderHint(QPainter::Antialiasing);

        mActiveScene->setRenderingContext(UBGraphicsScene::NonScreen);
        mActiveScene->setRenderingQuality(UBItem::RenderingQualityHigh, UBItem::CacheNotAllowed);

        mActiveScene->render(&painter, targetRect, pSceneRect);

        mActiveScene->setRenderingContext(UBGraphicsScene::Screen);
//        mActiveScene->setRenderingQuality(UBItem::RenderingQualityNormal);
        mActiveScene->setRenderingQuality(UBItem::RenderingQualityHigh, UBItem::CacheAllowed);


        mPaletteManager->addItem(QPixmap::fromImage(image));
        QDateTime now = QDateTime::currentDateTime();
        selectedDocument()->setMetaData(UBSettings::documentUpdatedAt, UBStringUtils::toUtcIsoDateTime(now));
    }
}

UBGraphicsMediaItem* UBBoardController::addVideo(const QUrl& pSourceUrl, bool startPlay, const QPointF& pos, bool bUseSource)
{
    QUuid uuid = QUuid::createUuid();
    QUrl concreteUrl = pSourceUrl;

    // media file is not in document folder yet
    if (!bUseSource)
    {
        QString destFile;
        bool b = UBPersistenceManager::persistenceManager()->addFileToDocument(selectedDocument(),
                    pSourceUrl.toLocalFile(),
                    UBPersistenceManager::videoDirectory,
                    uuid,
                    destFile);
        if (!b)
        {
            UBApplication::showMessage(tr("Add file operation failed: file copying error"));
            return NULL;
        }
        concreteUrl = QUrl::fromLocalFile(destFile);
    }// else we just use source Url.


    UBGraphicsMediaItem* vi = mActiveScene->addMedia(concreteUrl, startPlay, pos);
    QDateTime now  = QDateTime::currentDateTime();
    selectedDocument()->setMetaData(UBSettings::documentUpdatedAt, UBStringUtils::toUtcIsoDateTime(now));

    if (vi) {
        vi->setUuid(uuid);
        vi->setSourceUrl(pSourceUrl);
    }

    return vi;

}

UBGraphicsMediaItem* UBBoardController::addAudio(const QUrl& pSourceUrl, bool startPlay, const QPointF& pos, bool bUseSource)
{
    QUuid uuid = QUuid::createUuid();
    QUrl concreteUrl = pSourceUrl;

    // media file is not in document folder yet
    if (!bUseSource)
    {
        QString destFile;
        bool b = UBPersistenceManager::persistenceManager()->addFileToDocument(selectedDocument(),
            pSourceUrl.toLocalFile(),
            UBPersistenceManager::audioDirectory,
            uuid,
            destFile);
        if (!b)
        {
            UBApplication::showMessage(tr("Add file operation failed: file copying error"));
            return NULL;
        }
        concreteUrl = QUrl::fromLocalFile(destFile);
    }// else we just use source Url.

    UBGraphicsMediaItem* ai = mActiveScene->addMedia(concreteUrl, startPlay, pos);
    QDateTime now = QDateTime::currentDateTime();
    selectedDocument()->setMetaData(UBSettings::documentUpdatedAt, UBStringUtils::toUtcIsoDateTime(now));

    if (ai){
        ai->setUuid(uuid);
        ai->setSourceUrl(pSourceUrl);
    }

    return ai;

}

UBGraphicsWidgetItem *UBBoardController::addW3cWidget(const QUrl &pUrl, const QPointF &pos)
{
    UBGraphicsWidgetItem* w3cWidgetItem = 0;

    QUuid uuid = QUuid::createUuid();

    QString destPath;
    if (!UBPersistenceManager::persistenceManager()->addGraphicsWidgetToDocument(selectedDocument(), pUrl.toLocalFile(), uuid, destPath))
        return NULL;
    QUrl newUrl = QUrl::fromLocalFile(destPath);

    w3cWidgetItem = mActiveScene->addW3CWidget(newUrl, pos);

    if (w3cWidgetItem) {
        w3cWidgetItem->setUuid(uuid);
        w3cWidgetItem->setOwnFolder(newUrl);
        w3cWidgetItem->setSourceUrl(pUrl);

        QString struuid = UBStringUtils::toCanonicalUuid(uuid);
        QString snapshotPath = selectedDocument()->persistencePath() +  "/" + UBPersistenceManager::widgetDirectory + "/" + struuid + ".png";
        w3cWidgetItem->setSnapshotPath(QUrl::fromLocalFile(snapshotPath));
    }

    return w3cWidgetItem;
}

void UBBoardController::cut()
{
    //---------------------------------------------------------//

    QList<QGraphicsItem*> selectedItems;
    foreach(QGraphicsItem* gi, mActiveScene->selectedItems())
        selectedItems << gi;

    //---------------------------------------------------------//

    QList<UBItem*> selected;
    foreach(QGraphicsItem* gi, selectedItems)
    {
        gi->setSelected(false);

        UBItem* ubItem = dynamic_cast<UBItem*>(gi);
        UBGraphicsItem *ubGi =  dynamic_cast<UBGraphicsItem*>(gi);

        if (ubItem && ubGi && !mActiveScene->tools().contains(gi))
        {
            selected << ubItem->deepCopy();
            ubGi->remove();
        }
    }

    //---------------------------------------------------------//

    if (selected.size() > 0)
    {
        QClipboard *clipboard = QApplication::clipboard();

        UBMimeDataGraphicsItem*  mimeGi = new UBMimeDataGraphicsItem(selected);

        mimeGi->setData(UBApplication::mimeTypeUniboardPageItem, QByteArray());
        clipboard->setMimeData(mimeGi);

        QDateTime now = QDateTime::currentDateTime();
        selectedDocument()->setMetaData(UBSettings::documentUpdatedAt, UBStringUtils::toUtcIsoDateTime(now));
    }

    //---------------------------------------------------------//
}


void UBBoardController::copy()
{
    QList<UBItem*> selected;

    foreach(QGraphicsItem* gi, mActiveScene->selectedItems())
    {
        UBItem* ubItem = dynamic_cast<UBItem*>(gi);

        if (ubItem && !mActiveScene->tools().contains(gi))
            selected << ubItem;
    }

    if (selected.size() > 0)
    {
        QClipboard *clipboard = QApplication::clipboard();

        UBMimeDataGraphicsItem*  mimeGi = new UBMimeDataGraphicsItem(selected);

        mimeGi->setData(UBApplication::mimeTypeUniboardPageItem, QByteArray());
        clipboard->setMimeData(mimeGi);

    }
}


void UBBoardController::paste()
{
    QClipboard *clipboard = QApplication::clipboard();
    qreal xPosition = ((qreal)QRandomGenerator::global()->bounded(RAND_MAX)/(qreal)RAND_MAX) * 400;
    qreal yPosition = ((qreal)QRandomGenerator::global()->bounded(RAND_MAX)/(qreal)RAND_MAX) * 200;
    QPointF randomPos(xPosition -200 , yPosition - 100);
    QRect rect = mControlView->rect();
    QPoint center(rect.x() + rect.width() / 2, rect.y() + rect.height() / 2);
    QPointF viewRelativeCenter = mControlView->mapToScene(center);

    processMimeData(clipboard->mimeData(), viewRelativeCenter + randomPos);

    QDateTime now = QDateTime::currentDateTime();
    selectedDocument()->setMetaData(UBSettings::documentUpdatedAt, UBStringUtils::toUtcIsoDateTime(now));
}


bool zLevelLessThan( UBItem* s1, UBItem* s2)
{
    qreal s1Zvalue = dynamic_cast<QGraphicsItem*>(s1)->data(UBGraphicsItemData::ItemOwnZValue).toReal();
    qreal s2Zvalue = dynamic_cast<QGraphicsItem*>(s2)->data(UBGraphicsItemData::ItemOwnZValue).toReal();
    return s1Zvalue < s2Zvalue;
}

void UBBoardController::processMimeData(const QMimeData* pMimeData, const QPointF& pPos)
{
    if (pMimeData->hasFormat(UBApplication::mimeTypeUniboardPageItem))
    {
        const UBMimeDataGraphicsItem* mimeData = qobject_cast <const UBMimeDataGraphicsItem*>(pMimeData);

        if (mimeData)
        {
            QList<UBItem*> items = mimeData->items();
            std::stable_sort(items.begin(),items.end(),zLevelLessThan);
            foreach(UBItem* item, items)
            {
                QGraphicsItem* pItem = dynamic_cast<QGraphicsItem*>(item);
                if(NULL != pItem){
                    duplicateItem(item);
                }
            }

            return;
        }
    }

    if(pMimeData->hasHtml())
    {
        QString qsHtml = pMimeData->html();
        QString url = UBApplication::urlFromHtml(qsHtml);

        if("" != url)
        {
            downloadURL(url, QString(), pPos);
            return;
        }
    }

    if (pMimeData->hasUrls())
    {
        QList<QUrl> urls = pMimeData->urls();

        int index = 0;

        const UBFeaturesMimeData *internalMimeData = qobject_cast<const UBFeaturesMimeData*>(pMimeData);
        bool internalData = false;
        if (internalMimeData) {
            internalData = true;
        }

        foreach(const QUrl url, urls){
            QPointF pos(pPos + QPointF(index * 15, index * 15));

            downloadURL(url, QString(), pos, QSize(), false,  internalData);
            index++;
        }

        return;
    }

    if (pMimeData->hasImage())
    {
        const QStringList formats = pMimeData->formats();
        QString selectedFormat;

        for (const QString& format : formats)
        {
            if (format.startsWith("image/"))
            {
                selectedFormat = format;
                break;
            }
        }

        QBuffer buffer;

        if (selectedFormat.isEmpty())
        {
            // create an image and fill the buffer with PNG data
            QImage img = qvariant_cast<QImage> (pMimeData->imageData());
            img.save(&buffer, "png");
        }
        else
        {
            // get data from mime data
            buffer.setData(pMimeData->data(selectedFormat));
        }

        // validate that the image is really an image, webkit does not fill properly the image mime data
        if (!buffer.data().isEmpty())
        {
            mActiveScene->addImage(buffer.data(), NULL, pPos, 1.);
            return;
        }
    }

    if (pMimeData->hasText())
    {
        if("" != pMimeData->text()){
            // Sometimes, it is possible to have an URL as text. we check here if it is the case
            QString qsTmp = pMimeData->text().remove(QChar('\0'));
            if(qsTmp.startsWith("http"))
                downloadURL(QUrl(qsTmp), QString(), pPos);
            else{
                if(mActiveScene->selectedItems().count() && mActiveScene->selectedItems().at(0)->type() == UBGraphicsItemType::TextItemType)
                    dynamic_cast<UBGraphicsTextItem*>(mActiveScene->selectedItems().at(0))->setHtml(pMimeData->text());
                else
                    mActiveScene->addTextHtml("", pPos)->setHtml(pMimeData->text());
            }
        }
        else{
#ifdef Q_OS_OSX
                //  With Safari, in 95% of the drops, the mime datas are hidden in Apple Web Archive pasteboard type.
                //  This is due to the way Safari is working so we have to dig into the pasteboard in order to retrieve
                //  the data.
                QString qsUrl = UBPlatformUtils::urlFromClipboard();
                if("" != qsUrl){
                    // We finally got the url of the dropped ressource! Let's import it!
                    downloadURL(qsUrl, qsUrl, pPos);
                    return;
                }
#endif
        }
    }
}


void UBBoardController::togglePodcast(bool checked)
{
    if (UBPodcastController::instance())
        UBPodcastController::instance()->toggleRecordingPalette(checked);
}

void UBBoardController::moveGraphicsWidgetToControlView(UBGraphicsWidgetItem* graphicsWidget)
{
    mActiveScene->setURStackEnable(false);
     UBGraphicsItem *toolW3C = duplicateItem(dynamic_cast<UBItem *>(graphicsWidget));
    UBGraphicsWidgetItem *copyedGraphicsWidget = NULL;

    if (toolW3C)
    {
        if (UBGraphicsWidgetItem::Type == toolW3C->type())
            copyedGraphicsWidget = static_cast<UBGraphicsWidgetItem *>(toolW3C);

        UBToolWidget *toolWidget = new UBToolWidget(copyedGraphicsWidget, mControlView);

        graphicsWidget->remove(false);
        mActiveScene->addItemToDeletion(graphicsWidget);

        mActiveScene->setURStackEnable(true);

        QPoint controlViewPos = mControlView->mapFromScene(graphicsWidget->sceneBoundingRect().center());
        toolWidget->centerOn(mControlView->mapTo(mControlContainer, controlViewPos));
        toolWidget->show();
    }
}


void UBBoardController::moveToolWidgetToScene(UBToolWidget* toolWidget)
{
    UBGraphicsWidgetItem *widgetToScene = toolWidget->toolWidget();

    widgetToScene->resetTransform();

    QPoint mainWindowCenter = toolWidget->mapTo(mMainWindow, QPoint(toolWidget->width(), toolWidget->height()) / 2);
    QPoint controlViewCenter = mControlView->mapFrom(mMainWindow, mainWindowCenter);
    QPointF scenePos = mControlView->mapToScene(controlViewCenter);

    widgetToScene->setWebActive(true);
    mActiveScene->addGraphicsWidget(widgetToScene, scenePos);

    toolWidget->remove();
}


void UBBoardController::updateBackgroundActionsState(bool isDark, UBPageBackground pageBackground)
{
    switch (pageBackground) {

        case UBPageBackground::crossed:
            if (isDark)
                mMainWindow->actionCrossedDarkBackground->setChecked(true);
            else
                mMainWindow->actionCrossedLightBackground->setChecked(true);
        break;

        case UBPageBackground::ruled:
        {
            QAction* actionRuledBackground = nullptr;
            if(UBSettings::settings()->isSeyesRuledBackground())
                if(isDark)
                    actionRuledBackground = mMainWindow->actionSeyesRuledDarkBackground;
                else
                    actionRuledBackground = mMainWindow->actionSeyesRuledLightBackground;
            else
                if(isDark)
                    actionRuledBackground = mMainWindow->actionRuledDarkBackground;
                else
                    actionRuledBackground = mMainWindow->actionRuledLightBackground;
            if(actionRuledBackground)
                actionRuledBackground->setChecked(true);
        }
        break;

        default:
            if (isDark)
                mMainWindow->actionPlainDarkBackground->setChecked(true);
            else
                mMainWindow->actionPlainLightBackground->setChecked(true);
        break;
    }
}


void UBBoardController::addItem()
{
    QString defaultPath = UBSettings::settings()->lastImportToLibraryPath->get().toString();

    QString extensions;
    foreach(QString ext, UBSettings::imageFileExtensions)
    {
        extensions += " *.";
        extensions += ext;
    }

    QString filename = QFileDialog::getOpenFileName(mControlContainer, tr("Add Item"),
                                                    defaultPath,
                                                    tr("All Supported (%1)").arg(extensions), NULL, QFileDialog::DontUseNativeDialog);

    if (filename.length() > 0)
    {
        mPaletteManager->addItem(QUrl::fromLocalFile(filename));
        QFileInfo source(filename);
        UBSettings::settings()->lastImportToLibraryPath->set(QVariant(source.absolutePath()));
    }
}

void UBBoardController::importPage()
{
    int pageCount = selectedDocument()->pageCount();
    if (UBApplication::documentController->addFileToDocument(selectedDocument()))
    {
        setActiveDocumentScene(selectedDocument(), pageCount, true);
    }
}

void UBBoardController::notifyPageChanged()
{
    emit activeSceneChanged();
}

void UBBoardController::onDownloadModalFinished()
{

}

void UBBoardController::displayMetaData(QMap<QString, QString> metadatas)
{
    emit displayMetadata(metadatas);
}

void UBBoardController::freezeW3CWidgets(bool freeze)
{
    if (mActiveSceneIndex >= 0)
    {
        QList<QGraphicsItem *> list = UBApplication::boardController->activeScene()->items();
        foreach(QGraphicsItem *item, list)
        {
            freezeW3CWidget(item, freeze);
        }
    }
}

void UBBoardController::freezeW3CWidget(QGraphicsItem *item, bool freeze)
{
    if (item->type() == UBGraphicsW3CWidgetItem::Type)
    {
        UBGraphicsWidgetItem* widget = qgraphicsitem_cast<UBGraphicsWidgetItem*>(item);
        widget->setWebActive(!freeze);
    }
}

void UBBoardController::reloadThumbnails()
{
    //UBApplication::showMessage(tr("Reloading Board Thumbnails View (%1 pages)").arg(selectedDocument()->pageCount()));
    emit initThumbnailsRequired(selectedDocument());
    //UBApplication::showMessage(tr("Board Thumbnails View up-to-date"));
}
