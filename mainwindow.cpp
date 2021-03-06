/*

    Copyright (c) 2015 Oliver Lau <ola@ct.de>, Heise Medien GmbH & Co. KG

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/


#include <QDebug>
#include <QtAlgorithms>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QDateTime>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QDir>
#include <QFile>
#include <QPoint>
#include <QGraphicsOpacityEffect>
#include <QEasingCurve>
#include <QPropertyAnimation>
#include <QTime>
#include <QTimer>
#include <QVector>
#include <QShortcut>
#include <QLabel>
#include <QRegExp>
#include <QNetworkDiskCache>
#include <QSettings>
#include <QPixmapCache>
#include <qmath.h>

#include "globals.h"
#include "mainwindow.h"
#include "flowlayout.h"
#include "ui_mainwindow.h"

#include "o1twitter.h"
#include "o1requestor.h"
#include "o2globals.h"
#include "o2settingsstore.h"

struct KineticData {
    KineticData(void) : t(0) { /* ... */ }
    KineticData(const QPoint& p, int t) : p(p), t(t) { /* ... */ }
    QPoint p;
    int t;
};

QDebug operator<<(QDebug debug, const KineticData &kd)
{
  QDebugStateSaver saver(debug);
  (void)saver;
  debug.nospace() << "KineticData(" << kd.p << "," << kd.t << ")";
  return debug;
}


auto wordComparator = [](const QString &a, const QString &b) {
  return a.compare(b, Qt::CaseInsensitive) < 0;
};


auto idComparator = [](const QVariant &a, const QVariant &b) {
  return a.toMap()["id"].toLongLong() > b.toMap()["id"].toLongLong();
};



static const int MaxKineticDataSamples = 5;
static const qreal Friction = 0.95;
static const int TimeInterval = 25;
static const int AnimationDuration = 200;
enum ColumnIndexes {
  ColumnProfileImage = 0,
  ColumnText,
  ColumnCreatedAt,
  ColumnId
};

class MainWindowPrivate
{
public:
  MainWindowPrivate(QObject *parent)
    : oauth(new O1Twitter(parent))
    , store(new O2SettingsStore(O2_ENCRYPTION_KEY))
    , settings(QSettings::IniFormat, QSettings::UserScope, AppCompanyName, AppName)
    , tweetNAM(parent)
    , imageNAM(parent)
    , reply(Q_NULLPTR)
    , tableBuildCalled(false)
    , tweetFilepath(QStandardPaths::writableLocation(QStandardPaths::DataLocation))
    , mostRecentId(0)
    , mouseDown(false)
    , tweetFrameOpacityEffect(Q_NULLPTR)
    , mouseMoveTimerId(0)
    , imageCache(new QNetworkDiskCache(parent))
  {
    store->setGroupKey("twitter");
    oauth->setStore(store);
    oauth->setClientId(MY_CLIENT_KEY);
    oauth->setClientSecret(MY_CLIENT_SECRET);
    oauth->setLocalPort(44333);
    oauth->setSignatureMethod(O2_SIGNATURE_TYPE_HMAC_SHA1);
    floatInAnimation.setPropertyName("pos");
    floatInAnimation.setEasingCurve(QEasingCurve::InOutQuad);
    floatInAnimation.setDuration(AnimationDuration);
    floatOutAnimation.setPropertyName("pos");
    floatOutAnimation.setEasingCurve(QEasingCurve::InQuad);
    floatOutAnimation.setDuration(AnimationDuration);
    unfloatAnimation.setPropertyName("pos");
    unfloatAnimation.setDuration(AnimationDuration);
    unfloatAnimation.setEasingCurve(QEasingCurve::InOutQuad);
    imageCache->setCacheDirectory(QStandardPaths::writableLocation(QStandardPaths::DataLocation));
    imageNAM.setCache(imageCache);
  }
  ~MainWindowPrivate()
  {
    /* ... */
  }

  QVector<KineticData> kineticData;
  O1Twitter *oauth;
  O2SettingsStore *store;
  QSettings settings;
  QNetworkAccessManager tweetNAM;
  QNetworkAccessManager imageNAM;
  QNetworkReply *reply;
  bool tableBuildCalled;
  QString tweetFilepath;
  QString tweetFilename;
  QString badTweetFilename;
  QString goodTweetFilename;
  QString wordListFilename;
  QJsonArray storedTweets;
  QJsonArray badTweets;
  QJsonArray goodTweets;
  qlonglong mostRecentId;
  QPoint originalTweetFramePos;
  QPoint lastTweetFramePos;
  QPoint lastMousePos;
  bool mouseDown;
  QGraphicsOpacityEffect *tweetFrameOpacityEffect;
  QTime mouseMoveTimer;
  int mouseMoveTimerId;
  QPointF velocity;
  QJsonValue currentTweet;
  QPropertyAnimation unfloatAnimation;
  QPropertyAnimation floatInAnimation;
  QPropertyAnimation floatOutAnimation;
  QStringList relevantWords;
  QMenu *tableContextMenu;
  QNetworkDiskCache *imageCache;
};



MainWindow::MainWindow(QWidget *parent)
  : QMainWindow(parent)
  , ui(new Ui::MainWindow)
  , d_ptr(new MainWindowPrivate(this))
{
  Q_D(MainWindow);
  ui->setupUi(this);

  qDebug() << d->tweetFilepath;

  d->tweetFilename = d->tweetFilepath + "/all_tweets_of_" + d->settings.value("twitter/userId").toString() + ".json";
  d->badTweetFilename = d->tweetFilepath + "/bad_tweets_of_" + d->settings.value("twitter/userId").toString() + ".json";
  d->goodTweetFilename = d->tweetFilepath + "/good_tweets_of_" + d->settings.value("twitter/userId").toString() + ".json";
  d->wordListFilename = d->tweetFilepath + "/relevant_words_of_" + d->settings.value("twitter/userId").toString() + ".txt";

  bool ok;

  ok = QDir().mkpath(d->tweetFilepath);

  QFile tweetFile(d->tweetFilename);
  ok = tweetFile.open(QIODevice::ReadOnly);
  if (ok) {
    d->storedTweets = QJsonDocument::fromJson(tweetFile.readAll()).array();
    tweetFile.close();
  }
  QFile badTweetsFile(d->badTweetFilename);
  ok = badTweetsFile.open(QIODevice::ReadOnly);
  if (ok) {
    d->badTweets = QJsonDocument::fromJson(badTweetsFile.readAll()).array();
    badTweetsFile.close();
  }
  QFile goodTweets(d->goodTweetFilename);
  ok = goodTweets.open(QIODevice::ReadOnly);
  if (ok) {
    d->goodTweets = QJsonDocument::fromJson(goodTweets.readAll()).array();
    goodTweets.close();
  }
  QFile wordList(d->wordListFilename);
  ok = wordList.open(QIODevice::ReadOnly);
  if (ok) {
    while (!wordList.atEnd()) {
      QString word = QString::fromUtf8(wordList.readLine());
      d->relevantWords << word.trimmed();
    }
    wordList.close();
    qSort(d->relevantWords.begin(), d->relevantWords.end(), wordComparator);
  }

  QObject::connect(d->oauth, SIGNAL(linkedChanged()), SLOT(onLinkedChanged()));
  QObject::connect(d->oauth, SIGNAL(linkingFailed()), SLOT(onLinkingFailed()));
  QObject::connect(d->oauth, SIGNAL(linkingSucceeded()), SLOT(onLinkingSucceeded()));
  QObject::connect(d->oauth, SIGNAL(openBrowser(QUrl)), SLOT(onOpenBrowser(QUrl)));
  QObject::connect(d->oauth, SIGNAL(closeBrowser()), SLOT(onCloseBrowser()));
  QObject::connect(ui->likeButton, SIGNAL(clicked(bool)), SLOT(like()));
  QObject::connect(ui->dislikeButton, SIGNAL(clicked(bool)), SLOT(dislike()));
  QObject::connect(ui->actionExit, SIGNAL(triggered(bool)), SLOT(close()));
  QObject::connect(ui->actionRefresh, SIGNAL(triggered(bool)), SLOT(onRefresh()));
  ui->tweetFrame->installEventFilter(this);
  d->tweetFrameOpacityEffect = new QGraphicsOpacityEffect(ui->tweetFrame);
  d->tweetFrameOpacityEffect->setOpacity(1.0);
  ui->tweetFrame->setGraphicsEffect(d->tweetFrameOpacityEffect);
  d->floatOutAnimation.setTargetObject(ui->tweetFrame);
  d->floatInAnimation.setTargetObject(ui->tweetFrame);
  d->unfloatAnimation.setTargetObject(ui->tweetFrame);

  ui->likeButton->stackUnder(ui->tweetFrame);
  ui->dislikeButton->stackUnder(ui->tweetFrame);

  QObject::connect(&d->tweetNAM, SIGNAL(finished(QNetworkReply*)), this, SLOT(gotUserTimeline(QNetworkReply*)));
  QObject::connect(&d->imageNAM, SIGNAL(finished(QNetworkReply*)), this, SLOT(gotImage(QNetworkReply*)));

  ui->tableWidget->verticalHeader()->hide();
  ui->tableWidget->setContextMenuPolicy(Qt::CustomContextMenu);
  ui->tableWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
  QObject::connect(ui->tableWidget, SIGNAL(customContextMenuRequested(QPoint)), SLOT(onCustomMenuRequested(QPoint)));
  d->tableContextMenu = new QMenu(ui->tableWidget);
  d->tableContextMenu->addAction(tr("Delete"), this, SLOT(onDeleteTweet()));
  d->tableContextMenu->addAction(tr("Evaluate"), this, SLOT(onEvaluateTweet()));

  restoreSettings();

  d->oauth->link();

  QTimer::singleShot(100, this, SLOT(buildTable()));
}


MainWindow::~MainWindow()
{
  delete ui;
}


void MainWindow::showEvent(QShowEvent *)
{
  Q_D(MainWindow);
  d->originalTweetFramePos = ui->tweetFrame->pos();
}


void MainWindow::closeEvent(QCloseEvent*)
{
  Q_D(MainWindow);

  stopMotion();
  saveSettings();

  QFile tweetFile(d->tweetFilename);
  tweetFile.open(QIODevice::WriteOnly | QIODevice::Truncate);
  tweetFile.write(QJsonDocument(d->storedTweets).toJson(QJsonDocument::Indented));
  tweetFile.close();

  QFile badTweetFile(d->badTweetFilename);
  badTweetFile.open(QIODevice::WriteOnly | QIODevice::Truncate);
  badTweetFile.write(QJsonDocument(d->badTweets).toJson(QJsonDocument::Indented));
  badTweetFile.close();

  QFile goodTweetFile(d->goodTweetFilename);
  goodTweetFile.open(QIODevice::WriteOnly | QIODevice::Truncate);
  goodTweetFile.write(QJsonDocument(d->goodTweets).toJson(QJsonDocument::Indented));
  goodTweetFile.close();

  QFile wordFile(d->wordListFilename);
  wordFile.open(QIODevice::WriteOnly | QIODevice::Truncate);
  qSort(d->relevantWords.begin(), d->relevantWords.end(), wordComparator);
  foreach (QString word, d->relevantWords) {
    wordFile.write(word.toUtf8() + "\n");
  }
  wordFile.close();
}


void MainWindow::timerEvent(QTimerEvent *e)
{
  Q_D(MainWindow);
  if (e->timerId() == d->mouseMoveTimerId) {
    if (d->velocity.manhattanLength() > M_SQRT2) {
      scrollBy(d->velocity.toPoint());
      d->velocity *= Friction;
    }
    else {
      stopMotion();
      if (tweetFloating())
        unfloatTweet();
    }
  }
}


void MainWindow::unfloatTweet(void)
{
  Q_D(MainWindow);
  d->unfloatAnimation.setStartValue(ui->tweetFrame->pos());
  d->unfloatAnimation.setEndValue(d->originalTweetFramePos);
  d->unfloatAnimation.start();
  d->tweetFrameOpacityEffect->setOpacity(1.0);
}


void MainWindow::startMotion(const QPointF &velocity)
{
  Q_D(MainWindow);
  d->lastTweetFramePos = ui->tweetFrame->pos();
  d->velocity = velocity;
  if (d->mouseMoveTimerId == 0)
    d->mouseMoveTimerId = startTimer(TimeInterval);
}


void MainWindow::stopMotion(void)
{
  Q_D(MainWindow);
  if (d->mouseMoveTimerId) {
    killTimer(d->mouseMoveTimerId);
    d->mouseMoveTimerId = 0;
  }
  d->velocity = QPointF();
}


int MainWindow::likeLimit(void) const
{
  return ui->tweetFrame->width();
}


int MainWindow::dislikeLimit(void) const
{
  return -likeLimit();
}


bool MainWindow::tweetFloating(void) const
{
  const int x = ui->tweetFrame->pos().x();
  return dislikeLimit() < x && x < likeLimit();
}


void MainWindow::scrollBy(const QPoint &offset)
{
  Q_D(MainWindow);
  ui->tweetFrame->move(offset.x() + d->lastTweetFramePos.x(), d->lastTweetFramePos.y());
  d->lastTweetFramePos = ui->tweetFrame->pos();
  qreal opacity = qreal(ui->tweetFrame->width() - ui->tweetFrame->pos().x()) / ui->tweetFrame->width();
  if (opacity > 1.0)
    opacity = 2.0 - opacity;
  d->tweetFrameOpacityEffect->setOpacity(opacity - 0.25);
  if (ui->tweetFrame->pos().x() < dislikeLimit()) {
    dislike();
  }
  else if (ui->tweetFrame->pos().x() > likeLimit()) {
    like();
  }
}


bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
  Q_D(MainWindow);
  switch (event->type()) {
  case QEvent::MouseButtonPress:
  {
    if (obj->objectName() == ui->tweetFrame->objectName()) {
      QMouseEvent *mouseEvent = reinterpret_cast<QMouseEvent*>(event);
      if (mouseEvent->button() == Qt::LeftButton) {
        d->lastTweetFramePos = ui->tweetFrame->pos();
        d->lastMousePos = mouseEvent->globalPos();
        d->mouseDown = true;
        ui->tweetFrame->setCursor(Qt::ClosedHandCursor);
        d->mouseMoveTimer.start();
        d->kineticData.clear();
      }
    }
    break;
  }
  case QEvent::MouseMove:
  {
    QMouseEvent *mouseEvent = reinterpret_cast<QMouseEvent*>(event);
    if (d->mouseDown && obj->objectName() == ui->tweetFrame->objectName()) {
      scrollBy(QPoint(mouseEvent->globalPos().x() - d->lastMousePos.x(), 0));
      d->kineticData.append(KineticData(mouseEvent->globalPos(), d->mouseMoveTimer.elapsed()));
      if (d->kineticData.size() > MaxKineticDataSamples)
        d->kineticData.erase(d->kineticData.begin());
      d->lastMousePos = mouseEvent->globalPos();
    }
    break;
  }
  case QEvent::MouseButtonRelease:
  {
    if (obj->objectName() == ui->tweetFrame->objectName()) {
      QMouseEvent *mouseEvent = reinterpret_cast<QMouseEvent*>(event);
      if (mouseEvent->button() == Qt::LeftButton) {
        d->mouseDown = false;
        ui->tweetFrame->setCursor(Qt::OpenHandCursor);
        if (d->kineticData.count() == MaxKineticDataSamples) {
          int timeSinceLastMoveEvent = d->mouseMoveTimer.elapsed() - d->kineticData.last().t;
          if (timeSinceLastMoveEvent < 100) {
            int dt = d->mouseMoveTimer.elapsed() - d->kineticData.first().t;
            const QPointF &moveDist = QPointF(mouseEvent->globalPos().x() - d->kineticData.first().p.x(), 0);
            const QPointF &initialVector = 1000 * moveDist / dt / TimeInterval;
            startMotion(initialVector);
          }
          else {
            unfloatTweet();
          }
        }
      }
    }
    break;
  }
  case QEvent::LayoutRequest:
    if (obj->objectName() == ui->tweetFrame->objectName()) {
      return false;
    }
  default:
    break;
  }
  return QObject::eventFilter(obj, event);
}


void MainWindow::onLinkedChanged(void)
{
  Q_D(MainWindow);
  qDebug() << "MainWindow::onLinkedChanged()" << d->oauth->linked();
}


void MainWindow::onLinkingFailed(void)
{
  qWarning() << "MainWindow::onLinkingFailed()";
}


void MainWindow::onLinkingSucceeded(void)
{
  Q_D(MainWindow);
  O1Twitter* o1t = qobject_cast<O1Twitter*>(sender());
  if (!o1t->extraTokens().isEmpty()) {
    d->settings.setValue("twitter/screenName", o1t->extraTokens().value("screen_name"));
    d->settings.setValue("twitter/userId", o1t->extraTokens().value("user_id"));
    d->settings.sync();
  }
  if (d->oauth->linked()) {
    ui->screenNameLineEdit->setText(d->settings.value("twitter/screenName").toString());
    ui->userIdLineEdit->setText(d->settings.value("twitter/userId").toString());
  }
  else {
    ui->screenNameLineEdit->setText(QString());
    ui->userIdLineEdit->setText(QString());
  }
}


void MainWindow::onOpenBrowser(const QUrl &url)
{
  ui->statusBar->showMessage(tr("Opening browser: %1").arg(url.toString()), 3000);
  QDesktopServices::openUrl(url);
}


void MainWindow::onCloseBrowser(void)
{
  ui->statusBar->showMessage(tr("Closing browser"), 3000);
}


QJsonArray MainWindow::mergeTweets(const QJsonArray &storedJson, const QJsonArray &currentJson)
{
  const QList<QVariant> &currentList = currentJson.toVariantList();
  const QList<QVariant> &storedList = storedJson.toVariantList();
  QList<QVariant> result = storedList;
  foreach (QVariant post, currentList) {
    QList<QVariant>::const_iterator idx;
    idx = qBinaryFind(storedList.constBegin(), storedList.constEnd(), post, idComparator);
    if (idx == storedList.constEnd()) {
      result << post;
    }
  }
  qSort(result.begin(), result.end(), idComparator);
  return QJsonArray::fromVariantList(result);
}


void MainWindow::calculateMostRecentId(void)
{
  Q_D(MainWindow);
  qlonglong lastMostRecentId = d->mostRecentId;
  d->mostRecentId = d->currentTweet.toVariant().toMap()["id"].toLongLong();
  if (!d->storedTweets.isEmpty()) {
    qlonglong id = d->storedTweets.first().toVariant().toMap()["id"].toLongLong();
    d->mostRecentId = qMax(id, d->mostRecentId);
  }
  if (!d->badTweets.isEmpty()) {
    qlonglong id = d->badTweets.first().toVariant().toMap()["id"].toLongLong();
    d->mostRecentId = qMax(id, d->mostRecentId);
  }
  if (!d->goodTweets.isEmpty()) {
    qlonglong id = d->goodTweets.first().toVariant().toMap()["id"].toLongLong();
    d->mostRecentId = qMax(id, d->mostRecentId);
  }
  qDebug() << d->mostRecentId << lastMostRecentId << (lastMostRecentId <= d->mostRecentId);
}


void MainWindow::wordSelected(void)
{
  Q_D(MainWindow);
  if (sender() == Q_NULLPTR)
    return;
  QPushButton *btn = reinterpret_cast<QPushButton*>(sender());
  static const QRegExp reWord("([#\\w-']+)");
  reWord.exactMatch(btn->text());
  QString w = reWord.cap().trimmed();
  QStringList::const_iterator idx = qBinaryFind(d->relevantWords.constBegin(), d->relevantWords.constEnd(), w, wordComparator);
  if (idx == d->relevantWords.constEnd()) {
    d->relevantWords << w;
    qSort(d->relevantWords.begin(), d->relevantWords.end(), wordComparator);
    ui->statusBar->showMessage(tr("Added \"%1\" to list of relevant words.").arg(w), 3000);
  }
}


void MainWindow::onCustomMenuRequested(const QPoint &pos)
{
  Q_D(MainWindow);
  d->tableContextMenu->popup(ui->tableWidget->viewport()->mapToGlobal(pos));
}


void MainWindow::onDeleteTweet(void)
{
  QItemSelectionModel *select = ui->tableWidget->selectionModel();
  if (select->hasSelection()) {
    const QModelIndexList &idxs = select->selectedRows();
    foreach (QModelIndex idx, idxs) {
      ui->tableWidget->removeRow(idx.row());
    }
  }
  ui->tableWidget->clearSelection();
}


void MainWindow::onEvaluateTweet(void)
{
  qDebug() << "MainWindow::onEvaluateTweet()";
}


void clearLayout(QLayout *layout) {
  if (layout != Q_NULLPTR) {
    QLayoutItem *item;
    while ((item = layout->takeAt(0)) != Q_NULLPTR) {
      if (item->layout() != Q_NULLPTR) {
        clearLayout(item->layout());
        delete item->layout();
      }
      if (item->widget() != Q_NULLPTR) {
        delete item->widget();
      }
      if (item->spacerItem() != Q_NULLPTR) {
        delete item->spacerItem();
      }
    }
  }
}


void MainWindow::pickNextTweet(void)
{
  Q_D(MainWindow);
  stopMotion();
  if (ui->tableWidget->columnCount() > 0 && ui->tableWidget->rowCount() > 0) {
    clearLayout(ui->tweetFrameLayout->layout());
    d->currentTweet = d->storedTweets.first();
    d->storedTweets.pop_front();
    calculateMostRecentId();
    static const QRegExp delim("\\s", Qt::CaseSensitive, QRegExp::RegExp2);
//    static const QRegExp reUrl("^(https?:\\/\\/)?([\\da-z\\.-]+)\\.([a-z\\.]{2,6})([\\/\\w \\.-]*)*\\/?$", Qt::CaseInsensitive, QRegExp::RegExp2);
    QVariantMap tweet = d->currentTweet.toVariant().toMap();
    const QStringList &words = tweet["text"].toString().split(delim);
    QPixmap pix;
    if (QPixmapCache::find(tweet["user"].toMap()["profile_image_url"].toString(), &pix)) {
      ui->profileImageLabel->setPixmap(pix);
    }
    ui->profileImageLabel->setToolTip(QString("@%1").arg(tweet["user"].toMap()["name"].toString()));
    FlowLayout *flowLayout = new FlowLayout(2, 2, 2);
    foreach (QString word, words) {
      QPushButton *widget = new QPushButton;
      widget->setStyleSheet("border: 1px solid #444; background-color: #ffdab9; padding: 1px 2px; font-size: 12pt");
      widget->setText(word);
      widget->setCursor(Qt::PointingHandCursor);
      QObject::connect(widget, SIGNAL(clicked(bool)), SLOT(wordSelected()));
      flowLayout->addWidget(widget);
    }
    ui->tableWidget->removeRow(0);
    d->floatInAnimation.setStartValue(d->originalTweetFramePos + QPoint(0, ui->tweetFrame->height()));
    d->floatInAnimation.setEndValue(d->originalTweetFramePos);
    d->floatInAnimation.start();
    d->tweetFrameOpacityEffect->setOpacity(1.0);
    ui->tweetFrameLayout->addLayout(flowLayout);
  }
}


void MainWindow::buildTable(const QJsonArray &mostRecentTweets)
{
  Q_D(MainWindow);
  if (!mostRecentTweets.isEmpty()) {
    d->storedTweets = mergeTweets(d->storedTweets, mostRecentTweets);
    ui->statusBar->showMessage(tr("%1 new entries since id %2").arg(mostRecentTweets.size()).arg(d->mostRecentId), 3000);
    QFile tweetFile(d->tweetFilename);
    tweetFile.open(QIODevice::WriteOnly | QIODevice::Truncate);
    tweetFile.write(QJsonDocument(d->storedTweets).toJson(QJsonDocument::Indented));
    tweetFile.close();
  }
  calculateMostRecentId();

  if (d->storedTweets.isEmpty() && !d->tableBuildCalled) {
    getUserTimeline();
    return;
  }
  d->tableBuildCalled = true;

  ui->tableWidget->setRowCount(d->storedTweets.count());
  int row = 0;
  foreach(QJsonValue p, d->storedTweets) {
    const QVariantMap &post = p.toVariant().toMap();
    const QUrl &imageUrl = QUrl(post["user"].toMap()["profile_image_url"].toString());
    QTableWidgetItem *imgItem = new QTableWidgetItem;
    imgItem->setData(Qt::UserRole, imageUrl);
    QPixmap pix;
    if (QPixmapCache::find(imageUrl.toString(), &pix)) {
      imgItem->setData(Qt::DecorationRole, pix);
    }
    else {
      loadImage(imageUrl);
    }
    ui->tableWidget->setItem(row, 0, imgItem);
    QTableWidgetItem *textItem = new QTableWidgetItem(post["text"].toString());
    textItem->setTextAlignment(Qt::AlignTop | Qt::AlignLeft);
    ui->tableWidget->setItem(row, 1, textItem);
    QTableWidgetItem *createdAtItem = new QTableWidgetItem(post["created_at"].toString());
    createdAtItem->setTextAlignment(Qt::AlignTop | Qt::AlignLeft);
    ui->tableWidget->setItem(row, 2, createdAtItem);
    QTableWidgetItem *idItem = new QTableWidgetItem(QString("%1").arg(post["id"].toLongLong()));
    ui->tableWidget->setItem(row, 3, idItem);
    idItem->setTextAlignment(Qt::AlignTop | Qt::AlignLeft);
    ++row;
  }
  pickNextTweet();
}


void MainWindow::buildTable(void)
{
  buildTable(QJsonArray());
}


void MainWindow::gotUserTimeline(QNetworkReply *reply)
{
  Q_D(MainWindow);
  if (reply->error() != QNetworkReply::NoError) {
    ui->statusBar->showMessage(tr("Error: %1").arg(reply->errorString()));
    const QJsonDocument &msg = QJsonDocument::fromJson(reply->readAll());
    const QList<QVariant> &errors = msg.toVariant().toMap()["errors"].toList();
    QString errMsg;
    foreach (QVariant e, errors) {
      errMsg += QString("%1 (code: %2)\n").arg(e.toMap()["message"].toString()).arg(e.toMap()["code"].toInt());
    }
    QMessageBox::warning(this, tr("Error"), errMsg);
  }
  else {
    if (!d->currentTweet.isNull()) {
      d->storedTweets.push_front(d->currentTweet);
      d->currentTweet = QJsonValue();
    }
    const QJsonArray &mostRecentTweets = QJsonDocument::fromJson(reply->readAll()).array();
    buildTable(mostRecentTweets);
  }
}


void MainWindow::getUserTimeline(void)
{
  Q_D(MainWindow);
  O1Requestor *requestor = new O1Requestor(&d->tweetNAM, d->oauth, this);
  QList<O1RequestParameter> reqParams;
  reqParams << (d->mostRecentId > 0
                ? O1RequestParameter("since_id", QString::number(d->mostRecentId).toLatin1())
                : O1RequestParameter("count", "200"));
  QNetworkRequest request(QUrl("https://api.twitter.com/1.1/statuses/home_timeline.json"));
  request.setHeader(QNetworkRequest::ContentTypeHeader, O2_MIME_TYPE_XFORM);
  d->reply = requestor->get(request, reqParams);
}


void MainWindow::onRefresh(void)
{
  Q_D(MainWindow);
  if (d->oauth->linked()) {
    getUserTimeline();
  }
  else {
    ui->statusBar->showMessage(tr("Application is not linked to Twitter."));
  }
}


void MainWindow::gotImage(QNetworkReply *reply)
{
  const QUrl &url = reply->request().url();
  QPixmap pix;
  pix.loadFromData(reply->readAll());
  QPixmapCache::insert(url.toString(), pix);
  ui->tableWidget->resizeColumnToContents(0);
  for (int row = 0; row < ui->tableWidget->rowCount(); ++row) {
    QTableWidgetItem *item = ui->tableWidget->item(row, 0);
    if (url == item->data(Qt::UserRole).toUrl()) {
      item->setData(Qt::DecorationRole, pix);
      ui->tableWidget->setRowHeight(row, 48);
    }
  }
}


void MainWindow::loadImage(const QUrl &url)
{
  Q_D(MainWindow);
  QPixmap pix;
  if (!QPixmapCache::find(url.toString(), &pix)) {
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    d->imageNAM.get(request);
  }
}


void MainWindow::onLogout(void)
{
  Q_D(MainWindow);
  d->oauth->unlink();
}


void MainWindow::onLogin(void)
{
  Q_D(MainWindow);
  d->oauth->link();
}


void MainWindow::like(void)
{
  Q_D(MainWindow);
  stopMotion();
  d->goodTweets.push_front(d->currentTweet);
  d->floatOutAnimation.setStartValue(ui->tweetFrame->pos());
  d->floatOutAnimation.setEndValue(d->originalTweetFramePos + QPoint(3 * ui->tweetFrame->width() * 2, 0));
  d->floatOutAnimation.start();
  QTimer::singleShot(AnimationDuration, this, &MainWindow::pickNextTweet);
}


void MainWindow::dislike(void)
{
  Q_D(MainWindow);
  stopMotion();
  d->badTweets.push_front(d->currentTweet);
  d->floatOutAnimation.setStartValue(ui->tweetFrame->pos());
  d->floatOutAnimation.setEndValue(d->originalTweetFramePos - QPoint(3 * ui->tweetFrame->width() / 2, 0));
  d->floatOutAnimation.start();
  QTimer::singleShot(AnimationDuration, this, &MainWindow::pickNextTweet);
}


void MainWindow::saveSettings(void)
{
  Q_D(MainWindow);
  d->settings.setValue("mainwindow/geometry", saveGeometry());
  d->settings.setValue("mainwindow/state", saveState());
  for (int c = 0; c < ui->tableWidget->columnCount(); ++c) {
    d->settings.setValue(QString("table/column/%1/width").arg(c), ui->tableWidget->columnWidth(c));
  }
  d->settings.sync();
}


void MainWindow::restoreSettings(void)
{
  Q_D(MainWindow);
  restoreGeometry(d->settings.value("mainwindow/geometry").toByteArray());
  restoreState(d->settings.value("mainwindow/state").toByteArray());
  for (int c = 0; c < ui->tableWidget->columnCount(); ++c) {
    ui->tableWidget->setColumnWidth(c, d->settings.value(QString("table/column/%1/width").arg(c)).toInt());
  }
}
